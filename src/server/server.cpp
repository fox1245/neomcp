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

#include "neomcp/feature_host.h"
#include "neomcp/uri_template.h"

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

// What a call produced: any outcome a host can complete a call with.
using JobOutcome = std::variant<ToolResult, ReadResult, NotFound, InvalidRequest, Refusal, HostFailure>;

// Something a call can run: a registered tool, resource or resource template. A call runs it on a worker (plain) or on the coroutine thread, and the
// run never throws: what the handler throws becomes an outcome.
class Callable {
public:
    virtual ~Callable() = default;
    virtual bool coroutine() const noexcept = 0;
    // What the thing is called in the failures the server reports, such as "tool 'add'".
    virtual const std::string& label() const noexcept = 0;

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
    std::string subject;                 // what a read names: the uri
    std::optional<std::string> mime;      // the MIME type of the resource or template read
    std::optional<CacheHints> cache;     // the caching hints of the resource read, when it has its own
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

class ServerCore final : public ToolHost, public ResourceHost, public std::enable_shared_from_this<ServerCore> {
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

    std::size_t add_resource(ResourceDefinition definition, FeatureBody<ReadResult> body);
    std::size_t add_resource_template(ResourceTemplateDefinition definition, FeatureBody<ReadResult> body,
                                      std::vector<TemplateMember> members, TemplateCheck check);
    void edit_resource(std::size_t index, const std::function<void(ResourceDefinition&)>& change);
    void edit_resource_template(std::size_t index, const std::function<void(ResourceTemplateDefinition&)>& change);
    void set_resource_cache(std::size_t index, CacheHints hints);
    void set_template_cache(std::size_t index, CacheHints hints);
    const ResourceDefinition& resource_definition(std::size_t index) const { return resource_defs_.at(index); }
    const ResourceTemplateDefinition& template_definition(std::size_t index) const { return template_defs_.at(index); }

    std::variant<std::unique_ptr<Session>, CreateError> connect(Sink sink, IdleCallback on_idle);
    std::span<const ToolDefinition> tools() const override;
    void call(const CallRequest& request, CallControl control, Completion done) override;
    ResourceHost* resource_host() noexcept override;
    std::span<const ResourceDefinition> resources() const override;
    std::span<const ResourceTemplateDefinition> resource_templates() const override;
    ListSettings list_settings() const override;
    void read(const ReadRequest& request, CallControl control, Completion done) override;
    std::optional<std::string> check(const ToolDefinition& tool, const json& arguments) const;

    const ServerOptions& options() const noexcept { return options_; }

    void post_cancel(const std::weak_ptr<CallRecord>& record);
    void finish(const std::shared_ptr<CallRecord>& record, JobOutcome outcome);

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
    void admit(Callable& callable, json arguments, json meta, CallControl control, Completion done,
               std::string subject = {}, std::optional<std::string> mime = std::nullopt,
               std::optional<CacheHints> cache = std::nullopt);

