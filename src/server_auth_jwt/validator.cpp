// The JWT validator (include/neomcp/http/jwt.h).
#include <algorithm>
#include <chrono>
#include <condition_variable>
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

#include "neomcp/http/jwt.h"
#include "server_auth_jwt/fetch.h"
#include "server_auth_jwt/keys.h"
#include "server_auth_jwt/token.h"

namespace neomcp::http {

namespace {

using detail::FetchEnd;
using detail::FetchRequest;
using detail::FetchResult;
using detail::KeySet;
using Steady = std::chrono::steady_clock;

constexpr int status_ok = 200;
constexpr std::string_view jwks_uri_name = "the key set URL";
constexpr std::string_view metadata_jwks_uri_name = "the key set URL of the metadata";

// What a token's evaluation found: a verdict, or that the key set holds no key for it.
struct Decision {
    std::optional<TokenVerdict> verdict;  // empty: no key fits
    JwtEvent event = JwtEvent::malformed;
};

Decision decided(TokenVerdict verdict, JwtEvent event) { return Decision{std::move(verdict), event}; }
Decision invalid(JwtEvent event) { return decided(TokenVerdict{TokenRefusal::invalid}, event); }
Decision unavailable(JwtEvent event) { return decided(TokenVerdict{TokenRefusal::unavailable}, event); }

std::string check_options(const JwtOptions& options) {
    if (options.issuer.empty()) return "the issuer is empty";
    if (options.algorithms.empty()) return "the list of algorithms is empty";
    if (!options.jwks_uri.empty()) {
        if (std::string problem = detail::fetch_url_problem(options.jwks_uri, jwks_uri_name); !problem.empty()) {
            return problem;
        }
    } else if (!detail::metadata_url_of(options.issuer)) {
        return "without a key set URL, the issuer must be an https URL, or an http URL with a loopback host, without "
               "user information, query or fragment, so that its metadata can be read";
    }
    const JwtBounds& bounds = options.bounds;
    if (bounds.max_jwks_bytes < 1) return "max_jwks_bytes must be at least 1";
    if (bounds.max_metadata_bytes < 1) return "max_metadata_bytes must be at least 1";
    if (bounds.max_keys < 1) return "max_keys must be at least 1";
    if (bounds.min_key_refresh_interval < std::chrono::milliseconds::zero()) {
        return "min_key_refresh_interval must not be negative";
    }
    if (bounds.max_key_set_age < bounds.min_key_refresh_interval) {
        return "max_key_set_age must not be less than min_key_refresh_interval";
    }
    if (bounds.request_timeout <= std::chrono::milliseconds::zero()) return "request_timeout must be positive";
    if (bounds.connect_timeout <= std::chrono::milliseconds::zero()) return "connect_timeout must be positive";
    if (bounds.connect_timeout > bounds.request_timeout) return "connect_timeout must not be more than request_timeout";
    if (bounds.max_response_header_bytes < 1) return "max_response_header_bytes must be at least 1";
    if (bounds.max_pending < 1) return "max_pending must be at least 1";
    if (const auto problem = detail::key_set_resolver_problem(options.allow_blocking_resolver)) return *problem;
    return {};
}

// What the validator and its thread share. The thread holds it, so that a reply that ends the validator's life on
// that thread leaves the thread something to finish with.
struct State {
    struct Waiting {
        std::string token;
        TokenReply reply;
    };

    State(JwtOptions given, std::optional<std::string> metadata)
        : options(std::move(given)), metadata_url(std::move(metadata)) {}

    Steady::time_point now() const { return options.clock ? options.clock() : Steady::now(); }

    void report(JwtEvent event) const {
        if (!options.observer) return;
        try {
            options.observer(event);
        } catch (...) {
            // An observer that throws changes nothing about the verdict.
        }
    }

    void answer(const TokenReply& reply, TokenVerdict verdict, JwtEvent event) const {
        report(event);
        try {
            if (reply) reply(std::move(verdict));
        } catch (...) {
            // The reply is the endpoint's; it holds nothing that could go wrong here.
        }
    }

    // What `token` says with the keys of `set` (none yet: null).
    Decision evaluate(std::string_view token, const KeySet* set) const {
        const std::optional<detail::CompactParts> parts = detail::split_compact(token);
        if (!parts) return invalid(JwtEvent::malformed);
        auto header = detail::read_header(parts->header, options.algorithms);
        if (const auto* refused = std::get_if<JwtEvent>(&header)) return invalid(*refused);
        const auto& read = std::get<detail::JoseHeader>(header);
        if (set == nullptr) return Decision{};
        const auto candidates = set->candidates(read.algorithm, read.kid);
        if (candidates.empty()) return Decision{};
        const bool good = std::any_of(candidates.begin(), candidates.end(), [&](const auto& key) {
            return key->verifies(parts->signing_input, parts->signature);
        });
        if (!good) return invalid(JwtEvent::signature_failed);
        auto claims = detail::read_claims(parts->payload, options.issuer);
        if (const auto* refused = std::get_if<JwtEvent>(&claims)) return invalid(*refused);
        return decided(TokenVerdict{std::move(std::get<Authorization>(claims))}, JwtEvent::accepted);
    }

