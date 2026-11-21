// Discovery on the server's origin, for a server that publishes no protected resource metadata: when it is used, which
// identifier the authorization server has, what is read from the origin's metadata, which endpoints stand in when
// there is none, and what is left out of the requests on this path.
#include <algorithm>
#include <array>

#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;
using neomcp::profile::DefaultEndpoints;
using neomcp::profile::OriginMetadataDiscovery;

// Paths that no code of the library knows, so that a flow can only get them from the data it was given.
OriginMetadataDiscovery declared_paths() {
    OriginMetadataDiscovery declared;
    declared.metadata_path = "/discover/as-document";
    declared.default_endpoints = DefaultEndpoints{"/go/authorize", "/go/token", "/go/register"};
    return declared;
}

// What an authorization server on the resource's own origin publishes, with its endpoints under `prefix`.
json origin_document(const std::string& origin, const std::string& prefix = "/oauth") {
    return json{{"issuer", origin},
                {"authorization_endpoint", origin + prefix + "/authorize"},
                {"token_endpoint", origin + prefix + "/token"},
                {"registration_endpoint", origin + prefix + "/register"},
                {"response_types_supported", json::array({"code"})},
                {"code_challenge_methods_supported", json::array({"S256"})},
                {"token_endpoint_auth_methods_supported", json::array({"none"})}};
}

// A world whose resource publishes no protected resource metadata: every such URL answers 404.
void without_resource_metadata(Rig& rig) { rig.world.documents.clear(); }

std::vector<std::string> sequence(const Rig& rig) {
    std::vector<std::string> out;
    for (const auto& one : rig.world.requests) out.push_back(one.method + " " + one.url);
    return out;
}

bool asked_for(const Rig& rig, std::string_view url) { return rig.world.requested(url); }

// ---- When the fallback is taken

TEST(OAuthFlowFallback, TakenWhenEveryResourceMetadataUrlAnsweredWithAClientError) {
    for (const int status : {400, 401, 403, 404, 410, 451}) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.world.documents[rig.world.resource_metadata_path_url()] = answer(status, "no", "text/plain");
        rig.world.documents[rig.world.resource_metadata_root_url()] = answer(status, "no", "text/plain");
        rig.world.documents["http://localhost:3001/discover/as-document"] =
            json_answer(origin_document("http://localhost:3001"));
        EXPECT_TRUE(done_of(rig.run(unauthorized()))) << status;
        EXPECT_TRUE(asked_for(rig, "http://localhost:3001/discover/as-document")) << status;
    }
}

TEST(OAuthFlowFallback, TakenAfterAChallengeNamedUrlAnsweredWithAClientError) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001"));
    const FlowStep step = rig.run(unauthorized({"Bearer resource_metadata=\"http://localhost:3001/named-prm\""}));
    EXPECT_TRUE(done_of(step));
    EXPECT_EQ(sequence(rig).front(), "GET http://localhost:3001/named-prm");
    EXPECT_TRUE(asked_for(rig, "http://localhost:3001/discover/as-document"));
}

TEST(OAuthFlowFallback, TheRequestsInOrder) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001"));
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_EQ(sequence(rig), (std::vector<std::string>{
                                 "GET http://localhost:3001/.well-known/oauth-protected-resource/mcp",
                                 "GET http://localhost:3001/.well-known/oauth-protected-resource",
                                 "GET http://localhost:3001/discover/as-document",
                                 "POST http://localhost:3001/oauth/register",
                                 "POST http://localhost:3001/oauth/token"}));
}

TEST(OAuthFlowFallback, NotTakenWhenTheResourceMetadataAnsweredWithAServerError) {
    for (const int status : {500, 502, 503}) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.world.documents[rig.world.resource_metadata_path_url()] = answer(status, "no", "text/plain");
        rig.world.documents["http://localhost:3001/discover/as-document"] =
            json_answer(origin_document("http://localhost:3001"));
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed) << status;
        EXPECT_FALSE(asked_for(rig, "http://localhost:3001/discover/as-document")) << status;
        EXPECT_TRUE(rig.world.prompts.empty()) << status;
    }
}

