// Authorization server metadata: the candidate URLs in their order, the identity of the issuer, and what a document
// must hold.
#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;

std::vector<std::string> candidates_for(std::string_view issuer) {
    TestAdmission admission("http://localhost:3001/mcp");
    auto result = authorization_server_candidates(issuer, admission);
    auto* list = std::get_if<std::vector<std::string>>(&result);
    EXPECT_NE(list, nullptr) << issuer;
    return list != nullptr ? *list : std::vector<std::string>{};
}

TEST(OAuthFlowServerMetadata, CandidatesForAnIssuerWithAPath) {
    const std::vector<std::string> expected = {
        "https://auth.example.com/.well-known/oauth-authorization-server/tenant1",
        "https://auth.example.com/.well-known/openid-configuration/tenant1",
        "https://auth.example.com/tenant1/.well-known/openid-configuration"};
    EXPECT_EQ(candidates_for("https://auth.example.com/tenant1"), expected);
    EXPECT_EQ(candidates_for("https://auth.example.com/tenant1/"), expected);
}

TEST(OAuthFlowServerMetadata, CandidatesForAnIssuerWithoutAPath) {
    const std::vector<std::string> expected = {"https://auth.example.com/.well-known/oauth-authorization-server",
                                               "https://auth.example.com/.well-known/openid-configuration"};
    EXPECT_EQ(candidates_for("https://auth.example.com"), expected);
    EXPECT_EQ(candidates_for("https://auth.example.com/"), expected);
}

TEST(OAuthFlowServerMetadata, TheIssuerMustBeAUsableIdentifier) {
    TestAdmission admission("https://mcp.example/mcp");
    for (const char* issuer : {"https://auth.example.com/?tenant=1", "https://auth.example.com/#x",
                               "http://auth.example.com", "auth.example.com", "https://user@auth.example.com",
                               "http://localhost:3002"}) {
        auto result = authorization_server_candidates(issuer, admission);
        ASSERT_TRUE(std::holds_alternative<FlowFailure>(result)) << issuer;
        EXPECT_EQ(std::get<FlowFailure>(result).problem, AuthorizationProblem::refused_url) << issuer;
    }
}

// ---- Through the flow, with the runner's layouts

TEST(OAuthFlowServerMetadata, OAuthMetadataAtTheRoot) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const auto server = rig.world.requests_for(FetchPurpose::authorization_server_metadata);
    ASSERT_EQ(server.size(), 1u);
    EXPECT_EQ(server.front()->url, "http://localhost:3002/.well-known/oauth-authorization-server");
}

TEST(OAuthFlowServerMetadata, OpenIdConfigurationAtTheRoot) {
    Rig rig;
    rig.world.documents.erase(rig.world.server_metadata_url());
    rig.world.documents["http://localhost:3002/.well-known/openid-configuration"] =
        json_answer(rig.world.server_document());
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::authorization_server_metadata).size(), 2u);
}

TEST(OAuthFlowServerMetadata, PathInsertionForATenantNeverAsksTheRoot) {
    Rig rig;
    rig.world.issuer = "http://localhost:3002/tenant1";
    rig.world.documents.clear();
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(rig.world.resource_document());
    rig.world.documents["http://localhost:3002/.well-known/oauth-authorization-server/tenant1"] =
        json_answer(rig.world.server_document());
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_FALSE(rig.world.requested("http://localhost:3002/.well-known/oauth-authorization-server"));
    EXPECT_FALSE(rig.world.requested("http://localhost:3002/.well-known/openid-configuration"));
}

TEST(OAuthFlowServerMetadata, OpenIdConfigurationAppendedToTheTenantPath) {
    Rig rig;
    rig.world.issuer = "http://localhost:3002/tenant1";
    rig.world.documents.clear();
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(rig.world.resource_document());
    rig.world.documents["http://localhost:3002/tenant1/.well-known/openid-configuration"] =
        json_answer(rig.world.server_document());
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const auto server = rig.world.requests_for(FetchPurpose::authorization_server_metadata);
    ASSERT_EQ(server.size(), 3u);
    EXPECT_EQ(server[0]->url, "http://localhost:3002/.well-known/oauth-authorization-server/tenant1");
    EXPECT_EQ(server[1]->url, "http://localhost:3002/.well-known/openid-configuration/tenant1");
    EXPECT_EQ(server[2]->url, "http://localhost:3002/tenant1/.well-known/openid-configuration");
}

