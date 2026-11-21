// The OAuth authorizer of the HTTP client and the store in memory (neomcp/client/oauth.h).
//
// OAuthAuthorizer drives the flow without I/O (client_auth/flow.h): it hands each request the flow describes to the
// transport (AuthorizationHost::fetch, never another path, so that every URL passes the transport's guard once more),
// the prompt to the application's handler, and the changes to the credentials to the store, and it answers the
// refusals the flow serves. It is built into the HTTP client's target, not the flow's, because it uses the transport's
// URL guard as the flow's URL policy (one parser and one rule for every URL a server supplies, the authorization
// endpoint included, which the library hands to the person and never fetches) and the reply type of the transport's
// seam.
//
// Everything runs on the client's thread but forget() (any thread) and a plain prompt handler (a thread of its own,
// which hands its answer back through a channel that closes when the sign-in no longer waits). One mutex guards the
// state; it is never held while the application's prompt handler runs or while asio may run a completion inline:
// coroutines are spawned from posted functions, and every completion takes the mutex itself.
#include "neomcp/client/oauth.h"

#include <chrono>
#include <exception>
#include <set>
#include <string_view>
#include <thread>
#include <variant>

#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "client_auth/flow.h"
#include "client_auth/http_names.h"
#include "client_auth/registration.h"
#include "client_auth/url.h"
#include "client_http/url_guard.h"
#include "neomcp/http/headers.h"
#include "neomcp/random.h"

