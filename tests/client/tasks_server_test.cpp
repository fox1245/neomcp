// The client's half of tasks against the library's own server in the same process, over stdio and over Streamable
// HTTP (the in-memory transport and the in-process server): a tool call the server answers with a task is followed to
// its result, the requests the task waits on reach the client's handlers, the task's handle, cancellation, and a
// client that did not opt in. The server is the conformance fixture's, with tasks on (task_options and
// register_task_tools), and one tool of the test's own that runs until it is stopped.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

#include "../conformance/everything.h"
#include "neomcp/client.h"
#include "neomcp/client/in_process.h"
#include "neomcp/server.h"
#include "support.h"

namespace {

using namespace client_test;
using neomcp::ClientErrorKind;
using neomcp::profile::Transport;
using ms = std::chrono::milliseconds;

constexpr std::string_view revision = "2026-07-28";
constexpr int missing_capability_code = -32021;

// The tool of the test: it runs until its stop token is requested, and says so.
struct Held {
    std::atomic<bool> entered{false};
    std::atomic<bool> stopped{false};
};

neomcp::ServerOptions task_server_options(std::string_view rev = revision) {
    neomcp::ServerOptions options = conformance::task_options();
    neomcp::MemoryTaskStoreOptions store;
    store.poll_interval = ms(10);
    options.tasks.store = neomcp::make_memory_task_store(store);
    options.tasks.min_poll_interval = ms(1);
    options.profiles.push_back(neomcp::profile::builtin(rev));
    return options;
}

class TaskPair {
public:
    TaskPair(Transport transport, bool accept, std::shared_ptr<neomcp::TimerService> timers = nullptr,
             std::string_view rev = revision)
        : held(std::make_shared<Held>()), server_(task_server_options(rev)) {
        conformance::register_task_tools(server_);
        server_
            .tool("held", "Run until stopped",
                  [state = held](neomcp::Context& call) {
                      state->entered = true;
                      static_cast<void>(conformance::work_for(call, std::chrono::seconds(30)));
                      state->stopped = call.cancelled();
                      return std::string("ran");
                  })
            .task_support(neomcp::TaskSupport::optional);
        peer = neomcp::InProcessServer::create(server_, transport);
        neomcp::ClientOptions options = only({rev});
        options.tasks.accept = accept;
        options.timers = std::move(timers);
        options.tasks.min_poll_interval = ms(5);
        options.tasks.max_poll_interval = ms(20);
        options.handlers.form([this](const neomcp::ElicitationForm& form) {
            {
                const std::lock_guard lock(mutex_);
                asked_.push_back(form.message);
            }
            json content = json::object();
            for (const auto& [property, schema] : form.requested_schema["properties"].items()) {
                content[property] = schema.value("type", std::string()) == "boolean" ? json(true) : json("ada");
            }
            return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, content};
        });
        auto opened = neomcp::Client::open(std::move(options), peer->transport());
        if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
        client.emplace(std::move(opened).value());
    }

    std::vector<std::string> asked() {
        const std::lock_guard lock(mutex_);
        return asked_;
    }
    neomcp::Server& server() { return server_; }

    std::shared_ptr<Held> held;

private:
    std::mutex mutex_;
    std::vector<std::string> asked_;
    neomcp::Server server_;

public:
    std::shared_ptr<neomcp::InProcessServer> peer;
    std::optional<neomcp::Client> client;
};

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

class ClientTasksServer : public ::testing::TestWithParam<Transport> {};

