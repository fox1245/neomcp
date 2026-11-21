// The Client reads the errors of the application's own back from a server of this library: in process (over the
// in-memory transport of stdio and of Streamable HTTP), over a child process on stdio, and over Streamable HTTP on a
// loopback port the operating system chooses, once for every revision the transport defines.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "checks.h"
#include "neomcp/client.h"
#include "neomcp/client/in_process.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "registrations.h"
#if defined(NEOMCP_APPLICATION_ERRORS_SERVER)
#include "neomcp/client/stdio.h"
#endif
#if defined(NEOMCP_APPLICATION_ERRORS_HTTP)
#include "neomcp/client/http.h"
#include "neomcp/http_server.h"
#include "http_sse_server.h"
#endif

namespace {

using neomcp::profile::Transport;

struct Cell {
    std::string name;
    Transport transport = Transport::stdio;
    std::string revision;
};

void PrintTo(const Cell& cell, std::ostream* out) { *out << cell.name; }

std::vector<Cell> cells(Transport transport, const std::string& prefix) {
    std::vector<Cell> found;
    for (const std::string& revision : application_errors::revisions_defining(transport)) {
        found.push_back(Cell{prefix + application_errors::test_name(revision), transport, revision});
    }
    return found;
}

std::string name_of(const ::testing::TestParamInfo<Cell>& info) { return info.param.name; }

neomcp::ServerOptions server_options() {
    neomcp::ServerOptions options{.name = "application-errors", .version = "1.0.0"};
    options.resources_page_size = 4;
    return options;
}

// ---- In process

class InProcess : public ::testing::TestWithParam<Cell> {};

TEST_P(InProcess, EveryHandlerAnswersWithItsOwnCodeMessageAndData) {
    const Cell& cell = GetParam();
    neomcp::Server server(server_options());
    application_errors::register_failing(server);
    auto peer = neomcp::InProcessServer::create(server, cell.transport);
    auto client = neomcp::Client::open(application_errors::client_options(cell.revision), peer->transport());
    ASSERT_TRUE(client) << client.error().message;
    application_errors::run_checks(*client, cell.revision);
}

std::vector<Cell> in_process_cells() {
    std::vector<Cell> found = cells(Transport::stdio, "Stdio_");
    for (Cell& cell : cells(Transport::streamable_http, "StreamableHttp_")) found.push_back(std::move(cell));
    return found;
}

INSTANTIATE_TEST_SUITE_P(ApplicationErrors, InProcess, ::testing::ValuesIn(in_process_cells()), name_of);

// ---- A task that fails, read back by the Client

// Under 2025-11-25, where tasks belong to the core protocol, the client asks for a task with the request it already
// has for any method, and tasks/result answers the error the call would have answered. (Under 2026-07-28 the requester
// declares the extension, which the Client has no option for yet: the wire tests of the server cover that revision.)
TEST(ApplicationErrorsClientTasks, TheClientReadsTheFailureOfATaskWithItsCodeMessageAndData) {
    neomcp::ServerOptions options = server_options();
    options.tasks.store = neomcp::make_memory_task_store({});
    options.tasks.min_poll_interval = std::chrono::milliseconds(1);
    neomcp::Server server(std::move(options));
    application_errors::register_task_tools(server);
    auto peer = neomcp::InProcessServer::create(server, Transport::stdio);
    const std::string revision = "2025-11-25";
    auto client = neomcp::Client::open(application_errors::client_options(revision), peer->transport());
    ASSERT_TRUE(client) << client.error().message;
    for (const char* tool : {"task_fail", "task_fail_coroutine"}) {
        const std::string_view kind = std::string_view(tool) == "task_fail" ? application_errors::kind_tool
                                                                             : application_errors::kind_tool_coroutine;
        for (const int code : {-32050, 7, -33000}) {
            neomcp::json params = neomcp::json::object();
            params["name"] = tool;
            params["arguments"] = neomcp::json{{"code", code}};
            params["task"] = neomcp::json::object();
            auto created = client->request("tools/call", std::move(params));
            ASSERT_TRUE(created) << created.error().message;
            const std::string id = (*created).at("task").at("taskId").get<std::string>();
            auto result = client->request("tasks/result", neomcp::json{{"taskId", id}});
            ASSERT_FALSE(result) << "the task did not fail";
            const std::string where = std::string(tool) + " " + std::to_string(code);
            EXPECT_EQ(result.error().kind, neomcp::ClientErrorKind::server_error) << where;
            EXPECT_EQ(result.error().code, std::optional<int>(code)) << where;
            EXPECT_EQ(result.error().message, application_errors::message_of(kind)) << where;
            EXPECT_EQ(result.error().data, application_errors::data_of(kind, code)) << where;
        }
        // A code the revision names: the task fails as an internal error.
        neomcp::json params = neomcp::json::object();
        params["name"] = tool;
        params["arguments"] = neomcp::json{{"code", -32002}};
        params["task"] = neomcp::json::object();
        auto created = client->request("tools/call", std::move(params));
        ASSERT_TRUE(created) << created.error().message;
        auto refused = client->request("tasks/result", neomcp::json{{"taskId", (*created).at("task").at("taskId")}});
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, std::optional<int>(neomcp::error_code::internal_error));
        EXPECT_TRUE(refused.error().data.is_null());
    }
}

