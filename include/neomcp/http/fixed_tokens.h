// The Streamable HTTP endpoint protected by a fixed table of bearer tokens that the application gives, for a server
// without an authorization server: each token names a subject and the scopes it grants, and may have an expiry.
//
// With EndpointOptions::fixed_tokens (or HttpServerOptions::fixed_tokens) set, every request to the endpoint's path
// needs one of the table's tokens in its Authorization field (`Authorization: Bearer <token>`, RFC 6750, section 2.1).
// The grants of the token reach the session as those of an OAuth access token do (neomcp/authorization.h): the handler
// reads them with Context::authorization(), a session belongs to the principal of the request that opened it (here the
// subject, since there is no issuer and no client), and required_scopes and the scopes that items declare are checked
// exactly as with ResourceServerOptions (neomcp/http/resource_server.h). What differs:
//
//   - there is no authorization server: no protected resource metadata is published (its well-known path is answered
//     as any other path the listener does not serve), and the challenges carry no resource_metadata;
//   - there is no issuer and no audience to check: the Authorization a handler reads has an empty `issuer` and no
//     `audiences`;
//   - a token's expiry is the one the table gives, compared with the endpoint's clock without leeway (both are the
//     application's); a token without one does not expire;
//   - nothing is remembered and nothing waits: every request looks its token up in the table that is current when it
//     arrives.
//
// The answers, each with `Cache-Control: no-store`:
//
//   no Authorization field, or another scheme     401, `WWW-Authenticate: Bearer` (with scope, when required_scopes
//                                                 is not empty), no body
//   two fields, or a Bearer field that cannot     400, a challenge with error "invalid_request", the OAuth error object
//   be read
//   a token that is not in the table, has         401, a challenge with error "invalid_token" (and scope, as above),
//   expired, or is longer than the listener       the OAuth error object
//   lets through
//   a token without required_scopes, or without   403, a challenge with error "insufficient_scope" and the scopes to ask
//   the scopes an item declares                   for (as with ResourceServerOptions), the OAuth error object
//
// A request that arrives while the endpoint stops is answered 503 with a JSON-RPC error, as it is without tokens.
//
// The table. Tokens are kept only as their SHA-256 digests; the text of a token is never kept, and the strings the
// application gave are overwritten once digested. A lookup computes the digest of the presented token and compares it
// with the digest of every entry of the table, each comparison in a time that does not depend on the bytes compared,
// and does not stop at the entry that matches. FixedTokenTable::create refuses a table it cannot serve, with the kind
// of problem and the entry at fault, so an endpoint is never given one.
//
// Replacement. Endpoint::replace_fixed_tokens (and HttpServer::replace_fixed_tokens) swaps the whole table at once: a
// request sees the table before or the table after, never a part of each. A token that the new table does not hold, or
// holds with other grants, is refused, or granted what the new table says, from the next request on. Every stream open
// with such a token ends shortly after the replacement, on the endpoint's thread, as a stream ends when its token
// expires: the stream of a session (GET), the event stream of a request in progress, and a subscription. The client
// opens it again and is answered with what the new table says (401 for a token that is gone). A stream of a token whose
// entry stays the same (its subject, scopes and expiry) stays open; the entries are matched by the digests of their
// tokens.
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
#include "neomcp/http/resource_server.h"

namespace neomcp::http {

namespace detail {
struct FixedTokenAccess;
}  // namespace detail

// One entry of the table, as the application gives it.
struct FixedToken {
    // The token: a b64token (RFC 6750, section 2.1), so that a client can send it; at most FixedTokenDefaults::
    // max_token_bytes. Overwritten by FixedTokenTable::create once digested.
    std::string token;
    // The principal the token acts for: Authorization::subject. Not empty. Tokens of one subject share its sessions.
    std::string subject;
    // What the token grants: Authorization::scopes. Scope tokens (neomcp::is_scope_token).
    std::vector<std::string> scopes;
    // From when the token is refused; empty: never.
    std::optional<std::chrono::system_clock::time_point> expires;
};

// The defaults of FixedTokenBounds, and the fixed bound of a token.
struct FixedTokenDefaults {
    static constexpr std::size_t max_tokens = 1024;
    static constexpr std::size_t max_scopes = ResourceServerDefaults::max_scopes;
    // Fixed: the longest token that reaches the endpoint through this library's listener.
    static constexpr std::size_t max_token_bytes = ResourceServerDefaults::max_token_bytes;
};

struct FixedTokenBounds {
    // The entries of one table. At least 1.
    std::size_t max_tokens = FixedTokenDefaults::max_tokens;
    // The scopes of one entry. At least 1.
    std::size_t max_scopes = FixedTokenDefaults::max_scopes;
};

// Why FixedTokenTable::create refused a table.
enum class FixedTokenProblem {
    invalid_bounds,   // a bound of FixedTokenBounds is 0
    too_many_tokens,  // more entries than max_tokens
    empty_token,      // an entry's token is empty
    malformed_token,  // an entry's token is not a b64token
    token_too_large,  // an entry's token is longer than FixedTokenDefaults::max_token_bytes
    duplicate_token,  // two entries have the same token
    empty_subject,    // an entry's subject is empty
    too_many_scopes,  // an entry has more scopes than max_scopes
    invalid_scope     // an entry has a scope that is not a scope token
};

// The kind of problem, for a person to read; never a token.
std::string_view to_string(FixedTokenProblem problem) noexcept;

struct FixedTokenError {
    FixedTokenProblem problem = FixedTokenProblem::malformed_token;
    // The entry at fault, by its place in what was given (for duplicate_token, the later of the two); 0 for
    // invalid_bounds and too_many_tokens.
    std::size_t index = 0;
};

// A table of tokens, checked, as digests. Immutable; copies share it. A table made with no entries refuses every token.
class FixedTokenTable {
public:
    // A table with no entries.
    FixedTokenTable();

    // The table of `tokens`, or the first problem found (the bounds, then the count, then each entry in order, a
    // duplicate as the later of the two). In either case every token string of `tokens` is overwritten with zero bytes
    // (its size stays), and the subjects and scopes may have been moved out.
    static std::variant<FixedTokenTable, FixedTokenError> create(std::vector<FixedToken>&& tokens,
                                                                 const FixedTokenBounds& bounds = {});

    // What the entry whose token is `token` grants, or null when no entry is (see the top of the file for how it is
    // compared). The expiry is not checked here: the endpoint checks it with its clock. The same object for every
    // lookup of one entry of one table.
    std::shared_ptr<const Authorization> find(std::string_view token) const;

    std::size_t size() const noexcept;

    struct Entries;  // internal

private:
    friend struct detail::FixedTokenAccess;
    explicit FixedTokenTable(std::shared_ptr<const Entries> entries);

    std::shared_ptr<const Entries> entries_;
};

struct FixedTokenOptions {
    // The tokens accepted when the endpoint starts; replaced with Endpoint::replace_fixed_tokens.
    FixedTokenTable tokens;
    // The scopes every request needs; also the `scope` of the 401 challenges. Scope tokens; not offline_access. With
    // what one item declares, at most what one challenge carries (ResourceServerDefaults::max_challenge_scopes and
    // max_challenge_scope_bytes).
    std::vector<std::string> required_scopes;
    // The time expiries are checked against, and that streams end by; empty: std::chrono::system_clock::now. Called on
    // many threads, possibly at once. For tests.
    std::function<std::chrono::system_clock::time_point()> clock;
};

}  // namespace neomcp::http
