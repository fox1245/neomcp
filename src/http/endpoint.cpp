// The Streamable HTTP endpoint.
//
// Objects and ownership:
// - Impl holds the endpoint's state. Everything that calls back later (a session's sink, a job of the timer, a
//   responder's disconnect callback, a stream's close callback) holds it weakly.
// - Entry is one session, kept under an id or not. Impl holds every live entry, and the kept ones by id as well. An
//   entry owns its Session; the session's sink holds the entry weakly.
// - Exchange is one POST waiting for its answer. Its entry maps the unit number to it until the exchange is released,
//   and it holds its entry, so a session that has no id lives exactly as long as its one request.
//
// Locks, in the order they may be nested: Impl::mutex, Entry::mutex, Exchange::mutex, the timer's. None is held
// while a session, a responder, a stream or the policy is called. Two entries' locks are never held at once.
//
// Sessions nobody used. A kept entry is listed in Impl::unused, in the order it was kept, until a request names it
// (find) or it ends; the list and the entry's place in it are guarded by Impl::mutex, so a newcomer that takes the
// place of the oldest listed entry (keep_entry) and the first request of that entry are decided one after the other.
//
// Feeding. A session takes one input at a time, and requests of one session arrive on many threads. The inputs of an
// entry queue in it, and the thread that finds nobody feeding feeds them all, one after the other, without a lock
// held during Session::receive. The outputs of an input that arrive before receive() returned are held by its
// exchange: whether the session is kept, and so whether the answer carries a session id, is known only then.
//
// Answering. Every output of a unit goes to its exchange. What the exchange does about it (complete the responder,
// append to its stream, end the stream, abandon the unit, release itself) is decided under the exchange's lock and
// queued as steps; the thread that finds nobody running the steps runs them in order, without the lock. So the effects
// of one request happen in the order they were decided, whichever thread decided them: the session's writer, the
// feeder, the timer, or a thread of the listener.
#include "neomcp/http/endpoint.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "http/timer.h"
#include "neomcp/http/media_type.h"
#include "neomcp/http/random.h"
#include "neomcp/http/sse.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "neomcp/session.h"

namespace neomcp::http {

namespace {

using Clock = std::chrono::steady_clock;

// Why the endpoint answers as it does. The reasons a revision's status map can name are spelled as
// profile::declared_status_reasons() spells them (the session's own refusals as neomcp::to_string(RefusalReason));
// the others are the endpoint's.
namespace reason {
// Named by the revisions.
constexpr std::string_view missing_session = "missing_session";
constexpr std::string_view unknown_session = "unknown_session";
constexpr std::string_view accepted = "accepted";
constexpr std::string_view notification_refused = "notification_refused";
constexpr std::string_view origin_rejected = "origin_rejected";
constexpr std::string_view get_stream_not_offered = "get_stream_not_offered";
constexpr std::string_view delete_not_allowed = "delete_not_allowed";
constexpr std::string_view unsupported_version_header = "unsupported_version_header";
// The endpoint's own.
constexpr std::string_view answered = "answered";                // a result, or an error answer that refused nothing
constexpr std::string_view session_ended = "session_ended";      // a DELETE ended the session
constexpr std::string_view host_rejected = "host_rejected";      // the Host header is missing, repeated or not allowed
constexpr std::string_view method_not_allowed = "method_not_allowed";  // not POST, GET or DELETE
constexpr std::string_view not_acceptable = "not_acceptable";    // Accept admits nothing the endpoint can answer with
constexpr std::string_view stream_conflict = "stream_conflict";  // a second connection stream of one session
constexpr std::string_view body_too_large = "body_too_large";
constexpr std::string_view unsupported_media_type = "unsupported_media_type";
constexpr std::string_view malformed_session_header = "malformed_session_header";  // more than one session id
constexpr std::string_view no_capacity = "no_capacity";          // no room for a session, or no id could be made for it
constexpr std::string_view stopping = "stopping";                // the endpoint stops
constexpr std::string_view defect = "defect";                    // the library failed
}  // namespace reason

struct NamedDefault {
    std::string_view reason;
    int status;
};

// The status the library answers with where the revision that governs a request prescribes none. This is the only
// place the endpoint's statuses are written down; every other status comes from a profile.
constexpr std::array library_defaults{
    // A session refused the input before any method ran.
    NamedDefault{"parse_error", 400},
    NamedDefault{"invalid_request", 400},
    NamedDefault{"too_long", 413},
    NamedDefault{"too_deep", 400},
    NamedDefault{"header_mismatch", 400},
    NamedDefault{"unsupported_version", 400},
    NamedDefault{"invalid_request_meta", 400},
    NamedDefault{"unsupported_version_header", 400},
    NamedDefault{"method_not_found", 200},  // where the revision prescribes nothing, an ordinary JSON-RPC error
    NamedDefault{"missing_connection", 400},
    NamedDefault{"missing_client_capability", 400},
    NamedDefault{"insufficient_scope", 403},  // RFC 6750, section 3.1
    // Reasons of the transport.
    NamedDefault{reason::missing_session, 400},
    NamedDefault{reason::unknown_session, 404},
    NamedDefault{reason::accepted, 202},
    NamedDefault{reason::notification_refused, 400},
    NamedDefault{reason::origin_rejected, 403},
    NamedDefault{reason::get_stream_not_offered, 405},
    NamedDefault{reason::delete_not_allowed, 405},
    // The endpoint's own.
    NamedDefault{reason::answered, 200},
    NamedDefault{reason::session_ended, 204},
    NamedDefault{reason::host_rejected, 403},
    NamedDefault{reason::method_not_allowed, 405},
    NamedDefault{reason::not_acceptable, 406},
    NamedDefault{reason::stream_conflict, 409},
    NamedDefault{reason::body_too_large, 413},
    NamedDefault{reason::unsupported_media_type, 415},
    NamedDefault{reason::malformed_session_header, 400},
    NamedDefault{reason::no_capacity, 503},
    NamedDefault{reason::stopping, 503},
    NamedDefault{reason::defect, 500},
};

std::optional<int> library_default(std::string_view name) {
    for (const NamedDefault& entry : library_defaults) {
        if (entry.reason == name) return entry.status;
    }
    return std::nullopt;
}

// HTTP itself, not the protocol that runs over it.
constexpr std::string_view post_method = "POST";
constexpr std::string_view get_method = "GET";
constexpr std::string_view delete_method = "DELETE";
constexpr std::string_view content_type_field = "Content-Type";
constexpr std::string_view accept_field = "Accept";
constexpr std::string_view origin_field = "Origin";
constexpr std::string_view allow_field = "Allow";
constexpr std::string_view cache_control_field = "Cache-Control";
constexpr std::string_view proxy_buffering_field = "X-Accel-Buffering";
constexpr std::string_view no_store = "no-store";
constexpr std::string_view no = "no";
constexpr std::string_view json_type = "application";
constexpr std::string_view json_subtype = "json";
constexpr std::string_view json_media_type = "application/json";
constexpr std::string_view event_stream_type = "text";
constexpr std::string_view event_stream_subtype = "event-stream";

// The random bytes of a session id.
constexpr std::size_t session_id_bytes = 32;

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

// A JSON-RPC error that answers no request: the endpoint refused the HTTP request before any message was read, so
// there is no id to answer, and the member is left out.
std::string error_body(int code, std::string message) {
    json error = make_error(std::nullopt, code, std::move(message));
    error.erase("id");
    return serialize(error);
}

std::string_view refusal_reason(RefusalReason refusal) {
    // A request that neither names its revision nor arrives on an opened connection lacks the session it needs.
    if (refusal == RefusalReason::missing_connection) return reason::missing_session;
    return to_string(refusal);
}

struct Acceptance {
    bool valid = true;
    bool json = true;
    bool event_stream = true;
};

// What the Accept fields admit; no field admits everything.
Acceptance acceptance(const Headers& headers) {
    const std::vector<std::string_view> values = headers.get_all(accept_field);
    if (values.empty()) return {};
    std::string joined;
    for (const std::string_view value : values) {
        if (!joined.empty()) joined += ',';
        joined += value;
    }
    const std::optional<std::vector<MediaRange>> ranges = parse_accept(joined);
    if (!ranges) return {false, false, false};
    return {true, accepts(*ranges, json_type, json_subtype),
            accepts(*ranges, event_stream_type, event_stream_subtype)};
}

}  // namespace

struct Endpoint::Impl : std::enable_shared_from_this<Endpoint::Impl> {
    using Timer = detail::Timer;

