// The Client over Streamable HTTP: round trips with the library's own server on every revision that defines the
// transport, the probe of which kind of server answers (the rows of its two steps) against a scripted peer, and the
// header fields the engine mirrors from a tool's arguments as they reach the peer.
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../conformance/everything.h"
#include "neomcp/client.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/http.h"
#include "neomcp/http_server.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "rig.h"
#include "scripted_http_server.h"
#include "support.h"

namespace {

using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::ClientErrorKind;
using neomcp::ServerKind;

constexpr std::string_view modern = "2026-07-28";
constexpr std::string_view legacy = "2025-11-25";

// Every built-in revision, so that the probe runs.
neomcp::ClientOptions every_revision() {
    neomcp::ClientOptions options;
    options.client_info.name = "client-http-test";
    options.client_info.version = "1.0.0";
    return options;
}

// ---- Round trips with the library's own server

struct OwnServer {
    explicit OwnServer(std::chrono::milliseconds stream_after = std::chrono::hours(1))
        : server(conformance::server_options()) {
        conformance::register_everything(server);
        neomcp::HttpServerOptions options;
        options.endpoint.stream_after = stream_after;
        options.keep_alive_interval = std::chrono::hours(1);
        options.max_worker_threads = 32;
        http = std::make_unique<neomcp::HttpServer>(server, std::move(options));
        const auto started = http->start();
        if (!started) throw std::runtime_error("the server did not start: " + started.error);
        port = *started.value;
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; }

    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

class RoundTrip : public ::testing::TestWithParam<std::string> {};

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

TEST_P(RoundTrip, CallsTheLibrarysServer) {
    OwnServer own;
    neomcp::ClientOptions options = GetParam() == "all" ? every_revision() : client_test::only({GetParam()});
    std::atomic<int> logs{0};
    options.log_level = neomcp::LogLevel::debug;
    options.on_log = [&logs](const neomcp::LogEvent&) { ++logs; };
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto verdict = client->verdict();
    ASSERT_TRUE(verdict);
    EXPECT_EQ(verdict->transport, neomcp::profile::Transport::streamable_http);
    if (GetParam() == "all") {
        EXPECT_EQ(verdict->revision, modern);
        EXPECT_EQ(verdict->kind, ServerKind::per_request);
    } else {
        EXPECT_EQ(verdict->revision, GetParam());
    }
    const auto simple = client->call_tool("test_simple_text");
    ASSERT_TRUE(simple) << simple.error().message;
    EXPECT_EQ(text_of(*simple), "This is a simple text response for testing.");
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed->contains("tools"));
    // Log messages come before the answer, so the answer is an event stream.
    const auto logged = client->call_tool("test_tool_with_logging");
    ASSERT_TRUE(logged) << logged.error().message;
    EXPECT_EQ(text_of(*logged), "Logging test completed");
    EXPECT_EQ(logs.load(), 3);
    EXPECT_TRUE(client->close());
}

neomcp::ClientOptions options_for(const std::string& revision) {
    return revision == "all" ? every_revision() : client_test::only({revision});
}

// The server asks the client while a call is served: a request of its own on the call's stream where the revision
// sends requests, a result that asks where the revision asks so.
TEST_P(RoundTrip, AsksTheClientForInput) {
    OwnServer own;
    neomcp::ClientOptions options = options_for(GetParam());
    options.handlers.form([](const neomcp::ElicitationForm&) {
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept,
                                         json{{"username", "ada"}, {"email", "ada@example.com"}}};
    });
    options.handlers.sampling([](const neomcp::SamplingRequest&) {
        neomcp::SamplingAnswer answer;
        answer.content = json{{"type", "text"}, {"text", "Paris"}};
        answer.model = "scripted";
        return answer;
    });
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto sampled = client->call_tool("test_sampling", json{{"prompt", "The capital of France?"}});
    ASSERT_TRUE(sampled) << sampled.error().message;
    EXPECT_EQ(text_of(*sampled), "LLM response: Paris");
    const neomcp::profile::ProfilePtr governing = neomcp::profile::builtin(client->verdict()->revision);
    if (governing->request("elicitation/create") != nullptr || governing->input_request("elicitation/create")) {
        const auto asked = client->call_tool("test_elicitation", json{{"message", "Who are you?"}});
        ASSERT_TRUE(asked) << asked.error().message;
        EXPECT_NE(text_of(*asked).find("action=accept"), std::string::npos) << text_of(*asked);
        EXPECT_NE(text_of(*asked).find("ada@example.com"), std::string::npos) << text_of(*asked);
    }
    EXPECT_TRUE(client->close());
}