    // Under `mutex`: whether a fetch may start now, by the bound on how often.
    bool may_fetch(Steady::time_point at) const {
        return !last_fetch_start || at - *last_fetch_start >= options.bounds.min_key_refresh_interval;
    }

    // Under `mutex`: asks the thread for a fetch.
    void want_fetch(Steady::time_point at) {
        fetch_wanted = true;
        last_fetch_start = at;
        changed.notify_all();
    }

    FetchRequest request_for(std::string url, std::size_t max_body_bytes) const {
        FetchRequest request;
        request.url = std::move(url);
        request.total_timeout = options.bounds.request_timeout;
        request.connect_timeout = options.bounds.connect_timeout;
        request.max_body_bytes = max_body_bytes;
        request.max_header_bytes = options.bounds.max_response_header_bytes;
        request.use_proxy_from_environment = options.use_proxy_from_environment;
        if (options.ca_file) request.ca_file = options.ca_file->string();
        return request;
    }

    // The thread: the key set, or null when it could not be fetched.
    std::shared_ptr<const KeySet> fetch_key_set() {
        try {
            std::string url = options.jwks_uri;
            if (url.empty()) {
                const FetchResult metadata = fetcher.fetch(request_for(*metadata_url, options.bounds.max_metadata_bytes));
                if (metadata.end != FetchEnd::response || metadata.status != status_ok) return nullptr;
                auto named = detail::read_metadata_jwks_uri(metadata.body, options.issuer);
                if (!named || !detail::fetch_url_problem(*named, metadata_jwks_uri_name).empty()) return nullptr;
                url = std::move(*named);
            }
            const FetchResult response = fetcher.fetch(request_for(std::move(url), options.bounds.max_jwks_bytes));
            if (response.end != FetchEnd::response || response.status != status_ok) return nullptr;
            auto read = detail::read_key_set(response.body, options.algorithms, options.bounds.max_keys);
            if (!read) return nullptr;
            return std::make_shared<const KeySet>(std::move(*read));
        } catch (...) {
            // Nothing here throws but for lack of memory; the thread must not end the process.
            return nullptr;
        }
    }

    // The thread.
    void run() {
        std::unique_lock lock(mutex);
        while (true) {
            changed.wait(lock, [this] { return stopping || fetch_wanted; });
            if (stopping) return;
            fetch_wanted = false;
            fetching = true;
            lock.unlock();
            std::shared_ptr<const KeySet> fetched = fetch_key_set();
            report(fetched ? JwtEvent::key_set_fetched : JwtEvent::key_set_fetch_failed);
            lock.lock();
            if (fetched) {
                keys = fetched;
                keys_fetched_at = now();
            }
            fetching = false;
            std::vector<Waiting> answered;
            answered.swap(waiting);
            const bool stopped = stopping;
            lock.unlock();
            for (Waiting& next : answered) {
                Decision decision;
                if (stopped) {
                    decision = unavailable(JwtEvent::stopped);
                } else if (!fetched) {
                    // The token waited because the set held no key for it, and no newer set could be had.
                    decision = unavailable(JwtEvent::key_set_unavailable);
                } else {
                    try {
                        decision = evaluate(next.token, fetched.get());
                    } catch (...) {
                        decision = unavailable(JwtEvent::key_set_unavailable);
                    }
                    if (!decision.verdict) decision = invalid(JwtEvent::unknown_key);
                }
                answer(next.reply, std::move(*decision.verdict), decision.event);
                next = Waiting{};
            }
            lock.lock();
        }
    }

    const JwtOptions options;
    const std::optional<std::string> metadata_url;  // when the key set URL comes from the metadata
    detail::Fetcher fetcher;

    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const KeySet> keys;                   // the latest set fetched; null before the first
    std::optional<Steady::time_point> keys_fetched_at;    // when it came
    std::optional<Steady::time_point> last_fetch_start;   // when the latest fetch was asked for
    bool fetch_wanted = false;
    bool fetching = false;
    bool stopping = false;
    std::vector<Waiting> waiting;  // tokens for the fetch in progress, or the one wanted
};

class JwtValidator final : public TokenValidator {
public:
    explicit JwtValidator(std::shared_ptr<State> state) : state_(std::move(state)) {}

