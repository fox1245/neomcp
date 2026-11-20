// The Streamable HTTP transport of the client (neomcp/client/http.h), its factory, and the forms of Client::open that
// take an HttpEndpoint.
//
// The transport keeps, on the client's thread, what the protocol makes of the exchanges: which unit each one carries,
// the session a handshake opened, and the authorizer's part. The transfer thread (transfers.h) runs the exchanges and
// knows nothing of that. What it reads travels through one queue to the client's thread, where it is delivered in the
// order it was read, by one drain at a time, and never from inside start(), send() or close().
//
// Sessions. Under a revision whose connection state the client keeps and whose transport entry names a session header,
// the session id is read from the response to the handshake (the request whose method is the handshake the profile's
// open half writes) and sent on every later request; a handshake goes without one. An id that is not visible ASCII,
// is longer than HttpEndpoint::max_session_id_bytes or comes twice ends the handshake's exchange as lost. A request
// that carried the id and is answered with the status the profile names for an unknown session ends with
// SessionExpired, and no id is sent until a handshake opens a new session (the engine makes that handshake). When the
// client closes, a session the profile ends by a request is ended by a DELETE that carries its id, bounded by
// HttpEndpoint::delete_timeout; its answer, whatever it is, is not looked at.
//
// What each message goes out under is read from the profile the engine names on it (Outgoing::profile).
//
// The stream of the connection. When the engine asks for it, a GET that accepts an event stream and carries the
// engine's fields and the session id. Until its head arrives, requests wait (notifications and answers go at once), so
// that a server that asks the client something outside the request's own exchange, on this stream, can do so. The
// status the profile names for a stream not offered, or a 404, means that the server offers none for the session: it
// is never asked for again under that session. A 404 does not end the session even when the GET carried its id: a
// server without a route for GET answers so while the session lives, and a POST finds out whether it ended. A stream
// that is lost or refused otherwise is opened again after the server's advised wait (HttpEndpoint::retry_delay when it advised none),
// with the id of the last event it carried (one that carried no message no sooner than retry_delay, whatever was
// advised); max_resumptions reopens in a row that do not establish it give it up.
//
// Resumption. Under a revision whose streams may be resumed, an event stream that answers a request and ends before
// the answer, after it named an event id, is resumed: after the server's advised wait (never earlier;
// HttpEndpoint::retry_delay when it advised none) a GET on the same unit names the last id, and the stream goes on
// there; the client is not told of the break. A wait above max_retry_delay, or max_resumptions resumptions in a row
// that brought no message, end the request's exchange as lost. A stream without an event id, or under a revision that
// does not resume streams, is lost when it ends.
#include "client_http/streamable.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include "client_http/delivery.h"
#include "client_http/http_sse.h"
#include "client_http/transfers.h"
#include "client_http/url_guard.h"
#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/http/headers.h"
#include "neomcp/json.h"