namespace neomcp {

// ---------------------------------------------------------------------------------------------------------------
// MemoryTokenStore

std::optional<StoredTokens> MemoryTokenStore::tokens(const CredentialKey& key) {
    std::lock_guard lock(mutex_);
    const auto found = tokens_.find(key);
    if (found == tokens_.end()) return std::nullopt;
    return found->second;
}

void MemoryTokenStore::put_tokens(const CredentialKey& key, StoredTokens tokens) {
    std::lock_guard lock(mutex_);
    tokens_[key] = std::move(tokens);
}

void MemoryTokenStore::erase_tokens(const CredentialKey& key) {
    std::lock_guard lock(mutex_);
    tokens_.erase(key);
}

std::optional<StoredClient> MemoryTokenStore::client(const std::string& issuer) {
    std::lock_guard lock(mutex_);
    const auto found = clients_.find(issuer);
    if (found == clients_.end()) return std::nullopt;
    return found->second;
}

void MemoryTokenStore::put_client(const std::string& issuer, StoredClient client) {
    std::lock_guard lock(mutex_);
    clients_[issuer] = std::move(client);
}

void MemoryTokenStore::erase_client(const std::string& issuer) {
    std::lock_guard lock(mutex_);
    clients_.erase(issuer);
}

// ---------------------------------------------------------------------------------------------------------------
// The authorizer

namespace {

namespace oauth = detail::oauth;
namespace http_client = detail::http_client;

using FlowClock = oauth::FlowClock;

// The transport's rule for documents of authorization, as the flow's URL policy.
class GuardAdmission final : public oauth::UrlAdmission {
public:
    explicit GuardAdmission(http_client::UrlGuard guard) : guard_(std::move(guard)) {}
    std::optional<std::string> admit(oauth::UrlRole, std::string_view url) const override {
        return guard_.admit(http_client::UrlPurpose::authorization, url);
    }

private:
    http_client::UrlGuard guard_;
};

// The application's store, as the flow reads it.
class StoreLookup final : public oauth::CredentialLookup {
public:
    explicit StoreLookup(std::shared_ptr<TokenStore> store) : store_(std::move(store)) {}
    std::optional<StoredTokens> tokens(const CredentialKey& key) const override { return store_->tokens(key); }
    std::optional<StoredClient> client(const std::string& issuer) const override { return store_->client(issuer); }

private:
    std::shared_ptr<TokenStore> store_;
};

ClientError error_of(ClientErrorKind kind, std::string message) {
    ClientError error;
    error.kind = kind;
    error.message = std::move(message);
    return error;
}

// The problem as the application sees it. An answer of the browser that cannot belong to this sign-in (another URL
// than the redirect URI, too long, unreadable) is of the same kind as one with another state.
AuthorizationProblem public_problem(oauth::AuthorizationProblem problem) noexcept {
    switch (problem) {
        case oauth::AuthorizationProblem::discovery_failed: return AuthorizationProblem::discovery_failed;
        case oauth::AuthorizationProblem::resource_mismatch: return AuthorizationProblem::resource_mismatch;
        case oauth::AuthorizationProblem::metadata_issuer_mismatch:
            return AuthorizationProblem::metadata_issuer_mismatch;
        case oauth::AuthorizationProblem::pkce_unsupported: return AuthorizationProblem::pkce_unsupported;
        case oauth::AuthorizationProblem::refused_url: return AuthorizationProblem::refused_url;
        case oauth::AuthorizationProblem::insecure_endpoint: return AuthorizationProblem::insecure_endpoint;
        case oauth::AuthorizationProblem::no_client_registration: return AuthorizationProblem::no_client_registration;
        case oauth::AuthorizationProblem::registration_rejected: return AuthorizationProblem::registration_rejected;
        case oauth::AuthorizationProblem::prompt_cancelled: return AuthorizationProblem::prompt_cancelled;
        case oauth::AuthorizationProblem::prompt_timed_out: return AuthorizationProblem::prompt_timed_out;
        case oauth::AuthorizationProblem::callback_invalid: return AuthorizationProblem::state_mismatch;
        case oauth::AuthorizationProblem::state_mismatch: return AuthorizationProblem::state_mismatch;
        case oauth::AuthorizationProblem::issuer_mismatch: return AuthorizationProblem::issuer_mismatch;
        case oauth::AuthorizationProblem::authorization_denied: return AuthorizationProblem::authorization_denied;
        case oauth::AuthorizationProblem::token_request_failed: return AuthorizationProblem::token_request_failed;
        case oauth::AuthorizationProblem::unsupported_token_type: return AuthorizationProblem::unsupported_token_type;
        case oauth::AuthorizationProblem::insufficient_scope: return AuthorizationProblem::insufficient_scope;
        case oauth::AuthorizationProblem::forbidden: return AuthorizationProblem::forbidden;
        case oauth::AuthorizationProblem::attempts_exceeded: return AuthorizationProblem::attempts_exceeded;
        case oauth::AuthorizationProblem::document_too_large: return AuthorizationProblem::document_too_large;
        case oauth::AuthorizationProblem::timed_out: return AuthorizationProblem::timed_out;
        case oauth::AuthorizationProblem::random_source_failed: return AuthorizationProblem::random_source_failed;
    }
    return AuthorizationProblem::no_answer;
}

std::string_view sentence_for(oauth::AuthorizationProblem problem) noexcept {
    switch (problem) {
        case oauth::AuthorizationProblem::discovery_failed:
            return "the documents that name the server's authorization server could not be found or read";
        case oauth::AuthorizationProblem::resource_mismatch:
            return "the server's protected resource metadata describes another resource than the endpoint";
        case oauth::AuthorizationProblem::metadata_issuer_mismatch:
            return "the authorization server's metadata names another issuer than the one it was found by";
        case oauth::AuthorizationProblem::pkce_unsupported:
            return "the authorization server does not offer the S256 code challenge method";
        case oauth::AuthorizationProblem::refused_url:
            return "a URL the server or the authorization server named is not one the client reaches";
        case oauth::AuthorizationProblem::insecure_endpoint:
            return "the endpoint is plain HTTP on a host that is not this machine, and no token is sent there";
        case oauth::AuthorizationProblem::no_client_registration:
            return "no way to register the client with the authorization server is configured or offered";
        case oauth::AuthorizationProblem::registration_rejected:
            return "the authorization server refused to register the client";
        case oauth::AuthorizationProblem::prompt_cancelled: return "the sign-in was given up";
        case oauth::AuthorizationProblem::prompt_timed_out: return "the person did not finish signing in in time";
        case oauth::AuthorizationProblem::callback_invalid:
            return "the URL the browser came back to is not the redirect URI or cannot be read";
        case oauth::AuthorizationProblem::state_mismatch:
            return "the answer of the sign-in does not belong to the request the client made";
        case oauth::AuthorizationProblem::issuer_mismatch:
            return "the answer of the sign-in does not name the authorization server that was asked";
        case oauth::AuthorizationProblem::authorization_denied:
            return "the authorization server refused the authorization";
        case oauth::AuthorizationProblem::token_request_failed:
            return "the request for a token was refused, or its answer cannot be used";
        case oauth::AuthorizationProblem::unsupported_token_type:
            return "the authorization server issued a token of a type the client does not send";
        case oauth::AuthorizationProblem::insufficient_scope:
            return "the server asks again for scope that was asked for already";
        case oauth::AuthorizationProblem::forbidden:
            return "the server refused the request for a reason no new token changes";
        case oauth::AuthorizationProblem::attempts_exceeded:
            return "the request was refused for want of authorization more often than the bounds allow";
        case oauth::AuthorizationProblem::document_too_large:
            return "a document or an answer of the sign-in is larger than its bound";
        case oauth::AuthorizationProblem::timed_out:
            return "an exchange of the sign-in, or the whole sign-in, took longer than its bound";
        case oauth::AuthorizationProblem::random_source_failed:
            return "the operating system's random source failed";
    }
    return "the sign-in failed";
}

ClientError failure_error(const oauth::FlowFailure& failure) {
    std::string message(sentence_for(failure.problem));
    if (failure.credentials_for_other_issuer) {
        message += "; the pre-registered credentials are for another authorization server";
    }
    ClientError error = error_of(ClientErrorKind::authorization_failed, std::move(message));
    error.authorization =
        AuthorizationFailure{public_problem(failure.problem), failure.issuer, failure.url, failure.error_code};
    return error;
}

AuthorizationFetchPurpose host_purpose(oauth::FetchPurpose purpose) noexcept {
    switch (purpose) {
        case oauth::FetchPurpose::protected_resource_metadata: return AuthorizationFetchPurpose::resource_metadata;
        case oauth::FetchPurpose::authorization_server_metadata: return AuthorizationFetchPurpose::server_metadata;
        case oauth::FetchPurpose::registration: return AuthorizationFetchPurpose::registration;
        case oauth::FetchPurpose::token: return AuthorizationFetchPurpose::token;
    }
    return AuthorizationFetchPurpose::token;
}

// A request of the flow as the transport takes it. Which requests follow redirects is the transport's rule by
// purpose and method (a metadata GET follows them within its own origin, at most HttpEndpoint::max_redirects times),
// the same the flow describes.
AuthorizationFetch to_host(const oauth::FlowFetch& fetch) {
    AuthorizationFetch request;
    request.purpose = host_purpose(fetch.purpose);
    request.method = fetch.method == oauth::method_get ? AuthorizationFetchMethod::get : AuthorizationFetchMethod::post;
    request.url = fetch.url;
    for (const oauth::HeaderLine& line : fetch.headers) request.headers.push_back(HttpHeader{line.name, line.value});
    request.body = fetch.body;
    request.max_body_bytes = fetch.body_bound;
    request.timeout = fetch.timeout;
    return request;
}

// The transport's answer as the flow reads it: the Content-Type field as sent.
oauth::FetchResult from_host(AuthorizationFetchResult result) {
    oauth::FetchResult out;
    if (result.error) {
        switch (*result.error) {
            case AuthorizationFetchError::too_large: out.outcome = oauth::FetchOutcome::too_large; break;
            case AuthorizationFetchError::timed_out: out.outcome = oauth::FetchOutcome::timed_out; break;
            case AuthorizationFetchError::refused_url:
            case AuthorizationFetchError::invalid_request:
            case AuthorizationFetchError::transport:
            case AuthorizationFetchError::closed: out.outcome = oauth::FetchOutcome::transport_error; break;
        }
        return out;
    }
    out.outcome = oauth::FetchOutcome::response;
    out.status = result.status;
    for (const HttpHeader& field : result.headers) {
        if (http::equal_ignoring_case(field.name, oauth::header_content_type)) {
            out.content_type = field.value;
            break;
        }
    }
    out.body = std::move(result.body);
    return out;
}

// The header lines that carry the protocol version on every request of the request's revision, for the metadata
// requests to the endpoint's origin.
std::vector<oauth::HeaderLine> version_lines(const profile::ProfilePtr& profile) {
    std::vector<oauth::HeaderLine> lines;
    if (!profile || !profile->streamable_http()) return lines;
    for (const profile::RequestHeader& header : profile->streamable_http()->request_headers) {
        if (header.value == profile::HeaderValue::protocol_version &&
            header.scope != profile::HeaderScopeKind::per_method) {
            lines.push_back(oauth::HeaderLine{header.name, profile->revision()});
        }
    }
    return lines;
}

// The operation a request belongs to, for the bound on sign-ins in a row: the transport names a request by its unit
// (one call, or the stream of the connection), and that is the finest key it gives.
std::string operation_of(const HttpRequestInfo& request) {
    return request.method + " " + std::to_string(request.unit);
}

bool positive(const OAuthBounds& bounds) {
    return bounds.max_metadata_bytes > 0 && bounds.max_token_response_bytes > 0 && bounds.max_token_bytes > 0 &&
           bounds.max_callback_bytes > 0 && bounds.max_challenge_params > 0 && bounds.max_authorization_servers > 0 &&
           bounds.max_scopes > 0 && bounds.max_scope_bytes > 0 && bounds.max_error_code_bytes > 0 &&
           bounds.max_document_depth > 0 && bounds.authorization_fetch_timeout.count() > 0 &&
           bounds.prompt_timeout.count() > 0 && bounds.authorization_timeout.count() > 0 &&
           bounds.max_token_lifetime.count() > 0;
}

constexpr std::string_view bearer_prefix = StaticBearer::scheme;

// One run of the application's prompt handler. Its answer comes back to the client's thread while the channel is
// open; closing it (the sign-in no longer waits) requests stop on the handler's context.
struct PromptRun {
    using Deliver = std::function<void(std::optional<AuthorizationPromptAnswer>)>;

