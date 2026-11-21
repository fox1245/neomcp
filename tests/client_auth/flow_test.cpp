// The flow as a whole: refresh, step-up and its bounds, a changed authorization server, the endpoint's scheme, the
// time bound, inputs that do not belong, and what never leaves in a failure.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "neomcp/client/transport.h"
#include "support.h"

namespace {

using namespace oauth_test;

json tokens_with_refresh(std::string access, std::string refresh) {
    return json{{"access_token", std::move(access)}, {"token_type", "Bearer"}, {"refresh_token", std::move(refresh)}};
}

std::size_t authorizations(const Rig& rig) { return rig.world.prompts.size(); }

// The document padded with one member so that its text is exactly `size` bytes long.
std::string document_of_size(json document, std::size_t size) {
    document["pad"] = "";
    const std::size_t base = document.dump().size();
    EXPECT_LE(base, size);
    document["pad"] = std::string(size > base ? size - base : 0, 'p');
    return document.dump();
}

std::string challenge_with_scopes(const std::vector<std::string>& values) {
    return "Bearer scope=\"" + join_scopes(values) + "\"";
}

// The values "s0000", "s0001", ... one after the other.
std::vector<std::string> scope_list(std::size_t count) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string number = std::to_string(i);
        out.push_back("s" + std::string(4 - std::min<std::size_t>(4, number.size()), '0') + number);
    }
    return out;
}

bool reported(const std::vector<FlowDiagnostic>& diagnostics, FlowDiagnostic::Kind kind) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [kind](const FlowDiagnostic& one) { return one.kind == kind; });
}

// A metadata request follows as many redirects as the transport's own requests do by default.
static_assert(OAuthDefaults::max_redirects == neomcp::HttpClientDefaults::max_redirects);

// ---- Refresh

TEST(OAuthFlowRefresh, ARefusedTokenIsRefreshedOnce) {
    Rig rig;
    rig.world.token_answers = {json_answer(tokens_with_refresh("AccessOne", std::string(planted_refresh_token))),
                               json_answer(tokens_with_refresh("AccessTwo", "RefreshTwo"))};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const FlowStep step = rig.run(unauthorized({}, true));
    ASSERT_TRUE(done_of(step));
    EXPECT_TRUE(done_of(step)->refreshed);
    EXPECT_EQ(authorizations(rig), 1u);
    const auto tokens = rig.world.requests_for(FetchPurpose::token);
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(form_value(tokens[1]->body, "grant_type"), "refresh_token");
    EXPECT_EQ(form_value(tokens[1]->body, "refresh_token"), planted_refresh_token);
    EXPECT_EQ(form_value(tokens[1]->body, "resource"), rig.world.endpoint);
    EXPECT_EQ(form_value(tokens[1]->body, "client_id"), "test-client-id");
    EXPECT_FALSE(form_value(tokens[1]->body, "scope"));
    // The new refresh token replaced the old one.
    const StoredTokens& stored = rig.lookup.tokens_by_key.begin()->second;
    EXPECT_EQ(stored.access_token, "AccessTwo");
    EXPECT_EQ(stored.refresh_token, "RefreshTwo");
}

TEST(OAuthFlowRefresh, WithoutANewRefreshTokenTheOldOneStays) {
    Rig rig;
    rig.world.token_answers = {json_answer(tokens_with_refresh("AccessOne", "RefreshOne")),
                               json_answer(json{{"access_token", "AccessTwo"}, {"token_type", "Bearer"}})};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    ASSERT_TRUE(done_of(rig.run(unauthorized({}, true))));
    EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.refresh_token, "RefreshOne");
}

