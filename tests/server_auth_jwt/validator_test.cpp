// The validator against a scripted key set endpoint on a loopback port: signatures by each algorithm, the tokens it
// must refuse (`alg` none, HMAC with the public key as the secret, an algorithm outside the list, another key, a type
// that is not an access token's, a critical extension, key URLs and keys in the header, names twice), the key set's
// fetches and their bound, a key set too large or too slow, the authorization server's metadata, and the bounds on
// waiting tokens.
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "scripted_http_server.h"
#include "support.h"

namespace {

using namespace jwt_test;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::http::JwtOptions;

constexpr const char* metadata_path = "/.well-known/oauth-authorization-server";

// A key set endpoint whose set the test changes.
class KeySetServer {
public:
    explicit KeySetServer(json set)
        : state_(std::make_shared<Shared>()), server_([state = state_](const Request& request) {
              std::lock_guard lock(state->mutex);
              if (state->gate) {
                  http_test::Reply reply;
                  reply.wait(state->gate);
                  for (auto& step : http_test::json_reply(200, state->set).steps) reply.steps.push_back(std::move(step));
                  return reply;
              }
              if (request.target == metadata_path && state->metadata) return http_test::json_reply(200, *state->metadata);
              if (request.target != jwks_path) return http_test::empty_reply(404);
              return http_test::json_reply(state->status, state->set);
          }) {
        serve(std::move(set));
    }

    void serve(json set, int status = 200) {
        std::lock_guard lock(state_->mutex);
        state_->set = std::move(set);
        state_->status = status;
    }
    void serve_metadata(json metadata) {
        std::lock_guard lock(state_->mutex);
        state_->metadata = std::move(metadata);
    }
    // Every reply from now on waits at `gate`.
    void hold(std::shared_ptr<http_test::Gate> gate) {
        std::lock_guard lock(state_->mutex);
        state_->gate = std::move(gate);
    }

    std::string url(const char* path = jwks_path) const { return server_.url(path); }
    std::string origin() const { return server_.url(""); }
    std::size_t count() const { return server_.count(); }
    ScriptedHttpServer& scripted() { return server_; }

private:
    struct Shared {
        std::mutex mutex;
        json set;
        int status = 200;
        std::optional<json> metadata;
        std::shared_ptr<http_test::Gate> gate;
    };
    std::shared_ptr<Shared> state_;
    ScriptedHttpServer server_;
};

const std::vector<JwtAlgorithm> all_algorithms{JwtAlgorithm::rs256, JwtAlgorithm::ps256, JwtAlgorithm::es256};

// The set most tests use: an RS256 key "rs", a PS256 key "ps" and an ES256 key "es".
json standard_set() {
    return key_set({rsa_key(0).jwk("rs", "RS256"), rsa_key(1).jwk("ps", "PS256"), ec_key(0).jwk("es", "ES256")});
}

struct Rig {
    explicit Rig(json set = standard_set(), std::vector<JwtAlgorithm> algorithms = all_algorithms)
        : keys(std::move(set)) {
        options = options_for(keys.url(), log);
        options.algorithms = std::move(algorithms);
        options.clock = [moving = clock] { return moving->now(); };
    }
    std::shared_ptr<neomcp::http::TokenValidator> start() { return validator = make(options); }

