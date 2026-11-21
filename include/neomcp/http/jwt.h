// A TokenValidator that checks JWT access tokens itself (RFC 9068): the signature with the authorization server's
// public keys, which it fetches from the server's key set (a JWK Set, RFC 7517), and the token's header and claims. It
// is a component of its own (neomcp::server_auth_jwt), built with NEOMCP_JWT, which needs OpenSSL's libcrypto 3.0 or
// newer for the signatures and the HTTP client's libcurl (NEOMCP_HTTP_CLIENT) for the key set; the header names no
// type of either.
//
// One validator serves one issuer. For a token it reads (RFC 9068, section 4; RFC 8725):
//
//   form      the JWS compact form: three parts of base64url without padding, separated by '.'. Anything else (an
//             encrypted token of five parts, say) is malformed.
//   header    a JSON object; a name that occurs twice anywhere in it makes it malformed.
//     alg     must name an algorithm of JwtOptions::algorithms. The name only selects an entry of that list; it never
//             makes the validator use an algorithm the list does not hold. "none" and the HMAC algorithms are never
//             accepted: no list can hold them.
//     typ     must be "at+jwt" or "application/at+jwt", compared without regard to case (RFC 9068, section 2.1). An
//             absent `typ` is refused too, so an identity token cannot pass as an access token.
//     crit    must be absent: the validator understands no extension, so a token that requires one is refused
//             (RFC 7515, section 4.1.11).
//     kid     when present, a string; it selects the keys of the set with that identifier.
//     jku, x5u, jwk, x5c   are never read: keys come from the configured key set only.
//   signature checked over the header and payload as received, with a key of the set that fits the algorithm (below).
//   claims    a JSON object, read only after the signature was found good; a name that occurs twice anywhere in it
//             makes it malformed.
//     iss     must be present and equal to JwtOptions::issuer, compared exactly.
//     exp     must be present: a JSON number (NumericDate). It becomes Authorization::expires.
//     aud     must be present: a string or an array of strings. It becomes Authorization::audiences.
//     nbf     a number, or absent: Authorization::not_before.
//     sub, client_id   strings, or absent.
//     scope   a string of scope-tokens separated by spaces, or absent: Authorization::scopes.
//     every other claim is copied to Authorization::claims, for handlers; the library decides nothing from it.
//   The endpoint then applies its own checks to what the validator reports (issuer, audience, expiry, scopes).
//
// The keys. A key of the set is used for a token when its `kty` is the one the token's algorithm needs ("RSA" for RS256
// and PS256, "EC" with `crv` "P-256" for ES256), when its `use` is absent or "sig", when its `key_ops` is absent or holds
// "verify", and when its own `alg`, if it has one, is the token's. A key without `alg` is used only when exactly one
// algorithm of JwtOptions::algorithms fits its type, so that every key serves one algorithm (RFC 8725, section 3.1). RSA
// keys shorter than 2048 bits, and keys that cannot be read, are left out of the set; keys of other types (a symmetric
// "oct" key among them) are never used. When the token names a `kid`, only keys with that `kid` are tried; otherwise
// every key that fits.
//
// The key set comes from JwtOptions::jwks_uri, or, when that is empty, from the `jwks_uri` of the authorization server's
// metadata (RFC 8414), read at the issuer's well-known URL (/.well-known/oauth-authorization-server inserted between the
// issuer's host and its path); the metadata's `issuer` must equal JwtOptions::issuer. Each URL is an https URL, or an
// http URL with a loopback host, without user information or fragment; the certificate of an https URL is always
// verified and no redirect is followed. The set is fetched when the first token needs it, again when a token names no
// key the set holds, and again when it is older than max_key_set_age (the current set answers meanwhile). Two fetches
// start no closer than min_key_refresh_interval, whatever their outcome, so tokens with unknown keys cannot make the
// validator ask the authorization server more often than that. A token that waits for a fetch is answered when it ends.
//
// The verdict.
//
//   malformed, refused, or a signature that fails     TokenRefusal::invalid (401)
//   no key of the set fits, and no fetch may start    TokenRefusal::invalid
//   no key set could be fetched (the server cannot     TokenRefusal::unavailable (503): it cannot be reached, answers
//   be reached, or its answer cannot be used)          late, with another status than 200, with a body over its bound
//                                                      or a body that is not a key set; a key set held from an
//                                                      earlier fetch still answers
//   a good token                                       an Authorization, issuer set to JwtOptions::issuer
//
// Secrets. Neither the token, nor a key, nor a claim, nor a URL appears in any error text or in the observer's reports:
// reports name a kind of event and nothing else.
//
// Threads. validate() answers on the calling thread when the key set it holds decides the token, which is the usual
// case; a token that waits for the key set is answered on the validator's own thread, which runs the fetches. The
// validator may be used by several requests at once. Dropping it answers every token still waiting `unavailable`.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "neomcp/http/outcome.h"
#include "neomcp/http/resource_server.h"

