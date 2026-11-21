// The client's half of tasks (revision 2026-07-28 with the extension io.modelcontextprotocol/tasks) against a scripted
// server: what the client declares, how it reads a result that creates a task, the waits between its reads and every
// bound the server's answers could push, the requests a task waits on, cancellation, and the answers it refuses. The
// clocks are the test's (ManualTimers), so that no test waits for real time. The round trips with the library's own
// server are in tasks_server_test.cpp.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
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
using ms = std::chrono::milliseconds;

constexpr std::string_view revision = "2026-07-28";
constexpr const char* extension = "io.modelcontextprotocol/tasks";
constexpr const char* task_id = "task-1";
constexpr int invalid_params = -32602;

// What the scripted server answers to a request about a task: a result, or an error with this code.
struct Reply {
    json result = json::object();
    std::optional<int> error;
};

Reply ok(json result) {
    Reply reply;
    reply.result = std::move(result);
    return reply;
}

Reply refused(int code) {
    Reply reply;
    reply.error = code;
    return reply;
}

json state(std::string_view status, json more = json::object()) {
    more["taskId"] = task_id;
    more["status"] = std::string(status);
    more["createdAt"] = "2026-11-21T00:00:00Z";
    more["lastUpdatedAt"] = "2026-11-21T00:00:00Z";
    more["ttlMs"] = 60000;
    more["resultType"] = "complete";
    return more;
}

json created(json more = json::object()) {
    json body = state("working", std::move(more));
    body["resultType"] = "task";
    return body;
}

json tool_result(std::string_view text) {
    return json{{"content", json::array({json{{"type", "text"}, {"text", std::string(text)}}})}};
}

json completed(std::string_view text = "done") { return state("completed", json{{"result", tool_result(text)}}); }

