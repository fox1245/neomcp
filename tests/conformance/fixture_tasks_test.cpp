// The flows of the conformance runner's scenarios of the tasks extension (revision 2026-07-28), sent to the fixture's
// registrations with tasks on (task_options and register_task_tools in everything.h) through the in-memory peer, with
// the assertions those scenarios make. The runner runs only in CI; this test runs with the rest of the suite.
//
// Every wait is bounded; a bound is reached only when something is broken.
#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "everything.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;

constexpr std::chrono::milliseconds patience = 30s;
constexpr std::chrono::milliseconds poll_step = 20ms;
constexpr const char* modern = "2026-07-28";
constexpr const char* extension = "io.modelcontextprotocol/tasks";
constexpr const char* related_task_key = "io.modelcontextprotocol/related-task";

json declaring() {
    json capabilities = json{{"elicitation", json::object()}};
    capabilities["extensions"][extension] = json::object();
    return capabilities;
}

class FixtureTasks : public ::testing::Test {
protected:
    FixtureTasks() : server(conformance::task_options()) {
        conformance::register_everything(server);
        conformance::register_task_tools(server);
        peer.emplace(server);
    }

    json send(const std::string& method, json params, const json& capabilities = declaring()) {
        params["_meta"] = Peer::request_meta(*neomcp::profile::builtin(modern), capabilities);
        const json message{{"jsonrpc", "2.0"}, {"id", ++next_id}, {"method", method}, {"params", std::move(params)}};
        auto answer = peer->request(message, patience);
        if (!answer) throw std::runtime_error("no answer to " + method);
        return *answer;
    }

    json call(const std::string& tool, json arguments = json::object(), const json& capabilities = declaring()) {
        return send("tools/call", json{{"name", tool}, {"arguments", std::move(arguments)}}, capabilities);
    }

    json get(const std::string& task) { return send("tasks/get", json{{"taskId", task}}); }

    // tasks/get until the status is `wanted` (or, when empty, a terminal one); the last answer.
    json poll_until(const std::string& task, const std::string& wanted = {}) {
        const auto until = std::chrono::steady_clock::now() + patience;
        json answer;
        while (std::chrono::steady_clock::now() < until) {
            answer = get(task);
            if (!answer.contains("result")) return answer;
            const std::string status = answer["result"].value("status", "");
            const bool ended = status == "completed" || status == "failed" || status == "cancelled";
            if (wanted.empty() ? ended : status == wanted) return answer;
            std::this_thread::sleep_for(poll_step);
        }
        return answer;
    }

    // A task the call created; fails the test when the call was not answered with one.
    // An acknowledgement: resultType "complete" and nothing else but the result's _meta.
    static void expect_acknowledgement(const json& answer) {
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_EQ(answer["result"]["resultType"], "complete") << answer.dump();
        for (const auto& item : answer["result"].items()) {
            EXPECT_TRUE(item.key() == "resultType" || item.key() == "_meta") << answer.dump();
        }
    }

    std::string task_of(const json& created) {
        EXPECT_EQ(created["result"]["resultType"], "task") << created.dump();
        EXPECT_FALSE(created["result"].contains("requestState")) << created.dump();
        EXPECT_FALSE(created["result"].contains("task")) << "flat, not nested: " << created.dump();
        return created["result"].value("taskId", std::string());
    }

    neomcp::Server server;
    std::optional<Peer> peer;
    int next_id = 0;
};

}  // namespace

TEST_F(FixtureTasks, ToolsListHasEveryToolTheScenariosCall) {
    const json listed = send("tools/list", json::object());
    std::vector<std::string> names;
    for (const json& tool : listed["result"]["tools"]) names.push_back(tool["name"].get<std::string>());
    for (const char* wanted : {"greet", "slow_compute", "confirm_delete", "multi_input", "failing_job",
                               "protocol_error_job", "test_tool_with_task"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), wanted), names.end()) << wanted;
    }
    const json discovered = send("server/discover", json::object());
    EXPECT_EQ(discovered["result"]["capabilities"]["extensions"][extension], json::object()) << discovered.dump();
}

