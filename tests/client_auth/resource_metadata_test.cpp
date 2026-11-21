// Protected resource metadata: where the flow looks, what each answer means, and the identity of `resource`.
#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;

std::vector<PrmCandidate> candidates_for(std::string_view endpoint, std::optional<std::string> named = std::nullopt) {
    TestAdmission admission(endpoint);
    auto result = protected_resource_candidates(endpoint, named, admission);
    auto* list = std::get_if<std::vector<PrmCandidate>>(&result);
    EXPECT_NE(list, nullptr);
    return list != nullptr ? *list : std::vector<PrmCandidate>{};
}

std::optional<AuthorizationProblem> candidates_refused(std::string_view endpoint, std::optional<std::string> named) {
    TestAdmission admission(endpoint);
    auto result = protected_resource_candidates(endpoint, named, admission);
    auto* failure = std::get_if<FlowFailure>(&result);
    if (failure == nullptr) return std::nullopt;
    return failure->problem;
}

TEST(OAuthFlowResourceMetadata, TheNamedUrlIsTheOnlyCandidate) {
    const auto list = candidates_for("https://example.com/mcp",
                                     "https://example.com/.well-known/oauth-protected-resource/mcp");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_TRUE(list[0].named_by_server);
    EXPECT_EQ(list[0].identifiers, std::vector<std::string>{"https://example.com/mcp"});
}

TEST(OAuthFlowResourceMetadata, WithoutANamedUrlThePathFormThenTheRoot) {
    const auto list = candidates_for("https://example.com/public/mcp?tenant=1#ignored");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].url, "https://example.com/.well-known/oauth-protected-resource/public/mcp?tenant=1");
    EXPECT_EQ(list[0].identifiers, std::vector<std::string>{"https://example.com/public/mcp?tenant=1"});
    EXPECT_EQ(list[1].url, "https://example.com/.well-known/oauth-protected-resource");
    EXPECT_EQ(list[1].identifiers, std::vector<std::string>{"https://example.com"});
}

TEST(OAuthFlowResourceMetadata, AnEndpointWithoutPathIsRequestedOnce) {
    for (const char* endpoint : {"https://example.com", "https://example.com/"}) {
        const auto list = candidates_for(endpoint);
        ASSERT_EQ(list.size(), 1u) << endpoint;
        EXPECT_EQ(list[0].url, "https://example.com/.well-known/oauth-protected-resource");
        // Either identifier the URL was built from is accepted.
        EXPECT_EQ(list[0].identifiers.size(), 2u);
    }
}

TEST(OAuthFlowResourceMetadata, TheNamedUrlMustKeepTheEndpointsOrigin) {
    EXPECT_EQ(candidates_refused("https://example.com/mcp",
                                 "https://other.example/.well-known/oauth-protected-resource"),
              AuthorizationProblem::refused_url);
    EXPECT_EQ(candidates_refused("https://example.com/mcp", "http://example.com/.well-known/oauth-protected-resource"),
              AuthorizationProblem::refused_url);
    EXPECT_EQ(candidates_refused("https://example.com/mcp", "https://example.com:444/prm"),
              AuthorizationProblem::refused_url);
    EXPECT_EQ(candidates_refused("https://example.com/mcp", "https://user@example.com/prm"),
              AuthorizationProblem::refused_url);
    EXPECT_EQ(candidates_refused("https://example.com/mcp", "/relative"), AuthorizationProblem::refused_url);
    // The same origin written another way is the same origin.
    EXPECT_EQ(candidates_refused("https://example.com/mcp", "HTTPS://EXAMPLE.COM:443/prm"), std::nullopt);
}

TEST(OAuthFlowResourceMetadata, TheNamedUrlPassesThePolicy) {
    TestAdmission admission("https://example.com/mcp");
    admission.refused.insert("https://example.com/prm");
    auto result = protected_resource_candidates("https://example.com/mcp", std::string("https://example.com/prm"),
                                                admission);
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(result));
    EXPECT_EQ(std::get<FlowFailure>(result).problem, AuthorizationProblem::refused_url);
}

// ---- The runner's four layouts, run through the flow

TEST(OAuthFlowResourceMetadata, LayoutNamedInTheHeader) {
    Rig rig;
    const FlowStep step = rig.run(unauthorized(
        {"Bearer resource_metadata=\"http://localhost:3001/.well-known/oauth-protected-resource/mcp\""}));
    ASSERT_TRUE(done_of(step));
    EXPECT_FALSE(rig.world.requested(rig.world.resource_metadata_root_url()));
    EXPECT_EQ(done_of(step)->key.resource, "http://localhost:3001/mcp");
}

