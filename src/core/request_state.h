// Sealed request state: the state of a call that asked for input, carried by the client to the retried call.
//
// Signed, not encrypted. The client can read everything in it, so it must hold no secret; it cannot change any of it
// without the change being detected. Shared by the sources of the core; not part of the public interface.
//
// A sealed state is the text `<payload>.<code>`: the payload is a JSON object of claims, the code its HMAC-SHA-256,
// both in base64url without padding. The code covers a label of this format and the payload. Opening checks the code
// first, in constant time, against every key, and only then reads the payload: its format version, then the claims.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/session.h"

namespace neomcp::detail {

// What a sealed state says about itself and about the request it was issued for.
struct StateClaims {
    std::int64_t expires = 0;   // seconds since the Unix epoch; the state is refused from then on
    std::string method;         // the request's method
    std::string subject;        // the tool or prompt name, or the resource URI
    std::string arguments;      // the digest of the request's arguments (arguments_digest)
    std::string server;         // the name of the server that issued it
    std::string principal;      // who the client acts for (principal_digest); empty when the call had no authorization
    std::uint64_t round = 0;    // how many times the call asked before, counting this time
    std::map<std::string, std::string> asked;  // the keys of the requests asked, with each request's method
    std::optional<std::string> state;          // the host's own state
};

// The shortest key that seals (bytes).
inline constexpr std::size_t min_state_key_size = min_request_state_key_size;

// The process's default key: 32 bytes from the operating system's random source, made on first use. Empty when the
// source failed.
const std::optional<std::string>& default_state_key();

class StateSealer {
public:
    // `keys` is not empty and each key has at least min_state_key_size bytes. The first seals; every one opens.
    explicit StateSealer(std::vector<std::string> keys);

    std::string seal(const StateClaims& claims) const;
    // The claims of a sealed state this server issued and nobody changed, or why it is refused (malformed, signature,
    // version). Expiry and binding are the caller's checks (see check_claims).
    std::variant<StateClaims, RequestStateProblem> open(std::string_view text) const;

private:
    std::vector<std::string> keys_;
};

// What a retried request must match.
struct StateBinding {
    std::string_view method;
    std::string_view subject;
    std::string_view arguments;  // arguments_digest of the retried request
    std::string_view server;
    std::string_view principal;
};

// Why opened claims do not fit the request at time `now`: expired (now is at or after the expiry) or binding.
std::optional<RequestStateProblem> check_claims(const StateClaims& claims, const StateBinding& binding,
                                                std::chrono::system_clock::time_point now);

// The JSON text of a value in one canonical form: object members sorted by name (byte order) at every depth, arrays
// in their order, a number that holds an integer written as that integer (1.0 as 1, -0.0 as 0), any other number as
// the library writes doubles, strings as the library writes them. Equal values give equal texts whatever the order
// of their members and however their integers were written (the idea of RFC 8785, without its number format).
std::string canonical_json(const json& value);

// The base64url text of the SHA-256 digest of canonical_json(arguments).
std::string arguments_digest(const json& arguments);

// What a sealed state binds the principal of a call by: the base64url text of the SHA-256 digest of the text of its
// PrincipalKey, so that the state, which the client can read, does not carry who the principal is; empty for a call
// without an authorization. Equal for two authorizations of one principal, and different for different principals.
std::string principal_digest(const Authorization* granted);

}  // namespace neomcp::detail
