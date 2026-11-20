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
// is longer than HttpEndpoint::max_session_id_bytes or comes twice ends the handshake's exchange as lost.
#include "client_http/streamable.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/post.hpp>

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

// What the transfer thread read, carried to the client's thread: one queue, drained there by one drain at a time.
class Delivery : public std::enable_shared_from_this<Delivery> {
public:
    using Handler = std::function<void(UnitId unit, TransferItem item)>;

    // On the client's thread.
    void attach(asio::any_io_executor executor, Handler handler) {
        std::lock_guard lock(mutex_);
        executor_ = std::move(executor);
        handler_ = std::move(handler);
        attached_ = true;
    }

    // On the client's thread: nothing more is delivered.
    void detach() {
        std::lock_guard lock(mutex_);
        attached_ = false;
        handler_ = nullptr;
        items_.clear();
    }

    // Any thread.
    void push(UnitId unit, TransferItem item) {
        std::lock_guard lock(mutex_);
        if (!attached_) return;
        items_.emplace_back(unit, std::move(item));
        if (drain_posted_) return;
        drain_posted_ = true;
        asio::post(executor_, [self = shared_from_this()] { self->drain(); });
    }

private:
    // On the client's thread, where alone the handler runs and is cleared, so a delivery never races detach().
    void drain() {
        while (true) {
            std::pair<UnitId, TransferItem> next{connection_unit, StreamEnded{}};
            Handler handler;  // a copy: the handler may detach the delivery while it runs
            {
                std::lock_guard lock(mutex_);
                if (!attached_ || items_.empty()) {
                    drain_posted_ = false;
                    return;
                }
                next = std::move(items_.front());
                items_.pop_front();
                handler = handler_;
            }
            handler(next.first, std::move(next.second));
        }
    }

    std::mutex mutex_;
    asio::any_io_executor executor_;
    Handler handler_;
    bool attached_ = false;
    bool drain_posted_ = false;
    std::deque<std::pair<UnitId, TransferItem>> items_;
};

class StreamableTransport final : public Transport {
public:
    StreamableTransport(HttpEndpoint endpoint, UrlGuard guard, SessionRules rules, TransferLimits limits,
                        std::shared_ptr<CurlGlobal> global)
        : endpoint_(std::move(endpoint)),
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
            case OutgoingKind::open_connection_stream:
                units_[connection_unit] = UnitState{};
                delivery_->push(connection_unit,
                                StreamLost{"this transport does not open a stream outside requests"});
                return;
            case OutgoingKind::restart:
            case OutgoingKind::close: return;
        }
    }

    asio::awaitable<void> close(CloseCause) override {
        if (!closed_) {
            closed_ = true;
            loop_->stop();
            delivery_->detach();
            events_ = nullptr;
            units_.clear();
        }
        co_return;
    }

