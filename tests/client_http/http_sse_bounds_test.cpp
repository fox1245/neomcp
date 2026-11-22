// The bounds of the older transport of events over HTTP (2024-11-05), each driven to its edge by the scripted peer: the
// time the first event, which names where messages go, may take; the size of an event of the stream (the endpoint event
// and a message); the idle bound of the stream; and the bound on connecting. A bound of time is shown in both
// directions: the exchange is not given up before the bound has passed, and it is given up after it. What a bound
// measures from is the moment the transport starts, taken before it is made, so a measured wait is never short of the
// bound by the time the transport takes to start.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "client_http/http_sse.h"
#include "client_http/streamable.h"
#include "full_listener.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"
#include "rig.h"
#include "scripted_http_server.h"

namespace {

using namespace std::chrono_literals;
using http_rig::Delivered;
using http_rig::Driven;
using http_rig::events_of;
using http_rig::messages_of;
using http_test::Gate;
using http_test::Reply;
using http_test::Request;
using http_test::ScriptedHttpServer;

constexpr std::string_view older_revision = "2024-11-05";
constexpr std::string_view ping_line = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
// A wait that shows that something does not come.
constexpr auto absence = 300ms;

neomcp::detail::http_client::HttpTransportSettings settings_with(std::size_t max_message_bytes) {
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {neomcp::profile::builtin(older_revision)};
    settings.max_message_bytes = max_message_bytes;
    return settings;
}

neomcp::detail::http_client::HttpTransportSettings older_settings() {
    return settings_with(neomcp::ClientDefaults::max_message_bytes);
}

// Whether the exchange of a unit ended, in any way a transport ends one.
bool ended(const std::vector<Delivered>& seen, neomcp::UnitId unit) {
    for (const auto& one : seen) {
        const auto* event = std::get_if<http_rig::Event>(&one);
        if (event == nullptr || event->unit != unit) continue;
        if (std::holds_alternative<neomcp::StreamEnded>(event->event) ||
            std::holds_alternative<neomcp::StreamLost>(event->event) ||
            std::holds_alternative<neomcp::SendFailed>(event->event)) {
            return true;
        }
    }
    return false;
}

std::vector<Request> posts_of(const ScriptedHttpServer& server) {
    std::vector<Request> found;
    for (const auto& request : server.requests()) {
        if (request.method == "POST") found.push_back(request);
    }
    return found;
}

std::string endpoint_event(const std::string& reference) { return "event: endpoint\ndata: " + reference + "\n\n"; }

// A peer whose stream names the endpoint at `delay` after its head, or when the test opens `release`; each POST is
// taken. The stream stays open until `hold` opens.
struct EndpointPeer {
    explicit EndpointPeer(std::shared_ptr<Gate> gate, std::string reference = "/messages")
        : release(std::move(gate)),
          server([this, target = std::move(reference)](const Request& request) {
              if (request.method == "POST") return http_test::empty_reply(202);
              Reply stream;
              stream.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
              stream.wait(release);
              stream.send(endpoint_event(target));
              stream.wait(hold);
              return stream;
          }) {}
    ~EndpointPeer() { hold->open(); }

    std::shared_ptr<Gate> release;
    std::shared_ptr<Gate> hold = std::make_shared<Gate>();
    ScriptedHttpServer server;  // last: it goes first
};

}  // namespace

// ---- The time to the endpoint event

