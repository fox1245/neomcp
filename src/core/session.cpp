// The session engine.
//
// Locking design:
// - One mutex (SessionCore::mutex_) guards everything that changes after creation: the output queue, the calls in
//   flight, the batches being answered, the connection's handshake state, and the closed and draining flags.
// - Every output goes through one FIFO queue. A thread that enqueues while nobody drains becomes the drainer: it
//   pops a line, releases the mutex, calls the sink, takes the mutex again, and repeats until the queue is empty.
//   So the sink is called by one thread at a time, in queue order, never while the mutex is held, and a call into
//   the session from inside the sink only enqueues.
// - The mutex is never held across a call into the host (tools(), call(), the argument check) or across
//   std::stop_source::request_stop(), whose stop callbacks run inline and may complete the call.
// - Progress and log lines are dropped while the queue holds max_queued_notifications lines; responses never are.
// - A queued output of a call names the call, and the drainer skips it when the call was cancelled meanwhile: after
//   a cancellation is received nothing more is written for that request, even what was queued before it.
// - close() marks the session closed, empties the queue, takes the calls in flight, releases the mutex, requests
//   stop on them, and then waits until no other thread is inside the sink.
// - A sink that throws closes the session. The exception goes only to the thread that reads: out of receive() when
//   that thread was writing, also when it wrote through a Completion or CallControl a tool used inline during
//   receive() (the exception is held and rethrown after the tool's frames are left). A tool's own thread that was
//   writing through a Completion or CallControl never sees it: the transport that owns the sink knows already.
#include "neomcp/session.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "neomcp/jsonrpc.h"
#include "neomcp/wire.h"

namespace neomcp {
namespace detail {
namespace {

// Protocol identifiers the engine works with. Whether a revision has them is read from its profile.
constexpr std::string_view server_info_key = "io.modelcontextprotocol/serverInfo";
constexpr std::string_view result_meta_type = "ResultMetaObject";
constexpr std::string_view progress_method = "notifications/progress";
constexpr std::string_view log_method = "notifications/message";
constexpr std::string_view set_level_method = "logging/setLevel";
constexpr std::string_view unsupported_version_error = "unsupported_protocol_version";

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

// A cursor this server issued: the decimal index of a tool, written without leading zeros.
std::optional<std::size_t> parse_cursor(const std::string& text, std::size_t count) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return std::nullopt;
    std::size_t value = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc() || stop != end || value >= count) return std::nullopt;
    return value;
}

}  // namespace

enum class InvalidArguments { error_result, invalid_params_error };

// What serves one enabled profile.
struct Served {
    profile::ProfilePtr profile;
    const OpenMechanism* open = nullptr;
    const StateMechanism* state = nullptr;
    const BatchMechanism* batch = nullptr;
    const CancelMechanism* cancel = nullptr;
    InvalidArguments invalid_arguments = InvalidArguments::invalid_params_error;
};

// Where a request's client and log level come from.
struct Origin {
    ClientIdentity client;
    std::optional<LogLevel> log_level;  // fixed for the request
    bool connection = false;            // served under the connection's handshake
};

struct BatchState {
    const BatchMechanism* mechanism = nullptr;
    // Guarded by the core's mutex.
    std::vector<std::optional<json>> responses;
    std::size_t outstanding = 0;  // calls of the batch in flight
    bool reading = true;          // the array is still being read
};

struct CallState {
    std::weak_ptr<SessionCore> core;
    RequestId id;
    profile::ProfilePtr profile;
    const Served* served = nullptr;  // owned by the core; read only while the core is alive
    ClientIdentity client;
    json progress_token;  // null: the request asked for no progress
    std::optional<LogLevel> fixed_log_level;
    bool follows_connection_level = false;
    std::stop_source stop;
    std::shared_ptr<BatchState> batch;
    std::size_t batch_index = 0;

    // Guarded by the core's mutex.
    bool active = true;      // false once answered, cancelled or closed
    bool cancelled = false;  // the client cancelled it
    std::optional<double> last_progress;

    explicit CallState(RequestId request_id) : id(std::move(request_id)) {}
};

struct CompletionState {
    explicit CompletionState(std::shared_ptr<CallState> state) : call(std::move(state)) {}
    CompletionState(const CompletionState&) = delete;
    CompletionState& operator=(const CompletionState&) = delete;
    ~CompletionState();

    std::shared_ptr<CallState> call;
    std::atomic<bool> called{false};
};

class SessionCore : public std::enable_shared_from_this<SessionCore> {
    struct Private {};

public:
    SessionCore(Private, SessionOptions options, ToolHost& host, Sink sink)
        : options_(std::move(options)), host_(host), sink_(std::move(sink)) {}

    static std::variant<std::shared_ptr<SessionCore>, CreateError> create(SessionOptions options, ToolHost& host,
                                                                          Sink sink);

    void receive(std::string_view line);
    void close();

    void complete(const std::shared_ptr<CallState>& call, CallOutcome outcome);
    void complete_dropped(const std::shared_ptr<CallState>& call);
    void progress(const std::shared_ptr<CallState>& call, double value, std::optional<double> total,
                  std::optional<std::string> message);
    void log(const std::shared_ptr<CallState>& call, LogLevel level, json data, std::optional<std::string> logger);