    PromptRun(asio::any_io_executor executor_in, Deliver deliver_in)
        : executor(std::move(executor_in)), deliver(std::move(deliver_in)) {}

    // Any thread.
    void hand_back(std::optional<AuthorizationPromptAnswer> answer) {
        std::lock_guard lock(mutex);
        if (!open) return;
        open = false;
        asio::post(executor, [deliver_copy = deliver, answer = std::move(answer)]() mutable {
            deliver_copy(std::move(answer));
        });
    }

    // The client's thread.
    void close() {
        {
            std::lock_guard lock(mutex);
            open = false;
        }
        stop.request_stop();
    }

    std::stop_source stop;
    AuthorizationPromptContext context{stop.get_token()};
    std::mutex mutex;
    bool open = true;
    asio::any_io_executor executor;
    Deliver deliver;
};

// One sign-in under way.
struct SignIn {
    std::uint64_t id = 0;
    std::unique_ptr<oauth::OAuthFlow> flow;
    std::vector<ChallengeReply> replies;
    FlowClock::time_point started{};
    std::unique_ptr<asio::steady_timer> deadline;
    std::uint64_t waiting_fetch = 0;  // the flow's id of the request it waits for; 0: none
    std::shared_ptr<PromptRun> prompt;
    std::unique_ptr<asio::steady_timer> prompt_timer;
};

}  // namespace

struct OAuthAuthorizer::Impl : std::enable_shared_from_this<OAuthAuthorizer::Impl> {
    Impl(OAuthOptions options_in, std::optional<std::string> warning_in)
        : options(std::move(options_in)), store(options.store), lookup(store), warning(std::move(warning_in)) {
        config.registration.redirect_uri = options.redirect_uri;
        config.registration.client_name = options.client_name;
        config.registration.application_type = options.application_type;
        config.registration.pre_registered = options.pre_registered;
        config.registration.client_id_metadata_url = options.client_id_metadata_url;
        config.registration.dynamic_registration = options.dynamic_registration;
        config.request_offline_access = options.request_offline_access;
        config.accept_resource = options.accept_resource;
        static_cast<OAuthBounds&>(config.bounds) = options.bounds;
    }

