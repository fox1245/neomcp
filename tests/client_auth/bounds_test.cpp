// Every bound of the OAuth flow on what a server or an authorization server controls, at its edge and without I/O:
// one under, at, and one over, with the problem or the diagnostic the bound gives. Each figure the flow takes from a
// bound is also read off the request that carries it, so that a bound the flow forgets to pass on is seen.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "support.h"

namespace {

using namespace oauth_test;

// The document padded with one member so that its text is exactly `size` bytes long.
std::string document_of_size(json document, std::size_t size) {
    document["pad"] = "";
    const std::size_t base = document.dump().size();
    EXPECT_LE(base, size);
    document["pad"] = std::string(size > base ? size - base : 0, 'p');
    return document.dump();
}

// The values "s0000", "s0001", ... as one space-separated list.
std::vector<std::string> scope_values(std::size_t count) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        std::string number = std::to_string(i);
        out.push_back("s" + std::string(4 - std::min<std::size_t>(4, number.size()), '0') + number);
    }
    return out;
}

std::string joined(const std::vector<std::string>& values) { return join_scopes(values); }

bool reported(const std::vector<FlowDiagnostic>& diagnostics, FlowDiagnostic::Kind kind) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [kind](const FlowDiagnostic& one) { return one.kind == kind; });
}

std::string challenge_with_scopes(const std::vector<std::string>& values) {
    return "Bearer scope=\"" + joined(values) + "\"";
}

// ---- The figures themselves

TEST(OAuthBoundsFlow, TheDefaultsAreTheSpecifiedFigures) {
    const OAuthBounds bounds;
    EXPECT_EQ(bounds.max_metadata_bytes, std::size_t{64} * 1024);
    EXPECT_EQ(bounds.max_token_response_bytes, std::size_t{64} * 1024);
    EXPECT_EQ(bounds.max_token_bytes, std::size_t{16} * 1024);
    EXPECT_EQ(bounds.max_callback_bytes, std::size_t{16} * 1024);
    EXPECT_EQ(bounds.max_challenge_params, 64u);
    EXPECT_EQ(bounds.max_authorization_servers, 16u);
    EXPECT_EQ(bounds.max_scopes, 64u);
    EXPECT_EQ(bounds.max_scope_bytes, std::size_t{4} * 1024);
    EXPECT_EQ(bounds.max_error_code_bytes, 128u);
    EXPECT_EQ(bounds.max_document_depth, 64u);
    EXPECT_EQ(bounds.authorization_fetch_timeout, std::chrono::seconds(30));
    EXPECT_EQ(bounds.prompt_timeout, std::chrono::seconds(600));
    EXPECT_EQ(bounds.authorization_timeout, std::chrono::seconds(900));
    EXPECT_EQ(bounds.max_step_ups_per_operation, 2u);
    EXPECT_EQ(bounds.max_token_lifetime, std::chrono::hours(24 * 365));
    EXPECT_EQ(bounds.max_redirects, 5u);
    EXPECT_EQ(bounds.max_authorizations_per_request, 3u);
    EXPECT_EQ(OAuthDefaults::refresh_attempts_per_flow, 1u);
    // The defaults struct and the options start equal.
    EXPECT_EQ(bounds.max_metadata_bytes, OAuthDefaults::max_metadata_bytes);
    EXPECT_EQ(bounds.max_error_code_bytes, OAuthDefaults::max_error_code_bytes);
}

// ---- Documents: max_metadata_bytes

TEST(OAuthBoundsFlow, AnAuthorizationServerDocumentAtItsBoundIsReadAndOneByteMoreIsNot) {
    for (const std::ptrdiff_t shift : {-1, 0, 1}) {
        SCOPED_TRACE(shift);
        Rig probe;
        // The server document is made the largest one, so that the bound is its size.
        const std::string body = document_of_size(probe.world.server_document(), 1024);
        Rig rig;
        rig.world.documents[rig.world.server_metadata_url()] = answer(200, body);
        rig.config.bounds.max_metadata_bytes = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(body.size()) - shift);
        const FlowStep step = rig.run(unauthorized());
        if (shift <= 0) {
            EXPECT_TRUE(done_of(step));
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::document_too_large);
            EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty());
        }
    }
}

