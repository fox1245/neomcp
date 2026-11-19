// Checks that one tool call whose argument a `pattern` of the tool's input schema checks does not keep the server from
// answering other clients. The argument is a 21-byte string, far below json_schema::Limits::max_pattern_input; the
// pattern is an ordinary host-name pattern. The listener has one worker, so that what holds it is seen at once.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using Clock = std::chrono::steady_clock;

constexpr const char* path = "/mcp";
constexpr const char* modern = "2026-07-28";

std::string modern_request(int id, const std::string& method, json params) {
    params["_meta"] = json{{"io.modelcontextprotocol/protocolVersion", modern},
                           {"io.modelcontextprotocol/clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                           {"io.modelcontextprotocol/clientCapabilities", json::object()}};
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
}

httplib::Headers modern_headers(const std::string& method, const std::string& name = {}) {
    httplib::Headers headers{{"Accept", "application/json, text/event-stream"},
                             {"MCP-Protocol-Version", modern},
                             {"Mcp-Method", method}};
    if (!name.empty()) headers.emplace("Mcp-Name", name);
    return headers;
}

std::unique_ptr<httplib::Client> client_for(std::uint16_t port) {
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_keep_alive(false);
    client->set_read_timeout(60, 0);
    return client;
}

TEST(HttpPatternCheck, ACallWithAShortCraftedArgumentDoesNotHoldTheServer) {
    neomcp::Server server(neomcp::ServerOptions{.name = "pattern-test", .version = "0.0.1"});
    neomcp::ToolDefinition lookup;
    lookup.name = "lookup";
    lookup.description = "Look a host up";
    lookup.input_schema = json::parse(
        R"({"type":"object","properties":{"host":{"type":"string","pattern":"^([a-z0-9]+-?)+[a-z0-9]$"}}})");
    server.tool(lookup, [](const json&, neomcp::Context&) { return neomcp::ToolResult{}; });

    neomcp::HttpServerOptions options;
    options.path = path;
    options.worker_threads = 1;
    options.max_worker_threads = 1;
    options.max_queued_connections = 4;
    options.disconnect_poll_interval = 20ms;
    neomcp::HttpServer http(server, options);
    const auto started = http.start();
    ASSERT_TRUE(started) << started.error;
    const std::uint16_t port = *started.value;

    // The crafted call: 20 letters and one character the pattern does not admit.
    auto crafted = std::async(std::launch::async, [port] {
        const json params{{"name", "lookup"}, {"arguments", {{"host", std::string(20, 'a') + "!"}}}};
        return client_for(port)->Post(path, modern_headers("tools/call", "lookup"),
                                      modern_request(1, "tools/call", params), "application/json");
    });
    std::this_thread::sleep_for(100ms);  // the crafted call reaches the worker first

    const auto start = Clock::now();
    const auto listed = client_for(port)->Post(path, modern_headers("tools/list"),
                                               modern_request(2, "tools/list", json::object()), "application/json");
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->status, 200);
    EXPECT_LT(waited, 500ms) << "the other client's tools/list waited " << waited.count() << " ms for the crafted call";

    const auto answered = crafted.get();
    ASSERT_TRUE(answered);
    EXPECT_EQ(answered->status, 200);
}

}  // namespace