    // ---- The seam

    void attach(AuthorizationHost& attached) {
        std::lock_guard lock(mutex);
        host = &attached;
        executor = attached.executor();
        config.endpoint_url = attached.endpoint_url();
        auto guard = http_client::UrlGuard::for_endpoint(config.endpoint_url);
        if (auto* made = std::get_if<http_client::UrlGuard>(&guard)) admission.emplace(std::move(*made));
        if (warning && !warned) {
            warned = true;
            host->diagnostic(*warning);
        }
    }

    void detach() {
        std::lock_guard lock(mutex);
        if (sign_in) {
            // The flow is given the end it would meet at its time bound, so that the changes to the credentials it
            // made so far (an erased refresh token, an erased registration) are kept; the held requests end with
            // the transport.
            const oauth::FlowStep step = expire_step();
            if (const auto* failed = std::get_if<oauth::FlowFailed>(&step)) apply(failed->updates);
            if (const auto* done = std::get_if<oauth::FlowDone>(&step)) apply(done->updates);
            SignIn ended = take_sign_in();
            for (ChallengeReply& reply : ended.replies) {
                reply.fail(error_of(ClientErrorKind::closed, "the transport closed during the sign-in"));
            }
        }
        host = nullptr;
        admission.reset();
    }

    Credential authorization() {
        std::lock_guard lock(mutex);
        Credential credential;
        credential.generation = generation;
        if (const auto tokens = current_tokens()) {
            credential.value.append(bearer_prefix);
            credential.value.push_back(' ');
            credential.value.append(tokens->access_token);
        }
        return credential;
    }

