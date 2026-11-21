// The built-in mechanisms of the slot `tasks`, and what the core keeps of tasks itself: the spelling of a status and
// the owner of a request's tasks. The protocol identifiers a mechanism works with are constants of that mechanism;
// which types and members exist is read from the profile (check).
#include "core/tasks.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "neomcp/jsonrpc.h"
#include "neomcp/task_host.h"

namespace neomcp {

std::string_view to_string(TaskStatus status) noexcept {
    switch (status) {
        case TaskStatus::working: return "working";
        case TaskStatus::input_required: return "input_required";
        case TaskStatus::completed: return "completed";
        case TaskStatus::failed: return "failed";
        case TaskStatus::cancelled: return "cancelled";
    }
    return {};
}

TaskOwner task_owner_of(const Authorization* authorization) {
    if (authorization == nullptr) return TaskOwner{};
    return TaskOwner{principal_of(*authorization).text()};
}

namespace detail {

namespace {

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

// Appends `value` in decimal, with leading zeros to `width` digits.
void append_padded(std::string& out, std::int64_t value, std::size_t width) {
    std::array<char, 24> digits{};
    const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    static_cast<void>(error);
    const auto length = static_cast<std::size_t>(end - digits.data());
    if (length < width) out.append(width - length, '0');
    out.append(digits.data(), length);
}

// A request for input as a record keeps it: its method, and its params when it has any.
constexpr std::string_view entry_method_member = "method";
constexpr std::string_view entry_params_member = "params";

json entry_of(std::string method, json params) {
    json entry = json::object();
    entry[std::string(entry_method_member)] = std::move(method);
    if (!params.is_null()) entry[std::string(entry_params_member)] = std::move(params);
    return entry;
}

std::optional<std::string> method_in(const json& entry) {
    const json* method = member(entry, entry_method_member);
    const auto* text = method == nullptr ? nullptr : method->get_ptr<const std::string*>();
    if (text == nullptr) return std::nullopt;
    return *text;
}

json params_in(const json& entry) {
    const json* params = member(entry, entry_params_member);
    return params == nullptr ? json() : *params;
}

// `none`: every call is an ordinary one.
class NoTasks final : public TaskMechanism {
public:
    std::optional<std::string> check(const profile::Profile&) const override { return std::nullopt; }
    bool answers_with_tasks() const override { return false; }
    bool requester_takes_tasks(const ClientIdentity&) const override { return false; }
    bool requests_need_declaration() const override { return false; }
    std::variant<TaskAsk, RequestError> asked(const json&, const ClientIdentity&) const override { return TaskAsk{}; }
    TaskServing serving(TaskSupport, bool) const override { return TaskServing::ordinary; }
    bool prepares_inputs() const override { return false; }
    json required_capabilities() const override { return json::object(); }
    void declare(json&, bool) const override {}
    void describe_tool(json&, TaskSupport) const override {}
    bool serves(std::string_view) const override { return false; }
    std::variant<TaskRequest, RequestError> read(const Message&) const override { return not_applicable(); }
    json created(const TaskRecord&) const override { return json::object(); }
    json detailed(const TaskRecord&) const override { return json::object(); }
    json acknowledgement() const override { return json::object(); }
    json cancelled(const TaskRecord&) const override { return json::object(); }
    bool acknowledges_ended_cancel() const override { return false; }
    std::variant<json, RequestError> payload(const TaskRecord&) const override { return not_applicable(); }
    json page(const TaskPage&) const override { return json::object(); }
    void relate(json&, std::string_view) const override {}
    std::optional<FeedMessage> status_notice(const TaskRecord&) const override { return std::nullopt; }
    bool error_result_fails() const override { return false; }
    bool round_keys() const override { return false; }
    json input_entry(std::string, json) const override { return json(); }
    std::optional<std::string> input_method(const json&) const override { return std::nullopt; }
    json input_params(const json&) const override { return json(); }
    bool task_notifications() const override { return false; }

private:
    static RequestError not_applicable() {
        return RequestError{error_code::internal_error, "tasks do not apply to this mechanism"};
    }
};

// `server_directed`: the extension io.modelcontextprotocol/tasks. A requester takes tasks when the request's own
// client capabilities declare the extension (whatever it declared before); the server then decides, per call, and
// answers with a result of `resultType` "task". The task's own requests (tasks/get, tasks/update, tasks/cancel) are
// answered only for a requester that declares it. A read inlines what the task ended with, or the requests for input
// it waits on; updates and cancellations are acknowledged with an empty result, a cancellation also when the task has
// ended. A tool execution error completes the task (only an error of the call fails it), and a task sends neither
// progress nor log messages.
class ServerDirected final : public TaskMechanism {
public:
    static constexpr std::string_view extension = "io.modelcontextprotocol/tasks";
    static constexpr std::string_view extensions_member = "extensions";
    static constexpr std::string_view get_method = "tasks/get";
    static constexpr std::string_view update_method = "tasks/update";
    static constexpr std::string_view cancel_method = "tasks/cancel";
    static constexpr std::string_view task_id_member = "taskId";
    static constexpr std::string_view responses_member = "inputResponses";
    static constexpr std::string_view result_type_member = "resultType";
    static constexpr std::string_view task_kind = "task";
    static constexpr std::string_view complete_kind = "complete";
    static constexpr std::string_view status_member = "status";
    static constexpr std::string_view message_member = "statusMessage";
    static constexpr std::string_view created_member = "createdAt";
    static constexpr std::string_view updated_member = "lastUpdatedAt";
    static constexpr std::string_view ttl_member = "ttlMs";
    static constexpr std::string_view poll_member = "pollIntervalMs";
    static constexpr std::string_view result_member = "result";
    static constexpr std::string_view error_member = "error";
    static constexpr std::string_view error_code_member = "code";
    static constexpr std::string_view error_message_member = "message";
    static constexpr std::string_view requests_member = "inputRequests";
    static constexpr std::string_view created_type = "CreateTaskResult";
    static constexpr std::string_view update_type = "UpdateTaskResult";
    static constexpr std::string_view cancel_type = "CancelTaskResult";
    static constexpr std::string_view client_capabilities_type = "ClientCapabilities";
    static constexpr std::string_view server_capabilities_type = "ServerCapabilities";
    // The error a failed task carries when its record kept none.
    static constexpr std::string_view unknown_failure = "Internal error: the task failed";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        const auto extensions = profile.extensions();
        if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
            return "the profile is not composed with the extension " + std::string(extension);
        }
        for (const auto method : {get_method, update_method, cancel_method}) {
            const profile::Message* request = profile.request(method);
            if (request == nullptr || !request->from_client) {
                return "the profile has no client request " + std::string(method);
            }
        }
        const auto lacks = [&](std::string_view name,
                               std::initializer_list<std::string_view> members) -> std::optional<std::string> {
            const profile::Type* type = profile.type(name);
            if (type == nullptr) return "the profile has no type " + std::string(name);
            for (const auto property : members) {
                if (!type->has_property(property)) {
                    return "the profile's " + std::string(name) + " has no member " + std::string(property);
                }
            }
            return std::nullopt;
        };
        std::optional<std::string> problem = lacks(created_type, {result_type_member, task_id_member, status_member,
                                                                  created_member, updated_member, ttl_member,
                                                                  poll_member, message_member});
        if (!problem) problem = lacks(status_type(TaskStatus::working), {task_id_member, status_member, ttl_member});
        if (!problem) problem = lacks(status_type(TaskStatus::input_required), {requests_member});
        if (!problem) problem = lacks(status_type(TaskStatus::completed), {result_member});
        if (!problem) problem = lacks(status_type(TaskStatus::failed), {error_member, message_member});
        if (!problem) problem = lacks(status_type(TaskStatus::cancelled), {status_member});
        if (!problem) problem = lacks(update_type, {result_type_member});
        if (!problem) problem = lacks(cancel_type, {result_type_member});
        if (!problem) problem = lacks(client_capabilities_type, {extensions_member});
        if (!problem) problem = lacks(server_capabilities_type, {extensions_member});
        return problem;
    }
    bool answers_with_tasks() const override { return true; }