private:
    struct UnitState {
        bool handshake = false;
    };

    HttpRequestInfo info(UnitId unit) const { return HttpRequestInfo{std::string(post_method), endpoint_.url, unit}; }

    bool is_handshake(const Outgoing& outgoing) const {
        if (outgoing.kind != OutgoingKind::request || rules_.handshake_methods.empty()) return false;
        const json message = json::parse(outgoing.line, nullptr, false);
        const auto* members = message.get_ptr<const json::object_t*>();
        if (members == nullptr) return false;
        const auto found = members->find(std::string(method_member));
        if (found == members->end() || !found->second.is_string()) return false;
        return rules_.handshake_methods.count(found->second.get_ref<const std::string&>()) > 0;
    }

    void submit(Outgoing outgoing) {
        const UnitId unit = outgoing.unit;
        UnitState state;
        state.handshake = is_handshake(outgoing);
        TransferRequest request;
        request.unit = unit;
        request.url = endpoint_.url;
        std::optional<std::string> problem;
        const auto add = [&](std::string_view name, std::string_view value) {
            auto line = header_line(name, value);
            if (!line) {
                if (!problem) problem = "the header field " + std::string(name) + " cannot be sent as it is";
                return;
            }
            request.header_lines.push_back(std::move(*line));
        };
        add(field_content_type, request_media_type);
        add(field_accept, accepted_media_types);
        request.header_lines.emplace_back(no_expect_line);
        for (const HttpHeader& header : outgoing.headers) add(header.name, header.value);
        // A handshake starts a session of its own.
        if (session_id_ && !state.handshake) add(rules_.send_name, *session_id_);
        if (endpoint_.authorizer) {
            const std::string value = endpoint_.authorizer->authorization(info(unit));
            if (!value.empty()) add(field_authorization, value);
        }
        request.body = std::move(outgoing.line);
        units_[unit] = state;
        if (problem) {
            delivery_->push(unit, SendFailed{*problem});
            return;
        }
        loop_->submit(std::move(request));
    }

    void end(UnitId unit) {
        const auto found = units_.find(unit);
        if (found == units_.end()) return;
        units_.erase(found);
        loop_->remove(unit);
    }

    // The session id of a handshake's answer; why it cannot be used, otherwise.
    std::optional<std::string> adopt_session(const std::vector<HttpHeader>& headers) {
        std::vector<std::string_view> values;
        for (const HttpHeader& header : headers) {
            for (const std::string& name : rules_.header_names) {
                if (http::equal_ignoring_case(header.name, name)) values.push_back(header.value);
            }
        }
        if (values.empty()) {
            session_id_.reset();
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
        return std::nullopt;
    }

    // On the client's thread: the transport's own part first, then the client's, last, so that nothing of the
    // transport is touched after the client was told.
    void on_item(UnitId unit, TransferItem item) {
        if (closed_ || events_ == nullptr) return;
        const auto found = units_.find(unit);
        if (found == units_.end()) return;
        TransportEvents& events = *events_;
        if (auto* head = std::get_if<TransferHead>(&item)) {
            if (found->second.handshake && is_success(head->status)) {
                if (auto problem = adopt_session(head->headers)) {
                    units_.erase(found);
                    loop_->remove(unit);
                    events.stream_event(unit, StreamLost{std::move(*problem)});
                    return;
                }
            }
            if ((head->status == status_unauthorized || head->status == status_forbidden) && endpoint_.authorizer) {
                std::vector<std::string> challenges;
                for (const HttpHeader& header : head->headers) {
                    if (http::equal_ignoring_case(header.name, field_www_authenticate)) {
                        challenges.push_back(header.value);
                    }
                }
                endpoint_.authorizer->challenge(info(unit), head->status, challenges);
            }
            events.stream_event(unit, ResponseHead{head->status, std::move(head->media_type), std::move(head->headers)});
            return;
        }
        if (auto* message = std::get_if<TransferMessage>(&item)) {
            static_cast<void>(events.received(unit, message->text));
            return;
        }
        const bool terminal = std::holds_alternative<StreamEnded>(item) || std::holds_alternative<StreamLost>(item) ||
                              std::holds_alternative<SendFailed>(item);
        if (terminal) units_.erase(found);
        std::visit(
            [&](auto& event) {
                using Event = std::decay_t<decltype(event)>;
                if constexpr (!std::is_same_v<Event, TransferHead> && !std::is_same_v<Event, TransferMessage>) {
                    events.stream_event(unit, std::move(event));
                }
            },
            item);
    }

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
};

}  // namespace

Result<std::unique_ptr<Transport>> make_http_transport(HttpEndpoint endpoint, HttpTransportSettings settings) {
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
    SessionRules rules = session_rules(settings);
    std::unique_ptr<Transport> transport = std::make_unique<StreamableTransport>(
        std::move(endpoint), std::get<UrlGuard>(std::move(guard)), std::move(rules), std::move(limits),
        std::move(global));
    return transport;
}

}  // namespace detail::http_client

namespace {

using detail::http_client::HttpTransportSettings;
using detail::http_client::make_http_transport;

HttpTransportSettings settings_of(const ClientOptions& options) {
    HttpTransportSettings settings;
    settings.profiles = options.profiles;
    settings.mechanisms = options.mechanisms;
    settings.max_message_bytes = options.max_message_bytes;
    return settings;
}

}  // namespace

Result<std::unique_ptr<Transport>> make_transport(HttpEndpoint endpoint) {
    return make_http_transport(std::move(endpoint), HttpTransportSettings{});
}

Result<Client> Client::open(ClientOptions options, HttpEndpoint endpoint) {
    auto transport = make_http_transport(std::move(endpoint), settings_of(options));
    if (!transport) return std::move(transport).error();
    return open(std::move(options), std::move(transport).value());
}

asio::awaitable<Result<Client>> Client::async_open(ClientOptions options, HttpEndpoint endpoint) {
    auto transport = make_http_transport(std::move(endpoint), settings_of(options));
    if (!transport) return failed_open(std::move(transport).error());
    return async_open(std::move(options), std::move(transport).value());
}

}  // namespace neomcp
