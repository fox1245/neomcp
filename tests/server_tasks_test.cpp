// Tasks run by the server: a tool call started as a task runs on the server's threads with a stop token of its own,
// takes its place in the server's bounds, and ends in the task store. No protocol message of tasks is involved yet: a
// host stands in for the session that will start them, and the tests read the store.
//
// Every wait is on an event a test controls; the bound on a wait only limits how long a broken build takes to fail.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <gtest/gtest.h>

#include "neomcp/jsonrpc.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::TaskOwner;
using neomcp::TaskRecord;
using neomcp::TaskStatus;
using neomcp::testing::Peer;

// Long enough for a sanitizer build on a loaded machine; reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* revision = "2025-06-18";
const TaskOwner nobody{};

class Event {
public:
    void set() {
        const std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return set_; });
    }
    // Whether it was raised; false when the wait ended because stop was requested.
    bool wait(std::stop_token token) {
        std::stop_callback wake(token, [this] {
            const std::lock_guard<std::mutex> lock(mutex_);
            changed_.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return set_ || token.stop_requested(); });
        return set_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

// Ends when stop is requested on the token, without holding up the io thread.
asio::awaitable<void> until_stopped(std::stop_token token) {
    const auto executor = co_await asio::this_coro::executor;
    auto timer = std::make_shared<asio::steady_timer>(executor, asio::steady_timer::time_point::max());
    std::stop_callback wake(token, [timer] { asio::post(timer->get_executor(), [timer] { timer->cancel(); }); });
    auto outcome = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    static_cast<void>(outcome);
}

neomcp::profile::ProfilePtr builtin(const std::string& name) {
    neomcp::profile::ProfilePtr found = neomcp::profile::builtin(name);
    if (!found) throw std::logic_error("no built-in profile " + name);
    return found;
}

neomcp::ToolResult text_result(std::string text) {
    neomcp::ToolResult result;
    neomcp::TextContent block;
    block.text = std::move(text);
    result.content.emplace_back(std::move(block));
    return result;
}

neomcp::ServerOptions options_with(std::shared_ptr<neomcp::TaskStore> store) {
    neomcp::ServerOptions options{.name = "tasks", .version = "1.0.0"};
    options.tasks.store = std::move(store);
    return options;
}

json call_of(const std::string& tool, int id) {
    return Peer::make_request(*builtin(revision), id, "tools/call", json{{"name", tool}, {"arguments", json::object()}});
}

// Stands in for a session that serves tasks: every call it is handed is started as a task of the server. The call is
// answered with the task's id (or busy when it was refused), or, when `hold` is set, kept open, so that the request
// that started the task is still in flight when the test ends it.
class TaskingHost final : public neomcp::ToolHost {
public:
    TaskingHost(neomcp::Server& server, neomcp::TaskStart start, bool hold = false)
        : server_(server), start_(std::move(start)), hold_(hold) {}

    std::span<const neomcp::ToolDefinition> tools() const override { return server_.tools(); }
    bool tool_enabled(std::size_t index) const override { return server_.tool_enabled(index); }

    void call(const neomcp::CallRequest& request, neomcp::CallControl control, neomcp::Completion done) override {
        neomcp::TaskStartResult started = server_.start_task(request, control, start_);
        const auto* record = std::get_if<TaskRecord>(&started);
        const std::string id = record != nullptr ? record->id : std::string();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            controls_.push_back(control);
            results_.push_back(started);
            if (hold_) held_.push_back(done);
            changed_.notify_all();
        }
        if (hold_) return;
        if (record != nullptr) {
            done(text_result(id));
        } else {
            done(neomcp::Refusal::busy);
        }
    }

    // The outcome of the n-th start, once there is one.
    neomcp::TaskStartResult result(std::size_t n) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, patience, [&] { return results_.size() > n; });
        if (results_.size() <= n) throw std::runtime_error("no start");
        return results_[n];
    }
    std::string id(std::size_t n) {
        const neomcp::TaskStartResult started = result(n);
        const auto* record = std::get_if<TaskRecord>(&started);
        if (record == nullptr) throw std::runtime_error("the task was refused");
        return record->id;
    }
    // Starts `request` with the CallControl of the n-th call, as a session would for a call of its own; for a tool the
    // session does not hand over (one that is switched off).
    neomcp::TaskStartResult start_with(std::size_t n, const neomcp::CallRequest& request) {
        static_cast<void>(result(n));
        std::optional<neomcp::CallControl> control;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            control.emplace(controls_[n]);
        }
        return server_.start_task(request, *control, start_);
    }
    // Whether stop was requested on the request that started the n-th task.
    bool origin_stopped(std::size_t n) {
        static_cast<void>(result(n));
        const std::lock_guard<std::mutex> lock(mutex_);
        return controls_[n].stop_token().stop_requested();
    }

