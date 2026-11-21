// The client's side of the older transport of events over HTTP (neomcp/client/http.h, http_sse.h).
//
// The connection is one GET of the endpoint's URL that accepts an event stream and stays open. Its first event must be
// of the type the profile names for the endpoint event (transports.http_sse.endpoint_event), within
// HttpEndpoint::endpoint_timeout of the request; its data is a URI reference, resolved against the URL of the GET as
// RFC 3986 resolves references (without its fragment), and admitted by the guard of URLs a server supplies: http or
// https, no user information, the endpoint's origin. Until then the client's messages wait. From then on each message
// is one POST to that URL with `Content-Type: application/json`; any status of success is taken and the answer's body
// is dropped unread, since the server's answers come on the stream: the events of the type the profile names for
// messages (transports.http_sse.message_event), each one message. Events of other types are not messages, and an
// endpoint event later in the stream changes nothing. The transport holds no event name of its own.
//
// A connection that cannot be made (the GET is refused or answered without an event stream, its first event is of
// another type, the endpoint is refused by the guard, no endpoint event comes in time, the stream ends first) fails
// every message that waits for it with what the GET met: the head of a refusal by status, where the client reads the
// status, or else the reason in words. No description repeats a text the server sent.
//
// A connection that was made ends with its stream, and a stream that ends or breaks ends the connection as a whole:
// the client is told (StreamLost on connection_unit). When the client asks for the connection again (restart), a new
// GET opens it; once its endpoint event arrived, the client is told that it was made again (restarted) and opens
// contact anew. The transport defines no resumption, and what the client gives while the connection is lost fails at
// once.
//
// Threads, units and the transfer thread are as for the Streamable HTTP transport (streamable.cpp): one transfer thread
// per transport, and everything it reads reaches this object on the client's thread, through one queue.
//
// Authorization (authorization_gate.h). A GET of the stream refused with 401 or 403 is held, and so is every message
// that waits for the connection (the client hears of each hold through the queue, in order); the clock of the endpoint
// event waits too. On retry a new GET opens the connection; on fail every waiting message ends with the authorizer's
// error. A POST refused so is held, and sent again (the same bytes) on retry, as on Streamable HTTP.
#include "client_http/http_sse.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/steady_timer.hpp>

#include "client_http/authorization_gate.h"
#include "client_http/delivery.h"
#include "client_http/transfers.h"
#include "client_http/url_guard.h"
#include "core/uri.h"
#include "neomcp/client/http.h"
#include "neomcp/http/headers.h"

