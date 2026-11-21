// The part of an HTTP transport that takes part in authorization (authorization_gate.h), and the reply an authorizer
// answers a refusal through (neomcp/client/http.h).
#include "client_http/authorization_gate.h"

#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include "neomcp/http/headers.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail::http_client {

namespace {

constexpr std::string_view field_www_authenticate = "WWW-Authenticate";
// The line that keeps libcurl from adding `Expect: 100-continue` to a body.
constexpr const char* no_expect_line = "Expect:";

CallFailure authorization_failure(AuthorizationProblem problem, std::string message) {
    CallFailure failure;
    failure.kind = CallFailureKind::authorization_failed;
    failure.message = std::move(message);
    failure.authorization = AuthorizationFailure{problem, std::string(), std::string(), std::nullopt};
    return failure;
}

CallFailure no_answer() {
    return authorization_failure(AuthorizationProblem::no_answer,
                                 "the authorizer dropped the refused request without answering it");
}

AuthorizationFetchResult fetch_failed(AuthorizationFetchError error, std::string message) {
    AuthorizationFetchResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

bool metadata(AuthorizationFetchPurpose purpose) {
    return purpose == AuthorizationFetchPurpose::resource_metadata ||
           purpose == AuthorizationFetchPurpose::server_metadata;
}

}  // namespace

// ---- the reply channel

ReplyChannel::ReplyChannel(asio::any_io_executor executor, Apply apply)
    : executor_(std::move(executor)), apply_(std::move(apply)) {}

void ReplyChannel::answer(std::uint64_t flow, std::optional<CallFailure> failure) {
    std::lock_guard lock(mutex_);
    if (!open_) return;
    asio::post(executor_, [apply = apply_, flow, failure = std::move(failure)]() mutable {
        apply(flow, std::move(failure));
    });
}

void ReplyChannel::close() {
    std::lock_guard lock(mutex_);
    open_ = false;
}

ChallengeReply ReplyChannel::reply(std::shared_ptr<ReplyChannel> channel, std::uint64_t flow) {
    return ChallengeReply(std::move(channel), flow);
}

// ---- the claim

bool AuthorizerClaim::take(HttpAuthorizer& authorizer) noexcept { return !authorizer.claimed_.exchange(true); }

void AuthorizerClaim::release(HttpAuthorizer& authorizer) noexcept { authorizer.claimed_.store(false); }

// ---- the gate

struct AuthorizationGate::State final : AuthorizationHost, std::enable_shared_from_this<State> {
    // One fetch of the authorizer: woken by its end, or by its clock.
    struct Fetch {
        explicit Fetch(const asio::any_io_executor& executor) : timer(executor) {}
        asio::steady_timer timer;
        UnitId unit = connection_unit;
        std::size_t bound = 0;
        bool done = false;
        AuthorizationFetchResult result;
    };

    // One sign-in under way: the generation of the credentials whose refusal opened it.
    struct Flow {
        std::uint64_t generation = 0;
    };

    // The refusals of one unit's request in a row, and the request as the authorizer was told of it.
    struct Refusals {
        std::size_t count = 0;
        HttpRequestInfo request;
    };

    // A request that waits for a flow's answer.
    struct Hold {
        std::uint64_t flow = 0;
        int status = 0;
    };

    State(std::shared_ptr<HttpAuthorizer> authorizer_in, std::string endpoint_url_in, UrlGuard guard_in,
          std::size_t max_holds_in)
        : authorizer(std::move(authorizer_in)),
          endpoint(std::move(endpoint_url_in)),
          guard(std::move(guard_in)),
          max_holds(max_holds_in) {}

    // ---- AuthorizationHost

    asio::any_io_executor executor() const override { return client_executor; }

    const std::string& endpoint_url() const override { return endpoint; }

    std::span<const profile::ProfilePtr> profiles() const override { return enabled; }

