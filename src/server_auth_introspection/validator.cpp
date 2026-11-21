// The introspection validator (include/neomcp/http/introspection.h).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <curl/curl.h>

#include "neomcp/http/bearer.h"
#include "neomcp/http/form.h"
#include "neomcp/http/introspection.h"
#include "neomcp/http/policy.h"
#include "server_auth_introspection/answer.h"
#include "server_auth_introspection/exchange.h"

namespace neomcp::http {

namespace {

using detail::ExchangeEnd;
using detail::ExchangeLoop;
using detail::ExchangeRequest;
using detail::ExchangeResult;
using Clock = std::chrono::system_clock;

// The form of the request (RFC 7662, section 2.1) and its header fields.
constexpr std::string_view field_token = "token";
constexpr std::string_view field_token_type_hint = "token_type_hint";
constexpr std::string_view token_type_access = "access_token";
constexpr std::string_view field_client_id = "client_id";
constexpr std::string_view field_client_secret = "client_secret";
constexpr std::string_view header_content_type = "Content-Type: application/x-www-form-urlencoded";
constexpr std::string_view header_accept = "Accept: application/json";
constexpr std::string_view scheme_http = "http";
constexpr std::string_view scheme_https = "https";

struct UrlFree {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};
struct PartFree {
    void operator()(char* part) const noexcept { curl_free(part); }
};
using Part = std::unique_ptr<char, PartFree>;

// The part of a URL as libcurl, which makes the request, reads it; nothing when the URL has none (a part that is
// empty, such as the fragment of "https://host/path#", is none: libcurl sends nothing of it either).
std::optional<std::string> url_part(CURLU* url, CURLUPart which) {
    char* value = nullptr;
    if (curl_url_get(url, which, &value, 0) != CURLUE_OK || value == nullptr) return std::nullopt;
    const Part owned(value);
    return std::string(owned.get());
}

// What is wrong with the endpoint's URL; empty when nothing. The text never holds the URL.
std::string endpoint_problem(const std::string& endpoint) {
    if (endpoint.empty()) return "the introspection endpoint is empty";
    if (endpoint.find('\0') != std::string::npos) return "the introspection endpoint cannot be read as a URL";
    const std::unique_ptr<CURLU, UrlFree> url(curl_url());
    if (!url || curl_url_set(url.get(), CURLUPART_URL, endpoint.c_str(), 0) != CURLUE_OK) {
        return "the introspection endpoint cannot be read as an absolute URL";
    }
    const auto scheme = url_part(url.get(), CURLUPART_SCHEME);
    if (!scheme || (*scheme != scheme_http && *scheme != scheme_https)) {
        return "the introspection endpoint must be an https URL, or an http URL with a loopback host";
    }
    if (url_part(url.get(), CURLUPART_USER) || url_part(url.get(), CURLUPART_PASSWORD) ||
        url_part(url.get(), CURLUPART_OPTIONS)) {
        return "the introspection endpoint must have no user information";
    }
    if (url_part(url.get(), CURLUPART_FRAGMENT)) return "the introspection endpoint must have no fragment";
    auto host = url_part(url.get(), CURLUPART_HOST);
    if (!host || host->empty()) return "the introspection endpoint must have a host";
    if (*scheme == scheme_http) {
        // libcurl writes an IPv6 address in brackets.
        if (host->size() >= 2 && host->front() == '[' && host->back() == ']') *host = host->substr(1, host->size() - 2);
        if (!is_loopback_address(*host)) {
            return "the introspection endpoint must be an https URL, or an http URL with a loopback host";
        }
    }
    return {};
}

bool is_header_value(std::string_view text) {
    for (const char c : text) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return !text.empty();
}

// Everything of the options that a request needs, checked once.
struct Prepared {
    std::vector<std::string> header_lines;      // the fields that do not change from request to request
    std::vector<FormField> credential_fields;   // the fields client_secret_post adds
};

Outcome<Prepared> prepare(const IntrospectionOptions& options) {
    using Result = Outcome<Prepared>;
    Prepared prepared;
    prepared.header_lines.emplace_back(header_content_type);
    prepared.header_lines.emplace_back(header_accept);
    if (const auto* basic = std::get_if<IntrospectionBasicAuth>(&options.authentication)) {
        if (basic->client_id.empty() || basic->client_secret.empty()) {
            return Result::failure("the client's credentials for the introspection endpoint are incomplete");
        }
        prepared.header_lines.push_back(std::string(authorization_field_name) + ": " +
                                        oauth_basic_authorization(basic->client_id, basic->client_secret));
    } else if (const auto* post = std::get_if<IntrospectionPostAuth>(&options.authentication)) {
        if (post->client_id.empty() || post->client_secret.empty()) {
            return Result::failure("the client's credentials for the introspection endpoint are incomplete");
        }
        prepared.credential_fields.push_back(FormField{std::string(field_client_id), post->client_id});
        prepared.credential_fields.push_back(FormField{std::string(field_client_secret), post->client_secret});
    } else {
        const auto& bearer = std::get<IntrospectionBearerAuth>(options.authentication);
        if (!is_header_value(bearer.token)) {
            return Result::failure("the bearer token for the introspection endpoint cannot be sent as a header field");
        }
        prepared.header_lines.push_back(std::string(authorization_field_name) + ": " + std::string(bearer_scheme) +
                                        " " + bearer.token);
    }
    return Result::success(std::move(prepared));
}

std::string check_options(const IntrospectionOptions& options) {
    if (std::string problem = endpoint_problem(options.endpoint); !problem.empty()) return problem;
    if (options.issuer.empty()) return "the issuer is empty";
    const IntrospectionBounds& bounds = options.bounds;
    if (bounds.max_response_bytes < 1) return "max_response_bytes must be at least 1";
    if (bounds.request_timeout <= std::chrono::milliseconds::zero()) return "request_timeout must be positive";
    if (bounds.connect_timeout <= std::chrono::milliseconds::zero()) return "connect_timeout must be positive";
    if (bounds.connect_timeout > bounds.request_timeout) return "connect_timeout must not be more than request_timeout";
    if (bounds.max_response_header_bytes < 1) return "max_response_header_bytes must be at least 1";
    if (bounds.max_pending < 1) return "max_pending must be at least 1";
    if (bounds.default_lifetime <= std::chrono::seconds::zero()) return "default_lifetime must be positive";
    if (const auto problem = detail::exchange_resolver_problem(options.allow_blocking_resolver)) return *problem;
    return {};
}

// What the threads share. The loop thread finishes requests into it and the delivery thread drains it; the validator
// holds the loop. Nothing here owns the validator, so a reply that ends the validator's life cannot join its own thread
// through this.
struct State {
    struct Delivery {
        TokenReply reply;
        TokenVerdict verdict{TokenRefusal::unavailable};
        IntrospectionEvent event = IntrospectionEvent::stopped;
    };

