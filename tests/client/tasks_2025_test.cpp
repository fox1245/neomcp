// Tests of client-side 2025-11-25 core tasks (augmented_request): capability checks, tools/list execution.taskSupport
// lookup, params.task augmentation, tasks/get polling, tasks/result fetching (including input_required without a short
// request wait clock), tasks/cancel semantics, progress preservation, and malformed answers.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/client.h"
#include "neomcp/client/memory_transport.h"
#include "support.h"

namespace {

using namespace client_test;
using neomcp::ClientErrorKind;
using neomcp::json;
using neomcp::profile::Transport;
using ms = std::chrono::milliseconds;

constexpr std::string_view revision = "2025-11-25";
constexpr std::string_view task_id = "task-2025-1";
constexpr int invalid_params = -32602;
constexpr int server_app_error = -32050;

json init_result_with_tasks(bool with_tasks = true, bool with_tools_call = true) {
    json res = initialize_result(revision);
    if (!with_tasks) return res;
    json tasks = json{{"cancel", json::object()}, {"list", json::object()}};
    if (with_tools_call) tasks["requests"] = json{{"tools", {{"call", json::object()}}}};
    res["capabilities"]["tasks"] = std::move(tasks);
    return res;
}

json tool_entry(std::string_view name, std::optional<std::string_view> support, json output_schema = nullptr) {
    json entry = json{{"name", std::string(name)}, {"inputSchema", {{"type", "object"}}}};
    if (support) entry["execution"] = json{{"taskSupport", std::string(*support)}};
    if (!output_schema.is_null()) entry["outputSchema"] = std::move(output_schema);
    return entry;
}

json wire_task(std::string_view status, json more = json::object()) {
    more["taskId"] = task_id;
    more["status"] = std::string(status);
    more["createdAt"] = "2026-11-22T00:00:00Z";
    more["lastUpdatedAt"] = "2026-11-22T00:00:00Z";
    more["ttl"] = 60000;
    return more;
}

json wire_created(json more = json::object()) { return json{{"task", wire_task("working", std::move(more))}}; }

json wire_tool_result(std::string_view text, bool is_error = false, json structured = nullptr) {
    json res = json{{"content", json::array({json{{"type", "text"}, {"text", std::string(text)}}})}};
    if (is_error) res["isError"] = true;
    if (!structured.is_null()) res["structuredContent"] = std::move(structured);
    return res;
}

class Scripted2025Server {
public:
    using Handler = std::function<void(ScriptedPeer&, const Seen&)>;

    Scripted2025Server() {
        peer = std::make_shared<ScriptedPeer>([this](ScriptedPeer& side, const Seen& req) { handle(side, req); });
    }

    json capabilities = init_result_with_tasks();
    std::vector<json> tools = {tool_entry("opt", "optional"), tool_entry("req", "required"),
                               tool_entry("forb", "forbidden"), tool_entry("plain", std::nullopt)};
    Handler on_call;
    Handler on_get;
    Handler on_result;
    Handler on_cancel;

    std::atomic<int> list_calls{0};
    std::atomic<int> call_calls{0};
    std::atomic<int> get_calls{0};
    std::atomic<int> result_calls{0};
    std::atomic<int> cancel_calls{0};
    std::shared_ptr<ScriptedPeer> peer;

    std::optional<Seen> last_tool_call() const {
        const std::lock_guard lock(mutex_);
        return last_tool_call_;
    }

private:
    void handle(ScriptedPeer& side, const Seen& req) {
        const std::string method = req.method();
        if (method == "initialize") {
            side.answer(req, capabilities);
        } else if (method == "tools/list") {
            ++list_calls;
            side.answer(req, json{{"tools", tools}});
        } else if (method == "tools/call") {
            ++call_calls;
            {
                const std::lock_guard lock(mutex_);
                last_tool_call_ = req;
            }
            if (on_call) {
                on_call(side, req);
            } else {
                side.answer(req, wire_created());
            }
        } else if (method == "tasks/get") {
            ++get_calls;
            if (on_get) {
                on_get(side, req);
            } else {
                side.answer(req, wire_task("completed"));
            }
        } else if (method == "tasks/result") {
            ++result_calls;
            if (on_result) {
                on_result(side, req);
            } else {
                side.answer(req, wire_tool_result("done"));
            }
        } else if (method == "tasks/cancel") {
            ++cancel_calls;
            if (on_cancel) {
                on_cancel(side, req);
            } else {
                side.answer(req, wire_task("cancelled"));
            }
        }
    }

