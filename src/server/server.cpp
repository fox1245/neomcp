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
// - The destructor sets the stopping flag (so nothing new is admitted), answers the calls still queued, requests
//   stop on every call in flight while holding no lock, joins the workers, lets the io_context run out of work and
//   joins its thread. It cannot deadlock on a handler that waits for its stop token: the stop is requested before
//   any join, from a thread that holds nothing a handler or a stop callback needs.
#include "neomcp/server.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
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

// Something a call can run: a registered tool. A call runs it on a worker (plain) or on the coroutine thread, and the
// run never throws: what the handler throws becomes an outcome.
class Callable {
public:
    virtual ~Callable() = default;
    virtual bool coroutine() const noexcept = 0;
    // What the thing is called in the failures the server reports, such as "tool 'add'".
    virtual const std::string& label() const noexcept = 0;

    CallOutcome run_plain(const CallRecord& record, Context& context);
    asio::awaitable<CallOutcome> run_async(const CallRecord& record, Context& context);

protected:
    virtual CallOutcome do_plain(const CallRecord& record, Context& context) = 0;
    virtual asio::awaitable<CallOutcome> do_async(const CallRecord& record, Context& context) = 0;
    // The outcome for an exception the handler threw (or for a result that cannot be used).
    virtual CallOutcome failure(const std::exception_ptr& thrown) const = 0;
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

protected:
    CallOutcome do_plain(const CallRecord& record, Context& context) override;
    asio::awaitable<CallOutcome> do_async(const CallRecord& record, Context& context) override;
    CallOutcome failure(const std::exception_ptr& thrown) const override;

private:
    CallOutcome settle(ToolResult result) const;
};

// One call, from admission until it is answered.
struct CallRecord {
    CallRecord(Callable& callable_to_run, CallControl call_control, Completion completion, json call_arguments,
               json call_meta)
        : callable(&callable_to_run),
          control(std::move(call_control)),
          done(std::move(completion)),
          arguments(std::move(call_arguments)),
          meta(std::move(call_meta)) {}

    Callable* callable;
    CallControl control;
    Completion done;
    json arguments;
    json meta;
    std::uint64_t serial = 0;
    bool coroutine = false;  // set before the call is admitted

    // The handler's stop source, and the callbacks that request it; the callbacks are destroyed first.
    std::stop_source stop;
    std::optional<std::stop_callback<StopForward>> follow;  // the session's stop

    // Coroutine calls only. The signal is used on the io thread after the coroutine was spawned.
    asio::cancellation_signal signal;
    bool finished = false;  // io thread only: the coroutine produced its outcome
    std::optional<std::stop_callback<CancelPoster>> poster;
};

class ServerCore final : public ToolHost, public std::enable_shared_from_this<ServerCore> {
public:
    explicit ServerCore(ServerOptions options);
    ~ServerCore() override;
    ServerCore(const ServerCore&) = delete;
    ServerCore& operator=(const ServerCore&) = delete;

    // Stops and joins everything; idempotent.
    void shutdown();

    std::size_t add(ToolDefinition definition, Body body);
    void edit(std::size_t index, const std::function<void(ToolDefinition&)>& change);
    const ToolDefinition& definition(std::size_t index) const { return definitions_.at(index); }

    std::variant<std::unique_ptr<Session>, CreateError> connect(Sink sink, IdleCallback on_idle);
    std::span<const ToolDefinition> tools() const override;
    void call(const CallRequest& request, CallControl control, Completion done) override;
    std::optional<std::string> check(const ToolDefinition& tool, const json& arguments) const;

    const ServerOptions& options() const noexcept { return options_; }

    void post_cancel(const std::weak_ptr<CallRecord>& record);
    void finish(const std::shared_ptr<CallRecord>& record, CallOutcome outcome);

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
    void admit(Callable& callable, json arguments, json meta, CallControl control, Completion done);

    json_schema::Validator compile(const std::string& tool, std::string_view which, const json& schema) const;
    void ensure_registering(const std::string& tool, std::string_view action) const;
    bool admits() const;  // with mutex_ held

    void work();
    void run_io();
    void run_plain(const std::shared_ptr<CallRecord>& record);
    asio::awaitable<void> drive(std::shared_ptr<CallRecord> record);

    // Immutable once serving began.
    ServerOptions options_;
    std::vector<ToolDefinition> definitions_;
    std::vector<std::unique_ptr<ToolEntry>> entries_;
    std::map<std::string, std::size_t, std::less<>> by_name_;
    mutable std::atomic<bool> serving_{false};