    explicit State(IntrospectionOptions given, Prepared prepared_in)
        : options(std::move(given)), prepared(std::move(prepared_in)) {}

    Clock::time_point now() const { return options.clock ? options.clock() : Clock::now(); }

    // Hands a verdict to the delivery thread.
    void deliver(Delivery delivery) {
        {
            std::lock_guard lock(mutex);
            deliveries.push_back(std::move(delivery));
        }
        changed.notify_one();
    }

    // The loop thread: a request ended.
    void finish(std::uint64_t id, ExchangeResult result) {
        TokenReply reply;
        {
            std::lock_guard lock(mutex);
            const auto found = waiting.find(id);
            if (found == waiting.end()) return;
            reply = std::move(found->second);
            waiting.erase(found);
        }
        detail::IntrospectionReading reading;
        switch (result.end) {
            case ExchangeEnd::response: {
                detail::IntrospectionContext context;
                context.issuer = options.issuer;
                context.now = now();
                context.default_lifetime = options.bounds.default_lifetime;
                context.max_bytes = options.bounds.max_response_bytes;
                try {
                    reading = detail::read_introspection_answer(result.status, result.body, context);
                } catch (...) {
                    // Nothing in the reading throws but for lack of memory; a request that could not be read is one
                    // the endpoint cannot decide now, and the loop thread must not end the process.
                    reading = detail::IntrospectionReading{TokenVerdict{TokenRefusal::unavailable},
                                                           IntrospectionEvent::not_an_object};
                }
                break;
            }
            case ExchangeEnd::body_too_large:
                reading = detail::IntrospectionReading{TokenVerdict{TokenRefusal::unavailable},
                                                       IntrospectionEvent::response_too_large};
                break;
            case ExchangeEnd::failed:
                reading = detail::IntrospectionReading{TokenVerdict{TokenRefusal::unavailable},
                                                       IntrospectionEvent::transport_failed};
                break;
        }
        deliver(Delivery{std::move(reply), std::move(reading.verdict), reading.event});
    }

    // The delivery thread.
    void run_deliveries() {
        std::unique_lock lock(mutex);
        while (true) {
            changed.wait(lock, [this] { return stopping || !deliveries.empty(); });
            if (deliveries.empty()) return;  // stopping, and nothing left
            Delivery next = std::move(deliveries.front());
            deliveries.pop_front();
            lock.unlock();
            if (options.observer) {
                try {
                    options.observer(next.event);
                } catch (...) {
                    // An observer that throws changes nothing about the verdict.
                }
            }
            try {
                if (next.reply) next.reply(std::move(next.verdict));
            } catch (...) {
                // The reply is the endpoint's; it holds nothing that could go wrong here.
            }
            next = Delivery{};
            lock.lock();
        }
    }