    std::span<const profile::ProfilePtr> profiles() const noexcept { return profiles_; }

private:
    struct Output {
        std::string line;
        std::shared_ptr<CallState> owner;  // the call the line belongs to; null for others
    };

    struct Connection {
        const Served* served = nullptr;
        ClientIdentity client;
        LogLevel log_level = LogLevel::info;
    };

    using Batch = std::shared_ptr<BatchState>;
    using Handler = std::optional<json> (SessionCore::*)(Message&, const Served&, Origin&, const Batch&,
                                                         std::size_t);

    // How the thread that writes got there: reading input, or through a handle a tool holds.
    enum class Writer { reader, host };

    // ---- input
    void read(std::string_view line);
    void receive_array(json value);
    std::optional<json> handle(json value, const Batch& batch, std::size_t index);
    void on_notification(const Message& notification);
    std::optional<json> on_request(Message request, const Batch& batch, std::size_t index);
    std::optional<json> serve_named(Message request, const OpenMechanism& opener, const std::string& revision,
                                    const Batch& batch, std::size_t index);
    std::optional<json> open_connection(const Message& request, const OpenMechanism& opener, bool in_batch);
    std::optional<json> serve(Message request, const Served& served, Origin origin, const Batch& batch,
                              std::size_t index);

    std::optional<json> on_ping(Message& request, const Served& served, Origin&, const Batch&, std::size_t);
    std::optional<json> on_tools_list(Message& request, const Served& served, Origin&, const Batch&, std::size_t);
    std::optional<json> on_tools_call(Message& request, const Served& served, Origin& origin, const Batch& batch,
                                      std::size_t index);
    std::optional<json> on_discover(Message& request, const Served& served, Origin&, const Batch&, std::size_t);
    std::optional<json> on_set_level(Message& request, const Served& served, Origin& origin, const Batch&,
                                     std::size_t);
    static const std::map<std::string, Handler, std::less<>>& handlers();

    // ---- answers
    // The enabled revisions, newest first: those a request can name under `opener`, or every one when null.
    json supported_versions(const OpenMechanism* opener = nullptr) const;
    json capabilities(const profile::Profile& profile) const;
    void finish_result(json& body, std::string_view type, const profile::Profile& profile) const;
    json call_result(const ToolResult& result, const profile::Profile& profile) const;
    void cancel(const RequestId& id, const CancelMechanism& mechanism, const std::string& method);
    void finish_call(const std::shared_ptr<CallState>& call, json response);
    void emit_batch(const Batch& batch, Writer writer);

    // ---- output
    void emit(Output output, Writer writer);
    void emit_error(int code, std::string message);
    void drain(std::unique_lock<std::mutex>& lock, Writer writer);

    // Immutable after create().
    SessionOptions options_;
    ToolHost& host_;
    Sink sink_;
    std::vector<profile::ProfilePtr> profiles_;  // newest revision first
    std::vector<Served> served_;                 // in the order of profiles_
    std::map<std::string, std::unique_ptr<OpenMechanism>, std::less<>> openers_;  // every registered one
    std::map<std::string, std::unique_ptr<StateMechanism>, std::less<>> states_;
    std::map<std::string, std::unique_ptr<BatchMechanism>, std::less<>> batches_;
    std::map<std::string, std::unique_ptr<CancelMechanism>, std::less<>> cancels_;

    // Guarded by mutex_.
    std::mutex mutex_;
    std::condition_variable written_;
    std::deque<Output> queue_;
    bool draining_ = false;
    std::thread::id drainer_;
    bool closed_ = false;
    std::map<RequestId, std::shared_ptr<CallState>> in_flight_;
    std::optional<Connection> connection_;
    std::thread::id reading_thread_;   // the thread inside receive(), while it is there
    std::exception_ptr reader_failure_;  // a sink failure the reading thread met through a tool's handle
};