json form_request(std::string_view message) {
    return json{{"method", "elicitation/create"},
                {"params",
                 json{{"mode", "form"},
                      {"message", std::string(message)},
                      {"requestedSchema",
                       json::parse(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})")}}}};
}

json waiting_on(std::initializer_list<std::string_view> keys) {
    json requests = json::object();
    for (const auto key : keys) requests[std::string(key)] = form_request(key);
    return state("input_required", json{{"inputRequests", requests}});
}

// A server of revision 2026-07-28 whose tools/call creates `first` (a task, unless the test says otherwise) and whose
// answers to tasks/get come from `read` (by the number of the read, from 1); updates and cancellations are
// acknowledged.
class TaskServer {
public:
    using Reader = std::function<Reply(int read, const Seen& request)>;

    explicit TaskServer(Reader read, json first = created()) : read_(std::move(read)), first_(std::move(first)) {
        peer = std::make_shared<ScriptedPeer>([this](ScriptedPeer& side, const Seen& request) { answer(side, request); });
    }

    int reads() const { return reads_.load(); }

    std::shared_ptr<ScriptedPeer> peer;

private:
    void answer(ScriptedPeer& side, const Seen& request) {
        const std::string method = request.method();
        if (method == "server/discover") {
            side.answer(request, discover_result(revision));
        } else if (method == "tools/call") {
            side.answer(request, first_);
        } else if (method == "tasks/get") {
            const Reply reply = read_(++reads_, request);
            if (reply.error) {
                side.refuse(request, *reply.error, "Unknown task");
            } else {
                side.answer(request, reply.result);
            }
        } else if (method == "tasks/update" || method == "tasks/cancel") {
            side.answer(request, json{{"resultType", "complete"}});
        } else if (method == "prompts/get") {
            side.answer(request, created());
        }
    }

    Reader read_;
    json first_;
    std::atomic<int> reads_{0};
};

neomcp::ClientOptions taking(std::shared_ptr<ManualTimers> timers, bool accept = true) {
    neomcp::ClientOptions options = only({revision});
    options.tasks.accept = accept;
    options.timers = std::move(timers);
    return options;
}

neomcp::Client open_with(neomcp::ClientOptions options, const std::shared_ptr<ScriptedPeer>& peer) {
    auto opened = neomcp::Client::open(std::move(options),
                                       neomcp::make_memory_transport(neomcp::profile::Transport::stdio, peer));
    if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
    return std::move(opened).value();
}

bool is_kind(const ManualTimers::Started& one, neomcp::DeadlineKind kind) {
    const auto* deadline = std::get_if<neomcp::Deadline>(&one.purpose);
    return deadline != nullptr && deadline->kind == kind;
}

// The next poll timer once it is set; empty after the bound.
std::optional<ManualTimers::Started> next_wait(const ManualTimers& timers) {
    return timers.live([](const ManualTimers::Started& one) { return is_kind(one, neomcp::DeadlineKind::poll); });
}

std::vector<ManualTimers::Started> started(const ManualTimers& timers, neomcp::DeadlineKind kind) {
    std::vector<ManualTimers::Started> found;
    for (const auto& one : timers.started()) {
        if (is_kind(one, kind)) found.push_back(one);
    }
    return found;
}

// A tool call made on a thread of its own, so that the test can fire the client's timers meanwhile.
class Caller {
public:
    Caller(neomcp::Client& client, std::stop_token stop = {})
        : thread_([&client, this, stop] {
              neomcp::RequestOptions options;
              options.stop = stop;
              auto called = client.call_tool("work", json::object(), options);
              const std::lock_guard lock(mutex_);
              result_.emplace(std::move(called));
          }) {}
    Caller(const Caller&) = delete;
    Caller& operator=(const Caller&) = delete;
    ~Caller() { join(thread_); }

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

// Fires each wait before a read as it is set, until the call ended.
void drive(ManualTimers& timers, Caller& caller) {
    while (true) {
        std::optional<ManualTimers::Started> wait;
        const bool moved = eventually([&] {
            if (caller.done()) return true;
            for (const auto& one : timers.started()) {
                if (is_kind(one, neomcp::DeadlineKind::poll) && !one.cancelled && !one.fired) {
                    wait = one;
                    return true;
                }
            }
            return false;
        });
        if (!moved || !wait) return;
        timers.fire(wait->id);
    }
}

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Declaring, and taking a task only when the application opted in

TEST(ClientTasks, DeclaresTheExtensionOnlyWhenTheApplicationOptsIn) {
    for (const bool accept : {false, true}) {
        TaskServer server([](int, const Seen&) { return ok(completed()); }, tool_result("at once"));
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_with(taking(timers, accept), server.peer);
        const auto called = client.call_tool("work");
        ASSERT_TRUE(called) << called.error().message;
        EXPECT_EQ(text_of(*called), "at once");
        for (const Seen& seen : server.peer->written()) {
            if (seen.method() != "tools/call") continue;
            const json& declared = seen.message["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"];
            const bool declares = declared.contains("extensions") && declared["extensions"].contains(extension);
            EXPECT_EQ(declares, accept) << seen.message.dump();
        }
        EXPECT_TRUE(client.close());
    }
}

TEST(ClientTasks, ATaskForAClientThatDidNotOptInIsMalformed) {
    TaskServer server([](int, const Seen&) { return ok(completed()); });
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_with(taking(timers, false), server.peer);
    std::optional<neomcp::Result<neomcp::ToolCall>> outcome;
    {
        // A client that took the task would wait for its first read; closing ends its call.
        Caller caller(client);
        const OnExit close_first([&] {
            if (!caller.done()) static_cast<void>(client.close());
        });
        ASSERT_TRUE(eventually([&] { return caller.done(); }, std::chrono::seconds(2)));
        outcome.emplace(caller.result());
    }
    const auto& called = *outcome;
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::malformed_answer);
    EXPECT_EQ(server.peer->count("tasks/get"), 0u);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
    EXPECT_TRUE(started(*timers, neomcp::DeadlineKind::poll).empty());
    // Without the option there are no requests about tasks either.
    const auto handle = client.task(task_id);
    ASSERT_FALSE(handle);
    EXPECT_EQ(handle.error().kind, ClientErrorKind::unsupported_method);
    // start_tool is call_tool then.
    const auto start = client.start_tool("work");
    ASSERT_FALSE(start);
    EXPECT_EQ(start.error().kind, ClientErrorKind::malformed_answer);
}

TEST(ClientTasks, ATaskForARequestThatATaskCannotAnswerIsMalformed) {
    TaskServer server([](int, const Seen&) { return ok(completed()); });
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_with(taking(timers), server.peer);
    const auto prompt = client.get_prompt("p");
    ASSERT_FALSE(prompt);
    EXPECT_EQ(prompt.error().kind, ClientErrorKind::malformed_answer);
    EXPECT_EQ(server.peer->count("tasks/get"), 0u);
}

// ---------------------------------------------------------------------------------------------------------------
// The waits between reads

TEST(ClientTasks, WaitsWhatTheServerSuggestsKeptWithinTheBounds) {
    // The server's suggestions in turn: below the floor, at it, above it, at the ceiling, above it, the largest number
    // a client could be sent, and none (the last one stays).
    const std::vector<json> suggested{99, 100, 101, 1000, 1001, json(18446744073709551615ULL), json(nullptr)};
    TaskServer server(
        [&](int read, const Seen&) {
            if (read >= static_cast<int>(suggested.size())) return ok(completed());
            json more = json::object();
            if (!suggested[static_cast<std::size_t>(read)].is_null()) more["pollIntervalMs"] = suggested[static_cast<std::size_t>(read)];
            return ok(state("working", more));
        },
        created(json{{"pollIntervalMs", suggested[0]}}));
    auto timers = std::make_shared<ManualTimers>();
    auto options = taking(timers);
    options.tasks.min_poll_interval = ms(100);
    options.tasks.max_poll_interval = ms(1000);
    neomcp::Client client = open_with(std::move(options), server.peer);
    Caller caller(client);
    std::vector<ms> waits;
    for (std::size_t i = 0; i < suggested.size(); ++i) {
        const auto wait = next_wait(*timers);
        ASSERT_TRUE(wait) << i;
        // No read goes out before its wait lapsed.
        EXPECT_EQ(server.reads(), static_cast<int>(i));
        waits.push_back(wait->after);
        ASSERT_TRUE(timers->fire(wait->id));
    }
    const auto called = caller.result();
    ASSERT_TRUE(called) << called.error().message;
    EXPECT_EQ(text_of(*called), "done");
    EXPECT_EQ(waits, (std::vector<ms>{ms(100), ms(100), ms(101), ms(1000), ms(1000), ms(1000), ms(1000)}));
    EXPECT_EQ(server.peer->count("tools/call"), 1u);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
    EXPECT_TRUE(client.close());
}

TEST(ClientTasks, WithoutASuggestionTheClientWaitsTheShortestWait) {
    TaskServer server([](int, const Seen&) { return ok(completed()); });
    auto timers = std::make_shared<ManualTimers>();
    auto options = taking(timers);
    options.tasks.min_poll_interval = ms(250);
    neomcp::Client client = open_with(std::move(options), server.peer);
    Caller caller(client);
    const auto wait = next_wait(*timers);
    ASSERT_TRUE(wait);
    EXPECT_EQ(wait->after, ms(250));
    ASSERT_TRUE(timers->fire(wait->id));
    ASSERT_TRUE(caller.result());
}

// ---------------------------------------------------------------------------------------------------------------
// The bounds of one task: the reads, the time, the id

// max_polls reads at most: a task that ends at the last read allowed ends the call; one that would need one more
// ends it task_polls_exceeded, and the task is cancelled.
TEST(ClientTasks, ReadsATaskAtMostMaxPollsTimes) {
    for (const int ends_at : {3, 4}) {
        TaskServer server([&](int read, const Seen&) { return ok(read >= ends_at ? completed() : state("working")); });
        auto timers = std::make_shared<ManualTimers>();
        auto options = taking(timers);
        options.tasks.max_polls = 3;
        neomcp::Client client = open_with(std::move(options), server.peer);
        Caller caller(client);
        drive(*timers, caller);
        const auto called = caller.result();
        EXPECT_EQ(server.reads(), 3) << ends_at;
        if (ends_at == 3) {
            ASSERT_TRUE(called) << called.error().message;
            EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
        } else {
            ASSERT_FALSE(called);
            EXPECT_EQ(called.error().kind, ClientErrorKind::task_polls_exceeded);
            EXPECT_EQ(called.error().task, std::optional<std::string>(task_id));
            EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
        }
        EXPECT_TRUE(client.close());
    }
}

TEST(ClientTasks, MaxWaitEndsTheCallTimedOutAndCancelsTheTask) {
    TaskServer server([](int, const Seen&) { return ok(state("working")); });
    auto timers = std::make_shared<ManualTimers>();
    auto options = taking(timers);
    options.tasks.max_wait = ms(5000);
    neomcp::Client client = open_with(std::move(options), server.peer);
    Caller caller(client);
    const auto whole = timers->live([](const ManualTimers::Started& one) { return is_kind(one, neomcp::DeadlineKind::task); });
    ASSERT_TRUE(whole);
    EXPECT_EQ(whole->after, ms(5000));
    ASSERT_TRUE(timers->fire(whole->id));
    const auto called = caller.result();
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::timed_out);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
    timers->fire_all();
    EXPECT_EQ(server.peer->count("tasks/get"), 0u);
}

TEST(ClientTasks, AnIdOfMaxIdBytesIsTakenAndOneByteMoreIsMalformed) {
    for (const std::size_t length : {std::size_t{0}, std::size_t{16}, std::size_t{17}}) {
        const std::string id(length, 'x');
        json first = created();
        first["taskId"] = id;
        TaskServer server(
            [&](int, const Seen&) {
                json done = completed();
                done["taskId"] = id;
                return ok(done);
            },
            first);
        auto timers = std::make_shared<ManualTimers>();
        auto options = taking(timers);
        options.tasks.max_id_bytes = 16;
        neomcp::Client client = open_with(std::move(options), server.peer);
        Caller caller(client);
        // A client that took a longer id would wait for its first read; closing ends its call.
        const OnExit close_first([&] {
            if (!caller.done()) static_cast<void>(client.close());
        });
        if (length == 16) {
            const auto wait = next_wait(*timers);
            ASSERT_TRUE(wait);
            ASSERT_TRUE(timers->fire(wait->id));
            const auto called = caller.result();
            ASSERT_TRUE(called) << called.error().message;
            // The request names the task by its id.
            for (const Seen& seen : server.peer->written()) {
                if (seen.method() == "tasks/get") {
                    EXPECT_EQ(seen.message["params"]["taskId"], id);
                }
            }
        } else {
            ASSERT_TRUE(eventually([&] { return caller.done(); }, std::chrono::seconds(2))) << length;
            const auto called = caller.result();
            ASSERT_FALSE(called);
            EXPECT_EQ(called.error().kind, ClientErrorKind::malformed_answer) << length;
            EXPECT_EQ(server.peer->count("tasks/get"), 0u);
        }
        // An id the application gives is held to the same bound.
        const auto handle = client.task(std::string(17, 'x'));
        ASSERT_FALSE(handle);
        EXPECT_EQ(handle.error().kind, ClientErrorKind::invalid_options);
        EXPECT_TRUE(client.close());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The ends of a task

TEST(ClientTasks, ATaskTheServerDoesNotKnowEndsTaskLostAndTheCallIsNotSentAgain) {
    TaskServer server([](int, const Seen&) { return refused(invalid_params); });
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_with(taking(timers), server.peer);
    Caller caller(client);
    const auto wait = next_wait(*timers);
    ASSERT_TRUE(wait);
    ASSERT_TRUE(timers->fire(wait->id));
    const auto called = caller.result();
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::task_lost);
    EXPECT_TRUE(called.error().may_have_run);
    EXPECT_EQ(called.error().task, std::optional<std::string>(task_id));
    EXPECT_EQ(server.peer->count("tools/call"), 1u);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
    // Client::task finds the same.
    const auto handle = client.task(task_id);
    ASSERT_FALSE(handle);
    EXPECT_EQ(handle.error().kind, ClientErrorKind::task_lost);
}

TEST(ClientTasks, FailedAndCancelledTasksEndTheCall) {
    const json failed = state("failed", json{{"statusMessage", "broke"},
                                             {"error", json{{"code", -32603}, {"message", "Internal error"}}}});
    for (const json& end : {failed, state("cancelled")}) {
        TaskServer server([&](int, const Seen&) { return ok(end); });
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_with(taking(timers), server.peer);
        Caller caller(client);
        const auto wait = next_wait(*timers);
        ASSERT_TRUE(wait);
        ASSERT_TRUE(timers->fire(wait->id));
        const auto called = caller.result();
        ASSERT_FALSE(called);
        if (end["status"] == "failed") {
            EXPECT_EQ(called.error().kind, ClientErrorKind::server_error);
            EXPECT_EQ(called.error().code, std::optional<int>(-32603));
        } else {
            EXPECT_EQ(called.error().kind, ClientErrorKind::task_cancelled);
        }
        EXPECT_EQ(called.error().task, std::optional<std::string>(task_id));
        // A task that ended is never cancelled again.
        EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
        EXPECT_TRUE(client.close());
    }
}

// Answers no client should take: each ends the call malformed_answer, and the task the call created is cancelled.
TEST(ClientTasks, OddAnswersToAReadAreMalformed) {
    json other = completed();
    other["taskId"] = "another";
    json unknown_status = state("finished");
    json no_result = state("completed");
    json no_error = state("failed");
    json negative_wait = state("working", json{{"pollIntervalMs", -1}});
    json fractional_wait = state("working", json{{"pollIntervalMs", 1.5}});
    json asks_again = state("working");
    asks_again["resultType"] = "input_required";
    json no_requests = state("input_required");
    for (const json& odd : {other, unknown_status, no_result, no_error, negative_wait, fractional_wait, asks_again,
                            no_requests}) {
        TaskServer server([&](int, const Seen&) { return ok(odd); });
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_with(taking(timers), server.peer);
        Caller caller(client);
        const auto wait = next_wait(*timers);
        ASSERT_TRUE(wait);
        ASSERT_TRUE(timers->fire(wait->id));
        const auto called = caller.result();
        ASSERT_FALSE(called) << odd.dump();
        EXPECT_EQ(called.error().kind, ClientErrorKind::malformed_answer) << odd.dump();
        EXPECT_EQ(server.peer->count("tasks/cancel"), 1u) << odd.dump();
        EXPECT_TRUE(client.close());
    }
}

// A task that ended carries no requests the client answers, whatever else its state holds.
TEST(ClientTasks, NoUpdateIsSentForATaskThatEnded) {
    std::atomic<int> asked{0};
    json ended = completed("finished");
    ended["inputRequests"] = waiting_on({"late"})["inputRequests"];
    TaskServer server([&](int, const Seen&) { return ok(ended); });
    auto timers = std::make_shared<ManualTimers>();
    auto options = taking(timers);
    options.handlers.form([&](const neomcp::ElicitationForm&) {
        ++asked;
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
    });
    neomcp::Client client = open_with(std::move(options), server.peer);
    Caller caller(client);
    // A client that took the state for one that waits would read on; closing ends its call.
    const OnExit close_first([&] { static_cast<void>(client.close()); });
    const auto wait = next_wait(*timers);
    ASSERT_TRUE(wait);
    ASSERT_TRUE(timers->fire(wait->id));
    ASSERT_TRUE(eventually([&] { return caller.done(); }, std::chrono::seconds(2)));
    const auto called = caller.result();
    ASSERT_TRUE(called) << called.error().message;
    EXPECT_EQ(text_of(*called), "finished");
    EXPECT_EQ(asked.load(), 0);
    EXPECT_EQ(server.peer->count("tasks/update"), 0u);
}

// ---------------------------------------------------------------------------------------------------------------
// The requests a task waits on

// Each request reaches the handler once: a request the server still lists after the client answered it (it takes the
// answer in later) is not presented again, and only the new one of a later read is.
TEST(ClientTasks, EachRequestOfATaskReachesTheHandlerOnce) {
    std::mutex mutex;
    std::vector<std::string> asked;
    TaskServer server([](int read, const Seen&) {
        switch (read) {
            case 1: return ok(waiting_on({"a"}));
            case 2: return ok(waiting_on({"a"}));
            case 3: return ok(waiting_on({"a", "b"}));
            default: return ok(completed());
        }
    });
    auto timers = std::make_shared<ManualTimers>();
    auto options = taking(timers);
    options.handlers.form([&](const neomcp::ElicitationForm& form) {
        const std::lock_guard lock(mutex);
        asked.push_back(form.message);
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
    });
    neomcp::Client client = open_with(std::move(options), server.peer);
    Caller caller(client);
    for (int i = 0; i < 4; ++i) {
        const auto wait = next_wait(*timers);
        ASSERT_TRUE(wait) << i;
        ASSERT_TRUE(timers->fire(wait->id));
    }
    const auto called = caller.result();
    ASSERT_TRUE(called) << called.error().message;
    EXPECT_EQ(asked, (std::vector<std::string>{"a", "b"}));
    std::vector<json> updates;
    for (const Seen& seen : server.peer->written()) {
        if (seen.method() == "tasks/update") updates.push_back(seen.message["params"]);
    }
    ASSERT_EQ(updates.size(), 2u);
    EXPECT_EQ(updates[0]["taskId"], task_id);
    EXPECT_TRUE(updates[0]["inputResponses"].contains("a"));
    EXPECT_EQ(updates[0]["inputResponses"]["a"]["action"], "accept");
    EXPECT_FALSE(updates[1]["inputResponses"].contains("a"));
    EXPECT_TRUE(updates[1]["inputResponses"].contains("b"));
}

// The rounds of a task count against max_input_rounds, and its requests against max_input_requests_per_round: at the
// bound the call goes on, one past it ends it, nothing reaches a handler, and the task is cancelled.
TEST(ClientTasks, TheRoundsOfATaskAreBounded) {
    struct Case {
        std::size_t rounds;
        std::size_t per_round;
        bool ends_well;
        ClientErrorKind kind;
    };
    for (const Case& one : {Case{2, 2, true, ClientErrorKind::closed},
                            Case{1, 2, false, ClientErrorKind::input_rounds_exceeded},
                            Case{2, 1, false, ClientErrorKind::input_requests_exceeded}}) {
        std::atomic<int> asked{0};
        TaskServer server([](int read, const Seen&) {
            switch (read) {
                case 1: return ok(waiting_on({"a", "b"}));
                case 2: return ok(waiting_on({"c"}));
                default: return ok(completed());
            }
        });
        auto timers = std::make_shared<ManualTimers>();
        auto options = taking(timers);
        options.max_input_rounds = one.rounds;
        options.max_input_requests_per_round = one.per_round;
        options.handlers.form([&](const neomcp::ElicitationForm&) {
            ++asked;
            return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
        });
        neomcp::Client client = open_with(std::move(options), server.peer);
        Caller caller(client);
        drive(*timers, caller);
        const auto called = caller.result();
        if (one.ends_well) {
            ASSERT_TRUE(called) << called.error().message;
            EXPECT_EQ(asked.load(), 3);
            EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
        } else {
            ASSERT_FALSE(called);
            EXPECT_EQ(called.error().kind, one.kind);
            EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
            if (one.kind == ClientErrorKind::input_requests_exceeded) {
                EXPECT_EQ(asked.load(), 0);
            }
        }
        EXPECT_TRUE(client.close());
    }
}

TEST(ClientTasks, ARequestOfAKindTheClientDidNotDeclareIsRefused) {
    TaskServer server([](int, const Seen&) {
        json requests = json{{"s", json{{"method", "sampling/createMessage"},
                                        {"params", json{{"messages", json::array()}, {"maxTokens", 10}}}}}};
        return ok(state("input_required", json{{"inputRequests", requests}}));
    });
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_with(taking(timers), server.peer);
    Caller caller(client);
    const auto wait = next_wait(*timers);
    ASSERT_TRUE(wait);
    ASSERT_TRUE(timers->fire(wait->id));
    const auto called = caller.result();
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::server_asked_undeclared);
    EXPECT_EQ(server.peer->count("tasks/update"), 0u);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
}

// ---------------------------------------------------------------------------------------------------------------
// Cancellation

// Stop while the client waits for its next read: the task is cancelled before the call ends, and no read follows.
TEST(ClientTasks, StopCancelsTheTaskAndEndsTheReads) {
    TaskServer server([](int, const Seen&) { return ok(state("working")); });
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_with(taking(timers), server.peer);
    std::stop_source stop;
    Caller caller(client, stop.get_token());
    const auto wait = next_wait(*timers);
    ASSERT_TRUE(wait);
    stop.request_stop();
    const auto called = caller.result();
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::cancelled);
    EXPECT_EQ(called.error().task, std::optional<std::string>(task_id));
    EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
    for (const Seen& seen : server.peer->written()) {
        if (seen.method() == "tasks/cancel") {
            EXPECT_EQ(seen.message["params"]["taskId"], task_id);
        }
    }
    // The wait was given up, and whatever fires later sends nothing more.
    const auto waits = started(*timers, neomcp::DeadlineKind::poll);
    ASSERT_EQ(waits.size(), 1u);
    EXPECT_TRUE(waits.front().cancelled);
    timers->fire_all();
    EXPECT_EQ(server.peer->count("tasks/get"), 0u);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
}

// The call is given up (stopped, or its wait lapsed) while its request is still unanswered, and the server's answer
// then creates a task after all: nobody follows that task, so the client cancels it.
TEST(ClientTasks, ATaskCreatedAfterTheCallWasGivenUpIsCancelled) {
    for (const bool stopped : {true, false}) {
        std::mutex mutex;
        std::optional<Seen> held;
        auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
            if (request.method() == "server/discover") {
                side.answer(request, discover_result(revision));
            } else if (request.method() == "tools/call") {
                const std::lock_guard lock(mutex);
                held = request;
            } else if (request.method() == "tasks/cancel") {
                side.answer(request, json{{"resultType", "complete"}});
            }
        });
        auto timers = std::make_shared<ManualTimers>();
        neomcp::Client client = open_with(taking(timers), peer);
        std::stop_source stop;
        Caller caller(client, stop.get_token());
        ASSERT_TRUE(eventually([&] {
            const std::lock_guard lock(mutex);
            return held.has_value();
        }));
        if (stopped) {
            stop.request_stop();
        } else {
            const auto wait = timers->live([](const ManualTimers::Started& one) {
                return is_kind(one, neomcp::DeadlineKind::wait);
            });
            ASSERT_TRUE(wait);
            ASSERT_TRUE(timers->fire(wait->id));
        }
        const auto called = caller.result();
        ASSERT_FALSE(called);
        EXPECT_EQ(called.error().kind, stopped ? ClientErrorKind::cancelled : ClientErrorKind::timed_out);
        EXPECT_EQ(peer->count("tasks/cancel"), 0u);
        Seen request;
        {
            const std::lock_guard lock(mutex);
            request = *held;
        }
        peer->answer(request, created());
        EXPECT_TRUE(eventually([&] { return peer->count("tasks/cancel") == 1; })) << stopped;
        EXPECT_EQ(peer->count("tasks/get"), 0u);
        EXPECT_TRUE(client.close());
    }
}