#if defined(NEOMCP_APPLICATION_ERRORS_SERVER)

// ---- Over a child process

class Stdio : public ::testing::TestWithParam<Cell> {};

TEST_P(Stdio, EveryHandlerAnswersWithItsOwnCodeMessageAndData) {
    const Cell& cell = GetParam();
    neomcp::ChildProcess server;
    server.program = NEOMCP_APPLICATION_ERRORS_SERVER;
    auto client = neomcp::Client::open(application_errors::client_options(cell.revision), std::move(server));
    ASSERT_TRUE(client) << client.error().message;
    application_errors::run_checks(*client, cell.revision);
}

INSTANTIATE_TEST_SUITE_P(ApplicationErrors, Stdio, ::testing::ValuesIn(cells(Transport::stdio, "")), name_of);

#endif

#if defined(NEOMCP_APPLICATION_ERRORS_HTTP)

// ---- Over Streamable HTTP

class Http : public ::testing::TestWithParam<Cell> {};

TEST_P(Http, EveryHandlerAnswersWithItsOwnCodeMessageAndData) {
    const Cell& cell = GetParam();
    neomcp::Server server(server_options());
    application_errors::register_failing(server);
    neomcp::HttpServerOptions http;
    http.diagnostics = false;
    neomcp::HttpServer listener(server, std::move(http));
    const auto started = listener.start();
    ASSERT_TRUE(started) << started.error;
    neomcp::HttpEndpoint endpoint;
    endpoint.url = "http://127.0.0.1:" + std::to_string(*started.value) + "/mcp";
    // Loopback requests must not go to a proxy that the environment names.
    endpoint.proxy.mode = neomcp::ProxyMode::none;
    auto client = neomcp::Client::open(application_errors::client_options(cell.revision), std::move(endpoint));
    ASSERT_TRUE(client) << client.error().message;
    application_errors::run_checks(*client, cell.revision);
}

INSTANTIATE_TEST_SUITE_P(ApplicationErrors, Http,
                         ::testing::ValuesIn(cells(Transport::streamable_http, "")), name_of);

// ---- Over the older transport of events over HTTP (2024-11-05), served by the test-only server of that transport

class HttpSse : public ::testing::TestWithParam<Cell> {};

TEST_P(HttpSse, EveryHandlerAnswersWithItsOwnCodeMessageAndData) {
    const Cell& cell = GetParam();
    neomcp::Server server(server_options());
    application_errors::register_failing(server);
    http_sse_test::HttpSseServerOptions options;
    options.path = "/sse";
    http_sse_test::HttpSseServer listener(server, std::move(options));
    neomcp::HttpEndpoint endpoint;
    endpoint.url = listener.url();
    endpoint.proxy.mode = neomcp::ProxyMode::none;
    auto client = neomcp::Client::open(application_errors::client_options(cell.revision), std::move(endpoint));
    ASSERT_TRUE(client) << client.error().message;
    application_errors::run_checks(*client, cell.revision);
}

INSTANTIATE_TEST_SUITE_P(ApplicationErrors, HttpSse, ::testing::ValuesIn(cells(Transport::http_sse, "")), name_of);

#endif

}  // namespace