CompletionState::~CompletionState() {
    if (called.load()) return;
    // This may run while an exception from the host unwinds, so nothing may escape it; a failure here is a failure
    // of the sink, which has closed the session already.
    try {
        if (auto core = call->core.lock()) core->complete_dropped(call);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Creation

std::variant<std::shared_ptr<SessionCore>, CreateError> SessionCore::create(SessionOptions options, ToolHost& host,
                                                                          Sink sink) {
    const MechanismRegistry& registry = options.mechanisms != nullptr ? *options.mechanisms : builtin_mechanisms();
    CreateError error;
    const auto problem = [&](std::string line) {
        if (!error.message.empty()) error.message += '\n';
        error.message += line;
    };
    const auto missing = [&](const std::string& revision, MechanismSlot slot, std::string_view name) {
        problem("revision " + revision + ": no implementation of the " + std::string(to_string(slot)) +
                " mechanism '" + std::string(name) + "'");
        error.missing.push_back(MissingMechanism{revision, slot, std::string(name)});
    };

    std::vector<profile::ProfilePtr> profiles = options.profiles;
    if (profiles.empty()) {
        for (const auto revision : profile::builtin_revisions()) profiles.push_back(profile::builtin(revision));
    }
    if (std::find(profiles.begin(), profiles.end(), nullptr) != profiles.end()) {
        problem("the list of profiles holds a null profile");
        return error;
    }
    std::sort(profiles.begin(), profiles.end(),
              [](const profile::ProfilePtr& a, const profile::ProfilePtr& b) { return a->revision() > b->revision(); });
    for (std::size_t i = 1; i < profiles.size(); ++i) {
        if (profiles[i]->revision() == profiles[i - 1]->revision()) {
            problem("revision " + profiles[i]->revision() + " is enabled twice");
        }
    }
    if (!sink) problem("the sink is empty");
    if (options.tools_page_size && *options.tools_page_size == 0) problem("tools_page_size is zero");
    if (options.max_message_bytes == 0) problem("max_message_bytes is zero");
    if (options.max_queued_notifications == 0) problem("max_queued_notifications is zero");

    auto core = std::make_shared<SessionCore>(Private{}, std::move(options), host, std::move(sink));
    core->profiles_ = profiles;

    for (const auto& name : registry.names(MechanismSlot::open)) {
        if (auto made = registry.make_open(name)) core->openers_.emplace(name, std::move(made));
    }
    const auto find_or_make = [](auto& table, std::string_view name, auto make) {
        using Pointer = typename std::decay_t<decltype(table)>::mapped_type;
        const auto found = table.find(name);
        if (found != table.end()) return found->second.get();
        Pointer made = make(name);
        if (!made) return static_cast<decltype(made.get())>(nullptr);
        return table.emplace(std::string(name), std::move(made)).first->second.get();
    };

    core->served_.reserve(profiles.size());
    for (const auto& profile : profiles) {
        const std::string& revision = profile->revision();
        Served served;
        served.profile = profile;

        const std::string_view open_name = profile->mechanism(profile::Slot::open);
        if (const auto found = core->openers_.find(open_name); found != core->openers_.end()) {
            served.open = found->second.get();
        } else {
            missing(revision, MechanismSlot::open, open_name);
        }
        const std::string_view state_name = profile->mechanism(profile::Slot::state);
        served.state = find_or_make(core->states_, state_name,
                                    [&](std::string_view name) { return registry.make_state(name); });
        if (served.state == nullptr) missing(revision, MechanismSlot::state, state_name);
        const std::string_view batch_name = profile->mechanism(profile::Slot::batch);
        served.batch = find_or_make(core->batches_, batch_name,
                                    [&](std::string_view name) { return registry.make_batch(name); });
        if (served.batch == nullptr) missing(revision, MechanismSlot::batch, batch_name);
        if (!profile->stdio()) {
            problem("revision " + revision + ": the profile has no stdio transport, which a session speaks");
        } else {
            const std::string& cancel_name = profile->stdio()->cancel;
            served.cancel = find_or_make(core->cancels_, cancel_name,
                                         [&](std::string_view name) { return registry.make_cancel(name); });
            if (served.cancel == nullptr) missing(revision, MechanismSlot::cancel, cancel_name);
        }

        const std::string_view invalid = profile->invalid_arguments();
        if (invalid == "error_result") {
            served.invalid_arguments = InvalidArguments::error_result;
        } else if (invalid == "invalid_params_error") {
            served.invalid_arguments = InvalidArguments::invalid_params_error;
        } else {
            problem("revision " + revision + ": no implementation of tools.invalid_arguments '" + std::string(invalid) +
                    "'");
        }

        if (served.open != nullptr) {
            if (auto why = served.open->check(*profile)) {
                problem("revision " + revision + ": the open mechanism '" + std::string(open_name) +
                        "' cannot serve it: " + *why);
            }
            if (served.state != nullptr && served.open->needs_connection_state() && !served.state->keeps_connection()) {
                problem("revision " + revision + ": the open mechanism '" + std::string(open_name) +
                        "' needs connection state, which the state mechanism '" + std::string(state_name) +
                        "' does not keep");
            }
        }
        core->served_.push_back(std::move(served));
    }

    if (!error.message.empty()) return error;
    return core;
}

// ---------------------------------------------------------------------------------------------------------------
// Input

void SessionCore::receive(std::string_view line) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        reading_thread_ = std::this_thread::get_id();
    }
    std::exception_ptr failure;
    try {
        read(line);
    } catch (...) {
        failure = std::current_exception();
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        reading_thread_ = std::thread::id();
        std::exception_ptr held = std::exchange(reader_failure_, nullptr);
        if (!failure) failure = std::move(held);
    }
    if (failure) std::rethrow_exception(failure);
}

void SessionCore::read(std::string_view line) {
    if (line.size() > options_.max_message_bytes) {
        emit_error(error_code::invalid_request, "Invalid Request: the message is longer than " +
                                                    std::to_string(options_.max_message_bytes) + " bytes");
        return;
    }
    if (nesting_exceeds(line, options_.max_nesting_depth)) {
        emit_error(error_code::invalid_request, "Invalid Request: the message nests deeper than " +
                                                    std::to_string(options_.max_nesting_depth) + " levels");
        return;
    }
    json value = json::parse(line, nullptr, false);
    if (value.is_discarded()) {
        emit_error(error_code::parse_error, "Parse error");
        return;
    }
    if (value.is_array()) {
        receive_array(std::move(value));
        return;
    }
    if (auto answer = handle(std::move(value), nullptr, 0)) emit(Output{serialize(*answer), nullptr}, Writer::reader);
}