    struct Exchange;

    // One input waiting for its turn with its session.
    struct Feed {
        std::shared_ptr<Exchange> exchange;
        Request request;
    };

    enum class Keep { undecided, kept, transient };

    struct Entry : std::enable_shared_from_this<Entry> {
        std::uint64_t serial = 0;
        // Written under `mutex`; read anywhere.
        std::atomic<bool> closed{false};

        std::mutex mutex;  // guards the members below
        std::shared_ptr<Session> session;  // null until it was made
        Keep keep = Keep::undecided;
        std::string id;                // when kept
        profile::ProfilePtr profile;   // when kept: the profile the handshake opened the connection under
        bool feeding = false;
        std::deque<Feed> inputs;
        std::map<UnitId, std::shared_ptr<Exchange>> exchanges;
        std::shared_ptr<BodyStream> connection_stream;
        std::size_t active = 0;            // exchanges not released, and the connection stream
        std::uint64_t idle_generation = 0;  // a job of the idle timer acts only when it is still current
        Timer::Id idle_timer = 0;

        // Guarded by Impl::mutex.
        bool used = false;  // a request after the handshake named it
        std::optional<std::list<std::shared_ptr<Entry>>::iterator> unused_place;  // in Impl::unused
        Timer::Id unused_timer = 0;
    };

    // What an ended session leaves to close, once no lock is held.
    struct Ended {
        std::shared_ptr<Session> session;
        std::shared_ptr<BodyStream> stream;
    };

    enum class Phase { waiting, streaming, done };

    struct Step {
        enum class Kind { complete, append, end_stream, abandon_unit, release };
        Kind kind = Kind::release;
        Response response;                   // complete
        std::shared_ptr<BodyStream> stream;  // complete (when the response is a stream), append, end_stream
        std::string bytes;                   // append
        bool droppable = false;              // append: dropped when the stream is full, instead of ending it
    };

    struct Exchange {
        Exchange(UnitId number, Responder answer, std::shared_ptr<Entry> owner, const Acceptance& accept)
            : unit(number),
              responder(std::move(answer)),
              entry(std::move(owner)),
              admits_json(accept.json),
              admits_event_stream(accept.event_stream) {}

        const UnitId unit;
        const Responder responder;
        const std::shared_ptr<Entry> entry;
        const bool admits_json;
        const bool admits_event_stream;

        std::mutex mutex;  // guards the members below
        Phase phase = Phase::waiting;
        bool fed = false;        // receive() returned
        bool awaits = false;     // what receive() returned
        bool abandoned = false;  // the client left, or its stream closed before the end
        bool released = false;
        std::optional<Headers::Field> announce;  // the session id the answer carries, after a handshake kept it
        profile::ProfilePtr session_profile;     // the kept session's profile
        std::deque<Output> held;                 // outputs that came before receive() returned
        std::shared_ptr<BodyStream> stream;
        Timer::Id timer = 0;
        std::deque<Step> steps;
        bool running = false;  // a thread runs the steps
    };

    Impl(Server& owner, HostPolicy host_policy, EndpointOptions endpoint_options,
         std::vector<profile::ProfilePtr> served)
        : server(owner),
          options(endpoint_options),
          max_body_bytes(owner.options().max_message_bytes),
          profiles(std::move(served)),
          policy(std::make_shared<const HostPolicy>(std::move(host_policy))) {
        const auto add_name = [](std::vector<std::string>& names, const std::string& name) {
            const bool known = std::any_of(names.begin(), names.end(),
                                           [&](const std::string& seen) { return equal_ignoring_case(seen, name); });
            if (!known) names.push_back(name);
        };
        for (const auto& profile : profiles) {
            const profile::StreamableHttpTransport& http = *profile->streamable_http();
            if (http.session_header) add_name(session_headers, *http.session_header);
            if (http.version_header) add_name(version_headers, http.version_header->name);
            for (const auto& header : http.request_headers) {
                if (header.value == profile::HeaderValue::protocol_version) add_name(version_headers, header.name);
            }
            offers_get = offers_get || http.get_stream;
            offers_delete = offers_delete || http.delete_ends_session;
        }
        allow = std::string(post_method);
        if (offers_get) allow += ", " + std::string(get_method);
        if (offers_delete) allow += ", " + std::string(delete_method);
    }

    // ---- requests
    void handle(Request request, Responder responder);
    void post(Request request, Responder responder);
    void get(const Request& request, const Responder& responder);
    void remove(const Request& request, const Responder& responder);

