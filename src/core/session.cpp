// The session engine.
//
// Locking design:
// - One mutex (SessionCore::mutex_) guards everything that changes after creation: the output queue, the calls in
//   flight, the batches being answered, the input units still open, the connection's handshake state, and the
//   closed and draining flags.
// - Every output goes through one FIFO queue. A thread that enqueues while nobody drains becomes the drainer: it
//   pops an output, releases the mutex, calls the sink, takes the mutex again, and repeats until the queue is empty.
//   So the sink is called by one thread at a time, in queue order, never while the mutex is held, and a call into
//   the session from inside the sink only enqueues.
// - The mutex is never held across a call into the host (tools(), call(), the argument check) or across
//   std::stop_source::request_stop(), whose stop callbacks run inline and may complete the call.
// - Progress and log lines are dropped while the queue holds max_queued_notifications lines; responses never are.
// - A queued output of a call names the call, and the drainer skips it when the call was cancelled meanwhile: after
//   a cancellation is received nothing more is written for that request, even what was queued before it.
// - An input unit is open from the moment receive() takes it until the drainer pops its last output (its answer or
//   its end marker). The thread that queues the last output marks the unit finished under the mutex, and nothing
//   more is queued for a finished unit, so a unit gets exactly one last output; the paths that can end a unit (an
//   answer, a cancellation, the assembly of a batch, a unit that needs no answer) each queue it under that rule.
// - abandon() marks the unit abandoned and lets it go: nothing more is queued for it, the drainer skips what is
//   queued, and the calls whose cancellation mechanism takes a closed stream for a cancellation are stopped as a
//   cancellation would stop them. The drainer records which unit it is writing while it is inside the sink, and
//   abandon() waits until that is no longer the abandoned unit, unless the drainer is the thread calling it.
// - close() marks the session closed, empties the queue, takes the calls in flight, and queues an end marker for
//   every unit still open, since the queue no longer holds their last outputs. It releases the mutex, requests stop
//   on the calls, waits until no other thread is inside the sink and writes the end markers itself unless another
//   drainer did. Called from inside the sink, or after the sink failed, it queues no end markers.
// - The header fields of a unit are read once, in receive(), through the names the served profiles give the version
//   header, and checked per request against the request headers of the profile the request is served under. Nothing
//   about them is kept after receive() returns.
// - The idle callback runs with no lock held, after the thread that did the work found the session idle under the
//   mutex: at the end of a drain, after a cancellation, and when a batch was assembled. A batch counts as pending from
//   the moment it is read until its answer was handed to the queue, so that the gap between the last call of the batch
//   and its answer is not mistaken for idleness.
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
#include <initializer_list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "core/headers.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/wire.h"

namespace neomcp {

std::string_view to_string(RefusalReason reason) noexcept {
    switch (reason) {
        case RefusalReason::none: return "none";
        case RefusalReason::parse_error: return "parse_error";
        case RefusalReason::invalid_request: return "invalid_request";
        case RefusalReason::too_long: return "too_long";
        case RefusalReason::too_deep: return "too_deep";
        case RefusalReason::header_mismatch: return "header_mismatch";
        case RefusalReason::unsupported_version: return "unsupported_version";
        case RefusalReason::invalid_request_meta: return "invalid_request_meta";
        case RefusalReason::unsupported_version_header: return "unsupported_version_header";
        case RefusalReason::method_not_found: return "method_not_found";
        case RefusalReason::missing_connection: return "missing_connection";
    }
    return {};
}

namespace detail {
namespace {

// Protocol identifiers the engine works with. Whether a revision has them is read from its profile.
constexpr std::string_view server_info_key = "io.modelcontextprotocol/serverInfo";
constexpr std::string_view result_meta_type = "ResultMetaObject";
constexpr std::string_view progress_method = "notifications/progress";
constexpr std::string_view log_method = "notifications/message";
constexpr std::string_view set_level_method = "logging/setLevel";
constexpr std::string_view unsupported_version_error = "unsupported_protocol_version";
constexpr std::string_view header_mismatch_error = "header_mismatch";

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

// The cancellation mechanism a profile names for the transport; null when the profile does not define the transport.
const std::string* cancel_name(const profile::Profile& profile, profile::Transport transport) {
    switch (transport) {
        case profile::Transport::stdio: return profile.stdio() ? &profile.stdio()->cancel : nullptr;
        case profile::Transport::http_sse: return profile.http_sse() ? &profile.http_sse()->cancel : nullptr;
        case profile::Transport::streamable_http:
            return profile.streamable_http() ? &profile.streamable_http()->cancel : nullptr;
    }
    return nullptr;
}

Output make_output(std::string line, OutputKind kind, bool last, RefusalReason refusal, profile::ProfilePtr profile) {
    Output output;
    output.line = std::move(line);
    output.kind = kind;
    output.last = last;
    output.refusal = refusal;
    output.profile = std::move(profile);
    return output;
}

}  // namespace

enum class InvalidArguments { error_result, invalid_params_error };

// What serves one enabled profile on the session's transport.
struct Served {
    profile::ProfilePtr profile;
    const OpenMechanism* open = nullptr;
    const StateMechanism* state = nullptr;
    const BatchMechanism* batch = nullptr;
    const CancelMechanism* cancel = nullptr;  // the one the profile names for the session's transport
    InvalidArguments invalid_arguments = InvalidArguments::invalid_params_error;
    // What the profile's entry for the transport says about headers; views into the profile.
    std::span<const profile::RequestHeader> request_headers;
    const profile::EncodedHeaderValue* encoded = nullptr;
};

// What the header fields of an input unit claim, found through the served profiles' data.
struct Claims {
    std::span<const HeaderField> fields;
    std::size_t versions = 0;        // fields that carry the version header
    std::string_view version_name;   // the name of the version header, as a profile spells it
    std::string_view version;        // the value of the first such field
    const Served* named = nullptr;   // the served profile of the revision it names; null when it names none
};

// Where a request's client and log level come from.
struct Origin {
    ClientIdentity client;
    std::optional<LogLevel> log_level;  // fixed for the request
    bool connection = false;            // served under the connection's handshake
};

// An input unit fed with a number.
struct UnitState {
    explicit UnitState(UnitId number) : id(number) {}
    const UnitId id;
    // Guarded by the core's mutex.
    profile::ProfilePtr profile;  // the profile its calls are served under, once one is known
    bool finished = false;        // its last output was queued
    bool abandoned = false;       // the transport let it go: nothing more is written for it
};

using Unit = std::shared_ptr<UnitState>;

struct BatchState {
    const BatchMechanism* mechanism = nullptr;
    profile::ProfilePtr profile;  // the connection's, which the batch is served under
    Unit unit;                    // null when the batch belongs to no unit
    // Guarded by the core's mutex.
    std::vector<std::optional<json>> responses;
    std::size_t outstanding = 0;  // calls of the batch in flight
    bool reading = true;          // the array is still being read
};

using Batch = std::shared_ptr<BatchState>;

// Where a message of the input stands.
struct Place {
    Unit unit;              // null when the input belongs to no unit
    Batch batch;            // null when the message came alone
    std::size_t index = 0;  // its place in the batch
    const Claims* claims = nullptr;  // what the unit's headers claim; never null
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
    Unit unit;  // the unit the request came in; null when none
    Batch batch;
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

// The answer to one message, with what a transport needs to know about it.
struct Answer {
    json message;
    RefusalReason refusal = RefusalReason::none;
    profile::ProfilePtr profile;  // the profile that governs it; null when none is known
};

// What handling one message gave.
struct Handled {
    std::optional<Answer> answer;  // the answer now
    bool later = false;            // a call answers it later
};

class SessionCore : public std::enable_shared_from_this<SessionCore> {
    struct Private {};

public:
    SessionCore(Private, SessionOptions options, ToolHost& host, OutputSink sink)
        : options_(std::move(options)), host_(host), sink_(std::move(sink)) {}

