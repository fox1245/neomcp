// Checks that sessions that were opened and never used again do not keep a new client from opening one. The endpoint
// keeps at most four sessions here, with the default idle timeout of half an hour; one client opens four sessions with
// one handshake request each and sends nothing more; a second client then asks for a session.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <chrono>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;

constexpr const char* path = "/mcp";
constexpr std::size_t table = 4;

std::string initialize(int id) {
    const json params{{"protocolVersion", "2025-11-25"},
                      {"capabilities", json::object()},
                      {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"}, {"params", params}}.dump();
}

httplib::Result open_one(std::uint16_t port, int id) {
    httplib::Client client("127.0.0.1", port);
    client.set_keep_alive(false);
    client.set_read_timeout(30, 0);
    return client.Post(path, {{"Accept", "application/json, text/event-stream"}}, initialize(id), "application/json");
}

TEST(HttpSessionTable, SessionsOpenedAndNeverUsedDoNotKeepANewClientOut) {
    neomcp::Server server(neomcp::ServerOptions{.name = "session-table-test", .version = "0.0.1"});
    server.tool("noop", "Do nothing", [] { return std::string("ok"); });
    neomcp::HttpServerOptions options;
    options.path = path;
    options.endpoint.max_sessions = table;  // the idle timeout keeps its default
    neomcp::HttpServer http(server, options);
    const auto started = http.start();
    ASSERT_TRUE(started) << started.error;
    const std::uint16_t port = *started.value;

    for (std::size_t i = 0; i < table; ++i) {
        const auto opened = open_one(port, static_cast<int>(i) + 1);
        ASSERT_TRUE(opened);
        ASSERT_EQ(opened->status, 200);
        ASSERT_FALSE(opened->get_header_value("Mcp-Session-Id").empty());
    }
    ASSERT_EQ(http.session_count(), table);

    const auto newcomer = open_one(port, 99);
    ASSERT_TRUE(newcomer);
    EXPECT_EQ(newcomer->status, 200) << "the new client was answered " << newcomer->status
                                     << " while the table held only sessions nobody used after their handshake";
    EXPECT_FALSE(newcomer->get_header_value("Mcp-Session-Id").empty());
}

}  // namespace