    void challenge(const HttpRequestInfo& request, int status, std::span<const std::string> challenges,
                   std::uint64_t refused_generation, ChallengeReply reply) {
        std::unique_lock lock(mutex);
        if (host == nullptr) {
            reply.fail(error_of(ClientErrorKind::closed, "the authorizer serves no transport"));
            return;
        }
        // Newer credentials exist already.
        if (refused_generation < generation) {
            reply.retry();
            return;
        }
        // One sign-in at a time: a refusal while one runs waits for its answer.
        if (sign_in) {
            sign_in->replies.push_back(std::move(reply));
            return;
        }
        try {
            start(request, status, challenges, std::move(reply));
        } catch (...) {
            // The store or the flow failed in a way the flow does not describe: the replies it held are dropped,
            // which fails the requests (no_answer).
            sign_in.reset();
        }
    }

    void accepted(const HttpRequestInfo& request) {
        std::lock_guard lock(mutex);
        memory.succeeded(operation_of(request));
    }

    void forget() {
        std::lock_guard lock(mutex);
        for (const CredentialKey& key : used_keys) store->erase_tokens(key);
        for (const std::string& issuer : registered_at) store->erase_client(issuer);
        used_keys.clear();
        registered_at.clear();
        memory.active.reset();
        ++generation;
    }

    // ---- A sign-in

    void start(const HttpRequestInfo& request, int status, std::span<const std::string> challenges,
               ChallengeReply reply) {
        oauth::Trigger trigger;
        trigger.status = status;
        trigger.challenges.assign(challenges.begin(), challenges.end());
        // The refused request carried the current credentials (an older generation was answered above).
        trigger.carried_token = current_tokens().has_value();
        trigger.operation = operation_of(request);
        trigger.resource_headers = version_lines(request.profile);
        const profile::ProfilePtr profiles[] = {request.profile};
        config.origin_metadata = oauth::origin_metadata_of(profiles);

        if (!admission) {
            oauth::FlowFailure failure;
            failure.problem = oauth::AuthorizationProblem::refused_url;
            failure.url = oauth::without_query(config.endpoint_url);
            reply.fail(failure_error(failure));
            return;
        }

        SignIn& running = sign_in.emplace();
        running.id = next_sign_in++;
        running.flow = std::make_unique<oauth::OAuthFlow>(
            config, *admission, lookup,
            [](std::size_t count) -> std::optional<std::string> { return random_bytes(count); }, memory);
        running.replies.push_back(std::move(reply));
        running.started = FlowClock::now();
        running.deadline = std::make_unique<asio::steady_timer>(executor, options.bounds.authorization_timeout);
        running.deadline->async_wait([weak = weak_from_this(), id = running.id](const std::error_code& error) {
            if (error) return;
            if (const auto self = weak.lock()) self->on_deadline(id);
        });
        advance(running.flow->start(trigger, running.started));
    }