TEST_P(RoundTrip, ReportsTheProgressOfACall) {
    OwnServer own;
    auto client = neomcp::Client::open(options_for(GetParam()), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    std::atomic<int> reports{0};
    neomcp::RequestOptions call;
    call.on_progress = [&reports](const neomcp::Progress&) { ++reports; };
    const auto done = client->call_tool("test_tool_with_progress", json::object(), call);
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_EQ(text_of(*done), "Progress test completed");
    EXPECT_EQ(reports.load(), 3);
    EXPECT_TRUE(client->close());
}

// Under a revision with a handshake the changes come on the stream of the connection, which the listener opens.
TEST_P(RoundTrip, HearsOfAChangeOfTheTools) {
    OwnServer own;
    auto client = neomcp::Client::open(options_for(GetParam()), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    neomcp::ChangeFilter filter;
    filter.tools_list = true;
    auto subscription = client->listen(filter);
    ASSERT_TRUE(subscription) << subscription.error().message;
    const auto triggered = client->call_tool("test_trigger_tool_change");
    ASSERT_TRUE(triggered) << triggered.error().message;
    const auto change = subscription->next();
    ASSERT_TRUE(change) << change.error().message;
    EXPECT_EQ(change->kind, neomcp::ChangeKind::tools_list);
    EXPECT_TRUE(client->close());
}

// Every revision that defines Streamable HTTP, and all of them at once (the probe decides).
INSTANTIATE_TEST_SUITE_P(ClientHttp, RoundTrip,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28", "all"),
                         [](const auto& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return "Revision_" + name;
                         });

TEST(ClientHttp, AServerThatAnswersEveryCallWithAStreamIsRead) {
    OwnServer own(std::chrono::milliseconds(1));
    auto client = neomcp::Client::open(client_test::only({legacy}), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto progress = client->call_tool("test_tool_with_progress");
    ASSERT_TRUE(progress) << progress.error().message;
    EXPECT_EQ(text_of(*progress), "Progress test completed");
}

// ---- The probe, against a scripted peer

json discover_result() {
    return json{{"supportedVersions", json::array({std::string(modern)})},
                {"capabilities", json::object()},
                {"resultType", "complete"},
                {"ttlMs", 0},
                {"cacheScope", "private"}};
}

json initialize_result() {
    return json{{"protocolVersion", std::string(legacy)},
                {"capabilities", json::object()},
                {"serverInfo", {{"name", "scripted"}, {"version", "1.0.0"}}}};
}

// A peer whose answer to the probe is `probe`; the handshake is answered by `handshake` (an InitializeResult when not
// given), the rest with empty results.
class ProbePeer {
public:
    using Answer = std::function<http_test::Reply(const Request&)>;

    explicit ProbePeer(Answer probe, Answer handshake = nullptr)
        : server_([probe, handshake](const Request& request) {
              const std::string method = request.rpc_method();
              if (method == "server/discover") return probe(request);
              if (method == "initialize") {
                  if (handshake) return handshake(request);
                  return http_test::json_reply(200, http_test::result_for(request, initialize_result()));
              }
              if (request.rpc_id().is_null()) return http_test::empty_reply(202);
              return http_test::json_reply(200, http_test::result_for(request, json::object()));
          }) {}

    std::string url() const { return server_.url(); }
    std::size_t posts_of(std::string_view method) const {
        std::size_t count = 0;
        for (const auto& request : server_.requests()) {
            if (request.rpc_method() == method) ++count;
        }
        return count;
    }
    ScriptedHttpServer& server() { return server_; }

private:
    ScriptedHttpServer server_;
};

ProbePeer::Answer status_only(int status) {
    return [status](const Request&) { return http_test::empty_reply(status); };
}

ProbePeer::Answer error_with(int status, int code, json data = nullptr) {
    return [status, code, data](const Request& request) {
        return http_test::json_reply(status, http_test::error_for(request, code, "refused", data));
    };
}

neomcp::Result<neomcp::Client> open_against(ProbePeer& peer, neomcp::ClientOptions options = every_revision(),
                                            std::shared_ptr<neomcp::HttpAuthorizer> authorizer = nullptr) {
    auto described = http_rig::endpoint(peer.url());
    described.authorizer = std::move(authorizer);
    return neomcp::Client::open(std::move(options), std::move(described));
}

void expect_modern(neomcp::Result<neomcp::Client>& client, const ProbePeer& peer) {
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->kind, ServerKind::per_request);
    EXPECT_EQ(client->verdict()->revision, modern);
    EXPECT_EQ(peer.posts_of("initialize"), 0u);
}

void expect_handshake(neomcp::Result<neomcp::Client>& client, const ProbePeer& peer) {
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->kind, ServerKind::handshake);
    EXPECT_EQ(client->verdict()->revision, legacy);
    EXPECT_EQ(peer.posts_of("server/discover"), 1u);
    EXPECT_EQ(peer.posts_of("initialize"), 1u);
}

void expect_no_fallback(neomcp::Result<neomcp::Client>& client, const ProbePeer& peer, ClientErrorKind kind) {
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, kind) << client.error().message;
    EXPECT_EQ(peer.posts_of("server/discover"), 1u);
    EXPECT_EQ(peer.posts_of("initialize"), 0u);
}