void SessionCore::receive_array(json value) {
    const BatchMechanism* mechanism = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) mechanism = connection_->served->batch;
    }
    if (mechanism == nullptr || !mechanism->accepts_arrays()) {
        emit_error(error_code::invalid_request, "Invalid Request: this connection does not accept batches");
        return;
    }
    auto& items = *value.get_ptr<json::array_t*>();
    if (items.empty()) {
        emit_error(error_code::invalid_request, "Invalid Request: the batch is empty");
        return;
    }
    auto batch = std::make_shared<BatchState>();
    batch->mechanism = mechanism;
    batch->responses.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (auto answer = handle(std::move(items[i]), batch, i)) {
            const std::lock_guard<std::mutex> lock(mutex_);
            batch->responses[i] = std::move(*answer);
        }
    }
    bool finished = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        batch->reading = false;
        finished = batch->outstanding == 0;
    }
    if (finished) emit_batch(batch, Writer::reader);
}

std::optional<json> SessionCore::handle(json value, const Batch& batch, std::size_t index) {
    Message message = classify(std::move(value));
    switch (message.kind) {
        case MessageKind::invalid:
            return make_error(message.id, error_code::invalid_request, "Invalid Request: " + message.problem);
        case MessageKind::response: return std::nullopt;  // this server sends no requests
        case MessageKind::notification: on_notification(message); return std::nullopt;
        case MessageKind::request: return on_request(std::move(message), batch, index);
    }
    return std::nullopt;
}

void SessionCore::on_notification(const Message& notification) {
    for (const auto& entry : cancels_) {
        if (const auto id = entry.second->cancelled_request(notification)) {
            cancel(*id, *entry.second, notification.method);
            return;
        }
    }
    // No other notification a client sends needs anything from this server.
}

std::optional<json> SessionCore::on_request(Message request, const Batch& batch, std::size_t index) {
    const RequestId id = *request.id;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_.find(id) != in_flight_.end()) {
            return make_error(id, error_code::invalid_request,
                              "Invalid Request: a request with this id is still in progress");
        }
    }

    // A request that names its revision is served under it, whatever the connection holds.
    for (const auto& entry : openers_) {
        auto named = entry.second->named_revision(request);
        if (!named) continue;
        if (const auto* error = std::get_if<RequestError>(&*named)) {
            return make_error(id, error->code, error->message, error->data);
        }
        return serve_named(std::move(request), *entry.second, std::get<std::string>(*named), batch, index);
    }
    // A handshake opens the connection.
    for (const auto& entry : openers_) {
        if (entry.second->opens_connection(request)) return open_connection(request, *entry.second, batch != nullptr);
    }
    // A connection a handshake opened serves the rest.
    const Served* served = nullptr;
    Origin origin;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            served = connection_->served;
            origin.client = connection_->client;
            origin.connection = true;
        }
    }
    if (served != nullptr) return serve(std::move(request), *served, std::move(origin), batch, index);
    // Before a handshake, a request the opening mechanism allows then.
    for (const Served& candidate : served_) {
        if (!candidate.open->allowed_before_open(request.method)) continue;
        const profile::Message* entry = candidate.profile->request(request.method);
        if (entry == nullptr || !entry->from_client) continue;
        return serve(std::move(request), candidate, Origin{}, batch, index);
    }
    // Nothing tells which revision the request speaks.
    std::string reason;
    std::vector<const OpenMechanism*> said;
    for (const Served& candidate : served_) {
        if (std::find(said.begin(), said.end(), candidate.open) != said.end()) continue;
        said.push_back(candidate.open);
        if (!reason.empty()) reason += ", and ";
        reason += candidate.open->missing_signal();
    }
    json data = json::object();
    data["supported"] = supported_versions();
    return make_error(id, error_code::invalid_params, "Invalid params: " + reason, std::move(data));
}

std::optional<json> SessionCore::serve_named(Message request, const OpenMechanism& opener, const std::string& revision,
                                             const Batch& batch, std::size_t index) {
    const RequestId id = *request.id;
    const Served* chosen = nullptr;
    const Served* newest = nullptr;
    for (const Served& candidate : served_) {  // newest first
        if (candidate.open != &opener) continue;
        if (newest == nullptr) newest = &candidate;
        if (candidate.profile->revision() == revision) chosen = &candidate;
    }
    json data = json::object();
    if (newest == nullptr) {
        // No revision can be named this way; the list tells the reader what the server serves at all.
        data["supported"] = supported_versions();
        return make_error(id, error_code::invalid_params,
                          "Invalid params: this server serves no revision that a request names in params._meta",
                          std::move(data));
    }
    if (chosen == nullptr) {
        // The revisions the client can name in a retried request.
        data["supported"] = supported_versions(&opener);
        data["requested"] = revision;
        const auto code = newest->profile->error_code(unsupported_version_error);
        return make_error(id, code.value_or(error_code::invalid_params), "Unsupported protocol version",
                          std::move(data));
    }
    auto scope = opener.scope(request, *chosen->profile, options_.protocol_logging);
    if (const auto* error = std::get_if<RequestError>(&scope)) {
        return make_error(id, error->code, error->message, error->data);
    }
    auto& found = std::get<RequestScope>(scope);
    Origin origin;
    origin.client = std::move(found.client);
    origin.log_level = found.log_level;
    return serve(std::move(request), *chosen, std::move(origin), batch, index);
}

