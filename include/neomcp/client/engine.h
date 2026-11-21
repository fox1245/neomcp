// The protocol engine of a client, without I/O.
//
// A ClientEngine writes what a client sends and reads what a server answers, for every enabled profile, and owns no
// thread, socket, process, clock or file. A transport feeds it what it read (received, stream_event) and carries out
// what it writes (Outgoing, through the sink); a driver keeps the timers the engine asks for (DeadlineChange) and calls
// expire() when one lapses; the application layer takes the engine's events (next_event) and gives it calls (send) and
// answers. How each thing is written and read is asked of the client halves of the mechanisms the profiles name
// (neomcp/client/mechanism.h); the engine never compares revision strings or looks at a profile's era to decide what to
// do. The one place it orders revisions is "newest first", which it does as the server's session does.
//
// What the engine owns:
// - request ids: integers from 1, never used twice for the engine's whole life, also across restarts of the server;
// - the pending table, which binds every request on the wire to the unit it went out on, so that an answer counts
//   only on that unit; an answer with an unknown id, a second answer, and an answer on another unit are dropped and
//   reported;
// - the header fields of every output, from the profile's entry for the transport;
// - opening contact: a probe that asks the server which revisions it serves, a handshake, or both, and the verdict on
//   which kind of server answers (ContactState);
// - the corrective retry of a request whose revision the server refuses, once, at the newest revision both serve;
// - clocks as data: every request has a deadline, which pauses while the client itself is answering the server;
// - cancellation, progress, pings in both directions, and the server's notifications as events;
// - change feeds: subscriptions as long-lived calls without a clock once acknowledged, their messages correlated by the
//   call they name, and the connection's change notifications while the client listens there (listen,
//   listen_on_connection);
// - on HTTP, the session a server no longer knows (SessionExpired): a new handshake, and every call that met the end
//   sent once more under the new session;
// - the rounds of a call that asks for input, through one seam (InputSupport, ServerAsked, InputRoundStarted), with
//   the checks that need the revision: a request of a kind the client did not declare is refused, a form whose schema
//   is outside the restricted form never reaches a handler, and accepted content that fails the form's schema is
//   never sent. Running the handlers is the input dispatcher's (neomcp/client/input_dispatch.h).
//
// Units. A transport that carries each message in an exchange of its own (one HTTP POST per message) has one unit per
// exchange; the engine numbers them and says on every output which unit it opens or belongs to. The connection itself
// is unit `connection_unit`: on stdio every message travels on it, on HTTP it is the stream the client opens to hear
// the server outside any exchange. It is the same unit for the engine's whole life, across restarts of the server and
// reopenings of that stream; the number of an exchange is never used twice.
//
// Threads. Every method is called on one executor, the client's thread, and none is safe to call from two threads. The
// sink is called from inside the engine's methods, with the engine in the middle of a change: it must not call back
// into the engine (a transport queues what it is given and acts on it after the call returned). A call made from
// inside the sink throws std::logic_error. A sink that throws closes the engine: every pending call ends `closed`,
// nothing more is written (`close` included), and the exception propagates out of the method that was writing.
//
// What a server sends is not trusted: no method throws because of it, and no description the engine writes repeats a
// text the server sent.
#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/client/mechanism.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/json_schema.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/mechanism.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail {
class ClientEngineCore;
}  // namespace detail

// The number of a unit: what one exchange of the transport carries (see above). The same alias as the server's in
// neomcp/session.h, declared again so that this header does not depend on the server's; a program may include both.
using UnitId = std::uint64_t;

// The unit of the connection: on stdio the one stream each way; on HTTP the stream the client opens to hear the
// server outside a request (the GET stream, or the event stream of the older transport).
inline constexpr UnitId connection_unit = 0;

// One application call (or one request the engine makes itself: the probe, the handshake, a ping it was asked for). It
// stays the same while the call is sent again under a new request id (a corrective retry, a round of input), so the
// deadlines, the cancellation and the end of a call are all named by it. Never used twice.
struct RequestRef {
    std::uint64_t value = 0;
    friend bool operator==(const RequestRef&, const RequestRef&) = default;
    friend auto operator<=>(const RequestRef&, const RequestRef&) = default;
};

// One request of the server that the client answers (under the mechanism whose server sends requests). Never used
// twice.
struct InputRef {
    std::uint64_t value = 0;
    friend bool operator==(const InputRef&, const InputRef&) = default;
    friend auto operator<=>(const InputRef&, const InputRef&) = default;
};

// One header field of an HTTP exchange, owned. Names compare without regard to ASCII case.
struct HttpHeader {
    std::string name;
    std::string value;
};

// ---- What the engine writes

enum class OutgoingKind {
    request,                 // a request of the client; on HTTP it opens its unit (a POST) and the answer comes there
    notification,            // a notification of the client; on HTTP it opens its unit, which carries nothing back
    response,                // the client's answer to a server's request; on HTTP it opens its unit
    end_unit,                // no message: the transport ends the unit's exchange (its answer arrived, or the
                             // request was cancelled by ending it); ignored for a unit that ended already
    open_connection_stream,  // no message: the transport opens the stream of `connection_unit`, with the headers
    restart,                 // no message: make the connection again (stdio: start the server process again; the older
                             // transport of events over HTTP: open its event stream again), then call
                             // transport_restarted()
    close                    // no message: end the transport (an unanswered handshake, or close() of the engine)
};

