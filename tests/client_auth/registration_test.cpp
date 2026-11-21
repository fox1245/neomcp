// Client registration: the order of the mechanisms, the dynamic registration request and its answer, the method of
// client authentication, and the URLs the application gives.
#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;

constexpr std::string_view sent_redirect_uri = "http://127.0.0.1:3000/callback";

AuthorizationServer server_with(std::optional<std::vector<std::string>> methods, bool cimd = false,
                                bool registration = true) {
    AuthorizationServer server;
    server.issuer = "https://as.example";
    server.authorization_endpoint = "https://as.example/authorize";
    server.token_endpoint = "https://as.example/token";
    if (registration) server.registration_endpoint = "https://as.example/register";
    server.token_endpoint_auth_methods_supported = std::move(methods);
    server.client_id_metadata_document_supported = cimd;
    return server;
}

RegistrationConfig config_with() {
    RegistrationConfig config;
    config.redirect_uri = "http://127.0.0.1:3000/callback";
    config.client_name = "test client";
    return config;
}

TEST(OAuthFlowRegistration, PreRegisteredCredentialsForThisIssuerComeFirst) {
    RegistrationConfig config = config_with();
    config.client_id_metadata_url = "https://app.example/client.json";
    config.pre_registered.push_back({"https://other.example", "other-id", std::string("other-secret"), std::nullopt});
    config.pre_registered.push_back({"https://as.example", "pre-id", std::string("pre-secret"), std::nullopt});
    auto choice = choose_registration(server_with(std::nullopt, true), config, StoredClient{"dyn-id", {}, {}});
    ASSERT_TRUE(std::holds_alternative<ClientCredentials>(choice));
    const auto& client = std::get<ClientCredentials>(choice);
    EXPECT_EQ(client.client_id, "pre-id");
    EXPECT_EQ(client.source, RegistrationSource::pre_registered);
    // With a secret and no list of methods: client_secret_basic.
    EXPECT_EQ(client.method, TokenEndpointAuth::client_secret_basic);
}

TEST(OAuthFlowRegistration, PreRegisteredIssuersCompareExactly) {
    for (const char* issuer :
         {"https://as.example/", "HTTPS://as.example", "https://AS.example", "https://as.example:443"}) {
        RegistrationConfig config = config_with();
        config.dynamic_registration = false;
        config.pre_registered.push_back({issuer, "pre-id", std::string("pre-secret"), std::nullopt});
        auto choice = choose_registration(server_with(std::nullopt), config, std::nullopt);
        ASSERT_TRUE(std::holds_alternative<FlowFailure>(choice)) << issuer;
        EXPECT_EQ(std::get<FlowFailure>(choice).problem, AuthorizationProblem::no_client_registration);
        EXPECT_TRUE(std::get<FlowFailure>(choice).credentials_for_other_issuer);
    }
}

TEST(OAuthFlowRegistration, APreRegisteredMethodIsUsedAsItIs) {
    RegistrationConfig config = config_with();
    config.pre_registered.push_back(
        {"https://as.example", "pre-id", std::string("pre-secret"), TokenEndpointAuth::client_secret_post});
    auto choice =
        choose_registration(server_with(std::vector<std::string>{"client_secret_basic"}), config, std::nullopt);
    ASSERT_TRUE(std::holds_alternative<ClientCredentials>(choice));
    EXPECT_EQ(std::get<ClientCredentials>(choice).method, TokenEndpointAuth::client_secret_post);
}