TEST_P(ClientTasksServer, ACallFollowsItsTaskToTheResult) {
    TaskPair pair(GetParam(), true);
    const auto computed = pair.client->call_tool("slow_compute", json{{"seconds", 0.05}});
    ASSERT_TRUE(computed) << computed.error().message;
    EXPECT_EQ(text_of(*computed), "Computed after 50 ms");
    EXPECT_FALSE(computed->is_error);
    // A tool that never runs as a task answers as before.
    const auto greeted = pair.client->call_tool("greet", json{{"name", "ada"}});
    ASSERT_TRUE(greeted) << greeted.error().message;
    EXPECT_EQ(text_of(*greeted), "Hello, ada!");
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, AToolErrorCompletesTheTaskAndAnErrorOfTheCallFailsIt) {
    TaskPair pair(GetParam(), true);
    const auto failing = pair.client->call_tool("failing_job");
    ASSERT_TRUE(failing) << failing.error().message;
    EXPECT_TRUE(failing->is_error);
    const auto broken = pair.client->call_tool("protocol_error_job");
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().kind, ClientErrorKind::server_error);
    ASSERT_TRUE(broken.error().task);
    EXPECT_FALSE(broken.error().task->empty());
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, WhatTheTaskWaitsOnReachesTheHandlersOnce) {
    TaskPair pair(GetParam(), true);
    const auto both = pair.client->call_tool("multi_input");
    ASSERT_TRUE(both) << both.error().message;
    EXPECT_EQ(text_of(*both), R"(first={"value":"ada"}, second={"value":"ada"})");
    // The two requests of one round reach the handlers once each; the handlers' pool may take them in either order.
    std::vector<std::string> asked = pair.asked();
    std::sort(asked.begin(), asked.end());
    EXPECT_EQ(asked, (std::vector<std::string>{"What is the first value?", "What is the second value?"}));
    const auto confirmed = pair.client->call_tool("confirm_delete", json{{"filename", "a.txt"}});
    ASSERT_TRUE(confirmed) << confirmed.error().message;
    EXPECT_EQ(text_of(*confirmed), "Deleted (action=accept)");
    EXPECT_EQ(pair.asked().size(), 3u);
    // A round before the task exists, then the task.
    const auto greeted = pair.client->call_tool("test_tool_with_task");
    ASSERT_TRUE(greeted) << greeted.error().message;
    EXPECT_EQ(text_of(*greeted), "Hello, ada!");
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, StartToolGivesAHandleThatAnIdGivesAgain) {
    TaskPair pair(GetParam(), true);
    auto started = pair.client->start_tool("slow_compute", json{{"seconds", 0.1}});
    ASSERT_TRUE(started) << started.error().message;
    ASSERT_TRUE(std::holds_alternative<neomcp::TaskHandle>(*started));
    const auto& handle = std::get<neomcp::TaskHandle>(*started);
    EXPECT_FALSE(handle.id().empty());
    auto again = pair.client->task(handle.id());
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(again->id(), handle.id());
    const auto result = again->wait();
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(text_of(*result), "Computed after 100 ms");
    const auto now = handle.status();
    ASSERT_TRUE(now) << now.error().message;
    EXPECT_EQ(now->status, neomcp::TaskStatus::completed);
    ASSERT_TRUE(now->result);
    // A tool that answers at once gives its result.
    auto greeted = pair.client->start_tool("greet", json{{"name", "ada"}});
    ASSERT_TRUE(greeted) << greeted.error().message;
    ASSERT_TRUE(std::holds_alternative<neomcp::ToolCall>(*greeted));
    EXPECT_EQ(text_of(std::get<neomcp::ToolCall>(*greeted)), "Hello, ada!");
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, StopCancelsTheTaskOnTheServer) {
    // The test's clocks, so that the stop comes while the client waits for its next read (the task is known then).
    auto timers = std::make_shared<ManualTimers>();
    TaskPair pair(GetParam(), true, timers);
    std::stop_source stop;
    std::optional<neomcp::Result<neomcp::ToolCall>> called;
    std::thread caller([&] {
        neomcp::RequestOptions options;
        options.stop = stop.get_token();
        called.emplace(pair.client->call_tool("held", json::object(), options));
    });
    const OnExit join_caller([&] { join(caller); });
    ASSERT_TRUE(eventually([&] { return pair.held->entered.load(); }));
    ASSERT_TRUE(timers->live([](const ManualTimers::Started& one) {
        const auto* deadline = std::get_if<neomcp::Deadline>(&one.purpose);
        return deadline != nullptr && deadline->kind == neomcp::DeadlineKind::poll;
    }));
    stop.request_stop();
    join(caller);
    ASSERT_TRUE(called);
    ASSERT_FALSE(*called);
    EXPECT_EQ(called->error().kind, ClientErrorKind::cancelled);
    ASSERT_TRUE(called->error().task);
    const std::string id = *called->error().task;
    // The handler sees its stop, and the task stays cancelled.
    EXPECT_TRUE(eventually([&] { return pair.held->stopped.load(); }));
    EXPECT_TRUE(eventually([&] {
        const auto record = pair.server().task(id, neomcp::TaskOwner{});
        return record && record->status == neomcp::TaskStatus::cancelled;
    }));
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, AClientThatDidNotOptInGetsOrdinaryAnswers) {
    TaskPair pair(GetParam(), false);
    // A tool that may run as a task runs as an ordinary call.
    const auto computed = pair.client->call_tool("slow_compute", json{{"seconds", 0.01}});
    ASSERT_TRUE(computed) << computed.error().message;
    EXPECT_EQ(text_of(*computed), "Computed after 10 ms");
    // One that runs only as a task is refused, as the server must refuse a client that does not take tasks.
    const auto required = pair.client->call_tool("failing_job");
    ASSERT_FALSE(required);
    EXPECT_EQ(required.error().kind, ClientErrorKind::server_error);
    EXPECT_EQ(required.error().code, std::optional<int>(missing_capability_code));
    EXPECT_FALSE(required.error().task);
    const auto handle = pair.client->task("anything");
    ASSERT_FALSE(handle);
    EXPECT_EQ(handle.error().kind, ClientErrorKind::unsupported_method);
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, AnIdTheServerDoesNotKnowIsTaskLost) {
    TaskPair pair(GetParam(), true);
    const auto handle = pair.client->task("no-such-task");
    ASSERT_FALSE(handle);
    EXPECT_EQ(handle.error().kind, ClientErrorKind::task_lost);
    EXPECT_EQ(handle.error().task, std::optional<std::string>("no-such-task"));
    EXPECT_TRUE(pair.client->close());
}

