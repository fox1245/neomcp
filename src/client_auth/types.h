// The values the OAuth flow takes in and gives out: the requests it wants sent (described, never sent by it), their
// answers, the credentials it reads and the changes to them it asks for, and the problems a flow ends with.
//
// The flow does no I/O. The code that drives it sends each request it describes, hands the person the authorization
// URL, applies the changes to the credentials it reports, and enforces the time bounds it names.
#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "neomcp/client/oauth_types.h"

namespace neomcp::detail::oauth {

using FlowClock = std::chrono::system_clock;

// ---- Problems

enum class AuthorizationProblem {
    discovery_failed,          // no protected resource metadata or authorization server metadata could be read
    resource_mismatch,         // the metadata describes another resource than the endpoint
    metadata_issuer_mismatch,  // the authorization server metadata names another issuer than the one it was found by
    pkce_unsupported,          // the authorization server does not list the S256 code challenge method
    refused_url,               // a URL a server supplied is not admitted
    insecure_endpoint,         // the endpoint is plain HTTP on a host that is not loopback
    no_client_registration,    // no way to register a client at this authorization server
    registration_rejected,     // the authorization server refused the dynamic registration
    prompt_cancelled,          // the person or the application gave up
    prompt_timed_out,          // the person did not come back in time
    callback_invalid,          // the callback URL is not the redirect URI, too long, or not a valid response
    state_mismatch,            // the callback's state is not the one this flow sent
    issuer_mismatch,           // the callback's iss is not the recorded issuer, or is missing where it must be
    authorization_denied,      // the authorization server answered with an error, or without a code
    token_request_failed,      // the token endpoint refused, or answered with something that is not a usable token
    unsupported_token_type,    // the token is not a Bearer token
    insufficient_scope,        // a step-up would ask for nothing more than the last authorization did
    forbidden,                 // a 403 that is not about scope
    attempts_exceeded,         // a bound on repeated authorization was reached
    document_too_large,        // a document or response is larger than its bound
    timed_out,                 // a request or the whole flow took longer than its bound
    random_source_failed       // the operating system's random source failed
};

std::string_view to_string(AuthorizationProblem problem) noexcept;

// How a flow ended without credentials. It never carries a token, a code, a verifier, a state, a secret, a server's
// description or error URI, or a query string.
struct FlowFailure {
    AuthorizationProblem problem = AuthorizationProblem::discovery_failed;
    std::string issuer;  // the authorization server involved, when known
    std::string url;     // the URL involved, without its query and fragment
    // The error code an authorization server sent, when it is a code of the registered character set.
    std::optional<std::string> error_code;
    // No registration mechanism applied, and pre-registered credentials exist for another authorization server.
    bool credentials_for_other_issuer = false;
};

// Something a flow noticed and went on from. The text is the library's own and never holds what a peer sent.
struct FlowDiagnostic {
    enum class Kind {
        scope_dropped,                    // a scope value outside the scope-token syntax was left out
        scopes_truncated,                 // a list of scopes was longer than its bounds; the rest was left out
        authorization_servers_truncated,  // `authorization_servers` was longer than its bound; the rest was ignored
        authorization_server_skipped      // an `authorization_servers` entry was not admitted; the next one was tried
    };
    Kind kind = Kind::scope_dropped;
    std::string text;
};

// ---- Requests the flow wants sent

enum class FetchPurpose {
    protected_resource_metadata,
    authorization_server_metadata,
    registration,
    token
};

struct HeaderLine {
    std::string name;
    std::string value;
    friend bool operator==(const HeaderLine&, const HeaderLine&) = default;
};

// One request, described. It never carries the access token: the driver sends it without the Authorization field of
// the MCP requests; a header line here is the only Authorization it may carry (client authentication by the Basic
// scheme at a token endpoint).
struct FlowFetch {
    std::uint64_t id = 0;  // the answer is handed back with this id
    FetchPurpose purpose = FetchPurpose::protected_resource_metadata;
    std::string method;    // GET or POST
    std::string url;       // to fetch as written
    std::vector<HeaderLine> headers;
    std::string body;
    std::size_t body_bound = 0;  // the largest answer body to read; a longer one is reported as too large
    // Metadata requests follow 307 and 308 within the origin of `url`, at most `max_redirects` times; registration and
    // token requests follow none.
    bool follow_redirects = false;
    std::size_t max_redirects = 0;
    std::chrono::milliseconds timeout{0};
};

enum class FetchOutcome {
    response,         // an answer arrived; `status`, `content_type` and `body` hold it
    transport_error,  // no answer (connection, TLS, a refused redirect)
    timed_out,        // no answer within the request's timeout
    too_large         // the answer's body exceeded the request's bound
};

struct FetchResult {
    FetchOutcome outcome = FetchOutcome::response;
    int status = 0;
    std::string content_type;  // the Content-Type value as sent; empty when absent
    std::string body;
};

// ---- The prompt

struct PromptRequest {
    std::string authorization_url;  // to open in the person's browser
    std::string redirect_uri;       // where the browser will come back
    std::string issuer;             // the authorization server, for display
    std::vector<std::string> scopes;
    std::chrono::milliseconds timeout{0};
};

struct PromptAnswer {
    enum class Kind { callback, cancelled, timed_out };
    Kind kind = Kind::cancelled;
    std::string callback_url;  // with Kind::callback: the URL the browser came back to
};

// ---- Credentials

// The values an application also sees (neomcp/client/oauth_types.h).
using neomcp::ApplicationType;
using neomcp::CredentialKey;
using neomcp::PreRegisteredClient;
using neomcp::StoredClient;
using neomcp::StoredTokens;
using neomcp::TokenEndpointAuth;

std::string_view to_string(TokenEndpointAuth method) noexcept;
std::optional<TokenEndpointAuth> token_endpoint_auth_from(std::string_view name) noexcept;

std::string_view to_string(ApplicationType type) noexcept;

// Changes to the stored credentials a flow asks for, applied in this order: the erasures, then the puts. A flow that
// fails may still ask for some (an erased refresh token, an erased registration the server no longer knows).
struct StoreUpdate {
    std::vector<CredentialKey> erase_tokens;
    std::vector<std::string> erase_clients;  // by issuer
    std::optional<std::pair<std::string, StoredClient>> put_client;
    std::optional<std::pair<CredentialKey, StoredTokens>> put_tokens;
};

}  // namespace neomcp::detail::oauth