// One output. The headers are the ones the profile's entry for the transport puts on this message (none on stdio);
// a transport adds its own (the content type, what it accepts) besides.
struct Outgoing {
    std::string line;  // the message on one line, without a line terminator; empty for the kinds that carry none
    UnitId unit = connection_unit;
    std::vector<HttpHeader> headers;
    OutgoingKind kind = OutgoingKind::request;
    // The profile the message goes out under (for open_connection_stream: the governing one), so that a transport
    // reads what the revision decides of its exchanges from the profile's entry: whether a stream may be resumed, how
    // a session ends, which status means what. Null for the kinds that carry no message but that one, and from a
    // writer that knows none (a transport then treats the exchange as one of no revision in particular).
    profile::ProfilePtr profile;
};

// Receives each output. See the threading rules above: it must not call into the engine.
using OutgoingSink = std::function<void(Outgoing outgoing)>;

// ---- Why a call failed

// Why an authorizer could not give a refused request the authorization the server asks for (HTTP; ClientError carries
// it as `authorization`).
enum class AuthorizationProblem {
    discovery_failed,          // the documents that name the authorization server could not be found or read
    resource_mismatch,         // the resource a document names is not the endpoint
    metadata_issuer_mismatch,  // the authorization server's document names another issuer than its address
    pkce_unsupported,          // the authorization server does not offer the code challenge method the client sends
    refused_url,               // a URL a server named is one the client does not reach (UrlPurpose::authorization)
    insecure_endpoint,         // the endpoint is plain HTTP on a host that is not this machine: no token goes there
    no_client_registration,    // no way to register the client with the authorization server is configured or offered
    registration_rejected,     // the authorization server refused to register the client
    prompt_cancelled,          // the person did not finish signing in
    prompt_timed_out,          // the person did not finish signing in in time
    state_mismatch,            // the answer of the sign-in does not belong to the request the client made
    issuer_mismatch,           // the answer of the sign-in names another issuer than the one asked
    authorization_denied,      // the authorization server refused the authorization
    token_request_failed,      // the request for a token was refused or its answer cannot be used
    unsupported_token_type,    // the authorization server issued a token of a type the client does not send
    insufficient_scope,        // the server asks again for a scope that was asked for and not granted
    forbidden,                 // the server refused the request for a reason no new token changes
    attempts_exceeded,         // the request was refused more often than the bounds allow
    document_too_large,        // a document or an answer exceeded its bound
    timed_out,                 // an exchange with an authorization server or the whole sign-in took too long
    no_answer                  // the authorizer dropped the request without answering it
};

std::string_view to_string(AuthorizationProblem problem) noexcept;

// What an authorizer met, without any secret: never a token, a code, a verifier, a state or a client secret, and
// never a text the server or the authorization server sent.
struct AuthorizationFailure {
    AuthorizationProblem problem = AuthorizationProblem::forbidden;
    std::string issuer;  // the authorization server it concerns, when one is known
    std::string url;     // the URL involved, without its query, when one
};

// Why a call ended without an answer from the server, or was refused before it went out.
enum class CallFailureKind {
    timed_out,           // its wait (or its total) lapsed; it was cancelled as its transport prescribes
    cancelled,           // the application cancelled it
    server_exited,       // stdio: the process ended while it was pending
    stream_lost,         // HTTP: the exchange that carried it ended or broke before its answer
    send_failed,         // the transport could not deliver it
    http_status,         // HTTP: its exchange was answered with a status that carries no answer of the protocol
    bad_content_type,    // HTTP: its exchange was answered with content of a type the protocol does not use
    authorization_required,  // HTTP: the server asks for authorization the client cannot give
    authorization_failed,    // HTTP: the authorizer tried to obtain the authorization and could not (`authorization`)
    message_too_large,   // its answer was longer than max_message_bytes or nested deeper than max_nesting_depth
    malformed_answer,    // its answer is no result of its kind (such as a result that asks for input on a request
                         // that cannot ask, or of a kind the client does not know)
    version_refused,     // the server refused its revision a second time after the corrective retry
    no_common_revision,  // the server's refusal names no revision this client can speak without a handshake
    contact_failed,      // contact could not be made (ContactState::failure)
    too_many_pending,    // max_pending_requests calls are pending already
    unsupported_method,  // the governing revision has no such client request, or the message is malformed
    header_value,        // an argument cannot be mirrored into a header (an integer out of range)
    input_timed_out,     // a round of input was not answered within input_handler_timeout
    input_rounds_exceeded,  // the server asked for input more than max_input_rounds times
    input_requests_exceeded,  // one round asked for more than max_input_requests_per_round inputs
    server_asked_undeclared,  // a round asked for input of a kind the client did not declare (InputSupport); no
                              // retry is sent, since a server must not ask so
    form_not_restricted,  // a round asked for a form whose schema is outside the restricted form, or cannot be
                          // compiled within the peer limits; no handler saw it
    answer_invalid,      // an accepted form's content fails the form's schema; it was not sent
    input_refused,       // the user refused a sampling request of a round; no retry is sent
    handler_failed,      // a handler of a round failed (it threw); no retry is sent
    state_not_echoable,  // the state of a round cannot be sent back exactly as it was received
    answer_not_writable,  // an answer of a round cannot be written for the revision
    closed               // the engine was closed
};

struct CallFailure {
    CallFailureKind kind = CallFailureKind::closed;
    std::string message;
    // authorization_failed: what the authorizer met.
    std::optional<AuthorizationFailure> authorization = std::nullopt;
};

