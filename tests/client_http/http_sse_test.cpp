// The Client over the older transport of events over HTTP (2024-11-05): round trips with the library's own session
// engine behind the test-only server, the hand-over from Streamable HTTP when its handshake is refused, a verdict that
// goes there directly, the endpoint event resolved against the stream's URL, servers that do not keep to the
// transport, and a stream that is lost and opened again by the next request.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../conformance/everything.h"
#include "neomcp/client.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/http.h"
#include "neomcp/http_server.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "http_sse_server.h"
#include "rig.h"
#include "scripted_http_server.h"
#include "support.h"

namespace {

using http_sse_test::HttpSseServer;
using http_sse_test::HttpSseServerOptions;
using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::ClientErrorKind;
using neomcp::profile::Transport;

constexpr std::string_view revision = "2024-11-05";

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

bool mentions(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// The library's server with every conformance tool, behind the test-only server of events over HTTP.
struct OwnSseServer {
    explicit OwnSseServer(HttpSseServerOptions options = {}) : server(conformance::server_options()) {
        conformance::register_everything(server);
        sse = std::make_unique<HttpSseServer>(server, std::move(options));
    }
    std::string url() const { return sse->url(); }

    neomcp::Server server;
    std::unique_ptr<HttpSseServer> sse;
};

neomcp::ClientOptions older_only() { return client_test::only({revision}); }

neomcp::ClientOptions every_revision() {
    neomcp::ClientOptions options;
    options.client_info.name = "client-http-sse-test";
    options.client_info.version = "1.0.0";
    return options;
}

neomcp::SamplingAnswer paris() {
    neomcp::SamplingAnswer answer;
    neomcp::TextContent said;
    said.text = "Paris";
    answer.content.push_back(std::move(said));
    answer.model = "scripted";
    return answer;
}

// ---- Round trips

TEST(ClientHttpSse, RoundTripOnTheOlderRevision) {
    OwnSseServer own;
    neomcp::ClientOptions options = older_only();
    options.handlers.sampling([](const neomcp::SamplingRequest&) { return paris(); });
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto verdict = client->verdict();
    ASSERT_TRUE(verdict);
    EXPECT_EQ(verdict->transport, Transport::http_sse);
    EXPECT_EQ(verdict->revision, revision);
    EXPECT_EQ(verdict->kind, neomcp::ServerKind::handshake);

    const auto tools = client->list_all_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    EXPECT_FALSE(tools->items.empty());
    const auto simple = client->call_tool("test_simple_text");
    ASSERT_TRUE(simple) << simple.error().message;
    EXPECT_EQ(text_of(*simple), "This is a simple text response for testing.");

    const auto resources = client->list_all_resources();
    ASSERT_TRUE(resources) << resources.error().message;
    ASSERT_FALSE(resources->items.empty());
    const auto read = client->read_resource(resources->items.front().uri);
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_FALSE(read->contents.empty());

    const auto prompts = client->list_all_prompts();
    ASSERT_TRUE(prompts) << prompts.error().message;
    EXPECT_FALSE(prompts->items.empty());
    const auto prompt = client->get_prompt("test_simple_prompt");
    ASSERT_TRUE(prompt) << prompt.error().message;
    EXPECT_FALSE(prompt->messages.empty());

    // The server asks the client on the event stream, and the answer is posted.
    const auto sampled = client->call_tool("test_sampling", json{{"prompt", "The capital of France?"}});
    ASSERT_TRUE(sampled) << sampled.error().message;
    EXPECT_EQ(text_of(*sampled), "LLM response: Paris");

    std::atomic<int> reports{0};
    neomcp::RequestOptions call;
    call.on_progress = [&reports](const neomcp::Progress&) { ++reports; };
    const auto progressed = client->call_tool("test_tool_with_progress", json::object(), call);
    ASSERT_TRUE(progressed) << progressed.error().message;
    EXPECT_EQ(text_of(*progressed), "Progress test completed");
    EXPECT_EQ(reports.load(), 3);

    const auto pinged = client->ping();
    EXPECT_TRUE(pinged) << pinged.error().message;
    EXPECT_TRUE(client->close());

    EXPECT_EQ(own.sse->streams(), 1u);
    EXPECT_EQ(own.sse->posted("initialize"), 1u);
    EXPECT_EQ(own.sse->refused_posts(), 0u);
}

// ---- Detection

TEST(ClientHttpSse, AnEndpointThatRefusesTheStreamableHandshakeIsSpokenToOverEvents) {
    OwnSseServer own;
    auto client = neomcp::Client::open(every_revision(), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->transport, Transport::http_sse);
    EXPECT_EQ(client->verdict()->revision, revision);
    // The probe and the handshake of Streamable HTTP were refused, then the stream was opened.
    EXPECT_EQ(own.sse->refused_posts(), 2u);
    EXPECT_EQ(own.sse->streams(), 1u);
    const auto simple = client->call_tool("test_simple_text");
    ASSERT_TRUE(simple) << simple.error().message;
    EXPECT_EQ(text_of(*simple), "This is a simple text response for testing.");
    // The verdict, given back, goes to the transport of events at once.
    neomcp::ClientOptions again = every_revision();
    again.prior = client->verdict();
    EXPECT_TRUE(client->close());
    auto second = neomcp::Client::open(std::move(again), http_rig::endpoint(own.url()));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second->verdict()->transport, Transport::http_sse);
    EXPECT_EQ(own.sse->refused_posts(), 2u);
    EXPECT_EQ(own.sse->streams(), 2u);
}

TEST(ClientHttpSse, APriorVerdictGoesToTheTransportOfEventsDirectly) {
    OwnSseServer own;
    neomcp::ClientOptions options = every_revision();
    options.prior = neomcp::ContactVerdict{neomcp::ServerKind::handshake, Transport::http_sse, std::string(revision)};
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->transport, Transport::http_sse);
    EXPECT_EQ(own.sse->refused_posts(), 0u);
    EXPECT_EQ(own.sse->streams(), 1u);
}