// tasks-sync-tool-call, sep-2663-legacy-task-param-ignored, tasks-headers-tolerate-mcp-method-on-tools-call.
TEST_F(FixtureTasks, GreetIsAnOrdinaryCallWithOrWithoutTheOlderTaskMember) {
    for (const bool legacy : {false, true}) {
        json params{{"name", "greet"}, {"arguments", {{"name", "Ada"}}}};
        if (legacy) params["task"] = json{{"ttl", 60000}, {"pollInterval", 1000}};
        const json answer = send("tools/call", params);
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_EQ(answer["result"]["resultType"], "complete");
        EXPECT_EQ(answer["result"]["content"][0]["text"], "Hello, Ada!");
        EXPECT_FALSE(answer["result"].contains("taskId"));
    }
}

// sep-2663-result-type-task-on-create, sep-2663-tasks-get-status-working, sep-2663-tasks-get-status-completed,
// tasks-wire-field-renames, tasks-no-related-task-meta-on-inlined-result, sep-2663-durable-create-strong-consistency,
// tasks-request-state-removal, tasks-per-request-meta-opt-in, sep-2663-server-rejects-undeclared-client.
TEST_F(FixtureTasks, SlowComputeIsATaskForADeclaringRequestAndSyncOtherwise) {
    const json created = call("slow_compute", json{{"seconds", 0.3}});
    const std::string task = task_of(created);
    EXPECT_TRUE(created["result"]["ttlMs"].is_number_integer());
    EXPECT_TRUE(created["result"]["pollIntervalMs"].is_number_integer());
    EXPECT_FALSE(created["result"].contains("ttl"));
    EXPECT_FALSE(created["result"].contains("pollInterval"));
    const json at_once = get(task);
    ASSERT_TRUE(at_once.contains("result")) << "a read right after the creation resolves: " << at_once.dump();
    EXPECT_EQ(at_once["result"]["status"], "working");
    EXPECT_EQ(at_once["result"]["resultType"], "complete");
    const json done = poll_until(task);
    EXPECT_EQ(done["result"]["status"], "completed") << done.dump();
    ASSERT_TRUE(done["result"]["result"]["content"].is_array());
    EXPECT_FALSE(done["result"].contains("requestState"));
    const json& meta = done["result"]["result"].value("_meta", json::object());
    EXPECT_FALSE(meta.contains(related_task_key)) << done.dump();

    json undeclared = json::object();
    const json sync = call("slow_compute", json{{"seconds", 0}}, undeclared);
    EXPECT_EQ(sync["result"]["resultType"], "complete") << sync.dump();
    EXPECT_TRUE(sync["result"].contains("content"));
}

// sep-2663-cancel-ack-empty-result, tasks-cancel-terminal-idempotent-ack.
TEST_F(FixtureTasks, CancellingSlowComputeSettlesItCancelledAndASecondCancelIsTheSameAck) {
    const std::string task = task_of(call("slow_compute", json{{"seconds", 30}}));
    const json ack = send("tasks/cancel", json{{"taskId", task}});
    expect_acknowledgement(ack);
    EXPECT_EQ(poll_until(task)["result"]["status"], "cancelled");
    const json again = send("tasks/cancel", json{{"taskId", task}});
    EXPECT_EQ(again["result"], ack["result"]) << again.dump();
    EXPECT_EQ(get(task)["result"]["status"], "cancelled");
}

// sep-2663-tool-error-uses-completed-status, tasks-server-directed-creation-no-hint,
// sep-2663-server-returns-missing-capability-when-required.
TEST_F(FixtureTasks, FailingJobCompletesWithItsToolErrorAndIsRequired) {
    const json done = poll_until(task_of(call("failing_job")));
    EXPECT_EQ(done["result"]["status"], "completed") << done.dump();
    EXPECT_EQ(done["result"]["result"]["isError"], true);
    const json refused = call("failing_job", json::object(), json::object());
    EXPECT_EQ(refused["error"]["code"], -32021) << refused.dump();
    EXPECT_EQ(refused["error"]["data"]["requiredCapabilities"]["extensions"][extension], json::object());
}

