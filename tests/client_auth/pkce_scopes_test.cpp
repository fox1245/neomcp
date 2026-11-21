// PKCE and state, and the choice of scopes.
#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;

// RFC 7636, appendix B.
TEST(OAuthFlowPkce, TheExampleOfRfc7636) {
    const unsigned char octets[] = {116, 24,  223, 180, 151, 153, 224, 37,  79,  250, 96,  125, 216, 173, 187, 186,
                                    22,  212, 37,  77,  105, 214, 191, 240, 91,  88,  5,   88,  83,  132, 141, 121};
    const Pkce pkce = pkce_from_octets(std::string_view(reinterpret_cast<const char*>(octets), sizeof octets));
    EXPECT_EQ(pkce.verifier, "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
    EXPECT_EQ(pkce.challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    EXPECT_EQ(s256_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
              "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST(OAuthFlowPkce, TheVerifierHasTheLengthAndAlphabetOfRfc7636) {
    CountingRandom random;
    random.next = 200;  // octets that encode to '-' and '_' as well
    for (int i = 0; i < 8; ++i) {
        const auto pkce = make_pkce(random.source());
        ASSERT_TRUE(pkce);
        EXPECT_EQ(pkce->verifier.size(), 43u);
        for (const char c : pkce->verifier) {
            const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                    c == '-' || c == '.' || c == '_' || c == '~';
            EXPECT_TRUE(unreserved) << c;
        }
        EXPECT_EQ(pkce->challenge, s256_challenge(pkce->verifier));
        EXPECT_EQ(pkce->challenge.size(), 43u);
    }
}

TEST(OAuthFlowPkce, AFailedOrShortSourceGivesNothing) {
    CountingRandom random;
    random.fail = true;
    EXPECT_FALSE(make_pkce(random.source()));
    EXPECT_FALSE(make_state(random.source()));
    const RandomSource short_source = [](std::size_t count) {
        return std::optional<std::string>(std::string(count - 1, 'x'));
    };
    EXPECT_FALSE(make_pkce(short_source));
    EXPECT_FALSE(make_state(short_source));
    EXPECT_FALSE(make_pkce(RandomSource{}));
}

TEST(OAuthFlowPkce, StatesDiffer) {
    CountingRandom random;
    const auto first = make_state(random.source());
    const auto second = make_state(random.source());
    ASSERT_TRUE(first && second);
    EXPECT_NE(*first, *second);
    EXPECT_EQ(first->size(), 43u);
}

TEST(OAuthFlowPkce, TheFlowSendsTheChallengeAndLaterTheVerifier) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const std::string query = query_of(rig.world.prompts.front().authorization_url);
    const auto challenge = form_value(query, "code_challenge");
    ASSERT_TRUE(challenge);
    EXPECT_EQ(form_value(query, "code_challenge_method"), "S256");
    const auto verifier = form_value(rig.world.requests_for(FetchPurpose::token).front()->body, "code_verifier");
    ASSERT_TRUE(verifier);
    EXPECT_EQ(s256_challenge(*verifier), *challenge);
    // The verifier went only to the token endpoint.
    for (const FlowFetch& one : rig.world.requests) {
        if (one.purpose == FetchPurpose::token) continue;
        EXPECT_EQ(one.body.find(*verifier), std::string::npos);
    }
    EXPECT_EQ(rig.world.prompts.front().authorization_url.find(*verifier), std::string::npos);
}

TEST(OAuthFlowPkce, AFailedSourceEndsTheFlowBeforeThePrompt) {
    Rig rig;
    rig.random.fail = true;
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::random_source_failed);
    EXPECT_TRUE(rig.world.prompts.empty());
}

// ---- Scopes

TEST(OAuthFlowScopes, ScopeTokens) {
    EXPECT_TRUE(is_scope_token("mcp:basic"));
    EXPECT_TRUE(is_scope_token("!#[]~"));
    EXPECT_FALSE(is_scope_token(""));
    EXPECT_FALSE(is_scope_token("a b"));
    EXPECT_FALSE(is_scope_token("a\"b"));
    EXPECT_FALSE(is_scope_token("a\\b"));
    EXPECT_FALSE(is_scope_token("a\x7f"));
    EXPECT_FALSE(is_scope_token("\xc3\xa9"));
}

TEST(OAuthFlowScopes, BoundsAtTheirEdges) {
    OAuthBounds bounds;
    bounds.max_scopes = 2;
    std::vector<FlowDiagnostic> diagnostics;
    const std::vector<std::string> values = {"a", "b", "a", "c"};
    EXPECT_EQ(bounded_scopes(values, bounds, diagnostics), (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].kind, FlowDiagnostic::Kind::scopes_truncated);

    bounds.max_scopes = 64;
    bounds.max_scope_bytes = 3;  // "a b" fits, "a b c" does not
    diagnostics.clear();
    EXPECT_EQ(bounded_scopes(values, bounds, diagnostics), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(diagnostics.size(), 1u);
    bounds.max_scope_bytes = 5;
    diagnostics.clear();
    EXPECT_EQ(bounded_scopes(values, bounds, diagnostics), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_TRUE(diagnostics.empty());
}

TEST(OAuthFlowScopes, UnionAndSuperset) {
    const std::vector<std::string> basic = {"mcp:basic"};
    const std::vector<std::string> write = {"mcp:write"};
    const auto both = scope_union(basic, write);
    EXPECT_EQ(both, (std::vector<std::string>{"mcp:basic", "mcp:write"}));
    EXPECT_TRUE(is_strict_superset(both, basic));
    EXPECT_FALSE(is_strict_superset(basic, basic));
    EXPECT_FALSE(is_strict_superset(write, basic));
    EXPECT_FALSE(is_strict_superset(std::vector<std::string>{}, std::vector<std::string>{}));
    EXPECT_EQ(split_scopes("  a  b c "), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(join_scopes(both), "mcp:basic mcp:write");
}

TEST(OAuthFlowScopes, TheChallengesScopeComesFirst) {
    Rig rig;
    json document = rig.world.resource_document();
    document["scopes_supported"] = json::array({"mcp:basic", "mcp:read", "mcp:write"});
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(document);
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic\""}))));
    EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"), "mcp:basic");
    EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.requested_scopes, std::vector<std::string>{"mcp:basic"});
}

