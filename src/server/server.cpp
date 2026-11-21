// The server: registration, the worker pool, the coroutine thread, and the run of one call.
//
// Threads and ownership:
// - Server owns a shared ServerCore, which is the ToolHost of every session connect() opens; each such session
//   holds a reference to the core in its options, so a session that outlives the Server still has its tools.
// - The core owns max_concurrent_calls worker threads for plain-function handlers and one thread that runs an
//   asio::io_context for coroutine handlers. The io_context is the server's own so that coroutine handlers run
//   without a transport; a later transport may supply its executor instead.
// - One mutex (mutex_) guards the queue of calls waiting for a worker, the count of running ones, the count of
//   coroutine calls, the calls in flight and the stopping flag. It is never held while a handler runs or while a
//   call is completed.
// - A coroutine call is counted from its admission until it leaves the calls in flight, which happens once, in the
//   first finish() of the call, however it ends; so the count is given back exactly once.
// - Every call has its own stop source. A stop callback forwards the session's stop (cancellation or close) to it,
//   and the destructor requests it directly. For a coroutine handler a second callback posts a terminal
//   cancellation to the io thread, which cancels the asynchronous operation the coroutine awaits.
// - The destructor sets the stopping flag (so nothing new is admitted), requests stop on every call in flight
//   and then answers the calls still queued, both while holding no lock, joins the workers, cancels the input timers, lets the
//   io_context run out of work and joins its thread. It cannot deadlock on a handler that waits for its stop token:
//   the stop is requested before any join, from a thread that holds nothing a handler or a stop callback needs.
// - A call that asked for input is entered again as a new call of the same thing; nothing of the earlier run is
//   kept. While a session waits for a client's answers (InputWait), one asio::steady_timer per waiting call runs on
//   the io thread: when it fires first the wait expires, and when the wait ends first a stop callback on the wait's
//   token posts the timer's cancellation. The timers are found through timers_ (io_mutex_), which shutdown() uses to
//   cancel them all, so that the io_context runs out of work.
// - What a client sees can change while the server serves: every registered item has an atomic switch (in a deque,
//   which never moves its elements), and a switch that changes a list reports the change. Changes go to the sessions
//   through the feed: the listeners the sessions registered, the changes not yet told (each kind of change, and each
//   updated URI, once), and one thread that tells them, so that the thread that reports never waits for a client. The
//   feed's mutex (feed_mutex_) is taken alone, never together with mutex_ or io_mutex_, and is not held while a
//   listener is called or the watcher callback runs. shutdown() closes the feed first: the thread tells what is still
//   pending, then tells every listener that the feed ended (each session ends its subscriptions gracefully), and
//   leaves.
// - Tasks (when ServerOptions::tasks has a store): the runner (tasks.h) keeps the records, and a task's handler runs
//   here as a call does, in a CallRecord that has no Completion and follows the task's stop source instead of the
//   session's. A task takes its place in the pools before its record is made (reserved_ for the workers, coroutines_
//   for the io thread), so that a task that would be refused as busy leaves no record. finish() hands the outcome to
//   the runner. A timer on the io thread sweeps the runner every TaskOptions::sweep_interval; shutdown() cancels it
//   with the input timers.
#include "neomcp/server.h"

#include "neomcp/argument_headers.h"
#include "neomcp/feature_host.h"
#include "neomcp/uri_template.h"

#include "server/tasks.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

namespace neomcp {
namespace detail {

namespace {

// The object schema MCP requires of `inputSchema` and `outputSchema`: an object whose `type` is "object".
bool is_object_schema(const json& schema) {
    const auto* members = schema.get_ptr<const json::object_t*>();
    if (members == nullptr) return false;
    const auto type = members->find("type");
    if (type == members->end()) return false;
    const auto* text = type->second.get_ptr<const std::string*>();
    return text != nullptr && *text == "object";
}

std::string describe_issue(std::string_view subject, const json_schema::Issue& issue) {
    return std::string(subject) + issue.instance_pointer + " " + issue.message;
}

// The schema keywords that mark a tool's parameters for headers in the revisions the options serve (every built-in
// revision when none is named), each once. A revision that defines no argument headers adds none.
std::vector<std::string> argument_keywords(const std::vector<profile::ProfilePtr>& named) {
    std::vector<profile::ProfilePtr> served = named;
    if (served.empty()) {
        for (const std::string_view revision : profile::builtin_revisions()) served.push_back(profile::builtin(revision));
    }
    std::vector<std::string> keywords;
    for (const profile::ProfilePtr& entry : served) {
        if (!entry || !entry->streamable_http() || !entry->streamable_http()->argument_headers) continue;
        const std::string& keyword = entry->streamable_http()->argument_headers->schema_keyword;
        if (std::find(keywords.begin(), keywords.end(), keyword) == keywords.end()) keywords.push_back(keyword);
    }
    return keywords;
}

// The profiles `named` (every built-in one when none is named), each composed with the built-in extensions that give
// its revision tasks, unless it was composed with them already. Throws std::invalid_argument for a profile that cannot
// be composed with them.
std::vector<profile::ProfilePtr> with_task_extensions(const std::vector<profile::ProfilePtr>& named) {
    std::vector<profile::ProfilePtr> served = named;
    if (served.empty()) {
        for (const std::string_view revision : profile::builtin_revisions()) served.push_back(profile::builtin(revision));
    }
    for (profile::ProfilePtr& entry : served) {
        if (!entry) continue;
        std::vector<profile::ExtensionPtr> extensions;
        const auto composed_already = entry->extensions();
        for (const std::string& id : profile::builtin_extensions()) {
            profile::ExtensionPtr extension = profile::builtin_extension(id, entry->revision());
            if (!extension || !extension->mechanism(profile::Slot::tasks)) continue;
            if (std::find(composed_already.begin(), composed_already.end(), id) != composed_already.end()) continue;
            extensions.push_back(std::move(extension));
        }
        if (extensions.empty()) continue;
        auto composed = profile::compose(*entry, extensions);
        if (const auto* error = std::get_if<profile::Error>(&composed)) {
            throw std::invalid_argument("neomcp: revision " + entry->revision() +
                                        " cannot be served with the extensions that give it tasks: " + error->pointer +
                                        ": " + error->message);
        }
        entry = std::get<profile::ProfilePtr>(std::move(composed));
    }
    return served;
}

ToolResult error_result(std::string text) {
    ToolResult result;
    result.content.push_back(text_block(std::move(text)));
    result.is_error = true;
    return result;
}

}  // namespace

class ServerCore;
struct CallRecord;

// Forwards a stop request from one stop source to another.
struct StopForward {
    std::stop_source* target;
    void operator()() const noexcept { target->request_stop(); }
};

// Posts a terminal cancellation of a coroutine call to the io thread.
struct CancelPoster {
    ServerCore* core;
    std::weak_ptr<CallRecord> record;
    void operator()() const noexcept;
};

struct CallRecord;

// What a call produced: any outcome a host can complete a call with.
using JobOutcome = std::variant<ToolResult, ReadResult, PromptResult, CompletionResult, NotFound, InvalidRequest,
                                Refusal, HostFailure, InputRequired>;

// The answers and the state a call of a thing that asked for input comes back with.
struct Answers {
    InputResponses responses;
    std::optional<std::string> state;
};

// The time-out of one wait for a client's answers.
struct InputTimer;
struct TimerCancel {
    ServerCore* core;
    std::weak_ptr<InputTimer> timer;
    void operator()() const noexcept;
};
struct InputTimer {
    InputTimer(asio::io_context& io, InputWait input_wait) : timer(io), wait(std::move(input_wait)) {}
    asio::steady_timer timer;
    InputWait wait;
    std::uint64_t serial = 0;
    std::optional<std::stop_callback<TimerCancel>> on_end;  // destroyed with the timer, on the io thread
};

// Something a call can run: a registered tool, resource or resource template. A call runs it on a worker (plain) or on the coroutine thread, and the
// run never throws: what the handler throws becomes an outcome.
class Callable {
public:
    virtual ~Callable() = default;
    virtual bool coroutine() const noexcept = 0;
    // What the thing is called in the failures the server reports, such as "tool 'add'".
    virtual const std::string& label() const noexcept = 0;

    // The scopes a request for it needs (ToolHandle::scopes and the others); empty for a completer, whose prompt or
    // template has them. Fixed once the server serves.
    std::vector<std::string> scopes;

    JobOutcome run_plain(const CallRecord& record, Context& context);
    asio::awaitable<JobOutcome> run_async(const CallRecord& record, Context& context);

protected:
    virtual JobOutcome do_plain(const CallRecord& record, Context& context) = 0;
    virtual asio::awaitable<JobOutcome> do_async(const CallRecord& record, Context& context) = 0;
    // The outcome for an exception the handler threw (or for a result that cannot be used).
    virtual JobOutcome failure(const CallRecord& record, const std::exception_ptr& thrown) const = 0;
};

struct ToolEntry final : Callable {
    ToolEntry(const ServerOptions& server_options, Body tool_body, std::string tool_label)
        : options(server_options), body(std::move(tool_body)), name_label(std::move(tool_label)) {}

    bool coroutine() const noexcept override { return std::holds_alternative<std::unique_ptr<CoroutineBody>>(body); }
    const std::string& label() const noexcept override { return name_label; }

    const ServerOptions& options;
    Body body;
    std::optional<json_schema::Validator> input;
    std::optional<json_schema::Validator> output;
    std::string name_label;
    TaskSupport task_support = TaskSupport::forbidden;  // fixed once the server serves

protected:
    JobOutcome do_plain(const CallRecord& record, Context& context) override;
    asio::awaitable<JobOutcome> do_async(const CallRecord& record, Context& context) override;
    JobOutcome failure(const CallRecord& record, const std::exception_ptr& thrown) const override;

private:
    JobOutcome settle(ToolResult result) const;
};

// A resource or a resource template: a body that reads.
template <class Out>
class FeatureEntry final : public Callable {
public:
    FeatureEntry(FeatureBody<Out> entry_body, std::string entry_label)
        : body(std::move(entry_body)), name_label(std::move(entry_label)) {}

    bool coroutine() const noexcept override {
        return std::holds_alternative<std::unique_ptr<FeatureCoroutineBody<Out>>>(body);
    }
    const std::string& label() const noexcept override { return name_label; }

    FeatureBody<Out> body;
    std::string name_label;

protected:
    JobOutcome do_plain(const CallRecord& record, Context& context) override;
    asio::awaitable<JobOutcome> do_async(const CallRecord& record, Context& context) override;
    JobOutcome failure(const CallRecord& record, const std::exception_ptr& thrown) const override;
};

// One call, from admission until it is answered; or one entry of a task's handler, which has no completion.
struct CallRecord {
    CallRecord(Callable& callable_to_run, CallControl call_control, std::optional<Completion> completion,
               json call_arguments, json call_meta)
        : callable(&callable_to_run),
          control(std::move(call_control)),
          done(std::move(completion)),
          arguments(std::move(call_arguments)),
          meta(std::move(call_meta)) {}

    Callable* callable;
    CallControl control;
    std::optional<Completion> done;  // empty for a task's entry
    std::shared_ptr<TaskRun> task;   // the task the entry runs for; null for a call
    json arguments;
    json meta;
    Answers answers;                     // of the round before, when the thing asked for input
    std::string subject;                 // what a read names: the uri
    std::optional<std::string> mime;      // the MIME type of the resource or template read
    std::optional<CacheHints> cache;     // the caching hints of the resource read, when it has its own
    std::uint64_t serial = 0;
    bool coroutine = false;  // set before the call is admitted