std::optional<json> SessionCore::open_connection(const Message& request, const OpenMechanism& opener, bool in_batch) {
    const RequestId& id = *request.id;
    if (in_batch) {
        return make_error(id, error_code::invalid_request,
                          "Invalid Request: " + request.method + " must not be part of a batch");
    }
    std::vector<profile::ProfilePtr> candidates;  // newest first
    for (const Served& candidate : served_) {
        if (candidate.open == &opener) candidates.push_back(candidate.profile);
    }
    if (candidates.empty()) {
        json data = json::object();
        data["supported"] = supported_versions();
        return make_error(id, error_code::method_not_found, "Method not found: " + request.method, std::move(data));
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            return make_error(id, error_code::invalid_request, "Invalid Request: the connection is already open");
        }
    }
    auto opened = opener.open(request, candidates);
    if (const auto* error = std::get_if<RequestError>(&opened)) {
        return make_error(id, error->code, error->message, error->data);
    }
    Handshake& handshake = std::get<Handshake>(opened);
    const Served* served = nullptr;
    for (const Served& candidate : served_) {
        if (candidate.profile == handshake.profile) served = &candidate;
    }
    if (served == nullptr) {
        return make_error(id, error_code::internal_error, "Internal error: the handshake chose no enabled revision");
    }
    const profile::Profile& profile = *served->profile;

    json body = json::object();
    body["protocolVersion"] = profile.revision();
    body["capabilities"] = capabilities(profile);
    body["serverInfo"] = to_wire(options_.server_info, profile);
    if (options_.instructions) body["instructions"] = *options_.instructions;
    finish_result(body, "InitializeResult", profile);

    if (served->state->keeps_connection()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        Connection connection;
        connection.served = served;
        connection.client = std::move(handshake.client);
        connection.log_level = options_.default_log_level;
        connection_ = std::move(connection);
    }
    return make_result(id, std::move(body));
}

std::optional<json> SessionCore::serve(Message request, const Served& served, Origin origin, const Batch& batch,
                                       std::size_t index) {
    const RequestId id = *request.id;
    const profile::Profile& profile = *served.profile;
    const profile::Message* entry = profile.request(request.method);
    const auto& table = handlers();
    const auto handler = table.find(request.method);
    if (entry == nullptr || !entry->from_client || handler == table.end()) {
        return make_error(id, error_code::method_not_found, "Method not found: " + request.method);
    }
    if (!request.params.is_null() && !request.params.is_object()) {
        return make_error(id, error_code::invalid_params, "Invalid params: params must be an object");
    }
    for (const auto& name : entry->params_required) {
        if (member(request.params, name) == nullptr) {
            return make_error(id, error_code::invalid_params, "Invalid params: params lacks the required member " + name);
        }
    }
    return (this->*(handler->second))(request, served, origin, batch, index);
}

const std::map<std::string, SessionCore::Handler, std::less<>>& SessionCore::handlers() {
    static const std::map<std::string, Handler, std::less<>> table{
        {"ping", &SessionCore::on_ping},
        {"tools/list", &SessionCore::on_tools_list},
        {"tools/call", &SessionCore::on_tools_call},
        {"server/discover", &SessionCore::on_discover},
        {std::string(set_level_method), &SessionCore::on_set_level},
    };
    return table;
}

std::optional<json> SessionCore::on_ping(Message& request, const Served& served, Origin&, const Batch&, std::size_t) {
    json body = json::object();
    finish_result(body, "EmptyResult", *served.profile);
    return make_result(*request.id, std::move(body));
}

std::optional<json> SessionCore::on_tools_list(Message& request, const Served& served, Origin&, const Batch&,
                                               std::size_t) {
    const RequestId& id = *request.id;
    const profile::Profile& profile = *served.profile;
    std::span<const ToolDefinition> tools;
    try {
        tools = host_.tools();
    } catch (...) {
        return make_error(id, error_code::internal_error, "Internal error: the tools could not be listed");
    }
    std::size_t start = 0;
    if (const json* cursor = member(request.params, "cursor")) {
        if (!options_.tools_page_size) {
            return make_error(id, error_code::invalid_params,
                              "Invalid params: this server returns tools/list in one page, so no cursor is valid");
        }
        const auto* text = cursor->get_ptr<const std::string*>();
        const auto index = text == nullptr ? std::nullopt : parse_cursor(*text, tools.size());
        if (!index) return make_error(id, error_code::invalid_params, "Invalid params: invalid cursor");
        start = *index;
    }
    const std::size_t count =
        options_.tools_page_size ? std::min(tools.size() - start, *options_.tools_page_size) : tools.size() - start;
    json list = json::array();
    for (std::size_t i = start; i < start + count; ++i) list.push_back(to_wire(tools[i], profile));
    json body = json::object();
    body["tools"] = std::move(list);
    if (start + count < tools.size()) body["nextCursor"] = std::to_string(start + count);
    finish_result(body, "ListToolsResult", profile);
    return make_result(id, std::move(body));
}