    std::shared_ptr<EventLog> log = std::make_shared<EventLog>();
    std::shared_ptr<TestClock> clock = std::make_shared<TestClock>();
    KeySetServer keys;
    JwtOptions options;
    std::shared_ptr<neomcp::http::TokenValidator> validator;
};

std::string good_token(JwtAlgorithm algorithm, const char* kid, const TestKey& key) {
    return token(good_header(algorithm, std::string(kid)), good_claims(), key, algorithm);
}

TEST(JwtValidator, SignaturesOfEachAllowedAlgorithmAreAccepted) {
    Rig rig;
    auto validator = rig.start();
    const struct {
        JwtAlgorithm algorithm;
        const char* kid;
        const TestKey& key;
    } cases[] = {{JwtAlgorithm::rs256, "rs", rsa_key(0)}, {JwtAlgorithm::ps256, "ps", rsa_key(1)},
                 {JwtAlgorithm::es256, "es", ec_key(0)}};
    for (const auto& c : cases) {
        const TokenVerdict verdict = ask(*validator, good_token(c.algorithm, c.kid, c.key));
        const neomcp::Authorization* granted = authorization_of(verdict);
        ASSERT_NE(granted, nullptr) << neomcp::http::to_string(c.algorithm);
        EXPECT_EQ(granted->issuer, issuer);
        EXPECT_EQ(granted->subject, std::optional<std::string>("alice"));
        EXPECT_EQ(granted->audiences, (std::vector<std::string>{resource}));
        EXPECT_EQ(rig.log->last_verdict(), JwtEvent::accepted);
    }
    // One fetch served every token.
    EXPECT_EQ(rig.keys.count(), 1U);
    EXPECT_EQ(rig.log->count(JwtEvent::key_set_fetched), 1U);
}

TEST(JwtValidator, ATokenWithoutKidIsCheckedWithTheKeysOfItsAlgorithm) {
    Rig rig;
    auto validator = rig.start();
    EXPECT_NE(authorization_of(ask(*validator, token(good_header(JwtAlgorithm::es256, std::nullopt), good_claims(),
                                                     ec_key(0), JwtAlgorithm::es256))),
              nullptr);
    EXPECT_TRUE(is_refusal(ask(*validator, token(good_header(JwtAlgorithm::es256, std::nullopt), good_claims(),
                                                 ec_key(1), JwtAlgorithm::es256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::signature_failed);
}

TEST(JwtValidator, AlgNoneIsRefused) {
    Rig rig;
    auto validator = rig.start();
    const std::string input = base64url(R"({"alg":"none","typ":"at+jwt","kid":"rs"})") + "." +
                              base64url(good_claims().dump());
    for (const std::string& signature : {std::string(), base64url(rsa_key(0).sign(JwtAlgorithm::rs256, input))}) {
        EXPECT_TRUE(is_refusal(ask(*validator, input + "." + signature), TokenRefusal::invalid));
        EXPECT_EQ(rig.log->last_verdict(), JwtEvent::algorithm_refused);
    }
}

TEST(JwtValidator, Hs256WithThePublicKeyAsTheSecretIsRefused) {
    Rig rig(key_set({rsa_key(0).jwk("rs", "RS256")}), {JwtAlgorithm::rs256});
    auto validator = rig.start();
    const json jwk = rsa_key(0).jwk("rs", "RS256");
    const std::string input = base64url(R"({"alg":"HS256","typ":"at+jwt","kid":"rs"})") + "." +
                              base64url(good_claims().dump());
    // Every form of the public key a server might be led to take as the HMAC secret.
    for (const std::string& secret : {rsa_key(0).public_pem(), rsa_key(0).public_der(), jwk.dump(),
                                      jwk["n"].get<std::string>()}) {
        EXPECT_TRUE(is_refusal(ask(*validator, input + "." + base64url(hmac_sha256(secret, input))),
                               TokenRefusal::invalid));
        EXPECT_EQ(rig.log->last_verdict(), JwtEvent::algorithm_refused);
    }
}

TEST(JwtValidator, AnAlgorithmOutsideTheListIsRefusedEvenWithAKeyForIt) {
    // The set holds an ES256 key and an RSA key without `alg`; only RS256 is allowed.
    Rig rig(key_set({ec_key(0).jwk("es", "ES256"), rsa_key(0).jwk("rs")}), {JwtAlgorithm::rs256});
    auto validator = rig.start();
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::es256, "es", ec_key(0))), TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::algorithm_refused);
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::ps256, "rs", rsa_key(0))), TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::algorithm_refused);
    EXPECT_NE(authorization_of(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0)))), nullptr);
}

TEST(JwtValidator, AKeyServesOnlyItsOwnAlgorithm) {
    // "rs" is an RS256 key: a PS256 signature by the same key pair is not accepted with it.
    Rig rig(key_set({rsa_key(0).jwk("rs", "RS256")}));
    auto validator = rig.start();
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::ps256, "rs", rsa_key(0))), TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::unknown_key);
}

TEST(JwtValidator, ATokenSignedByAnotherKeyIsRefused) {
    Rig rig;
    auto validator = rig.start();
    // The kid of a key of the set, the signature of another key.
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(1))), TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::signature_failed);
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::es256, "es", ec_key(1))), TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::signature_failed);
    // A good token with one byte of its claims changed after signing.
    std::string changed = good_token(JwtAlgorithm::rs256, "rs", rsa_key(0));
    const auto dot = changed.find('.');
    changed[dot + 2] = changed[dot + 2] == 'A' ? 'B' : 'A';
    EXPECT_TRUE(is_refusal(ask(*validator, changed), TokenRefusal::invalid));
    // A failed signature asks for no new key set.
    EXPECT_EQ(rig.keys.count(), 1U);
}

