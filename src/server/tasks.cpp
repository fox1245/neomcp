// The task runner (see tasks.h).
#include "server/tasks.h"

#include <iterator>
#include <stdexcept>

#include "core/application_error.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/wire.h"

namespace neomcp {

namespace {

// The texts of the errors a task fails with. They say what happened and nothing of the server's internals; the text
// behind a failure of the server program is added only when ServerOptions::expose_error_details is on.
constexpr std::string_view tool_failed_text = "Internal error: the tool call failed";
constexpr std::string_view busy_text = "Internal error: the server is busy";
constexpr std::string_view busy_reason = "busy";  // the reason in the data of a busy refusal, as for a call
constexpr std::string_view shut_down_text = "Internal error: the server shut down before the task ended";
constexpr std::string_view restarted_text = "Internal error: the server restarted before the task ended";
constexpr std::string_view unanswered_text = "Internal error: the client did not answer the task's requests in time";
constexpr std::string_view cannot_ask_text = "Internal error: the task asked for input that cannot be asked for";
constexpr std::string_view too_many_rounds_text = "Internal error: the task asked for input too many times";
constexpr std::string_view invalid_params_prefix = "Invalid params: ";
constexpr std::string_view too_large_text = "Internal error: the outcome of the task is too large to keep";

json error_object(int code, std::string_view message, json data) {
    json error = json::object();
    error["code"] = code;
    error["message"] = std::string(message);
    if (!data.is_null()) error["data"] = std::move(data);
    return error;
}

// The record of what a task runs: the call as the session handed it.
json recorded_call(const CallRequest& request) {
    json call = json::object();
    call["name"] = request.name;
    call["arguments"] = request.arguments;
    if (!request.meta.is_null()) call["_meta"] = request.meta;
    return call;
}

}  // namespace

std::optional<std::string_view> Context::task_id() const noexcept {
    if (task_ == nullptr) return std::nullopt;
    return std::string_view(task_->id);
}

void Context::task_status(std::string message) const {
    if (task_ != nullptr) task_->runner->status(*task_, std::move(message));
}

namespace detail {

namespace {

// The statuses a change of the runner expects the task to be in.
constexpr TaskStatus unfinished[] = {TaskStatus::working, TaskStatus::input_required};
constexpr TaskStatus working_only[] = {TaskStatus::working};
constexpr TaskStatus waiting_only[] = {TaskStatus::input_required};

}  // namespace

void TaskRunner::check(const TaskOptions& options) {
    if (options.min_poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("neomcp: TaskOptions::min_poll_interval is not positive");
    }
    if (options.max_waiting_results == 0) throw std::invalid_argument("neomcp: TaskOptions::max_waiting_results is zero");
    if (options.sweep_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("neomcp: TaskOptions::sweep_interval is not positive");
    }
}

TaskRunner::TaskRunner(const ServerOptions& options)
    : options_(options.tasks),
      expose_error_details_(options.expose_error_details),
      max_input_rounds_(options.max_input_rounds),
      input_timeout_(options.input_timeout) {
    check(options_);
    if (!options_.store) return;
    // The code that ran an unfinished task died with the process that ran it.
    TaskUpdate restarted = failure(error_code::internal_error, restarted_text);
    try {
        options_.store->recover(restarted);
    } catch (const std::invalid_argument&) {
        // The store keeps no room for an outcome (its reserve is smaller than the error): the status alone.
        TaskUpdate bare;
        bare.status = TaskStatus::failed;
        options_.store->recover(bare);
    }
}

std::chrono::system_clock::time_point TaskRunner::now() const {
    return options_.clock ? options_.clock() : std::chrono::system_clock::now();
}

TaskUpdate TaskRunner::failure(int code, std::string_view message, json data) const {
    TaskUpdate update;
    update.status = TaskStatus::failed;
    update.error = error_object(code, message, std::move(data));
    update.message = std::string(message);
    return update;
}

TaskUpdate TaskRunner::host_failure(const HostFailure& failed) const {
    std::string message(tool_failed_text);
    if (expose_error_details_ && !failed.message.empty()) message += ": " + failed.message;
    return failure(error_code::internal_error, message);
}

void TaskRunner::forget(const std::string& id) {
    runs_.erase(id);
    cache_.erase(id);
}

void TaskRunner::told(const TaskRun& run, const Transition& change) {
    if (change.outcome == TransitionOutcome::applied) told(run, change.record);
}

void TaskRunner::told(const TaskRun& run, const std::optional<TaskRecord>& record) {
    if (!run.start.on_change || !record) return;
    try {
        run.start.on_change(*record);
    } catch (...) {
        // Telling of a change is a courtesy; the change stands.
    }
}

std::variant<TaskRunner::Created, TaskRefusal> TaskRunner::create(std::size_t tool, const CallRequest& request,
                                                                  CallControl origin, TaskStart start) {
    TaskDraft draft;
    draft.owner = task_owner_of(origin.authorization());
    draft.ttl = start.ttl;
    draft.revision = origin.profile().revision();
    draft.request = recorded_call(request);
    const TaskDraft again = draft;
    CreateResult made = options_.store->create(std::move(draft));
    if (const auto* refused = std::get_if<CreateRefusal>(&made); refused != nullptr && *refused != CreateRefusal::id_unavailable) {
        // Room may be held by records whose lifetime ended and that the store has not removed yet.
        static_cast<void>(options_.store->expire(now()));
        made = options_.store->create(again);
    }
    if (const auto* refused = std::get_if<CreateRefusal>(&made)) {
        return *refused == CreateRefusal::id_unavailable ? TaskRefusal::no_id : TaskRefusal::busy;
    }
    TaskRecord record = std::get<TaskRecord>(std::move(made));
    CallRequest call;
    call.name = request.name;
    call.arguments = request.arguments;
    call.meta = request.meta;
    call.arguments.materialize();
    call.meta.materialize();
    auto run = std::make_shared<TaskRun>(*this, record.id, record.owner, tool, std::move(call), std::move(origin),
                                         std::move(start));
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        runs_.emplace(run->id, run);
    }
    return Created{std::move(run), std::move(record)};
}

void TaskRunner::end(const std::shared_ptr<TaskRun>& run, TaskUpdate update) {
    Transition ended = options_.store->transition(run->id, unfinished, std::move(update));
    if (ended.outcome == TransitionOutcome::too_large || ended.outcome == TransitionOutcome::over_budget) {
        // Small enough to fit the reserve the store keeps for a running task.
        ended = options_.store->transition(run->id, unfinished, failure(error_code::internal_error, too_large_text));
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        forget(run->id);
    }
    told(*run, ended);
}

std::optional<TaskResume> TaskRunner::settle(const std::shared_ptr<TaskRun>& run, TaskOutcome outcome, bool stopping) {
    if (stopping) {
        end(run, failure(error_code::internal_error, shut_down_text));
        return std::nullopt;
    }
    if (auto* asked = std::get_if<InputRequired>(&outcome)) return ask(run, std::move(*asked));
    if (const auto* result = std::get_if<ToolResult>(&outcome)) {
        TaskUpdate update;
        update.status =
            result->is_error && run->start.error_result_fails ? TaskStatus::failed : TaskStatus::completed;
        update.result =
            run->start.write_result ? run->start.write_result(*result) : to_wire(*result, run->origin.profile());
        // Only a running task takes its outcome: one that was cancelled, or whose lifetime ended, stays as it is.
        Transition ended = options_.store->transition(run->id, working_only, std::move(update));
        if (ended.outcome == TransitionOutcome::too_large || ended.outcome == TransitionOutcome::over_budget) {
            ended = options_.store->transition(run->id, working_only, failure(error_code::internal_error, too_large_text));
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            forget(run->id);
        }
        told(*run, ended);
        return std::nullopt;
    }
    TaskUpdate failed;
    if (std::holds_alternative<Refusal>(outcome)) {
        failed = failure(error_code::internal_error, busy_text, json{{"reason", std::string(busy_reason)}});
    } else if (const auto* host = std::get_if<HostFailure>(&outcome)) {
        failed = host_failure(*host);
    } else if (const auto* application = std::get_if<ApplicationFailure>(&outcome)) {
        // The code under the revision that governs the task's call, as for a call answered at once.
        std::string refused = refused_application_code(run->origin.profile(), application->code);
        failed = refused.empty() ? failure(application->code, application->message, application->data)
                                 : host_failure(HostFailure{std::move(refused)});
    } else {
        failed = failure(error_code::invalid_params,
                         std::string(invalid_params_prefix) + std::get<InvalidRequest>(outcome).message);
    }
    Transition ended = options_.store->transition(run->id, working_only, std::move(failed));
    if (ended.outcome == TransitionOutcome::too_large || ended.outcome == TransitionOutcome::over_budget) {
        // An error of the application's own that the store cannot keep: the task fails without it.
        ended = options_.store->transition(run->id, working_only, failure(error_code::internal_error, too_large_text));
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        forget(run->id);
    }
    told(*run, ended);
    return std::nullopt;
}

std::optional<TaskResume> TaskRunner::ask(const std::shared_ptr<TaskRun>& run, InputRequired asked) {
    bool too_many = false;
    std::size_t round = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        too_many = run->rounds >= max_input_rounds_;
        if (!too_many) round = ++run->rounds;
    }
    if (too_many) {
        end(run, failure(error_code::internal_error, too_many_rounds_text));
        return std::nullopt;
    }
    // Asking for nothing asks to be entered again, with the state.
    if (asked.requests.empty()) return TaskResume{InputResponses{}, std::move(asked.state)};