// A request that went without a token (the first one of an application that keeps its tokens in a store) is served by
// the tokens stored for the resource and the authorization server: no refresh and no new authorization, only the
// protected resource metadata that names the server.
TEST(OAuthFlowRefresh, StoredTokensServeARequestThatCarriedNone) {
    Rig rig;
    rig.world.token_answers = {json_answer(tokens_with_refresh("AccessOne", "RefreshOne"))};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    rig.memory = FlowMemory{};  // a new authorizer with the same store
    const std::size_t before = rig.world.requests.size();
    const FlowStep step = rig.run(unauthorized({}, false));
    ASSERT_TRUE(done_of(step));
    EXPECT_FALSE(done_of(step)->refreshed);
    EXPECT_EQ(done_of(step)->key, (CredentialKey{rig.world.endpoint, rig.world.issuer}));
    EXPECT_EQ(authorizations(rig), 1u);
    ASSERT_EQ(rig.world.requests.size(), before + 1);
    EXPECT_EQ(rig.world.requests.back().purpose, FetchPurpose::protected_resource_metadata);
    EXPECT_EQ(rig.memory.active, (CredentialKey{rig.world.endpoint, rig.world.issuer}));
    // Once those are refused too, the refresh follows.
    const FlowStep refreshed = rig.run(unauthorized({}, true));
    ASSERT_TRUE(done_of(refreshed));
    EXPECT_TRUE(done_of(refreshed)->refreshed);
}

// A step-up is never served by the stored tokens: they are the ones the server found too narrow.
TEST(OAuthFlowRefresh, AStepUpIsNotServedByStoredTokens) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"a\""}))));
    Trigger step_up = insufficient_scope("b");
    step_up.carried_token = false;
    ASSERT_TRUE(done_of(rig.run(step_up)));
    EXPECT_EQ(authorizations(rig), 2u);
}

TEST(OAuthFlowRefresh, ARefusedRefreshErasesItAndTheFullFlowFollows) {
    Rig rig;
    rig.world.token_answers = {json_answer(tokens_with_refresh("AccessOne", "RefreshOne")),
                               json_answer(json{{"error", "invalid_grant"}}, 400),
                               json_answer(json{{"access_token", "AccessThree"}, {"token_type", "Bearer"}})};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const FlowStep step = rig.run(unauthorized({}, true));
    ASSERT_TRUE(done_of(step));
    EXPECT_FALSE(done_of(step)->refreshed);
    EXPECT_EQ(authorizations(rig), 2u);
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::token).size(), 3u);
    const StoredTokens& stored = rig.lookup.tokens_by_key.begin()->second;
    EXPECT_EQ(stored.access_token, "AccessThree");
    EXPECT_FALSE(stored.refresh_token);
}

TEST(OAuthFlowRefresh, AServerErrorOnRefreshEndsTheFlow) {
    Rig rig;
    rig.world.token_answers = {json_answer(tokens_with_refresh("AccessOne", "RefreshOne")),
                               answer(503, "", "text/plain")};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_EQ(problem_of(rig.run(unauthorized({}, true))), AuthorizationProblem::token_request_failed);
    EXPECT_EQ(authorizations(rig), 1u);
}

TEST(OAuthFlowRefresh, AForgottenRegistrationIsErased) {
    Rig rig;
    rig.world.token_answers = {json_answer(tokens_with_refresh("AccessOne", "RefreshOne")),
                               json_answer(json{{"error", "invalid_client"}}, 401),
                               json_answer(json{{"access_token", "AccessThree"}, {"token_type", "Bearer"}})};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    ASSERT_TRUE(done_of(rig.run(unauthorized({}, true))));
    // The flow registered anew instead of using the registration the server refused.
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::registration).size(), 2u);
}

TEST(OAuthFlowRefresh, InvalidClientAtTheTokenEndpointErasesTheRegistration) {
    Rig rig;
    rig.world.token_answers = {json_answer(json{{"error", "invalid_client"}}, 401)};
    const FlowStep step = rig.run(unauthorized());
    ASSERT_EQ(problem_of(step), AuthorizationProblem::token_request_failed);
    const auto& updates = std::get<FlowFailed>(step).updates;
    EXPECT_FALSE(updates.put_client);
    EXPECT_TRUE(rig.lookup.clients.empty());
    rig.world.token_answers = {json_answer(json{{"access_token", "A"}, {"token_type", "Bearer"}})};
    rig.world.token_answer_index = 0;
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::registration).size(), 2u);
}

// ---- Step-up

