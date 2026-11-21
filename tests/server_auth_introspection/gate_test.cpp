// The introspection validator behind the endpoint's gate, over real sockets on the loopback interface: the HTTP library's
// client sends requests with a bearer token to the server, the server asks a scripted introspection endpoint, and the
// gate applies its own checks to whatever the validator reports.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"
#include "support.h"

namespace {

using namespace introspection_test;
using http_test::ScriptedHttpServer;
using SystemClock = std::chrono::system_clock;

constexpr const char* path = "/mcp";
constexpr const char* modern = "2026-07-28";
constexpr const char* json_type = "application/json";
constexpr const char* both_types = "application/json, text/event-stream";

struct Reply {
    bool ok = false;
    int status = 0;
    httplib::Headers headers;
    std::string body;
    std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string() : found->second;
    }
};

Reply list_tools(std::uint16_t port, const std::string& token) {
    httplib::Client client("127.0.0.1", port);
    client.set_keep_alive(false);
    client.set_connection_timeout(30, 0);
    client.set_read_timeout(30, 0);
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.headers = httplib::Headers{{"Content-Type", json_type},
                                       {"Accept", both_types},
                                       {"MCP-Protocol-Version", modern},
                                       {"Mcp-Method", "tools/list"},
                                       {"Authorization", "Bearer " + token}};
    json params = json::object();
    params["_meta"] = json{{"io.modelcontextprotocol/protocolVersion", modern},
                           {"io.modelcontextprotocol/clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                           {"io.modelcontextprotocol/clientCapabilities", json::object()}};
    request.body = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", std::move(params)}}.dump();
    Reply reply;
    if (auto result = client.send(request)) {
        reply.ok = true;
        reply.status = result->status;
        reply.headers = result->headers;
        reply.body = result->body;
    }
    return reply;
}

// The HTTP server with the introspection validator in front of it. The scripted endpoint comes first: it outlives the
// server, and the validator inside the server's options is dropped with it.
struct Harness {
    explicit Harness(ScriptedHttpServer::Script script)
        : introspection(std::move(script)), server(neomcp::ServerOptions{.name = "introspection-gate", .version = "0.0.1"}) {
        server.tool("hello", "A tool anyone with a token may list", [] { return std::string("hello"); });
        neomcp::http::ResourceServerOptions authorization;
        authorization.resource = resource;
        authorization.authorization_servers = {issuer};
        authorization.required_scopes = {"mcp:use"};
        authorization.validator = make(options_for(introspection.url(introspection_path), log));
        neomcp::HttpServerOptions options;
        options.disconnect_poll_interval = std::chrono::milliseconds(20);
        options.endpoint.stream_after = std::chrono::hours(1);
        options.keep_alive_interval = std::chrono::hours(1);
        options.worker_threads = 4;
        options.authorization = std::move(authorization);
        http = std::make_unique<neomcp::HttpServer>(server, std::move(options));
        auto started = http->start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }

    std::shared_ptr<EventLog> log = std::make_shared<EventLog>();
    ScriptedHttpServer introspection;
    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

ScriptedHttpServer::Script answer(json body, int status = 200) {
    return [body = std::move(body), status](const http_test::Request&) { return http_test::json_reply(status, body); };
}

std::chrono::seconds::rep epoch_seconds(std::chrono::seconds from_now) {
    return std::chrono::duration_cast<std::chrono::seconds>((SystemClock::now() + from_now).time_since_epoch()).count();
}

json active_for_an_hour() {
    json body = active_answer();
    body["exp"] = epoch_seconds(std::chrono::hours(1));
    return body;
}

TEST(IntrospectionGate, AnActiveTokenIsServedAndAskedAboutOnce) {
    Harness harness(answer(active_for_an_hour()));
    const Reply first = list_tools(harness.port, "token-a");
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.status, 200) << first.body;
    const Reply second = list_tools(harness.port, "token-a");
    EXPECT_EQ(second.status, 200) << second.body;
    // The endpoint remembers the verdict: the introspection endpoint was asked once, with the token.
    EXPECT_EQ(harness.introspection.count(), 1U);
    EXPECT_NE(harness.introspection.requests().front().body.find("token=token-a"), std::string::npos);
}

TEST(IntrospectionGate, AnInactiveTokenIsAnswered401InvalidToken) {
    Harness harness(answer(json{{"active", false}}));
    const Reply reply = list_tools(harness.port, "token-a");
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 401);
    EXPECT_NE(reply.header("WWW-Authenticate").find("invalid_token"), std::string::npos) << reply.header("WWW-Authenticate");
}

TEST(IntrospectionGate, AnUnavailableIntrospectionEndpointIsAnswered503) {
    for (const int status : {500, 401, 302}) {
        Harness harness(answer(active_for_an_hour(), status));
        const Reply reply = list_tools(harness.port, "token-a");
        ASSERT_TRUE(reply.ok);
        EXPECT_EQ(reply.status, 503) << status;
        EXPECT_TRUE(reply.header("WWW-Authenticate").empty()) << status;
    }
}

TEST(IntrospectionGate, AnEndpointThatIsGoneIsAnswered503) {
    Harness harness(answer(active_for_an_hour()));
    harness.introspection.stop();
    const Reply reply = list_tools(harness.port, "token-a");
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 503);
}

TEST(IntrospectionGate, TheGateStillChecksTheAudience) {
    json other_audience = active_for_an_hour();
    other_audience["aud"] = json::array({"https://elsewhere.example/mcp"});
    Harness wrong(answer(other_audience));
    EXPECT_EQ(list_tools(wrong.port, "token-a").status, 401);
    // An answer without an audience tells the gate nothing about who the token is for.
    json no_audience = active_for_an_hour();
    no_audience.erase("aud");
    Harness none(answer(no_audience));
    EXPECT_EQ(list_tools(none.port, "token-a").status, 401);
}

TEST(IntrospectionGate, TheGateStillChecksTheIssuerAndTheScope) {
    json other_issuer = active_for_an_hour();
    other_issuer["iss"] = "https://other-auth.example";
    Harness wrong(answer(other_issuer));
    EXPECT_EQ(list_tools(wrong.port, "token-a").status, 401);
    json other_scope = active_for_an_hour();
    other_scope["scope"] = "files:read";
    Harness narrow(answer(other_scope));
    const Reply reply = list_tools(narrow.port, "token-a");
    EXPECT_EQ(reply.status, 403);
    EXPECT_NE(reply.header("WWW-Authenticate").find("insufficient_scope"), std::string::npos);
}

TEST(IntrospectionGate, AnExpiredTokenIsRefusedWhatEverTheEndpointCalledIt) {
    json expired = active_for_an_hour();
    expired["exp"] = epoch_seconds(-std::chrono::hours(2));
    Harness harness(answer(expired));
    EXPECT_EQ(list_tools(harness.port, "token-a").status, 401);
    json not_yet = active_for_an_hour();
    not_yet["nbf"] = epoch_seconds(std::chrono::hours(1));
    Harness later(answer(not_yet));
    EXPECT_EQ(list_tools(later.port, "token-a").status, 401);
}

TEST(IntrospectionGate, ATokenWithoutAnExpiryIsServed) {
    json body = active_answer();
    body.erase("exp");
    Harness harness(answer(body));
    EXPECT_EQ(list_tools(harness.port, "token-a").status, 200);
}

}  // namespace