TEST(ClientHttpSseBounds, TheEndpointEventIsWaitedForUntilTheBoundAndTheConnectionFailsAfterIt) {
    constexpr auto bound = 1000ms;
    auto release = std::make_shared<Gate>();
    EndpointPeer peer(release);
    auto described = http_rig::endpoint(peer.server.url());
    described.endpoint_timeout = bound;
    const auto began = std::chrono::steady_clock::now();
    Driven driven(described, older_settings(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, std::string(ping_line));
    // Not given up short of the bound.
    EXPECT_FALSE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return ended(seen, 1); },
                                        bound - 300ms));
    const auto seen = driven.recorder().until_end(1);
    const auto took = std::chrono::steady_clock::now() - began;
    EXPECT_GE(took, bound);
    ASSERT_EQ(events_of<neomcp::SendFailed>(seen).size(), 1u);
    EXPECT_NE(events_of<neomcp::SendFailed>(seen)[0].cause.find("endpoint_timeout"), std::string::npos);
    // An endpoint event that comes after the bound names nothing: the connection was given up.
    release->open();
    EXPECT_FALSE(peer.server.wait_for(2, absence));
    EXPECT_TRUE(posts_of(peer.server).empty());
    // A message after it fails at once.
    driven.post(2, std::string(ping_line));
    EXPECT_EQ(events_of<neomcp::SendFailed>(driven.recorder().until_end(2)).size(), 1u);
    EXPECT_TRUE(posts_of(peer.server).empty());
}

TEST(ClientHttpSseBounds, AnEndpointEventJustInsideTheBoundIsTaken) {
    // The event comes two seconds before the bound, so that a slow host does not make it late.
    constexpr auto bound = 3000ms;
    constexpr auto arrives = 1000ms;
    auto release = std::make_shared<Gate>();
    EndpointPeer peer(release);
    auto described = http_rig::endpoint(peer.server.url());
    described.endpoint_timeout = bound;
    const auto began = std::chrono::steady_clock::now();
    Driven driven(described, older_settings(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, std::string(ping_line));
    std::this_thread::sleep_for(arrives);
    EXPECT_FALSE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return ended(seen, 1); }, 0ms));
    release->open();
    const auto seen = driven.recorder().until_end(1);
    EXPECT_LT(std::chrono::steady_clock::now() - began, bound) << "the test itself was late";
    EXPECT_TRUE(events_of<neomcp::SendFailed>(seen).empty());
    EXPECT_EQ(events_of<neomcp::StreamEnded>(seen).size(), 1u);
    const auto posts = posts_of(peer.server);
    ASSERT_EQ(posts.size(), 1u);
    EXPECT_EQ(posts[0].target, "/messages");
    // The clock was stopped by the event: nothing fails when the bound passes.
    std::this_thread::sleep_for(bound - arrives + 300ms);
    driven.post(2, std::string(ping_line));
    EXPECT_TRUE(events_of<neomcp::SendFailed>(driven.recorder().until_end(2)).empty());
    EXPECT_EQ(posts_of(peer.server).size(), 2u);
}