    asio::awaitable<AuthorizationFetchResult> fetch(AuthorizationFetch request) override {
        return run_fetch(weak_from_this(), std::move(request));
    }

    // Posted, so that a report made inside attach() (inside the transport's start()) reaches the client afterwards.
    void diagnostic(std::string message) override {
        if (!attached) return;
        asio::post(client_executor, [weak = weak_from_this(), message = std::move(message)]() mutable {
            const std::shared_ptr<State> self = weak.lock();
            if (!self || !self->attached || !self->hooks.diagnostic) return;
            self->hooks.diagnostic(
                Diagnostic{DiagnosticKind::authorization, std::move(message), std::nullopt, std::nullopt});
        });
    }

    // ---- fetches

    // The fetch's coroutine holds the gate only weakly: the transport may close while it waits.
    static asio::awaitable<AuthorizationFetchResult> run_fetch(std::weak_ptr<State> weak, AuthorizationFetch request) {
        std::shared_ptr<Fetch> fetch;
        {
            const std::shared_ptr<State> self = weak.lock();
            if (!self || !self->attached) {
                co_return fetch_failed(AuthorizationFetchError::closed, "the transport is closed");
            }
            auto started = self->begin_fetch(std::move(request));
            if (auto* refusal = std::get_if<AuthorizationFetchResult>(&started)) co_return std::move(*refusal);
            fetch = std::get<std::shared_ptr<Fetch>>(std::move(started));
        }
        std::error_code woken;
        co_await fetch->timer.async_wait(asio::redirect_error(asio::use_awaitable, woken));
        if (!fetch->done) {
            if (const std::shared_ptr<State> self = weak.lock()) self->end_fetch(fetch->unit);
            fetch->done = true;
            fetch->result = fetch_failed(AuthorizationFetchError::timed_out, "the exchange took longer than its timeout");
        }
        co_return std::move(fetch->result);
    }

    std::variant<std::shared_ptr<Fetch>, AuthorizationFetchResult> begin_fetch(AuthorizationFetch request) {
        if (request.max_body_bytes == 0 || request.timeout.count() <= 0) {
            return fetch_failed(AuthorizationFetchError::invalid_request, "a bound of the fetch is not positive");
        }
        if (auto refusal = guard.admit(UrlPurpose::authorization, request.url)) {
            return fetch_failed(AuthorizationFetchError::refused_url, std::move(*refusal));
        }
        TransferRequest transfer;
        transfer.unit = next_fetch++;
        transfer.url = std::move(request.url);
        transfer.whole_body = true;
        transfer.max_body_bytes = request.max_body_bytes;
        const bool get = request.method == AuthorizationFetchMethod::get;
        transfer.method = get ? TransferMethod::get : TransferMethod::post;
        transfer.redirects = get && metadata(request.purpose) ? RedirectRule::request_origin : RedirectRule::none;
        for (const HttpHeader& field : request.headers) {
            auto line = header_line(field.name, field.value);
            if (!line) {
                return fetch_failed(AuthorizationFetchError::invalid_request,
                                    "the header field " + field.name + " cannot be sent as it is");
            }
            transfer.header_lines.push_back(std::move(*line));
        }
        if (!get) {
            transfer.header_lines.emplace_back(no_expect_line);
            transfer.body = std::move(request.body);
        }
        auto fetch = std::make_shared<Fetch>(client_executor);
        fetch->unit = transfer.unit;
        fetch->bound = request.max_body_bytes;
        fetch->timer.expires_after(request.timeout);
        fetches.emplace(transfer.unit, fetch);
        loop->submit(std::move(transfer));
        return fetch;
    }

    void end_fetch(UnitId unit) {
        if (fetches.erase(unit) != 0 && loop != nullptr) loop->remove(unit);
    }

    static void complete(Fetch& fetch, std::optional<AuthorizationFetchError> error, std::string message) {
        fetch.done = true;
        if (error) {
            fetch.result.error = error;
            fetch.result.message = std::move(message);
        }
        fetch.timer.cancel();
    }

