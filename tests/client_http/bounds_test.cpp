// The bounds of the Streamable HTTP transport, each driven to its edge by the scripted peer: one message as a JSON body
// and as an event, the nesting of a message, the bytes of a response's head, the session id, the server's advised wait
// before a stream is resumed, the idle bound of an event stream and the bound on connecting. Where the wait itself is
// what a bound sets, it is measured; everything else waits on the peer's gates and on what is delivered.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "client_http/streamable.h"
#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/profile.h"
#include "rig.h"
#include "scripted_http_server.h"
#include "support.h"

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
using neomcp::OutgoingKind;
using neomcp::ResponseHead;
using neomcp::StreamEnded;
using neomcp::StreamLost;

constexpr std::string_view modern = "2026-07-28";
constexpr std::string_view legacy = "2025-11-25";
// A bound for a wait that shows that something does not come.
constexpr auto absence = 300ms;

json discover_result() {
    return json{{"supportedVersions", json::array({std::string(modern)})},
                {"capabilities", json::object()},
                {"serverInfo", json{{"name", "scripted"}, {"version", "1"}}},
                {"ttlMs", 0},
                {"cacheScope", "private"},
                {"resultType", "complete"}};
}

// The answer of a list as JSON text padded with spaces after the message to `bytes` bytes.
std::string padded_answer(const Request& request, std::size_t bytes) {
    std::string text = http_test::result_for(request, json{{"tools", json::array()}, {"ttlMs", 0},
                                                           {"cacheScope", "private"}, {"resultType", "complete"}})
                           .dump();
    if (text.size() < bytes) text.append(bytes - text.size(), ' ');
    return text;
}

// The answer of a list whose `_meta` holds arrays nested `levels` deep.
std::string nested_answer(const Request& request, std::size_t levels) {
    std::string inner(levels, '[');
    inner.append(levels, ']');
    return R"({"jsonrpc":"2.0","id":)" + request.rpc_id().dump() +
           R"(,"result":{"tools":[],"ttlMs":0,"cacheScope":"private","resultType":"complete","_meta":{"x":)" + inner +
           "}}}";
}

// A peer of the newest revision: discovery, then each list answered by `answer` (the bytes to send) with the shape
// `as`, which a test changes between calls.
enum class Shape { json_body, event };

struct ListPeer {
    explicit ListPeer(std::function<std::string(const Request&, std::size_t)> make)
        : server([this, answer = std::move(make)](const Request& request) {
              if (request.rpc_method() == "server/discover") {
                  return http_test::json_reply(200, http_test::result_for(request, discover_result()));
              }
              const std::string text = answer(request, size.load());
              if (shape.load() == Shape::event) {
                  return http_test::event_stream_reply({"data: " + text + "\n\n"});
              }
              return http_test::raw_reply(200, {{"Content-Type", "application/json"}}, text);
          }) {}

    std::atomic<std::size_t> size{0};
    std::atomic<Shape> shape{Shape::json_body};
    ScriptedHttpServer server;
};

}  // namespace

// ---- One message

TEST(ClientBounds, AnAnswerAtTheMessageBoundIsReadAndOneByteMoreFailsTheCall) {
    constexpr std::size_t bound = 4096;
    ListPeer peer(padded_answer);
    auto options = client_test::only({modern});
    options.max_message_bytes = bound;
    auto client = neomcp::Client::open(std::move(options), endpoint(peer.server.url()));
    ASSERT_TRUE(client) << client.error().message;
    for (const Shape shape : {Shape::json_body, Shape::event}) {
        SCOPED_TRACE(shape == Shape::event ? "event" : "json");
        peer.shape = shape;
        peer.size = bound;
        const auto at = client->request("tools/list");
        ASSERT_TRUE(at) << at.error().message;
        peer.size = bound + 1;
        const auto past = client->request("tools/list");
        ASSERT_FALSE(past);
        EXPECT_EQ(past.error().kind, ClientErrorKind::message_too_large);
        // The client goes on.
        peer.size = 0;
        const auto after = client->request("tools/list");
        ASSERT_TRUE(after) << after.error().message;
    }
}