TEST(OAuthFlowRegistration, TheMetadataDocumentUrlWhenTheServerSupportsIt) {
    RegistrationConfig config = config_with();
    config.client_id_metadata_url = "https://conformance-test.local/client-metadata.json";
    auto choice = choose_registration(server_with(std::vector<std::string>{"client_secret_basic"}, true), config,
                                      StoredClient{"dyn-id", std::string("s"), {}});
    ASSERT_TRUE(std::holds_alternative<ClientCredentials>(choice));
    const auto& client = std::get<ClientCredentials>(choice);
    EXPECT_EQ(client.client_id, "https://conformance-test.local/client-metadata.json");
    EXPECT_EQ(client.method, TokenEndpointAuth::none);
    EXPECT_FALSE(client.client_secret);

    // Not supported: the stored dynamic registration, then a new one.
    auto stored = choose_registration(server_with(std::nullopt, false), config, StoredClient{"dyn-id", {}, {}});
    ASSERT_TRUE(std::holds_alternative<ClientCredentials>(stored));
    EXPECT_EQ(std::get<ClientCredentials>(stored).client_id, "dyn-id");
    EXPECT_EQ(std::get<ClientCredentials>(stored).source, RegistrationSource::dynamic_stored);
    EXPECT_TRUE(std::holds_alternative<NeedsRegistration>(choose_registration(server_with(std::nullopt), config,
                                                                              std::nullopt)));
}

TEST(OAuthFlowRegistration, DynamicRegistrationNeedsTheOptionAndAnEndpoint) {
    RegistrationConfig config = config_with();
    EXPECT_TRUE(std::holds_alternative<NeedsRegistration>(choose_registration(server_with(std::nullopt), config,
                                                                              std::nullopt)));
    auto no_endpoint = choose_registration(server_with(std::nullopt, false, false), config, std::nullopt);
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(no_endpoint));
    EXPECT_EQ(std::get<FlowFailure>(no_endpoint).problem, AuthorizationProblem::no_client_registration);
    EXPECT_FALSE(std::get<FlowFailure>(no_endpoint).credentials_for_other_issuer);
    config.dynamic_registration = false;
    auto off = choose_registration(server_with(std::nullopt), config, std::nullopt);
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(off));
}

TEST(OAuthFlowRegistration, TheMethodOfClientAuthentication) {
    using M = TokenEndpointAuth;
    using L = std::optional<std::vector<std::string>>;
    const auto only = [](const char* name) { return L(std::vector<std::string>{name}); };
    // The registration's own method, when listed or nothing is listed.
    EXPECT_EQ(choose_token_endpoint_auth(M::client_secret_post, true, only("client_secret_post")),
              M::client_secret_post);
    EXPECT_EQ(choose_token_endpoint_auth(M::client_secret_post, true, std::nullopt), M::client_secret_post);
    EXPECT_EQ(choose_token_endpoint_auth(M::none, false, only("none")), M::none);
    // Not listed: basic if listed, then post, else none.
    EXPECT_EQ(choose_token_endpoint_auth(M::client_secret_post, true, only("client_secret_basic")),
              M::client_secret_basic);
    EXPECT_EQ(choose_token_endpoint_auth(M::client_secret_basic, true, only("client_secret_post")),
              M::client_secret_post);
    EXPECT_EQ(choose_token_endpoint_auth(std::nullopt, true, only("none")), M::none);
    // Nothing listed means client_secret_basic.
    EXPECT_EQ(choose_token_endpoint_auth(std::nullopt, true, std::nullopt), M::client_secret_basic);
    // Never a method that needs a secret without one.
    EXPECT_EQ(choose_token_endpoint_auth(M::client_secret_basic, false, std::nullopt), M::none);
    EXPECT_EQ(choose_token_endpoint_auth(std::nullopt, false, only("client_secret_basic")), M::none);
}

TEST(OAuthFlowRegistration, TheRegistrationRequest) {
    RegistrationConfig config = config_with();
    const json body = json::parse(registration_request_body(config));
    EXPECT_EQ(body["redirect_uris"], json::array({"http://127.0.0.1:3000/callback"}));
    EXPECT_EQ(body["grant_types"], json::array({"authorization_code", "refresh_token"}));
    EXPECT_EQ(body["response_types"], json::array({"code"}));
    EXPECT_EQ(body["token_endpoint_auth_method"], "none");
    EXPECT_EQ(body["application_type"], "native");
    EXPECT_EQ(body["client_name"], "test client");
    EXPECT_FALSE(body.contains("client_secret"));
    EXPECT_FALSE(body.contains("scope"));

    config.redirect_uri = "https://app.example/callback";
    EXPECT_EQ(json::parse(registration_request_body(config))["application_type"], "web");
    config.application_type = ApplicationType::native;
    EXPECT_EQ(json::parse(registration_request_body(config))["application_type"], "native");
    config.client_name.clear();
    EXPECT_FALSE(json::parse(registration_request_body(config)).contains("client_name"));
}