    // The handler's stop source, and the callbacks that request it; the callbacks are destroyed first.
    std::stop_source stop;
    std::optional<std::stop_callback<StopForward>> follow;  // the session's stop, or the task's

    // Coroutine calls only. The signal is used on the io thread after the coroutine was spawned.
    asio::cancellation_signal signal;
    bool finished = false;  // io thread only: the coroutine produced its outcome
    std::optional<std::stop_callback<CancelPoster>> poster;
};

class ServerCore final : public ToolHost,
                         public ResourceHost,
                         public PromptHost,
                         public CompletionHost,
                         public TaskHost,
                         public std::enable_shared_from_this<ServerCore> {
public:
    explicit ServerCore(ServerOptions options);
    ~ServerCore() override;
    ServerCore(const ServerCore&) = delete;
    ServerCore& operator=(const ServerCore&) = delete;

    // Stops and joins everything; idempotent.
    void shutdown();

    // The options of a session on this server for the transport.
    SessionOptions session_options(profile::Transport transport, IdleCallback on_idle);

    std::size_t add(ToolDefinition definition, Body body, std::vector<HeaderMark> marks);
    void edit(std::size_t index, const std::function<void(ToolDefinition&)>& change);
    const ToolDefinition& definition(std::size_t index) const { return definitions_.at(index); }

    std::size_t add_resource(ResourceDefinition definition, FeatureBody<ReadResult> body);
    std::size_t add_resource_template(ResourceTemplateDefinition definition, FeatureBody<ReadResult> body,
                                      std::vector<TemplateMember> members, TemplateCheck check);
    void edit_resource(std::size_t index, const std::function<void(ResourceDefinition&)>& change);
    void edit_resource_template(std::size_t index, const std::function<void(ResourceTemplateDefinition&)>& change);
    void set_resource_cache(std::size_t index, CacheHints hints);
    void set_template_cache(std::size_t index, CacheHints hints);
    const ResourceDefinition& resource_definition(std::size_t index) const { return resource_defs_.at(index); }
    const ResourceTemplateDefinition& template_definition(std::size_t index) const { return template_defs_.at(index); }

    std::size_t add_prompt(PromptDefinition definition, FeatureBody<PromptResult> body);
    void edit_prompt(std::size_t index, const std::function<void(PromptDefinition&)>& change);
    const PromptDefinition& prompt_definition(std::size_t index) const { return prompt_defs_.at(index); }
    void add_prompt_completer(std::size_t index, std::string argument, FeatureBody<CompletionResult> body);
    void add_template_completer(std::size_t index, std::string variable, FeatureBody<CompletionResult> body);

    std::variant<std::unique_ptr<Session>, CreateError> connect(Sink sink, IdleCallback on_idle);
    std::variant<std::unique_ptr<Session>, CreateError> connect_tagged(profile::Transport transport, OutputSink sink,
                                                                       IdleCallback on_idle);
    std::span<const ToolDefinition> tools() const override;
    bool tool_enabled(std::size_t index) const override;
    std::span<const std::string> tool_scopes(std::size_t index) const override;
    void call(const CallRequest& request, CallControl control, Completion done) override;
    ChangeSupport change_support() const override;
    ChangeRegistration watch(std::weak_ptr<ChangeListener> listener) override;
    ResourceHost* resource_host() noexcept override;
    std::span<const std::string> read_scopes(std::string_view uri) const override;
    std::span<const std::string> prompt_scopes(std::size_t index) const override;
    std::span<const std::string> completion_scopes(const CompletionRequest& request) const override;
    // The scopes of an item, set through its handle.
    void set_tool_scopes(std::size_t index, std::vector<std::string> list);
    void set_resource_scopes(std::size_t index, std::vector<std::string> list);
    void set_template_scopes(std::size_t index, std::vector<std::string> list);
    void set_prompt_scopes(std::size_t index, std::vector<std::string> list);
    bool declares_scopes() const;
    std::span<const ResourceDefinition> resources() const override;
    std::span<const ResourceTemplateDefinition> resource_templates() const override;
    bool resource_enabled(std::size_t index) const override;
    bool template_enabled(std::size_t index) const override;
    bool offers(std::string_view uri) const override;
    ListSettings resource_list_settings() const override;
    void read(const ReadRequest& request, CallControl control, Completion done) override;
    PromptHost* prompt_host() noexcept override;
    std::span<const PromptDefinition> prompts() const override;
    bool prompt_enabled(std::size_t index) const override;
    ListSettings prompt_list_settings() const override;
    void get(const PromptRequest& request, CallControl control, Completion done) override;
    CompletionHost* completion_host() noexcept override;
    void complete(const CompletionRequest& request, CallControl control, Completion done) override;
    std::optional<std::string> check(const ToolDefinition& tool, const json& arguments) const;

    const ServerOptions& options() const noexcept { return options_; }

    // Switching items; each reports the change of its list when the switch changed.
    void enable_tool(std::size_t index, bool on);
    void enable_resource(std::size_t index, bool on);
    void enable_template(std::size_t index, bool on);
    void enable_prompt(std::size_t index, bool on);
    // The feed: a change to tell the sessions, and the sessions registered.
    void report(Change change);
    std::size_t watchers() const;

    void post_cancel(const std::weak_ptr<CallRecord>& record);
    void finish(const std::shared_ptr<CallRecord>& record, JobOutcome outcome);

    // Tasks.
    void set_task_support(std::size_t index, TaskSupport mode);
    // Not null when the server has a store and a tool declares that it may run as a task.
    TaskHost* task_host() noexcept override;
    TaskSupport task_support(std::size_t index) const override { return entries_.at(index)->task_support; }
    TaskPreparation prepare_task(const CallRequest& request, const CallControl& origin) override;
    TaskStartResult start_task(const CallRequest& request, CallControl origin, TaskStart start) override;
    std::optional<TaskRecord> task(std::string_view id, const TaskOwner& owner) override;
    std::optional<TaskRecord> current_task(std::string_view id, const TaskOwner& owner) override;
    std::optional<TaskPage> list_tasks(const TaskOwner& owner, std::optional<std::string_view> cursor) override;
    TaskCancelResult cancel_task(std::string_view id, const TaskOwner& owner) override;
    AnswersResult answer_task(std::string_view id, const TaskOwner& owner, json answers,
                              InputResponses responses) override;
    TaskWatchStart watch_task(std::string_view id, const TaskOwner& owner,
                              std::weak_ptr<TaskListener> listener) override;

    // A call waits for its client's answers: its time-out starts.
    void awaiting_input(InputWait wait) override;
    void cancel_timer(const std::weak_ptr<InputTimer>& timer);
    void cancel_timer_locked(const std::weak_ptr<InputTimer>& timer);  // with io_mutex_ held

private:
    // The completion of a spawned coroutine, on the io thread; it answers the call only when the coroutine failed
    // to.
    struct Spawned {
        ServerCore* core;
        std::shared_ptr<CallRecord> record;
        void operator()(const std::exception_ptr& failure) const {
            record->finished = true;
            if (failure) {
                core->finish(record, HostFailure{"the coroutine of " + record->callable->label() + " failed"});
            }
        }
    };

    // Queues or spawns one call of a registered thing, or refuses it.
    void admit(Callable& callable, json arguments, json meta, CallControl control, Completion done, Answers answers,
               std::string subject = {}, std::optional<std::string> mime = std::nullopt,
               std::optional<CacheHints> cache = std::nullopt);
    void on_timer(const std::shared_ptr<InputTimer>& timer, const std::error_code& error);

    json_schema::Validator compile(const std::string& tool, std::string_view which, const json& schema) const;
    // Throws std::invalid_argument when the schema marks a parameter for a header against a rule of the revisions
    // served.
    void check_header_marks(const std::string& tool, const json& schema) const;
    // Writes the keyword of each served revision on the property that each mark of a described struct names.
    void write_header_marks(const std::string& tool, json& schema, const std::vector<HeaderMark>& marks) const;
    void ensure_registering(const std::string& label, std::string_view action) const;
    // Checks and stores the scopes of a registered thing.
    void set_scopes(Callable& entry, std::vector<std::string> list) const;
    // Whether the call's authorization meets `required`; true where the call came without one (nothing is checked
    // there). The session checks before it calls the host; this check closes the gap in which an item is switched on
    // or off between the session's look and the host's, so that the host never runs what the session would refuse.
    bool permits(const CallControl& control, std::span<const std::string> required) const;
    bool admits() const;  // with mutex_ held

    // What a read of `uri` finds: a resource that is on at that URI, else the first template that is on, matches and
    // whose variables decode.
    struct Found {
        Callable* entry = nullptr;
        json arguments;
        std::optional<std::string> mime;
        std::optional<CacheHints> cache;
    };
    std::optional<Found> find_resource(std::string_view uri) const;
    void unwatch(std::uint64_t id);
    void watchers_changed() const;
    void run_feed();

    // With mutex_ held, for a call that was admitted: puts it in flight, queued for a worker or spawned on the io
    // thread. False when its coroutine could not be spawned; nothing of it is in flight then.
    bool start_locked(const std::shared_ptr<CallRecord>& record);
    // After start_locked, without the lock: wakes a worker, or arms the cancellation of the coroutine.
    void started(const std::shared_ptr<CallRecord>& record);

    // Tasks. `reserved`: the entry has the place start_task reserved for it.
    enum class Launch { started, busy, stopping };
    Launch launch(const std::shared_ptr<TaskRun>& run, Answers answers, bool reserved);
    void unreserve(bool coroutine);  // gives back a place reserved for a task, with mutex_ held
    void settle_task(const std::shared_ptr<TaskRun>& run, JobOutcome outcome, bool stopping);
    void resume_task(const std::shared_ptr<TaskRun>& run, TaskResume resume);
    void arm_sweep();  // on the io thread

    void work();
    void run_io();
    void run_plain(const std::shared_ptr<CallRecord>& record);
    asio::awaitable<void> drive(std::shared_ptr<CallRecord> record);

    // Immutable once serving began.
    ServerOptions options_;
    std::vector<std::string> argument_keywords_;  // see argument_keywords()
    std::vector<ToolDefinition> definitions_;
    std::vector<std::unique_ptr<ToolEntry>> entries_;
    std::map<std::string, std::size_t, std::less<>> by_name_;

    // A resource at a fixed URI.
    struct ResourceSlot {
        std::unique_ptr<FeatureEntry<ReadResult>> entry;
        std::optional<CacheHints> cache;
    };
    // A resource template: the URI template, how its variables become the parameters, and the body.
    struct TemplateSlot {
        std::unique_ptr<FeatureEntry<ReadResult>> entry;
        UriTemplate matcher;
        std::vector<TemplateMember> members;
        TemplateCheck check;
        std::optional<CacheHints> cache;
    };
    std::vector<ResourceDefinition> resource_defs_;
    std::vector<ResourceSlot> resource_slots_;
    std::map<std::string, std::size_t, std::less<>> resource_by_uri_;
    std::vector<ResourceTemplateDefinition> template_defs_;
    std::vector<TemplateSlot> template_slots_;

    std::vector<PromptDefinition> prompt_defs_;
    std::vector<std::unique_ptr<FeatureEntry<PromptResult>>> prompt_entries_;
    std::map<std::string, std::size_t, std::less<>> prompt_by_name_;

    // The completers of an argument, by the argument's name; parallel to the prompts and to the templates.
    // In a deque: a vector that grows copies its elements when their move constructor may throw, as a map's may in
    // one standard library, and a map of unique pointers cannot be copied.
    using Completers = std::map<std::string, std::unique_ptr<FeatureEntry<CompletionResult>>, std::less<>>;
    // What a completion refers to, as complete() resolves it: the completers of a prompt or template that is on, or a
    // resource at a fixed URI that is on (which has none); `owner` is the thing whose scopes the completion needs.
    struct CompletionTarget {
        const Completers* completers = nullptr;
        const Callable* owner = nullptr;
    };
    std::optional<CompletionTarget> completion_target(const CompletionRequest& request) const;
    std::deque<Completers> prompt_completers_;
    std::deque<Completers> template_completers_;
    std::size_t completer_count_ = 0;
    mutable std::atomic<bool> serving_{false};

    // Whether each item is on, parallel to its definitions.
    std::deque<std::atomic<bool>> tool_on_;
    std::deque<std::atomic<bool>> resource_on_;
    std::deque<std::atomic<bool>> template_on_;
    std::deque<std::atomic<bool>> prompt_on_;

    // The feed. Guarded by feed_mutex_.
    mutable std::mutex feed_mutex_;
    std::condition_variable feed_ready_;
    std::map<std::uint64_t, std::weak_ptr<ChangeListener>> watchers_;
    std::uint64_t next_watcher_ = 1;
    std::deque<Change> pending_;                                  // in the order reported
    std::set<std::pair<ChangeKind, std::string>, std::less<>> pending_keys_;  // what pending_ holds
    bool feed_open_ = true;  // false once shutdown() began: no registration and no change is taken any more
    std::thread feed_thread_;

    // Guarded by mutex_.
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::deque<std::shared_ptr<CallRecord>> queue_;
    std::size_t running_ = 0;
    std::size_t coroutines_ = 0;  // coroutine calls in flight, and places reserved for tasks of coroutine tools
    std::size_t reserved_ = 0;    // places reserved for tasks of plain tools, counted as calls that wait
    bool stopping_ = false;
    std::uint64_t next_serial_ = 0;
    std::map<std::uint64_t, std::shared_ptr<CallRecord>> in_flight_;

    std::vector<std::thread> workers_;

    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> io_work_;
    std::thread io_thread_;
    std::mutex io_mutex_;  // guards io_open_, timers_open_, timers_ and next_timer_
    bool io_open_ = true;  // cancellations may still be posted
    bool timers_open_ = true;  // input timers may still be started
    std::map<std::uint64_t, std::weak_ptr<InputTimer>> timers_;
    std::uint64_t next_timer_ = 0;

    // Tasks: null when the server has no task store. The sweep's timer is used on the io thread only.
    std::unique_ptr<TaskRunner> tasks_;
    std::optional<asio::steady_timer> sweep_timer_;
    // The profiles the sessions serve while tasks are on: those of the options composed with the extensions that give
    // their revisions tasks. Made once, at the first session.
    std::once_flag composed_once_;
    std::vector<profile::ProfilePtr> composed_;
};

void CancelPoster::operator()() const noexcept { core->post_cancel(record); }

void TimerCancel::operator()() const noexcept { core->cancel_timer(timer); }

// ---------------------------------------------------------------------------------------------------------------
// Lifetime

ServerCore::ServerCore(ServerOptions options)
    : options_(std::move(options)), argument_keywords_(argument_keywords(options_.profiles)) {
    if (options_.max_concurrent_calls == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_concurrent_calls is zero");
    }
    if (options_.max_coroutine_calls == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_coroutine_calls is zero");
    }
    if (options_.resources_page_size && *options_.resources_page_size == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::resources_page_size is zero");
    }
    if (options_.prompts_page_size && *options_.prompts_page_size == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::prompts_page_size is zero");
    }
    if (options_.max_input_rounds == 0) throw std::invalid_argument("neomcp: ServerOptions::max_input_rounds is zero");
    if (options_.max_declared_state_bytes == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_declared_state_bytes is zero");
    }
    if (options_.max_subscriptions == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_subscriptions is zero");
    }
    if (options_.max_watched_resources == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_watched_resources is zero");
    }
    if (options_.max_watched_uri_bytes == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_watched_uri_bytes is zero");
    }
    if (options_.max_watched_bytes == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_watched_bytes is zero");
    }
    if (options_.max_client_info_bytes == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_client_info_bytes is zero");
    }
    if (options_.max_token_bytes == 0) throw std::invalid_argument("neomcp: ServerOptions::max_token_bytes is zero");
    if (options_.input_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("neomcp: ServerOptions::input_timeout is not positive");
    }
    if (options_.request_state_ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("neomcp: ServerOptions::request_state_ttl is not positive");
    }
    for (const auto& key : options_.request_state_keys) {
        if (key.size() < min_request_state_key_size) {
            throw std::invalid_argument("neomcp: a key of ServerOptions::request_state_keys has fewer than " +
                                        std::to_string(min_request_state_key_size) + " bytes");
        }
    }
    TaskRunner::check(options_.tasks);
    // Before any thread runs: the store's unfinished tasks are failed before a new one can start.
    if (options_.tasks.store) {
        tasks_ = std::make_unique<TaskRunner>(options_);
        sweep_timer_.emplace(io_);
    }
    io_work_.emplace(asio::make_work_guard(io_));
    if (tasks_) asio::post(io_, [this] { arm_sweep(); });
    try {
        feed_thread_ = std::thread([this] { run_feed(); });
        io_thread_ = std::thread([this] { run_io(); });
        workers_.reserve(options_.max_concurrent_calls);
        for (std::size_t i = 0; i < options_.max_concurrent_calls; ++i) workers_.emplace_back([this] { work(); });
    } catch (...) {
        shutdown();
        throw;
    }
}

ServerCore::~ServerCore() { shutdown(); }

void ServerCore::shutdown() {
    // The feed first: what is pending is told, then every listener learns that the feed ended, while the handlers may
    // still run.
    {
        const std::lock_guard<std::mutex> lock(feed_mutex_);
        feed_open_ = false;
    }
    feed_ready_.notify_all();
    if (feed_thread_.joinable()) feed_thread_.join();

    std::deque<std::shared_ptr<CallRecord>> waiting;
    std::vector<std::shared_ptr<CallRecord>> running;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        waiting.swap(queue_);
        for (const auto& entry : in_flight_) running.push_back(entry.second);
    }
    work_ready_.notify_all();
    // No lock is held from here on: stop callbacks run inline and may report or complete.
    for (const auto& record : running) record->stop.request_stop();
    for (const auto& record : waiting) {
        finish(record, HostFailure{"the server shut down before " + record->callable->label() + " ran"});
    }
    // Tasks that wait for answers have no handler to stop; they fail here.
    if (tasks_) tasks_->shutdown();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    // A pending input timer is work of the io_context: every one is cancelled, and none starts from now on.
    {
        const std::lock_guard<std::mutex> lock(io_mutex_);
        timers_open_ = false;
        for (const auto& entry : timers_) cancel_timer_locked(entry.second);
        if (sweep_timer_ && io_open_) {
            asio::post(io_, [this] { sweep_timer_->cancel(); });
        }
    }
    // call() spawns no coroutine once stopping_ is set, so the io_context runs out of work when the coroutines in
    // flight have finished.
    io_work_.reset();
    if (io_thread_.joinable()) io_thread_.join();
    const std::lock_guard<std::mutex> lock(io_mutex_);
    io_open_ = false;
}

void ServerCore::run_io() {
    for (;;) {
        try {
            io_.run();
            return;
        } catch (...) {
            // A completion handler threw; the others keep running (Asio allows run() to be called again).
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Registration

void ServerCore::ensure_registering(const std::string& label, std::string_view action) const {
    if (serving_.load()) {
        throw std::logic_error("neomcp: " + label + " cannot be " + std::string(action) +
                               ": the server serves already");
    }
}

void ServerCore::set_scopes(Callable& entry, std::vector<std::string> list) const {
    ensure_registering(entry.label(), "given scopes");
    for (const std::string& scope : list) {
        if (!is_scope_token(scope)) {
            throw std::invalid_argument("neomcp: " + entry.label() + ": the scope '" + scope +
                                        "' is not a scope-token");
        }
    }
    entry.scopes = std::move(list);
}

void ServerCore::set_tool_scopes(std::size_t index, std::vector<std::string> list) {
    set_scopes(*entries_.at(index), std::move(list));
}

void ServerCore::set_resource_scopes(std::size_t index, std::vector<std::string> list) {
    set_scopes(*resource_slots_.at(index).entry, std::move(list));
}

void ServerCore::set_template_scopes(std::size_t index, std::vector<std::string> list) {
    set_scopes(*template_slots_.at(index).entry, std::move(list));
}

void ServerCore::set_prompt_scopes(std::size_t index, std::vector<std::string> list) {
    set_scopes(*prompt_entries_.at(index), std::move(list));
}

bool ServerCore::declares_scopes() const {
    const auto declares = [](const auto& entries) {
        return std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return !entry->scopes.empty(); });
    };
    const auto slot_declares = [](const auto& slots) {
        return std::any_of(slots.begin(), slots.end(), [](const auto& slot) { return !slot.entry->scopes.empty(); });
    };
    return declares(entries_) || declares(prompt_entries_) || slot_declares(resource_slots_) ||
           slot_declares(template_slots_);
}

bool ServerCore::permits(const CallControl& control, std::span<const std::string> required) const {
    const Authorization* granted = control.authorization();
    if (granted == nullptr) return true;
    return missing_scopes(granted->scopes, required, options_.scope_implications).empty();
}

json_schema::Validator ServerCore::compile(const std::string& tool, std::string_view which, const json& schema) const {
    if (!is_object_schema(schema)) {
        throw std::invalid_argument("neomcp: tool '" + tool + "': the " + std::string(which) +
                                    " schema is not an object schema (an object with \"type\": \"object\")");
    }
    auto compiled = json_schema::Validator::compile(schema, options_.schema_limits);
    if (const auto* issue = std::get_if<json_schema::Issue>(&compiled)) {
        throw std::invalid_argument("neomcp: tool '" + tool + "': the validator refuses the " + std::string(which) +
                                    " schema at \"" + issue->schema_pointer + "\": " + issue->message);
    }
    return std::get<json_schema::Validator>(std::move(compiled));
}

void ServerCore::check_header_marks(const std::string& tool, const json& schema) const {
    for (const std::string& keyword : argument_keywords_) {
        const auto marks = header_parameters(schema, keyword);
        if (const auto* issue = std::get_if<HeaderMarkIssue>(&marks)) {
            throw std::invalid_argument("neomcp: tool '" + tool + "': the mark '" + keyword + "' at \"" +
                                        issue->pointer + "\" breaks a rule: " + issue->rule);
        }
    }
}

void ServerCore::write_header_marks(const std::string& tool, json& schema, const std::vector<HeaderMark>& marks) const {
    const auto child = [](json& object, std::string_view name) -> json* {
        auto* members = object.get_ptr<json::object_t*>();
        if (members == nullptr) return nullptr;
        const auto found = members->find(std::string(name));
        return found == members->end() ? nullptr : &found->second;
    };
    // A parameter has one header: a second mark on the same member would silently replace the first.
    std::set<std::vector<std::string>> marked;
    for (const HeaderMark& mark : marks) {
        if (!marked.insert(mark.path).second) {
            throw std::invalid_argument("neomcp: tool '" + tool + "': the member '" + mark.path.back() +
                                        "' is marked to be mirrored into more than one header");
        }
        json* property = &schema;
        for (const std::string& step : mark.path) {
            json* properties = child(*property, "properties");
            property = properties == nullptr ? nullptr : child(*properties, step);
            if (property == nullptr || !property->is_object()) {
                throw std::invalid_argument("neomcp: tool '" + tool + "': the schema has no property schema for the member '" +
                                            step + "', which is marked to be mirrored into a header");
            }
        }
        for (const std::string& keyword : argument_keywords_) (*property)[keyword] = json(mark.name);
    }
}

std::size_t ServerCore::add(ToolDefinition definition, Body body, std::vector<HeaderMark> marks) {
    const std::string name = definition.name;
    ensure_registering("tool '" + name + "'", "registered");
    if (name.empty()) throw std::invalid_argument("neomcp: a tool needs a name");
    if (by_name_.find(name) != by_name_.end()) {
        throw std::invalid_argument("neomcp: a tool named '" + name + "' is registered already");
    }
    if (definition.meta && !definition.meta->is_object()) {
        throw std::invalid_argument("neomcp: tool '" + name + "': _meta must be an object");
    }
    write_header_marks(name, definition.input_schema, marks);
    auto entry = std::make_unique<ToolEntry>(options_, std::move(body), "tool '" + name + "'");
    entry->input = compile(name, "input", definition.input_schema);
    check_header_marks(name, definition.input_schema);
    if (definition.output_schema) entry->output = compile(name, "output", *definition.output_schema);
    const std::size_t index = definitions_.size();
    definitions_.push_back(std::move(definition));
    entries_.push_back(std::move(entry));
    by_name_.emplace(name, index);
    tool_on_.emplace_back(true);
    return index;
}

void ServerCore::edit(std::size_t index, const std::function<void(ToolDefinition&)>& change) {
    ToolDefinition& tool = definitions_.at(index);
    ensure_registering("tool '" + tool.name + "'", "changed");
    change(tool);
}

std::optional<json> variable_value(VariableKind kind, const std::string& text) {
    switch (kind) {
        case VariableKind::text: return json(text);
        case VariableKind::boolean:
            if (text == "true") return json(true);
            if (text == "false") return json(false);
            return std::nullopt;
        case VariableKind::signed_integer: {
            std::int64_t value = 0;
            const char* end = text.data() + text.size();
            const auto [stop, error] = std::from_chars(text.data(), end, value);
            if (error != std::errc() || stop != end || text.empty()) return std::nullopt;
            return json(value);
        }
        case VariableKind::unsigned_integer: {
            std::uint64_t value = 0;
            const char* end = text.data() + text.size();
            const auto [stop, error] = std::from_chars(text.data(), end, value);
            if (error != std::errc() || stop != end || text.empty()) return std::nullopt;
            return json(value);
        }
    }
    return std::nullopt;
}

std::size_t ServerCore::add_resource(ResourceDefinition definition, FeatureBody<ReadResult> body) {
    ensure_registering("resource '" + definition.uri + "'", "registered");
    const std::string label = "resource '" + definition.uri + "'";
    if (definition.uri.empty()) throw std::invalid_argument("neomcp: a resource needs a URI");
    if (definition.name.empty()) throw std::invalid_argument("neomcp: " + label + " needs a name");
    if (resource_by_uri_.find(definition.uri) != resource_by_uri_.end()) {
        throw std::invalid_argument("neomcp: a resource with the URI '" + definition.uri + "' is registered already");
    }
    const std::size_t index = resource_defs_.size();
    resource_by_uri_.emplace(definition.uri, index);
    resource_defs_.push_back(std::move(definition));
    resource_slots_.push_back(ResourceSlot{std::make_unique<FeatureEntry<ReadResult>>(std::move(body), label), std::nullopt});
    resource_on_.emplace_back(true);
    return index;
}

std::size_t ServerCore::add_resource_template(ResourceTemplateDefinition definition, FeatureBody<ReadResult> body,
                                              std::vector<TemplateMember> members, TemplateCheck check) {
    ensure_registering("resource template '" + definition.uri_template + "'", "registered");
    const std::string label = "resource template '" + definition.uri_template + "'";
    if (definition.uri_template.empty()) throw std::invalid_argument("neomcp: a resource template needs a URI template");
    if (definition.name.empty()) throw std::invalid_argument("neomcp: " + label + " needs a name");
    for (const auto& other : template_defs_) {
        if (other.uri_template == definition.uri_template) {
            throw std::invalid_argument("neomcp: the resource template '" + definition.uri_template +
                                        "' is registered already");
        }
    }
    auto parsed = UriTemplate::parse(definition.uri_template);
    if (const auto* error = std::get_if<UriTemplateError>(&parsed)) {
        throw std::invalid_argument("neomcp: " + label + " is not a URI template: " + error->message + " (at offset " +
                                    std::to_string(error->position) + ")");
    }
    UriTemplate matcher = std::get<UriTemplate>(std::move(parsed));
    if (matcher.unsupported()) {
        throw std::invalid_argument("neomcp: " + label + " uses a form that cannot be matched: " + *matcher.unsupported());
    }
    // The variables of the template and the members of the parameters struct are the same set.
    const std::vector<std::string> variables = matcher.variables();
    for (const auto& variable : variables) {
        const bool known = std::any_of(members.begin(), members.end(),
                                       [&](const TemplateMember& member) { return member.name == variable; });
        if (!known) {
            throw std::invalid_argument("neomcp: " + label + ": the variable '" + variable +
                                        "' is not a member of the parameters struct");
        }
    }
    for (const auto& member : members) {
        if (std::find(variables.begin(), variables.end(), member.name) == variables.end()) {
            throw std::invalid_argument("neomcp: " + label + ": the member '" + member.name +
                                        "' of the parameters struct is not a variable of the template");
        }
    }
    const std::size_t index = template_defs_.size();
    template_defs_.push_back(std::move(definition));
    template_slots_.push_back(TemplateSlot{std::make_unique<FeatureEntry<ReadResult>>(std::move(body), label),
                                           std::move(matcher), std::move(members), std::move(check), std::nullopt});
    template_completers_.emplace_back();
    template_on_.emplace_back(true);
    return index;
}

std::size_t ServerCore::add_prompt(PromptDefinition definition, FeatureBody<PromptResult> body) {
    const std::string label = "prompt '" + definition.name + "'";
    ensure_registering(label, "registered");
    if (definition.name.empty()) throw std::invalid_argument("neomcp: a prompt needs a name");
    if (prompt_by_name_.find(definition.name) != prompt_by_name_.end()) {
        throw std::invalid_argument("neomcp: a prompt named '" + definition.name + "' is registered already");
    }
    const std::size_t index = prompt_defs_.size();
    prompt_by_name_.emplace(definition.name, index);
    prompt_defs_.push_back(std::move(definition));
    prompt_entries_.push_back(std::make_unique<FeatureEntry<PromptResult>>(std::move(body), label));
    prompt_completers_.emplace_back();
    prompt_on_.emplace_back(true);
    return index;
}

void ServerCore::edit_prompt(std::size_t index, const std::function<void(PromptDefinition&)>& change) {
    PromptDefinition& prompt = prompt_defs_.at(index);
    ensure_registering("prompt '" + prompt.name + "'", "changed");
    change(prompt);
}

void ServerCore::add_prompt_completer(std::size_t index, std::string argument, FeatureBody<CompletionResult> body) {
    const PromptDefinition& prompt = prompt_defs_.at(index);
    const std::string label = "the completer of argument '" + argument + "' of prompt '" + prompt.name + "'";
    ensure_registering("prompt '" + prompt.name + "'", "given a completer");
    const bool known = std::any_of(prompt.arguments.begin(), prompt.arguments.end(),
                                   [&](const PromptArgumentDefinition& candidate) { return candidate.name == argument; });
    if (!known) {
        throw std::invalid_argument("neomcp: prompt '" + prompt.name + "' has no argument '" + argument + "' to complete");
    }
    Completers& completers = prompt_completers_.at(index);
    if (completers.find(argument) != completers.end()) {
        throw std::invalid_argument("neomcp: argument '" + argument + "' of prompt '" + prompt.name +
                                    "' has a completer already");
    }
    completers.emplace(std::move(argument), std::make_unique<FeatureEntry<CompletionResult>>(std::move(body), label));
    ++completer_count_;
}

void ServerCore::add_template_completer(std::size_t index, std::string variable, FeatureBody<CompletionResult> body) {
    const std::string& uri_template = template_defs_.at(index).uri_template;
    const std::string label = "the completer of variable '" + variable + "' of resource template '" + uri_template + "'";
    ensure_registering("resource template '" + uri_template + "'", "given a completer");
    const auto& members = template_slots_.at(index).members;
    const bool known = std::any_of(members.begin(), members.end(),
                                   [&](const TemplateMember& candidate) { return candidate.name == variable; });
    if (!known) {
        throw std::invalid_argument("neomcp: resource template '" + uri_template + "' has no variable '" + variable +
                                    "' to complete");
    }
    Completers& completers = template_completers_.at(index);
    if (completers.find(variable) != completers.end()) {
        throw std::invalid_argument("neomcp: variable '" + variable + "' of resource template '" + uri_template +
                                    "' has a completer already");
    }
    completers.emplace(std::move(variable), std::make_unique<FeatureEntry<CompletionResult>>(std::move(body), label));
    ++completer_count_;
}

void ServerCore::edit_resource(std::size_t index, const std::function<void(ResourceDefinition&)>& change) {
    ResourceDefinition& resource = resource_defs_.at(index);
    ensure_registering("resource '" + resource.uri + "'", "changed");
    change(resource);
}

void ServerCore::edit_resource_template(std::size_t index,
                                        const std::function<void(ResourceTemplateDefinition&)>& change) {
    ResourceTemplateDefinition& resource = template_defs_.at(index);
    ensure_registering("resource template '" + resource.uri_template + "'", "changed");
    change(resource);
}

void ServerCore::set_resource_cache(std::size_t index, CacheHints hints) {
    ensure_registering("resource '" + resource_defs_.at(index).uri + "'", "changed");
    resource_slots_.at(index).cache = hints;
}

void ServerCore::set_template_cache(std::size_t index, CacheHints hints) {
    ensure_registering("resource template '" + template_defs_.at(index).uri_template + "'", "changed");
    template_slots_.at(index).cache = hints;
}

// ---------------------------------------------------------------------------------------------------------------
// Serving

std::variant<std::unique_ptr<Session>, CreateError> ServerCore::connect(Sink sink, IdleCallback on_idle) {
    return Session::create(session_options(profile::Transport::stdio, std::move(on_idle)), *this, std::move(sink));
}

std::variant<std::unique_ptr<Session>, CreateError> ServerCore::connect_tagged(profile::Transport transport,
                                                                             OutputSink sink, IdleCallback on_idle) {
    return Session::create_tagged(session_options(transport, std::move(on_idle)), *this, std::move(sink));
}

SessionOptions ServerCore::session_options(profile::Transport transport, IdleCallback on_idle) {
    serving_.store(true);
    SessionOptions session;
    session.transport = transport;
    session.profiles = options_.profiles;
    if (task_host() != nullptr) {
        // Registration has ended: which tools declare tasks is fixed, and so is the profile they are served under.
        std::call_once(composed_once_, [this] { composed_ = with_task_extensions(options_.profiles); });
        session.profiles = composed_;
    }
    session.server_info.name = options_.name;
    session.server_info.version = options_.version;
    session.server_info.title = options_.title;
    session.server_info.description = options_.description;
    session.server_info.website_url = options_.website_url;
    session.server_info.icons = options_.icons;
    session.instructions = options_.instructions;
    session.tools_cache = options_.tools_cache;
    session.tools_page_size = options_.tools_page_size;
    session.protocol_logging = options_.protocol_logging;
    session.default_log_level = options_.default_log_level;
    session.include_server_info = options_.include_server_info;
    session.max_message_bytes = options_.max_message_bytes;
    session.max_nesting_depth = options_.max_nesting_depth;
    session.max_queued_notifications = options_.max_queued_notifications;
    session.max_client_info_bytes = options_.max_client_info_bytes;
    session.max_token_bytes = options_.max_token_bytes;
    session.expose_error_details = options_.expose_error_details;
    session.max_input_rounds = options_.max_input_rounds;
    session.request_state_keys = options_.request_state_keys;
    session.request_state_ttl = options_.request_state_ttl;
    session.clock = options_.clock;
    session.on_request_state_rejected = options_.on_request_state_rejected;
    session.answers_need_state = options_.answers_need_state;
    session.max_subscriptions = options_.max_subscriptions;
    session.max_watched_resources = options_.max_watched_resources;
    session.max_watched_uri_bytes = options_.max_watched_uri_bytes;
    session.max_watched_bytes = options_.max_watched_bytes;
    session.scope_implications = options_.scope_implications;
    session.on_idle = std::move(on_idle);
    // The session keeps this, and with it the core: the core outlives every session it serves.
    session.check_arguments = [core = shared_from_this()](const ToolDefinition& tool, const json& arguments) {
        return core->check(tool, arguments);
    };
    return session;
}

std::span<const ToolDefinition> ServerCore::tools() const {
    serving_.store(true);
    return definitions_;
}

bool ServerCore::tool_enabled(std::size_t index) const { return tool_on_.at(index).load(); }
std::span<const std::string> ServerCore::tool_scopes(std::size_t index) const { return entries_.at(index)->scopes; }
bool ServerCore::resource_enabled(std::size_t index) const { return resource_on_.at(index).load(); }
bool ServerCore::template_enabled(std::size_t index) const { return template_on_.at(index).load(); }
bool ServerCore::prompt_enabled(std::size_t index) const { return prompt_on_.at(index).load(); }

// A switch is a change of its list only when it changed: switching on what is on tells nobody anything.
void ServerCore::enable_tool(std::size_t index, bool on) {
    if (tool_on_.at(index).exchange(on) != on) report(Change{ChangeKind::tools_list, {}});
}
void ServerCore::enable_resource(std::size_t index, bool on) {
    if (resource_on_.at(index).exchange(on) != on) report(Change{ChangeKind::resources_list, {}});
}
void ServerCore::enable_template(std::size_t index, bool on) {
    if (template_on_.at(index).exchange(on) != on) report(Change{ChangeKind::resources_list, {}});
}
void ServerCore::enable_prompt(std::size_t index, bool on) {
    if (prompt_on_.at(index).exchange(on) != on) report(Change{ChangeKind::prompts_list, {}});
}

ChangeSupport ServerCore::change_support() const {
    // Every list can change, and every resource can be updated; a kind the server has nothing of is not declared by
    // the session (tools: the server declares the capability always, so an empty list reports no changes).
    ChangeSupport support;
    support.tools_list = !definitions_.empty();
    support.prompts_list = true;
    support.resources_list = true;
    support.resource_updates = true;
    return support;
}

ChangeRegistration ServerCore::watch(std::weak_ptr<ChangeListener> listener) {
    std::uint64_t id = 0;
    {
        const std::lock_guard<std::mutex> lock(feed_mutex_);
        if (!feed_open_) return {};
        id = next_watcher_++;
        watchers_.emplace(id, std::move(listener));
    }
    watchers_changed();
    return ChangeRegistration([core = weak_from_this(), id] {
        if (const auto held = core.lock()) held->unwatch(id);
    });
}

void ServerCore::unwatch(std::uint64_t id) {
    bool erased = false;
    {
        const std::lock_guard<std::mutex> lock(feed_mutex_);
        erased = watchers_.erase(id) != 0;
    }
    if (erased) watchers_changed();
}

std::size_t ServerCore::watchers() const {
    const std::lock_guard<std::mutex> lock(feed_mutex_);
    return watchers_.size();
}

void ServerCore::watchers_changed() const {
    if (!options_.on_change_watchers) return;
    try {
        options_.on_change_watchers();
    } catch (...) {
        // A diagnostic that fails changes nothing.
    }
}

void ServerCore::report(Change change) {
    {
        const std::lock_guard<std::mutex> lock(feed_mutex_);
        // Nobody listens, or nobody will: the change reaches no client.
        if (!feed_open_ || watchers_.empty()) return;
        // A change that is pending already is told once.
        if (!pending_keys_.emplace(change.kind, change.uri).second) return;
        pending_.push_back(std::move(change));
    }
    feed_ready_.notify_one();
}

void ServerCore::run_feed() {
    std::unique_lock<std::mutex> lock(feed_mutex_);
    for (;;) {
        feed_ready_.wait(lock, [&] { return !pending_.empty() || !feed_open_; });
        // The listeners alive now; the expired ones (a session that went without closing) are dropped.
        std::vector<std::shared_ptr<ChangeListener>> listeners;
        bool dropped = false;
        for (auto it = watchers_.begin(); it != watchers_.end();) {
            if (auto held = it->second.lock()) {
                listeners.push_back(std::move(held));
                ++it;
            } else {
                it = watchers_.erase(it);
                dropped = true;
            }
        }
        if (!pending_.empty()) {
            Change change = std::move(pending_.front());
            pending_.pop_front();
            pending_keys_.erase(std::pair<ChangeKind, std::string>(change.kind, change.uri));
            lock.unlock();
            if (dropped) watchers_changed();
            for (const auto& listener : listeners) listener->changed(change);
            listeners.clear();  // a session may end here, and its registration takes the lock
            lock.lock();
            continue;
        }
        // The feed is closed and nothing is pending: every listener learns that it ended.
        watchers_.clear();
        lock.unlock();
        for (const auto& listener : listeners) listener->ended();
        listeners.clear();
        watchers_changed();
        return;
    }
}

std::optional<std::string> ServerCore::check(const ToolDefinition& tool, const json& arguments) const {
    const auto found = by_name_.find(tool.name);
    if (found == by_name_.end()) return std::nullopt;  // not a tool of this server; call() answers it
    if (auto issue = entries_[found->second]->input->validate(arguments)) return describe_issue("arguments", *issue);
    return std::nullopt;
}

bool ServerCore::admits() const {
    // Calls a worker will take at once, then the ones that may wait.
    const std::size_t idle = options_.max_concurrent_calls - running_;
    const std::size_t waiting = queue_.size() + reserved_;
    return waiting < idle || waiting - idle < options_.max_pending_calls;
}

void ServerCore::call(const CallRequest& request, CallControl control, Completion done) {
    serving_.store(true);
    const auto found = by_name_.find(request.name);
    if (found == by_name_.end()) {
        done(HostFailure{"the server has no tool named '" + request.name + "'"});
        return;
    }
    admit(*entries_[found->second], request.arguments, request.meta, std::move(control), std::move(done),
          Answers{request.input_responses, request.request_state});
}

void ServerCore::admit(Callable& callable, json arguments, json meta, CallControl control, Completion done,
                       Answers answers, std::string subject, std::optional<std::string> mime,
                       std::optional<CacheHints> cache) {
    if (!permits(control, callable.scopes)) {
        done(HostFailure{"the authorization lacks a scope of " + callable.label()});
        return;
    }
    auto record = std::make_shared<CallRecord>(callable, std::move(control), std::move(done), std::move(arguments),
                                               std::move(meta));
    record->answers = std::move(answers);
    record->subject = std::move(subject);
    record->mime = std::move(mime);
    record->cache = cache;
    // Runs at once when the session already requested stop.
    record->follow.emplace(record->control.stop_token(), StopForward{&record->stop});
    const bool coroutine = callable.coroutine();
    record->coroutine = coroutine;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            lock.unlock();
            (*record->done)(HostFailure{"the server has shut down"});
            return;
        }
        if (coroutine ? coroutines_ >= options_.max_coroutine_calls : !admits()) {
            lock.unlock();
            (*record->done)(Refusal::busy);
            return;
        }
        if (!start_locked(record)) {
            lock.unlock();
            (*record->done)(HostFailure{record->callable->label() + " could not be started"});
            return;
        }
    }
    started(record);
}