TEST(ClientBounds, AnAnswerNestedToTheBoundIsReadAndOneLevelDeeperFailsTheCall) {
    constexpr std::size_t bound = neomcp::ClientDefaults::max_nesting_depth;
    // The message, its result and the result's _meta are three levels; the arrays inside make up the rest.
    constexpr std::size_t outer = 3;
    ListPeer peer(nested_answer);
    auto client = neomcp::Client::open(client_test::only({modern}), endpoint(peer.server.url()));
    ASSERT_TRUE(client) << client.error().message;
    peer.size = bound - outer;
    const auto at = client->request("tools/list");
    ASSERT_TRUE(at) << at.error().message;
    peer.size = bound - outer + 1;
    const auto past = client->request("tools/list");
    ASSERT_FALSE(past);
    EXPECT_EQ(past.error().kind, ClientErrorKind::message_too_large);
    peer.size = 1;
    EXPECT_TRUE(client->request("tools/list"));
}

// A transport made apart from the client keeps the bound it was made with: an answer it cuts there fails its call as
// too long, whatever bound the client's own options name.
TEST(ClientBounds, AnAnswerOverTheTransportsBoundFailsTheCallAsTooLong) {
    constexpr std::size_t bound = 1024;
    ListPeer peer(padded_answer);
    peer.size = 2 * bound;
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.max_message_bytes = bound;
    auto transport = neomcp::detail::http_client::make_http_transport(endpoint(peer.server.url()), settings);
    ASSERT_TRUE(transport) << transport.error().message;
    auto client = neomcp::Client::open(client_test::only({modern}), std::move(transport).value());
    ASSERT_TRUE(client) << client.error().message;
    for (const Shape shape : {Shape::json_body, Shape::event}) {
        SCOPED_TRACE(shape == Shape::event ? "event" : "json");
        peer.shape = shape;
        neomcp::RequestOptions call;
        call.timeout = 5s;  // without the bound the call would wait for its clock
        const auto past = client->request("tools/list", nullptr, call);
        ASSERT_FALSE(past);
        EXPECT_EQ(past.error().kind, ClientErrorKind::message_too_large);
    }
}

// ---- The head of a response

// libcurl hands over the status line, each field with its line end and the blank line: the head of a reply is exactly
// the bytes the bound counts.
TEST(ClientBounds, AResponseHeadAtTheBoundIsReadAndOneByteMoreAbortsTheExchange) {
    const std::string body = R"({"jsonrpc":"2.0","id":1,"result":{}})";
    const http_test::Fields fields{{"Content-Type", "application/json"}, {"X-Pad", std::string(512, 'p')}};
    const std::size_t head_bytes = http_test::head(200, fields, body.size()).size();
    ScriptedHttpServer server([&](const Request&) { return http_test::raw_reply(200, fields, body); });
    for (const std::size_t bound : {head_bytes, head_bytes - 1}) {
        SCOPED_TRACE(bound);
        auto described = endpoint(server.url());
        described.max_response_header_bytes = bound;
        Driven driven(described);
        ASSERT_EQ(driven.problem(), "");
        driven.post(1, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
        const auto seen = driven.recorder().until_end(1);
        if (bound == head_bytes) {
            ASSERT_EQ(events_of<ResponseHead>(seen).size(), 1u);
            EXPECT_EQ(messages_of(seen), std::vector<std::string>{body});
            EXPECT_EQ(events_of<StreamEnded>(seen).size(), 1u);
        } else {
            EXPECT_TRUE(events_of<ResponseHead>(seen).empty());
            EXPECT_TRUE(messages_of(seen).empty());
            EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);
        }
    }
}

// ---- The session id