TEST(JwtValidator, TheTypeMustBeAnAccessTokens) {
    Rig rig;
    auto validator = rig.start();
    for (const char* type : {"JWT", "application/jwt", "id_token+jwt"}) {
        json header = good_header(JwtAlgorithm::rs256, std::string("rs"));
        header["typ"] = type;
        EXPECT_TRUE(is_refusal(ask(*validator, token(header, good_claims(), rsa_key(0), JwtAlgorithm::rs256)),
                               TokenRefusal::invalid))
            << type;
        EXPECT_EQ(rig.log->last_verdict(), JwtEvent::type_refused) << type;
    }
    json untyped = good_header(JwtAlgorithm::rs256, std::string("rs"));
    untyped.erase("typ");
    EXPECT_TRUE(is_refusal(ask(*validator, token(untyped, good_claims(), rsa_key(0), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::type_refused);
    for (const char* type : {"AT+JWT", "application/at+jwt"}) {
        json header = good_header(JwtAlgorithm::rs256, std::string("rs"));
        header["typ"] = type;
        EXPECT_NE(authorization_of(ask(*validator, token(header, good_claims(), rsa_key(0), JwtAlgorithm::rs256))),
                  nullptr)
            << type;
    }
}

TEST(JwtValidator, ACriticalExtensionIsRefused) {
    Rig rig;
    auto validator = rig.start();
    json header = good_header(JwtAlgorithm::rs256, std::string("rs"));
    header["crit"] = json::array({"b64"});
    header["b64"] = false;
    EXPECT_TRUE(is_refusal(ask(*validator, token(header, good_claims(), rsa_key(0), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::critical_header);
}

TEST(JwtValidator, KeyUrlsAndKeysInTheHeaderAreIgnored) {
    Rig rig;
    // Another endpoint that would serve another key under the token's kid.
    KeySetServer elsewhere(key_set({rsa_key(1).jwk("other", "RS256")}));
    auto validator = rig.start();
    json pointing = good_header(JwtAlgorithm::rs256, std::string("other"));
    pointing["jku"] = elsewhere.url();
    pointing["x5u"] = elsewhere.url();
    EXPECT_TRUE(is_refusal(ask(*validator, token(pointing, good_claims(), rsa_key(1), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::unknown_key);
    EXPECT_EQ(elsewhere.count(), 0U);
    // The kid of a key of the set, another public key in `jwk`, and the signature of that other key.
    json embedding = good_header(JwtAlgorithm::rs256, std::string("rs"));
    embedding["jwk"] = rsa_key(1).jwk("rs", "RS256");
    EXPECT_TRUE(is_refusal(ask(*validator, token(embedding, good_claims(), rsa_key(1), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::signature_failed);
    // The same headers on a token the set's key signed change nothing.
    json ignored = good_header(JwtAlgorithm::rs256, std::string("rs"));
    ignored["jku"] = elsewhere.url();
    ignored["x5u"] = elsewhere.url();
    ignored["jwk"] = rsa_key(1).jwk("rs", "RS256");
    EXPECT_NE(authorization_of(ask(*validator, token(ignored, good_claims(), rsa_key(0), JwtAlgorithm::rs256))),
              nullptr);
    EXPECT_EQ(elsewhere.count(), 0U);
}

TEST(JwtValidator, NamesTwiceAreRefused) {
    Rig rig;
    auto validator = rig.start();
    const std::string claims = good_claims().dump();
    const std::string header_twice = R"({"alg":"RS256","typ":"at+jwt","kid":"ps","kid":"rs"})";
    EXPECT_TRUE(is_refusal(ask(*validator, token_from_text(header_twice, claims, rsa_key(0), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::malformed);
    const std::string header = good_header(JwtAlgorithm::rs256, std::string("rs")).dump();
    // The audience twice: the first is another resource's, the last this one's.
    std::string audience_twice = claims;
    audience_twice.insert(1, R"("aud":"https://elsewhere.example/mcp",)");
    EXPECT_TRUE(is_refusal(ask(*validator, token_from_text(header, audience_twice, rsa_key(0), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::malformed);
    std::string issuer_twice = claims;
    issuer_twice.insert(1, R"("iss":"https://other-auth.example",)");
    EXPECT_TRUE(is_refusal(ask(*validator, token_from_text(header, issuer_twice, rsa_key(0), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::malformed);
}

TEST(JwtValidator, TheIssuerIsTheConfiguredOne) {
    Rig rig;
    auto validator = rig.start();
    json claims = good_claims();
    claims["iss"] = "https://other-auth.example";
    EXPECT_TRUE(is_refusal(ask(*validator, token(good_header(JwtAlgorithm::rs256, std::string("rs")), claims,
                                                 rsa_key(0), JwtAlgorithm::rs256)),
                           TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::issuer_mismatch);
}

TEST(JwtValidator, MalformedTokensAskForNoKeySet) {
    Rig rig;
    auto validator = rig.start();
    for (const std::string& text : {std::string("opaque-token"), std::string("a.b"), std::string("a.b.c.d.e"),
                                    base64url("{}") + "." + base64url("{}") + ".",
                                    base64url("not json") + "." + base64url("{}") + "." + base64url("x")}) {
        EXPECT_TRUE(is_refusal(ask(*validator, text), TokenRefusal::invalid)) << text;
    }
    EXPECT_EQ(rig.keys.count(), 0U);
}

TEST(JwtValidator, AnUnknownKidFetchesAtMostOncePerInterval) {
    Rig rig(key_set({rsa_key(0).jwk("a", "RS256")}));
    auto validator = rig.start();
    EXPECT_NE(authorization_of(ask(*validator, good_token(JwtAlgorithm::rs256, "a", rsa_key(0)))), nullptr);
    EXPECT_EQ(rig.keys.count(), 1U);
    // The authorization server adds key "b" to its set; a token with it arrives at once.
    rig.keys.serve(key_set({rsa_key(0).jwk("a", "RS256"), rsa_key(1).jwk("b", "RS256")}));
    const std::string with_b = good_token(JwtAlgorithm::rs256, "b", rsa_key(1));
    EXPECT_TRUE(is_refusal(ask(*validator, with_b), TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::unknown_key);
    EXPECT_EQ(rig.keys.count(), 1U);
    // Just before the bound: still no fetch.
    rig.clock->advance(rig.options.bounds.min_key_refresh_interval - 1ms);
    EXPECT_TRUE(is_refusal(ask(*validator, with_b), TokenRefusal::invalid));
    EXPECT_EQ(rig.keys.count(), 1U);
    // At the bound: one fetch, which finds the key.
    rig.clock->advance(1ms);
    EXPECT_NE(authorization_of(ask(*validator, with_b)), nullptr);
    EXPECT_EQ(rig.keys.count(), 2U);
    // A kid that is in no set: refused, and the fetch just made counts.
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "c", rsa_key(1))), TokenRefusal::invalid));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::unknown_key);
    EXPECT_EQ(rig.keys.count(), 2U);
}

TEST(JwtValidator, AFetchThatFindsNoKeyStillCountsAndRefuses) {
    Rig rig(key_set({rsa_key(0).jwk("a", "RS256")}));
    auto validator = rig.start();
    EXPECT_NE(authorization_of(ask(*validator, good_token(JwtAlgorithm::rs256, "a", rsa_key(0)))), nullptr);
    rig.clock->advance(rig.options.bounds.min_key_refresh_interval);
    for (int round = 0; round < 3; ++round) {
        EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "z", rsa_key(1))), TokenRefusal::invalid));
        EXPECT_EQ(rig.log->last_verdict(), JwtEvent::unknown_key);
    }
    EXPECT_EQ(rig.keys.count(), 2U);
}

TEST(JwtValidator, TokensThatWaitForTheSameFetchShareIt) {
    Rig rig;
    auto gate = std::make_shared<http_test::Gate>();
    rig.keys.hold(gate);
    auto validator = rig.start();
    // validate() returns at once for a token that waits, so the three are waiting before the set comes.
    std::vector<std::future<TokenVerdict>> verdicts;
    verdicts.push_back(validate_later(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0))));
    verdicts.push_back(validate_later(*validator, good_token(JwtAlgorithm::es256, "es", ec_key(0))));
    verdicts.push_back(validate_later(*validator, good_token(JwtAlgorithm::ps256, "ps", rsa_key(1))));
    ASSERT_TRUE(rig.keys.scripted().wait_for(1));
    for (auto& verdict : verdicts) EXPECT_EQ(verdict.wait_for(0ms), std::future_status::timeout);
    gate->open();
    for (auto& verdict : verdicts) {
        ASSERT_EQ(verdict.wait_for(20s), std::future_status::ready);
        EXPECT_NE(authorization_of(verdict.get()), nullptr);
    }
    EXPECT_EQ(rig.keys.count(), 1U);
}

TEST(JwtValidator, AnOldKeySetIsFetchedAgainWhileItStillAnswers) {
    Rig rig(key_set({rsa_key(0).jwk("a", "RS256")}));
    auto validator = rig.start();
    const std::string with_a = good_token(JwtAlgorithm::rs256, "a", rsa_key(0));
    EXPECT_NE(authorization_of(ask(*validator, with_a)), nullptr);
    // The authorization server retires key "a".
    rig.keys.serve(key_set({rsa_key(1).jwk("b", "RS256")}));
    rig.clock->advance(rig.options.bounds.max_key_set_age - 1ms);
    EXPECT_NE(authorization_of(ask(*validator, with_a)), nullptr);
    EXPECT_EQ(rig.keys.count(), 1U);
    rig.clock->advance(1ms);
    // This token is still decided with the set held; the new set is fetched meanwhile.
    EXPECT_NE(authorization_of(ask(*validator, with_a)), nullptr);
    ASSERT_TRUE(rig.log->wait_for(JwtEvent::key_set_fetched, 2));
    EXPECT_TRUE(is_refusal(ask(*validator, with_a), TokenRefusal::invalid));
    EXPECT_NE(authorization_of(ask(*validator, good_token(JwtAlgorithm::rs256, "b", rsa_key(1)))), nullptr);
    EXPECT_EQ(rig.keys.count(), 2U);
}

TEST(JwtValidator, AKeySetOverItsBoundIsUnavailable) {
    const json set = standard_set();
    const std::size_t size = set.dump().size();
    {
        Rig rig(set);
        rig.options.bounds.max_jwks_bytes = size;
        auto validator = rig.start();
        EXPECT_NE(authorization_of(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0)))), nullptr);
    }
    Rig rig(set);
    rig.options.bounds.max_jwks_bytes = size - 1;
    auto validator = rig.start();
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0))), TokenRefusal::unavailable));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::key_set_unavailable);
    EXPECT_EQ(rig.log->count(JwtEvent::key_set_fetch_failed), 1U);
    // Within the bound on fetches, the next token is answered without one.
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0))), TokenRefusal::unavailable));
    EXPECT_EQ(rig.keys.count(), 1U);
}