TEST(OAuthFlowStepUp, TheUnionOfTheLastRequestAndTheChallenge) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic\""}))));
    const FlowStep step = rig.run(insufficient_scope("mcp:write"));
    ASSERT_TRUE(done_of(step));
    ASSERT_EQ(authorizations(rig), 2u);
    EXPECT_EQ(form_value(query_of(rig.world.prompts[1].authorization_url), "scope"), "mcp:basic mcp:write");
    EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.requested_scopes,
              (std::vector<std::string>{"mcp:basic", "mcp:write"}));
    // A step-up never refreshes.
    EXPECT_EQ(form_value(rig.world.requests_for(FetchPurpose::token).back()->body, "grant_type"),
              "authorization_code");
}

TEST(OAuthFlowStepUp, NothingNewMeansNoNewAuthorization) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic mcp:write\""}))));
    const std::size_t requests = rig.world.requests.size();
    for (const char* scope : {"mcp:basic", "mcp:write mcp:basic", ""}) {
        EXPECT_EQ(problem_of(rig.run(insufficient_scope(scope))), AuthorizationProblem::insufficient_scope) << scope;
    }
    EXPECT_EQ(rig.world.requests.size(), requests);
    EXPECT_EQ(authorizations(rig), 1u);
}

TEST(OAuthFlowStepUp, AnotherForbiddenIsNotAboutScope) {
    for (const std::vector<std::string>& challenges :
         std::vector<std::vector<std::string>>{{}, {"Bearer error=\"invalid_token\""}, {"Basic realm=\"x\""},
                                               {"Bearer error=\"Insufficient_Scope\", scope=\"x\""}}) {
        Rig rig;
        Trigger trigger = insufficient_scope("x");
        trigger.challenges = challenges;
        EXPECT_EQ(problem_of(rig.run(trigger)), AuthorizationProblem::forbidden);
        EXPECT_TRUE(rig.world.requests.empty());
    }
}

TEST(OAuthFlowStepUp, AnOperationMayStartABoundedNumberOfFlowsInARow) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"a\""}))));
    ASSERT_TRUE(done_of(rig.run(insufficient_scope("b"))));
    ASSERT_TRUE(done_of(rig.run(insufficient_scope("c"))));
    // One more than max_step_ups_per_operation flows were made; the next is refused before any request.
    const std::size_t requests = rig.world.requests.size();
    EXPECT_EQ(problem_of(rig.run(insufficient_scope("d"))), AuthorizationProblem::attempts_exceeded);
    EXPECT_EQ(rig.world.requests.size(), requests);
    EXPECT_EQ(authorizations(rig), 3u);
    // Another operation has its own count, and a success resets an operation's.
    Trigger other = insufficient_scope("d");
    other.operation = "POST tools/call other-tool";
    EXPECT_TRUE(done_of(rig.run(other)));
    rig.memory.succeeded("POST tools/call test-tool");
    EXPECT_TRUE(done_of(rig.run(insufficient_scope("e"))));
}

TEST(OAuthFlowStepUp, TheBoundIsAnOption) {
    Rig rig;
    rig.config.bounds.max_step_ups_per_operation = 0;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"a\""}))));
    EXPECT_EQ(problem_of(rig.run(insufficient_scope("b"))), AuthorizationProblem::attempts_exceeded);
}

TEST(OAuthFlowStepUp, EndlessRefusalsMakeAtMostThreeAuthorizations) {
    // The server answers every request with 401 and then 403 for the same scope, forever.
    Rig rig;
    std::size_t ended = 0;
    for (int i = 0; i < 10; ++i) {
        const FlowStep step = rig.run(i == 0 ? unauthorized({"Bearer scope=\"mcp:basic\""})
                                             : insufficient_scope("mcp:basic mcp:write"));
        if (std::holds_alternative<FlowFailed>(step)) ++ended;
    }
    EXPECT_LE(authorizations(rig), 3u);
    EXPECT_EQ(ended, 8u);
}

TEST(OAuthFlowStepUp, ARequestIsHeldABoundedNumberOfTimes) {
    Rig rig;
    Trigger trigger = unauthorized();
    trigger.refusals = OAuthDefaults::max_authorizations_per_request - 1;
    EXPECT_TRUE(done_of(rig.run(trigger)));
    Rig again;
    trigger.refusals = OAuthDefaults::max_authorizations_per_request;
    EXPECT_EQ(problem_of(again.run(trigger)), AuthorizationProblem::attempts_exceeded);
    EXPECT_TRUE(again.world.requests.empty());
}