TEST(OAuthFlowRegistration, TheRegistrationAnswer) {
    const OAuthBounds bounds;
    auto ok = read_registration_answer(
        json_answer(json{{"client_id", "id-1"}, {"client_secret", "sec"},
                         {"token_endpoint_auth_method", "client_secret_basic"}}, 201),
        "https://as.example", "https://as.example/register", sent_redirect_uri, bounds);
    ASSERT_TRUE(std::holds_alternative<StoredClient>(ok));
    EXPECT_EQ(std::get<StoredClient>(ok),
              (StoredClient{"id-1", std::string("sec"), TokenEndpointAuth::client_secret_basic}));
    auto plain = read_registration_answer(json_answer(json{{"client_id", "id-2"}, {"token_endpoint_auth_method", "x"}}),
                                          "https://as.example", "https://as.example/register", sent_redirect_uri, bounds);
    ASSERT_TRUE(std::holds_alternative<StoredClient>(plain));
    EXPECT_EQ(std::get<StoredClient>(plain), (StoredClient{"id-2", std::nullopt, std::nullopt}));

    struct Case {
        FetchResult result;
        AuthorizationProblem problem;
    };
    const std::vector<Case> refused = {
        {json_answer(json{{"error", "invalid_redirect_uri"}, {"error_description", "Planted text"}}, 400),
         AuthorizationProblem::registration_rejected},
        {json_answer(json{{"client_secret", "s"}}, 201), AuthorizationProblem::registration_rejected},
        {json_answer(json{{"client_id", ""}}, 201), AuthorizationProblem::registration_rejected},
        {json_answer(json{{"client_id", 5}}, 201), AuthorizationProblem::registration_rejected},
        {answer(201, "{\"client_id\":\"x\"}", "text/plain"), AuthorizationProblem::registration_rejected},
        {answer(302, "", "text/plain"), AuthorizationProblem::registration_rejected},
        {answer(500, "", "text/plain"), AuthorizationProblem::registration_rejected},
        {no_answer(FetchOutcome::timed_out), AuthorizationProblem::timed_out},
        {no_answer(FetchOutcome::too_large), AuthorizationProblem::document_too_large},
    };
    for (const Case& one : refused) {
        auto result = read_registration_answer(one.result, "https://as.example", "https://as.example/register", sent_redirect_uri, bounds);
        ASSERT_TRUE(std::holds_alternative<FlowFailure>(result)) << one.result.body;
        EXPECT_EQ(std::get<FlowFailure>(result).problem, one.problem);
    }
    auto coded =
        read_registration_answer(refused[0].result, "https://as.example", "https://as.example/register", sent_redirect_uri, bounds);
    EXPECT_EQ(std::get<FlowFailure>(coded).error_code, "invalid_redirect_uri");
}

TEST(OAuthFlowRegistration, TheRegisteredRedirectUrisMustHoldTheOneTheClientSends) {
    const OAuthBounds bounds;
    const auto read = [&](json document) {
        return read_registration_answer(json_answer(std::move(document), 201), "https://as.example",
                                        "https://as.example/register", sent_redirect_uri, bounds);
    };
    const std::string sent(sent_redirect_uri);
    EXPECT_TRUE(std::holds_alternative<StoredClient>(read(json{{"client_id", "a"}})));
    EXPECT_TRUE(std::holds_alternative<StoredClient>(read(json{{"client_id", "a"}, {"redirect_uris", json::array({sent})}})));
    EXPECT_TRUE(std::holds_alternative<StoredClient>(
        read(json{{"client_id", "a"}, {"redirect_uris", json::array({"https://other.example/cb", sent})}})));
    for (json registered : {json::array({"https://elsewhere.example/cb"}), json::array(), json::array({sent + "/"}),
                            json::array({1}), json("http://127.0.0.1:3000/callback"), json(nullptr)}) {
        const auto result = read(json{{"client_id", "a"}, {"redirect_uris", registered}});
        ASSERT_TRUE(std::holds_alternative<FlowFailure>(result)) << registered.dump();
        EXPECT_EQ(std::get<FlowFailure>(result).problem, AuthorizationProblem::registration_rejected);
    }
}