namespace neomcp {

namespace detail::http_client {

namespace {

constexpr std::string_view post_method = "POST";
constexpr std::string_view get_method = "GET";
constexpr std::string_view delete_method = "DELETE";
constexpr std::string_view field_last_event_id = "Last-Event-ID";
constexpr std::string_view event_stream_media_type = "text/event-stream";
// The reasons, in a profile's status map, of the answers this transport tells apart.
constexpr std::string_view unknown_session_reason = "unknown_session";
constexpr std::string_view not_offered_reason = "get_stream_not_offered";
// The stream of the connection answered with this status is not offered at the endpoint, with or without a session.
constexpr int status_not_found = 404;
// The unit of the request that ends a session: the transport's own, one the engine never numbers (it numbers from
// connection_unit upwards). The GETs of the connection's stream take their own units below it.
constexpr UnitId end_session_unit = std::numeric_limits<UnitId>::max();
constexpr std::string_view field_content_type = "Content-Type";
constexpr std::string_view field_accept = "Accept";
constexpr std::string_view field_authorization = "Authorization";
constexpr std::string_view field_www_authenticate = "WWW-Authenticate";
constexpr std::string_view request_media_type = "application/json";
// Every POST accepts both forms of an answer.
constexpr std::string_view accepted_media_types = "application/json, text/event-stream";
// The line that keeps libcurl from adding `Expect: 100-continue` to a large body (a field with nothing after the colon
// is removed, not sent).
constexpr const char* no_expect_line = "Expect:";
constexpr std::string_view method_member = "method";

constexpr int status_success_first = 200;
constexpr int status_success_last = 299;
constexpr int status_unauthorized = 401;
constexpr int status_forbidden = 403;
constexpr char visible_first = 0x21;
constexpr char visible_last = 0x7E;

bool is_success(int status) { return status >= status_success_first && status <= status_success_last; }

ClientError settings_error(ClientErrorKind kind, std::string message) {
    ClientError error;
    error.kind = kind;
    error.message = std::move(message);
    return error;
}

// Where a session id comes from and the name of its header, read from the profiles.
struct SessionRules {
    std::set<std::string, std::less<>> handshake_methods;
    std::vector<std::string> header_names;  // distinct without regard to case
    std::string send_name;                  // the name a request carries the id under
};

SessionRules session_rules(const HttpTransportSettings& settings) {
    SessionRules rules;
    std::vector<profile::ProfilePtr> profiles = settings.profiles;
    if (profiles.empty()) {
        for (const auto revision : profile::builtin_revisions()) profiles.push_back(profile::builtin(revision));
    }
    const ClientMechanismRegistry& registry =
        settings.mechanisms != nullptr ? *settings.mechanisms : builtin_client_mechanisms();
    auto selected = select_client_mechanisms(profiles, profile::Transport::streamable_http, registry);
    // Profiles the client cannot speak make the engine refuse the client; the transport then is never started.
    if (!std::holds_alternative<std::vector<ClientMechanisms>>(selected)) return rules;
    for (const ClientMechanisms& halves : std::get<std::vector<ClientMechanisms>>(selected)) {
        const auto& http = halves.profile->streamable_http();
        if (!http || !http->session_header) continue;
        if (!halves.open->handshakes() || !halves.state->keeps_connection()) continue;
        const auto handshake = halves.open->handshake(*halves.profile, ClientSelf{});
        if (!handshake) continue;
        rules.handshake_methods.insert(handshake->method);
        const std::string& name = *http->session_header;
        bool known = false;
        for (const std::string& seen : rules.header_names) known = known || http::equal_ignoring_case(seen, name);
        if (!known) rules.header_names.push_back(name);
        rules.send_name = name;  // the newest revision's spelling (revisions come oldest first)
    }
    return rules;
}

class StreamableTransport final : public Transport {
public:
    StreamableTransport(HttpEndpoint endpoint, UrlGuard guard, SessionRules rules, TransferLimits limits,
                        std::shared_ptr<CurlGlobal> global)
        : max_message_bytes_(limits.max_message_bytes),
          endpoint_(std::move(endpoint)),
          rules_(std::move(rules)),
          global_(std::move(global)),
          delivery_(std::make_shared<Delivery>()),
          loop_(std::make_unique<TransferLoop>(std::move(limits), std::move(guard),
                                               [delivery = delivery_](UnitId unit, TransferItem item) {
                                                   delivery->push(unit, std::move(item));
                                               })) {}

    ~StreamableTransport() override {
        loop_->stop();
        delivery_->detach();
    }

    profile::Transport kind() const noexcept override { return profile::Transport::streamable_http; }

    ConnectionStreamMode connection_stream() const noexcept override { return endpoint_.connection_stream; }

    std::optional<std::size_t> max_message_bytes() const noexcept override { return max_message_bytes_; }

    std::optional<std::string> start(TransportEvents& events) override {
        if (started_) throw std::logic_error("an HTTP transport was started twice");
        started_ = true;
        events_ = &events;
        delivery_->attach(events.executor(), [this](UnitId unit, TransferItem item) { on_item(unit, std::move(item)); });
        return loop_->start();
    }

    void send(Outgoing outgoing) override {
        if (closed_ || !started_) return;
        switch (outgoing.kind) {
            case OutgoingKind::request:
            case OutgoingKind::notification:
            case OutgoingKind::response: submit(std::move(outgoing)); return;
            case OutgoingKind::end_unit: end(outgoing.unit); return;
            case OutgoingKind::open_connection_stream: open_connection(std::move(outgoing)); return;
            case OutgoingKind::restart:
            case OutgoingKind::close: return;
        }
    }

    // Ends every exchange; then, when the client closes a session the revision ends by a request, makes that request
    // and waits for its answer (any answer: a server may refuse to end sessions so) at most delete_timeout.
    asio::awaitable<void> close(CloseCause cause) override {
        if (closed_) co_return;
        closed_ = true;
        if (!started_ || events_ == nullptr) co_return;
        for (const auto& [unit, state] : units_) loop_->remove(unit);
        units_.clear();
        deferred_.clear();
        drop_connection();
        if (cause == CloseCause::client_closed && session_ends_by_request()) {
            asio::steady_timer answered(events_->executor());
            answered.expires_after(endpoint_.delete_timeout);
            delete_wait_ = &answered;
            loop_->submit(end_session_request());
            std::error_code woken;
            co_await answered.async_wait(asio::redirect_error(asio::use_awaitable, woken));
            delete_wait_ = nullptr;
        }
        loop_->stop();
        delivery_->detach();
        events_ = nullptr;
        co_return;
    }

private:
    // One exchange the engine opened: a request, a notification or an answer, and how its answer stream is resumed.
    struct UnitState {
        OutgoingKind kind = OutgoingKind::request;
        bool handshake = false;
        profile::ProfilePtr profile;             // the one it went out under; null when the engine named none
        std::optional<std::string> session;      // the session id the request carried
        std::vector<HttpHeader> resume_fields;   // the engine's fields a resumption of its stream carries as well
        bool stream = false;                     // its answer is an event stream that began
        bool resuming = false;                   // its exchange now is a resumption
        std::optional<std::string> last_event_id;
        std::optional<std::chrono::milliseconds> retry;  // the server's advice on the wait before resuming
        std::size_t resumptions = 0;             // in a row, since the last message
    };

