// The first contact, and a step-up for a tool's scope, against the library's own protected server when it checks signed
// tokens itself with the authorization server's key set (the JWT validator, RFC 9068). The scripted authorization
// server signs the tokens it issues and serves the key set; the keys are made when the tests run. Built where the JWT
// validator is (NEOMCP_JWT). This program links the JWT validator only, as no program of the tests links both
// validators.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../server_auth_jwt/support.h"
#include "end_to_end_server_rig.h"
#include "neomcp/http/jwt.h"

namespace {

using namespace server_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;

constexpr const char* key_id = "end-to-end-key";

// The tokens of the scripted authorization server are signed with `signing` (a key of the test's own making); the key
// set it serves, and names in its metadata, holds `published`.
void serve_signed_tokens(ServerWorld& world, const jwt_test::TestKey& signing, const jwt_test::TestKey& published) {
    const std::string issuer = world.as.issuer();
    world.as.edit_metadata([issuer](json& metadata) { metadata["jwks_uri"] = issuer + "/jwks"; });
    world.as.mint_with([&signing, issuer](const oauth_rig::AuthServer::Grant& grant) {
        json claims{{"iss", issuer},
                    {"sub", grant.subject},
                    {"client_id", grant.client_id},
                    {"aud", grant.audience},
                    {"exp", epoch_seconds(grant.expires)},
                    {"iat", epoch_seconds(std::chrono::system_clock::now())}};
        if (!grant.scopes.empty()) claims["scope"] = join_scopes(grant.scopes);
        return jwt_test::token(jwt_test::good_header(neomcp::http::JwtAlgorithm::rs256, std::string(key_id)), claims,
                               signing, neomcp::http::JwtAlgorithm::rs256);
    });
    const json key_set = jwt_test::key_set({published.jwk(std::string(key_id), std::string("RS256"))});
    world.as.serve_extra([key_set](const oauth_rig::Request& request) -> std::optional<http_test::Reply> {
        if (request.method == "GET" && request.target == "/jwks") return http_test::json_reply(200, key_set);
        return std::nullopt;
    });
}

ServerSetup jwt_setup(const std::shared_ptr<jwt_test::EventLog>& events) {
    ServerSetup setup;
    setup.validator = [events](ServerWorld& world) {
        // The key set's URL is not given: the validator reads it from the authorization server's metadata.
        neomcp::http::JwtOptions options;
        options.issuer = world.as.issuer();
        options.bounds.request_timeout = 10s;
        options.bounds.connect_timeout = 5s;
        options.observer = [events](neomcp::http::JwtEvent event) { events->add(event); };
        return jwt_test::make(std::move(options));
    };
    return setup;
}

class ServerEndToEndJwt : public ::testing::TestWithParam<std::string> {};

TEST_P(ServerEndToEndJwt, SignsInAndTheServerChecksTheSignedTokensWithTheKeySetOfTheAuthorizationServer) {
    auto events = std::make_shared<jwt_test::EventLog>();
    ServerWorld world(jwt_setup(events));
    serve_signed_tokens(world, jwt_test::rsa_key(0), jwt_test::rsa_key(0));
    expect_first_contact_and_step_up(world, GetParam());

    // Both tokens were checked, with a key set fetched once, from the URL the metadata names.
    EXPECT_EQ(events->count(neomcp::http::JwtEvent::accepted), 2u);
    EXPECT_EQ(events->count(neomcp::http::JwtEvent::key_set_fetched), 1u);
    EXPECT_EQ(world.as.requests_to("/jwks").size(), 1u);
    for (const std::string& token : world.as.access_tokens()) {
        EXPECT_EQ(std::count(token.begin(), token.end(), '.'), 2) << "a token of three parts";
    }
}

// A token signed with a key the published set does not hold is refused every time, however often the client signs in:
// it gives up within its bounds, and the server ran no tool.
TEST_P(ServerEndToEndJwt, RefusesTokensSignedWithAKeyTheSetDoesNotHoldAndTheClientGivesUp) {
    auto events = std::make_shared<jwt_test::EventLog>();
    ServerWorld world(jwt_setup(events));
    serve_signed_tokens(world, jwt_test::rsa_key(1), jwt_test::rsa_key(0));
    use_revision(world, GetParam());
    auto client = world.connect();
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed) << client.error().message;
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::attempts_exceeded);
    EXPECT_GE(events->count(neomcp::http::JwtEvent::signature_failed), 1u);
    EXPECT_EQ(events->count(neomcp::http::JwtEvent::accepted), 0u);
    EXPECT_EQ(world.items.open.load(), 0);
    for (const std::string& token : world.as.access_tokens()) {
        for (const int status : statuses_for(world, token)) EXPECT_EQ(status, 401);
    }
    expect_tokens_kept(world, {client.error()});
}

INSTANTIATE_TEST_SUITE_P(ServerEndToEndJwtRevisions, ServerEndToEndJwt,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28", "all"),
                         [](const auto& info) { return revision_name(info.param); });

}  // namespace