TEST(JwtValidator, ASlowKeySetIsUnavailable) {
    Rig rig;
    auto never = std::make_shared<http_test::Gate>();
    rig.keys.hold(never);
    rig.options.bounds.request_timeout = 300ms;
    rig.options.bounds.connect_timeout = 300ms;
    auto validator = rig.start();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0))), TokenRefusal::unavailable));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::key_set_unavailable);
    never->open();
}

TEST(JwtValidator, AKeySetThatCannotBeUsedIsUnavailable) {
    for (const auto& [body, status] : {std::pair{standard_set(), 500}, std::pair{json::array(), 200},
                                       std::pair{json{{"keys", "none"}}, 200}}) {
        Rig rig(body);
        rig.keys.serve(body, status);
        auto validator = rig.start();
        EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0))),
                               TokenRefusal::unavailable))
            << status;
    }
    // Nothing listens.
    std::string gone;
    {
        KeySetServer stopped(standard_set());
        gone = stopped.url();
    }
    auto log = std::make_shared<EventLog>();
    auto validator = make(options_for(gone, log));
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0))), TokenRefusal::unavailable));
}

TEST(JwtValidator, AFailedRefreshKeepsTheSetItHas) {
    Rig rig(key_set({rsa_key(0).jwk("a", "RS256")}));
    auto validator = rig.start();
    const std::string with_a = good_token(JwtAlgorithm::rs256, "a", rsa_key(0));
    EXPECT_NE(authorization_of(ask(*validator, with_a)), nullptr);
    rig.keys.serve(json::object(), 503);
    rig.clock->advance(rig.options.bounds.min_key_refresh_interval);
    // A token with a key the set lacks cannot be decided now; one with a key it holds still can.
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::rs256, "b", rsa_key(1))), TokenRefusal::unavailable));
    EXPECT_NE(authorization_of(ask(*validator, with_a)), nullptr);
}