TEST(OAuthFlowResourceMetadata, LayoutPathFormNotInTheHeader) {
    Rig rig;
    const FlowStep step = rig.run(unauthorized({"Bearer"}));
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(rig.world.requests.front().url, rig.world.resource_metadata_path_url());
}

TEST(OAuthFlowResourceMetadata, LayoutAtTheRootWithTheOriginAsResource) {
    Rig rig;
    rig.world.documents.erase(rig.world.resource_metadata_path_url());
    rig.world.documents[rig.world.resource_metadata_root_url()] = json_answer(
        json{{"resource", "http://localhost:3001"}, {"authorization_servers", json::array({rig.world.issuer})}});
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key.resource, "http://localhost:3001");
    ASSERT_GE(rig.world.requests.size(), 2u);
    EXPECT_EQ(rig.world.requests[0].url, rig.world.resource_metadata_path_url());
    EXPECT_EQ(rig.world.requests[1].url, rig.world.resource_metadata_root_url());
}

TEST(OAuthFlowResourceMetadata, LayoutAtACustomPathNamedInTheHeader) {
    Rig rig;
    rig.world.documents["http://localhost:3001/custom/metadata/location.json"] =
        json_answer(rig.world.resource_document());
    const FlowStep step =
        rig.run(unauthorized({"Bearer resource_metadata=\"http://localhost:3001/custom/metadata/location.json\""}));
    ASSERT_TRUE(done_of(step));
}

TEST(OAuthFlowResourceMetadata, ANamedUrlThatFailsIsNotFollowedByTheWellKnownForms) {
    Rig rig;
    const FlowStep step = rig.run(unauthorized({"Bearer resource_metadata=\"http://localhost:3001/missing\""}));
    EXPECT_EQ(problem_of(step), AuthorizationProblem::discovery_failed);
    EXPECT_EQ(rig.world.requests.size(), 1u);
}

TEST(OAuthFlowResourceMetadata, AChallengeThatCannotBeUsedCountsAsNone) {
    for (const char* challenge : {"Bearer abc==", "Bearer resource_metadata=\"http://x\", RESOURCE_METADATA=\"y\"",
                                  "Bearer resource_metadata=\"unterminated", "Basic realm=\"x\""}) {
        Rig rig;
        const FlowStep step = rig.run(unauthorized({challenge}));
        ASSERT_TRUE(done_of(step)) << challenge;
        EXPECT_EQ(rig.world.requests.front().url, rig.world.resource_metadata_path_url()) << challenge;
    }
}

TEST(OAuthFlowResourceMetadata, TheChallengeParameterBoundAtItsEdge) {
    const std::string named = "http://localhost:3001/custom/prm.json";
    const std::string two = "Bearer resource_metadata=\"" + named + "\", scope=\"mcp:basic\"";
    {
        Rig rig;
        rig.config.bounds.max_challenge_params = 2;
        rig.world.documents[named] = json_answer(rig.world.resource_document());
        ASSERT_TRUE(done_of(rig.run(unauthorized({two}))));
        EXPECT_EQ(rig.world.requests.front().url, named);
    }
    {
        // One parameter more: the challenge is not used, and the well-known URL is asked instead.
        Rig rig;
        rig.config.bounds.max_challenge_params = 2;
        rig.world.documents[named] = json_answer(rig.world.resource_document());
        ASSERT_TRUE(done_of(rig.run(unauthorized({two + ", error=\"invalid_token\""}))));
        EXPECT_EQ(rig.world.requests.front().url, rig.world.resource_metadata_path_url());
    }
}

// ---- What each answer means