TEST(OAuthFlowFallback, NotTakenWhenTheLastResourceMetadataUrlAnsweredWithAServerError) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents[rig.world.resource_metadata_root_url()] = answer(503, "no", "text/plain");
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001"));
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
    EXPECT_FALSE(asked_for(rig, "http://localhost:3001/discover/as-document"));
}

TEST(OAuthFlowFallback, NotTakenAfterTooManyRequestsOrAnUnreadableAnswer) {
    const std::vector<FetchResult> answers = {
        answer(429, "slow down", "text/plain"), answer(200, "<html></html>", "text/html"),
        answer(200, "[1, 2]"),                  answer(302, "", "text/plain")};
    for (const FetchResult& one : answers) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.world.documents[rig.world.resource_metadata_path_url()] = one;
        rig.world.documents["http://localhost:3001/discover/as-document"] =
            json_answer(origin_document("http://localhost:3001"));
        EXPECT_FALSE(std::holds_alternative<FlowDone>(rig.run(unauthorized()))) << one.status;
        EXPECT_FALSE(asked_for(rig, "http://localhost:3001/discover/as-document")) << one.status;
    }
}

TEST(OAuthFlowFallback, NotTakenWithoutAnAnswer) {
    for (const FetchOutcome outcome :
         {FetchOutcome::transport_error, FetchOutcome::timed_out, FetchOutcome::too_large}) {
        Rig rig;
        rig.config.origin_metadata = declared_paths();
        rig.world.documents["http://localhost:3001/discover/as-document"] =
            json_answer(origin_document("http://localhost:3001"));
        const FlowStep first = [&] {
            // The path form answers 404, the root form never answers.
            if (!rig.admission) rig.admission.emplace(rig.config.endpoint_url);
            OAuthFlow flow(rig.config, *rig.admission, rig.lookup, rig.random.source(), rig.memory);
            FlowStep step = flow.start(unauthorized(), test_time());
            for (int i = 0; i < 4; ++i) {
                const auto* fetch = std::get_if<FlowFetch>(&step);
                if (fetch == nullptr) break;
                rig.world.requests.push_back(*fetch);
                const FetchResult result = i == 0 ? not_found() : no_answer(outcome);
                step = flow.fetched(fetch->id, result, test_time());
            }
            return step;
        }();
        EXPECT_FALSE(std::holds_alternative<FlowDone>(first));
        EXPECT_TRUE(std::holds_alternative<FlowFailed>(first));
        EXPECT_FALSE(asked_for(rig, "http://localhost:3001/discover/as-document"));
    }
}

TEST(OAuthFlowFallback, NotTakenWhenNoEnabledRevisionDeclaresIt) {
    Rig rig;
    without_resource_metadata(rig);
    // The data is there for the server, but the configuration holds no declaration.
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001"));
    rig.world.documents["http://localhost:3001/.well-known/oauth-authorization-server"] =
        json_answer(origin_document("http://localhost:3001"));
    ASSERT_FALSE(rig.config.origin_metadata);
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
    EXPECT_EQ(sequence(rig), (std::vector<std::string>{
                                 "GET http://localhost:3001/.well-known/oauth-protected-resource/mcp",
                                 "GET http://localhost:3001/.well-known/oauth-protected-resource"}));
}

TEST(OAuthFlowFallback, TheDeclarationIsTheFirstOfTheEnabledRevisionsThatHasOne) {
    const std::array<neomcp::profile::ProfilePtr, 2> without_one{neomcp::profile::builtin("2025-11-25"), nullptr};
    EXPECT_FALSE(origin_metadata_of(without_one));
    const std::array<neomcp::profile::ProfilePtr, 2> with_one{neomcp::profile::builtin("2025-11-25"),
                                                              neomcp::profile::builtin("2025-03-26")};
    const auto declared = origin_metadata_of(with_one);
    ASSERT_TRUE(declared);
    EXPECT_EQ(declared->metadata_path, "/.well-known/oauth-authorization-server");
    EXPECT_EQ(declared->default_endpoints.authorization, "/authorize");
    EXPECT_EQ(declared->default_endpoints.token, "/token");
    EXPECT_EQ(declared->default_endpoints.registration, "/register");
}