namespace neomcp::detail::http_client {

namespace {

constexpr std::string_view post_method = "POST";
constexpr std::string_view get_method = "GET";
constexpr std::string_view field_content_type = "Content-Type";
constexpr std::string_view field_accept = "Accept";
constexpr std::string_view field_authorization = "Authorization";
constexpr std::string_view request_media_type = "application/json";
constexpr std::string_view event_stream_media_type = "text/event-stream";
// The line that keeps libcurl from adding `Expect: 100-continue` to a large body.
constexpr const char* no_expect_line = "Expect:";

constexpr int status_success_first = 200;
constexpr int status_success_last = 299;
constexpr int status_unauthorized = 401;
constexpr int status_forbidden = 403;
// The bytes a URI reference cannot hold (RFC 3986, section 2): controls, the space and DEL.
constexpr unsigned char reference_byte_first = 0x21;
constexpr unsigned char reference_byte_last = 0x7E;

// The units of the GETs: the transport's own, which the engine never numbers (it numbers from connection_unit up).
constexpr UnitId first_stream_unit = std::numeric_limits<UnitId>::max();

bool is_success(int status) { return status >= status_success_first && status <= status_success_last; }
bool is_refusal_for_authorization(int status) { return status == status_unauthorized || status == status_forbidden; }

ClientError settings_error(std::string message) {
    ClientError error;
    error.kind = ClientErrorKind::invalid_options;
    error.message = std::move(message);
    return error;
}

// The types of the transport's two events, read from the enabled profiles that define it.
struct EventNames {
    std::string endpoint;
    std::string message;
};

std::variant<EventNames, std::string> event_names(const HttpTransportSettings& settings) {
    std::vector<profile::ProfilePtr> profiles = settings.profiles;
    if (profiles.empty()) {
        for (const auto revision : profile::builtin_revisions()) profiles.push_back(profile::builtin(revision));
    }
    std::optional<EventNames> names;
    for (const profile::ProfilePtr& one : profiles) {
        if (!one || !one->http_sse()) continue;
        EventNames these{one->http_sse()->endpoint_event, one->http_sse()->message_event};
        if (!names) {
            names = std::move(these);
        } else if (names->endpoint != these.endpoint || names->message != these.message) {
            return std::string("the enabled revisions name the events of the transport of events over HTTP "
                               "differently");
        }
    }
    if (!names) return std::string("no enabled revision defines the transport of events over HTTP");
    return *std::move(names);
}

// The URL a URI reference names, seen from `base`, without its fragment; empty when the reference holds a byte no URI
// reference holds.
std::optional<std::string> resolve_endpoint(std::string_view base, std::string_view reference) {
    if (reference.empty()) return std::nullopt;
    for (const char c : reference) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < reference_byte_first || byte > reference_byte_last) return std::nullopt;
    }
    Uri target = resolve_uri(parse_uri(base), parse_uri(reference));
    target.fragment.reset();
    return to_string(target);
}

// The header lines of a request, and why one cannot be sent.
struct Lines {
    std::vector<std::string> lines;
    std::optional<std::string> problem;

    void add(std::string_view name, std::string_view value) {
        auto line = header_line(name, value);
        if (!line) {
            if (!problem) problem = "the header field " + std::string(name) + " cannot be sent as it is";
            return;
        }
        lines.push_back(std::move(*line));
    }
};

class SseTransport final : public Transport {
public:
    SseTransport(HttpEndpoint endpoint, HttpParts parts, EventNames names, std::unique_ptr<AuthorizationGate> gate)
        : endpoint_(std::move(endpoint)),
          names_(std::move(names)),
          guard_(parts.guard),
          max_message_bytes_(parts.limits.max_message_bytes),
          global_(std::move(parts.global)),
          delivery_(std::make_shared<Delivery>()),
          loop_(std::make_unique<TransferLoop>(std::move(parts.limits), std::move(parts.guard),
                                               [delivery = delivery_](UnitId unit, TransferItem item) {
                                                   delivery->push(unit, std::move(item));
                                               })),
          gate_(std::move(gate)) {}

    ~SseTransport() override {
        if (timer_) timer_->cancel();
        gate_->stop();
        loop_->stop();
        delivery_->detach();
    }

    profile::Transport kind() const noexcept override { return profile::Transport::http_sse; }

    ConnectionStreamMode connection_stream() const noexcept override { return endpoint_.connection_stream; }

    std::optional<std::string> start(TransportEvents& events) override {
        if (started_) throw std::logic_error("an HTTP transport was started twice");
        started_ = true;
        events_ = &events;
        timer_.emplace(events.executor());
        delivery_->attach(events.executor(), [this](UnitId unit, TransferItem item) { on_item(unit, std::move(item)); });
        if (auto problem = loop_->start()) return problem;
        GateHooks hooks;
        hooks.hold = [this](UnitId unit) { hold(unit); };
        hooks.resend = [this](UnitId unit) { resend(unit); };
        hooks.deny = [this](UnitId unit, CallFailure failure, int status) { deny(unit, std::move(failure), status); };
        hooks.diagnostic = [this](Diagnostic diagnostic) {
            if (!closed_ && events_ != nullptr) events_->diagnostic(std::move(diagnostic));
        };
        gate_->start(events.executor(), *loop_, std::move(hooks));
        connect();
        return std::nullopt;
    }