    json written = json::object();
    std::map<std::string, std::string, std::less<>> keys;  // the record's key -> the handler's
    for (const auto& [key, request] : asked.requests) {
        std::optional<json> one = run->start.write_input ? run->start.write_input(request) : std::nullopt;
        if (!one) {
            end(run, failure(error_code::internal_error, cannot_ask_text));
            return std::nullopt;
        }
        std::string kept = run->start.round_keys ? std::to_string(round) + "." + key : key;
        written[kept] = std::move(*one);
        keys.emplace(std::move(kept), key);
    }
    // The run waits before the record says so, so that a cancellation that comes in between finds it waiting.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        run->asked = std::move(asked);
        run->keys = std::move(keys);
        run->answers.clear();
        run->deadline = Clock::now() + input_timeout_;
        run->running = false;
        cache_.erase(run->id);
    }
    TaskUpdate waiting;
    waiting.status = TaskStatus::input_required;
    waiting.asked = std::move(written);
    const Transition moved = options_.store->transition(run->id, working_only, std::move(waiting));
    if (moved.outcome == TransitionOutcome::too_large || moved.outcome == TransitionOutcome::over_budget) {
        end(run, failure(error_code::internal_error, too_large_text));
    } else if (moved.outcome != TransitionOutcome::applied) {
        // Cancelled, or its lifetime ended, meanwhile.
        const std::lock_guard<std::mutex> lock(mutex_);
        forget(run->id);
    } else {
        told(*run, moved);
    }
    return std::nullopt;
}