    // ---- sessions
    std::variant<std::shared_ptr<Entry>, std::string_view> open_entry();
    std::variant<std::string, std::string_view> keep_entry(const std::shared_ptr<Entry>& entry,
                                                           const profile::ProfilePtr& opened);
    // The kept entry under `id`, marked used; null when there is none.
    std::shared_ptr<Entry> find(std::string_view id);
    void forget(const Entry& entry);
    void unlist(Entry& entry);                  // with `mutex` held
    Ended end_unused(Entry& entry);             // with `mutex` held; takes the entry's lock
    void expire_unused(const std::shared_ptr<Entry>& entry);
    void feed(const std::shared_ptr<Entry>& entry, Feed input);
    void feed_one(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Session>& session, Feed& input);
    void activate(Entry& entry);    // with the entry's lock held
    void deactivate(Entry& entry);  // with the entry's lock held
    void expire(const std::shared_ptr<Entry>& entry, std::uint64_t generation);
    void connection_closed(const std::shared_ptr<Entry>& entry, const BodyStream* stream);

    // ---- answers
    void on_output(const std::shared_ptr<Entry>& entry, Output output);
    void to_connection(const std::shared_ptr<Entry>& entry, const Output& output);
    void fed(const std::shared_ptr<Exchange>& exchange, bool awaits, std::optional<Headers::Field> announce,
             std::optional<std::string_view> refused);
    void decide(Exchange& exchange, Output output);  // with the exchange's lock held
    void open_stream(Exchange& exchange);            // with the exchange's lock held
    void schedule_stream(const std::shared_ptr<Exchange>& exchange);  // with the exchange's lock held
    void stream_due(const std::shared_ptr<Exchange>& exchange);
    void abandon(const std::shared_ptr<Exchange>& exchange);
    void run_steps(std::unique_lock<std::mutex>& lock, const std::shared_ptr<Exchange>& exchange);
    void execute(const std::shared_ptr<Exchange>& exchange, Step& step);
    void release(const std::shared_ptr<Exchange>& exchange);

    // ---- statuses and responses
    int status(std::string_view why, const profile::Profile* governing) const;
    Response refusal(std::string_view why, const profile::Profile* governing, std::string message,
                     int code = error_code::invalid_request) const;
    void refuse(const Responder& responder, std::string_view why, const profile::Profile* governing,
                std::string message, int code = error_code::invalid_request) const;
    Response stream_response(const Exchange* exchange, const profile::Profile* governing,
                             std::shared_ptr<BodyStream> stream) const;
    Response answer_response(const Exchange& exchange, const Output& output) const;
    Response no_answer_response(const Exchange& exchange, const Output* output) const;
    Response ended_response(const profile::Profile* rules) const;  // the session ended, or the endpoint stops
    static const profile::Profile* governing(const Exchange& exchange, const Output* output);
    bool version_header_ok(const Headers& headers) const;

    // The session id a request names: how many fields name one, and the value of the last.
    struct SessionClaim {
        std::size_t count = 0;
        std::string_view id;
    };
    SessionClaim session_claim(const Headers& headers) const;

    void stop() noexcept;

    Server& server;
    const EndpointOptions options;
    const std::size_t max_body_bytes;
    const std::vector<profile::ProfilePtr> profiles;  // those that define Streamable HTTP, newest first
    std::vector<std::string> session_headers;          // the names the revisions give the session header
    std::vector<std::string> version_headers;          // the names they give the version header
    bool offers_get = false;
    bool offers_delete = false;
    std::string allow;  // the Allow value of a 405
    Timer timer;
    std::atomic<UnitId> next_unit{1};
    std::atomic<bool> stopping{false};  // written under `mutex`; read anywhere