TEST(OAuthFlowResourceMetadata, ClientErrorsGoOnAndOtherAnswersEndTheFlow) {
    struct Case {
        FetchResult first;
        bool goes_on;
        AuthorizationProblem problem;
    };
    const std::vector<Case> cases = {
        {not_found(), true, {}},
        {answer(400, "", "text/plain"), true, {}},
        {answer(401, "", "text/plain"), true, {}},
        {answer(410, "", "text/plain"), true, {}},
        {answer(429, "", "text/plain"), false, AuthorizationProblem::discovery_failed},
        {answer(500, "", "text/plain"), false, AuthorizationProblem::discovery_failed},
        {answer(503, "", "text/plain"), false, AuthorizationProblem::discovery_failed},
        {answer(302, "", "text/plain"), false, AuthorizationProblem::discovery_failed},
        {answer(200, "{\"resource\":1", "application/json"), false, AuthorizationProblem::discovery_failed},
        {answer(200, "[]", "application/json"), false, AuthorizationProblem::discovery_failed},
        {answer(200, "{}", "text/html"), false, AuthorizationProblem::discovery_failed},
        {no_answer(FetchOutcome::transport_error), false, AuthorizationProblem::discovery_failed},
        {no_answer(FetchOutcome::timed_out), false, AuthorizationProblem::timed_out},
        {no_answer(FetchOutcome::too_large), false, AuthorizationProblem::document_too_large},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        SCOPED_TRACE(i);
        Rig rig;
        rig.world.documents[rig.world.resource_metadata_path_url()] = cases[i].first;
        // The root form's document names the origin, the identifier that URL is built from.
        rig.world.documents[rig.world.resource_metadata_root_url()] = json_answer(
            json{{"resource", "http://localhost:3001"}, {"authorization_servers", json::array({rig.world.issuer})}});
        const FlowStep step = rig.run(unauthorized());
        if (cases[i].goes_on) {
            EXPECT_TRUE(done_of(step));
        } else {
            EXPECT_EQ(problem_of(step), cases[i].problem);
            EXPECT_FALSE(rig.world.requested(rig.world.resource_metadata_root_url()));
        }
    }
}

TEST(OAuthFlowResourceMetadata, NoDocumentAnywhereFailsDiscovery) {
    Rig rig;
    rig.world.documents.clear();
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
    EXPECT_EQ(rig.world.requests.size(), 2u);
}

TEST(OAuthFlowResourceMetadata, AMediaTypeWithParametersIsJson) {
    Rig rig;
    rig.world.documents[rig.world.resource_metadata_path_url()] =
        answer(200, rig.world.resource_document().dump(), "application/json; charset=utf-8");
    EXPECT_TRUE(done_of(rig.run(unauthorized())));
}

TEST(OAuthFlowResourceMetadata, TheDocumentBoundIsKeptAtItsEdge) {
    Rig rig;
    // The bound holds for both metadata documents; the resource's is made the larger one.
    json document = rig.world.resource_document();
    document["padding"] = std::string(rig.world.server_document().dump().size(), 'x');
    const std::string body = document.dump();
    rig.world.documents[rig.world.resource_metadata_path_url()] = answer(200, body);
    rig.config.bounds.max_metadata_bytes = body.size();
    EXPECT_TRUE(done_of(rig.run(unauthorized())));

    Rig over;
    over.world.documents[over.world.resource_metadata_path_url()] = answer(200, body);
    over.config.bounds.max_metadata_bytes = body.size() - 1;
    EXPECT_EQ(problem_of(over.run(unauthorized())), AuthorizationProblem::document_too_large);
    // The request said so too: the driver reads no more than the bound.
    EXPECT_EQ(over.world.requests.front().body_bound, body.size() - 1);
}

TEST(OAuthFlowResourceMetadata, TheNestingBoundIsKept) {
    Rig rig;
    json document = rig.world.resource_document();
    json deep = json::array();
    for (int i = 0; i < 10; ++i) deep = json::array({deep});
    document["extra"] = deep;
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(document);
    rig.config.bounds.max_document_depth = 8;
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
}

TEST(OAuthFlowResourceMetadata, MetadataRequestsAskForJsonAndFollowRedirectsWithinBounds) {
    Rig rig;
    Trigger trigger = unauthorized();
    trigger.resource_headers = {{"MCP-Protocol-Version", "2025-11-25"}};
    ASSERT_TRUE(done_of(rig.run(trigger)));
    const FlowFetch& first = rig.world.requests.front();
    EXPECT_EQ(first.method, "GET");
    EXPECT_EQ(header_value(first, "Accept"), "application/json");
    EXPECT_TRUE(first.follow_redirects);
    EXPECT_EQ(first.max_redirects, OAuthDefaults::max_redirects);
    EXPECT_EQ(first.timeout, OAuthDefaults::authorization_fetch_timeout);
    EXPECT_EQ(header_value(first, "MCP-Protocol-Version"), "2025-11-25");
    // The authorization server is on another origin: no protocol header goes there.
    const auto server = rig.world.requests_for(FetchPurpose::authorization_server_metadata);
    ASSERT_FALSE(server.empty());
    EXPECT_FALSE(header_value(*server.front(), "MCP-Protocol-Version"));
    for (const FlowFetch& one : rig.world.requests) {
        if (one.purpose != FetchPurpose::token) {
            EXPECT_FALSE(header_value(one, "Authorization"));
        }
    }
}

