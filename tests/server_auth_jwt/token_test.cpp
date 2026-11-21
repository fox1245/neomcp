// The reading of a token's parts, of the metadata and of a key set, from tables, without sockets: the compact form, the
// header's rules (`alg`, `typ`, `crit`), the claims and names that occur twice, the well-known URL of an issuer, and
// which keys of a set are used for which algorithm.
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "server_auth_jwt/fetch.h"
#include "server_auth_jwt/keys.h"
#include "server_auth_jwt/token.h"
#include "support.h"

namespace {

using namespace jwt_test;
using neomcp::http::detail::JoseHeader;
using neomcp::http::detail::metadata_url_of;
using neomcp::http::detail::read_claims;
using neomcp::http::detail::read_header;
using neomcp::http::detail::read_key_set;
using neomcp::http::detail::read_metadata_jwks_uri;
using neomcp::http::detail::split_compact;

const std::vector<JwtAlgorithm> all_algorithms{JwtAlgorithm::rs256, JwtAlgorithm::ps256, JwtAlgorithm::es256};
const std::vector<JwtAlgorithm> only_rs256{JwtAlgorithm::rs256};

std::optional<JwtEvent> header_refusal(const std::string& text, const std::vector<JwtAlgorithm>& allowed = all_algorithms) {
    const auto read = read_header(text, allowed);
    if (const auto* event = std::get_if<JwtEvent>(&read)) return *event;
    return std::nullopt;
}

std::optional<JwtEvent> claims_refusal(const std::string& text) {
    const auto read = read_claims(text, issuer);
    if (const auto* event = std::get_if<JwtEvent>(&read)) return *event;
    return std::nullopt;
}

TEST(JwtToken, TheCompactFormHasThreePartsOfStrictBase64url) {
    const std::string part = base64url("{}");
    EXPECT_TRUE(split_compact(part + "." + part + "." + base64url("sig")));
    EXPECT_TRUE(split_compact(part + "." + part + "."));  // an empty signature is a part; no key verifies it
    EXPECT_FALSE(split_compact(part + "." + part));
    EXPECT_FALSE(split_compact(part));
    // Five parts: an encrypted token, which an access token in this profile is not.
    EXPECT_FALSE(split_compact(part + "." + part + "." + part + "." + part + "." + part));
    EXPECT_FALSE(split_compact(part + "=." + part + "." + part));   // padding
    EXPECT_FALSE(split_compact("e30+." + part + "." + part));       // the standard alphabet
    EXPECT_FALSE(split_compact(part + " ." + part + "." + part));   // a blank
    // The signing input is a view into the token, which must outlive it.
    const std::string text = part + "." + base64url("{\"a\":1}") + "." + base64url("xy");
    const auto parts = split_compact(text);
    ASSERT_TRUE(parts);
    EXPECT_EQ(parts->signing_input, part + "." + base64url("{\"a\":1}"));
    EXPECT_EQ(parts->payload, "{\"a\":1}");
    EXPECT_EQ(parts->signature, "xy");
}

TEST(JwtToken, NoneAndHmacAreNeverAlgorithms) {
    EXPECT_EQ(header_refusal(R"({"alg":"none","typ":"at+jwt"})"), JwtEvent::algorithm_refused);
    EXPECT_EQ(header_refusal(R"({"alg":"None","typ":"at+jwt"})"), JwtEvent::algorithm_refused);
    for (const char* hmac : {"HS256", "HS384", "HS512"}) {
        EXPECT_EQ(header_refusal(std::string(R"({"typ":"at+jwt","alg":")") + hmac + "\"}"), JwtEvent::algorithm_refused)
            << hmac;
    }
    EXPECT_EQ(header_refusal(R"({"typ":"at+jwt"})"), JwtEvent::algorithm_refused);
    EXPECT_EQ(header_refusal(R"({"alg":256,"typ":"at+jwt"})"), JwtEvent::algorithm_refused);
    EXPECT_EQ(header_refusal(R"({"alg":"rs256","typ":"at+jwt"})"), JwtEvent::algorithm_refused);  // names are exact
}

TEST(JwtToken, TheAlgorithmComesFromTheListNotFromTheToken) {
    EXPECT_EQ(header_refusal(R"({"alg":"ES256","typ":"at+jwt"})", only_rs256), JwtEvent::algorithm_refused);
    EXPECT_EQ(header_refusal(R"({"alg":"PS256","typ":"at+jwt"})", only_rs256), JwtEvent::algorithm_refused);
    const auto read = read_header(R"({"alg":"RS256","typ":"at+jwt","kid":"k1"})", only_rs256);
    ASSERT_TRUE(std::holds_alternative<JoseHeader>(read));
    EXPECT_EQ(std::get<JoseHeader>(read).algorithm, JwtAlgorithm::rs256);
    EXPECT_EQ(std::get<JoseHeader>(read).kid, std::optional<std::string>("k1"));
}

TEST(JwtToken, TheTypeMustBeAnAccessTokens) {
    for (const char* accepted : {"at+jwt", "AT+JWT", "application/at+jwt", "Application/At+Jwt"}) {
        EXPECT_EQ(header_refusal(std::string(R"({"alg":"RS256","typ":")") + accepted + "\"}"), std::nullopt) << accepted;
    }
    for (const char* refused : {"JWT", "jwt", "application/jwt", "at+jwt; x=1", " at+jwt", "application/at+jwt/", ""}) {
        EXPECT_EQ(header_refusal(std::string(R"({"alg":"RS256","typ":")") + refused + "\"}"), JwtEvent::type_refused)
            << refused;
    }
    EXPECT_EQ(header_refusal(R"({"alg":"RS256"})"), JwtEvent::type_refused);
    EXPECT_EQ(header_refusal(R"({"alg":"RS256","typ":["at+jwt"]})"), JwtEvent::type_refused);
}

TEST(JwtToken, AnyCriticalExtensionIsRefused) {
    EXPECT_EQ(header_refusal(R"({"alg":"RS256","typ":"at+jwt","crit":["exp"],"exp":1})"), JwtEvent::critical_header);
    EXPECT_EQ(header_refusal(R"({"alg":"RS256","typ":"at+jwt","crit":["b64"],"b64":false})"), JwtEvent::critical_header);
    EXPECT_EQ(header_refusal(R"({"alg":"RS256","typ":"at+jwt","crit":[]})"), JwtEvent::critical_header);
}

TEST(JwtToken, KeyUrlsAndKeysInTheHeaderAreNotRead) {
    EXPECT_EQ(header_refusal(R"({"alg":"RS256","typ":"at+jwt","jku":"https://elsewhere.example/jwks",)"
                             R"("x5u":"https://elsewhere.example/cert","jwk":{"kty":"RSA"},"x5c":["AAAA"]})"),
              std::nullopt);
}

TEST(JwtToken, ANameTwiceInTheHeaderIsMalformed) {
    EXPECT_EQ(header_refusal(R"({"alg":"none","alg":"RS256","typ":"at+jwt"})"), JwtEvent::malformed);
    EXPECT_EQ(header_refusal(R"({"alg":"RS256","typ":"JWT","typ":"at+jwt"})"), JwtEvent::malformed);
    EXPECT_EQ(header_refusal(R"({"alg":"RS256","typ":"at+jwt","kid":7})"), JwtEvent::malformed);
    EXPECT_EQ(header_refusal(R"(["alg"])"), JwtEvent::malformed);
    EXPECT_EQ(header_refusal("not json"), JwtEvent::malformed);
}

TEST(JwtToken, TheClaimsGiveTheAuthorization) {
    json claims = good_claims();
    claims["nbf"] = 1700000000;
    claims["roles"] = json::array({"reader"});
    const auto read = read_claims(claims.dump(), issuer);
    ASSERT_TRUE(std::holds_alternative<neomcp::Authorization>(read));
    const auto& granted = std::get<neomcp::Authorization>(read);
    EXPECT_EQ(granted.issuer, issuer);
    EXPECT_EQ(granted.subject, std::optional<std::string>("alice"));
    EXPECT_EQ(granted.client_id, std::optional<std::string>("client-1"));
    EXPECT_EQ(granted.audiences, (std::vector<std::string>{resource}));
    EXPECT_EQ(granted.scopes, (std::vector<std::string>{"mcp:use", "files:read"}));
    ASSERT_TRUE(granted.expires);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(granted.expires->time_since_epoch()).count(),
              claims["exp"].get<long long>());
    ASSERT_TRUE(granted.not_before);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(granted.not_before->time_since_epoch()).count(),
              1700000000);
    // The claims the typed members hold are not repeated; the others are kept for handlers.
    EXPECT_EQ(granted.claims, (json{{"iat", claims["iat"]}, {"jti", "id-1"}, {"roles", json::array({"reader"})}}));
    json audiences = good_claims();
    audiences["aud"] = json::array({"https://other.example", resource});
    const auto both = read_claims(audiences.dump(), issuer);
    ASSERT_TRUE(std::holds_alternative<neomcp::Authorization>(both));
    EXPECT_EQ(std::get<neomcp::Authorization>(both).audiences,
              (std::vector<std::string>{"https://other.example", resource}));
}

