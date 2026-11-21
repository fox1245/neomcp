// The bounds of the OAuth flow on what a server or an authorization server controls, each with its default.
//
// Every value a peer can make large or slow has a bound here. The defaults are in OAuthDefaults; OAuthBounds holds
// the values a flow uses and starts from the defaults.
#pragma once

#include <chrono>
#include <cstddef>

#include "neomcp/http/challenge.h"

namespace neomcp::detail::oauth {

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
    // Nesting of a JSON document an authorization server or a resource sends; these documents are flat.
    static constexpr std::size_t max_document_depth = 64;
    // One request to an authorization server or for a metadata document.
    static constexpr std::chrono::seconds authorization_fetch_timeout{30};
    // Redirects of a metadata request (registration and token requests follow none): the same number as the HTTP
    // transport's default for its own requests (HttpClientDefaults::max_redirects), which the driver passes when the
    // application set another.
    static constexpr std::size_t max_redirects = 5;
    // The prompt: a person in a browser.
    static constexpr std::chrono::seconds prompt_timeout{600};
    // A whole flow, the prompt included.
    static constexpr std::chrono::seconds authorization_timeout{900};
    // How many times one request may be refused and held for authorization.
    static constexpr std::size_t max_authorizations_per_request = 3;
    // Step-ups of one operation in a row without a success; with the first authorization, one more flow than this.
    static constexpr std::size_t max_step_ups_per_operation = 2;
    // The longest lifetime an `expires_in` is taken to give.
    static constexpr std::chrono::hours max_token_lifetime{24 * 365};
    // Refresh attempts in one flow; not an option.
    static constexpr std::size_t refresh_attempts_per_flow = 1;
};

struct OAuthBounds {
    std::size_t max_metadata_bytes = OAuthDefaults::max_metadata_bytes;
    std::size_t max_token_response_bytes = OAuthDefaults::max_token_response_bytes;
    std::size_t max_token_bytes = OAuthDefaults::max_token_bytes;
    std::size_t max_callback_bytes = OAuthDefaults::max_callback_bytes;
    std::size_t max_challenge_params = OAuthDefaults::max_challenge_params;
    std::size_t max_authorization_servers = OAuthDefaults::max_authorization_servers;
    std::size_t max_scopes = OAuthDefaults::max_scopes;
    std::size_t max_scope_bytes = OAuthDefaults::max_scope_bytes;
    std::size_t max_document_depth = OAuthDefaults::max_document_depth;
    std::chrono::milliseconds authorization_fetch_timeout = OAuthDefaults::authorization_fetch_timeout;
    std::size_t max_redirects = OAuthDefaults::max_redirects;
    std::chrono::milliseconds prompt_timeout = OAuthDefaults::prompt_timeout;
    std::chrono::milliseconds authorization_timeout = OAuthDefaults::authorization_timeout;
    std::size_t max_authorizations_per_request = OAuthDefaults::max_authorizations_per_request;
    std::size_t max_step_ups_per_operation = OAuthDefaults::max_step_ups_per_operation;
    std::chrono::seconds max_token_lifetime = OAuthDefaults::max_token_lifetime;
};

}  // namespace neomcp::detail::oauth