// A verdict of the transport of events that no longer holds (the endpoint now serves Streamable HTTP): the endpoint is
// asked once more from the start.
TEST(ClientHttpSse, APriorVerdictThatFailsIsProbedAgain) {
    neomcp::Server server(conformance::server_options());
    conformance::register_everything(server);
    neomcp::HttpServerOptions served;
    served.keep_alive_interval = std::chrono::hours(1);
    neomcp::HttpServer http(server, std::move(served));
    const auto started = http.start();
    ASSERT_TRUE(started) << started.error;
    neomcp::ClientOptions options = every_revision();
    options.prior = neomcp::ContactVerdict{neomcp::ServerKind::handshake, Transport::http_sse, std::string(revision)};
    auto endpoint = http_rig::endpoint("http://127.0.0.1:" + std::to_string(*started.value) + "/mcp");
    endpoint.endpoint_timeout = std::chrono::seconds(2);
    auto client = neomcp::Client::open(std::move(options), std::move(endpoint));
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->transport, Transport::streamable_http);
}

// Step 3 fails too: the error names what each step met.
TEST(ClientHttpSse, AnEndpointThatServesNeitherNamesEveryAnswer) {
    ScriptedHttpServer peer([](const Request& request) {
        if (request.method == "GET") return http_test::empty_reply(405);
        return http_test::empty_reply(404);
    });
    auto client = neomcp::Client::open(every_revision(), http_rig::endpoint(peer.url()));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::http_status) << client.error().message;
    EXPECT_FALSE(client.error().next_transport);
    EXPECT_TRUE(mentions(client.error().message, "server/discover")) << client.error().message;
    EXPECT_TRUE(mentions(client.error().message, "initialize")) << client.error().message;
    EXPECT_TRUE(mentions(client.error().message, "transport of events over HTTP")) << client.error().message;
    std::size_t gets = 0;
    for (const auto& request : peer.requests()) gets += request.method == "GET" ? 1 : 0;
    EXPECT_EQ(gets, 1u);
}

// ---- The endpoint event, resolved against the stream's URL

class EndpointReference : public ::testing::TestWithParam<std::string> {};