TEST(ClientHttpSseBounds, TheEndpointClockCoversTheTimeTheHeadTakes) {
    constexpr auto bound = 600ms;
    // The server accepts the connection and answers nothing until the test opens the gate.
    auto head_gate = std::make_shared<Gate>();
    ScriptedHttpServer server([head_gate](const Request&) {
        Reply stream;
        stream.wait(head_gate);
        stream.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
        stream.send(endpoint_event("/messages"));
        return stream;
    });
    auto described = http_rig::endpoint(server.url());
    described.endpoint_timeout = bound;
    const auto began = std::chrono::steady_clock::now();
    Driven driven(described, older_settings(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, std::string(ping_line));
    const auto seen = driven.recorder().until_end(1);
    EXPECT_GE(std::chrono::steady_clock::now() - began, bound);
    ASSERT_EQ(events_of<neomcp::SendFailed>(seen).size(), 1u);
    EXPECT_NE(events_of<neomcp::SendFailed>(seen)[0].cause.find("endpoint_timeout"), std::string::npos);
    head_gate->open();
}

// ---- The size of an event

TEST(ClientHttpSseBounds, AnEndpointEventAtTheMessageBoundIsTakenAndOneByteMoreFailsTheConnection) {
    constexpr std::size_t bound = 1024;
    const std::string stem = "/messages?pad=";
    for (const std::size_t length : {bound, bound + 1}) {
        SCOPED_TRACE(length);
        const std::string reference = stem + std::string(length - stem.size(), 'p');
        ASSERT_EQ(reference.size(), length);
        auto release = std::make_shared<Gate>();
        release->open();
        EndpointPeer peer(release, reference);
        Driven driven(http_rig::endpoint(peer.server.url()), settings_with(bound), http_rig::Kind::events);
        ASSERT_EQ(driven.problem(), "");
        driven.post(1, std::string(ping_line));
        const auto seen = driven.recorder().until_end(1);
        if (length == bound) {
            EXPECT_EQ(events_of<neomcp::StreamEnded>(seen).size(), 1u);
            const auto posts = posts_of(peer.server);
            ASSERT_EQ(posts.size(), 1u);
            EXPECT_EQ(posts[0].target, reference);
        } else {
            ASSERT_EQ(events_of<neomcp::SendFailed>(seen).size(), 1u);
            EXPECT_NE(events_of<neomcp::SendFailed>(seen)[0].cause.find("max_message_bytes"), std::string::npos)
                << events_of<neomcp::SendFailed>(seen)[0].cause;
            EXPECT_FALSE(peer.server.wait_for(2, absence));
            EXPECT_TRUE(posts_of(peer.server).empty());
        }
    }
}

namespace {

// A peer whose stream names its endpoint, then sends each of `events` when the test opens its gate.
struct EventPeer {
    explicit EventPeer(std::vector<std::string> sent) : events(std::move(sent)) {
        for (std::size_t i = 0; i < events.size(); ++i) gates.push_back(std::make_shared<Gate>());
        server = std::make_unique<ScriptedHttpServer>([this](const Request& request) {
            if (request.method == "POST") return http_test::empty_reply(202);
            Reply stream = http_test::event_stream_reply({endpoint_event("/messages")});
            for (std::size_t i = 0; i < events.size(); ++i) {
                stream.wait(gates[i]);
                stream.send(events[i]);
            }
            stream.wait(hold);
            return stream;
        });
    }
    ~EventPeer() {
        hold->open();
        for (const auto& gate : gates) gate->open();
        server.reset();
    }

    std::vector<std::string> events;
    std::vector<std::shared_ptr<Gate>> gates;
    std::shared_ptr<Gate> hold = std::make_shared<Gate>();
    std::unique_ptr<ScriptedHttpServer> server;
};

// A message event whose data is `bytes` long.
std::string message_event_of(std::size_t bytes) {
    std::string data = R"({"jsonrpc":"2.0","id":1,"result":{}})";
    data.append(bytes - data.size(), ' ');
    return "event: message\ndata: " + data + "\n\n";
}

}  // namespace

TEST(ClientHttpSseBounds, AMessageEventAtTheBoundIsDeliveredAndOneByteMoreIsDeliveredAndEndsTheConnection) {
    constexpr std::size_t bound = 512;
    EventPeer peer({message_event_of(bound), message_event_of(bound + 1)});
    Driven driven(http_rig::endpoint(peer.server->url()), settings_with(bound), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    // The connection is made when a message posted to it is taken.
    driven.post(1, std::string(ping_line));
    driven.recorder().until_end(1);
    peer.gates[0]->open();
    ASSERT_TRUE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return !messages_of(seen).empty(); }));
    ASSERT_EQ(messages_of(driven.recorder().all()).size(), 1u);
    EXPECT_EQ(messages_of(driven.recorder().all())[0].size(), bound);
    EXPECT_TRUE(events_of<neomcp::StreamLost>(driven.recorder().of(neomcp::connection_unit)).empty());
    peer.gates[1]->open();
    ASSERT_TRUE(driven.recorder().until_count<neomcp::StreamLost>(neomcp::connection_unit, 1));
    const auto messages = messages_of(driven.recorder().all());
    // The client hears of the message that is too long (it ends the call it answers), and then of the lost stream.
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[1].size(), bound + 1);
    const auto lost = events_of<neomcp::StreamLost>(driven.recorder().of(neomcp::connection_unit));
    ASSERT_EQ(lost.size(), 1u);
    EXPECT_NE(lost[0].cause.find("max_message_bytes"), std::string::npos) << lost[0].cause;
    // The connection is over: the next message waits for the client's restart and fails as undeliverable.
    driven.post(2, std::string(ping_line));
    EXPECT_EQ(events_of<neomcp::SendFailed>(driven.recorder().until_end(2)).size(), 1u);
}