// ---- Validation and identity

TEST(OAuthFlowResourceMetadata, TheDocumentNeedsAResourceAndAuthorizationServers) {
    const std::vector<json> documents = {
        json{{"authorization_servers", json::array({"http://localhost:3002"})}},
        json{{"resource", 5}, {"authorization_servers", json::array({"http://localhost:3002"})}},
        json{{"resource", "http://localhost:3001/mcp"}},
        json{{"resource", "http://localhost:3001/mcp"}, {"authorization_servers", json::array()}},
        json{{"resource", "http://localhost:3001/mcp"}, {"authorization_servers", "http://localhost:3002"}},
        json{{"resource", "http://localhost:3001/mcp"}, {"authorization_servers", json::array({1})}},
    };
    for (const json& document : documents) {
        SCOPED_TRACE(document.dump());
        Rig rig;
        rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(document);
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::authorization_server_metadata).empty());
    }
}

struct IdentityCase {
    const char* endpoint;
    const char* resource;
    bool accepted;
};

TEST(OAuthFlowResourceMetadata, TheResourceMustBeTheEndpoint) {
    const std::vector<IdentityCase> cases = {
        {"http://localhost:3001/mcp", "http://localhost:3001/mcp", true},
        {"http://localhost:3001/mcp", "HTTP://LOCALHOST:3001/mcp", true},
        {"http://LocalHost:3001/mcp", "http://localhost:3001/mcp", true},
        {"http://localhost:3001/mcp", "http://localhost:3001/mcp/", false},
        {"http://localhost:3001/mcp/", "http://localhost:3001/mcp", false},
        {"http://localhost:3001/mcp", "http://localhost:3001/MCP", false},
        {"http://localhost:3001/mcp", "http://localhost:3001/%6Dcp", false},
        {"http://localhost:3001/mcp", "http://localhost:3001", false},
        {"http://localhost:3001/mcp", "http://localhost:3002/mcp", false},
        {"http://localhost:80/mcp", "http://localhost/mcp", false},
        {"http://localhost/mcp", "http://localhost:80/mcp", false},
        {"http://localhost:3001/mcp", "https://localhost:3001/mcp", false},
        {"http://localhost:3001/mcp", "http://example.com/mcp", false},
    };
    for (const IdentityCase& one : cases) {
        SCOPED_TRACE(std::string(one.endpoint) + " / " + one.resource);
        Rig rig;
        rig.config.endpoint_url = one.endpoint;
        // The URL the challenge names, on the endpoint's origin.
        const std::string named = origin_text(*read_url(one.endpoint)) + "/prm";
        rig.world.documents[named] =
            json_answer(json{{"resource", one.resource}, {"authorization_servers", json::array({rig.world.issuer})}});
        const FlowStep step = rig.run(unauthorized({"Bearer resource_metadata=\"" + named + "\""}));
        if (one.accepted) {
            EXPECT_TRUE(done_of(step));
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::resource_mismatch);
            // Nothing was asked of any authorization server.
            EXPECT_EQ(rig.world.requests.size(), 1u);
            EXPECT_TRUE(rig.world.prompts.empty());
        }
    }
}

TEST(OAuthFlowResourceMetadata, TheRootFormNeedsTheOriginAsWritten) {
    for (const auto& [resource, accepted] : std::vector<std::pair<std::string, bool>>{
             {"http://localhost:3001", true}, {"http://LOCALHOST:3001", true}, {"http://localhost:3001/", false},
             {"http://localhost:3001/mcp", false}}) {
        SCOPED_TRACE(resource);
        Rig rig;
        rig.world.documents.erase(rig.world.resource_metadata_path_url());
        rig.world.documents[rig.world.resource_metadata_root_url()] =
            json_answer(json{{"resource", resource}, {"authorization_servers", json::array({rig.world.issuer})}});
        const FlowStep step = rig.run(unauthorized());
        if (accepted) {
            EXPECT_TRUE(done_of(step));
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::resource_mismatch);
        }
    }
}