TEST(JwtValidator, TheKeySetUrlComesFromTheMetadata) {
    KeySetServer server(standard_set());
    const std::string own_issuer = server.origin();
    server.serve_metadata(json{{"issuer", own_issuer}, {"jwks_uri", server.url()}});
    auto log = std::make_shared<EventLog>();
    JwtOptions options = options_for("", log);
    options.issuer = own_issuer;
    options.algorithms = all_algorithms;
    auto validator = make(options);
    json claims = good_claims();
    claims["iss"] = own_issuer;
    EXPECT_NE(authorization_of(ask(*validator, token(good_header(JwtAlgorithm::es256, std::string("es")), claims,
                                                     ec_key(0), JwtAlgorithm::es256))),
              nullptr);
    const auto requests = server.scripted().requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].target, metadata_path);
    EXPECT_EQ(requests[1].target, jwks_path);
}

TEST(JwtValidator, MetadataOfAnotherIssuerGivesNoKeys) {
    KeySetServer server(standard_set());
    const std::string own_issuer = server.origin();
    server.serve_metadata(json{{"issuer", "https://auth.example"}, {"jwks_uri", server.url()}});
    auto log = std::make_shared<EventLog>();
    JwtOptions options = options_for("", log);
    options.issuer = own_issuer;
    auto validator = make(options);
    json claims = good_claims();
    claims["iss"] = own_issuer;
    EXPECT_TRUE(is_refusal(ask(*validator, token(good_header(JwtAlgorithm::rs256, std::string("rs")), claims,
                                                 rsa_key(0), JwtAlgorithm::rs256)),
                           TokenRefusal::unavailable));
    EXPECT_EQ(server.count(), 1U);
}