    mutable std::mutex mutex;  // guards the members below
    std::shared_ptr<const HostPolicy> policy;
    std::uint64_t next_serial = 1;
    std::map<std::uint64_t, std::shared_ptr<Entry>> live;
    std::map<std::string, std::shared_ptr<Entry>, std::less<>> by_id;
    std::list<std::shared_ptr<Entry>> unused;  // kept entries nobody used yet, the longest kept first
};

// ---------------------------------------------------------------------------------------------------------------------
// Statuses and responses

int Endpoint::Impl::status(std::string_view why, const profile::Profile* governing) const {
    // A request served under a revision follows that revision; a refusal no revision governs follows the newest one
    // that prescribes a status for its reason.
    if (governing != nullptr) {
        if (const auto& http = governing->streamable_http()) {
            if (const std::optional<int> prescribed = http->status_for(why)) return *prescribed;
        }
    } else {
        for (const auto& profile : profiles) {
            if (const std::optional<int> prescribed = profile->streamable_http()->status_for(why)) return *prescribed;
        }
    }
    if (const std::optional<int> fallback = library_default(why)) return *fallback;
    return *library_default(reason::defect);
}

Response Endpoint::Impl::refusal(std::string_view why, const profile::Profile* governing, std::string message,
                                 int code) const {
    Response response = make_response(status(why, governing), json_media_type, error_body(code, std::move(message)));
    if (why == reason::method_not_allowed || why == reason::get_stream_not_offered ||
        why == reason::delete_not_allowed) {
        response.headers.add(std::string(allow_field), allow);
    }
    return response;
}

void Endpoint::Impl::refuse(const Responder& responder, std::string_view why, const profile::Profile* governing,
                            std::string message, int code) const {
    responder.complete(refusal(why, governing, std::move(message), code));
}

Response Endpoint::Impl::stream_response(const Exchange* exchange, const profile::Profile* governing,
                                         std::shared_ptr<BodyStream> stream) const {
    Response response = make_response(status(reason::answered, governing), event_stream_media_type);
    response.headers.add(std::string(cache_control_field), std::string(no_store));
    response.headers.add(std::string(proxy_buffering_field), std::string(no));
    if (exchange != nullptr && exchange->announce) {
        response.headers.add(exchange->announce->name, exchange->announce->value);
    }
    response.stream = std::move(stream);
    return response;
}

const profile::Profile* Endpoint::Impl::governing(const Exchange& exchange, const Output* output) {
    if (output != nullptr && output->profile) return output->profile.get();
    return exchange.session_profile.get();
}

Response Endpoint::Impl::answer_response(const Exchange& exchange, const Output& output) const {
    const std::string_view why =
        output.refusal == RefusalReason::none ? reason::answered : refusal_reason(output.refusal);
    Response response = make_response(status(why, governing(exchange, &output)), json_media_type, output.line);
    if (exchange.announce) response.headers.add(exchange.announce->name, exchange.announce->value);
    return response;
}

Response Endpoint::Impl::ended_response(const profile::Profile* rules) const {
    if (stopping.load()) {
        return refusal(reason::stopping, rules, "Service unavailable: the server is stopping",
                       error_code::internal_error);
    }
    return refusal(reason::unknown_session, rules, "Not found: the session ended");
}

Response Endpoint::Impl::no_answer_response(const Exchange& exchange, const Output* output) const {
    const profile::Profile* rules = governing(exchange, output);
    if (stopping.load()) return ended_response(rules);
    if (exchange.awaits) {
        // A request that ends without an answer: its session ended, or it was cancelled.
        if (exchange.entry->closed.load()) return ended_response(rules);
        Response response = make_response(status(reason::accepted, rules));
        if (exchange.announce) response.headers.add(exchange.announce->name, exchange.announce->value);
        return response;
    }
    // Notifications and responses: taken within a session; outside one, no revision served takes them.
    if (!exchange.session_profile) {
        return refusal(reason::notification_refused, rules,
                       "Bad request: a notification or response is taken only within a session");
    }
    return make_response(status(reason::accepted, rules));
}

bool Endpoint::Impl::version_header_ok(const Headers& headers) const {
    std::size_t count = 0;
    std::string_view value;
    for (const std::string& name : version_headers) {
        for (const std::string_view field : headers.get_all(name)) {
            ++count;
            value = trim(field);
        }
    }
    if (count == 0) return true;
    if (count > 1) return false;
    return std::any_of(profiles.begin(), profiles.end(),
                       [&](const profile::ProfilePtr& profile) { return profile->revision() == value; });
}

Endpoint::Impl::SessionClaim Endpoint::Impl::session_claim(const Headers& headers) const {
    SessionClaim claim;
    for (const std::string& name : session_headers) {
        for (const std::string_view field : headers.get_all(name)) {
            ++claim.count;
            claim.id = trim(field);
        }
    }
    return claim;
}

// ---------------------------------------------------------------------------------------------------------------------
// Requests

void Endpoint::Impl::handle(Request request, Responder responder) {
    std::shared_ptr<const HostPolicy> current;
    {
        std::lock_guard lock(mutex);
        current = policy;
    }
    if (stopping.load()) {
        refuse(responder, reason::stopping, nullptr, "Service unavailable: the server is stopping",
               error_code::internal_error);
        return;
    }
    const PolicyVerdict verdict = current->check(request.headers);
    if (!verdict.allowed) {
        const std::vector<std::string_view> origins = request.headers.get_all(origin_field);
        const bool origin_refused = origins.size() > 1 || (origins.size() == 1 && !current->origin_allowed(origins[0]));
        refuse(responder, origin_refused ? reason::origin_rejected : reason::host_rejected, nullptr,
               "Forbidden: " + verdict.reason);
        return;
    }
    if (request.method == post_method) {
        post(std::move(request), std::move(responder));
    } else if (request.method == get_method) {
        get(request, responder);
    } else if (request.method == delete_method) {
        remove(request, responder);
    } else {
        refuse(responder, reason::method_not_allowed, nullptr, "Method not allowed: " + request.method);
    }
}

void Endpoint::Impl::post(Request request, Responder responder) {
    const std::vector<std::string_view> types = request.headers.get_all(content_type_field);
    if (types.size() != 1 || !is_media_type(types.front(), json_type, json_subtype)) {
        refuse(responder, reason::unsupported_media_type, nullptr,
               "Unsupported media type: the body must be " + std::string(json_media_type));
        return;
    }
    const Acceptance accept = acceptance(request.headers);
    if (!accept.valid || (!accept.json && !accept.event_stream)) {
        refuse(responder, reason::not_acceptable, nullptr,
               "Not acceptable: the answer is " + std::string(json_media_type) + " or " +
                   std::string(event_stream_media_type));
        return;
    }
    if (request.body.size() > max_body_bytes) {
        refuse(responder, reason::body_too_large, nullptr,
               "Payload too large: the body is longer than " + std::to_string(max_body_bytes) + " bytes");
        return;
    }

    const SessionClaim claim = session_claim(request.headers);
    if (claim.count > 1) {
        refuse(responder, reason::malformed_session_header, nullptr, "Bad request: more than one session id");
        return;
    }
    std::shared_ptr<Entry> entry;
    if (claim.count == 1) {
        entry = find(claim.id);
        if (!entry) {
            refuse(responder, reason::unknown_session, nullptr, "Not found: no such session");
            return;
        }
    } else {
        auto opened = open_entry();
        if (const auto* why = std::get_if<std::string_view>(&opened)) {
            refuse(responder, *why, nullptr, "Service unavailable: no session can be served now",
                   error_code::internal_error);
            return;
        }
        entry = std::get<std::shared_ptr<Entry>>(std::move(opened));
    }

    auto exchange = std::make_shared<Exchange>(next_unit.fetch_add(1), std::move(responder), entry, accept);
    {
        std::unique_lock lock(entry->mutex);
        if (entry->closed.load()) {
            const profile::ProfilePtr rules = entry->profile;
            lock.unlock();
            if (stopping.load()) {
                refuse(exchange->responder, reason::stopping, rules.get(),
                       "Service unavailable: the server is stopping", error_code::internal_error);
            } else {
                refuse(exchange->responder, reason::unknown_session, rules.get(), "Not found: the session ended");
            }
            return;
        }
        entry->exchanges.emplace(exchange->unit, exchange);
        activate(*entry);
    }
    exchange->responder.on_disconnect(
        [self = weak_from_this(), weak = std::weak_ptr<Exchange>(exchange)] {
            const auto impl = self.lock();
            const auto lost = weak.lock();
            if (impl && lost) impl->abandon(lost);
        });
    feed(entry, Feed{exchange, std::move(request)});
}

void Endpoint::Impl::get(const Request& request, const Responder& responder) {
    const SessionClaim claim = session_claim(request.headers);
    if (claim.count == 0) {
        refuse(responder, reason::get_stream_not_offered, nullptr, "Method not allowed: no stream without a session");
        return;
    }
    if (claim.count > 1) {
        refuse(responder, reason::malformed_session_header, nullptr, "Bad request: more than one session id");
        return;
    }
    const std::shared_ptr<Entry> entry = find(claim.id);
    if (!entry) {
        refuse(responder, reason::unknown_session, nullptr, "Not found: no such session");
        return;
    }
    // A kept entry's profile was set before the entry could be found.
    const profile::Profile* rules = entry->profile.get();
    if (!rules->streamable_http()->get_stream) {
        refuse(responder, reason::get_stream_not_offered, rules, "Method not allowed: this session offers no stream");
        return;
    }
    if (!version_header_ok(request.headers)) {
        refuse(responder, reason::unsupported_version_header, rules,
               "Bad request: the protocol version header names no revision this server serves");
        return;
    }
    const Acceptance accept = acceptance(request.headers);
    if (!accept.valid || !accept.event_stream) {
        refuse(responder, reason::not_acceptable, rules,
               "Not acceptable: the stream is " + std::string(event_stream_media_type));
        return;
    }

    auto stream = std::make_shared<BodyStream>(options.max_stream_buffer_bytes);
    {
        std::unique_lock lock(entry->mutex);
        if (entry->closed.load()) {
            lock.unlock();
            refuse(responder, stopping.load() ? reason::stopping : reason::unknown_session, rules,
                   "Not found: the session ended");
            return;
        }
        if (entry->connection_stream) {
            lock.unlock();
            refuse(responder, reason::stream_conflict, rules, "Conflict: the session's stream is open already");
            return;
        }
        entry->connection_stream = stream;
        activate(*entry);
    }
    stream->on_close([self = weak_from_this(), weak = std::weak_ptr<Entry>(entry), raw = stream.get()] {
        const auto impl = self.lock();
        const auto owner = weak.lock();
        if (impl && owner) impl->connection_closed(owner, raw);
    });
    if (!responder.complete(stream_response(nullptr, rules, stream))) stream->close();
}

void Endpoint::Impl::remove(const Request& request, const Responder& responder) {
    const SessionClaim claim = session_claim(request.headers);
    if (claim.count == 0) {
        refuse(responder, reason::delete_not_allowed, nullptr, "Method not allowed: no session to end");
        return;
    }
    if (claim.count > 1) {
        refuse(responder, reason::malformed_session_header, nullptr, "Bad request: more than one session id");
        return;
    }
    const std::shared_ptr<Entry> entry = find(claim.id);
    if (!entry) {
        refuse(responder, reason::unknown_session, nullptr, "Not found: no such session");
        return;
    }
    const profile::Profile* rules = entry->profile.get();
    if (!rules->streamable_http()->delete_ends_session) {
        refuse(responder, reason::delete_not_allowed, rules, "Method not allowed: this session is not ended that way");
        return;
    }
    if (!version_header_ok(request.headers)) {
        refuse(responder, reason::unsupported_version_header, rules,
               "Bad request: the protocol version header names no revision this server serves");
        return;
    }
    std::shared_ptr<Session> session;
    std::shared_ptr<BodyStream> stream;
    {
        std::lock_guard table(mutex);
        std::lock_guard lock(entry->mutex);
        if (!entry->closed.load()) {
            entry->closed = true;
            session = entry->session;
            stream = std::move(entry->connection_stream);
            if (entry->idle_timer != 0) timer.cancel(entry->idle_timer);
            entry->idle_timer = 0;
            by_id.erase(entry->id);
            live.erase(entry->serial);
            unlist(*entry);
        }
    }
    if (!session) {
        refuse(responder, reason::unknown_session, rules, "Not found: the session ended");
        return;
    }
    session->close();  // every request still open ends; each is answered as one whose session ended
    if (stream) stream->end();
    responder.complete(make_response(status(reason::session_ended, rules)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Sessions

std::variant<std::shared_ptr<Endpoint::Impl::Entry>, std::string_view> Endpoint::Impl::open_entry() {
    auto entry = std::make_shared<Entry>();
    {
        std::lock_guard lock(mutex);
        if (stopping.load()) return reason::stopping;
        entry->serial = next_serial++;
        live.emplace(entry->serial, entry);
    }
    auto created = server.connect_tagged(
        profile::Transport::streamable_http,
        [self = weak_from_this(), weak = std::weak_ptr<Entry>(entry)](Output output) {
            const auto impl = self.lock();
            const auto owner = weak.lock();
            if (impl && owner) impl->on_output(owner, std::move(output));
        });
    if (std::holds_alternative<CreateError>(created)) {
        forget(*entry);
        return reason::no_capacity;
    }
    std::shared_ptr<Session> session(std::get<std::unique_ptr<Session>>(std::move(created)));
    bool closed = false;
    {
        std::lock_guard lock(entry->mutex);
        entry->session = session;
        closed = entry->closed.load();
    }
    if (closed) {  // stop() came between
        session->close();
        return reason::stopping;
    }
    return entry;
}

std::variant<std::string, std::string_view> Endpoint::Impl::keep_entry(const std::shared_ptr<Entry>& entry,
                                                                     const profile::ProfilePtr& opened) {
    // The id is the session's only secret: without the operating system's random source there is no session.
    std::optional<std::string> id = random_hex(session_id_bytes);
    if (!id) return reason::no_capacity;
    Ended displaced;
    {
        std::lock_guard table(mutex);
        if (stopping.load()) return reason::stopping;
        if (by_id.find(*id) != by_id.end()) return reason::no_capacity;  // never two sessions under one id
        // A full table makes room only by ending the session nobody used that was kept longest.
        const bool full = by_id.size() >= options.max_sessions;
        if (full && unused.empty()) return reason::no_capacity;
        {
            std::lock_guard lock(entry->mutex);
            if (entry->closed.load()) return reason::stopping;
            entry->keep = Keep::kept;
            entry->id = *id;
            entry->profile = opened;
        }
        if (full) {
            const std::shared_ptr<Entry> oldest = unused.front();
            displaced = end_unused(*oldest);
        }
        by_id.emplace(*id, entry);
        entry->unused_place = unused.insert(unused.end(), entry);
        entry->unused_timer = timer.schedule(Clock::now() + options.unused_session_timeout,
                                             [self = weak_from_this(), weak = std::weak_ptr<Entry>(entry)] {
                                                 const auto impl = self.lock();
                                                 const auto owner = weak.lock();
                                                 if (impl && owner) impl->expire_unused(owner);
                                             });
    }
    // Every request still open on the displaced session ends, and is answered as one whose session ended.
    if (displaced.session) displaced.session->close();
    if (displaced.stream) displaced.stream->end();
    return std::move(*id);
}

std::shared_ptr<Endpoint::Impl::Entry> Endpoint::Impl::find(std::string_view id) {
    std::lock_guard lock(mutex);
    const auto found = by_id.find(id);
    if (found == by_id.end()) return nullptr;
    // From its first request after the handshake on, a session never gives its place to a newcomer. Constant work:
    // only that first request takes it off the list.
    Entry& entry = *found->second;
    if (!entry.used) {
        entry.used = true;
        unlist(entry);
    }
    return found->second;
}

void Endpoint::Impl::unlist(Entry& entry) {
    if (entry.unused_place) {
        unused.erase(*entry.unused_place);
        entry.unused_place.reset();
    }
    if (entry.unused_timer != 0) timer.cancel(entry.unused_timer);
    entry.unused_timer = 0;
}

Endpoint::Impl::Ended Endpoint::Impl::end_unused(Entry& entry) {
    unlist(entry);
    by_id.erase(entry.id);
    live.erase(entry.serial);
    std::lock_guard lock(entry.mutex);
    if (entry.closed.load()) return {};
    entry.closed = true;
    if (entry.idle_timer != 0) timer.cancel(entry.idle_timer);
    entry.idle_timer = 0;
    return Ended{entry.session, std::move(entry.connection_stream)};
}

void Endpoint::Impl::expire_unused(const std::shared_ptr<Entry>& entry) {
    Ended ended;
    {
        std::lock_guard table(mutex);
        entry->unused_timer = 0;  // this is its job
        if (stopping.load() || entry->used || !entry->unused_place) return;
        ended = end_unused(*entry);
    }
    if (ended.session) ended.session->close();
    if (ended.stream) ended.stream->end();
}

void Endpoint::Impl::forget(const Entry& entry) {
    std::lock_guard lock(mutex);
    live.erase(entry.serial);
    if (!entry.id.empty()) {
        const auto found = by_id.find(entry.id);
        if (found != by_id.end() && found->second.get() == &entry) by_id.erase(found);
    }
}

void Endpoint::Impl::feed(const std::shared_ptr<Entry>& entry, Feed input) {
    std::unique_lock lock(entry->mutex);
    entry->inputs.push_back(std::move(input));
    if (entry->feeding) return;  // the thread that feeds takes this one too
    entry->feeding = true;
    while (!entry->inputs.empty()) {
        Feed next = std::move(entry->inputs.front());
        entry->inputs.pop_front();
        const std::shared_ptr<Session> session = entry->session;
        lock.unlock();
        feed_one(entry, session, next);
        lock.lock();
    }
    entry->feeding = false;
}

void Endpoint::Impl::feed_one(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Session>& session,
                              Feed& input) {
    const std::shared_ptr<Exchange>& exchange = input.exchange;
    bool skip = false;
    {
        std::lock_guard lock(exchange->mutex);
        skip = exchange->abandoned;
    }
    if (skip) {  // the client left before its turn: the input is never read
        release(exchange);
        return;
    }

    std::vector<HeaderField> fields;
    fields.reserve(input.request.headers.size());
    for (const Headers::Field& field : input.request.headers.fields()) {
        fields.push_back(HeaderField{field.name, trim(field.value)});
    }
    bool awaits = false;
    std::optional<std::string_view> refused;
    try {
        awaits = session->receive(exchange->unit, input.request.body, fields);
    } catch (...) {
        refused = reason::defect;  // the session's sink does not throw, and every unit number is new
    }

    std::optional<Headers::Field> announce;
    Keep keep = Keep::kept;
    {
        std::lock_guard lock(entry->mutex);
        keep = entry->keep;
    }
    if (keep == Keep::undecided) {
        // A session without an id is kept when its input opened a connection under a revision that names sessions.
        const profile::ProfilePtr opened = refused ? nullptr : session->connection_profile();
        const std::optional<std::string>* header =
            opened && opened->streamable_http() ? &opened->streamable_http()->session_header : nullptr;
        bool kept = false;
        if (header != nullptr && header->has_value()) {
            auto made = keep_entry(entry, opened);
            if (auto* id = std::get_if<std::string>(&made)) {
                announce = Headers::Field{**header, std::move(*id)};
                kept = true;
            } else {
                refused = std::get<std::string_view>(made);
            }
        }
        if (!kept) {
            std::lock_guard lock(entry->mutex);
            entry->keep = Keep::transient;
        }
    }
    fed(exchange, awaits, std::move(announce), refused);
}

void Endpoint::Impl::activate(Entry& entry) {
    if (entry.active++ == 0) {
        ++entry.idle_generation;
        if (entry.idle_timer != 0) timer.cancel(entry.idle_timer);
        entry.idle_timer = 0;
    }
}

void Endpoint::Impl::deactivate(Entry& entry) {
    if (entry.active > 0) --entry.active;
    if (entry.active != 0 || entry.keep != Keep::kept || entry.closed.load()) return;
    // A job that fires after the session was used again finds a newer generation and does nothing.
    const std::uint64_t generation = ++entry.idle_generation;
    entry.idle_timer = timer.schedule(Clock::now() + options.session_idle_timeout,
                                      [self = weak_from_this(), weak = entry.weak_from_this(), generation] {
                                          const auto impl = self.lock();
                                          const auto owner = weak.lock();
                                          if (impl && owner) impl->expire(owner, generation);
                                      });
}

void Endpoint::Impl::expire(const std::shared_ptr<Entry>& entry, std::uint64_t generation) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard table(mutex);
        std::lock_guard lock(entry->mutex);
        if (entry->closed.load() || entry->active > 0 || entry->idle_generation != generation) return;
        entry->closed = true;
        entry->idle_timer = 0;
        session = entry->session;
        by_id.erase(entry->id);
        live.erase(entry->serial);
        unlist(*entry);
    }
    if (session) session->close();
}

void Endpoint::Impl::connection_closed(const std::shared_ptr<Entry>& entry, const BodyStream* stream) {
    std::lock_guard lock(entry->mutex);
    if (entry->connection_stream.get() != stream) return;
    entry->connection_stream.reset();
    deactivate(*entry);
}

// ---------------------------------------------------------------------------------------------------------------------
// Answers

void Endpoint::Impl::on_output(const std::shared_ptr<Entry>& entry, Output output) {
    if (!output.unit) {
        to_connection(entry, output);
        return;
    }
    std::shared_ptr<Exchange> exchange;
    {
        std::lock_guard lock(entry->mutex);
        const auto found = entry->exchanges.find(*output.unit);
        if (found != entry->exchanges.end()) exchange = found->second;
    }
    if (!exchange) return;  // released: abandoned, or answered by a refusal
    std::unique_lock lock(exchange->mutex);
    if (exchange->released) return;
    if (!exchange->fed) {
        exchange->held.push_back(std::move(output));
        return;
    }
    decide(*exchange, std::move(output));
    run_steps(lock, exchange);
}

void Endpoint::Impl::to_connection(const std::shared_ptr<Entry>& entry, const Output& output) {
    if (output.kind == OutputKind::end) return;
    std::shared_ptr<BodyStream> stream;
    {
        std::lock_guard lock(entry->mutex);
        stream = entry->connection_stream;
    }
    if (!stream) return;  // no stream is open: messages of the connection are dropped
    if (stream->append(sse_event(output.line)) == BodyStream::Append::full &&
        output.kind != OutputKind::notification) {
        stream->close();  // a request to the client is not dropped silently: the client sees the stream end
    }
}

void Endpoint::Impl::fed(const std::shared_ptr<Exchange>& exchange, bool awaits, std::optional<Headers::Field> announce,
                         std::optional<std::string_view> refused) {
    const std::shared_ptr<Entry>& entry = exchange->entry;
    profile::ProfilePtr session_profile;
    {
        std::lock_guard lock(entry->mutex);
        session_profile = entry->profile;
    }
    std::unique_lock lock(exchange->mutex);
    exchange->fed = true;
    exchange->awaits = awaits;
    exchange->announce = std::move(announce);
    exchange->session_profile = std::move(session_profile);
    std::deque<Output> held = std::move(exchange->held);
    exchange->held.clear();
    if (refused) {
        // What the session wrote is not sent: the request is refused as a whole.
        if (exchange->phase != Phase::done) {
            exchange->phase = Phase::done;
            const bool internal = *refused == reason::no_capacity || *refused == reason::stopping ||
                                  *refused == reason::defect;
            Step step;
            step.kind = Step::Kind::complete;
            step.response = refusal(*refused, nullptr, "Service unavailable: the session cannot be kept",
                                    internal ? error_code::internal_error : error_code::invalid_request);
            exchange->steps.push_back(std::move(step));
        }
        exchange->steps.push_back(Step{Step::Kind::release, {}, nullptr, {}, false});
        run_steps(lock, exchange);
        return;
    }
    for (Output& output : held) decide(*exchange, std::move(output));
    if (exchange->abandoned) {
        // The client left while the input was read: the session lets the unit go.
        exchange->steps.push_back(Step{Step::Kind::abandon_unit, {}, nullptr, {}, false});
        exchange->steps.push_back(Step{Step::Kind::release, {}, nullptr, {}, false});
    } else if (exchange->phase == Phase::waiting && entry->closed.load()) {
        // The session ended before it read the input (which it then ignores, whatever the input was): nothing will
        // come, and the request is answered as one that names an ended session.
        exchange->phase = Phase::done;
        Step step;
        step.kind = Step::Kind::complete;
        step.response = ended_response(governing(*exchange, nullptr));
        exchange->steps.push_back(std::move(step));
        exchange->steps.push_back(Step{Step::Kind::release, {}, nullptr, {}, false});
    } else if (exchange->phase == Phase::waiting && exchange->admits_event_stream) {
        schedule_stream(exchange);
    }
    run_steps(lock, exchange);
}

void Endpoint::Impl::schedule_stream(const std::shared_ptr<Exchange>& exchange) {
    exchange->timer = timer.schedule(Clock::now() + options.stream_after,
                                     [self = weak_from_this(), weak = std::weak_ptr<Exchange>(exchange)] {
                                         const auto impl = self.lock();
                                         const auto due = weak.lock();
                                         if (impl && due) impl->stream_due(due);
                                     });
}

void Endpoint::Impl::stream_due(const std::shared_ptr<Exchange>& exchange) {
    std::unique_lock lock(exchange->mutex);
    exchange->timer = 0;
    if (exchange->phase != Phase::waiting || exchange->abandoned || exchange->released) return;
    open_stream(*exchange);
    run_steps(lock, exchange);
}

void Endpoint::Impl::open_stream(Exchange& exchange) {
    exchange.phase = Phase::streaming;
    if (exchange.timer != 0) timer.cancel(exchange.timer);
    exchange.timer = 0;
    exchange.stream = std::make_shared<BodyStream>(options.max_stream_buffer_bytes);
    Step step;
    step.kind = Step::Kind::complete;
    step.response = stream_response(&exchange, governing(exchange, nullptr), exchange.stream);
    step.stream = exchange.stream;
    exchange.steps.push_back(std::move(step));
}

void Endpoint::Impl::decide(Exchange& exchange, Output output) {
    if (exchange.phase == Phase::done) return;
    if (exchange.phase == Phase::waiting) {
        if (output.last) {
            exchange.phase = Phase::done;
            if (exchange.timer != 0) timer.cancel(exchange.timer);
            exchange.timer = 0;
            Step step;
            step.kind = Step::Kind::complete;
            if (output.kind == OutputKind::end) {
                step.response = no_answer_response(exchange, &output);
                exchange.steps.push_back(std::move(step));
            } else if (exchange.admits_json || output.refusal != RefusalReason::none) {
                // A refusal keeps its status, so it is JSON whatever the client prefers.
                step.response = answer_response(exchange, output);
                exchange.steps.push_back(std::move(step));
            } else {
                // The client takes only an event stream: the answer is its one event.
                exchange.phase = Phase::streaming;
                open_stream(exchange);
                exchange.phase = Phase::done;
                exchange.steps.push_back(Step{Step::Kind::append, {}, exchange.stream, sse_event(output.line), false});
                exchange.steps.push_back(Step{Step::Kind::end_stream, {}, exchange.stream, {}, false});
            }
            exchange.steps.push_back(Step{Step::Kind::release, {}, nullptr, {}, false});
            return;
        }
        // Something comes before the answer.
        if (exchange.admits_event_stream) {
            open_stream(exchange);
        } else if (output.kind == OutputKind::notification && !output.long_lived) {
            return;  // the client takes only JSON: one answer, so the notification is dropped
        } else {
            // A request to the client, or a subscription (whose messages are all on its stream), cannot reach a client
            // that takes only JSON on this request.
            exchange.phase = Phase::done;
            exchange.steps.push_back(Step{Step::Kind::abandon_unit, {}, nullptr, {}, false});
            Step step;
            step.kind = Step::Kind::complete;
            step.response = refusal(reason::not_acceptable, governing(exchange, &output),
                                    "Not acceptable: the server needs an event stream to answer this request");
            exchange.steps.push_back(std::move(step));
            exchange.steps.push_back(Step{Step::Kind::release, {}, nullptr, {}, false});
            return;
        }
    }
    // Streaming.
    if (output.kind != OutputKind::end) {
        exchange.steps.push_back(Step{Step::Kind::append, {}, exchange.stream, sse_event(output.line),
                                      output.kind == OutputKind::notification});
    }
    if (output.last) {
        exchange.phase = Phase::done;
        exchange.steps.push_back(Step{Step::Kind::end_stream, {}, exchange.stream, {}, false});
        exchange.steps.push_back(Step{Step::Kind::release, {}, nullptr, {}, false});
    }
}

void Endpoint::Impl::abandon(const std::shared_ptr<Exchange>& exchange) {
    std::unique_lock lock(exchange->mutex);
    if (exchange->abandoned || exchange->released || exchange->phase == Phase::done) return;
    exchange->abandoned = true;
    exchange->phase = Phase::done;
    if (exchange->timer != 0) timer.cancel(exchange->timer);
    exchange->timer = 0;
    if (!exchange->fed) return;  // the feeder lets the unit go once receive() returned, or never feeds it
    exchange->steps.push_back(Step{Step::Kind::abandon_unit, {}, nullptr, {}, false});
    exchange->steps.push_back(Step{Step::Kind::release, {}, nullptr, {}, false});
    run_steps(lock, exchange);
}

void Endpoint::Impl::run_steps(std::unique_lock<std::mutex>& lock, const std::shared_ptr<Exchange>& exchange) {
    if (exchange->running) return;  // the thread that runs them takes these too
    exchange->running = true;
    while (!exchange->steps.empty()) {
        Step step = std::move(exchange->steps.front());
        exchange->steps.pop_front();
        lock.unlock();
        execute(exchange, step);
        lock.lock();
    }
    exchange->running = false;
}

void Endpoint::Impl::execute(const std::shared_ptr<Exchange>& exchange, Step& step) {
    switch (step.kind) {
        case Step::Kind::complete: {
            if (!step.stream) {
                exchange->responder.complete(std::move(step.response));
                return;
            }
            const std::shared_ptr<BodyStream> stream = step.stream;
            stream->on_close([self = weak_from_this(), weak = std::weak_ptr<Exchange>(exchange)] {
                const auto impl = self.lock();
                const auto closed = weak.lock();
                if (impl && closed) impl->abandon(closed);
            });
            // A response the listener cannot take any more (the client left) leaves the stream unread: closing it
            // reports that like any early close.
            if (!exchange->responder.complete(std::move(step.response))) stream->close();
            return;
        }
        case Step::Kind::append:
            // An answer is never dropped and never waits for a slow reader: the stream ends and the request is
            // abandoned instead. A notification that does not fit is dropped.
            if (step.stream->append(step.bytes) == BodyStream::Append::full && !step.droppable) step.stream->close();
            return;
        case Step::Kind::end_stream: step.stream->end(); return;
        case Step::Kind::abandon_unit: {
            std::shared_ptr<Session> session;
            {
                std::lock_guard lock(exchange->entry->mutex);
                session = exchange->entry->session;
            }
            if (session) session->abandon(exchange->unit);
            return;
        }
        case Step::Kind::release: release(exchange); return;
    }
}

void Endpoint::Impl::release(const std::shared_ptr<Exchange>& exchange) {
    {
        std::lock_guard lock(exchange->mutex);
        if (exchange->released) return;
        exchange->released = true;
        if (exchange->timer != 0) timer.cancel(exchange->timer);
        exchange->timer = 0;
    }
    const std::shared_ptr<Entry>& entry = exchange->entry;
    std::shared_ptr<Session> finished;
    {
        std::lock_guard lock(entry->mutex);
        const auto found = entry->exchanges.find(exchange->unit);
        if (found != entry->exchanges.end() && found->second == exchange) entry->exchanges.erase(found);
        deactivate(*entry);
        // A session without an id ends with its request.
        if (entry->keep != Keep::kept && entry->exchanges.empty() && entry->inputs.empty() && !entry->closed.load()) {
            entry->closed = true;
            finished = entry->session;
        }
    }
    if (finished) {
        forget(*entry);
        finished->close();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Stopping

void Endpoint::Impl::stop() noexcept {
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock(mutex);
        if (stopping.load()) return;
        stopping = true;
        for (auto& item : live) entries.push_back(std::move(item.second));
        live.clear();
        by_id.clear();
        // The timer's jobs are dropped below; a job that runs meanwhile finds `stopping` and leaves.
        for (const auto& listed : unused) listed->unused_place.reset();
        unused.clear();
    }
    for (const auto& entry : entries) {
        std::shared_ptr<Session> session;
        std::shared_ptr<BodyStream> stream;
        std::vector<std::shared_ptr<Exchange>> open;
        {
            std::lock_guard lock(entry->mutex);
            entry->closed = true;
            session = entry->session;
            stream = std::move(entry->connection_stream);
            for (const auto& item : entry->exchanges) open.push_back(item.second);
            if (entry->idle_timer != 0) timer.cancel(entry->idle_timer);
            entry->idle_timer = 0;
        }
        try {
            if (session) session->close();  // every open request ends: it is answered 503, or its stream ends
        } catch (...) {
        }
        if (stream) stream->close();
        // A stream whose last output came is ended rather than cut: what it holds (the graceful end of a
        // subscription, which close() wrote) is still read out by its listener. Every other stream is cut.
        for (const auto& exchange : open) {
            std::shared_ptr<BodyStream> cut;
            {
                std::lock_guard lock(exchange->mutex);
                if (exchange->phase != Phase::done) cut = exchange->stream;
            }
            if (cut) cut->close();
        }
    }
    timer.stop();
}

// ---------------------------------------------------------------------------------------------------------------------
// The public handle

Outcome<std::unique_ptr<Endpoint>> Endpoint::create(Server& server, HostPolicy policy, EndpointOptions options) {
    using Result = Outcome<std::unique_ptr<Endpoint>>;
    if (options.max_sessions < 1) return Result::failure("max_sessions must be at least 1");
    if (options.session_idle_timeout <= std::chrono::milliseconds::zero()) {
        return Result::failure("session_idle_timeout must be positive");
    }
    if (options.unused_session_timeout <= std::chrono::milliseconds::zero()) {
        return Result::failure("unused_session_timeout must be positive");
    }
    if (options.stream_after <= std::chrono::milliseconds::zero()) return Result::failure("stream_after must be positive");
    if (options.max_stream_buffer_bytes < 1) return Result::failure("max_stream_buffer_bytes must be at least 1");
    for (const std::string_view name : profile::declared_status_reasons()) {
        if (!library_default(name)) {
            return Result::failure("the library has no status for the reason '" + std::string(name) + "'");
        }
    }

    // Which revisions are served on this transport: a session made to find out says.
    auto probe = server.connect_tagged(profile::Transport::streamable_http, [](Output) {});
    if (const auto* error = std::get_if<CreateError>(&probe)) {
        return Result::failure("cannot serve Streamable HTTP: " + error->message);
    }
    const std::unique_ptr<Session> session = std::get<std::unique_ptr<Session>>(std::move(probe));
    std::vector<profile::ProfilePtr> profiles(session->profiles().begin(), session->profiles().end());
    session->close();

    try {
        auto impl = std::make_shared<Impl>(server, std::move(policy), options, std::move(profiles));
        return Result::success(std::unique_ptr<Endpoint>(new Endpoint(std::move(impl))));
    } catch (const std::system_error& error) {
        return Result::failure(std::string("cannot start the endpoint's timer thread: ") + error.what());
    }
}

Endpoint::Endpoint(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Endpoint::~Endpoint() { stop(); }

void Endpoint::handle(Request request, Responder responder) { impl_->handle(std::move(request), std::move(responder)); }

Handler Endpoint::handler() {
    return [impl = impl_](Request request, Responder responder) {
        impl->handle(std::move(request), std::move(responder));
    };
}

void Endpoint::set_policy(HostPolicy policy) {
    auto replacement = std::make_shared<const HostPolicy>(std::move(policy));
    std::lock_guard lock(impl_->mutex);
    impl_->policy = std::move(replacement);
}

void Endpoint::stop() noexcept { impl_->stop(); }

std::size_t Endpoint::session_count() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->by_id.size();
}

}  // namespace neomcp::http