bool ServerCore::start_locked(const std::shared_ptr<CallRecord>& record) {
    record->serial = next_serial_++;
    in_flight_.emplace(record->serial, record);
    if (!record->coroutine) {
        queue_.push_back(record);
        return true;
    }
    ++coroutines_;
    // Nothing of the call runs here (drive() begins by posting), so spawning may happen under the lock; it does, so
    // that shutdown() cannot let the io_context run out of work in between.
    try {
        asio::co_spawn(io_, drive(record), asio::bind_cancellation_slot(record->signal.slot(), Spawned{this, record}));
    } catch (...) {
        in_flight_.erase(record->serial);
        --coroutines_;
        return false;
    }
    return true;
}

void ServerCore::started(const std::shared_ptr<CallRecord>& record) {
    if (!record->coroutine) {
        work_ready_.notify_one();
        return;
    }
    // After spawning: the cancellation it posts must find the coroutine's handler in the signal's slot.
    record->poster.emplace(record->stop.get_token(), CancelPoster{this, record});
}

// ---------------------------------------------------------------------------------------------------------------
// Tasks

void ServerCore::set_task_support(std::size_t index, TaskSupport mode) {
    ToolEntry& entry = *entries_.at(index);
    ensure_registering(entry.label(), "changed");
    entry.task_support = mode;
}