    static std::variant<std::shared_ptr<SessionCore>, CreateError> create(SessionOptions options, ToolHost& host,
                                                                          OutputSink sink);

    // `unit` empty: the input belongs to no unit.
    bool receive(std::optional<UnitId> unit, std::string_view input, std::span<const HeaderField> headers);
    void abandon(UnitId unit);
    void close();
    bool idle() const;
    profile::ProfilePtr connection_profile() const;

    void complete(const std::shared_ptr<CallState>& call, CallOutcome outcome);
    void complete_dropped(const std::shared_ptr<CallState>& call);
    void progress(const std::shared_ptr<CallState>& call, double value, std::optional<double> total,
                  std::optional<std::string> message);
    void log(const std::shared_ptr<CallState>& call, LogLevel level, json data, std::optional<std::string> logger);

    std::span<const profile::ProfilePtr> profiles() const noexcept { return profiles_; }

private:
    struct Queued {
        Output output;                     // its unit number is filled in from `unit` when it is queued
        std::shared_ptr<CallState> owner;  // the call the output belongs to; null for others
        Unit unit;                         // the unit it belongs to; null when it belongs to the connection
    };

    struct Connection {
        const Served* served = nullptr;
        ClientIdentity client;
        LogLevel log_level = LogLevel::info;
    };

    using Handler = std::optional<Answer> (SessionCore::*)(Message&, const Served&, Origin&, const Place&);

    // How the thread that writes got there: reading input, or through a handle a tool holds.
    enum class Writer { reader, host };

    // ---- input
    // Each returns whether the input awaits an answer.
    bool read(std::string_view line, const Unit& unit, const Claims& claims);
    bool receive_array(json value, const Unit& unit, const Claims& claims);
    Handled handle(json value, const Place& place);
    void on_notification(const Message& notification);
    std::optional<Answer> on_request(Message request, const Place& place);
    std::optional<Answer> serve_named(Message request, const OpenMechanism& opener, const std::string& revision,
                                      const Place& place);
    Answer open_connection(const Message& request, const OpenMechanism& opener, bool in_batch);
    std::optional<Answer> serve(Message request, const Served& served, Origin origin, const Place& place);