// Step 1: the probe.

TEST(ClientHttp, ProbeADiscoveryResultIsAModernServer) {
    ProbePeer peer([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, discover_result()));
    });
    auto client = open_against(peer);
    expect_modern(client, peer);
}

TEST(ClientHttp, ProbeARefusedRevisionIsAskedOnceMoreAtAServedOne) {
    auto refused = std::make_shared<bool>(false);
    ProbePeer peer([refused](const Request& request) {
        if (!*refused) {
            *refused = true;
            return http_test::json_reply(
                400, http_test::error_for(request, -32022, "unsupported",
                                          json{{"supported", json::array({std::string(modern)})},
                                               {"requested", std::string(modern)}}));
        }
        return http_test::json_reply(200, http_test::result_for(request, discover_result()));
    });
    auto client = open_against(peer);
    expect_modern(client, peer);
    EXPECT_EQ(peer.posts_of("server/discover"), 2u);
    // The second probe has a new id.
    const auto requests = peer.server().requests();
    EXPECT_NE(requests[0].rpc_id(), requests[1].rpc_id());
}

TEST(ClientHttp, ProbeAnotherErrorOnlyAModernServerSendsIsAModernServer) {
    ProbePeer peer(error_with(400, -32021));
    auto client = open_against(peer);
    expect_modern(client, peer);
}

TEST(ClientHttp, ProbeANotFoundWithMethodNotFoundIsAModernServer) {
    ProbePeer peer(error_with(404, -32601));
    auto client = open_against(peer);
    expect_modern(client, peer);
}

