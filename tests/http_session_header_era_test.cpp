// The session header on the endpoint, over loopback sockets: a request of the 2026-07-28 revision, which has no
// sessions, is served on its own whatever session header it carries (the header names no session there, not even a
// live one, and the answer carries none), while the revisions that keep sessions still route by it.
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "support/resource_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr const char* session_header = "Mcp-Session-Id";

// A session of 2025-11-25 that is opened (the handshake was answered) and was never used after: its client did not
// send notifications/initialized. Empty when the handshake was refused.
std::string handshake_only(std::uint16_t port, const std::string& token) {
    const json params{{"protocolVersion", latest_legacy_revision},
                      {"capabilities", json::object()},
                      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    const Reply opened = send(port, "POST", legacy_headers({}, token), rpc(1, "initialize", params));
    return opened.status == 200 ? opened.header(session_header) : std::string();
}

// tools/list of 2026-07-28 carrying `session` in the session header.
Reply modern_with_session(std::uint16_t port, const std::string& token, const std::string& session) {
    httplib::Headers headers = modern_headers("tools/list", token);
    headers.emplace(session_header, session);
    return send(port, "POST", std::move(headers), rpc(1, "tools/list", modern_params()));
}

void expect_served_on_its_own(const Reply& reply, const std::string& label) {
    EXPECT_EQ(reply.status, 200) << label << ": " << reply.body;
    EXPECT_EQ(reply.count(session_header), 0u) << label;
    if (reply.status == 200) {
        EXPECT_TRUE(reply.message().at("result").contains("tools")) << label << ": " << reply.body;
    }
}

TEST(HttpSessionHeaderEra, ARequestOfTheRevisionWithoutSessionsIsServedOnItsOwnWhateverSessionItNames) {
    Harness harness;
    auto& clock = harness.clock;
    const std::string alice = harness.token("alice", granted(clock, "alice", {"mcp:use"}));
    const std::string bob = harness.token("bob", granted(clock, "bob", {"mcp:use"}));
    const std::string session = open_session(harness.port, alice);
    ASSERT_EQ(harness.http->session_count(), 1u);

    expect_served_on_its_own(modern_with_session(harness.port, alice, session), "the live session of its principal");
    expect_served_on_its_own(modern_with_session(harness.port, bob, session), "the live session of another principal");
    expect_served_on_its_own(modern_with_session(harness.port, alice, std::string(64, 'a')), "no session at all");
    EXPECT_EQ(harness.http->session_count(), 1u) << "no session was kept for them";

    // GET and DELETE of that revision have no session to name either: the revision offers neither.
    for (const char* method : {"GET", "DELETE"}) {
        httplib::Headers headers{{"Accept", "text/event-stream"},
                                 {"MCP-Protocol-Version", modern_revision},
                                 {"Authorization", "Bearer " + alice},
                                 {session_header, session}};
        const Reply reply = send(harness.port, method, std::move(headers));
        EXPECT_EQ(reply.status, 405) << method << ": " << reply.body;
    }

    // The session's own revision still routes by the header: its principal reaches it, another one does not, and it
    // is still there after the requests above.
    EXPECT_EQ(ping(harness.port, session, alice).status, 200);
    EXPECT_EQ(ping(harness.port, session, bob).status, 404);
    EXPECT_EQ(harness.http->session_count(), 1u);
}

// A session nobody used gives its place to a new handshake when the table is full; a session some request named does
// not. The request of 2026-07-28 that names it is not one of its requests, so the session is still unused after it.
TEST(HttpSessionHeaderEra, ARequestOfTheRevisionWithoutSessionsDoesNotTouchTheSessionItNames) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.endpoint.max_sessions = 1; });
    const std::string alice = harness.token("alice", granted(harness.clock, "alice", {"mcp:use"}));
    const std::string unused = handshake_only(harness.port, alice);
    ASSERT_FALSE(unused.empty());

    expect_served_on_its_own(modern_with_session(harness.port, alice, unused), "an unused session");

    const std::string newcomer = handshake_only(harness.port, alice);
    EXPECT_FALSE(newcomer.empty()) << "the unused session gave its place";
    EXPECT_NE(newcomer, unused);
    EXPECT_EQ(ping(harness.port, unused, alice).status, 404) << "the unused session ended for the newcomer";
    EXPECT_EQ(harness.http->session_count(), 1u);
}

}  // namespace