    bool requester_takes_tasks(const ClientIdentity& client) const override {
        const json* declared = member(client.capabilities, extensions_member);
        const json* entry = declared == nullptr ? nullptr : member(*declared, extension);
        return entry != nullptr && entry->is_object();
    }

    bool requests_need_declaration() const override { return true; }

    // What the call itself carries about tasks is not read: the declaration of its client decides.
    std::variant<TaskAsk, RequestError> asked(const json&, const ClientIdentity& client) const override {
        TaskAsk opt_in;
        opt_in.takes = requester_takes_tasks(client);
        return opt_in;
    }

    TaskServing serving(TaskSupport support, bool takes) const override {
        switch (support) {
            case TaskSupport::forbidden: return TaskServing::ordinary;
            case TaskSupport::optional: return takes ? TaskServing::task : TaskServing::ordinary;
            case TaskSupport::required: return takes ? TaskServing::task : TaskServing::missing_capability;
        }
        return TaskServing::ordinary;
    }

    bool prepares_inputs() const override { return true; }

    json required_capabilities() const override {
        json required = json::object();
        required[std::string(extensions_member)][std::string(extension)] = json::object();
        return required;
    }

    void declare(json& capabilities, bool) const override {
        json& extensions = capabilities[std::string(extensions_member)];
        if (!extensions.is_object()) extensions = json::object();
        extensions[std::string(extension)] = json::object();
    }

