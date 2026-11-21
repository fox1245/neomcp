// Proof Key for Code Exchange with the S256 method (RFC 7636), and the `state` of an authorization request.
//
// The code verifier is 32 random octets in base64url without padding: 43 characters of the unreserved set, the
// length and alphabet RFC 7636, section 4.1 asks for. The challenge is the base64url of the SHA-256 digest of the
// verifier's ASCII text (section 4.2). `state` is made the same way. The random octets come from a source the caller
// gives, the operating system's in use and a fixed one in tests.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace neomcp::detail::oauth {

// `count` random octets, or nothing when the source failed.
using RandomSource = std::function<std::optional<std::string>(std::size_t count)>;

inline constexpr std::size_t pkce_verifier_octets = 32;
inline constexpr std::size_t state_octets = 32;
inline constexpr std::string_view code_challenge_method_s256 = "S256";

struct Pkce {
    std::string verifier;
    std::string challenge;
};

// The S256 challenge of a verifier.
std::string s256_challenge(std::string_view verifier);

// The verifier made from these octets, with its challenge.
Pkce pkce_from_octets(std::string_view octets);

// A fresh verifier and challenge; nothing when the source failed or gave the wrong number of octets.
std::optional<Pkce> make_pkce(const RandomSource& random);

// A fresh state; nothing when the source failed or gave the wrong number of octets.
std::optional<std::string> make_state(const RandomSource& random);

}  // namespace neomcp::detail::oauth