// ---- The identifier of the authorization server

TEST(OAuthFlowFallback, TheIdentifierIsTheEndpointsSchemeAndAuthorityAsWritten) {
    Rig rig;
    rig.config.endpoint_url = "https://Example.test:8443/a/b/mcp/?tenant=1#part";
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["https://Example.test:8443/discover/as-document"] =
        json_answer(origin_document("https://Example.test:8443"));
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key.issuer, "https://Example.test:8443");
    ASSERT_EQ(rig.world.prompts.size(), 1u);
    EXPECT_EQ(rig.world.prompts.front().issuer, "https://Example.test:8443");
    // The metadata is on the origin, with the path of the endpoint and its query left out.
    EXPECT_TRUE(asked_for(rig, "https://Example.test:8443/discover/as-document"));
}

TEST(OAuthFlowFallback, ADefaultPortWrittenOutIsKept) {
    Rig rig;
    rig.config.endpoint_url = "https://example.test:443/mcp";
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["https://example.test:443/discover/as-document"] =
        json_answer(origin_document("https://example.test:443"));
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key.issuer, "https://example.test:443");
}

TEST(OAuthFlowFallback, NoPathAndNoTrailingSlashIsAddedToTheIdentifier) {
    Rig rig;
    rig.config.endpoint_url = "http://localhost:3001";
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001"));
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key.issuer, "http://localhost:3001");
    EXPECT_EQ(rig.world.prompts.front().issuer, "http://localhost:3001");
}

TEST(OAuthFlowFallback, TheCredentialsAreKeptUnderTheEndpointAsTheResource) {
    Rig rig;
    rig.config.endpoint_url = "http://localhost:3001/mcp?tenant=1#part";
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001"));
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key.resource, "http://localhost:3001/mcp?tenant=1");
    EXPECT_EQ(done_of(step)->key.issuer, "http://localhost:3001");
    EXPECT_EQ(rig.memory.active, done_of(step)->key);
}

// ---- The origin's metadata

TEST(OAuthFlowFallback, MetadataOnTheOriginIsUsedWhenItsIssuerIsTheOrigin) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001", "/elsewhere"));
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    const auto registrations = rig.world.requests_for(FetchPurpose::registration);
    ASSERT_EQ(registrations.size(), 1u);
    EXPECT_EQ(registrations.front()->url, "http://localhost:3001/elsewhere/register");
    ASSERT_EQ(rig.world.prompts.size(), 1u);
    EXPECT_EQ(rig.world.prompts.front().authorization_url.rfind("http://localhost:3001/elsewhere/authorize?", 0), 0u);
    const auto tokens = rig.world.requests_for(FetchPurpose::token);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens.front()->url, "http://localhost:3001/elsewhere/token");
    // The default endpoints were not asked.
    EXPECT_FALSE(asked_for(rig, "http://localhost:3001/go/authorize"));
    EXPECT_FALSE(rig.world.requested("http://localhost:3001/go/register"));
    // The document is kept like any authorization server's.
    EXPECT_EQ(rig.memory.servers.count("http://localhost:3001"), 1u);
}

TEST(OAuthFlowFallback, AnIssuerThatIsNotTheOriginAsWrittenEndsTheFlow) {
    const std::vector<std::string> issuers = {"http://localhost:3001/",       "http://localhost:3001/oauth",
                                              "http://LOCALHOST:3001",        "http://localhost:3001/mcp",
                                              "http://localhost:3002",        ""};
    for (const std::string& issuer : issuers) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        json document = origin_document("http://localhost:3001");
        document["issuer"] = issuer;
        rig.world.documents["http://localhost:3001/discover/as-document"] = json_answer(document);
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::metadata_issuer_mismatch) << issuer;
        // Nothing of the document was used and the default endpoints were not tried instead.
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty()) << issuer;
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::token).empty()) << issuer;
        EXPECT_TRUE(rig.world.prompts.empty()) << issuer;
        EXPECT_TRUE(rig.lookup.clients.empty()) << issuer;
    }
}