    void describe_tool(json&, TaskSupport) const override {}

    bool serves(std::string_view method) const override {
        return method == get_method || method == update_method || method == cancel_method;
    }

    std::variant<TaskRequest, RequestError> read(const Message& request) const override {
        TaskRequest reading;
        if (request.method == get_method) {
            reading.action = TaskRequest::Action::get;
        } else if (request.method == update_method) {
            reading.action = TaskRequest::Action::update;
        } else if (request.method == cancel_method) {
            reading.action = TaskRequest::Action::cancel;
        } else {
            return RequestError{error_code::method_not_found, "Method not found: " + request.method};
        }
        const json* id = member(request.params, task_id_member);
        const auto* text = id == nullptr ? nullptr : id->get_ptr<const std::string*>();
        if (text == nullptr) {
            return RequestError{error_code::invalid_params,
                                "Invalid params: params." + std::string(task_id_member) + " must be a string"};
        }
        reading.task_id = *text;
        if (reading.action == TaskRequest::Action::update) {
            const json* responses = member(request.params, responses_member);
            if (responses == nullptr || !responses->is_object()) {
                return RequestError{error_code::invalid_params,
                                    "Invalid params: params." + std::string(responses_member) + " must be an object"};
            }
            reading.responses = *responses;
        }
        return reading;
    }

    json created(const TaskRecord& record) const override {
        json body = task_of(record);
        body[std::string(result_type_member)] = std::string(task_kind);
        return body;
    }

    json detailed(const TaskRecord& record) const override {
        json body = task_of(record);
        body[std::string(result_type_member)] = std::string(complete_kind);
        switch (record.status) {
            case TaskStatus::input_required: {
                // Every request still outstanding: those asked that have no answer yet.
                json outstanding = json::object();
                if (record.asked && record.asked->is_object()) {
                    for (const auto& item : record.asked->items()) {
                        const bool answered = record.answers && member(*record.answers, item.key()) != nullptr;
                        if (!answered) outstanding[item.key()] = item.value();
                    }
                }
                body[std::string(requests_member)] = std::move(outstanding);
                break;
            }
            case TaskStatus::completed:
                body[std::string(result_member)] = record.result ? *record.result : json::object();
                break;
            case TaskStatus::failed: {
                json error = record.error ? *record.error : json::object();
                const json* text = member(error, error_message_member);
                if (member(error, error_code_member) == nullptr || text == nullptr) {
                    error = json::object();
                    error[std::string(error_code_member)] = error_code::internal_error;
                    error[std::string(error_message_member)] = std::string(unknown_failure);
                    text = member(error, error_message_member);
                }
                if (member(body, message_member) == nullptr) body[std::string(message_member)] = *text;
                body[std::string(error_member)] = std::move(error);
                break;
            }
            case TaskStatus::working:
            case TaskStatus::cancelled: break;
        }
        return body;
    }

    json acknowledgement() const override {
        json body = json::object();
        body[std::string(result_type_member)] = std::string(complete_kind);
        return body;
    }

