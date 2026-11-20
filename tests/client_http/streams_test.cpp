// The event streams of the Streamable HTTP transport, against a scripted peer: answers that are streams, requests of
// the server on a call's stream and on the stream of the connection, sessions the server forgets and the request that
// ends one, the stream of the connection, and the resumption of a stream that ends before its answer. Waits are on the
// peer's gates and on what the transport delivers; a test measures time only where a least wait is what it shows.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "client_http/streamable.h"
#include "neomcp/client.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/http.h"
#include "neomcp/profile.h"
#include "rig.h"
#include "scripted_http_server.h"
#include "support.h"

namespace {

using namespace std::chrono_literals;
using http_rig::Delivered;
using http_rig::Driven;
using http_rig::endpoint;
using http_rig::events_of;
using http_rig::messages_of;
using http_test::Gate;
using http_test::json;
using http_test::Reply;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::ClientErrorKind;
using neomcp::HttpHeader;
using neomcp::OutgoingKind;
using neomcp::ResponseHead;
using neomcp::SessionExpired;
using neomcp::StreamEnded;
using neomcp::StreamLost;
using neomcp::connection_unit;

constexpr std::string_view legacy = "2025-11-25";
constexpr std::string_view modern = "2026-07-28";
constexpr const char* session_field = "MCP-Session-Id";
constexpr const char* version_field = "MCP-Protocol-Version";
constexpr const char* last_event_field = "Last-Event-ID";
// A bound for a wait that shows that something does not come.
constexpr auto absence = 300ms;

neomcp::profile::ProfilePtr profile_of(std::string_view revision) { return neomcp::profile::builtin(revision); }

// ---- What a peer writes

std::string event(const json& message) { return "data: " + message.dump() + "\n\n"; }
std::string event(std::string_view id, const json& message) {
    return "id: " + std::string(id) + "\ndata: " + message.dump() + "\n\n";
}
// An event that only names its id (and the advised wait), as a server writes one before anything else.
std::string priming(std::string_view id, std::optional<int> retry_ms = std::nullopt) {
    std::string text = "id: " + std::string(id) + "\n";
    if (retry_ms) text += "retry: " + std::to_string(*retry_ms) + "\n";
    return text + "data: \n\n";
}
std::string stream_head(http_test::Fields fields = {}) {
    fields.emplace_back("Content-Type", "text/event-stream");
    return http_test::head(200, fields, std::nullopt);
}

json notification(std::string method, json params = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}
json result_with_id(json id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}
json tool_text(std::string text) {
    return json{{"content", json::array({json{{"type", "text"}, {"text", std::move(text)}}})}};
}

bool is_answer(const Request& request) {
    const json message = request.message();
    return message.is_object() && !message.contains("method") &&
           (message.contains("result") || message.contains("error"));
}

std::vector<Request> requests_of(const ScriptedHttpServer& server, std::string_view http_method,
                                 std::string_view rpc_method = {}) {
    std::vector<Request> found;
    for (const auto& request : server.requests()) {
        if (request.method != http_method) continue;
        if (!rpc_method.empty() && request.rpc_method() != rpc_method) continue;
        found.push_back(request);
    }
    return found;
}

// A peer of a revision that opens with a handshake: the N-th handshake's answer names the session "session-N";
// everything else goes to the test's handler, and what it leaves (an empty optional) is answered as such a server
// does: notifications and answers with 202, other requests with an empty result, a GET with 405, a DELETE with 200.
class SessionPeer {
public:
    using Handler = std::function<std::optional<Reply>(const Request&)>;

    explicit SessionPeer(Handler handler, json capabilities = json::object())
        : handler_(std::move(handler)),
          capabilities_(std::move(capabilities)),
          server_([this](const Request& request) { return reply(request); }) {}

    std::string url() const { return server_.url(); }
    ScriptedHttpServer& server() { return server_; }
    std::size_t handshakes() const { return handshakes_.load(); }

private:
    Reply reply(const Request& request) {
        if (request.method == "POST" && request.rpc_method() == "initialize") {
            const std::string session = "session-" + std::to_string(++handshakes_);
            json result{{"protocolVersion", std::string(legacy)},
                        {"capabilities", capabilities_},
                        {"serverInfo", {{"name", "scripted"}, {"version", "1"}}}};
            return http_test::json_reply(200, http_test::result_for(request, std::move(result)),
                                         {{session_field, session}});
        }
        if (handler_) {
            if (auto chosen = handler_(request)) return std::move(*chosen);
        }
        if (request.method == "GET") return http_test::empty_reply(405);
        if (request.method == "DELETE") return http_test::empty_reply(200);
        if (request.rpc_id().is_null() || is_answer(request)) return http_test::empty_reply(202);
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    }