void ServerCore::unreserve(bool coroutine) {
    if (coroutine) {
        --coroutines_;
    } else {
        --reserved_;
    }
}

TaskStartResult ServerCore::start_task(const CallRequest& request, CallControl origin, TaskStart start) {
    serving_.store(true);
    if (!tasks_) return TaskRefusal::off;
    const auto found = by_name_.find(request.name);
    if (found == by_name_.end() || !tool_on_[found->second].load()) return TaskRefusal::unknown_tool;
    const std::size_t index = found->second;
    const ToolEntry& entry = *entries_[index];
    if (entry.task_support == TaskSupport::forbidden) return TaskRefusal::not_declared;
    if (!permits(origin, entry.scopes)) return TaskRefusal::not_permitted;
    // The place first, then the record: a task refused as busy leaves nothing behind.
    const bool coroutine = entry.coroutine();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return TaskRefusal::shut_down;
        if (coroutine ? coroutines_ >= options_.max_coroutine_calls : !admits()) return TaskRefusal::busy;
        if (coroutine) {
            ++coroutines_;
        } else {
            ++reserved_;
        }
    }
    std::variant<TaskRunner::Created, TaskRefusal> created = TaskRefusal::busy;
    try {
        created = tasks_->create(index, request, std::move(origin), std::move(start));
    } catch (...) {
        const std::lock_guard<std::mutex> lock(mutex_);
        unreserve(coroutine);
        throw;
    }
    if (const auto* refused = std::get_if<TaskRefusal>(&created)) {
        const std::lock_guard<std::mutex> lock(mutex_);
        unreserve(coroutine);
        return *refused;
    }
    auto& made = std::get<TaskRunner::Created>(created);
    // The first entry gets what the call came with: the answers and the state of the inputs asked before the task.
    const Launch launched = launch(made.run, Answers{request.input_responses, request.request_state}, true);
    if (launched != Launch::started) {
        tasks_->fail_to_run(made.run, launched == Launch::busy);
        return launched == Launch::busy ? TaskRefusal::busy : TaskRefusal::shut_down;
    }
    return std::move(made.record);
}

