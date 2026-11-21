// Tasks on the wire, revision 2026-07-28 with the extension io.modelcontextprotocol/tasks: a session of a server with a
// task store, through the in-memory peer. The server decides: a call of a tool that declares that it may run as a task
// is answered with a task when the request declares the extension; tasks/get, tasks/update and tasks/cancel then ask
// about it. The tests over the Streamable HTTP endpoint are in http_tasks_test.cpp.
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
#include <vector>

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
constexpr const char* modern = "2026-07-28";
constexpr const char* extension = "io.modelcontextprotocol/tasks";
constexpr int missing_capability_code = -32021;

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

json declaring(json more = json::object()) {
    more["extensions"][extension] = json::object();
    return more;
}

// A request of 2026-07-28 whose `_meta` declares `capabilities`, with `meta` added to it.
json request(int id, const std::string& method, json params, const json& capabilities, const json& meta = {}) {
    json full = Peer::request_meta(profile_of(modern), capabilities);
    if (meta.is_object()) {
        for (const auto& item : meta.items()) full[item.key()] = item.value();
    }
    params["_meta"] = std::move(full);
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

json call_tool(Peer& peer, int id, const std::string& tool, const json& capabilities = declaring(),
               json arguments = json::object(), const json& meta = {}) {
    auto answer = peer.request(
        request(id, "tools/call", json{{"name", tool}, {"arguments", std::move(arguments)}}, capabilities, meta),
        patience);
    if (!answer) throw std::runtime_error("no answer to the call of " + tool);
    return *answer;
}

json task_request(Peer& peer, int id, const std::string& method, json params, const json& capabilities = declaring()) {
    auto answer = peer.request(request(id, method, std::move(params), capabilities), patience);
    if (!answer) throw std::runtime_error("no answer to " + method);
    return *answer;
}

json get_task(Peer& peer, int id, const std::string& task) {
    return task_request(peer, id, "tasks/get", json{{"taskId", task}});
}

bool terminal(const std::string& status) { return status == "completed" || status == "failed" || status == "cancelled"; }

// tasks/get until the task's status is `wanted` (or, when empty, any terminal one); the last answer.
json poll_until(Peer& peer, const std::string& task, const std::string& wanted = {}) {
    const auto until = std::chrono::steady_clock::now() + patience;
    int id = 10000;
    json answer;
    while (std::chrono::steady_clock::now() < until) {
        answer = get_task(peer, ++id, task);
        if (!answer.contains("result")) return answer;
        const std::string status = answer["result"].value("status", "");
        if (wanted.empty() ? terminal(status) : status == wanted) return answer;
        std::this_thread::sleep_for(poll_step);
    }
    return answer;
}

std::string task_id_of(const json& created) { return created.at("result").at("taskId").get<std::string>(); }

neomcp::ServerOptions options_with(std::shared_ptr<neomcp::TaskStore> store) {
    neomcp::ServerOptions options{.name = "tasks-wire", .version = "1.0.0"};
    options.tasks.store = std::move(store);
    options.tasks.min_poll_interval = 1ms;
    return options;
}

std::shared_ptr<neomcp::TaskStore> store_of(std::size_t max_tasks = neomcp::TaskStoreDefaults::max_tasks) {
    neomcp::MemoryTaskStoreOptions options;
    options.max_tasks = max_tasks;
    options.max_tasks_per_owner = max_tasks;
    return neomcp::make_memory_task_store(options);
}

neomcp::ElicitationForm name_form(std::string message) {
    return neomcp::ElicitationForm{
        std::move(message),
        json::parse(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})")};
}

json accepted(const std::string& name) { return json{{"action", "accept"}, {"content", {{"name", name}}}}; }

std::string name_in(const neomcp::Context& call, const std::string& key) {
    const auto found = call.input_responses().find(key);
    if (found == call.input_responses().end()) return std::string();
    const auto* answer = std::get_if<neomcp::ElicitationAnswer>(&found->second);
    if (answer == nullptr || !answer->content.contains("name")) return std::string();
    return answer->content.at("name").get<std::string>();
}

using Outcome = std::variant<std::string, neomcp::InputRequired>;

// The form a declared input asks for.
struct Person {
    std::string name;
};

}  // namespace

NEOMCP_DESCRIBE(Person, name, "the name")