    std::optional<Answer> on_ping(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_tools_list(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_tools_call(Message& request, const Served& served, Origin& origin, const Place& place);
    std::optional<Answer> on_discover(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_set_level(Message& request, const Served& served, Origin& origin, const Place&);
    static const std::map<std::string, Handler, std::less<>>& handlers();

    // ---- headers
    Claims claims_of(std::span<const HeaderField> fields) const;
    // A refusal for what the version header claims about a unit as a whole: it is repeated, or names no revision
    // served. Empty when it claims nothing wrong.
    std::optional<Answer> version_refusal(const Claims& claims, const std::optional<RequestId>& id) const;
    Answer unsupported_version_header(const Claims& claims, const std::optional<RequestId>& id) const;
    Answer header_mismatch(const std::optional<RequestId>& id, std::string message,
                           std::initializer_list<const Served*> preferred) const;
    // The code of a named error: from the first of `preferred` whose profile defines it, else from the newest served
    // profile that does, with that profile; else `fallback` with the first of `preferred`.
    std::pair<int, profile::ProfilePtr> error_from(std::string_view name, std::initializer_list<const Served*> preferred,
                                                   int fallback) const;
    bool opens_connection(const Message& request) const;

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
    // Puts an output in the queue unless the session closed or its unit finished; with mutex_ held.
    bool enqueue(Queued queued);
    // Queues an output and drains; when it is dropped, tells the idle callback what a drain would have told it.
    void push(std::unique_lock<std::mutex>& lock, Queued queued, Writer writer);
    void emit(Queued queued, Writer writer);
    void emit_answer(Answer answer, const Unit& unit, Writer writer);
    void emit_refusal(const Unit& unit, int code, std::string message, RefusalReason reason);
    // Ends a unit that awaits no answer.
    void end_unit(const Unit& unit, Writer writer);
    void drain(std::unique_lock<std::mutex>& lock, Writer writer);
    bool idle_locked() const;          // with mutex_ held
    void signal_idle() noexcept;       // with no lock held

    // Immutable after create().
    SessionOptions options_;
    ToolHost& host_;
    OutputSink sink_;
    std::vector<profile::ProfilePtr> profiles_;  // the served ones, newest revision first
    std::vector<Served> served_;                 // in the order of profiles_
    std::vector<std::string> version_headers_;   // the names the served profiles give the version header
    std::map<std::string, std::unique_ptr<OpenMechanism>, std::less<>> openers_;  // every registered one
    std::map<std::string, std::unique_ptr<StateMechanism>, std::less<>> states_;
    std::map<std::string, std::unique_ptr<BatchMechanism>, std::less<>> batches_;
    std::map<std::string, std::unique_ptr<CancelMechanism>, std::less<>> cancels_;

    // Guarded by mutex_.
    mutable std::mutex mutex_;
    std::condition_variable written_;
    std::deque<Queued> queue_;
    bool draining_ = false;
    std::thread::id drainer_;
    bool closed_ = false;
    bool broken_ = false;  // the sink threw; it is not called again
    std::map<RequestId, std::shared_ptr<CallState>> in_flight_;
    std::map<UnitId, Unit> units_;     // the units still open
    const UnitState* writing_ = nullptr;  // the unit whose output the drainer is writing; null when none
    std::size_t abandoning_ = 0;       // threads in abandon() waiting for a write to end
    std::size_t pending_batches_ = 0;  // batches read whose answer was not handed to the queue yet
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
                                                                          OutputSink sink) {
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

    const profile::Transport transport = options.transport;
    auto core = std::make_shared<SessionCore>(Private{}, std::move(options), host, std::move(sink));

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
        // A profile that does not define the transport is not served on it.
        const std::string* cancel = cancel_name(*profile, transport);
        if (cancel == nullptr) continue;

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
        served.cancel = find_or_make(core->cancels_, *cancel,
                                     [&](std::string_view name) { return registry.make_cancel(name); });
        if (served.cancel == nullptr) missing(revision, MechanismSlot::cancel, *cancel);
        if (transport == profile::Transport::streamable_http) {
            const profile::StreamableHttpTransport& http = *profile->streamable_http();
            served.request_headers = http.request_headers;
            if (http.encoded_header_value) served.encoded = &*http.encoded_header_value;
            std::vector<std::string_view> names;
            if (http.version_header) names.push_back(http.version_header->name);
            for (const auto& header : http.request_headers) {
                if (header.value == profile::HeaderValue::protocol_version) names.push_back(header.name);
            }
            for (const auto name : names) {
                const bool known = std::any_of(core->version_headers_.begin(), core->version_headers_.end(),
                                               [&](const std::string& seen) { return same_header_name(seen, name); });
                if (!known) core->version_headers_.emplace_back(name);
            }
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
        core->profiles_.push_back(profile);
        core->served_.push_back(std::move(served));
    }
    if (core->served_.empty()) {
        problem("no enabled profile defines the " + std::string(profile::to_string(transport)) + " transport");
    }

    if (!error.message.empty()) return error;
    return core;
}

// ---------------------------------------------------------------------------------------------------------------
// Input

bool SessionCore::receive(std::optional<UnitId> number, std::string_view input,
                          std::span<const HeaderField> headers) {
    Unit unit;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        if (number) {
            if (units_.find(*number) != units_.end()) {
                throw std::invalid_argument("neomcp: input unit " + std::to_string(*number) + " is still open");
            }
            unit = std::make_shared<UnitState>(*number);
            units_.emplace(*number, unit);
        }
        reading_thread_ = std::this_thread::get_id();
    }
    std::exception_ptr failure;
    bool awaits = false;
    try {
        const Claims claims = claims_of(headers);
        awaits = read(input, unit, claims);
        if (unit && !awaits) end_unit(unit, Writer::reader);
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
    return awaits;
}

bool SessionCore::read(std::string_view line, const Unit& unit, const Claims& claims) {
    if (line.size() > options_.max_message_bytes) {
        emit_refusal(unit, error_code::invalid_request,
                     "Invalid Request: the message is longer than " + std::to_string(options_.max_message_bytes) +
                         " bytes",
                     RefusalReason::too_long);
        return true;
    }
    if (nesting_exceeds(line, options_.max_nesting_depth)) {
        emit_refusal(unit, error_code::invalid_request,
                     "Invalid Request: the message nests deeper than " + std::to_string(options_.max_nesting_depth) +
                         " levels",
                     RefusalReason::too_deep);
        return true;
    }
    json value = json::parse(line, nullptr, false);
    if (value.is_discarded()) {
        emit_refusal(unit, error_code::parse_error, "Parse error", RefusalReason::parse_error);
        return true;
    }
    if (value.is_array()) return receive_array(std::move(value), unit, claims);
    Handled handled = handle(std::move(value), Place{unit, nullptr, 0, &claims});
    if (handled.answer) {
        emit_answer(std::move(*handled.answer), unit, Writer::reader);
        return true;
    }
    return handled.later;
}

bool SessionCore::receive_array(json value, const Unit& unit, const Claims& claims) {
    // The headers belong to the whole batch.
    if (auto refusal = version_refusal(claims, std::nullopt)) {
        emit_answer(std::move(*refusal), unit, Writer::reader);
        return true;
    }
    const BatchMechanism* mechanism = nullptr;
    profile::ProfilePtr profile;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            mechanism = connection_->served->batch;
            profile = connection_->served->profile;
        }
    }
    if (mechanism == nullptr || !mechanism->accepts_arrays()) {
        emit_refusal(unit, error_code::invalid_request, "Invalid Request: this connection does not accept batches",
                     RefusalReason::invalid_request);
        return true;
    }
    auto& items = *value.get_ptr<json::array_t*>();
    if (items.empty()) {
        emit_refusal(unit, error_code::invalid_request, "Invalid Request: the batch is empty",
                     RefusalReason::invalid_request);
        return true;
    }
    auto batch = std::make_shared<BatchState>();
    batch->mechanism = mechanism;
    batch->profile = profile;
    batch->unit = unit;
    batch->responses.resize(items.size());
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++pending_batches_;
        if (unit) unit->profile = profile;
    }
    bool awaits = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        Handled handled = handle(std::move(items[i]), Place{unit, batch, i, &claims});
        if (handled.answer || handled.later) awaits = true;
        if (handled.answer) {
            const std::lock_guard<std::mutex> lock(mutex_);
            batch->responses[i] = std::move(handled.answer->message);
        }
    }
    bool finished = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        batch->reading = false;
        finished = batch->outstanding == 0;
    }
    if (finished) emit_batch(batch, Writer::reader);
    return awaits;
}

Handled SessionCore::handle(json value, const Place& place) {
    Message message = classify(std::move(value));
    switch (message.kind) {
        case MessageKind::invalid:
            return Handled{Answer{make_error(message.id, error_code::invalid_request, "Invalid Request: " + message.problem),
                                  RefusalReason::invalid_request, nullptr},
                           false};
        case MessageKind::response: return Handled{};  // this server sends no requests
        case MessageKind::notification:
            // A notification alone is refused for what its headers claim; in a batch the batch was.
            if (!place.batch) {
                if (auto refusal = version_refusal(*place.claims, std::nullopt)) return Handled{std::move(refusal), false};
            }
            on_notification(message);
            return Handled{};
        case MessageKind::request: {
            auto answer = on_request(std::move(message), place);
            if (answer) return Handled{std::move(answer), false};
            return Handled{std::nullopt, true};
        }
    }
    return Handled{};
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

std::optional<Answer> SessionCore::on_request(Message request, const Place& place) {
    const RequestId id = *request.id;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_.find(id) != in_flight_.end()) {
            return Answer{make_error(id, error_code::invalid_request,
                                     "Invalid Request: a request with this id is still in progress"),
                          RefusalReason::invalid_request, nullptr};
        }
    }