namespace neomcp::http {

// The signature algorithms the validator can check (RFC 7518, section 3).
enum class JwtAlgorithm {
    rs256,  // RSASSA-PKCS1-v1_5 with SHA-256; every validator of RFC 9068 must support it
    ps256,  // RSASSA-PSS with SHA-256 and MGF1 with SHA-256
    es256   // ECDSA on P-256 with SHA-256
};

// The name of an algorithm as a JOSE header writes it ("RS256", "PS256", "ES256").
std::string_view to_string(JwtAlgorithm algorithm) noexcept;

// The defaults of JwtBounds, as constants.
struct JwtDefaults {
    static constexpr std::size_t max_jwks_bytes = std::size_t{64} * 1024;
    static constexpr std::size_t max_metadata_bytes = ResourceServerDefaults::max_metadata_bytes;
    static constexpr std::size_t max_keys = 64;
    static constexpr std::chrono::milliseconds min_key_refresh_interval{30000};
    static constexpr std::chrono::milliseconds max_key_set_age{600000};
    // Per request. Shorter than ResourceServerDefaults::validation_timeout, so that a key set fetched in one request
    // that comes late still comes in time for the tokens that wait for it; a set found through the metadata takes two
    // requests, one after the other.
    static constexpr std::chrono::milliseconds request_timeout{8000};
    static constexpr std::chrono::milliseconds connect_timeout{3000};
    static constexpr std::size_t max_response_header_bytes = std::size_t{16} * 1024;
    static constexpr std::size_t max_pending = ResourceServerDefaults::max_pending_validations;
};

struct JwtBounds {
    // The bytes of the key set's body; a longer one is not read further and the fetch fails. At least 1.
    std::size_t max_jwks_bytes = JwtDefaults::max_jwks_bytes;
    // The same for the authorization server's metadata. At least 1.
    std::size_t max_metadata_bytes = JwtDefaults::max_metadata_bytes;
    // The keys read from a set; the keys after them are ignored. At least 1.
    std::size_t max_keys = JwtDefaults::max_keys;
    // The least time between the starts of two fetches of the key set. Not negative.
    std::chrono::milliseconds min_key_refresh_interval = JwtDefaults::min_key_refresh_interval;
    // The age after which the key set is fetched again (the current set answers until the new one is there). Not less
    // than min_key_refresh_interval.
    std::chrono::milliseconds max_key_set_age = JwtDefaults::max_key_set_age;
    // The time one request may take from its start to the end of its body. Positive.
    std::chrono::milliseconds request_timeout = JwtDefaults::request_timeout;
    // The time connecting may take; not more than request_timeout. Positive.
    std::chrono::milliseconds connect_timeout = JwtDefaults::connect_timeout;
    // The bytes of an answer's header fields. At least 1.
    std::size_t max_response_header_bytes = JwtDefaults::max_response_header_bytes;
    // Tokens that wait for a fetch at once; one more is `unavailable`. At least 1.
    std::size_t max_pending = JwtDefaults::max_pending;
};

// What happened, for an application that counts it. The kinds carry no text of a token, a key or an exchange.
enum class JwtEvent {
    accepted,              // a good token: the verdict is an Authorization
    malformed,             // not a JWS in compact form, or a header or claims that cannot be read: invalid
    algorithm_refused,     // `alg` is absent or names no algorithm of JwtOptions::algorithms: invalid
    type_refused,          // `typ` is absent or is not an access token's: invalid
    critical_header,       // the header has `crit`: invalid
    unknown_key,           // no key of the set fits the token, and the set could not be fetched again now: invalid
    signature_failed,      // no key that fits verifies the signature: invalid
    issuer_mismatch,       // `iss` is not JwtOptions::issuer: invalid
    key_set_unavailable,   // no key set to decide with: unavailable
    overloaded,            // max_pending tokens waited for a fetch: unavailable
    stopped,               // the validator was dropped while the token waited: unavailable
    key_set_fetched,       // a fetch of the key set ended with a set
    key_set_fetch_failed   // a fetch ended without one
};

// The name of an event, for a person to read.
std::string_view to_string(JwtEvent event) noexcept;

struct JwtOptions {
    // The issuer of the tokens (Authorization::issuer), the only `iss` a token may name. One of
    // ResourceServerOptions::authorization_servers. Not empty; a URL as described above when jwks_uri is empty.
    std::string issuer;
    // The URL of the key set; empty: taken from the authorization server's metadata.
    std::string jwks_uri;
    // The algorithms a token may be signed with. Not empty.
    std::vector<JwtAlgorithm> algorithms{JwtAlgorithm::rs256, JwtAlgorithm::es256};
    JwtBounds bounds{};
    // A file of certificate authorities to verify https URLs with, instead of the system's. Verification itself
    // cannot be switched off.
    std::optional<std::filesystem::path> ca_file;
    // Let libcurl use the proxy the environment names (http_proxy, https_proxy, no_proxy). By default none is used.
    bool use_proxy_from_environment = false;
    // The library resolving host names must resolve them asynchronously, or a fetch could not be stopped while a name
    // is resolved; set this to accept a library that blocks.
    bool allow_blocking_resolver = false;
    // Told of each event, on the thread it happened on; it must return promptly and must not call the validator.
    // Optional.
    std::function<void(JwtEvent)> observer;
    // The clock the refresh bounds are measured on; empty: the steady clock. For tests.
    std::function<std::chrono::steady_clock::time_point()> clock;
};

// Makes the validator, or says (without any of the options' URLs) what is wrong with the options. Nothing is fetched
// until a token needs the key set.
Outcome<std::shared_ptr<TokenValidator>> make_jwt_validator(JwtOptions options);

}  // namespace neomcp::http
