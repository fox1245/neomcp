// The Streamable HTTP endpoint as an OAuth 2.1 protected resource: what it is configured with, and the validator of
// access tokens that an application supplies.
//
// With EndpointOptions::authorization (or HttpServerOptions::authorization) set, every request to the endpoint's path
// needs a valid Bearer token in its Authorization field (RFC 6750, section 2.1; neither the body nor the query is
// looked at). The endpoint reads the field, asks the TokenValidator what the token says, and then checks on its own,
// whatever the validator is, what no validator can switch off (RFC 9068, section 4):
//
//   - the token has an issuer and an expiry, and at most ResourceServerBounds::max_scopes scopes;
//   - the issuer is one of `authorization_servers`, compared exactly;
//   - the audiences hold `resource` or one of `accepted_audiences`, compared exactly (an empty list is refused);
//   - the token has not expired, and is valid already when it says from when (`nbf`), both with `leeway`;
//   - the token holds `required_scopes`, with the implications the server declares (ServerOptions::scope_implications).
//
// The answers (RFC 6750, section 3; RFC 9728, section 5.1), each with `Cache-Control: no-store`:
//
//   no Authorization field, or another scheme     401, a challenge with resource_metadata (and scope, when
//                                                 required_scopes is not empty), no body
//   two fields, or a Bearer field that cannot     400, a challenge with error "invalid_request", the OAuth error object
//   be read
//   a token the validator or a check above        401, a challenge with error "invalid_token", the OAuth error object
//   refuses, or longer than max_token_bytes
//   a token without required_scopes, or without   403, a challenge with error "insufficient_scope" and the scopes to ask
//   the scopes an item declares                   for (see below), the OAuth error object
//   the validator cannot answer now, does not     503, a JSON-RPC error; nothing is remembered
//   answer within validation_timeout, or too
//   many tokens wait for it
//
// The scopes a 403 asks for: those the request lacks, then required_scopes, then the scopes the token holds that the
// server names itself (in required_scopes, in scopes_supported, or in what one of its tools, resources, resource
// templates or prompts declares), each once. A token's other scopes are never echoed. When they would exceed what a
// client reads (ResourceServerDefaults::max_challenge_scopes, max_challenge_scope_bytes), the token's scopes are left
// out first. The endpoint does not start when what one item declares, with required_scopes, would exceed that; what
// several items need together (a batch, a subscription that names several resources) is cut to fit from its end,
// keeping required_scopes.
//
// The Authorization field never reaches the session: it is removed, and the session gets what was validated (an
// Authorization, see neomcp/authorization.h), with every call it starts. A session that a request opened belongs to
// the principal of that request (issuer, subject, client): a request of another principal that names it is answered
// as one that names no live session (404). A stream that a request opened (the stream of a session, a subscription)
// ends when the token that opened it expires (with the leeway); the client opens it again with its current token.
//
// The protected resource metadata document (RFC 9728) is served at `resource`'s origin, then
// /.well-known/oauth-protected-resource, then `resource`'s path (none when the path is empty or "/"), for GET and HEAD,
// without a token. Every URL in it and in the challenges is taken from the options verbatim; none is derived from a
// request (its Host field, say).
//
// What a validator said is remembered by a digest of the token (SHA-256; the token itself is not kept): a valid answer
// until the token expires (with the leeway) or for max_cache_age, whichever comes first; an invalid one for
// negative_cache_age; an unavailable one not at all. A token that is being validated is not validated again: the
// requests that carry it wait for the one answer, and go on in the order they arrived.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/authorization.h"
#include "neomcp/http/bearer.h"
#include "neomcp/http/challenge.h"
#include "neomcp/http/listener.h"

namespace neomcp::http {

// One token to validate, and the identifier of the resource that asks.
struct TokenCheck {
    // The token as presented. Valid during the call to validate() only: a validator that answers later copies what it
    // needs. Never logged, never kept beyond the validation.
    std::string_view token;
    // ResourceServerOptions::resource, verbatim.
    std::string_view resource;
};

enum class TokenRefusal {
    invalid,     // not a token this validator accepts (unknown, revoked, malformed, its signature fails): 401
    unavailable  // cannot be decided now (the authorization server is unreachable, say): 503, and nothing is remembered
};

using TokenVerdict = std::variant<Authorization, TokenRefusal>;

// Called with the verdict, exactly once, on any thread. A second call has no effect.
using TokenReply = std::function<void(TokenVerdict verdict)>;

// Decides what a token says. Implemented by the application (a table of opaque tokens, a call to an introspection
// endpoint, a check of a signed token); the endpoint checks what it reports (see above), so a validator reports what
// the token says and does not need to repeat those checks.
class TokenValidator {
public:
    virtual ~TokenValidator() = default;

