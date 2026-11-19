// Checks that what a legacy session keeps for the resources its client watches is bounded in bytes, not only in
// number. The server offers a resource template; one client opens a session and watches sixteen URIs of 512 KiB each
// (each within the message bound), then the memory the process holds is compared with what it held before.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "neomcp/http_server.h"
#include "neomcp/server.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define NEOMCP_TEST_SANITIZER_HEAP 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define NEOMCP_TEST_SANITIZER_HEAP 1
#endif
#endif
#if defined(NEOMCP_TEST_SANITIZER_HEAP)
// The sanitizer runtimes define it; not every compiler installs the header that declares it.
extern "C" std::size_t __sanitizer_get_current_allocated_bytes();
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

struct WatchedFile {
    std::string path;
};
NEOMCP_DESCRIBE(WatchedFile, path, "the path")

namespace {

using neomcp::json;

constexpr const char* path = "/mcp";
constexpr const char* legacy = "2025-11-25";
constexpr std::size_t uri_bytes = 512 * 1024;
constexpr int watched = 16;
constexpr std::size_t allowance = 2 * 1024 * 1024;  // far above what 16 bounded entries need

// The bytes the process's heap holds now, where the build can tell.
std::optional<std::size_t> heap_in_use() {
#if defined(NEOMCP_TEST_SANITIZER_HEAP)
    return __sanitizer_get_current_allocated_bytes();
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    // Blocks above the mmap threshold (as large as these URIs) are counted in hblkhd, not in uordblks.
    const struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
#else
    return std::nullopt;
#endif
}

std::string rpc(int id, const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
}

httplib::Headers headers(const std::string& session) {
    httplib::Headers fields{{"Accept", "application/json, text/event-stream"}, {"MCP-Protocol-Version", legacy}};
    if (!session.empty()) fields.emplace("Mcp-Session-Id", session);
    return fields;
}

httplib::Result post(std::uint16_t port, const std::string& session, const std::string& body) {
    httplib::Client client("127.0.0.1", port);
    client.set_keep_alive(false);
    client.set_read_timeout(30, 0);
    client.set_write_timeout(30, 0);
    return client.Post(path, headers(session), body, "application/json");
}

TEST(HttpWatchBytes, WatchingLongUrisKeepsABoundedNumberOfBytes) {
    if (!heap_in_use()) GTEST_SKIP() << "this build cannot tell how much the heap holds";
    neomcp::Server server(neomcp::ServerOptions{.name = "watch-test", .version = "0.0.1"});
    server.resource_template<WatchedFile>("file:///{+path}", "A file",
                                          [](const WatchedFile& file) { return file.path.substr(0, 8); });
    neomcp::HttpServerOptions options;
    options.path = path;
    neomcp::HttpServer http(server, options);
    const auto started = http.start();
    ASSERT_TRUE(started) << started.error;
    const std::uint16_t port = *started.value;

    const json init{{"protocolVersion", legacy},
                    {"capabilities", json::object()},
                    {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
    const auto opened = post(port, "", rpc(1, "initialize", init));
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200);
    const std::string session = opened->get_header_value("Mcp-Session-Id");
    ASSERT_FALSE(session.empty());
    const auto ready = post(port, session, json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->status, 202);

    const std::size_t before = *heap_in_use();
    for (int i = 0; i < watched; ++i) {
        const std::string uri = "file:///" + std::string(uri_bytes, 'a') + std::to_string(i);
        const auto answered = post(port, session, rpc(10 + i, "resources/subscribe", json{{"uri", uri}}));
        ASSERT_TRUE(answered);
        ASSERT_EQ(answered->status, 200);
        const json message = json::parse(answered->body);
        ASSERT_TRUE(message.contains("result")) << message.dump().substr(0, 300);
    }
    const std::size_t after = *heap_in_use();
    const std::size_t kept = after > before ? after - before : 0;
    EXPECT_LT(kept, allowance) << "the session keeps " << kept << " more bytes after watching " << watched
                               << " URIs of " << uri_bytes << " bytes";
}

}  // namespace