    void send(Outgoing outgoing) override {
        if (closed_ || !started_) return;
        switch (outgoing.kind) {
            case OutgoingKind::request:
            case OutgoingKind::notification:
            case OutgoingKind::response: post(std::move(outgoing)); return;
            case OutgoingKind::end_unit: end(outgoing.unit); return;
            case OutgoingKind::restart:
                // Asked for while the connection is made, or being made, it changes nothing.
                if (state_ == State::lost) connect();
                return;
            case OutgoingKind::open_connection_stream:
            case OutgoingKind::close: return;
        }
    }

    // The stream and every exchange end; the server learns of the end by the closed stream.
    asio::awaitable<void> close(CloseCause) override {
        if (closed_) co_return;
        closed_ = true;
        if (timer_) timer_->cancel();
        gate_->stop();
        loop_->stop();
        delivery_->detach();
        events_ = nullptr;
        co_return;
    }

private:
    enum class State {
        connecting,  // the GET runs, and its endpoint event has not come
        connected,   // messages go to endpoint_url_
        lost,        // a connection that was made ended; the next one waits for the client's restart
        failed       // the first connection could not be made; every message fails as it did
    };

    // What a GET that made no connection met: the head of its refusal by status, or a reason in words; or the
    // authorizer's end of a refusal it held.
    struct Failure {
        std::optional<TransferHead> head;
        std::string cause;
        std::optional<AuthorizationDenied> denial = std::nullopt;
    };

    HttpRequestInfo info(std::string_view method, UnitId unit, const std::string& url,
                         profile::ProfilePtr profile) const {
        return HttpRequestInfo{std::string(method), url, unit, std::move(profile)};
    }

    // Adds the authorizer's field to a request about to go out, and notes the generation it came from; why the field
    // cannot be sent, otherwise.
    std::optional<std::string> stamp(TransferRequest& request, const HttpRequestInfo& about,
                                     std::uint64_t& generation) {
        const Credential credential = gate_->credential(about);
        generation = credential.generation;
        if (credential.value.empty()) return std::nullopt;
        auto line = header_line(field_authorization, credential.value);
        if (!line) return std::string("the header field Authorization cannot be sent as it is");
        request.header_lines.push_back(std::move(*line));
        return std::nullopt;
    }

    // ---- the stream of the connection

    // Opens a connection: the GET, and the clock of its endpoint event.
    void connect() {
        state_ = State::connecting;
        stream_unit_ = next_stream_unit_--;
        TransferRequest request;
        request.unit = stream_unit_;
        request.url = endpoint_.url;
        request.method = TransferMethod::get;
        request.typed_events = true;
        Lines lines;
        lines.add(field_accept, event_stream_media_type);
        request.header_lines = std::move(lines.lines);
        std::optional<std::string> problem = lines.problem;
        if (!problem) {
            problem = stamp(request, info(get_method, connection_unit, endpoint_.url, nullptr), stream_generation_);
        }
        arm_endpoint_clock();
        if (problem) {
            // Told through the queue: never from inside start() or send().
            delivery_->push(stream_unit_, SendFailed{*problem});
            return;
        }
        loop_->submit(std::move(request));
    }

    void arm_endpoint_clock() {
        const std::uint64_t attempt = ++attempt_;
        timer_->expires_after(endpoint_.endpoint_timeout);
        timer_->async_wait([this, alive = std::weak_ptr<const bool>(alive_), attempt](const std::error_code& error) {
            if (error || alive.expired()) return;
            if (closed_ || attempt != attempt_ || state_ != State::connecting) return;
            loop_->remove(stream_unit_);
            connection_failed(Failure{std::nullopt, "the event stream named no endpoint for messages within "
                                                    "endpoint_timeout"});
        });
    }

    void stop_endpoint_clock() {
        ++attempt_;
        timer_->cancel();
    }