// Stop while a handler answers a request of the task: the task is cancelled, and what the handler gives later is
// never sent.
TEST(ClientTasks, StopWhileAHandlerAnswersSendsNoUpdate) {
    TaskServer server([](int, const Seen&) { return ok(waiting_on({"a"})); });
    auto timers = std::make_shared<ManualTimers>();
    auto options = taking(timers);
    auto latch = std::make_shared<Latch>();
    std::atomic<bool> entered{false};
    options.handlers.form([&, latch](const neomcp::ElicitationForm&, neomcp::InputContext& context) {
        entered = true;
        static_cast<void>(latch->wait(context.stop_token()));
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
    });
    neomcp::Client client = open_with(std::move(options), server.peer);
    const OnExit open_latch([&] { latch->open(); });
    std::stop_source stop;
    Caller caller(client, stop.get_token());
    const auto wait = next_wait(*timers);
    ASSERT_TRUE(wait);
    ASSERT_TRUE(timers->fire(wait->id));
    ASSERT_TRUE(eventually([&] { return entered.load(); }));
    stop.request_stop();
    const auto called = caller.result();
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::cancelled);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
    latch->open();
    EXPECT_TRUE(client.close());
    EXPECT_EQ(server.peer->count("tasks/update"), 0u);
}

// ---------------------------------------------------------------------------------------------------------------
// The handle