    const Claims& claims = *place.claims;
    if (claims.versions > 1) {
        return header_mismatch(id,
                               std::string(header_mismatch_prefix) + "the " + std::string(claims.version_name) +
                                   " header appears more than once",
                               {});
    }

    // A request that names its revision is served under it, whatever the connection holds.
    for (const auto& entry : openers_) {
        auto named = entry.second->named_revision(request);
        if (!named) continue;
        if (const auto* error = std::get_if<RequestError>(&*named)) {
            return Answer{make_error(id, error->code, error->message, error->data), RefusalReason::invalid_request_meta,
                          nullptr};
        }
        return serve_named(std::move(request), *entry.second, std::get<std::string>(*named), place);
    }
    // The body names no revision. A version header that names a revision whose requests must name it themselves
    // finds a malformed request; one that names no revision served refuses it, unless it is a handshake (the header
    // is defined for the requests after it). Otherwise the connection decides.
    if (claims.versions == 1) {
        if (claims.named != nullptr && claims.named->open->requests_name_revision()) {
            json data = json::object();
            data["supported"] = supported_versions(claims.named->open);
            return Answer{make_error(id, error_code::invalid_params, "Invalid params: " + claims.named->open->missing_signal(),
                                     std::move(data)),
                          RefusalReason::invalid_request_meta, claims.named->profile};
        }
        if (claims.named == nullptr && !opens_connection(request)) return unsupported_version_header(claims, id);
    }
    // A handshake opens the connection.
    for (const auto& entry : openers_) {
        if (entry.second->opens_connection(request)) {
            return open_connection(request, *entry.second, place.batch != nullptr);
        }
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
    if (served != nullptr) return serve(std::move(request), *served, std::move(origin), place);
    // Before a handshake, a request the opening mechanism allows then.
    for (const Served& candidate : served_) {
        if (!candidate.open->allowed_before_open(request.method)) continue;
        const profile::Message* entry = candidate.profile->request(request.method);
        if (entry == nullptr || !entry->from_client) continue;
        return serve(std::move(request), candidate, Origin{}, place);
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
    return Answer{make_error(id, error_code::invalid_params, "Invalid params: " + reason, std::move(data)),
                  RefusalReason::missing_connection, nullptr};
}

std::optional<Answer> SessionCore::serve_named(Message request, const OpenMechanism& opener,
                                               const std::string& revision, const Place& place) {
    const RequestId id = *request.id;
    const Served* chosen = nullptr;
    const Served* newest = nullptr;
    for (const Served& candidate : served_) {  // newest first
        if (candidate.open != &opener) continue;
        if (newest == nullptr) newest = &candidate;
        if (candidate.profile->revision() == revision) chosen = &candidate;
    }
    // What the headers claim is checked first, a mismatch before the version: the header must name the revision the
    // body names, and must be there when the rules of that revision (or, when it is not served, of the newest
    // revision named this way) require it.
    const Claims& claims = *place.claims;
    if (claims.versions == 1 && claims.version != revision) {
        return header_mismatch(id,
                               std::string(header_mismatch_prefix) + std::string(claims.version_name) + " header value '" +
                                   std::string(claims.version) + "' does not match body value '" + revision + "'",
                               {claims.named, chosen});
    }
    const Served* rules = chosen != nullptr ? chosen : newest;
    if (rules != nullptr && claims.versions == 0) {
        if (const auto name = required_version_header(rules->request_headers, request.method)) {
            return header_mismatch(id,
                                   std::string(header_mismatch_prefix) + "the request lacks the " + std::string(*name) +
                                       " header",
                                   {chosen, newest});
        }
    }
    json data = json::object();
    if (newest == nullptr) {
        // No revision can be named this way; the list tells the reader what the server serves at all.
        data["supported"] = supported_versions();
        return Answer{make_error(id, error_code::invalid_params,
                                 "Invalid params: this server serves no revision that a request names in params._meta",
                                 std::move(data)),
                      RefusalReason::unsupported_version, nullptr};
    }
    if (chosen == nullptr) {
        // The revisions the client can name in a retried request.
        data["supported"] = supported_versions(&opener);
        data["requested"] = revision;
        const auto code = newest->profile->error_code(unsupported_version_error);
        return Answer{make_error(id, code.value_or(error_code::invalid_params), "Unsupported protocol version",
                                 std::move(data)),
                      RefusalReason::unsupported_version, newest->profile};
    }
    if (auto problem = check_request_headers(request, claims.fields, chosen->request_headers, chosen->encoded,
                                             chosen->profile->revision())) {
        return header_mismatch(id, std::move(*problem), {chosen});
    }
    auto scope = opener.scope(request, *chosen->profile, options_.protocol_logging);
    if (const auto* error = std::get_if<RequestError>(&scope)) {
        return Answer{make_error(id, error->code, error->message, error->data), RefusalReason::invalid_request_meta,
                      chosen->profile};
    }
    auto& found = std::get<RequestScope>(scope);
    Origin origin;
    origin.client = std::move(found.client);
    origin.log_level = found.log_level;
    return serve(std::move(request), *chosen, std::move(origin), place);
}

Answer SessionCore::open_connection(const Message& request, const OpenMechanism& opener, bool in_batch) {
    const RequestId& id = *request.id;
    if (in_batch) {
        return Answer{make_error(id, error_code::invalid_request,
                                 "Invalid Request: " + request.method + " must not be part of a batch"),
                      RefusalReason::invalid_request, nullptr};
    }
    std::vector<profile::ProfilePtr> candidates;  // newest first
    for (const Served& candidate : served_) {
        if (candidate.open == &opener) candidates.push_back(candidate.profile);
    }
    if (candidates.empty()) {
        json data = json::object();
        data["supported"] = supported_versions();
        return Answer{make_error(id, error_code::method_not_found, "Method not found: " + request.method, std::move(data)),
                      RefusalReason::method_not_found, nullptr};
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            return Answer{make_error(id, error_code::invalid_request, "Invalid Request: the connection is already open"),
                          RefusalReason::invalid_request, connection_->served->profile};
        }
    }
    auto opened = opener.open(request, candidates);
    if (const auto* error = std::get_if<RequestError>(&opened)) {
        return Answer{make_error(id, error->code, error->message, error->data), RefusalReason::none, nullptr};
    }
    Handshake& handshake = std::get<Handshake>(opened);
    const Served* served = nullptr;
    for (const Served& candidate : served_) {
        if (candidate.profile == handshake.profile) served = &candidate;
    }
    if (served == nullptr) {
        return Answer{make_error(id, error_code::internal_error, "Internal error: the handshake chose no enabled revision"),
                      RefusalReason::none, nullptr};
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
    return Answer{make_result(id, std::move(body)), RefusalReason::none, served->profile};
}

std::optional<Answer> SessionCore::serve(Message request, const Served& served, Origin origin, const Place& place) {
    const RequestId id = *request.id;
    const profile::Profile& profile = *served.profile;
    const profile::Message* entry = profile.request(request.method);
    const auto& table = handlers();
    const auto handler = table.find(request.method);
    if (entry == nullptr || !entry->from_client || handler == table.end()) {
        return Answer{make_error(id, error_code::method_not_found, "Method not found: " + request.method),
                      RefusalReason::method_not_found, served.profile};
    }
    if (!request.params.is_null() && !request.params.is_object()) {
        return Answer{make_error(id, error_code::invalid_params, "Invalid params: params must be an object"),
                      RefusalReason::none, served.profile};
    }
    for (const auto& name : entry->params_required) {
        if (member(request.params, name) == nullptr) {
            return Answer{make_error(id, error_code::invalid_params,
                                     "Invalid params: params lacks the required member " + name),
                          RefusalReason::none, served.profile};
        }
    }
    return (this->*(handler->second))(request, served, origin, place);
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

namespace {

// An answer of a request served under a profile.
Answer served_answer(json message, const Served& served, RefusalReason refusal = RefusalReason::none) {
    return Answer{std::move(message), refusal, served.profile};
}

}  // namespace

std::optional<Answer> SessionCore::on_ping(Message& request, const Served& served, Origin&, const Place&) {
    json body = json::object();
    finish_result(body, "EmptyResult", *served.profile);
    return served_answer(make_result(*request.id, std::move(body)), served);
}

std::optional<Answer> SessionCore::on_tools_list(Message& request, const Served& served, Origin&, const Place&) {
    const RequestId& id = *request.id;
    const profile::Profile& profile = *served.profile;
    std::span<const ToolDefinition> tools;
    try {
        tools = host_.tools();
    } catch (...) {
        return served_answer(make_error(id, error_code::internal_error, "Internal error: the tools could not be listed"),
                             served);
    }
    std::size_t start = 0;
    if (const json* cursor = member(request.params, "cursor")) {
        if (!options_.tools_page_size) {
            return served_answer(
                make_error(id, error_code::invalid_params,
                           "Invalid params: this server returns tools/list in one page, so no cursor is valid"),
                served);
        }
        const auto* text = cursor->get_ptr<const std::string*>();
        const auto index = text == nullptr ? std::nullopt : parse_cursor(*text, tools.size());
        if (!index) return served_answer(make_error(id, error_code::invalid_params, "Invalid params: invalid cursor"), served);
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
    return served_answer(make_result(id, std::move(body)), served);
}

std::optional<Answer> SessionCore::on_tools_call(Message& request, const Served& served, Origin& origin,
                                                 const Place& place) {
    const RequestId& id = *request.id;
    const profile::Profile& profile = *served.profile;
    const json* name = member(request.params, "name");
    const auto* tool_name = name == nullptr ? nullptr : name->get_ptr<const std::string*>();
    if (tool_name == nullptr) {
        return served_answer(make_error(id, error_code::invalid_params, "Invalid params: params.name must be a string"),
                             served);
    }
    json arguments = json::object();
    if (const json* given = member(request.params, "arguments")) {
        if (!given->is_object()) {
            return served_answer(
                make_error(id, error_code::invalid_params, "Invalid params: params.arguments must be an object"), served);
        }
        arguments = *given;
    }
    const json* meta = member(request.params, "_meta");
    json progress_token = nullptr;
    if (const json* token = meta == nullptr ? nullptr : member(*meta, "progressToken")) {
        if (!token->is_string() && !token->is_number_integer()) {
            return served_answer(make_error(id, error_code::invalid_params,
                                            "Invalid params: params._meta.progressToken must be a string or an integer"),
                                 served);
        }
        progress_token = *token;
    }

    std::span<const ToolDefinition> tools;
    try {
        tools = host_.tools();
    } catch (...) {
        return served_answer(make_error(id, error_code::internal_error, "Internal error: the tools could not be listed"),
                             served);
    }
    const auto tool = std::find_if(tools.begin(), tools.end(),
                                   [&](const ToolDefinition& definition) { return definition.name == *tool_name; });
    if (tool == tools.end()) {
        return served_answer(make_error(id, error_code::invalid_params, "Unknown tool: " + *tool_name), served);
    }

    if (options_.check_arguments) {
        std::optional<std::string> failure;
        try {
            failure = options_.check_arguments(*tool, arguments);
        } catch (...) {
            return served_answer(
                make_error(id, error_code::internal_error, "Internal error: the arguments could not be checked"), served);
        }
        if (failure) {
            switch (served.invalid_arguments) {
                case InvalidArguments::error_result: {
                    ToolResult result;
                    TextContent text;
                    text.text = *failure;
                    result.content.emplace_back(std::move(text));
                    result.is_error = true;
                    return served_answer(make_result(id, call_result(result, profile)), served);
                }
                case InvalidArguments::invalid_params_error:
                    return served_answer(make_error(id, error_code::invalid_params,
                                                    "Invalid arguments for tool " + tool->name + ": " + *failure),
                                         served);
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
    call->unit = place.unit;
    call->batch = place.batch;
    call->batch_index = place.index;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return std::nullopt;
        // A unit abandoned while it was read: when a closed stream cancels the call, it is not started.
        if (place.unit && place.unit->abandoned && served.cancel->stops_on_stream_close()) return std::nullopt;
        if (place.unit && !place.batch) place.unit->profile = served.profile;
        in_flight_.emplace(id, call);
        if (place.batch) ++place.batch->outstanding;
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

std::optional<Answer> SessionCore::on_discover(Message& request, const Served& served, Origin&, const Place&) {
    const profile::Profile& profile = *served.profile;
    json body = json::object();
    body["supportedVersions"] = supported_versions(served.open);
    body["capabilities"] = capabilities(profile);
    if (options_.instructions) body["instructions"] = *options_.instructions;
    finish_result(body, "DiscoverResult", profile);
    return served_answer(make_result(*request.id, std::move(body)), served);
}

std::optional<Answer> SessionCore::on_set_level(Message& request, const Served& served, Origin& origin, const Place&) {
    const RequestId& id = *request.id;
    if (!options_.protocol_logging || !origin.connection || !served.state->keeps_connection()) {
        return served_answer(make_error(id, error_code::method_not_found, "Method not found: " + request.method), served,
                             RefusalReason::method_not_found);
    }
    const json* level = member(request.params, "level");
    const auto* text = level == nullptr ? nullptr : level->get_ptr<const std::string*>();
    const auto parsed = text == nullptr ? std::nullopt : parse_log_level(*text);
    if (!parsed) {
        return served_answer(make_error(id, error_code::invalid_params, "Invalid params: params.level is not a log level"),
                             served);
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) connection_->log_level = *parsed;
    }
    json body = json::object();
    finish_result(body, "EmptyResult", *served.profile);
    return served_answer(make_result(id, std::move(body)), served);
}

// ---------------------------------------------------------------------------------------------------------------
// Headers

Claims SessionCore::claims_of(std::span<const HeaderField> fields) const {
    Claims claims;
    claims.fields = fields;
    for (const HeaderField& field : fields) {
        for (const std::string& name : version_headers_) {
            if (!same_header_name(field.name, name)) continue;
            if (claims.versions == 0) {
                claims.version_name = name;
                claims.version = field.value;
            }
            ++claims.versions;
            break;
        }
    }
    if (claims.versions == 1) {
        for (const Served& served : served_) {
            if (served.profile->revision() == claims.version) claims.named = &served;
        }
    }
    return claims;
}

std::optional<Answer> SessionCore::version_refusal(const Claims& claims, const std::optional<RequestId>& id) const {
    if (claims.versions > 1) {
        return header_mismatch(id,
                               std::string(header_mismatch_prefix) + "the " + std::string(claims.version_name) +
                                   " header appears more than once",
                               {});
    }
    if (claims.versions == 1 && claims.named == nullptr) return unsupported_version_header(claims, id);
    return std::nullopt;
}

Answer SessionCore::unsupported_version_header(const Claims& claims, const std::optional<RequestId>& id) const {
    const Served* connection = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) connection = connection_->served;
    }
    json data = json::object();
    data["supported"] = supported_versions();
    data["requested"] = std::string(claims.version);
    auto [code, governing] = error_from(unsupported_version_error, {connection}, error_code::invalid_request);
    return Answer{make_error(id, code,
                             "Unsupported protocol version: the " + std::string(claims.version_name) +
                                 " header names a revision this server does not serve",
                             std::move(data)),
                  RefusalReason::unsupported_version_header, std::move(governing)};
}

Answer SessionCore::header_mismatch(const std::optional<RequestId>& id, std::string message,
                                    std::initializer_list<const Served*> preferred) const {
    auto [code, governing] = error_from(header_mismatch_error, preferred, error_code::invalid_request);
    return Answer{make_error(id, code, std::move(message)), RefusalReason::header_mismatch, std::move(governing)};
}

std::pair<int, profile::ProfilePtr> SessionCore::error_from(std::string_view name,
                                                            std::initializer_list<const Served*> preferred,
                                                            int fallback) const {
    for (const Served* served : preferred) {
        if (served == nullptr) continue;
        if (const auto code = served->profile->error_code(name)) return {*code, served->profile};
    }
    for (const Served& served : served_) {  // newest first
        if (const auto code = served.profile->error_code(name)) return {*code, served.profile};
    }
    for (const Served* served : preferred) {
        if (served != nullptr) return {fallback, served->profile};
    }
    return {fallback, nullptr};
}

bool SessionCore::opens_connection(const Message& request) const {
    return std::any_of(openers_.begin(), openers_.end(),
                       [&](const auto& entry) { return entry.second->opens_connection(request); });
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
        // The response is the last output of the call's unit.
        push(lock,
             Queued{make_output(std::move(line), OutputKind::response, call->unit != nullptr, RefusalReason::none,
                                call->profile),
                    call, call->unit},
             Writer::host);
    }
    lock.unlock();
    if (batch_finished) emit_batch(call->batch, Writer::host);
}

void SessionCore::cancel(const RequestId& id, const CancelMechanism& mechanism, const std::string& method) {
    std::shared_ptr<CallState> call;
    bool batch_finished = false;
    bool ended = false;  // an end marker of the call's unit was queued
    bool now_idle = false;
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
        if (call->batch) {
            batch_finished = --call->batch->outstanding == 0 && !call->batch->reading;
        } else if (call->unit) {
            // No answer will come, so the unit ends here. The marker has no owner: the drainer must not skip it.
            ended = enqueue(Queued{make_output({}, OutputKind::end, true, RefusalReason::none, call->profile), nullptr,
                                   call->unit});
        }
        now_idle = !ended && !closed_ && idle_locked();
    }
    call->stop.request_stop();
    if (ended) {
        std::unique_lock<std::mutex> lock(mutex_);
        drain(lock, Writer::reader);
    }
    if (batch_finished) emit_batch(call->batch, Writer::reader);
    if (now_idle) signal_idle();
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
        emit(Queued{make_output(serialize(*answer), OutputKind::response, batch->unit != nullptr, RefusalReason::none,
                                batch->profile),
                    nullptr, batch->unit},
             writer);
    } else if (batch->unit) {
        // Nothing in the batch is answered: the unit ends without an answer.
        emit(Queued{make_output({}, OutputKind::end, true, RefusalReason::none, batch->profile), nullptr, batch->unit},
             writer);
    }
    bool now_idle = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        --pending_batches_;
        now_idle = !closed_ && idle_locked();
    }
    if (now_idle) signal_idle();
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
    push(lock,
         Queued{make_output(std::move(line), OutputKind::notification, false, RefusalReason::none, call->profile), call,
                call->unit},
         Writer::host);
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
    push(lock,
         Queued{make_output(std::move(line), OutputKind::notification, false, RefusalReason::none, call->profile), call,
                call->unit},
         Writer::host);
}

// ---------------------------------------------------------------------------------------------------------------
// Output

bool SessionCore::enqueue(Queued queued) {
    if (closed_) return false;
    if (queued.unit) {
        UnitState& unit = *queued.unit;
        if (unit.finished || unit.abandoned) return false;  // nothing follows the last output of a unit
        queued.output.unit = unit.id;
        if (queued.output.last) unit.finished = true;
    }
    queue_.push_back(std::move(queued));
    return true;
}

void SessionCore::push(std::unique_lock<std::mutex>& lock, Queued queued, Writer writer) {
    if (enqueue(std::move(queued))) {
        drain(lock, writer);
        return;
    }
    if (!closed_ && options_.on_idle && idle_locked()) {
        lock.unlock();
        signal_idle();
        lock.lock();
    }
}

void SessionCore::emit(Queued queued, Writer writer) {
    std::unique_lock<std::mutex> lock(mutex_);
    push(lock, std::move(queued), writer);
}

void SessionCore::emit_answer(Answer answer, const Unit& unit, Writer writer) {
    emit(Queued{make_output(serialize(answer.message), OutputKind::response, unit != nullptr, answer.refusal,
                            std::move(answer.profile)),
                nullptr, unit},
         writer);
}

void SessionCore::emit_refusal(const Unit& unit, int code, std::string message, RefusalReason reason) {
    emit_answer(Answer{make_error(std::nullopt, code, std::move(message)), reason, nullptr}, unit, Writer::reader);
}

void SessionCore::end_unit(const Unit& unit, Writer writer) {
    std::unique_lock<std::mutex> lock(mutex_);
    profile::ProfilePtr profile = unit->profile;
    if (!profile && connection_) profile = connection_->served->profile;
    // A batch of notifications ended its unit already when it was assembled; enqueue() drops a second end.
    if (enqueue(Queued{make_output({}, OutputKind::end, true, RefusalReason::none, std::move(profile)), nullptr, unit})) {
        drain(lock, writer);
    }
}

void SessionCore::drain(std::unique_lock<std::mutex>& lock, Writer writer) {
    if (draining_) return;  // the thread that drains writes this output too
    draining_ = true;
    drainer_ = std::this_thread::get_id();
    std::exception_ptr failure;
    // After close() the queue holds only the end markers close() put there.
    while (!queue_.empty()) {
        Queued queued = std::move(queue_.front());
        queue_.pop_front();
        if (queued.unit && queued.output.last) {
            // The unit ends here: from now on close() leaves it alone and its number may be used again.
            const auto open = units_.find(queued.unit->id);
            if (open != units_.end() && open->second == queued.unit) units_.erase(open);
        }
        if (queued.owner && queued.owner->cancelled) continue;
        if (queued.unit && queued.unit->abandoned) continue;
        writing_ = queued.unit.get();
        lock.unlock();
        try {
            sink_(std::move(queued.output));
        } catch (...) {
            failure = std::current_exception();
        }
        lock.lock();
        writing_ = nullptr;
        if (abandoning_ > 0) written_.notify_all();
        if (failure) break;
    }
    draining_ = false;
    drainer_ = std::thread::id();
    written_.notify_all();
    if (failure) {
        // A sink that throws cannot carry the connection any further: it is not called again.
        broken_ = true;
        queue_.clear();
        lock.unlock();
        close();
        lock.lock();
        if (writer == Writer::reader) std::rethrow_exception(failure);
        // Through a tool's handle: the reading thread gets it when receive() returns; a tool's thread never does.
        if (reading_thread_ == std::this_thread::get_id() && !reader_failure_) reader_failure_ = failure;
        return;
    }
    if (!closed_ && options_.on_idle && idle_locked()) {
        lock.unlock();
        signal_idle();
        lock.lock();
    }
}

bool SessionCore::idle_locked() const {
    // A queued line implies a drainer, so the queue is checked only for the sake of the invariant. A closed session
    // has nothing left to wait for.
    return closed_ || (in_flight_.empty() && queue_.empty() && !draining_ && pending_batches_ == 0);
}

bool SessionCore::idle() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return idle_locked();
}