std::optional<json> SessionCore::on_tools_call(Message& request, const Served& served, Origin& origin,
                                               const Batch& batch, std::size_t index) {
    const RequestId& id = *request.id;
    const profile::Profile& profile = *served.profile;
    const json* name = member(request.params, "name");
    const auto* tool_name = name == nullptr ? nullptr : name->get_ptr<const std::string*>();
    if (tool_name == nullptr) return make_error(id, error_code::invalid_params, "Invalid params: params.name must be a string");
    json arguments = json::object();
    if (const json* given = member(request.params, "arguments")) {
        if (!given->is_object()) {
            return make_error(id, error_code::invalid_params, "Invalid params: params.arguments must be an object");
        }
        arguments = *given;
    }
    const json* meta = member(request.params, "_meta");
    json progress_token = nullptr;
    if (const json* token = meta == nullptr ? nullptr : member(*meta, "progressToken")) {
        if (!token->is_string() && !token->is_number_integer()) {
            return make_error(id, error_code::invalid_params,
                              "Invalid params: params._meta.progressToken must be a string or an integer");
        }
        progress_token = *token;
    }

    std::span<const ToolDefinition> tools;
    try {
        tools = host_.tools();
    } catch (...) {
        return make_error(id, error_code::internal_error, "Internal error: the tools could not be listed");
    }
    const auto tool = std::find_if(tools.begin(), tools.end(),
                                   [&](const ToolDefinition& definition) { return definition.name == *tool_name; });
    if (tool == tools.end()) return make_error(id, error_code::invalid_params, "Unknown tool: " + *tool_name);

    if (options_.check_arguments) {
        std::optional<std::string> failure;
        try {
            failure = options_.check_arguments(*tool, arguments);
        } catch (...) {
            return make_error(id, error_code::internal_error, "Internal error: the arguments could not be checked");
        }
        if (failure) {
            switch (served.invalid_arguments) {
                case InvalidArguments::error_result: {
                    ToolResult result;
                    TextContent text;
                    text.text = *failure;
                    result.content.emplace_back(std::move(text));
                    result.is_error = true;
                    return make_result(id, call_result(result, profile));
                }
                case InvalidArguments::invalid_params_error:
                    return make_error(id, error_code::invalid_params,
                                      "Invalid arguments for tool " + tool->name + ": " + *failure);
            }
        }
    }

    auto call = std::make_shared<CallState>(id);
    call->core = weak_from_this();
    call->profile = served.profile;
    call->served = &served;
    call->client = std::move(origin.client);
    call->progress_token = std::move(progress_token);
    call->fixed_log_level = origin.log_level;
    if (origin.connection) {
        const profile::Message* set_level = profile.request(set_level_method);
        call->follows_connection_level = set_level != nullptr && set_level->from_client;
    }
    call->batch = batch;
    call->batch_index = index;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return std::nullopt;
        in_flight_.emplace(id, call);
        if (batch) ++batch->outstanding;
    }

    CallRequest call_request;
    call_request.name = *tool_name;
    call_request.arguments = std::move(arguments);
    if (meta != nullptr) call_request.meta = *meta;
    try {
        host_.call(call_request, CallControl(call), Completion(std::make_shared<CompletionState>(call)));
    } catch (...) {
        // The completion answers with an internal error once its last copy is gone.
    }
    return std::nullopt;
}

std::optional<json> SessionCore::on_discover(Message& request, const Served& served, Origin&, const Batch&,
                                             std::size_t) {
    const profile::Profile& profile = *served.profile;
    json body = json::object();
    body["supportedVersions"] = supported_versions(served.open);
    body["capabilities"] = capabilities(profile);
    if (options_.instructions) body["instructions"] = *options_.instructions;
    finish_result(body, "DiscoverResult", profile);
    return make_result(*request.id, std::move(body));
}

std::optional<json> SessionCore::on_set_level(Message& request, const Served& served, Origin& origin, const Batch&,
                                              std::size_t) {
    const RequestId& id = *request.id;
    if (!options_.protocol_logging || !origin.connection || !served.state->keeps_connection()) {
        return make_error(id, error_code::method_not_found, "Method not found: " + request.method);
    }
    const json* level = member(request.params, "level");
    const auto* text = level == nullptr ? nullptr : level->get_ptr<const std::string*>();
    const auto parsed = text == nullptr ? std::nullopt : parse_log_level(*text);
    if (!parsed) return make_error(id, error_code::invalid_params, "Invalid params: params.level is not a log level");
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) connection_->log_level = *parsed;
    }
    json body = json::object();
    finish_result(body, "EmptyResult", *served.profile);
    return make_result(id, std::move(body));
}

// ---------------------------------------------------------------------------------------------------------------
// Answers

json SessionCore::supported_versions(const OpenMechanism* opener) const {
    json list = json::array();
    for (const Served& served : served_) {
        if (opener == nullptr || served.open == opener) list.push_back(served.profile->revision());
    }
    return list;
}

