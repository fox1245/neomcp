// The JWT validator behind the endpoint's gate, over real sockets on the loopback interface: the HTTP library's client
// sends requests with a bearer token to the server, the validator checks the token with the keys of a scripted key set
// endpoint, and the gate applies its own checks to whatever the validator reports.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"
#include "scripted_http_server.h"
#include "support.h"

namespace {

using namespace jwt_test;
using http_test::ScriptedHttpServer;

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

Reply list_tools(std::uint16_t port, const std::string& bearer) {
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
                                       {"Authorization", "Bearer " + bearer}};
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

// The HTTP server with the JWT validator in front of it. The key set endpoint comes first: it outlives the server, and
// the validator inside the server's options is dropped with it.
struct Harness {
    explicit Harness(int status = 200)
        : keys([status](const http_test::Request&) {
              return http_test::json_reply(status, key_set({rsa_key(0).jwk("rs", "RS256"), ec_key(0).jwk("es")}));
          }),
          server(neomcp::ServerOptions{.name = "jwt-gate", .version = "0.0.1"}) {
        server.tool("hello", "A tool anyone with a token may list", [] { return std::string("hello"); });
        neomcp::http::ResourceServerOptions authorization;
        authorization.resource = resource;
        authorization.authorization_servers = {issuer};
        authorization.required_scopes = {"mcp:use"};
        authorization.validator = make(options_for(keys.url(jwks_path), log));
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
    ScriptedHttpServer keys;
    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

std::string signed_rs256(const json& claims) {
    return token(good_header(JwtAlgorithm::rs256, std::string("rs")), claims, rsa_key(0), JwtAlgorithm::rs256);
}

TEST(JwtGate, AGoodTokenIsServed) {
    Harness harness;
    const Reply first = list_tools(harness.port, signed_rs256(good_claims()));
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.status, 200) << first.body;
    const Reply second = list_tools(harness.port,
                                    token(good_header(JwtAlgorithm::es256, std::string("es")), good_claims(), ec_key(0),
                                          JwtAlgorithm::es256));
    EXPECT_EQ(second.status, 200) << second.body;
    EXPECT_EQ(harness.keys.count(), 1U);
}

TEST(JwtGate, RefusedTokensAreAnswered401InvalidToken) {
    Harness harness;
    const std::string claims = base64url(good_claims().dump());
    const std::string none = base64url(R"({"alg":"none","typ":"at+jwt","kid":"rs"})") + "." + claims + ".";
    const std::string hmac_input = base64url(R"({"alg":"HS256","typ":"at+jwt","kid":"rs"})") + "." + claims;
    const std::string hmac = hmac_input + "." + base64url(hmac_sha256(rsa_key(0).public_pem(), hmac_input));
    json identity_header = good_header(JwtAlgorithm::rs256, std::string("rs"));
    identity_header["typ"] = "JWT";
    const std::string identity = token(identity_header, good_claims(), rsa_key(0), JwtAlgorithm::rs256);
    const std::string another_key =
        token(good_header(JwtAlgorithm::rs256, std::string("rs")), good_claims(), rsa_key(1), JwtAlgorithm::rs256);
    for (const std::string& refused : {none, hmac, identity, another_key}) {
        const Reply reply = list_tools(harness.port, refused);
        ASSERT_TRUE(reply.ok);
        EXPECT_EQ(reply.status, 401);
        EXPECT_NE(reply.header("WWW-Authenticate").find("invalid_token"), std::string::npos)
            << reply.header("WWW-Authenticate");
        // Nothing of the token comes back.
        EXPECT_EQ(reply.body.find(refused.substr(0, 16)), std::string::npos);
        EXPECT_EQ(reply.header("WWW-Authenticate").find(refused.substr(0, 16)), std::string::npos);
    }
}

TEST(JwtGate, TheGateStillChecksTheAudienceTheExpiryAndTheScope) {
    Harness harness;
    json other_audience = good_claims();
    other_audience["aud"] = json::array({"https://elsewhere.example/mcp"});
    EXPECT_EQ(list_tools(harness.port, signed_rs256(other_audience)).status, 401);
    json expired = good_claims();
    expired["exp"] = std::chrono::duration_cast<std::chrono::seconds>(
                         (std::chrono::system_clock::now() - std::chrono::hours(2)).time_since_epoch())
                         .count();
    EXPECT_EQ(list_tools(harness.port, signed_rs256(expired)).status, 401);
    json narrow = good_claims();
    narrow["scope"] = "files:read";
    const Reply reply = list_tools(harness.port, signed_rs256(narrow));
    EXPECT_EQ(reply.status, 403);
    EXPECT_NE(reply.header("WWW-Authenticate").find("insufficient_scope"), std::string::npos);
}

TEST(JwtGate, AKeySetThatCannotBeFetchedIsAnswered503) {
    Harness harness(500);
    const Reply reply = list_tools(harness.port, signed_rs256(good_claims()));
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, 503);
    EXPECT_TRUE(reply.header("WWW-Authenticate").empty());
}

}  // namespace