    Handler handler_;
    json capabilities_;
    std::atomic<std::size_t> handshakes_{0};
    ScriptedHttpServer server_;  // last: it goes first, so that no request reaches the members after them
};

// ---- The transport alone

neomcp::detail::http_client::HttpTransportSettings legacy_settings() {
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {profile_of(legacy)};
    return settings;
}

const std::vector<HttpHeader> legacy_fields{{version_field, std::string(legacy)}};

// Opens the session of the peer on unit 100, as the engine's handshake would.
void handshake(Driven& driven) {
    driven.post(100, R"({"jsonrpc":"2.0","id":100,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})",
                {}, OutgoingKind::request, profile_of(legacy));
    driven.recorder().until_end(100);
}

std::string call_line(int id, std::string_view method = "tools/call") {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", json::object()}}.dump();
}

// Waits until `unit` received a message.
bool message_on(Driven& driven, neomcp::UnitId unit) {
    return driven.recorder().wait(
        [unit](const std::vector<Delivered>& seen) {
            for (const auto& one : seen) {
                const auto* received = std::get_if<http_rig::Received>(&one);
                if (received != nullptr && received->unit == unit) return true;
            }
            return false;
        });
}

template <class E>
bool has(const std::vector<Delivered>& seen) {
    return !events_of<E>(seen).empty();
}

bool connection_lost(const std::vector<Delivered>& seen) {
    for (const auto& one : seen) {
        const auto* lost = http_rig::event_of<StreamLost>(one);
        if (lost != nullptr && std::get<http_rig::Event>(one).unit == connection_unit) return true;
    }
    return false;
}

// -- the stream of the connection

TEST(ClientHttp, TheConnectionStreamIsAGetWithTheSessionAndTheRevision) {
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([hold](const Request& request) -> std::optional<Reply> {
        if (request.method != "GET") return std::nullopt;
        Reply reply;
        reply.send(stream_head());
        reply.send(event(notification("notifications/tools/list_changed")));
        reply.wait(hold);
        return reply;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(message_on(driven, connection_unit));
    const auto messages = messages_of(driven.recorder().of(connection_unit));
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(json::parse(messages.front())["method"], "notifications/tools/list_changed");
    const auto gets = requests_of(peer.server(), "GET");
    ASSERT_EQ(gets.size(), 1u);
    EXPECT_EQ(gets[0].target, "/mcp");
    EXPECT_EQ(gets[0].field("Accept"), "text/event-stream");
    EXPECT_EQ(gets[0].fields_named(session_field), std::vector<std::string>{"session-1"});
    EXPECT_EQ(gets[0].field(version_field), std::string(legacy));
    EXPECT_FALSE(gets[0].field(last_event_field));
    EXPECT_FALSE(gets[0].field("Content-Type"));
    EXPECT_TRUE(gets[0].body.empty());
}

TEST(ClientHttp, AConnectionStreamNotOfferedIsNeverAskedForAgain) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET") return http_test::empty_reply(405);
        return std::nullopt;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(driven.recorder().wait(connection_lost));
    driven.open_stream(legacy_fields, profile_of(legacy));
    driven.open_stream(legacy_fields, profile_of(legacy));
    driven.post(2, call_line(2, "tools/list"), legacy_fields, OutgoingKind::request, profile_of(legacy));
    driven.recorder().until_end(2);
    EXPECT_FALSE(peer.server().wait_for(4, absence));  // the handshake, one GET, the request
    EXPECT_EQ(requests_of(peer.server(), "GET").size(), 1u);
}

TEST(ClientHttp, ANotFoundToAGetWithoutASessionMeansNotOffered) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.method == "GET") return http_test::empty_reply(404);
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    Driven driven(endpoint(server.url()), legacy_settings());
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(driven.recorder().wait(connection_lost));
    EXPECT_FALSE(server.requests().front().field(session_field));
    EXPECT_TRUE(events_of<SessionExpired>(driven.recorder().all()).empty());
    driven.open_stream(legacy_fields, profile_of(legacy));
    driven.post(2, call_line(2, "tools/list"), legacy_fields, OutgoingKind::request, profile_of(legacy));
    driven.recorder().until_end(2);
    EXPECT_FALSE(server.wait_for(3, absence));
    EXPECT_EQ(requests_of(server, "GET").size(), 1u);
}

TEST(ClientHttp, ANotFoundToAGetWithItsSessionMeansNotOfferedAndKeepsTheSession) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET") return http_test::empty_reply(404);
        return std::nullopt;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(driven.recorder().wait(connection_lost));
    EXPECT_TRUE(events_of<SessionExpired>(driven.recorder().all()).empty());
    driven.open_stream(legacy_fields, profile_of(legacy));
    driven.post(2, call_line(2, "tools/list"), legacy_fields, OutgoingKind::request, profile_of(legacy));
    driven.recorder().until_end(2);
    EXPECT_EQ(requests_of(peer.server(), "GET").size(), 1u);
    const auto lists = requests_of(peer.server(), "POST", "tools/list");
    ASSERT_EQ(lists.size(), 1u);
    EXPECT_EQ(lists[0].fields_named(session_field), std::vector<std::string>{"session-1"});
}

TEST(ClientHttp, ALostConnectionStreamIsOpenedAgainWithTheLastEventId) {
    auto hold = std::make_shared<Gate>();
    auto gets = std::make_shared<std::atomic<int>>(0);
    SessionPeer peer([hold, gets](const Request& request) -> std::optional<Reply> {
        if (request.method != "GET") return std::nullopt;
        Reply reply;
        reply.send(stream_head());
        if (++*gets == 1) {
            reply.send("retry: 0\n" + event("e1", notification("notifications/tools/list_changed")));
            return reply;  // the stream ends
        }
        reply.wait(hold);
        return reply;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(peer.server().wait_for(3));
    const auto requests = requests_of(peer.server(), "GET");
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_FALSE(requests[0].field(last_event_field));
    EXPECT_EQ(requests[1].field(last_event_field), "e1");
    EXPECT_EQ(requests[1].fields_named(session_field), std::vector<std::string>{"session-1"});
    EXPECT_EQ(requests[1].field(version_field), std::string(legacy));
    EXPECT_EQ(messages_of(driven.recorder().of(connection_unit)).size(), 1u);
    EXPECT_FALSE(connection_lost(driven.recorder().all()));
}

TEST(ClientHttp, ReopensOfTheConnectionStreamThatFailAreBounded) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET") return http_test::empty_reply(409);
        return std::nullopt;
    });
    auto described = endpoint(peer.url());
    described.retry_delay = 0ms;
    described.max_resumptions = 2;
    Driven driven(described, legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(driven.recorder().wait(connection_lost));
    EXPECT_FALSE(peer.server().wait_for(5, absence));  // the handshake and three GETs
    EXPECT_EQ(requests_of(peer.server(), "GET").size(), 3u);
}