    // Under the mutex, with a sign-in running: acts on the flow's step.
    void advance(oauth::FlowStep step) {
        report_diagnostics();
        if (auto* fetch = std::get_if<oauth::FlowFetch>(&step)) {
            send(*fetch);
            return;
        }
        if (auto* prompt = std::get_if<oauth::PromptRequest>(&step)) {
            ask(std::move(*prompt));
            return;
        }
        if (auto* done = std::get_if<oauth::FlowDone>(&step)) {
            apply(done->updates);
            used_keys.insert(done->key);
            // The new credentials are in place before any request is sent again with them.
            ++generation;
            finish(std::nullopt);
            return;
        }
        if (auto* failed = std::get_if<oauth::FlowFailed>(&step)) {
            apply(failed->updates);
            finish(failure_error(failed->failure));
        }
        // FlowIgnored: an input that did not belong to what the flow waits for; nothing changed.
    }

    void send(const oauth::FlowFetch& fetch) {
        SignIn& running = *sign_in;
        running.waiting_fetch = fetch.id;
        // The awaitable starts nothing until it is spawned, which happens outside the mutex.
        asio::awaitable<AuthorizationFetchResult> exchange = host->fetch(to_host(fetch));
        asio::post(executor, [weak = weak_from_this(), id = running.id, fetch_id = fetch.id, ex = executor,
                              exchange = std::move(exchange)]() mutable {
            asio::co_spawn(ex, std::move(exchange),
                           [weak, id, fetch_id](std::exception_ptr failure, AuthorizationFetchResult result) {
                               const auto self = weak.lock();
                               if (!self) return;
                               if (failure) {
                                   result = AuthorizationFetchResult{};
                                   result.error = AuthorizationFetchError::transport;
                               }
                               self->on_fetched(id, fetch_id, std::move(result));
                           });
        });
    }

    void on_fetched(std::uint64_t id, std::uint64_t fetch_id, AuthorizationFetchResult result) {
        std::lock_guard lock(mutex);
        if (!sign_in || sign_in->id != id || sign_in->waiting_fetch != fetch_id) return;
        sign_in->waiting_fetch = 0;
        guarded([&] { advance(sign_in->flow->fetched(fetch_id, from_host(std::move(result)), FlowClock::now())); });
    }

    void ask(oauth::PromptRequest prompt) {
        SignIn& running = *sign_in;
        auto run = std::make_shared<PromptRun>(
            executor, [weak = weak_from_this(), id = running.id](std::optional<AuthorizationPromptAnswer> answer) {
                if (const auto self = weak.lock()) self->on_prompted(id, std::move(answer));
            });
        running.prompt = run;
        running.prompt_timer = std::make_unique<asio::steady_timer>(executor, prompt.timeout);
        running.prompt_timer->async_wait(
            [weak = weak_from_this(), id = running.id, which = std::weak_ptr<PromptRun>(run)](
                const std::error_code& error) {
                if (error) return;
                if (const auto self = weak.lock()) self->on_prompt_timeout(id, which.lock());
            });

        AuthorizationPromptInfo info;
        info.authorization_url = std::move(prompt.authorization_url);
        info.redirect_uri = std::move(prompt.redirect_uri);
        info.issuer = std::move(prompt.issuer);
        info.scopes = std::move(prompt.scopes);

        if (const auto& body = options.prompt.coroutine()) {
            asio::post(executor, [body, info = std::move(info), run, ex = executor]() mutable {
                asio::co_spawn(ex, run_coroutine(body, std::move(info), run),
                               [run](std::exception_ptr, std::optional<AuthorizationPromptAnswer> answer) {
                                   run->hand_back(std::move(answer));
                               });
            });
            return;
        }
        try {
            std::thread([body = options.prompt.plain(), info = std::move(info), run]() {
                std::optional<AuthorizationPromptAnswer> answer;
                try {
                    answer = body->run(info, run->context);
                } catch (...) {
                    // A handler that throws gave up; its text goes nowhere.
                }
                run->hand_back(std::move(answer));
            }).detach();
        } catch (...) {
            // No thread could be started: as a handler that gave up, once this call has returned.
            run->hand_back(std::nullopt);
        }
    }