TEST(OAuthFlowResourceMetadata, TheHookMayAcceptAnotherResource) {
    Rig rig;
    std::optional<ResourceMismatch> seen;
    rig.config.accept_resource = [&](const ResourceMismatch& mismatch) {
        seen = mismatch;
        return true;
    };
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(
        json{{"resource", "http://localhost:3001"}, {"authorization_servers", json::array({rig.world.issuer})}});
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->expected, "http://localhost:3001/mcp");
    EXPECT_EQ(seen->received, "http://localhost:3001");
    // The resource is used as received.
    EXPECT_EQ(done_of(step)->key.resource, "http://localhost:3001");
    const auto tokens = rig.world.requests_for(FetchPurpose::token);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(form_value(tokens.front()->body, "resource"), "http://localhost:3001");

    Rig refusing;
    refusing.config.accept_resource = [](const ResourceMismatch&) { return false; };
    refusing.world.documents = rig.world.documents;
    EXPECT_EQ(problem_of(refusing.run(unauthorized())), AuthorizationProblem::resource_mismatch);
}

TEST(OAuthFlowResourceMetadata, TheResourceIsSentExactlyAsReceived) {
    Rig rig;
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(
        json{{"resource", "HTTP://LocalHost:3001/mcp"}, {"authorization_servers", json::array({rig.world.issuer})}});
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    ASSERT_EQ(rig.world.prompts.size(), 1u);
    EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "resource"),
              "HTTP://LocalHost:3001/mcp");
    EXPECT_EQ(form_value(rig.world.requests_for(FetchPurpose::token).front()->body, "resource"),
              "HTTP://LocalHost:3001/mcp");
}

TEST(OAuthFlowResourceMetadata, TheFirstAdmittedAuthorizationServerIsUsed) {
    Rig rig;
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(
        json{{"resource", rig.world.endpoint},
             {"authorization_servers",
              json::array({"http://plain.example", "https://as.example/?q", "http://localhost:3009",
                           rig.world.issuer, "http://localhost:3010"})}});
    rig.admission.emplace(rig.config.endpoint_url);
    rig.admission->refused.insert("http://localhost:3009");
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key.issuer, rig.world.issuer);
    EXPECT_FALSE(rig.diagnostics.empty());
}

TEST(OAuthFlowResourceMetadata, NoAdmittedAuthorizationServerRefusesTheUrl) {
    Rig rig;
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(
        json{{"resource", rig.world.endpoint}, {"authorization_servers", json::array({"http://plain.example"})}});
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::refused_url);
}

TEST(OAuthFlowResourceMetadata, OnlyTheBoundedNumberOfServersIsRead) {
    Rig rig;
    rig.config.bounds.max_authorization_servers = 2;
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(
        json{{"resource", rig.world.endpoint},
             {"authorization_servers",
              json::array({"http://plain.example", "http://plain2.example", rig.world.issuer})}});
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::refused_url);

    Rig edge;
    edge.config.bounds.max_authorization_servers = 3;
    edge.world.documents = rig.world.documents;
    EXPECT_TRUE(done_of(edge.run(unauthorized())));
}

TEST(OAuthFlowResourceMetadata, ALoopbackAuthorizationServerOnlyForALoopbackEndpoint) {
    Rig rig;
    rig.config.endpoint_url = "https://mcp.example/mcp";
    rig.world.documents.clear();
    rig.world.documents["https://mcp.example/.well-known/oauth-protected-resource/mcp"] = json_answer(
        json{{"resource", "https://mcp.example/mcp"}, {"authorization_servers", json::array({"http://127.0.0.1:9"})}});
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::refused_url);
}

TEST(OAuthFlowResourceMetadata, ScopesSupportedAreRead) {
    Rig rig;
    json document = rig.world.resource_document();
    document["scopes_supported"] = json::array({"mcp:basic", "bad scope", "mcp:read"});
    rig.world.documents[rig.world.resource_metadata_path_url()] = json_answer(document);
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    ASSERT_EQ(rig.world.prompts.size(), 1u);
    EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"), "mcp:basic mcp:read");
    ASSERT_FALSE(rig.diagnostics.empty());
}

TEST(OAuthFlowResourceMetadata, EveryFlowFetchesTheMetadataAgain) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const std::size_t first = rig.world.requests_for(FetchPurpose::protected_resource_metadata).size();
    ASSERT_TRUE(done_of(rig.run(unauthorized({}, false))));
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::protected_resource_metadata).size(), first * 2);
    // The authorization server metadata is kept for the same issuer.
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::authorization_server_metadata).size(), 1u);
}

}  // namespace