// A server that ends every stream at once, with nothing on it and an advice to wait nothing, is asked again at the
// pace of retry_delay, not without pause.
TEST(ClientHttp, AConnectionStreamThatCarriedNothingIsNotOpenedAgainSoonerThanTheWait) {
    auto hold = std::make_shared<Gate>();
    auto gets = std::make_shared<std::atomic<int>>(0);
    auto ended = std::make_shared<std::atomic<std::int64_t>>(0);
    SessionPeer peer([=](const Request& request) -> std::optional<Reply> {
        if (request.method != "GET") return std::nullopt;
        Reply reply;
        reply.send(stream_head());
        if (++*gets == 1) {
            reply.send(priming("e1", 0));
            reply.run([ended] { ended->store(std::chrono::steady_clock::now().time_since_epoch().count()); });
            return reply;
        }
        reply.wait(hold);
        return reply;
    });
    auto described = endpoint(peer.url());
    described.retry_delay = 300ms;
    Driven driven(described, legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(peer.server().wait_for(3));
    const auto requests = requests_of(peer.server(), "GET");
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1].field(last_event_field), "e1");
    const std::chrono::steady_clock::time_point closed{std::chrono::steady_clock::duration(ended->load())};
    EXPECT_GE(requests[1].arrived - closed, 300ms);
}

TEST(ClientHttp, AConnectionStreamThatFallsSilentIsOpenedAgain) {
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([hold](const Request& request) -> std::optional<Reply> {
        if (request.method != "GET") return std::nullopt;
        Reply reply;
        reply.send(stream_head());
        reply.send(": open\n\n");
        reply.wait(hold);
        return reply;
    });
    auto described = endpoint(peer.url());
    described.stream_idle_timeout = 200ms;
    described.retry_delay = 0ms;
    Driven driven(described, legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(peer.server().wait_for(3));
    EXPECT_GE(requests_of(peer.server(), "GET").size(), 2u);
}

TEST(ClientHttp, RequestsWaitForTheHeadOfTheConnectionStream) {
    auto head = std::make_shared<Gate>();
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([head, hold](const Request& request) -> std::optional<Reply> {
        if (request.method != "GET") return std::nullopt;
        Reply reply;
        reply.wait(head);
        reply.send(stream_head());
        reply.wait(hold);
        return reply;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.open_stream(legacy_fields, profile_of(legacy));
    ASSERT_TRUE(peer.server().wait_for(2));
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    // A notification goes at once.
    driven.post(3, notification("notifications/initialized").dump(), legacy_fields, OutgoingKind::notification,
                profile_of(legacy));
    driven.recorder().until_end(3);
    EXPECT_FALSE(peer.server().wait_for(4, absence));
    EXPECT_TRUE(requests_of(peer.server(), "POST", "tools/call").empty());
    head->open();
    const auto seen = driven.recorder().until_end(2);
    EXPECT_EQ(messages_of(seen).size(), 1u);
    EXPECT_EQ(requests_of(peer.server(), "POST", "tools/call").size(), 1u);
}

// -- resuming a request's stream

// A peer whose tools/call answer is an event stream that names an event id and then ends without the answer; a GET
// that names that id gets the answer on a stream that stays open. The time the first stream ended is noted.
struct BrokenStreamPeer {
    explicit BrokenStreamPeer(std::optional<int> retry_ms)
        : peer([this, retry_ms](const Request& request) -> std::optional<Reply> {
              if (request.method == "POST" && request.rpc_method() == "tools/call") {
                  Reply reply;
                  reply.send(stream_head());
                  reply.send(priming("e1", retry_ms));
                  reply.run([this] {
                      std::lock_guard lock(mutex);
                      ended = std::chrono::steady_clock::now();
                  });
                  return reply;
              }
              if (request.method == "GET") {
                  Reply reply;
                  reply.send(stream_head());
                  reply.send(event("e2", result_with_id(2, tool_text("resumed"))));
                  reply.wait(hold);
                  return reply;
              }
              return std::nullopt;
          }) {}

    std::chrono::steady_clock::time_point ended_at() {
        std::lock_guard lock(mutex);
        return ended;
    }

    std::shared_ptr<Gate> hold = std::make_shared<Gate>();
    std::mutex mutex;
    std::chrono::steady_clock::time_point ended{};
    SessionPeer peer;
};

void expect_resumed(BrokenStreamPeer& broken, Driven& driven, std::chrono::milliseconds least) {
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    ASSERT_TRUE(message_on(driven, 2));
    const auto seen = driven.recorder().of(2);
    // The break is not told: one head, the answer, no end.
    EXPECT_EQ(events_of<ResponseHead>(seen).size(), 1u);
    EXPECT_FALSE(has<StreamEnded>(seen));
    EXPECT_FALSE(has<StreamLost>(seen));
    ASSERT_EQ(messages_of(seen).size(), 1u);
    EXPECT_EQ(json::parse(messages_of(seen).front())["result"], tool_text("resumed"));
    const auto gets = requests_of(broken.peer.server(), "GET");
    ASSERT_EQ(gets.size(), 1u);
    EXPECT_EQ(gets[0].field(last_event_field), "e1");
    EXPECT_EQ(gets[0].fields_named(session_field), std::vector<std::string>{"session-1"});
    EXPECT_EQ(gets[0].field(version_field), std::string(legacy));
    EXPECT_EQ(gets[0].field("Accept"), "text/event-stream");
    EXPECT_GE(gets[0].arrived - broken.ended_at(), least);
    // The answer arrived: the engine ends the unit, and the resumed stream with it.
    driven.end(2);
}

TEST(ClientHttp, AStreamThatEndsBeforeItsAnswerIsResumedAfterTheAdvisedWait) {
    BrokenStreamPeer broken(300);
    Driven driven(endpoint(broken.peer.url()), legacy_settings());
    expect_resumed(broken, driven, 300ms);
}

TEST(ClientHttp, WithoutAdviceAStreamIsResumedAfterTheDefaultWait) {
    BrokenStreamPeer broken(std::nullopt);
    Driven driven(endpoint(broken.peer.url()), legacy_settings());
    expect_resumed(broken, driven, neomcp::HttpClientDefaults::retry_delay);
}

TEST(ClientHttp, AnAdvisedWaitOverItsBoundEndsTheRequest) {
    BrokenStreamPeer broken(5000);
    auto described = endpoint(broken.peer.url());
    described.max_retry_delay = 1000ms;
    Driven driven(described, legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    const auto seen = driven.recorder().until_end(2);
    EXPECT_TRUE(has<StreamLost>(seen));
    EXPECT_FALSE(has<StreamEnded>(seen));
    EXPECT_FALSE(broken.peer.server().wait_for(3, absence));
    EXPECT_TRUE(requests_of(broken.peer.server(), "GET").empty());
}

// Every resumption brings only a new event id, never a message: max_resumptions of them, then the request is lost.
TEST(ClientHttp, ResumptionsWithoutAMessageAreBounded) {
    auto ids = std::make_shared<std::atomic<int>>(1);
    SessionPeer peer([ids](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET" || request.rpc_method() == "tools/call") {
            Reply reply;
            reply.send(stream_head());
            reply.send(priming("e" + std::to_string((*ids)++), 0));
            return reply;
        }
        return std::nullopt;
    });
    auto described = endpoint(peer.url());
    described.max_resumptions = 2;
    Driven driven(described, legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    const auto seen = driven.recorder().until_end(2);
    EXPECT_TRUE(has<StreamLost>(seen));
    EXPECT_FALSE(peer.server().wait_for(5, absence));  // the handshake, the call, two GETs
    const auto gets = requests_of(peer.server(), "GET");
    ASSERT_EQ(gets.size(), 2u);
    EXPECT_EQ(gets[0].field(last_event_field), "e1");
    EXPECT_EQ(gets[1].field(last_event_field), "e2");
}

TEST(ClientHttp, AMessageStartsTheBoundOfResumptionsAgain) {
    auto gets = std::make_shared<std::atomic<int>>(0);
    SessionPeer peer([gets](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() == "tools/call") {
            Reply reply;
            reply.send(stream_head());
            reply.send(priming("e1", 0));
            return reply;
        }
        if (request.method == "GET") {
            Reply reply;
            reply.send(stream_head());
            if (++*gets == 1) {
                reply.send(event("e2", notification("notifications/message", {{"level", "info"}, {"data", "x"}})));
            } else {
                reply.send(priming("e3", 0));
            }
            return reply;
        }
        return std::nullopt;
    });
    auto described = endpoint(peer.url());
    described.max_resumptions = 1;
    Driven driven(described, legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    const auto seen = driven.recorder().until_end(2);
    EXPECT_TRUE(has<StreamLost>(seen));
    EXPECT_EQ(messages_of(seen).size(), 1u);
    EXPECT_FALSE(peer.server().wait_for(5, absence));
    EXPECT_EQ(requests_of(peer.server(), "GET").size(), 2u);
}

TEST(ClientHttp, AStreamWithoutAnEventIdIsNotResumed) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() != "tools/call") return std::nullopt;
        Reply reply;
        reply.send(stream_head());
        reply.send("retry: 0\n: no id\n\n");
        return reply;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    const auto seen = driven.recorder().until_end(2);
    EXPECT_TRUE(has<StreamEnded>(seen));
    EXPECT_FALSE(peer.server().wait_for(3, absence));
    EXPECT_TRUE(requests_of(peer.server(), "GET").empty());
}

TEST(ClientHttp, UnderARevisionThatResumesNoStreamAStreamIsNotResumed) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.method == "GET") return http_test::empty_reply(405);
        Reply reply;
        reply.send(stream_head());
        reply.send(priming("e1", 0));
        return reply;
    });
    Driven driven(endpoint(server.url()));
    driven.post(2, call_line(2), {{version_field, std::string(modern)}}, OutgoingKind::request, profile_of(modern));
    const auto seen = driven.recorder().until_end(2);
    EXPECT_TRUE(has<StreamEnded>(seen));
    EXPECT_FALSE(server.wait_for(2, absence));
    EXPECT_TRUE(requests_of(server, "GET").empty());
}