// C1.2: server/discover declares the extension when the server has a store and a tool that may run as a task; with
// either missing, nothing about tasks is declared and the task's requests do not exist.
TEST(ServerTasksWireTest, DiscoverDeclaresTheExtensionOnlyWhenATaskCanRun) {
    const auto discover = [](neomcp::Server& server) {
        Peer peer(server);
        auto answer = peer.request(request(1, "server/discover", json::object(), declaring()), patience);
        EXPECT_TRUE(answer && answer->contains("result")) << (answer ? answer->dump() : "none");
        return answer ? (*answer)["result"]["capabilities"] : json();
    };
    {
        neomcp::Server server(options_with(store_of()));
        server.tool("work", "Work", [] { return std::string("done"); }).task_support(TaskSupport::optional);
        const json capabilities = discover(server);
        ASSERT_TRUE(capabilities.contains("extensions")) << capabilities.dump();
        EXPECT_EQ(capabilities["extensions"][extension], json::object());
        EXPECT_FALSE(capabilities.contains("tasks")) << "no capability of the older revision";
    }
    for (const bool with_store : {true, false}) {
        neomcp::Server server(with_store ? options_with(store_of()) : neomcp::ServerOptions{.name = "plain"});
        server.tool("work", "Work", [] { return std::string("done"); })
            .task_support(with_store ? TaskSupport::forbidden : TaskSupport::optional);
        const json capabilities = discover(server);
        EXPECT_FALSE(capabilities.contains("extensions")) << capabilities.dump();
        Peer peer(server);
        const json missing = get_task(peer, 2, "0123456789abcdef0123456789abcdef");
        EXPECT_EQ(missing["error"]["code"], neomcp::error_code::method_not_found) << missing.dump();
        const json plain = call_tool(peer, 3, "work");
        EXPECT_EQ(plain["result"]["content"][0]["text"], "done") << plain.dump();
    }
}

// C0.2, C0.3: a revision whose profile has no tasks serves every call as an ordinary one, whatever the client
// declares; the requests of tasks do not exist there. (2025-11-25 has tasks of its own: server_tasks_2025_test.cpp.)
TEST(ServerTasksWireTest, RevisionsWithoutTheExtensionServeOrdinaryCalls) {
    neomcp::Server server(options_with(store_of()));
    server.tool("maybe", "Maybe a task", [] { return std::string("ordinary"); }).task_support(TaskSupport::optional);
    server.tool("always", "Only a task", [] { return std::string("ordinary too"); }).task_support(TaskSupport::required);
    for (const std::string revision : {"2025-06-18"}) {
        const auto& profile = profile_of(revision);
        Peer peer(server);
        ASSERT_TRUE(peer.initialize(profile, patience));
        for (const std::string tool : {"maybe", "always"}) {
            const json params{{"name", tool},
                              {"arguments", json::object()},
                              {"task", {{"ttl", 1000}}},
                              {"_meta", {{"io.modelcontextprotocol/clientCapabilities", declaring()}}}};
            auto answer = peer.request(Peer::make_request(profile, 5, "tools/call", params), patience);
            ASSERT_TRUE(answer);
            ASSERT_TRUE((*answer)["result"].contains("content")) << revision << " " << answer->dump();
            EXPECT_FALSE((*answer)["result"].contains("taskId"));
        }
        auto get = peer.request(Peer::make_request(profile, 6, "tasks/get", json{{"taskId", "x"}}), patience);
        ASSERT_TRUE(get);
        EXPECT_EQ((*get)["error"]["code"], neomcp::error_code::method_not_found) << revision << " " << get->dump();
    }
}

