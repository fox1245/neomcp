// Tasks on the wire, revision 2025-11-25, continued: the input a task asks for, which reaches the client as requests
// on the stream of a waiting tasks/result; the task's progress and status notifications; the listing of a principal's
// tasks; and the differences between the tasks of 2025-11-25 and those of the 2026-07-28 extension, one table.
//
// Every wait is bounded; a bound is reached only when something is broken.
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/authorization.h"
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
constexpr std::chrono::milliseconds quiet = 300ms;
constexpr const char* legacy = "2025-11-25";
constexpr const char* modern = "2026-07-28";
constexpr const char* extension = "io.modelcontextprotocol/tasks";
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
    neomcp::ServerOptions options{.name = "tasks-2025-flow", .version = "1.0.0"};
    options.tasks.store = std::move(store);
    options.tasks.min_poll_interval = 1ms;
    return options;
}

std::shared_ptr<neomcp::TaskStore> store_of(neomcp::MemoryTaskStoreOptions options = {}) {
    return neomcp::make_memory_task_store(std::move(options));
}

json message(int id, const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

json open(Peer& peer, const json& capabilities = json::object()) {
    auto answer = peer.request(Peer::initialize_request(profile_of(legacy), 0, capabilities), patience);
    if (!answer || !answer->contains("result")) throw std::runtime_error("initialize failed");
    peer.notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    return (*answer)["result"];
}

json ask(Peer& peer, int id, const std::string& method, json params) {
    auto answer = peer.request(message(id, method, std::move(params)), patience);
    if (!answer) throw std::runtime_error("no answer to " + method);
    return *answer;
}

json poll_until(Peer& peer, const std::string& task, const std::string& wanted) {
    const auto until = std::chrono::steady_clock::now() + patience;
    int id = 10000;
    json answer;
    while (std::chrono::steady_clock::now() < until) {
        answer = ask(peer, ++id, "tasks/get", json{{"taskId", task}});
        if (!answer.contains("result") || answer["result"].value("status", "") == wanted) return answer;
        std::this_thread::sleep_for(poll_step);
    }
    return answer;
}

neomcp::ElicitationForm name_form(std::string text) {
    return neomcp::ElicitationForm{
        std::move(text), json::parse(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})")};
}

json accepted(const std::string& name) { return json{{"action", "accept"}, {"content", {{"name", name}}}}; }

std::string name_in(const neomcp::Context& ctx, const std::string& key) {
    const auto found = ctx.input_responses().find(key);
    if (found == ctx.input_responses().end()) return std::string();
    const auto* answer = std::get_if<neomcp::ElicitationAnswer>(&found->second);
    if (answer == nullptr || !answer->content.contains("name")) return std::string();
    return answer->content.at("name").get<std::string>();
}

using Outcome = std::variant<std::string, neomcp::InputRequired>;

struct Person {
    std::string name;
};

// The next notification of `method`, skipping the others; empty after the patience.
std::optional<json> next_of(Peer& peer, const std::string& method, std::chrono::milliseconds bound = patience) {
    const auto until = std::chrono::steady_clock::now() + bound;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds::zero()) return std::nullopt;
        auto notification = peer.next_notification(left);
        if (!notification) return std::nullopt;
        if ((*notification)["method"] == method) return notification;
    }
}

// Feeds units to one session of the server as a transport that authorizes does, and returns each unit's answer.
class Rig {
public:
    explicit Rig(neomcp::Server& server) {
        auto made = server.connect_tagged(neomcp::profile::Transport::stdio,
                                          [this](neomcp::Output output) { record(std::move(output)); });
        if (auto* error = std::get_if<neomcp::CreateError>(&made)) throw std::runtime_error(error->message);
        session_ = std::get<std::unique_ptr<neomcp::Session>>(std::move(made));
    }