    json cancelled(const TaskRecord&) const override { return acknowledgement(); }
    bool acknowledges_ended_cancel() const override { return true; }

    // There is no result request: a read inlines what the task ended with.
    std::variant<json, RequestError> payload(const TaskRecord&) const override {
        return RequestError{error_code::method_not_found, "Method not found"};
    }
    json page(const TaskPage&) const override { return json::object(); }
    // The requests of a task are read with the task, not sent.
    void relate(json&, std::string_view) const override {}
    std::optional<FeedMessage> status_notice(const TaskRecord&) const override { return std::nullopt; }

    bool error_result_fails() const override { return false; }
    bool round_keys() const override { return true; }

    json input_entry(std::string method, json params) const override {
        return entry_of(std::move(method), std::move(params));
    }
    std::optional<std::string> input_method(const json& entry) const override { return method_in(entry); }
    json input_params(const json& entry) const override { return params_in(entry); }

    bool task_notifications() const override { return false; }

private:
    // The type of the state of a task in a given status, as a read answers it.
    static std::string_view status_type(TaskStatus status) {
        switch (status) {
            case TaskStatus::working: return "WorkingTask";
            case TaskStatus::input_required: return "InputRequiredTask";
            case TaskStatus::completed: return "CompletedTask";
            case TaskStatus::failed: return "FailedTask";
            case TaskStatus::cancelled: return "CancelledTask";
        }
        return {};
    }

    // The members every state of a task has.
    static json task_of(const TaskRecord& record) {
        json task = json::object();
        task[std::string(task_id_member)] = record.id;
        task[std::string(status_member)] = std::string(to_string(record.status));
        if (record.status_message) task[std::string(message_member)] = *record.status_message;
        task[std::string(created_member)] = iso_8601(record.created);
        task[std::string(updated_member)] = iso_8601(record.updated);
        if (record.ttl) {
            task[std::string(ttl_member)] = record.ttl->count();
        } else {
            task[std::string(ttl_member)] = nullptr;
        }
        task[std::string(poll_member)] = record.poll_interval.count();
        return task;
    }
};

// `augmented_request`: the experimental tasks of the core protocol. The requester asks for a task in the call itself
// (a member `task` of its params, which may ask for a lifetime), of a tool whose listing says it may run as one; a
// call of a tool that never runs as a task that asks for one, and a call of a tool that runs only as one that does
// not, are refused as methods that do not exist. The answer nests the task in the result; a read gives the task's
// state alone, and what it ended with is fetched by a result request, which waits until the task has ended and carries
// the task's requests for input meanwhile (as requests of the server, marked with the task). A cancellation answers
// with the task, and one of a task that has ended is refused. A tool execution error fails the task, and the
// progress of a task's handler reaches the requester, as do the changes of the task's status.
class AugmentedRequest final : public TaskMechanism {
public:
    static constexpr std::string_view task_member = "task";
    static constexpr std::string_view requested_ttl_member = "ttl";
    static constexpr std::string_view get_method = "tasks/get";
    static constexpr std::string_view result_method = "tasks/result";
    static constexpr std::string_view cancel_method = "tasks/cancel";
    static constexpr std::string_view list_method = "tasks/list";
    static constexpr std::string_view status_method = "notifications/tasks/status";
    static constexpr std::string_view call_params_type = "CallToolRequestParams";
    static constexpr std::string_view metadata_type = "TaskMetadata";
    static constexpr std::string_view task_type = "Task";
    static constexpr std::string_view created_type = "CreateTaskResult";
    static constexpr std::string_view get_type = "GetTaskResult";
    static constexpr std::string_view cancel_type = "CancelTaskResult";
    static constexpr std::string_view list_type = "ListTasksResult";
    static constexpr std::string_view tool_type = "Tool";
    static constexpr std::string_view execution_type = "ToolExecution";
    static constexpr std::string_view capabilities_type = "ServerCapabilities";
    static constexpr std::string_view task_capabilities_type = "ServerCapabilities.tasks";
    static constexpr std::string_view task_requests_type = "ServerCapabilities.tasks.requests";
    static constexpr std::string_view task_tools_type = "ServerCapabilities.tasks.requests.tools";
    static constexpr std::string_view tasks_member = "tasks";
    static constexpr std::string_view cancel_member = "cancel";
    static constexpr std::string_view list_member = "list";
    static constexpr std::string_view requests_member = "requests";
    static constexpr std::string_view tools_member = "tools";
    static constexpr std::string_view call_member = "call";
    static constexpr std::string_view execution_member = "execution";
    static constexpr std::string_view task_support_member = "taskSupport";
    static constexpr std::string_view optional_support = "optional";
    static constexpr std::string_view required_support = "required";
    static constexpr std::string_view task_id_member = "taskId";
    static constexpr std::string_view cursor_member = "cursor";
    static constexpr std::string_view next_cursor_member = "nextCursor";
    static constexpr std::string_view status_member = "status";
    static constexpr std::string_view message_member = "statusMessage";
    static constexpr std::string_view created_member = "createdAt";
    static constexpr std::string_view updated_member = "lastUpdatedAt";
    static constexpr std::string_view ttl_member = "ttl";
    static constexpr std::string_view poll_member = "pollInterval";
    static constexpr std::string_view meta_member = "_meta";
    static constexpr std::string_view related_task_key = "io.modelcontextprotocol/related-task";
    static constexpr std::string_view error_code_member = "code";
    static constexpr std::string_view error_message_member = "message";
    static constexpr std::string_view error_data_member = "data";
    // The error of a result request for a task that ended without an outcome: one that was cancelled.
    static constexpr std::string_view no_outcome = "Internal error: the task ended without a result";