TEST(OAuthBoundsFlow, TheMetadataRequestsCarryTheirBoundsAndTimeouts) {
    Rig rig;
    rig.config.bounds.max_metadata_bytes = 5000;
    rig.config.bounds.max_token_response_bytes = 6000;
    rig.config.bounds.authorization_fetch_timeout = std::chrono::milliseconds(7000);
    rig.config.bounds.max_redirects = 3;
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    for (const FlowFetch& one : rig.world.requests) {
        EXPECT_EQ(one.timeout, std::chrono::milliseconds(7000)) << static_cast<int>(one.purpose);
        if (one.method == "GET") {
            EXPECT_EQ(one.body_bound, 5000u);
            EXPECT_TRUE(one.follow_redirects);
            EXPECT_EQ(one.max_redirects, 3u);
        } else {
            EXPECT_EQ(one.body_bound, 6000u);
            EXPECT_FALSE(one.follow_redirects);
            EXPECT_EQ(one.max_redirects, 0u);
        }
    }
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::registration).size(), 1u);
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::token).size(), 1u);
}

// ---- Registration and token answers: max_token_response_bytes

TEST(OAuthBoundsFlow, ARegistrationAnswerAtItsBoundIsReadAndOneByteMoreIsNot) {
    const json registered{{"client_id", "test-client-id"},
                          {"token_endpoint_auth_method", "none"},
                          {"redirect_uris", json::array({"http://127.0.0.1:3000/callback"})}};
    const std::string body = document_of_size(registered, 600);
    for (const std::ptrdiff_t shift : {-1, 0, 1}) {
        SCOPED_TRACE(shift);
        Rig rig;
        rig.world.registration = answer(201, body);
        rig.config.bounds.max_token_response_bytes = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(body.size()) - shift);
        const FlowStep step = rig.run(unauthorized());
        if (shift <= 0) {
            EXPECT_TRUE(done_of(step));
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::document_too_large);
            EXPECT_TRUE(rig.world.prompts.empty());
        }
    }
}

// An error answer over the bound is a document too large, not the server's refusal.
TEST(OAuthBoundsFlow, AnErrorAnswerOverTheBoundIsTooLargeNotARefusal) {
    const std::string body = document_of_size(json{{"error", "invalid_client_metadata"}}, 300);
    {
        Rig rig;
        rig.world.registration = answer(400, body);
        rig.config.bounds.max_token_response_bytes = body.size() - 1;
        const FlowStep step = rig.run(unauthorized());
        EXPECT_EQ(problem_of(step), AuthorizationProblem::document_too_large);
        EXPECT_FALSE(std::get<FlowFailed>(step).failure.error_code);
    }
    {
        Rig rig;
        rig.world.registration = answer(400, body);
        rig.config.bounds.max_token_response_bytes = body.size();
        const FlowStep step = rig.run(unauthorized());
        EXPECT_EQ(problem_of(step), AuthorizationProblem::registration_rejected);
        EXPECT_EQ(std::get<FlowFailed>(step).failure.error_code, "invalid_client_metadata");
    }
}

TEST(OAuthBoundsFlow, ARefreshAnswerAtItsBoundIsReadAndOneByteMoreIsNot) {
    const json refreshed{{"access_token", "AccessTwo"}, {"token_type", "Bearer"}};
    const std::string body = document_of_size(refreshed, 400);
    for (const std::ptrdiff_t shift : {-1, 0, 1}) {
        SCOPED_TRACE(shift);
        Rig rig;
        rig.world.token_answers = {
            json_answer(json{{"access_token", "AccessOne"}, {"token_type", "Bearer"}, {"refresh_token", "RefreshOne"}}),
            answer(200, body)};
        ASSERT_TRUE(done_of(rig.run(unauthorized())));
        rig.config.bounds.max_token_response_bytes = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(body.size()) - shift);
        const FlowStep step = rig.run(unauthorized({}, true));
        if (shift <= 0) {
            ASSERT_TRUE(done_of(step));
            EXPECT_TRUE(done_of(step)->refreshed);
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::document_too_large);
        }
    }
}

// ---- Tokens: max_token_bytes

TEST(OAuthBoundsFlow, AnAccessTokenAtItsBoundIsKeptAndOneByteLongerIsNot) {
    for (const std::ptrdiff_t shift : {-1, 0, 1}) {
        SCOPED_TRACE(shift);
        Rig rig;
        rig.config.bounds.max_token_bytes = 64;
        const std::string token(static_cast<std::size_t>(64 + shift), 'a');
        rig.world.token_answers = {json_answer(json{{"access_token", token}, {"token_type", "Bearer"}})};
        const FlowStep step = rig.run(unauthorized());
        if (shift <= 0) {
            ASSERT_TRUE(done_of(step));
            EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.access_token, token);
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::token_request_failed);
        }
    }
}

