// The built-in mechanisms of the slot `tasks`, and what the core keeps of tasks itself: the spelling of a status and
// the owner of a request's tasks. The protocol identifiers a mechanism works with are constants of that mechanism;
// which types and members exist is read from the profile (check).
#include "core/tasks.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <initializer_list>
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

// `none`, and `augmented_request` while no task is served under it: every call is an ordinary one.
class NoTasks final : public TaskMechanism {
public:
    std::optional<std::string> check(const profile::Profile&) const override { return std::nullopt; }
    bool answers_with_tasks() const override { return false; }
    bool requester_takes_tasks(const ClientIdentity&) const override { return false; }
    json required_capabilities() const override { return json::object(); }
    void declare(json&) const override {}
    bool serves(std::string_view) const override { return false; }
    std::variant<TaskRequest, RequestError> read(const Message&) const override {
        return RequestError{error_code::internal_error, "tasks do not apply to this mechanism"};
    }
    json created(const TaskRecord&) const override { return json::object(); }
    json detailed(const TaskRecord&) const override { return json::object(); }
    json acknowledgement() const override { return json::object(); }
    bool acknowledges_ended_cancel() const override { return false; }
    bool error_result_fails() const override { return false; }
    bool round_keys() const override { return false; }
    json input_entry(std::string, json) const override { return json(); }
    std::optional<std::string> input_method(const json&) const override { return std::nullopt; }
    bool task_notifications() const override { return false; }
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
    static constexpr std::string_view method_member = "method";
    static constexpr std::string_view params_member = "params";
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

    json required_capabilities() const override {
        json required = json::object();
        required[std::string(extensions_member)][std::string(extension)] = json::object();
        return required;
    }

    void declare(json& capabilities) const override {
        json& extensions = capabilities[std::string(extensions_member)];
        if (!extensions.is_object()) extensions = json::object();
        extensions[std::string(extension)] = json::object();
    }

    bool serves(std::string_view method) const override {
        return method == get_method || method == update_method || method == cancel_method;
    }

    std::variant<TaskRequest, RequestError> read(const Message& request) const override {
        TaskRequest asked;
        if (request.method == get_method) {
            asked.action = TaskRequest::Action::get;
        } else if (request.method == update_method) {
            asked.action = TaskRequest::Action::update;
        } else if (request.method == cancel_method) {
            asked.action = TaskRequest::Action::cancel;
        } else {
            return RequestError{error_code::method_not_found, "Method not found: " + request.method};
        }
        const json* id = member(request.params, task_id_member);
        const auto* text = id == nullptr ? nullptr : id->get_ptr<const std::string*>();
        if (text == nullptr) {
            return RequestError{error_code::invalid_params,
                                "Invalid params: params." + std::string(task_id_member) + " must be a string"};
        }
        asked.task_id = *text;
        if (asked.action == TaskRequest::Action::update) {
            const json* responses = member(request.params, responses_member);
            if (responses == nullptr || !responses->is_object()) {
                return RequestError{error_code::invalid_params,
                                    "Invalid params: params." + std::string(responses_member) + " must be an object"};
            }
            asked.responses = *responses;
        }
        return asked;
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

    bool acknowledges_ended_cancel() const override { return true; }
    bool error_result_fails() const override { return false; }
    bool round_keys() const override { return true; }

    json input_entry(std::string method, json params) const override {
        json entry = json::object();
        entry[std::string(method_member)] = std::move(method);
        if (!params.is_null()) entry[std::string(params_member)] = std::move(params);
        return entry;
    }

    std::optional<std::string> input_method(const json& entry) const override {
        const json* method = member(entry, method_member);
        const auto* text = method == nullptr ? nullptr : method->get_ptr<const std::string*>();
        if (text == nullptr) return std::nullopt;
        return *text;
    }

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

}  // namespace

std::unique_ptr<TaskMechanism> make_no_tasks() { return std::make_unique<NoTasks>(); }
std::unique_ptr<TaskMechanism> make_server_directed_tasks() { return std::make_unique<ServerDirected>(); }

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
