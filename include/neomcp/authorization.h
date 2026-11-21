// Who sent a request, as a validated access token says, and the scopes a request needs.
//
// A transport that authorizes its requests (an OAuth resource server, such as the Streamable HTTP endpoint with
// authorization configured) validates the access token of each request and feeds the session what it found, an
// Authorization, with the input unit (Session::receive). The session keeps it with every call the unit starts, and a
// handler reads it (CallControl::authorization, Context::authorization). It never holds the token itself: what a
// handler sees cannot be passed on as a credential.
//
// Scopes. A tool, a resource, a resource template and a prompt may declare the scopes a request for it needs, all of
// them (ToolHost::tool_scopes and the matching members of the other hosts; the server's registration handles set them).
// When a session has read a request and found its item, and before anything of the item runs, it checks the unit's
// authorization against the item's declaration: a request whose authorization lacks a declared scope is refused with
// RefusalReason::insufficient_scope, and Output::required_scopes names what it lacks. In a batch, one request that lacks
// a scope refuses the whole batch, and no request of it runs. A scope is met by itself, or by a granted scope that the
// server declares implies it (ScopeImplications), never by a reading of its text: scope values are opaque, so a prefix
// or a wildcard implies nothing that is not declared.
//
// Where there is no authorization there is no identity to check, and declared scopes are not checked: on stdio and in
// process (credentials come from the environment there), and for every unit a transport feeds without one. A server
// that is meant to protect its items serves them only over a transport that authorizes every request.
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "neomcp/json.h"

namespace neomcp {

// What a validated access token says. Immutable once made, and shared by the requests that carried the token and by
// the calls they started.
struct Authorization {
    std::string issuer;                    // the authorization server that issued the token, exactly as it names itself
    std::optional<std::string> subject;    // `sub`: the resource owner, unique per issuer
    std::optional<std::string> client_id;  // the client the token was issued to
    std::vector<std::string> scopes;       // granted, as reported; compared exactly (case-sensitive)
    std::vector<std::string> audiences;    // `aud`, as reported
    std::optional<std::chrono::system_clock::time_point> expires;     // `exp`
    std::optional<std::chrono::system_clock::time_point> not_before;  // `nbf`, when the token has one
    // Whatever else the validator chose to report, for handlers only: the library decides nothing from it. Never the
    // token.
    json claims = json::object();
};

// The principal a request acts for: the issuer, the subject and the client, an absent part empty. Two requests act
// for the same principal exactly when their keys are equal, whatever else their tokens say (a refreshed token has
// other scopes and another expiry, and the same principal).
struct PrincipalKey {
    std::string issuer;
    std::string subject;
    std::string client_id;

    // One text per key: equal keys give equal texts and different keys different ones. Each part is written after
    // its length in bytes, so that no part's text can be mistaken for the boundary between two; the parts are bytes,
    // compared and written as they are.
    std::string text() const;

    friend bool operator==(const PrincipalKey&, const PrincipalKey&) = default;
};

PrincipalKey principal_of(const Authorization& granted);

// Whether `value` is a scope-token (RFC 6749, section 3.3): one or more of the characters %x21, %x23-5B and %x5D-7E.
bool is_scope_token(std::string_view value) noexcept;

// The scopes each scope implies, as the server declares them ("admin" implies "write"), closed transitively when the
// object is made: a scope implies what the scopes it implies imply, through any number of steps, and a cycle is fine
// (the closure is a fixed point). Copies share the closed table.
class ScopeImplications {
public:
    // A scope, and the scopes it implies directly.
    using Declared = std::map<std::string, std::vector<std::string>>;

    // Nothing implies anything.
    ScopeImplications() = default;
    // Throws std::invalid_argument, naming the value, for a scope that is not a scope-token.
    explicit ScopeImplications(const Declared& declared);

    // Whether `granted`, with what its scopes imply, holds `scope`.
    bool met(std::span<const std::string> granted, std::string_view scope) const;
    // What `scope` implies, closed and sorted, without `scope` itself unless a cycle leads back to it; empty when it
    // implies nothing.
    std::span<const std::string> implied_by(std::string_view scope) const;
    bool empty() const noexcept;

private:
    std::shared_ptr<const std::map<std::string, std::vector<std::string>, std::less<>>> closed_;
};

// The scopes of `required` that `granted` does not meet (ScopeImplications::met), in the order of `required`, each
// once. Empty when every one is met.
std::vector<std::string> missing_scopes(std::span<const std::string> granted, std::span<const std::string> required,
                                        const ScopeImplications& implications);

}  // namespace neomcp