TEST(OAuthFlowScopes, ThenTheResourcesScopesSupported) {
    Rig rig;
    json document = rig.world.resource_document();
    document["scopes_supported"] = json::array({"mcp:basic", "mcp:read", "mcp:write"});
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(document);
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer"}))));
    EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"),
              "mcp:basic mcp:read mcp:write");
}

TEST(OAuthFlowScopes, OtherwiseNoScopeParameter) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const auto fields = form_fields(query_of(rig.world.prompts.front().authorization_url));
    EXPECT_EQ(neomcp::http::find_unique_field(fields, "scope").state, neomcp::http::FieldLookup::State::absent);
    for (const auto& field : fields) {
        EXPECT_NE(field.name, "scope");
    }
}

TEST(OAuthFlowScopes, OfflineAccessOnlyWhenTheServerListsIt) {
    {
        Rig rig;
        json document = rig.world.server_document();
        document["scopes_supported"] = json::array({"mcp:basic", "offline_access"});
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic\""}))));
        EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"),
                  "mcp:basic offline_access");
    }
    {
        // With no other scope it is sent alone.
        Rig rig;
        json document = rig.world.server_document();
        document["scopes_supported"] = json::array({"offline_access"});
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        ASSERT_TRUE(done_of(rig.run(unauthorized())));
        EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"), "offline_access");
    }
    {
        // Listed by the resource only: not requested.
        Rig rig;
        json document = rig.world.resource_document();
        document["scopes_supported"] = json::array({"mcp:basic", "offline_access"});
        rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(document);
        ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic\""}))));
        EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"), "mcp:basic");
    }
    {
        // The option off.
        Rig rig;
        rig.config.request_offline_access = false;
        json document = rig.world.server_document();
        document["scopes_supported"] = json::array({"mcp:basic", "offline_access"});
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic\""}))));
        EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"), "mcp:basic");
    }
}

TEST(OAuthFlowScopes, ScopesOutsideTheSyntaxAreDropped) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic bad\\\"x mcp:read\""}))));
    EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"), "mcp:basic mcp:read");
    ASSERT_FALSE(rig.diagnostics.empty());
    EXPECT_EQ(rig.diagnostics.front().kind, FlowDiagnostic::Kind::scope_dropped);
}

}  // namespace