void TaskRunner::fail_to_run(const std::shared_ptr<TaskRun>& run, bool busy) {
    end(run, busy ? failure(error_code::internal_error, busy_text, json{{"reason", std::string(busy_reason)}})
                  : failure(error_code::internal_error, shut_down_text));
}

TaskRunner::Answered TaskRunner::answer(std::string_view id, const TaskOwner& owner, json answers,
                                        InputResponses responses) {
    Answered answered;
    std::shared_ptr<TaskRun> run;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = runs_.find(id);
        if (found != runs_.end() && found->second->owner == owner) run = found->second;
    }
    if (!run) {
        // A task this server does not run (or that is not this owner's): the store answers for it.
        answered.result = options_.store->put_answers(id, owner, std::move(answers));
        return answered;
    }
    const std::lock_guard<std::mutex> one_at_a_time(run->answering);
    std::vector<std::string> keys;
    if (answers.is_object()) {
        for (const auto& item : answers.items()) keys.push_back(item.key());
    }
    answered.result = options_.store->put_answers(id, owner, std::move(answers));
    if (answered.result.outcome != AnswersOutcome::applied) return answered;
    {
        // The store kept the answers for keys that were asked and had none yet; the run keeps the same ones.
        const std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(run->id);
        if (run->asked) {
            for (const std::string& key : keys) {
                const auto response = responses.find(key);
                const auto own = run->keys.find(key);
                if (response == responses.end() || own == run->keys.end()) continue;
                if (run->asked->requests.count(own->second) == 0 || run->answers.count(own->second) != 0) continue;
                run->answers.emplace(own->second, std::move(response->second));
            }
        }
    }
    if (!options_.store->take_answers(id)) return answered;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!run->asked) return answered;
        answered.resume = TaskResume{std::move(run->answers), std::move(run->asked->state)};
        run->answers.clear();
        run->asked.reset();
        run->keys.clear();
        run->running = true;
    }
    if (run->start.on_change) told(*run, options_.store->get(id, owner));
    answered.run = std::move(run);
    return answered;
}