    void on_stream_item(TransferItem item) {
        if (auto* head = std::get_if<TransferHead>(&item)) {
            if (state_ != State::connecting) return;
            if (!is_refusal_for_authorization(head->status)) gate_->accepted(connection_unit);
            if (is_success(head->status) && head->media_type && *head->media_type == event_stream_media_type) return;
            loop_->remove(stream_unit_);
            if (is_refusal_for_authorization(head->status) && gate_->takes_refusals()) {
                hold_stream(*head);
                return;
            }
            const std::string status = std::to_string(head->status);
            Failure failure;
            failure.cause = is_success(head->status)
                                ? "the request for the event stream was answered with status " + status +
                                      " and no event stream"
                                : "the request for the event stream was refused with status " + status;
            if (!is_success(head->status)) failure.head = std::move(*head);
            connection_failed(std::move(failure));
            return;
        }
        if (auto* event = std::get_if<TransferEvent>(&item)) {
            if (state_ == State::connecting) {
                first_event(*event);
                return;
            }
            if (event->data.size() > max_message_bytes_) {
                // The transfer thread stopped the stream there: the client hears of the message too long, and the
                // connection ends.
                const std::string cause = "an event of the stream exceeds max_message_bytes";
                lose_stream();
                static_cast<void>(events_->received(connection_unit, event->data));
                if (events_ != nullptr && !closed_) events_->stream_event(connection_unit, StreamLost{cause});
                return;
            }
            if (event->type == names_.message) static_cast<void>(events_->received(connection_unit, event->data));
            return;
        }
        if (std::holds_alternative<TransferMessage>(item) || std::holds_alternative<StreamEventId>(item) ||
            std::holds_alternative<StreamRetryAdvice>(item)) {
            return;  // no resumption: ids and advice mean nothing here
        }
        // The stream ended, broke, or could not be made.
        std::string cause = "the event stream ended";
        if (const auto* lost = std::get_if<StreamLost>(&item)) cause = "the event stream broke: " + lost->cause;
        if (const auto* failed = std::get_if<SendFailed>(&item)) {
            cause = "the request for the event stream could not be made: " + failed->cause;
        }
        if (state_ == State::connecting) {
            connection_failed(Failure{std::nullopt, std::move(cause)});
            return;
        }
        lose_stream();
        events_->stream_event(connection_unit, StreamLost{std::move(cause)});
    }

    // The first event of the stream: the endpoint event, or the end of the connection being made.
    void first_event(const TransferEvent& event) {
        if (event.type != names_.endpoint) {
            loop_->remove(stream_unit_);
            connection_failed(Failure{std::nullopt, "the event stream's first event is not the endpoint event"});
            return;
        }
        const auto target = resolve_endpoint(endpoint_.url, event.data);
        if (!target) {
            loop_->remove(stream_unit_);
            connection_failed(Failure{std::nullopt, "the endpoint event's data is no URI reference"});
            return;
        }
        if (auto refusal = guard_.admit(UrlPurpose::event_endpoint, *target)) {
            loop_->remove(stream_unit_);
            connection_failed(Failure{std::nullopt, std::move(*refusal)});
            return;
        }
        stop_endpoint_clock();
        endpoint_url_ = *target;
        state_ = State::connected;
        const bool again = made_;
        made_ = true;
        std::vector<Outgoing> waiting;
        waiting.swap(queued_);
        for (Outgoing& outgoing : waiting) submit_post(std::move(outgoing));
        if (again) events_->restarted();
    }

    // The connection being made cannot be: a connection made again is lost once more (the client decides whether to
    // ask again); the first one fails every message that waits, and every later one.
    void connection_failed(Failure failure) {
        stop_endpoint_clock();
        if (made_) {
            state_ = State::lost;
            events_->stream_event(connection_unit, StreamLost{std::move(failure.cause)});
            return;
        }
        state_ = State::failed;
        failure_ = std::move(failure);
        std::vector<Outgoing> waiting;
        waiting.swap(queued_);
        for (const Outgoing& outgoing : waiting) refuse(outgoing.unit);
    }

    // ---- authorization (the gate's hooks)