TEST(ClientHttp, AResumedStreamThatFallsSilentIsResumedAgain) {
    auto hold = std::make_shared<Gate>();
    auto gets = std::make_shared<std::atomic<int>>(0);
    SessionPeer peer([hold, gets](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() == "tools/call") {
            Reply reply;
            reply.send(stream_head());
            reply.send(priming("e1", 0));
            return reply;
        }
        if (request.method == "GET") {
            Reply reply;
            reply.send(stream_head());
            if (++*gets == 1) {
                reply.send(priming("e2", 0));
            } else {
                reply.send(event("e3", result_with_id(2, tool_text("resumed twice"))));
            }
            reply.wait(hold);
            return reply;
        }
        return std::nullopt;
    });
    auto described = endpoint(peer.url());
    described.stream_idle_timeout = 200ms;
    Driven driven(described, legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    ASSERT_TRUE(message_on(driven, 2));
    const auto gets_seen = requests_of(peer.server(), "GET");
    ASSERT_EQ(gets_seen.size(), 2u);
    EXPECT_EQ(gets_seen[1].field(last_event_field), "e2");
    EXPECT_FALSE(has<StreamLost>(driven.recorder().of(2)));
    driven.end(2);
}

TEST(ClientHttp, AResumptionAnsweredWithoutAStreamEndsTheRequest) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() == "tools/call") {
            Reply reply;
            reply.send(stream_head());
            reply.send(priming("e1", 0));
            return reply;
        }
        if (request.method == "GET") return http_test::empty_reply(500);
        return std::nullopt;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    const auto seen = driven.recorder().until_end(2);
    EXPECT_TRUE(has<StreamLost>(seen));
    EXPECT_EQ(events_of<ResponseHead>(seen).size(), 1u);  // the POST's; the GET's 500 is not told as the answer
    EXPECT_EQ(requests_of(peer.server(), "GET").size(), 1u);
}

// -- sessions