TEST(OAuthFlowServerMetadata, NoCandidateAnswersFailsDiscovery) {
    Rig rig;
    rig.world.documents.erase(rig.world.server_metadata_url());
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::authorization_server_metadata).size(), 2u);
}

TEST(OAuthFlowServerMetadata, AServerErrorEndsTheFlowAtThatCandidate) {
    for (const FetchResult& first : {answer(500, "", "text/plain"), answer(429, "", "text/plain"),
                                     no_answer(FetchOutcome::transport_error)}) {
        Rig rig;
        rig.world.documents[rig.world.server_metadata_url()] = first;
        rig.world.documents["http://localhost:3002/.well-known/openid-configuration"] =
            json_answer(rig.world.server_document());
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
        EXPECT_EQ(rig.world.requests_for(FetchPurpose::authorization_server_metadata).size(), 1u);
    }
}

// ---- The issuer's identity

TEST(OAuthFlowServerMetadata, AnotherIssuerStopsTheFlowAtOnce) {
    for (const char* stated : {"http://localhost:3002/", "HTTP://localhost:3002", "http://localhost:3002/other",
                               "http://localhost:3003", "http://localhost:3002:80", "http://127.0.0.1:3002", ""}) {
        SCOPED_TRACE(stated);
        Rig rig;
        json document = rig.world.server_document();
        document["issuer"] = stated;
        // Endpoints only that document names: using any of them would show it was used.
        document["authorization_endpoint"] = "http://localhost:3002/trap/authorize";
        document["token_endpoint"] = "http://localhost:3002/trap/token";
        document["registration_endpoint"] = "http://localhost:3002/trap/register";
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        rig.world.documents["http://localhost:3002/.well-known/openid-configuration"] =
            json_answer(rig.world.server_document());
        const FlowStep step = rig.run(unauthorized());
        ASSERT_EQ(problem_of(step), AuthorizationProblem::metadata_issuer_mismatch);
        EXPECT_EQ(std::get<FlowFailed>(step).failure.issuer, "http://localhost:3002");
        // The next candidate is not tried, and nothing of the document is used.
        EXPECT_EQ(rig.world.requests_for(FetchPurpose::authorization_server_metadata).size(), 1u);
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty());
        EXPECT_TRUE(rig.world.prompts.empty());
        EXPECT_TRUE(rig.memory.servers.empty());
    }
}

TEST(OAuthFlowServerMetadata, AMissingIssuerIsAMismatch) {
    Rig rig;
    json document = rig.world.server_document();
    document.erase("issuer");
    rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::metadata_issuer_mismatch);
}

TEST(OAuthFlowServerMetadata, TheIssuerIsComparedAsReceivedFromTheResourceMetadata) {
    // The resource metadata names the issuer with a trailing slash; the document says the same: identical.
    Rig rig;
    rig.world.issuer = "http://localhost:3002/";
    rig.world.documents.clear();
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(rig.world.resource_document());
    json document = rig.world.server_document();
    document["authorization_endpoint"] = "http://localhost:3002/authorize";
    document["token_endpoint"] = "http://localhost:3002/token";
    document["registration_endpoint"] = "http://localhost:3002/register";
    rig.world.documents["http://localhost:3002/.well-known/oauth-authorization-server"] = json_answer(document);
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key.issuer, "http://localhost:3002/");
}

// ---- What the document must hold

TEST(OAuthFlowServerMetadata, ItMustListS256) {
    for (const json& methods : {json(), json::array(), json::array({"plain"}), json::array({"s256"}),
                                json("S256")}) {
        SCOPED_TRACE(methods.dump());
        Rig rig;
        json document = rig.world.server_document();
        if (methods.is_null()) {
            document.erase("code_challenge_methods_supported");
        } else {
            document["code_challenge_methods_supported"] = methods;
        }
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::pkce_unsupported);
        EXPECT_TRUE(rig.world.prompts.empty());
    }
    Rig rig;
    json document = rig.world.server_document();
    document["code_challenge_methods_supported"] = json::array({"plain", "S256"});
    rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
    EXPECT_TRUE(done_of(rig.run(unauthorized())));
}