    // The response of the unit, or null for a unit that has none (a notification).
    json send(const json& line, std::shared_ptr<const neomcp::Authorization> who) {
        const neomcp::UnitId unit = ++next_;
        session_->receive(unit, neomcp::serialize(line), {}, std::move(who));
        std::unique_lock lock(mutex_);
        if (!arrived_.wait_for(lock, patience, [&] { return ended_.count(unit) != 0; })) {
            throw std::runtime_error("no last output of a unit within the patience");
        }
        const auto found = responses_.find(unit);
        return found == responses_.end() ? json() : found->second;
    }

private:
    void record(neomcp::Output output) {
        if (!output.unit) return;
        const std::lock_guard lock(mutex_);
        if (output.kind == neomcp::OutputKind::response) responses_[*output.unit] = json::parse(output.line);
        if (output.last) ended_.insert(*output.unit);
        arrived_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable arrived_;
    std::map<neomcp::UnitId, json> responses_;
    std::set<neomcp::UnitId> ended_;
    neomcp::UnitId next_ = 0;
    std::unique_ptr<neomcp::Session> session_;  // last: closed before the members its sink uses go
};

std::shared_ptr<const neomcp::Authorization> principal(const std::string& subject) {
    auto granted = std::make_shared<neomcp::Authorization>();
    granted->issuer = "https://issuer.example";
    granted->subject = subject;
    return granted;
}

}  // namespace

NEOMCP_DESCRIBE(Person, name, "the name")

// C3.6: a task that asks for input waits in input_required; a tasks/result then carries the requests to the client as
// requests of the server, marked with the task; the answers go to the task, which runs on, and the waiting tasks/result
// answers with the result. A later round is sent on the same stream.
TEST(ServerTasks2025FlowTest, InputReachesTheClientOnTheResultStream) {
    neomcp::Server server(options_with(store_of()));
    server.tool("ask", "Ask twice", [](neomcp::Context& ctx) -> Outcome {
        const std::optional<std::string>& state = ctx.request_state();
        if (!state) {
            neomcp::InputRequired asked;
            asked.requests.emplace("first", name_form("First name?"));
            asked.state = "round-1";
            return asked;
        }
        if (*state == "round-1") {
            // The second round asks by the key the first one used.
            neomcp::InputRequired asked;
            asked.requests.emplace("first", name_form("Nickname?"));
            asked.state = name_in(ctx, "first");
            return asked;
        }
        return *state + " alias " + name_in(ctx, "first");
    }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer, json{{"elicitation", json::object()}});
    json created = ask(peer, 1, "tools/call", json{{"name", "ask"}, {"arguments", json::object()}, {"task", json::object()}});
    const std::string id = created["result"]["task"]["taskId"];
    EXPECT_EQ(poll_until(peer, id, "input_required")["result"]["status"], "input_required");

    peer.notify(message(2, "tasks/result", json{{"taskId", id}}));
    for (const std::string name : {"Ada", "Countess"}) {
        auto asked = peer.next_request(patience);
        ASSERT_TRUE(asked) << "no request for input reached the client";
        EXPECT_EQ((*asked)["method"], "elicitation/create") << asked->dump();
        EXPECT_EQ((*asked)["params"]["_meta"][related_task]["taskId"], id) << asked->dump();
        peer.send(neomcp::serialize(json{{"jsonrpc", "2.0"}, {"id", (*asked)["id"]}, {"result", accepted(name)}}));
    }
    auto answer = peer.await_response(2, patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ((*answer)["result"]["content"][0]["text"], "Ada alias Countess") << answer->dump();
    EXPECT_EQ((*answer)["result"]["_meta"][related_task]["taskId"], id);
    EXPECT_FALSE(peer.next_request(quiet)) << "nothing was asked twice";
}

// C3.6: the requests of a task wait in its record while no tasks/result is open; a tasks/result that ends before they
// are answered leaves them outstanding, and the next one sends them again. Inputs the tool declares are asked by the
// task, after it was created.
TEST(ServerTasks2025FlowTest, UnansweredRequestsAreSentAgainByTheNextResultRequest) {
    auto store = store_of();
    neomcp::Server server(options_with(store));
    server.tool(
        "greet", "Greet",
        [](const neomcp::Answer<Person>& person) { return "Hello, " + (person.accepted() ? person->name : "nobody"); },
        neomcp::elicit<Person>("person", [] { return std::string("Who?"); }))
        .task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer, json{{"elicitation", json::object()}});
    json created = ask(peer, 1, "tools/call", json{{"name", "greet"}, {"arguments", json::object()}, {"task", json::object()}});
    ASSERT_TRUE(created["result"].contains("task")) << "the task exists before its declared input is answered";
    const std::string id = created["result"]["task"]["taskId"];
    EXPECT_EQ(poll_until(peer, id, "input_required")["result"]["status"], "input_required");

    peer.notify(message(2, "tasks/result", json{{"taskId", id}}));
    auto first = peer.next_request(patience);
    ASSERT_TRUE(first);
    peer.notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 2}}}});
    // The answer to a request of a stream that ended is not taken; the task still waits.
    peer.send(neomcp::serialize(json{{"jsonrpc", "2.0"}, {"id", (*first)["id"]}, {"result", accepted("Late")}}));
    EXPECT_EQ(poll_until(peer, id, "input_required")["result"]["status"], "input_required");

    peer.notify(message(3, "tasks/result", json{{"taskId", id}}));
    auto again = peer.next_request(patience);
    ASSERT_TRUE(again);
    EXPECT_EQ((*again)["params"]["message"], (*first)["params"]["message"]);
    EXPECT_NE((*again)["id"], (*first)["id"]);
    peer.send(neomcp::serialize(json{{"jsonrpc", "2.0"}, {"id", (*again)["id"]}, {"result", accepted("Grace")}}));
    auto answer = peer.await_response(3, patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ((*answer)["result"]["content"][0]["text"], "Hello, Grace") << answer->dump();
}