    // Guarded by mutex_.
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::deque<std::shared_ptr<CallRecord>> queue_;
    std::size_t running_ = 0;
    std::size_t coroutines_ = 0;  // coroutine calls in flight
    bool stopping_ = false;
    std::uint64_t next_serial_ = 0;
    std::map<std::uint64_t, std::shared_ptr<CallRecord>> in_flight_;

    std::vector<std::thread> workers_;

    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> io_work_;
    std::thread io_thread_;
    std::mutex io_mutex_;  // guards io_open_
    bool io_open_ = true;  // cancellations may still be posted
};

void CancelPoster::operator()() const noexcept { core->post_cancel(record); }

// ---------------------------------------------------------------------------------------------------------------
// Lifetime

ServerCore::ServerCore(ServerOptions options) : options_(std::move(options)) {
    if (options_.max_concurrent_calls == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_concurrent_calls is zero");
    }
    if (options_.max_coroutine_calls == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::max_coroutine_calls is zero");
    }
    io_work_.emplace(asio::make_work_guard(io_));
    try {
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
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
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

void ServerCore::ensure_registering(const std::string& tool, std::string_view action) const {
    if (serving_.load()) {
        throw std::logic_error("neomcp: tool '" + tool + "' cannot be " + std::string(action) +
                               ": the server serves already");
    }
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

std::size_t ServerCore::add(ToolDefinition definition, Body body) {
    const std::string name = definition.name;
    ensure_registering(name, "registered");
    if (name.empty()) throw std::invalid_argument("neomcp: a tool needs a name");
    if (by_name_.find(name) != by_name_.end()) {
        throw std::invalid_argument("neomcp: a tool named '" + name + "' is registered already");
    }
    if (definition.meta && !definition.meta->is_object()) {
        throw std::invalid_argument("neomcp: tool '" + name + "': _meta must be an object");
    }
    auto entry = std::make_unique<ToolEntry>(options_, std::move(body), "tool '" + name + "'");
    entry->input = compile(name, "input", definition.input_schema);
    if (definition.output_schema) entry->output = compile(name, "output", *definition.output_schema);
    const std::size_t index = definitions_.size();
    definitions_.push_back(std::move(definition));
    entries_.push_back(std::move(entry));
    by_name_.emplace(name, index);
    return index;
}

void ServerCore::edit(std::size_t index, const std::function<void(ToolDefinition&)>& change) {
    ToolDefinition& tool = definitions_.at(index);
    ensure_registering(tool.name, "changed");
    change(tool);
}

// ---------------------------------------------------------------------------------------------------------------
// Serving

std::variant<std::unique_ptr<Session>, CreateError> ServerCore::connect(Sink sink, IdleCallback on_idle) {
    serving_.store(true);
    SessionOptions session;
    session.profiles = options_.profiles;
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
    session.expose_error_details = options_.expose_error_details;
    session.on_idle = std::move(on_idle);
    // The session keeps this, and with it the core: the core outlives every session it serves.
    session.check_arguments = [core = shared_from_this()](const ToolDefinition& tool, const json& arguments) {
        return core->check(tool, arguments);
    };
    return Session::create(std::move(session), *this, std::move(sink));
}

std::span<const ToolDefinition> ServerCore::tools() const {
    serving_.store(true);
    return definitions_;
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
    return queue_.size() < idle || queue_.size() - idle < options_.max_pending_calls;
}

void ServerCore::call(const CallRequest& request, CallControl control, Completion done) {
    serving_.store(true);
    const auto found = by_name_.find(request.name);
    if (found == by_name_.end()) {
        done(HostFailure{"the server has no tool named '" + request.name + "'"});
        return;
    }
    admit(*entries_[found->second], request.arguments, request.meta, std::move(control), std::move(done));
}

void ServerCore::admit(Callable& callable, json arguments, json meta, CallControl control, Completion done) {
    auto record = std::make_shared<CallRecord>(callable, std::move(control), std::move(done), std::move(arguments),
                                               std::move(meta));
    // Runs at once when the session already requested stop.
    record->follow.emplace(record->control.stop_token(), StopForward{&record->stop});
    const bool coroutine = callable.coroutine();
    record->coroutine = coroutine;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            lock.unlock();
            record->done(HostFailure{"the server has shut down"});
            return;
        }
        if (coroutine ? coroutines_ >= options_.max_coroutine_calls : !admits()) {
            lock.unlock();
            record->done(Refusal::busy);
            return;
        }
        record->serial = next_serial_++;
        in_flight_.emplace(record->serial, record);
        if (!coroutine) {
            queue_.push_back(record);
        } else {
            ++coroutines_;
            // Nothing of the call runs here (drive() begins by posting), so spawning may happen under the lock; it
            // does, so that shutdown() cannot let the io_context run out of work in between.
            try {
                asio::co_spawn(io_, drive(record),
                               asio::bind_cancellation_slot(record->signal.slot(), Spawned{this, record}));
            } catch (...) {
                in_flight_.erase(record->serial);
                --coroutines_;
                lock.unlock();
                record->done(HostFailure{record->callable->label() + " could not be started"});
                return;
            }
        }
    }
    if (!coroutine) {
        work_ready_.notify_one();
        return;
    }
    // After spawning: the cancellation it posts must find the coroutine's handler in the signal's slot.
    record->poster.emplace(record->stop.get_token(), CancelPoster{this, record});
}

void ServerCore::post_cancel(const std::weak_ptr<CallRecord>& record) {
    const std::lock_guard<std::mutex> lock(io_mutex_);
    if (!io_open_) return;
    asio::post(io_, [record] {
        const auto held = record.lock();
        if (held && !held->finished) held->signal.emit(asio::cancellation_type::terminal);
    });
}

void ServerCore::finish(const std::shared_ptr<CallRecord>& record, CallOutcome outcome) {
    // The call leaves the calls in flight, and a coroutine call gives back its place, before the answer is written:
    // a client that has the answer may start another call at once. Only the first finish() finds the call there.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_.erase(record->serial) != 0 && record->coroutine) --coroutines_;
    }
    record->done(std::move(outcome));
}

