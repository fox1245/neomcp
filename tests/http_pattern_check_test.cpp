// Checks that one tool call whose argument a `pattern` of the tool's input schema checks does not keep the server from
// answering other clients, even after the caller left. The pattern is an ordinary host-name pattern. The argument is
// the shortest string (at most 65 bytes, far below json_schema::Limits::max_pattern_input) whose check takes `slow` or
// more in this build when the test measures it on its own; a matcher whose work grows linearly with the input never
// gets there, and the argument is then 65 bytes. The listener has one worker, so that what holds it is seen at once.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/json_schema.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr const char* path = "/mcp";
constexpr const char* modern = "2026-07-28";
constexpr const char* schema =
    R"({"type":"object","properties":{"host":{"type":"string","pattern":"^([a-z0-9]+-?)+[a-z0-9]$"}}})";
constexpr milliseconds slow{600};
constexpr milliseconds head_start{150};
constexpr std::size_t longest = 64;

json arguments_of(std::size_t letters) { return json{{"host", std::string(letters, 'a') + "!"}}; }

// The number of letters whose check takes `slow` or more here, or `longest` when none up to it does.
std::size_t crafted_letters() {
    auto made = neomcp::json_schema::Validator::compile(json::parse(schema));
    const auto& validator = std::get<neomcp::json_schema::Validator>(made);
    for (std::size_t n = 12; n <= longest; n += 2) {
        const auto start = Clock::now();
        (void)validator.validate(arguments_of(n));
        if (Clock::now() - start >= slow) return n;
    }
    return longest;
}

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

std::shared_ptr<httplib::Client> client_for(std::uint16_t port) {
    auto client = std::make_shared<httplib::Client>("127.0.0.1", port);
    client->set_keep_alive(false);
    client->set_read_timeout(60, 0);
    return client;
}

TEST(HttpPatternCheck, ACallWithAShortCraftedArgumentDoesNotHoldTheServerAfterItsCallerLeft) {
    const std::size_t letters = crafted_letters();

    neomcp::Server server(neomcp::ServerOptions{.name = "pattern-test", .version = "0.0.1"});
    neomcp::ToolDefinition lookup;
    lookup.name = "lookup";
    lookup.description = "Look a host up";
    lookup.input_schema = json::parse(schema);
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

    const auto caller = client_for(port);
    auto crafted = std::async(std::launch::async, [caller, letters] {
        const json params{{"name", "lookup"}, {"arguments", arguments_of(letters)}};
        return caller->Post(path, modern_headers("tools/call", "lookup"), modern_request(1, "tools/call", params),
                            "application/json");
    });
    std::this_thread::sleep_for(head_start);  // the crafted call reaches the worker first
    caller->stop();                           // and its caller leaves

    const auto start = Clock::now();
    const auto listed = client_for(port)->Post(path, modern_headers("tools/list"),
                                               modern_request(2, "tools/list", json::object()), "application/json");
    const auto waited = std::chrono::duration_cast<milliseconds>(Clock::now() - start);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->status, 200);
    EXPECT_LT(waited, 100ms) << "another client's tools/list waited " << waited.count() << " ms for a call with a "
                             << letters + 1 << "-byte argument whose caller had left";
    (void)crafted.get();
}

}  // namespace