// ---- A changed authorization server

TEST(OAuthFlowMigration, TokensAndRegistrationsOfTheOldServerAreDropped) {
    Rig rig;
    rig.world.token_answers = {json_answer(tokens_with_refresh(std::string(planted_access_token), "RefreshOne"))};
    rig.world.registration = json_answer(json{{"client_id", "as1-client"}}, 201);
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    ASSERT_EQ(rig.lookup.clients.count("http://localhost:3002"), 1u);

    // The resource now names another authorization server.
    const std::string as2 = "http://localhost:3004";
    World& world = rig.world;
    world.documents[world.resource_metadata_path_url()] =
        json_answer(json{{"resource", world.endpoint}, {"authorization_servers", json::array({as2})}});
    json document = world.server_document();
    document["issuer"] = as2;
    document["authorization_endpoint"] = as2 + "/authorize";
    document["token_endpoint"] = as2 + "/token";
    document["registration_endpoint"] = as2 + "/register";
    world.documents[as2 + "/.well-known/oauth-authorization-server"] = json_answer(document);
    world.registrations_by_url[as2 + "/register"] = json_answer(json{{"client_id", "as2-client"}}, 201);
    world.token_answers = {json_answer(json{{"access_token", "AccessFromTwo"}, {"token_type", "Bearer"}})};
    world.token_answer_index = 0;
    const std::size_t before = world.requests.size();

    const FlowStep step = rig.run(unauthorized({}, true));
    ASSERT_TRUE(done_of(step));
    EXPECT_EQ(done_of(step)->key, (CredentialKey{world.endpoint, as2}));
    const StoreUpdate& updates = done_of(step)->updates;
    EXPECT_EQ(updates.erase_tokens, (std::vector<CredentialKey>{{world.endpoint, "http://localhost:3002"}}));
    EXPECT_EQ(updates.erase_clients, std::vector<std::string>{"http://localhost:3002"});
    // No refresh at the new server with the old server's token, a registration there, and the old client id nowhere.
    bool registered_there = false;
    for (std::size_t i = before; i < world.requests.size(); ++i) {
        const FlowFetch& one = world.requests[i];
        EXPECT_EQ(one.url.find("localhost:3002"), std::string::npos) << one.url;
        EXPECT_EQ(one.body.find("as1-client"), std::string::npos);
        EXPECT_EQ(one.body.find("RefreshOne"), std::string::npos);
        if (one.purpose == FetchPurpose::token) {
            EXPECT_EQ(form_value(one.body, "grant_type"), "authorization_code");
        }
        if (one.purpose == FetchPurpose::registration && one.url == as2 + "/register") registered_there = true;
    }
    EXPECT_TRUE(registered_there);
    ASSERT_EQ(world.prompts.size(), 2u);
    EXPECT_EQ(form_value(query_of(world.prompts[1].authorization_url), "client_id"), "as2-client");
    EXPECT_EQ(rig.lookup.tokens_by_key.size(), 1u);
    EXPECT_EQ(rig.lookup.clients.count("http://localhost:3002"), 0u);
    EXPECT_EQ(rig.memory.active, (CredentialKey{world.endpoint, as2}));
}