// ---- What a transport tells the engine besides messages

// HTTP: the head of a response arrived; its body follows as messages (received) and the unit ends with StreamEnded.
struct ResponseHead {
    int status = 0;
    std::optional<std::string> media_type;  // the type and subtype of the content, lower case; empty when none
    std::vector<HttpHeader> headers;        // the response's header fields, as many as the transport's bound keeps
};

// HTTP event streams: the id of the last event seen on the unit, and the server's advice on how long to wait before
// reconnecting. Kept for resuming a stream; nothing ends because of them.
struct StreamEventId {
    std::string id;
};
struct StreamRetryAdvice {
    std::chrono::milliseconds delay{0};
};

// The unit's exchange ended in order (HTTP: the body is complete). A request still waiting on it lost its answer.
struct StreamEnded {};

// The unit's stream broke before its end (the connection was reset, the stream stayed idle too long). Under the older
// transport of events over HTTP, StreamLost or StreamEnded on `connection_unit` means that the event stream of the
// connection ended, which ends the connection as a whole, as ProcessExited does on stdio.
struct StreamLost {
    std::string cause;  // written by the transport, for diagnostics
};

// The unit's message could not be delivered at all (a connection could not be made, a write failed).
struct SendFailed {
    std::string cause;
};

// stdio, on `connection_unit`: the server process ended. Every request pending on it lost its answer.
struct ProcessExited {
    std::optional<int> status;  // the exit status, when the process exited rather than being ended by a signal
};

// HTTP: the server answered the unit's request, which carried the session id of a handshake, as one of a session it
// does not know (the status the profile names for that). The server did not take the request: nothing more comes for
// the unit, and the transport sends no session id from now on until a handshake opens a new session. `current`: the
// session the request carried is the one in use (false when a newer handshake replaced it already).
struct SessionExpired {
    bool current = true;
};

// HTTP: the server refused the unit's request for want of authorization, and the transport holds the request while
// the authorizer works for it (`begin`), or the hold ended and the request went out again, the same bytes with the
// same id and new credentials (not `begin`). While a request is held its call's clocks wait, as they wait while an
// input handler answers the server: what takes the time is the client's own sign-in, not the server.
struct AuthorizationHold {
    bool begin = true;
};

// HTTP: the authorizer gave up on the unit's refused request: nothing more comes for the unit, and the calls it
// carried end with `failure` (authorization_required or authorization_failed). `status` is that of the refusal.
struct AuthorizationDenied {
    CallFailure failure;
    int status = 0;
};

using StreamEvent = std::variant<ResponseHead, StreamEventId, StreamRetryAdvice, StreamEnded, StreamLost, SendFailed,
                                 ProcessExited, SessionExpired, AuthorizationHold, AuthorizationDenied>;

// What received() made of one message, so that a transport can count what it must bound (stdio counts the lines that
// are not MCP messages).
enum class Reception {
    accepted,     // a JSON-RPC message, or an array of them where the revision allows one, and was handled
    too_large,    // longer than max_message_bytes: not parsed
    too_deep,     // nested deeper than max_nesting_depth: not parsed
    not_json,     // not JSON
    not_message,  // JSON, but not a JSON-RPC message (or an array where the revision allows none)
    ignored       // after close(), or on a unit the engine does not know or has ended
};

// ---- Clocks

// Which clock of a call or of a server's request.
enum class DeadlineKind {
    wait,   // how long a request waits for its answer (the request's timeout); started again by progress when the
            // call asked for that
    total,  // the longest a call that is started again by progress may wait in all
    input   // how long the client may take to answer the server: one server request, or one round of a call
};

// Whose clock it is.
using DeadlineOwner = std::variant<RequestRef, InputRef>;

// Names one running stretch of a clock. Every change of a clock gives it a new epoch, so that a timer the driver set
// for an older stretch (one that lapsed while the change was on its way) is recognised and ignored by expire().
struct Deadline {
    DeadlineOwner owner;
    DeadlineKind kind = DeadlineKind::wait;
    std::uint64_t epoch = 0;
    friend bool operator==(const Deadline&, const Deadline&) = default;
};

enum class DeadlineAction {
    start,   // run for `duration` from now; replaces what ran before for the same owner and kind
    pause,   // stop counting and keep what is left
    resume,  // go on counting what was left at the pause
    stop     // the clock is gone
};

// A change of one clock, for the driver. On pause the driver keeps the time left; on resume it sets a timer for that
// time under the new epoch. When a timer lapses it calls expire() with the Deadline of the change that set it.
struct DeadlineChange {
    Deadline deadline;
    DeadlineAction action = DeadlineAction::start;
    std::chrono::milliseconds duration{0};  // start only
};

// ---- Contact

// How a server is opened, named by what the open mechanism does.
enum class ServerKind {
    per_request,  // every request names its revision; the server serves each on its own
    handshake     // a handshake fixes the revision for the connection
};

// Which kind of server answers, on which transport, at which revision: what a probe found out, kept as data so that an
// application can give it back to a later engine for the same server (EngineOptions::prior). The transport is the one
// the server answered on: an HTTP endpoint that turns out to serve only the older event-stream transport is reported
// as such, and a later client goes to it directly. The revision is data: the engine finds the enabled profile of that
// revision and never decides anything from the text.
struct ContactVerdict {
    ServerKind kind = ServerKind::per_request;
    profile::Transport transport = profile::Transport::stdio;
    std::string revision;
    friend bool operator==(const ContactVerdict&, const ContactVerdict&) = default;
};