TEST(OAuthFlowFallback, AMissingIssuerIsAMismatchToo) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    json document = origin_document("http://localhost:3001");
    document.erase("issuer");
    rig.world.documents["http://localhost:3001/discover/as-document"] = json_answer(document);
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::metadata_issuer_mismatch);
}

TEST(OAuthFlowFallback, TheMetadataIsValidatedAsAnyAuthorizationServersIs) {
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        json document = origin_document("http://localhost:3001");
        document["code_challenge_methods_supported"] = json::array({"plain"});
        rig.world.documents["http://localhost:3001/discover/as-document"] = json_answer(document);
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::pkce_unsupported);
    }
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        json document = origin_document("http://localhost:3001");
        document.erase("token_endpoint");
        rig.world.documents["http://localhost:3001/discover/as-document"] = json_answer(document);
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed);
    }
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        json document = origin_document("http://localhost:3001");
        rig.world.documents["http://localhost:3001/discover/as-document"] = json_answer(document);
        rig.admission.emplace(rig.config.endpoint_url);
        rig.admission->refused.insert("http://localhost:3001/oauth/token");
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::refused_url);
    }
}

TEST(OAuthFlowFallback, TheIssParameterFollowsTheMetadata) {
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        json document = origin_document("http://localhost:3001");
        document["authorization_response_iss_parameter_supported"] = true;
        rig.world.documents["http://localhost:3001/discover/as-document"] = json_answer(document);
        rig.world.callback_iss = "http://localhost:3001";
        EXPECT_TRUE(done_of(rig.run(unauthorized())));
    }
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.world.documents["http://localhost:3001/discover/as-document"] =
            json_answer(origin_document("http://localhost:3001"));
        rig.world.callback_iss = "http://localhost:3001/";
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::issuer_mismatch);
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::token).empty());
    }
}

// ---- The default endpoints

TEST(OAuthFlowFallback, WithoutMetadataTheDeclaredDefaultEndpointsAreUsed) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(sequence(rig), (std::vector<std::string>{
                                 "GET http://localhost:3001/.well-known/oauth-protected-resource/mcp",
                                 "GET http://localhost:3001/.well-known/oauth-protected-resource",
                                 "GET http://localhost:3001/discover/as-document",
                                 "POST http://localhost:3001/go/register", "POST http://localhost:3001/go/token"}));
    ASSERT_EQ(rig.world.prompts.size(), 1u);
    EXPECT_EQ(rig.world.prompts.front().authorization_url.rfind("http://localhost:3001/go/authorize?", 0), 0u);
    EXPECT_EQ(done_of(step)->key.issuer, "http://localhost:3001");
    // Nothing is known of the server beyond the endpoints: the registration is stored under the origin.
    EXPECT_EQ(rig.lookup.clients.count("http://localhost:3001"), 1u);
    // A later flow looks for the metadata again.
    EXPECT_TRUE(rig.memory.servers.empty());
}

TEST(OAuthFlowFallback, TheEndpointsComeFromTheDeclarationOfTheRevision) {
    // The built-in declaration of the revision that has one, which is what the conformance server relies on.
    const std::array<neomcp::profile::ProfilePtr, 1> profiles{neomcp::profile::builtin("2025-03-26")};
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = origin_metadata_of(profiles);
    ASSERT_TRUE(rig.config.origin_metadata);
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    const OriginMetadataDiscovery& declared = *rig.config.origin_metadata;
    EXPECT_TRUE(asked_for(rig, "http://localhost:3001" + declared.metadata_path));
    EXPECT_TRUE(asked_for(rig, "http://localhost:3001" + declared.default_endpoints.registration));
    EXPECT_TRUE(asked_for(rig, "http://localhost:3001" + declared.default_endpoints.token));
    EXPECT_EQ(rig.world.prompts.front().authorization_url.rfind(
                  "http://localhost:3001" + declared.default_endpoints.authorization + "?", 0),
              0u);
}

