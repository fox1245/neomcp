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
// - The change feed. The session registers its listener with the host (ToolHost::watch) and resets the registration
//   with mutex_ released, and a host calls the listener with no lock of its own held, so mutex_ and a lock of the host
//   never nest. A change is queued under mutex_ like any output. A subscription is a call in flight that becomes
//   visible to changes in the same hold of mutex_ that queues its acknowledgement, so nothing precedes that; it ends
//   through the paths of a call (cancellation, abandon(), close()) or when the host ends its feed, and the last two end
//   it with outputs of its own, which a full queue never drops and close() keeps.
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
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "core/headers.h"
#include "core/input.h"
#include "core/request_state.h"
#include "neomcp/feature_host.h"
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
        case RefusalReason::missing_client_capability: return "missing_client_capability";
    }
    return {};
}

std::string_view to_string(RequestStateProblem problem) noexcept {
    switch (problem) {
        case RequestStateProblem::malformed: return "malformed";
        case RequestStateProblem::signature: return "signature";
        case RequestStateProblem::version: return "version";
        case RequestStateProblem::expired: return "expired";
        case RequestStateProblem::binding: return "binding";
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
constexpr std::string_view resource_not_found_error = "resource_not_found";
constexpr std::string_view prompt_reference_type = "ref/prompt";
constexpr std::string_view resource_reference_type = "ref/resource";
constexpr std::string_view missing_capability_error = "missing_required_client_capability";
constexpr std::string_view required_capabilities_member = "requiredCapabilities";
// The reason in the data of a request refused because a bound of the session is reached.
constexpr std::string_view limit_reason = "limit";
// The answer to every refused request state, whatever the cause: the text other implementations use, so that a
// client sees the same refusal from any server.
constexpr std::string_view invalid_state_message = "Invalid or expired requestState";
constexpr std::string_view invalid_state_reason = "invalid_request_state";

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

// Adds the members of `source` to `target`, both objects, merging the objects they both have.
void merge_objects(json& target, const json& source) {
    const auto* members = source.get_ptr<const json::object_t*>();
    if (members == nullptr) return;
    for (const auto& [name, value] : *members) {
        json& slot = target[name];
        if (slot.is_object() && value.is_object()) {
            merge_objects(slot, value);
        } else {
            slot = value;
        }
    }
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
    const InputMechanism* input = nullptr;
    const ChangeFeedMechanism* feed = nullptr;
    InvalidArguments invalid_arguments = InvalidArguments::invalid_params_error;
    // What the profile's entry for the transport says about headers; views into the profile.
    std::span<const profile::RequestHeader> request_headers;
    const profile::EncodedHeaderValue* encoded = nullptr;
    // The headers that mirror tool arguments, where the profile defines them for the transport.
    const profile::ArgumentHeaders* argument_headers = nullptr;
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

// What a call in flight is for; it decides which outcomes may complete it.
enum class CallKind {
    tool,
    resource_read,
    prompt_get,
    completion,
    subscription  // an open subscription of the change feed: it is answered when it ends
};

// What a call asks its host, kept to ask it again when answers arrive.
using HostRequest = std::variant<std::monostate, CallRequest, ReadRequest, PromptRequest>;

// What a call is about, as sealed request state is bound to it: the method, the tool or prompt name or the resource
// URI, and the arguments.
struct CallSubject {
    std::string method;
    std::string name;
    json arguments = json::object();
    HostRequest request;  // the first round's request to the host
    bool retry = false;   // the request carried answers or a state of an earlier round
    std::uint64_t round = 0;  // how many times the call asked before, by its sealed state
};

struct WaitState;

struct CallState {
    std::weak_ptr<SessionCore> core;
    CallKind kind = CallKind::tool;
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
    CallSubject subject;

    // Guarded by the core's mutex.
    bool active = true;      // false once answered, cancelled or closed
    bool cancelled = false;  // the client cancelled it
    std::optional<double> last_progress;
    std::size_t rounds = 0;            // rounds of requests sent to the client
    std::shared_ptr<WaitState> wait;   // the round that waits for its answers; null when none

    // A subscription: what of its request is honoured. Written before the call is in flight.
    ChangeFilter filter;

    explicit CallState(RequestId request_id) : id(std::move(request_id)) {}
};

// One round of requests a call sent to the client, until every answer arrived or the wait ended otherwise.
struct WaitState {
    std::weak_ptr<SessionCore> core;
    std::weak_ptr<CallState> call;
    std::stop_source ended;
    std::optional<std::string> state;  // the host's, given back with the answers

    // Guarded by the core's mutex.
    bool open = true;
    std::map<std::string, std::string> asked;  // key -> method of the request
    std::vector<RequestId> ids;                // the ids the round's requests were sent with
    std::size_t outstanding = 0;
    InputResponses answers;
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
    void complete(const std::shared_ptr<CallState>& call, FeatureOutcome outcome);
    void complete_dropped(const std::shared_ptr<CallState>& call);
    // Ends a wait as timed out (InputWait::expire).
    void expire(const std::shared_ptr<WaitState>& wait);
    bool client_supports(const CallState& call, const InputRequest& request) const;
    void progress(const std::shared_ptr<CallState>& call, double value, std::optional<double> total,
                  std::optional<std::string> message);
    void log(const std::shared_ptr<CallState>& call, LogLevel level, json data, std::optional<std::string> logger);

    // ---- the host's changes (through the session's ChangeListener)
    void on_change(const Change& change);
    void on_feed_end();

    std::span<const profile::ProfilePtr> profiles() const noexcept { return profiles_; }

private:
    struct Queued {
        Output output;                     // its unit number is filled in from `unit` when it is queued
        std::shared_ptr<CallState> owner;  // the call the output belongs to; null for others
        Unit unit;                         // the unit it belongs to; null when it belongs to the connection
        // An acknowledgement or a graceful end of a subscription: never dropped for a full queue, and kept by close().
        bool essential = false;
    };

    struct Connection {
        const Served* served = nullptr;
        ClientIdentity client;
        LogLevel log_level = LogLevel::info;
        // Under a change feed that delivers to the connection: whether the client said it is ready, and the
        // resources whose updates it watches.
        bool ready = false;
        std::set<std::string, std::less<>> watched;
    };

    // Where the session stands with the host's changes. Only the reading thread registers.
    enum class Watch {
        none,         // not registered
        registering,  // the reading thread is registering
        registered,
        ended         // the host takes no registration, or stopped reporting: never registered again
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
    void on_response(const Message& response);
    std::optional<Answer> on_request(Message request, const Place& place);
    std::optional<Answer> serve_named(Message request, const OpenMechanism& opener, const std::string& revision,
                                      const Place& place);
    Answer open_connection(const Message& request, const OpenMechanism& opener, bool in_batch);
    std::optional<Answer> serve(Message request, const Served& served, Origin origin, const Place& place);

    std::optional<Answer> on_ping(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_tools_list(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_tools_call(Message& request, const Served& served, Origin& origin, const Place& place);
    std::optional<Answer> on_discover(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_resources_list(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_resource_templates_list(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_resources_read(Message& request, const Served& served, Origin& origin, const Place& place);
    std::optional<Answer> on_completion(Message& request, const Served& served, Origin& origin, const Place& place);
    std::optional<Answer> on_prompts_list(Message& request, const Served& served, Origin&, const Place&);
    std::optional<Answer> on_prompts_get(Message& request, const Served& served, Origin& origin, const Place& place);
    std::optional<Answer> on_set_level(Message& request, const Served& served, Origin& origin, const Place&);
    static const std::map<std::string, Handler, std::less<>>& handlers();

    // ---- the change feed
    std::optional<Answer> on_feed(Message& request, const Served& served, Origin& origin, const Place& place);
    std::optional<Answer> on_watch(const Message& request, const FeedRequest& asked, const Served& served,
                                   const Origin& origin);
    std::optional<Answer> on_listen(const Message& request, const FeedRequest& asked, const Served& served,
                                    Origin& origin, const Place& place);
    // The outputs that end a subscription the server tears down, in order; with mutex_ held.
    void ending_outputs(const std::shared_ptr<CallState>& call, std::vector<Queued>& out) const;
    // Registers the session for the host's changes, once; false when the host takes no registration.
    bool watch_host();
    // The line of a notification of the feed under `served`; empty when the profile has no such notification.
    std::optional<std::string> feed_line(const Served& served, FeedMessage message) const;
    Answer limit_refusal(const RequestId& id, std::string message, const Served& served) const;

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
    // The kinds of change the host reports among the kinds it has things of: what the session declares.
    ChangeSupport reported() const;
    void finish_result(json& body, std::string_view type, const profile::Profile& profile) const;
    void finish_result(json& body, std::string_view type, const profile::Profile& profile, const CacheHints& cache) const;
    json call_result(const ToolResult& result, const profile::Profile& profile) const;
    // The page of a list of `total` items that a request asks for: the indices of the next `page_size` items from the
    // request's cursor that `enabled` admits, and the cursor of the page after it; or the answer to a bad cursor. A
    // cursor is the index of an item in the whole list, so that switching items on and off moves no page.
    struct Page {
        std::vector<std::size_t> items;
        std::optional<std::size_t> next;  // empty: no admitted item follows
    };
    std::variant<Page, Answer> page_of(const Message& request, const Served& served, std::size_t total,
                                       const std::optional<std::size_t>& page_size,
                                       const std::function<bool(std::size_t)>& enabled) const;
    // A list result with its page of items already written as `member`.
    Answer list_answer(const Message& request, const Served& served, std::string_view member, json items,
                       const std::optional<std::size_t>& next, std::string_view type, const CacheHints& cache) const;
    static Answer method_not_found(const Message& request, const Served& served);
    // Registers a call of `kind` that the host will complete; null when nothing is to be started (the session closed,
    // or the unit was abandoned).
    std::shared_ptr<CallState> begin_call(CallKind kind, const RequestId& id, const Served& served, Origin& origin,
                                          const Place& place, json progress_token, CallSubject subject);
    // Hands a call's request to its host (no lock held).
    void start(const std::shared_ptr<CallState>& call, const HostRequest& request);

    // ---- input requests
    // What a request carries about an earlier round of its call, read under the call's input mechanism: the answers
    // and the host's state go into `subject.request`. The answer when the state is refused or an answer is
    // malformed; empty otherwise.
    std::optional<Answer> take_retry(const Message& request, const Served& served, CallSubject& subject) const;
    Answer state_refusal(const RequestId& id, RequestStateProblem problem, const std::string& method) const;
    // The call asks for input: its result asks, or its requests go to the client and it waits.
    void ask(const std::shared_ptr<CallState>& call, InputRequired asked);
    // The host is called again with the answers of a round.
    void continue_call(const std::shared_ptr<CallState>& call, InputResponses answers, std::optional<std::string> state);
    // Ends the call with the error its kind has for a failure to get input: a tool execution error for a tool, an
    // internal error otherwise.
    void fail_call(const std::shared_ptr<CallState>& call, const std::string& text, json data = nullptr);
    // Ends a wait; with mutex_ held. Stop on its token is the caller's to request, without the lock.
    void close_wait(const std::shared_ptr<WaitState>& wait);
    std::chrono::system_clock::time_point now() const;
    // The progress token of a request's `_meta`, null when it has none; the answer when it is malformed.
    std::variant<json, Answer> progress_token_of(const Message& request, const Served& served) const;
    void cancel(const RequestId& id, const CancelMechanism& mechanism, const std::string& method);
    void finish_call(const std::shared_ptr<CallState>& call, json response,
                     RefusalReason refusal = RefusalReason::none);
    void emit_batch(const Batch& batch, Writer writer);

    // ---- output
    // Puts an output in the queue unless the session closed or its unit finished; with mutex_ held.
    bool enqueue(Queued queued);
    // Queues an output and drains; when it is dropped, tells the idle callback what a drain would have told it.
    void push(std::unique_lock<std::mutex>& lock, Queued queued, Writer writer);
    void emit(Queued queued, Writer writer);
    void emit_answer(Answer answer, const Unit& unit, const Claims& claims, Writer writer);
    void emit_refusal(const Unit& unit, const Claims& claims, int code, std::string message, RefusalReason reason);
    // The profile of the client that sent what is being answered, as far as it is known: the connection's, else the
    // unit's, else the one its version header names; null when none is.
    profile::ProfilePtr client_profile(const Unit& unit, const Claims* claims) const;
    // Leaves `id` out of an error answer that has none to give where the client's revision allows it: an answer to
    // what could not be read has `id` null otherwise, which the revisions' schemas do not allow either, but which is the
    // form JSON-RPC prescribes. When the client's revision is not known the member is left out only if every served
    // revision allows it.
    void conform_id(json& message, const profile::ProfilePtr& client) const;
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
    std::map<std::string, std::unique_ptr<InputMechanism>, std::less<>> inputs_;
    std::map<std::string, std::unique_ptr<ChangeFeedMechanism>, std::less<>> feeds_;
    std::optional<StateSealer> sealer_;  // present when a served revision's results carry sealed state
    // What the session registers with the host for its changes; it holds the core weakly.
    std::shared_ptr<ChangeListener> listener_;

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
    // The requests sent to the client whose responses are awaited, by the ids the session gave them.
    struct Outgoing {
        std::shared_ptr<WaitState> wait;
        std::string key;
    };
    std::map<RequestId, Outgoing> outgoing_;
    std::uint64_t next_outgoing_ = 0;
    Watch watch_ = Watch::none;
    ChangeRegistration registration_;  // reset without the lock held
    std::size_t listening_ = 0;        // the calls in flight that are open subscriptions
};

// The listener a session registers with its host: it holds the session weakly, so that a registration the host keeps
// longer than the session reaches nothing.
class FeedListener final : public ChangeListener {
public:
    explicit FeedListener(std::weak_ptr<SessionCore> core) : core_(std::move(core)) {}

    void changed(const Change& change) override {
        if (auto core = core_.lock()) {
            try {
                core->on_change(change);
            } catch (...) {
                // A change that cannot be written (no memory) is a change the client does not hear of.
            }
        }
    }

    void ended() override {
        if (auto core = core_.lock()) {
            try {
                core->on_feed_end();
            } catch (...) {
            }
        }
    }

private:
    std::weak_ptr<SessionCore> core_;
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
    if (options.max_input_rounds == 0) problem("max_input_rounds is zero");
    if (options.max_watched_resources == 0) problem("max_watched_resources is zero");
    if (options.max_subscriptions == 0) problem("max_subscriptions is zero");
    if (options.request_state_ttl <= std::chrono::seconds::zero()) problem("request_state_ttl is not positive");
    for (const auto& key : options.request_state_keys) {
        if (key.size() < min_state_key_size) {
            problem("a request state key has " + std::to_string(key.size()) + " bytes; it needs at least " +
                    std::to_string(min_state_key_size));
        }
    }

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
        const std::string_view input_name = profile->mechanism(profile::Slot::server_input);
        served.input = find_or_make(core->inputs_, input_name,
                                    [&](std::string_view name) { return registry.make_input(name); });
        if (served.input == nullptr) {
            missing(revision, MechanismSlot::server_input, input_name);
        } else if (auto why = served.input->check(*profile)) {
            problem("revision " + revision + ": the server_input mechanism '" + std::string(input_name) +
                    "' cannot serve it: " + *why);
        }
        const std::string_view feed_name = profile->mechanism(profile::Slot::change_feed);
        served.feed = find_or_make(core->feeds_, feed_name,
                                   [&](std::string_view name) { return registry.make_feed(name); });
        if (served.feed == nullptr) {
            missing(revision, MechanismSlot::change_feed, feed_name);
        } else if (auto why = served.feed->check(*profile)) {
            problem("revision " + revision + ": the change_feed mechanism '" + std::string(feed_name) +
                    "' cannot serve it: " + *why);
        } else if (served.feed->delivery() == FeedDelivery::connection && served.state != nullptr &&
                   !served.state->keeps_connection()) {
            problem("revision " + revision + ": the change_feed mechanism '" + std::string(feed_name) +
                    "' delivers to the connection, which the state mechanism '" + std::string(state_name) +
                    "' does not keep");
        }
        if (transport == profile::Transport::streamable_http) {
            const profile::StreamableHttpTransport& http = *profile->streamable_http();
            served.request_headers = http.request_headers;
            if (http.encoded_header_value) served.encoded = &*http.encoded_header_value;
            if (http.argument_headers) served.argument_headers = &*http.argument_headers;
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
    // Sealed request state, where a served revision's results carry it.
    const bool seals = std::any_of(core->served_.begin(), core->served_.end(), [](const Served& served) {
        return served.input != nullptr && served.input->delivery() == InputDelivery::result;
    });
    if (seals && error.message.empty()) {
        std::vector<std::string> keys = core->options_.request_state_keys;
        if (keys.empty()) {
            if (const auto& made = default_state_key()) {
                keys.push_back(*made);
            } else {
                problem("no request state key: none was given and the operating system's random source failed");
            }
        }
        if (!keys.empty()) core->sealer_.emplace(std::move(keys));
    }

    if (!error.message.empty()) return error;
    core->listener_ = std::make_shared<FeedListener>(core);
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
        emit_refusal(unit, claims, error_code::invalid_request,
                     "Invalid Request: the message is longer than " + std::to_string(options_.max_message_bytes) +
                         " bytes",
                     RefusalReason::too_long);
        return true;
    }
    if (nesting_exceeds(line, options_.max_nesting_depth)) {
        emit_refusal(unit, claims, error_code::invalid_request,
                     "Invalid Request: the message nests deeper than " + std::to_string(options_.max_nesting_depth) +
                         " levels",
                     RefusalReason::too_deep);
        return true;
    }
    json value = json::parse(line, nullptr, false);
    if (value.is_discarded()) {
        emit_refusal(unit, claims, error_code::parse_error, "Parse error", RefusalReason::parse_error);
        return true;
    }
    if (value.is_array()) return receive_array(std::move(value), unit, claims);
    Handled handled = handle(std::move(value), Place{unit, nullptr, 0, &claims});
    if (handled.answer) {
        emit_answer(std::move(*handled.answer), unit, claims, Writer::reader);
        return true;
    }
    return handled.later;
}

bool SessionCore::receive_array(json value, const Unit& unit, const Claims& claims) {
    // The headers belong to the whole batch.
    if (auto refusal = version_refusal(claims, std::nullopt)) {
        emit_answer(std::move(*refusal), unit, claims, Writer::reader);
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
        emit_refusal(unit, claims, error_code::invalid_request, "Invalid Request: this connection does not accept batches",
                     RefusalReason::invalid_request);
        return true;
    }
    auto& items = *value.get_ptr<json::array_t*>();
    if (items.empty()) {
        emit_refusal(unit, claims, error_code::invalid_request, "Invalid Request: the batch is empty",
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
            conform_id(handled.answer->message, profile);
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
        case MessageKind::response:
            // The answer to a request the session sent; it awaits no answer of its own.
            on_response(message);
            return Handled{};
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
    // A client that says it is ready opens the notifications of its connection, where the change feed delivers there.
    bool ready = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_ && !connection_->ready && connection_->served->feed->readies(notification)) {
            const profile::Message* entry = connection_->served->profile->notification(notification.method);
            ready = entry != nullptr && entry->from_client;
            connection_->ready = ready;
        }
    }
    if (!ready) return;  // no other notification a client sends needs anything from this server
    const ChangeSupport reported = this->reported();
    if (reported.tools_list || reported.prompts_list || reported.resources_list || reported.resource_updates) {
        watch_host();
    }
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
                               std::string(header_mismatch_prefix) + "the " + std::string(claims.version_name) +
                                   " header value does not match the revision the body names",
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
    // The requests of the change feed are the mechanism's to name.
    const bool feed = served.feed->serves(request.method);
    if (entry == nullptr || !entry->from_client || (!feed && handler == table.end())) {
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
    if (feed) return on_feed(request, served, origin, place);
    return (this->*(handler->second))(request, served, origin, place);
}

const std::map<std::string, SessionCore::Handler, std::less<>>& SessionCore::handlers() {
    static const std::map<std::string, Handler, std::less<>> table{
        {"ping", &SessionCore::on_ping},
        {"tools/list", &SessionCore::on_tools_list},
        {"tools/call", &SessionCore::on_tools_call},
        {"server/discover", &SessionCore::on_discover},
        {"resources/list", &SessionCore::on_resources_list},
        {"resources/templates/list", &SessionCore::on_resource_templates_list},
        {"resources/read", &SessionCore::on_resources_read},
        {"completion/complete", &SessionCore::on_completion},
        {"prompts/list", &SessionCore::on_prompts_list},
        {"prompts/get", &SessionCore::on_prompts_get},
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

std::variant<SessionCore::Page, Answer> SessionCore::page_of(const Message& request, const Served& served,
                                                              std::size_t total,
                                                              const std::optional<std::size_t>& page_size,
                                                              const std::function<bool(std::size_t)>& enabled) const {
    const RequestId& id = *request.id;
    std::size_t start = 0;
    if (const json* cursor = member(request.params, "cursor")) {
        if (!page_size) {
            return served_answer(
                make_error(id, error_code::invalid_params,
                           "Invalid params: this server returns the list in one page, so no cursor is valid"),
                served);
        }
        const auto* text = cursor->get_ptr<const std::string*>();
        const auto index = text == nullptr ? std::nullopt : parse_cursor(*text, total);
        if (!index) return served_answer(make_error(id, error_code::invalid_params, "Invalid params: invalid cursor"), served);
        start = *index;
    }
    Page page;
    for (std::size_t index = start; index < total; ++index) {
        if (!enabled(index)) continue;
        if (page_size && page.items.size() == *page_size) {
            page.next = index;
            break;
        }
        page.items.push_back(index);
    }
    return page;
}

Answer SessionCore::list_answer(const Message& request, const Served& served, std::string_view member_name, json items,
                                const std::optional<std::size_t>& next, std::string_view type,
                                const CacheHints& cache) const {
    json body = json::object();
    body[std::string(member_name)] = std::move(items);
    if (next) body["nextCursor"] = std::to_string(*next);
    finish_result(body, type, *served.profile, cache);
    return served_answer(make_result(*request.id, std::move(body)), served);
}

Answer SessionCore::method_not_found(const Message& request, const Served& served) {
    return served_answer(make_error(*request.id, error_code::method_not_found, "Method not found: " + request.method),
                         served, RefusalReason::method_not_found);
}

std::optional<Answer> SessionCore::on_tools_list(Message& request, const Served& served, Origin&, const Place&) {
    const RequestId& id = *request.id;
    const profile::Profile& profile = *served.profile;
    std::span<const ToolDefinition> tools;
    std::variant<Page, Answer> page;
    try {
        tools = host_.tools();
        page = page_of(request, served, tools.size(), options_.tools_page_size,
                       [this](std::size_t index) { return host_.tool_enabled(index); });
    } catch (...) {
        return served_answer(make_error(id, error_code::internal_error, "Internal error: the tools could not be listed"),
                             served);
    }
    if (auto* refusal = std::get_if<Answer>(&page)) return std::move(*refusal);
    const Page& found = std::get<Page>(page);
    json list = json::array();
    for (const std::size_t index : found.items) list.push_back(to_wire(tools[index], profile));
    return list_answer(request, served, "tools", std::move(list), found.next, "ListToolsResult", options_.tools_cache);
}

std::variant<json, Answer> SessionCore::progress_token_of(const Message& request, const Served& served) const {
    using Result = std::variant<json, Answer>;
    const json* meta = member(request.params, "_meta");
    if (const json* token = meta == nullptr ? nullptr : member(*meta, "progressToken")) {
        if (!token->is_string() && !token->is_number_integer()) {
            return Result(std::in_place_index<1>,
                          served_answer(make_error(*request.id, error_code::invalid_params,
                                                   "Invalid params: params._meta.progressToken must be a string or an "
                                                   "integer"),
                                        served));
        }
        return Result(std::in_place_index<0>, *token);
    }
    return Result(std::in_place_index<0>, json(nullptr));
}

std::shared_ptr<CallState> SessionCore::begin_call(CallKind kind, const RequestId& id, const Served& served,
                                                   Origin& origin, const Place& place, json progress_token,
                                                   CallSubject subject) {
    const profile::Profile& profile = *served.profile;
    auto call = std::make_shared<CallState>(id);
    call->subject = std::move(subject);
    call->core = weak_from_this();
    call->kind = kind;
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
        if (closed_) return nullptr;
        // A unit abandoned while it was read: when a closed stream cancels the call, it is not started.
        if (place.unit && place.unit->abandoned && served.cancel->stops_on_stream_close()) return nullptr;
        if (place.unit && !place.batch) place.unit->profile = served.profile;
        in_flight_.emplace(id, call);
        if (place.batch) ++place.batch->outstanding;
    }
    return call;
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
    auto token = progress_token_of(request, served);
    if (auto* refusal = std::get_if<Answer>(&token)) return std::move(*refusal);
    json progress_token = std::move(std::get<json>(token));

    // A tool that is switched off is answered as one that does not exist. The check is the moment a call is decided:
    // one admitted here runs even when the tool is switched off before it starts.
    std::span<const ToolDefinition> tools;
    auto tool = tools.end();
    try {
        tools = host_.tools();
        tool = std::find_if(tools.begin(), tools.end(),
                            [&](const ToolDefinition& definition) { return definition.name == *tool_name; });
        if (tool != tools.end() && !host_.tool_enabled(static_cast<std::size_t>(tool - tools.begin()))) {
            tool = tools.end();
        }
    } catch (...) {
        return served_answer(make_error(id, error_code::internal_error, "Internal error: the tools could not be listed"),
                             served);
    }
    if (tool == tools.end()) {
        return served_answer(make_error(id, error_code::invalid_params, "Unknown tool: " + *tool_name), served);
    }

    // What the headers claim about the arguments is checked before the arguments are checked against the schema: a
    // header that contradicts the body is refused as such, whatever else is wrong with the call.
    if (served.argument_headers != nullptr) {
        if (auto failure = check_argument_headers(arguments, tool->input_schema, place.claims->fields,
                                                  *served.argument_headers, served.encoded)) {
            if (failure->defect) {
                return served_answer(make_error(id, error_code::internal_error,
                                                "Internal error: the tool " + tool->name + " cannot be called: " +
                                                    failure->message),
                                     served);
            }
            return header_mismatch(id, std::move(failure->message), {&served});
        }
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

    CallSubject subject;
    subject.method = request.method;
    subject.name = *tool_name;
    subject.arguments = arguments;
    CallRequest call_request;
    call_request.name = *tool_name;
    call_request.arguments = std::move(arguments);
    if (meta != nullptr) call_request.meta = *meta;
    subject.request = std::move(call_request);
    if (auto refusal = take_retry(request, served, subject)) return refusal;

    auto call = begin_call(CallKind::tool, id, served, origin, place, std::move(progress_token), std::move(subject));
    if (!call) return std::nullopt;
    start(call, call->subject.request);
    return std::nullopt;
}

void SessionCore::start(const std::shared_ptr<CallState>& call, const HostRequest& request) {
    try {
        if (const auto* tool = std::get_if<CallRequest>(&request)) {
            host_.call(*tool, CallControl(call), Completion(std::make_shared<CompletionState>(call)));
        } else if (const auto* read = std::get_if<ReadRequest>(&request)) {
            if (ResourceHost* resources = host_.resource_host()) {
                resources->read(*read, CallControl(call), Completion(std::make_shared<CompletionState>(call)));
            }
        } else if (const auto* get = std::get_if<PromptRequest>(&request)) {
            if (PromptHost* prompts = host_.prompt_host()) {
                prompts->get(*get, CallControl(call), Completion(std::make_shared<CompletionState>(call)));
            }
        }
    } catch (...) {
        // The completion answers with an internal error once its last copy is gone.
    }
}

std::optional<Answer> SessionCore::on_resources_list(Message& request, const Served& served, Origin&, const Place&) {
    ResourceHost* host = host_.resource_host();
    if (host == nullptr) return method_not_found(request, served);
    std::span<const ResourceDefinition> resources;
    ListSettings settings;
    std::variant<Page, Answer> page;
    try {
        resources = host->resources();
        settings = host->resource_list_settings();
        page = page_of(request, served, resources.size(), settings.page_size,
                       [host](std::size_t index) { return host->resource_enabled(index); });
    } catch (...) {
        return served_answer(
            make_error(*request.id, error_code::internal_error, "Internal error: the resources could not be listed"), served);
    }
    if (auto* refusal = std::get_if<Answer>(&page)) return std::move(*refusal);
    const Page& found = std::get<Page>(page);
    json list = json::array();
    for (const std::size_t index : found.items) list.push_back(to_wire(resources[index], *served.profile));
    return list_answer(request, served, "resources", std::move(list), found.next, "ListResourcesResult",
                       settings.cache);
}

std::optional<Answer> SessionCore::on_resource_templates_list(Message& request, const Served& served, Origin&,
                                                              const Place&) {
    ResourceHost* host = host_.resource_host();
    if (host == nullptr) return method_not_found(request, served);
    std::span<const ResourceTemplateDefinition> templates;
    ListSettings settings;
    std::variant<Page, Answer> page;
    try {
        templates = host->resource_templates();
        settings = host->resource_list_settings();
        page = page_of(request, served, templates.size(), settings.page_size,
                       [host](std::size_t index) { return host->template_enabled(index); });
    } catch (...) {
        return served_answer(make_error(*request.id, error_code::internal_error,
                                        "Internal error: the resource templates could not be listed"),
                             served);
    }
    if (auto* refusal = std::get_if<Answer>(&page)) return std::move(*refusal);
    const Page& found = std::get<Page>(page);
    json list = json::array();
    for (const std::size_t index : found.items) list.push_back(to_wire(templates[index], *served.profile));
    return list_answer(request, served, "resourceTemplates", std::move(list), found.next, "ListResourceTemplatesResult",
                       settings.cache);
}

std::optional<Answer> SessionCore::on_resources_read(Message& request, const Served& served, Origin& origin,
                                                     const Place& place) {
    ResourceHost* host = host_.resource_host();
    if (host == nullptr) return method_not_found(request, served);
    const RequestId& id = *request.id;
    const json* uri = member(request.params, "uri");
    const auto* text = uri == nullptr ? nullptr : uri->get_ptr<const std::string*>();
    if (text == nullptr) {
        return served_answer(make_error(id, error_code::invalid_params, "Invalid params: params.uri must be a string"),
                             served);
    }
    auto token = progress_token_of(request, served);
    if (auto* refusal = std::get_if<Answer>(&token)) return std::move(*refusal);

    ReadRequest read;
    read.uri = *text;
    if (const json* meta = member(request.params, "_meta")) read.meta = *meta;
    CallSubject subject;
    subject.method = request.method;
    subject.name = *text;
    subject.request = std::move(read);
    if (auto refusal = take_retry(request, served, subject)) return refusal;

    auto call = begin_call(CallKind::resource_read, id, served, origin, place, std::move(std::get<json>(token)),
                           std::move(subject));
    if (!call) return std::nullopt;
    try {
        host->read(std::get<ReadRequest>(call->subject.request), CallControl(call),
                   Completion(std::make_shared<CompletionState>(call)));
    } catch (...) {
        // The completion answers with an internal error once its last copy is gone.
    }
    return std::nullopt;
}

std::optional<Answer> SessionCore::on_completion(Message& request, const Served& served, Origin& origin,
                                                 const Place& place) {
    CompletionHost* host = host_.completion_host();
    if (host == nullptr) return method_not_found(request, served);
    const RequestId& id = *request.id;
    const auto invalid = [&](std::string message) {
        return served_answer(make_error(id, error_code::invalid_params, "Invalid params: " + message), served);
    };
    const auto text_of = [](const json* value) -> const std::string* {
        return value == nullptr ? nullptr : value->get_ptr<const std::string*>();
    };
    CompletionRequest completion;

    const json* ref = member(request.params, "ref");
    if (ref == nullptr || !ref->is_object()) return invalid("params.ref must be an object");
    const std::string* type = text_of(member(*ref, "type"));
    if (type == nullptr) return invalid("params.ref.type must be a string");
    if (*type == prompt_reference_type) {
        const std::string* name = text_of(member(*ref, "name"));
        if (name == nullptr) return invalid("params.ref.name must be a string");
        completion.kind = CompletionRefKind::prompt;
        completion.reference = *name;
    } else if (*type == resource_reference_type) {
        const std::string* uri = text_of(member(*ref, "uri"));
        if (uri == nullptr) return invalid("params.ref.uri must be a string");
        completion.kind = CompletionRefKind::resource;
        completion.reference = *uri;
    } else {
        return invalid("params.ref.type must be '" + std::string(prompt_reference_type) + "' or '" +
                       std::string(resource_reference_type) + "'");
    }

    const json* argument = member(request.params, "argument");
    if (argument == nullptr || !argument->is_object()) return invalid("params.argument must be an object");
    const std::string* argument_name = text_of(member(*argument, "name"));
    const std::string* argument_value = text_of(member(*argument, "value"));
    if (argument_name == nullptr) return invalid("params.argument.name must be a string");
    if (argument_value == nullptr) return invalid("params.argument.value must be a string");
    completion.argument = *argument_name;
    completion.value = *argument_value;

    // `context.arguments` where the request has it (the revisions that define it); read whenever it is given.
    if (const json* context = member(request.params, "context")) {
        if (!context->is_object()) return invalid("params.context must be an object");
        if (const json* resolved = member(*context, "arguments")) {
            const auto* members = resolved->get_ptr<const json::object_t*>();
            if (members == nullptr) return invalid("params.context.arguments must be an object");
            for (const auto& [name, value] : *members) {
                const std::string* text = value.get_ptr<const std::string*>();
                if (text == nullptr) return invalid("params.context.arguments." + name + " must be a string");
                completion.resolved.emplace(name, *text);
            }
        }
    }
    auto token = progress_token_of(request, served);
    if (auto* refusal = std::get_if<Answer>(&token)) return std::move(*refusal);

    CallSubject subject;
    subject.method = request.method;
    subject.name = completion.reference;
    auto call = begin_call(CallKind::completion, id, served, origin, place, std::move(std::get<json>(token)),
                           std::move(subject));
    if (!call) return std::nullopt;
    if (const json* meta = member(request.params, "_meta")) completion.meta = *meta;
    try {
        host->complete(completion, CallControl(call), Completion(std::make_shared<CompletionState>(call)));
    } catch (...) {
        // The completion answers with an internal error once its last copy is gone.
    }
    return std::nullopt;
}

std::optional<Answer> SessionCore::on_prompts_list(Message& request, const Served& served, Origin&, const Place&) {
    PromptHost* host = host_.prompt_host();
    if (host == nullptr) return method_not_found(request, served);
    std::span<const PromptDefinition> prompts;
    ListSettings settings;
    std::variant<Page, Answer> page;
    try {
        prompts = host->prompts();
        settings = host->prompt_list_settings();
        page = page_of(request, served, prompts.size(), settings.page_size,
                       [host](std::size_t index) { return host->prompt_enabled(index); });
    } catch (...) {
        return served_answer(
            make_error(*request.id, error_code::internal_error, "Internal error: the prompts could not be listed"), served);
    }
    if (auto* refusal = std::get_if<Answer>(&page)) return std::move(*refusal);
    const Page& found = std::get<Page>(page);
    json list = json::array();
    for (const std::size_t index : found.items) list.push_back(to_wire(prompts[index], *served.profile));
    return list_answer(request, served, "prompts", std::move(list), found.next, "ListPromptsResult", settings.cache);
}

std::optional<Answer> SessionCore::on_prompts_get(Message& request, const Served& served, Origin& origin,
                                                  const Place& place) {
    PromptHost* host = host_.prompt_host();
    if (host == nullptr) return method_not_found(request, served);
    const RequestId& id = *request.id;
    const auto invalid = [&](std::string message) {
        return served_answer(make_error(id, error_code::invalid_params, "Invalid params: " + message), served);
    };
    const json* name = member(request.params, "name");
    const auto* prompt_name = name == nullptr ? nullptr : name->get_ptr<const std::string*>();
    if (prompt_name == nullptr) return invalid("params.name must be a string");
    PromptRequest get;
    get.name = *prompt_name;
    if (const json* given = member(request.params, "arguments")) {
        const auto* members = given->get_ptr<const json::object_t*>();
        if (members == nullptr) return invalid("params.arguments must be an object");
        for (const auto& [argument, value] : *members) {
            const auto* text = value.get_ptr<const std::string*>();
            if (text == nullptr) return invalid("params.arguments." + argument + " must be a string");
            get.arguments.emplace(argument, *text);
        }
    }
    std::span<const PromptDefinition> prompts;
    auto prompt = prompts.end();
    try {
        prompts = host->prompts();
        prompt = std::find_if(prompts.begin(), prompts.end(),
                              [&](const PromptDefinition& definition) { return definition.name == get.name; });
        if (prompt != prompts.end() && !host->prompt_enabled(static_cast<std::size_t>(prompt - prompts.begin()))) {
            prompt = prompts.end();
        }
    } catch (...) {
        return served_answer(
            make_error(id, error_code::internal_error, "Internal error: the prompts could not be listed"), served);
    }
    if (prompt == prompts.end()) return invalid("unknown prompt: " + get.name);
    for (const auto& argument : prompt->arguments) {
        if (argument.required && get.arguments.find(argument.name) == get.arguments.end()) {
            return invalid("missing required argument: " + argument.name);
        }
    }
    auto token = progress_token_of(request, served);
    if (auto* refusal = std::get_if<Answer>(&token)) return std::move(*refusal);

    if (const json* meta = member(request.params, "_meta")) get.meta = *meta;
    CallSubject subject;
    subject.method = request.method;
    subject.name = get.name;
    for (const auto& [argument, value] : get.arguments) subject.arguments[argument] = value;
    subject.request = std::move(get);
    if (auto refusal = take_retry(request, served, subject)) return refusal;

    auto call = begin_call(CallKind::prompt_get, id, served, origin, place, std::move(std::get<json>(token)),
                           std::move(subject));
    if (!call) return std::nullopt;
    try {
        host->get(std::get<PromptRequest>(call->subject.request), CallControl(call),
                  Completion(std::make_shared<CompletionState>(call)));
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
// The change feed

std::optional<Answer> SessionCore::on_feed(Message& request, const Served& served, Origin& origin,
                                           const Place& place) {
    auto read = served.feed->read(request);
    if (const auto* error = std::get_if<RequestError>(&read)) {
        return served_answer(make_error(*request.id, error->code, error->message, error->data), served);
    }
    const FeedRequest& asked = std::get<FeedRequest>(read);
    switch (asked.action) {
        case FeedRequest::Action::watch:
        case FeedRequest::Action::unwatch: return on_watch(request, asked, served, origin);
        case FeedRequest::Action::listen: return on_listen(request, asked, served, origin, place);
    }
    return method_not_found(request, served);
}

std::optional<Answer> SessionCore::on_watch(const Message& request, const FeedRequest& asked, const Served& served,
                                            const Origin& origin) {
    const RequestId& id = *request.id;
    // Watching a resource is offered where the session declared its updates, on a connection the feed delivers to.
    ResourceHost* resources = host_.resource_host();
    if (served.feed->delivery() != FeedDelivery::connection || !origin.connection || resources == nullptr ||
        !reported().resource_updates) {
        return method_not_found(request, served);
    }
    if (asked.action == FeedRequest::Action::watch) {
        // A URI that names nothing the host offers is refused as a read of it would be: the pages require a server
        // to validate every resource URI and name the error for a resource that does not exist.
        bool offered = false;
        try {
            offered = resources->offers(asked.uri);
        } catch (...) {
            return served_answer(
                make_error(id, error_code::internal_error, "Internal error: the resource could not be found"), served);
        }
        if (!offered) {
            const int code = served.profile->error_code(resource_not_found_error).value_or(error_code::invalid_params);
            json data = json::object();
            data["uri"] = asked.uri;
            return served_answer(make_error(id, code, "Resource not found: " + asked.uri, std::move(data)), served);
        }
    }
    std::size_t full = 0;  // the bound, when the URI would exceed it
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            auto& watched = connection_->watched;
            if (asked.action == FeedRequest::Action::unwatch) {
                // Removing a URI the connection does not watch changes nothing, and is not an error.
                const auto found = watched.find(asked.uri);
                if (found != watched.end()) watched.erase(found);
            } else if (watched.find(asked.uri) == watched.end()) {
                if (watched.size() >= options_.max_watched_resources) {
                    full = options_.max_watched_resources;
                } else {
                    watched.insert(asked.uri);
                }
            }
        }
    }
    if (full != 0) {
        return limit_refusal(id,
                             "Internal error: the connection watches " + std::to_string(full) +
                                 " resources already, the most it may",
                             served);
    }
    json body = json::object();
    finish_result(body, "EmptyResult", *served.profile);
    return served_answer(make_result(id, std::move(body)), served);
}

std::optional<Answer> SessionCore::on_listen(const Message& request, const FeedRequest& asked, const Served& served,
                                             Origin& origin, const Place& place) {
    const RequestId& id = *request.id;
    const ChangeSupport reported = this->reported();
    // A server that reports no kind of change offers no subscription.
    if (served.feed->delivery() != FeedDelivery::subscription ||
        !(reported.tools_list || reported.prompts_list || reported.resources_list || reported.resource_updates)) {
        return method_not_found(request, served);
    }
    // Its answer comes only when it ends, which a batch's answer cannot wait for.
    if (place.batch) {
        return served_answer(make_error(id, error_code::invalid_request,
                                        "Invalid Request: " + request.method + " must not be part of a batch"),
                             served, RefusalReason::invalid_request);
    }
    if (asked.filter.resources.size() > options_.max_watched_resources) {
        return served_answer(make_error(id, error_code::invalid_params,
                                        "Invalid params: a subscription names at most " +
                                            std::to_string(options_.max_watched_resources) + " resources"),
                             served);
    }
    // What is honoured: the kinds of change the server reports and the revision has a notification for, and of the
    // resources asked for those the host offers now. The rest is left out of the acknowledgement and never sent.
    const auto has = [&](ChangeKind kind) {
        const profile::Message* entry =
            served.profile->notification(served.feed->notification(Change{kind, {}}).method);
        return entry != nullptr && entry->from_server;
    };
    ChangeFilter honoured;
    honoured.tools_list = asked.filter.tools_list && reported.tools_list && has(ChangeKind::tools_list);
    honoured.prompts_list = asked.filter.prompts_list && reported.prompts_list && has(ChangeKind::prompts_list);
    honoured.resources_list =
        asked.filter.resources_list && reported.resources_list && has(ChangeKind::resources_list);
    if (reported.resource_updates && has(ChangeKind::resource_updated)) {
        ResourceHost* resources = host_.resource_host();
        try {
            for (const auto& uri : asked.filter.resources) {
                if (resources->offers(uri)) honoured.resources.push_back(uri);
            }
        } catch (...) {
            return served_answer(make_error(id, error_code::internal_error,
                                            "Internal error: the resources could not be found"),
                                 served);
        }
    }
    if (!watch_host()) {
        return served_answer(
            make_error(id, error_code::internal_error, "Internal error: the server reports no changes now"), served);
    }
    FeedMessage acknowledgement = served.feed->acknowledgement(honoured);
    served.feed->tag(acknowledgement.params, id);
    auto line = feed_line(served, std::move(acknowledgement));
    if (!line) {
        return served_answer(
            make_error(id, error_code::internal_error, "Internal error: the subscription cannot be acknowledged"),
            served);
    }

    auto call = std::make_shared<CallState>(id);
    call->core = weak_from_this();
    call->kind = CallKind::subscription;
    call->profile = served.profile;
    call->served = &served;
    call->client = std::move(origin.client);
    call->unit = place.unit;
    call->filter = std::move(honoured);
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) return std::nullopt;
    // A unit abandoned while it was read has nobody to read the subscription.
    if (place.unit && place.unit->abandoned) return std::nullopt;
    // The host's feed may have ended since the registration above: its end ended the subscriptions then in flight,
    // and nothing would end this one.
    if (watch_ != Watch::registered) {
        lock.unlock();
        return served_answer(
            make_error(id, error_code::internal_error, "Internal error: the server reports no changes now"), served);
    }
    if (listening_ >= options_.max_subscriptions) {
        lock.unlock();
        return limit_refusal(id,
                             "Internal error: the session has " + std::to_string(options_.max_subscriptions) +
                                 " subscriptions open, the most it may",
                             served);
    }
    // In flight and acknowledged under one hold of the lock: no change reaches the subscription before its
    // acknowledgement is queued, and every one reported after that is queued after it.
    in_flight_.emplace(id, call);
    ++listening_;
    if (place.unit) place.unit->profile = served.profile;
    Output output = make_output(std::move(*line), OutputKind::notification, false, RefusalReason::none, served.profile);
    output.long_lived = true;
    enqueue(Queued{std::move(output), call, place.unit, true});
    drain(lock, Writer::reader);
    return std::nullopt;
}

void SessionCore::ending_outputs(const std::shared_ptr<CallState>& call, std::vector<Queued>& out) const {
    const Served& served = *call->served;
    if (auto notice = served.feed->ending_notice(call->id)) {
        served.feed->tag(notice->params, call->id);
        if (auto line = feed_line(served, std::move(*notice))) {
            Output output =
                make_output(std::move(*line), OutputKind::notification, false, RefusalReason::none, call->profile);
            output.long_lived = true;
            out.push_back(Queued{std::move(output), nullptr, call->unit, true});
        }
    }
    json body = served.feed->ending_result();
    served.feed->tag(body, call->id);
    finish_result(body, served.feed->ending_result_type(), *call->profile);
    Output output = make_output(serialize(make_result(call->id, std::move(body))), OutputKind::response,
                                call->unit != nullptr, RefusalReason::none, call->profile);
    output.long_lived = true;
    out.push_back(Queued{std::move(output), nullptr, call->unit, true});
}

Answer SessionCore::limit_refusal(const RequestId& id, std::string message, const Served& served) const {
    json data = json::object();
    data["reason"] = std::string(limit_reason);
    return served_answer(make_error(id, error_code::internal_error, std::move(message), std::move(data)), served);
}

bool SessionCore::watch_host() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || watch_ == Watch::ended) return false;
        if (watch_ == Watch::registered) return true;
        watch_ = Watch::registering;
    }
    ChangeRegistration registration;
    try {
        registration = host_.watch(listener_);
    } catch (...) {
        // A host that cannot register the session reports nothing to it.
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // The session closed, or the host ended its feed, while it registered.
        const bool keep = registration && !closed_ && watch_ == Watch::registering;
        if (watch_ == Watch::registering) watch_ = keep ? Watch::registered : Watch::ended;
        if (keep) {
            registration_ = std::move(registration);
            return true;
        }
    }
    registration.reset();
    return false;
}

std::optional<std::string> SessionCore::feed_line(const Served& served, FeedMessage message) const {
    const profile::Profile& profile = *served.profile;
    const profile::Message* entry = profile.notification(message.method);
    if (entry == nullptr || !entry->from_server) return std::nullopt;
    if (entry->params) keep_members(message.params, *entry->params, profile);
    json params = message.params.empty() ? json(nullptr) : std::move(message.params);
    return serialize(make_notification(entry->method, std::move(params)));
}

void SessionCore::on_change(const Change& change) {
    // Only the kinds the session declared reach its client.
    const ChangeSupport reported = this->reported();
    const bool declared = [&] {
        switch (change.kind) {
            case ChangeKind::tools_list: return reported.tools_list;
            case ChangeKind::prompts_list: return reported.prompts_list;
            case ChangeKind::resources_list: return reported.resources_list;
            case ChangeKind::resource_updated: return reported.resource_updates;
        }
        return false;
    }();
    if (!declared) return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || watch_ != Watch::registered) return;
    bool queued = false;
    // The connection, once its client is ready; an update only when the connection watches the resource. Like a
    // progress notification, a change notification is dropped while the queue is full.
    if (connection_ && connection_->ready && connection_->served->feed->delivery() == FeedDelivery::connection &&
        (change.kind != ChangeKind::resource_updated ||
         connection_->watched.find(change.uri) != connection_->watched.end()) &&
        queue_.size() < options_.max_queued_notifications) {
        const Served& served = *connection_->served;
        if (auto line = feed_line(served, served.feed->notification(change))) {
            queued = enqueue(Queued{make_output(std::move(*line), OutputKind::notification, false, RefusalReason::none,
                                                served.profile),
                                    nullptr, nullptr});
        }
    }
    // Every open subscription that asked for it, tagged with its id.
    for (const auto& entry : in_flight_) {
        const std::shared_ptr<CallState>& call = entry.second;
        if (call->kind != CallKind::subscription || !call->active || !call->filter.wants(change)) continue;
        if (queue_.size() >= options_.max_queued_notifications) break;
        const Served& served = *call->served;
        FeedMessage message = served.feed->notification(change);
        served.feed->tag(message.params, call->id);
        if (auto line = feed_line(served, std::move(message))) {
            Output output =
                make_output(std::move(*line), OutputKind::notification, false, RefusalReason::none, call->profile);
            output.long_lived = true;
            queued = enqueue(Queued{std::move(output), call, call->unit}) || queued;
        }
    }
    if (queued) drain(lock, Writer::host);
}

void SessionCore::on_feed_end() {
    ChangeRegistration registration;
    bool now_idle = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        watch_ = Watch::ended;
        registration = std::move(registration_);
        // The host stops: it tears down every open subscription, which ends gracefully.
        std::vector<Queued> ending;
        for (auto it = in_flight_.begin(); it != in_flight_.end();) {
            const std::shared_ptr<CallState> call = it->second;
            if (call->kind != CallKind::subscription) {
                ++it;
                continue;
            }
            it = in_flight_.erase(it);
            --listening_;
            call->active = false;
            if (!closed_) ending_outputs(call, ending);
        }
        bool queued = false;
        for (auto& output : ending) queued = enqueue(std::move(output)) || queued;
        if (queued) {
            drain(lock, Writer::host);
        } else {
            now_idle = !closed_ && idle_locked();
        }
    }
    // The host has let the listener go already; releasing the registration again changes nothing there.
    registration.reset();
    if (now_idle) signal_idle();
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
    const ChangeSupport reported = this->reported();
    json capabilities = json::object();
    json tools = json::object();
    tools["listChanged"] = reported.tools_list;
    capabilities["tools"] = std::move(tools);
    if (options_.protocol_logging) capabilities["logging"] = json::object();
    if (host_.resource_host() != nullptr) {
        json resources = json::object();
        if (reported.resources_list) resources["listChanged"] = true;
        if (reported.resource_updates) resources["subscribe"] = true;
        capabilities["resources"] = std::move(resources);
    }
    if (host_.prompt_host() != nullptr) {
        json prompts = json::object();
        if (reported.prompts_list) prompts["listChanged"] = true;
        capabilities["prompts"] = std::move(prompts);
    }
    if (host_.completion_host() != nullptr) capabilities["completions"] = json::object();
    // Every revision's schema gives the capabilities of tools, prompts and resources these members, so only the
    // capabilities themselves are filtered.
    keep_members(capabilities, "ServerCapabilities", profile);
    return capabilities;
}

ChangeSupport SessionCore::reported() const {
    const ChangeSupport support = host_.change_support();
    ChangeSupport reported;
    reported.tools_list = support.tools_list;
    reported.prompts_list = support.prompts_list && host_.prompt_host() != nullptr;
    reported.resources_list = support.resources_list && host_.resource_host() != nullptr;
    reported.resource_updates = support.resource_updates && host_.resource_host() != nullptr;
    return reported;
}

// Adds the members a revision's result type requires that the engine provides, and the server's identity in
// `_meta`, each only when the type (or the result meta type) of the revision has it.
void SessionCore::finish_result(json& body, std::string_view type, const profile::Profile& profile) const {
    finish_result(body, type, profile, options_.tools_cache);
}

void SessionCore::finish_result(json& body, std::string_view type, const profile::Profile& profile,
                                const CacheHints& cache) const {
    if (const profile::Type* result = profile.type(type)) {
        // A result that asks for input says so itself.
        if (result->has_property("resultType") && !body.contains("resultType")) body["resultType"] = "complete";
        if (result->has_property("ttlMs")) body["ttlMs"] = cache.ttl_ms;
        if (result->has_property("cacheScope")) body["cacheScope"] = std::string(to_string(cache.scope));
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
    if (auto* asked = std::get_if<InputRequired>(&outcome)) {
        ask(call, std::move(*asked));
        return;
    }
    if (const auto* result = std::get_if<ToolResult>(&outcome)) {
        if (call->kind != CallKind::tool) {
            // A tool result cannot answer another kind of call.
            finish_call(call, make_error(call->id, error_code::internal_error,
                                         "Internal error: the host answered with a result of another kind"));
            return;
        }
        response = make_result(call->id, call_result(*result, *call->profile));
    } else if (std::holds_alternative<Refusal>(outcome)) {
        json data = json::object();
        data["reason"] = "busy";
        response = make_error(call->id, error_code::internal_error, "Internal error: the server is busy", std::move(data));
    } else {
        std::string message = call->kind == CallKind::tool ? "Internal error: the tool call failed"
                                                           : "Internal error: the call failed";
        const auto& failure = std::get<HostFailure>(outcome);
        if (options_.expose_error_details && !failure.message.empty()) message += ": " + failure.message;
        response = make_error(call->id, error_code::internal_error, std::move(message));
    }
    finish_call(call, std::move(response));
}

void SessionCore::complete(const std::shared_ptr<CallState>& call, FeatureOutcome outcome) {
    const profile::Profile& profile = *call->profile;
    json response;
    const auto wrong_kind = [&] {
        return make_error(call->id, error_code::internal_error,
                          "Internal error: the host answered with a result of another kind");
    };
    if (const auto* result = std::get_if<ReadResult>(&outcome)) {
        if (call->kind != CallKind::resource_read) {
            response = wrong_kind();
        } else {
            json body = to_wire(*result, profile);
            // The result of a retried read depends on inputs that are not part of a cache key: stale at once.
            const CacheHints hints = call->subject.retry ? CacheHints{} : result->cache.value_or(CacheHints{});
            finish_result(body, "ReadResourceResult", profile, hints);
            response = make_result(call->id, std::move(body));
        }
    } else if (const auto* prompt = std::get_if<PromptResult>(&outcome)) {
        if (call->kind != CallKind::prompt_get) {
            response = wrong_kind();
        } else {
            json body = to_wire(*prompt, profile);
            finish_result(body, "GetPromptResult", profile);
            response = make_result(call->id, std::move(body));
        }
    } else if (const auto* completion = std::get_if<CompletionResult>(&outcome)) {
        if (call->kind != CallKind::completion) {
            response = wrong_kind();
        } else {
            json body = to_wire(*completion, profile);
            finish_result(body, "CompleteResult", profile);
            response = make_result(call->id, std::move(body));
        }
    } else if (const auto* missing = std::get_if<NotFound>(&outcome)) {
        if (call->kind != CallKind::resource_read) {
            response = wrong_kind();
        } else {
            // The code is the one this revision defines for it, else Invalid params: never a code of another
            // revision, which this one may forbid.
            const int code = profile.error_code(resource_not_found_error).value_or(error_code::invalid_params);
            json data = json::object();
            data["uri"] = missing->uri;
            response = make_error(call->id, code, "Resource not found: " + missing->uri, std::move(data));
        }
    } else {
        response = make_error(call->id, error_code::invalid_params,
                              "Invalid params: " + std::get<InvalidRequest>(outcome).message);
    }
    finish_call(call, std::move(response));
}

void SessionCore::complete_dropped(const std::shared_ptr<CallState>& call) {
    finish_call(call, make_error(call->id, error_code::internal_error,
                                 call->kind == CallKind::tool ? "Internal error: the tool call ended without a result"
                                                              : "Internal error: the call ended without a result"));
}

void SessionCore::finish_call(const std::shared_ptr<CallState>& call, json response, RefusalReason refusal) {
    std::string line;
    if (!call->batch) line = serialize(response);
    bool batch_finished = false;
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_ || !call->active) return;
    call->active = false;
    std::shared_ptr<WaitState> wait = call->wait;
    if (wait) close_wait(wait);
    if (const auto found = in_flight_.find(call->id); found != in_flight_.end() && found->second == call) {
        in_flight_.erase(found);
    }
    if (call->batch) {
        call->batch->responses[call->batch_index] = std::move(response);
        batch_finished = --call->batch->outstanding == 0 && !call->batch->reading;
    } else {
        // The response is the last output of the call's unit.
        push(lock,
             Queued{make_output(std::move(line), OutputKind::response, call->unit != nullptr, refusal, call->profile),
                    call, call->unit},
             Writer::host);
    }
    lock.unlock();
    if (wait) wait->ended.request_stop();
    if (batch_finished) emit_batch(call->batch, Writer::host);
}

// ---------------------------------------------------------------------------------------------------------------
// Input requests

std::chrono::system_clock::time_point SessionCore::now() const {
    return options_.clock ? options_.clock() : std::chrono::system_clock::now();
}

Answer SessionCore::state_refusal(const RequestId& id, RequestStateProblem problem, const std::string& method) const {
    if (options_.on_request_state_rejected) {
        try {
            options_.on_request_state_rejected(RequestStateRejection{problem, method});
        } catch (...) {
            // A diagnostic that fails changes nothing about the answer.
        }
    }
    json data = json::object();
    data["reason"] = std::string(invalid_state_reason);
    return Answer{make_error(id, error_code::invalid_params, std::string(invalid_state_message), std::move(data)),
                  RefusalReason::none, nullptr};
}

std::optional<Answer> SessionCore::take_retry(const Message& request, const Served& served, CallSubject& subject) const {
    const RequestId& id = *request.id;
    auto read = served.input->retry(request, *served.profile);
    if (const auto* error = std::get_if<RequestError>(&read)) {
        return served_answer(make_error(id, error->code, error->message, error->data), served);
    }
    InputRetry& retry = std::get<InputRetry>(read);
    subject.retry = retry.responses.has_value() || retry.state.has_value();
    if (!retry.state) {
        // No state says what was asked. Strict: such answers are no answers, and the host asks for what it still needs.
        if (options_.answers_need_state || !retry.responses) return std::nullopt;
        // Otherwise each answer reaches the host as the answer of the one kind of request its members make it; keys
        // the host did not ask by it ignores.
        InputResponses answers;
        if (const auto* given = retry.responses->get_ptr<const json::object_t*>()) {
            for (const auto& [key, value] : *given) {
                const auto method = answer_method(value, *served.profile, *served.input);
                auto answer = method ? read_answer(*method, value)
                                     : std::variant<InputResponse, std::string>(
                                           std::string("it is the answer of no request, or of more than one"));
                if (const auto* why = std::get_if<std::string>(&answer)) {
                    return served_answer(make_error(id, error_code::invalid_params,
                                                    "Invalid params: the answer " + key + " in params.inputResponses is "
                                                    "malformed: " + *why),
                                         served);
                }
                answers.emplace(key, std::get<InputResponse>(std::move(answer)));
            }
        }
        std::visit(
            [&](auto& host_request) {
                using T = std::decay_t<decltype(host_request)>;
                if constexpr (!std::is_same_v<T, std::monostate>) host_request.input_responses = std::move(answers);
            },
            subject.request);
        return std::nullopt;
    }
    const auto refuse = [&](RequestStateProblem problem) {
        Answer answer = state_refusal(id, problem, request.method);
        answer.profile = served.profile;
        return answer;
    };
    const auto* text = retry.state->get_ptr<const std::string*>();
    if (text == nullptr || !sealer_) return refuse(RequestStateProblem::malformed);
    auto opened = sealer_->open(*text);
    if (const auto* problem = std::get_if<RequestStateProblem>(&opened)) return refuse(*problem);
    StateClaims& claims = std::get<StateClaims>(opened);
    const std::string digest = arguments_digest(subject.arguments);
    const StateBinding binding{subject.method, subject.name, digest, options_.server_info.name, std::string_view()};
    if (const auto problem = check_claims(claims, binding, now())) return refuse(*problem);

    // Each answer asked for must have the shape of its kind; answers to keys that were not asked are ignored.
    InputResponses answers;
    if (retry.responses) {
        for (const auto& [key, method] : claims.asked) {
            const json* given = member(*retry.responses, key);
            if (given == nullptr) continue;
            auto answer = read_answer(method, *given);
            if (const auto* why = std::get_if<std::string>(&answer)) {
                return served_answer(make_error(id, error_code::invalid_params,
                                                "Invalid params: the answer " + key + " in params.inputResponses is "
                                                "malformed: " + *why),
                                     served);
            }
            answers.emplace(key, std::get<InputResponse>(std::move(answer)));
        }
    }
    subject.round = claims.round;
    std::visit(
        [&](auto& host_request) {
            using T = std::decay_t<decltype(host_request)>;
            if constexpr (!std::is_same_v<T, std::monostate>) {
                host_request.input_responses = std::move(answers);
                host_request.request_state = std::move(claims.state);
            }
        },
        subject.request);
    return std::nullopt;
}

bool SessionCore::client_supports(const CallState& call, const InputRequest& request) const {
    const Served& served = *call.served;
    return served.input != nullptr &&
           input_supported(request, call.client.capabilities, *call.profile, *served.input);
}

void SessionCore::fail_call(const std::shared_ptr<CallState>& call, const std::string& text, json data) {
    if (call->kind == CallKind::tool) {
        ToolResult result;
        TextContent block;
        block.text = text;
        result.content.emplace_back(std::move(block));
        result.is_error = true;
        finish_call(call, make_result(call->id, call_result(result, *call->profile)));
        return;
    }
    finish_call(call, make_error(call->id, error_code::internal_error, "Internal error: " + text, std::move(data)));
}

void SessionCore::close_wait(const std::shared_ptr<WaitState>& wait) {
    wait->open = false;
    for (const auto& id : wait->ids) outgoing_.erase(id);
    wait->ids.clear();
    if (auto call = wait->call.lock(); call && call->wait == wait) call->wait.reset();
}

void SessionCore::ask(const std::shared_ptr<CallState>& call, InputRequired asked) {
    const Served& served = *call->served;
    const InputMechanism& mechanism = *served.input;
    const profile::Profile& profile = *call->profile;
    if (call->kind == CallKind::completion || !mechanism.asks_on(profile, call->subject.method)) {
        finish_call(call, make_error(call->id, error_code::internal_error,
                                     "Internal error: the host asked for input on a request that cannot ask"));
        return;
    }
    // What the client lacks, checked before anything is put in a result or sent.
    json missing = json::object();
    for (const auto& [key, request] : asked.requests) {
        if (mechanism.carries(profile, method_of(request)) &&
            capabilities_declare(request, call->client.capabilities)) {
            continue;
        }
        merge_objects(missing, required_capabilities(request));
    }
    if (!missing.empty()) {
        json data = json::object();
        data[std::string(required_capabilities_member)] = std::move(missing);
        if (const auto code = profile.error_code(missing_capability_error)) {
            finish_call(call, make_error(call->id, *code, "Missing required client capability", std::move(data)),
                        RefusalReason::missing_client_capability);
        } else {
            // The text first: the arguments of a call are evaluated in no fixed order, and the data moves.
            const std::string text = "the client did not declare a capability this call needs: " +
                                     serialize(data.at(std::string(required_capabilities_member)));
            fail_call(call, text, std::move(data));
        }
        return;
    }
    // The requests as the revision writes them.
    std::vector<std::pair<std::string, std::pair<std::string, json>>> written;  // key, method, params
    written.reserve(asked.requests.size());
    for (const auto& [key, request] : asked.requests) {
        auto params = write_params(request, profile, mechanism);
        if (const auto* why = std::get_if<std::string>(&params)) {
            finish_call(call, make_error(call->id, error_code::internal_error,
                                         options_.expose_error_details
                                             ? "Internal error: the input request " + key + " cannot be sent: " + *why
                                             : std::string("Internal error: the call failed")));
            return;
        }
        written.emplace_back(key, std::make_pair(std::string(method_of(request)), std::get<json>(std::move(params))));
    }

    if (mechanism.delivery() == InputDelivery::result) {
        json requests = json::object();
        StateClaims claims;
        for (auto& [key, entry] : written) {
            json request = json::object();
            request["method"] = entry.first;
            if (!entry.second.is_null()) request["params"] = std::move(entry.second);
            requests[key] = std::move(request);
            claims.asked.emplace(key, entry.first);
        }
        const std::int64_t issued =
            std::chrono::duration_cast<std::chrono::seconds>(now().time_since_epoch()).count();
        claims.expires = issued + static_cast<std::int64_t>(options_.request_state_ttl.count());
        claims.method = call->subject.method;
        claims.subject = call->subject.name;
        claims.arguments = arguments_digest(call->subject.arguments);
        claims.server = options_.server_info.name;
        claims.round = call->subject.round + 1;
        claims.state = std::move(asked.state);
        json body = mechanism.asking_result(std::move(requests), sealer_->seal(claims));
        finish_result(body, mechanism.asking_result_type(), profile);
        finish_call(call, make_result(call->id, std::move(body)));
        return;
    }

    // The requests go to the client, all of the round together, and the call waits for the answers.
    auto wait = std::make_shared<WaitState>();
    wait->core = weak_from_this();
    wait->call = call;
    wait->state = std::move(asked.state);
    bool again = false;     // no requests: the host is called again at once
    bool exhausted = false;  // the call asked once too often
    bool unreachable = false;  // the unit is gone: nothing can reach the client
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || !call->active) return;
        if (call->rounds >= options_.max_input_rounds) {
            exhausted = true;
        } else if (call->unit && (call->unit->abandoned || call->unit->finished)) {
            unreachable = true;
        } else {
            ++call->rounds;
            if (written.empty()) {
                again = true;
            } else {
                for (auto& [key, entry] : written) {
                    const RequestId request_id(next_outgoing_++);
                    outgoing_.emplace(request_id, Outgoing{wait, key});
                    wait->ids.push_back(request_id);
                    wait->asked.emplace(key, entry.first);
                    ++wait->outstanding;
                    std::string line =
                        serialize(mechanism.asking_request(request_id, entry.first, std::move(entry.second)));
                    enqueue(Queued{make_output(std::move(line), OutputKind::request, false, RefusalReason::none,
                                               call->profile),
                                   call, call->unit});
                }
                call->wait = wait;
                drain(lock, Writer::host);
            }
        }
    }
    if (exhausted) {
        fail_call(call, "the call asked for input more than " + std::to_string(options_.max_input_rounds) + " times");
        return;
    }
    if (unreachable) {
        fail_call(call, "the call asked for input after its stream was gone");
        return;
    }
    if (again) {
        continue_call(call, {}, std::move(wait->state));
        return;
    }
    host_.awaiting_input(InputWait(wait));
}

void SessionCore::continue_call(const std::shared_ptr<CallState>& call, InputResponses answers,
                                std::optional<std::string> state) {
    HostRequest request = call->subject.request;
    std::visit(
        [&](auto& host_request) {
            using T = std::decay_t<decltype(host_request)>;
            if constexpr (!std::is_same_v<T, std::monostate>) {
                host_request.input_responses = std::move(answers);
                host_request.request_state = std::move(state);
            }
        },
        request);
    start(call, request);
}

void SessionCore::on_response(const Message& response) {
    if (!response.id) return;
    std::shared_ptr<WaitState> wait;
    std::string key;
    std::string method;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = outgoing_.find(*response.id);
        if (found == outgoing_.end()) return;  // not a request this session waits for: ignored
        wait = found->second.wait;
        key = found->second.key;
        if (!wait->open) return;
        method = wait->asked.at(key);
    }
    // Read without the lock; whatever happened meanwhile is decided below.
    std::optional<std::string> failure;
    std::optional<InputResponse> answer;
    if (response.failed) {
        failure = "the client answered the input request " + key + " with an error";
    } else {
        auto read = read_answer(method, response.outcome);
        if (const auto* why = std::get_if<std::string>(&read)) {
            failure = "the client's answer to the input request " + key + " is malformed: " + *why;
        } else {
            answer = std::get<InputResponse>(std::move(read));
        }
    }
    std::shared_ptr<CallState> call;
    bool done = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = outgoing_.find(*response.id);
        if (found == outgoing_.end() || found->second.wait != wait || !wait->open) return;
        outgoing_.erase(found);
        call = wait->call.lock();
        if (!call || !call->active) {
            close_wait(wait);
            done = true;
            call.reset();
        } else if (failure) {
            close_wait(wait);
            done = true;
        } else {
            wait->answers.insert_or_assign(key, std::move(*answer));
            if (--wait->outstanding == 0) {
                close_wait(wait);
                done = true;
            }
        }
    }
    if (!done) return;
    wait->ended.request_stop();
    if (!call) return;
    if (failure) {
        fail_call(call, *failure);
        return;
    }
    continue_call(call, std::move(wait->answers), std::move(wait->state));
}

void SessionCore::expire(const std::shared_ptr<WaitState>& wait) {
    std::shared_ptr<CallState> call;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!wait->open) return;
        call = wait->call.lock();
        close_wait(wait);
        if (call && !call->active) call.reset();
    }
    wait->ended.request_stop();
    if (call) fail_call(call, "the client did not answer the call's input requests in time");
}

