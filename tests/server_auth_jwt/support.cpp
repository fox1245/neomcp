// Helpers of the JWT validator's tests (support.h).
#include "support.h"

#include <future>
#include <stdexcept>
#include <utility>
#include <variant>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "core/base64.h"

namespace jwt_test {

namespace {

constexpr std::size_t p256_coordinate_bytes = 32;
constexpr int rsa_bits = 2048;
constexpr int short_rsa_bits = 1024;

const unsigned char* bytes_of(std::string_view text) { return reinterpret_cast<const unsigned char*>(text.data()); }

std::string bignum_bytes(const BIGNUM* number, int padded_to = 0) {
    const int size = padded_to > 0 ? padded_to : BN_num_bytes(number);
    std::string out(static_cast<std::size_t>(size), '\0');
    const int written = padded_to > 0 ? BN_bn2binpad(number, reinterpret_cast<unsigned char*>(out.data()), size)
                                      : BN_bn2bin(number, reinterpret_cast<unsigned char*>(out.data()));
    if (written != size) throw std::runtime_error("a number of a test key could not be written");
    return out;
}

std::string bignum_param(EVP_PKEY* key, const char* name, int padded_to = 0) {
    BIGNUM* number = nullptr;
    if (EVP_PKEY_get_bn_param(key, name, &number) != 1) throw std::runtime_error("a test key has no such number");
    std::string out = bignum_bytes(number, padded_to);
    BN_free(number);
    return out;
}

}  // namespace

struct TestKey::Impl {
    EVP_PKEY* key = nullptr;
    ~Impl() { EVP_PKEY_free(key); }
};

TestKey::TestKey(Kind kind, int bits) : kind_(kind), impl_(std::make_unique<Impl>()) {
    impl_->key = kind == Kind::rsa ? EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(bits))
                                   : EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    if (impl_->key == nullptr) throw std::runtime_error("OpenSSL could not make a test key");
}

TestKey::~TestKey() = default;

json TestKey::jwk(std::optional<std::string> kid, std::optional<std::string> alg) const {
    json out = json::object();
    if (kind_ == Kind::rsa) {
        out["kty"] = "RSA";
        out["n"] = base64url(bignum_param(impl_->key, OSSL_PKEY_PARAM_RSA_N));
        out["e"] = base64url(bignum_param(impl_->key, OSSL_PKEY_PARAM_RSA_E));
    } else {
        out["kty"] = "EC";
        out["crv"] = "P-256";
        out["x"] = base64url(bignum_param(impl_->key, OSSL_PKEY_PARAM_EC_PUB_X, static_cast<int>(p256_coordinate_bytes)));
        out["y"] = base64url(bignum_param(impl_->key, OSSL_PKEY_PARAM_EC_PUB_Y, static_cast<int>(p256_coordinate_bytes)));
    }
    if (kid) out["kid"] = *kid;
    if (alg) out["alg"] = *alg;
    return out;
}

std::string TestKey::sign(JwtAlgorithm algorithm, std::string_view input) const {
    EVP_MD_CTX* digest = EVP_MD_CTX_new();
    EVP_PKEY_CTX* context = nullptr;
    std::string signature;
    bool ok = digest != nullptr && EVP_DigestSignInit(digest, &context, EVP_sha256(), nullptr, impl_->key) == 1;
    if (ok && algorithm == JwtAlgorithm::ps256) {
        ok = EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PSS_PADDING) > 0 &&
             EVP_PKEY_CTX_set_rsa_pss_saltlen(context, RSA_PSS_SALTLEN_DIGEST) > 0 &&
             EVP_PKEY_CTX_set_rsa_mgf1_md(context, EVP_sha256()) > 0;
    } else if (ok && algorithm == JwtAlgorithm::rs256) {
        ok = EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) > 0;
    }
    std::size_t length = 0;
    ok = ok && EVP_DigestSign(digest, nullptr, &length, bytes_of(input), input.size()) == 1;
    if (ok) {
        signature.resize(length);
        ok = EVP_DigestSign(digest, reinterpret_cast<unsigned char*>(signature.data()), &length, bytes_of(input),
                            input.size()) == 1;
        signature.resize(length);
    }
    EVP_MD_CTX_free(digest);
    if (!ok) throw std::runtime_error("OpenSSL could not sign with a test key");
    if (algorithm != JwtAlgorithm::es256) return signature;
    // The JWS form of an ECDSA signature: the two numbers at the size of the field, one after the other.
    const unsigned char* der = bytes_of(signature);
    ECDSA_SIG* pair = d2i_ECDSA_SIG(nullptr, &der, static_cast<long>(signature.size()));
    if (pair == nullptr) throw std::runtime_error("OpenSSL wrote a signature it cannot read");
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(pair, &r, &s);
    std::string joined = bignum_bytes(r, static_cast<int>(p256_coordinate_bytes)) +
                         bignum_bytes(s, static_cast<int>(p256_coordinate_bytes));
    ECDSA_SIG_free(pair);
    return joined;
}

std::string TestKey::public_pem() const {
    BIO* memory = BIO_new(BIO_s_mem());
    if (memory == nullptr || PEM_write_bio_PUBKEY(memory, impl_->key) != 1) {
        BIO_free(memory);
        throw std::runtime_error("OpenSSL could not write a test key");
    }
    char* data = nullptr;
    const long size = BIO_get_mem_data(memory, &data);
    std::string out(data, static_cast<std::size_t>(size));
    BIO_free(memory);
    return out;
}