    mutable std::mutex mutex_;
    std::optional<Seen> last_tool_call_;
};

neomcp::ClientOptions options_2025(std::shared_ptr<ManualTimers> timers, bool accept = true) {
    neomcp::ClientOptions opts = only({revision});
    opts.timers = std::move(timers);
    opts.tasks.accept = accept;
    return opts;
}

neomcp::Client open_2025(neomcp::ClientOptions opts, const std::shared_ptr<ScriptedPeer>& peer) {
    auto opened = neomcp::Client::open(std::move(opts), neomcp::make_memory_transport(Transport::stdio, peer));
    if (!opened) throw std::runtime_error("open failed: " + opened.error().message);
    return std::move(opened).value();
}

bool is_kind(const ManualTimers::Started& one, neomcp::DeadlineKind kind) {
    const auto* deadline = std::get_if<neomcp::Deadline>(&one.purpose);
    return deadline != nullptr && deadline->kind == kind;
}

std::vector<ManualTimers::Started> started(const ManualTimers& timers, neomcp::DeadlineKind kind) {
    std::vector<ManualTimers::Started> found;
    for (const auto& one : timers.started()) {
        if (is_kind(one, kind)) found.push_back(one);
    }
    return found;
}

std::optional<ManualTimers::Started> next_poll(const ManualTimers& timers) {
    return timers.live([](const ManualTimers::Started& one) { return is_kind(one, neomcp::DeadlineKind::poll); });
}

class ToolCaller {
public:
    ToolCaller(neomcp::Client& client, std::string name, neomcp::RequestOptions options = {})
        : thread_([&client, name = std::move(name), options = std::move(options), this]() mutable {
              auto res = client.call_tool(name, json::object(), std::move(options));
              const std::lock_guard lock(mutex_);
              result_.emplace(std::move(res));
          }) {}
    ToolCaller(const ToolCaller&) = delete;
    ToolCaller& operator=(const ToolCaller&) = delete;
    ~ToolCaller() { join(thread_); }