TEST(OAuthFlowMigration, AChangeOfServerNeverRefreshes) {
    // The store holds tokens of both servers (kept from an earlier run); the ones in use are the first server's.
    Rig rig;
    const std::string as2 = "http://localhost:3004";
    World& world = rig.world;
    const CredentialKey first{world.endpoint, world.issuer};
    const CredentialKey second{world.endpoint, as2};
    StoredTokens kept;
    kept.access_token = "KeptAccess";
    kept.refresh_token = std::string(planted_refresh_token);
    rig.lookup.tokens_by_key[first] = kept;
    rig.lookup.tokens_by_key[second] = kept;
    rig.memory.active = first;
    // A client the refresh could authenticate with.
    rig.config.registration.pre_registered.push_back({as2, "pre-id", std::nullopt, std::nullopt});
    world.documents[world.resource_metadata_path_url()] =
        json_answer(json{{"resource", world.endpoint}, {"authorization_servers", json::array({as2})}});
    json document = world.server_document();
    document["issuer"] = as2;
    document["authorization_endpoint"] = as2 + "/authorize";
    document["token_endpoint"] = as2 + "/token";
    document["registration_endpoint"] = as2 + "/register";
    world.documents[as2 + "/.well-known/oauth-authorization-server"] = json_answer(document);
    const FlowStep step = rig.run(unauthorized({}, true));
    ASSERT_TRUE(done_of(step));
    EXPECT_FALSE(done_of(step)->refreshed);
    for (const FlowFetch& one : world.requests) {
        EXPECT_EQ(one.body.find(planted_refresh_token), std::string::npos) << one.url;
    }
    EXPECT_EQ(rig.lookup.tokens_by_key.count(first), 0u);
}

TEST(OAuthFlowMigration, AStepUpAtANewServerStartsFromTheChallenge) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic\""}))));
    const std::string as2 = "http://localhost:3004";
    World& world = rig.world;
    world.documents[world.resource_metadata_path_url()] =
        json_answer(json{{"resource", world.endpoint}, {"authorization_servers", json::array({as2})}});
    json document = world.server_document();
    document["issuer"] = as2;
    document["authorization_endpoint"] = as2 + "/authorize";
    document["token_endpoint"] = as2 + "/token";
    document["registration_endpoint"] = as2 + "/register";
    world.documents[as2 + "/.well-known/oauth-authorization-server"] = json_answer(document);
    ASSERT_TRUE(done_of(rig.run(insufficient_scope("mcp:write"))));
    EXPECT_EQ(form_value(query_of(world.prompts[1].authorization_url), "scope"), "mcp:write");
}

// ---- The endpoint, the time bound, and inputs that do not belong

TEST(OAuthFlowRun, NoFlowForPlainHttpToAnotherHost) {
    for (const char* endpoint : {"http://mcp.example/mcp", "http://10.0.0.1:3001/mcp", "ftp://mcp.example/mcp"}) {
        Rig rig;
        rig.config.endpoint_url = endpoint;
        EXPECT_EQ(problem_of(rig.run(unauthorized())), AuthorizationProblem::insecure_endpoint) << endpoint;
        EXPECT_TRUE(rig.world.requests.empty());
    }
}

TEST(OAuthFlowRun, TheWholeFlowIsBoundInTime) {
    World world;
    FlowConfig config;
    config.endpoint_url = world.endpoint;
    config.registration.redirect_uri = world.redirect_uri;
    TestAdmission admission(world.endpoint);
    MapLookup lookup;
    CountingRandom random;
    FlowMemory memory;
    const auto t0 = test_time();
    {
        OAuthFlow flow(config, admission, lookup, random.source(), memory);
        FlowStep step = flow.start(unauthorized(), t0);
        auto* fetch = std::get_if<FlowFetch>(&step);
        ASSERT_NE(fetch, nullptr);
        step = flow.fetched(fetch->id, world.respond(*fetch), t0 + OAuthDefaults::authorization_timeout);
        EXPECT_TRUE(std::holds_alternative<FlowFetch>(step));
        fetch = std::get_if<FlowFetch>(&step);
        ASSERT_NE(fetch, nullptr);
        step = flow.fetched(fetch->id, world.respond(*fetch),
                            t0 + OAuthDefaults::authorization_timeout + std::chrono::seconds(1));
        ASSERT_TRUE(std::holds_alternative<FlowFailed>(step));
        EXPECT_EQ(std::get<FlowFailed>(step).failure.problem, AuthorizationProblem::timed_out);
        EXPECT_TRUE(flow.finished());
    }
    {
        // The prompt counts too.
        FlowMemory fresh;
        OAuthFlow flow(config, admission, lookup, random.source(), fresh);
        FlowStep step = flow.start(unauthorized(), t0);
        while (auto* fetch = std::get_if<FlowFetch>(&step)) step = flow.fetched(fetch->id, world.respond(*fetch), t0);
        auto* prompt = std::get_if<PromptRequest>(&step);
        ASSERT_NE(prompt, nullptr);
        step = flow.prompted(world.redirect(*prompt), t0 + std::chrono::hours(1));
        ASSERT_TRUE(std::holds_alternative<FlowFailed>(step));
        EXPECT_EQ(std::get<FlowFailed>(step).failure.problem, AuthorizationProblem::timed_out);
    }
}

