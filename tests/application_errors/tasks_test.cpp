// A call that runs as a task and ends with an error of the application's own: the task fails with that code, message and
// data, and the requester who reads the failure gets them, under both revisions that have tasks (2026-07-28 with the
// extension io.modelcontextprotocol/tasks, which inlines the error in the task, and 2025-11-25, where tasks/result
// answers the error of the call). A code the revision does not let an application use, and an answer the store cannot
// keep, fail the task as an internal error.
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/jsonrpc.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"
#include "registrations.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::TaskSupport;
using neomcp::testing::Peer;

constexpr std::chrono::milliseconds patience = 30s;
constexpr std::chrono::milliseconds poll_step = 2ms;
constexpr const char* modern = "2026-07-28";
constexpr const char* legacy = "2025-11-25";
constexpr const char* extension = "io.modelcontextprotocol/tasks";
constexpr std::string_view tool_plain = "task_fail";
constexpr std::string_view tool_coroutine = "task_fail_coroutine";
constexpr std::string_view fixed_failure = "Internal error: the tool call failed";

const neomcp::profile::Profile& profile_of(const std::string& revision) {
    const neomcp::profile::ProfilePtr found = neomcp::profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return *found;
}

std::set<int> named_codes(const neomcp::profile::Profile& profile) {
    std::set<int> codes;
    for (const auto& named : profile.error_codes()) codes.insert(named.code);
    for (const int code : profile.not_emitted()) codes.insert(code);
    return codes;
}

neomcp::ServerOptions options_with(std::shared_ptr<neomcp::TaskStore> store) {
    neomcp::ServerOptions options{.name = "application-errors-tasks", .version = "1.0.0"};
    options.tasks.store = std::move(store);
    options.tasks.min_poll_interval = 1ms;
    return options;
}

std::shared_ptr<neomcp::TaskStore> store_of(std::size_t max_result_bytes = neomcp::TaskStoreDefaults::max_result_bytes) {
    neomcp::MemoryTaskStoreOptions options;
    options.max_result_bytes = max_result_bytes;
    return neomcp::make_memory_task_store(options);
}