TEST(ClientHttp, ProbeAnotherResultIsAServerThatHandshakes) {
    ProbePeer peer([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    auto client = open_against(peer);
    expect_handshake(client, peer);
}

TEST(ClientHttp, ProbeAnotherErrorInASuccessIsAServerThatHandshakes) {
    ProbePeer peer(error_with(200, -32601));
    auto client = open_against(peer);
    expect_handshake(client, peer);
}

TEST(ClientHttp, ProbeAStatusThatRefusesTheRequestIsAServerThatHandshakes) {
    for (const int status : {400, 404, 405}) {
        ProbePeer peer(status_only(status));
        auto client = open_against(peer);
        expect_handshake(client, peer);
    }
    ProbePeer invalid(error_with(400, -32600));
    auto client = open_against(invalid);
    expect_handshake(client, invalid);
}

TEST(ClientHttp, ProbeARefusalForWantOfAuthorizationStops) {
    for (const int status : {401, 403}) {
        ProbePeer peer(status_only(status));
        auto client = open_against(peer);
        expect_no_fallback(client, peer, ClientErrorKind::authorization_required);
    }
}

class ChallengeCounter final : public neomcp::HttpAuthorizer {
public:
    neomcp::Credential authorization(const neomcp::HttpRequestInfo&) override {
        return neomcp::Credential{"Bearer not-enough", 0};
    }
    void challenge(const neomcp::HttpRequestInfo&, int status, std::span<const std::string> challenges, std::uint64_t,
                   neomcp::ChallengeReply reply) override {
        statuses.push_back(status);
        values.assign(challenges.begin(), challenges.end());
        neomcp::ClientError error;
        error.kind = neomcp::ClientErrorKind::authorization_required;
        reply.fail(std::move(error));
    }
    std::vector<int> statuses;
    std::vector<std::string> values;
};

TEST(ClientHttp, ProbeARefusalIsToldToTheAuthorizerAndStillStops) {
    ProbePeer peer([](const Request&) {
        return http_test::empty_reply(401, {{"WWW-Authenticate", "Bearer realm=\"mcp\""}});
    });
    auto authorizer = std::make_shared<ChallengeCounter>();
    auto client = open_against(peer, every_revision(), authorizer);
    expect_no_fallback(client, peer, ClientErrorKind::authorization_required);
    EXPECT_EQ(authorizer->statuses, std::vector<int>{401});
    EXPECT_EQ(authorizer->values, std::vector<std::string>{"Bearer realm=\"mcp\""});
    const auto requests = peer.server().requests();
    ASSERT_FALSE(requests.empty());
    EXPECT_EQ(requests.front().field("Authorization"), "Bearer not-enough");
}

TEST(ClientHttp, ProbeAnotherStatusIsAnErrorWithoutFallback) {
    for (const int status : {409, 500, 503}) {
        ProbePeer peer(status_only(status));
        auto client = open_against(peer);
        expect_no_fallback(client, peer, ClientErrorKind::http_status);
    }
}

TEST(ClientHttp, ProbeAServerErrorWithAnErrorBodyIsAnErrorWithoutFallback) {
    ProbePeer peer(error_with(500, -32603));
    auto client = open_against(peer);
    expect_no_fallback(client, peer, ClientErrorKind::http_status);
}

TEST(ClientHttp, ProbeASuccessThatIsNoJsonRpcIsAnErrorWithoutFallback) {
    for (const char* body : {"[1,2]", "hello", ""}) {
        ProbePeer peer([body](const Request&) {
            return http_test::raw_reply(200, {{"Content-Type", "application/json"}}, body);
        });
        auto client = open_against(peer);
        expect_no_fallback(client, peer, ClientErrorKind::transport);
    }
}

TEST(ClientHttp, ProbeASuccessOfAnotherTypeIsAnErrorWithoutFallback) {
    ProbePeer peer([](const Request&) {
        return http_test::raw_reply(200, {{"Content-Type", "text/html"}}, "<html></html>");
    });
    auto client = open_against(peer);
    expect_no_fallback(client, peer, ClientErrorKind::transport);
}

TEST(ClientHttp, ProbeATimeoutIsAnErrorWithoutFallback) {
    auto gate = std::make_shared<http_test::Gate>();
    ProbePeer peer([gate](const Request&) {
        http_test::Reply reply;
        reply.wait(gate);
        return reply;
    });
    auto timers = std::make_shared<client_test::ManualTimers>();
    neomcp::ClientOptions options = every_revision();
    options.timers = timers;
    neomcp::Result<neomcp::Client> client = neomcp::ClientError{};
    std::thread opening([&] { client = open_against(peer, options); });
    ASSERT_TRUE(peer.server().wait_for(1));
    const auto timer = timers->live([](const client_test::ManualTimers::Started& one) {
        return std::holds_alternative<neomcp::Deadline>(one.purpose);
    });
    ASSERT_TRUE(timer);
    timers->fire(timer->id);
    opening.join();
    gate->open();
    expect_no_fallback(client, peer, ClientErrorKind::transport);
}

TEST(ClientHttp, ProbeAConnectFailureIsAnErrorWithoutFallback) {
    std::string url;
    {
        ScriptedHttpServer gone([](const Request&) { return http_test::empty_reply(500); });
        url = gone.url();
    }
    auto client = neomcp::Client::open(every_revision(), http_rig::endpoint(url));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::transport);
}

// Step 2: the handshake.

// A refusing status goes on to step 3 where a revision defines the transport of events over HTTP (this peer answers
// its GET without an event stream); without such a revision it is an error.
TEST(ClientHttp, HandshakeARefusingStatusIsAnError) {
    for (const int status : {400, 404, 405}) {
        ProbePeer peer(status_only(status), status_only(status));
        auto client = open_against(peer);
        ASSERT_FALSE(client);
        EXPECT_EQ(client.error().kind, ClientErrorKind::transport) << client.error().message;
        EXPECT_EQ(peer.posts_of("initialize"), 1u);
        std::size_t gets = 0;
        for (const auto& request : peer.server().requests()) gets += request.method == "GET" ? 1 : 0;
        EXPECT_EQ(gets, 1u);

        ProbePeer streamable_only(status_only(status), status_only(status));
        neomcp::ClientOptions options = client_test::only({modern, legacy});
        auto refused = open_against(streamable_only, std::move(options));
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().kind, ClientErrorKind::http_status) << refused.error().message;
        EXPECT_FALSE(refused.error().next_transport);
        EXPECT_EQ(streamable_only.posts_of("initialize"), 1u);
    }
}