TEST(OAuthFlowRun, InputsThatDoNotBelongChangeNothing) {
    World world;
    FlowConfig config;
    config.endpoint_url = world.endpoint;
    config.registration.redirect_uri = world.redirect_uri;
    TestAdmission admission(world.endpoint);
    MapLookup lookup;
    CountingRandom random;
    FlowMemory memory;
    const auto now = test_time();
    OAuthFlow flow(config, admission, lookup, random.source(), memory);
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.fetched(1, not_found(), now)));
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.prompted(PromptAnswer{}, now)));
    FlowStep step = flow.start(unauthorized(), now);
    auto* fetch = std::get_if<FlowFetch>(&step);
    ASSERT_NE(fetch, nullptr);
    const FlowFetch first = *fetch;
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.start(unauthorized(), now)));
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.fetched(first.id + 1, not_found(), now)));
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.fetched(0, not_found(), now)));
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.prompted(PromptAnswer{}, now)));
    step = flow.fetched(first.id, world.respond(first), now);
    ASSERT_TRUE(std::holds_alternative<FlowFetch>(step));
    // The answer to the first request does not count twice.
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.fetched(first.id, world.respond(first), now)));
    while (auto* next = std::get_if<FlowFetch>(&step)) step = flow.fetched(next->id, world.respond(*next), now);
    auto* prompt = std::get_if<PromptRequest>(&step);
    ASSERT_NE(prompt, nullptr);
    step = flow.prompted(world.redirect(*prompt), now);
    while (auto* next = std::get_if<FlowFetch>(&step)) step = flow.fetched(next->id, world.respond(*next), now);
    ASSERT_TRUE(std::holds_alternative<FlowDone>(step));
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.prompted(PromptAnswer{}, now)));
    EXPECT_TRUE(std::holds_alternative<FlowIgnored>(flow.fetched(1, not_found(), now)));
}

TEST(OAuthFlowRun, ACompleteFlowInTheRunnersShape) {
    Rig rig;
    const FlowStep step =
        rig.run(unauthorized({"Bearer error=\"invalid_token\", resource_metadata=\""
                              "http://localhost:3001/.well-known/oauth-protected-resource/mcp\""}));
    ASSERT_TRUE(done_of(step));
    std::vector<FetchPurpose> purposes;
    for (const auto& one : rig.world.requests) purposes.push_back(one.purpose);
    EXPECT_EQ(purposes, (std::vector<FetchPurpose>{FetchPurpose::protected_resource_metadata,
                                                   FetchPurpose::authorization_server_metadata,
                                                   FetchPurpose::registration, FetchPurpose::token}));
    const std::string query = query_of(rig.world.prompts.front().authorization_url);
    EXPECT_EQ(form_value(query, "response_type"), "code");
    EXPECT_EQ(form_value(query, "redirect_uri"), rig.world.redirect_uri);
    EXPECT_EQ(form_value(query, "resource"), rig.world.endpoint);
    const FlowFetch& token = rig.world.requests.back();
    EXPECT_EQ(form_value(token.body, "code"), planted_code);
    EXPECT_EQ(form_value(token.body, "redirect_uri"), rig.world.redirect_uri);
    EXPECT_EQ(form_value(token.body, "resource"), form_value(query, "resource"));
    const StoredTokens& stored = rig.lookup.tokens_by_key.at(done_of(step)->key);
    EXPECT_EQ(stored.access_token, planted_access_token);
    EXPECT_EQ(stored.expires_at, test_time() + std::chrono::seconds(3600));
    EXPECT_EQ(rig.memory.active, done_of(step)->key);
    // The code went only to the token endpoint.
    for (const auto& one : rig.world.requests) {
        if (one.purpose == FetchPurpose::token) continue;
        EXPECT_EQ(one.body.find(planted_code), std::string::npos);
        EXPECT_EQ(one.url.find(planted_code), std::string::npos);
    }
}