TEST(ClientHttp, ARequestOfASessionTheServerForgotEndsWithSessionExpired) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() == "tools/call") {
            return http_test::json_reply(404, http_test::error_for(request, -32001, "Session not found"));
        }
        return std::nullopt;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2), legacy_fields, OutgoingKind::request, profile_of(legacy));
    const auto seen = driven.recorder().until_end(2);
    ASSERT_TRUE(driven.recorder().wait(
        [](const std::vector<Delivered>& all) { return has<SessionExpired>(all); }));
    const auto unit = driven.recorder().of(2);
    ASSERT_EQ(events_of<SessionExpired>(unit).size(), 1u);
    EXPECT_TRUE(events_of<SessionExpired>(unit).front().current);
    EXPECT_FALSE(has<ResponseHead>(unit));
    EXPECT_FALSE(has<StreamEnded>(unit));
    EXPECT_TRUE(messages_of(unit).empty());
    driven.post(3, call_line(3, "tools/list"), legacy_fields, OutgoingKind::request, profile_of(legacy));
    driven.recorder().until_end(3);
    const auto lists = requests_of(peer.server(), "POST", "tools/list");
    ASSERT_EQ(lists.size(), 1u);
    EXPECT_FALSE(lists[0].field(session_field));
}

class EndOfSession : public ::testing::TestWithParam<int> {};

TEST_P(EndOfSession, ClosingEndsTheSessionWithADeleteWhateverItsAnswer) {
    const int status = GetParam();
    SessionPeer peer([status](const Request& request) -> std::optional<Reply> {
        if (request.method == "DELETE") return http_test::empty_reply(status);
        return std::nullopt;
    });
    Driven driven(endpoint(peer.url()), legacy_settings());
    handshake(driven);
    driven.close();
    const auto deletes = requests_of(peer.server(), "DELETE");
    ASSERT_EQ(deletes.size(), 1u);
    EXPECT_EQ(deletes[0].fields_named(session_field), std::vector<std::string>{"session-1"});
    EXPECT_EQ(deletes[0].field(version_field), std::string(legacy));
    EXPECT_TRUE(deletes[0].body.empty());
}

INSTANTIATE_TEST_SUITE_P(ClientHttp, EndOfSession, ::testing::Values(200, 204, 404, 405),
                         [](const auto& info) { return "Status_" + std::to_string(info.param); });

TEST(ClientHttp, TheRequestThatEndsASessionIsBounded) {
    auto never = std::make_shared<Gate>();
    SessionPeer peer([never](const Request& request) -> std::optional<Reply> {
        if (request.method != "DELETE") return std::nullopt;
        Reply reply;
        reply.wait(never);
        return reply;
    });
    auto described = endpoint(peer.url());
    described.delete_timeout = 200ms;
    Driven driven(described, legacy_settings());
    handshake(driven);
    const auto started = std::chrono::steady_clock::now();
    driven.close();
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(requests_of(peer.server(), "DELETE").size(), 1u);
    EXPECT_GE(took, 200ms);
    EXPECT_LT(took, 10s);
}

TEST(ClientHttp, WithoutASessionClosingSendsNoDelete) {
    ScriptedHttpServer server([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    Driven driven(endpoint(server.url()), legacy_settings());
    handshake(driven);
    driven.close();
    EXPECT_TRUE(requests_of(server, "DELETE").empty());
}

// ---- The client

neomcp::Result<neomcp::Client> open_legacy(const SessionPeer& peer, neomcp::ClientOptions options) {
    return neomcp::Client::open(std::move(options), endpoint(peer.url()));
}

neomcp::ClientOptions legacy_options() { return client_test::only({legacy}); }

std::string text_of(const neomcp::ToolCall& call) {
    for (const auto& block : call.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

TEST(ClientHttp, AStreamAnswerCarriesProgressAndLogsBeforeTheResult) {
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([hold](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() != "tools/call") return std::nullopt;
        const json token = request.message()["params"]["_meta"]["progressToken"];
        Reply reply;
        reply.send(stream_head());
        reply.send(event(notification("notifications/progress", {{"progressToken", token}, {"progress", 1}})));
        reply.send(event(notification("notifications/message", {{"level", "info"}, {"data", "working"}})));
        reply.send(event(http_test::result_for(request, tool_text("done"))));
        reply.wait(hold);  // the server keeps the stream open; the client ends it
        return reply;
    });
    std::atomic<int> logs{0};
    auto options = legacy_options();
    options.on_log = [&logs](const neomcp::LogEvent&) { ++logs; };
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client) << client.error().message;
    std::atomic<int> progress{0};
    neomcp::RequestOptions call;
    call.on_progress = [&progress](const neomcp::Progress&) { ++progress; };
    const auto done = client->call_tool("work", json::object(), call);
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_EQ(text_of(*done), "done");
    EXPECT_EQ(progress.load(), 1);
    EXPECT_EQ(logs.load(), 1);
    EXPECT_TRUE(requests_of(peer.server(), "GET").empty());  // nothing needs the stream of the connection
    EXPECT_TRUE(client->close());
}

// Whether a timer of the client's wait for a call's answer is set.
bool wait_clock_runs(const client_test::ManualTimers& timers) {
    for (const auto& one : timers.started()) {
        const auto* deadline = std::get_if<neomcp::Deadline>(&one.purpose);
        if (deadline == nullptr || one.cancelled || one.fired) continue;
        const bool of_a_call = std::holds_alternative<neomcp::RequestRef>(deadline->owner);
        if (deadline->kind == neomcp::DeadlineKind::wait && of_a_call) return true;
    }
    return false;
}

TEST(ClientHttp, AServerRequestOnACallsStreamIsAnsweredByPostWhileTheCallsClockWaits) {
    auto answered = std::make_shared<Gate>();
    auto result = std::make_shared<Gate>();
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([=](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() == "tools/call") {
            const json ask = json::parse(R"({"jsonrpc":"2.0","id":"ask-1","method":"elicitation/create",
                "params":{"message":"Your name?",
                "requestedSchema":{"type":"object","properties":{"name":{"type":"string"}}}}})");
            Reply reply;
            reply.send(stream_head());
            reply.send(event(ask));
            reply.wait(result);
            reply.send(event(http_test::result_for(request, tool_text("named"))));
            reply.wait(hold);
            return reply;
        }
        if (is_answer(request)) {
            answered->open();
            return http_test::empty_reply(202);
        }
        return std::nullopt;
    });
    auto timers = std::make_shared<client_test::ManualTimers>();
    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    auto options = legacy_options();
    options.timers = timers;
    options.handlers.form([entered, release](const neomcp::ElicitationForm&) {
        entered->open();
        release->wait(client_test::wait_bound);
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"name", "Ada"}}};
    });
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client) << client.error().message;
    auto call = std::async(std::launch::async, [&client] { return client->call_tool("ask"); });
    ASSERT_TRUE(entered->wait(client_test::wait_bound));
    // The handler answers the server: the call's clock waits meanwhile.
    EXPECT_FALSE(wait_clock_runs(*timers));
    release->open();
    ASSERT_TRUE(answered->wait(client_test::wait_bound));
    EXPECT_TRUE(client_test::eventually([&] { return wait_clock_runs(*timers); }));
    result->open();
    const auto done = call.get();
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_EQ(text_of(*done), "named");
    std::vector<Request> answers;
    for (const auto& request : peer.server().requests()) {
        if (is_answer(request)) answers.push_back(request);
    }
    ASSERT_EQ(answers.size(), 1u);
    const json answer = answers[0].message();
    EXPECT_EQ(answer["id"], "ask-1");
    EXPECT_EQ(answer["result"]["action"], "accept");
    EXPECT_EQ(answer["result"]["content"], (json{{"name", "Ada"}}));
    EXPECT_EQ(answers[0].fields_named(session_field), std::vector<std::string>{"session-1"});
    EXPECT_EQ(answers[0].field(version_field), std::string(legacy));
    EXPECT_TRUE(client->close());
}