json SessionCore::capabilities(const profile::Profile& profile) const {
    json capabilities = json::object();
    json tools = json::object();
    tools["listChanged"] = false;
    capabilities["tools"] = std::move(tools);
    if (options_.protocol_logging) capabilities["logging"] = json::object();
    keep_members(capabilities, "ServerCapabilities", profile);
    return capabilities;
}

// Adds the members a revision's result type requires that the engine provides, and the server's identity in
// `_meta`, each only when the type (or the result meta type) of the revision has it.
void SessionCore::finish_result(json& body, std::string_view type, const profile::Profile& profile) const {
    if (const profile::Type* result = profile.type(type)) {
        if (result->has_property("resultType")) body["resultType"] = "complete";
        if (result->has_property("ttlMs")) body["ttlMs"] = options_.tools_cache.ttl_ms;
        if (result->has_property("cacheScope")) body["cacheScope"] = std::string(to_string(options_.tools_cache.scope));
    }
    if (options_.include_server_info) {
        const profile::Type* meta = profile.type(result_meta_type);
        if (meta != nullptr && meta->has_property(server_info_key)) {
            json& slot = body["_meta"];
            if (slot.is_null()) slot = json::object();
            if (slot.is_object()) slot[std::string(server_info_key)] = to_wire(options_.server_info, profile);
        }
    }
    keep_members(body, type, profile);
}

json SessionCore::call_result(const ToolResult& result, const profile::Profile& profile) const {
    json body = to_wire(result, profile);
    finish_result(body, "CallToolResult", profile);
    return body;
}

void SessionCore::complete(const std::shared_ptr<CallState>& call, CallOutcome outcome) {
    json response;
    if (const auto* result = std::get_if<ToolResult>(&outcome)) {
        response = make_result(call->id, call_result(*result, *call->profile));
    } else if (std::holds_alternative<Refusal>(outcome)) {
        json data = json::object();
        data["reason"] = "busy";
        response = make_error(call->id, error_code::internal_error, "Internal error: the server is busy", std::move(data));
    } else {
        std::string message = "Internal error: the tool call failed";
        const auto& failure = std::get<HostFailure>(outcome);
        if (options_.expose_error_details && !failure.message.empty()) message += ": " + failure.message;
        response = make_error(call->id, error_code::internal_error, std::move(message));
    }
    finish_call(call, std::move(response));
}

void SessionCore::complete_dropped(const std::shared_ptr<CallState>& call) {
    finish_call(call, make_error(call->id, error_code::internal_error,
                                 "Internal error: the tool call ended without a result"));
}

void SessionCore::finish_call(const std::shared_ptr<CallState>& call, json response) {
    std::string line;
    if (!call->batch) line = serialize(response);
    bool batch_finished = false;
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || !call->active) return;
    call->active = false;
    if (const auto found = in_flight_.find(call->id); found != in_flight_.end() && found->second == call) {
        in_flight_.erase(found);
    }
    if (call->batch) {
        call->batch->responses[call->batch_index] = std::move(response);
        batch_finished = --call->batch->outstanding == 0 && !call->batch->reading;
    } else {
        queue_.push_back(Output{std::move(line), call});
        drain(lock, Writer::host);
    }
    lock.unlock();
    if (batch_finished) emit_batch(call->batch, Writer::host);
}

void SessionCore::cancel(const RequestId& id, const CancelMechanism& mechanism, const std::string& method) {
    std::shared_ptr<CallState> call;
    bool batch_finished = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = in_flight_.find(id);
        if (found == in_flight_.end()) return;  // unknown or finished: ignored
        const auto& candidate = found->second;
        // The request's own profile decides whether this notification cancels it.
        if (candidate->served->cancel != &mechanism) return;
        const profile::Message* entry = candidate->profile->notification(method);
        if (entry == nullptr || !entry->from_client) return;
        call = candidate;
        in_flight_.erase(found);
        call->active = false;
        call->cancelled = true;
        if (call->batch) batch_finished = --call->batch->outstanding == 0 && !call->batch->reading;
    }
    call->stop.request_stop();
    if (batch_finished) emit_batch(call->batch, Writer::reader);
}

void SessionCore::emit_batch(const Batch& batch, Writer writer) {
    std::vector<json> responses;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        for (auto& response : batch->responses) {
            if (response) responses.push_back(std::move(*response));
        }
        batch->responses.clear();
    }
    if (auto answer = batch->mechanism->assemble(std::move(responses))) {
        emit(Output{serialize(*answer), nullptr}, writer);
    }
}

void SessionCore::progress(const std::shared_ptr<CallState>& call, double value, std::optional<double> total,
                           std::optional<std::string> message) {
    if (call->progress_token.is_null() || !std::isfinite(value) || (total && !std::isfinite(*total))) return;
    const profile::Profile& profile = *call->profile;
    const profile::Message* entry = profile.notification(progress_method);
    if (entry == nullptr || !entry->from_server) return;
    json params = json::object();
    params["progressToken"] = call->progress_token;
    params["progress"] = value;
    if (total) params["total"] = *total;
    if (message) params["message"] = std::move(*message);
    if (entry->params) keep_members(params, *entry->params, profile);
    std::string line = serialize(make_notification(entry->method, std::move(params)));

    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || !call->active) return;
    if (call->last_progress && !(value > *call->last_progress)) return;
    // A full queue drops the line; the value does not count as sent.
    if (queue_.size() >= options_.max_queued_notifications) return;
    call->last_progress = value;
    queue_.push_back(Output{std::move(line), call});
    drain(lock, Writer::host);
}