TEST(ClientTasks, StartToolHandsOverTheTask) {
    TaskServer server([](int read, const Seen&) { return ok(read == 1 ? state("working") : completed("handled")); });
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_with(taking(timers), server.peer);
    auto started_call = client.start_tool("work");
    ASSERT_TRUE(started_call) << started_call.error().message;
    auto* handle = std::get_if<neomcp::TaskHandle>(&*started_call);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(handle->id(), task_id);
    EXPECT_EQ(handle->state().status, neomcp::TaskStatus::working);
    EXPECT_EQ(handle->tool(), std::optional<std::string>("work"));
    // Nothing is read until the application asks.
    EXPECT_EQ(server.peer->count("tasks/get"), 0u);
    EXPECT_TRUE(started(*timers, neomcp::DeadlineKind::poll).empty());
    const auto now = handle->status();
    ASSERT_TRUE(now) << now.error().message;
    EXPECT_EQ(now->status, neomcp::TaskStatus::working);
    // wait() reads at once, then after each wait.
    std::optional<neomcp::Result<neomcp::ToolCall>> waited;
    std::thread waiter([&] { waited.emplace(handle->wait()); });
    const OnExit join_waiter([&] { join(waiter); });
    ASSERT_TRUE(eventually([&] { return server.reads() >= 2; }));
    join(waiter);
    ASSERT_TRUE(waited && *waited) << (waited ? waited->error().message : std::string("no result"));
    EXPECT_EQ(text_of(**waited), "handled");
    EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
    // A handle from an id, and its cancellation.
    const auto again = client.task(task_id);
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(again->state().status, neomcp::TaskStatus::completed);
    EXPECT_FALSE(again->tool());
    ASSERT_TRUE(again->cancel());
    EXPECT_EQ(server.peer->count("tasks/cancel"), 1u);
}

