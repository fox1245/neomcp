// Tasks: a tool call answered at once with the id of a task, whose handler runs on after the request (and its session)
// ended, and which the requester then asks about by that id.
//
// A host that runs tasks offers them through ToolHost::task_host (neomcp::Server does, given a task store and a tool
// that declares it may run as one). A session reaches the tasks of its host through that interface only; what tasks
// look like on the wire, and whether a revision has them at all, is the profile's `tasks` mechanism's
// (neomcp/mechanism.h), so the host never sees a protocol message of tasks.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>

#include "neomcp/authorization.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/server/task_store.h"
#include "neomcp/tool_host.h"

namespace neomcp {

// TaskSupport is declared in neomcp/model.h (which this header includes).

// How a task is started: what the requester asked for, and how the revision it is served under writes the task's
// record.
struct TaskStart {
    // The lifetime asked for; empty: the store's default.
    std::optional<std::chrono::milliseconds> ttl{};
    // Whether a result marked as an error (a tool execution error) ends the task failed; otherwise it ends it
    // completed, with that result. An error that is not a result (the call could not run) always fails it.
    bool error_result_fails = false;
    // Writes the handler's result as the task's record keeps it: what the call would have been answered with. Empty:
    // the result as the revision's CallToolResult type has it, without what the protocol engine adds to it.
    std::function<json(const ToolResult&)> write_result{};
    // Writes one request for input as the task's record keeps it (the form the revision carries it in). Empty, or
    // empty for a request: the task cannot ask, and a handler that asks fails it.
    std::function<std::optional<json>(const InputRequest&)> write_input{};
    // Whether the record keys each request for input by the number of its round, a dot and the handler's key
    // ("2.confirm"), so that no key is used twice over the task's life although a handler that is entered again from
    // its start may ask by a key it used before. The answers come back by those keys and reach the handler by its own.
    bool round_keys = false;
    // Told of every change of the task's status, or of its status message, that the server makes after the task
    // started, with the task as it is after the change, on the thread that made it; not of its removal at the end of
    // its lifetime. Empty: nothing is told. It must not block for long; what it throws is dropped.
    std::function<void(const TaskRecord&)> on_change{};
};

// Why a task was not started. No record exists for a task that was refused, except one whose start the server's
// shutdown overtook, or whose coroutine could not be started, after the record was made: that one is failed.
enum class TaskRefusal {
    off,            // the server has no task store
    unknown_tool,   // no tool of that name is on
    not_declared,   // the tool does not declare that it may run as a task
    not_permitted,  // the call's authorization lacks a scope the tool needs
    busy,           // no capacity: the pools of handlers, or the store's bounds
    no_id,          // the store could not draw an id
    shut_down       // the server is shutting down
};

using TaskStartResult = std::variant<TaskRecord, TaskRefusal>;

// What a call needs before it can be started as a task: nothing (it is ready); the answers to inputs the tool
// declares (neomcp/server/inputs.h), which are asked as for an ordinary call, before any task exists; or nothing more,
// because the answers it came with cannot be used.
using TaskPreparation = std::variant<std::monostate, InputRequired, InvalidRequest>;

enum class TaskCancelOutcome {
    cancelled,  // the task is now cancelled, and its handler was asked to stop; `record` is the task
    ended,      // the task had ended already, and stays as it ended; `record` is the task
    not_found   // no such task for that owner (or it expired)
};

struct TaskCancelResult {
    TaskCancelOutcome outcome = TaskCancelOutcome::not_found;
    std::optional<TaskRecord> record;
};

// The owner of the tasks a request with `authorization` starts and may see: its principal, or the empty owner when it
// carries none.
TaskOwner task_owner_of(const Authorization* authorization);

// The tasks of a host. Every call is thread-safe and may come from any thread; none of them blocks for the duration of
// a handler.
class TaskHost {
public:
    virtual ~TaskHost() = default;

    // Whether the tool at `index` of the host's tools() may run as a task.
    virtual TaskSupport task_support(std::size_t index) const = 0;
    // What the call `request` needs before it is started as a task (see TaskPreparation), decided from the answers and
    // the state it carries. The answers to inputs the tool declares are asked for by the call itself, as the revision
    // asks for a call's input; a task is started only once every one of them has its outcome.
    virtual TaskPreparation prepare_task(const CallRequest& request, const CallControl& origin) = 0;
    // Starts the call as a task, for the owner that `origin`'s authorization gives (task_owner_of). The record exists
    // when this returns. The handler's first entry gets the answers and the state `request` carries.
    virtual TaskStartResult start_task(const CallRequest& request, CallControl origin, TaskStart start) = 0;
    // The task, when it exists for `owner`. A host may answer a read that comes soon after another one of the same task
    // with what it read then.
    virtual std::optional<TaskRecord> task(std::string_view id, const TaskOwner& owner) = 0;
    // The task as it is at this moment, never an earlier read: for a waiter that a change of the task woke.
    virtual std::optional<TaskRecord> current_task(std::string_view id, const TaskOwner& owner) = 0;
    // The tasks of `owner`, oldest first, a page at a time from `cursor` (empty: from the first), as many as the store
    // gives in a page (TaskStore::list). Empty when the cursor is not one the store gave.
    virtual std::optional<TaskPage> list_tasks(const TaskOwner& owner, std::optional<std::string_view> cursor) = 0;
    // Cancels the task: cancelled before this returns, and its handler asked to stop. A task that ended stays as it
    // ended.
    virtual TaskCancelResult cancel_task(std::string_view id, const TaskOwner& owner) = 0;
    // The answers to the requests of a task waiting for input: `answers` as the record keeps them (an object by the
    // record's keys), and the same answers read (`responses`, by the same keys). Answers for keys that were not asked,
    // or were answered already, are ignored; once every request has its answer, the handler is entered again.
    virtual AnswersResult answer_task(std::string_view id, const TaskOwner& owner, json answers,
                                      InputResponses responses) = 0;
    // Watches the task (see TaskStore::watch).
    virtual TaskWatchStart watch_task(std::string_view id, const TaskOwner& owner,
                                      std::weak_ptr<TaskListener> listener) = 0;
};

}  // namespace neomcp