private:
    neomcp::Server& server_;
    neomcp::TaskStart start_;
    bool hold_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<neomcp::CallControl> controls_;
    std::vector<neomcp::TaskStartResult> results_;
    std::vector<neomcp::Completion> held_;
};

neomcp::TaskRefusal refusal_of(const neomcp::TaskStartResult& started) {
    const auto* refused = std::get_if<neomcp::TaskRefusal>(&started);
    if (refused == nullptr) throw std::runtime_error("the task was started");
    return *refused;
}

// Told of a task's changes; raises `reached` once the task is in a status `wanted` accepts, or is gone.
class StatusWatcher final : public neomcp::TaskListener {
public:
    explicit StatusWatcher(std::function<bool(TaskStatus)> wanted) : wanted_(std::move(wanted)) {}
    void on_task_change(const neomcp::TaskChange& change) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (neomcp::is_terminal(change.status) && change.kind == neomcp::TaskChangeKind::updated) ++terminal_changes_;
        if (change.kind == neomcp::TaskChangeKind::removed || wanted_(change.status)) reached_.set();
    }
    Event& reached() { return reached_; }
    int terminal_changes() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return terminal_changes_;
    }

private:
    std::function<bool(TaskStatus)> wanted_;
    std::mutex mutex_;
    int terminal_changes_ = 0;
    Event reached_;
};

// The task once it is in a status `wanted` accepts (or once it is gone: empty then).
std::optional<TaskRecord> wait_for(neomcp::TaskStore& store, const std::string& id,
                                   const std::function<bool(TaskStatus)>& wanted) {
    auto watcher = std::make_shared<StatusWatcher>(wanted);
    neomcp::TaskWatchStart start = store.watch(id, nobody, watcher);
    if (start.outcome != neomcp::WatchOutcome::watching) return store.get(id, nobody);
    if (!wanted(start.record->status)) watcher->reached().wait(patience);
    return store.get(id, nobody);
}

std::optional<TaskRecord> wait_for_end(neomcp::TaskStore& store, const std::string& id) {
    return wait_for(store, id, [](TaskStatus status) { return neomcp::is_terminal(status); });
}

std::string result_text(const TaskRecord& record) {
    return record.result.value().at("content").at(0).at("text").get<std::string>();
}

bool contains(const std::optional<json>& value, const std::string& part) {
    return value && value->dump().find(part) != std::string::npos;
}

bool is_busy_error(const json& answer) {
    return answer.contains("error") && answer.at("error").at("code") == neomcp::error_code::internal_error &&
           answer.at("error").contains("data") && answer.at("error").at("data").at("reason") == "busy";
}