    // The stream of the connection: a GET the engine asked for, kept (and opened again after a loss) for the session.
    struct ConnectionState {
        bool asked = false;    // the engine asked for it under the current session
        bool refused = false;  // the server does not offer it for this session (or it was given up): never again
        bool running = false;  // a GET runs, or waits for its start
        UnitId transfer = connection_unit;  // the loop's unit of the GET that runs: one of the transport's own
        profile::ProfilePtr profile;
        std::vector<HttpHeader> fields;          // the engine's fields for it
        std::optional<std::string> session;      // the session id the running GET carries
        std::optional<std::string> last_event_id;
        std::optional<std::chrono::milliseconds> retry;
        std::size_t reopens = 0;    // in a row that did not establish the stream
        bool carried = false;       // the running GET carried a message
    };

    using Clock = std::chrono::steady_clock;

    HttpRequestInfo info(std::string_view method, UnitId unit) const {
        return HttpRequestInfo{std::string(method), endpoint_.url, unit};
    }

    bool is_handshake(const Outgoing& outgoing) const {
        if (outgoing.kind != OutgoingKind::request || rules_.handshake_methods.empty()) return false;
        const json message = json::parse(outgoing.line, nullptr, false);
        const auto* members = message.get_ptr<const json::object_t*>();
        if (members == nullptr) return false;
        const auto found = members->find(std::string(method_member));
        if (found == members->end() || !found->second.is_string()) return false;
        return rules_.handshake_methods.count(found->second.get_ref<const std::string&>()) > 0;
    }

    // The header lines of a request: the transport's own fields, the engine's, the session's and the authorizer's.
    // Why one cannot be sent, otherwise.
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

    void authorize(Lines& lines, std::string_view method, UnitId unit) const {
        if (!endpoint_.authorizer) return;
        const std::string value = endpoint_.authorizer->authorization(info(method, unit));
        if (!value.empty()) lines.add(field_authorization, value);
    }

    // The engine's fields that every message of the profile carries (the revision, where a header names it): what a
    // request outside the engine's messages (a resumption, the end of a session) carries of them.
    static std::vector<HttpHeader> standing_fields(const std::vector<HttpHeader>& fields,
                                                   const profile::ProfilePtr& profile) {
        std::vector<HttpHeader> kept;
        if (!profile || !profile->streamable_http()) return kept;
        const auto& http = *profile->streamable_http();
        for (const HttpHeader& field : fields) {
            bool standing = http.version_header && http::equal_ignoring_case(field.name, http.version_header->name);
            for (const auto& header : http.request_headers) {
                standing = standing || (header.scope == profile::HeaderScopeKind::every_message &&
                                        http::equal_ignoring_case(field.name, header.name));
            }
            if (standing) kept.push_back(field);
        }
        return kept;
    }

    // The status the profile names for a reason; empty without a profile or without such a status.
    static std::optional<int> status_for(const profile::ProfilePtr& profile, std::string_view reason) {
        if (!profile || !profile->streamable_http()) return std::nullopt;
        return profile->streamable_http()->status_for(reason);
    }

    static bool resumable(const profile::ProfilePtr& profile) {
        return profile && profile->streamable_http() && profile->streamable_http()->resumable;
    }

    static bool is_stream(const TransferHead& head) {
        return is_success(head.status) && head.media_type && *head.media_type == event_stream_media_type;
    }

    void submit(Outgoing outgoing) {
        const UnitId unit = outgoing.unit;
        UnitState state;
        state.kind = outgoing.kind;
        state.handshake = is_handshake(outgoing);
        state.profile = outgoing.profile;
        state.resume_fields = standing_fields(outgoing.headers, outgoing.profile);
        TransferRequest request;
        request.unit = unit;
        request.url = endpoint_.url;
        Lines lines;
        lines.add(field_content_type, request_media_type);
        lines.add(field_accept, accepted_media_types);
        lines.lines.emplace_back(no_expect_line);
        for (const HttpHeader& header : outgoing.headers) lines.add(header.name, header.value);
        // A handshake starts a session of its own.
        if (session_id_ && !state.handshake) {
            lines.add(session_name(), *session_id_);
            state.session = session_id_;
        }
        authorize(lines, post_method, unit);
        request.header_lines = std::move(lines.lines);
        request.body = std::move(outgoing.line);
        const bool waits = outgoing.kind == OutgoingKind::request && holding_;
        units_[unit] = std::move(state);
        if (lines.problem) {
            delivery_->push(unit, SendFailed{*lines.problem});
            return;
        }
        if (waits) {
            deferred_.push_back(std::move(request));
            return;
        }
        loop_->submit(std::move(request));
    }