    // The GET was refused for want of authorization, and the gate takes it: the GET, its clock and every message that
    // waits for the connection are held. Nothing is touched after the gate was told: its hooks may end the connection.
    void hold_stream(const TransferHead& head) {
        stop_endpoint_clock();
        stream_held_ = true;
        for (const Outgoing& waiting : queued_) hold_waiting(waiting.unit);
        gate_->refused(connection_unit, info(get_method, connection_unit, endpoint_.url, nullptr), stream_generation_,
                       head.status, head.headers);
    }

    // A message that waits for the held connection: the client hears that it is held, through the queue, so that the
    // end of the hold always comes after its start.
    void hold_waiting(UnitId unit) {
        if (!held_waiting_.insert(unit).second) return;
        delivery_->push(unit, AuthorizationHold{true});
    }

    void release_waiting() {
        for (const UnitId unit : held_waiting_) delivery_->push(unit, AuthorizationHold{false});
        held_waiting_.clear();
    }

    void hold(UnitId unit) {
        if (closed_ || unit == connection_unit) return;
        const auto found = posts_.find(unit);
        if (found == posts_.end()) return;
        found->second.held = true;
        delivery_->push(unit, AuthorizationHold{true});
    }

    void resend(UnitId unit) {
        if (closed_ || events_ == nullptr) return;
        if (unit == connection_unit) {
            if (!stream_held_) return;
            stream_held_ = false;
            release_waiting();
            connect();
            return;
        }
        const auto found = posts_.find(unit);
        if (found == posts_.end() || !found->second.retained) return;
        Post& exchange = found->second;
        TransferRequest request = *exchange.retained;
        if (auto problem = stamp(request, info(post_method, unit, exchange.url, exchange.profile), exchange.generation)) {
            posts_.erase(found);
            events_->stream_event(unit, SendFailed{std::move(*problem)});
            return;
        }
        loop_->submit(std::move(request));
        if (std::exchange(exchange.held, false)) delivery_->push(unit, AuthorizationHold{false});
    }

    void deny(UnitId unit, CallFailure failure, int status) {
        if (closed_ || events_ == nullptr) return;
        if (unit == connection_unit) {
            if (!stream_held_) return;
            stream_held_ = false;
            held_waiting_.clear();
            Failure ended;
            ended.cause = "the request for the event stream was refused for want of authorization";
            ended.denial = AuthorizationDenied{std::move(failure), status};
            connection_failed(std::move(ended));
            return;
        }
        if (posts_.erase(unit) == 0) return;
        events_->stream_event(unit, AuthorizationDenied{std::move(failure), status});
    }

    // The connection that was made ends: its GET and its endpoint go.
    void lose_stream() {
        loop_->remove(stream_unit_);
        state_ = State::lost;
        endpoint_url_.reset();
    }

    // ---- the client's messages

    void post(Outgoing outgoing) {
        switch (state_) {
            case State::connected: submit_post(std::move(outgoing)); return;
            case State::connecting:
                if (!made_) {
                    if (stream_held_) hold_waiting(outgoing.unit);
                    queued_.push_back(std::move(outgoing));
                    return;
                }
                refuse(outgoing.unit);
                return;
            case State::lost:
            case State::failed: refuse(outgoing.unit); return;
        }
    }

    void submit_post(Outgoing outgoing) {
        const UnitId unit = outgoing.unit;
        TransferRequest request;
        request.unit = unit;
        request.url = *endpoint_url_;
        request.method = TransferMethod::post;
        request.discard_body = true;
        Lines lines;
        lines.add(field_content_type, request_media_type);
        lines.lines.emplace_back(no_expect_line);
        for (const HttpHeader& header : outgoing.headers) {
            // `Authorization` is the authorizer's alone and never taken from the engine's fields.
            if (http::equal_ignoring_case(header.name, field_authorization)) continue;
            lines.add(header.name, header.value);
        }
        request.header_lines = std::move(lines.lines);
        request.body = std::move(outgoing.line);
        Post exchange;
        exchange.sent = true;
        exchange.url = request.url;
        exchange.profile = outgoing.profile;
        std::optional<std::string> problem = lines.problem;
        if (!problem) {
            if (gate_->configured()) exchange.retained = request;
            problem = stamp(request, info(post_method, unit, request.url, outgoing.profile), exchange.generation);
        }
        posts_[unit] = std::move(exchange);
        if (problem) {
            delivery_->push(unit, SendFailed{*problem});
            return;
        }
        loop_->submit(std::move(request));
    }