    std::optional<std::string> check(const profile::Profile& profile) const override {
        for (const auto method : {get_method, result_method, cancel_method, list_method}) {
            const profile::Message* request = profile.request(method);
            if (request == nullptr || !request->from_client) {
                return "the profile has no client request " + std::string(method);
            }
        }
        const auto lacks = [&](std::string_view name,
                               std::initializer_list<std::string_view> members) -> std::optional<std::string> {
            const profile::Type* type = profile.type(name);
            if (type == nullptr) return "the profile has no type " + std::string(name);
            for (const auto property : members) {
                if (!type->has_property(property)) {
                    return "the profile's " + std::string(name) + " has no member " + std::string(property);
                }
            }
            return std::nullopt;
        };
        std::optional<std::string> problem = lacks(call_params_type, {task_member});
        if (!problem) problem = lacks(metadata_type, {requested_ttl_member});
        for (const auto type : {task_type, get_type, cancel_type}) {
            if (!problem) {
                problem = lacks(type, {task_id_member, status_member, message_member, created_member, updated_member,
                                       ttl_member, poll_member});
            }
        }
        if (!problem) problem = lacks(created_type, {task_member});
        if (!problem) problem = lacks(list_type, {tasks_member, next_cursor_member});
        if (!problem) problem = lacks(tool_type, {execution_member});
        if (!problem) problem = lacks(execution_type, {task_support_member});
        if (!problem) problem = lacks(capabilities_type, {tasks_member});
        if (!problem) problem = lacks(task_capabilities_type, {cancel_member, list_member, requests_member});
        if (!problem) problem = lacks(task_requests_type, {tools_member});
        if (!problem) problem = lacks(task_tools_type, {call_member});
        return problem;
    }
    bool answers_with_tasks() const override { return true; }

    // Any requester may ask about a task it holds the id of; whether a call is answered with one, the call says.
    bool requester_takes_tasks(const ClientIdentity&) const override { return true; }
    bool requests_need_declaration() const override { return false; }

    std::variant<TaskAsk, RequestError> asked(const json& params, const ClientIdentity&) const override {
        TaskAsk opt_in;
        const json* task = member(params, task_member);
        if (task == nullptr) return opt_in;
        if (!task->is_object()) {
            return RequestError{error_code::invalid_params,
                                "Invalid params: params." + std::string(task_member) + " must be an object"};
        }
        opt_in.takes = true;
        const json* ttl = member(*task, requested_ttl_member);
        // No lifetime asked for, or one without a limit, which is never granted: the store's default.
        if (ttl == nullptr || ttl->is_null()) return opt_in;
        constexpr auto longest = (std::numeric_limits<std::chrono::milliseconds::rep>::max)();
        if (ttl->is_number_unsigned()) {
            const auto asked_for = ttl->get<std::uint64_t>();
            opt_in.ttl = std::chrono::milliseconds(
                asked_for > static_cast<std::uint64_t>(longest) ? longest
                                                                : static_cast<std::chrono::milliseconds::rep>(asked_for));
            return opt_in;
        }
        if (ttl->is_number_integer() && ttl->get<std::int64_t>() >= 0) {
            opt_in.ttl = std::chrono::milliseconds(ttl->get<std::int64_t>());
            return opt_in;
        }
        return RequestError{error_code::invalid_params, "Invalid params: params." + std::string(task_member) + "." +
                                                            std::string(requested_ttl_member) +
                                                            " must be a non-negative integer"};
    }

