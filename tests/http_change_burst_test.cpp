// Change notifications over HTTP after a burst of changes, on loopback sockets. The server's feed is held
// (support/feed_gate.h) while the test reports repeated and distinct resource updates, twice as many distinct ones as
// a session's queue holds, and two changes of the tool list; then it is released. The stream of a 2025-11-25 session
// (its GET) and the event stream of a 2026-07-28 subscription carry the list change first and once, each resource
// once, and every distinct resource: the feed drops none.
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/sse_parser.h"
#include "support/feed_gate.h"
#include "support/resource_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr std::size_t bound = 4;  // max_queued_notifications of the server's sessions
constexpr const char* gate_uri = "test://gate";
constexpr const char* live_uri = "test://live";
constexpr const char* last_uri = "test://last";

std::string item(std::size_t index) { return "test://item/" + std::to_string(index); }

std::vector<std::string> watched_uris() {
    std::vector<std::string> uris{live_uri, last_uri};
    for (std::size_t i = 0; i < 2 * bound; ++i) uris.push_back(item(i));
    return uris;
}

// A server with a resource at every URI above and the gate's, and a tool switched off, served over HTTP.
struct BurstServer {
    BurstServer() : server(make_options()) {
        std::vector<std::string> uris = watched_uris();
        uris.emplace_back(gate_uri);
        for (const std::string& uri : uris) server.resource(uri, uri, [] { return std::string("content"); });
        beta = std::make_unique<neomcp::ToolHandle>(
            server.tool("beta", "Offered later", [] { return std::string("beta"); }).enable(false));
        http = std::make_unique<neomcp::HttpServer>(server, quick_options());
        auto started = http->start();
        if (!started) throw std::runtime_error("start failed: " + started.error);
        port = *started.value;
    }
    ~BurstServer() {
        if (http) http->stop();
    }
    BurstServer(const BurstServer&) = delete;
    BurstServer& operator=(const BurstServer&) = delete;

    // Holds the feed, reports the burst, and releases the feed.
    void burst() {
        neomcp::testing::FeedGate gate(server, gate_uri, latest_legacy_revision);
        gate.hold(server);
        for (std::size_t round = 0; round < bound + 2; ++round) server.notify_resource_updated(item(0));
        for (std::size_t i = 1; i < 2 * bound; ++i) server.notify_resource_updated(item(i));
        beta->enable(true);
        beta->enable(false);
        gate.release();
    }

    static neomcp::ServerOptions make_options() {
        neomcp::ServerOptions options{.name = "burst-server", .version = "0.0.1"};
        options.max_queued_notifications = bound;
        return options;
    }

    neomcp::Server server;
    std::unique_ptr<neomcp::ToolHandle> beta;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

// The change notifications of an event stream in short ("tools", "updated <uri>"), each after the update of the live
// resource that showed the stream open; empty until that update arrived.
std::vector<std::string> changes_after_live(const std::string& body) {
    std::vector<std::string> out;
    bool live = false;
    neomcp::http::SseParser parser(body.size() + 1);
    parser.feed(body, [&](neomcp::http::SseItem entry) {
        const auto* event = std::get_if<neomcp::http::SseEvent>(&entry);
        if (event == nullptr) return;
        const json message = json::parse(event->data);
        const std::string method = message.value("method", std::string());
        std::string text;
        if (method == "notifications/tools/list_changed") {
            text = "tools";
        } else if (method == "notifications/resources/updated") {
            text = "updated " + message.at("params").at("uri").get<std::string>();
        } else {
            return;
        }
        if (live) out.push_back(std::move(text));
        if (text == std::string("updated ") + live_uri) live = true;
    });
    return out;
}

bool live_seen(const std::string& body) {
    const std::string marker = std::string("\"") + live_uri + "\"";
    return body.find(marker) != std::string::npos;
}

std::vector<std::string> expected_after_burst() {
    std::vector<std::string> out{"tools"};
    for (std::size_t i = 0; i < 2 * bound; ++i) out.push_back("updated " + item(i));
    return out;
}

// Shows the stream open, makes the burst, and checks what the stream carries after it.
void check_burst(BurstServer& served, Call& stream) {
    served.server.notify_resource_updated(live_uri);
    ASSERT_TRUE(stream.wait_until(live_seen)) << stream.now().body;
    served.burst();
    const std::vector<std::string> expected = expected_after_burst();
    ASSERT_TRUE(stream.wait_until(
        [&](const std::string& body) { return changes_after_live(body).size() >= expected.size(); }))
        << stream.now().body;
    EXPECT_EQ(changes_after_live(stream.now().body), expected);
    // Nothing else was pending: an update reported now is the next change on the stream.
    served.server.notify_resource_updated(last_uri);
    ASSERT_TRUE(stream.wait_until(
        [&](const std::string& body) { return changes_after_live(body).size() > expected.size(); }))
        << stream.now().body;
    const std::vector<std::string> all = changes_after_live(stream.now().body);
    EXPECT_EQ(all.at(expected.size()), std::string("updated ") + last_uri);
}

httplib::Headers session_headers(const std::string& session, const char* accept = both_media_types) {
    httplib::Headers headers{{"Content-Type", json_media_type},
                             {"Accept", accept},
                             {"MCP-Protocol-Version", latest_legacy_revision}};
    if (!session.empty()) headers.emplace("Mcp-Session-Id", session);
    return headers;
}

TEST(HttpChangeBurst, TheStreamOfASessionCarriesTheListChangeFirstAndEveryResource) {
    BurstServer served;
    const json params{{"protocolVersion", latest_legacy_revision},
                      {"capabilities", json::object()},
                      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    const Reply opened = send(served.port, "POST", session_headers({}), rpc(1, "initialize", params));
    ASSERT_EQ(opened.status, 200) << opened.body;
    const std::string session = opened.header("Mcp-Session-Id");
    ASSERT_FALSE(session.empty());
    const Reply ready = send(served.port, "POST", session_headers(session),
                             json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
    ASSERT_EQ(ready.status, 202);
    int id = 10;
    for (const std::string& uri : watched_uris()) {
        const Reply answered =
            send(served.port, "POST", session_headers(session), rpc(id++, "resources/subscribe", json{{"uri", uri}}));
        ASSERT_EQ(answered.status, 200) << answered.body;
        ASSERT_TRUE(answered.message().contains("result")) << answered.body;
    }
    Call stream(served.port, "GET", session_headers(session, "text/event-stream"));
    ASSERT_TRUE(stream.wait_for_headers());
    check_burst(served, stream);
    stream.disconnect();
}

TEST(HttpChangeBurst, TheStreamOfASubscriptionCarriesTheListChangeFirstAndEveryResource) {
    BurstServer served;
    const json filter{{"toolsListChanged", true}, {"resourceSubscriptions", watched_uris()}};
    Call stream(served.port, "POST", modern_headers("subscriptions/listen", std::nullopt),
                rpc(5, "subscriptions/listen", modern_params({{"notifications", filter}})));
    ASSERT_TRUE(stream.wait_until([](const std::string& body) {
        return body.find("notifications/subscriptions/acknowledged") != std::string::npos;
    })) << stream.now().body;
    check_burst(served, stream);
    stream.disconnect();
}

}  // namespace