TEST(ClientHttp, HandshakeAnotherStatusIsAnError) {
    ProbePeer peer(status_only(404), status_only(500));
    auto client = open_against(peer);
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::http_status);
}

TEST(ClientHttp, HandshakeAModernErrorProbesOnceMore) {
    auto probes = std::make_shared<int>(0);
    ProbePeer peer(
        [probes](const Request& request) {
            if ((*probes)++ == 0) return http_test::empty_reply(404);
            return http_test::json_reply(200, http_test::result_for(request, discover_result()));
        },
        error_with(400, -32021));
    auto client = open_against(peer);
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->kind, ServerKind::per_request);
    EXPECT_EQ(peer.posts_of("server/discover"), 2u);
}

// ---- Calls over HTTP

TEST(ClientHttp, ACallAnsweredWithAnotherTypeFailsWithBadContentType) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.rpc_method() == "server/discover") {
            return http_test::json_reply(200, http_test::result_for(request, discover_result()));
        }
        return http_test::raw_reply(200, {{"Content-Type", "text/plain"}}, "fine");
    });
    auto client = neomcp::Client::open(client_test::only({modern}), http_rig::endpoint(server.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto listed = client->request("tools/list");
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::bad_content_type);
}

TEST(ClientHttp, ACallRefusedForWantOfAuthorizationFailsWithAuthorizationRequired) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.rpc_method() == "server/discover") {
            return http_test::json_reply(200, http_test::result_for(request, discover_result()));
        }
        return http_test::empty_reply(403, {{"WWW-Authenticate", "Bearer error=\"insufficient_scope\""}});
    });
    auto client = neomcp::Client::open(client_test::only({modern}), http_rig::endpoint(server.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto listed = client->request("tools/list");
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::authorization_required);
}

TEST(ClientHttp, AnAnswerOverTheBoundFailsWithMessageTooLarge) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.rpc_method() == "server/discover") {
            return http_test::json_reply(200, http_test::result_for(request, discover_result()));
        }
        json result{{"tools", json::array()}, {"pad", std::string(8192, 'x')}};
        return http_test::json_reply(200, http_test::result_for(request, result));
    });
    auto options = client_test::only({modern});
    options.max_message_bytes = 4096;
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(server.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto listed = client->request("tools/list");
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::message_too_large);
}

TEST(ClientHttp, AnUnusableUrlFailsToOpenWithInvalidOptions) {
    auto client = neomcp::Client::open(every_revision(), http_rig::endpoint("ftp://127.0.0.1/mcp"));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::invalid_options);
}

// ---- Mirrored header fields, from the engine to the peer

// An engine and an HTTP transport on one thread, the engine's outputs carried to the transport after each call.
class EngineOverHttp final : public neomcp::TransportEvents {
public:
    explicit EngineOverHttp(const std::string& url) {
        neomcp::EngineOptions options;
        options.profiles = {neomcp::profile::builtin(modern)};
        options.client_info.name = "engine-test";
        options.client_info.version = "1.0.0";
        auto made = neomcp::ClientEngine::create(std::move(options), neomcp::profile::Transport::streamable_http,
                                                 [this](neomcp::Outgoing outgoing) { out_.push_back(std::move(outgoing)); });
        engine_ = std::get<std::unique_ptr<neomcp::ClientEngine>>(std::move(made));
        neomcp::detail::http_client::HttpTransportSettings settings;
        settings.profiles = {neomcp::profile::builtin(modern)};
        transport_ = neomcp::detail::http_client::make_http_transport(http_rig::endpoint(url), settings).value();
        io_.run([this] {
            static_cast<void>(transport_->start(*this));
            engine_->open();
            flush();
        });
    }

    ~EngineOverHttp() override {
        io_.run([this] {
            engine_->close();
            out_.clear();
        });
        auto done = asio::co_spawn(io_.executor(), transport_->close(neomcp::CloseCause::client_closed),
                                   asio::use_future);
        done.get();
        io_.run([this] { transport_.reset(); });
    }