profile::ProfilePtr SessionCore::connection_profile() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return connection_ ? connection_->served->profile : nullptr;
}

void SessionCore::signal_idle() noexcept {
    if (!options_.on_idle) return;
    try {
        options_.on_idle();
    } catch (...) {
    }
}

void SessionCore::abandon(UnitId number) {
    std::vector<std::shared_ptr<CallState>> stopped;
    std::vector<Batch> assembled;
    bool now_idle = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto found = units_.find(number);
        if (found == units_.end()) return;  // unknown, ended, or the session closed
        const Unit unit = found->second;
        units_.erase(found);
        unit->abandoned = true;
        // A call whose cancellation mechanism takes a closed stream for a cancellation stops; the others run on.
        for (auto it = in_flight_.begin(); it != in_flight_.end();) {
            const std::shared_ptr<CallState> call = it->second;
            if (call->unit != unit || !call->served->cancel->stops_on_stream_close()) {
                ++it;
                continue;
            }
            it = in_flight_.erase(it);
            call->active = false;
            call->cancelled = true;
            if (call->batch && --call->batch->outstanding == 0 && !call->batch->reading) assembled.push_back(call->batch);
            stopped.push_back(call);
        }
        // An output of the unit that another thread is writing now is the last one the transport gets.
        if (drainer_ != std::this_thread::get_id()) {
            ++abandoning_;
            written_.wait(lock, [&] { return writing_ != unit.get(); });
            --abandoning_;
        }
        now_idle = assembled.empty() && !closed_ && idle_locked();
    }
    for (const auto& call : stopped) call->stop.request_stop();
    for (const auto& batch : assembled) emit_batch(batch, Writer::host);
    if (now_idle) signal_idle();
}

