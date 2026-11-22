// The client engine (neomcp/client/engine.h).
//
// The protocol identifiers the engine works with itself (the notifications of progress and of log messages, the
// members it reads from them, the key of a progress token) are constants here, as they are in the server's session;
// everything a revision decides (which messages exist, which header carries what, which error codes a server of each
// kind sends, how a call is opened, cancelled or asked for input) is read from the profiles or asked of the client
// halves of the mechanisms.
#include "neomcp/client/engine.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/input.h"
#include "neomcp/argument_headers.h"
#include "neomcp/json_schema.h"
#include "neomcp/wire.h"

namespace neomcp {

namespace {

constexpr std::string_view progress_method = "notifications/progress";
constexpr std::string_view log_method = "notifications/message";
constexpr std::string_view meta_member = "_meta";
constexpr std::string_view progress_token_key = "progressToken";
constexpr std::string_view progress_member = "progress";
constexpr std::string_view total_member = "total";
constexpr std::string_view message_member = "message";
constexpr std::string_view level_member = "level";
constexpr std::string_view logger_member = "logger";
constexpr std::string_view data_member = "data";
constexpr std::string_view code_member = "code";
constexpr std::string_view id_member = "id";
// The member of a tool call's params whose values a revision may mirror into headers.
constexpr std::string_view arguments_member = "arguments";
// The name, in a profile's `errors`, of the code a server gives a request whose headers it refuses.
constexpr std::string_view header_mismatch_error = "header_mismatch";
// The media types of an HTTP answer that carry messages of the protocol.
constexpr std::string_view json_media = "application/json";
constexpr std::string_view event_stream_media = "text/event-stream";
// HTTP statuses the opening of contact distinguishes.
constexpr int status_bad_request = 400;
constexpr int status_unauthorized = 401;
constexpr int status_forbidden = 403;
constexpr int status_not_found = 404;
constexpr int status_method_not_allowed = 405;
// The reason, in a profile's Streamable HTTP status map, of the status a server gives a method it does not know: with
// a body whose code is Method not found, that status is an answer only a server that names revisions per request
// sends.
constexpr std::string_view method_not_found_reason = "method_not_found";

constexpr std::string_view busy_message = "The client is answering as many requests of the server as it takes";
constexpr std::string_view unanswered_message = "The client did not answer in time";
constexpr std::string_view unwritable_message = "The client's answer cannot be written for the revision";
constexpr std::string_view timed_out_reason = "The request timed out";
constexpr std::string_view unrestricted_form_message =
    "Invalid params: the requested schema is outside the restricted form of a form";
constexpr std::string_view invalid_content_message = "The client's answer does not fit the requested schema";

bool is_success(int status) { return status >= 200 && status < 300; }

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

// The integer code of an error member, when an int holds it.
std::optional<int> code_of(const json& error) {
    const json* code = member(error, code_member);
    if (code == nullptr || !code->is_number_integer()) return std::nullopt;
    if (code->is_number_unsigned()) {
        const auto value = code->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return std::nullopt;
        return static_cast<int>(value);
    }
    const auto value = code->get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(value);
}

// An error member read as a RequestError; empty when it lacks an integer code or a text message.
std::optional<RequestError> read_error(const json& error) {
    const auto code = code_of(error);
    const json* text = member(error, message_member);
    if (!code || text == nullptr || !text->is_string()) return std::nullopt;
    RequestError read;
    read.code = *code;
    read.message = text->get<std::string>();
    if (const json* data = member(error, data_member)) read.data = *data;
    return read;
}

// The number of a request id this client gave: a non-negative integer. Empty for any other id.
std::optional<std::uint64_t> wire_number(const std::optional<RequestId>& id) {
    if (!id || id->is_string()) return std::nullopt;
    const json value = id->to_json();
    if (!value.is_number_unsigned()) return std::nullopt;
    return value.get<std::uint64_t>();
}

// One request of each kind InputSupport takes, as much as it takes of it: the capabilities the server's half requires
// of these are the ones the client declares, so the two cannot disagree.
std::vector<InputRequest> supported_kinds(const InputSupport& support) {
    std::vector<InputRequest> kinds;
    if (support.form) kinds.emplace_back(ElicitationForm{});
    if (support.url) kinds.emplace_back(ElicitationUrl{});
    if (support.sampling) {
        SamplingRequest request;
        if (support.sampling_tools) request.tools.emplace_back();
        if (support.sampling_context) request.include_context = IncludeContext::this_server;
        kinds.emplace_back(std::move(request));
    }
    if (support.roots) kinds.emplace_back(RootsRequest{});
    return kinds;
}

// The capabilities that declare what InputSupport takes, for no revision in particular.
json support_capabilities(const InputSupport& support) {
    json capabilities = json::object();
    for (const InputRequest& kind : supported_kinds(support)) {
        const json needed = detail::required_capabilities(kind);
        for (const auto& [name, inner] : needed.get_ref<const json::object_t&>()) {
            json& slot = capabilities[name];
            if (!slot.is_object()) slot = json::object();
            for (const auto& [nested, value] : inner.get_ref<const json::object_t&>()) slot[nested] = value;
        }
    }
    return capabilities;
}

// The name a refusal or a failure gives a kind of request: its method, never a text the server sent.
std::string kind_name(const InputRequest& request) { return std::string(detail::method_of(request)); }

}  // namespace

bool InputSupport::takes(const InputRequest& request) const noexcept {
    try {
        return detail::capabilities_declare(request, support_capabilities(*this));
    } catch (...) {
        return false;
    }
}

json client_capabilities(const InputSupport& support, const profile::Profile& profile) {
    const std::vector<InputRequest> kinds = supported_kinds(support);
    return detail::declared_capabilities(kinds, support.roots && support.roots_list_changed, profile);
}

namespace detail {

// The halves of one enabled profile, and what the client says about itself under it.
struct Speaker {
    ClientMechanisms halves;
    ClientSelf self;
    bool per_request = false;
    bool handshake = false;
};

enum class Purpose {
    call,        // the application's
    probe,       // asks which revisions the server serves
    handshake,   // opens the connection
    level,       // sets the level of the server's log messages
    task_cancel  // cancels a task a call created and gave up on; its answer changes nothing
};

// What a call does with a task.
enum class TaskMode {
    follow,  // reads it until it ends, and answers the requests it waits on
    read,    // reads it once
    cancel   // cancels it
};

// Where a call about a task stands.
enum class TaskStep {
    waiting,          // its poll clock runs; nothing of it is on the wire
    reading,          // a read of the task is on the wire
    fetching_result,  // a tasks/result request is on the wire
    updating,         // answers to the task's requests are on the wire
    cancelling        // the cancellation is on the wire
};

enum class FetchFor { none, completed, failed, input_required };

struct TaskWork {
    std::string id;
    TaskMode mode = TaskMode::follow;
    TaskStep step = TaskStep::waiting;
    // The call's own result created the task: when the call ends before the task did, it cancels the task.
    bool owned = false;
    // The task ended, or the server knows no such task: there is nothing to cancel.
    bool settled = false;
    FetchFor fetch_for = FetchFor::none;
    // Whether the originating call asked for progress notifications (preserved across task reads/fetches).
    bool accepts_progress = false;
    std::size_t polls = 0;  // reads sent so far
    // The wait between reads the server suggested last.
    std::optional<std::chrono::milliseconds> suggested;
    // The keys of the requests the call answered, or is answering: never presented to a handler again.
    std::set<std::string> answered;
};

struct Clock {
    bool exists = false;
    bool running = false;
    std::uint64_t epoch = 0;
};

struct Call {
    RequestRef ref;
    Purpose purpose = Purpose::call;
    ClientMessage message;              // as the application gave it
    std::optional<json> round_params;   // the params of a round's retry, which replace message.params
    json input_schema = nullptr;
    std::chrono::milliseconds timeout{0};
    bool reset_on_progress = false;
    bool progress = false;
    const Speaker* speaker = nullptr;   // the profile it went out under last
    std::optional<std::uint64_t> wire;  // its id while it waits on the wire
    UnitId unit = connection_unit;      // where its answer counts
    UnitId sent_unit = connection_unit; // where it went out
    bool held = false;                  // waits in the engine until contact is open
    bool refused = false;               // its revision was refused once
    bool session_resent = false;        // sent once more after the server no longer knew the session
    bool in_round = false;              // waits for the answers of a round
    std::size_t round = 0;              // rounds of input so far (server requests tied to it, where they come so)
    std::optional<InputAsking> asking;
    std::map<std::string, json_schema::Validator> forms;  // the round's forms by key, their schemas compiled
    std::size_t pausers = 0;            // server requests whose answers its clocks wait for
    bool authorizing = false;           // HTTP: the transport holds it while its authorizer works (AuthorizationHold)
    // A subscription (listen): its request is written by the feed half from `filter` each time it goes out, and its
    // wait clock runs only until the acknowledgement.
    bool subscription = false;
    ChangeFilter filter;
    // Where the client takes tasks: a result that creates one ends the call (hand_over_task), or the call follows it.
    bool hand_over_task = false;
    std::optional<TaskWork> task;
    Clock wait;
    Clock total;
    Clock input;
    Clock poll;        // the wait before the next read of its task
    Clock task_clock;  // the whole of its task (ClientTaskOptions::max_wait)
};

struct Ask {
    InputRef ref;
    RequestId server_id;
    InputRequest request;
    const Speaker* speaker = nullptr;
    std::vector<RequestRef> paused;
    Clock input;
    std::optional<json_schema::Validator> form;  // a form's requested schema, compiled
};

// A request the client gave up on (cancelled or timed out) whose answer could still create a task.
struct Abandoned {
    const Speaker* speaker = nullptr;
    std::string method;
};

struct WireEntry {
    RequestRef ref;
    UnitId unit = connection_unit;  // where its answer counts
};

struct Exchange {
    std::optional<int> status;
    std::optional<std::string> media;
};

class ClientEngineCore {
public:
    ClientEngineCore(EngineOptions options, profile::Transport transport, OutgoingSink sink,
                     std::vector<Speaker> speakers)
        : options_(std::move(options)), transport_(transport), sink_(std::move(sink)), speakers_(std::move(speakers)) {
        for (const auto& speaker : speakers_) {
            profiles_.push_back(speaker.halves.profile);
            if (speaker.handshake) handshake_profiles_.push_back(speaker.halves.profile);
            if (speaker.per_request) {
                for (const auto& named : speaker.halves.profile->error_codes()) per_request_codes_.insert(named.code);
            }
        }
    }

    void guard() const {
        if (in_sink_) throw std::logic_error("the client engine was called from inside its own sink");
    }

    // Whether an enabled profile defines the older transport of events over HTTP (the step after a handshake that a
    // Streamable HTTP endpoint refused by its status).
    void set_http_sse_enabled(bool enabled) noexcept { http_sse_enabled_ = enabled; }

    // Whether the server's answers come on the connection rather than on the exchange that carried the request: on
    // stdio, and on the older transport of events over HTTP, whose POSTs carry nothing back.
    bool answers_on_connection() const noexcept { return transport_ != profile::Transport::streamable_http; }
    // Whether the connection is one event stream, whose end ends the connection (the older transport of events over
    // HTTP): the next request opens it again, as a new server process is started on stdio.
    bool connection_is_stream() const noexcept { return transport_ == profile::Transport::http_sse; }
    // Whether the connection ends as a whole and is made again for the next request (restart).
    bool connection_restarts() const noexcept {
        return transport_ == profile::Transport::stdio || connection_is_stream();
    }
    // How the calls that waited on a connection that ended end.
    CallFailure connection_loss() const {
        if (connection_is_stream()) {
            return CallFailure{CallFailureKind::stream_lost, "the event stream of the connection ended"};
        }
        return CallFailure{CallFailureKind::server_exited, "the server process ended"};
    }

    // ---- the public operations

    void open() {
        if (contact_.phase != ContactPhase::unopened || closed_) return;
        if (options_.prior && options_.prior->transport == transport_) {
            const ContactVerdict& prior = *options_.prior;
            const Speaker* named = named_speaker(prior.revision);
            if (named != nullptr && prior.kind == ServerKind::per_request && named->per_request) {
                open_per_request(*named);
                return;
            }
            if (named != nullptr && prior.kind == ServerKind::handshake && named->handshake) {
                start_handshake(*named, false);
                return;
            }
        }
        start_contact();
    }

    std::variant<RequestRef, CallFailure> send(OutgoingCall call) { return send_call(std::move(call), nullptr); }

    // ---- tasks

    bool takes_tasks() const {
        if (closed_ || contact_.phase != ContactPhase::open || governing_ == nullptr) return false;
        const ClientTaskMechanism* half = tasks_of(*governing_);
        return half != nullptr && half->server_supports_tasks(contact_.server.capabilities);
    }

    bool augments_tool_tasks() const {
        if (!takes_tasks()) return false;
        const ClientTaskMechanism* half = tasks_of(*governing_);
        return half->augments_tool_call() && half->server_takes_tool_tasks(contact_.server.capabilities);
    }

    void augment_tool_call(json& params) const {
        if (augments_tool_tasks()) tasks_of(*governing_)->augment_tool_call(params);
    }

    // A call about the task `id` under the governing profile.
    std::variant<RequestRef, CallFailure> task_call(std::string id, TaskMode mode,
                                                    std::optional<std::chrono::milliseconds> timeout) {
        if (closed_) return CallFailure{CallFailureKind::closed, "the engine is closed"};
        if (contact_.phase == ContactPhase::failed) {
            return CallFailure{CallFailureKind::contact_failed, "contact with the server could not be made"};
        }
        if (governing_ == nullptr) return CallFailure{CallFailureKind::contact_failed, "contact with the server is not open"};
        const ClientTaskMechanism* half = tasks_of(*governing_);
        if (half == nullptr || !half->server_supports_tasks(contact_.server.capabilities)) {
            return CallFailure{CallFailureKind::unsupported_method,
                               "the governing revision has no tasks the client takes"};
        }
        if (!task_id_fits(id)) {
            return CallFailure{CallFailureKind::unsupported_method,
                               "the id of a task must be neither empty nor longer than max_id_bytes"};
        }
        if (timeout && timeout->count() <= 0) {
            return CallFailure{CallFailureKind::unsupported_method, "the timeout of a call must be positive"};
        }
        if (application_calls_ >= options_.max_pending_requests) {
            return CallFailure{CallFailureKind::too_many_pending, "as many calls as the client allows are pending"};
        }
        Call& made = new_call(Purpose::call, ClientMessage{}, timeout.value_or(options_.request_timeout));
        const RequestRef ref = made.ref;
        made.speaker = governing_;
        TaskWork work;
        work.id = std::move(id);
        work.mode = mode;
        made.task = std::move(work);
        ++application_calls_;
        if (mode == TaskMode::follow) {
            clock_start(ref, DeadlineKind::task, made.task_clock, options_.tasks.max_wait);
            ++made.task->polls;
        }
        const std::string& task_id = made.task->id;
        ClientMessage message = mode == TaskMode::cancel ? half->cancel(task_id) : half->read(task_id);
        send_about_task(made, std::move(message), mode == TaskMode::cancel ? TaskStep::cancelling : TaskStep::reading);
        return ref;
    }

