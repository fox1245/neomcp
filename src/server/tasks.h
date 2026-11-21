// The task runner of a server: the records of the tasks it runs, kept in the store of ServerOptions::tasks, and what
// happens to a task between its start and its end. The server's core runs the handler of a task on its pools, as it
// runs a call, and tells the runner what the handler produced.
//
// Ownership and threads:
// - A task's run belongs to the runner, never to a session: it holds the CallControl of the request that started it
//   only for what the handler's Context reads (the revision, the client, the authorization), and a stop source of
//   its own, which the server's run of the handler follows. Nothing that ends the starting request or its session
//   reaches that stop source.
// - One mutex (mutex_) guards the runs, their fields marked so, and the cache of reads. It is never held while the
//   store is called, while a stop is requested (stop callbacks run inline), or while the server is called.
// - The store's status is the truth. Every change the runner makes is one compare-and-set on it, so of a cancellation
//   and the handler's end only the first counts, and an outcome that comes after the task ended is dropped.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/server.h"

namespace neomcp::detail {

class TaskRunner;

// What one entry of a task's handler ended with.
using TaskOutcome = std::variant<ToolResult, Refusal, HostFailure, InvalidRequest, InputRequired>;

// The answers and the state the handler of a task is entered again with.
struct TaskResume {
    InputResponses responses;
    std::optional<std::string> state;
};

// One task the server runs, from its start until its handler's last entry ended (or, while it waits for answers,
// until it ends otherwise).
struct TaskRun {
    TaskRun(TaskRunner& task_runner, std::string task_id, TaskOwner task_owner, std::size_t tool_index,
            CallRequest call, CallControl call_origin, TaskStart task_start)
        : runner(&task_runner),
          id(std::move(task_id)),
          owner(std::move(task_owner)),
          tool(tool_index),
          request(std::move(call)),
          origin(std::move(call_origin)),
          start(std::move(task_start)) {}

    TaskRunner* runner;
    const std::string id;
    const TaskOwner owner;
    const std::size_t tool;      // the index of the tool in the server's tools
    const CallRequest request;   // the call: name, arguments, _meta
    const CallControl origin;    // of the request that started the task; its stop token is never followed
    const TaskStart start;
    std::stop_source stop;       // the task's own: cancellation, the end of its lifetime, the server's destruction
    std::mutex answering;        // one answer_task of this task at a time

    // Guarded by the runner's mutex.
    bool running = true;  // an entry of the handler is admitted or runs; false while the task waits for answers
    std::size_t rounds = 0;  // rounds of input the handler asked for
    std::optional<InputRequired> asked;  // what it waits for; empty while running
    InputResponses answers;              // the answers of the round so far
    std::chrono::steady_clock::time_point deadline{};  // when the wait for the round's answers ends
};

class TaskRunner {
public:
    // Checks the options and fails every unfinished task of the store (see TaskOptions::store). Throws
    // std::invalid_argument for an option that cannot work.
    explicit TaskRunner(const ServerOptions& options);
    // Throws std::invalid_argument for an option that cannot work; whether or not a store is given.
    static void check(const TaskOptions& options);

    struct Created {
        std::shared_ptr<TaskRun> run;
        TaskRecord record;
    };
    // Makes the record of a task and its run. A store that is full is swept once and asked again.
    std::variant<Created, TaskRefusal> create(std::size_t tool, const CallRequest& request, CallControl origin,
                                              TaskStart start);

    // The handler's entry ended. `stopping`: the server is shutting down, and the task fails whatever the outcome.
    // Returns the answers to enter the handler with again at once, when it asked to be entered again without
    // requests.
    std::optional<TaskResume> settle(const std::shared_ptr<TaskRun>& run, TaskOutcome outcome, bool stopping);
    // The handler could not be entered (no place in the bounds, or the server is shutting down): the task fails.
    void fail_to_run(const std::shared_ptr<TaskRun>& run, bool busy);

    struct Answered {
        AnswersResult result;
        std::shared_ptr<TaskRun> run;      // with `resume`: the task whose handler is to be entered again
        std::optional<TaskResume> resume;  // set once every request of the round has its answer
    };
    Answered answer(std::string_view id, const TaskOwner& owner, json answers, InputResponses responses);

    TaskCancelResult cancel(std::string_view id, const TaskOwner& owner);
    std::optional<TaskRecord> read(std::string_view id, const TaskOwner& owner);
    TaskWatchStart watch(std::string_view id, const TaskOwner& owner, std::weak_ptr<TaskListener> listener);
    // Sets the status message of a running task.
    void status(const TaskRun& run, std::string message);

    // Removes what ended its lifetime, stops the handlers of tasks that are gone or ended, and fails the tasks whose
    // wait for answers lasted longer than the input time-out.
    void sweep();
    // Fails the tasks that wait for answers; the server fails the running ones as their handlers return.
    void shutdown();

    std::chrono::milliseconds sweep_interval() const noexcept { return options_.sweep_interval; }

private:
    using Clock = std::chrono::steady_clock;

    struct Cached {
        TaskOwner owner;
        TaskRecord record;
        Clock::time_point read{};
    };

    std::chrono::system_clock::time_point now() const;
    // Ends the task with `update` if it is still working or waiting for input, and forgets its run.
    void end(const std::shared_ptr<TaskRun>& run, TaskUpdate update);
    std::optional<TaskResume> ask(const std::shared_ptr<TaskRun>& run, InputRequired asked);
    void forget(const std::string& id);  // with mutex_ held
    TaskUpdate failure(int code, std::string_view message, json data = nullptr) const;
    TaskUpdate host_failure(const HostFailure& failure) const;

    const TaskOptions options_;
    const bool expose_error_details_;
    const std::size_t max_input_rounds_;
    const std::chrono::milliseconds input_timeout_;
    // Shared with the watches handed out, which may outlive the runner.
    const std::shared_ptr<std::atomic<std::size_t>> waiting_ = std::make_shared<std::atomic<std::size_t>>(0);

    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<TaskRun>, std::less<>> runs_;
    std::map<std::string, Cached, std::less<>> cache_;
};

}  // namespace neomcp::detail
