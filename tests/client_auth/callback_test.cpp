// The authorization URL, and the check of the callback: where it came back, repeated parameters, iss, state, errors
// and the code.
#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;

constexpr std::string_view issuer = "https://as.example";
constexpr std::string_view redirect = "http://127.0.0.1:3000/callback";
constexpr std::string_view state = "StateValue0123";

PendingAuthorization pending(bool iss_supported) {
    PendingAuthorization out;
    out.issuer = std::string(issuer);
    out.iss_parameter_supported = iss_supported;
    out.state = std::string(state);
    out.redirect_uri = std::string(redirect);
    return out;
}

std::string callback(std::vector<neomcp::http::FormField> fields, std::string_view base = redirect) {
    return std::string(base) + "?" + neomcp::http::form_encode(fields);
}

std::optional<AuthorizationProblem> refused(std::string_view url, bool iss_supported) {
    auto result = check_callback(url, pending(iss_supported), OAuthBounds{});
    auto* failure = std::get_if<FlowFailure>(&result);
    if (failure == nullptr) return std::nullopt;
    expect_clean(*failure, {std::string(state)});
    return failure->problem;
}

std::optional<std::string> accepted(std::string_view url, bool iss_supported) {
    auto result = check_callback(url, pending(iss_supported), OAuthBounds{});
    auto* code = std::get_if<std::string>(&result);
    if (code == nullptr) return std::nullopt;
    return *code;
}

TEST(OAuthFlowCallback, TheAuthorizationUrl) {
    AuthorizationRequest request;
    request.authorization_endpoint = "https://as.example/authorize";
    request.client_id = "https://app.example/client.json";
    request.redirect_uri = std::string(redirect);
    request.code_challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    request.state = std::string(state);
    request.resource = "https://mcp.example/mcp";
    request.scopes = {"mcp:basic", "mcp:write"};
    const std::string url = authorization_url(request);
    ASSERT_TRUE(url.starts_with("https://as.example/authorize?"));
    const auto fields = form_fields(query_of(url));
    const std::vector<neomcp::http::FormField> expected = {
        {"response_type", "code"},
        {"client_id", "https://app.example/client.json"},
        {"redirect_uri", std::string(redirect)},
        {"code_challenge", "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"},
        {"code_challenge_method", "S256"},
        {"state", std::string(state)},
        {"resource", "https://mcp.example/mcp"},
        {"scope", "mcp:basic mcp:write"}};
    EXPECT_EQ(fields, expected);
}

TEST(OAuthFlowCallback, TheAuthorizationUrlKeepsTheEndpointsQuery) {
    AuthorizationRequest request;
    request.client_id = "c";
    request.redirect_uri = std::string(redirect);
    request.code_challenge = "x";
    request.state = "s";
    request.authorization_endpoint = "https://as.example/authorize?tenant=a";
    EXPECT_TRUE(authorization_url(request).starts_with("https://as.example/authorize?tenant=a&response_type=code&"));
    request.authorization_endpoint = "https://as.example/authorize?";
    EXPECT_TRUE(authorization_url(request).starts_with("https://as.example/authorize?response_type=code&"));
    // Without resource and scopes neither parameter appears.
    const auto fields = form_fields(query_of(authorization_url(request)));
    for (const auto& field : fields) {
        EXPECT_NE(field.name, "resource");
        EXPECT_NE(field.name, "scope");
    }
}

TEST(OAuthFlowCallback, ACodeWithTheRightStateIsAccepted) {
    EXPECT_EQ(accepted(callback({{"code", "c1"}, {"state", std::string(state)}}), false), "c1");
    // Parameters the flow does not read are ignored, and so is a fragment.
    EXPECT_EQ(accepted(callback({{"code", "c1"}, {"state", std::string(state)}, {"x", "y"}}) + "#frag", false), "c1");
    // The redirect URI's host is compared without case, a default port as the same port.
    EXPECT_EQ(accepted(callback({{"code", "c1"}, {"state", std::string(state)}}, "HTTP://127.0.0.1:3000/callback"),
                       false),
              "c1");
}

TEST(OAuthFlowCallback, TheCallbackMustComeBackToTheRedirectUri) {
    const std::vector<neomcp::http::FormField> good = {{"code", "c1"}, {"state", std::string(state)}};
    for (const char* base : {"http://127.0.0.1:3001/callback", "http://127.0.0.2:3000/callback",
                             "https://127.0.0.1:3000/callback", "http://127.0.0.1:3000/callback/",
                             "http://127.0.0.1:3000/Callback", "http://127.0.0.1:3000/other"}) {
        EXPECT_EQ(refused(callback(good, base), false), AuthorizationProblem::callback_invalid) << base;
    }
    EXPECT_EQ(refused("not a url", false), AuthorizationProblem::callback_invalid);
    EXPECT_EQ(refused(std::string(redirect) + "?code=%zz&state=x", false), AuthorizationProblem::callback_invalid);
}