TEST(JwtToken, ClaimsThatCannotBeReadAreRefused) {
    const auto without = [](const char* name) {
        json claims = good_claims();
        claims.erase(name);
        return claims.dump();
    };
    const auto with = [](const char* name, json value) {
        json claims = good_claims();
        claims[name] = std::move(value);
        return claims.dump();
    };
    EXPECT_EQ(claims_refusal(without("iss")), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(without("exp")), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(without("aud")), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(without("sub")), std::nullopt);
    EXPECT_EQ(claims_refusal(without("scope")), std::nullopt);
    EXPECT_EQ(claims_refusal(with("iss", "https://other-auth.example")), JwtEvent::issuer_mismatch);
    EXPECT_EQ(claims_refusal(with("iss", "https://auth.example/")), JwtEvent::issuer_mismatch);
    EXPECT_EQ(claims_refusal(with("exp", "4102444800")), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(with("nbf", true)), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(with("aud", 7)), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(with("aud", json::array({resource, 7}))), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(with("sub", 7)), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(with("scope", json::array({"mcp:use"}))), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(with("scope", "mcp:use \"x\"")), JwtEvent::malformed);
    EXPECT_EQ(claims_refusal("[]"), JwtEvent::malformed);
}

TEST(JwtToken, ANameTwiceInTheClaimsIsMalformed) {
    const std::string tail = R"("sub":"alice","exp":4102444800})";
    EXPECT_EQ(claims_refusal(R"({"iss":"https://auth.example","aud":"x","aud":")" + std::string(resource) + "\"," + tail),
              JwtEvent::malformed);
    EXPECT_EQ(claims_refusal(R"({"iss":"https://other.example","iss":"https://auth.example","aud":")" +
                             std::string(resource) + "\"," + tail),
              JwtEvent::malformed);
    // Anywhere in the claims, not only at the top.
    EXPECT_EQ(claims_refusal(R"({"iss":"https://auth.example","aud":")" + std::string(resource) +
                             R"(","cnf":{"jkt":"a","jkt":"b"},)" + tail),
              JwtEvent::malformed);
    // The same name in two objects is no repetition.
    EXPECT_EQ(claims_refusal(R"({"iss":"https://auth.example","aud":")" + std::string(resource) +
                             R"(","a":{"x":1},"b":{"x":2},)" + tail),
              std::nullopt);
}

TEST(JwtToken, TheMetadataUrlOfAnIssuer) {
    EXPECT_EQ(metadata_url_of("https://auth.example"), "https://auth.example/.well-known/oauth-authorization-server");
    EXPECT_EQ(metadata_url_of("https://auth.example/"), "https://auth.example/.well-known/oauth-authorization-server");
    EXPECT_EQ(metadata_url_of("https://auth.example:8443/tenant/one/"),
              "https://auth.example:8443/.well-known/oauth-authorization-server/tenant/one");
    EXPECT_EQ(metadata_url_of("http://127.0.0.1:9/realm"),
              "http://127.0.0.1:9/.well-known/oauth-authorization-server/realm");
    EXPECT_EQ(metadata_url_of("http://auth.example"), std::nullopt);
    EXPECT_EQ(metadata_url_of("https://auth.example/?tenant=1"), std::nullopt);
    EXPECT_EQ(metadata_url_of("https://auth.example/#x"), std::nullopt);
    EXPECT_EQ(metadata_url_of("https://user@auth.example"), std::nullopt);
    EXPECT_EQ(metadata_url_of("auth.example"), std::nullopt);
}

TEST(JwtToken, TheMetadataMustNameTheIssuer) {
    EXPECT_EQ(read_metadata_jwks_uri(R"({"issuer":"https://auth.example","jwks_uri":"https://auth.example/k"})", issuer),
              "https://auth.example/k");
    EXPECT_EQ(read_metadata_jwks_uri(R"({"issuer":"https://auth.example/","jwks_uri":"https://auth.example/k"})", issuer),
              std::nullopt);
    EXPECT_EQ(read_metadata_jwks_uri(R"({"jwks_uri":"https://auth.example/k"})", issuer), std::nullopt);
    EXPECT_EQ(read_metadata_jwks_uri(R"({"issuer":"https://auth.example"})", issuer), std::nullopt);
    EXPECT_EQ(read_metadata_jwks_uri(
                  R"({"issuer":"https://auth.example","jwks_uri":"https://a.example/k","jwks_uri":"https://b.example/k"})",
                  issuer),
              std::nullopt);
}

std::size_t keys_read(const json& set, const std::vector<JwtAlgorithm>& allowed, std::size_t max_keys = 64) {
    const auto read = read_key_set(set.dump(), allowed, max_keys);
    return read ? read->size() : static_cast<std::size_t>(-1);
}

TEST(JwtKeySet, EachKeyServesOneAlgorithm) {
    // A key with its own `alg` serves that one, when the list holds it.
    EXPECT_EQ(keys_read(key_set({rsa_key(0).jwk("a", "RS256")}), all_algorithms), 1U);
    EXPECT_EQ(keys_read(key_set({rsa_key(0).jwk("a", "PS256")}), only_rs256), 0U);
    EXPECT_EQ(keys_read(key_set({rsa_key(0).jwk("a", "ES256")}), all_algorithms), 0U);  // not an RSA algorithm
    EXPECT_EQ(keys_read(key_set({rsa_key(0).jwk("a", "HS256")}), all_algorithms), 0U);
    // Without `alg`, a key serves the one listed algorithm of its type, and none when two fit.
    EXPECT_EQ(keys_read(key_set({rsa_key(0).jwk("a")}), only_rs256), 1U);
    EXPECT_EQ(keys_read(key_set({rsa_key(0).jwk("a")}), all_algorithms), 0U);
    EXPECT_EQ(keys_read(key_set({ec_key(0).jwk("e")}), all_algorithms), 1U);
    EXPECT_EQ(keys_read(key_set({ec_key(0).jwk("e")}), only_rs256), 0U);
    const auto set = read_key_set(key_set({rsa_key(0).jwk("a", "RS256"), rsa_key(1).jwk("b", "PS256")}).dump(),
                                  all_algorithms, 64);
    ASSERT_TRUE(set);
    EXPECT_EQ(set->candidates(JwtAlgorithm::rs256, std::string("a")).size(), 1U);
    EXPECT_EQ(set->candidates(JwtAlgorithm::ps256, std::string("a")).size(), 0U);
    EXPECT_EQ(set->candidates(JwtAlgorithm::ps256, std::nullopt).size(), 1U);
    EXPECT_EQ(set->candidates(JwtAlgorithm::rs256, std::string("c")).size(), 0U);
}

TEST(JwtKeySet, KeysThatCannotServeAreLeftOut) {
    json encryption = rsa_key(0).jwk("a", "RS256");
    encryption["use"] = "enc";
    json signing_only = rsa_key(0).jwk("b", "RS256");
    signing_only["key_ops"] = json::array({"sign"});
    json verifying = rsa_key(0).jwk("c", "RS256");
    verifying["key_ops"] = json::array({"verify"});
    verifying["use"] = "sig";
    json symmetric = json{{"kty", "oct"}, {"k", base64url("secret")}, {"kid", "d"}, {"alg", "HS256"}};
    json off_curve = ec_key(0).jwk("e", "ES256");
    off_curve["y"] = base64url(std::string(32, '\x01'));
    json short_coordinate = ec_key(0).jwk("f", "ES256");
    short_coordinate["x"] = base64url(std::string(31, '\x01'));
    json bad_text = rsa_key(0).jwk("g", "RS256");
    bad_text["n"] = "not+base64url";
    json numeric_kid = rsa_key(0).jwk("h", "RS256");
    numeric_kid["kid"] = 8;
    EXPECT_EQ(keys_read(key_set({encryption, signing_only, verifying, symmetric, off_curve, short_coordinate, bad_text,
                                 numeric_kid, short_rsa_key().jwk("i", "RS256"), json::array()}),
                        all_algorithms),
              1U);
}

TEST(JwtKeySet, KeysAfterTheBoundAreIgnored) {
    const json set = key_set({rsa_key(0).jwk("a", "RS256"), rsa_key(1).jwk("b", "RS256"), ec_key(0).jwk("c")});
    EXPECT_EQ(keys_read(set, all_algorithms, 3), 3U);
    EXPECT_EQ(keys_read(set, all_algorithms, 2), 2U);
    EXPECT_EQ(keys_read(set, all_algorithms, 1), 1U);
}

TEST(JwtKeySet, ADocumentThatIsNoKeySetIsRefused) {
    EXPECT_FALSE(read_key_set("[]", all_algorithms, 64));
    EXPECT_FALSE(read_key_set(R"({"keys":{}})", all_algorithms, 64));
    EXPECT_FALSE(read_key_set(R"({"key":[]})", all_algorithms, 64));
    EXPECT_TRUE(read_key_set(R"({"keys":[]})", all_algorithms, 64));
    // A name twice, in the set or in one of its keys.
    EXPECT_FALSE(read_key_set(R"({"keys":[],"keys":[]})", all_algorithms, 64));
    const json key = rsa_key(0).jwk("a", "RS256");
    const std::string twice = R"({"keys":[{"kty":"RSA","kid":"a","alg":"RS256","n":")" + key["n"].get<std::string>() +
                              R"(","n":")" + rsa_key(1).jwk("a")["n"].get<std::string>() + R"(","e":"AQAB"}]})";
    EXPECT_FALSE(read_key_set(twice, all_algorithms, 64));
}

}  // namespace
