// Sessions nobody used (EndpointOptions::unused_session_timeout and max_sessions) at their edges: a full table of them
// gives the place of the one kept longest to a newcomer, and its client is answered as for an ended session; a session
// that was used keeps its place under any pressure; one nobody used ends after its timeout, while a client whose first
// request after the handshake is not the notification that completes it is not cut; and handshakes racing each other,
// or the first request of the session they would displace, leave the table consistent.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;

constexpr const char* path = "/mcp";
constexpr const char* legacy = "2025-11-25";
constexpr int ok = 200;
constexpr int accepted = 202;
constexpr int not_found = 404;
constexpr int unavailable = 503;
// Reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;

struct Reply {
    int status = 0;  // 0: no answer
    std::string session;
};

Reply post(std::uint16_t port, const std::string& session, const std::string& body) {
    httplib::Client client("127.0.0.1", port);
    client.set_keep_alive(false);
    client.set_read_timeout(30, 0);
    httplib::Headers fields{{"Accept", "application/json, text/event-stream"}};
    if (!session.empty()) {
        fields.emplace("Mcp-Session-Id", session);
        fields.emplace("MCP-Protocol-Version", legacy);
    }
    const auto result = client.Post(path, fields, body, "application/json");
    if (!result) return {};
    return Reply{result->status, result->get_header_value("Mcp-Session-Id")};
}

std::string request(int id, const std::string& method, json params = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
}

std::string notification(const std::string& method) {
    return json{{"jsonrpc", "2.0"}, {"method", method}}.dump();
}

Reply handshake(std::uint16_t port, int id = 1) {
    const json params{{"protocolVersion", legacy},
                      {"capabilities", json::object()},
                      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    return post(port, "", request(id, "initialize", params));
}

// The notification that completes a handshake: the usual first use of a session.
Reply ready(std::uint16_t port, const std::string& session) {
    return post(port, session, notification("notifications/initialized"));
}

Reply list(std::uint16_t port, const std::string& session, int id = 2) {
    return post(port, session, request(id, "tools/list"));
}

struct Harness {
    explicit Harness(std::size_t max_sessions, std::chrono::milliseconds unused_timeout = 1h)
        : server(neomcp::ServerOptions{.name = "unused-session-test", .version = "0.0.1"}) {
        server.tool("noop", "Do nothing", [] { return std::string("ok"); });
        neomcp::HttpServerOptions options;
        options.path = path;
        options.max_worker_threads = 64;
        options.endpoint.max_sessions = max_sessions;
        options.endpoint.unused_session_timeout = unused_timeout;
        http = std::make_unique<neomcp::HttpServer>(server, options);
        const auto started = http->start();
        if (started) port = *started.value;
    }

    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

std::string open_unused(std::uint16_t port) {
    const Reply opened = handshake(port);
    EXPECT_EQ(opened.status, ok);
    EXPECT_FALSE(opened.session.empty());
    return opened.session;
}

std::string open_used(std::uint16_t port) {
    const std::string session = open_unused(port);
    EXPECT_EQ(ready(port, session).status, accepted);
    return session;
}

// Waits until the endpoint keeps `count` sessions; false when that did not happen in time.
bool wait_for_sessions(neomcp::HttpServer& http, std::size_t count) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < patience) {
        if (http.session_count() == count) return true;
        std::this_thread::sleep_for(5ms);  // pacing a look at a counter; the subject here is a deadline
    }
    return false;
}

TEST(HttpUnusedSessions, ATableFullOfUnusedSessionsTakesExactlyItsBoundAndThenGivesTheOldestPlace) {
    constexpr std::size_t table = 3;
    Harness harness(table);
    ASSERT_NE(harness.port, 0);
    std::vector<std::string> sessions;
    for (std::size_t i = 0; i < table; ++i) sessions.push_back(open_unused(harness.port));
    EXPECT_EQ(harness.http->session_count(), table);

    // One past the bound: the newcomer gets a session, and the session kept longest ends.
    const Reply newcomer = handshake(harness.port, 9);
    EXPECT_EQ(newcomer.status, ok);
    EXPECT_FALSE(newcomer.session.empty());
    EXPECT_EQ(harness.http->session_count(), table);
    // Its client is answered as for an ended session, which tells it to open a new one.
    EXPECT_EQ(ready(harness.port, sessions[0]).status, not_found);
    EXPECT_EQ(list(harness.port, sessions[0]).status, not_found);
    // The others were not touched.
    for (std::size_t i = 1; i < table; ++i) EXPECT_EQ(ready(harness.port, sessions[i]).status, accepted) << i;
    EXPECT_EQ(ready(harness.port, newcomer.session).status, accepted);
    EXPECT_EQ(harness.http->session_count(), table);
}

TEST(HttpUnusedSessions, AUsedSessionNeverGivesItsPlace) {
    constexpr std::size_t table = 2;
    Harness harness(table);
    ASSERT_NE(harness.port, 0);
    // The older session was used, the newer one not: the newcomer takes the newer one's place.
    const std::string used = open_used(harness.port);
    const std::string idle = open_unused(harness.port);
    const Reply first = handshake(harness.port, 3);
    EXPECT_EQ(first.status, ok);
    EXPECT_EQ(list(harness.port, idle).status, not_found);
    EXPECT_EQ(list(harness.port, used).status, ok);

    // Every kept session used: a newcomer is refused, however many come, and none of them ends a session.
    EXPECT_EQ(ready(harness.port, first.session).status, accepted);
    for (int i = 0; i < 5; ++i) {
        const Reply refused = handshake(harness.port, 10 + i);
        EXPECT_EQ(refused.status, unavailable) << i;
        EXPECT_TRUE(refused.session.empty());
    }
    EXPECT_EQ(list(harness.port, used).status, ok);
    EXPECT_EQ(list(harness.port, first.session).status, ok);
    EXPECT_EQ(harness.http->session_count(), table);
}