    void end(UnitId unit) {
        if (unit == connection_unit) return;
        const auto found = units_.find(unit);
        if (found == units_.end()) return;
        units_.erase(found);
        deferred_.erase(std::remove_if(deferred_.begin(), deferred_.end(),
                                       [unit](const TransferRequest& waiting) { return waiting.unit == unit; }),
                        deferred_.end());
        loop_->remove(unit);
    }

    // The requests that waited for the head of the connection's first stream go out.
    void release_deferred() {
        holding_ = false;
        std::vector<TransferRequest> waiting;
        waiting.swap(deferred_);
        for (TransferRequest& request : waiting) {
            if (units_.count(request.unit) != 0) loop_->submit(std::move(request));
        }
    }

    // ---- sessions

    // The session id of a handshake's answer; why it cannot be used, otherwise. A handshake's answer opens a new
    // connection, whose stream the engine asks for anew.
    std::optional<std::string> adopt_session(const std::vector<HttpHeader>& headers,
                                             const profile::ProfilePtr& profile) {
        drop_connection();
        connection_ = ConnectionState{};
        std::vector<std::string_view> values;
        for (const HttpHeader& header : headers) {
            for (const std::string& name : rules_.header_names) {
                if (http::equal_ignoring_case(header.name, name)) values.push_back(header.value);
            }
        }
        if (values.empty()) {
            session_id_.reset();
            session_profile_.reset();
            return std::nullopt;
        }
        if (values.size() > 1) return std::string("the server's answer to the handshake names two session ids");
        const std::string_view id = values.front();
        if (id.empty() || id.size() > endpoint_.max_session_id_bytes) {
            return std::string("the server's session id is empty or longer than max_session_id_bytes");
        }
        for (const char c : id) {
            if (c < visible_first || c > visible_last) {
                return std::string("the server's session id has a character that is not visible ASCII");
            }
        }
        session_id_ = std::string(id);
        session_profile_ = profile;
        return std::nullopt;
    }

    // The server no longer knows the session `id`: no id is sent until a handshake opens a new session. Whether it
    // was the session in use.
    bool forget_session(const std::string& id) {
        if (!session_id_ || *session_id_ != id) return false;
        session_id_.reset();
        session_profile_.reset();
        drop_connection();
        connection_ = ConnectionState{};
        return true;
    }

    // The name the session id goes under: the one the profile of the session's handshake gives it.
    const std::string& session_name() const {
        const auto* http = session_profile_ ? &session_profile_->streamable_http() : nullptr;
        if (http != nullptr && *http && (*http)->session_header) return *(*http)->session_header;
        return rules_.send_name;
    }

    bool session_ends_by_request() const {
        return session_id_ && session_profile_ && session_profile_->streamable_http() &&
               session_profile_->streamable_http()->delete_ends_session;
    }

    // The request that ends the session: DELETE with the session id and the revision's header, bounded as a whole.
    TransferRequest end_session_request() const {
        TransferRequest request;
        request.unit = end_session_unit;
        request.url = endpoint_.url;
        request.method = TransferMethod::remove;
        request.total_timeout = endpoint_.delete_timeout;
        Lines lines;
        lines.add(session_name(), *session_id_);
        const auto& http = *session_profile_->streamable_http();
        if (http.version_header) lines.add(http.version_header->name, session_profile_->revision());
        authorize(lines, delete_method, end_session_unit);
        request.header_lines = std::move(lines.lines);  // a field that cannot be sent is left out
        return request;
    }

    // ---- the stream of the connection

    void open_connection(Outgoing outgoing) {
        if (connection_.refused || connection_.running) return;
        connection_.asked = true;
        connection_.profile = std::move(outgoing.profile);
        connection_.fields = std::move(outgoing.headers);
        connection_.reopens = 0;
        // Until its head arrives the server may not yet be able to speak there: a request it answers by asking the
        // client outside that request's own exchange would be asked into nothing. So requests wait for it.
        holding_ = true;
        start_connection(Clock::now());
    }

    void start_connection(Clock::time_point not_before) {
        TransferRequest request;
        request.unit = next_private_unit_--;
        request.url = endpoint_.url;
        request.method = TransferMethod::get;
        request.not_before = not_before;
        Lines lines;
        lines.add(field_accept, event_stream_media_type);
        for (const HttpHeader& header : connection_.fields) lines.add(header.name, header.value);
        if (session_id_) lines.add(session_name(), *session_id_);
        if (connection_.last_event_id) lines.add(field_last_event_id, *connection_.last_event_id);
        authorize(lines, get_method, connection_unit);
        connection_.session = session_id_;
        connection_.transfer = request.unit;
        connection_.running = true;
        connection_.carried = false;
        if (lines.problem) {
            connection_.running = false;
            connection_.refused = true;
            release_deferred();
            return;
        }
        request.header_lines = std::move(lines.lines);
        loop_->submit(std::move(request));
    }