    neomcp::Result<neomcp::ToolCall> result() {
        join(thread_);
        const std::lock_guard lock(mutex_);
        return *result_;
    }
    bool done() {
        const std::lock_guard lock(mutex_);
        return result_.has_value();
    }

private:
    std::mutex mutex_;
    std::optional<neomcp::Result<neomcp::ToolCall>> result_;
    std::thread thread_;
};

void drive_polls(ManualTimers& timers, ToolCaller& caller, neomcp::Client& client) {
    while (true) {
        std::optional<ManualTimers::Started> timer;
        const bool ready = eventually([&] {
            if (caller.done()) return true;
            for (const auto& one : timers.started()) {
                if (is_kind(one, neomcp::DeadlineKind::poll) && !one.cancelled && !one.fired) {
                    timer = one;
                    return true;
                }
            }
            return false;
        });
        if (!ready || !timer) {
            if (!caller.done()) {
                ADD_FAILURE() << "timed out waiting for poll timer";
                static_cast<void>(client.close());
            }
            return;
        }
        timers.fire(timer->id);
    }
}

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

TEST(ClientTasks2025, OnlyAugmentsToolsWhoseSupportIsOptionalOrRequired) {
    Scripted2025Server server;
    server.on_call = [](ScriptedPeer& side, const Seen& req) {
        const std::string name = req.message["params"].value("name", "");
        if ((name == "forb" || name == "plain" || name == "unknown_tool") &&
            req.message["params"].contains("task")) {
            ADD_FAILURE() << "unexpected params.task for " << name;
            side.answer(req, wire_tool_result("bug"));
            return;
        }
        if (req.message["params"].contains("task")) {
            side.answer(req, wire_created());
        } else {
            side.answer(req, wire_tool_result("sync"));
        }
    };
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_2025(options_2025(timers), server.peer);

    // First call triggers tools/list warmup, sees `forb` -> forbidden, does NOT augment.
    const auto forb = client.call_tool("forb");
    ASSERT_TRUE(forb) << forb.error().message;
    EXPECT_EQ(text_of(*forb), "sync");
    EXPECT_EQ(server.list_calls.load(), 1);
    ASSERT_TRUE(server.last_tool_call());
    EXPECT_FALSE(server.last_tool_call()->message["params"].contains("task"));

    // Plain tool without execution.taskSupport -> default forbidden, does NOT augment; uses cached listing.
    const auto plain = client.call_tool("plain");
    ASSERT_TRUE(plain) << plain.error().message;
    EXPECT_EQ(text_of(*plain), "sync");
    EXPECT_EQ(server.list_calls.load(), 1);
    ASSERT_TRUE(server.last_tool_call());
    EXPECT_FALSE(server.last_tool_call()->message["params"].contains("task"));

    // Optional & required tools ask for a task and fetch tasks/result after tasks/get reports completed, reusing cache.
    for (const char* tool_name : {"opt", "req"}) {
        ToolCaller caller(client, tool_name);
        drive_polls(*timers, caller, client);
        const auto res = caller.result();
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_EQ(text_of(*res), "done");
        EXPECT_EQ(server.list_calls.load(), 1);
        ASSERT_TRUE(server.last_tool_call());
        EXPECT_TRUE(server.last_tool_call()->message["params"].contains("task"));
    }

    // An unlisted tool triggers a listing refresh, stays unaugmented (default forbidden), and runs normally.
    const auto unlisted = client.call_tool("unknown_tool");
    ASSERT_TRUE(unlisted) << unlisted.error().message;
    EXPECT_EQ(text_of(*unlisted), "sync");
    EXPECT_EQ(server.list_calls.load(), 2);
    ASSERT_TRUE(server.last_tool_call());
    EXPECT_FALSE(server.last_tool_call()->message["params"].contains("task"));
    EXPECT_EQ(server.cancel_calls.load(), 0);
}

TEST(ClientTasks2025, DoesNotAugmentOrListWithoutCapabilitiesOrOptIn) {
    {
        Scripted2025Server server;
        server.capabilities = init_result_with_tasks(false, false);
        server.on_call = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_tool_result("plain")); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers, true), server.peer);
        const auto missing = client.task(std::string(task_id));
        ASSERT_FALSE(missing);
        EXPECT_EQ(missing.error().kind, ClientErrorKind::unsupported_method);
        EXPECT_EQ(server.get_calls.load(), 0);
        const auto res = client.call_tool("opt");
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_EQ(server.list_calls.load(), 0);
        ASSERT_TRUE(server.last_tool_call());
        EXPECT_FALSE(server.last_tool_call()->message["params"].contains("task"));
    }
    {
        Scripted2025Server server;
        server.capabilities = init_result_with_tasks(true, false);  // tasks present, requests.tools.call absent
        server.on_call = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_tool_result("plain")); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers, true), server.peer);
        const auto existing = client.task(std::string(task_id));
        ASSERT_TRUE(existing) << existing.error().message;
        EXPECT_EQ(server.get_calls.load(), 1);
        const auto res = client.call_tool("opt");
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_EQ(server.list_calls.load(), 0);
        ASSERT_TRUE(server.last_tool_call());
        EXPECT_FALSE(server.last_tool_call()->message["params"].contains("task"));
    }
    {
        Scripted2025Server server;
        server.on_call = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_tool_result("no-opt")); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers, false), server.peer);
        const auto disabled = client.task(std::string(task_id));
        ASSERT_FALSE(disabled);
        EXPECT_EQ(disabled.error().kind, ClientErrorKind::unsupported_method);
        EXPECT_EQ(server.get_calls.load(), 0);
        const auto res = client.call_tool("opt");
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_EQ(server.list_calls.load(), 0);
        ASSERT_TRUE(server.last_tool_call());
        EXPECT_FALSE(server.last_tool_call()->message["params"].contains("task"));
    }
}