TEST(OAuthFlowRegistration, AnErrorCodeOutsideItsCharacterSetIsNotKept) {
    const OAuthBounds bounds;
    for (const std::string& code : {std::string("bad\r\ncode"), std::string("a\"b"), std::string("a\\b"),
                                    std::string(""), std::string("\xc3\xa9")}) {
        auto result = read_registration_answer(json_answer(json{{"error", code}}, 400), "https://as.example",
                                               "https://as.example/register", sent_redirect_uri, bounds);
        ASSERT_TRUE(std::holds_alternative<FlowFailure>(result));
        EXPECT_FALSE(std::get<FlowFailure>(result).error_code);
    }
}

TEST(OAuthFlowRegistration, TheMetadataDocumentUrlTheApplicationGives) {
    bool has_query = false;
    EXPECT_FALSE(check_client_id_metadata_url("https://conformance-test.local/client-metadata.json", has_query));
    EXPECT_FALSE(has_query);
    EXPECT_FALSE(check_client_id_metadata_url("https://app.example/meta?v=1", has_query));
    EXPECT_TRUE(has_query);
    for (const char* bad : {"http://app.example/meta", "https://app.example", "https://app.example/",
                            "https://app.example/a/./meta", "https://app.example/a/../meta", "https://app.example/..",
                            "https://app.example/meta#x", "https://user@app.example/meta", "app.example/meta"}) {
        EXPECT_TRUE(check_client_id_metadata_url(bad, has_query)) << bad;
    }
}

TEST(OAuthFlowRegistration, TheRedirectUriTheApplicationGives) {
    for (const char* good : {"http://127.0.0.1:3000/callback", "http://localhost:8080/cb", "http://[::1]:9/cb",
                             "https://app.example/callback"}) {
        EXPECT_FALSE(check_redirect_uri(good)) << good;
    }
    for (const char* bad : {"http://app.example/callback", "https://app.example/cb#x", "com.example.app:/cb",
                            "https://user@app.example/cb", "/callback"}) {
        EXPECT_TRUE(check_redirect_uri(bad)) << bad;
    }
}

// ---- Through the flow

TEST(OAuthFlowRegistration, ADynamicRegistrationIsMadeAndReported) {
    Rig rig;
    const FlowStep step = rig.run(unauthorized());
    ASSERT_TRUE(done_of(step));
    const auto registrations = rig.world.requests_for(FetchPurpose::registration);
    ASSERT_EQ(registrations.size(), 1u);
    const FlowFetch& request = *registrations.front();
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.url, "http://localhost:3002/register");
    EXPECT_EQ(header_value(request, "Content-Type"), "application/json");
    EXPECT_FALSE(request.follow_redirects);
    EXPECT_FALSE(header_value(request, "Authorization"));
    ASSERT_TRUE(done_of(step)->updates.put_client);
    EXPECT_EQ(done_of(step)->updates.put_client->first, "http://localhost:3002");
    EXPECT_EQ(rig.lookup.clients.at("http://localhost:3002").client_id, "test-client-id");
    EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "client_id"), "test-client-id");

    // The next flow uses the stored registration.
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_EQ(rig.world.requests_for(FetchPurpose::registration).size(), 1u);
}

TEST(OAuthFlowRegistration, ARejectedRegistrationEndsTheFlow) {
    Rig rig;
    rig.world.registration = json_answer(json{{"error", "invalid_client_metadata"}}, 400);
    const FlowStep step = rig.run(unauthorized());
    ASSERT_EQ(problem_of(step), AuthorizationProblem::registration_rejected);
    EXPECT_EQ(std::get<FlowFailed>(step).failure.error_code, "invalid_client_metadata");
    EXPECT_TRUE(rig.world.prompts.empty());
}