    // ---- change feeds

    std::optional<FeedDelivery> feed_delivery() const {
        if (governing_ == nullptr || contact_.phase == ContactPhase::failed || closed_) return std::nullopt;
        return governing_->halves.change_feed->delivery();
    }

    ChangeFilter offered(const ChangeFilter& asked) const {
        if (governing_ == nullptr) return asked;
        return governing_->halves.change_feed->offered(asked, contact_.server.capabilities);
    }

    std::variant<RequestRef, CallFailure> listen(ChangeFilter filter, std::optional<std::chrono::milliseconds> timeout) {
        if (closed_) return CallFailure{CallFailureKind::closed, "the engine is closed"};
        const auto delivery = feed_delivery();
        if (!delivery) return CallFailure{CallFailureKind::contact_failed, "contact with the server is not open"};
        if (*delivery != FeedDelivery::subscription) {
            return CallFailure{CallFailureKind::unsupported_method,
                               "the revision tells of changes on the connection, not on subscriptions"};
        }
        OutgoingCall call;
        call.timeout = timeout;
        return send_call(std::move(call), &filter);
    }

    std::variant<ClientMessage, CallFailure> watch_request(std::string uri, bool watch) const {
        if (governing_ == nullptr || closed_) {
            return CallFailure{CallFailureKind::contact_failed, "contact with the server is not open"};
        }
        FeedRequest asked;
        asked.action = watch ? FeedRequest::Action::watch : FeedRequest::Action::unwatch;
        asked.uri = std::move(uri);
        auto written = governing_->halves.change_feed->write(asked, *governing_->halves.profile);
        if (auto* problem = std::get_if<std::string>(&written)) {
            return CallFailure{CallFailureKind::unsupported_method, std::move(*problem)};
        }
        return std::get<ClientMessage>(std::move(written));
    }

    void listen_on_connection(bool listening) {
        if (closed_ || connection_listening_ == listening) return;
        connection_listening_ = listening;
        if (!listening) return;
        // Once asked for, the stream stays for the connection; a listener that goes leaves it open.
        open_connection_stream();
        // A server process that ended while nobody listened is started again now.
        if (contact_.phase == ContactPhase::lost) request_restart();
    }

private:
    // A call of the application, or a subscription when `filter` is given.
    std::variant<RequestRef, CallFailure> send_call(OutgoingCall call, const ChangeFilter* filter) {
        if (closed_) return CallFailure{CallFailureKind::closed, "the engine is closed"};
        if (contact_.phase == ContactPhase::failed) {
            return CallFailure{CallFailureKind::contact_failed, "contact with the server could not be made"};
        }
        if (!call.message.params.is_null() && !call.message.params.is_object()) {
            return CallFailure{CallFailureKind::unsupported_method, "the params of a request must be an object"};
        }
        if (call.timeout && call.timeout->count() <= 0) {
            return CallFailure{CallFailureKind::unsupported_method, "the timeout of a call must be positive"};
        }
        if (application_calls_ >= options_.max_pending_requests) {
            return CallFailure{CallFailureKind::too_many_pending, "as many calls as the client allows are pending"};
        }
        const Speaker* now = nullptr;
        if (contact_.phase == ContactPhase::open) {
            now = governing_;
        } else if (contact_.phase == ContactPhase::handshaking && opening_ != nullptr &&
                   opening_->halves.open->allowed_before_open(call.message.method)) {
            now = opening_;
        }
        Call& made = new_call(Purpose::call, std::move(call.message), call.timeout.value_or(options_.request_timeout));
        made.input_schema = std::move(call.input_schema);
        made.hand_over_task = call.hand_over_task;
        if (filter != nullptr) {
            made.subscription = true;
            made.filter = *filter;
        } else {
            made.reset_on_progress = call.reset_timeout_on_progress;
            made.progress = call.progress;
        }
        const RequestRef ref = made.ref;
        ++application_calls_;
        if (now != nullptr) {
            if (auto failure = build_and_send(made, *now)) {
                --application_calls_;
                calls_.erase(ref);
                return *failure;
            }
        } else {
            made.held = true;
            held_.push_back(ref);
        }
        Call& sent = calls_.at(ref);
        clock_start(ref, DeadlineKind::wait, sent.wait, sent.timeout);
        if (sent.reset_on_progress) clock_start(ref, DeadlineKind::total, sent.total, options_.max_total_timeout);
        if (sent.held && contact_.phase == ContactPhase::lost) request_restart();
        return ref;
    }

public:
    std::optional<CallFailure> notify(ClientMessage message) {
        if (closed_) return CallFailure{CallFailureKind::closed, "the engine is closed"};
        if (contact_.phase != ContactPhase::open || governing_ == nullptr) {
            return CallFailure{CallFailureKind::contact_failed, "contact with the server is not open"};
        }
        const profile::Message* entry = governing_->halves.profile->notification(message.method);
        if (entry == nullptr || !entry->from_client) {
            return CallFailure{CallFailureKind::unsupported_method, "the revision has no such client notification"};
        }
        return send_notification(message, *governing_);
    }

    Reception received(UnitId unit, std::string_view text) {
        if (closed_) return Reception::ignored;
        if (unit != connection_unit && exchanges_.find(unit) == exchanges_.end()) return Reception::ignored;
        // On stdio the line is dropped and the calls wait on; on HTTP the call its exchange carries fails.
        if (text.size() > options_.max_message_bytes) {
            diagnose(DiagnosticKind::too_large, "a message longer than the client takes was dropped", unit);
            if (unit != connection_unit) {
                lose_unit(unit, CallFailure{CallFailureKind::message_too_large, "its answer was too long"});
            }
            return Reception::too_large;
        }
        if (nesting_exceeds(text, options_.max_nesting_depth)) {
            diagnose(DiagnosticKind::too_deep, "a message nested deeper than the client takes was dropped", unit);
            if (unit != connection_unit) {
                lose_unit(unit, CallFailure{CallFailureKind::message_too_large, "its answer was nested too deep"});
            }
            return Reception::too_deep;
        }
        json value = json::parse(text, nullptr, false);
        if (value.is_discarded()) {
            diagnose(DiagnosticKind::not_json, "a message that is not JSON was dropped", unit);
            return Reception::not_json;
        }
        auto split = speaker_for(unit).halves.batch->split(std::move(value));
        if (auto* problem = std::get_if<std::string>(&split)) {
            diagnose(DiagnosticKind::not_message, "a message the revision does not allow was dropped: " + *problem,
                     unit);
            return Reception::not_message;
        }
        bool any = false;
        for (json& one : std::get<std::vector<json>>(split)) {
            if (closed_) break;
            Message message = classify(std::move(one));
            if (message.kind == MessageKind::invalid) {
                diagnose(DiagnosticKind::not_message, "a value that is not a JSON-RPC message was dropped", unit);
                continue;
            }
            any = true;
            switch (message.kind) {
                case MessageKind::response: on_response(unit, message); break;
                case MessageKind::request: on_server_request(unit, message); break;
                case MessageKind::notification: on_notification(unit, message); break;
                case MessageKind::invalid: break;
            }
        }
        return any ? Reception::accepted : Reception::not_message;
    }

    void stream_event(UnitId unit, StreamEvent event) {
        if (closed_) return;
        std::visit([&](auto& happened) { on_stream(unit, happened); }, event);
    }

    void expire(const Deadline& deadline) {
        if (closed_) return;
        if (const auto* ref = std::get_if<RequestRef>(&deadline.owner)) {
            const auto found = calls_.find(*ref);
            if (found == calls_.end()) return;
            Call& call = found->second;
            Clock* clock = &call.input;
            switch (deadline.kind) {
                case DeadlineKind::wait: clock = &call.wait; break;
                case DeadlineKind::total: clock = &call.total; break;
                case DeadlineKind::input: clock = &call.input; break;
                case DeadlineKind::poll: clock = &call.poll; break;
                case DeadlineKind::task: clock = &call.task_clock; break;
            }
            if (!live(*clock, deadline)) return;
            expire_call(call, deadline.kind);
            return;
        }
        const auto ref = std::get<InputRef>(deadline.owner);
        const auto found = asks_.find(ref);
        if (found == asks_.end() || !live(found->second.input, deadline)) return;
        // The server's question was not answered in time: a form or URL request is answered as dismissed, any other
        // kind with an error, and the paused clocks go on.
        Ask& ask = found->second;
        InputReply reply = RequestError{error_code::internal_error, std::string(unanswered_message)};
        if (std::holds_alternative<ElicitationForm>(ask.request) ||
            std::holds_alternative<ElicitationUrl>(ask.request)) {
            reply = InputResponse(ElicitationAnswer{ElicitationAction::cancel, json::object()});
        }
        finish_ask(ref, response_of(ask, reply), true, false);
    }

    void cancel(RequestRef ref, std::string reason) {
        if (closed_) return;
        const auto found = calls_.find(ref);
        if (found == calls_.end() || found->second.purpose != Purpose::call) return;
        Call& call = found->second;
        // What the server still sends to end a subscription it was told to end is expected, not a defect.
        if (call.wire && call.subscription) remember_ended(*call.wire);
        remember_abandoned(call);
        if (call.wire) cancel_on_wire(call, reason);
        end_call(ref, CallFailure{CallFailureKind::cancelled, "the call was cancelled"});
    }

    void answer(InputRef ref, InputReply reply) {
        if (closed_) return;
        const auto found = asks_.find(ref);
        if (found == asks_.end()) return;
        const Ask& ask = found->second;
        if (const auto* response = std::get_if<InputResponse>(&reply); response != nullptr && ask.form) {
            if (!content_fits(*response, *ask.form)) {
                // The content is reported, never sent: the server learns only that the client could not answer.
                push(Diagnostic{DiagnosticKind::answer_invalid,
                                "an accepted form's content does not fit its requested schema and was not sent",
                                std::nullopt, ref},
                     true);
                reply = RequestError{error_code::internal_error, std::string(invalid_content_message)};
            }
        }
        if (std::holds_alternative<InputRefused>(reply)) {
            reply = ask.speaker->halves.server_input->user_refusal(*ask.speaker->halves.profile);
        }
        finish_ask(ref, response_of(ask, reply), false, false);
    }

    void answer_round(RequestRef ref, InputResponses answers) {
        if (closed_) return;
        const auto found = calls_.find(ref);
        if (found == calls_.end() || !found->second.in_round) return;
        Call& call = found->second;
        if (call.task) {
            answer_task_round(call, std::move(answers));
            return;
        }
        const ClientInputMechanism& input = *call.speaker->halves.server_input;
        const profile::Profile& profile = *call.speaker->halves.profile;
        for (const auto& [key, given] : answers) {
            const auto form = call.forms.find(key);
            if (form != call.forms.end() && !content_fits(given, form->second)) {
                end_call(ref, CallFailure{CallFailureKind::answer_invalid,
                                          "an accepted form's content does not fit its requested schema; nothing was sent"});
                return;
            }
        }
        // The state alone first: what fails then is the state, which cannot be sent back unchanged.
        auto echoed = input.retry_params(call.message.params, {}, call.asking->state, profile);
        if (std::holds_alternative<std::string>(echoed)) {
            end_call(ref, CallFailure{CallFailureKind::state_not_echoable, std::get<std::string>(echoed)});
            return;
        }
        auto retry = input.retry_params(call.message.params, answers, call.asking->state, profile);
        if (auto* problem = std::get_if<std::string>(&retry)) {
            end_call(ref, CallFailure{CallFailureKind::answer_not_writable, *problem});
            return;
        }
        call.round_params = std::move(std::get<json>(retry));
        call.in_round = false;
        call.asking.reset();
        call.forms.clear();
        clock_stop(ref, DeadlineKind::input, call.input);
        if (auto failure = build_and_send(call, *call.speaker)) {
            end_call(ref, *failure);
            return;
        }
        resume_call(calls_.at(ref));
    }

    void fail_round(RequestRef ref, CallFailure failure) {
        if (closed_) return;
        const auto found = calls_.find(ref);
        if (found == calls_.end() || !found->second.in_round) return;
        end_call(ref, std::move(failure));
    }

    bool roots_changed() {
        if (closed_ || !options_.input.roots || !options_.input.roots_list_changed) return false;
        if (contact_.phase != ContactPhase::open || governing_ == nullptr) return false;
        auto notification = governing_->halves.server_input->roots_changed(*governing_->halves.profile);
        if (!notification) return false;
        return !send_notification(*notification, *governing_).has_value();
    }

    void transport_restarted() {
        if (closed_) return;
        const CallFailure loss = connection_loss();
        lose_all_on_wire(loss.kind, loss.message);
        end_asks();
        restarting_ = false;
        switch (contact_.phase) {
            case ContactPhase::unopened:
            case ContactPhase::failed:
            case ContactPhase::closed: return;
            default: break;
        }
        reopening_ = false;
        if (restart_for_probe_ || contact_.phase == ContactPhase::probing) {
            restart_for_probe_ = false;
            // The process ended while the probe was out: the new one gets the handshake when a revision that opens
            // with one is enabled, and the probe again when none is.
            if (newest(false) != nullptr) {
                fallback_handshake();
            } else {
                start_contact();
            }
            return;
        }
        if (contact_.verdict && contact_.verdict->kind == ServerKind::per_request && governing_ != nullptr) {
            if (contact_.phase != ContactPhase::open) set_phase(ContactPhase::open);
            release_held();
            return;
        }
        fallback_handshake();
    }

    void close() {
        if (closed_) return;
        closed_ = true;
        end_everything(CallFailureKind::closed, "the engine was closed");
        set_phase(ContactPhase::closed);
        emit(Outgoing{std::string(), connection_unit, {}, OutgoingKind::close, nullptr});
    }