    // Ends the GET that runs, if one does; nothing more of it is delivered.
    void drop_connection() {
        if (connection_.running) loop_->remove(connection_.transfer);
        connection_.running = false;
        if (holding_) release_deferred();
    }

    // The stream was lost or refused in a way that may pass: it is opened again after the server's advised wait,
    // until max_resumptions reopens in a row did not establish it. Then it is given up for the session. A stream that
    // carried no message is not opened again sooner than retry_delay, whatever the server advised, so that a server
    // that ends each stream at once is asked at that pace and not without pause.
    void reopen_connection() {
        if (connection_.refused || !connection_.asked || connection_.session != session_id_) return;
        auto delay = connection_.retry.value_or(endpoint_.retry_delay);
        if (!connection_.carried) delay = std::max(delay, endpoint_.retry_delay);
        if (delay > endpoint_.max_retry_delay || connection_.reopens >= endpoint_.max_resumptions) {
            connection_.refused = true;
            events_->stream_event(connection_unit,
                                  StreamLost{"the stream of the connection was lost and could not be opened again"});
            return;
        }
        ++connection_.reopens;
        start_connection(Clock::now() + delay);
    }

    void on_connection_item(TransferItem item) {
        TransportEvents& events = *events_;
        if (auto* head = std::get_if<TransferHead>(&item)) {
            if (is_stream(*head)) {
                connection_.reopens = 0;
                if (holding_) release_deferred();
                return;
            }
            loop_->remove(connection_.transfer);
            connection_.running = false;
            if (holding_) release_deferred();
            const int status = head->status;
            // A 404 is taken as no stream at the endpoint even when the GET carried the session id: a server without
            // a route for GET answers so while the session lives. Whether the session ended, a POST finds out.
            if (status_for(connection_.profile, not_offered_reason) == status || status == status_not_found) {
                connection_.refused = true;  // not offered for this session: never asked again
                events.stream_event(connection_unit,
                                    StreamLost{"the server offers no stream of the connection for this session"});
                return;
            }
            if (status == status_unauthorized || status == status_forbidden) {
                challenge(get_method, connection_unit, *head);
                connection_.refused = true;
                events.stream_event(connection_unit, StreamLost{"the server refused the stream of the connection for "
                                                                "want of authorization"});
                return;
            }
            reopen_connection();
            return;
        }
        if (auto* message = std::get_if<TransferMessage>(&item)) {
            connection_.carried = true;
            static_cast<void>(events.received(connection_unit, message->text));
            return;
        }
        if (auto* id = std::get_if<StreamEventId>(&item)) {
            connection_.last_event_id = std::move(id->id);
            return;
        }
        if (auto* advice = std::get_if<StreamRetryAdvice>(&item)) {
            connection_.retry = advice->delay;
            return;
        }
        connection_.running = false;
        if (holding_) release_deferred();
        reopen_connection();
    }

    void challenge(std::string_view method, UnitId unit, const TransferHead& head) {
        if (!endpoint_.authorizer) return;
        std::vector<std::string> challenges;
        for (const HttpHeader& header : head.headers) {
            if (http::equal_ignoring_case(header.name, field_www_authenticate)) challenges.push_back(header.value);
        }
        endpoint_.authorizer->challenge(info(method, unit), head.status, challenges);
    }

    // ---- resuming a request's stream

    // Resumes the stream of a request that ended before its answer: a GET that names the last event id, once the
    // server's advised wait passed. Why it cannot be resumed, otherwise.
    std::optional<std::string> resume(UnitId unit, UnitState& state) {
        const auto delay = state.retry.value_or(endpoint_.retry_delay);
        if (delay > endpoint_.max_retry_delay) {
            return std::string("the server advised a wait before resuming the stream longer than max_retry_delay");
        }
        if (state.resumptions >= endpoint_.max_resumptions) {
            return std::string("the stream was resumed max_resumptions times in a row without a new message");
        }
        TransferRequest request;
        request.unit = unit;
        request.url = endpoint_.url;
        request.method = TransferMethod::get;
        request.not_before = Clock::now() + delay;
        Lines lines;
        lines.add(field_accept, event_stream_media_type);
        for (const HttpHeader& header : state.resume_fields) lines.add(header.name, header.value);
        if (state.session) lines.add(session_name(), *state.session);
        lines.add(field_last_event_id, *state.last_event_id);
        authorize(lines, get_method, unit);
        if (lines.problem) return lines.problem;
        request.header_lines = std::move(lines.lines);
        ++state.resumptions;
        state.resuming = true;
        loop_->submit(std::move(request));
        return std::nullopt;
    }