json message(int id, const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

bool terminal(const std::string& status) { return status == "completed" || status == "failed" || status == "cancelled"; }

// What a requester of one revision sees of a task that failed: the error the call ended with, found where the revision
// puts it, and the status message.
struct Failure {
    std::string status;
    json error;           // null when the task carries none
    json status_message;  // null when none
};

// A requester of one revision, and how it starts a task and reads its failure.
class Requester {
public:
    Requester(neomcp::Server& server, const std::string& revision)
        : revision_(revision), profile_(&profile_of(revision)), peer_(server) {
        if (!is_modern()) {
            auto opened = peer_.request(Peer::initialize_request(*profile_, 0), patience);
            if (!opened || !opened->contains("result")) throw std::runtime_error("initialize failed");
            peer_.notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        }
    }

    bool is_modern() const { return revision_ == modern; }

    // Calls `tool` as a task; the task's id.
    std::string start(std::string_view tool, json arguments = json::object()) {
        json params{{"name", std::string(tool)}, {"arguments", std::move(arguments)}};
        json answer;
        if (is_modern()) {
            json meta = Peer::request_meta(*profile_, json{{"extensions", {{extension, json::object()}}}});
            params["_meta"] = std::move(meta);
            answer = ask(next_id(), "tools/call", std::move(params));
            EXPECT_TRUE(answer.contains("result")) << answer.dump();
            return answer.at("result").at("taskId").get<std::string>();
        }
        params["task"] = json::object();
        answer = ask(next_id(), "tools/call", std::move(params));
        EXPECT_TRUE(answer.contains("result")) << answer.dump();
        return answer.at("result").at("task").at("taskId").get<std::string>();
    }

    // Reads the task until it has ended.
    Failure read(const std::string& task) {
        const auto until = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < until) {
            const json answer = ask(next_id(), "tasks/get", json{{"taskId", task}});
            EXPECT_TRUE(answer.contains("result")) << answer.dump();
            if (!answer.contains("result")) break;
            const std::string status = answer["result"].value("status", "");
            if (!terminal(status)) {
                std::this_thread::sleep_for(poll_step);
                continue;
            }
            Failure failure;
            failure.status = status;
            failure.status_message = answer["result"].value("statusMessage", json());
            if (is_modern()) {
                failure.error = answer["result"].value("error", json());
            } else {
                const json result = ask(next_id(), "tasks/result", json{{"taskId", task}});
                failure.error = result.value("error", json());
            }
            return failure;
        }
        ADD_FAILURE() << "the task did not end";
        return {};
    }

private:
    int next_id() { return ++id_; }

    json ask(int id, const std::string& method, json params) {
        if (is_modern()) {
            json meta = Peer::request_meta(*profile_, json{{"extensions", {{extension, json::object()}}}});
            params["_meta"] = std::move(meta);
        }
        auto answer = peer_.request(message(id, method, std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer to " + method);
        return *answer;
    }

    std::string revision_;
    const neomcp::profile::Profile* profile_;
    Peer peer_;
    int id_ = 0;
};

class Tasks : public ::testing::TestWithParam<std::string> {};

TEST_P(Tasks, ATaskFailsWithTheCodeMessageAndDataOfTheHandler) {
    neomcp::Server server(options_with(store_of()));
    application_errors::register_task_tools(server);
    Requester requester(server, GetParam());
    const std::set<int> named = named_codes(profile_of(GetParam()));
    for (const std::string_view tool : {tool_plain, tool_coroutine}) {
        const std::string_view kind =
            tool == tool_plain ? application_errors::kind_tool : application_errors::kind_tool_coroutine;
        for (const int code : {-32050, 7, -33000, -32099}) {
            ASSERT_FALSE(named.contains(code));
            const Failure failure = requester.read(requester.start(tool, json{{"code", code}}));
            const std::string where = std::string(tool) + " " + std::to_string(code);
            EXPECT_EQ(failure.status, "failed") << where;
            ASSERT_TRUE(failure.error.is_object()) << where;
            EXPECT_EQ(failure.error["code"], code) << where;
            EXPECT_EQ(failure.error["message"], application_errors::message_of(kind)) << where;
            EXPECT_EQ(failure.error["data"], application_errors::data_of(kind, code)) << where;
            EXPECT_EQ(failure.status_message, application_errors::message_of(kind)) << where;
        }
    }
}

TEST_P(Tasks, ATaskThatEndsWithACodeTheRevisionDoesNotAllowFailsAsAnInternalError) {
    neomcp::Server server(options_with(store_of()));
    application_errors::register_task_tools(server);
    Requester requester(server, GetParam());
    std::set<int> refused = named_codes(profile_of(GetParam()));
    refused.insert({neomcp::error_code::parse_error, neomcp::error_code::invalid_request,
                    neomcp::error_code::method_not_found, neomcp::error_code::invalid_params,
                    neomcp::error_code::internal_error, neomcp::error_code::reserved_range_first,
                    neomcp::error_code::implementation_range_first - 1});
    for (const int code : refused) {
        const Failure failure = requester.read(requester.start(tool_plain, json{{"code", code}}));
        const std::string where = std::to_string(code);
        EXPECT_EQ(failure.status, "failed") << where;
        ASSERT_TRUE(failure.error.is_object()) << where;
        EXPECT_EQ(failure.error["code"], neomcp::error_code::internal_error) << where;
        EXPECT_EQ(failure.error["message"], std::string(fixed_failure)) << where;
        EXPECT_FALSE(failure.error.contains("data")) << where << ": " << failure.error.dump();
        EXPECT_EQ(failure.status_message, std::string(fixed_failure)) << where;
    }
}

TEST_P(Tasks, AnErrorTheStoreCannotKeepFailsTheTaskAtItsBoundOnePast) {
    // The bound of the store on the error object, at its edge and one past.
    const auto error_size = [](std::size_t message_bytes) {
        json error = json::object();
        error["code"] = -32050;
        error["message"] = std::string(message_bytes, 'x');
        return error.dump().size();
    };
    constexpr std::size_t bound = 2048;
    const std::size_t at_edge = bound - error_size(0);
    ASSERT_EQ(error_size(at_edge), bound);

    neomcp::Server server(options_with(store_of(bound)));
    server.tool("long", "Fails with a message of the given length", [at_edge]() -> neomcp::ToolResult {
        throw neomcp::ApplicationError(-32050, std::string(at_edge, 'x'));
    }).task_support(TaskSupport::optional);
    server.tool("longer", "Fails with a message one byte longer", [at_edge]() -> neomcp::ToolResult {
        throw neomcp::ApplicationError(-32050, std::string(at_edge + 1, 'x'));
    }).task_support(TaskSupport::optional);
    Requester requester(server, GetParam());

    const Failure fits = requester.read(requester.start("long"));
    ASSERT_TRUE(fits.error.is_object());
    EXPECT_EQ(fits.error["code"], -32050);
    EXPECT_EQ(fits.error["message"].get<std::string>().size(), at_edge);

    const Failure past = requester.read(requester.start("longer"));
    ASSERT_TRUE(past.error.is_object());
    EXPECT_EQ(past.status, "failed");
    EXPECT_EQ(past.error["code"], neomcp::error_code::internal_error);
    EXPECT_LT(past.error.dump().size(), bound);
}

INSTANTIATE_TEST_SUITE_P(ApplicationErrors, Tasks, ::testing::Values(std::string(modern), std::string(legacy)),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return "Revision_" + name;
                         });

}  // namespace