// C3.7, C3.8: the progress of a task's handler, under the call's progress token, and the changes of its status reach
// the connection that created it; a status notification carries the task without the mark of the task.
TEST(ServerTasks2025FlowTest, ProgressAndStatusReachTheConnection) {
    Event release;
    neomcp::Server server(options_with(store_of()));
    server.tool("slow", "Report and wait", [&](neomcp::Context& ctx) {
        ctx.progress(0.5, 1.0);
        ctx.task_status("half way");
        release.wait();
        return std::string("done");
    }).task_support(TaskSupport::optional);
    Peer peer(server);
    open(peer);
    json created = ask(peer, 1, "tools/call",
                             json{{"name", "slow"},
                                  {"arguments", json::object()},
                                  {"task", json::object()},
                                  {"_meta", {{"progressToken", "p-1"}}}});
    const std::string id = created["result"]["task"]["taskId"];
    const auto progress = next_of(peer, "notifications/progress");
    ASSERT_TRUE(progress);
    EXPECT_EQ((*progress)["params"]["progressToken"], "p-1");
    EXPECT_EQ((*progress)["params"]["progress"], 0.5);
    release.set();
    std::optional<json> status;
    for (;;) {
        status = next_of(peer, "notifications/tasks/status");
        if (!status || (*status)["params"]["status"] == "completed") break;
        EXPECT_EQ((*status)["params"]["statusMessage"], "half way") << status->dump();
    }
    ASSERT_TRUE(status) << "no notification of the completed status";
    EXPECT_EQ((*status)["params"]["taskId"], id);
    EXPECT_TRUE((*status)["params"]["ttl"].is_number_integer());
    EXPECT_FALSE((*status)["params"].contains("_meta")) << status->dump();
}