    TaskServing serving(TaskSupport support, bool takes) const override {
        switch (support) {
            case TaskSupport::forbidden: return takes ? TaskServing::method_not_found : TaskServing::ordinary;
            case TaskSupport::optional: return takes ? TaskServing::task : TaskServing::ordinary;
            case TaskSupport::required: return takes ? TaskServing::task : TaskServing::method_not_found;
        }
        return TaskServing::ordinary;
    }

    bool prepares_inputs() const override { return false; }
    json required_capabilities() const override { return json::object(); }

    void declare(json& capabilities, bool lists) const override {
        json tasks = json::object();
        tasks[std::string(cancel_member)] = json::object();
        if (lists) tasks[std::string(list_member)] = json::object();
        tasks[std::string(requests_member)][std::string(tools_member)][std::string(call_member)] = json::object();
        capabilities[std::string(tasks_member)] = std::move(tasks);
    }

    void describe_tool(json& tool, TaskSupport support) const override {
        std::string_view name;
        switch (support) {
            case TaskSupport::forbidden: return;  // the absence says it
            case TaskSupport::optional: name = optional_support; break;
            case TaskSupport::required: name = required_support; break;
        }
        json& execution = tool[std::string(execution_member)];
        if (!execution.is_object()) execution = json::object();
        execution[std::string(task_support_member)] = std::string(name);
    }

    bool serves(std::string_view method) const override {
        return method == get_method || method == result_method || method == cancel_method || method == list_method;
    }

    std::variant<TaskRequest, RequestError> read(const Message& request) const override {
        TaskRequest reading;
        if (request.method == list_method) {
            reading.action = TaskRequest::Action::list;
            if (const json* cursor = member(request.params, cursor_member)) {
                const auto* text = cursor->get_ptr<const std::string*>();
                if (text == nullptr) {
                    return RequestError{error_code::invalid_params,
                                        "Invalid params: params." + std::string(cursor_member) + " must be a string"};
                }
                reading.cursor = *text;
            }
            return reading;
        }
        if (request.method == get_method) {
            reading.action = TaskRequest::Action::get;
        } else if (request.method == result_method) {
            reading.action = TaskRequest::Action::result;
        } else if (request.method == cancel_method) {
            reading.action = TaskRequest::Action::cancel;
        } else {
            return RequestError{error_code::method_not_found, "Method not found: " + request.method};
        }
        // The id in the params names the task; whatever the request's `_meta` says about a task is not read.
        const json* id = member(request.params, task_id_member);
        const auto* text = id == nullptr ? nullptr : id->get_ptr<const std::string*>();
        if (text == nullptr) {
            return RequestError{error_code::invalid_params,
                                "Invalid params: params." + std::string(task_id_member) + " must be a string"};
        }
        reading.task_id = *text;
        return reading;
    }

    json created(const TaskRecord& record) const override {
        json body = json::object();
        body[std::string(task_member)] = task_of(record);
        return body;
    }
    json detailed(const TaskRecord& record) const override { return task_of(record); }
    json acknowledgement() const override { return json::object(); }
    json cancelled(const TaskRecord& record) const override { return task_of(record); }
    bool acknowledges_ended_cancel() const override { return false; }

