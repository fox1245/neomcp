// Tasks on the wire, revision 2025-11-25, where the core protocol has tasks: a session of a server with a task store,
// through the in-memory peer. The requester asks for a task with `task` in the params of a call, of a tool whose listing
// says it may run as one; tasks/get reads the task, tasks/result waits for what it ended with, tasks/cancel cancels it.
// The input a task asks for, status notifications, listing and the differences from 2026-07-28 are in
// server_tasks_2025_flow_test.cpp; the tests over the Streamable HTTP endpoint in http_tasks_2025_test.cpp.
//
// Every wait is bounded; a bound is reached only when something is broken.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include "neomcp/jsonrpc.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::TaskSupport;
using neomcp::testing::Peer;

constexpr std::chrono::milliseconds patience = 30s;
constexpr std::chrono::milliseconds poll_step = 5ms;
// How long an answer that must not come yet is waited for.
constexpr std::chrono::milliseconds quiet = 300ms;
constexpr const char* legacy = "2025-11-25";
constexpr const char* related_task = "io.modelcontextprotocol/related-task";

class Event {
public:
    void set() {
        const std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds bound = patience) {
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

const neomcp::profile::Profile& profile_of(const std::string& revision) {
    const neomcp::profile::ProfilePtr found = neomcp::profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return *found;
}

neomcp::ServerOptions options_with(std::shared_ptr<neomcp::TaskStore> store) {
    neomcp::ServerOptions options{.name = "tasks-2025", .version = "1.0.0"};
    options.tasks.store = std::move(store);
    options.tasks.min_poll_interval = 1ms;
    return options;
}

std::shared_ptr<neomcp::TaskStore> store_of(neomcp::MemoryTaskStoreOptions options = {}) {
    return neomcp::make_memory_task_store(std::move(options));
}

// Opens the connection under 2025-11-25; the initialize result.
json open(Peer& peer, const json& capabilities = json::object()) {
    auto answer = peer.request(Peer::initialize_request(profile_of(legacy), 0, capabilities), patience);
    if (!answer || !answer->contains("result")) throw std::runtime_error("initialize failed");
    peer.notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    return (*answer)["result"];
}

json message(int id, const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

json ask(Peer& peer, int id, const std::string& method, json params) {
    auto answer = peer.request(message(id, method, std::move(params)), patience);
    if (!answer) throw std::runtime_error("no answer to " + method);
    return *answer;
}

// A call of `tool`; with `task` (an object) as its params' member when given.
json call(Peer& peer, int id, const std::string& tool, std::optional<json> task = json::object(),
          json arguments = json::object()) {
    json params{{"name", tool}, {"arguments", std::move(arguments)}};
    if (task) params["task"] = *task;
    return ask(peer, id, "tools/call", std::move(params));
}

std::string task_id_of(const json& created) { return created.at("result").at("task").at("taskId").get<std::string>(); }

bool terminal(const std::string& status) { return status == "completed" || status == "failed" || status == "cancelled"; }

json poll_until(Peer& peer, const std::string& task, const std::string& wanted = {}) {
    const auto until = std::chrono::steady_clock::now() + patience;
    int id = 10000;
    json answer;
    while (std::chrono::steady_clock::now() < until) {
        answer = ask(peer, ++id, "tasks/get", json{{"taskId", task}});
        if (!answer.contains("result")) return answer;
        const std::string status = answer["result"].value("status", "");
        if (wanted.empty() ? terminal(status) : status == wanted) return answer;
        std::this_thread::sleep_for(poll_step);
    }
    return answer;
}

void expect_task_fields(json task) {
    const std::string id = task.value("taskId", "");
    EXPECT_EQ(id.size(), 32u) << task.dump();
    EXPECT_TRUE(std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) << id;
    for (const char* member : {"createdAt", "lastUpdatedAt"}) {
        const std::string at = task.value(member, "");
        EXPECT_EQ(at.size(), 24u) << member << " " << at;
        EXPECT_EQ(at.back(), 'Z') << at;
    }
    EXPECT_TRUE(task["ttl"].is_number_integer()) << task.dump();
    EXPECT_TRUE(task["pollInterval"].is_number_integer()) << task.dump();
    EXPECT_FALSE(task.contains("ttlMs") || task.contains("resultType")) << "members of the other revision";
}

}  // namespace

// C1.1: with a store and a tool that may run as a task, initialize declares tasks (cancel, tools/call; no list without
// an authorization) and tools/list says per tool whether it may run as one; forbidden is the absence.
TEST(ServerTasks2025Test, CapabilitiesAndToolListingDeclareTasks) {
    neomcp::Server server(options_with(store_of()));
    server.tool("maybe", "Maybe a task", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    server.tool("always", "Only a task", [] { return std::string("done"); }).task_support(TaskSupport::required);
    server.tool("never", "Never a task", [] { return std::string("done"); });
    Peer peer(server);
    json opened = open(peer);
    json expected = json::parse(R"({"cancel":{},"requests":{"tools":{"call":{}}}})");
    EXPECT_EQ(opened["capabilities"]["tasks"], expected) << opened.dump();
    EXPECT_FALSE(opened["capabilities"].contains("extensions"));

    json listed = ask(peer, 1, "tools/list", json::object());
    ASSERT_TRUE(listed.contains("result")) << listed.dump();
    int seen = 0;
    for (json tool : listed["result"]["tools"]) {
        const std::string name = tool["name"];
        if (name == "maybe") {
            EXPECT_EQ(tool["execution"]["taskSupport"], "optional") << tool.dump();
        } else if (name == "always") {
            EXPECT_EQ(tool["execution"]["taskSupport"], "required") << tool.dump();
        } else {
            EXPECT_FALSE(tool.contains("execution")) << tool.dump();
        }
        ++seen;
    }
    EXPECT_EQ(seen, 3);
}

// C0.1: without a store, or without a tool that declares it, nothing about tasks is declared, `task` in a call is not
// read (not even a malformed one), and the requests of tasks do not exist.
TEST(ServerTasks2025Test, WithoutAStoreOrADeclaringToolEverythingIsAsBefore) {
    for (const bool with_store : {true, false}) {
        neomcp::Server server(with_store ? options_with(store_of()) : neomcp::ServerOptions{.name = "plain"});
        server.tool("work", "Work", [] { return std::string("done"); })
            .task_support(with_store ? TaskSupport::forbidden : TaskSupport::required);
        Peer peer(server);
        json opened = open(peer);
        EXPECT_FALSE(opened["capabilities"].contains("tasks")) << opened.dump();
        json listed = ask(peer, 1, "tools/list", json::object());
        EXPECT_FALSE(listed["result"]["tools"][0].contains("execution")) << listed.dump();
        for (const json& task : {json::object(), json(3)}) {
            json plain = call(peer, 2, "work", task);
            EXPECT_EQ(plain["result"]["content"][0]["text"], "done") << plain.dump();
        }
        for (const std::string method : {"tasks/get", "tasks/result", "tasks/cancel", "tasks/list"}) {
            json missing = ask(peer, 3, method, json{{"taskId", "0123456789abcdef0123456789abcdef"}});
            EXPECT_EQ(missing["error"]["code"], neomcp::error_code::method_not_found) << method << missing.dump();
        }
    }
}

// C2.1, C2.4: a call becomes a task exactly when it carries `task` and its tool declares optional or required; a call
// that asks for a task of a tool that never runs as one, or does not ask for one of a tool that runs only as one, is
// refused as a method that does not exist. The answer nests the task, working.
TEST(ServerTasks2025Test, TheRequestAsksForATaskOfATaskTool) {
    auto store = store_of();
    neomcp::Server server(options_with(store));
    server.tool("maybe", "Maybe a task", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    server.tool("always", "Only a task", [] { return std::string("done"); }).task_support(TaskSupport::required);
    server.tool("never", "Never a task", [] { return std::string("plain"); });
    Peer peer(server);
    open(peer);

    json created = call(peer, 1, "maybe");
    ASSERT_TRUE(created.contains("result")) << created.dump();
    json& task = created["result"]["task"];
    EXPECT_EQ(task["status"], "working");
    expect_task_fields(task);
    EXPECT_FALSE(created["result"].contains("content"));
    EXPECT_EQ(store->usage().tasks, 1u);

    json plain = call(peer, 2, "maybe", std::nullopt);
    EXPECT_EQ(plain["result"]["content"][0]["text"], "done") << plain.dump();
    EXPECT_EQ(store->usage().tasks, 1u) << "a call that did not ask made no task";

    EXPECT_TRUE(call(peer, 3, "always")["result"].contains("task"));
    json unasked = call(peer, 4, "always", std::nullopt);
    EXPECT_EQ(unasked["error"]["code"], neomcp::error_code::method_not_found) << unasked.dump();

    json forbidden = call(peer, 5, "never");
    EXPECT_EQ(forbidden["error"]["code"], neomcp::error_code::method_not_found) << forbidden.dump();
    json never = call(peer, 6, "never", std::nullopt);
    EXPECT_EQ(never["result"]["content"][0]["text"], "plain") << never.dump();
    EXPECT_EQ(store->usage().tasks, 2u);
}

// C2.1 (malformed), C2.3: a `task` that is not an object, or a lifetime that is not a non-negative integer, is invalid
// params; a call that fails its checks is answered as an ordinary call fails, and no task exists.
TEST(ServerTasks2025Test, AMalformedOrInvalidCallMakesNoTask) {
    auto store = store_of();
    neomcp::Server server(options_with(store));
    server.tool("maybe", "Maybe a task", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);
    int id = 0;
    for (const json& task : {json(3), json("x"), json::array(), json{{"ttl", -1}}, json{{"ttl", "long"}},
                             json{{"ttl", 1.5}}}) {
        json refused = call(peer, ++id, "maybe", task);
        EXPECT_EQ(refused["error"]["code"], neomcp::error_code::invalid_params) << task.dump() << refused.dump();
    }
    json unknown = call(peer, ++id, "absent");
    EXPECT_EQ(unknown["error"]["code"], neomcp::error_code::invalid_params) << unknown.dump();
    EXPECT_EQ(store->usage().tasks, 0u);
}

// C2.2: the lifetime asked for is kept within the store's bounds and the one granted is answered; none asked for, or
// null, is the store's default.
TEST(ServerTasks2025Test, TheLifetimeAskedForIsKeptWithinTheBounds) {
    neomcp::MemoryTaskStoreOptions bounds;
    bounds.default_ttl = 20s;
    bounds.min_ttl = 10ms;
    bounds.max_ttl = 60s;
    neomcp::Server server(options_with(store_of(bounds)));
    server.tool("maybe", "Maybe a task", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);
    const auto ttl_of = [&](int id, const json& task) { return call(peer, id, "maybe", task)["result"]["task"]["ttl"]; };
    EXPECT_EQ(ttl_of(1, json{{"ttl", 5000}}), 5000);
    EXPECT_EQ(ttl_of(2, json::object()), 20000);
    EXPECT_EQ(ttl_of(3, json{{"ttl", nullptr}}), 20000);
    EXPECT_EQ(ttl_of(4, json{{"ttl", 1000000000000ULL}}), 60000);
    EXPECT_EQ(ttl_of(5, json{{"ttl", 0}}), 10);
}

// C3.1: tasks/get answers the task's fields alone, without the mark of the task; an id that names no task of the
// requester is invalid params; a mark in the request is not read.
TEST(ServerTasks2025Test, AReadAnswersTheTaskAlone) {
    neomcp::Server server(options_with(store_of()));
    server.tool("ok", "Answer", [] { return std::string("the answer"); }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);
    const std::string id = task_id_of(call(peer, 1, "ok"));
    json done = poll_until(peer, id);
    ASSERT_TRUE(done.contains("result")) << done.dump();
    EXPECT_EQ(done["result"]["status"], "completed");
    EXPECT_EQ(done["result"]["taskId"], id);
    expect_task_fields(done["result"]);
    EXPECT_FALSE(done["result"].contains("content")) << "the result is fetched with tasks/result";
    EXPECT_FALSE(done["result"].contains("_meta") && done["result"]["_meta"].contains(related_task));

    json unknown = ask(peer, 2, "tasks/get", json{{"taskId", "ffffffffffffffffffffffffffffffff"}});
    EXPECT_EQ(unknown["error"]["code"], neomcp::error_code::invalid_params) << unknown.dump();
    json marked = ask(peer, 3, "tasks/get",
                            json{{"taskId", "ffffffffffffffffffffffffffffffff"},
                                 {"_meta", {{related_task, {{"taskId", id}}}}}});
    EXPECT_EQ(marked["error"]["code"], neomcp::error_code::invalid_params) << "the params name the task";
    json malformed = ask(peer, 4, "tasks/get", json{{"taskId", 7}});
    EXPECT_EQ(malformed["error"]["code"], neomcp::error_code::invalid_params) << malformed.dump();
}

// C3.2: tasks/result of a task that has not ended answers nothing until it ends, then exactly what the call would
// have answered, marked with the task; one of a task that has ended answers at once.
TEST(ServerTasks2025Test, AResultRequestWaitsUntilTheTaskEnds) {
    Event release;
    neomcp::Server server(options_with(store_of()));
    server.tool("held", "Held until released", [&] {
        release.wait();
        return std::string("released");
    }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);
    const std::string id = task_id_of(call(peer, 1, "held"));

    peer.notify(message(2, "tasks/result", json{{"taskId", id}}));
    EXPECT_FALSE(peer.await_response(2, quiet)) << "answered before the task ended";
    // The connection serves other requests meanwhile.
    EXPECT_EQ(ask(peer, 3, "tasks/get", json{{"taskId", id}})["result"]["status"], "working");
    release.set();
    auto answer = peer.await_response(2, patience);
    ASSERT_TRUE(answer);
    ASSERT_TRUE(answer->contains("result")) << answer->dump();
    EXPECT_EQ((*answer)["result"]["content"][0]["text"], "released");
    EXPECT_EQ((*answer)["result"]["_meta"][related_task]["taskId"], id) << answer->dump();
    EXPECT_FALSE((*answer)["result"].contains("taskId")) << "the result, not the task";

    json again = ask(peer, 4, "tasks/result", json{{"taskId", id}});
    EXPECT_EQ(again["result"]["content"][0]["text"], "released") << again.dump();
    json unknown = ask(peer, 5, "tasks/result", json{{"taskId", "ffffffffffffffffffffffffffffffff"}});
    EXPECT_EQ(unknown["error"]["code"], neomcp::error_code::invalid_params) << unknown.dump();
}

// C3.3: a tool result marked as an error fails the task, and tasks/result answers that result; an error of the call
// fails it too, and tasks/result answers that error.
TEST(ServerTasks2025Test, AToolErrorFailsTheTaskAndTheResultIsWhatTheCallAnswered) {
    neomcp::Server server(options_with(store_of()));
    server.tool("tool_error", "Fail as a tool", []() -> std::string {
        throw neomcp::ToolError("the tool could not");
    }).task_support(TaskSupport::optional);
    neomcp::ToolDefinition failing;
    failing.name = "failure";
    failing.output_schema = json::parse(R"({"type":"object","properties":{"value":{"type":"string"}}})");
    server.tool(failing, [](const json&) { return neomcp::ToolResult{}; }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);

    const std::string tool_error = task_id_of(call(peer, 1, "tool_error"));
    EXPECT_EQ(poll_until(peer, tool_error)["result"]["status"], "failed");
    json result = ask(peer, 2, "tasks/result", json{{"taskId", tool_error}});
    ASSERT_TRUE(result.contains("result")) << result.dump();
    EXPECT_EQ(result["result"]["isError"], true);
    EXPECT_EQ(result["result"]["_meta"][related_task]["taskId"], tool_error);

    const std::string failure = task_id_of(call(peer, 3, "failure"));
    json failed = poll_until(peer, failure);
    EXPECT_EQ(failed["result"]["status"], "failed");
    EXPECT_TRUE(failed["result"]["statusMessage"].is_string()) << failed.dump();
    json error = ask(peer, 4, "tasks/result", json{{"taskId", failure}});
    ASSERT_TRUE(error.contains("error")) << error.dump();
    EXPECT_EQ(error["error"]["code"], neomcp::error_code::internal_error);
}

// C3.4: a cancellation of a running task answers with the task, cancelled, before its handler returns; the handler is
// asked to stop, and what it returns later changes nothing. A waiting tasks/result of the same connection is answered;
// a cancellation of a task that has ended is invalid params.
TEST(ServerTasks2025Test, ACancelledTaskStaysCancelled) {
    Event started;
    Event saw_stop;
    Event returned;
    neomcp::Server server(options_with(store_of()));
    server.tool("long", "Run until cancelled", [&](neomcp::Context& ctx) {
        started.set();
        Event never;
        if (!never.wait(ctx.stop_token())) saw_stop.set();
        returned.set();
        return std::string("finished anyway");
    }).task_support(TaskSupport::optional);
    server.tool("quick", "Answer", [] { return std::string("kept"); }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);

    const std::string running = task_id_of(call(peer, 1, "long"));
    ASSERT_TRUE(started.wait());
    peer.notify(message(2, "tasks/result", json{{"taskId", running}}));
    EXPECT_FALSE(peer.await_response(2, quiet));
    json cancelled = ask(peer, 3, "tasks/cancel", json{{"taskId", running}});
    ASSERT_TRUE(cancelled.contains("result")) << cancelled.dump();
    EXPECT_EQ(cancelled["result"]["status"], "cancelled");
    EXPECT_EQ(cancelled["result"]["taskId"], running);
    expect_task_fields(cancelled["result"]);
    auto waited = peer.await_response(2, patience);
    ASSERT_TRUE(waited) << "the waiting result request is answered";
    EXPECT_TRUE(waited->contains("error")) << waited->dump();
    EXPECT_TRUE(saw_stop.wait());
    ASSERT_TRUE(returned.wait());
    std::this_thread::sleep_for(4 * poll_step);
    EXPECT_EQ(ask(peer, 4, "tasks/get", json{{"taskId", running}})["result"]["status"], "cancelled");

    json again = ask(peer, 5, "tasks/cancel", json{{"taskId", running}});
    EXPECT_EQ(again["error"]["code"], neomcp::error_code::invalid_params) << again.dump();
    const std::string ended = task_id_of(call(peer, 6, "quick"));
    ASSERT_EQ(poll_until(peer, ended)["result"]["status"], "completed");
    json late = ask(peer, 7, "tasks/cancel", json{{"taskId", ended}});
    EXPECT_EQ(late["error"]["code"], neomcp::error_code::invalid_params) << late.dump();
    EXPECT_EQ(poll_until(peer, ended)["result"]["status"], "completed");
    json unknown = ask(peer, 8, "tasks/cancel", json{{"taskId", "ffffffffffffffffffffffffffffffff"}});
    EXPECT_EQ(unknown["error"]["code"], neomcp::error_code::invalid_params) << unknown.dump();
}

// C3.2: the wait of tasks/result ends with the request (its cancellation), not the task, which a later tasks/result
// still reaches; beyond TaskOptions::max_waiting_results a result request is answered busy.
TEST(ServerTasks2025Test, AResultWaitEndsWithItsRequestAndIsBounded) {
    Event release;
    auto options = options_with(store_of());
    options.tasks.max_waiting_results = 1;
    neomcp::Server server(std::move(options));
    server.tool("held", "Held until released", [&] {
        release.wait();
        return std::string("released");
    }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);
    const std::string id = task_id_of(call(peer, 1, "held"));

    peer.notify(message(2, "tasks/result", json{{"taskId", id}}));
    EXPECT_FALSE(peer.await_response(2, quiet));
    json busy = ask(peer, 3, "tasks/result", json{{"taskId", id}});
    EXPECT_EQ(busy["error"]["code"], neomcp::error_code::internal_error) << busy.dump();
    EXPECT_EQ(busy["error"]["data"]["reason"], "busy");
    peer.notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 2}}}});
    // The place is given back: another wait is taken.
    peer.notify(message(4, "tasks/result", json{{"taskId", id}}));
    EXPECT_FALSE(peer.await_response(4, quiet));
    EXPECT_EQ(ask(peer, 5, "tasks/get", json{{"taskId", id}})["result"]["status"], "working");
    release.set();
    auto answer = peer.await_response(4, patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ((*answer)["result"]["content"][0]["text"], "released") << answer->dump();
    EXPECT_FALSE(peer.await_response(2, quiet)) << "a cancelled request is not answered";
}

// C3.9: the end of the connection that created a task does not end it; another connection reaches it by its id.
TEST(ServerTasks2025Test, ATaskOutlivesItsConnection) {
    Event release;
    neomcp::Server server(options_with(store_of()));
    server.tool("held", "Held until released", [&] {
        release.wait();
        return std::string("released");
    }).task_support(TaskSupport::optional);
    std::string id;
    {
        Peer first(server);
        open(first);
        id = task_id_of(call(first, 1, "held"));
        first.close();
    }
    release.set();
    Peer second(server);
    open(second);
    EXPECT_EQ(poll_until(second, id)["result"]["status"], "completed");
    EXPECT_EQ(ask(second, 2, "tasks/result", json{{"taskId", id}})["result"]["content"][0]["text"], "released");
}