enum class ContactPhase {
    unopened,     // before open()
    probing,      // the request that asks which revisions the server serves is out
    handshaking,  // the handshake is out
    open,         // requests go out under `profile`
    lost,         // stdio: the server process ended; the next request starts it again (restart)
    failed,       // contact cannot be made; `failure` says why; every request fails
    closed        // after close()
};

// Why contact failed.
enum class ContactFailureKind {
    no_common_revision,   // the server serves none of the enabled revisions (`server_revisions` lists its)
    unserved_revision,    // the handshake's answer names a revision none of the candidates is
    handshake_refused,    // the handshake was answered with an error, after its one retry where that applies
    handshake_timed_out,  // the handshake was not answered in time (it is never cancelled: the transport ends)
    malformed_answer,     // the answer that opens contact cannot be read
    authorization_required,  // HTTP: the server asks for authorization the client cannot give
    authorization_failed,    // HTTP: the authorizer tried to obtain the authorization and could not
    http_status,          // HTTP: a status after which no step of opening goes on (a server error, an unknown 4xx)
    transport,            // the transport failed while contact was being made
    too_many_restarts     // the server lost the client's state (stdio: the process ended; HTTP: the session ended)
                          // more than max_restarts times in a row without an answer between
};

struct ContactFailure {
    ContactFailureKind kind = ContactFailureKind::transport;
    std::string message;
    std::vector<std::string> server_revisions;  // what the server said it serves, when it said
    // HTTP: the transport this endpoint may serve instead, for the next step of opening contact. Set to http_sse when
    // the handshake was refused with 400, 404 or 405 and no body only a server that names revisions per request sends,
    // and an enabled profile defines the older transport of events over HTTP; empty otherwise.
    std::optional<profile::Transport> next_transport;
    // authorization_failed: what the authorizer met.
    std::optional<AuthorizationFailure> authorization = std::nullopt;
};

// What one step of opening contact met, in order, so that a failure can name every answer.
struct ContactStep {
    RequestRef request;
    std::string method;
    std::string outcome;  // in words; never a text the server sent
};

struct ContactState {
    ContactPhase phase = ContactPhase::unopened;
    std::optional<ContactVerdict> verdict;
    profile::ProfilePtr profile;                      // the profile requests go out under; null until open
    ServerAnnouncement server;                        // from the discovery result or the handshake's result
    std::optional<std::vector<std::string>> revisions;  // the revisions a discovery result listed
    std::optional<ContactFailure> failure;
    std::vector<ContactStep> steps;
};

// ---- Calls

// A request the application makes: the method and params as the application gives them. The engine adds what the open
// mechanism puts on every request, a progress token where asked, and the request id.
struct OutgoingCall {
    ClientMessage message;
    // How long the call waits for its answer; empty: EngineOptions::request_timeout. Zero is an error.
    std::optional<std::chrono::milliseconds> timeout;
    // Each progress notification starts the wait again, never beyond EngineOptions::max_total_timeout in all.
    bool reset_timeout_on_progress = false;
    // Ask the server for progress notifications of the call.
    bool progress = false;
    // tools/call on a transport whose profile mirrors tool arguments into headers: the tool's input schema, from which
    // the marked arguments are mirrored. Null: no argument is mirrored.
    json input_schema = nullptr;
};

// How a call ended.
struct Answered {
    json result;  // the result as the server sent it
};
struct ErrorAnswered {
    RequestError error;  // the server's error answer: its code, message and data
};
using RequestOutcome = std::variant<Answered, ErrorAnswered, CallFailure>;

// The user refused the server's request (a sampling request): answered as the revision prescribes.
struct InputRefused {};

// What the client answers to a server's request: the answer, an error answer, or the user's refusal.
using InputReply = std::variant<InputResponse, RequestError, InputRefused>;

// ---- Input: which requests of a server the client takes

// The kinds of input request the client has handlers for. The capabilities the client declares are written from it
// for each profile, and a request of a kind it does not name is refused (or, for a result that asks, handed to the
// caller as the call's answer) without reaching a handler.
struct InputSupport {
    bool form = false;              // elicitation in form mode
    bool url = false;               // elicitation in URL mode
    bool sampling = false;
    bool sampling_tools = false;    // sampling with tools; only with `sampling`
    bool sampling_context = false;  // sampling that includes context; only with `sampling`
    bool roots = false;
    bool roots_list_changed = false;  // the client tells when its roots change; only with `roots`

    // Whether the client takes the request.
    bool takes(const InputRequest& request) const noexcept;

    friend bool operator==(const InputSupport&, const InputSupport&) = default;
};

// The ClientCapabilities object that declares `support` under `profile`: a member for each kind the client takes and
// nothing for a kind it does not, kept to the members the profile's ClientCapabilities type has. A nested member is
// written only where the profile carries what it declares: the mode of an elicitation where the profile's form
// params have a mode (elsewhere an empty elicitation object declares forms), URLs where the profile has URL params,
// tools where its sampling params have tools, and that the roots change where the client may send the notification
// that says so. The engine declares exactly this for each profile.
json client_capabilities(const InputSupport& support, const profile::Profile& profile);

// ---- Events

// A call ended. Its ref is not used again.
struct RequestEnded {
    RequestRef request;
    RequestOutcome outcome;
    profile::ProfilePtr profile;  // the profile it went out under last; null when it never went out
};