// C3.5, C6: tasks/list lists the tasks of the request's principal only, in pages, and is declared only to a connection
// whose handshake carried an authorization; a request without one lists nothing (the method does not exist for it),
// and a cursor the server did not give is invalid params. Another principal's task is unknown to every request.
TEST(ServerTasks2025FlowTest, AListingShowsThePrincipalsOwnTasks) {
    neomcp::MemoryTaskStoreOptions bounds;
    bounds.max_page_size = 2;
    neomcp::Server server(options_with(store_of(bounds)));
    server.tool("ok", "Answer", [] { return std::string("done"); }).task_support(TaskSupport::optional);
    const auto ada = principal("ada");
    const auto bob = principal("bob");
    json task_call{{"name", "ok"}, {"arguments", json::object()}, {"task", json::object()}};

    Rig bound(server);
    json opened = bound.send(Peer::initialize_request(profile_of(legacy), 0), ada);
    EXPECT_EQ(opened["result"]["capabilities"]["tasks"]["list"], json::object()) << opened.dump();
    bound.send(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, ada);
    std::set<std::string> adas;
    for (int id = 1; id <= 3; ++id) {
        adas.insert(bound.send(message(id, "tools/call", task_call), ada)["result"]["task"]["taskId"].get<std::string>());
    }
    const std::string bobs = bound.send(message(4, "tools/call", task_call), bob)["result"]["task"]["taskId"];

    std::set<std::string> listed;
    json page = bound.send(message(5, "tasks/list", json::object()), ada);
    ASSERT_TRUE(page.contains("result")) << page.dump();
    EXPECT_EQ(page["result"]["tasks"].size(), 2u);
    ASSERT_TRUE(page["result"]["nextCursor"].is_string()) << page.dump();
    for (json task : page["result"]["tasks"]) listed.insert(task["taskId"].get<std::string>());
    page = bound.send(message(6, "tasks/list", json{{"cursor", page["result"]["nextCursor"]}}), ada);
    for (json task : page["result"]["tasks"]) listed.insert(task["taskId"].get<std::string>());
    EXPECT_FALSE(page["result"].contains("nextCursor")) << page.dump();
    EXPECT_EQ(listed, adas);

    json theirs = bound.send(message(7, "tasks/list", json::object()), bob);
    ASSERT_EQ(theirs["result"]["tasks"].size(), 1u) << theirs.dump();
    EXPECT_EQ(theirs["result"]["tasks"][0]["taskId"], bobs);
    json crossed = bound.send(message(8, "tasks/get", json{{"taskId", bobs}}), ada);
    EXPECT_EQ(crossed["error"]["code"], neomcp::error_code::invalid_params) << crossed.dump();
    json bad = bound.send(message(9, "tasks/list", json{{"cursor", "not a cursor"}}), ada);
    EXPECT_EQ(bad["error"]["code"], neomcp::error_code::invalid_params) << bad.dump();
    json anonymous = bound.send(message(10, "tasks/list", json::object()), nullptr);
    EXPECT_EQ(anonymous["error"]["code"], neomcp::error_code::method_not_found) << anonymous.dump();

    // Without an authorization: no listing declared, none served, although tasks of nobody exist.
    Peer peer(server);
    json plain = open(peer);
    EXPECT_FALSE(plain["capabilities"]["tasks"].contains("list")) << plain.dump();
    ask(peer, 1, "tools/call", task_call);
    json refused = ask(peer, 2, "tasks/list", json::object());
    EXPECT_EQ(refused["error"]["code"], neomcp::error_code::method_not_found) << refused.dump();
}

// ---- The two revisions side by side ----------------------------------------------------------------------------

// What differs between the tasks of the two revisions, for the same server and the same tools.
struct Difference {
    std::string revision;
    bool asked_by_the_call;            // `task` in the call's params asks for a task
    const char* tool_error_status;     // the status a tool execution error ends the task with
    bool ended_cancel_taken;           // a cancellation of a task that ended is taken (otherwise invalid params)
    bool cancel_answers_the_task;      // the answer to a cancellation is the task (otherwise an acknowledgement)
    bool result_request;               // tasks/result exists
    bool read_inlines_the_result;      // tasks/get of a completed task carries its result
};

class ServerTasks2025DifferenceTest : public ::testing::TestWithParam<Difference> {
protected:
    ServerTasks2025DifferenceTest() : server(options_with(store_of())) {
        server.tool("ok", "Answer", [] { return std::string("the answer"); }).task_support(TaskSupport::optional);
        server.tool("tool_error", "Fail as a tool", []() -> std::string {
            throw neomcp::ToolError("the tool could not");
        }).task_support(TaskSupport::optional);
        server.tool("long", "Run until cancelled", [](neomcp::Context& ctx) {
            while (!ctx.cancelled()) std::this_thread::sleep_for(poll_step);
            return std::string("finished anyway");
        }).task_support(TaskSupport::optional);
    }

    bool modern_revision() const { return GetParam().revision == modern; }