TEST(ClientHttpSseBounds, AnEventOfSeveralLinesIsBoundedByItsDataAsOneLineIs) {
    constexpr std::size_t bound = 512;
    // Two data lines whose data, joined by a line feed, is exactly the bound, and one byte more.
    const auto in_lines = [](std::size_t total) {
        const std::size_t first = (total - 1) / 2;
        return "event: message\ndata: " + std::string(first, 'a') + "\ndata: " + std::string(total - 1 - first, 'b') +
               "\n\n";
    };
    EventPeer peer({in_lines(bound), in_lines(bound + 1)});
    Driven driven(http_rig::endpoint(peer.server->url()), settings_with(bound), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, std::string(ping_line));
    driven.recorder().until_end(1);
    peer.gates[0]->open();
    ASSERT_TRUE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return !messages_of(seen).empty(); }));
    EXPECT_EQ(messages_of(driven.recorder().all())[0].size(), bound);
    EXPECT_TRUE(events_of<neomcp::StreamLost>(driven.recorder().of(neomcp::connection_unit)).empty());
    peer.gates[1]->open();
    ASSERT_TRUE(driven.recorder().until_count<neomcp::StreamLost>(neomcp::connection_unit, 1));
    ASSERT_EQ(messages_of(driven.recorder().all()).size(), 2u);
    EXPECT_GT(messages_of(driven.recorder().all())[1].size(), bound);
}

// ---- The idle bound