    // ---- what the transfer thread read, on the client's thread

    // The transport's own part first, then the client's, last: the client may call back into the transport while it
    // is told, so nothing of a unit is touched after the client was told.
    void on_item(UnitId unit, TransferItem item) {
        if (closed_) {
            const bool ended = std::holds_alternative<StreamEnded>(item) || std::holds_alternative<StreamLost>(item) ||
                               std::holds_alternative<SendFailed>(item);
            if (unit == end_session_unit && ended && delete_wait_ != nullptr) delete_wait_->cancel();
            return;
        }
        if (events_ == nullptr) return;
        if (connection_.running && unit == connection_.transfer) {
            on_connection_item(std::move(item));
            return;
        }
        const auto found = units_.find(unit);
        if (found == units_.end()) return;
        UnitState& state = found->second;
        TransportEvents& events = *events_;
        if (auto* head = std::get_if<TransferHead>(&item)) {
            if (state.resuming) {
                on_resumed_head(unit, state, *head);
                return;
            }
            if (state.handshake && is_success(head->status)) {
                if (auto problem = adopt_session(head->headers, state.profile)) {
                    units_.erase(found);
                    loop_->remove(unit);
                    events.stream_event(unit, StreamLost{std::move(*problem)});
                    return;
                }
            }
            if (state.session && status_for(state.profile, unknown_session_reason) == head->status) {
                const bool current = forget_session(*state.session);
                units_.erase(found);
                loop_->remove(unit);
                events.stream_event(unit, SessionExpired{current});
                return;
            }
            if (head->status == status_unauthorized || head->status == status_forbidden) {
                challenge(post_method, unit, *head);
            }
            state.stream = is_stream(*head);
            events.stream_event(unit,
                                ResponseHead{head->status, std::move(head->media_type), std::move(head->headers)});
            return;
        }
        if (auto* message = std::get_if<TransferMessage>(&item)) {
            state.resumptions = 0;
            static_cast<void>(events.received(unit, message->text));
            return;
        }
        if (auto* id = std::get_if<StreamEventId>(&item)) {
            state.last_event_id = id->id;
            events.stream_event(unit, std::move(*id));
            return;
        }
        if (auto* advice = std::get_if<StreamRetryAdvice>(&item)) {
            state.retry = advice->delay;
            events.stream_event(unit, *advice);
            return;
        }
        // The exchange ended. A request whose answer stream ended before the answer is resumed where the revision
        // resumes streams and an event id was seen; otherwise the client is told.
        if (state.kind == OutgoingKind::request && state.stream && state.last_event_id && resumable(state.profile)) {
            auto refusal = resume(unit, state);
            if (!refusal) return;
            units_.erase(found);
            events.stream_event(unit, StreamLost{std::move(*refusal)});
            return;
        }
        units_.erase(found);
        std::visit(
            [&](auto& event) {
                using Event = std::decay_t<decltype(event)>;
                if constexpr (!std::is_same_v<Event, TransferHead> && !std::is_same_v<Event, TransferMessage> &&
                              !std::is_same_v<Event, TransferEvent>) {
                    events.stream_event(unit, std::move(event));
                }
            },
            item);
    }

    // The head of a resumption: a stream goes on as the one it resumes (its head is not told again); any other answer
    // ends the request's exchange.
    void on_resumed_head(UnitId unit, UnitState& state, const TransferHead& head) {
        if (is_stream(head)) return;
        TransportEvents& events = *events_;
        const std::optional<std::string> session = state.session;
        const profile::ProfilePtr profile = state.profile;
        units_.erase(unit);
        loop_->remove(unit);
        if (session && status_for(profile, unknown_session_reason) == head.status) {
            // The session is gone, and with it the stream. The request was taken, so it is not sent again here.
            const bool current = forget_session(*session);
            events.stream_event(connection_unit, SessionExpired{current});
            events.stream_event(unit, StreamLost{"the session of the stream ended before its answer"});
            return;
        }
        if (head.status == status_unauthorized || head.status == status_forbidden) {
            challenge(get_method, unit, head);
            events.stream_event(unit, ResponseHead{head.status, head.media_type, head.headers});
            return;
        }
        events.stream_event(unit, StreamLost{"the server answered the stream's resumption without a stream"});
    }

    std::size_t max_message_bytes_;  // of a JSON body and of one event's data
    HttpEndpoint endpoint_;
    SessionRules rules_;
    std::shared_ptr<CurlGlobal> global_;  // released after the loop (declared before it)
    std::shared_ptr<Delivery> delivery_;
    std::unique_ptr<TransferLoop> loop_;