TaskHost* ServerCore::task_host() noexcept {
    if (!tasks_) return nullptr;
    const bool declared = std::any_of(entries_.begin(), entries_.end(), [](const std::unique_ptr<ToolEntry>& entry) {
        return entry->task_support != TaskSupport::forbidden;
    });
    return declared ? this : nullptr;
}

TaskPreparation ServerCore::prepare_task(const CallRequest& request, const CallControl& origin) {
    serving_.store(true);
    const auto found = by_name_.find(request.name);
    if (found == by_name_.end()) return std::monostate{};  // start_task refuses it
    ToolEntry& entry = *entries_[found->second];
    // Only the question functions run: nothing of the handler, so no stop can concern them.
    const std::stop_source unused;
    const Context context(origin, request.meta, unused.get_token(), request.input_responses, request.request_state);
    try {
        std::optional<InputRequired> pending =
            std::visit([&](auto& body) { return body->pending_inputs(request.arguments, context); }, entry.body);
        if (pending) return std::move(*pending);
    } catch (const AnswerRefused& refused) {
        return InvalidRequest{refused.what()};
    } catch (...) {
        // What else fails here fails the handler's first entry the same way, and the task ends with it.
    }
    return std::monostate{};
}

ServerCore::Launch ServerCore::launch(const std::shared_ptr<TaskRun>& run, Answers answers, bool reserved) {
    ToolEntry& entry = *entries_[run->tool];
    auto record = std::make_shared<CallRecord>(entry, run->origin, std::nullopt, run->request.arguments,
                                               run->request.meta);
    record->task = run;
    record->answers = std::move(answers);
    // The task's stop, never the session's: the task outlives the request that started it. Runs at once when the
    // task was cancelled already.
    record->follow.emplace(run->stop.get_token(), StopForward{&record->stop});
    record->coroutine = entry.coroutine();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // A reserved place becomes the entry's own.
        if (reserved) unreserve(record->coroutine);
        if (stopping_) return Launch::stopping;
        if (!reserved && (record->coroutine ? coroutines_ >= options_.max_coroutine_calls : !admits())) {
            return Launch::busy;
        }
        if (!start_locked(record)) return Launch::busy;
    }
    started(record);
    return Launch::started;
}