TEST(OAuthFlowServerMetadata, ItMustNameUsableEndpoints) {
    struct Case {
        const char* member;
        json value;
        AuthorizationProblem problem;
    };
    const std::vector<Case> cases = {
        {"authorization_endpoint", json(), AuthorizationProblem::discovery_failed},
        {"token_endpoint", json(), AuthorizationProblem::discovery_failed},
        {"authorization_endpoint", json(3), AuthorizationProblem::discovery_failed},
        {"registration_endpoint", json(3), AuthorizationProblem::discovery_failed},
        {"authorization_endpoint", json("http://as.example/authorize"), AuthorizationProblem::refused_url},
        {"token_endpoint", json("http://localhost:3002/token#x"), AuthorizationProblem::refused_url},
        {"token_endpoint", json("/token"), AuthorizationProblem::refused_url},
        {"registration_endpoint", json("ftp://localhost:3002/register"), AuthorizationProblem::refused_url},
    };
    for (const Case& one : cases) {
        SCOPED_TRACE(std::string(one.member) + " " + one.value.dump());
        Rig rig;
        json document = rig.world.server_document();
        if (one.value.is_null()) {
            document.erase(one.member);
        } else {
            document[one.member] = one.value;
        }
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        EXPECT_EQ(problem_of(rig.run(unauthorized())), one.problem);
    }
}

TEST(OAuthFlowServerMetadata, EveryEndpointPassesThePolicy) {
    for (const char* refused : {"http://localhost:3002/authorize", "http://localhost:3002/token",
                                "http://localhost:3002/register",
                                "http://localhost:3002/.well-known/oauth-authorization-server"}) {
        Rig rig;
        rig.admission.emplace(rig.config.endpoint_url);
        rig.admission->refused.insert(refused);
        const FlowStep step = rig.run(unauthorized());
        EXPECT_EQ(problem_of(step), AuthorizationProblem::refused_url) << refused;
        EXPECT_TRUE(rig.world.prompts.empty());
    }
}

TEST(OAuthFlowServerMetadata, TheOptionalMembersAreRead) {
    TestAdmission admission("http://localhost:3001/mcp");
    std::vector<FlowDiagnostic> diagnostics;
    World world;
    json document = world.server_document();
    document["client_id_metadata_document_supported"] = true;
    document["authorization_response_iss_parameter_supported"] = true;
    document["scopes_supported"] = json::array({"mcp:basic", "offline_access"});
    auto result = read_authorization_server(document, world.issuer, world.server_metadata_url(), admission,
                                            OAuthBounds{}, diagnostics);
    ASSERT_TRUE(std::holds_alternative<AuthorizationServer>(result));
    const auto& server = std::get<AuthorizationServer>(result);
    EXPECT_TRUE(server.client_id_metadata_document_supported);
    EXPECT_TRUE(server.authorization_response_iss_parameter_supported);
    EXPECT_EQ(server.scopes_supported, (std::vector<std::string>{"mcp:basic", "offline_access"}));
    ASSERT_TRUE(server.token_endpoint_auth_methods_supported);
    EXPECT_EQ(*server.token_endpoint_auth_methods_supported, std::vector<std::string>{"none"});

    // A flag that is not the boolean true is false; an absent method list stays absent.
    document["client_id_metadata_document_supported"] = "true";
    document.erase("token_endpoint_auth_methods_supported");
    auto again = read_authorization_server(document, world.issuer, world.server_metadata_url(), admission,
                                           OAuthBounds{}, diagnostics);
    ASSERT_TRUE(std::holds_alternative<AuthorizationServer>(again));
    EXPECT_FALSE(std::get<AuthorizationServer>(again).client_id_metadata_document_supported);
    EXPECT_FALSE(std::get<AuthorizationServer>(again).token_endpoint_auth_methods_supported);
}

}  // namespace