TEST(ClientTasks2025, UnaugmentedToolReturningTaskIsNotAdoptedAsTask) {
    // Verify both pure CreateTaskResult and a mixed {"task":..., "content":[]} reply are rejected as malformed_answer
    // when params.task was not sent.
    for (const json& reply : {wire_created(), json{{"task", wire_task("working")}, {"content", json::array()}}}) {
        Scripted2025Server server;
        server.on_call = [reply](ScriptedPeer& side, const Seen& req) { side.answer(req, reply); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers, true), server.peer);
        ToolCaller caller(client, "forb");
        const OnExit close_first([&] {
            if (!caller.done()) static_cast<void>(client.close());
        });
        ASSERT_TRUE(eventually([&] { return caller.done(); }));
        const auto res = caller.result();
        ASSERT_FALSE(res);
        EXPECT_EQ(res.error().kind, ClientErrorKind::malformed_answer);
        EXPECT_EQ(server.call_calls.load(), 1);
        ASSERT_TRUE(server.last_tool_call());
        EXPECT_FALSE(server.last_tool_call()->message["params"].contains("task"));
        EXPECT_EQ(server.get_calls.load(), 0);
    }
}

TEST(ClientTasks2025, FailedTaskFetchesResultForToolErrorOrRpcErrorWithoutCancelling) {
    // Case A: task `status = "failed"` because tool returned `isError: true` -> tasks/result returns CallToolResult.
    {
        Scripted2025Server server;
        server.on_get = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_task("failed")); };
        server.on_result = [](ScriptedPeer& side, const Seen& req) {
            side.answer(req, wire_tool_result("tool failed softly", true));
        };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers), server.peer);
        ToolCaller caller(client, "opt");
        drive_polls(*timers, caller, client);
        const auto res = caller.result();
        ASSERT_TRUE(res) << res.error().message;
        EXPECT_TRUE(res->is_error);
        EXPECT_EQ(text_of(*res), "tool failed softly");
        EXPECT_EQ(server.cancel_calls.load(), 0);
    }
    // Case B: tasks/result returns JSON-RPC error (-32050) -> ErrorAnswered without spurious tasks/cancel.
    {
        Scripted2025Server server;
        server.on_get = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_task("failed")); };
        server.on_result = [](ScriptedPeer& side, const Seen& req) { side.refuse(req, server_app_error, "Boom"); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers), server.peer);
        ToolCaller caller(client, "opt");
        drive_polls(*timers, caller, client);
        const auto res = caller.result();
        ASSERT_FALSE(res);
        EXPECT_EQ(res.error().kind, ClientErrorKind::server_error);
        EXPECT_EQ(res.error().code, server_app_error);
        EXPECT_EQ(res.error().message, "Boom");
        EXPECT_EQ(server.cancel_calls.load(), 0);
    }
    // Case C: failed task whose tasks/result returns -32602 (invalid_params) is server_error, NOT task_lost.
    {
        Scripted2025Server server;
        server.on_get = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_task("failed")); };
        server.on_result = [](ScriptedPeer& side, const Seen& req) { side.refuse(req, invalid_params, "Bad arg"); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers), server.peer);
        ToolCaller caller(client, "opt");
        drive_polls(*timers, caller, client);
        const auto res = caller.result();
        ASSERT_FALSE(res);
        EXPECT_EQ(res.error().kind, ClientErrorKind::server_error);
        EXPECT_EQ(res.error().code, invalid_params);
        EXPECT_EQ(res.error().message, "Bad arg");
        EXPECT_EQ(server.cancel_calls.load(), 0);
    }
    // Case D: input_required task whose tasks/result fails (-32603) is NOT settled, so owned task IS cancelled.
    {
        Scripted2025Server server;
        server.on_get = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_task("input_required")); };
        server.on_result = [](ScriptedPeer& side, const Seen& req) { side.refuse(req, -32603, "Internal fail"); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers), server.peer);
        ToolCaller caller(client, "opt");
        drive_polls(*timers, caller, client);
        const auto res = caller.result();
        ASSERT_FALSE(res);
        EXPECT_EQ(res.error().kind, ClientErrorKind::server_error);
        EXPECT_EQ(res.error().code, -32603);
        ASSERT_TRUE(eventually([&] { return server.cancel_calls.load() == 1; }));
    }
}