void SessionCore::close() {
    std::vector<std::shared_ptr<CallState>> calls;
    std::unique_lock<std::mutex> lock(mutex_);
    const bool inside_sink = drainer_ == std::this_thread::get_id();
    if (!closed_) {
        closed_ = true;
        queue_.clear();
        for (auto& entry : in_flight_) {
            entry.second->active = false;
            calls.push_back(entry.second);
        }
        in_flight_.clear();
        // The queue no longer holds the last outputs of the units still open, so each gets an end marker; none when
        // the sink cannot be called again, from inside it or after it failed.
        if (!inside_sink && !broken_) {
            for (const auto& entry : units_) {
                const Unit& unit = entry.second;
                unit->finished = true;
                Queued marker{make_output({}, OutputKind::end, true, RefusalReason::none, unit->profile), nullptr, unit};
                marker.output.unit = unit->id;
                queue_.push_back(std::move(marker));
            }
        }
        units_.clear();
    }
    lock.unlock();
    for (const auto& call : calls) call->stop.request_stop();
    lock.lock();
    if (inside_sink) return;
    // Another drainer writes the end markers it finds; whatever is left is written here.
    for (;;) {
        written_.wait(lock, [&] { return !draining_; });
        if (queue_.empty() || broken_) break;
        drain(lock, Writer::host);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// Public handles

std::variant<std::unique_ptr<Session>, CreateError> Session::create(SessionOptions options, ToolHost& host, Sink sink) {
    OutputSink outputs;
    if (sink) {
        outputs = [lines = std::move(sink)](Output output) {
            if (output.kind != OutputKind::end) lines(std::move(output.line));
        };
    }
    return create_tagged(std::move(options), host, std::move(outputs));
}

std::variant<std::unique_ptr<Session>, CreateError> Session::create_tagged(SessionOptions options, ToolHost& host,
                                                                           OutputSink sink) {
    auto made = detail::SessionCore::create(std::move(options), host, std::move(sink));
    if (auto* error = std::get_if<CreateError>(&made)) return std::move(*error);
    return std::unique_ptr<Session>(new Session(std::get<std::shared_ptr<detail::SessionCore>>(std::move(made))));
}

Session::~Session() {
    if (core_) core_->close();
}

void Session::receive(std::string_view line) {
    const auto core = core_;  // the sink may destroy the session while this runs
    core->receive(std::nullopt, line, {});
}

bool Session::receive(UnitId unit, std::string_view input, std::span<const HeaderField> headers) {
    const auto core = core_;
    return core->receive(unit, input, headers);
}

void Session::abandon(UnitId unit) {
    const auto core = core_;
    core->abandon(unit);
}

void Session::close() {
    const auto core = core_;
    core->close();
}

bool Session::idle() const { return core_->idle(); }

std::span<const profile::ProfilePtr> Session::profiles() const noexcept { return core_->profiles(); }

profile::ProfilePtr Session::connection_profile() const { return core_->connection_profile(); }

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