void SessionCore::cancel(const RequestId& id, const CancelMechanism& mechanism, const std::string& method) {
    std::shared_ptr<CallState> call;
    std::shared_ptr<WaitState> wait;  // the call's wait for answers, which ends with it
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
        if (call->kind == CallKind::subscription) --listening_;
        call->active = false;
        call->cancelled = true;
        wait = call->wait;
        if (wait) close_wait(wait);
        if (call->batch) {
            batch_finished = --call->batch->outstanding == 0 && !call->batch->reading;
        } else if (call->unit) {
            // No answer will come, so the unit ends here. The marker has no owner: the drainer must not skip it.
            ended = enqueue(Queued{make_output({}, OutputKind::end, true, RefusalReason::none, call->profile), nullptr,
                                   call->unit});
        }
        now_idle = !ended && !closed_ && idle_locked();
    }
    if (wait) wait->ended.request_stop();
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
    if (closed_ || broken_) return false;  // a sink that threw is not called again, not even before close() ran
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

profile::ProfilePtr SessionCore::client_profile(const Unit& unit, const Claims* claims) const {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) return connection_->served->profile;
        if (unit && unit->profile) return unit->profile;
    }
    if (claims != nullptr && claims->named != nullptr) return claims->named->profile;
    return nullptr;
}