// As the runner's server asks: outside the call's own stream, on the stream of the connection, which the client
// opened because it has a handler, before the call went out.
TEST(ClientHttp, AServerRequestOnTheConnectionStreamIsAnsweredByPost) {
    auto call_arrived = std::make_shared<Gate>();
    auto answered = std::make_shared<Gate>();
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([=](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET") {
            const json ask = json::parse(R"({"jsonrpc":"2.0","id":7,"method":"elicitation/create",
                "params":{"message":"Defaults?","requestedSchema":{"type":"object",
                "properties":{"name":{"type":"string","default":"John Doe"}}}}})");
            Reply reply;
            reply.send(stream_head());
            reply.wait(call_arrived);
            reply.send(event(ask));
            reply.wait(hold);
            return reply;
        }
        if (request.rpc_method() == "tools/call") {
            call_arrived->open();
            Reply reply;
            reply.send(stream_head());
            reply.wait(answered);
            reply.send(event(http_test::result_for(request, tool_text("answered"))));
            reply.wait(hold);
            return reply;
        }
        if (is_answer(request)) {
            answered->open();
            return http_test::empty_reply(202);
        }
        return std::nullopt;
    });
    auto timers = std::make_shared<client_test::ManualTimers>();
    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    auto options = legacy_options();
    options.timers = timers;
    options.handlers.form([entered, release](const neomcp::ElicitationForm& form) {
        entered->open();
        release->wait(client_test::wait_bound);
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, form.prefilled()};
    });
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client) << client.error().message;
    auto call = std::async(std::launch::async, [&client] { return client->call_tool("ask"); });
    ASSERT_TRUE(entered->wait(client_test::wait_bound));
    // The stream does not say which call the request belongs to: the clock of every pending call waits.
    EXPECT_FALSE(wait_clock_runs(*timers));
    release->open();
    const auto done = call.get();
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_EQ(text_of(*done), "answered");
    const auto requests = peer.server().requests();
    std::vector<std::string> order;
    for (const auto& request : requests) {
        order.push_back(request.method == "GET" ? "GET" : (is_answer(request) ? "answer" : request.rpc_method()));
    }
    EXPECT_EQ(order,
              (std::vector<std::string>{"initialize", "notifications/initialized", "GET", "tools/call", "answer"}));
    for (const auto& request : requests) {
        if (!is_answer(request)) continue;
        EXPECT_EQ(request.message()["id"], 7);
        EXPECT_EQ(request.message()["result"]["content"], (json{{"name", "John Doe"}}));
    }
    EXPECT_TRUE(client->close());
}

TEST(ClientHttp, TheConnectionStreamIsOpenedOnlyWhereTheClientNeedsIt) {
    struct Case {
        const char* name;
        bool handler;
        bool log_level;
        neomcp::ConnectionStreamMode mode;
        std::size_t gets;
    };
    const std::vector<Case> cases{
        {"nothing", false, false, neomcp::ConnectionStreamMode::when_needed, 0},
        {"a handler", true, false, neomcp::ConnectionStreamMode::when_needed, 1},
        {"a log level", false, true, neomcp::ConnectionStreamMode::when_needed, 1},
        {"never", true, true, neomcp::ConnectionStreamMode::never, 0},
        {"always", false, false, neomcp::ConnectionStreamMode::always, 1},
    };
    for (const Case& one : cases) {
        auto hold = std::make_shared<Gate>();
        SessionPeer peer([hold](const Request& request) -> std::optional<Reply> {
            if (request.method != "GET") return std::nullopt;
            Reply reply;
            reply.send(stream_head());
            reply.wait(hold);
            return reply;
        });
        auto options = legacy_options();
        if (one.handler) {
            options.handlers.form([](const neomcp::ElicitationForm&) {
                return neomcp::ElicitationAnswer{neomcp::ElicitationAction::decline, json::object()};
            });
        }
        if (one.log_level) options.log_level = neomcp::LogLevel::info;
        auto described = endpoint(peer.url());
        described.connection_stream = one.mode;
        auto client = neomcp::Client::open(std::move(options), std::move(described));
        ASSERT_TRUE(client) << one.name << ": " << client.error().message;
        // A request after the stream was asked for goes out once its head arrived (or nothing was asked for).
        ASSERT_TRUE(client->request("tools/list")) << one.name;
        EXPECT_EQ(requests_of(peer.server(), "GET").size(), one.gets) << one.name;
        EXPECT_TRUE(client->close()) << one.name;
    }
}

