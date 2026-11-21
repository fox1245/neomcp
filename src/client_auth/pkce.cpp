// PKCE and state (pkce.h).
#include "client_auth/pkce.h"

#include "core/base64.h"
#include "crypto/sha256.h"

namespace neomcp::detail::oauth {

namespace {

std::optional<std::string> octets_from(const RandomSource& random, std::size_t count) {
    if (!random) return std::nullopt;
    std::optional<std::string> octets = random(count);
    if (!octets || octets->size() != count) return std::nullopt;
    return octets;
}

}  // namespace

std::string s256_challenge(std::string_view verifier) {
    const Sha256Digest digest = sha256(verifier);
    return encode_base64url(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

Pkce pkce_from_octets(std::string_view octets) {
    Pkce pkce;
    pkce.verifier = encode_base64url(octets);
    pkce.challenge = s256_challenge(pkce.verifier);
    return pkce;
}

std::optional<Pkce> make_pkce(const RandomSource& random) {
    std::optional<std::string> octets = octets_from(random, pkce_verifier_octets);
    if (!octets) return std::nullopt;
    Pkce pkce = pkce_from_octets(*octets);
    wipe(octets->data(), octets->size());
    return pkce;
}

std::optional<std::string> make_state(const RandomSource& random) {
    std::optional<std::string> octets = octets_from(random, state_octets);
    if (!octets) return std::nullopt;
    return encode_base64url(*octets);
}

}  // namespace neomcp::detail::oauth