TEST(JwtValidator, TooManyWaitingTokensAreUnavailable) {
    Rig rig;
    auto gate = std::make_shared<http_test::Gate>();
    rig.keys.hold(gate);
    rig.options.bounds.max_pending = 1;
    auto validator = rig.start();
    auto waiting = validate_later(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0)));
    EXPECT_TRUE(is_refusal(ask(*validator, good_token(JwtAlgorithm::es256, "es", ec_key(0))), TokenRefusal::unavailable));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::overloaded);
    gate->open();
    ASSERT_EQ(waiting.wait_for(20s), std::future_status::ready);
    EXPECT_NE(authorization_of(waiting.get()), nullptr);
}

TEST(JwtValidator, DroppingTheValidatorAnswersTheWaitingTokens) {
    Rig rig;
    auto gate = std::make_shared<http_test::Gate>();
    rig.keys.hold(gate);
    auto validator = rig.start();
    rig.validator.reset();
    auto verdict = validate_later(*validator, good_token(JwtAlgorithm::rs256, "rs", rsa_key(0)));
    ASSERT_TRUE(rig.keys.scripted().wait_for(1));
    validator.reset();
    ASSERT_EQ(verdict.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(is_refusal(verdict.get(), TokenRefusal::unavailable));
    EXPECT_EQ(rig.log->last_verdict(), JwtEvent::stopped);
    gate->open();
}

TEST(JwtValidator, OptionsAreCheckedWithoutNamingTheirUrls) {
    auto log = std::make_shared<EventLog>();
    const auto refused = [](JwtOptions options) {
        auto made = neomcp::http::make_jwt_validator(std::move(options));
        return made ? std::string() : made.error;
    };
    JwtOptions plain = options_for("http://keys.example/secret-path", log);
    const std::string problem = refused(plain);
    EXPECT_FALSE(problem.empty());
    EXPECT_EQ(problem.find("secret-path"), std::string::npos) << problem;
    EXPECT_EQ(problem.find("keys.example"), std::string::npos) << problem;
    JwtOptions empty_list = options_for("https://keys.example/jwks", log);
    empty_list.algorithms.clear();
    EXPECT_FALSE(refused(empty_list).empty());
    JwtOptions no_issuer = options_for("https://keys.example/jwks", log);
    no_issuer.issuer.clear();
    EXPECT_FALSE(refused(no_issuer).empty());
    JwtOptions issuer_not_a_url = options_for("", log);
    issuer_not_a_url.issuer = "auth.example";
    EXPECT_FALSE(refused(issuer_not_a_url).empty());
    JwtOptions young = options_for("https://keys.example/jwks", log);
    young.bounds.max_key_set_age = young.bounds.min_key_refresh_interval - 1ms;
    EXPECT_FALSE(refused(young).empty());
    EXPECT_TRUE(refused(options_for("https://keys.example/jwks", log)).empty());
}

}  // namespace