TEST(ClientTasks2025, InputRequiredUsesTasksResultWithoutShortWaitTimer) {
    Scripted2025Server server;
    std::mutex res_mutex;
    std::optional<Seen> pending_result;
    std::atomic<bool> elicited_sent{false};

    server.on_get = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_task("input_required")); };
    server.on_result = [&](ScriptedPeer& side, const Seen& req) {
        {
            const std::lock_guard lock(res_mutex);
            pending_result = req;
        }
        if (!elicited_sent.exchange(true)) {
            side.send(req.unit, neomcp::serialize(json{
                                    {"jsonrpc", "2.0"},
                                    {"id", 901},
                                    {"method", "elicitation/create"},
                                    {"params",
                                     {{"mode", "form"},
                                      {"message", "Confirm?"},
                                      {"requestedSchema",
                                       json::parse(R"({"type":"object","properties":{"ok":{"type":"boolean"}}})")},
                                      {"_meta", {{"io.modelcontextprotocol/related-task", {{"taskId", task_id}}}}}}}}));
        }
    };

    auto timers = std::make_shared<ManualTimers>();
    auto opts = options_2025(timers);
    opts.handlers.form([](const neomcp::ElicitationForm&) {
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
    });
    neomcp::Client client = open_2025(std::move(opts), server.peer);

    // Warm up tools/list first so we isolate timers during tasks/get -> tasks/result.
    server.on_call = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_tool_result("sync")); };
    ASSERT_TRUE(client.call_tool("forb"));

    server.on_call = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_created()); };
    const std::size_t waits_before = started(*timers, neomcp::DeadlineKind::wait).size();
    ToolCaller caller(client, "opt");

    const auto poll = next_poll(*timers);
    ASSERT_TRUE(poll);
    // Expected waits up to this point: `tools/call` (+1), `tasks/get` (+1). Wait for tasks/get to trigger tasks/result.
    timers->fire(poll->id);

    // Wait until the client has answered the server's elicitation request on id 901.
    ASSERT_TRUE(eventually([&] {
        for (const Seen& one : server.peer->written()) {
            if (one.message.contains("result") && one.message.contains("id") && one.message["id"] == 901 &&
                one.message["result"].value("action", "") == "accept") {
                return true;
            }
        }
        return false;
    }));

    // Crucially, issuing `tasks/result` under `input_required` did NOT arm another short `DeadlineKind::wait` timer!
    EXPECT_EQ(started(*timers, neomcp::DeadlineKind::wait).size(), waits_before + 2U);

    {
        const std::lock_guard lock(res_mutex);
        ASSERT_TRUE(pending_result);
        server.peer->answer(*pending_result, wire_tool_result("elicited-done"));
    }
    const auto res = caller.result();
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(text_of(*res), "elicited-done");
    EXPECT_EQ(server.get_calls.load(), 1);
    EXPECT_EQ(server.result_calls.load(), 1);
    EXPECT_EQ(server.cancel_calls.load(), 0);
}