TaskCancelResult TaskRunner::cancel(std::string_view id, const TaskOwner& owner) {
    TaskCancelResult result;
    // The owner is checked by reading; a transition is not bound to an owner, and the owner of a task never changes.
    if (!options_.store->get(id, owner)) return result;
    TaskUpdate cancelled;
    cancelled.status = TaskStatus::cancelled;
    Transition moved = options_.store->transition(id, unfinished, std::move(cancelled));
    result.record = std::move(moved.record);
    if (moved.outcome == TransitionOutcome::status_mismatch) {
        result.outcome = TaskCancelOutcome::ended;
        return result;
    }
    if (moved.outcome != TransitionOutcome::applied) {
        result.record.reset();
        return result;
    }
    result.outcome = TaskCancelOutcome::cancelled;
    std::shared_ptr<TaskRun> run;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = runs_.find(id);
        if (found != runs_.end()) {
            run = found->second;
            // A task that waits for answers has no handler to wait for.
            if (!run->running) forget(run->id);
            cache_.erase(std::string(id));
        }
    }
    // Without a lock: stop callbacks run inline.
    if (run) {
        run->stop.request_stop();
        told(*run, result.record);
    }
    return result;
}

std::optional<TaskRecord> TaskRunner::read(std::string_view id, const TaskOwner& owner) {
    const Clock::time_point at = Clock::now();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto cached = cache_.find(id);
        if (cached != cache_.end() && cached->second.owner == owner && at - cached->second.read < options_.min_poll_interval) {
            return cached->second.record;
        }
    }
    std::optional<TaskRecord> record = options_.store->get(id, owner);
    if (!record) return record;
    const std::lock_guard<std::mutex> lock(mutex_);
    cache_.insert_or_assign(std::string(id), Cached{owner, *record, at});
    return record;
}

std::optional<TaskRecord> TaskRunner::read_now(std::string_view id, const TaskOwner& owner) const {
    return options_.store->get(id, owner);
}

std::optional<TaskPage> TaskRunner::list(const TaskOwner& owner, std::optional<std::string_view> cursor) const {
    // Zero: as many as the store puts in a page.
    return options_.store->list(owner, cursor, 0);
}

TaskWatchStart TaskRunner::watch(std::string_view id, const TaskOwner& owner, std::weak_ptr<TaskListener> listener) {
    TaskWatchStart start;
    if (waiting_->fetch_add(1) >= options_.max_waiting_results) {
        waiting_->fetch_sub(1);
        start.outcome = WatchOutcome::limit;
        return start;
    }
    start = options_.store->watch(id, owner, std::move(listener));
    if (start.outcome != WatchOutcome::watching) {
        waiting_->fetch_sub(1);
        return start;
    }
    // The place is given back when the watch ends, also after the runner is gone.
    auto inner = std::make_shared<TaskWatch>(std::move(start.watch));
    start.watch = TaskWatch([inner, count = waiting_] {
        inner->release();
        count->fetch_sub(1);
    });
    return start;
}

void TaskRunner::status(const TaskRun& run, std::string message) {
    TaskUpdate update;
    update.status = TaskStatus::working;
    update.message = std::move(message);
    const Transition changed = options_.store->transition(run.id, working_only, std::move(update));
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(run.id);
    }
    told(run, changed);
}

void TaskRunner::sweep() {
    static_cast<void>(options_.store->expire(now()));
    const Clock::time_point at = Clock::now();
    std::vector<std::shared_ptr<TaskRun>> runs;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            it = at - it->second.read < options_.min_poll_interval ? std::next(it) : cache_.erase(it);
        }
        runs.reserve(runs_.size());
        for (const auto& entry : runs_) runs.push_back(entry.second);
    }
    for (const auto& run : runs) {
        const std::optional<TaskRecord> record = options_.store->get(run->id, run->owner);
        if (!record || is_terminal(record->status)) {
            // Its lifetime ended, or it ended elsewhere: the handler has nothing more to do.
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (!run->running) forget(run->id);
            }
            run->stop.request_stop();
            continue;
        }
        bool overdue = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            overdue = !run->running && run->asked && !(at < run->deadline);
        }
        if (!overdue) continue;
        const Transition failed =
            options_.store->transition(run->id, waiting_only, failure(error_code::internal_error, unanswered_text));
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            forget(run->id);
        }
        told(*run, failed);
    }
}

void TaskRunner::shutdown() {
    std::vector<std::shared_ptr<TaskRun>> waiting;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : runs_) {
            if (!entry.second->running) waiting.push_back(entry.second);
        }
    }
    for (const auto& run : waiting) end(run, failure(error_code::internal_error, shut_down_text));
}

}  // namespace detail
}  // namespace neomcp