TEST(OAuthFlowRun, NoFailureOrDiagnosticCarriesASecret) {
    std::vector<std::function<void(Rig&)>> setups = {
        [](Rig& rig) { rig.world.token_answers = {json_answer(json{{"error", "invalid_grant"},
                                                                   {"error_description", std::string(planted_code)}},
                                                              400)}; },
        [](Rig& rig) { rig.world.callback_iss = std::string("http://evil.example/") + std::string(planted_code); },
        [](Rig& rig) { rig.world.callback_extra = "&state=" + std::string(planted_code); },
        [](Rig& rig) {
            rig.world.token_answers = {json_answer(json{{"access_token", std::string(planted_access_token)},
                                                        {"token_type", "DPoP"}})};
        },
        [](Rig& rig) {
            rig.world.callback_extra =
                "&error=access_denied&error_description=" + std::string(planted_client_secret) + "&error_uri=" +
                std::string(planted_refresh_token);
        },
    };
    for (std::size_t i = 0; i < setups.size(); ++i) {
        SCOPED_TRACE(i);
        Rig rig;
        rig.config.registration.pre_registered.push_back(
            {rig.world.issuer, "pre-id", std::string(planted_client_secret), std::nullopt});
        setups[i](rig);
        const FlowStep step = rig.run(unauthorized());
        ASSERT_TRUE(std::holds_alternative<FlowFailed>(step));
        std::vector<std::string> also;
        if (!rig.world.prompts.empty()) {
            const std::string query = query_of(rig.world.prompts.front().authorization_url);
            also.push_back(form_value(query, "state").value_or(""));
            also.push_back(form_value(query, "code_challenge").value_or(""));
        }
        expect_clean(std::get<FlowFailed>(step).failure, also);
        for (const FlowDiagnostic& diagnostic : rig.diagnostics) {
            for (const std::string_view secret : {planted_access_token, planted_refresh_token, planted_code,
                                                  planted_client_secret}) {
                EXPECT_EQ(diagnostic.text.find(secret), std::string::npos);
            }
        }
    }
}

// The size check follows the answer, not its status: an oversized refusal of a refresh ends the flow with the bound's
// problem and keeps the refresh token, instead of counting as the server's refusal of it.
TEST(OAuthFlowRefresh, AnOversizedRefusalOfARefreshKeepsTheRefreshToken) {
    const std::string body = document_of_size(json{{"error", "invalid_grant"}}, 300);
    Rig rig;
    rig.world.token_answers = {
        json_answer(json{{"access_token", "AccessOne"}, {"token_type", "Bearer"}, {"refresh_token", "RefreshOne"}}),
        answer(400, body)};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    rig.config.bounds.max_token_response_bytes = body.size() - 1;
    const std::size_t prompts = rig.world.prompts.size();
    const FlowStep step = rig.run(unauthorized({}, true));
    EXPECT_EQ(problem_of(step), AuthorizationProblem::document_too_large);
    EXPECT_EQ(rig.world.prompts.size(), prompts) << "a new sign-in was started";
    ASSERT_EQ(rig.lookup.tokens_by_key.size(), 1u);
    EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.refresh_token, "RefreshOne");
}

// A refresh token is held to the same bound as the access token. One over it is a token answer that cannot be used:
// the sign-in ends with token_request_failed and nothing of the answer is kept, so that no stored token is replaced by
// a half of an answer. On a refresh the stored refresh token stays what it was.
TEST(OAuthFlowRefresh, ARefreshTokenAtTheTokenBoundIsKeptAndOneByteLongerRefusesTheAnswer) {
    for (const std::ptrdiff_t shift : {-1, 0, 1}) {
        SCOPED_TRACE(shift);
        Rig rig;
        rig.config.bounds.max_token_bytes = 64;
        const std::string refresh(static_cast<std::size_t>(64 + shift), 'r');
        rig.world.token_answers = {
            json_answer(json{{"access_token", "AccessOne"}, {"token_type", "Bearer"}, {"refresh_token", refresh}})};
        const FlowStep step = rig.run(unauthorized());
        if (shift <= 0) {
            ASSERT_TRUE(done_of(step));
            EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.refresh_token, refresh);
        } else {
            EXPECT_EQ(problem_of(step), AuthorizationProblem::token_request_failed);
            EXPECT_TRUE(rig.lookup.tokens_by_key.empty());
        }
    }
}