    void on_fetch(UnitId unit, TransferItem item) {
        const auto found = fetches.find(unit);
        if (found == fetches.end()) return;
        const std::shared_ptr<Fetch> fetch = found->second;
        if (auto* head = std::get_if<TransferHead>(&item)) {
            fetch->result.status = head->status;
            fetch->result.media_type = std::move(head->media_type);
            fetch->result.headers = std::move(head->headers);
            return;
        }
        if (auto* message = std::get_if<TransferMessage>(&item)) {
            if (message->text.size() > fetch->bound) {
                end_fetch(unit);
                complete(*fetch, AuthorizationFetchError::too_large, "the answer's body exceeds max_body_bytes");
                return;
            }
            fetch->result.body = std::move(message->text);
            return;
        }
        if (std::holds_alternative<StreamEnded>(item)) {
            fetches.erase(found);
            complete(*fetch, std::nullopt, std::string());
            return;
        }
        if (auto* lost = std::get_if<StreamLost>(&item)) {
            fetches.erase(found);
            complete(*fetch, AuthorizationFetchError::transport, "the exchange broke: " + lost->cause);
            return;
        }
        if (auto* failed = std::get_if<SendFailed>(&item)) {
            fetches.erase(found);
            complete(*fetch, AuthorizationFetchError::transport, "the exchange could not be made: " + failed->cause);
        }
    }

    void end_fetches() {
        std::map<UnitId, std::shared_ptr<Fetch>> ending;
        ending.swap(fetches);
        for (auto& [unit, fetch] : ending) {
            if (loop != nullptr) loop->remove(unit);
            complete(*fetch, AuthorizationFetchError::closed, "the transport closed");
        }
    }

    // ---- refusals

    void refused(UnitId unit, const HttpRequestInfo& request, std::uint64_t generation, int status,
                 const std::vector<HttpHeader>& headers) {
        const std::shared_ptr<State> self = shared_from_this();  // a hook may close the transport
        if (!guard.carries_tokens()) {
            hooks.deny(unit,
                       authorization_failure(AuthorizationProblem::insecure_endpoint,
                                             "the endpoint is plain HTTP on a host that is not this machine, and no "
                                             "token is sent there"),
                       status);
            return;
        }
        Refusals& counted = counts[unit];
        counted.request = request;
        std::size_t& refusals = counted.count;
        if (refusals >= max_holds) {
            hooks.deny(unit,
                       authorization_failure(AuthorizationProblem::attempts_exceeded,
                                             "the request was refused for want of authorization more often than "
                                             "max_authorizations_per_request"),
                       status);
            return;
        }
        ++refusals;
        // Newer credentials exist already: the request goes out again with them, without asking the authorizer.
        if (generation < newest) {
            hooks.resend(unit);
            return;
        }
        // One flow per generation: a refusal of credentials whose flow runs joins it.
        std::uint64_t flow = 0;
        bool opened = false;
        if (const auto running = open.find(generation); running != open.end()) {
            flow = running->second;
        } else {
            flow = next_flow++;
            flows.emplace(flow, Flow{generation});
            open.emplace(generation, flow);
            opened = true;
        }
        holds[unit] = Hold{flow, status};
        hooks.hold(unit);
        if (opened && attached) ask(request, status, headers, generation, flow);
    }

    void ask(const HttpRequestInfo& request, int status, const std::vector<HttpHeader>& headers,
             std::uint64_t generation, std::uint64_t flow) {
        std::vector<std::string> challenges;
        for (const HttpHeader& field : headers) {
            if (http::equal_ignoring_case(field.name, field_www_authenticate)) challenges.push_back(field.value);
        }
        try {
            authorizer->challenge(request, status, challenges, generation, ReplyChannel::reply(channel, flow));
        } catch (...) {
            // The reply, dropped by the unwinding, answers for the authorizer (no_answer).
        }
    }