    // What the call would have been answered with: its result, marked with the task, or its error.
    std::variant<json, RequestError> payload(const TaskRecord& record) const override {
        if (record.result) {
            json body = *record.result;
            if (!body.is_object()) body = json::object();
            json& meta = body[std::string(meta_member)];
            if (!meta.is_object()) meta = json::object();
            json related = json::object();
            related[std::string(task_id_member)] = record.id;
            meta[std::string(related_task_key)] = std::move(related);
            return body;
        }
        if (record.error) {
            const json* code = member(*record.error, error_code_member);
            const json* message = member(*record.error, error_message_member);
            if (code != nullptr && code->is_number_integer() && message != nullptr && message->is_string()) {
                RequestError error;
                error.code = code->get<int>();
                error.message = message->get<std::string>();
                if (const json* data = member(*record.error, error_data_member)) error.data = *data;
                return error;
            }
        }
        return RequestError{error_code::internal_error, std::string(no_outcome)};
    }

    json page(const TaskPage& page) const override {
        json body = json::object();
        json tasks = json::array();
        for (const TaskRecord& record : page.tasks) tasks.push_back(task_of(record));
        body[std::string(tasks_member)] = std::move(tasks);
        if (page.next_cursor) body[std::string(next_cursor_member)] = *page.next_cursor;
        return body;
    }

    void relate(json& params, std::string_view task_id) const override {
        if (!params.is_object()) params = json::object();
        json& meta = params[std::string(meta_member)];
        if (!meta.is_object()) meta = json::object();
        json related = json::object();
        related[std::string(task_id_member)] = std::string(task_id);
        meta[std::string(related_task_key)] = std::move(related);
    }

    std::optional<FeedMessage> status_notice(const TaskRecord& record) const override {
        return FeedMessage{std::string(status_method), task_of(record)};
    }

    bool error_result_fails() const override { return true; }
    // The keys stay internal (the requests go out under the server's request ids), and differ between rounds, so that
    // a stream that sent the requests of one round sends those of the next.
    bool round_keys() const override { return true; }
    json input_entry(std::string method, json params) const override {
        return entry_of(std::move(method), std::move(params));
    }
    std::optional<std::string> input_method(const json& entry) const override { return method_in(entry); }
    json input_params(const json& entry) const override { return params_in(entry); }
    bool task_notifications() const override { return true; }

private:
    static json task_of(const TaskRecord& record) {
        json task = json::object();
        task[std::string(task_id_member)] = record.id;
        task[std::string(status_member)] = std::string(to_string(record.status));
        if (record.status_message) task[std::string(message_member)] = *record.status_message;
        task[std::string(created_member)] = iso_8601(record.created);
        task[std::string(updated_member)] = iso_8601(record.updated);
        if (record.ttl) {
            task[std::string(ttl_member)] = record.ttl->count();
        } else {
            task[std::string(ttl_member)] = nullptr;
        }
        task[std::string(poll_member)] = record.poll_interval.count();
        return task;
    }
};

}  // namespace

std::unique_ptr<TaskMechanism> make_no_tasks() { return std::make_unique<NoTasks>(); }
std::unique_ptr<TaskMechanism> make_server_directed_tasks() { return std::make_unique<ServerDirected>(); }
std::unique_ptr<TaskMechanism> make_augmented_request_tasks() { return std::make_unique<AugmentedRequest>(); }

std::string iso_8601(std::chrono::system_clock::time_point at) {
    const auto at_ms = std::chrono::floor<std::chrono::milliseconds>(at);
    const auto day_start = std::chrono::floor<std::chrono::days>(at_ms);
    const std::chrono::year_month_day calendar{day_start};
    std::int64_t rest = (at_ms - day_start).count();  // milliseconds into the day
    const std::int64_t millis = rest % 1000;
    rest /= 1000;
    const std::int64_t secs = rest % 60;
    rest /= 60;
    const std::int64_t mins = rest % 60;
    const std::int64_t hrs = rest / 60;
    std::string out;
    out.reserve(24);
    append_padded(out, static_cast<int>(calendar.year()), 4);
    out += '-';
    append_padded(out, static_cast<unsigned>(calendar.month()), 2);
    out += '-';
    append_padded(out, static_cast<unsigned>(calendar.day()), 2);
    out += 'T';
    append_padded(out, hrs, 2);
    out += ':';
    append_padded(out, mins, 2);
    out += ':';
    append_padded(out, secs, 2);
    out += '.';
    append_padded(out, millis, 3);
    out += 'Z';
    return out;
}

}  // namespace detail
}  // namespace neomcp