// The server reported progress of a call that asked for it.
struct ProgressEvent {
    RequestRef request;
    double progress = 0;
    std::optional<double> total;
    std::optional<std::string> message;
};

// The server sent a log message.
struct LogEvent {
    LogLevel level = LogLevel::info;
    std::optional<std::string> logger;
    json data;
    std::optional<RequestRef> request;  // the call whose exchange carried it, when one did
};

// A message of a change feed (a change, an acknowledgement, an end).
struct ChangeEvent {
    FeedEvent feed;
    std::optional<RequestRef> subscription;  // the call that opened the subscription, when the feed names one
};

// A request of the server that a handler takes (InputSupport). The engine waits for answer(); meanwhile the clocks of
// `paused` wait, and the input clock of `ref` runs. When that clock lapses the engine answers itself (a form or URL
// request with a cancel, any other kind with an error), and ServerAskEnded follows. A form arrives here only when its
// schema fits the restricted form and compiles within the peer limits; any other is answered with invalid params.
struct ServerAsked {
    InputRef ref;
    std::string method;
    InputRequest request;
    std::optional<RequestRef> call;  // the call whose exchange carried the request, when the wire says which
    std::vector<RequestRef> paused;  // the calls whose clocks wait for the answer
    profile::ProfilePtr profile;
};

// A server's request ended without answer(): the server cancelled it, or its input clock lapsed and the engine
// answered it. An answer() for it is ignored.
struct ServerAskEnded {
    InputRef ref;
    bool cancelled_by_server = false;
};

// A call's result asks for input, and handlers take every request it asks. The call waits, with its clocks paused and
// its input clock running, until answer_round(), fail_round() or cancel(). A round arrives here only when it asks for
// at most max_input_requests_per_round inputs and every form in it fits the restricted form; any other ends the call.
struct InputRoundStarted {
    RequestRef request;
    InputAsking asking;
    std::size_t round = 1;  // 1 for the first result that asks
    profile::ProfilePtr profile;  // the profile the call went out under
};

// The phase of contact changed; contact() has the whole state.
struct ContactChanged {
    ContactPhase phase = ContactPhase::unopened;
};

enum class DiagnosticKind {
    unknown_id,          // an answer whose id names no pending request
    repeated_answer,     // a second answer for a request that was answered
    wrong_unit,          // an answer on another unit than the one its request went out on
    too_large,           // a message longer than max_message_bytes
    too_deep,            // a message nested deeper than max_nesting_depth
    not_json,            // a message that is not JSON
    not_message,         // JSON that is not a JSON-RPC message
    malformed_message,   // a JSON-RPC message the revision does not allow as it is (a malformed notification, a
                         // request the mechanism does not let a server send)
    malformed_contact,   // an answer that opens contact could not be read, and the engine went on as for another kind
    server_defect,       // the server answered in a way only a wrong server does (such as refusing a header this
                         // client writes as the revision says)
    events_dropped,      // events were dropped because max_queued_events were queued (the Client tells this once its
                         // queue is taken; the engine itself only counts them, dropped_events())
    level_refused,       // the server refused the log level the client asked for
    form_not_restricted,  // a server's request for a form outside the restricted form was answered invalid params
    answer_invalid,      // an answer to a server's request held accepted content that fails the form's schema: it
                         // was not sent, and the request was answered with an internal error
    schema_budget_spent,  // the budget of schema compiles of the server (max_peer_schema_compiles) is spent: every
                          // later schema of the server is refused without being compiled; reported once
    session_renewed,      // HTTP: the server no longer knew the session (SessionExpired), and a new handshake opens
                          // another; whatever the server held for the old one is gone
    authorization         // HTTP: what the authorizer reports while it works (AuthorizationHost::diagnostic); never a
                          // secret
};

struct Diagnostic {
    DiagnosticKind kind = DiagnosticKind::not_json;
    std::string message;          // in words; never a text the server sent
    std::optional<UnitId> unit;   // the unit it concerns, when one
    std::optional<InputRef> input = std::nullopt;  // the server's request it concerns, when one
};

using EngineEvent = std::variant<RequestEnded, ProgressEvent, LogEvent, ChangeEvent, ServerAsked, ServerAskEnded,
                                 InputRoundStarted, DeadlineChange, ContactChanged, Diagnostic>;

// ---- Options