// C2.5, C2.6, C2.8, C2.9: an optional tool becomes a task exactly when the request itself declares the extension; the
// answer is the flat task with resultType "task" and an id that cannot be guessed. The older capability and the older
// request member are not a declaration.
TEST(ServerTasksWireTest, AnOptionalToolIsATaskOnlyForARequestThatDeclaresTheExtension) {
    auto store = store_of();
    neomcp::Server server(options_with(store));
    server.tool("maybe", "Maybe a task", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    server.tool("never", "Never a task", [] { return std::string("plain"); });
    Peer peer(server);

    const json created = call_tool(peer, 1, "maybe");
    ASSERT_TRUE(created.contains("result")) << created.dump();
    const json& task = created["result"];
    EXPECT_EQ(task["resultType"], "task");
    EXPECT_EQ(task["status"], "working");
    const std::string id = task.value("taskId", "");
    EXPECT_EQ(id.size(), 32u);
    EXPECT_TRUE(std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) << id;
    for (const char* member : {"createdAt", "lastUpdatedAt"}) {
        const std::string at = task.value(member, "");
        EXPECT_EQ(at.size(), 24u) << member << " " << at;
        EXPECT_EQ(at.back(), 'Z') << at;
        EXPECT_EQ(at[10], 'T') << at;
    }
    EXPECT_TRUE(task["ttlMs"].is_number_integer()) << task.dump();
    EXPECT_TRUE(task["pollIntervalMs"].is_number_integer()) << task.dump();
    EXPECT_FALSE(task.contains("content"));
    EXPECT_EQ(store->usage().tasks, 1u);

    // The same request without the declaration, with only the older capability, or with the older request member.
    const json plain = call_tool(peer, 2, "maybe", json::object());
    EXPECT_EQ(plain["result"]["content"][0]["text"], "done") << plain.dump();
    EXPECT_EQ(plain["result"]["resultType"], "complete");
    const json legacy = call_tool(peer, 3, "maybe", json{{"tasks", {{"requests", {{"tools", {{"call", json::object()}}}}}}}});
    EXPECT_EQ(legacy["result"]["content"][0]["text"], "done") << legacy.dump();
    auto flagged = peer.request(
        request(4, "tools/call", json{{"name", "maybe"}, {"arguments", json::object()}, {"task", {{"ttl", 5}}}},
                json::object()),
        patience);
    ASSERT_TRUE(flagged);
    EXPECT_EQ((*flagged)["result"]["content"][0]["text"], "done") << flagged->dump();
    const json never = call_tool(peer, 5, "never");
    EXPECT_EQ(never["result"]["content"][0]["text"], "plain") << never.dump();
    EXPECT_EQ(store->usage().tasks, 1u) << "only the declaring call of the declaring tool made a task";
}

// C2.6: a required tool called by a request that does not declare the extension is refused with -32021, naming the
// extension.
TEST(ServerTasksWireTest, ARequiredToolRefusesARequestThatDoesNotDeclareIt) {
    auto store = store_of();
    neomcp::Server server(options_with(store));
    server.tool("always", "Only a task", [] { return std::string("done"); }).task_support(TaskSupport::required);
    Peer peer(server);
    const json refused = call_tool(peer, 1, "always", json::object());
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused["error"]["code"], missing_capability_code);
    EXPECT_EQ(refused["error"]["data"]["requiredCapabilities"], declaring()) << refused.dump();
    EXPECT_EQ(store->usage().tasks, 0u);
    const json created = call_tool(peer, 2, "always");
    EXPECT_EQ(created["result"]["resultType"], "task") << created.dump();
}

// C4.2: a read answers with resultType "complete" and the task's state; a completed task inlines its result as the call
// would have answered (a tool execution error included), a failed one the error of the call and a status message.
TEST(ServerTasksWireTest, AReadInlinesWhatTheTaskEndedWith) {
    neomcp::Server server(options_with(store_of()));
    server.tool("ok", "Answer", [] { return std::string("the answer"); }).task_support(TaskSupport::optional);
    server.tool("tool_error", "Fail as a tool", []() -> std::string {
        throw neomcp::ToolError("the tool could not");
    }).task_support(TaskSupport::optional);
    neomcp::ToolDefinition failing;
    failing.name = "failure";
    failing.output_schema = json::parse(R"({"type":"object","properties":{"value":{"type":"string"}}})");
    server.tool(failing, [](const json&) { return neomcp::ToolResult{}; }).task_support(TaskSupport::optional);
    Peer peer(server);

    const json ok = poll_until(peer, task_id_of(call_tool(peer, 1, "ok")));
    ASSERT_TRUE(ok.contains("result")) << ok.dump();
    EXPECT_EQ(ok["result"]["resultType"], "complete");
    EXPECT_EQ(ok["result"]["status"], "completed");
    ASSERT_TRUE(ok["result"].contains("result")) << "the result is inline: " << ok.dump();
    const json& result = ok["result"]["result"];
    EXPECT_EQ(result["content"][0]["text"], "the answer");
    EXPECT_EQ(result["resultType"], "complete") << "what the call would have answered";
    EXPECT_FALSE(result.contains("_meta") && result["_meta"].contains("io.modelcontextprotocol/related-task"));
    EXPECT_FALSE(ok["result"].contains("error"));

    const json tool_error = poll_until(peer, task_id_of(call_tool(peer, 2, "tool_error")));
    EXPECT_EQ(tool_error["result"]["status"], "completed") << tool_error.dump();
    EXPECT_EQ(tool_error["result"]["result"]["isError"], true);

    const json failed = poll_until(peer, task_id_of(call_tool(peer, 3, "failure")));
    EXPECT_EQ(failed["result"]["status"], "failed") << failed.dump();
    EXPECT_EQ(failed["result"]["error"]["code"], neomcp::error_code::internal_error);
    EXPECT_TRUE(failed["result"]["statusMessage"].is_string());
    EXPECT_FALSE(failed["result"].contains("result"));
}