TEST_P(EndpointReference, IsResolvedAgainstTheStreamsUrl) {
    auto origin = std::make_shared<std::string>();
    HttpSseServerOptions options;
    options.path = "/sse/stream";
    const std::string form = GetParam();
    options.endpoint_reference = [form, origin](const std::string& query) -> std::string {
        if (form == "path_relative") return "stream?" + query;
        if (form == "dot_segments") return "../sse/./stream?" + query;
        if (form == "query_only") return "?" + query;
        if (form == "absolute_url") return *origin + "/sse/stream?" + query + "#ignored";
        return "/sse/stream?" + query;
    };
    OwnSseServer own(std::move(options));
    *origin = own.sse->origin();
    auto client = neomcp::Client::open(older_only(), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto simple = client->call_tool("test_simple_text");
    ASSERT_TRUE(simple) << simple.error().message;
    // Every message reached the stream's session.
    EXPECT_EQ(own.sse->posted("initialize"), 1u);
    EXPECT_EQ(own.sse->posted("tools/call"), 1u);
}

INSTANTIATE_TEST_SUITE_P(ClientHttpSse, EndpointReference,
                         ::testing::Values("absolute_path", "path_relative", "dot_segments", "query_only",
                                           "absolute_url"));

// ---- The event names come from the profile

TEST(ClientHttpSse, TheEventNamesAreTheProfiles) {
    json document = json::parse(neomcp::profile::builtin_text(revision));
    document["transports"]["http_sse"]["endpoint_event"] = "where";
    document["transports"]["http_sse"]["message_event"] = "said";
    auto loaded = neomcp::profile::load(document.dump());
    ASSERT_TRUE(std::holds_alternative<neomcp::profile::ProfilePtr>(loaded));
    HttpSseServerOptions served;
    served.endpoint_event = "where";
    served.message_event = "said";
    OwnSseServer own(std::move(served));
    neomcp::ClientOptions options = older_only();
    options.profiles = {std::get<neomcp::profile::ProfilePtr>(loaded)};
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto simple = client->call_tool("test_simple_text");
    ASSERT_TRUE(simple) << simple.error().message;
}

// ---- Servers that do not keep to the transport

neomcp::Result<neomcp::Client> open_older(const std::string& url, std::chrono::milliseconds endpoint_timeout =
                                                                      neomcp::HttpClientDefaults::endpoint_timeout) {
    auto endpoint = http_rig::endpoint(url);
    endpoint.endpoint_timeout = endpoint_timeout;
    return neomcp::Client::open(older_only(), std::move(endpoint));
}

TEST(ClientHttpSse, AFirstEventOfAnotherTypeFailsOpening) {
    HttpSseServerOptions options;
    options.first_event = "message";
    OwnSseServer own(std::move(options));
    auto client = open_older(own.url());
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::transport) << client.error().message;
    EXPECT_TRUE(mentions(client.error().message, "first event is not the endpoint event")) << client.error().message;
    EXPECT_TRUE(own.sse->posted_methods().empty());
}

TEST(ClientHttpSse, AnEndpointOfAnotherOriginIsNeverReached) {
    ScriptedHttpServer other([](const Request&) { return http_test::empty_reply(202); });
    for (const std::string& reference :
         {other.url("/sse/stream"), "//127.0.0.1:" + std::to_string(other.port()) + "/sse/stream",
          "https://127.0.0.1:" + std::to_string(other.port()) + "/sse/stream"}) {
        HttpSseServerOptions options;
        options.endpoint_reference = [reference](const std::string& query) { return reference + "?" + query; };
        OwnSseServer own(std::move(options));
        auto client = open_older(own.url());
        ASSERT_FALSE(client) << reference;
        EXPECT_EQ(client.error().kind, ClientErrorKind::transport) << client.error().message;
        EXPECT_TRUE(mentions(client.error().message, "leaves the endpoint's origin")) << client.error().message;
        EXPECT_FALSE(mentions(client.error().message, "127.0.0.1")) << client.error().message;
        EXPECT_TRUE(own.sse->posted_methods().empty());
    }
    EXPECT_EQ(other.count(), 0u);
}

TEST(ClientHttpSse, AnEndpointWithUserInformationIsNeverReached) {
    auto origin = std::make_shared<std::string>();
    HttpSseServerOptions options;
    options.endpoint_reference = [origin](const std::string& query) {
        return "http://someone:secret@" + *origin + "/sse/stream?" + query;
    };
    OwnSseServer own(std::move(options));
    *origin = "127.0.0.1:" + std::to_string(own.sse->port());
    auto client = open_older(own.url());
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::transport) << client.error().message;
    EXPECT_TRUE(mentions(client.error().message, "user information")) << client.error().message;
    EXPECT_FALSE(mentions(client.error().message, "secret")) << client.error().message;
    EXPECT_TRUE(own.sse->posted_methods().empty());
}

TEST(ClientHttpSse, NoEndpointWithinTheTimeoutFailsOpening) {
    HttpSseServerOptions options;
    options.send_endpoint = false;
    OwnSseServer own(std::move(options));
    const auto began = std::chrono::steady_clock::now();
    auto client = open_older(own.url(), std::chrono::milliseconds(300));
    const auto took = std::chrono::steady_clock::now() - began;
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::transport) << client.error().message;
    EXPECT_TRUE(mentions(client.error().message, "endpoint_timeout")) << client.error().message;
    // Far sooner than the handshake's own clock (60 s).
    EXPECT_LT(took, std::chrono::seconds(10));
    EXPECT_EQ(own.sse->streams(), 1u);
}