    TransportEvents* events_ = nullptr;
    bool started_ = false;
    bool closed_ = false;
    std::map<UnitId, UnitState> units_;
    std::optional<std::string> session_id_;
    profile::ProfilePtr session_profile_;  // the profile of the handshake that opened the session
    ConnectionState connection_;
    bool holding_ = false;                    // requests wait for the head of the connection's stream
    std::vector<TransferRequest> deferred_;   // those requests
    UnitId next_private_unit_ = end_session_unit - 1;
    asio::steady_timer* delete_wait_ = nullptr;  // close() waits on it for the end of the session's request
};

}  // namespace

std::variant<HttpParts, ClientError> prepare_http(const HttpEndpoint& endpoint, const HttpTransportSettings& settings) {
    auto guard = UrlGuard::for_endpoint(endpoint.url);
    if (auto* problem = std::get_if<std::string>(&guard)) {
        return settings_error(ClientErrorKind::invalid_options, *problem);
    }
    if (endpoint.proxy.mode == ProxyMode::named && endpoint.proxy.url.empty()) {
        return settings_error(ClientErrorKind::invalid_options, "a named proxy needs its URL");
    }
    if (endpoint.ca_file) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(*endpoint.ca_file, error)) {
            return settings_error(ClientErrorKind::invalid_options, "the file of certificate authorities does not exist");
        }
    }
    if (endpoint.connect_timeout.count() <= 0) {
        return settings_error(ClientErrorKind::invalid_options, "connect_timeout is not positive");
    }
    if (endpoint.stream_idle_timeout.count() <= 0) {
        return settings_error(ClientErrorKind::invalid_options, "stream_idle_timeout is not positive");
    }
    if (endpoint.retry_delay.count() < 0 || endpoint.max_retry_delay.count() < 0) {
        return settings_error(ClientErrorKind::invalid_options, "retry_delay or max_retry_delay is negative");
    }
    if (endpoint.delete_timeout.count() <= 0) {
        return settings_error(ClientErrorKind::invalid_options, "delete_timeout is not positive");
    }
    if (endpoint.endpoint_timeout.count() <= 0) {
        return settings_error(ClientErrorKind::invalid_options, "endpoint_timeout is not positive");
    }
    if (endpoint.max_response_header_bytes == 0) {
        return settings_error(ClientErrorKind::invalid_options, "max_response_header_bytes is zero");
    }
    if (endpoint.max_session_id_bytes == 0) {
        return settings_error(ClientErrorKind::invalid_options, "max_session_id_bytes is zero");
    }
    if (settings.max_message_bytes == 0) {
        return settings_error(ClientErrorKind::invalid_options, "max_message_bytes is zero");
    }
    auto global = std::make_shared<CurlGlobal>();
    if (!global->ok()) {
        return settings_error(ClientErrorKind::transport, "the HTTP library could not be initialized");
    }
    if (auto refusal = resolver_refusal(endpoint.allow_blocking_resolver)) {
        return settings_error(ClientErrorKind::transport, std::move(*refusal));
    }
    TransferLimits limits;
    limits.max_message_bytes = settings.max_message_bytes;
    limits.max_response_header_bytes = endpoint.max_response_header_bytes;
    limits.connect_timeout = endpoint.connect_timeout;
    limits.stream_idle_timeout = endpoint.stream_idle_timeout;
    limits.max_redirects = endpoint.max_redirects;
    limits.proxy = endpoint.proxy;
    limits.ca_file = endpoint.ca_file;
    return HttpParts{std::get<UrlGuard>(std::move(guard)), std::move(limits), std::move(global)};
}

Result<std::unique_ptr<Transport>> make_http_transport(HttpEndpoint endpoint, HttpTransportSettings settings) {
    auto prepared = prepare_http(endpoint, settings);
    if (auto* error = std::get_if<ClientError>(&prepared)) return std::move(*error);
    HttpParts parts = std::get<HttpParts>(std::move(prepared));
    SessionRules rules = session_rules(settings);
    std::unique_ptr<Transport> transport =
        std::make_unique<StreamableTransport>(std::move(endpoint), std::move(parts.guard), std::move(rules),
                                              std::move(parts.limits), std::move(parts.global));
    return transport;
}

}  // namespace detail::http_client