TEST(ClientHttp, ASessionTheServerForgotIsOpenedAgainAndTheCallSentOnceMore) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() != "tools/call") return std::nullopt;
        if (request.field(session_field) == std::optional<std::string>("session-1")) {
            return http_test::json_reply(404, http_test::error_for(request, -32001, "Session not found"));
        }
        return http_test::json_reply(200, http_test::result_for(request, tool_text("again")));
    });
    std::atomic<int> renewed{0};
    auto options = legacy_options();
    options.on_diagnostic = [&renewed](const neomcp::ClientDiagnostic& diagnostic) {
        const auto* engine = std::get_if<neomcp::Diagnostic>(&diagnostic);
        if (engine != nullptr && engine->kind == neomcp::DiagnosticKind::session_renewed) ++renewed;
    };
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client) << client.error().message;
    const auto done = client->call_tool("t");
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_EQ(text_of(*done), "again");
    EXPECT_EQ(renewed.load(), 1);
    const auto handshakes = requests_of(peer.server(), "POST", "initialize");
    ASSERT_EQ(handshakes.size(), 2u);
    EXPECT_FALSE(handshakes[1].field(session_field));
    const auto calls = requests_of(peer.server(), "POST", "tools/call");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].fields_named(session_field), std::vector<std::string>{"session-2"});
    EXPECT_NE(calls[0].rpc_id(), calls[1].rpc_id());
    EXPECT_EQ(calls[0].message()["params"], calls[1].message()["params"]);
    EXPECT_TRUE(client->close());
}

TEST(ClientHttp, ACallWhoseSessionIsForgottenTwiceFails) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() != "tools/call") return std::nullopt;
        return http_test::json_reply(404, http_test::error_for(request, -32001, "Session not found"));
    });
    auto options = legacy_options();
    options.request_timeout = 5s;  // bounds a client that would send it forever
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client) << client.error().message;
    const auto done = client->call_tool("t");
    ASSERT_FALSE(done);
    EXPECT_EQ(done.error().kind, ClientErrorKind::http_status) << done.error().message;
    EXPECT_EQ(requests_of(peer.server(), "POST", "tools/call").size(), 2u);
    EXPECT_TRUE(client->close());
}

TEST(ClientHttp, AListenerOfTheConnectionIsReestablishedUnderANewSession) {
    auto hold = std::make_shared<Gate>();
    const json capabilities{{"tools", {{"listChanged", true}}}, {"resources", {{"subscribe", true}}}};
    SessionPeer peer(
        [hold](const Request& request) -> std::optional<Reply> {
            const bool first_session = request.field(session_field) == std::optional<std::string>("session-1");
            if (request.method == "GET") {
                // The new session's stream carries a change; the first one's stays quiet.
                Reply reply;
                reply.send(stream_head());
                if (!first_session) reply.send(event(notification("notifications/tools/list_changed")));
                reply.wait(hold);
                return reply;
            }
            // A call finds the first session gone.
            if (request.rpc_method() == "tools/call") {
                if (first_session) {
                    return http_test::json_reply(404, http_test::error_for(request, -32001, "Session not found"));
                }
                return http_test::json_reply(200, http_test::result_for(request, tool_text("again")));
            }
            return std::nullopt;
        },
        capabilities);
    auto client = open_legacy(peer, legacy_options());
    ASSERT_TRUE(client) << client.error().message;
    neomcp::ChangeFilter filter;
    filter.tools_list = true;
    filter.resources = {"res://watched"};
    auto subscription = client->listen(filter);
    ASSERT_TRUE(subscription) << subscription.error().message;
    ASSERT_EQ(requests_of(peer.server(), "POST", "resources/subscribe").size(), 1u);
    const auto called = client->call_tool("t");
    ASSERT_TRUE(called) << called.error().message;
    EXPECT_EQ(peer.handshakes(), 2u);
    const auto change = subscription->next();
    ASSERT_TRUE(change) << change.error().message;
    EXPECT_EQ(change->kind, neomcp::ChangeKind::tools_list);
    // The stream of the connection and the watched resource, again under the new session.
    ASSERT_TRUE(client_test::eventually([&] { return requests_of(peer.server(), "GET").size() == 2u; }));
    EXPECT_EQ(requests_of(peer.server(), "GET")[1].fields_named(session_field),
              std::vector<std::string>{"session-2"});
    ASSERT_TRUE(client_test::eventually(
        [&] { return requests_of(peer.server(), "POST", "resources/subscribe").size() == 2u; }));
    EXPECT_EQ(requests_of(peer.server(), "POST", "resources/subscribe")[1].fields_named(session_field),
              std::vector<std::string>{"session-2"});
    EXPECT_TRUE(client->close());
}

TEST(ClientHttp, ClosingTheClientEndsItsSession) {
    for (const int status : {200, 404, 405}) {
        SessionPeer peer([status](const Request& request) -> std::optional<Reply> {
            if (request.method == "DELETE") return http_test::empty_reply(status);
            return std::nullopt;
        });
        auto client = open_legacy(peer, legacy_options());
        ASSERT_TRUE(client) << client.error().message;
        EXPECT_TRUE(client->close()) << status;
        const auto deletes = requests_of(peer.server(), "DELETE");
        ASSERT_EQ(deletes.size(), 1u) << status;
        EXPECT_EQ(deletes[0].fields_named(session_field), std::vector<std::string>{"session-1"});
    }
}