TEST(ClientBounds, ASessionIdAtTheBoundIsUsedAndOneByteLongerEndsTheHandshake) {
    constexpr std::size_t bound = neomcp::HttpClientDefaults::max_session_id_bytes;
    for (const std::size_t length : {bound, bound + 1}) {
        SCOPED_TRACE(length);
        const std::string id(length, 's');
        ScriptedHttpServer server([&id](const Request& request) {
            return http_test::json_reply(200, http_test::result_for(request, json::object()),
                                         {{"MCP-Session-Id", id}});
        });
        neomcp::detail::http_client::HttpTransportSettings settings;
        settings.profiles = {neomcp::profile::builtin(legacy)};
        Driven driven(endpoint(server.url()), settings);
        driven.post(1, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
        const auto seen = driven.recorder().until_end(1);
        if (length == bound) {
            ASSERT_EQ(messages_of(seen).size(), 1u);
            driven.post(2, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
            driven.recorder().until_end(2);
            ASSERT_TRUE(server.wait_for(2));
            EXPECT_EQ(server.requests()[1].fields_named("MCP-Session-Id"), std::vector<std::string>{id});
        } else {
            EXPECT_TRUE(events_of<ResponseHead>(seen).empty());
            EXPECT_TRUE(messages_of(seen).empty());
            EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);
        }
    }
}

// ---- Resuming a stream

namespace {

// A peer of a revision that opens with a handshake (one session) whose tools/call answer is an event stream that names
// an event id and the advised wait and then ends; a GET that names the id gets the answer on a stream that stays open.
// The time the first stream ended is noted.
struct BrokenStream {
    explicit BrokenStream(int retry_ms)
        : server([this, retry_ms](const Request& request) {
              if (request.method == "POST" && request.rpc_method() == "initialize") {
                  json result{{"protocolVersion", std::string(legacy)},
                              {"capabilities", json::object()},
                              {"serverInfo", {{"name", "scripted"}, {"version", "1"}}}};
                  return http_test::json_reply(200, http_test::result_for(request, std::move(result)),
                                               {{"MCP-Session-Id", "session-1"}});
              }
              Reply reply;
              if (request.method == "POST" && request.rpc_method() == "tools/call") {
                  reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
                  reply.send("id: e1\nretry: " + std::to_string(retry_ms) + "\ndata: \n\n");
                  reply.run([this] {
                      std::lock_guard lock(mutex);
                      ended = std::chrono::steady_clock::now();
                  });
                  return reply;
              }
              if (request.method == "GET") {
                  reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
                  reply.send("id: e2\ndata: " +
                             json{{"jsonrpc", "2.0"}, {"id", 2}, {"result", json{{"content", json::array()}}}}.dump() +
                             "\n\n");
                  reply.wait(hold);
                  return reply;
              }
              return http_test::empty_reply(202);
          }) {}

    std::chrono::steady_clock::time_point ended_at() {
        std::lock_guard lock(mutex);
        return ended;
    }

    std::shared_ptr<Gate> hold = std::make_shared<Gate>();
    std::mutex mutex;
    std::chrono::steady_clock::time_point ended{};
    ScriptedHttpServer server;  // last: it goes first
};

void open_session(Driven& driven) {
    driven.post(100, R"({"jsonrpc":"2.0","id":100,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})",
                {}, OutgoingKind::request, neomcp::profile::builtin(legacy));
    driven.recorder().until_end(100);
}

std::vector<Request> gets_of(const ScriptedHttpServer& server) {
    std::vector<Request> found;
    for (const auto& request : server.requests()) {
        if (request.method == "GET") found.push_back(request);
    }
    return found;
}

}  // namespace

TEST(ClientBounds, AnAdvisedWaitAtItsBoundIsWaitedAndOneMillisecondMoreEndsTheRequest) {
    constexpr auto bound = 300ms;
    const std::vector<neomcp::HttpHeader> fields{{"MCP-Protocol-Version", std::string(legacy)}};
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {neomcp::profile::builtin(legacy)};
    const std::string call =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"slow","arguments":{}}})";
    {
        BrokenStream broken(static_cast<int>(bound.count()));
        auto described = endpoint(broken.server.url());
        described.max_retry_delay = bound;
        Driven driven(described, settings);
        open_session(driven);
        driven.post(2, call, fields, OutgoingKind::request, neomcp::profile::builtin(legacy));
        ASSERT_TRUE(driven.recorder().wait([](const std::vector<Delivered>& seen) {
            for (const auto& one : seen) {
                const auto* received = std::get_if<http_rig::Received>(&one);
                if (received != nullptr && received->unit == 2) return true;
            }
            return false;
        }));
        const auto gets = gets_of(broken.server);
        ASSERT_EQ(gets.size(), 1u);
        EXPECT_EQ(gets[0].field("Last-Event-ID"), "e1");
        EXPECT_GE(gets[0].arrived - broken.ended_at(), bound);
        EXPECT_TRUE(events_of<StreamLost>(driven.recorder().of(2)).empty());
        driven.end(2);
    }
    {
        BrokenStream broken(static_cast<int>(bound.count()) + 1);
        auto described = endpoint(broken.server.url());
        described.max_retry_delay = bound;
        Driven driven(described, settings);
        open_session(driven);
        driven.post(2, call, fields, OutgoingKind::request, neomcp::profile::builtin(legacy));
        const auto seen = driven.recorder().until_end(2);
        EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);
        EXPECT_FALSE(broken.server.wait_for(3, absence));  // the handshake and the call: no GET
        EXPECT_TRUE(gets_of(broken.server).empty());
    }
}