TEST(HttpUnusedSessions, ASessionNobodyUsedEndsAfterItsTimeoutAndAnyFirstRequestKeepsOne) {
    // A deadline is the subject: a short one, and margins far above the time the requests take.
    constexpr auto unused_timeout = 1s;
    Harness harness(8, unused_timeout);
    ASSERT_NE(harness.port, 0);
    const std::string forgotten = open_unused(harness.port);
    // A client whose first request after the handshake is not the notification that completes it, and which sends
    // that notification only after the timeout passed.
    const std::string late = open_unused(harness.port);
    EXPECT_EQ(list(harness.port, late).status, ok);
    const std::string used = open_used(harness.port);
    ASSERT_TRUE(wait_for_sessions(*harness.http, 2)) << harness.http->session_count();
    EXPECT_EQ(ready(harness.port, forgotten).status, not_found);
    std::this_thread::sleep_for(2 * unused_timeout);
    EXPECT_EQ(ready(harness.port, late).status, accepted);
    EXPECT_EQ(list(harness.port, used).status, ok);
    EXPECT_EQ(harness.http->session_count(), 2U);
}

// The newcomer that would take the place of the only session, and that session's first request, sent at once, many
// times. Exactly one wins: either the session was used first (it answers, and the newcomer is refused) or it gave its
// place first (it is unknown, and the newcomer has a session). Never both, and never a table over its bound.
TEST(HttpUnusedSessions, DisplacementRacingTheFirstRequestOfTheDisplacedSessionHasOneWinner) {
    Harness harness(1);
    ASSERT_NE(harness.port, 0);
    std::string current = open_unused(harness.port);
    int used_first = 0;
    int displaced_first = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        SCOPED_TRACE(attempt);
        std::atomic<bool> go{false};
        Reply first_use;
        Reply newcomer;
        std::thread user([&] {
            while (!go.load()) std::this_thread::yield();
            first_use = ready(harness.port, current);
        });
        std::thread other([&] {
            while (!go.load()) std::this_thread::yield();
            newcomer = handshake(harness.port, 100 + attempt);
        });
        go = true;
        user.join();
        other.join();
        ASSERT_LE(harness.http->session_count(), 1U);
        if (first_use.status == accepted) {
            ++used_first;
            EXPECT_EQ(newcomer.status, unavailable);
            EXPECT_TRUE(newcomer.session.empty());
            EXPECT_EQ(list(harness.port, current).status, ok);
            // Make room for the next round: the used session ends, and a new one nobody used takes its place.
            httplib::Client client("127.0.0.1", harness.port);
            const httplib::Headers fields{{"Mcp-Session-Id", current}, {"MCP-Protocol-Version", legacy}};
            const auto ended = client.Delete(path, fields);
            ASSERT_TRUE(ended);
            EXPECT_EQ(ended->status, 204);
            current = open_unused(harness.port);
        } else {
            ++displaced_first;
            EXPECT_EQ(first_use.status, not_found);
            ASSERT_EQ(newcomer.status, ok);
            ASSERT_FALSE(newcomer.session.empty());
            current = newcomer.session;  // nobody used it: the next round races for it
        }
        EXPECT_EQ(harness.http->session_count(), 1U);
    }
    // Not a requirement, only what the rounds showed.
    std::cout << "used first: " << used_first << ", displaced first: " << displaced_first << "\n";
}

// Many handshakes at once against a table that holds sessions in use: every handshake gets a session (there is
// always a session nobody used whose place it can take), the table never holds more than its bound, and the sessions
// in use all keep their places.
TEST(HttpUnusedSessions, ManyHandshakesRacingEachOtherKeepTheBoundAndTheSessionsInUse) {
    constexpr std::size_t table = 8;
    constexpr std::size_t in_use = 4;
    constexpr int clients = 12;
    constexpr int each = 8;
    Harness harness(table);
    ASSERT_NE(harness.port, 0);
    std::vector<std::string> kept;
    for (std::size_t i = 0; i < in_use; ++i) kept.push_back(open_used(harness.port));

    std::atomic<int> granted{0};
    std::atomic<int> refused{0};
    std::atomic<int> without_id{0};
    std::atomic<bool> over_bound{false};
    std::vector<std::thread> threads;
    for (int c = 0; c < clients; ++c) {
        threads.emplace_back([&, c] {
            for (int i = 0; i < each; ++i) {
                const Reply opened = handshake(harness.port, c * each + i + 1);
                if (opened.status == ok) {
                    ++granted;
                    if (opened.session.empty()) ++without_id;
                } else {
                    ++refused;
                }
                if (harness.http->session_count() > table) over_bound = true;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(granted.load(), clients * each);
    EXPECT_EQ(refused.load(), 0);
    EXPECT_EQ(without_id.load(), 0);
    EXPECT_FALSE(over_bound.load());
    EXPECT_EQ(harness.http->session_count(), table);
    for (const std::string& session : kept) EXPECT_EQ(list(harness.port, session).status, ok);
}

}  // namespace