// ---- Lifetime: max_token_lifetime

TEST(OAuthBoundsFlow, ALifetimeAtItsBoundIsKeptAndOneSecondMoreIsCapped) {
    const auto cap = std::chrono::duration_cast<std::chrono::seconds>(OAuthDefaults::max_token_lifetime);
    for (const std::int64_t shift : {-1, 0, 1}) {
        SCOPED_TRACE(shift);
        Rig rig;
        rig.world.token_answers = {json_answer(
            json{{"access_token", "AccessOne"}, {"token_type", "Bearer"}, {"expires_in", cap.count() + shift}})};
        ASSERT_TRUE(done_of(rig.run(unauthorized())));
        const auto expires = rig.lookup.tokens_by_key.begin()->second.expires_at;
        ASSERT_TRUE(expires);
        EXPECT_EQ(*expires, test_time() + (shift < 0 ? cap - std::chrono::seconds(1) : cap));
    }
}

// ---- The error code: max_error_code_bytes

// ---- Challenges: max_challenge_params

TEST(OAuthBoundsFlow, AChallengeAtTheDefaultParameterBoundIsReadAndOneMoreIsIgnored) {
    const OAuthBounds defaults;
    const std::string named = "http://localhost:3001/custom/prm.json";
    for (const std::size_t count : {defaults.max_challenge_params - 1, defaults.max_challenge_params,
                                    defaults.max_challenge_params + 1}) {
        SCOPED_TRACE(count);
        std::string challenge = "Bearer resource_metadata=\"" + named + "\"";
        for (std::size_t i = 1; i < count; ++i) challenge += ", p" + std::to_string(i) + "=\"v\"";
        Rig rig;
        rig.world.documents[named] = json_answer(rig.world.resource_document());
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge}))));
        EXPECT_EQ(rig.world.requests.front().url,
                  count <= defaults.max_challenge_params ? named : rig.world.resource_metadata_path_url());
    }
}

TEST(OAuthBoundsFlow, AChallengeUnderTheParameterBoundIsRead) {
    const std::string named = "http://localhost:3001/custom/prm.json";
    Rig rig;
    rig.config.bounds.max_challenge_params = 2;
    rig.world.documents[named] = json_answer(rig.world.resource_document());
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer resource_metadata=\"" + named + "\""}))));
    EXPECT_EQ(rig.world.requests.front().url, named);
}

// ---- Authorization servers: max_authorization_servers

TEST(OAuthBoundsFlow, TheAuthorizationServerAtTheBoundIsUsedAndTheOneAfterItIsNot) {
    const OAuthBounds defaults;
    for (const std::size_t at : {std::size_t{1}, defaults.max_authorization_servers,
                                 defaults.max_authorization_servers + 1}) {
        SCOPED_TRACE(at);
        Rig rig;
        json servers = json::array();
        for (std::size_t i = 1; i <= defaults.max_authorization_servers + 2; ++i) {
            servers.push_back(i == at ? rig.world.issuer : "http://plain" + std::to_string(i) + ".example");
        }
        rig.world.documents[rig.world.resource_metadata_path_url()] =
            json_answer(json{{"resource", rig.world.endpoint}, {"authorization_servers", servers}});
        const FlowStep step = rig.run(unauthorized());
        if (at <= defaults.max_authorization_servers) {
            ASSERT_TRUE(done_of(step));
            EXPECT_EQ(done_of(step)->key.issuer, rig.world.issuer);
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::refused_url);
        }
        // The list was longer than the bound in every case.
        EXPECT_TRUE(reported(rig.diagnostics, FlowDiagnostic::Kind::authorization_servers_truncated));
    }
}

TEST(OAuthBoundsFlow, AListAtTheBoundIsNotReportedAsCut) {
    Rig rig;
    rig.config.bounds.max_authorization_servers = 2;
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(
        json{{"resource", rig.world.endpoint}, {"authorization_servers", json::array({rig.world.issuer, "http://a.example"})}});
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_FALSE(reported(rig.diagnostics, FlowDiagnostic::Kind::authorization_servers_truncated));
}