void ServerCore::settle_task(const std::shared_ptr<TaskRun>& run, JobOutcome outcome, bool stopping) {
    TaskOutcome ended = std::visit(
        [](auto&& value) -> TaskOutcome {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ToolResult> || std::is_same_v<T, Refusal> || std::is_same_v<T, HostFailure> ||
                          std::is_same_v<T, InvalidRequest> || std::is_same_v<T, InputRequired>) {
                return TaskOutcome(std::move(value));
            } else {
                return TaskOutcome(HostFailure{"the tool answered with a result of another kind"});
            }
        },
        std::move(outcome));
    std::optional<TaskResume> resume = tasks_->settle(run, std::move(ended), stopping);
    if (resume) resume_task(run, std::move(*resume));
}

void ServerCore::resume_task(const std::shared_ptr<TaskRun>& run, TaskResume resume) {
    const Launch launched = launch(run, Answers{std::move(resume.responses), std::move(resume.state)}, false);
    if (launched != Launch::started) tasks_->fail_to_run(run, launched == Launch::busy);
}

std::optional<TaskRecord> ServerCore::task(std::string_view id, const TaskOwner& owner) {
    if (!tasks_) return std::nullopt;
    return tasks_->read(id, owner);
}

std::optional<TaskRecord> ServerCore::current_task(std::string_view id, const TaskOwner& owner) {
    if (!tasks_) return std::nullopt;
    return tasks_->read_now(id, owner);
}

std::optional<TaskPage> ServerCore::list_tasks(const TaskOwner& owner, std::optional<std::string_view> cursor) {
    if (!tasks_) return std::nullopt;
    return tasks_->list(owner, cursor);
}

TaskCancelResult ServerCore::cancel_task(std::string_view id, const TaskOwner& owner) {
    if (!tasks_) return {};
    return tasks_->cancel(id, owner);
}

AnswersResult ServerCore::answer_task(std::string_view id, const TaskOwner& owner, json answers,
                                      InputResponses responses) {
    if (!tasks_) return {};
    TaskRunner::Answered answered = tasks_->answer(id, owner, std::move(answers), std::move(responses));
    if (answered.resume) resume_task(answered.run, std::move(*answered.resume));
    return std::move(answered.result);
}

TaskWatchStart ServerCore::watch_task(std::string_view id, const TaskOwner& owner,
                                      std::weak_ptr<TaskListener> listener) {
    if (!tasks_) return {};
    return tasks_->watch(id, owner, std::move(listener));
}

void ServerCore::arm_sweep() {
    {
        const std::lock_guard<std::mutex> lock(io_mutex_);
        if (!timers_open_) return;
    }
    // shutdown() posts the timer's cancellation after it closed the timers, so a timer armed here is cancelled.
    sweep_timer_->expires_after(tasks_->sweep_interval());
    sweep_timer_->async_wait([this](const std::error_code& error) {
        if (error) return;
        try {
            tasks_->sweep();
        } catch (...) {
            // A store that failed: the next sweep tries again.
        }
        arm_sweep();
    });
}

ResourceHost* ServerCore::resource_host() noexcept {
    serving_.store(true);
    return resource_defs_.empty() && template_defs_.empty() ? nullptr : this;
}

std::span<const ResourceDefinition> ServerCore::resources() const {
    serving_.store(true);
    return resource_defs_;
}

std::span<const ResourceTemplateDefinition> ServerCore::resource_templates() const {
    serving_.store(true);
    return template_defs_;
}

ListSettings ServerCore::resource_list_settings() const {
    ListSettings settings;
    settings.page_size = options_.resources_page_size;
    settings.cache = options_.resources_cache;
    return settings;
}

std::optional<ServerCore::Found> ServerCore::find_resource(std::string_view uri) const {
    // A resource registered at the URI wins over every template; among templates the first registered that matches.
    if (const auto found = resource_by_uri_.find(uri); found != resource_by_uri_.end()) {
        const std::size_t index = found->second;
        if (resource_on_[index].load()) {
            return Found{resource_slots_[index].entry.get(), json(nullptr), resource_defs_[index].mime_type,
                         resource_slots_[index].cache};
        }
    }
    const std::string text(uri);
    for (std::size_t index = 0; index < template_slots_.size(); ++index) {
        if (!template_on_[index].load()) continue;
        const TemplateSlot& slot = template_slots_[index];
        const auto bindings = slot.matcher.match(text);
        if (!bindings) continue;
        json arguments = json::object();
        bool decodable = true;
        for (const auto& [name, value_text] : *bindings) {
            const auto member = std::find_if(slot.members.begin(), slot.members.end(),
                                             [&](const TemplateMember& candidate) { return candidate.name == name; });
            auto value = member == slot.members.end() ? std::nullopt : variable_value(member->kind, value_text);
            if (!value) {
                decodable = false;
                break;
            }
            arguments[name] = std::move(*value);
        }
        // A match whose variables are not of the member's kind, or that lacks one the struct requires, is no match.
        if (!decodable || !slot.check(arguments)) continue;
        return Found{slot.entry.get(), std::move(arguments), template_defs_[index].mime_type, slot.cache};
    }
    return std::nullopt;
}

bool ServerCore::offers(std::string_view uri) const {
    serving_.store(true);
    return find_resource(uri).has_value();
}

std::span<const std::string> ServerCore::read_scopes(std::string_view uri) const {
    const auto found = find_resource(uri);
    if (!found) return {};
    return found->entry->scopes;
}

std::span<const std::string> ServerCore::prompt_scopes(std::size_t index) const {
    return prompt_entries_.at(index)->scopes;
}

void ServerCore::read(const ReadRequest& request, CallControl control, Completion done) {
    serving_.store(true);
    auto found = find_resource(request.uri);
    if (!found) {
        done(FeatureOutcome(NotFound{request.uri}));
        return;
    }
    admit(*found->entry, std::move(found->arguments), request.meta, std::move(control), std::move(done),
          Answers{request.input_responses, request.request_state}, request.uri, std::move(found->mime), found->cache);
}

PromptHost* ServerCore::prompt_host() noexcept {
    serving_.store(true);
    return prompt_defs_.empty() ? nullptr : this;
}

std::span<const PromptDefinition> ServerCore::prompts() const {
    serving_.store(true);
    return prompt_defs_;
}

ListSettings ServerCore::prompt_list_settings() const {
    ListSettings settings;
    settings.page_size = options_.prompts_page_size;
    settings.cache = options_.prompts_cache;
    return settings;
}

void ServerCore::get(const PromptRequest& request, CallControl control, Completion done) {
    serving_.store(true);
    const auto found = prompt_by_name_.find(request.name);
    if (found == prompt_by_name_.end()) {
        done(FeatureOutcome(InvalidRequest{"unknown prompt: " + request.name}));
        return;
    }
    json arguments = json::object();
    for (const auto& [name, value] : request.arguments) arguments[name] = value;
    admit(*prompt_entries_[found->second], std::move(arguments), request.meta, std::move(control), std::move(done),
          Answers{request.input_responses, request.request_state}, request.name);
}

CompletionHost* ServerCore::completion_host() noexcept {
    serving_.store(true);
    return completer_count_ == 0 ? nullptr : this;
}

std::optional<ServerCore::CompletionTarget> ServerCore::completion_target(const CompletionRequest& request) const {
    if (request.kind == CompletionRefKind::prompt) {
        const auto found = prompt_by_name_.find(request.reference);
        // A prompt that is off is one the server lacks.
        if (found == prompt_by_name_.end() || !prompt_on_[found->second].load()) return std::nullopt;
        return CompletionTarget{&prompt_completers_[found->second], prompt_entries_[found->second].get()};
    }
    // A resource template is named by its text; a resource at a fixed URI is a reference too, with nothing to
    // complete.
    const auto found =
        std::find_if(template_defs_.begin(), template_defs_.end(),
                     [&](const ResourceTemplateDefinition& candidate) {
                         return candidate.uri_template == request.reference;
                     });
    const auto fixed = resource_by_uri_.find(request.reference);
    const auto index = static_cast<std::size_t>(found - template_defs_.begin());
    if (found != template_defs_.end() && template_on_[index].load()) {
        return CompletionTarget{&template_completers_[index], template_slots_[index].entry.get()};
    }
    // A template or a resource that is off is one the server lacks.
    if (fixed == resource_by_uri_.end() || !resource_on_[fixed->second].load()) return std::nullopt;
    return CompletionTarget{nullptr, resource_slots_[fixed->second].entry.get()};
}

std::span<const std::string> ServerCore::completion_scopes(const CompletionRequest& request) const {
    const auto target = completion_target(request);
    if (!target) return {};
    return target->owner->scopes;
}