// Asks for the tool through `peer` until the server has room for it again (the end of a handler and the place it
// gives back are not observable from outside, so this retries). Returns the answer that was not busy.
std::optional<json> call_when_admitted(Peer& peer, const std::string& tool, int first_id) {
    const auto until = std::chrono::steady_clock::now() + patience;
    for (int id = first_id; std::chrono::steady_clock::now() < until; ++id) {
        std::optional<json> answer = peer.request(call_of(tool, id), patience);
        if (!answer) return answer;
        if (!is_busy_error(*answer)) return answer;
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

TEST(ServerTasksTest, OutlivesTheRequestAndTheSessionThatStartedIt) {
    auto store = neomcp::make_memory_task_store();
    Event entered;
    Event release;
    std::atomic<bool> saw_stop{false};
    std::mutex seen_mutex;
    std::optional<std::string> seen_id;
    neomcp::Server server(options_with(store));
    server
        .tool("gated", "Waits for the test",
              [&](neomcp::Context& ctx) {
                  {
                      const std::lock_guard<std::mutex> lock(seen_mutex);
                      if (ctx.task_id()) seen_id = std::string(*ctx.task_id());
                  }
                  entered.set();
                  release.wait(ctx.stop_token());
                  saw_stop = ctx.cancelled();
                  return std::string("done");
              })
        .task_support(neomcp::TaskSupport::optional);
    TaskingHost host(server, neomcp::TaskStart{}, true);
    {
        Peer peer(host);
        ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
        peer.send(call_of("gated", 1).dump());
        ASSERT_TRUE(entered.wait(patience));
        // The client gives the request up, then the session closes: both stop the request that started the task.
        peer.notify(json{{"jsonrpc", "2.0"},
                         {"method", "notifications/cancelled"},
                         {"params", {{"requestId", 1}, {"reason", "test"}}}});
        peer.close();
    }
    ASSERT_TRUE(host.origin_stopped(0));
    const std::string id = host.id(0);
    EXPECT_EQ(store->get(id, nobody).value().status, TaskStatus::working);
    release.set();
    const std::optional<TaskRecord> ended = wait_for_end(*store, id);
    ASSERT_TRUE(ended);
    EXPECT_EQ(ended->status, TaskStatus::completed);
    EXPECT_EQ(result_text(*ended), "done");
    EXPECT_FALSE(saw_stop.load());
    const std::lock_guard<std::mutex> lock(seen_mutex);
    EXPECT_EQ(seen_id, id);
}

TEST(ServerTasksTest, CancelStopsTheHandlerAndDropsItsLateResult) {
    auto store = neomcp::make_memory_task_store();
    Event entered;
    Event stopped;
    Event release;
    auto options = options_with(store);
    options.max_concurrent_calls = 1;
    options.max_pending_calls = 1;
    neomcp::Server server(std::move(options));
    server
        .tool("stubborn", "Ignores its stop token",
              [&](neomcp::Context& ctx) {
                  std::stop_callback seen(ctx.stop_token(), [&] { stopped.set(); });
                  entered.set();
                  release.wait(patience);
                  return std::string("late");
              })
        .task_support(neomcp::TaskSupport::optional);
    server.tool("probe", "Answers at once", [] { return std::string("probe"); });
    TaskingHost host(server, neomcp::TaskStart{});
    Peer starter(host);
    ASSERT_TRUE(starter.initialize(*builtin(revision), patience));
    ASSERT_TRUE(starter.request(call_of("stubborn", 1), patience));
    ASSERT_TRUE(entered.wait(patience));
    const std::string id = host.id(0);

    const neomcp::TaskCancelResult cancelled = server.cancel_task(id, nobody);
    EXPECT_EQ(cancelled.outcome, neomcp::TaskCancelOutcome::cancelled);
    ASSERT_TRUE(cancelled.record);
    EXPECT_EQ(cancelled.record->status, TaskStatus::cancelled);
    // Cancelled before the handler returned, and its stop token was requested.
    EXPECT_EQ(store->get(id, nobody).value().status, TaskStatus::cancelled);
    EXPECT_TRUE(stopped.wait(patience));

    // The probe waits behind the handler on the one worker; its answer comes after the handler's end was settled.
    Peer caller(server);
    ASSERT_TRUE(caller.initialize(*builtin(revision), patience));
    caller.send(call_of("probe", 7).dump());
    release.set();
    const std::optional<json> probed = caller.await_response(neomcp::RequestId(7), patience);
    ASSERT_TRUE(probed);
    ASSERT_TRUE(probed->contains("result")) << probed->dump();

    const TaskRecord after = store->get(id, nobody).value();
    EXPECT_EQ(after.status, TaskStatus::cancelled);
    EXPECT_FALSE(after.result);
    EXPECT_FALSE(after.error);
    EXPECT_EQ(server.cancel_task(id, nobody).outcome, neomcp::TaskCancelOutcome::ended);
    EXPECT_EQ(server.cancel_task(id, TaskOwner{"someone else"}).outcome, neomcp::TaskCancelOutcome::not_found);
}

TEST(ServerTasksTest, AHandlerThatThrowsFailsTheTaskWithoutItsText) {
    const std::string marker = "secret-detail-7f3a";
    auto store = neomcp::make_memory_task_store();
    neomcp::Server server(options_with(store));
    server.tool("throws", "Throws", [&]() -> std::string { throw std::runtime_error(marker); })
        .task_support(neomcp::TaskSupport::optional);
    neomcp::ToolDefinition broken;
    broken.name = "broken";
    broken.input_schema = json{{"type", "object"}};
    broken.output_schema = json{{"type", "object"}, {"required", json::array({"n"})}};
    // Its result lacks the structured content its output schema needs: a defect of the server program.
    server.tool(broken, [](const json&, neomcp::Context&) { return text_result("no structure"); })
        .task_support(neomcp::TaskSupport::optional);

    neomcp::TaskStart fails_on_error;
    fails_on_error.error_result_fails = true;
    TaskingHost failing(server, fails_on_error);
    Peer peer(failing);
    ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(peer.request(call_of("throws", 1), patience));
    ASSERT_TRUE(peer.request(call_of("broken", 2), patience));

    const TaskRecord thrown = wait_for_end(*store, failing.id(0)).value();
    EXPECT_EQ(thrown.status, TaskStatus::failed);
    ASSERT_TRUE(thrown.result);
    EXPECT_EQ(thrown.result->at("isError"), true);
    EXPECT_FALSE(contains(thrown.result, marker));
    EXPECT_FALSE(contains(thrown.error, marker));
    EXPECT_FALSE(thrown.status_message && thrown.status_message->find(marker) != std::string::npos);

    const TaskRecord defect = wait_for_end(*store, failing.id(1)).value();
    EXPECT_EQ(defect.status, TaskStatus::failed);
    ASSERT_TRUE(defect.error);
    EXPECT_EQ(defect.error->at("code"), neomcp::error_code::internal_error);
    EXPECT_FALSE(contains(defect.error, "schema"));
    EXPECT_FALSE(contains(defect.error, "broken"));
    EXPECT_FALSE(defect.status_message && defect.status_message->find("schema") != std::string::npos);

    // Where a result marked as an error completes the task, it does so with that result.
    TaskingHost completing(server, neomcp::TaskStart{});
    Peer other(completing);
    ASSERT_TRUE(other.initialize(*builtin(revision), patience));
    ASSERT_TRUE(other.request(call_of("throws", 1), patience));
    const TaskRecord completed = wait_for_end(*store, completing.id(0)).value();
    EXPECT_EQ(completed.status, TaskStatus::completed);
    EXPECT_EQ(completed.result.value().at("isError"), true);
    EXPECT_FALSE(contains(completed.result, marker));
}

TEST(ServerTasksTest, RunningTasksCountInTheWorkerBounds) {
    auto store = neomcp::make_memory_task_store();
    Event entered;
    Event release;
    auto options = options_with(store);
    options.max_concurrent_calls = 1;
    options.max_pending_calls = 0;
    neomcp::Server server(std::move(options));
    server
        .tool("stubborn", "Ignores its stop token",
              [&] {
                  entered.set();
                  release.wait(patience);
                  return std::string("late");
              })
        .task_support(neomcp::TaskSupport::optional);
    server.tool("probe", "Answers at once", [] { return std::string("probe"); })
        .task_support(neomcp::TaskSupport::optional);
    TaskingHost host(server, neomcp::TaskStart{});
    Peer starter(host);
    ASSERT_TRUE(starter.initialize(*builtin(revision), patience));
    Peer caller(server);
    ASSERT_TRUE(caller.initialize(*builtin(revision), patience));

    ASSERT_TRUE(starter.request(call_of("stubborn", 1), patience));
    ASSERT_TRUE(entered.wait(patience));
    const std::string id = host.id(0);

    // The running task holds the one worker: another task is refused without a record, and so is a call.
    ASSERT_TRUE(starter.request(call_of("probe", 2), patience));
    EXPECT_EQ(refusal_of(host.result(1)), neomcp::TaskRefusal::busy);
    EXPECT_EQ(store->usage().tasks, 1U);
    const std::optional<json> refused = caller.request(call_of("probe", 3), patience);
    ASSERT_TRUE(refused);
    EXPECT_TRUE(is_busy_error(*refused)) << refused->dump();

    // Cancelled, its handler still runs: it keeps its place until it returns.
    EXPECT_EQ(server.cancel_task(id, nobody).outcome, neomcp::TaskCancelOutcome::cancelled);
    ASSERT_TRUE(starter.request(call_of("probe", 4), patience));
    EXPECT_EQ(refusal_of(host.result(2)), neomcp::TaskRefusal::busy);
    const std::optional<json> still = caller.request(call_of("probe", 5), patience);
    ASSERT_TRUE(still);
    EXPECT_TRUE(is_busy_error(*still)) << still->dump();
    EXPECT_EQ(store->usage().tasks, 1U);

    release.set();
    const std::optional<json> admitted = call_when_admitted(caller, "probe", 10);
    ASSERT_TRUE(admitted);
    EXPECT_TRUE(admitted->contains("result")) << admitted->dump();
}

TEST(ServerTasksTest, RunningCoroutineTasksCountInTheCoroutineBound) {
    auto store = neomcp::make_memory_task_store();
    Event entered;
    auto options = options_with(store);
    options.max_coroutine_calls = 1;
    neomcp::Server server(std::move(options));
    server
        .tool("waits", "Waits for its stop token",
              [&](neomcp::Context& ctx) -> asio::awaitable<std::string> {
                  entered.set();
                  co_await until_stopped(ctx.stop_token());
                  co_return std::string("stopped");
              })
        .task_support(neomcp::TaskSupport::optional);
    server.tool("quick", "Answers at once", []() -> asio::awaitable<std::string> { co_return std::string("quick"); });
    TaskingHost host(server, neomcp::TaskStart{});
    Peer starter(host);
    ASSERT_TRUE(starter.initialize(*builtin(revision), patience));
    Peer caller(server);
    ASSERT_TRUE(caller.initialize(*builtin(revision), patience));

    ASSERT_TRUE(starter.request(call_of("waits", 1), patience));
    ASSERT_TRUE(entered.wait(patience));
    ASSERT_TRUE(starter.request(call_of("waits", 2), patience));
    EXPECT_EQ(refusal_of(host.result(1)), neomcp::TaskRefusal::busy);
    const std::optional<json> refused = caller.request(call_of("quick", 3), patience);
    ASSERT_TRUE(refused);
    EXPECT_TRUE(is_busy_error(*refused)) << refused->dump();

    EXPECT_EQ(server.cancel_task(host.id(0), nobody).outcome, neomcp::TaskCancelOutcome::cancelled);
    const std::optional<json> admitted = call_when_admitted(caller, "quick", 10);
    ASSERT_TRUE(admitted);
    EXPECT_TRUE(admitted->contains("result")) << admitted->dump();
    EXPECT_EQ(store->get(host.id(0), nobody).value().status, TaskStatus::cancelled);
}

TEST(ServerTasksTest, StartingFailsTheTasksAnEarlierServerLeftUnfinished) {
    auto store = neomcp::make_memory_task_store();
    const auto make = [&] {
        neomcp::TaskDraft draft;
        draft.revision = revision;
        draft.request = json{{"name", "gone"}};
        return std::get<TaskRecord>(store->create(draft)).id;
    };
    const std::string working = make();
    const std::string waiting = make();
    const std::string done = make();
    const TaskStatus from_working[] = {TaskStatus::working};
    neomcp::TaskUpdate ask;
    ask.status = TaskStatus::input_required;
    ask.asked = json{{"k", json::object()}};
    ASSERT_EQ(store->transition(waiting, from_working, ask).outcome, neomcp::TransitionOutcome::applied);
    neomcp::TaskUpdate finish;
    finish.status = TaskStatus::completed;
    finish.result = json{{"content", json::array()}};
    ASSERT_EQ(store->transition(done, from_working, finish).outcome, neomcp::TransitionOutcome::applied);

    neomcp::Server server(options_with(store));
    for (const std::string& id : {working, waiting}) {
        const TaskRecord record = store->get(id, nobody).value();
        EXPECT_EQ(record.status, TaskStatus::failed);
        ASSERT_TRUE(record.error);
        EXPECT_EQ(record.error->at("code"), neomcp::error_code::internal_error);
    }
    EXPECT_EQ(store->get(done, nobody).value().status, TaskStatus::completed);
}

TEST(ServerTasksTest, AStoreWithoutRoomForAnOutcomeStillHasItsUnfinishedTasksFailed) {
    neomcp::MemoryTaskStoreOptions tight;
    tight.outcome_reserve_bytes = 0;
    auto store = neomcp::make_memory_task_store(tight);
    neomcp::TaskDraft draft;
    draft.revision = revision;
    const std::string id = std::get<TaskRecord>(store->create(draft)).id;
    neomcp::Server server(options_with(store));
    EXPECT_EQ(store->get(id, nobody).value().status, TaskStatus::failed);
}

TEST(ServerTasksTest, AToolThatDoesNotDeclareTasksNeverRunsAsOne) {
    auto store = neomcp::make_memory_task_store();
    std::atomic<int> runs{0};
    neomcp::Server server(options_with(store));
    const auto plain = server.tool("plain", "Not a task", [&] {
        ++runs;
        return std::string("plain");
    });
    server.tool("off", "Switched off", [&] { return std::string("off"); })
        .task_support(neomcp::TaskSupport::required)
        .enable(false);
    EXPECT_EQ(server.task_support(0), neomcp::TaskSupport::forbidden);
    EXPECT_EQ(server.task_support(1), neomcp::TaskSupport::required);
    TaskingHost host(server, neomcp::TaskStart{});
    Peer peer(host);
    ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(peer.request(call_of("plain", 1), patience));
    EXPECT_EQ(refusal_of(host.result(0)), neomcp::TaskRefusal::not_declared);
    EXPECT_EQ(store->usage().tasks, 0U);
    EXPECT_EQ(runs.load(), 0);
    neomcp::CallRequest off;
    off.name = "off";
    EXPECT_EQ(refusal_of(host.start_with(0, off)), neomcp::TaskRefusal::unknown_tool);
    neomcp::CallRequest missing;
    missing.name = "missing";
    EXPECT_EQ(refusal_of(host.start_with(0, missing)), neomcp::TaskRefusal::unknown_tool);
    EXPECT_EQ(store->usage().tasks, 0U);
    // Once the server serves, the declaration is fixed.
    EXPECT_THROW(plain.task_support(neomcp::TaskSupport::optional), std::logic_error);

    // Without a store, nothing is a task.
    neomcp::Server bare(neomcp::ServerOptions{.name = "bare", .version = "1.0.0"});
    bare.tool("declared", "", [] { return std::string("x"); }).task_support(neomcp::TaskSupport::optional);
    TaskingHost bare_host(bare, neomcp::TaskStart{});
    Peer bare_peer(bare_host);
    ASSERT_TRUE(bare_peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(bare_peer.request(call_of("declared", 1), patience));
    EXPECT_EQ(refusal_of(bare_host.result(0)), neomcp::TaskRefusal::off);
}

TEST(ServerTasksTest, TheContextNamesTheTaskAndSetsItsStatusMessage) {
    auto store = neomcp::make_memory_task_store();
    Event entered;
    Event release;
    neomcp::Server server(options_with(store));
    server
        .tool("reports", "Reports how far it got",
              [&](neomcp::Context& ctx) {
                  if (!ctx.task_id()) {
                      ctx.task_status("dropped: not a task");
                      return std::string("call");
                  }
                  ctx.task_status("halfway");
                  entered.set();
                  release.wait(ctx.stop_token());
                  return std::string(*ctx.task_id());
              })
        .task_support(neomcp::TaskSupport::optional);
    TaskingHost host(server, neomcp::TaskStart{});
    Peer starter(host);
    ASSERT_TRUE(starter.initialize(*builtin(revision), patience));
    ASSERT_TRUE(starter.request(call_of("reports", 1), patience));
    ASSERT_TRUE(entered.wait(patience));
    const std::string id = host.id(0);
    const TaskRecord running = store->get(id, nobody).value();
    EXPECT_EQ(running.status, TaskStatus::working);
    EXPECT_EQ(running.status_message, "halfway");

    Peer caller(server);
    ASSERT_TRUE(caller.initialize(*builtin(revision), patience));
    const std::optional<json> called = caller.request(call_of("reports", 2), patience);
    ASSERT_TRUE(called);
    EXPECT_EQ(called->at("result").at("content").at(0).at("text"), "call");

    release.set();
    const TaskRecord ended = wait_for_end(*store, id).value();
    EXPECT_EQ(ended.status, TaskStatus::completed);
    EXPECT_EQ(result_text(ended), id);
}

TEST(ServerTasksTest, DestroyingTheServerStopsAndFailsItsTasks) {
    auto store = neomcp::make_memory_task_store();
    Event entered;
    Event never;
    std::atomic<bool> saw_stop{false};
    std::string id;
    {
        neomcp::Server server(options_with(store));
        server
            .tool("waits", "Waits for its stop token",
                  [&](neomcp::Context& ctx) {
                      entered.set();
                      never.wait(ctx.stop_token());
                      saw_stop = ctx.cancelled();
                      return std::string("stopped");
                  })
            .task_support(neomcp::TaskSupport::optional);
        TaskingHost host(server, neomcp::TaskStart{});
        Peer peer(host);
        ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
        ASSERT_TRUE(peer.request(call_of("waits", 1), patience));
        ASSERT_TRUE(entered.wait(patience));
        id = host.id(0);
    }
    EXPECT_TRUE(saw_stop.load());
    const TaskRecord record = store->get(id, nobody).value();
    EXPECT_EQ(record.status, TaskStatus::failed);
    EXPECT_FALSE(record.result);
}

// A clock the test moves, shared by the store and the server.
class FakeClock {
public:
    std::chrono::system_clock::time_point now() const {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(millis_.load()));
    }
    void advance(std::chrono::milliseconds by) { millis_ += by.count(); }
    neomcp::TaskClock function() {
        return [this] { return now(); };
    }

private:
    std::atomic<std::int64_t> millis_{1'000'000'000};
};

TEST(ServerTasksTest, TheEndOfItsLifetimeStopsTheHandler) {
    FakeClock ticks;
    neomcp::MemoryTaskStoreOptions kept;
    kept.clock = ticks.function();
    auto store = neomcp::make_memory_task_store(kept);
    Event entered;
    Event never;
    Event returned;
    auto options = options_with(store);
    options.tasks.clock = ticks.function();
    options.tasks.sweep_interval = 5ms;
    neomcp::Server server(std::move(options));
    server
        .tool("waits", "Waits for its stop token",
              [&](neomcp::Context& ctx) {
                  entered.set();
                  never.wait(ctx.stop_token());
                  returned.set();
                  return std::string("stopped");
              })
        .task_support(neomcp::TaskSupport::optional);
    neomcp::TaskStart start;
    start.ttl = 1000ms;
    TaskingHost host(server, start);
    Peer peer(host);
    ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(peer.request(call_of("waits", 1), patience));
    ASSERT_TRUE(entered.wait(patience));
    const std::string id = host.id(0);
    ticks.advance(999ms);
    EXPECT_TRUE(store->get(id, nobody));
    ticks.advance(1ms);
    EXPECT_TRUE(returned.wait(patience));
    EXPECT_FALSE(store->get(id, nobody));
}

// Asks for a confirmation, as many times as `rounds` says (forever when it is empty), then answers with the last.
std::variant<std::string, neomcp::InputRequired> confirming(neomcp::Context& ctx, std::optional<int> rounds) {
    const int asked = ctx.request_state() ? std::stoi(*ctx.request_state()) : 0;
    if (asked > 0) {
        const auto answer = ctx.input_responses().find("confirm");
        if (answer == ctx.input_responses().end()) return std::string("no answer");
        const auto* confirm = std::get_if<neomcp::ElicitationAnswer>(&answer->second);
        if (confirm == nullptr) return std::string("not a form");
        if (rounds && asked >= *rounds) return confirm->content.at("word").get<std::string>();
    }
    neomcp::InputRequired ask;
    ask.requests.emplace("confirm", neomcp::ElicitationForm{"Go on?", json{{"type", "object"}}});
    ask.state = std::to_string(asked + 1);
    return ask;
}

neomcp::TaskStart asking_start() {
    neomcp::TaskStart start;
    start.write_input = [](const neomcp::InputRequest& request) -> std::optional<json> {
        const auto* form = std::get_if<neomcp::ElicitationForm>(&request);
        if (form == nullptr) return std::nullopt;
        return json{{"message", form->message}};
    };
    return start;
}

neomcp::AnswersResult answer(neomcp::Server& server, const std::string& id, const std::string& word) {
    neomcp::ElicitationAnswer accepted;
    accepted.action = neomcp::ElicitationAction::accept;
    accepted.content = json{{"word", word}};
    neomcp::InputResponses responses;
    responses.emplace("confirm", accepted);
    return server.answer_task(id, nobody, json{{"confirm", json{{"action", "accept"}}}}, std::move(responses));
}

bool waits_for_input(TaskStatus status) { return status == TaskStatus::input_required || neomcp::is_terminal(status); }

TEST(ServerTasksTest, AnswersEnterTheHandlerAgain) {
    auto store = neomcp::make_memory_task_store();
    neomcp::Server server(options_with(store));
    server
        .tool("confirm", "Asks twice",
              [](neomcp::Context& ctx) { return confirming(ctx, 2); })
        .task_support(neomcp::TaskSupport::optional);
    TaskingHost host(server, asking_start());
    Peer peer(host);
    ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(peer.request(call_of("confirm", 1), patience));
    const std::string id = host.id(0);
    for (const std::string word : {"first", "second"}) {
        const TaskRecord waiting = wait_for(*store, id, waits_for_input).value();
        ASSERT_EQ(waiting.status, TaskStatus::input_required);
        EXPECT_EQ(waiting.asked.value().at("confirm").at("message"), "Go on?");
        EXPECT_EQ(answer(server, id, word).outcome, neomcp::AnswersOutcome::applied);
    }
    const TaskRecord ended = wait_for_end(*store, id).value();
    EXPECT_EQ(ended.status, TaskStatus::completed);
    EXPECT_EQ(result_text(ended), "second");
}

TEST(ServerTasksTest, InputRoundsAreBoundedAndTimed) {
    auto store = neomcp::make_memory_task_store();
    auto options = options_with(store);
    options.max_input_rounds = 2;
    options.input_timeout = 50ms;
    options.tasks.sweep_interval = 5ms;
    neomcp::Server server(std::move(options));
    server.tool("forever", "Always asks", [](neomcp::Context& ctx) { return confirming(ctx, std::nullopt); })
        .task_support(neomcp::TaskSupport::optional);
    TaskingHost host(server, asking_start());
    Peer peer(host);
    ASSERT_TRUE(peer.initialize(*builtin(revision), patience));

    // The third round is one too many.
    ASSERT_TRUE(peer.request(call_of("forever", 1), patience));
    const std::string bounded = host.id(0);
    for (const std::string word : {"one", "two"}) {
        ASSERT_EQ(wait_for(*store, bounded, waits_for_input).value().status, TaskStatus::input_required);
        EXPECT_EQ(answer(server, bounded, word).outcome, neomcp::AnswersOutcome::applied);
    }
    const TaskRecord too_many = wait_for_end(*store, bounded).value();
    EXPECT_EQ(too_many.status, TaskStatus::failed);
    EXPECT_EQ(too_many.error.value().at("code"), neomcp::error_code::internal_error);

    // Nobody answers: the wait ends after the input time-out.
    ASSERT_TRUE(peer.request(call_of("forever", 2), patience));
    const TaskRecord unanswered = wait_for_end(*store, host.id(1)).value();
    EXPECT_EQ(unanswered.status, TaskStatus::failed);

    // A task whose starter cannot carry input fails when it asks.
    TaskingHost mute(server, neomcp::TaskStart{});
    Peer mute_peer(mute);
    ASSERT_TRUE(mute_peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(mute_peer.request(call_of("forever", 1), patience));
    EXPECT_EQ(wait_for_end(*store, mute.id(0)).value().status, TaskStatus::failed);
}

TEST(ServerTasksTest, CancelRacingCompletionEndsTheTaskOnce) {
    constexpr int round_count = 200;
    neomcp::MemoryTaskStoreOptions room;
    room.max_tasks_per_owner = round_count;
    auto store = neomcp::make_memory_task_store(room);
    struct Round {
        std::string id;
        neomcp::TaskCancelOutcome cancel = neomcp::TaskCancelOutcome::not_found;
        std::shared_ptr<StatusWatcher> watcher;
        neomcp::TaskWatch watch;
        int terminal_at_start = 0;
    };
    std::vector<Round> rounds;
    {
        // The rounds start faster than a slow machine finishes them: room in the queue of calls for all of them, so
        // none is refused as busy.
        neomcp::ServerOptions options = options_with(store);
        options.max_pending_calls = round_count;
        neomcp::Server server(std::move(options));
        server.tool("quick", "Answers at once", [] { return std::string("quick"); })
            .task_support(neomcp::TaskSupport::optional);
        TaskingHost host(server, neomcp::TaskStart{});
        Peer peer(host);
        ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
        for (int n = 0; n < round_count; ++n) {
            ASSERT_TRUE(peer.request(call_of("quick", n + 1), patience));
            Round round;
            round.id = host.id(static_cast<std::size_t>(n));
            round.watcher = std::make_shared<StatusWatcher>([](TaskStatus) { return false; });
            neomcp::TaskWatchStart start = store->watch(round.id, nobody, round.watcher);
            ASSERT_EQ(start.outcome, neomcp::WatchOutcome::watching);
            round.terminal_at_start = neomcp::is_terminal(start.record->status) ? 1 : 0;
            round.watch = std::move(start.watch);
            round.cancel = server.cancel_task(round.id, nobody).outcome;
            rounds.push_back(std::move(round));
        }
    }  // every handler has returned and its end was settled
    for (const Round& round : rounds) {
        const TaskRecord record = store->get(round.id, nobody).value();
        if (round.cancel == neomcp::TaskCancelOutcome::cancelled) {
            EXPECT_EQ(record.status, TaskStatus::cancelled);
            EXPECT_FALSE(record.result);
        } else {
            EXPECT_EQ(round.cancel, neomcp::TaskCancelOutcome::ended);
            EXPECT_EQ(record.status, TaskStatus::completed);
        }
        EXPECT_EQ(round.terminal_at_start + round.watcher->terminal_changes(), 1) << round.id;
    }
}

TEST(ServerTasksTest, ReadsSoonAfterTheLastAreServedFromIt) {
    auto store = neomcp::make_memory_task_store();
    Event entered;
    Event release;
    auto options = options_with(store);
    options.tasks.min_poll_interval = std::chrono::hours(1);
    neomcp::Server server(std::move(options));
    server
        .tool("gated", "Waits for the test",
              [&](neomcp::Context& ctx) {
                  entered.set();
                  release.wait(ctx.stop_token());
                  return std::string("done");
              })
        .task_support(neomcp::TaskSupport::optional);
    TaskingHost host(server, neomcp::TaskStart{});
    Peer peer(host);
    ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(peer.request(call_of("gated", 1), patience));
    ASSERT_TRUE(entered.wait(patience));
    const std::string id = host.id(0);

    EXPECT_FALSE(server.task(id, nobody).value().status_message);
    const TaskStatus from_working[] = {TaskStatus::working};
    neomcp::TaskUpdate elsewhere;
    elsewhere.message = "changed in the store";
    ASSERT_EQ(store->transition(id, from_working, elsewhere).outcome, neomcp::TransitionOutcome::applied);
    EXPECT_FALSE(server.task(id, nobody).value().status_message);
    EXPECT_FALSE(server.task(id, TaskOwner{"someone else"}));
    // A change the server makes is seen at once.
    EXPECT_EQ(server.cancel_task(id, nobody).outcome, neomcp::TaskCancelOutcome::cancelled);
    EXPECT_EQ(server.task(id, nobody).value().status, TaskStatus::cancelled);
    release.set();
}

TEST(ServerTasksTest, WatchesAreBounded) {
    auto store = neomcp::make_memory_task_store();
    auto options = options_with(store);
    options.tasks.max_waiting_results = 1;
    neomcp::Server server(std::move(options));
    server.tool("quick", "Answers at once", [] { return std::string("quick"); })
        .task_support(neomcp::TaskSupport::optional);
    TaskingHost host(server, neomcp::TaskStart{});
    Peer peer(host);
    ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
    ASSERT_TRUE(peer.request(call_of("quick", 1), patience));
    const std::string id = host.id(0);
    auto listener = std::make_shared<StatusWatcher>([](TaskStatus) { return false; });

    EXPECT_EQ(server.watch_task("no such task", nobody, listener).outcome, neomcp::WatchOutcome::not_found);
    neomcp::TaskWatchStart first = server.watch_task(id, nobody, listener);
    EXPECT_EQ(first.outcome, neomcp::WatchOutcome::watching);
    EXPECT_EQ(server.watch_task(id, nobody, listener).outcome, neomcp::WatchOutcome::limit);
    first.watch.release();
    EXPECT_EQ(server.watch_task(id, nobody, listener).outcome, neomcp::WatchOutcome::watching);
}

TEST(ServerTasksTest, OptionsThatCannotWorkAreRefused) {
    const auto refuses = [](const std::function<void(neomcp::TaskOptions&)>& change) {
        neomcp::ServerOptions options{.name = "tasks", .version = "1.0.0"};
        change(options.tasks);
        EXPECT_THROW({ neomcp::Server server(options); }, std::invalid_argument);
    };
    refuses([](neomcp::TaskOptions& tasks) { tasks.min_poll_interval = 0ms; });
    refuses([](neomcp::TaskOptions& tasks) { tasks.max_waiting_results = 0; });
    refuses([](neomcp::TaskOptions& tasks) { tasks.sweep_interval = 0ms; });
}

}  // namespace