    // A reply's answer, on the client's thread.
    void apply(std::uint64_t flow, std::optional<CallFailure> failure) {
        if (!attached) return;
        const auto found = flows.find(flow);
        if (found == flows.end()) return;
        if (const auto running = open.find(found->second.generation); running != open.end() && running->second == flow) {
            open.erase(running);
        }
        flows.erase(found);
        std::vector<std::pair<UnitId, int>> waiting;
        for (auto it = holds.begin(); it != holds.end();) {
            if (it->second.flow == flow) {
                waiting.emplace_back(it->first, it->second.status);
                it = holds.erase(it);
            } else {
                ++it;
            }
        }
        const std::shared_ptr<State> self = shared_from_this();
        for (const auto& [unit, status] : waiting) {
            if (!attached) return;  // a hook closed the transport
            if (failure) {
                hooks.deny(unit, *failure, status);
            } else {
                hooks.resend(unit);
            }
        }
    }

    void forget(UnitId unit) {
        holds.erase(unit);
        counts.erase(unit);
    }

    void stop() {
        if (attached) {
            attached = false;
            channel->close();  // replies still held become inert
            flows.clear();
            open.clear();
            holds.clear();
            counts.clear();
            end_fetches();
            authorizer->detach();
        }
        if (claimed) {
            AuthorizerClaim::release(*authorizer);
            claimed = false;
        }
    }

    std::shared_ptr<HttpAuthorizer> authorizer;
    std::string endpoint;
    std::vector<profile::ProfilePtr> enabled;  // the client's enabled profiles, in its order
    UrlGuard guard;
    std::size_t max_holds = HttpClientDefaults::max_authorizations_per_request;
    bool claimed = false;
    bool attached = false;

    asio::any_io_executor client_executor;
    TransferLoop* loop = nullptr;
    GateHooks hooks;
    std::shared_ptr<ReplyChannel> channel;