void ServerCore::complete(const CompletionRequest& request, CallControl control, Completion done) {
    serving_.store(true);
    const auto target = completion_target(request);
    if (!target) {
        done(FeatureOutcome(InvalidRequest{request.kind == CompletionRefKind::prompt
                                               ? "unknown prompt: " + request.reference
                                               : "unknown resource reference: " + request.reference}));
        return;
    }
    if (!permits(control, target->owner->scopes)) {
        done(HostFailure{"the authorization lacks a scope of " + target->owner->label()});
        return;
    }
    const Completers* completers = target->completers;
    const auto completer = completers == nullptr ? std::nullopt : std::optional(completers->find(request.argument));
    if (!completer || *completer == completers->end()) {
        done(FeatureOutcome(CompletionResult{}));
        return;
    }
    json arguments = json::object();
    arguments["value"] = request.value;
    json resolved = json::object();
    for (const auto& [name, value] : request.resolved) resolved[name] = value;
    arguments["arguments"] = std::move(resolved);
    admit(*(*completer)->second, std::move(arguments), request.meta, std::move(control), std::move(done), Answers{},
          request.reference);
}

void ServerCore::post_cancel(const std::weak_ptr<CallRecord>& record) {
    const std::lock_guard<std::mutex> lock(io_mutex_);
    if (!io_open_) return;
    asio::post(io_, [record] {
        const auto held = record.lock();
        if (held && !held->finished) held->signal.emit(asio::cancellation_type::terminal);
    });
}

void ServerCore::finish(const std::shared_ptr<CallRecord>& record, JobOutcome outcome) {
    // The call leaves the calls in flight, and a coroutine call gives back its place, before the answer is written:
    // a client that has the answer may start another call at once. Only the first finish() finds the call there.
    bool stopping = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_.erase(record->serial) != 0 && record->coroutine) --coroutines_;
        stopping = stopping_;
    }
    if (record->task) {
        settle_task(record->task, std::move(outcome), stopping);
        return;
    }
    if (auto* read = std::get_if<ReadResult>(&outcome)) {
        // A result that carries no hints of its own gets the resource's, else the server's default for reads.
        if (!read->cache) read->cache = record->cache ? *record->cache : options_.resource_read_cache;
    }
    std::visit(
        [&](auto&& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ToolResult> || std::is_same_v<T, Refusal> || std::is_same_v<T, HostFailure> ||
                          std::is_same_v<T, InputRequired>) {
                (*record->done)(CallOutcome(std::move(value)));
            } else {
                (*record->done)(FeatureOutcome(std::move(value)));
            }
        },
        std::move(outcome));
}

// ---------------------------------------------------------------------------------------------------------------
// Running a call

JobOutcome Callable::run_plain(const CallRecord& record, Context& context) {
    try {
        return do_plain(record, context);
    } catch (...) {
        return failure(record, std::current_exception());
    }
}

asio::awaitable<JobOutcome> Callable::run_async(const CallRecord& record, Context& context) {
    JobOutcome outcome = HostFailure{};
    try {
        outcome = co_await do_async(record, context);
    } catch (...) {
        outcome = failure(record, std::current_exception());
    }
    co_return outcome;
}

JobOutcome ToolEntry::failure(const CallRecord&, const std::exception_ptr& thrown) const {
    const auto failed = [&](const char* what) {
        std::string text = "The tool failed";
        if (options.expose_error_details) {
            text += ": ";
            text += what != nullptr ? what : "an exception that is not a std::exception";
        }
        return error_result(std::move(text));
    };
    try {
        std::rethrow_exception(thrown);
    } catch (const AnswerRefused& refused) {
        // Answers to declared inputs that cannot be used: Invalid params, as for a malformed answer.
        return InvalidRequest{refused.what()};
    } catch (const ToolError& error) {
        return error_result(error.what());
    } catch (const std::exception& error) {
        return failed(error.what());
    } catch (...) {
        return failed(nullptr);
    }
}

JobOutcome ToolEntry::settle(ToolResult result) const {
    if (!output || result.is_error) return result;
    if (!result.structured_content) {
        return HostFailure{name_label + " has an output schema and its result has no structured content"};
    }
    if (auto issue = output->validate(*result.structured_content)) {
        return HostFailure{"the structured content of " + name_label + " does not match its output schema: " +
                           describe_issue("structuredContent", *issue)};
    }
    return result;
}

JobOutcome ToolEntry::do_plain(const CallRecord& record, Context& context) {
    ToolOutcome outcome = std::get<std::unique_ptr<PlainBody>>(body)->run(record.arguments, context);
    if (auto* asked = std::get_if<InputRequired>(&outcome)) return JobOutcome(std::move(*asked));
    return settle(std::get<ToolResult>(std::move(outcome)));
}

asio::awaitable<JobOutcome> ToolEntry::do_async(const CallRecord& record, Context& context) {
    ToolOutcome outcome = co_await std::get<std::unique_ptr<CoroutineBody>>(body)->run(record.arguments, context);
    if (auto* asked = std::get_if<InputRequired>(&outcome)) co_return JobOutcome(std::move(*asked));
    co_return settle(std::get<ToolResult>(std::move(outcome)));
}

template <class Out>
JobOutcome FeatureEntry<Out>::do_plain(const CallRecord& record, Context& context) {
    const Invocation invocation{record.arguments, record.subject, record.mime};
    Produced<Out> produced = std::get<std::unique_ptr<FeaturePlainBody<Out>>>(body)->run(invocation, context);
    if (auto* asked = std::get_if<InputRequired>(&produced)) return JobOutcome(std::move(*asked));
    return JobOutcome(std::get<Out>(std::move(produced)));
}

template <class Out>
asio::awaitable<JobOutcome> FeatureEntry<Out>::do_async(const CallRecord& record, Context& context) {
    const Invocation invocation{record.arguments, record.subject, record.mime};
    Produced<Out> produced = co_await std::get<std::unique_ptr<FeatureCoroutineBody<Out>>>(body)->run(invocation, context);
    if (auto* asked = std::get_if<InputRequired>(&produced)) co_return JobOutcome(std::move(*asked));
    co_return JobOutcome(std::get<Out>(std::move(produced)));
}

template <class Out>
JobOutcome FeatureEntry<Out>::failure(const CallRecord& record, const std::exception_ptr& thrown) const {
    try {
        std::rethrow_exception(thrown);
    } catch (const AnswerRefused& refused) {
        return InvalidRequest{refused.what()};
    } catch (const ResourceNotFound&) {
        return NotFound{record.subject};
    } catch (const std::exception& error) {
        return HostFailure{name_label + " failed: " + error.what()};
    } catch (...) {
        return HostFailure{name_label + " failed with an exception that is not a std::exception"};
    }
}

void ServerCore::awaiting_input(InputWait wait) {
    auto timer = std::make_shared<InputTimer>(io_, std::move(wait));
    {
        const std::lock_guard<std::mutex> lock(io_mutex_);
        if (!io_open_ || !timers_open_) return;
        timer->serial = next_timer_++;
        // Armed here, before anything can cancel it: a cancellation is posted to the io thread, after this.
        timer->timer.expires_after(options_.input_timeout);
        timer->timer.async_wait([this, timer](const std::error_code& error) { on_timer(timer, error); });
        timers_.emplace(timer->serial, timer);
    }
    // Runs at once when the wait has ended already.
    timer->on_end.emplace(timer->wait.ended(), TimerCancel{this, timer});
}

void ServerCore::on_timer(const std::shared_ptr<InputTimer>& timer, const std::error_code& error) {
    {
        const std::lock_guard<std::mutex> lock(io_mutex_);
        timers_.erase(timer->serial);
    }
    // A wait that ended meanwhile ignores expire(); cancelled timers do not call it at all.
    if (!error) timer->wait.expire();
}

void ServerCore::cancel_timer(const std::weak_ptr<InputTimer>& timer) {
    const std::lock_guard<std::mutex> lock(io_mutex_);
    cancel_timer_locked(timer);
}

void ServerCore::cancel_timer_locked(const std::weak_ptr<InputTimer>& timer) {
    if (!io_open_) return;
    asio::post(io_, [timer] {
        if (const auto held = timer.lock()) held->timer.cancel();
    });
}

void ServerCore::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;  // shutdown() answers what is still queued
        std::shared_ptr<CallRecord> record = std::move(queue_.front());
        queue_.pop_front();
        ++running_;
        lock.unlock();
        try {
            run_plain(record);
        } catch (...) {
            // Completing failed; the call is answered when its completion is destroyed.
        }
        record.reset();  // its stop callbacks are deregistered without the lock
        lock.lock();
        --running_;
    }
}

void ServerCore::run_plain(const std::shared_ptr<CallRecord>& record) {
    if (record->stop.stop_requested()) {
        finish(record, HostFailure{record->callable->label() + " was stopped before it ran"});
        return;
    }
    JobOutcome outcome = HostFailure{};
    {
        Context context(record->control, record->meta, record->stop.get_token(), record->answers.responses,
                        record->answers.state, record->task.get());
        outcome = record->callable->run_plain(*record, context);
    }
    finish(record, std::move(outcome));
}