TEST(ClientTasks2025, DeliversProgressAndHandlesCancelUnknownAndStopSemantics) {
    // 1. Progress notifications arriving while polling/following keep firing on_progress.
    {
        Scripted2025Server server;
        json seen_token;
        std::mutex tok_m;
        server.on_call = [&](ScriptedPeer& side, const Seen& req) {
            if (req.message["params"].contains("_meta")) {
                const std::lock_guard lk(tok_m);
                seen_token = req.message["params"]["_meta"]["progressToken"];
            }
            side.answer(req, wire_created());
        };
        std::atomic<int> poll_round{0};
        server.on_get = [&](ScriptedPeer& side, const Seen& req) {
            if (++poll_round == 1) {
                json token;
                {
                    const std::lock_guard lk(tok_m);
                    token = seen_token;
                }
                side.send(req.unit, neomcp::serialize(json{
                                        {"jsonrpc", "2.0"},
                                        {"method", "notifications/progress"},
                                        {"params", {{"progressToken", token}, {"progress", 0.5}, {"total", 1.0}}}}));
                side.answer(req, wire_task("working"));
            } else {
                side.answer(req, wire_task("completed"));
            }
        };
        auto timers = std::make_shared<ManualTimers>();
        std::mutex pm;
        std::vector<double> seen_progress;
        neomcp::Client client = open_2025(options_2025(timers), server.peer);
        neomcp::RequestOptions call_opts;
        call_opts.on_progress = [&](const neomcp::Progress& p) {
            const std::lock_guard lk(pm);
            seen_progress.push_back(p.progress);
        };
        ToolCaller caller(client, "opt", std::move(call_opts));
        drive_polls(*timers, caller, client);
        const auto res = caller.result();
        ASSERT_TRUE(res) << res.error().message;
        {
            const std::lock_guard lk(pm);
            EXPECT_EQ(seen_progress, (std::vector<double>{0.5}));
        }
    }

    // 2. tasks/get returning -32602 yields task_lost; tasks/cancel returning -32602 yields server_error (not task_lost).
    {
        Scripted2025Server server;
        server.on_get = [](ScriptedPeer& side, const Seen& req) { side.refuse(req, invalid_params, "Unknown task"); };
        server.on_cancel = [](ScriptedPeer& side, const Seen& req) {
            side.refuse(req, invalid_params, "Task has already ended");
        };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers), server.peer);

        const auto lost = client.task(std::string(task_id));
        ASSERT_FALSE(lost);
        EXPECT_EQ(lost.error().kind, ClientErrorKind::task_lost);
        EXPECT_GE(server.get_calls.load(), 1);

        auto started_call = client.start_tool("opt");
        ASSERT_TRUE(started_call) << started_call.error().message;
        auto* handle = std::get_if<neomcp::TaskHandle>(&*started_call);
        ASSERT_NE(handle, nullptr);
        const auto already_ended = handle->cancel();
        ASSERT_FALSE(already_ended);
        EXPECT_EQ(already_ended.error().kind, ClientErrorKind::server_error);
        EXPECT_EQ(already_ended.error().code, invalid_params);
        EXPECT_EQ(server.cancel_calls.load(), 1);
    }

    // 3. stop on call_tool cancels the task via tasks/cancel and ends cancelled.
    {
        Scripted2025Server server;
        server.on_get = [](ScriptedPeer& side, const Seen& req) { side.answer(req, wire_task("working")); };
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_2025(options_2025(timers), server.peer);
        std::stop_source stop;
        neomcp::RequestOptions opts;
        opts.stop = stop.get_token();
        ToolCaller caller(client, "opt", std::move(opts));
        ASSERT_TRUE(next_poll(*timers));
        stop.request_stop();
        const auto res = caller.result();
        ASSERT_FALSE(res);
        EXPECT_EQ(res.error().kind, ClientErrorKind::cancelled);
        ASSERT_TRUE(eventually([&] { return server.cancel_calls.load() == 1; }));
    }
}

}  // namespace