TEST(OAuthFlowRefresh, ANewRefreshTokenOverTheBoundOnARefreshLeavesTheStoredOneAlone) {
    Rig rig;
    rig.config.bounds.max_token_bytes = 64;
    rig.world.token_answers = {
        json_answer(json{{"access_token", "AccessOne"}, {"token_type", "Bearer"}, {"refresh_token", "RefreshOne"}}),
        json_answer(json{{"access_token", "AccessTwo"},
                         {"token_type", "Bearer"},
                         {"refresh_token", std::string(65, 'r')}})};
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    const FlowStep step = rig.run(unauthorized({}, true));
    EXPECT_EQ(problem_of(step), AuthorizationProblem::token_request_failed);
    ASSERT_EQ(rig.lookup.tokens_by_key.size(), 1u);
    EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.access_token, "AccessOne");
    EXPECT_EQ(rig.lookup.tokens_by_key.begin()->second.refresh_token, "RefreshOne");
}

// A step-up asks for the scopes of the last authorization and the challenge's together; that union is bound like any
// list, so that repeated step-ups cannot grow what is asked past the bounds. A union that the bound cuts back to the
// last request asks for nothing more, and no sign-in is made.
TEST(OAuthFlowStepUp, AStepUpNeverAsksForMoreThanTheBoundsAllow) {
    {
        Rig rig;
        rig.config.bounds.max_scopes = 4;
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge_with_scopes({"a", "b", "c"})}))));
        ASSERT_TRUE(done_of(rig.run(insufficient_scope("d"))));  // four: at the bound
        EXPECT_EQ(rig.world.prompts.back().scopes, (std::vector<std::string>{"a", "b", "c", "d"}));
        const std::size_t prompts = rig.world.prompts.size();
        EXPECT_EQ(problem_of(rig.run(insufficient_scope("e"))), AuthorizationProblem::insufficient_scope);
        EXPECT_EQ(rig.world.prompts.size(), prompts);
    }
    {
        // By bytes.
        Rig rig;
        rig.config.bounds.max_scope_bytes = 5;
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge_with_scopes({"a", "b"})}))));
        ASSERT_TRUE(done_of(rig.run(insufficient_scope("c"))));  // "a b c" is five bytes
        EXPECT_EQ(rig.world.prompts.back().scopes, (std::vector<std::string>{"a", "b", "c"}));
        const std::size_t prompts = rig.world.prompts.size();
        EXPECT_EQ(problem_of(rig.run(insufficient_scope("d"))), AuthorizationProblem::insufficient_scope);
        EXPECT_EQ(rig.world.prompts.size(), prompts);
    }
}

// The scope the library adds itself is inside the bounds too: the values the servers named give way to it.
TEST(OAuthFlowStepUp, TheOfflineAccessScopeIsInsideTheBoundsToo) {
    for (const std::size_t count : {std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
        SCOPED_TRACE(count);
        Rig rig;
        rig.config.bounds.max_scopes = 3;
        rig.config.request_offline_access = true;
        json server = rig.world.server_document();
        server["scopes_supported"] = json::array({"offline_access"});
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(server);
        ASSERT_TRUE(done_of(rig.run(unauthorized({challenge_with_scopes(scope_list(count))}))));
        std::vector<std::string> expected = scope_list(std::min<std::size_t>(count, 2));
        expected.emplace_back("offline_access");
        EXPECT_EQ(rig.world.prompts.front().scopes, expected);
        EXPECT_EQ(reported(rig.diagnostics, FlowDiagnostic::Kind::scopes_truncated), count > 2);
    }
}

}  // namespace