TEST(OAuthFlowCallback, ARepeatedParameterRefusesTheResponse) {
    const std::string s(state);
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}, {"state", s}}), false),
              AuthorizationProblem::callback_invalid);
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"code", "c2"}, {"state", s}}), false),
              AuthorizationProblem::callback_invalid);
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}, {"iss", std::string(issuer)}, {"iss", "x"}}), true),
              AuthorizationProblem::callback_invalid);
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}, {"other", "a"}, {"other", "b"}}), false),
              AuthorizationProblem::callback_invalid);
    // A parameter without value counts as absent, so it neither repeats one nor stands for one.
    EXPECT_EQ(accepted(callback({{"code", "c1"}, {"state", s}, {"state", ""}}), false), "c1");
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", ""}}), false), AuthorizationProblem::state_mismatch);
}

TEST(OAuthFlowCallback, TheStateMustBeExactlyTheOneSent) {
    const std::string s(state);
    for (const std::string& other : {std::string("statevalue0123"), s + " ", s.substr(1), s.substr(0, s.size() - 1),
                                     s.substr(0, 1), s + "x", std::string()}) {
        EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", other}}), false), AuthorizationProblem::state_mismatch)
            << other;
    }
    EXPECT_EQ(refused(callback({{"code", "c1"}}), false), AuthorizationProblem::state_mismatch);
    EXPECT_EQ(refused(std::string(redirect), false), AuthorizationProblem::state_mismatch);
}

// RFC 9207 and the table of the MCP authorization page.
TEST(OAuthFlowCallback, IssWhenTheServerAdvertisesIt) {
    const std::string s(state);
    EXPECT_EQ(accepted(callback({{"code", "c1"}, {"state", s}, {"iss", std::string(issuer)}}), true), "c1");
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}}), true), AuthorizationProblem::issuer_mismatch);
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}, {"iss", ""}}), true),
              AuthorizationProblem::issuer_mismatch);
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}, {"iss", "https://evil.example"}}), true),
              AuthorizationProblem::issuer_mismatch);
}

TEST(OAuthFlowCallback, IssWhenTheServerDoesNotAdvertiseIt) {
    const std::string s(state);
    EXPECT_EQ(accepted(callback({{"code", "c1"}, {"state", s}}), false), "c1");
    EXPECT_EQ(accepted(callback({{"code", "c1"}, {"state", s}, {"iss", std::string(issuer)}}), false), "c1");
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}, {"iss", "https://evil.example"}}), false),
              AuthorizationProblem::issuer_mismatch);
}

TEST(OAuthFlowCallback, IssIsComparedWithoutAnyNormalization) {
    const std::string s(state);
    for (const char* variant : {"https://as.example/", "HTTPS://as.example", "https://AS.example",
                                "https://as.example:443", "https://as%2Eexample", "https://as.example#",
                                "https://as.example "}) {
        EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", s}, {"iss", variant}}), true),
                  AuthorizationProblem::issuer_mismatch)
            << variant;
    }
    // iss is compared after form decoding, once: an encoded form decodes to the issuer, a twice-encoded one does not.
    EXPECT_EQ(accepted(std::string(redirect) + "?code=c1&state=" + s + "&iss=https%3A%2F%2Fas.example", true), "c1");
    EXPECT_EQ(refused(std::string(redirect) + "?code=c1&state=" + s + "&iss=https%253A%252F%252Fas.example", true),
              AuthorizationProblem::issuer_mismatch);
}

TEST(OAuthFlowCallback, AnIssMismatchComesBeforeTheErrorAndUsesNothingOfIt) {
    const std::string s(state);
    auto result = check_callback(
        callback({{"error", "access_denied"}, {"error_description", "Planted"}, {"state", s}, {"iss", "https://x"}}),
        pending(true), OAuthBounds{});
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(result));
    EXPECT_EQ(std::get<FlowFailure>(result).problem, AuthorizationProblem::issuer_mismatch);
    EXPECT_FALSE(std::get<FlowFailure>(result).error_code);
    // The iss check comes before the state check.
    EXPECT_EQ(refused(callback({{"code", "c1"}, {"state", "wrong"}, {"iss", "https://x"}}), false),
              AuthorizationProblem::issuer_mismatch);
}

