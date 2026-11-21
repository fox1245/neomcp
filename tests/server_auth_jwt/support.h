// Helpers of the JWT validator's tests: keys made by OpenSSL when the tests run (no key is stored in the repository),
// their JWKs, tokens signed with them, the events a validator reported, a clock the test moves, and a blocking ask.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "neomcp/http/jwt.h"
#include "neomcp/http/resource_server.h"
#include "neomcp/json.h"

namespace neomcp::http {

// So that a test that fails names the event.
inline void PrintTo(JwtEvent event, std::ostream* out) { *out << to_string(event); }

}  // namespace neomcp::http

namespace jwt_test {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::http::JwtAlgorithm;
using neomcp::http::JwtEvent;
using neomcp::http::TokenRefusal;
using neomcp::http::TokenVerdict;

constexpr const char* issuer = "https://auth.example";
constexpr const char* resource = "http://localhost/mcp";
constexpr const char* jwks_path = "/jwks";

// A key pair made by OpenSSL: an RSA key of `bits`, or a P-256 key.
class TestKey {
public:
    enum class Kind { rsa, ec_p256 };

    TestKey(Kind kind, int bits);
    ~TestKey();
    TestKey(const TestKey&) = delete;
    TestKey& operator=(const TestKey&) = delete;

    Kind kind() const noexcept { return kind_; }
    // The public key as a JWK, with `kid` and `alg` when given.
    json jwk(std::optional<std::string> kid, std::optional<std::string> alg = std::nullopt) const;
    // The JWS signature of `input` by `algorithm` (RFC 7518, section 3), as the token carries it.
    std::string sign(JwtAlgorithm algorithm, std::string_view input) const;
    // Representations of the public key: the PEM of its SubjectPublicKeyInfo, and the DER.
    std::string public_pem() const;
    std::string public_der() const;

private:
    struct Impl;
    Kind kind_;
    std::unique_ptr<Impl> impl_;
};

// Keys made once per test program (RSA keys take a while to make): two of each kind, and a short RSA key.
const TestKey& rsa_key(int index);
const TestKey& ec_key(int index);
const TestKey& short_rsa_key();

std::string base64url(std::string_view bytes);

// The HMAC-SHA-256 of `input` with `secret`.
std::string hmac_sha256(std::string_view secret, std::string_view input);

// A token from a header and claims written as text (so that they may hold a name twice), signed by `key` with
// `algorithm`.
std::string token_from_text(std::string_view header, std::string_view claims, const TestKey& key,
                            JwtAlgorithm algorithm);
// A token from JSON header and claims.
std::string token(const json& header, const json& claims, const TestKey& key, JwtAlgorithm algorithm);

// The header of a good token: `alg`, `typ` "at+jwt" and `kid` (when given).
json good_header(JwtAlgorithm algorithm, std::optional<std::string> kid);
// The claims of a good token, valid for an hour from now.
json good_claims();
// A JWK Set of the keys given.
json key_set(std::vector<json> keys);

// The events a validator reported, in order.
class EventLog {
public:
    void add(JwtEvent event);
    std::vector<JwtEvent> all();
    std::size_t count(JwtEvent event);
    // The last event that is no fetch event.
    std::optional<JwtEvent> last_verdict();
    // Waits until `event` was reported `n` times; false when the bound lapsed first.
    bool wait_for(JwtEvent event, std::size_t n, std::chrono::milliseconds bound = 20s);

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<JwtEvent> events_;
};

// A steady clock that moves only when the test moves it.
class TestClock {
public:
    std::chrono::steady_clock::time_point now() const;
    void advance(std::chrono::milliseconds by);

private:
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::atomic<std::chrono::milliseconds::rep> offset_{0};
};

neomcp::http::JwtOptions options_for(const std::string& jwks_url, const std::shared_ptr<EventLog>& log);

std::shared_ptr<neomcp::http::TokenValidator> make(neomcp::http::JwtOptions options);

// The verdict of a validate() call that is made now and may be answered later.
std::future<TokenVerdict> validate_later(neomcp::http::TokenValidator& validator, const std::string& text);

// The verdict of one validate() call; throws when none comes within the bound.
TokenVerdict ask(neomcp::http::TokenValidator& validator, const std::string& text,
                 std::chrono::milliseconds bound = 20s);

bool is_refusal(const TokenVerdict& verdict, TokenRefusal expected);
const neomcp::Authorization* authorization_of(const TokenVerdict& verdict);

}  // namespace jwt_test