// ---- The idle bound

TEST(ClientBounds, AnEventStreamIsLostNoSoonerThanTheIdleBoundAfterItsLastByte) {
    constexpr auto bound = 300ms;
    auto silence = std::make_shared<Gate>();
    std::mutex mutex;
    std::chrono::steady_clock::time_point last{};
    ScriptedHttpServer server([&](const Request&) {
        Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
        reply.send(": begun\n\n");
        reply.pause(100ms);
        // Noted before the last bytes go, so that the client's last byte comes no sooner than this.
        reply.run([&] {
            std::lock_guard lock(mutex);
            last = std::chrono::steady_clock::now();
        });
        reply.send(": still here\n\n");
        reply.wait(silence);
        return reply;
    });
    auto described = endpoint(server.url());
    described.stream_idle_timeout = bound;
    Driven driven(described);
    driven.post(1, R"({"jsonrpc":"2.0","id":1,"method":"tools/call"})");
    const auto seen = driven.recorder().until_end(1);
    const auto lost_at = std::chrono::steady_clock::now();
    EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);
    std::lock_guard lock(mutex);
    ASSERT_NE(last, std::chrono::steady_clock::time_point{});
    EXPECT_GE(lost_at - last, bound);
    RecordProperty("idle_lost_after_ms",
                   static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(lost_at - last).count()));
    silence->open();
}

// ---- Connecting

#ifdef __linux__
namespace {

// A listener on a loopback port the system chooses that never accepts and whose queue of connections is full, so that
// a further connection is never established: its SYN is dropped and retried until the connecting side gives up.
class FullListener {
public:
    FullListener() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) return;
        if (::listen(listener_, 0) != 0) return;
        socklen_t length = sizeof address;
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        // Connections are made until one stays unestablished: from then on the queue is full.
        for (int attempt = 0; attempt < 16 && !full_; ++attempt) {
            const int one = ::socket(AF_INET, SOCK_STREAM, 0);
            ::fcntl(one, F_SETFL, ::fcntl(one, F_GETFL) | O_NONBLOCK);
            fillers_.push_back(one);
            ::connect(one, reinterpret_cast<sockaddr*>(&address), sizeof address);
            pollfd wait{one, POLLOUT, 0};
            full_ = ::poll(&wait, 1, 200) == 0;
        }
    }
    FullListener(const FullListener&) = delete;
    FullListener& operator=(const FullListener&) = delete;
    ~FullListener() {
        for (const int one : fillers_) ::close(one);
        if (listener_ >= 0) ::close(listener_);
    }

    bool full() const noexcept { return full_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/mcp"; }

private:
    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::vector<int> fillers_;
    bool full_ = false;
};

}  // namespace

TEST(ClientBounds, AConnectionThatIsNeverEstablishedIsGivenUpAfterTheConnectBound) {
    constexpr auto bound = 500ms;
    FullListener listener;
    ASSERT_TRUE(listener.full()) << "the listener's queue could not be filled";
    auto described = endpoint(listener.url());
    described.connect_timeout = bound;
    Driven driven(described);
    ASSERT_EQ(driven.problem(), "");
    const auto started = std::chrono::steady_clock::now();
    driven.post(1, R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    const auto seen = driven.recorder().until_end(1);
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(events_of<neomcp::SendFailed>(seen).size(), 1u);
    EXPECT_TRUE(events_of<ResponseHead>(seen).empty());
    EXPECT_GE(took, bound);
    EXPECT_LT(took, http_rig::wait_bound);
    RecordProperty("connect_given_up_after_ms",
                   static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
}
#endif