// C4.1: the task's requests need the declaration in the request itself (-32021 otherwise), and a known id (-32602,
// the same for every id that does not name a task of the requester).
TEST(ServerTasksWireTest, TaskRequestsNeedTheDeclarationAndAKnownTask) {
    neomcp::Server server(options_with(store_of()));
    server.tool("ok", "Answer", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    Peer peer(server);
    const std::string id = task_id_of(call_tool(peer, 1, "ok"));
    int next = 2;
    const json update_params{{"taskId", id}, {"inputResponses", json::object()}};
    for (const auto& [method, params] : {std::pair<std::string, json>{"tasks/get", {{"taskId", id}}},
                                         std::pair<std::string, json>{"tasks/update", update_params},
                                         std::pair<std::string, json>{"tasks/cancel", {{"taskId", id}}}}) {
        const json undeclared = task_request(peer, next++, method, params, json::object());
        EXPECT_EQ(undeclared["error"]["code"], missing_capability_code) << method << " " << undeclared.dump();
        EXPECT_EQ(undeclared["error"]["data"]["requiredCapabilities"], declaring());
        json unknown_params = params;
        unknown_params["taskId"] = "ffffffffffffffffffffffffffffffff";
        const json unknown = task_request(peer, next++, method, unknown_params);
        EXPECT_EQ(unknown["error"]["code"], neomcp::error_code::invalid_params) << method << " " << unknown.dump();
        json malformed_params = params;
        malformed_params["taskId"] = 7;
        const json malformed = task_request(peer, next++, method, malformed_params);
        EXPECT_EQ(malformed["error"]["code"], neomcp::error_code::invalid_params) << method << " " << malformed.dump();
    }
    const json not_object = task_request(peer, next++, "tasks/update", json{{"taskId", id}, {"inputResponses", 3}});
    EXPECT_EQ(not_object["error"]["code"], neomcp::error_code::invalid_params) << not_object.dump();
    // tasks/result and tasks/list belong to the older revision only.
    for (const std::string removed : {"tasks/result", "tasks/list"}) {
        const json answer = task_request(peer, next++, removed, json{{"taskId", id}});
        EXPECT_EQ(answer["error"]["code"], neomcp::error_code::method_not_found) << removed << " " << answer.dump();
    }
}

// C4.6: a cancellation is acknowledged with an empty result; a running task is cancelled at once and its handler asked
// to stop; a task that has ended is acknowledged too and stays as it ended, with its result.
TEST(ServerTasksWireTest, ACancellationIsAcknowledgedAndLeavesAnEndedTaskAsItEnded) {
    Event started;
    Event saw_stop;
    auto store = store_of();
    neomcp::Server server(options_with(store));
    server.tool("long", "Run until cancelled", [&](neomcp::Context& call) {
        started.set();
        Event never;
        if (!never.wait(call.stop_token())) saw_stop.set();
        return std::string("stopped");
    }).task_support(TaskSupport::optional);
    server.tool("quick", "Answer", [] { return std::string("kept"); }).task_support(TaskSupport::optional);
    Peer peer(server);

    const std::string running = task_id_of(call_tool(peer, 1, "long"));
    ASSERT_TRUE(started.wait());
    const json ack = task_request(peer, 2, "tasks/cancel", json{{"taskId", running}});
    ASSERT_TRUE(ack.contains("result")) << ack.dump();
    EXPECT_EQ(ack["result"]["resultType"], "complete");
    EXPECT_FALSE(ack["result"].contains("status")) << "an acknowledgement, not the task";
    EXPECT_TRUE(saw_stop.wait());
    EXPECT_EQ(get_task(peer, 3, running)["result"]["status"], "cancelled");

    const std::string ended = task_id_of(call_tool(peer, 4, "quick"));
    ASSERT_EQ(poll_until(peer, ended)["result"]["status"], "completed");
    const json late = task_request(peer, 5, "tasks/cancel", json{{"taskId", ended}});
    EXPECT_EQ(late["result"]["resultType"], "complete") << late.dump();
    const std::optional<neomcp::TaskRecord> kept = store->get(ended, neomcp::TaskOwner{});
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->status, neomcp::TaskStatus::completed);
    // A read later than the interval within which reads share one copy of the record.
    std::this_thread::sleep_for(4 * poll_step);
    const json after = get_task(peer, 6, ended);
    EXPECT_EQ(after["result"]["status"], "completed") << after.dump();
    EXPECT_EQ(after["result"]["result"]["content"][0]["text"], "kept");
}