// ---------------------------------------------------------------------------------------------------------------
// Running a call

CallOutcome Callable::run_plain(const CallRecord& record, Context& context) {
    try {
        return do_plain(record, context);
    } catch (...) {
        return failure(std::current_exception());
    }
}

asio::awaitable<CallOutcome> Callable::run_async(const CallRecord& record, Context& context) {
    CallOutcome outcome = HostFailure{};
    try {
        outcome = co_await do_async(record, context);
    } catch (...) {
        outcome = failure(std::current_exception());
    }
    co_return outcome;
}

CallOutcome ToolEntry::failure(const std::exception_ptr& thrown) const {
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
    } catch (const ToolError& error) {
        return error_result(error.what());
    } catch (const std::exception& error) {
        return failed(error.what());
    } catch (...) {
        return failed(nullptr);
    }
}

CallOutcome ToolEntry::settle(ToolResult result) const {
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

CallOutcome ToolEntry::do_plain(const CallRecord& record, Context& context) {
    return settle(std::get<std::unique_ptr<PlainBody>>(body)->run(record.arguments, context));
}

asio::awaitable<CallOutcome> ToolEntry::do_async(const CallRecord& record, Context& context) {
    ToolResult result = co_await std::get<std::unique_ptr<CoroutineBody>>(body)->run(record.arguments, context);
    co_return settle(std::move(result));
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
    CallOutcome outcome = HostFailure{};
    {
        Context context(record->control, record->meta, record->stop.get_token());
        outcome = record->callable->run_plain(*record, context);
    }
    finish(record, std::move(outcome));
}

asio::awaitable<void> ServerCore::drive(std::shared_ptr<CallRecord> record) {
    CallOutcome outcome = HostFailure{record->callable->label() + " was stopped before it ran"};
    try {
        // Begin on a later turn of the io thread. Spawning dispatches, which runs inline on the io thread itself, and
        // nothing of the call may run inside call().
        co_await asio::post(io_, asio::use_awaitable);
        if (!record->stop.stop_requested()) {
            Context context(record->control, record->meta, record->stop.get_token());
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

const std::string& ToolHandle::name() const { return core_->definition(index_).name; }

// ---------------------------------------------------------------------------------------------------------------
// Server

Server::Server(ServerOptions options) : core_(std::make_shared<detail::ServerCore>(std::move(options))) {}

Server::~Server() { core_->shutdown(); }

ToolHandle Server::add(ToolDefinition definition, detail::Body body) {
    return ToolHandle(core_.get(), core_->add(std::move(definition), std::move(body)));
}

std::variant<std::unique_ptr<Session>, CreateError> Server::connect(Sink sink, IdleCallback on_idle) {
    return core_->connect(std::move(sink), std::move(on_idle));
}

std::span<const ToolDefinition> Server::tools() const { return core_->tools(); }

void Server::call(const CallRequest& request, CallControl control, Completion done) {
    core_->call(request, std::move(control), std::move(done));
}

const ServerOptions& Server::options() const noexcept { return core_->options(); }

}  // namespace neomcp
