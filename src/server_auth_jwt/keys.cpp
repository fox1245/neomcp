// The key set and the signatures (keys.h), on OpenSSL's libcrypto 3 (the EVP interface only).
#include "server_auth_jwt/keys.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

#include "core/base64.h"
#include "server_auth_jwt/token.h"

namespace neomcp::http::detail {

namespace {

using neomcp::detail::decode_base64url;

// The members of a JWK Set and of a JWK (RFC 7517, sections 4 and 5; RFC 7518, section 6).
constexpr std::string_view set_keys = "keys";
constexpr std::string_view jwk_kty = "kty";
constexpr std::string_view jwk_use = "use";
constexpr std::string_view jwk_key_ops = "key_ops";
constexpr std::string_view jwk_alg = "alg";
constexpr std::string_view jwk_kid = "kid";
constexpr std::string_view jwk_n = "n";
constexpr std::string_view jwk_e = "e";
constexpr std::string_view jwk_crv = "crv";
constexpr std::string_view jwk_x = "x";
constexpr std::string_view jwk_y = "y";
constexpr std::string_view use_signature = "sig";
constexpr std::string_view operation_verify = "verify";
constexpr std::string_view kty_rsa = "RSA";
constexpr std::string_view kty_ec = "EC";
constexpr std::string_view crv_p256 = "P-256";
// OpenSSL's names of the key types and of the curve.
constexpr const char* openssl_rsa = "RSA";
constexpr const char* openssl_ec = "EC";
constexpr const char* openssl_p256 = "P-256";
// RSA keys shorter than this are not used (RFC 7518, section 3.3).
constexpr int min_rsa_bits = 2048;
// The bytes of a coordinate of P-256 and of an ES256 signature (RFC 7518, sections 3.4 and 6.2.1.2).
constexpr std::size_t p256_coordinate_bytes = 32;
constexpr std::size_t es256_signature_bytes = 2 * p256_coordinate_bytes;
// The first byte of an uncompressed point (SEC 1, section 2.3.3).
constexpr unsigned char uncompressed_point = 0x04;

struct PkeyFree {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
struct PkeyContextFree {
    void operator()(EVP_PKEY_CTX* context) const noexcept { EVP_PKEY_CTX_free(context); }
};
struct DigestContextFree {
    void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};
struct BignumFree {
    void operator()(BIGNUM* number) const noexcept { BN_free(number); }
};
struct ParamBuildFree {
    void operator()(OSSL_PARAM_BLD* build) const noexcept { OSSL_PARAM_BLD_free(build); }
};
struct ParamFree {
    void operator()(OSSL_PARAM* params) const noexcept { OSSL_PARAM_free(params); }
};
struct SignatureFree {
    void operator()(ECDSA_SIG* signature) const noexcept { ECDSA_SIG_free(signature); }
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;
using Bignum = std::unique_ptr<BIGNUM, BignumFree>;

const unsigned char* bytes_of(std::string_view text) noexcept {
    return reinterpret_cast<const unsigned char*>(text.data());
}

// The kind of key an algorithm needs.
enum class KeyKind { rsa, ec_p256 };

KeyKind kind_of(JwtAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case JwtAlgorithm::rs256:
        case JwtAlgorithm::ps256: return KeyKind::rsa;
        case JwtAlgorithm::es256: return KeyKind::ec_p256;
    }
    return KeyKind::rsa;
}

Bignum bignum_of(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return nullptr;
    return Bignum(BN_bin2bn(bytes_of(bytes), static_cast<int>(bytes.size()), nullptr));
}

// A public key made from parameters, and checked as OpenSSL checks a public key (an EC point on its curve, an RSA
// modulus that is odd and has no small factors).
Pkey public_key_from(const char* type, OSSL_PARAM_BLD* build) {
    const std::unique_ptr<OSSL_PARAM, ParamFree> params(OSSL_PARAM_BLD_to_param(build));
    if (!params) return nullptr;
    const std::unique_ptr<EVP_PKEY_CTX, PkeyContextFree> maker(EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr));
    if (!maker || EVP_PKEY_fromdata_init(maker.get()) != 1) return nullptr;
    EVP_PKEY* made = nullptr;
    if (EVP_PKEY_fromdata(maker.get(), &made, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) return nullptr;
    Pkey key(made);
    const std::unique_ptr<EVP_PKEY_CTX, PkeyContextFree> checker(EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr));
    if (!checker || EVP_PKEY_public_check(checker.get()) != 1) return nullptr;
    return key;
}

Pkey rsa_key(const json& jwk) {
    const std::string* n_text = string_member(jwk, jwk_n);
    const std::string* e_text = string_member(jwk, jwk_e);
    if (n_text == nullptr || e_text == nullptr) return nullptr;
    const auto n_bytes = decode_base64url(*n_text);
    const auto e_bytes = decode_base64url(*e_text);
    if (!n_bytes || !e_bytes) return nullptr;
    const Bignum n = bignum_of(*n_bytes);
    const Bignum e = bignum_of(*e_bytes);
    if (!n || !e) return nullptr;
    const std::unique_ptr<OSSL_PARAM_BLD, ParamBuildFree> build(OSSL_PARAM_BLD_new());
    if (!build || OSSL_PARAM_BLD_push_BN(build.get(), OSSL_PKEY_PARAM_RSA_N, n.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(build.get(), OSSL_PKEY_PARAM_RSA_E, e.get()) != 1) {
        return nullptr;
    }
    Pkey key = public_key_from(openssl_rsa, build.get());
    if (!key || EVP_PKEY_get_bits(key.get()) < min_rsa_bits) return nullptr;
    return key;
}

Pkey ec_p256_key(const json& jwk) {
    const std::string* x_text = string_member(jwk, jwk_x);
    const std::string* y_text = string_member(jwk, jwk_y);
    if (x_text == nullptr || y_text == nullptr) return nullptr;
    const auto x = decode_base64url(*x_text);
    const auto y = decode_base64url(*y_text);
    // Each coordinate is exactly the size of the field (RFC 7518, section 6.2.1.2).
    if (!x || !y || x->size() != p256_coordinate_bytes || y->size() != p256_coordinate_bytes) return nullptr;
    std::string point(1, static_cast<char>(uncompressed_point));
    point += *x;
    point += *y;
    const std::unique_ptr<OSSL_PARAM_BLD, ParamBuildFree> build(OSSL_PARAM_BLD_new());
    if (!build ||
        OSSL_PARAM_BLD_push_utf8_string(build.get(), OSSL_PKEY_PARAM_GROUP_NAME, openssl_p256, 0) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(build.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) != 1) {
        return nullptr;
    }
    return public_key_from(openssl_ec, build.get());
}

// The kind of key a JWK is, when it is one the validator can use at all.
std::optional<KeyKind> jwk_kind(const json& jwk) {
    const std::string* kty = string_member(jwk, jwk_kty);
    if (kty == nullptr) return std::nullopt;
    if (*kty == kty_rsa) return KeyKind::rsa;
    if (*kty == kty_ec) {
        const std::string* crv = string_member(jwk, jwk_crv);
        if (crv != nullptr && *crv == crv_p256) return KeyKind::ec_p256;
    }
    return std::nullopt;
}

// Whether the JWK may verify signatures: `use` absent or "sig", `key_ops` absent or holding "verify" (RFC 7517,
// sections 4.2 and 4.3).
bool may_verify(const json& jwk) {
    if (const json* use = member(jwk, jwk_use)) {
        const auto* text = use->get_ptr<const json::string_t*>();
        if (text == nullptr || *text != use_signature) return false;
    }
    if (const json* operations = member(jwk, jwk_key_ops)) {
        const auto* items = operations->get_ptr<const json::array_t*>();
        if (items == nullptr) return false;
        bool verify = false;
        for (const json& item : *items) {
            const auto* text = item.get_ptr<const json::string_t*>();
            if (text == nullptr) return false;
            verify = verify || *text == operation_verify;
        }
        if (!verify) return false;
    }
    return true;
}

// The one algorithm a JWK serves among those allowed: its own `alg` when it has one, otherwise the only allowed
// algorithm of its kind; nothing when there is none, or more than one.
std::optional<JwtAlgorithm> algorithm_of(const json& jwk, KeyKind kind, std::span<const JwtAlgorithm> allowed) {
    if (const json* alg = member(jwk, jwk_alg)) {
        const auto* text = alg->get_ptr<const json::string_t*>();
        if (text == nullptr) return std::nullopt;
        const auto named = algorithm_named(*text, allowed);
        if (!named || kind_of(*named) != kind) return std::nullopt;
        return named;
    }
    std::optional<JwtAlgorithm> only;
    for (const JwtAlgorithm candidate : allowed) {
        if (kind_of(candidate) != kind || (only && *only == candidate)) continue;
        if (only) return std::nullopt;
        only = candidate;
    }
    return only;
}

bool verify_with(EVP_PKEY* key, JwtAlgorithm algorithm, std::string_view signing_input, std::string_view signature) {
    // The signature as OpenSSL takes it: RSA signatures as they are, at the size of the modulus; an ECDSA signature
    // as the DER of its two numbers, from the fixed-size pair the JWS carries (RFC 7518, section 3.4).
    std::string der;
    if (kind_of(algorithm) == KeyKind::rsa) {
        const int size = EVP_PKEY_get_size(key);
        if (size <= 0 || signature.size() != static_cast<std::size_t>(size)) return false;
    } else {
        if (signature.size() != es256_signature_bytes) return false;
        const std::unique_ptr<ECDSA_SIG, SignatureFree> pair(ECDSA_SIG_new());
        if (!pair) return false;
        Bignum r(BN_bin2bn(bytes_of(signature), static_cast<int>(p256_coordinate_bytes), nullptr));
        Bignum s(BN_bin2bn(bytes_of(signature.substr(p256_coordinate_bytes)), static_cast<int>(p256_coordinate_bytes),
                           nullptr));
        if (!r || !s || ECDSA_SIG_set0(pair.get(), r.get(), s.get()) != 1) return false;
        static_cast<void>(r.release());
        static_cast<void>(s.release());
        const int length = i2d_ECDSA_SIG(pair.get(), nullptr);
        if (length <= 0) return false;
        der.resize(static_cast<std::size_t>(length));
        auto* out = reinterpret_cast<unsigned char*>(der.data());
        if (i2d_ECDSA_SIG(pair.get(), &out) != length) return false;
        signature = der;
    }
    const std::unique_ptr<EVP_MD_CTX, DigestContextFree> digest(EVP_MD_CTX_new());
    if (!digest) return false;
    EVP_PKEY_CTX* context = nullptr;  // owned by `digest`
    if (EVP_DigestVerifyInit(digest.get(), &context, EVP_sha256(), nullptr, key) != 1) return false;
    switch (algorithm) {
        case JwtAlgorithm::rs256:
            if (EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) <= 0) return false;
            break;
        case JwtAlgorithm::ps256:
            // The salt is as long as the digest, and MGF1 uses the same digest (RFC 7518, section 3.5).
            if (EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PSS_PADDING) <= 0 ||
                EVP_PKEY_CTX_set_rsa_pss_saltlen(context, RSA_PSS_SALTLEN_DIGEST) <= 0 ||
                EVP_PKEY_CTX_set_rsa_mgf1_md(context, EVP_sha256()) <= 0) {
                return false;
            }
            break;
        case JwtAlgorithm::es256: break;
    }
    return EVP_DigestVerify(digest.get(), bytes_of(signature), signature.size(), bytes_of(signing_input),
                            signing_input.size()) == 1;
}

}  // namespace