// As the runner's scenario of reconnection does it: the call's stream names an event id and the wait, then ends; the
// answer comes on the resumed stream, which the server never ends. No GET goes out before (nothing needs one).
TEST(ClientHttp, ACallWhoseStreamEndsEarlyIsAnsweredOnTheResumedStream) {
    auto hold = std::make_shared<Gate>();
    auto ended = std::make_shared<std::atomic<std::int64_t>>(0);
    auto pending = std::make_shared<std::atomic<std::int64_t>>(-1);
    SessionPeer peer([=](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() == "tools/call") {
            pending->store(request.rpc_id().get<std::int64_t>());
            Reply reply;
            reply.send(stream_head());
            reply.send(priming("event-1", 500));
            reply.run([ended] { ended->store(std::chrono::steady_clock::now().time_since_epoch().count()); });
            return reply;
        }
        if (request.method == "GET") {
            Reply reply;
            reply.send(stream_head());
            reply.send(priming("event-2", 500));
            reply.send("event: message\n" + event("event-3", result_with_id(pending->load(), tool_text("back"))));
            reply.wait(hold);
            return reply;
        }
        return std::nullopt;
    });
    auto client = open_legacy(peer, legacy_options());
    ASSERT_TRUE(client) << client.error().message;
    const auto done = client->call_tool("test_reconnection");
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_EQ(text_of(*done), "back");
    const auto gets = requests_of(peer.server(), "GET");
    ASSERT_EQ(gets.size(), 1u);
    EXPECT_EQ(gets[0].field(last_event_field), "event-1");
    const std::chrono::steady_clock::time_point closed{std::chrono::steady_clock::duration(ended->load())};
    EXPECT_GE(gets[0].arrived - closed, 500ms);
    EXPECT_TRUE(client->close());
}

// Without an event id a stream that ends before its answer is lost: a read is asked for once more under a new id, a
// tool call is not (it may have run).
TEST(ClientHttp, AStreamLostWithoutAnEventIdSendsAReadAgainButNoToolCall) {
    auto lists = std::make_shared<std::atomic<int>>(0);
    SessionPeer peer([lists](const Request& request) -> std::optional<Reply> {
        const std::string method = request.rpc_method();
        if (method == "tools/call" || (method == "tools/list" && (*lists)++ == 0)) {
            Reply reply;
            reply.send(stream_head());
            reply.send(": no event id\n\n");
            return reply;
        }
        if (method == "tools/list") {
            return http_test::json_reply(200, http_test::result_for(request, json{{"tools", json::array()}}));
        }
        return std::nullopt;
    });
    auto client = open_legacy(peer, legacy_options());
    ASSERT_TRUE(client) << client.error().message;
    const auto called = client->call_tool("t");
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::stream_lost);
    EXPECT_EQ(requests_of(peer.server(), "POST", "tools/call").size(), 1u);
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    const auto sent = requests_of(peer.server(), "POST", "tools/list");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_NE(sent[0].rpc_id(), sent[1].rpc_id());
    EXPECT_TRUE(requests_of(peer.server(), "GET").empty());
    EXPECT_TRUE(client->close());
}

// A server without a route for GET answers the stream of the connection 404, though the session lives: the stream is
// taken as not offered, and the session is not renewed for it.
TEST(ClientHttp, AGetAnsweredNotFoundMeansNoStreamAndKeepsTheSession) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET") return http_test::empty_reply(404);
        return std::nullopt;
    });
    auto options = legacy_options();
    options.request_timeout = 5s;
    options.handlers.form([](const neomcp::ElicitationForm&) {
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::decline, json::object()};
    });
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_TRUE(client->request("tools/list"));
    ASSERT_TRUE(client->request("tools/list"));
    EXPECT_FALSE(peer.server().wait_for(peer.server().count() + 1, absence));
    EXPECT_EQ(peer.handshakes(), 1u);
    EXPECT_EQ(requests_of(peer.server(), "GET").size(), 1u);
    for (const auto& request : requests_of(peer.server(), "POST", "tools/list")) {
        EXPECT_EQ(request.fields_named(session_field), std::vector<std::string>{"session-1"});
    }
    EXPECT_TRUE(client->close());
}

// A server that names a session and keeps none: every renewal meets the same end. The client renews at most
// max_restarts times in a row without an answer between, then contact fails.
TEST(ClientHttp, RenewalsOfASessionInARowAreBounded) {
    SessionPeer peer([](const Request& request) -> std::optional<Reply> {
        if (request.method == "POST" && request.field(session_field)) {
            return http_test::json_reply(404, http_test::error_for(request, -32001, "Session not found"));
        }
        return std::nullopt;
    });
    auto options = legacy_options();
    options.max_restarts = 2;
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client_test::eventually([&] { return peer.handshakes() >= 3u; }));
    // Until the requests stop (a client that renewed forever would not let them).
    for (int quiet = 0; quiet < 20 && peer.server().wait_for(peer.server().count() + 1, absence); ++quiet) {
    }
    EXPECT_FALSE(peer.server().wait_for(peer.server().count() + 1, absence));
    EXPECT_EQ(peer.handshakes(), 3u);  // the first, and two renewals
    // Contact failed; the opener may have seen that already.
    if (!client) {
        EXPECT_EQ(client.error().kind, ClientErrorKind::too_many_restarts) << client.error().message;
        return;
    }
    const auto listed = client->request("tools/list");
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::too_many_restarts) << listed.error().message;
    static_cast<void>(client->close());  // contact failed: closing is all that is left
}

// An answer between renewals shows that the server serves: the count starts again.
TEST(ClientHttp, AnAnswerBetweenRenewalsStartsTheirCountAgain) {
    auto forget = std::make_shared<std::atomic<bool>>(true);
    SessionPeer peer([forget](const Request& request) -> std::optional<Reply> {
        if (request.rpc_method() != "tools/call") return std::nullopt;
        // Every other call finds its session gone; the call sent again under the new one is answered.
        if (forget->exchange(!forget->load())) {
            return http_test::json_reply(404, http_test::error_for(request, -32001, "Session not found"));
        }
        return http_test::json_reply(200, http_test::result_for(request, tool_text("served")));
    });
    auto options = legacy_options();
    options.max_restarts = 1;
    auto client = open_legacy(peer, std::move(options));
    ASSERT_TRUE(client) << client.error().message;
    for (int i = 0; i < 3; ++i) {
        const auto done = client->call_tool("t");
        ASSERT_TRUE(done) << i << ": " << done.error().message;
    }
    EXPECT_EQ(peer.handshakes(), 4u);
    EXPECT_TRUE(client->close());
}

}  // namespace