asio::awaitable<void> ServerCore::drive(std::shared_ptr<CallRecord> record) {
    JobOutcome outcome = HostFailure{record->callable->label() + " was stopped before it ran"};
    try {
        // Begin on a later turn of the io thread. Spawning dispatches, which runs inline on the io thread itself, and
        // nothing of the call may run inside call().
        co_await asio::post(io_, asio::use_awaitable);
        if (!record->stop.stop_requested()) {
            Context context(record->control, record->meta, record->stop.get_token(), record->answers.responses,
                            record->answers.state, record->task.get());
            outcome = co_await record->callable->run_async(*record, context);
        }
    } catch (...) {
        // The first resumption throws when the call was cancelled before the tool started; the outcome says so.
    }
    // The signal's slot keeps the handler of this coroutine after it ended, so no cancellation may be emitted from
    // here on.
    record->finished = true;
    finish(record, std::move(outcome));
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Context

void Context::progress(double value, std::optional<double> total, std::optional<std::string> message) const {
    control_.progress(value, total, std::move(message));
}

void Context::log(LogLevel level, json data, std::optional<std::string> logger) const {
    control_.log(level, std::move(data), std::move(logger));
}

void Context::debug(json data, std::optional<std::string> logger) const {
    log(LogLevel::debug, std::move(data), std::move(logger));
}

void Context::info(json data, std::optional<std::string> logger) const {
    log(LogLevel::info, std::move(data), std::move(logger));
}

void Context::warning(json data, std::optional<std::string> logger) const {
    log(LogLevel::warning, std::move(data), std::move(logger));
}

void Context::error(json data, std::optional<std::string> logger) const {
    log(LogLevel::error, std::move(data), std::move(logger));
}

const std::string& Context::revision() const noexcept { return control_.profile().revision(); }
profile::Era Context::era() const noexcept { return control_.profile().era(); }
const json& Context::client_info() const noexcept { return control_.client_info(); }
const json& Context::client_capabilities() const noexcept { return control_.client_capabilities(); }
const Authorization* Context::authorization() const noexcept { return control_.authorization(); }
bool Context::client_supports(const InputRequest& request) const { return control_.client_supports(request); }

// ---------------------------------------------------------------------------------------------------------------
// ToolHandle

namespace {

ToolAnnotations& annotations_of(ToolDefinition& tool) {
    if (!tool.annotations) tool.annotations.emplace();
    return *tool.annotations;
}

}  // namespace

ToolHandle ToolHandle::title(std::string text) const {
    core_->edit(index_, [&](ToolDefinition& tool) {
        tool.title = text;
        annotations_of(tool).title = std::move(text);
    });
    return *this;
}

ToolHandle ToolHandle::read_only(bool value) const {
    core_->edit(index_, [&](ToolDefinition& tool) { annotations_of(tool).read_only_hint = value; });
    return *this;
}

ToolHandle ToolHandle::destructive(bool value) const {
    core_->edit(index_, [&](ToolDefinition& tool) { annotations_of(tool).destructive_hint = value; });
    return *this;
}

ToolHandle ToolHandle::idempotent(bool value) const {
    core_->edit(index_, [&](ToolDefinition& tool) { annotations_of(tool).idempotent_hint = value; });
    return *this;
}

ToolHandle ToolHandle::open_world(bool value) const {
    core_->edit(index_, [&](ToolDefinition& tool) { annotations_of(tool).open_world_hint = value; });
    return *this;
}

ToolHandle ToolHandle::meta(json value) const {
    core_->edit(index_, [&](ToolDefinition& tool) {
        if (!value.is_object()) {
            throw std::invalid_argument("neomcp: tool '" + tool.name + "': _meta must be an object");
        }
        tool.meta = std::move(value);
    });
    return *this;
}

ToolHandle ToolHandle::icons(std::vector<Icon> list) const {
    core_->edit(index_, [&](ToolDefinition& tool) { tool.icons = std::move(list); });
    return *this;
}

ToolHandle ToolHandle::scopes(std::vector<std::string> list) const {
    core_->set_tool_scopes(index_, std::move(list));
    return *this;
}

ToolHandle ToolHandle::task_support(TaskSupport mode) const {
    core_->set_task_support(index_, mode);
    return *this;
}

ToolHandle ToolHandle::enable(bool on) const {
    core_->enable_tool(index_, on);
    return *this;
}

bool ToolHandle::enabled() const { return core_->tool_enabled(index_); }

const std::string& ToolHandle::name() const { return core_->definition(index_).name; }

// ---------------------------------------------------------------------------------------------------------------
// PromptHandle

PromptHandle PromptHandle::title(std::string text) const {
    core_->edit_prompt(index_, [&](PromptDefinition& prompt) { prompt.title = std::move(text); });
    return *this;
}

PromptHandle PromptHandle::icons(std::vector<Icon> list) const {
    core_->edit_prompt(index_, [&](PromptDefinition& prompt) { prompt.icons = std::move(list); });
    return *this;
}

PromptHandle PromptHandle::meta(json value) const {
    core_->edit_prompt(index_, [&](PromptDefinition& prompt) {
        if (!value.is_object()) throw std::invalid_argument("neomcp: prompt '" + prompt.name + "': _meta must be an object");
        prompt.meta = std::move(value);
    });
    return *this;
}

void PromptHandle::add_completer(std::string argument, detail::FeatureBody<CompletionResult> body) const {
    core_->add_prompt_completer(index_, std::move(argument), std::move(body));
}

void ResourceTemplateHandle::add_completer(std::string variable, detail::FeatureBody<CompletionResult> body) const {
    core_->add_template_completer(index_, std::move(variable), std::move(body));
}

PromptHandle PromptHandle::scopes(std::vector<std::string> list) const {
    core_->set_prompt_scopes(index_, std::move(list));
    return *this;
}

PromptHandle PromptHandle::enable(bool on) const {
    core_->enable_prompt(index_, on);
    return *this;
}

bool PromptHandle::enabled() const { return core_->prompt_enabled(index_); }

const std::string& PromptHandle::name() const { return core_->prompt_definition(index_).name; }

// ---------------------------------------------------------------------------------------------------------------
// ResourceHandle and ResourceTemplateHandle

namespace {

void require_meta_object(const json& value, const std::string& label) {
    if (!value.is_object()) throw std::invalid_argument("neomcp: " + label + ": _meta must be an object");
}

}  // namespace

ResourceHandle ResourceHandle::title(std::string text) const {
    core_->edit_resource(index_, [&](ResourceDefinition& resource) { resource.title = std::move(text); });
    return *this;
}

ResourceHandle ResourceHandle::description(std::string text) const {
    core_->edit_resource(index_, [&](ResourceDefinition& resource) { resource.description = std::move(text); });
    return *this;
}

ResourceHandle ResourceHandle::mime_type(std::string type) const {
    core_->edit_resource(index_, [&](ResourceDefinition& resource) { resource.mime_type = std::move(type); });
    return *this;
}

ResourceHandle ResourceHandle::size(std::uint64_t bytes) const {
    core_->edit_resource(index_, [&](ResourceDefinition& resource) { resource.size = bytes; });
    return *this;
}

ResourceHandle ResourceHandle::annotations(ContentAnnotations value) const {
    core_->edit_resource(index_, [&](ResourceDefinition& resource) { resource.annotations = std::move(value); });
    return *this;
}

ResourceHandle ResourceHandle::icons(std::vector<Icon> list) const {
    core_->edit_resource(index_, [&](ResourceDefinition& resource) { resource.icons = std::move(list); });
    return *this;
}

ResourceHandle ResourceHandle::meta(json value) const {
    core_->edit_resource(index_, [&](ResourceDefinition& resource) {
        require_meta_object(value, "resource '" + resource.uri + "'");
        resource.meta = std::move(value);
    });
    return *this;
}

ResourceHandle ResourceHandle::cache(CacheHints hints) const {
    core_->set_resource_cache(index_, hints);
    return *this;
}

ResourceHandle ResourceHandle::scopes(std::vector<std::string> list) const {
    core_->set_resource_scopes(index_, std::move(list));
    return *this;
}

ResourceHandle ResourceHandle::enable(bool on) const {
    core_->enable_resource(index_, on);
    return *this;
}

bool ResourceHandle::enabled() const { return core_->resource_enabled(index_); }

const std::string& ResourceHandle::uri() const { return core_->resource_definition(index_).uri; }

ResourceTemplateHandle ResourceTemplateHandle::title(std::string text) const {
    core_->edit_resource_template(index_, [&](ResourceTemplateDefinition& resource) { resource.title = std::move(text); });
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::description(std::string text) const {
    core_->edit_resource_template(index_,
                                  [&](ResourceTemplateDefinition& resource) { resource.description = std::move(text); });
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::mime_type(std::string type) const {
    core_->edit_resource_template(index_, [&](ResourceTemplateDefinition& resource) { resource.mime_type = std::move(type); });
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::annotations(ContentAnnotations value) const {
    core_->edit_resource_template(index_,
                                  [&](ResourceTemplateDefinition& resource) { resource.annotations = std::move(value); });
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::icons(std::vector<Icon> list) const {
    core_->edit_resource_template(index_, [&](ResourceTemplateDefinition& resource) { resource.icons = std::move(list); });
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::meta(json value) const {
    core_->edit_resource_template(index_, [&](ResourceTemplateDefinition& resource) {
        require_meta_object(value, "resource template '" + resource.uri_template + "'");
        resource.meta = std::move(value);
    });
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::cache(CacheHints hints) const {
    core_->set_template_cache(index_, hints);
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::scopes(std::vector<std::string> list) const {
    core_->set_template_scopes(index_, std::move(list));
    return *this;
}

ResourceTemplateHandle ResourceTemplateHandle::enable(bool on) const {
    core_->enable_template(index_, on);
    return *this;
}

bool ResourceTemplateHandle::enabled() const { return core_->template_enabled(index_); }

const std::string& ResourceTemplateHandle::uri_template() const { return core_->template_definition(index_).uri_template; }


// ---------------------------------------------------------------------------------------------------------------
// Server

Server::Server(ServerOptions options) : core_(std::make_shared<detail::ServerCore>(std::move(options))) {}

Server::~Server() { core_->shutdown(); }

ToolHandle Server::add(ToolDefinition definition, detail::Body body, std::vector<HeaderMark> marks) {
    return ToolHandle(core_.get(), core_->add(std::move(definition), std::move(body), std::move(marks)));
}

std::variant<std::unique_ptr<Session>, CreateError> Server::connect(Sink sink, IdleCallback on_idle) {
    return core_->connect(std::move(sink), std::move(on_idle));
}

std::variant<std::unique_ptr<Session>, CreateError> Server::connect_tagged(profile::Transport transport,
                                                                           OutputSink sink, IdleCallback on_idle) {
    return core_->connect_tagged(transport, std::move(sink), std::move(on_idle));
}

std::span<const ToolDefinition> Server::tools() const { return core_->tools(); }

bool Server::tool_enabled(std::size_t index) const { return core_->tool_enabled(index); }

std::span<const std::string> Server::tool_scopes(std::size_t index) const { return core_->tool_scopes(index); }

bool Server::declares_scopes() const { return core_->declares_scopes(); }

TaskSupport Server::task_support(std::size_t index) const { return core_->task_support(index); }

TaskHost* Server::task_host() noexcept { return core_->task_host(); }

TaskStartResult Server::start_task(const CallRequest& request, CallControl origin, TaskStart start) {
    return core_->start_task(request, std::move(origin), std::move(start));
}

std::optional<TaskRecord> Server::task(std::string_view id, const TaskOwner& owner) { return core_->task(id, owner); }

std::optional<TaskRecord> Server::current_task(std::string_view id, const TaskOwner& owner) {
    return core_->current_task(id, owner);
}

std::optional<TaskPage> Server::list_tasks(const TaskOwner& owner, std::optional<std::string_view> cursor) {
    return core_->list_tasks(owner, cursor);
}

TaskCancelResult Server::cancel_task(std::string_view id, const TaskOwner& owner) {
    return core_->cancel_task(id, owner);
}

AnswersResult Server::answer_task(std::string_view id, const TaskOwner& owner, json answers,
                                  InputResponses responses) {
    return core_->answer_task(id, owner, std::move(answers), std::move(responses));
}

TaskWatchStart Server::watch_task(std::string_view id, const TaskOwner& owner, std::weak_ptr<TaskListener> listener) {
    return core_->watch_task(id, owner, std::move(listener));
}

ChangeSupport Server::change_support() const { return core_->change_support(); }

ChangeRegistration Server::watch(std::weak_ptr<ChangeListener> listener) { return core_->watch(std::move(listener)); }

void Server::notify_resource_updated(std::string uri) {
    core_->report(Change{ChangeKind::resource_updated, std::move(uri)});
}

std::size_t Server::change_watchers() const { return core_->watchers(); }

void Server::call(const CallRequest& request, CallControl control, Completion done) {
    core_->call(request, std::move(control), std::move(done));
}

const ServerOptions& Server::options() const noexcept { return core_->options(); }

ResourceHost* Server::resource_host() noexcept { return core_->resource_host(); }

PromptHost* Server::prompt_host() noexcept { return core_->prompt_host(); }

CompletionHost* Server::completion_host() noexcept { return core_->completion_host(); }

PromptHandle Server::add_prompt(PromptDefinition definition, detail::FeatureBody<PromptResult> body) {
    return PromptHandle(core_.get(), core_->add_prompt(std::move(definition), std::move(body)));
}

ResourceHandle Server::add_resource(ResourceDefinition definition, detail::FeatureBody<ReadResult> body) {
    return ResourceHandle(core_.get(), core_->add_resource(std::move(definition), std::move(body)));
}

ResourceTemplateHandle Server::add_resource_template(ResourceTemplateDefinition definition,
                                                     detail::FeatureBody<ReadResult> body,
                                                     std::vector<detail::TemplateMember> members,
                                                     detail::TemplateCheck check) {
    return ResourceTemplateHandle(core_.get(), core_->add_resource_template(std::move(definition), std::move(body),
                                                                            std::move(members), std::move(check)));
}

}  // namespace neomcp
