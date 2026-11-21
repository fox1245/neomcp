// Signing in to an HTTP server with OAuth (the target neomcp::client_http): an HttpAuthorizer that obtains the
// tokens a server asks for, as the authorization pages of the protocol describe it (OAuth 2.1 with PKCE).
//
// When a server refuses a request with 401, OAuthAuthorizer finds the server's authorization server (the protected
// resource metadata, then the authorization server's metadata), registers the client there when it must, and hands
// the application the URL a person opens in a browser to sign in. The application's prompt handler opens it however it
// can, waits until the browser comes back to the redirect URI, and returns the URL it came back to. The authorizer
// checks that answer, exchanges the code for tokens, and every request that waited goes out again with them. Later
// refusals are answered with a refresh when a refresh token exists, and a 403 that asks for more scope with a new
// sign-in for the scopes asked for so far together with the new ones. The library never fetches the authorization
// URL and never opens a browser.
//
// What is checked: the resource the metadata names must be the endpoint (or the application's accept_resource hook
// accepts it); the authorization server's metadata must name the issuer it was found by, and offer the S256 code
// challenge; every URL a server supplies passes the transport's rule for documents of authorization; the callback
// must come back to the redirect URI with the state this sign-in sent and, where the server sends one, its issuer.
// Tokens are kept per resource and authorization server (TokenStore): those of one authorization server are never
// sent to, refreshed at, or used to register with another, and a resource whose authorization server changed signs
// in anew.
//
// Registration, in this order: credentials the application registered beforehand for exactly the server's issuer
// (OAuthOptions::pre_registered); the client ID metadata document URL the application hosts, when the server supports
// them (client_id_metadata_url); a registration this authorizer made at that server before; a new dynamic
// registration, when it is on (dynamic_registration, on by default) and the server offers it.
//
// Bounds. Every document and answer a server sends is bounded (OAuthBounds). One sign-in runs at a time: requests
// refused while it runs wait for it, and it answers them all. A whole sign-in, the person's visit included, ends after
// OAuthBounds::authorization_timeout and the person's visit alone after prompt_timeout, so that no request waits for
// ever on a browser nobody looks at; every request it held then ends with authorization_failed (timed_out or
// prompt_timed_out). One request (one call, or the stream of the connection, as the transport tells them apart) may
// start at most max_step_ups_per_operation + 1 sign-ins in a row before the server accepts it.
//
// Errors. A request that cannot be authorized ends with ClientErrorKind::authorization_failed, and
// ClientError::authorization says what happened: the problem, the authorization server and the URL involved without
// its query, and the error code an authorization server answered with. No error and no diagnostic ever carries a
// token, a code, a verifier, a state, a client secret, a callback's query, or a description or URI an authorization
// server sent.
//
// Threads. The authorizer runs on the client's thread; its exchanges run on the transport's own thread. A plain prompt
// handler runs on a thread of its own, a coroutine prompt handler on the client's thread, where it must not block.
// forget() may be called from any thread.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/awaitable.hpp>

#include "neomcp/client/http.h"
#include "neomcp/client/oauth_types.h"
#include "neomcp/client/result.h"
#include "neomcp/detail/callable.h"

namespace neomcp {

// ---- The prompt

// What the application shows the person, by value.
struct AuthorizationPromptInfo {
    std::string authorization_url;  // open this in the person's browser
    std::string redirect_uri;       // where the browser comes back once the person signed in (or refused)
    std::string issuer;             // the authorization server, for display
    std::vector<std::string> scopes;
};

// The URL the browser came back to, whole; empty when the person gave up.
struct AuthorizationPromptAnswer {
    std::optional<std::string> callback_url;
};

// What a prompt handler is told besides the prompt. Stop is requested when the sign-in no longer waits for the
// answer: the prompt's or the sign-in's time ran out, or the client closed. Nothing returned afterwards is used.
class AuthorizationPromptContext {
public:
    explicit AuthorizationPromptContext(std::stop_token stop) : stop_(std::move(stop)) {}
    AuthorizationPromptContext(const AuthorizationPromptContext&) = delete;
    AuthorizationPromptContext& operator=(const AuthorizationPromptContext&) = delete;