TEST(OAuthFlowFallback, OnlyAMissingDocumentStandsInForTheMetadata) {
    for (const int status : {400, 401, 403, 410}) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.world.documents["http://localhost:3001/discover/as-document"] = answer(status, "no", "text/plain");
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::discovery_failed) << status;
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty()) << status;
    }
    for (const int status : {500, 503, 429}) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.world.documents["http://localhost:3001/discover/as-document"] = answer(status, "no", "text/plain");
        EXPECT_FALSE(std::holds_alternative<FlowDone>(rig.run(unauthorized()))) << status;
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty()) << status;
    }
}

TEST(OAuthFlowFallback, ADefaultEndpointThePolicyRefusesEndsTheFlowBeforeAnyRequestToIt) {
    for (const std::string refused : {"http://localhost:3001/go/authorize", "http://localhost:3001/go/token",
                                      "http://localhost:3001/go/register"}) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.admission.emplace(rig.config.endpoint_url);
        rig.admission->refused.insert(refused);
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::refused_url) << refused;
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty()) << refused;
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::token).empty()) << refused;
        EXPECT_TRUE(rig.world.prompts.empty()) << refused;
    }
}

TEST(OAuthFlowFallback, ADefaultEndpointNobodyUsesIsNotAskedAbout) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.documents["http://localhost:3001/discover/as-document"] =
        json_answer(origin_document("http://localhost:3001"));
    rig.admission.emplace(rig.config.endpoint_url);
    rig.admission->refused.insert("http://localhost:3001/go/token");
    EXPECT_TRUE(done_of(rig.run(unauthorized())));
}

TEST(OAuthFlowFallback, TheOriginAndItsMetadataUrlAreAdmittedInTheirRoles) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.admission.emplace(rig.config.endpoint_url);
    rig.admission->refused.insert("http://localhost:3001/discover/as-document");
    EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::refused_url);
    EXPECT_FALSE(asked_for(rig, "http://localhost:3001/discover/as-document"));
}

TEST(OAuthFlowFallback, NoIssIsRecordedWithoutMetadata) {
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        EXPECT_TRUE(done_of(rig.run(unauthorized())));
    }
    {
        // There is nothing to compare an iss with, so one that arrives is not taken for the server's.
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        rig.world.callback_iss = "http://localhost:3001";
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::issuer_mismatch);
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::token).empty());
    }
}

// ---- What is left out and what is kept

TEST(OAuthFlowFallback, NoResourceParameterIsSentOnThisPath) {
    for (const bool metadata : {true, false}) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        if (metadata) {
            rig.world.documents["http://localhost:3001/discover/as-document"] =
                json_answer(origin_document("http://localhost:3001"));
        }
        ASSERT_TRUE(done_of(rig.run(unauthorized()))) << metadata;
        ASSERT_EQ(rig.world.prompts.size(), 1u);
        const std::string query = query_of(rig.world.prompts.front().authorization_url);
        EXPECT_FALSE(form_value(query, "resource")) << metadata;
        EXPECT_EQ(query.find("resource"), std::string::npos) << metadata;
        const auto tokens = rig.world.requests_for(FetchPurpose::token);
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_FALSE(form_value(tokens.front()->body, "resource")) << metadata;
        EXPECT_EQ(tokens.front()->body.find("resource"), std::string::npos) << metadata;
        for (const auto& one : rig.world.requests) {
            EXPECT_EQ(one.body.find("resource"), std::string::npos) << one.url;
        }
    }
}