TEST(OAuthFlowRegistration, PreRegisteredCredentialsAuthenticateByBasic) {
    Rig rig;
    json document = rig.world.server_document();
    document.erase("registration_endpoint");
    document["token_endpoint_auth_methods_supported"] = json::array({"client_secret_basic"});
    rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
    rig.config.registration.pre_registered.push_back(
        {rig.world.issuer, "pre:id", std::string(planted_client_secret), std::nullopt});
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty());
    const auto token = rig.world.requests_for(FetchPurpose::token);
    ASSERT_EQ(token.size(), 1u);
    // The identifier's ':' is encoded, so it cannot be read as the separator.
    EXPECT_EQ(header_value(*token.front(), "Authorization"),
              neomcp::http::oauth_basic_authorization("pre:id", planted_client_secret));
    EXPECT_FALSE(form_value(token.front()->body, "client_id"));
    EXPECT_FALSE(form_value(token.front()->body, "client_secret"));
    // The secret goes nowhere else.
    for (const FlowFetch& one : rig.world.requests) {
        if (one.purpose == FetchPurpose::token) continue;
        EXPECT_EQ(one.body.find(planted_client_secret), std::string::npos);
        EXPECT_EQ(one.url.find(planted_client_secret), std::string::npos);
    }
    EXPECT_EQ(rig.world.prompts.front().authorization_url.find(planted_client_secret), std::string::npos);
}

TEST(OAuthFlowRegistration, PreRegisteredForAnotherServerAreNotUsed) {
    Rig rig;
    json document = rig.world.server_document();
    document.erase("registration_endpoint");
    rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
    rig.config.registration.pre_registered.push_back(
        {"http://localhost:3002/", "pre-id", std::string(planted_client_secret), std::nullopt});
    const FlowStep step = rig.run(unauthorized());
    ASSERT_EQ(problem_of(step), AuthorizationProblem::no_client_registration);
    EXPECT_TRUE(std::get<FlowFailed>(step).failure.credentials_for_other_issuer);
    EXPECT_TRUE(rig.world.requests_for(FetchPurpose::token).empty());
}

TEST(OAuthFlowRegistration, TheMetadataDocumentUrlIsTheClientId) {
    Rig rig;
    json document = rig.world.server_document();
    document["client_id_metadata_document_supported"] = true;
    rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
    rig.config.registration.client_id_metadata_url = "https://conformance-test.local/client-metadata.json";
    ASSERT_TRUE(done_of(rig.run(unauthorized())));
    EXPECT_TRUE(rig.world.requests_for(FetchPurpose::registration).empty());
    EXPECT_EQ(form_value(query_of(rig.world.prompts.front().authorization_url), "client_id"),
              "https://conformance-test.local/client-metadata.json");
    EXPECT_EQ(form_value(rig.world.requests_for(FetchPurpose::token).front()->body, "client_id"),
              "https://conformance-test.local/client-metadata.json");
}

TEST(OAuthFlowRegistration, TheRegisteredSecretAndMethodAreUsed) {
    for (const auto& [method, basic] : std::vector<std::pair<std::string, bool>>{{"client_secret_basic", true},
                                                                                  {"client_secret_post", false}}) {
        SCOPED_TRACE(method);
        Rig rig;
        json document = rig.world.server_document();
        document["token_endpoint_auth_methods_supported"] = json::array({method});
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        rig.world.registration = json_answer(
            json{{"client_id", "reg-id"}, {"client_secret", std::string(planted_client_secret)},
                 {"token_endpoint_auth_method", method}},
            201);
        ASSERT_TRUE(done_of(rig.run(unauthorized())));
        const FlowFetch& token = *rig.world.requests_for(FetchPurpose::token).front();
        if (basic) {
            EXPECT_EQ(header_value(token, "Authorization"),
                      neomcp::http::oauth_basic_authorization("reg-id", planted_client_secret));
            EXPECT_FALSE(form_value(token.body, "client_secret"));
        } else {
            EXPECT_FALSE(header_value(token, "Authorization"));
            EXPECT_EQ(form_value(token.body, "client_id"), "reg-id");
            EXPECT_EQ(form_value(token.body, "client_secret"), planted_client_secret);
        }
    }
}

}  // namespace