    std::stop_token stop_token() const noexcept { return stop_; }
    bool cancelled() const noexcept { return stop_.stop_requested(); }

private:
    std::stop_token stop_;
};

namespace detail {

// Runs on a thread of its own; throws what the handler throws.
class PlainPromptBody {
public:
    virtual ~PlainPromptBody() = default;
    virtual AuthorizationPromptAnswer run(const AuthorizationPromptInfo& info, AuthorizationPromptContext& context) = 0;
};

// Runs on the client's thread; the context outlives the coroutine.
class CoroutinePromptBody {
public:
    virtual ~CoroutinePromptBody() = default;
    virtual asio::awaitable<AuthorizationPromptAnswer> run(AuthorizationPromptInfo info,
                                                           AuthorizationPromptContext& context) = 0;
};

template <class T>
inline constexpr bool prompt_handler_unmatched = false;

template <class F>
struct PromptShape {
    using signature = CallSignature<F>;
    using returned = std::remove_cv_t<typename signature::result>;
    static constexpr bool coroutine = AwaitableTraits<returned>::value;
    using value = std::remove_cv_t<typename AwaitableTraits<returned>::value_type>;
    static constexpr bool takes_context = signature::arity == 2;
    using first = param_t<0, typename signature::params>;
};

template <class F>
constexpr bool check_prompt_handler() {
    using Sig = CallSignature<F>;
    if constexpr (!Sig::known) {
        static_assert(prompt_handler_unmatched<F>,
                      "neomcp: a prompt handler must be a function, a function pointer, or an object with exactly one "
                      "operator() that is not a template (write out the parameter types of a lambda instead of auto)");
        return false;
    } else {
        using P0 = param_t<0, typename Sig::params>;
        using P1 = param_t<1, typename Sig::params>;
        if constexpr (Sig::arity == 0 || Sig::arity > 2 ||
                      !(std::is_same_v<P0, AuthorizationPromptInfo> ||
                        std::is_same_v<P0, const AuthorizationPromptInfo&>) ||
                      (Sig::arity == 2 && !std::is_same_v<P1, AuthorizationPromptContext&>)) {
            static_assert(prompt_handler_unmatched<F>,
                          "neomcp: a prompt handler takes neomcp::AuthorizationPromptInfo (as `const T&` or `T`) and "
                          "optionally `neomcp::AuthorizationPromptContext&` after it");
            return false;
        } else if constexpr (!AwaitableTraits<typename PromptShape<F>::returned>::default_executor) {
            static_assert(prompt_handler_unmatched<F>,
                          "neomcp: a coroutine prompt handler returns asio::awaitable<T> with the default executor");
            return false;
        } else if constexpr (!std::is_same_v<typename PromptShape<F>::value, AuthorizationPromptAnswer>) {
            static_assert(prompt_handler_unmatched<F>,
                          "neomcp: a prompt handler returns neomcp::AuthorizationPromptAnswer, or an asio::awaitable "
                          "of it");
            return false;
        } else {
            return true;
        }
    }
}

template <class F, class Passed>
typename PromptShape<F>::returned invoke_prompt_handler(F& handler, Passed&& info,
                                                        AuthorizationPromptContext& context) {
    if constexpr (PromptShape<F>::takes_context) {
        return handler(std::forward<Passed>(info), context);
    } else {
        static_cast<void>(context);
        return handler(std::forward<Passed>(info));
    }
}

template <class F>
class PlainPromptBodyOf final : public PlainPromptBody {
public:
    template <class H>
    explicit PlainPromptBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    AuthorizationPromptAnswer run(const AuthorizationPromptInfo& info, AuthorizationPromptContext& context) override {
        if constexpr (std::is_reference_v<typename PromptShape<F>::first>) {
            return invoke_prompt_handler(handler_, info, context);
        } else {
            return invoke_prompt_handler(handler_, AuthorizationPromptInfo(info), context);
        }
    }

private:
    F handler_;
};

template <class F>
class CoroutinePromptBodyOf final : public CoroutinePromptBody {
public:
    template <class H>
    explicit CoroutinePromptBodyOf(H&& handler) : handler_(std::forward<H>(handler)) {}

    asio::awaitable<AuthorizationPromptAnswer> run(AuthorizationPromptInfo info,
                                                   AuthorizationPromptContext& context) override {
        // The prompt lives in this frame, which outlives the handler's coroutine: a reference parameter is safe.
        if constexpr (std::is_reference_v<typename PromptShape<F>::first>) {
            co_return co_await invoke_prompt_handler(handler_, info, context);
        } else {
            co_return co_await invoke_prompt_handler(handler_, std::move(info), context);
        }
    }

private:
    F handler_;
};

}  // namespace detail

// The application's prompt handler, plain or coroutine:
//
//   AuthorizationPromptAnswer(const AuthorizationPromptInfo&, AuthorizationPromptContext&)
//   asio::awaitable<AuthorizationPromptAnswer>(AuthorizationPromptInfo, AuthorizationPromptContext&)
//
// The prompt may be taken as `const T&` or as `T` in either form, and the context may be left out. A plain handler
// runs on a thread of its own and may block until the person is back; a coroutine runs on the client's thread and must
// not block it. Neither closing the client nor the end of a sign-in waits for a plain handler: stop is requested on
// its context, and it should return soon after. A handler that throws counts as a person who gave up (prompt_cancelled), and the exception's text goes
// nowhere. Cheap to copy: copies share the handler.
class AuthorizationPrompt {
public:
    template <class H>
    AuthorizationPrompt& handler(H&& callable) {
        using F = std::decay_t<H>;
        plain_.reset();
        coroutine_.reset();
        if constexpr (detail::check_prompt_handler<F>()) {
            if constexpr (detail::PromptShape<F>::coroutine) {
                coroutine_ = std::make_shared<detail::CoroutinePromptBodyOf<F>>(std::forward<H>(callable));
            } else {
                plain_ = std::make_shared<detail::PlainPromptBodyOf<F>>(std::forward<H>(callable));
            }
        }
        return *this;
    }