std::string TestKey::public_der() const {
    unsigned char* data = nullptr;
    const int size = i2d_PUBKEY(impl_->key, &data);
    if (size <= 0) throw std::runtime_error("OpenSSL could not write a test key");
    std::string out(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
    OPENSSL_free(data);
    return out;
}

const TestKey& rsa_key(int index) {
    static const TestKey first(TestKey::Kind::rsa, rsa_bits);
    static const TestKey second(TestKey::Kind::rsa, rsa_bits);
    return index == 0 ? first : second;
}

const TestKey& ec_key(int index) {
    static const TestKey first(TestKey::Kind::ec_p256, 0);
    static const TestKey second(TestKey::Kind::ec_p256, 0);
    return index == 0 ? first : second;
}

const TestKey& short_rsa_key() {
    static const TestKey key(TestKey::Kind::rsa, short_rsa_bits);
    return key;
}

std::string base64url(std::string_view bytes) { return neomcp::detail::encode_base64url(bytes); }

std::string hmac_sha256(std::string_view secret, std::string_view input) {
    unsigned char out[EVP_MAX_MD_SIZE];
    std::size_t length = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret.data(), secret.size(), bytes_of(input),
                  input.size(), out, sizeof(out), &length) == nullptr) {
        throw std::runtime_error("OpenSSL could not compute an HMAC");
    }
    return std::string(reinterpret_cast<const char*>(out), length);
}

std::string token_from_text(std::string_view header, std::string_view claims, const TestKey& key,
                            JwtAlgorithm algorithm) {
    const std::string input = base64url(header) + "." + base64url(claims);
    return input + "." + base64url(key.sign(algorithm, input));
}

std::string token(const json& header, const json& claims, const TestKey& key, JwtAlgorithm algorithm) {
    return token_from_text(header.dump(), claims.dump(), key, algorithm);
}

json good_header(JwtAlgorithm algorithm, std::optional<std::string> kid) {
    json header = json{{"alg", std::string(neomcp::http::to_string(algorithm))}, {"typ", "at+jwt"}};
    if (kid) header["kid"] = *kid;
    return header;
}

json good_claims() {
    const auto in_an_hour = std::chrono::duration_cast<std::chrono::seconds>(
                                (std::chrono::system_clock::now() + std::chrono::hours(1)).time_since_epoch())
                                .count();
    return json{{"iss", issuer},
                {"sub", "alice"},
                {"client_id", "client-1"},
                {"aud", resource},
                {"exp", in_an_hour},
                {"iat", in_an_hour - 3600},
                {"jti", "id-1"},
                {"scope", "mcp:use files:read"}};
}

json key_set(std::vector<json> keys) {
    json set = json::object();
    set["keys"] = json::array();
    for (json& key : keys) set["keys"].push_back(std::move(key));
    return set;
}

void EventLog::add(JwtEvent event) {
    {
        std::lock_guard lock(mutex_);
        events_.push_back(event);
    }
    changed_.notify_all();
}

bool EventLog::wait_for(JwtEvent event, std::size_t n, std::chrono::milliseconds bound) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, bound, [&] {
        std::size_t found = 0;
        for (const JwtEvent seen : events_) found += seen == event ? 1 : 0;
        return found >= n;
    });
}

std::vector<JwtEvent> EventLog::all() {
    std::lock_guard lock(mutex_);
    return events_;
}

std::size_t EventLog::count(JwtEvent event) {
    std::lock_guard lock(mutex_);
    std::size_t found = 0;
    for (const JwtEvent seen : events_) found += seen == event ? 1 : 0;
    return found;
}

std::optional<JwtEvent> EventLog::last_verdict() {
    std::lock_guard lock(mutex_);
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
        if (*it != JwtEvent::key_set_fetched && *it != JwtEvent::key_set_fetch_failed) return *it;
    }
    return std::nullopt;
}

std::chrono::steady_clock::time_point TestClock::now() const {
    return start_ + std::chrono::milliseconds(offset_.load());
}

void TestClock::advance(std::chrono::milliseconds by) { offset_ += by.count(); }

neomcp::http::JwtOptions options_for(const std::string& jwks_url, const std::shared_ptr<EventLog>& log) {
    neomcp::http::JwtOptions options;
    options.issuer = issuer;
    options.jwks_uri = jwks_url;
    options.bounds.request_timeout = 10s;
    options.bounds.connect_timeout = 5s;
    options.observer = [log](JwtEvent event) { log->add(event); };
    return options;
}

std::shared_ptr<neomcp::http::TokenValidator> make(neomcp::http::JwtOptions options) {
    auto made = neomcp::http::make_jwt_validator(std::move(options));
    if (!made) throw std::runtime_error("the validator was refused: " + made.error);
    return *made.value;
}

std::future<TokenVerdict> validate_later(neomcp::http::TokenValidator& validator, const std::string& text) {
    auto verdict = std::make_shared<std::promise<TokenVerdict>>();
    auto future = verdict->get_future();
    auto once = std::make_shared<std::once_flag>();
    validator.validate(neomcp::http::TokenCheck{text, resource}, [verdict, once](TokenVerdict given) {
        std::call_once(*once, [&] { verdict->set_value(std::move(given)); });
    });
    return future;
}

TokenVerdict ask(neomcp::http::TokenValidator& validator, const std::string& text, std::chrono::milliseconds bound) {
    auto future = validate_later(validator, text);
    if (future.wait_for(bound) != std::future_status::ready) throw std::runtime_error("no verdict came");
    return future.get();
}

bool is_refusal(const TokenVerdict& verdict, TokenRefusal expected) {
    const auto* refusal = std::get_if<TokenRefusal>(&verdict);
    return refusal != nullptr && *refusal == expected;
}

const neomcp::Authorization* authorization_of(const TokenVerdict& verdict) {
    return std::get_if<neomcp::Authorization>(&verdict);
}

}  // namespace jwt_test