TEST(OAuthFlowFallback, TheCodeChallengeIsStillSentWithS256) {
    for (const bool metadata : {true, false}) {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        if (metadata) {
            rig.world.documents["http://localhost:3001/discover/as-document"] =
                json_answer(origin_document("http://localhost:3001"));
        }
        ASSERT_TRUE(done_of(rig.run(unauthorized()))) << metadata;
        const std::string query = query_of(rig.world.prompts.front().authorization_url);
        EXPECT_EQ(form_value(query, "code_challenge_method"), "S256") << metadata;
        const auto challenge = form_value(query, "code_challenge");
        ASSERT_TRUE(challenge) << metadata;
        EXPECT_EQ(challenge->size(), 43u) << metadata;
        const auto verifier = form_value(rig.world.requests_for(FetchPurpose::token).front()->body, "code_verifier");
        ASSERT_TRUE(verifier) << metadata;
        EXPECT_EQ(verifier->size(), 43u) << metadata;
        EXPECT_EQ(form_value(query, "response_type"), "code");
        EXPECT_EQ(form_value(query, "redirect_uri"), rig.world.redirect_uri);
        EXPECT_TRUE(form_value(query, "state"));
    }
}

TEST(OAuthFlowFallback, AChallengeScopeIsRequestedAndNoneIsMadeUpOtherwise) {
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"read write\""}))));
        EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"), "read write");
    }
    {
        Rig rig;
        without_resource_metadata(rig);
        rig.config.origin_metadata = declared_paths();
        ASSERT_TRUE(done_of(rig.run(unauthorized())));
        EXPECT_FALSE(form_value(query_of(rig.world.prompts.front().authorization_url), "scope"));
    }
}

TEST(OAuthFlowFallback, ARefreshSendsNoResourceEither) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.token_answers = {json_answer(json{{"access_token", "AccessOne"},
                                                {"token_type", "Bearer"},
                                                {"refresh_token", std::string(planted_refresh_token)}}),
                               json_answer(json{{"access_token", "AccessTwo"}, {"token_type", "Bearer"}})};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const FlowStep step = rig.run(unauthorized({}, true));
    ASSERT_TRUE(done_of(step));
    EXPECT_TRUE(done_of(step)->refreshed);
    const auto tokens = rig.world.requests_for(FetchPurpose::token);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(form_value(tokens[1]->body, "grant_type"), "refresh_token");
    EXPECT_FALSE(form_value(tokens[1]->body, "resource"));
}

TEST(OAuthFlowFallback, StoredTokensServeARequestThatCarriedNone) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const std::size_t before = rig.world.requests.size();
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    EXPECT_FALSE(done_of(step)->refreshed);
    // Only the two resource metadata URLs were asked again: the stored tokens serve the request, and the origin was
    // not needed for them.
    EXPECT_EQ(rig.world.requests.size(), before + 2u);
}

TEST(OAuthFlowFallback, NoFailureCarriesASecret) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    rig.world.token_answers = {json_answer(
        json{{"error", "invalid_grant"}, {"error_description", std::string(planted_code)}}, 400)};
    const FlowStep step = rig.run(unauthorized());
    EXPECT_EQ(problem_of(step), AuthorizationProblem::token_request_failed);
    const auto& failure = std::get<FlowFailed>(step).failure;
    EXPECT_EQ(failure.issuer, "http://localhost:3001");
}

TEST(OAuthFlowFallback, TheEndpointsOriginIsTheOneTheMetadataRequestHeadersGoTo) {
    Rig rig;
    without_resource_metadata(rig);
    rig.config.origin_metadata = declared_paths();
    Trigger trigger = unauthorized();
    trigger.resource_headers.push_back({"MCP-Protocol-Version", "2025-03-26"});
    ASSERT_TRUE(done_of(rig.run(trigger)));
    const auto metadata = std::find_if(rig.world.requests.begin(), rig.world.requests.end(), [](const FlowFetch& one) {
        return one.url == "http://localhost:3001/discover/as-document";
    });
    ASSERT_NE(metadata, rig.world.requests.end());
    EXPECT_EQ(header_value(*metadata, "MCP-Protocol-Version"), "2025-03-26");
    // Not to the registration or token endpoint, which are POSTs of the flow itself.
    for (const FlowFetch* post : rig.world.requests_for(FetchPurpose::token)) {
        EXPECT_FALSE(header_value(*post, "MCP-Protocol-Version"));
    }
}

}  // namespace
