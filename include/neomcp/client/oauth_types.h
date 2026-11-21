// The values of the client's OAuth sign-in that an application sees or keeps: how the client is known to an
// authorization server, the credentials a sign-in leaves and the key they are kept under, and the bounds of a sign-in
// on what a server or an authorization server controls. The authorizer itself is in neomcp/client/oauth.h; this header
// holds only values, so that code which stores credentials needs nothing else of the library.
#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "neomcp/http/challenge.h"

namespace neomcp {

// How the client authenticates at an authorization server's token endpoint.
enum class TokenEndpointAuth {
    none,                 // a public client: only its client_id
    client_secret_basic,  // the id and the secret in the Authorization field (Basic)
    client_secret_post    // the id and the secret in the request's body
};

// What a dynamic registration says the client is.
enum class ApplicationType { native, web };

// Credentials the application registered beforehand at one authorization server.
struct PreRegisteredClient {
    std::string issuer;  // the authorization server's issuer identifier, compared exactly
    std::string client_id;
    std::optional<std::string> client_secret;
    std::optional<TokenEndpointAuth> method;  // when set, used as it is; otherwise chosen from what the server lists
};

// A client registered dynamically at one authorization server.
struct StoredClient {
    std::string client_id;
    std::optional<std::string> client_secret;
    std::optional<TokenEndpointAuth> method;  // what the registration answered
    friend bool operator==(const StoredClient&, const StoredClient&) = default;
};

// Credentials are kept per resource and per authorization server; those of one issuer are never read for another.
struct CredentialKey {
    std::string resource;  // the resource the server's protected resource metadata names, as received
    std::string issuer;    // the authorization server's validated issuer identifier, as received
    friend bool operator==(const CredentialKey&, const CredentialKey&) = default;
    friend auto operator<=>(const CredentialKey&, const CredentialKey&) = default;
};

// The tokens of one sign-in.
struct StoredTokens {
    std::string access_token;
    std::optional<std::string> refresh_token;
    std::optional<std::chrono::system_clock::time_point> expires_at;  // a hint; the server's refusal decides
    std::vector<std::string> granted_scopes;    // as granted, or as requested when the answer named none
    std::vector<std::string> requested_scopes;  // what the authorization that made them asked for
    friend bool operator==(const StoredTokens&, const StoredTokens&) = default;
};

// The resource the client expected (the endpoint, or the origin a document was found at) and the one a protected
// resource metadata document names, when they differ.
struct ResourceMismatch {
    std::string expected;
    std::string received;
};

// The defaults of OAuthBounds, as constants.
struct OAuthDefaults {
    // A protected resource metadata or authorization server metadata document. Such documents are a few kilobytes.
    static constexpr std::size_t max_metadata_bytes = std::size_t{64} * 1024;
    // A registration or token response; room for large self-contained tokens.
    static constexpr std::size_t max_token_response_bytes = std::size_t{64} * 1024;
    // An access token or a refresh token. The access token rides in a request header field.
    static constexpr std::size_t max_token_bytes = std::size_t{16} * 1024;
    // The callback URL the application hands back after the person's browser returned to it.
    static constexpr std::size_t max_callback_bytes = std::size_t{16} * 1024;
    // Parameters of one WWW-Authenticate challenge: the challenge reader's own default.
    static constexpr std::size_t max_challenge_params = neomcp::http::ChallengeLimits::default_max_params;
    // Entries of `authorization_servers` that are read; only one is used.
    static constexpr std::size_t max_authorization_servers = 16;
    // Scopes in one list, and the bytes of all of them together.
    static constexpr std::size_t max_scopes = 64;
    static constexpr std::size_t max_scope_bytes = std::size_t{4} * 1024;
    // An error code an authorization server or the callback sends and the application is told of. Codes are short
    // words such as `invalid_grant`.
    static constexpr std::size_t max_error_code_bytes = 128;
    // Nesting of a JSON document an authorization server or a resource sends; these documents are flat.
    static constexpr std::size_t max_document_depth = 64;
    // One request to an authorization server or for a metadata document.
    static constexpr std::chrono::seconds authorization_fetch_timeout{30};
    // The prompt: a person in a browser.
    static constexpr std::chrono::seconds prompt_timeout{600};
    // A whole sign-in, the prompt included.
    static constexpr std::chrono::seconds authorization_timeout{900};
    // Sign-ins one request may start in a row before the server accepts it, beyond the first.
    static constexpr std::size_t max_step_ups_per_operation = 2;
    // The longest lifetime an `expires_in` is taken to give.
    static constexpr std::chrono::hours max_token_lifetime{24 * 365};
};

// The bounds of a sign-in on what a server or an authorization server controls. Every value a peer can make large or
// slow has one. How often one request may be refused and held, and how many redirects a metadata request follows, are
// the transport's (HttpEndpoint::max_authorizations_per_request, HttpEndpoint::max_redirects).
struct OAuthBounds {
    std::size_t max_metadata_bytes = OAuthDefaults::max_metadata_bytes;
    std::size_t max_token_response_bytes = OAuthDefaults::max_token_response_bytes;
    std::size_t max_token_bytes = OAuthDefaults::max_token_bytes;
    std::size_t max_callback_bytes = OAuthDefaults::max_callback_bytes;
    std::size_t max_challenge_params = OAuthDefaults::max_challenge_params;
    std::size_t max_authorization_servers = OAuthDefaults::max_authorization_servers;
    std::size_t max_scopes = OAuthDefaults::max_scopes;
    std::size_t max_scope_bytes = OAuthDefaults::max_scope_bytes;
    // An error code longer than this is not passed on; the sign-in still ends with the problem it ended with.
    std::size_t max_error_code_bytes = OAuthDefaults::max_error_code_bytes;
    std::size_t max_document_depth = OAuthDefaults::max_document_depth;
    std::chrono::milliseconds authorization_fetch_timeout = OAuthDefaults::authorization_fetch_timeout;
    // How long the person may take in the browser; the sign-in fails prompt_timed_out after it.
    std::chrono::milliseconds prompt_timeout = OAuthDefaults::prompt_timeout;
    // How long a whole sign-in may take; it fails timed_out after it, and every request it holds ends with that.
    std::chrono::milliseconds authorization_timeout = OAuthDefaults::authorization_timeout;
    std::size_t max_step_ups_per_operation = OAuthDefaults::max_step_ups_per_operation;
    std::chrono::seconds max_token_lifetime = OAuthDefaults::max_token_lifetime;
};

}  // namespace neomcp