    json_schema::Validator compile(const std::string& tool, std::string_view which, const json& schema) const;
    void ensure_registering(const std::string& label, std::string_view action) const;
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
    if (options_.resources_page_size && *options_.resources_page_size == 0) {
        throw std::invalid_argument("neomcp: ServerOptions::resources_page_size is zero");
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

void ServerCore::ensure_registering(const std::string& label, std::string_view action) const {
    if (serving_.load()) {
        throw std::logic_error("neomcp: " + label + " cannot be " + std::string(action) +
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
    ensure_registering("tool '" + name + "'", "registered");
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
    return index;
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

void ServerCore::admit(Callable& callable, json arguments, json meta, CallControl control, Completion done,
                       std::string subject, std::optional<std::string> mime, std::optional<CacheHints> cache) {
    auto record = std::make_shared<CallRecord>(callable, std::move(control), std::move(done), std::move(arguments),
                                               std::move(meta));
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

ListSettings ServerCore::list_settings() const {
    ListSettings settings;
    settings.page_size = options_.resources_page_size;
    settings.cache = options_.resources_cache;
    return settings;
}

void ServerCore::read(const ReadRequest& request, CallControl control, Completion done) {
    serving_.store(true);
    // A resource registered at the URI wins over every template; among templates the first registered that matches.
    if (const auto found = resource_by_uri_.find(request.uri); found != resource_by_uri_.end()) {
        const std::size_t index = found->second;
        admit(*resource_slots_[index].entry, json(nullptr), request.meta, std::move(control), std::move(done), request.uri,
              resource_defs_[index].mime_type, resource_slots_[index].cache);
        return;
    }
    for (std::size_t index = 0; index < template_slots_.size(); ++index) {
        TemplateSlot& slot = template_slots_[index];
        const auto bindings = slot.matcher.match(request.uri);
        if (!bindings) continue;
        json arguments = json::object();
        bool decodable = true;
        for (const auto& [name, text] : *bindings) {
            const auto member = std::find_if(slot.members.begin(), slot.members.end(),
                                             [&](const TemplateMember& candidate) { return candidate.name == name; });
            auto value = member == slot.members.end() ? std::nullopt : variable_value(member->kind, text);
            if (!value) {
                decodable = false;
                break;
            }
            arguments[name] = std::move(*value);
        }
        // A match whose variables are not of the member's kind, or that lacks one the struct requires, is no match.
        if (!decodable || !slot.check(arguments)) continue;
        admit(*slot.entry, std::move(arguments), request.meta, std::move(control), std::move(done), request.uri,
              template_defs_[index].mime_type, slot.cache);
        return;
    }
    done(FeatureOutcome(NotFound{request.uri}));
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
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_.erase(record->serial) != 0 && record->coroutine) --coroutines_;
    }
    if (auto* read = std::get_if<ReadResult>(&outcome)) {
        // A result that carries no hints of its own gets the resource's, else the server's default for reads.
        if (!read->cache) read->cache = record->cache ? *record->cache : options_.resource_read_cache;
    }
    std::visit(
        [&](auto&& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ToolResult> || std::is_same_v<T, Refusal> || std::is_same_v<T, HostFailure>) {
                record->done(CallOutcome(std::move(value)));
            } else {
                record->done(FeatureOutcome(std::move(value)));
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
    return settle(std::get<std::unique_ptr<PlainBody>>(body)->run(record.arguments, context));
}

asio::awaitable<JobOutcome> ToolEntry::do_async(const CallRecord& record, Context& context) {
    ToolResult result = co_await std::get<std::unique_ptr<CoroutineBody>>(body)->run(record.arguments, context);
    co_return settle(std::move(result));
}

template <class Out>
JobOutcome FeatureEntry<Out>::do_plain(const CallRecord& record, Context& context) {
    const Invocation invocation{record.arguments, record.subject, record.mime};
    return std::get<std::unique_ptr<FeaturePlainBody<Out>>>(body)->run(invocation, context);
}

template <class Out>
asio::awaitable<JobOutcome> FeatureEntry<Out>::do_async(const CallRecord& record, Context& context) {
    const Invocation invocation{record.arguments, record.subject, record.mime};
    Out result = co_await std::get<std::unique_ptr<FeatureCoroutineBody<Out>>>(body)->run(invocation, context);
    co_return JobOutcome(std::move(result));
}

template <class Out>
JobOutcome FeatureEntry<Out>::failure(const CallRecord& record, const std::exception_ptr& thrown) const {
    try {
        std::rethrow_exception(thrown);
    } catch (const ResourceNotFound&) {
        return NotFound{record.subject};
    } catch (const std::exception& error) {
        return HostFailure{name_label + " failed: " + error.what()};
    } catch (...) {
        return HostFailure{name_label + " failed with an exception that is not a std::exception"};
    }
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
        Context context(record->control, record->meta, record->stop.get_token());
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

const std::string& ResourceTemplateHandle::uri_template() const { return core_->template_definition(index_).uri_template; }


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

ResourceHost* Server::resource_host() noexcept { return core_->resource_host(); }

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