// sep-2663-tasks-get-status-failed.
TEST_F(FixtureTasks, ProtocolErrorJobFailsWithAnInlinedError) {
    const json done = poll_until(task_of(call("protocol_error_job")));
    EXPECT_EQ(done["result"]["status"], "failed") << done.dump();
    EXPECT_TRUE(done["result"]["error"]["code"].is_number_integer());
    EXPECT_TRUE(done["result"]["error"]["message"].is_string());
    EXPECT_FALSE(done["result"].contains("result"));
}

// sep-2663-tasks-get-status-input-required, tasks-mrtr-tasks-update-resumes.
TEST_F(FixtureTasks, ConfirmDeleteWaitsForOneAnswerAndResumes) {
    const std::string task = task_of(call("confirm_delete", json{{"filename", "a.txt"}}));
    const json waiting = poll_until(task, "input_required");
    ASSERT_EQ(waiting["result"]["status"], "input_required") << waiting.dump();
    const json& requests = waiting["result"]["inputRequests"];
    ASSERT_EQ(requests.size(), 1u) << requests.dump();
    const std::string key = requests.begin().key();
    EXPECT_EQ(requests[key]["method"], "elicitation/create");
    const json ack = send("tasks/update", json{{"taskId", task},
                                               {"inputResponses", {{key, {{"action", "accept"}, {"content", {{"ok", true}}}}}}}});
    expect_acknowledgement(ack);
    const json done = poll_until(task);
    EXPECT_EQ(done["result"]["status"], "completed") << done.dump();
}

// tasks-mrtr-partial-fulfillment.
TEST_F(FixtureTasks, MultiInputKeepsWaitingUntilBothAreAnswered) {
    const std::string task = task_of(call("multi_input"));
    const json waiting = poll_until(task, "input_required");
    const json& requests = waiting["result"]["inputRequests"];
    ASSERT_EQ(requests.size(), 2u) << waiting.dump();
    std::vector<std::string> keys;
    for (const auto& item : requests.items()) keys.push_back(item.key());
    const json answer{{"action", "accept"}, {"content", {{"value", "x"}}}};
    send("tasks/update", json{{"taskId", task}, {"inputResponses", {{keys[0], answer}}}});
    const json half = get(task);
    EXPECT_EQ(half["result"]["status"], "input_required") << half.dump();
    ASSERT_EQ(half["result"]["inputRequests"].size(), 1u) << half.dump();
    EXPECT_TRUE(half["result"]["inputRequests"].contains(keys[1]));
    send("tasks/update", json{{"taskId", task}, {"inputResponses", {{keys[1], answer}}}});
    EXPECT_EQ(poll_until(task)["result"]["status"], "completed");
}

// sep-2663-mrtr-synchronous-before-task-creation.
TEST_F(FixtureTasks, TheToolWithTaskAsksFirstAndThenBecomesATaskWithTheAnswer) {
    const json first = call("test_tool_with_task");
    ASSERT_EQ(first["result"]["resultType"], "input_required") << first.dump();
    EXPECT_FALSE(first["result"].contains("taskId"));
    ASSERT_TRUE(first["result"]["inputRequests"].contains("user_name")) << first.dump();
    json params{{"name", "test_tool_with_task"},
                {"arguments", json::object()},
                {"inputResponses", {{"user_name", {{"action", "accept"}, {"content", {{"name", "Ada"}}}}}}},
                {"requestState", first["result"]["requestState"]}};
    const json created = send("tools/call", params);
    const json done = poll_until(task_of(created));
    EXPECT_EQ(done["result"]["status"], "completed") << done.dump();
    EXPECT_NE(done["result"]["result"]["content"][0]["text"].get<std::string>().find("Ada"), std::string::npos);
}