// A coroutine that follows a task holds no thread of the client: other calls are answered meanwhile.
TEST_P(ClientTasksServer, AnAwaitedTaskHoldsNoThread) {
    TaskPair pair(GetParam(), true);
    asio::io_context own;
    std::optional<neomcp::Result<neomcp::ToolCall>> awaited;
    std::stop_source stop;
    asio::co_spawn(
        own,
        [&]() -> asio::awaitable<void> {
            neomcp::RequestOptions options;
            options.stop = stop.get_token();
            awaited.emplace(co_await pair.client->async_call_tool("held", json::object(), options));
        },
        [](std::exception_ptr) {});
    std::thread runner([&] { own.run(); });
    const OnExit join_runner([&] {
        stop.request_stop();
        join(runner);
    });
    ASSERT_TRUE(eventually([&] { return pair.held->entered.load(); }));
    for (int i = 0; i < 3; ++i) {
        const auto greeted = pair.client->call_tool("greet", json{{"name", "ada"}});
        ASSERT_TRUE(greeted) << greeted.error().message;
    }
    stop.request_stop();
    join(runner);
    ASSERT_TRUE(awaited);
    ASSERT_FALSE(*awaited);
    EXPECT_EQ(awaited->error().kind, ClientErrorKind::cancelled);
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, Follows2025TasksAndHandlesInputErrorAndDoubleCancel) {
    TaskPair pair(GetParam(), true, nullptr, "2025-11-25");
    const auto missing = pair.client->task("no-such-task");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().kind, ClientErrorKind::task_lost);

    // Forbidden tool runs synchronously without task augmentation.
    const auto greeted = pair.client->call_tool("greet", json{{"name", "ada"}});
    ASSERT_TRUE(greeted) << greeted.error().message;
    EXPECT_EQ(text_of(*greeted), "Hello, ada!");

    // Optional slow tool executes as a task and completes through tasks/result.
    const auto computed = pair.client->call_tool("slow_compute", json{{"seconds", 0.05}});
    ASSERT_TRUE(computed) << computed.error().message;
    EXPECT_EQ(text_of(*computed), "Computed after 50 ms");

    // Optional tool asking for inputs delivers requests over tasks/result.
    const auto both = pair.client->call_tool("multi_input");
    ASSERT_TRUE(both) << both.error().message;
    EXPECT_EQ(text_of(*both), R"(first={"value":"ada"}, second={"value":"ada"})");
    EXPECT_FALSE(pair.asked().empty());

    // Required tool failing with ToolError completes with TaskResult{is_error=true}.
    const auto failed = pair.client->call_tool("failing_job");
    ASSERT_TRUE(failed) << failed.error().message;
    EXPECT_TRUE(failed->is_error);

    // start_tool + cancel; cancelling again after already terminal yields server_error (-32602), not task_lost.
    auto started = pair.client->start_tool("held");
    ASSERT_TRUE(started) << started.error().message;
    auto* handle = std::get_if<neomcp::TaskHandle>(&*started);
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(eventually([&] { return pair.held->entered.load(); }));
    ASSERT_TRUE(handle->cancel());
    EXPECT_TRUE(eventually([&] { return pair.held->stopped.load(); }));
    const auto again = handle->cancel();
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().kind, ClientErrorKind::server_error);
    EXPECT_EQ(again.error().code, -32602);

    const auto waited = handle->wait();
    ASSERT_FALSE(waited);
    EXPECT_EQ(waited.error().kind, ClientErrorKind::task_cancelled);
    EXPECT_TRUE(pair.client->close());
}

TEST_P(ClientTasksServer, Refuses2025RequiredToolWithoutTaskOptIn) {
    TaskPair pair(GetParam(), false, nullptr, "2025-11-25");
    const auto disabled = pair.client->task("no-such-task");
    ASSERT_FALSE(disabled);
    EXPECT_EQ(disabled.error().kind, ClientErrorKind::unsupported_method);
    const auto greeted = pair.client->call_tool("greet", json{{"name", "ada"}});
    ASSERT_TRUE(greeted) << greeted.error().message;
    EXPECT_EQ(text_of(*greeted), "Hello, ada!");

    const auto refused = pair.client->call_tool("failing_job");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::server_error);
    EXPECT_EQ(refused.error().code, -32601);
    EXPECT_TRUE(pair.client->close());
}

INSTANTIATE_TEST_SUITE_P(ClientTasks, ClientTasksServer, ::testing::Values(Transport::stdio, Transport::streamable_http),
                         [](const ::testing::TestParamInfo<Transport>& info) {
                             return info.param == Transport::stdio ? std::string("Stdio") : std::string("Http");
                         });

}  // namespace