// ---- Scopes: max_scopes, max_scope_bytes

TEST(OAuthBoundsFlow, TheScopesOfAChallengeAtTheCountBoundAreAskedAndTheRestIsLeftOut) {
    for (const std::size_t count : {std::size_t{3}, std::size_t{4}, std::size_t{5}}) {
        SCOPED_TRACE(count);
        Rig rig;
        rig.config.bounds.max_scopes = 4;
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge_with_scopes(scope_values(count))}))));
        ASSERT_EQ(rig.world.prompts.size(), 1u);
        EXPECT_EQ(rig.world.prompts.front().scopes, scope_values(std::min<std::size_t>(count, 4)));
        EXPECT_EQ(reported(rig.diagnostics, FlowDiagnostic::Kind::scopes_truncated), count > 4);
        const auto asked = form_value(query_of(rig.world.prompts.front().authorization_url), "scope");
        ASSERT_TRUE(asked);
        EXPECT_EQ(*asked, joined(scope_values(std::min<std::size_t>(count, 4))));
    }
}

TEST(OAuthBoundsFlow, TheScopesOfAChallengeAtTheDefaultCountBoundAreAsked) {
    const OAuthBounds defaults;
    for (const std::size_t count : {defaults.max_scopes, defaults.max_scopes + 1}) {
        SCOPED_TRACE(count);
        Rig rig;
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge_with_scopes(scope_values(count))}))));
        EXPECT_EQ(rig.world.prompts.front().scopes.size(), defaults.max_scopes);
        EXPECT_EQ(reported(rig.diagnostics, FlowDiagnostic::Kind::scopes_truncated), count > defaults.max_scopes);
    }
}

// Seven bytes each; n of them joined with spaces are 8n - 1 bytes.
TEST(OAuthBoundsFlow, TheScopesOfAChallengeAtTheByteBoundAreAskedAndTheRestIsLeftOut) {
    const std::vector<std::string> values = {"scope01", "scope02", "scope03", "scope04"};
    for (const std::size_t bound : {std::size_t{22}, std::size_t{23}, std::size_t{24}}) {
        SCOPED_TRACE(bound);
        Rig rig;
        rig.config.bounds.max_scope_bytes = bound;
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge_with_scopes(values)}))));
        const std::size_t kept = bound >= 31 ? 4 : (bound + 1) / 8;
        EXPECT_EQ(rig.world.prompts.front().scopes, std::vector<std::string>(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(kept)));
        EXPECT_TRUE(reported(rig.diagnostics, FlowDiagnostic::Kind::scopes_truncated));
    }
}

TEST(OAuthBoundsFlow, TheScopesSupportedOfTheResourceAreBoundedLikeTheOnesOfAChallenge) {
    for (const std::size_t count : {std::size_t{4}, std::size_t{5}}) {
        SCOPED_TRACE(count);
        Rig rig;
        rig.config.bounds.max_scopes = 4;
        json document = rig.world.resource_document();
        document["scopes_supported"] = scope_values(count);
        rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(document);
        ASSERT_TRUE(done_of(rig.run(unauthorized())));
        EXPECT_EQ(rig.world.prompts.front().scopes, scope_values(std::min<std::size_t>(count, 4)));
        EXPECT_EQ(reported(rig.diagnostics, FlowDiagnostic::Kind::scopes_truncated), count > 4);
    }
}

TEST(OAuthBoundsFlow, TheScopesGrantedAtTheCountBoundAreKeptAndTheRestIsLeftOut) {
    for (const std::size_t count : {std::size_t{4}, std::size_t{5}}) {
        SCOPED_TRACE(count);
        Rig rig;
        rig.config.bounds.max_scopes = 4;
        rig.world.token_answers = {json_answer(json{{"access_token", "AccessOne"},
                                                    {"token_type", "Bearer"},
                                                    {"scope", joined(scope_values(count))}})};
        ASSERT_TRUE(done_of(rig.run(unauthorized())));
        EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.granted_scopes,
                  scope_values(std::min<std::size_t>(count, 4)));
        EXPECT_EQ(reported(rig.diagnostics, FlowDiagnostic::Kind::scopes_truncated), count > 4);
    }
}

// ---- The sign-in's own bounds