    // A request of the revision: under 2026-07-28 it names the revision and, when `declares`, the extension.
    json request(int id, const std::string& method, json params, bool declares = true) const {
        if (modern_revision()) {
            json capabilities = json::object();
            if (declares) capabilities["extensions"][extension] = json::object();
            params["_meta"] = Peer::request_meta(profile_of(modern), capabilities);
        }
        return message(id, method, std::move(params));
    }
    json send(Peer& peer, int id, const std::string& method, json params, bool declares = true) const {
        auto answer = peer.request(request(id, method, std::move(params), declares), patience);
        if (!answer) throw std::runtime_error("no answer to " + method);
        return *answer;
    }
    // A call that becomes a task under either revision; its id.
    std::string start(Peer& peer, int id, const std::string& tool) const {
        json params{{"name", tool}, {"arguments", json::object()}};
        if (!modern_revision()) params["task"] = json::object();
        json created = send(peer, id, "tools/call", std::move(params));
        const json& task = modern_revision() ? created["result"] : created["result"]["task"];
        return task.at("taskId").get<std::string>();
    }
    json until_ended(Peer& peer, const std::string& task) const {
        const auto until = std::chrono::steady_clock::now() + patience;
        int id = 20000;
        json answer;
        while (std::chrono::steady_clock::now() < until) {
            answer = send(peer, ++id, "tasks/get", json{{"taskId", task}});
            const std::string status = answer["result"].value("status", "");
            if (status == "completed" || status == "failed" || status == "cancelled") return answer;
            std::this_thread::sleep_for(poll_step);
        }
        return answer;
    }

    neomcp::Server server;
};

TEST_P(ServerTasks2025DifferenceTest, TheRevisionsDifferAsTheTableSays) {
    const Difference& expected = GetParam();
    Peer peer(server);
    if (!modern_revision()) open(peer);

    // Who asks for a task: the call's `task` member, or the request's declaration of the extension.
    json flagged = send(peer, 1, "tools/call", json{{"name", "ok"}, {"arguments", json::object()}, {"task", json::object()}},
                              false);
    ASSERT_TRUE(flagged.contains("result")) << flagged.dump();
    EXPECT_EQ(flagged["result"].contains("content"), !expected.asked_by_the_call) << flagged.dump();

    const std::string done = start(peer, 2, "ok");
    json read = until_ended(peer, done);
    EXPECT_EQ(read["result"]["status"], "completed");
    EXPECT_EQ(read["result"].contains("result"), expected.read_inlines_the_result) << read.dump();

    const std::string tool_error = start(peer, 3, "tool_error");
    EXPECT_EQ(until_ended(peer, tool_error)["result"]["status"], expected.tool_error_status);

    json result = send(peer, 4, "tasks/result", json{{"taskId", done}});
    if (expected.result_request) {
        EXPECT_EQ(result["result"]["content"][0]["text"], "the answer") << result.dump();
    } else {
        EXPECT_EQ(result["error"]["code"], neomcp::error_code::method_not_found) << result.dump();
    }

    json late = send(peer, 5, "tasks/cancel", json{{"taskId", done}});
    EXPECT_EQ(late.contains("result"), expected.ended_cancel_taken) << late.dump();
    if (!expected.ended_cancel_taken) {
        EXPECT_EQ(late["error"]["code"], neomcp::error_code::invalid_params);
    }

    const std::string running = start(peer, 6, "long");
    json cancelled = send(peer, 7, "tasks/cancel", json{{"taskId", running}});
    ASSERT_TRUE(cancelled.contains("result")) << cancelled.dump();
    EXPECT_EQ(cancelled["result"].contains("status"), expected.cancel_answers_the_task) << cancelled.dump();
    // Both: cancelled before the answer, and it stays cancelled when the handler returns.
    std::this_thread::sleep_for(10 * poll_step);
    EXPECT_EQ(until_ended(peer, running)["result"]["status"], "cancelled");
}

INSTANTIATE_TEST_SUITE_P(ServerTasks2025Revisions, ServerTasks2025DifferenceTest,
                         ::testing::Values(Difference{legacy, true, "failed", false, true, true, false},
                                           Difference{modern, false, "completed", true, false, false, true}),
                         [](const ::testing::TestParamInfo<Difference>& info) {
                             return info.param.revision == legacy ? std::string("Revision2025") : std::string("Revision2026");
                         });