// Stop on wait() ends the wait and leaves the task alone: the handle's caller did not create it there.
TEST(ClientTasks, StopOnWaitLeavesTheTaskRunning) {
    TaskServer server([](int, const Seen&) { return ok(state("working")); });
    auto timers = std::make_shared<ManualTimers>();
    neomcp::Client client = open_with(taking(timers), server.peer);
    auto started_call = client.start_tool("work");
    ASSERT_TRUE(started_call) << started_call.error().message;
    const auto handle = std::get<neomcp::TaskHandle>(*started_call);
    std::stop_source stop;
    std::optional<neomcp::Result<neomcp::ToolCall>> waited;
    std::thread waiter([&] {
        neomcp::RequestOptions options;
        options.stop = stop.get_token();
        waited.emplace(handle.wait(options));
    });
    const OnExit join_waiter([&] { join(waiter); });
    ASSERT_TRUE(next_wait(*timers));
    stop.request_stop();
    join(waiter);
    ASSERT_TRUE(waited);
    ASSERT_FALSE(*waited);
    EXPECT_EQ(waited->error().kind, ClientErrorKind::cancelled);
    EXPECT_EQ(server.peer->count("tasks/cancel"), 0u);
    timers->fire_all();
    EXPECT_EQ(server.reads(), 1);
}