    std::uint64_t newest = 0;  // the newest generation the authorizer gave
    std::uint64_t next_flow = 1;
    std::map<std::uint64_t, Flow> flows;          // open flows by id
    std::map<std::uint64_t, std::uint64_t> open;  // the open flow of each generation
    std::map<UnitId, Hold> holds;
    std::map<UnitId, Refusals> counts;  // refusals of each unit's request so far
    std::map<UnitId, std::shared_ptr<Fetch>> fetches;
    UnitId next_fetch = first_fetch_unit;
};

AuthorizationGate::AuthorizationGate(std::shared_ptr<State> state) : state_(std::move(state)) {}

std::variant<std::unique_ptr<AuthorizationGate>, ClientError> AuthorizationGate::make(
    const HttpEndpoint& endpoint, const UrlGuard& guard, std::vector<profile::ProfilePtr> profiles) {
    auto state = std::make_shared<State>(endpoint.authorizer, endpoint.url, guard,
                                         endpoint.max_authorizations_per_request);
    if (profiles.empty()) {
        for (const auto revision : profile::builtin_revisions()) profiles.push_back(profile::builtin(revision));
    }
    state->enabled = std::move(profiles);
    if (endpoint.authorizer) {
        if (!AuthorizerClaim::take(*endpoint.authorizer)) {
            ClientError error;
            error.kind = ClientErrorKind::invalid_options;
            error.message = "the authorizer serves another transport; an authorizer serves one transport at a time";
            return error;
        }
        state->claimed = true;
    }
    return std::unique_ptr<AuthorizationGate>(new AuthorizationGate(std::move(state)));
}

AuthorizationGate::~AuthorizationGate() { stop(); }

bool AuthorizationGate::configured() const noexcept { return state_->authorizer != nullptr; }

void AuthorizationGate::start(asio::any_io_executor executor, TransferLoop& loop, GateHooks hooks) {
    State& state = *state_;
    if (!state.authorizer || state.attached) return;
    state.client_executor = executor;
    state.loop = &loop;
    state.hooks = std::move(hooks);
    const std::weak_ptr<State> weak = state_;
    state.channel = std::make_shared<ReplyChannel>(
        std::move(executor), [weak](std::uint64_t flow, std::optional<CallFailure> failure) {
            if (const std::shared_ptr<State> self = weak.lock()) self->apply(flow, std::move(failure));
        });
    state.attached = true;
    state.authorizer->attach(state);
}

void AuthorizationGate::stop() { state_->stop(); }

bool AuthorizationGate::takes_refusals() const noexcept { return state_->authorizer && state_->attached; }

Credential AuthorizationGate::credential(const HttpRequestInfo& request) {
    State& state = *state_;
    if (!state.authorizer || !state.guard.carries_tokens()) return Credential{std::string(), state.newest};
    // Only toward the endpoint's origin.
    const auto origin = origin_of(request.url);
    if (!std::holds_alternative<Origin>(origin) || std::get<Origin>(origin) != state.guard.origin()) {
        return Credential{std::string(), state.newest};
    }
    Credential credential = state.authorizer->authorization(request);
    if (credential.generation > state.newest) state.newest = credential.generation;
    return credential;
}

void AuthorizationGate::refused(UnitId unit, const HttpRequestInfo& request, std::uint64_t generation, int status,
                                const std::vector<HttpHeader>& headers) {
    state_->refused(unit, request, generation, status, headers);
}

void AuthorizationGate::forget(UnitId unit) { state_->forget(unit); }

void AuthorizationGate::accepted(UnitId unit) {
    State& state = *state_;
    const auto found = state.counts.find(unit);
    if (found == state.counts.end()) return;
    const HttpRequestInfo request = std::move(found->second.request);
    state.counts.erase(found);
    if (!state.attached) return;
    try {
        state.authorizer->accepted(request);
    } catch (...) {
        // What the authorizer makes of a success is its own; the answer goes on to the client either way.
    }
}

bool AuthorizationGate::owns(UnitId unit) const noexcept {
    return unit >= first_fetch_unit && unit < state_->next_fetch;
}

void AuthorizationGate::on_fetch(UnitId unit, TransferItem item) { state_->on_fetch(unit, std::move(item)); }

}  // namespace detail::http_client

// ---- the reply

ChallengeReply::ChallengeReply(std::shared_ptr<detail::http_client::ReplyChannel> channel, std::uint64_t flow)
    : channel_(std::move(channel)), flow_(flow) {}

ChallengeReply::ChallengeReply(ChallengeReply&& other) noexcept
    : channel_(std::move(other.channel_)), flow_(other.flow_) {}

ChallengeReply& ChallengeReply::operator=(ChallengeReply&& other) noexcept {
    if (this == &other) return *this;
    try {
        answer(detail::http_client::no_answer());
    } catch (...) {
        // Posting the answer failed (no memory): the requests stay held until the transport closes.
    }
    channel_ = std::move(other.channel_);
    flow_ = other.flow_;
    return *this;
}

ChallengeReply::~ChallengeReply() {
    try {
        answer(detail::http_client::no_answer());
    } catch (...) {
        // As in the move assignment.
    }
}

void ChallengeReply::retry() { answer(std::nullopt); }

void ChallengeReply::fail(ClientError error) {
    CallFailure failure;
    if (error.kind == ClientErrorKind::authorization_failed) {
        failure.kind = CallFailureKind::authorization_failed;
        failure.authorization = std::move(error.authorization);
    } else {
        failure.kind = CallFailureKind::authorization_required;
    }
    failure.message = error.message.empty() ? std::string("the authorizer could not authorize the request")
                                            : std::move(error.message);
    answer(std::move(failure));
}

void ChallengeReply::answer(std::optional<CallFailure> failure) {
    if (!channel_) return;
    const std::shared_ptr<detail::http_client::ReplyChannel> channel = std::move(channel_);
    channel_.reset();
    channel->answer(flow_, std::move(failure));
}

}  // namespace neomcp