// C4.3, C4.4: the requests a task waits on are read with tasks/get under keys unique over the task's life, and answered
// with tasks/update: a part of them is kept, unknown or answered keys are ignored, a malformed answer is refused, and
// the handler is entered again once every request of the round has its answer.
TEST(ServerTasksWireTest, AnUpdateAnswersTheRequestsATaskWaitsOn) {
    neomcp::Server server(options_with(store_of()));
    server.tool("ask", "Ask twice", [](neomcp::Context& call) -> Outcome {
        const std::optional<std::string>& state = call.request_state();
        if (!state) {
            neomcp::InputRequired asked;
            asked.requests.emplace("first", name_form("First name?"));
            asked.requests.emplace("last", name_form("Last name?"));
            asked.state = "round-1";
            return asked;
        }
        if (*state == "round-1") {
            // The second round asks by a key the first one used.
            neomcp::InputRequired asked;
            asked.requests.emplace("first", name_form("Nickname?"));
            asked.state = name_in(call, "first") + " " + name_in(call, "last");
            return asked;
        }
        return *state + " alias " + name_in(call, "first");
    }).task_support(TaskSupport::optional);
    Peer peer(server);
    const json capabilities = declaring(json{{"elicitation", json::object()}});
    const std::string id = task_id_of(call_tool(peer, 1, "ask", capabilities));

    json waiting = poll_until(peer, id, "input_required");
    ASSERT_EQ(waiting["result"]["status"], "input_required") << waiting.dump();
    const json& requests = waiting["result"]["inputRequests"];
    ASSERT_TRUE(requests.contains("1.first") && requests.contains("1.last")) << requests.dump();
    EXPECT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests["1.first"]["method"], "elicitation/create");
    EXPECT_EQ(requests["1.first"]["params"]["message"], "First name?");

    const auto update = [&](int request_id, json responses) {
        return task_request(peer, request_id, "tasks/update",
                            json{{"taskId", id}, {"inputResponses", std::move(responses)}}, capabilities);
    };
    const json malformed = update(2, json{{"1.first", {{"action", 5}}}});
    EXPECT_EQ(malformed["error"]["code"], neomcp::error_code::invalid_params) << malformed.dump();
    const json part = update(3, json{{"1.first", accepted("Ada")}, {"nope", accepted("x")}});
    EXPECT_EQ(part["result"]["resultType"], "complete") << part.dump();
    const json half = get_task(peer, 4, id);
    EXPECT_EQ(half["result"]["status"], "input_required");
    EXPECT_EQ(half["result"]["inputRequests"].size(), 1u) << half.dump();
    EXPECT_TRUE(half["result"]["inputRequests"].contains("1.last"));
    // An answer for a key answered already changes nothing.
    EXPECT_EQ(update(5, json{{"1.first", accepted("Eve")}})["result"]["resultType"], "complete");
    EXPECT_EQ(update(6, json{{"1.last", accepted("Lovelace")}})["result"]["resultType"], "complete");

    waiting = poll_until(peer, id, "input_required");
    ASSERT_EQ(waiting["result"]["status"], "input_required") << waiting.dump();
    const json& second = waiting["result"]["inputRequests"];
    ASSERT_TRUE(second.contains("2.first")) << "a new key for the request of the second round: " << second.dump();
    EXPECT_EQ(second.size(), 1u);
    // The key of the first round no longer answers anything.
    EXPECT_EQ(update(7, json{{"1.first", accepted("Old")}})["result"]["resultType"], "complete");
    EXPECT_EQ(get_task(peer, 8, id)["result"]["status"], "input_required");
    EXPECT_EQ(update(9, json{{"2.first", accepted("Countess")}})["result"]["resultType"], "complete");

    const json done = poll_until(peer, id);
    EXPECT_EQ(done["result"]["status"], "completed") << done.dump();
    EXPECT_EQ(done["result"]["result"]["content"][0]["text"], "Ada Lovelace alias Countess");
}