    // Starts the thread; why it could not, otherwise.
    std::optional<std::string> start() {
        if (auto problem = state_->fetcher.start()) return problem;
        try {
            thread_ = std::thread([state = state_] { state->run(); });
        } catch (const std::system_error&) {
            return std::string("the thread that fetches the key set could not be started");
        }
        return std::nullopt;
    }

    ~JwtValidator() override {
        std::vector<State::Waiting> unanswered;
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
            unanswered.swap(state_->waiting);
        }
        state_->fetcher.interrupt();
        state_->changed.notify_all();
        if (thread_.joinable()) {
            // A reply that ended the endpoint, and with it this validator, runs on the thread that would be joined.
            if (thread_.get_id() == std::this_thread::get_id()) {
                thread_.detach();
            } else {
                thread_.join();
            }
        }
        for (State::Waiting& next : unanswered) {
            state_->answer(next.reply, TokenVerdict{TokenRefusal::unavailable}, JwtEvent::stopped);
        }
    }

    void validate(const TokenCheck& check, TokenReply reply) override {
        State& state = *state_;
        while (true) {
            std::shared_ptr<const KeySet> keys;
            {
                std::lock_guard lock(state.mutex);
                keys = state.keys;
                // A set older than its bound is fetched again; this token is decided with it meanwhile.
                const Steady::time_point at = state.now();
                if (keys && !state.fetch_wanted && !state.fetching && state.keys_fetched_at &&
                    at - *state.keys_fetched_at >= state.options.bounds.max_key_set_age && state.may_fetch(at)) {
                    state.want_fetch(at);
                }
            }
            Decision decision = state.evaluate(check.token, keys.get());
            if (decision.verdict) {
                state.answer(reply, std::move(*decision.verdict), decision.event);
                return;
            }
            // No key of the set fits: the token waits for a fetch, or is decided now when none may start.
            {
                std::lock_guard lock(state.mutex);
                if (state.keys != keys) continue;  // a newer set came meanwhile: decide with it
                const Steady::time_point at = state.now();
                if (state.stopping) {
                    decision = unavailable(JwtEvent::stopped);
                } else if (state.fetch_wanted || state.fetching || state.may_fetch(at)) {
                    if (state.waiting.size() >= state.options.bounds.max_pending) {
                        decision = unavailable(JwtEvent::overloaded);
                    } else {
                        if (!state.fetch_wanted && !state.fetching) state.want_fetch(at);
                        state.waiting.push_back(State::Waiting{std::string(check.token), std::move(reply)});
                        return;
                    }
                } else if (keys) {
                    decision = invalid(JwtEvent::unknown_key);
                } else {
                    decision = unavailable(JwtEvent::key_set_unavailable);
                }
            }
            state.answer(reply, std::move(*decision.verdict), decision.event);
            return;
        }
    }

private:
    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace

std::string_view to_string(JwtAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case JwtAlgorithm::rs256: return "RS256";
        case JwtAlgorithm::ps256: return "PS256";
        case JwtAlgorithm::es256: return "ES256";
    }
    return "unknown";
}

std::string_view to_string(JwtEvent event) noexcept {
    switch (event) {
        case JwtEvent::accepted: return "accepted";
        case JwtEvent::malformed: return "malformed";
        case JwtEvent::algorithm_refused: return "algorithm_refused";
        case JwtEvent::type_refused: return "type_refused";
        case JwtEvent::critical_header: return "critical_header";
        case JwtEvent::unknown_key: return "unknown_key";
        case JwtEvent::signature_failed: return "signature_failed";
        case JwtEvent::issuer_mismatch: return "issuer_mismatch";
        case JwtEvent::key_set_unavailable: return "key_set_unavailable";
        case JwtEvent::overloaded: return "overloaded";
        case JwtEvent::stopped: return "stopped";
        case JwtEvent::key_set_fetched: return "key_set_fetched";
        case JwtEvent::key_set_fetch_failed: return "key_set_fetch_failed";
    }
    return "unknown";
}

Outcome<std::shared_ptr<TokenValidator>> make_jwt_validator(JwtOptions options) {
    using Result = Outcome<std::shared_ptr<TokenValidator>>;
    if (std::string problem = check_options(options); !problem.empty()) return Result::failure(std::move(problem));
    std::optional<std::string> metadata;
    if (options.jwks_uri.empty()) metadata = detail::metadata_url_of(options.issuer);
    auto state = std::make_shared<State>(std::move(options), std::move(metadata));
    auto validator = std::make_shared<JwtValidator>(std::move(state));
    if (auto problem = validator->start()) return Result::failure(std::move(*problem));
    return Result::success(std::move(validator));
}

}  // namespace neomcp::http