TEST(OAuthFlowCallback, AnErrorAnswerEndsTheFlowWithItsCodeOnly) {
    const std::string s(state);
    auto result = check_callback(callback({{"error", "access_denied"},
                                           {"error_description", "PlantedDescription"},
                                           {"error_uri", "https://as.example/PlantedUri"},
                                           {"state", s},
                                           {"iss", std::string(issuer)}}),
                                 pending(true), OAuthBounds{});
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(result));
    const FlowFailure& failure = std::get<FlowFailure>(result);
    EXPECT_EQ(failure.problem, AuthorizationProblem::authorization_denied);
    EXPECT_EQ(failure.error_code, "access_denied");
    expect_clean(failure, {"PlantedDescription", "PlantedUri", s});
    // An error code outside its character set is not kept.
    auto odd = check_callback(callback({{"error", "a\r\nb"}, {"state", s}}), pending(false), OAuthBounds{});
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(odd));
    EXPECT_FALSE(std::get<FlowFailure>(odd).error_code);
    // An error with the wrong state is a state mismatch.
    EXPECT_EQ(refused(callback({{"error", "access_denied"}, {"state", "wrong"}}), false),
              AuthorizationProblem::state_mismatch);
}

TEST(OAuthFlowCallback, NoCodeIsADenial) {
    EXPECT_EQ(refused(callback({{"state", std::string(state)}}), false), AuthorizationProblem::authorization_denied);
    EXPECT_EQ(refused(callback({{"state", std::string(state)}, {"code", ""}}), false),
              AuthorizationProblem::authorization_denied);
}

TEST(OAuthFlowCallback, TheBoundAtItsEdge) {
    const std::string url = callback({{"code", "c1"}, {"state", std::string(state)}});
    OAuthBounds bounds;
    bounds.max_callback_bytes = url.size();
    EXPECT_TRUE(std::holds_alternative<std::string>(check_callback(url, pending(false), bounds)));
    bounds.max_callback_bytes = url.size() - 1;
    auto result = check_callback(url, pending(false), bounds);
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(result));
    EXPECT_EQ(std::get<FlowFailure>(result).problem, AuthorizationProblem::callback_invalid);
}

// ---- Through the flow, with the runner's iss modes

TEST(OAuthFlowCallback, TheRunnersIssModes) {
    struct Case {
        bool advertised;
        std::optional<std::string> iss;
        std::optional<AuthorizationProblem> problem;
    };
    const std::vector<Case> cases = {
        {true, std::string("http://localhost:3002"), std::nullopt},
        {false, std::nullopt, std::nullopt},
        {true, std::nullopt, AuthorizationProblem::issuer_mismatch},
        {true, std::string("http://localhost:3003"), AuthorizationProblem::issuer_mismatch},
        {false, std::string("http://localhost:3003"), AuthorizationProblem::issuer_mismatch},
        {true, std::string("http://localhost:3002/"), AuthorizationProblem::issuer_mismatch},
    };
    for (const Case& one : cases) {
        SCOPED_TRACE(one.iss.value_or("(none)"));
        Rig rig;
        json document = rig.world.server_document();
        document["authorization_response_iss_parameter_supported"] = one.advertised;
        rig.world.documents[rig.world.server_metadata_url()] = json_answer(document);
        rig.world.callback_iss = one.iss;
        const FlowStep step = rig.run(unauthorized());
        if (!one.problem) {
            EXPECT_TRUE(done_of(step));
            continue;
        }
        EXPECT_EQ(problem_of(step), *one.problem);
        // The authorization endpoint was reached, then no token request.
        EXPECT_EQ(rig.world.prompts.size(), 1u);
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::token).empty());
    }
}

TEST(OAuthFlowCallback, CancelAndTimeoutOfThePrompt) {
    for (const auto& [kind, problem] : std::vector<std::pair<PromptAnswer::Kind, AuthorizationProblem>>{
             {PromptAnswer::Kind::cancelled, AuthorizationProblem::prompt_cancelled},
             {PromptAnswer::Kind::timed_out, AuthorizationProblem::prompt_timed_out}}) {
        Rig rig;
        rig.world.prompt = [answer_kind = kind](const PromptRequest&) {
            PromptAnswer out;
            out.kind = answer_kind;
            return out;
        };
        EXPECT_EQ(problem_of(rig.run(unauthorized())), problem);
        EXPECT_TRUE(rig.world.requests_for(FetchPurpose::token).empty());
    }
}

TEST(OAuthFlowCallback, ThePromptTellsWhatTheApplicationNeeds) {
    Rig rig;
    ASSERT_TRUE(done_of(rig.run(unauthorized({"Bearer scope=\"mcp:basic\""}))));
    const PromptRequest& prompt = rig.world.prompts.front();
    EXPECT_EQ(prompt.redirect_uri, rig.world.redirect_uri);
    EXPECT_EQ(prompt.issuer, rig.world.issuer);
    EXPECT_EQ(prompt.scopes, std::vector<std::string>{"mcp:basic"});
    EXPECT_EQ(prompt.timeout, OAuthDefaults::prompt_timeout);
    EXPECT_TRUE(prompt.authorization_url.starts_with("http://localhost:3002/authorize?"));
}

}  // namespace