    // Called once for each token whose verdict is not remembered, on the thread of the request that carried it,
    // possibly for several tokens at once. Answers through `reply`, at once or later from any thread. An answer that
    // comes after ResourceServerBounds::validation_timeout is dropped (the requests were answered 503); a reply that
    // is dropped without being called, and an exception that leaves validate(), count as `unavailable`.
    virtual void validate(const TokenCheck& check, TokenReply reply) = 0;
};

// A validator that answers at once, on the calling thread, with what `check` returns (a test, a table of tokens).
// `check` is called as validate() is: possibly on several threads at once. An exception counts as `unavailable`.
std::shared_ptr<TokenValidator> make_validator(std::function<TokenVerdict(const TokenCheck&)> check);

// The defaults of ResourceServerBounds, and the fixed bounds of what the endpoint writes.
struct ResourceServerDefaults {
    // The longest token that reaches the endpoint through this library's listener: the header line of
    // `Authorization: Bearer <token>` with its CRLF fits ListenerDefaults::max_header_line_bytes.
    static constexpr std::size_t max_token_bytes = ListenerDefaults::max_header_line_bytes -
                                                   authorization_field_name.size() - std::string_view(": ").size() -
                                                   bearer_scheme.size() - std::string_view(" ").size() -
                                                   std::string_view("\r\n").size();
    static constexpr std::size_t max_pending_validations = 256;
    static constexpr std::chrono::milliseconds validation_timeout{10000};
    static constexpr std::size_t max_cache_entries = 4096;
    static constexpr std::chrono::seconds max_cache_age{60};
    static constexpr std::chrono::seconds negative_cache_age{5};
    static constexpr std::chrono::seconds leeway{60};
    static constexpr std::chrono::seconds max_leeway{300};  // "no more than a few minutes" (RFC 9068, section 4)
    static constexpr std::size_t max_scopes = 256;
    static constexpr std::chrono::seconds metadata_max_age{300};

    // Fixed: what the client side of this library reads, so that what the server writes is read whole.
    static constexpr std::size_t max_authorization_servers = 16;
    static constexpr std::size_t max_metadata_bytes = std::size_t{64} * 1024;
    static constexpr std::size_t max_challenge_scopes = 64;
    static constexpr std::size_t max_challenge_scope_bytes = std::size_t{4} * 1024;
};

struct ResourceServerBounds {
    // The bytes of a token. A longer one is answered 401 invalid_token and is not validated. At least 1. HttpServer
    // refuses to start with more than the default, which is what its listener lets through.
    std::size_t max_token_bytes = ResourceServerDefaults::max_token_bytes;
    // Distinct tokens being validated at once; a request with one more new token is answered 503. At least 1.
    std::size_t max_pending_validations = ResourceServerDefaults::max_pending_validations;
    // The time one validation may take; the requests that wait for it are answered 503 after it. Positive.
    std::chrono::milliseconds validation_timeout = ResourceServerDefaults::validation_timeout;
    // Verdicts remembered at once; when full, the one nearest its end gives way. 0: none is remembered.
    std::size_t max_cache_entries = ResourceServerDefaults::max_cache_entries;
    // How long a valid verdict is remembered at most (a token revoked meanwhile still passes until then); 0: never.
    std::chrono::seconds max_cache_age = ResourceServerDefaults::max_cache_age;
    // How long an invalid verdict is remembered; 0: never.
    std::chrono::seconds negative_cache_age = ResourceServerDefaults::negative_cache_age;
    // The difference between clocks allowed for the expiry and `nbf`. At most ResourceServerDefaults::max_leeway.
    std::chrono::seconds leeway = ResourceServerDefaults::leeway;
    // The scopes a token may hold; one with more is refused 401 invalid_token. At least 1.
    std::size_t max_scopes = ResourceServerDefaults::max_scopes;
    // The max-age of the metadata document's Cache-Control.
    std::chrono::seconds metadata_max_age = ResourceServerDefaults::metadata_max_age;
};

struct ResourceServerOptions {
    // The resource identifier: the public URL of the endpoint, exactly as clients are given it (behind a proxy, the
    // proxy's). An absolute URL, https or http with a loopback host (for development and tests), without user
    // information, query or fragment. Written verbatim into the metadata document; tokens must name it (or one of
    // accepted_audiences) as their audience.
    std::string resource;
    // The issuer identifiers of the authorization servers, in the order clients should prefer them: one at least,
    // ResourceServerDefaults::max_authorization_servers at most, each https (or http with a loopback host), without
    // query or fragment. Only tokens of these issuers are accepted.
    std::vector<std::string> authorization_servers;
    // The scopes every request needs; also the `scope` of the 401 challenge. Scope tokens; not offline_access.
    std::vector<std::string> required_scopes;
    // The metadata document's scopes_supported. Empty: required_scopes. Left out of the document when empty. Scope
    // tokens; not offline_access.
    std::optional<std::vector<std::string>> scopes_supported;
    // The metadata document's resource_name and resource_documentation, when given. Not empty.
    std::optional<std::string> resource_name;
    std::optional<std::string> resource_documentation;
    // Audiences accepted besides `resource`, for an authorization server that names this resource otherwise. Not
    // empty strings.
    std::vector<std::string> accepted_audiences;
    // The path the listener serves the metadata document on, when a proxy in front rewrites paths. Empty: the path of
    // the document's URL (see above). The URL in challenges is always the public one, computed from `resource`.
    std::optional<std::string> metadata_path;
    // Required.
    std::shared_ptr<TokenValidator> validator;
    ResourceServerBounds bounds{};
    // The time tokens are checked against, and that remembered verdicts and streams end by; empty:
    // std::chrono::system_clock::now. Called on many threads, possibly at once. For tests.
    std::function<std::chrono::system_clock::time_point()> clock;
};

}  // namespace neomcp::http