    // Whether a handler is set.
    explicit operator bool() const noexcept { return plain_ != nullptr || coroutine_ != nullptr; }

    const std::shared_ptr<detail::PlainPromptBody>& plain() const noexcept { return plain_; }
    const std::shared_ptr<detail::CoroutinePromptBody>& coroutine() const noexcept { return coroutine_; }

private:
    std::shared_ptr<detail::PlainPromptBody> plain_;
    std::shared_ptr<detail::CoroutinePromptBody> coroutine_;
};

// ---- Where credentials are kept

// The tokens of each resource and authorization server, and the clients registered dynamically at each authorization
// server. Its methods are called on the client's thread and must return promptly and not throw; a store shared by
// several clients is called from their threads at once and must be safe for that. A store must not call back into the
// authorizer. Everything it holds is secret: a store that keeps it beyond the process keeps it where only the person
// can read it.
class TokenStore {
public:
    virtual ~TokenStore() = default;
    virtual std::optional<StoredTokens> tokens(const CredentialKey& key) = 0;
    virtual void put_tokens(const CredentialKey& key, StoredTokens tokens) = 0;
    virtual void erase_tokens(const CredentialKey& key) = 0;
    virtual std::optional<StoredClient> client(const std::string& issuer) = 0;
    virtual void put_client(const std::string& issuer, StoredClient client) = 0;
    virtual void erase_client(const std::string& issuer) = 0;
};

// A store in memory, for the life of the object; safe to share between threads. The default of OAuthOptions::store.
class MemoryTokenStore final : public TokenStore {
public:
    std::optional<StoredTokens> tokens(const CredentialKey& key) override;
    void put_tokens(const CredentialKey& key, StoredTokens tokens) override;
    void erase_tokens(const CredentialKey& key) override;
    std::optional<StoredClient> client(const std::string& issuer) override;
    void put_client(const std::string& issuer, StoredClient client) override;
    void erase_client(const std::string& issuer) override;

private:
    std::mutex mutex_;
    std::map<CredentialKey, StoredTokens> tokens_;
    std::map<std::string, StoredClient> clients_;
};

// ---- The authorizer

struct OAuthOptions {
    // Where the browser comes back to: https, or http to a loopback host (127.0.0.1, ::1 or localhost); no fragment.
    std::string redirect_uri;
    // The application's prompt handler; required.
    AuthorizationPrompt prompt;
    // A client ID metadata document the application hosts at this https URL, for authorization servers that support
    // them. Its path must not be "/" nor hold "." or ".." segments; a query is accepted and reported.
    std::optional<std::string> client_id_metadata_url;
    // Credentials registered beforehand, each for one issuer.
    std::vector<PreRegisteredClient> pre_registered;
    // Register dynamically where nothing else applies and the server offers it.
    bool dynamic_registration = true;
    // The name a dynamic registration gives the client, which the authorization server may show.
    std::string client_name;
    // What a dynamic registration says the client is; not set: native for a loopback redirect URI, web otherwise.
    std::optional<ApplicationType> application_type;
    // Ask for offline_access (a refresh token) where the authorization server lists that scope.
    bool request_offline_access = true;
    // Where tokens and registrations are kept; null: a MemoryTokenStore of this authorizer's own.
    std::shared_ptr<TokenStore> store;
    // Called when the resource a protected resource metadata document names is not the one expected; true uses the
    // document anyway. Not set: the sign-in fails with resource_mismatch. Called on the client's thread while the
    // authorizer works; it must return promptly and must not call into the authorizer.
    std::function<bool(const ResourceMismatch&)> accept_resource;
    OAuthBounds bounds;
};

class OAuthAuthorizer final : public HttpAuthorizer {
public:
    // An authorizer for these options, or invalid_options when one cannot work (no prompt handler, a redirect URI or
    // a client ID metadata document URL that is not admitted, a bound that is zero).
    static Result<std::shared_ptr<OAuthAuthorizer>> create(OAuthOptions options);

    ~OAuthAuthorizer() override;

    // Drops from the store the tokens this authorizer obtained or used, and the registrations it made, and sends no
    // token until a new sign-in; no I/O. A sign-in that runs goes on.
    void forget();

    void attach(AuthorizationHost& host) override;
    void detach() override;
    Credential authorization(const HttpRequestInfo& request) override;
    void challenge(const HttpRequestInfo& request, int status, std::span<const std::string> challenges,
                   std::uint64_t generation, ChallengeReply reply) override;
    void accepted(const HttpRequestInfo& request) override;

    struct Impl;

private:
    explicit OAuthAuthorizer(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace neomcp