void SessionCore::conform_id(json& message, const profile::ProfilePtr& client) const {
    auto* members = message.get_ptr<json::object_t*>();
    if (members == nullptr || members->find("error") == members->end()) return;
    const auto id = members->find("id");
    if (id == members->end() || !id->second.is_null()) return;
    const bool omit =
        client ? client->error_response_may_omit_id()
               : !served_.empty() && std::all_of(served_.begin(), served_.end(), [](const Served& served) {
                     return served.profile->error_response_may_omit_id();
                 });
    if (omit) members->erase(id);
}

void SessionCore::emit_answer(Answer answer, const Unit& unit, const Claims& claims, Writer writer) {
    conform_id(answer.message, client_profile(unit, &claims));
    emit(Queued{make_output(serialize(answer.message), OutputKind::response, unit != nullptr, answer.refusal,
                            std::move(answer.profile)),
                nullptr, unit},
         writer);
}

void SessionCore::emit_refusal(const Unit& unit, const Claims& claims, int code, std::string message,
                               RefusalReason reason) {
    emit_answer(Answer{make_error(std::nullopt, code, std::move(message)), reason, nullptr}, unit, claims,
                Writer::reader);
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
    // An open subscription waits for nothing the client sends; it does not keep the session busy.
    return closed_ ||
           (in_flight_.size() == listening_ && queue_.empty() && !draining_ && pending_batches_ == 0);
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
    std::vector<std::shared_ptr<WaitState>> waits;  // waits for answers that end with the unit
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
        // A call that waits for answers ends too: its requests went out on the stream that is gone, and nothing more
        // is sent for it.
        // A subscription of the unit ends whatever the mechanism says: the stream it is written on is gone.
        for (auto it = in_flight_.begin(); it != in_flight_.end();) {
            const std::shared_ptr<CallState> call = it->second;
            const bool waiting = call->wait != nullptr;
            const bool subscription = call->kind == CallKind::subscription;
            if (call->unit != unit ||
                (!waiting && !subscription && !call->served->cancel->stops_on_stream_close())) {
                ++it;
                continue;
            }
            it = in_flight_.erase(it);
            if (subscription) --listening_;
            call->active = false;
            call->cancelled = true;
            if (waiting) {
                waits.push_back(call->wait);
                close_wait(call->wait);
            }
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
    for (const auto& wait : waits) wait->ended.request_stop();
    for (const auto& call : stopped) call->stop.request_stop();
    for (const auto& batch : assembled) emit_batch(batch, Writer::host);
    if (now_idle) signal_idle();
}

void SessionCore::close() {
    std::vector<std::shared_ptr<CallState>> calls;
    std::vector<std::shared_ptr<WaitState>> waits;
    ChangeRegistration registration;  // released without the lock: the host takes its own
    std::unique_lock<std::mutex> lock(mutex_);
    const bool inside_sink = drainer_ == std::this_thread::get_id();
    if (!closed_) {
        closed_ = true;
        registration = std::move(registration_);
        const bool writable = !inside_sink && !broken_;
        // What is queued is dropped, but for the graceful ends of subscriptions queued before (a host that stopped).
        std::deque<Queued> kept;
        if (writable) {
            for (auto& queued : queue_) {
                if (queued.essential) kept.push_back(std::move(queued));
            }
        }
        queue_ = std::move(kept);
        // The session tears down its subscriptions: each ends gracefully.
        std::vector<Queued> ending;
        for (auto& entry : in_flight_) {
            entry.second->active = false;
            calls.push_back(entry.second);
            if (entry.second->wait) {
                waits.push_back(entry.second->wait);
                close_wait(entry.second->wait);
            }
            if (writable && entry.second->kind == CallKind::subscription) ending_outputs(entry.second, ending);
        }
        in_flight_.clear();
        listening_ = 0;
        outgoing_.clear();
        for (auto& output : ending) {
            if (output.unit) {
                if (output.unit->finished || output.unit->abandoned) continue;
                output.output.unit = output.unit->id;
                if (output.output.last) output.unit->finished = true;
            }
            queue_.push_back(std::move(output));
        }
        // The units still open whose last output is not queued get an end marker; none when the sink cannot be
        // called again, from inside it or after it failed.
        if (writable) {
            std::set<const UnitState*> ended;
            for (const auto& queued : queue_) {
                if (queued.unit && queued.output.last) ended.insert(queued.unit.get());
            }
            for (const auto& entry : units_) {
                const Unit& unit = entry.second;
                if (ended.find(unit.get()) != ended.end()) continue;
                unit->finished = true;
                Queued marker{make_output({}, OutputKind::end, true, RefusalReason::none, unit->profile), nullptr, unit};
                marker.output.unit = unit->id;
                queue_.push_back(std::move(marker));
            }
        }
        units_.clear();
    }
    lock.unlock();
    registration.reset();
    for (const auto& wait : waits) wait->ended.request_stop();
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

bool CallControl::client_supports(const InputRequest& request) const {
    // The mechanism the call is served under belongs to the core; without the core the call can ask nothing.
    if (auto core = state_->core.lock()) return core->client_supports(*state_, request);
    return false;
}

std::stop_token InputWait::ended() const noexcept { return state_->ended.get_token(); }

void InputWait::expire() const {
    if (auto core = state_->core.lock()) core->expire(state_);
}
const json& CallControl::client_info() const noexcept { return state_->client.info; }
const json& CallControl::client_capabilities() const noexcept { return state_->client.capabilities; }

void Completion::operator()(CallOutcome outcome) const {
    if (!state_ || state_->called.exchange(true)) return;
    auto core = state_->call->core.lock();
    if (!core) return;
    try {
        core->complete(state_->call, std::move(outcome));
    } catch (...) {
        // Answering failed inside the session (it ran out of memory, say): the call still gets an answer.
        core->complete_dropped(state_->call);
    }
}

void Completion::operator()(FeatureOutcome outcome) const {
    if (!state_ || state_->called.exchange(true)) return;
    auto core = state_->call->core.lock();
    if (!core) return;
    try {
        core->complete(state_->call, std::move(outcome));
    } catch (...) {
        core->complete_dropped(state_->call);
    }
}

}  // namespace neomcp
