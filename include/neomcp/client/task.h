// Tasks, as a client sees them: a tool call that the server answers at once with a task, runs on after that request,
// and tells about when the client asks.
//
// A client takes tasks only when the application says so (ClientOptions::tasks.accept): then every request declares
// that the client takes them, under each revision that has tasks of this kind (2026-07-28 served with the extension
// io.modelcontextprotocol/tasks), and the server decides, per tool call, whether to answer with a task. Without it no
// server may answer with one, and a result that says it did is malformed_answer.
//
// Client::call_tool and the typed calls keep their contract: when the server answers with a task, they follow it
// (reading it after each wait the server suggests, answering what it asks through the client's input handlers) and
// return the tool's result once it ended; their options' stop cancels the task as well. An application that wants the
// task itself starts the call with Client::start_tool:
//
//   auto started = client->start_tool("export", json{{"what", "everything"}});
//   if (!started) { /* started.error() */ }
//   if (auto* handle = std::get_if<neomcp::TaskHandle>(&*started)) {
//       save(handle->id());              // to follow it after a restart: client->task(id)
//       auto result = handle->wait();   // the tool's result once the task ended
//   }
//
// The bounds of following a task (the waits between reads, the longest a task is followed, the reads in all, the
// length of an id) are ClientTaskOptions (neomcp/client/engine.h). A task the server no longer knows (it never had
// it, or it restarted, or the task's lifetime ended) ends the call with task_lost, and the tool call is never sent
// again. Every error of a call about a task carries the task's id (ClientError::task).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <asio/awaitable.hpp>

#include "neomcp/client/calls.h"
#include "neomcp/client/mechanism.h"
#include "neomcp/client/options.h"
#include "neomcp/client/result.h"

namespace neomcp {

namespace detail {
class ClientCore;
struct TaskAccess;
}  // namespace detail

// One task of the server, by its id. A handle holds the client's state, as a Subscription does: a handle that outlives
// its Client answers every call with `closed`. Copies name the same task. Every method may be called from any thread;
// a blocking one called on the client's own thread returns on_client_thread.
class TaskHandle {
public:
    // The task's id, as the server gave it.
    const std::string& id() const noexcept { return state_.id; }
    // The task's state when the handle was made: when the server created it (start_tool), or as Client::task read it.
    const TaskState& state() const noexcept { return state_; }
    // The tool the task runs, when the handle comes from start_tool; empty from Client::task. The result wait() gives
    // is checked against the output schema of this tool's definition, as a result of call_tool is.
    const std::optional<std::string>& tool() const noexcept { return tool_; }

    // Reads the task once: its state now. Answers nothing it asks for (wait() does).
    Result<TaskState> status(RequestOptions options = {}) const;
    asio::awaitable<Result<TaskState>> async_status(RequestOptions options = {}) const;

    // Follows the task until it ended, as call_tool does, and gives the tool's result: a result with `is_error` is a
    // value, a task that failed is the server's error (server_error), one cancelled elsewhere task_cancelled. The
    // requests the task waits on go to the client's input handlers. `options.timeout` bounds each read; stop (or
    // cancelling the awaiter) ends the wait `cancelled` and leaves the task running: cancel() cancels it.
    Result<ToolCall> wait(RequestOptions options = {}) const;
    asio::awaitable<Result<ToolCall>> async_wait(RequestOptions options = {}) const;

    // Asks the server to cancel the task, and returns once the server acknowledged that. Cancelling is cooperative:
    // the task may still end otherwise, when it ended first.
    Result<void> cancel(RequestOptions options = {}) const;
    asio::awaitable<Result<void>> async_cancel(RequestOptions options = {}) const;

private:
    friend struct detail::TaskAccess;
    TaskHandle(std::shared_ptr<detail::ClientCore> core, TaskState state, std::optional<std::string> tool) noexcept;

    std::shared_ptr<detail::ClientCore> core_;
    TaskState state_;
    std::optional<std::string> tool_;
};

// What Client::start_tool gives: the tool's result, when the server answered the call at once, or the task it
// answered with.
using ToolStart = std::variant<ToolCall, TaskHandle>;

}  // namespace neomcp