    std::optional<EngineEvent> next_event() {
        if (events_.empty()) return std::nullopt;
        EngineEvent event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    std::uint64_t dropped_events() const noexcept { return dropped_; }
    const ContactState& contact() const noexcept { return contact_; }
    std::span<const profile::ProfilePtr> profiles() const noexcept { return profiles_; }
    std::size_t pending() const noexcept { return application_calls_; }
    const InputSupport& input() const noexcept { return options_.input; }

private:
    // ---- forms

    json_schema::Limits peer_limits() const {
        json_schema::Limits limits;
        limits.max_schema_depth = options_.peer_schema_depth;
        limits.max_pattern_length = options_.peer_pattern_length;
        limits.max_pattern_memory = options_.peer_pattern_memory;
        limits.max_evaluations = options_.peer_max_evaluations;
        limits.max_patterns = options_.peer_patterns_per_schema;
        return limits;
    }

    // ---- schemas of the server, against one budget

public:
    PeerSchema compile_peer_schema(const json& schema) {
        if (peer_compiles_ >= options_.max_peer_schema_compiles) {
            if (!budget_reported_) {
                budget_reported_ = true;
                diagnose(DiagnosticKind::schema_budget_spent,
                         "the client compiled as many schemas of the server as it allows (max_peer_schema_compiles); "
                         "later ones are refused");
            }
            return PeerSchemaRefused{true, std::nullopt};
        }
        ++peer_compiles_;
        auto compiled = json_schema::Validator::compile(schema, peer_limits());
        if (auto* issue = std::get_if<json_schema::Issue>(&compiled)) {
            return PeerSchemaRefused{false, std::move(*issue)};
        }
        return std::get<json_schema::Validator>(std::move(compiled));
    }

    std::size_t peer_schema_compiles() const noexcept { return peer_compiles_; }

private:
    // The form's requested schema, compiled when it fits the restricted form; else why not, in words that repeat
    // nothing the server sent. The compile draws on the budget of the server's schemas.
    std::variant<json_schema::Validator, std::string> check_form(const ElicitationForm& form, const Speaker& speaker) {
        if (auto problem = speaker.halves.server_input->form_problem(form.requested_schema, *speaker.halves.profile)) {
            return std::move(*problem);
        }
        auto compiled = compile_peer_schema(form.requested_schema);
        if (auto* refused = std::get_if<PeerSchemaRefused>(&compiled)) {
            if (refused->budget_spent) {
                return std::string("the requested schema was not compiled: the budget of schema compiles is spent");
            }
            return std::string("the requested schema cannot be compiled within the limits for a server's schemas");
        }
        return std::get<json_schema::Validator>(std::move(compiled));
    }

    // Whether an answer to a form may go out: anything but an accepted form, or accepted content that its schema
    // takes.
    static bool content_fits(const InputResponse& response, const json_schema::Validator& form) {
        const auto* elicited = std::get_if<ElicitationAnswer>(&response);
        if (elicited == nullptr || elicited->action != ElicitationAction::accept) return true;
        return !form.validate(elicited->content).has_value();
    }

    // Whether the client declared, under the speaker's profile, what the request needs.
    static bool declared(const InputRequest& request, const Speaker& speaker) {
        return detail::capabilities_declare(request, speaker.self.capabilities);
    }

    // ---- events and outputs

    void push(EngineEvent event, bool droppable) {
        if (droppable && events_.size() >= options_.max_queued_events) {
            ++dropped_;
            return;
        }
        events_.push_back(std::move(event));
    }

    void diagnose(DiagnosticKind kind, std::string message, std::optional<UnitId> unit = std::nullopt) {
        push(Diagnostic{kind, std::move(message), unit, std::nullopt}, true);
    }

    void set_phase(ContactPhase phase) {
        contact_.phase = phase;
        push(ContactChanged{phase}, false);
    }

    void step(RequestRef ref, std::string method, std::string outcome) {
        contact_.steps.push_back(ContactStep{ref, std::move(method), std::move(outcome)});
    }

    // Writes through the sink. A sink that throws closes the engine, and the exception goes on to the caller.
    void emit(Outgoing outgoing) {
        if (dead_) return;
        in_sink_ = true;
        try {
            sink_(std::move(outgoing));
        } catch (...) {
            in_sink_ = false;
            die();
            throw;
        }
        in_sink_ = false;
    }

    void die() {
        dead_ = true;
        closed_ = true;
        end_everything(CallFailureKind::closed, "the transport failed to take a message");
        if (contact_.phase != ContactPhase::closed) set_phase(ContactPhase::closed);
    }

    UnitId new_unit() {
        if (transport_ == profile::Transport::stdio) return connection_unit;
        const UnitId unit = next_unit_++;
        exchanges_.emplace(unit, Exchange{});
        return unit;
    }

    // Ends an exchange the engine is done with; nothing for one that ended already.
    void end_exchange(UnitId unit) {
        if (unit == connection_unit || exchanges_.erase(unit) == 0) return;
        emit(Outgoing{std::string(), unit, {}, OutgoingKind::end_unit, nullptr});
    }

    // ---- clocks

    void change(DeadlineOwner owner, DeadlineKind kind, Clock& clock, DeadlineAction action,
                std::chrono::milliseconds duration = std::chrono::milliseconds{0}) {
        clock.epoch = next_epoch_++;
        push(DeadlineChange{Deadline{owner, kind, clock.epoch}, action, duration}, false);
    }
    void clock_start(DeadlineOwner owner, DeadlineKind kind, Clock& clock, std::chrono::milliseconds duration) {
        clock.exists = true;
        clock.running = true;
        change(owner, kind, clock, DeadlineAction::start, duration);
    }
    void clock_pause(DeadlineOwner owner, DeadlineKind kind, Clock& clock) {
        if (!clock.exists || !clock.running) return;
        clock.running = false;
        change(owner, kind, clock, DeadlineAction::pause);
    }
    void clock_resume(DeadlineOwner owner, DeadlineKind kind, Clock& clock) {
        if (!clock.exists || clock.running) return;
        clock.running = true;
        change(owner, kind, clock, DeadlineAction::resume);
    }
    void clock_stop(DeadlineOwner owner, DeadlineKind kind, Clock& clock) {
        if (!clock.exists) return;
        clock.exists = false;
        clock.running = false;
        change(owner, kind, clock, DeadlineAction::stop);
    }
    static bool live(const Clock& clock, const Deadline& deadline) {
        return clock.exists && clock.running && clock.epoch == deadline.epoch;
    }

    void pause_call(Call& call) {
        clock_pause(call.ref, DeadlineKind::wait, call.wait);
        clock_pause(call.ref, DeadlineKind::total, call.total);
    }
    void resume_call(Call& call) {
        if (call.in_round || call.pausers > 0 || call.authorizing) return;
        clock_resume(call.ref, DeadlineKind::wait, call.wait);
        clock_resume(call.ref, DeadlineKind::total, call.total);
    }

    // ---- calls

    Call& new_call(Purpose purpose, ClientMessage message, std::chrono::milliseconds timeout) {
        const RequestRef ref{next_ref_++};
        Call call;
        call.ref = ref;
        call.purpose = purpose;
        call.message = std::move(message);
        call.timeout = timeout;
        return calls_.emplace(ref, std::move(call)).first->second;
    }

    // Ends a call: its clocks stop, it leaves the pending table, and the application's calls get their end.
    void end_call(RequestRef ref, RequestOutcome outcome) {
        const auto found = calls_.find(ref);
        if (found == calls_.end()) return;
        Call& call = found->second;
        clock_stop(ref, DeadlineKind::wait, call.wait);
        clock_stop(ref, DeadlineKind::total, call.total);
        clock_stop(ref, DeadlineKind::input, call.input);
        clock_stop(ref, DeadlineKind::poll, call.poll);
        clock_stop(ref, DeadlineKind::task, call.task_clock);
        if (call.wire) wires_.erase(*call.wire);
        if (call.held) held_.erase(std::remove(held_.begin(), held_.end(), ref), held_.end());
        const bool application = call.purpose == Purpose::call;
        profile::ProfilePtr profile = call.speaker != nullptr ? call.speaker->halves.profile : nullptr;
        std::optional<std::string> task_id;
        const Speaker* task_speaker = nullptr;
        if (call.task) {
            task_id = call.task->id;
            if (call.task->owned && !call.task->settled && gives_up(outcome)) task_speaker = call.speaker;
        }
        calls_.erase(found);
        // A call that gives up on the task it created cancels it; where the answer was lost, the task may be followed
        // again by its id, and while contact is not open nothing can go out.
        if (task_speaker != nullptr && !closed_ && contact_.phase == ContactPhase::open) {
            if (const ClientTaskMechanism* half = tasks_of(*task_speaker)) {
                internal(Purpose::task_cancel, half->cancel(*task_id), options_.request_timeout, *task_speaker, false);
            }
        }
        if (!application) return;
        --application_calls_;
        push(RequestEnded{ref, std::move(outcome), std::move(profile), std::move(task_id)}, false);
    }

    // Whether a call that ends so gives up on its task (rather than having lost its answer or the connection).
    static bool gives_up(const RequestOutcome& outcome) {
        const auto* failure = std::get_if<CallFailure>(&outcome);
        if (failure == nullptr) return true;
        switch (failure->kind) {
            case CallFailureKind::server_exited:
            case CallFailureKind::stream_lost:
            case CallFailureKind::send_failed:
            case CallFailureKind::contact_failed:
            case CallFailureKind::closed: return false;
            default: return true;
        }
    }

    // ---- tasks

    // The tasks half of a speaker, when the client takes tasks under it.
    const ClientTaskMechanism* tasks_of(const Speaker& speaker) const {
        if (!options_.tasks.accept || speaker.halves.tasks == nullptr || !speaker.halves.tasks->takes_tasks()) {
            return nullptr;
        }
        return speaker.halves.tasks.get();
    }

    bool task_id_fits(const std::string& id) const { return !id.empty() && id.size() <= options_.tasks.max_id_bytes; }

    // A call whose result created the task `state` follows it.
    void start_following(Call& call, TaskState state) {
        const RequestRef ref = call.ref;
        clock_stop(ref, DeadlineKind::wait, call.wait);
        clock_stop(ref, DeadlineKind::total, call.total);
        TaskWork work;
        work.id = std::move(state.id);
        work.owned = true;
        work.accepts_progress = call.progress;
        work.suggested = state.poll_interval;
        call.task = std::move(work);
        clock_start(ref, DeadlineKind::task, call.task_clock, options_.tasks.max_wait);
        schedule_read(call);
    }

    // The wait before the next read, or the end of the call when it read its task as often as it may.
    void schedule_read(Call& call) {
        const RequestRef ref = call.ref;
        TaskWork& task = *call.task;
        if (task.polls >= options_.tasks.max_polls) {
            end_call(ref, CallFailure{CallFailureKind::task_polls_exceeded,
                                      "the task was read as often as the client reads one, and has not ended"});
            return;
        }
        task.step = TaskStep::waiting;
        clock_stop(ref, DeadlineKind::wait, call.wait);
        const std::chrono::milliseconds wait = std::clamp(task.suggested.value_or(options_.tasks.min_poll_interval),
                                                          options_.tasks.min_poll_interval,
                                                          options_.tasks.max_poll_interval);
        clock_start(ref, DeadlineKind::poll, call.poll, wait);
    }

    // The poll clock lapsed: the task is read.
    void read_due(Call& call) {
        const ClientTaskMechanism* half = tasks_of(*call.speaker);
        if (half == nullptr) {
            end_call(call.ref, CallFailure{CallFailureKind::unsupported_method, "the revision has no tasks"});
            return;
        }
        ++call.task->polls;
        send_about_task(call, half->read(call.task->id), TaskStep::reading);
    }

    // Sends a request about the call's task, under the profile the call went out under, or keeps it until contact is
    // open again. When `use_wait_clock` is true, its short request wait clock runs from now; for a blocking
    // `tasks/result` on `input_required`, the overall task clock and input handler clock govern instead.
    void send_about_task(Call& call, ClientMessage message, TaskStep step, bool use_wait_clock = true) {
        const RequestRef ref = call.ref;
        call.message = std::move(message);
        call.round_params.reset();
        call.progress = false;
        call.reset_on_progress = false;
        call.input_schema = nullptr;
        call.task->step = step;
        clock_stop(ref, DeadlineKind::poll, call.poll);
        if (contact_.phase == ContactPhase::open && governing_ != nullptr) {
            if (auto failure = build_and_send(call, *call.speaker)) {
                end_call(ref, *failure);
                return;
            }
        } else {
            call.held = true;
            held_.push_back(ref);
            if (contact_.phase == ContactPhase::lost) request_restart();
        }
        const auto found = calls_.find(ref);
        if (use_wait_clock && found != calls_.end()) {
            clock_start(ref, DeadlineKind::wait, found->second.wait, found->second.timeout);
        }
    }

    // The answer to a request about a task.
    void on_task_answer(RequestRef ref, const Message& message) {
        Call& call = calls_.at(ref);
        TaskWork& task = *call.task;
        const profile::Profile& profile = *call.speaker->halves.profile;
        const ClientTaskMechanism* half = tasks_of(*call.speaker);
        clock_stop(ref, DeadlineKind::wait, call.wait);
        if (half == nullptr) {
            end_call(ref, CallFailure{CallFailureKind::malformed_answer, "the revision has no tasks"});
            return;
        }
        if (message.failed) {
            if ((task.step == TaskStep::fetching_result && task.fetch_for != FetchFor::input_required) ||
                task.step == TaskStep::cancelling) {
                task.settled = true;
            }
            auto error = read_error(message.outcome);
            if (!error) {
                end_call(ref, CallFailure{CallFailureKind::malformed_answer, "the error answer cannot be read"});
                return;
            }
            const bool check_unknown =
                (task.step != TaskStep::cancelling || half->cancel_error_means_unknown()) &&
                !(task.step == TaskStep::fetching_result && task.fetch_for == FetchFor::failed);
            if (check_unknown && half->says_unknown(*error)) {
                task.settled = true;
                end_call(ref, CallFailure{CallFailureKind::task_lost, "the server does not know the task"});
                return;
            }
            note_defect(error->code, profile);
            end_call(ref, ErrorAnswered{std::move(*error)});
            return;
        }
        switch (task.step) {
            case TaskStep::reading: break;
            case TaskStep::fetching_result:
                task.settled = true;
                end_call(ref, Answered{message.outcome});
                return;
            case TaskStep::updating:
            case TaskStep::cancelling:
                if (task.step == TaskStep::cancelling) task.settled = true;
                if (auto problem = half->read_acknowledgement(message.outcome, profile)) {
                    end_call(ref, CallFailure{CallFailureKind::malformed_answer,
                                              "the acknowledgement cannot be read: " + *problem});
                    return;
                }
                if (task.step == TaskStep::cancelling) {
                    end_call(ref, Answered{message.outcome});
                } else {
                    schedule_read(call);
                }
                return;
            case TaskStep::waiting:
                end_call(ref, CallFailure{CallFailureKind::malformed_answer, "an answer came while no request was out"});
                return;
        }
        auto read = half->read_state(message.outcome, profile);
        if (auto* problem = std::get_if<std::string>(&read)) {
            end_call(ref, CallFailure{CallFailureKind::malformed_answer, "the state of the task cannot be read: " + *problem});
            return;
        }
        TaskState state = std::get<TaskState>(std::move(read));
        if (state.id != task.id) {
            end_call(ref, CallFailure{CallFailureKind::malformed_answer, "the server answered with another task"});
            return;
        }
        if (state.poll_interval) task.suggested = state.poll_interval;
        if (task.mode == TaskMode::read) {
            end_call(ref, Answered{message.outcome, std::move(state)});
            return;
        }
        switch (state.status) {
            case TaskStatus::completed:
                if (half->result_is_separate()) {
                    task.fetch_for = FetchFor::completed;
                    send_about_task(call, half->result(task.id), TaskStep::fetching_result, true);
                    return;
                }
                task.settled = true;
                end_call(ref, Answered{std::move(*state.result)});
                return;
            case TaskStatus::failed:
                if (half->result_is_separate()) {
                    task.fetch_for = FetchFor::failed;
                    send_about_task(call, half->result(task.id), TaskStep::fetching_result, true);
                    return;
                }
                task.settled = true;
                end_call(ref, ErrorAnswered{std::move(*state.error)});
                return;
            case TaskStatus::cancelled:
                task.settled = true;
                end_call(ref, CallFailure{CallFailureKind::task_cancelled, "the task was cancelled"});
                return;
            case TaskStatus::working: schedule_read(call); return;
            case TaskStatus::input_required:
                if (half->result_is_separate()) {
                    task.fetch_for = FetchFor::input_required;
                    send_about_task(call, half->result(task.id), TaskStep::fetching_result, false);
                    return;
                }
                break;
        }
        // The requests the call has not answered yet make a round; with none, the reads go on.
        InputAsking asking;
        for (AskedInput& one : state.requests) {
            if (task.answered.count(one.key) == 0) asking.requests.push_back(std::move(one));
        }
        if (asking.requests.empty()) {
            schedule_read(call);
            return;
        }
        open_round(call, std::move(asking));
    }

    // The answers of a round of a task go to the task.
    void answer_task_round(Call& call, InputResponses answers) {
        const RequestRef ref = call.ref;
        const ClientTaskMechanism* half = tasks_of(*call.speaker);
        const profile::Profile& profile = *call.speaker->halves.profile;
        for (const auto& [key, given] : answers) {
            const auto form = call.forms.find(key);
            if (form != call.forms.end() && !content_fits(given, form->second)) {
                end_call(ref, CallFailure{CallFailureKind::answer_invalid,
                                          "an accepted form's content does not fit its requested schema; nothing was sent"});
                return;
            }
        }
        if (half == nullptr) {
            end_call(ref, CallFailure{CallFailureKind::answer_not_writable, "the revision has no tasks"});
            return;
        }
        auto written = half->update(call.task->id, answers, profile);
        if (auto* problem = std::get_if<std::string>(&written)) {
            end_call(ref, CallFailure{CallFailureKind::answer_not_writable, *problem});
            return;
        }
        for (const AskedInput& one : call.asking->requests) call.task->answered.insert(one.key);
        call.in_round = false;
        call.asking.reset();
        call.forms.clear();
        clock_stop(ref, DeadlineKind::input, call.input);
        send_about_task(call, std::get<ClientMessage>(std::move(written)), TaskStep::updating);
    }

    // A round of the call: what it asks is checked as every round is (declared, bounded, forms restricted), and the
    // call waits for the answers with its clocks paused. Ends the call when a check fails.
    void open_round(Call& call, InputAsking asking) {
        const RequestRef ref = call.ref;
        const Speaker& speaker = *call.speaker;
        for (const AskedInput& one : asking.requests) {
            if (!declared(one.request, speaker)) {
                end_call(ref, CallFailure{CallFailureKind::server_asked_undeclared,
                                          "the server asked for " + kind_name(one.request) +
                                              " in a way the client did not declare"});
                return;
            }
        }
        if (asking.requests.size() > options_.max_input_requests_per_round) {
            end_call(ref, CallFailure{CallFailureKind::input_requests_exceeded,
                                      "the server asked for more inputs in one round than the client allows"});
            return;
        }
        if (call.round >= options_.max_input_rounds) {
            end_call(ref, CallFailure{CallFailureKind::input_rounds_exceeded,
                                      "the server asked for input more often than the client allows"});
            return;
        }
        std::map<std::string, json_schema::Validator> forms;
        for (const AskedInput& one : asking.requests) {
            const auto* form = std::get_if<ElicitationForm>(&one.request);
            if (form == nullptr) continue;
            auto checked = check_form(*form, speaker);
            if (auto* problem = std::get_if<std::string>(&checked)) {
                end_call(ref, CallFailure{CallFailureKind::form_not_restricted, std::move(*problem)});
                return;
            }
            forms.emplace(one.key, std::get<json_schema::Validator>(std::move(checked)));
        }
        ++call.round;
        call.in_round = true;
        call.asking = asking;
        call.forms = std::move(forms);
        pause_call(call);
        clock_start(ref, DeadlineKind::input, call.input, options_.input_handler_timeout);
        push(InputRoundStarted{ref, std::move(asking), call.round, speaker.halves.profile}, false);
    }

    void finish(RequestRef ref) { end_call(ref, Answered{}); }

    // Writes a call under a profile: its params with what the open mechanism and a progress token add, its id and its
    // headers. Nothing is written or changed when it fails.
    std::optional<CallFailure> build_and_send(Call& call, const Speaker& speaker) {
        const profile::Profile& profile = *speaker.halves.profile;
        if (call.subscription) {
            // The feed half writes the request for the profile it goes out under.
            FeedRequest asked;
            asked.action = FeedRequest::Action::listen;
            asked.filter = call.filter;
            auto written = speaker.halves.change_feed->write(asked, profile);
            if (auto* problem = std::get_if<std::string>(&written)) {
                return CallFailure{CallFailureKind::unsupported_method, std::move(*problem)};
            }
            call.message = std::get<ClientMessage>(std::move(written));
        }
        const profile::Message* entry = profile.request(call.message.method);
        if (entry == nullptr || !entry->from_client) {
            return CallFailure{CallFailureKind::unsupported_method,
                               "revision " + profile.revision() + " has no client request " + call.message.method};
        }
        json params = call.round_params.value_or(call.message.params);
        if (call.progress) {
            if (params.is_null()) params = json::object();
            json& meta = params[std::string(meta_member)];
            if (meta.is_null()) meta = json::object();
            if (!meta.is_object()) {
                return CallFailure{CallFailureKind::unsupported_method, "params._meta must be an object"};
            }
            meta[std::string(progress_token_key)] = call.ref.value;
        }
        if (auto problem = speaker.halves.open->annotate(params, profile, speaker.self)) {
            return CallFailure{CallFailureKind::unsupported_method, *problem};
        }
        const std::uint64_t id = next_id_;
        json message = make_notification(call.message.method, std::move(params));
        message[std::string(id_member)] = id;
        auto headers = headers_for(OutgoingKind::request, message, speaker, call.input_schema);
        if (auto* failure = std::get_if<CallFailure>(&headers)) return *failure;
        ++next_id_;
        const UnitId unit = new_unit();
        const UnitId answer_unit = transport_ == profile::Transport::streamable_http ? unit : connection_unit;
        wires_[id] = WireEntry{call.ref, answer_unit};
        call.wire = id;
        call.unit = answer_unit;
        call.sent_unit = unit;
        call.speaker = &speaker;
        call.held = false;
        emit(Outgoing{serialize(message), unit, std::move(std::get<std::vector<HttpHeader>>(headers)),
                      OutgoingKind::request, speaker.halves.profile});
        return std::nullopt;
    }

    // An internal request of the engine (probe, handshake, level): written at once under `speaker`.
    std::optional<RequestRef> internal(Purpose purpose, ClientMessage message, std::chrono::milliseconds timeout,
                                       const Speaker& speaker, bool refused) {
        Call& call = new_call(purpose, std::move(message), timeout);
        call.refused = refused;
        const RequestRef ref = call.ref;
        if (auto failure = build_and_send(call, speaker)) {
            finish(ref);
            return std::nullopt;
        }
        Call& sent = calls_.at(ref);
        clock_start(ref, DeadlineKind::wait, sent.wait, sent.timeout);
        return ref;
    }

    std::optional<CallFailure> send_notification(const ClientMessage& notification, const Speaker& speaker) {
        json message = make_notification(notification.method, notification.params);
        auto headers = headers_for(OutgoingKind::notification, message, speaker, nullptr);
        if (auto* failure = std::get_if<CallFailure>(&headers)) return *failure;
        const UnitId unit = new_unit();
        emit(Outgoing{serialize(message), unit, std::move(std::get<std::vector<HttpHeader>>(headers)),
                      OutgoingKind::notification, speaker.halves.profile});
        return std::nullopt;
    }

    void send_response(const json& message, const Speaker& speaker) {
        auto headers = headers_for(OutgoingKind::response, message, speaker, nullptr);
        if (std::holds_alternative<CallFailure>(headers)) {
            diagnose(DiagnosticKind::malformed_message, "an answer to the server could not be given its headers");
            return;
        }
        const UnitId unit = new_unit();
        emit(Outgoing{serialize(message), unit, std::move(std::get<std::vector<HttpHeader>>(headers)),
                      OutgoingKind::response, speaker.halves.profile});
    }

    // The header fields the profile's entry for the transport puts on a message.
    std::variant<std::vector<HttpHeader>, CallFailure> headers_for(OutgoingKind kind, const json& message,
                                                                   const Speaker& speaker, const json& input_schema) {
        std::vector<HttpHeader> headers;
        if (transport_ != profile::Transport::streamable_http) return headers;
        const profile::Profile& profile = *speaker.halves.profile;
        const profile::StreamableHttpTransport& http = *profile.streamable_http();
        const json* method_value = member(message, "method");
        const std::string method =
            method_value != nullptr && method_value->is_string() ? method_value->get<std::string>() : std::string();
        static const json no_params = nullptr;
        const json* params_value = member(message, "params");
        const json& params = params_value != nullptr ? *params_value : no_params;
        for (const auto& header : http.request_headers) {
            bool applies = false;
            switch (header.scope) {
                case profile::HeaderScopeKind::every_message: applies = true; break;
                case profile::HeaderScopeKind::every_request: applies = kind == OutgoingKind::request; break;
                case profile::HeaderScopeKind::per_method:
                    applies = kind == OutgoingKind::request && header.params_member(method).has_value();
                    break;
            }
            if (!applies) continue;
            switch (header.value) {
                case profile::HeaderValue::protocol_version:
                    headers.push_back({header.name, profile.revision()});
                    break;
                case profile::HeaderValue::method:
                    if (kind == OutgoingKind::response) break;
                    headers.push_back({header.name, method});
                    break;
                case profile::HeaderValue::params_member: {
                    const std::string_view key = *header.params_member(method);
                    const json* value = member(params, key);
                    if (value == nullptr || !value->is_string()) {
                        return CallFailure{CallFailureKind::header_value,
                                           "the " + header.name + " header mirrors params." + std::string(key) +
                                               ", which the request does not give as a string"};
                    }
                    const std::string& text = value->get_ref<const std::string&>();
                    headers.push_back({header.name, http.encoded_header_value
                                                        ? encode_header_text(text, *http.encoded_header_value)
                                                        : text});
                    break;
                }
            }
        }
        if (http.version_header && handshake_answered_ && &speaker == governing_) {
            headers.push_back({http.version_header->name, profile.revision()});
        }
        if (kind == OutgoingKind::request && !input_schema.is_null() && http.argument_headers) {
            auto marks = header_parameters(input_schema, http.argument_headers->schema_keyword);
            if (std::holds_alternative<HeaderMarkIssue>(marks)) {
                return CallFailure{CallFailureKind::header_value,
                                   "the tool's input schema marks its parameters wrongly"};
            }
            const json* arguments = member(params, arguments_member);
            for (const auto& parameter : std::get<std::vector<HeaderParameter>>(marks)) {
                const json* value = arguments;
                for (const auto& name : parameter.path) {
                    value = value == nullptr ? nullptr : member(*value, name);
                }
                if (value == nullptr) continue;
                if (!http.encoded_header_value) {
                    return CallFailure{CallFailureKind::header_value, "the revision defines no form for header values"};
                }
                auto encoded = encode_header_value(*value, *http.encoded_header_value);
                if (auto* issue = std::get_if<HeaderValueIssue>(&encoded)) {
                    return CallFailure{CallFailureKind::header_value,
                                       "the argument " + parameter.name + " cannot be mirrored: " + issue->rule};
                }
                const auto& text = std::get<std::optional<std::string>>(encoded);
                if (text) headers.push_back({http.argument_headers->header_prefix + parameter.name, *text});
            }
        }
        return headers;
    }

    // Cancels a call that waits on the wire, as the cancellation mechanism of its transport prescribes.
    void cancel_on_wire(Call& call, std::string_view reason) {
        const std::uint64_t id = *call.wire;
        wires_.erase(id);
        call.wire.reset();
        const Speaker& speaker = *call.speaker;
        const UnitId unit = call.sent_unit;
        if (speaker.halves.cancel->stream_close_cancels()) {
            end_exchange(unit);
            return;
        }
        if (auto notification = speaker.halves.cancel->notification(RequestId(id), reason, *speaker.halves.profile)) {
            send_notification(*notification, speaker);
        }
        // Dropping the exchange alone cancels nothing here, but its answer is no longer wanted.
        if (transport_ == profile::Transport::streamable_http) end_exchange(unit);
    }

    void expire_call(Call& call, DeadlineKind kind) {
        const RequestRef ref = call.ref;
        switch (call.purpose) {
            case Purpose::probe:
                step(ref, call.message.method, "no answer in time");
                finish(ref);
                if (transport_ == profile::Transport::stdio) {
                    // A server that opens with a handshake may not answer an unknown request at all.
                    fallback_handshake();
                } else {
                    fail_contact(ContactFailureKind::transport, "the probe was not answered in time");
                }
                return;
            case Purpose::handshake:
                // The handshake is never cancelled: the transport ends.
                step(ref, call.message.method, "no answer in time");
                finish(ref);
                fail_contact(ContactFailureKind::handshake_timed_out, "the handshake was not answered in time");
                emit(Outgoing{std::string(), connection_unit, {}, OutgoingKind::close, nullptr});
                return;
            case Purpose::level:
            case Purpose::task_cancel:
                if (call.wire) cancel_on_wire(call, timed_out_reason);
                finish(ref);
                return;
            case Purpose::call: break;
        }
        if (kind == DeadlineKind::poll) {
            read_due(call);
            return;
        }
        if (kind == DeadlineKind::task) {
            if (call.wire) cancel_on_wire(call, timed_out_reason);
            end_call(ref, CallFailure{CallFailureKind::timed_out,
                                      "the task did not end within the longest the client follows one"});
            return;
        }
        if (kind == DeadlineKind::input) {
            end_call(ref, CallFailure{CallFailureKind::input_timed_out, "a round of input was not answered in time"});
            return;
        }
        if (call.wire && call.subscription) remember_ended(*call.wire);
        remember_abandoned(call);
        if (call.wire) cancel_on_wire(call, timed_out_reason);
        end_call(ref, CallFailure{CallFailureKind::timed_out, "the call was not answered in time"});
    }

    // Every call that waits on the wire loses its answer.
    void lose_all_on_wire(CallFailureKind kind, const std::string& message) {
        std::vector<RequestRef> refs;
        for (const auto& [id, entry] : wires_) refs.push_back(entry.ref);
        for (const RequestRef ref : refs) {
            const auto found = calls_.find(ref);
            if (found == calls_.end()) continue;
            if (found->second.purpose == Purpose::call) {
                end_call(ref, CallFailure{kind, message});
            } else {
                finish(ref);
            }
        }
    }

    // The calls whose answer counts on `unit` lose it (HTTP exchanges; on stdio the connection). Where answers come
    // on the connection, an exchange carries only its request: the calls it carried lose their answer with it.
    void lose_unit(UnitId unit, CallFailure failure, std::optional<int> status = std::nullopt) {
        std::vector<RequestRef> refs;
        for (const auto& [id, entry] : wires_) {
            if (unit == connection_unit) {
                if (entry.unit == unit && transport_ == profile::Transport::stdio) refs.push_back(entry.ref);
                continue;
            }
            if (entry.unit == unit) {
                refs.push_back(entry.ref);
                continue;
            }
            if (!answers_on_connection()) continue;
            const auto call = calls_.find(entry.ref);
            if (call != calls_.end() && call->second.sent_unit == unit) refs.push_back(entry.ref);
        }
        if (unit != connection_unit && !refs.empty()) end_exchange(unit);
        for (const RequestRef ref : refs) lose(ref, failure, status);
    }

    void lose(RequestRef ref, const CallFailure& failure, std::optional<int> status) {
        const auto found = calls_.find(ref);
        if (found == calls_.end()) return;
        const Purpose purpose = found->second.purpose;
        const std::string method = found->second.message.method;
        if (purpose == Purpose::call) {
            end_call(ref, failure);
            return;
        }
        finish(ref);
        if (purpose == Purpose::level || purpose == Purpose::task_cancel) return;
        step(ref, method, status ? failure.message + " (status " + std::to_string(*status) + ")" : failure.message);
        if (purpose == Purpose::probe && status && refusing_status(*status)) {
            fallback_handshake();
            return;
        }
        if (purpose == Purpose::handshake && status && refusing_status(*status)) {
            fail_handshake_by_status();
            return;
        }
        if (failure.kind == CallFailureKind::authorization_required) {
            fail_contact(ContactFailureKind::authorization_required, failure.message);
        } else if (failure.kind == CallFailureKind::authorization_failed) {
            fail_contact(ContactFailureKind::authorization_failed, failure.message, {}, std::nullopt,
                         failure.authorization);
        } else if (status && !is_success(*status)) {
            fail_contact(ContactFailureKind::http_status, failure.message);
        } else {
            fail_contact(ContactFailureKind::transport, failure.message);
        }
    }

    void release_held() {
        while (!held_.empty() && contact_.phase == ContactPhase::open && governing_ != nullptr) {
            const RequestRef ref = held_.front();
            held_.pop_front();
            const auto found = calls_.find(ref);
            if (found == calls_.end()) continue;
            found->second.held = false;
            if (auto failure = build_and_send(found->second, *governing_)) end_call(ref, *failure);
        }
    }

    void end_asks() {
        std::vector<InputRef> refs;
        for (const auto& [ref, ask] : asks_) refs.push_back(ref);
        for (const InputRef ref : refs) finish_ask(ref, std::nullopt, true, false);
    }

    void end_everything(CallFailureKind kind, const std::string& message) {
        std::vector<RequestRef> refs;
        for (const auto& [ref, call] : calls_) refs.push_back(ref);
        for (const RequestRef ref : refs) end_call(ref, CallFailure{kind, message});
        held_.clear();
        std::vector<InputRef> asks;
        for (const auto& [ref, ask] : asks_) asks.push_back(ref);
        for (const InputRef ref : asks) {
            auto found = asks_.find(ref);
            clock_stop(ref, DeadlineKind::input, found->second.input);
            asks_.erase(found);
            push(ServerAskEnded{ref, false}, false);
        }
    }

    void request_restart() {
        if (restarting_ || !connection_restarts()) return;
        restarting_ = true;
        emit(Outgoing{std::string(), connection_unit, {}, OutgoingKind::restart, nullptr});
    }

    // ---- contact

    const Speaker* newest(bool per_request) const {
        for (const auto& speaker : speakers_) {
            if (per_request ? speaker.per_request : speaker.handshake) return &speaker;
        }
        return nullptr;
    }

    // The newest enabled speaker of a kind whose revision is in the list.
    const Speaker* newest_in(const std::vector<std::string>& revisions, bool per_request) const {
        for (const auto& speaker : speakers_) {
            if (!(per_request ? speaker.per_request : speaker.handshake)) continue;
            if (std::find(revisions.begin(), revisions.end(), speaker.halves.profile->revision()) != revisions.end()) {
                return &speaker;
            }
        }
        return nullptr;
    }

    const Speaker* named_speaker(const std::string& revision) const {
        for (const auto& speaker : speakers_) {
            if (speaker.halves.profile->revision() == revision) return &speaker;
        }
        return nullptr;
    }

    const Speaker* speaker_of(const profile::ProfilePtr& profile) const {
        for (const auto& speaker : speakers_) {
            if (speaker.halves.profile == profile) return &speaker;
        }
        return nullptr;
    }

    // The profile a message on `unit` is read under: that of the call its exchange carries, else the governing one,
    // else the one contact is being opened under, else the newest.
    const Speaker& speaker_for(UnitId unit) const {
        if (unit != connection_unit) {
            for (const auto& [id, entry] : wires_) {
                if (entry.unit != unit) continue;
                const auto found = calls_.find(entry.ref);
                if (found != calls_.end() && found->second.speaker != nullptr) return *found->second.speaker;
            }
        }
        if (governing_ != nullptr) return *governing_;
        if (opening_ != nullptr) return *opening_;
        return speakers_.front();
    }

    void start_contact() {
        if (const Speaker* named_per_request = newest(true)) {
            start_probe(*named_per_request, false);
        } else {
            start_handshake(*newest(false), false);
        }
    }

    void start_probe(const Speaker& speaker, bool refused) {
        auto message = speaker.halves.open->probe(*speaker.halves.profile);
        if (!message) {
            fail_contact(ContactFailureKind::transport,
                         "the revision has no request that asks for the server's revisions");
            return;
        }
        opening_ = &speaker;
        if (contact_.phase != ContactPhase::probing) set_phase(ContactPhase::probing);
        const auto timeout = transport_ == profile::Transport::stdio ? options_.discover_probe_timeout
                                                                     : options_.request_timeout;
        if (!internal(Purpose::probe, std::move(*message), timeout, speaker, refused)) {
            fail_contact(ContactFailureKind::transport, "the probe could not be written");
        }
    }

    void start_handshake(const Speaker& speaker, bool refused) {
        auto message = speaker.halves.open->handshake(*speaker.halves.profile, speaker.self);
        if (!message) {
            fail_contact(ContactFailureKind::transport, "the revision has no handshake");
            return;
        }
        handshake_answered_ = false;
        connection_stream_asked_ = false;  // a new handshake opens a new connection
        opening_ = &speaker;
        if (contact_.phase != ContactPhase::handshaking) set_phase(ContactPhase::handshaking);
        if (!internal(Purpose::handshake, std::move(*message), options_.request_timeout, speaker, refused)) {
            fail_contact(ContactFailureKind::transport, "the handshake could not be written");
        }
    }

    // The server is taken for one that opens with a handshake.
    void fallback_handshake() {
        const Speaker* named_handshake = newest(false);
        if (named_handshake == nullptr) {
            fail_contact(ContactFailureKind::no_common_revision,
                         "the server does not answer as one that serves revisions named per request, and no revision "
                         "that opens with a handshake is enabled");
            return;
        }
        start_handshake(*named_handshake, false);
    }

    void open_per_request(const Speaker& speaker) {
        governing_ = &speaker;
        opening_ = nullptr;
        contact_.profile = speaker.halves.profile;
        contact_.verdict = ContactVerdict{ServerKind::per_request, transport_, speaker.halves.profile->revision()};
        set_phase(ContactPhase::open);
        open_connection_stream();
        release_held();
    }

    // Whether the client needs the stream of the connection under the governing profile (EngineOptions::
    // connection_stream): something of its own arrives outside any request there. A change listener of the connection;
    // a log level it asked for (messages that belong to no request come there); a handler of a request the server
    // sends as a request of its own (one that belongs to no call of the client comes there).
    bool connection_needed() const {
        switch (options_.connection_stream) {
            case ConnectionStreamMode::never: return false;
            case ConnectionStreamMode::always: return true;
            case ConnectionStreamMode::when_needed: break;
        }
        if (connection_listening_ || options_.log_level) return true;
        if (governing_ == nullptr || governing_->halves.server_input->delivery() != InputDelivery::requests) {
            return false;
        }
        return !supported_kinds(options_.input).empty();
    }

    // Asks the transport for the stream of the connection, once per connection, while the client needs it and the
    // governing profile's entry for the transport offers one. Its headers are the ones a request outside any message
    // carries: the revision, where the entry names it in a header (no method, no mirrored member).
    void open_connection_stream() {
        if (connection_stream_asked_ || closed_) return;
        if (transport_ != profile::Transport::streamable_http || contact_.phase != ContactPhase::open ||
            governing_ == nullptr || !connection_needed()) {
            return;
        }
        const profile::ProfilePtr& governing = governing_->halves.profile;
        const auto& http = governing->streamable_http();
        if (!http || !http->get_stream) return;
        connection_stream_asked_ = true;
        std::vector<HttpHeader> headers;
        for (const auto& header : http->request_headers) {
            if (header.scope == profile::HeaderScopeKind::every_message &&
                header.value == profile::HeaderValue::protocol_version) {
                headers.push_back({header.name, governing->revision()});
            }
        }
        if (http->version_header && handshake_answered_) {
            headers.push_back({http->version_header->name, governing->revision()});
        }
        emit(Outgoing{std::string(), connection_unit, std::move(headers), OutgoingKind::open_connection_stream,
                      governing});
    }

    void fail_contact(ContactFailureKind kind, std::string message, std::vector<std::string> revisions = {},
                      std::optional<profile::Transport> next_transport = std::nullopt,
                      std::optional<AuthorizationFailure> authorization = std::nullopt) {
        if (contact_.phase == ContactPhase::failed || contact_.phase == ContactPhase::closed) return;
        contact_.failure =
            ContactFailure{kind, std::move(message), std::move(revisions), next_transport, std::move(authorization)};
        opening_ = nullptr;
        set_phase(ContactPhase::failed);
        std::vector<RequestRef> refs;
        for (const auto& [ref, call] : calls_) refs.push_back(ref);
        for (const RequestRef ref : refs) {
            end_call(ref, CallFailure{CallFailureKind::contact_failed, "contact with the server could not be made"});
        }
    }

    // 400, 404 and 405: the statuses after which a server is asked in the way of the step that follows.
    static bool refusing_status(int status) {
        return status == status_bad_request || status == status_not_found || status == status_method_not_allowed;
    }

    // An error only a server that names revisions per request sends: a code of the `errors` table of an enabled
    // profile of that kind, or on HTTP the code Method not found with the status such a profile gives an unknown
    // method.
    bool recognized_modern(const Message& message, std::optional<int> status) const {
        const auto code = code_of(message.outcome);
        if (!code) return false;
        if (per_request_codes_.count(*code) > 0) return true;
        if (transport_ != profile::Transport::streamable_http || !status || *code != error_code::method_not_found) {
            return false;
        }
        for (const auto& speaker : speakers_) {
            if (!speaker.per_request) continue;
            const auto& http = speaker.halves.profile->streamable_http();
            if (http && http->status_for(method_not_found_reason) == *status) return true;
        }
        return false;
    }

    // The handshake was refused with 400, 404 or 405 and a body that is no answer only a server that names revisions
    // per request sends: the step after it is the older transport of events over HTTP, where a profile defines it.
    void fail_handshake_by_status() {
        // The steps so far are named, so that the failure (and a failure of the step after it) names every answer.
        std::string met;
        for (const ContactStep& one : contact_.steps) {
            met += met.empty() ? " (" : "; ";
            met += one.method + ": " + one.outcome;
        }
        if (!met.empty()) met += ")";
        if (http_sse_enabled_) {
            fail_contact(ContactFailureKind::http_status,
                         "the server refused the handshake with a status after which it may serve the older "
                         "transport of events over HTTP" + met,
                         {}, profile::Transport::http_sse);
        } else {
            fail_contact(ContactFailureKind::http_status,
                         "the server refused the handshake with a status after which no enabled revision can be "
                         "tried" + met);
        }
    }

    void on_probe_answer(RequestRef ref, const Message& message, std::optional<int> status) {
        const Call& call = calls_.at(ref);
        const Speaker& speaker = *call.speaker;
        const bool refused = call.refused;
        const std::string method = call.message.method;
        const profile::Profile& profile = *speaker.halves.profile;
        finish(ref);
        if (!message.failed) {
            auto read = speaker.halves.open->read_discovery(message.outcome, profile);
            if (!read) {
                step(ref, method, "a result that is no discovery result");
                fallback_handshake();
                return;
            }
            if (auto* problem = std::get_if<std::string>(&*read)) {
                step(ref, method, "a discovery result that cannot be read");
                diagnose(DiagnosticKind::malformed_contact, "the discovery result cannot be read: " + *problem);
                fallback_handshake();
                return;
            }
            const Discovery& discovery = std::get<Discovery>(*read);
            step(ref, method, "a discovery result");
            contact_.revisions = discovery.versions;
            contact_.server = discovery.server;
            if (const Speaker* named_per_request = newest_in(discovery.versions, true)) {
                open_per_request(*named_per_request);
            } else if (const Speaker* named_handshake = newest_in(discovery.versions, false)) {
                start_handshake(*named_handshake, false);
            } else {
                fail_contact(ContactFailureKind::no_common_revision, "the server serves none of the enabled revisions",
                             discovery.versions);
            }
            return;
        }
        auto refusal = speaker.halves.open->read_version_refusal(message.outcome, profile);
        if (refusal) {
            if (auto* problem = std::get_if<std::string>(&*refusal)) {
                step(ref, method, "a refusal of the revision that cannot be read");
                fail_contact(ContactFailureKind::malformed_answer,
                             "the refusal of the revision cannot be read: " + *problem);
                return;
            }
            const VersionRefusal& versions = std::get<VersionRefusal>(*refusal);
            step(ref, method, "a refusal of the revision");
            const Speaker* again = newest_in(versions.supported, true);
            if (again == nullptr || refused) {
                fail_contact(ContactFailureKind::no_common_revision,
                             refused ? "the server refused the revision twice"
                                     : "the server serves no revision named per request that is enabled",
                             versions.supported);
                return;
            }
            start_probe(*again, true);
            return;
        }
        if (recognized_modern(message, status)) {
            // A server that names its revisions per request, which refused something else than the revision.
            step(ref, method, "an error only a server that names revisions per request sends");
            note_defect(*code_of(message.outcome), profile);
            open_per_request(speaker);
            return;
        }
        if (status && !is_success(*status) && !refusing_status(*status)) {
            // Another error with a status after which no step of opening goes on (a server error, another 4xx).
            step(ref, method, "an error with a status after which no step goes on");
            fail_contact(ContactFailureKind::http_status,
                         "the probe was answered with an error and a status after which no step of opening goes on");
            return;
        }
        step(ref, method, "an error");
        fallback_handshake();
    }

    void on_handshake_answer(RequestRef ref, const Message& message, std::optional<int> status) {
        const Call& call = calls_.at(ref);
        const Speaker& speaker = *call.speaker;
        const bool refused = call.refused;
        const std::string method = call.message.method;
        finish(ref);
        if (!message.failed) {
            auto read = speaker.halves.open->read_handshake(message.outcome, handshake_profiles_);
            if (auto* accepted = std::get_if<HandshakeAccepted>(&read)) {
                const Speaker* chosen = speaker_of(accepted->profile);
                step(ref, method, "accepted");
                governing_ = chosen;
                opening_ = nullptr;
                handshake_answered_ = true;
                contact_.profile = accepted->profile;
                contact_.server = accepted->server;
                contact_.verdict = ContactVerdict{ServerKind::handshake, transport_, accepted->profile->revision()};
                const json capabilities = accepted->server.capabilities;
                if (auto ready = chosen->halves.open->opened(*accepted->profile)) send_notification(*ready, *chosen);
                if (options_.log_level) {
                    if (auto level = chosen->halves.open->level_request(*options_.log_level, *accepted->profile,
                                                                        capabilities)) {
                        internal(Purpose::level, std::move(*level), options_.request_timeout, *chosen, false);
                    }
                }
                set_phase(ContactPhase::open);
                // After the notification that the client is ready: the server speaks on the connection from then on.
                open_connection_stream();
                release_held();
                return;
            }
            if (auto* unserved = std::get_if<UnservedRevision>(&read)) {
                step(ref, method, "a revision no candidate is");
                fail_contact(ContactFailureKind::unserved_revision,
                             "the handshake's answer names a revision this client does not speak",
                             {unserved->revision});
            } else {
                step(ref, method, "a result that cannot be read");
                fail_contact(ContactFailureKind::malformed_answer,
                             "the handshake's result cannot be read: " + std::get<std::string>(read));
            }
            emit(Outgoing{std::string(), connection_unit, {}, OutgoingKind::close, nullptr});
            return;
        }
        auto refusal = speaker.halves.open->read_version_refusal(message.outcome, *speaker.halves.profile);
        if (refusal && std::holds_alternative<VersionRefusal>(*refusal)) {
            const VersionRefusal& versions = std::get<VersionRefusal>(*refusal);
            step(ref, method, "a refusal of the revision");
            if (const Speaker* again = refused ? nullptr : newest_in(versions.supported, false)) {
                start_handshake(*again, true);
                return;
            }
            fail_contact(ContactFailureKind::handshake_refused, "the server refused the handshake's revision",
                         versions.supported);
            emit(Outgoing{std::string(), connection_unit, {}, OutgoingKind::close, nullptr});
            return;
        }
        if (recognized_modern(message, status) && !reprobed_ && newest(true) != nullptr) {
            // A server that names revisions per request answered: find out once more which revisions it serves.
            step(ref, method, "an error only a server that names revisions per request sends");
            reprobed_ = true;
            start_probe(*newest(true), false);
            return;
        }
        if (status && refusing_status(*status)) {
            step(ref, method, "an error with a status after which the next step is another transport");
            fail_handshake_by_status();
            emit(Outgoing{std::string(), connection_unit, {}, OutgoingKind::close, nullptr});
            return;
        }
        step(ref, method, "an error");
        fail_contact(ContactFailureKind::handshake_refused, "the server refused the handshake");
        emit(Outgoing{std::string(), connection_unit, {}, OutgoingKind::close, nullptr});
    }

    // A server that refuses headers this client wrote as the revision says has a defect worth reporting.
    void note_defect(int code, const profile::Profile& profile) {
        if (profile.error_code(header_mismatch_error) == code) {
            diagnose(DiagnosticKind::server_defect, "the server refused the headers of a request this client wrote");
        }
    }

    // ---- what the server sends

    void on_response(UnitId unit, const Message& message) {
        const auto number = wire_number(message.id);
        if (!number) {
            diagnose(DiagnosticKind::unknown_id, "an answer whose id is none this client gives was dropped", unit);
            return;
        }
        const auto found = wires_.find(*number);
        if (found == wires_.end()) {
            // The result that ends a subscription the server ended with a notice, or the client cancelled.
            if (ended_set_.erase(*number) > 0) return;
            // A request the client gave up on was answered with a task after all: nobody follows it, so it is
            // cancelled.
            if (const auto abandoned = abandoned_map_.find(*number); abandoned != abandoned_map_.end()) {
                const Abandoned given_up = std::move(abandoned->second);
                abandoned_map_.erase(abandoned);
                cancel_late_task(message, given_up);
                return;
            }
            if (answered_set_.count(*number) > 0) {
                diagnose(DiagnosticKind::repeated_answer, "a second answer to a request was dropped", unit);
            } else {
                diagnose(DiagnosticKind::unknown_id, "an answer to no pending request was dropped", unit);
            }
            return;
        }
        if (found->second.unit != unit) {
            diagnose(DiagnosticKind::wrong_unit, "an answer on another exchange than its request's was dropped", unit);
            return;
        }
        const RequestRef ref = found->second.ref;
        wires_.erase(found);
        remember_answered(*number);
        Call& call = calls_.at(ref);
        // Only an answer to a call of the application shows that the server serves: the answers to the engine's own
        // requests (the probe, the handshake, the log level) come before any call can fail again.
        if (call.purpose == Purpose::call) exits_in_row_ = renewals_in_row_ = 0;
        call.wire.reset();
        // The status of the exchange that carried the answer, read before the exchange ends: opening contact decides
        // from both.
        std::optional<int> status;
        if (const auto exchange = exchanges_.find(unit); unit != connection_unit && exchange != exchanges_.end()) {
            status = exchange->second.status;
        }
        if (transport_ == profile::Transport::streamable_http) end_exchange(unit);
        switch (call.purpose) {
            case Purpose::probe: on_probe_answer(ref, message, status); return;
            case Purpose::handshake: on_handshake_answer(ref, message, status); return;
            case Purpose::level:
                if (message.failed) diagnose(DiagnosticKind::level_refused, "the server refused the log level");
                finish(ref);
                return;
            case Purpose::task_cancel: finish(ref); return;
            case Purpose::call: on_call_answer(ref, message, *number); return;
        }
    }

    void remember_answered(std::uint64_t id) {
        answered_.push_back(id);
        answered_set_.insert(id);
        while (answered_.size() > options_.max_pending_requests) {
            answered_set_.erase(answered_.front());
            answered_.pop_front();
        }
    }

    // The id of a subscription that ended without its result: what the server still sends for it is dropped
    // silently. As many are kept as calls may be pending.
    void remember_ended(std::uint64_t id) {
        ended_.push_back(id);
        ended_set_.insert(id);
        while (ended_.size() > options_.max_pending_requests) {
            ended_set_.erase(ended_.front());
            ended_.pop_front();
        }
    }

    // A request on the wire whose answer could create a task the call would follow (a call of the application that
    // follows no task yet, under a speaker whose tasks the client takes): an answer that comes after the call gave it
    // up is read for a task to cancel. As many are kept as calls may be pending.
    void remember_abandoned(const Call& call) {
        if (!call.wire || call.task || call.subscription || call.speaker == nullptr) return;
        const ClientTaskMechanism* tasks = tasks_of(*call.speaker);
        if (tasks == nullptr) return;
        if (tasks->augments_tool_call() && !tasks->is_augmented(call.message)) return;
        abandoned_.push_back(*call.wire);
        abandoned_map_[*call.wire] = Abandoned{call.speaker, call.message.method};
        while (abandoned_.size() > options_.max_pending_requests) {
            abandoned_map_.erase(abandoned_.front());
            abandoned_.pop_front();
        }
    }

    // The late answer of a request the client gave up on: when it created a task, the task is cancelled.
    void cancel_late_task(const Message& message, const Abandoned& given_up) {
        if (message.failed || closed_ || contact_.phase != ContactPhase::open) return;
        const ClientTaskMechanism* half = tasks_of(*given_up.speaker);
        if (half == nullptr) return;
        auto created = half->read_created(message.outcome, given_up.method, *given_up.speaker->halves.profile);
        if (!created || std::holds_alternative<std::string>(*created)) return;
        const TaskState& state = std::get<TaskState>(*created);
        if (!task_id_fits(state.id)) return;
        internal(Purpose::task_cancel, half->cancel(state.id), options_.request_timeout, *given_up.speaker, false);
    }

    void on_call_answer(RequestRef ref, const Message& message, std::uint64_t wire) {
        Call& call = calls_.at(ref);
        const Speaker& speaker = *call.speaker;
        const profile::Profile& profile = *speaker.halves.profile;
        if (call.subscription && !message.failed) {
            // The result that ends a subscription, as the feed half reads it.
            if (auto problem = speaker.halves.change_feed->read_end(message.outcome, RequestId(wire), profile)) {
                end_call(ref, CallFailure{CallFailureKind::malformed_answer,
                                          "the result that ends the subscription cannot be read: " + *problem});
                return;
            }
            end_call(ref, Answered{message.outcome});
            return;
        }
        if (call.task) {
            on_task_answer(ref, message);
            return;
        }
        if (message.failed) {
            if (speaker.per_request) {
                if (auto refusal = speaker.halves.open->read_version_refusal(message.outcome, profile)) {
                    if (auto* problem = std::get_if<std::string>(&*refusal)) {
                        end_call(ref, CallFailure{CallFailureKind::malformed_answer,
                                                  "the refusal of the revision cannot be read: " + *problem});
                        return;
                    }
                    const VersionRefusal& versions = std::get<VersionRefusal>(*refusal);
                    if (call.refused) {
                        end_call(ref, CallFailure{CallFailureKind::version_refused,
                                                  "the server refused the revision of the call twice"});
                        return;
                    }
                    // A refusal of the revision never leads to a handshake: only revisions named per request.
                    const Speaker* again = newest_in(versions.supported, true);
                    if (again == nullptr) {
                        end_call(ref, CallFailure{CallFailureKind::no_common_revision,
                                                  "the server serves no revision named per request that is enabled"});
                        return;
                    }
                    call.refused = true;
                    if (governing_ != nullptr && governing_->per_request && again != governing_) {
                        governing_ = again;
                        contact_.profile = again->halves.profile;
                        contact_.verdict =
                            ContactVerdict{ServerKind::per_request, transport_, again->halves.profile->revision()};
                    }
                    if (auto failure = build_and_send(call, *again)) end_call(ref, *failure);
                    return;
                }
            }
            auto error = read_error(message.outcome);
            if (!error) {
                end_call(ref, CallFailure{CallFailureKind::malformed_answer, "the error answer cannot be read"});
                return;
            }
            note_defect(error->code, profile);
            end_call(ref, ErrorAnswered{std::move(*error)});
            return;
        }
        if (const ClientTaskMechanism* tasks = tasks_of(speaker)) {
            if (auto created = tasks->read_created(message.outcome, call.message.method, profile)) {
                if (tasks->augments_tool_call() && !tasks->is_augmented(call.message)) {
                    end_call(ref, CallFailure{CallFailureKind::malformed_answer,
                                              "the server answered an unaugmented call with a task"});
                    return;
                }
                if (auto* problem = std::get_if<std::string>(&*created)) {
                    end_call(ref, CallFailure{CallFailureKind::malformed_answer,
                                              "the result that creates a task cannot be read: " + *problem});
                    return;
                }
                TaskState state = std::get<TaskState>(std::move(*created));
                if (!task_id_fits(state.id)) {
                    end_call(ref, CallFailure{CallFailureKind::malformed_answer,
                                              "the result creates a task whose id is empty or longer than the client "
                                              "takes"});
                    return;
                }
                if (call.hand_over_task) {
                    TaskWork work;
                    work.id = state.id;
                    call.task = std::move(work);
                    end_call(ref, Answered{message.outcome, std::move(state)});
                    return;
                }
                start_following(call, std::move(state));
                return;
            }
        }
        const ClientInputMechanism& input = *speaker.halves.server_input;
        if (input.delivery() == InputDelivery::result) {
            auto read = input.read_result(message.outcome, call.message.method, profile);
            if (auto* problem = std::get_if<std::string>(&read)) {
                end_call(ref, CallFailure{CallFailureKind::malformed_answer, *problem});
                return;
            }
            auto& asking = std::get<std::optional<InputAsking>>(read);
            if (asking) {
                open_round(call, std::move(*asking));
                return;
            }
        }
        end_call(ref, Answered{message.outcome});
    }

    void on_server_request(UnitId unit, const Message& message) {
        const Speaker& speaker = speaker_for(unit);
        const profile::Profile& profile = *speaker.halves.profile;
        const ClientInputMechanism& input = *speaker.halves.server_input;
        if (input.delivery() != InputDelivery::requests) {
            diagnose(DiagnosticKind::malformed_message, "a request of the server, which the revision does not allow, "
                                                        "was dropped",
                     unit);
            return;
        }
        auto read = input.read_request(message, profile);
        if (std::holds_alternative<ServerPing>(read)) {
            send_response(input.ping_answer(*message.id), speaker);
            return;
        }
        if (auto* error = std::get_if<RequestError>(&read)) {
            send_response(make_error(message.id, error->code, error->message, error->data), speaker);
            return;
        }
        auto& asked = std::get<ServerAsk>(read);
        if (!declared(asked.request, speaker)) {
            const RequestError refusal = input.refusal(asked.request, speaker.self.capabilities, profile);
            send_response(make_error(message.id, refusal.code, refusal.message, refusal.data), speaker);
            return;
        }
        // The call the request belongs to, where the wire says so (it came on that call's exchange).
        std::optional<RequestRef> tied;
        if (unit != connection_unit) {
            for (const auto& [id, entry] : wires_) {
                if (entry.unit == unit) tied = entry.ref;
            }
        }
        if (asks_.size() >= options_.max_concurrent_server_requests) {
            send_response(make_error(message.id, error_code::internal_error, std::string(busy_message)), speaker);
            return;
        }
        std::optional<json_schema::Validator> form;
        if (const auto* asked_form = std::get_if<ElicitationForm>(&asked.request)) {
            auto checked = check_form(*asked_form, speaker);
            if (std::holds_alternative<std::string>(checked)) {
                diagnose(DiagnosticKind::form_not_restricted,
                         "a server's form was refused: " + std::get<std::string>(checked), unit);
                send_response(make_error(message.id, error_code::invalid_params, std::string(unrestricted_form_message)),
                              speaker);
                return;
            }
            form = std::get<json_schema::Validator>(std::move(checked));
        }
        std::vector<RequestRef> paused;
        if (tied) {
            Call& call = calls_.at(*tied);
            if (call.round >= options_.max_input_rounds) {
                // Past the bound the call's clock no longer waits for the client's answers.
                send_response(make_error(message.id, error_code::internal_error, std::string(busy_message)), speaker);
                return;
            }
            ++call.round;
            paused.push_back(*tied);
        } else {
            // The wire does not say which call it belongs to: every call that waits on the server waits for it.
            for (const auto& [id, entry] : wires_) {
                const auto found = calls_.find(entry.ref);
                if (found != calls_.end() && found->second.purpose == Purpose::call) paused.push_back(entry.ref);
            }
        }
        const InputRef ref{next_input_++};
        Ask ask{ref, *message.id, asked.request, &speaker, paused, Clock{}, std::move(form)};
        auto& stored = asks_.emplace(ref, std::move(ask)).first->second;
        for (const RequestRef one : paused) {
            Call& call = calls_.at(one);
            ++call.pausers;
            pause_call(call);
        }
        clock_start(ref, DeadlineKind::input, stored.input, options_.input_handler_timeout);
        push(ServerAsked{ref, std::move(asked.method), std::move(asked.request), tied, std::move(paused),
                         speaker.halves.profile},
             false);
    }

    // The response that answers a server's request with `reply`.
    json response_of(const Ask& ask, const InputReply& reply) {
        if (const auto* error = std::get_if<RequestError>(&reply)) {
            return make_error(ask.server_id, error->code, error->message, error->data);
        }
        auto written = ask.speaker->halves.server_input->answer(ask.server_id, std::get<InputResponse>(reply),
                                                                *ask.speaker->halves.profile);
        if (auto* problem = std::get_if<std::string>(&written)) {
            diagnose(DiagnosticKind::malformed_message, "an answer to the server cannot be written: " + *problem);
            return make_error(ask.server_id, error_code::internal_error, std::string(unwritable_message));
        }
        return std::get<json>(std::move(written));
    }

    // Ends a server's request: its clock stops, the calls it paused go on, and the response goes out when there is one.
    void finish_ask(InputRef ref, std::optional<json> response, bool announce, bool by_server) {
        const auto found = asks_.find(ref);
        if (found == asks_.end()) return;
        Ask ask = std::move(found->second);
        asks_.erase(found);
        clock_stop(ref, DeadlineKind::input, ask.input);
        for (const RequestRef one : ask.paused) {
            const auto call = calls_.find(one);
            if (call == calls_.end()) continue;
            if (call->second.pausers > 0) --call->second.pausers;
            resume_call(call->second);
        }
        if (announce) push(ServerAskEnded{ref, by_server}, false);
        if (response) send_response(*response, *ask.speaker);
    }

    void on_notification(UnitId unit, const Message& message) {
        const Speaker& speaker = speaker_for(unit);
        const profile::Profile& profile = *speaker.halves.profile;
        const profile::Message* entry = profile.notification(message.method);
        const bool from_server = entry != nullptr && entry->from_server;
        if (from_server && message.method == progress_method) {
            on_progress(unit, message);
            return;
        }
        if (from_server && message.method == log_method) {
            on_log(unit, message);
            return;
        }
        if (auto feed = speaker.halves.change_feed->read(message, profile)) {
            if (auto* problem = std::get_if<std::string>(&*feed)) {
                diagnose(DiagnosticKind::malformed_message, "a change notification is malformed: " + *problem, unit);
                return;
            }
            FeedEvent& event = std::get<FeedEvent>(*feed);
            std::optional<RequestRef> subscription;
            if (event.subscription) {
                const auto number = wire_number(event.subscription);
                if (number) {
                    const auto opened = wires_.find(*number);
                    if (opened != wires_.end() && calls_.at(opened->second.ref).subscription) {
                        subscription = opened->second.ref;
                    }
                }
                if (!subscription) {
                    // An ending notice may cancel a request of the server's instead.
                    if (event.kind == FeedEvent::Kind::ended && cancel_ask(message, speaker)) return;
                    // A message the server sent before it heard that the subscription ended.
                    if (number && ended_set_.count(*number) > 0) return;
                    diagnose(DiagnosticKind::unknown_id, "a notification names no subscription this client opened",
                             unit);
                    return;
                }
                on_subscription_message(*subscription, std::move(event));
                return;
            }
            // A change of the connection: kept while the client listens there, else droppable as before.
            if (connection_listening_) exits_in_row_ = renewals_in_row_ = 0;
            push(ChangeEvent{std::move(event), std::nullopt}, !connection_listening_);
            return;
        }
        cancel_ask(message, speaker);
    }

    // A message of a subscription the client opened. Each is kept in the queue: the acknowledgement stops the call's
    // clock (from then on it has none), a change goes to the client, and an ending notice ends the call gracefully.
    void on_subscription_message(RequestRef ref, FeedEvent event) {
        Call& call = calls_.at(ref);
        exits_in_row_ = renewals_in_row_ = 0;
        const FeedEvent::Kind kind = event.kind;
        push(ChangeEvent{std::move(event), ref}, false);
        switch (kind) {
            case FeedEvent::Kind::acknowledged:
                clock_stop(ref, DeadlineKind::wait, call.wait);
                clock_stop(ref, DeadlineKind::total, call.total);
                return;
            case FeedEvent::Kind::change: return;
            case FeedEvent::Kind::ended: {
                const std::uint64_t id = *call.wire;
                wires_.erase(id);
                call.wire.reset();
                remember_ended(id);
                end_exchange(call.sent_unit);
                end_call(ref, Answered{nullptr});
                return;
            }
        }
    }

    // A cancellation of a request of the server's: its handler stops and nothing is answered. Returns whether the
    // notification was one.
    bool cancel_ask(const Message& message, const Speaker& speaker) {
        const auto id = speaker.halves.cancel->cancelled_request(message);
        if (!id) return false;
        for (const auto& [ref, ask] : asks_) {
            if (ask.server_id == *id) {
                finish_ask(ref, std::nullopt, true, true);
                break;
            }
        }
        return true;
    }

    void on_progress(UnitId unit, const Message& message) {
        const json* token = member(message.params, progress_token_key);
        const json* value = member(message.params, progress_member);
        const json* total = member(message.params, total_member);
        const json* text = member(message.params, message_member);
        if (token == nullptr || value == nullptr || !value->is_number() || (total != nullptr && !total->is_number()) ||
            (text != nullptr && !text->is_string())) {
            diagnose(DiagnosticKind::malformed_message, "a progress notification is malformed", unit);
            return;
        }
        const auto number = wire_number(RequestId::from_json(*token));
        const auto found = number ? calls_.find(RequestRef{*number}) : calls_.end();
        const bool accepts_progress =
            found != calls_.end() && (found->second.progress || (found->second.task && found->second.task->accepts_progress));
        if (found == calls_.end() || found->second.purpose != Purpose::call || !accepts_progress) {
            diagnose(DiagnosticKind::unknown_id, "a progress notification names no call that asked for progress", unit);
            return;
        }
        Call& call = found->second;
        if (call.reset_on_progress && call.wait.exists && call.wait.running) {
            clock_start(call.ref, DeadlineKind::wait, call.wait, call.timeout);
        }
        ProgressEvent event;
        event.request = call.ref;
        event.progress = value->get<double>();
        if (total != nullptr) event.total = total->get<double>();
        if (text != nullptr) event.message = text->get<std::string>();
        push(std::move(event), true);
    }

    void on_log(UnitId unit, const Message& message) {
        const json* level = member(message.params, level_member);
        const json* data = member(message.params, data_member);
        const json* logger = member(message.params, logger_member);
        const auto parsed = level != nullptr && level->is_string() ? parse_log_level(level->get<std::string>())
                                                                    : std::nullopt;
        if (!parsed || data == nullptr || (logger != nullptr && !logger->is_string())) {
            diagnose(DiagnosticKind::malformed_message, "a log message is malformed", unit);
            return;
        }
        LogEvent event;
        event.level = *parsed;
        event.data = *data;
        if (logger != nullptr) event.logger = logger->get<std::string>();
        if (unit != connection_unit) {
            for (const auto& [id, entry] : wires_) {
                if (entry.unit == unit) event.request = entry.ref;
            }
        }
        push(std::move(event), true);
    }

    // ---- stream events

    void on_stream(UnitId unit, const ResponseHead& head) {
        const auto found = exchanges_.find(unit);
        if (found == exchanges_.end()) return;
        found->second.status = head.status;
        found->second.media = head.media_type;
        if (head.status == status_unauthorized || head.status == status_forbidden) {
            lose_unit(unit, CallFailure{CallFailureKind::authorization_required, "the server asks for authorization"},
                      head.status);
            return;
        }
        if (is_success(head.status) && head.media_type && *head.media_type != json_media &&
            *head.media_type != event_stream_media) {
            lose_unit(unit, CallFailure{CallFailureKind::bad_content_type, "the answer is of a type the protocol does "
                                                                           "not use"},
                      head.status);
        }
    }
    void on_stream(UnitId, const StreamEventId&) {}
    void on_stream(UnitId, const StreamRetryAdvice&) {}
    void on_stream(UnitId unit, const StreamEnded&) {
        if (unit == connection_unit) {
            if (connection_is_stream()) connection_ended();
            return;
        }
        const auto found = exchanges_.find(unit);
        if (found == exchanges_.end()) return;
        const std::optional<int> status = found->second.status;
        exchanges_.erase(found);
        const bool refused = status && !is_success(*status);
        // An exchange that carried only its request was taken; its answer comes on the connection.
        if (!refused && answers_on_connection()) return;
        lose_unit(unit,
                  CallFailure{refused ? CallFailureKind::http_status : CallFailureKind::stream_lost,
                              refused ? "the exchange was answered with a status that carries no answer"
                                      : "the exchange ended before its answer"},
                  status);
    }
    void on_stream(UnitId unit, const StreamLost&) {
        if (unit != connection_unit) exchanges_.erase(unit);
        if (unit == connection_unit && connection_is_stream()) {
            connection_ended();
            return;
        }
        if (unit == connection_unit && transport_ != profile::Transport::stdio) return;
        lose_unit(unit, CallFailure{CallFailureKind::stream_lost, "the exchange broke before its answer"});
    }
    void on_stream(UnitId unit, const SendFailed& failed) {
        std::vector<RequestRef> refs;
        for (const auto& [ref, call] : calls_) {
            if (call.wire && call.sent_unit == unit) refs.push_back(ref);
        }
        if (unit != connection_unit) exchanges_.erase(unit);
        for (const RequestRef ref : refs) {
            lose(ref, CallFailure{CallFailureKind::send_failed, "the transport could not deliver it: " + failed.cause},
                 std::nullopt);
        }
    }
    // The server no longer knew the session the unit's request carried. It did not take the request: a call goes out
    // once more with a new id, under a new session, and fails when that one is not known either. The engine's own
    // requests are not sent again (the handshake that opens the new session asks for the log level again). When the
    // session in use ended, a new handshake opens another, once for every exchange that met the end, at most
    // max_restarts times in a row without an answer between (then contact fails); the calls wait in the engine
    // meanwhile, and what the server asked under the old session is ended.
    void on_stream(UnitId unit, const SessionExpired& expired) {
        if (unit != connection_unit) exchanges_.erase(unit);
        std::vector<RequestRef> refs;
        for (const auto& [id, entry] : wires_) {
            if (unit != connection_unit && entry.unit == unit) refs.push_back(entry.ref);
        }
        for (const RequestRef ref : refs) {
            const auto found = calls_.find(ref);
            if (found == calls_.end()) continue;
            Call& call = found->second;
            if (call.purpose != Purpose::call || call.session_resent) {
                lose(ref,
                     CallFailure{CallFailureKind::http_status,
                                 "the server did not know the session of the request, nor that of its second sending"},
                     std::nullopt);
                continue;
            }
            call.session_resent = true;
            wires_.erase(*call.wire);
            call.wire.reset();
            call.held = true;
            held_.push_back(ref);
        }
        const bool renew = expired.current && contact_.phase == ContactPhase::open && governing_ != nullptr &&
                           governing_->handshake && contact_.verdict && contact_.verdict->kind == ServerKind::handshake;
        if (renew) {
            // A server that forgets every session it opens is not given new ones forever.
            if (renewals_in_row_ >= options_.max_restarts) {
                fail_contact(ContactFailureKind::too_many_restarts,
                             "the server forgot the session more often in a row than the client opens a new one");
                return;
            }
            ++renewals_in_row_;
            diagnose(DiagnosticKind::session_renewed,
                     "the server no longer knew the session; a new handshake opens another", unit);
            end_asks();
            start_handshake(*governing_, false);
            return;
        }
        if (contact_.phase == ContactPhase::open) release_held();
    }

    // The calls a unit's exchange carries: those whose answer counts on it, and where answers come on the connection,
    // those it carried out.
    std::vector<RequestRef> calls_on(UnitId unit) const {
        std::vector<RequestRef> refs;
        if (unit == connection_unit) return refs;
        for (const auto& [id, entry] : wires_) {
            if (entry.unit == unit) {
                refs.push_back(entry.ref);
                continue;
            }
            if (!answers_on_connection()) continue;
            const auto call = calls_.find(entry.ref);
            if (call != calls_.end() && call->second.sent_unit == unit) refs.push_back(entry.ref);
        }
        return refs;
    }

    // The transport holds the unit's request while its authorizer works, or sent it again: the clocks of every call
    // it carries (the probe and the handshake among them) wait meanwhile, as for an input handler.
    void on_stream(UnitId unit, const AuthorizationHold& hold) {
        for (const RequestRef ref : calls_on(unit)) {
            const auto found = calls_.find(ref);
            if (found == calls_.end()) continue;
            Call& call = found->second;
            call.authorizing = hold.begin;
            if (hold.begin) {
                pause_call(call);
            } else {
                resume_call(call);
            }
        }
    }

    // The authorizer gave up on the unit's request: its calls end with the authorizer's failure, and the probe or the
    // handshake fail contact so.
    void on_stream(UnitId unit, const AuthorizationDenied& denied) {
        if (unit == connection_unit) return;
        const auto found = exchanges_.find(unit);
        if (found != exchanges_.end()) found->second.status = denied.status;
        for (const RequestRef ref : calls_on(unit)) {
            const auto call = calls_.find(ref);
            if (call != calls_.end()) call->second.authorizing = false;
        }
        lose_unit(unit, denied.failure, denied.status);
        end_exchange(unit);
    }

    void on_stream(UnitId unit, const ProcessExited&) {
        if (transport_ != profile::Transport::stdio || unit != connection_unit) return;
        connection_ended();
    }

    // The connection ended as a whole: the server process (stdio), or the event stream of the older transport of
    // events over HTTP. Every call that waited on it loses its answer; contact is opened again for the next request
    // (restart), at most max_restarts times in a row without an answer between.
    void connection_ended() {
        const CallFailure loss = connection_loss();
        const ContactPhase phase = contact_.phase;
        if (phase == ContactPhase::probing) {
            for (const auto& [ref, call] : calls_) {
                if (call.purpose == Purpose::probe) {
                    step(ref, call.message.method,
                         connection_is_stream() ? "the event stream ended first" : "the process ended first");
                }
            }
            restart_for_probe_ = true;
            if (!probe_restart_used_) {
                // A server that opens with a handshake may end on a request it does not know: the handshake goes to
                // a fresh process, a start that belongs to finding out which kind of server it is. Once only: a
                // process that ends while the probe is out again counts as any other end.
                probe_restart_used_ = true;
                lose_all_on_wire(loss.kind, loss.message);
                reopening_ = true;
                restarting_ = false;
                set_phase(ContactPhase::lost);
                request_restart();
                return;
            }
        }
        lose_all_on_wire(loss.kind, loss.message);
        end_asks();
        switch (phase) {
            case ContactPhase::open:
            case ContactPhase::probing:
            case ContactPhase::handshaking:
            case ContactPhase::lost: break;
            default: return;
        }
        ++exits_in_row_;
        restarting_ = false;
        if (exits_in_row_ > options_.max_restarts) {
            fail_contact(ContactFailureKind::too_many_restarts,
                         connection_is_stream()
                             ? "the event stream of the connection ended more often in a row than the client opens it "
                               "again"
                             : "the server process ended more often in a row than the client starts it again");
            return;
        }
        // Contact that was being opened is still wanted, with or without a call that waits for it; a held call needs the
        // server, and so does a client that listens to the connection's notifications.
        if (phase == ContactPhase::probing || phase == ContactPhase::handshaking) reopening_ = true;
        if (phase != ContactPhase::lost) set_phase(ContactPhase::lost);
        if (reopening_ || !held_.empty() || connection_listening_) request_restart();
    }

    EngineOptions options_;
    profile::Transport transport_;
    OutgoingSink sink_;
    std::vector<Speaker> speakers_;  // newest revision first
    std::vector<profile::ProfilePtr> profiles_;
    std::vector<profile::ProfilePtr> handshake_profiles_;
    std::set<int> per_request_codes_;

    ContactState contact_;
    const Speaker* governing_ = nullptr;
    const Speaker* opening_ = nullptr;
    bool handshake_answered_ = false;
    bool reprobed_ = false;
    bool http_sse_enabled_ = false;  // an enabled profile defines the older transport of events over HTTP
    bool restarting_ = false;
    bool restart_for_probe_ = false;
    bool probe_restart_used_ = false;  // the one start after an end while the probe was out that does not count
    bool reopening_ = false;           // the process ended while contact was being opened: a new one is wanted
    std::size_t exits_in_row_ = 0;
    std::size_t renewals_in_row_ = 0;  // HTTP: sessions renewed in a row without an answer between
    bool connection_listening_ = false;    // the client takes the connection's change notifications
    bool connection_stream_asked_ = false; // open_connection_stream was written for this connection

    std::uint64_t next_id_ = 1;
    std::uint64_t next_ref_ = 1;
    std::uint64_t next_input_ = 1;
    std::uint64_t next_epoch_ = 1;
    UnitId next_unit_ = connection_unit + 1;

    std::map<std::uint64_t, WireEntry> wires_;
    std::map<RequestRef, Call> calls_;
    std::deque<RequestRef> held_;
    std::map<InputRef, Ask> asks_;
    std::map<UnitId, Exchange> exchanges_;
    std::deque<std::uint64_t> answered_;
    std::set<std::uint64_t> answered_set_;
    std::deque<std::uint64_t> ended_;     // subscriptions that ended without their result (remember_ended)
    std::set<std::uint64_t> ended_set_;
    // Requests the client gave up on while their answer could still create a task (remember_abandoned), with the
    // speaker each went out under.
    std::deque<std::uint64_t> abandoned_;
    std::map<std::uint64_t, Abandoned> abandoned_map_;
    std::size_t application_calls_ = 0;
    std::size_t peer_compiles_ = 0;     // schemas of the server compiled or refused (compile_peer_schema)
    bool budget_reported_ = false;

    std::deque<EngineEvent> events_;
    std::uint64_t dropped_ = 0;
    bool in_sink_ = false;
    bool closed_ = false;
    bool dead_ = false;
};

}  // namespace detail

std::variant<std::unique_ptr<ClientEngine>, ClientMechanismsError> ClientEngine::create(EngineOptions options,
                                                                                      profile::Transport transport,
                                                                                      OutgoingSink sink) {
    ClientMechanismsError error;
    const auto problem = [&](std::string line) {
        if (!error.message.empty()) error.message += '\n';
        error.message += line;
    };
    std::vector<profile::ProfilePtr> profiles = options.profiles;
    if (profiles.empty()) {
        for (const auto revision : profile::builtin_revisions()) profiles.push_back(profile::builtin(revision));
    }
    if (std::find(profiles.begin(), profiles.end(), nullptr) != profiles.end()) {
        problem("the list of profiles holds a null profile");
        return error;
    }
    // Newest first, as the server's session orders them.
    std::sort(profiles.begin(), profiles.end(),
              [](const profile::ProfilePtr& a, const profile::ProfilePtr& b) { return a->revision() > b->revision(); });
    for (std::size_t i = 1; i < profiles.size(); ++i) {
        if (profiles[i]->revision() == profiles[i - 1]->revision()) {
            problem("revision " + profiles[i]->revision() + " is enabled twice");
        }
    }
    if (!sink) problem("the sink is empty");
    if (options.request_timeout.count() <= 0) problem("request_timeout is not positive");
    if (options.max_total_timeout.count() <= 0) problem("max_total_timeout is not positive");
    if (options.discover_probe_timeout.count() <= 0) problem("discover_probe_timeout is not positive");
    if (options.input_handler_timeout.count() <= 0) problem("input_handler_timeout is not positive");
    if (options.max_pending_requests == 0) problem("max_pending_requests is zero");
    if (options.max_queued_events == 0) problem("max_queued_events is zero");
    if (options.max_message_bytes == 0) problem("max_message_bytes is zero");
    if (options.max_nesting_depth == 0) problem("max_nesting_depth is zero");
    if (options.max_input_rounds == 0) problem("max_input_rounds is zero");
    if (options.max_concurrent_server_requests == 0) problem("max_concurrent_server_requests is zero");
    if (options.max_input_requests_per_round == 0) problem("max_input_requests_per_round is zero");
    if (options.peer_schema_depth == 0) problem("peer_schema_depth is zero");
    if (options.peer_pattern_length == 0) problem("peer_pattern_length is zero");
    if (options.peer_pattern_memory == 0) problem("peer_pattern_memory is zero");
    if (options.peer_max_evaluations == 0) problem("peer_max_evaluations is zero");
    if (options.peer_patterns_per_schema == 0) problem("peer_patterns_per_schema is zero");
    if (options.max_peer_schema_compiles == 0) problem("max_peer_schema_compiles is zero");
    if (options.tasks.min_poll_interval.count() <= 0 || options.tasks.max_poll_interval.count() <= 0) {
        problem("the waits between the reads of a task are not positive");
    } else if (options.tasks.min_poll_interval > options.tasks.max_poll_interval) {
        problem("tasks.min_poll_interval exceeds tasks.max_poll_interval");
    }
    if (options.tasks.max_wait.count() <= 0) problem("tasks.max_wait is not positive");
    if (options.tasks.max_polls == 0) problem("tasks.max_polls is zero");
    if (options.tasks.max_id_bytes == 0) problem("tasks.max_id_bytes is zero");
    // A client that takes tasks speaks each revision with the built-in extensions that give it tasks.
    if (options.tasks.accept) {
        for (profile::ProfilePtr& entry : profiles) {
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
            if (const auto* failed = std::get_if<profile::Error>(&composed)) {
                problem("revision " + entry->revision() + " cannot be spoken with the extensions that give it tasks: " +
                        failed->pointer + ": " + failed->message);
                continue;
            }
            entry = std::get<profile::ProfilePtr>(std::move(composed));
        }
    }

    const ClientMechanismRegistry& registry =
        options.mechanisms != nullptr ? *options.mechanisms : builtin_client_mechanisms();
    auto selected = select_client_mechanisms(profiles, transport, registry);
    if (auto* failed = std::get_if<ClientMechanismsError>(&selected)) {
        problem(failed->message);
        error.missing = std::move(failed->missing);
        return error;
    }
    auto& halves = std::get<std::vector<ClientMechanisms>>(selected);
    if (halves.empty()) {
        problem("no enabled profile defines the transport " + std::string(profile::to_string(transport)));
    }
    if (!error.message.empty()) return error;

    std::vector<detail::Speaker> speakers;
    speakers.reserve(halves.size());
    for (auto& half : halves) {
        detail::Speaker speaker;
        speaker.per_request = half.open->names_revision_per_request();
        speaker.handshake = half.open->handshakes();
        speaker.self.info = options.client_info;
        speaker.self.log_level = options.log_level;
        speaker.self.capabilities = client_capabilities(options.input, *half.profile);
        if (options.tasks.accept && half.tasks->takes_tasks()) half.tasks->declare(speaker.self.capabilities);
        speaker.halves = std::move(half);
        speakers.push_back(std::move(speaker));
    }
    const bool http_sse_enabled =
        transport == profile::Transport::streamable_http &&
        std::any_of(profiles.begin(), profiles.end(),
                    [](const profile::ProfilePtr& profile) { return profile->http_sse().has_value(); });
    auto core = std::make_unique<detail::ClientEngineCore>(std::move(options), transport, std::move(sink),
                                                          std::move(speakers));
    core->set_http_sse_enabled(http_sse_enabled);
    return std::unique_ptr<ClientEngine>(new ClientEngine(std::move(core)));
}

ClientEngine::ClientEngine(std::unique_ptr<detail::ClientEngineCore> core) : core_(std::move(core)) {}
ClientEngine::~ClientEngine() = default;

void ClientEngine::open() {
    core_->guard();
    core_->open();
}
std::variant<RequestRef, CallFailure> ClientEngine::send(OutgoingCall call) {
    core_->guard();
    return core_->send(std::move(call));
}
bool ClientEngine::takes_tasks() const { return core_->takes_tasks(); }
bool ClientEngine::augments_tool_tasks() const { return core_->augments_tool_tasks(); }
void ClientEngine::augment_tool_call(json& params) const { core_->augment_tool_call(params); }
std::variant<RequestRef, CallFailure> ClientEngine::follow_task(std::string id,
                                                                std::optional<std::chrono::milliseconds> timeout) {
    core_->guard();
    return core_->task_call(std::move(id), detail::TaskMode::follow, timeout);
}
std::variant<RequestRef, CallFailure> ClientEngine::read_task(std::string id,
                                                              std::optional<std::chrono::milliseconds> timeout) {
    core_->guard();
    return core_->task_call(std::move(id), detail::TaskMode::read, timeout);
}
std::variant<RequestRef, CallFailure> ClientEngine::cancel_task(std::string id,
                                                                std::optional<std::chrono::milliseconds> timeout) {
    core_->guard();
    return core_->task_call(std::move(id), detail::TaskMode::cancel, timeout);
}
std::optional<CallFailure> ClientEngine::notify(ClientMessage message) {
    core_->guard();
    return core_->notify(std::move(message));
}
std::optional<FeedDelivery> ClientEngine::feed_delivery() const { return core_->feed_delivery(); }
ChangeFilter ClientEngine::offered(const ChangeFilter& asked) const { return core_->offered(asked); }
std::variant<RequestRef, CallFailure> ClientEngine::listen(ChangeFilter filter,
                                                           std::optional<std::chrono::milliseconds> timeout) {
    core_->guard();
    return core_->listen(std::move(filter), timeout);
}
std::variant<ClientMessage, CallFailure> ClientEngine::watch_request(std::string uri, bool watch) const {
    return core_->watch_request(std::move(uri), watch);
}
void ClientEngine::listen_on_connection(bool listening) {
    core_->guard();
    core_->listen_on_connection(listening);
}
Reception ClientEngine::received(UnitId unit, std::string_view message) {
    core_->guard();
    return core_->received(unit, message);
}
void ClientEngine::stream_event(UnitId unit, StreamEvent event) {
    core_->guard();
    core_->stream_event(unit, std::move(event));
}
void ClientEngine::expire(const Deadline& deadline) {
    core_->guard();
    core_->expire(deadline);
}
void ClientEngine::cancel(RequestRef request, std::string reason) {
    core_->guard();
    core_->cancel(request, std::move(reason));
}
void ClientEngine::answer(InputRef request, InputReply reply) {
    core_->guard();
    core_->answer(request, std::move(reply));
}
void ClientEngine::answer_round(RequestRef request, InputResponses answers) {
    core_->guard();
    core_->answer_round(request, std::move(answers));
}
void ClientEngine::fail_round(RequestRef request, CallFailure failure) {
    core_->guard();
    core_->fail_round(request, std::move(failure));
}
PeerSchema ClientEngine::compile_peer_schema(const json& schema) {
    core_->guard();
    return core_->compile_peer_schema(schema);
}
std::size_t ClientEngine::peer_schema_compiles() const noexcept { return core_->peer_schema_compiles(); }
bool ClientEngine::roots_changed() {
    core_->guard();
    return core_->roots_changed();
}
void ClientEngine::transport_restarted() {
    core_->guard();
    core_->transport_restarted();
}
void ClientEngine::close() {
    core_->guard();
    core_->close();
}
std::optional<EngineEvent> ClientEngine::next_event() {
    core_->guard();
    return core_->next_event();
}
std::uint64_t ClientEngine::dropped_events() const noexcept { return core_->dropped_events(); }
const ContactState& ClientEngine::contact() const noexcept { return core_->contact(); }
std::span<const profile::ProfilePtr> ClientEngine::profiles() const noexcept { return core_->profiles(); }
std::size_t ClientEngine::pending() const noexcept { return core_->pending(); }
const InputSupport& ClientEngine::input() const noexcept { return core_->input(); }

}  // namespace neomcp