namespace {

using detail::http_client::HttpTransportSettings;
using detail::http_client::make_http_sse_transport;
using detail::http_client::make_http_transport;

HttpTransportSettings settings_of(const ClientOptions& options) {
    HttpTransportSettings settings;
    settings.profiles = options.profiles;
    settings.mechanisms = options.mechanisms;
    settings.max_message_bytes = options.max_message_bytes;
    return settings;
}

// Opening an HTTP endpoint. Streamable HTTP is tried first; when its handshake is refused by a status after which the
// endpoint may serve the older transport of events over HTTP (ClientError::next_transport), that transport is tried on
// the same endpoint, with a client of its own: the transports differ in where every answer comes, so the engine that
// spoke the first cannot speak the second. A verdict of the older transport (ClientOptions::prior) goes there
// directly; when it fails there, the endpoint is asked once more from the start, without the verdict. An endpoint
// whose enabled revisions define only the older transport goes there directly too.
struct HttpPlan {
    bool streamable = false;  // an enabled profile defines Streamable HTTP
    bool events = false;      // an enabled profile defines the transport of events over HTTP
    bool events_first = false;
    bool prior_events = false;  // the verdict names the transport of events over HTTP
};

HttpPlan plan_of(const ClientOptions& options) {
    std::vector<profile::ProfilePtr> profiles = options.profiles;
    if (profiles.empty()) {
        for (const auto revision : profile::builtin_revisions()) profiles.push_back(profile::builtin(revision));
    }
    HttpPlan plan;
    for (const profile::ProfilePtr& one : profiles) {
        if (!one) continue;
        plan.streamable = plan.streamable || one->streamable_http().has_value();
        plan.events = plan.events || one->http_sse().has_value();
    }
    plan.prior_events = plan.events && options.prior && options.prior->transport == profile::Transport::http_sse;
    plan.events_first = plan.prior_events || (plan.events && !plan.streamable);
    return plan;
}

// Whether a verdict that failed is worth a second look from the start: not when the options cannot work, nor when the
// server wants authorization the client cannot give (it would want it on every step).
bool probe_again(const ClientError& error) {
    return error.kind != ClientErrorKind::invalid_options && error.kind != ClientErrorKind::authorization_required;
}

// The failure of both transports: the second's kind, and what each met.
ClientError both_failed(const ClientError& first, ClientError second) {
    second.message = first.message + "; then, over the transport of events over HTTP: " + second.message;
    second.next_transport.reset();
    return second;
}

Result<Client> open_streamable(ClientOptions options, HttpEndpoint endpoint) {
    auto transport = make_http_transport(std::move(endpoint), settings_of(options));
    if (!transport) return std::move(transport).error();
    return Client::open(std::move(options), std::move(transport).value());
}

Result<Client> open_events(ClientOptions options, HttpEndpoint endpoint) {
    auto transport = make_http_sse_transport(std::move(endpoint), settings_of(options));
    if (!transport) return std::move(transport).error();
    return Client::open(std::move(options), std::move(transport).value());
}

asio::awaitable<Result<Client>> async_open_streamable(ClientOptions options, HttpEndpoint endpoint) {
    auto transport = make_http_transport(std::move(endpoint), settings_of(options));
    if (!transport) {
        Result<Client> failed(std::move(transport).error());
        co_return failed;
    }
    Result<Client> opened = co_await Client::async_open(std::move(options), std::move(transport).value());
    co_return opened;
}

asio::awaitable<Result<Client>> async_open_events(ClientOptions options, HttpEndpoint endpoint) {
    auto transport = make_http_sse_transport(std::move(endpoint), settings_of(options));
    if (!transport) {
        Result<Client> failed(std::move(transport).error());
        co_return failed;
    }
    Result<Client> opened = co_await Client::async_open(std::move(options), std::move(transport).value());
    co_return opened;
}

bool hands_over(const Result<Client>& client) {
    return !client && client.error().next_transport == profile::Transport::http_sse;
}

}  // namespace

Result<std::unique_ptr<Transport>> make_transport(HttpEndpoint endpoint) {
    return make_http_transport(std::move(endpoint), HttpTransportSettings{});
}

Result<Client> Client::open(ClientOptions options, HttpEndpoint endpoint) {
    const HttpPlan plan = plan_of(options);
    if (plan.events_first) {
        Result<Client> client = open_events(options, endpoint);
        if (client || !plan.prior_events || !plan.streamable || !probe_again(client.error())) return client;
        options.prior.reset();
    }
    if (!plan.events) return open_streamable(std::move(options), std::move(endpoint));
    Result<Client> client = open_streamable(options, endpoint);
    if (!hands_over(client)) return client;
    Result<Client> events = open_events(std::move(options), std::move(endpoint));
    if (events) return events;
    return both_failed(client.error(), events.error());
}

asio::awaitable<Result<Client>> Client::async_open(ClientOptions options, HttpEndpoint endpoint) {
    const HttpPlan plan = plan_of(options);
    if (plan.events_first) {
        Result<Client> client = co_await async_open_events(options, endpoint);
        if (client || !plan.prior_events || !plan.streamable || !probe_again(client.error())) co_return client;
        options.prior.reset();
    }
    if (!plan.events) {
        Result<Client> client = co_await async_open_streamable(std::move(options), std::move(endpoint));
        co_return client;
    }
    Result<Client> client = co_await async_open_streamable(options, endpoint);
    if (!hands_over(client)) co_return client;
    Result<Client> events = co_await async_open_events(std::move(options), std::move(endpoint));
    if (events) co_return events;
    Result<Client> failed(both_failed(client.error(), events.error()));
    co_return failed;
}

}  // namespace neomcp