    const IntrospectionOptions options;
    const Prepared prepared;

    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::uint64_t, TokenReply> waiting;  // requests in progress
    std::uint64_t next_id = 1;
    std::deque<Delivery> deliveries;
    bool stopping = false;
};

class IntrospectionValidator final : public TokenValidator {
public:
    explicit IntrospectionValidator(std::shared_ptr<State> state) : state_(std::move(state)) {}

    // Starts both threads; why it could not, otherwise.
    std::optional<std::string> start() {
        if (auto problem = loop_.start()) return problem;
        try {
            deliverer_ = std::thread([state = state_] { state->run_deliveries(); });
        } catch (const std::system_error&) {
            loop_.stop();
            return std::string("the thread that hands verdicts over could not be started");
        }
        return std::nullopt;
    }

    ~IntrospectionValidator() override {
        loop_.stop();
        std::map<std::uint64_t, TokenReply> unanswered;
        {
            std::lock_guard lock(state_->mutex);
            unanswered.swap(state_->waiting);
        }
        for (auto& [id, reply] : unanswered) {
            state_->deliver(State::Delivery{std::move(reply), TokenVerdict{TokenRefusal::unavailable},
                                            IntrospectionEvent::stopped});
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
        }
        state_->changed.notify_all();
        if (!deliverer_.joinable()) return;
        // A reply that ended the endpoint, and with it this validator, runs on the thread that would be joined.
        if (deliverer_.get_id() == std::this_thread::get_id()) {
            deliverer_.detach();
        } else {
            deliverer_.join();
        }
    }

    void validate(const TokenCheck& check, TokenReply reply) override {
        const IntrospectionOptions& options = state_->options;
        std::uint64_t id = 0;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->waiting.size() >= options.bounds.max_pending) {
                id = 0;
            } else {
                id = state_->next_id++;
                state_->waiting.emplace(id, reply);
            }
        }
        if (id == 0) {
            state_->deliver(State::Delivery{std::move(reply), TokenVerdict{TokenRefusal::unavailable},
                                            IntrospectionEvent::overloaded});
            return;
        }
        std::vector<FormField> fields;
        fields.push_back(FormField{std::string(field_token), std::string(check.token)});
        fields.push_back(FormField{std::string(field_token_type_hint), std::string(token_type_access)});
        fields.insert(fields.end(), state_->prepared.credential_fields.begin(),
                      state_->prepared.credential_fields.end());
        ExchangeRequest request;
        request.url = options.endpoint;
        request.header_lines = state_->prepared.header_lines;
        request.body = form_encode(fields);
        request.total_timeout = options.bounds.request_timeout;
        request.connect_timeout = options.bounds.connect_timeout;
        request.max_body_bytes = options.bounds.max_response_bytes;
        request.max_header_bytes = options.bounds.max_response_header_bytes;
        request.use_proxy_from_environment = options.use_proxy_from_environment;
        if (options.ca_file) request.ca_file = options.ca_file->string();
        loop_.submit(std::move(request), [state = state_, id](ExchangeResult result) {
            state->finish(id, std::move(result));
        });
    }

private:
    std::shared_ptr<State> state_;
    ExchangeLoop loop_;
    std::thread deliverer_;
};

}  // namespace

std::string_view to_string(IntrospectionEvent event) noexcept {
    switch (event) {
        case IntrospectionEvent::active: return "active";
        case IntrospectionEvent::inactive: return "inactive";
        case IntrospectionEvent::malformed: return "malformed";
        case IntrospectionEvent::issuer_mismatch: return "issuer_mismatch";
        case IntrospectionEvent::transport_failed: return "transport_failed";
        case IntrospectionEvent::unexpected_status: return "unexpected_status";
        case IntrospectionEvent::response_too_large: return "response_too_large";
        case IntrospectionEvent::not_an_object: return "not_an_object";
        case IntrospectionEvent::overloaded: return "overloaded";
        case IntrospectionEvent::stopped: return "stopped";
    }
    return "unknown";
}

Outcome<std::shared_ptr<TokenValidator>> make_introspection_validator(IntrospectionOptions options) {
    using Result = Outcome<std::shared_ptr<TokenValidator>>;
    if (std::string problem = check_options(options); !problem.empty()) return Result::failure(std::move(problem));
    auto prepared = prepare(options);
    if (!prepared) return Result::failure(prepared.error);
    auto state = std::make_shared<State>(std::move(options), std::move(*prepared.value));
    auto validator = std::make_shared<IntrospectionValidator>(std::move(state));
    if (auto problem = validator->start()) return Result::failure(std::move(*problem));
    return Result::success(std::move(validator));
}

}  // namespace neomcp::http