// C2.7: inputs a tool declares are asked by the call itself, before any task exists; the retried call with the answers
// then becomes the task, whose handler gets them.
TEST(ServerTasksWireTest, DeclaredInputsAreAskedBeforeTheTaskExists) {
    auto store = store_of();
    neomcp::Server server(options_with(store));
    server.tool(
        "greet", "Greet",
        [](const neomcp::Answer<Person>& person) { return "Hello, " + (person.accepted() ? person->name : "nobody"); },
        neomcp::elicit<Person>("person", [] { return std::string("Who?"); }))
        .task_support(TaskSupport::optional);
    Peer peer(server);
    const json capabilities = declaring(json{{"elicitation", json::object()}});

    const json asking = call_tool(peer, 1, "greet", capabilities);
    ASSERT_TRUE(asking.contains("result")) << asking.dump();
    EXPECT_EQ(asking["result"]["resultType"], "input_required") << asking.dump();
    ASSERT_TRUE(asking["result"]["inputRequests"].contains("person"));
    EXPECT_EQ(store->usage().tasks, 0u) << "no task before the inputs are answered";

    json retry = request(2, "tools/call",
                         json{{"name", "greet"},
                              {"arguments", json::object()},
                              {"inputResponses", {{"person", accepted("Ada")}}},
                              {"requestState", asking["result"]["requestState"]}},
                         capabilities);
    auto created = peer.request(retry, patience);
    ASSERT_TRUE(created);
    ASSERT_EQ((*created)["result"]["resultType"], "task") << created->dump();
    const json done = poll_until(peer, task_id_of(*created));
    EXPECT_EQ(done["result"]["result"]["content"][0]["text"], "Hello, Ada") << done.dump();
}

// C4.9: under this revision a task's handler sends neither progress nor log messages, though the request asked for
// both and its session is still open.
TEST(ServerTasksWireTest, ATaskSendsNoProgressAndNoLogMessages) {
    Event reported;
    Event release;
    neomcp::ServerOptions options = options_with(store_of());
    options.protocol_logging = true;
    neomcp::Server server(std::move(options));
    server.tool("chatty", "Report, then answer", [&](neomcp::Context& call) {
        call.progress(1, 2, "half");
        call.info("working");
        reported.set();
        release.wait();
        return std::string("quiet");
    }).task_support(TaskSupport::optional);
    server.tool("chatty_call", "Report, then answer", [](neomcp::Context& call) {
        call.progress(1, 2, "half");
        call.info("working");
        return std::string("loud");
    });
    Peer peer(server);
    const json meta{{"progressToken", "p-1"}, {"io.modelcontextprotocol/logLevel", "debug"}};
    // The same reports from an ordinary call do reach the client: the request is one that asks for them.
    const json loud = call_tool(peer, 1, "chatty_call", declaring(), json::object(), meta);
    ASSERT_EQ(loud["result"]["content"][0]["text"], "loud") << loud.dump();
    const std::size_t heard = peer.transcript().size();
    EXPECT_GE(heard, 3u) << "the call's progress and log message, and its answer";

    const json created = call_tool(peer, 2, "chatty", declaring(), json::object(), meta);
    ASSERT_EQ(created["result"]["resultType"], "task") << created.dump();
    ASSERT_TRUE(reported.wait());
    release.set();
    EXPECT_EQ(poll_until(peer, task_id_of(created))["result"]["status"], "completed");
    std::size_t notifications = 0;
    for (const json& message : peer.transcript()) {
        if (message.contains("method")) ++notifications;
    }
    EXPECT_EQ(notifications, 2u) << "only the ordinary call's two reports";
}