    static asio::awaitable<std::optional<AuthorizationPromptAnswer>> run_coroutine(
        std::shared_ptr<detail::CoroutinePromptBody> body, AuthorizationPromptInfo info, std::shared_ptr<PromptRun> run) {
        std::optional<AuthorizationPromptAnswer> answer;
        try {
            answer = co_await body->run(std::move(info), run->context);
        } catch (...) {
            // As a plain handler that throws.
        }
        co_return answer;
    }

    void on_prompted(std::uint64_t id, std::optional<AuthorizationPromptAnswer> answer) {
        std::lock_guard lock(mutex);
        if (!sign_in || sign_in->id != id || !sign_in->prompt) return;
        end_prompt(*sign_in);
        oauth::PromptAnswer given;
        if (!answer) {
            if (host != nullptr) host->diagnostic("the prompt handler threw; the sign-in counts as given up");
            given.kind = oauth::PromptAnswer::Kind::cancelled;
        } else if (!answer->callback_url) {
            given.kind = oauth::PromptAnswer::Kind::cancelled;
        } else {
            given.kind = oauth::PromptAnswer::Kind::callback;
            given.callback_url = std::move(*answer->callback_url);
        }
        guarded([&] { advance(sign_in->flow->prompted(given, FlowClock::now())); });
    }

    void on_prompt_timeout(std::uint64_t id, const std::shared_ptr<PromptRun>& which) {
        std::lock_guard lock(mutex);
        if (!sign_in || sign_in->id != id || !which || sign_in->prompt != which) return;
        end_prompt(*sign_in);
        oauth::PromptAnswer given;
        given.kind = oauth::PromptAnswer::Kind::timed_out;
        guarded([&] { advance(sign_in->flow->prompted(given, FlowClock::now())); });
    }

    void on_deadline(std::uint64_t id) {
        std::lock_guard lock(mutex);
        if (!sign_in || sign_in->id != id) return;
        guarded([&] {
            advance(expire_step());
            // The flow ends at its time bound whatever it waited for; should it not have, the sign-in ends here.
            if (sign_in && sign_in->id == id) {
                oauth::FlowFailure failure;
                failure.problem = oauth::AuthorizationProblem::timed_out;
                failure.url = oauth::without_query(config.endpoint_url);
                finish(failure_error(failure));
            }
        });
    }

    // The flow's end at its time bound: the input it waits for, at a time past the bound.
    oauth::FlowStep expire_step() {
        SignIn& running = *sign_in;
        const FlowClock::time_point late =
            running.started + options.bounds.authorization_timeout + std::chrono::milliseconds(1);
        if (running.prompt) {
            end_prompt(running);
            oauth::PromptAnswer given;
            given.kind = oauth::PromptAnswer::Kind::timed_out;
            return running.flow->prompted(given, late);
        }
        if (running.waiting_fetch != 0) {
            const std::uint64_t fetch_id = std::exchange(running.waiting_fetch, 0);
            oauth::FetchResult result;
            result.outcome = oauth::FetchOutcome::timed_out;
            return running.flow->fetched(fetch_id, result, late);
        }
        return oauth::FlowIgnored{};
    }

    static void end_prompt(SignIn& running) {
        if (running.prompt_timer) running.prompt_timer->cancel();
        running.prompt_timer.reset();
        if (running.prompt) running.prompt->close();
        running.prompt.reset();
    }

    SignIn take_sign_in() {
        SignIn ended = std::move(*sign_in);
        sign_in.reset();
        if (ended.deadline) ended.deadline->cancel();
        end_prompt(ended);
        return ended;
    }

    // Ends the sign-in and answers every request it held: retry, or fail with `error`.
    void finish(std::optional<ClientError> error) {
        SignIn ended = take_sign_in();
        for (ChallengeReply& reply : ended.replies) {
            if (error) {
                reply.fail(*error);
            } else {
                reply.retry();
            }
        }
    }

    // A completion that fails in a way the flow does not describe (the store threw) ends the sign-in; dropping its
    // replies fails the requests it held (no_answer).
    template <class F>
    void guarded(F&& action) {
        try {
            action();
        } catch (...) {
            if (sign_in) static_cast<void>(take_sign_in());
        }
    }