    // A message that cannot go out: it fails, through the queue, as the connection failed (with the head of a refusal
    // by status, so that the client reads the status, or with the authorizer's end of the refusal) or, while the
    // connection is lost, as undeliverable.
    void refuse(UnitId unit) {
        Post refused;
        refused.url = endpoint_.url;
        posts_[unit] = std::move(refused);
        if (state_ == State::failed && failure_.denial) {
            delivery_->push(unit, *failure_.denial);
            return;
        }
        if (state_ == State::failed && failure_.head) {
            delivery_->push(unit, *failure_.head);
            delivery_->push(unit, StreamEnded{});
            return;
        }
        delivery_->push(unit, SendFailed{state_ == State::failed ? failure_.cause
                                                                  : std::string("the event stream of the connection "
                                                                                "is not open")});
    }

    void end(UnitId unit) {
        gate_->forget(unit);
        held_waiting_.erase(unit);
        queued_.erase(std::remove_if(queued_.begin(), queued_.end(),
                                     [unit](const Outgoing& waiting) { return waiting.unit == unit; }),
                      queued_.end());
        if (posts_.erase(unit) != 0) loop_->remove(unit);
    }

    // What a POST met: its head only when it is no success (its body is never read); its end, as an end in order once
    // a success was seen (the server took the message, whose answer comes on the stream).
    void on_post_item(UnitId unit, TransferItem item) {
        Post& exchange = posts_.at(unit);
        TransportEvents& events = *events_;
        // What a withdrawn exchange still delivered, up to the loop's fence, belongs to no request of the unit.
        if (exchange.fenced) {
            if (std::holds_alternative<TransferWithdrawn>(item)) exchange.fenced = false;
            return;
        }
        if (std::holds_alternative<TransferWithdrawn>(item)) return;
        if (auto* head = std::get_if<TransferHead>(&item)) {
            if (!is_refusal_for_authorization(head->status)) gate_->accepted(unit);
            if (is_success(head->status)) {
                exchange.taken = true;
                exchange.retained.reset();
                return;
            }
            // A refusal of a message that went out is the gate's; that of a GET that made no connection was the
            // GET's.
            if (exchange.sent && is_refusal_for_authorization(head->status) && gate_->takes_refusals()) {
                loop_->withdraw(unit);
                exchange.fenced = true;
                const HttpRequestInfo about = info(post_method, unit, exchange.url, exchange.profile);
                const std::uint64_t generation = exchange.generation;
                gate_->refused(unit, about, generation, head->status, head->headers);
                return;
            }
            events.stream_event(unit, ResponseHead{head->status, std::move(head->media_type), std::move(head->headers)});
            return;
        }
        if (std::holds_alternative<StreamEnded>(item)) {
            posts_.erase(unit);
            events.stream_event(unit, StreamEnded{});
            return;
        }
        if (auto* lost = std::get_if<StreamLost>(&item)) {
            const bool taken = exchange.taken;
            posts_.erase(unit);
            if (taken) {
                events.stream_event(unit, StreamEnded{});
            } else {
                events.stream_event(unit, StreamLost{std::move(lost->cause)});
            }
            return;
        }
        if (auto* failed = std::get_if<SendFailed>(&item)) {
            posts_.erase(unit);
            events.stream_event(unit, SendFailed{std::move(failed->cause)});
        }
        // Bodies, events, ids and advice of a POST's answer are not read.
    }