struct PublicKey::Handle {
    Pkey key;
};

PublicKey::PublicKey(JwtAlgorithm algorithm, std::unique_ptr<Handle> handle)
    : algorithm_(algorithm), handle_(std::move(handle)) {}

PublicKey::~PublicKey() = default;

bool PublicKey::verifies(std::string_view signing_input, std::string_view signature) const {
    return verify_with(handle_->key.get(), algorithm_, signing_input, signature);
}

// Makes the keys; a friend of PublicKey.
struct KeyMaker {
    static std::shared_ptr<const PublicKey> make(JwtAlgorithm algorithm, Pkey key) {
        auto handle = std::make_unique<PublicKey::Handle>();
        handle->key = std::move(key);
        return std::shared_ptr<const PublicKey>(new PublicKey(algorithm, std::move(handle)));
    }
};

std::vector<std::shared_ptr<const PublicKey>> KeySet::candidates(JwtAlgorithm algorithm,
                                                                 const std::optional<std::string>& kid) const {
    std::vector<std::shared_ptr<const PublicKey>> found;
    for (const KeyEntry& entry : entries_) {
        if (entry.key->algorithm() != algorithm) continue;
        if (kid && entry.kid != kid) continue;
        found.push_back(entry.key);
    }
    return found;
}

std::optional<KeySet> read_key_set(std::string_view body, std::span<const JwtAlgorithm> allowed, std::size_t max_keys) {
    const std::optional<json> set = parse_unique_object(body);
    if (!set) return std::nullopt;
    const json* keys = member(*set, set_keys);
    const auto* items = keys == nullptr ? nullptr : keys->get_ptr<const json::array_t*>();
    if (items == nullptr) return std::nullopt;
    std::vector<KeyEntry> entries;
    const std::size_t count = (std::min)(items->size(), max_keys);
    for (std::size_t index = 0; index < count; ++index) {
        const json& jwk = (*items)[index];
        if (!jwk.is_object() || !may_verify(jwk)) continue;
        const std::optional<KeyKind> kind = jwk_kind(jwk);
        if (!kind) continue;
        const std::optional<JwtAlgorithm> algorithm = algorithm_of(jwk, *kind, allowed);
        if (!algorithm) continue;
        KeyEntry entry;
        if (const json* kid = member(jwk, jwk_kid)) {
            const auto* text = kid->get_ptr<const json::string_t*>();
            if (text == nullptr) continue;
            entry.kid = *text;
        }
        Pkey key = *kind == KeyKind::rsa ? rsa_key(jwk) : ec_p256_key(jwk);
        if (!key) continue;
        entry.key = KeyMaker::make(*algorithm, std::move(key));
        entries.push_back(std::move(entry));
    }
    return KeySet(std::move(entries));
}

}  // namespace neomcp::http::detail