TEST(ClientTasks, OptionsThatCannotWorkAreRefused) {
    const auto refuses = [](const std::function<void(neomcp::ClientTaskOptions&)>& change) {
        TaskServer server([](int, const Seen&) { return ok(completed()); });
        auto options = taking(std::make_shared<ManualTimers>());
        change(options.tasks);
        auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(
                                                                     neomcp::profile::Transport::stdio, server.peer));
        return !opened && opened.error().kind == ClientErrorKind::invalid_options;
    };
    EXPECT_TRUE(refuses([](neomcp::ClientTaskOptions& tasks) { tasks.min_poll_interval = ms(0); }));
    EXPECT_TRUE(refuses([](neomcp::ClientTaskOptions& tasks) { tasks.max_poll_interval = ms(0); }));
    EXPECT_TRUE(refuses([](neomcp::ClientTaskOptions& tasks) {
        tasks.min_poll_interval = ms(2);
        tasks.max_poll_interval = ms(1);
    }));
    EXPECT_TRUE(refuses([](neomcp::ClientTaskOptions& tasks) { tasks.max_wait = ms(0); }));
    EXPECT_TRUE(refuses([](neomcp::ClientTaskOptions& tasks) { tasks.max_polls = 0; }));
    EXPECT_TRUE(refuses([](neomcp::ClientTaskOptions& tasks) { tasks.max_id_bytes = 0; }));
    EXPECT_FALSE(refuses([](neomcp::ClientTaskOptions& tasks) {
        tasks.min_poll_interval = ms(1);
        tasks.max_poll_interval = ms(1);
    }));
}

}  // namespace