    // ---- Credentials

    std::optional<StoredTokens> current_tokens() const {
        if (!memory.active) return std::nullopt;
        return store->tokens(*memory.active);
    }

    // The changes a flow asks for: erasures, then puts.
    void apply(const oauth::StoreUpdate& update) {
        for (const CredentialKey& key : update.erase_tokens) store->erase_tokens(key);
        for (const std::string& issuer : update.erase_clients) {
            store->erase_client(issuer);
            registered_at.erase(issuer);
        }
        if (update.put_client) {
            store->put_client(update.put_client->first, update.put_client->second);
            registered_at.insert(update.put_client->first);
        }
        if (update.put_tokens) {
            store->put_tokens(update.put_tokens->first, update.put_tokens->second);
            used_keys.insert(update.put_tokens->first);
        }
    }

    void report_diagnostics() {
        if (!sign_in) return;
        for (oauth::FlowDiagnostic& noticed : sign_in->flow->take_diagnostics()) {
            if (host != nullptr) host->diagnostic(std::move(noticed.text));
        }
    }

    // Set at creation.
    const OAuthOptions options;
    const std::shared_ptr<TokenStore> store;
    const StoreLookup lookup;
    const std::optional<std::string> warning;

    std::mutex mutex;  // guards everything below

    AuthorizationHost* host = nullptr;  // while attached
    asio::any_io_executor executor;
    std::optional<GuardAdmission> admission;
    oauth::FlowConfig config;
    oauth::FlowMemory memory;
    std::set<CredentialKey> used_keys;     // whose tokens this authorizer obtained or used
    std::set<std::string> registered_at;   // where it registered dynamically
    std::uint64_t generation = 0;
    std::optional<SignIn> sign_in;
    std::uint64_t next_sign_in = 1;
    bool warned = false;
};

Result<std::shared_ptr<OAuthAuthorizer>> OAuthAuthorizer::create(OAuthOptions options) {
    if (!options.prompt) {
        return error_of(ClientErrorKind::invalid_options, "OAuthOptions::prompt has no handler");
    }
    if (auto problem = oauth::check_redirect_uri(options.redirect_uri)) {
        return error_of(ClientErrorKind::invalid_options, "OAuthOptions::redirect_uri: " + *problem);
    }
    std::optional<std::string> warning;
    if (options.client_id_metadata_url) {
        bool has_query = false;
        if (auto problem = oauth::check_client_id_metadata_url(*options.client_id_metadata_url, has_query)) {
            return error_of(ClientErrorKind::invalid_options, "OAuthOptions::client_id_metadata_url: " + *problem);
        }
        if (has_query) {
            warning = "the client ID metadata document URL has a query, which authorization servers are asked not "
                      "to rely on";
        }
    }
    if (!positive(options.bounds)) {
        return error_of(ClientErrorKind::invalid_options,
                        "every bound of OAuthOptions::bounds but max_step_ups_per_operation must be positive");
    }
    if (!options.store) options.store = std::make_shared<MemoryTokenStore>();
    auto impl = std::make_shared<Impl>(std::move(options), std::move(warning));
    return std::shared_ptr<OAuthAuthorizer>(new OAuthAuthorizer(std::move(impl)));
}

OAuthAuthorizer::OAuthAuthorizer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

OAuthAuthorizer::~OAuthAuthorizer() = default;

void OAuthAuthorizer::forget() { impl_->forget(); }

void OAuthAuthorizer::attach(AuthorizationHost& host) { impl_->attach(host); }

void OAuthAuthorizer::detach() { impl_->detach(); }

Credential OAuthAuthorizer::authorization(const HttpRequestInfo&) { return impl_->authorization(); }

void OAuthAuthorizer::challenge(const HttpRequestInfo& request, int status, std::span<const std::string> challenges,
                                std::uint64_t generation, ChallengeReply reply) {
    impl_->challenge(request, status, challenges, generation, std::move(reply));
}

void OAuthAuthorizer::accepted(const HttpRequestInfo& request) { impl_->accepted(request); }

}  // namespace neomcp
