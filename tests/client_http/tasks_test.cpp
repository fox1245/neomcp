// The client's half of tasks over Streamable HTTP with the library's own HTTP server (revision 2026-07-28 with the
// extension io.modelcontextprotocol/tasks): each request about a task is an exchange of its own, which carries the
// task's id in its header as the revision asks and which the server checks against the body.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <variant>

#include "../conformance/everything.h"
#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"
#include "rig.h"
#include "support.h"

namespace {

using neomcp::ClientErrorKind;
using neomcp::json;
using ms = std::chrono::milliseconds;

constexpr std::string_view revision = "2026-07-28";

struct TaskHttpServer {
    TaskHttpServer() : server(options()) {
        conformance::register_task_tools(server);
        neomcp::HttpServerOptions http_options;
        http_options.keep_alive_interval = std::chrono::hours(1);
        http_options.max_worker_threads = 32;
        http = std::make_unique<neomcp::HttpServer>(server, std::move(http_options));
        const auto started = http->start();
        if (!started) throw std::runtime_error("the server did not start: " + started.error);
        port = *started.value;
    }
    static neomcp::ServerOptions options() {
        neomcp::ServerOptions made = conformance::task_options();
        neomcp::MemoryTaskStoreOptions store;
        store.poll_interval = ms(10);
        made.tasks.store = neomcp::make_memory_task_store(store);
        made.tasks.min_poll_interval = ms(1);
        return made;
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; }

    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

TEST(ClientTasksHttp, FollowsTasksOfTheLibrarysServer) {
    TaskHttpServer own;
    neomcp::ClientOptions options = client_test::only({revision});
    options.tasks.accept = true;
    options.tasks.min_poll_interval = ms(5);
    options.tasks.max_poll_interval = ms(20);
    options.handlers.form([](const neomcp::ElicitationForm&) {
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"value", "ada"}}};
    });
    auto client = neomcp::Client::open(std::move(options), http_rig::endpoint(own.url()));
    ASSERT_TRUE(client) << client.error().message;
    const auto computed = client->call_tool("slow_compute", json{{"seconds", 0.05}});
    ASSERT_TRUE(computed) << computed.error().message;
    EXPECT_EQ(text_of(*computed), "Computed after 50 ms");
    const auto both = client->call_tool("multi_input");
    ASSERT_TRUE(both) << both.error().message;
    EXPECT_EQ(text_of(*both), R"(first={"value":"ada"}, second={"value":"ada"})");
    // A handle, followed again from its id, and a cancellation the server acknowledges.
    auto started = client->start_tool("slow_compute", json{{"seconds", 30}});
    ASSERT_TRUE(started) << started.error().message;
    ASSERT_TRUE(std::holds_alternative<neomcp::TaskHandle>(*started));
    const auto again = client->task(std::get<neomcp::TaskHandle>(*started).id());
    ASSERT_TRUE(again) << again.error().message;
    ASSERT_TRUE(again->cancel());
    const auto now = again->status();
    ASSERT_TRUE(now) << now.error().message;
    EXPECT_EQ(now->status, neomcp::TaskStatus::cancelled);
    const auto waited = again->wait();
    ASSERT_FALSE(waited);
    EXPECT_EQ(waited.error().kind, ClientErrorKind::task_cancelled);
    EXPECT_TRUE(client->close());
}

}  // namespace