    asio::any_io_executor executor() const noexcept override { return executor_; }
    neomcp::Reception received(neomcp::UnitId unit, std::string_view message) override {
        const auto reception = engine_->received(unit, message);
        flush();
        return reception;
    }
    void stream_event(neomcp::UnitId unit, neomcp::StreamEvent event) override {
        engine_->stream_event(unit, std::move(event));
        flush();
    }
    void restarted() override {}

    bool open() {
        return client_test::eventually(
            [&] { return io_.run([this] { return engine_->contact().phase == neomcp::ContactPhase::open; }); });
    }

    void call(neomcp::OutgoingCall call) {
        io_.run([this, &call] {
            const auto sent = engine_->send(std::move(call));
            EXPECT_TRUE(std::holds_alternative<neomcp::RequestRef>(sent));
            flush();
        });
    }

private:
    void flush() {
        while (!out_.empty()) {
            neomcp::Outgoing next = std::move(out_.front());
            out_.erase(out_.begin());
            if (next.kind != neomcp::OutgoingKind::close) transport_->send(std::move(next));
        }
    }

    http_rig::IoThread io_;
    asio::any_io_executor executor_ = io_.executor();
    std::unique_ptr<neomcp::ClientEngine> engine_;
    std::unique_ptr<neomcp::Transport> transport_;
    std::vector<neomcp::Outgoing> out_;
};

TEST(ClientHttp, EmptyMirroredValueIsSent) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.rpc_method() == "server/discover") {
            return http_test::json_reply(200, http_test::result_for(request, discover_result()));
        }
        return http_test::json_reply(
            200, http_test::result_for(request, json{{"content", json::array()}, {"resultType", "complete"}}));
    });
    EngineOverHttp engine(server.url());
    ASSERT_TRUE(engine.open());
    const auto marked = [](const char* header) {
        return json{{"type", "string"}, {"x-mcp-header", header}};
    };
    neomcp::OutgoingCall call;
    call.message.method = "tools/call";
    call.message.params = json{{"name", "mirror"},
                               {"arguments",
                                {{"empty_val", ""},
                                 {"spaces", "   "},
                                 {"tab", "\t"},
                                 {"non_ascii", "Hello, \xE4\xB8\x96\xE7\x95\x8C"},
                                 {"look_alike", "=?base64?abc?="},
                                 {"inner", "us west 1"},
                                 {"unmarked", "x"}}}};
    call.input_schema = json{{"type", "object"},
                             {"properties",
                              {{"empty_val", marked("EmptyVal")},
                               {"spaces", marked("Spaces")},
                               {"tab", marked("Tab")},
                               {"non_ascii", marked("NonAscii")},
                               {"look_alike", marked("LookAlike")},
                               {"inner", marked("Inner")},
                               {"unmarked", {{"type", "string"}}}}}};
    engine.call(std::move(call));
    ASSERT_TRUE(server.wait_for(2));
    const Request sent = server.requests().back();
    ASSERT_EQ(sent.rpc_method(), "tools/call");
    // The empty value is sent as a field with an empty value, not left out.
    EXPECT_EQ(sent.fields_named("Mcp-Param-EmptyVal"), std::vector<std::string>{""});
    EXPECT_TRUE(sent.has_line("Mcp-Param-EmptyVal:") || sent.has_line("Mcp-Param-EmptyVal: "));
    EXPECT_EQ(sent.field("Mcp-Param-Spaces"), "=?base64?ICAg?=");
    EXPECT_EQ(sent.field("Mcp-Param-Tab"), "=?base64?CQ==?=");
    EXPECT_EQ(sent.field("Mcp-Param-NonAscii"), "=?base64?SGVsbG8sIOS4lueVjA==?=");
    EXPECT_EQ(sent.field("Mcp-Param-LookAlike"), "=?base64?PT9iYXNlNjQ/YWJjPz0=?=");
    EXPECT_EQ(sent.field("Mcp-Param-Inner"), "us west 1");
    EXPECT_FALSE(sent.field("Mcp-Param-Unmarked"));
    EXPECT_EQ(sent.field("Mcp-Method"), "tools/call");
    EXPECT_EQ(sent.field("Mcp-Name"), "mirror");
    EXPECT_EQ(sent.field("MCP-Protocol-Version"), modern);
}

}  // namespace