TEST(OAuthBoundsFlow, TheSignInEndsOnlyAfterItsBound) {
    World world;
    FlowConfig config;
    config.endpoint_url = world.endpoint;
    config.registration.redirect_uri = world.redirect_uri;
    config.bounds.authorization_timeout = std::chrono::milliseconds(5000);
    TestAdmission admission(world.endpoint);
    MapLookup lookup;
    const auto t0 = test_time();
    for (const std::int64_t shift : {-1, 0, 1}) {
        SCOPED_TRACE(shift);
        CountingRandom random;
        FlowMemory memory;
        OAuthFlow flow(config, admission, lookup, random.source(), memory);
        FlowStep step = flow.start(unauthorized(), t0);
        auto* fetch = std::get_if<FlowFetch>(&step);
        ASSERT_NE(fetch, nullptr);
        step = flow.fetched(fetch->id, world.respond(*fetch), t0 + std::chrono::milliseconds(5000 + shift));
        if (shift <= 0) {
            EXPECT_TRUE(std::holds_alternative<FlowFetch>(step));
        } else {
            ASSERT_TRUE(std::holds_alternative<FlowFailed>(step));
            EXPECT_EQ(std::get<FlowFailed>(step).failure.problem, AuthorizationProblem::timed_out);
        }
    }
}

TEST(OAuthBoundsFlow, ThePromptCarriesItsTimeout) {
    Rig rig;
    rig.config.bounds.prompt_timeout = std::chrono::milliseconds(1234);
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    ASSERT_EQ(rig.world.prompts.size(), 1u);
    EXPECT_EQ(rig.world.prompts.front().timeout, std::chrono::milliseconds(1234));
}

// An operation may start max_step_ups_per_operation + 1 sign-ins in a row; the next ends before any request.
TEST(OAuthBoundsFlow, AnOperationStartsOneSignInMoreThanItsStepUpBound) {
    for (const std::size_t bound : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
        SCOPED_TRACE(bound);
        Rig rig;
        rig.config.bounds.max_step_ups_per_operation = bound;
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge_with_scopes({"s0"})}))));
        for (std::size_t i = 1; i <= bound; ++i) {
            ASSERT_TRUE(done_of(rig.run(insufficient_scope("s" + std::to_string(i))))) << i;
        }
        EXPECT_EQ(rig.world.prompts.size(), bound + 1);
        const std::size_t requests = rig.world.requests.size();
        EXPECT_EQ(problem_of(rig.run(insufficient_scope("sx"))), AuthorizationProblem::attempts_exceeded);
        EXPECT_EQ(rig.world.requests.size(), requests);
    }
}

// A refused request is held up to its bound: the flow also refuses a trigger that names more refusals than that.
TEST(OAuthBoundsFlow, ARequestRefusedAsOftenAsItsBoundIsNotHeldAgain) {
    for (const std::size_t bound : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
        SCOPED_TRACE(bound);
        {
            Rig rig;
            rig.config.bounds.max_authorizations_per_request = bound;
            Trigger trigger = unauthorized();
            trigger.refusals = bound - 1;
            EXPECT_TRUE(done_of(rig.run(trigger)));
        }
        Rig rig;
        rig.config.bounds.max_authorizations_per_request = bound;
        Trigger trigger = unauthorized();
        trigger.refusals = bound;
        EXPECT_EQ(problem_of(rig.run(trigger)), AuthorizationProblem::attempts_exceeded);
        EXPECT_TRUE(rig.world.requests.empty());
    }
}

// One refresh in a flow: a refused one is followed by the full flow and never by a second refresh.
TEST(OAuthBoundsFlow, OneRefreshIsMadeInAFlow) {
    Rig rig;
    rig.world.token_answers = {
        json_answer(json{{"access_token", "AccessOne"}, {"token_type", "Bearer"}, {"refresh_token", "RefreshOne"}}),
        json_answer(json{{"error", "invalid_grant"}}, 400),
        json_answer(json{{"access_token", "AccessThree"}, {"token_type", "Bearer"}, {"refresh_token", "RefreshThree"}})};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    ASSERT_TRUE(done_of(rig.run(unauthorized({}, true))));
    std::vector<std::string> grants;
    for (const FlowFetch* one : rig.world.requests_for(FetchPurpose::token)) {
        grants.push_back(form_value(one->body, "grant_type").value_or(""));
    }
    EXPECT_EQ(grants, (std::vector<std::string>{"authorization_code", "refresh_token", "authorization_code"}));
}

}  // namespace