    // ---- what the transfer thread read, on the client's thread

    void on_item(UnitId unit, TransferItem item) {
        if (closed_ || events_ == nullptr) return;
        if (gate_->owns(unit)) {
            gate_->on_fetch(unit, std::move(item));
            return;
        }
        // What the transport itself queued for a held request.
        if (auto* hold = std::get_if<AuthorizationHold>(&item)) {
            events_->stream_event(unit, *hold);
            return;
        }
        if (auto* denial = std::get_if<AuthorizationDenied>(&item)) {
            posts_.erase(unit);
            events_->stream_event(unit, std::move(*denial));
            return;
        }
        // A GET held for authorization was ended: what it still delivers is dropped.
        if (unit == stream_unit_ && !stream_held_ && (state_ == State::connecting || state_ == State::connected)) {
            on_stream_item(std::move(item));
            return;
        }
        if (posts_.count(unit) != 0) on_post_item(unit, std::move(item));
    }

    // One exchange of a message.
    struct Post {
        bool sent = false;   // it went out (else it fails as the connection did)
        bool taken = false;  // its head was a success
        std::string url;     // where it went
        profile::ProfilePtr profile;  // the profile it went out under
        // With an authorizer: the request as it went out, without its `Authorization` field, until a success; and
        // the generation of the credentials it carried.
        std::optional<TransferRequest> retained;
        std::uint64_t generation = 0;
        bool fenced = false;  // a refused exchange was withdrawn: its items are dropped until the loop's fence
        bool held = false;    // the client was told that the request is held
    };

    HttpEndpoint endpoint_;
    EventNames names_;
    UrlGuard guard_;
    std::size_t max_message_bytes_;
    std::shared_ptr<CurlGlobal> global_;  // released after the loop (declared before it)
    std::shared_ptr<Delivery> delivery_;
    std::unique_ptr<TransferLoop> loop_;
    std::unique_ptr<AuthorizationGate> gate_;  // stopped before the loop
    std::uint64_t stream_generation_ = 0;      // of the credentials the GET carried
    bool stream_held_ = false;                 // a refused GET is held by the gate
    std::set<UnitId> held_waiting_;            // messages waiting for the connection whose hold the client was told
    std::optional<asio::steady_timer> timer_;  // the clock of the endpoint event
    std::shared_ptr<const bool> alive_ = std::make_shared<const bool>(true);  // the clock's handler checks it

    TransportEvents* events_ = nullptr;
    bool started_ = false;
    bool closed_ = false;
    State state_ = State::connecting;
    bool made_ = false;  // a connection was made once
    Failure failure_;
    UnitId stream_unit_ = first_stream_unit;
    UnitId next_stream_unit_ = first_stream_unit;
    std::uint64_t attempt_ = 0;  // the connection the clock runs for
    std::optional<std::string> endpoint_url_;
    std::vector<Outgoing> queued_;  // messages that wait for the first connection
    std::map<UnitId, Post> posts_;
};

}  // namespace

Result<std::unique_ptr<Transport>> make_http_sse_transport(HttpEndpoint endpoint, HttpTransportSettings settings) {
    auto names = event_names(settings);
    if (auto* problem = std::get_if<std::string>(&names)) return settings_error(*problem);
    auto prepared = prepare_http(endpoint, settings);
    if (auto* error = std::get_if<ClientError>(&prepared)) return std::move(*error);
    HttpParts parts = std::get<HttpParts>(std::move(prepared));
    auto gate = AuthorizationGate::make(endpoint, parts.guard, settings.profiles);
    if (auto* error = std::get_if<ClientError>(&gate)) return std::move(*error);
    std::unique_ptr<Transport> transport = std::make_unique<SseTransport>(
        std::move(endpoint), std::move(parts), std::get<EventNames>(std::move(names)),
        std::get<std::unique_ptr<AuthorizationGate>>(std::move(gate)));
    return transport;
}

}  // namespace neomcp::detail::http_client