// The defaults of EngineOptions and of the client's options (ClientOptions, which repeats those of EngineOptions). They
// are constants and not read from an options object in a default member initializer, for the reason SessionDefaults
// gives (neomcp/session.h): options objects are initialized as aggregates.
struct ClientDefaults {
    static constexpr std::chrono::milliseconds request_timeout{60'000};
    static constexpr std::chrono::milliseconds max_total_timeout{600'000};
    static constexpr std::chrono::milliseconds discover_probe_timeout{10'000};
    static constexpr std::chrono::milliseconds input_handler_timeout{600'000};
    static constexpr std::size_t max_pending_requests = 256;
    static constexpr std::size_t max_queued_events = 1024;
    static constexpr std::size_t max_message_bytes = std::size_t{16} * 1024 * 1024;
    static constexpr std::size_t max_nesting_depth = 128;
    static constexpr std::size_t max_input_rounds = 10;
    static constexpr std::size_t max_input_requests_per_round = 16;
    static constexpr std::size_t max_concurrent_server_requests = 16;
    static constexpr std::size_t max_restarts = 3;
    // The bounds of a schema a server sent (a form's requested schema), for json_schema::Limits.
    static constexpr std::size_t peer_schema_depth = 32;
    static constexpr std::size_t peer_pattern_length = 256;
    static constexpr std::size_t peer_pattern_memory = std::size_t{512} * 1024;
    static constexpr std::size_t peer_max_evaluations = 1'000'000;
    static constexpr std::size_t peer_patterns_per_schema = 32;
    // How many schemas of a server the client compiles in its whole life (forms, output schemas), refused ones too.
    static constexpr std::size_t max_peer_schema_compiles = 128;
    // The client's own (ClientOptions, neomcp/client/options.h).
    static constexpr std::size_t max_concurrent_input_handlers = 4;
    static constexpr std::chrono::milliseconds state_retry_first_delay{50};
    static constexpr std::chrono::milliseconds state_retry_max_delay{250};
    static constexpr std::size_t max_list_pages = 64;
    static constexpr std::size_t max_list_items = 10'000;
    // Change feeds (neomcp/client/subscription.h): subscriptions open at once, as many as a server's session keeps
    // (FeedDefaults::max_subscriptions); reopens of a lost subscription in a row; the wait before the first reopen,
    // doubled for each further one up to the last bound (a wait drawn at random up to that bound).
    static constexpr std::size_t max_subscriptions = 16;
    static constexpr std::size_t max_subscription_reopens = 5;
    static constexpr std::chrono::milliseconds subscription_reopen_first_delay{1'000};
    static constexpr std::chrono::milliseconds subscription_reopen_max_delay{60'000};
};

// When the client opens the stream on which a server speaks outside any request (the connection unit, where the
// governing profile's entry for the transport offers one).
enum class ConnectionStreamMode {
    when_needed,  // when the client has something that arrives outside a request: a log level it asks for, a handler
                  // of a request the server sends as a request of its own, a change listener of the connection
    always,
    never
};

struct EngineOptions {
    // The revisions the client speaks; empty: every built-in profile. Two profiles of the same revision are an error.
    // Only the profiles that define the engine's transport take part.
    std::vector<profile::ProfilePtr> profiles;

    // When the engine asks the transport for the stream of the connection (open_connection_stream). Once contact is
    // open it asks at most once per connection; a new handshake (a new session) makes a new connection.
    ConnectionStreamMode connection_stream = ConnectionStreamMode::when_needed;

    // What the client says about itself, written for each revision where it has a place.
    Implementation client_info;
    // The level the client asks the server's log messages at; empty: it asks none.
    std::optional<LogLevel> log_level;
    // The requests of a server the client has handlers for.
    InputSupport input;

    // A verdict an earlier engine reached for the same server: contact starts at it without a probe. Under a verdict
    // that the server handshakes, a handshake answered with an error only a server that names revisions per request
    // sends makes the engine probe once.
    std::optional<ContactVerdict> prior;

    // Where the client halves come from; null: builtin_client_mechanisms(). Must outlive the engine's creation.
    const ClientMechanismRegistry* mechanisms = nullptr;

    // How long a call waits for its answer unless it says otherwise. Zero is an error. Default 60 s.
    std::chrono::milliseconds request_timeout = ClientDefaults::request_timeout;
    // The longest a call whose wait progress starts again may wait in all. Zero is an error. Default 10 min.
    std::chrono::milliseconds max_total_timeout = ClientDefaults::max_total_timeout;
    // How long the probe waits on stdio before the server is taken for one that opens with a handshake (whose answer
    // to an unknown request may be none at all). Zero is an error. Default 10 s.
    std::chrono::milliseconds discover_probe_timeout = ClientDefaults::discover_probe_timeout;
    // How long one server request, or one round of a call's input, may wait for the client's answer. Zero is an
    // error. Default 600 s.
    std::chrono::milliseconds input_handler_timeout = ClientDefaults::input_handler_timeout;

    // Calls of the application pending at once (the engine's own requests do not count); one more fails at once with
    // too_many_pending. Subscriptions (listen) count as calls. Zero is an error. Default 256.
    std::size_t max_pending_requests = ClientDefaults::max_pending_requests;
    // Events queued for next_event() at once: when this many are queued, progress, log and diagnostic events are
    // dropped (and counted), and so are change events nobody takes; the end of a call, a server's request, a round, a
    // clock change, a change of contact and every message of a change feed the client takes (one of a subscription it
    // opened, or a change of the connection while it listens there, listen_on_connection) never are. The queue is
    // drained after every call into the engine, so what it holds at once is bounded by what one message brings (an
    // array, where the revision reads arrays, within max_message_bytes). Zero is an error. Default 1024.
    std::size_t max_queued_events = ClientDefaults::max_queued_events;
    // A longer message is not parsed: on stdio it is dropped, on HTTP the call its unit carries fails and the unit
    // ends. Zero is an error. Default 16 MiB.
    std::size_t max_message_bytes = ClientDefaults::max_message_bytes;
    // A message nested deeper is treated as one that is too long. Zero is an error. Default 128.
    std::size_t max_nesting_depth = ClientDefaults::max_nesting_depth;
    // Rounds of input one call may take; the call fails with input_rounds_exceeded when the server asks once more.
    // Zero is an error. Default 10.
    std::size_t max_input_rounds = ClientDefaults::max_input_rounds;
    // Inputs one round may ask for (each may open a dialog for a person); a round that asks for more fails the call
    // with input_requests_exceeded and reaches no handler. Requests a server sends of its own are bounded by
    // max_concurrent_server_requests instead. Zero is an error. Default 16.
    std::size_t max_input_requests_per_round = ClientDefaults::max_input_requests_per_round;
    // Requests of a server that wait for the client's answer at once (ServerAsked); one more is answered at once with
    // an internal error, without reaching a handler. A ping does not count. Zero is an error. Default 16.
    std::size_t max_concurrent_server_requests = ClientDefaults::max_concurrent_server_requests;
    // How many times in a row the server may lose what it held for the client, without an answer between, before
    // contact fails: on stdio the server process ends (zero: it is never started again after it ended); on HTTP the
    // server no longer knows the session (SessionExpired), and a new handshake opens another (zero: none is opened).
    // What counts as an answer is a sign that the server serves: the answer to a call of the application, a
    // subscription's acknowledgement, a change notification the client takes; the answers that open contact do not
    // count, so that a server that ends after each handshake, or forgets each session it opens, is not opened again
    // forever. The one start that follows a process that ended while the probe was out (to make the handshake with a
    // fresh process) belongs to finding out which kind of server it is and does not count. Default 3.
    std::size_t max_restarts = ClientDefaults::max_restarts;

    // The bounds every schema a server sent is compiled and validated under (compile_peer_schema; json_schema::Limits:
    // max_schema_depth, max_pattern_length, max_pattern_memory, max_evaluations, max_patterns). A form's schema that
    // does not compile within them is treated as one outside the restricted form; content whose validation exceeds
    // them is not sent. Zero is an error. Defaults 32, 256 bytes, 512 KiB, 1 000 000, 32.
    std::size_t peer_schema_depth = ClientDefaults::peer_schema_depth;
    std::size_t peer_pattern_length = ClientDefaults::peer_pattern_length;
    std::size_t peer_pattern_memory = ClientDefaults::peer_pattern_memory;
    std::size_t peer_max_evaluations = ClientDefaults::peer_max_evaluations;
    std::size_t peer_patterns_per_schema = ClientDefaults::peer_patterns_per_schema;
    // How many schemas of the server the engine compiles in its whole life, through compile_peer_schema: a schema the
    // validator refuses counts as one compiled. Once the budget is spent every later schema is refused without being
    // compiled (and a Diagnostic, schema_budget_spent, says so once). The budget is never given back: not when the
    // server lists its tools again, and not after a restart. It bounds the work a server can make the client do by
    // sending schemas. Zero is an error. Default 128.
    std::size_t max_peer_schema_compiles = ClientDefaults::max_peer_schema_compiles;
};

// Why compile_peer_schema gave no validator.
struct PeerSchemaRefused {
    // The budget of compiles was spent before this schema: it was not looked at.
    bool budget_spent = false;
    // Otherwise the validator's refusal: where in the schema, and why. The message may quote the schema (a pattern),
    // which the server wrote.
    std::optional<json_schema::Issue> issue;
};

using PeerSchema = std::variant<json_schema::Validator, PeerSchemaRefused>;

// ---- The engine

class ClientEngine {
public:
    // Selects the client halves of every enabled profile that defines `transport`. An error names every missing half
    // and every bad option; no profile left for the transport is an error, and so is an empty sink.
    static std::variant<std::unique_ptr<ClientEngine>, ClientMechanismsError> create(EngineOptions options,
                                                                                    profile::Transport transport,
                                                                                    OutgoingSink sink);

    ClientEngine(const ClientEngine&) = delete;
    ClientEngine& operator=(const ClientEngine&) = delete;
    ~ClientEngine();

    // Starts contact: under a prior verdict, at it; else the probe when a revision that names itself per request is
    // enabled (it asks the server which revisions it serves, before anything else), the handshake when only handshake
    // revisions are. Calls made before contact is open wait in the engine and go out once it is (a ping may go out
    // during a handshake). Ignored unless the phase is unopened.
    void open();

    // Sends a call under the governing profile, or keeps it until contact is open. Fails at once (and nothing is
    // queued) when the bound of pending calls is reached, when contact failed or the engine was closed, when the
    // governing revision has no such client request, when the params are neither null nor an object, and when an
    // argument cannot be mirrored into a header.
    std::variant<RequestRef, CallFailure> send(OutgoingCall call);

    // Sends a notification of the client (such as one that tells the server the client's roots changed) under the
    // governing profile. An error when contact is not open or the revision has no such client notification.
    std::optional<CallFailure> notify(ClientMessage message);

    // ---- Change feeds (the client halves of `change_feed`)

    // How the governing revision tells the client of changes; empty before contact opened and after it failed.
    std::optional<FeedDelivery> feed_delivery() const;
    // What the client may expect of `asked` from the server: the feed half's offer (ClientFeedMechanism::offered) for
    // the capabilities the server declared when contact opened; `asked` itself before contact opened.
    ChangeFilter offered(const ChangeFilter& asked) const;

    // Opens a subscription, under a revision whose feed delivers on subscriptions: a long-lived call whose request the
    // feed half writes for `filter` under the profile the call goes out under (again under another, when the server
    // refuses the revision once). Its wait clock (`timeout`; empty: request_timeout) runs only until the server's
    // acknowledgement arrives (a ChangeEvent of kind acknowledged that names the call); from then on no clock runs,
    // and the call ends only:
    // - with the result that ends the subscription, read by the feed half: Answered; a result that is not one is
    //   malformed_answer;
    // - with an error answer (the server refuses the subscription): ErrorAnswered;
    // - when the server ends it with a notice: a ChangeEvent of kind ended, then Answered with a null result;
    // - with the loss of its stream or of the server process (stream_lost, server_exited and the other failures of a
    //   call's exchange), when contact fails, and when the engine closes;
    // - when the client cancels it (cancel(): as its transport cancels a request).
    // A result the server still sends after the notice or the cancellation is dropped without a diagnostic. Fails as
    // send() does, with contact_failed before contact opened, and with unsupported_method under a revision whose feed
    // delivers on the connection.
    std::variant<RequestRef, CallFailure> listen(ChangeFilter filter,
                                                 std::optional<std::chrono::milliseconds> timeout = std::nullopt);
    // The request that watches one resource (`watch` true) or stops watching it, under a revision whose feed delivers
    // on the connection, as the feed half writes it for the governing profile; the client sends it with send(). An
    // error under a revision that has no such request, and before contact opened.
    std::variant<ClientMessage, CallFailure> watch_request(std::string uri, bool watch) const;
    // Whether the client takes the change notifications of the connection (it holds a subscription under a revision
    // whose feed delivers there). While it does: those notifications are never dropped from the queue; on HTTP, where
    // the governing profile's entry offers a stream outside requests, the client needs that stream
    // (EngineOptions::connection_stream), so the engine asks the transport to open it (open_connection_stream) once
    // contact is open, and again each time contact opens anew; on stdio a server process that ended is started again at
    // once, as for a held call, so that the notifications flow again (bounded by max_restarts).
    void listen_on_connection(bool listening);

    // One message read on a unit: a line on stdio, a JSON body or one event's data on HTTP.
    Reception received(UnitId unit, std::string_view message);

    // Something about a unit's exchange other than a message (see StreamEvent).
    void stream_event(UnitId unit, StreamEvent event);

    // A timer the driver set for `deadline` lapsed. Ignored when the clock has changed since (another epoch), is
    // paused, or is gone. The call ends timed_out (or input_timed_out) and is cancelled as its transport prescribes;
    // the probe gives the verdict that the server opens with a handshake; the handshake ends the transport.
    void expire(const Deadline& deadline);

    // The application cancels a call: it is cancelled as its transport prescribes (a notification, ending its
    // exchange, or both) and ends `cancelled`; between rounds of input nothing is sent. The handshake is never
    // cancelled. Ignored for a call that ended.
    void cancel(RequestRef request, std::string reason = {});

    // The answer to a server's request a handler took (ServerAsked). An accepted form whose content fails the form's
    // schema is not sent: the request is answered with an internal error and a Diagnostic (answer_invalid) says so.
    // InputRefused is answered as the revision prescribes a user's refusal. Ignored once the request ended.
    void answer(InputRef request, InputReply reply);

    // The answers of a round (InputRoundStarted): the call goes out again, with a new id, the answers under the keys
    // they answer and the state exactly as received. The call fails when the answers cannot be written, when the
    // state cannot be sent back unchanged, and when an accepted form's content fails its schema (answer_invalid);
    // then nothing is sent. Ignored unless the call waits for this round.
    void answer_round(RequestRef request, InputResponses answers);

    // Ends a call that waits for a round (InputRoundStarted) with `failure`, such as a handler that failed or a user
    // who refused; nothing is sent, as no retry follows. Ignored unless the call waits for a round.
    void fail_round(RequestRef request, CallFailure failure);

    // Compiles a schema the server sent (a form's requested schema, a tool's output schema) within the peer bounds of
    // EngineOptions, and counts it against max_peer_schema_compiles. A `$ref` to another document is refused by the
    // validator, never fetched. The engine compiles the forms of a server through this too, so that every schema of
    // the server draws on one budget.
    PeerSchema compile_peer_schema(const json& schema);
    // Schemas compiled (or refused by the validator) so far.
    std::size_t peer_schema_compiles() const noexcept;

    // Tells the server that the client's roots changed, where the governing profile has a notification for that and
    // the client declared that it sends it (InputSupport::roots_list_changed). False, and nothing is sent, otherwise,
    // and when contact is not open.
    bool roots_changed();

    // The connection was made again (after a restart output, or on the transport's own account): stdio, a new server
    // process runs; the older transport of events over HTTP, a new event stream named where messages go. Every request
    // still pending ends server_exited (stdio) or stream_lost; ids go on counting; the verdict stays, and under a
    // revision that keeps a connection the handshake is made again before held calls go out.
    void transport_restarted();

    // Ends every pending call `closed`, stops every clock, and writes `close`. Later calls are refused and later input
    // is ignored.
    void close();

    // The next event, oldest first; empty when none is queued. Events are queued by the methods above.
    std::optional<EngineEvent> next_event();
    // How many events were dropped because the queue was full, since the engine was created.
    std::uint64_t dropped_events() const noexcept;

    const ContactState& contact() const noexcept;
    // The requests of a server the client has handlers for (EngineOptions::input).
    const InputSupport& input() const noexcept;
    // The enabled profiles that define the engine's transport, newest revision first.
    std::span<const profile::ProfilePtr> profiles() const noexcept;
    // Calls of the application pending now (sent or held), the bound of max_pending_requests.
    std::size_t pending() const noexcept;

private:
    explicit ClientEngine(std::unique_ptr<detail::ClientEngineCore> core);
    std::unique_ptr<detail::ClientEngineCore> core_;
};

}  // namespace neomcp