TEST(ClientHttpSseBounds, TheStreamIsKeptByAByteWithinTheIdleBoundAndLostNoSoonerThanTheBoundAfterTheLast) {
    constexpr auto bound = 900ms;
    constexpr auto interval = bound - 500ms;  // inside the bound, and two of them pass it
    auto silence = std::make_shared<Gate>();
    std::mutex mutex;
    std::chrono::steady_clock::time_point last{};
    ScriptedHttpServer server([&](const Request& request) {
        if (request.method == "POST") return http_test::empty_reply(202);
        Reply stream = http_test::event_stream_reply({endpoint_event("/messages")});
        for (int i = 0; i < 3; ++i) {
            stream.pause(interval);
            stream.send(": still here\n\n");
        }
        // Noted before the last byte goes, so that the client's last byte comes no sooner than this.
        stream.run([&] {
            std::lock_guard lock(mutex);
            last = std::chrono::steady_clock::now();
        });
        stream.send(": last\n\n");
        stream.wait(silence);
        return stream;
    });
    auto described = http_rig::endpoint(server.url());
    described.stream_idle_timeout = bound;
    Driven driven(described, older_settings(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, std::string(ping_line));
    driven.recorder().until_end(1);
    ASSERT_TRUE(driven.recorder().until_count<neomcp::StreamLost>(neomcp::connection_unit, 1));
    const auto lost_at = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    ASSERT_NE(last, std::chrono::steady_clock::time_point{});
    // Three bytes came, each within the bound of the one before, over more than the bound in all.
    EXPECT_GE(lost_at - last, bound);
    const auto lost = events_of<neomcp::StreamLost>(driven.recorder().of(neomcp::connection_unit));
    ASSERT_EQ(lost.size(), 1u);
    EXPECT_NE(lost[0].cause.find("stream_idle_timeout"), std::string::npos) << lost[0].cause;
    silence->open();
}

TEST(ClientHttpSseBounds, TheShorterOfTheIdleBoundAndTheEndpointBoundEndsAStreamThatNamesNoEndpoint) {
    struct Case {
        std::chrono::milliseconds idle;
        std::chrono::milliseconds endpoint;
        const char* named;
    };
    for (const Case& one : {Case{600ms, 5000ms, "stream_idle_timeout"}, Case{5000ms, 600ms, "endpoint_timeout"}}) {
        SCOPED_TRACE(one.named);
        auto never = std::make_shared<Gate>();
        EndpointPeer peer(never);
        auto described = http_rig::endpoint(peer.server.url());
        described.stream_idle_timeout = one.idle;
        described.endpoint_timeout = one.endpoint;
        const auto shorter = (std::min)(one.idle, one.endpoint);
        const auto began = std::chrono::steady_clock::now();
        Driven driven(described, older_settings(), http_rig::Kind::events);
        ASSERT_EQ(driven.problem(), "");
        driven.post(1, std::string(ping_line));
        const auto seen = driven.recorder().until_end(1);
        const auto took = std::chrono::steady_clock::now() - began;
        EXPECT_GE(took, shorter);
        EXPECT_LT(took, shorter + 3s);
        ASSERT_EQ(events_of<neomcp::SendFailed>(seen).size(), 1u);
        EXPECT_NE(events_of<neomcp::SendFailed>(seen)[0].cause.find(one.named), std::string::npos)
            << events_of<neomcp::SendFailed>(seen)[0].cause;
        never->open();
    }
}

// ---- Connecting

#ifdef __linux__
TEST(ClientHttpSseBounds, AConnectionThatIsNeverEstablishedIsNotGivenUpBeforeTheConnectBoundAndIsAfterIt) {
    constexpr auto bound = 1500ms;
    http_rig::FullListener listener;
    ASSERT_TRUE(listener.full()) << "the listener's queue could not be filled";
    auto described = http_rig::endpoint(listener.url());
    described.connect_timeout = bound;
    const auto began = std::chrono::steady_clock::now();
    Driven driven(described, older_settings(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, std::string(ping_line));
    EXPECT_FALSE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return ended(seen, 1); },
                                        bound - 500ms));
    const auto seen = driven.recorder().until_end(1);
    const auto took = std::chrono::steady_clock::now() - began;
    EXPECT_GE(took, bound);
    EXPECT_LT(took, bound + 3s);
    ASSERT_EQ(events_of<neomcp::SendFailed>(seen).size(), 1u);
    EXPECT_NE(events_of<neomcp::SendFailed>(seen)[0].cause.find("event stream could not be made"), std::string::npos)
        << events_of<neomcp::SendFailed>(seen)[0].cause;
    RecordProperty("sse_connect_given_up_after_ms",
                   static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
}

TEST(ClientHttpSseBounds, TheEndpointBoundEndsAConnectionThatTheConnectBoundWouldWaitFor) {
    constexpr auto endpoint_bound = 600ms;
    http_rig::FullListener listener;
    ASSERT_TRUE(listener.full()) << "the listener's queue could not be filled";
    auto described = http_rig::endpoint(listener.url());
    described.connect_timeout = 10s;
    described.endpoint_timeout = endpoint_bound;
    const auto began = std::chrono::steady_clock::now();
    Driven driven(described, older_settings(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, std::string(ping_line));
    const auto seen = driven.recorder().until_end(1);
    const auto took = std::chrono::steady_clock::now() - began;
    EXPECT_GE(took, endpoint_bound);
    EXPECT_LT(took, endpoint_bound + 3s);
    ASSERT_EQ(events_of<neomcp::SendFailed>(seen).size(), 1u);
    EXPECT_NE(events_of<neomcp::SendFailed>(seen)[0].cause.find("endpoint_timeout"), std::string::npos)
        << events_of<neomcp::SendFailed>(seen)[0].cause;
}
#endif