void SessionCore::log(const std::shared_ptr<CallState>& call, LogLevel level, json data,
                      std::optional<std::string> logger) {
    if (!options_.protocol_logging) return;
    const profile::Profile& profile = *call->profile;
    const profile::Message* entry = profile.notification(log_method);
    if (entry == nullptr || !entry->from_server) return;
    json params = json::object();
    params["level"] = std::string(to_string(level));
    if (logger) params["logger"] = std::move(*logger);
    params["data"] = std::move(data);
    if (entry->params) keep_members(params, *entry->params, profile);
    std::string line = serialize(make_notification(entry->method, std::move(params)));

    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || !call->active) return;
    std::optional<LogLevel> threshold = call->fixed_log_level;
    if (call->follows_connection_level && connection_) threshold = connection_->log_level;
    if (!threshold || level < *threshold) return;
    if (queue_.size() >= options_.max_queued_notifications) return;
    queue_.push_back(Output{std::move(line), call});
    drain(lock, Writer::host);
}

// ---------------------------------------------------------------------------------------------------------------
// Output

void SessionCore::emit(Output output, Writer writer) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) return;
    queue_.push_back(std::move(output));
    drain(lock, writer);
}

void SessionCore::emit_error(int code, std::string message) {
    emit(Output{serialize(make_error(std::nullopt, code, std::move(message))), nullptr}, Writer::reader);
}

void SessionCore::drain(std::unique_lock<std::mutex>& lock, Writer writer) {
    if (draining_) return;  // the thread that drains writes this output too
    draining_ = true;
    drainer_ = std::this_thread::get_id();
    std::exception_ptr failure;
    while (!closed_ && !queue_.empty()) {
        Output output = std::move(queue_.front());
        queue_.pop_front();
        if (output.owner && output.owner->cancelled) continue;
        lock.unlock();
        try {
            sink_(std::move(output.line));
        } catch (...) {
            failure = std::current_exception();
        }
        lock.lock();
        if (failure) break;
    }
    draining_ = false;
    drainer_ = std::thread::id();
    written_.notify_all();
    if (failure) {
        // A sink that throws cannot carry the connection any further.
        lock.unlock();
        close();
        lock.lock();
        if (writer == Writer::reader) std::rethrow_exception(failure);
        // Through a tool's handle: the reading thread gets it when receive() returns; a tool's thread never does.
        if (reading_thread_ == std::this_thread::get_id() && !reader_failure_) reader_failure_ = failure;
    }
}

void SessionCore::close() {
    std::vector<std::shared_ptr<CallState>> calls;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_) {
            closed_ = true;
            queue_.clear();
            for (auto& entry : in_flight_) {
                entry.second->active = false;
                calls.push_back(entry.second);
            }
            in_flight_.clear();
        }
    }
    for (const auto& call : calls) call->stop.request_stop();
    std::unique_lock<std::mutex> lock(mutex_);
    if (drainer_ != std::this_thread::get_id()) written_.wait(lock, [&] { return !draining_; });
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Public handles

std::variant<std::unique_ptr<Session>, CreateError> Session::create(SessionOptions options, ToolHost& host, Sink sink) {
    auto made = detail::SessionCore::create(std::move(options), host, std::move(sink));
    if (auto* error = std::get_if<CreateError>(&made)) return std::move(*error);
    return std::unique_ptr<Session>(new Session(std::get<std::shared_ptr<detail::SessionCore>>(std::move(made))));
}

Session::~Session() {
    if (core_) core_->close();
}

void Session::receive(std::string_view line) {
    const auto core = core_;  // the sink may destroy the session while this runs
    core->receive(line);
}

void Session::close() {
    const auto core = core_;
    core->close();
}

std::span<const profile::ProfilePtr> Session::profiles() const noexcept { return core_->profiles(); }

std::stop_token CallControl::stop_token() const noexcept { return state_->stop.get_token(); }

void CallControl::progress(double value, std::optional<double> total, std::optional<std::string> message) const {
    if (auto core = state_->core.lock()) core->progress(state_, value, total, std::move(message));
}

void CallControl::log(LogLevel level, json data, std::optional<std::string> logger) const {
    if (auto core = state_->core.lock()) core->log(state_, level, std::move(data), std::move(logger));
}

const profile::Profile& CallControl::profile() const noexcept { return *state_->profile; }
const json& CallControl::client_info() const noexcept { return state_->client.info; }
const json& CallControl::client_capabilities() const noexcept { return state_->client.capabilities; }

void Completion::operator()(CallOutcome outcome) const {
    if (!state_ || state_->called.exchange(true)) return;
    if (auto core = state_->call->core.lock()) core->complete(state_->call, std::move(outcome));
}

}  // namespace neomcp