// C4.5 and the lifetime of a task: its requests for input carry only what the request that created it declared, and it
// can still ask once the session of that request is gone.
TEST(ServerTasksWireTest, ATaskAsksWithTheCapabilitiesOfTheRequestThatCreatedIt) {
    Event go;
    neomcp::Server server(options_with(store_of()));
    server.tool("polite", "Ask only what the client takes", [&](neomcp::Context& call) -> Outcome {
        if (!name_in(call, "name").empty()) return "Hello, " + name_in(call, "name");
        go.wait();
        if (!call.client_supports(name_form("Name?"))) return std::string("not asked");
        neomcp::InputRequired asked;
        asked.requests.emplace("name", name_form("Name?"));
        return asked;
    }).task_support(TaskSupport::optional);
    server.tool("rude", "Ask anyway", [](neomcp::Context&) -> Outcome {
        neomcp::InputRequired asked;
        asked.requests.emplace("name", name_form("Name?"));
        return asked;
    }).task_support(TaskSupport::optional);

    std::string declared;
    std::string undeclared;
    std::string rude;
    {
        Peer creating(server);
        declared = task_id_of(call_tool(creating, 1, "polite", declaring(json{{"elicitation", json::object()}})));
        undeclared = task_id_of(call_tool(creating, 2, "polite"));
        rude = task_id_of(call_tool(creating, 3, "rude"));
        creating.close();
    }
    go.set();  // the tasks ask now, with the session that created them closed
    Peer peer(server);
    const json waiting = poll_until(peer, declared, "input_required");
    ASSERT_EQ(waiting["result"]["status"], "input_required") << waiting.dump();
    ASSERT_TRUE(waiting["result"]["inputRequests"].contains("1.name"));
    task_request(peer, 4, "tasks/update", json{{"taskId", declared}, {"inputResponses", {{"1.name", accepted("Ada")}}}});
    EXPECT_EQ(poll_until(peer, declared)["result"]["result"]["content"][0]["text"], "Hello, Ada");
    EXPECT_EQ(poll_until(peer, undeclared)["result"]["result"]["content"][0]["text"], "not asked");
    const json failed = poll_until(peer, rude);
    EXPECT_EQ(failed["result"]["status"], "failed") << failed.dump();
}

// C2.10: a store that is full refuses the task, and the call is answered as a busy call is, with no task made.
TEST(ServerTasksWireTest, AFullStoreRefusesTheCallAsBusy) {
    Event release;
    auto store = store_of(1);
    neomcp::Server server(options_with(store));
    server.tool("hold", "Hold", [&](neomcp::Context& call) {
        release.wait(call.stop_token());
        return std::string("held");
    }).task_support(TaskSupport::optional);
    Peer peer(server);
    const json first = call_tool(peer, 1, "hold");
    ASSERT_EQ(first["result"]["resultType"], "task") << first.dump();
    const json second = call_tool(peer, 2, "hold");
    ASSERT_TRUE(second.contains("error")) << second.dump();
    EXPECT_EQ(second["error"]["code"], neomcp::error_code::internal_error);
    EXPECT_EQ(second["error"]["data"]["reason"], "busy");
    EXPECT_EQ(store->usage().tasks, 1u);
    release.set();
}

// C2.9, C2.10: a store that cannot draw an id refuses the task; the call is answered as busy, with no task made.
TEST(ServerTasksWireTest, AStoreWithoutAnIdRefusesTheCallAsBusy) {
    neomcp::MemoryTaskStoreOptions store_options;
    store_options.new_id = [] { return std::optional<std::string>(); };
    auto store = neomcp::make_memory_task_store(store_options);
    neomcp::Server server(options_with(store));
    server.tool("quick", "Answer", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    Peer peer(server);
    const json refused = call_tool(peer, 1, "quick");
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused["error"]["code"], neomcp::error_code::internal_error);
    EXPECT_EQ(refused["error"]["data"]["reason"], "busy");
    EXPECT_EQ(store->usage().tasks, 0u);
}

// The registry has the three names a profile can select for tasks, so that every built-in revision can be served.
TEST(ServerTasksWireTest, TheRegistryImplementsEveryDeclaredTasksName) {
    const neomcp::MechanismRegistry& registry = neomcp::builtin_mechanisms();
    for (const auto name : neomcp::profile::declared_mechanisms(neomcp::profile::Slot::tasks)) {
        EXPECT_TRUE(registry.contains(neomcp::MechanismSlot::tasks, name)) << name;
        EXPECT_NE(registry.make_tasks(name), nullptr) << name;
    }
    const auto composed = neomcp::profile::builtin(modern, std::vector<std::string_view>{extension});
    ASSERT_TRUE(composed);
    const auto directed = registry.make_tasks("server_directed");
    EXPECT_FALSE(directed->check(*composed).has_value()) << directed->check(*composed).value_or("");
    EXPECT_TRUE(directed->check(profile_of(modern)).has_value()) << "the core profile alone has no tasks";
}