// A peer scripted by hand: the stream names its endpoint, answers the handshake when it is posted, and stays open;
// each POST is answered by `answer`.
struct ScriptedSsePeer {
    explicit ScriptedSsePeer(std::function<http_test::Reply(const Request&)> answer)
        : handshake(std::make_shared<http_test::Gate>()),
          held(std::make_shared<http_test::Gate>()),
          server([this, answer](const Request& request) {
              if (request.method == "GET") {
                  const std::string result =
                      json{{"jsonrpc", "2.0"},
                           {"id", 1},
                           {"result",
                            {{"protocolVersion", std::string(revision)},
                             {"capabilities", {{"tools", json::object()}}},
                             {"serverInfo", {{"name", "scripted"}, {"version", "1.0.0"}}}}}}
                          .dump();
                  return http_test::event_stream_reply({"event: endpoint\ndata: /mcp?stream=1\n\n"})
                      .wait(handshake)
                      .send("event: message\ndata: " + result + "\n\n")
                      .wait(held);
              }
              if (request.rpc_method() == "initialize") {
                  auto opened = handshake;
                  return http_test::empty_reply(202).run([opened] { opened->open(); });
              }
              return answer(request);
          }) {}
    ~ScriptedSsePeer() { held->open(); }

    std::shared_ptr<http_test::Gate> handshake;
    std::shared_ptr<http_test::Gate> held;
    ScriptedHttpServer server;
};

TEST(ClientHttpSse, AMessageRefusedByStatusFailsAtOnce) {
    // Every message after the handshake is refused (a tools/list the client may send first included).
    ScriptedSsePeer peer([](const Request& request) {
        if (request.rpc_method() == "notifications/initialized") return http_test::empty_reply(202);
        return http_test::empty_reply(500);
    });
    auto client = open_older(peer.server.url());
    ASSERT_TRUE(client) << client.error().message;
    neomcp::RequestOptions call;
    call.timeout = std::chrono::seconds(60);
    const auto began = std::chrono::steady_clock::now();
    const auto refused = client->call_tool("anything", json::object(), call);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::http_status) << refused.error().message;
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(10));
}

TEST(ClientHttpSse, AStreamRefusedByStatusFailsOpening) {
    ScriptedHttpServer peer([](const Request&) { return http_test::empty_reply(404); });
    auto client = open_older(peer.url());
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::http_status) << client.error().message;
}

// ---- A lost stream

TEST(ClientHttpSse, ALostStreamEndsTheConnectionAndTheNextRequestOpensANewOne) {
    OwnSseServer own;
    HttpSseServer* sse = own.sse.get();
    auto first = std::make_shared<std::atomic<bool>>(true);
    neomcp::ClientOptions options = older_only();
    options.request_timeout = std::chrono::seconds(10);
    // The first question of the server: the stream is dropped before it is answered.
    options.handlers.sampling([first, sse](const neomcp::SamplingRequest&) {
        if (first->exchange(false)) sse->drop_streams();
        return paris();
    });
    std::mutex seen_mutex;
    std::vector<neomcp::ServerRestarted> restarts;
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (const auto* restarted = std::get_if<neomcp::ServerRestarted>(&diagnostic)) {
            std::lock_guard lock(seen_mutex);
            restarts.push_back(*restarted);
        }
    };
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto lost = client->call_tool("test_sampling", json{{"prompt", "The capital of France?"}});
    ASSERT_FALSE(lost);
    EXPECT_EQ(lost.error().kind, ClientErrorKind::stream_lost) << lost.error().message;
    EXPECT_TRUE(lost.error().may_have_run);

    const auto simple = client->call_tool("test_simple_text");
    ASSERT_TRUE(simple) << simple.error().message;
    EXPECT_EQ(text_of(*simple), "This is a simple text response for testing.");
    const auto sampled = client->call_tool("test_sampling", json{{"prompt", "The capital of France?"}});
    ASSERT_TRUE(sampled) << sampled.error().message;
    EXPECT_EQ(text_of(*sampled), "LLM response: Paris");

    EXPECT_EQ(own.sse->streams(), 2u);
    EXPECT_EQ(own.sse->posted("initialize"), 2u);
    std::lock_guard lock(seen_mutex);
    ASSERT_EQ(restarts.size(), 1u);
    EXPECT_TRUE(restarts.front().handshake_repeated);
}

}  // namespace
