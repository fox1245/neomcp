// The token requests and the check of their answers.
#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;

constexpr std::string_view token_endpoint = "https://as.example/token";
constexpr std::string_view issuer = "https://as.example";

ClientCredentials client_with(TokenEndpointAuth method) {
    ClientCredentials client;
    client.client_id = "client:id";
    if (method != TokenEndpointAuth::none) client.client_secret = std::string(planted_client_secret);
    client.method = method;
    return client;
}

std::variant<StoredTokens, FlowFailure> read_answer(const FetchResult& result, const OAuthBounds& bounds = {}) {
    std::vector<FlowDiagnostic> diagnostics;
    return read_token_answer(result, issuer, token_endpoint, {"mcp:basic"}, test_time(), bounds, diagnostics);
}

std::optional<AuthorizationProblem> refused(const FetchResult& result, const OAuthBounds& bounds = {}) {
    auto out = read_answer(result, bounds);
    auto* failure = std::get_if<FlowFailure>(&out);
    if (failure == nullptr) return std::nullopt;
    expect_clean(*failure);
    return failure->problem;
}

json token_body() {
    return json{{"access_token", std::string(planted_access_token)}, {"token_type", "Bearer"}, {"expires_in", 3600}};
}

TEST(OAuthFlowTokens, TheAuthorizationCodeRequest) {
    const FlowFetch fetch = authorization_code_request(token_endpoint, client_with(TokenEndpointAuth::none),
                                                       planted_code, "http://127.0.0.1:3000/callback", "verifier",
                                                       std::string("https://mcp.example/mcp"), OAuthBounds{});
    EXPECT_EQ(fetch.purpose, FetchPurpose::token);
    EXPECT_EQ(fetch.method, "POST");
    EXPECT_EQ(fetch.url, token_endpoint);
    EXPECT_FALSE(fetch.follow_redirects);
    EXPECT_EQ(fetch.max_redirects, 0u);
    EXPECT_EQ(fetch.body_bound, OAuthDefaults::max_token_response_bytes);
    EXPECT_EQ(header_value(fetch, "Content-Type"), "application/x-www-form-urlencoded");
    EXPECT_EQ(header_value(fetch, "Accept"), "application/json");
    EXPECT_FALSE(header_value(fetch, "Authorization"));
    const std::vector<neomcp::http::FormField> expected = {{"grant_type", "authorization_code"},
                                                           {"code", std::string(planted_code)},
                                                           {"redirect_uri", "http://127.0.0.1:3000/callback"},
                                                           {"code_verifier", "verifier"},
                                                           {"resource", "https://mcp.example/mcp"},
                                                           {"client_id", "client:id"}};
    EXPECT_EQ(form_fields(fetch.body), expected);
    EXPECT_EQ(fetch.url.find('?'), std::string::npos);
}

TEST(OAuthFlowTokens, TheClientAuthenticates) {
    const FlowFetch basic =
        authorization_code_request(token_endpoint, client_with(TokenEndpointAuth::client_secret_basic), "c",
                                   "http://127.0.0.1:3000/cb", "v", std::nullopt, OAuthBounds{});
    EXPECT_EQ(header_value(basic, "Authorization"),
              neomcp::http::oauth_basic_authorization("client:id", planted_client_secret));
    EXPECT_FALSE(form_value(basic.body, "client_id"));
    EXPECT_FALSE(form_value(basic.body, "client_secret"));
    EXPECT_FALSE(form_value(basic.body, "resource"));

    const FlowFetch post =
        authorization_code_request(token_endpoint, client_with(TokenEndpointAuth::client_secret_post), "c",
                                   "http://127.0.0.1:3000/cb", "v", std::nullopt, OAuthBounds{});
    EXPECT_FALSE(header_value(post, "Authorization"));
    EXPECT_EQ(form_value(post.body, "client_id"), "client:id");
    EXPECT_EQ(form_value(post.body, "client_secret"), planted_client_secret);
}

TEST(OAuthFlowTokens, TheRefreshRequest) {
    const FlowFetch fetch = refresh_token_request(token_endpoint, client_with(TokenEndpointAuth::none),
                                                  planted_refresh_token, std::string("https://mcp.example/mcp"),
                                                  OAuthBounds{});
    const std::vector<neomcp::http::FormField> expected = {{"grant_type", "refresh_token"},
                                                           {"refresh_token", std::string(planted_refresh_token)},
                                                           {"resource", "https://mcp.example/mcp"},
                                                           {"client_id", "client:id"}};
    EXPECT_EQ(form_fields(fetch.body), expected);
    EXPECT_FALSE(fetch.follow_redirects);
}

TEST(OAuthFlowTokens, AGoodAnswer) {
    json body = token_body();
    body["refresh_token"] = std::string(planted_refresh_token);
    body["scope"] = "mcp:basic mcp:write";
    auto out = read_answer(json_answer(body));
    ASSERT_TRUE(std::holds_alternative<StoredTokens>(out));
    const auto& tokens = std::get<StoredTokens>(out);
    EXPECT_EQ(tokens.access_token, planted_access_token);
    EXPECT_EQ(tokens.refresh_token, planted_refresh_token);
    EXPECT_EQ(tokens.expires_at, test_time() + std::chrono::seconds(3600));
    EXPECT_EQ(tokens.granted_scopes, (std::vector<std::string>{"mcp:basic", "mcp:write"}));
    EXPECT_EQ(tokens.requested_scopes, std::vector<std::string>{"mcp:basic"});
}

TEST(OAuthFlowTokens, TheTokenTypeIsBearerInAnyCase) {
    for (const char* type : {"Bearer", "bearer", "BEARER"}) {
        json body = token_body();
        body["token_type"] = type;
        EXPECT_TRUE(std::holds_alternative<StoredTokens>(read_answer(json_answer(body)))) << type;
    }
    for (const json& type : {json("DPoP"), json("Bearer "), json("Bearerx"), json(1), json()}) {
        json body = token_body();
        if (type.is_null()) {
            body.erase("token_type");
        } else {
            body["token_type"] = type;
        }
        EXPECT_EQ(refused(json_answer(body)), AuthorizationProblem::unsupported_token_type) << type.dump();
    }
}

TEST(OAuthFlowTokens, TheAccessTokenIsAB64Token) {
    EXPECT_TRUE(is_b64token("abc-._~+/XYZ019=="));
    EXPECT_TRUE(is_b64token("a"));
    EXPECT_FALSE(is_b64token(""));
    EXPECT_FALSE(is_b64token("=="));
    EXPECT_FALSE(is_b64token("ab=c"));
    EXPECT_FALSE(is_b64token("a b"));
    EXPECT_FALSE(is_b64token("a\r\nX-Injected: 1"));
    EXPECT_FALSE(is_b64token(std::string("a\0b", 3)));
    EXPECT_FALSE(is_b64token("a,b"));
    for (const json& token : {json("bad token"), json("a\nb"), json(""), json(5), json()}) {
        json body = token_body();
        if (token.is_null()) {
            body.erase("access_token");
        } else {
            body["access_token"] = token;
        }
        EXPECT_EQ(refused(json_answer(body)), AuthorizationProblem::token_request_failed) << token.dump();
    }
}

TEST(OAuthFlowTokens, TheTokenBoundAtItsEdge) {
    OAuthBounds bounds;
    bounds.max_token_bytes = planted_access_token.size();
    EXPECT_TRUE(std::holds_alternative<StoredTokens>(read_answer(json_answer(token_body()), bounds)));
    bounds.max_token_bytes = planted_access_token.size() - 1;
    EXPECT_EQ(refused(json_answer(token_body()), bounds), AuthorizationProblem::token_request_failed);
}

TEST(OAuthFlowTokens, TheResponseBoundAtItsEdge) {
    const std::string body = token_body().dump();
    OAuthBounds bounds;
    bounds.max_token_response_bytes = body.size();
    EXPECT_TRUE(std::holds_alternative<StoredTokens>(read_answer(answer(200, body), bounds)));
    bounds.max_token_response_bytes = body.size() - 1;
    EXPECT_EQ(refused(answer(200, body), bounds), AuthorizationProblem::document_too_large);
    EXPECT_EQ(refused(no_answer(FetchOutcome::too_large)), AuthorizationProblem::document_too_large);
}

TEST(OAuthFlowTokens, ExpiresInIsAHintWithinItsBound) {
    for (const json& value : {json(0), json(-5), json("3600"), json()}) {
        json body = token_body();
        if (value.is_null()) {
            body.erase("expires_in");
        } else {
            body["expires_in"] = value;
        }
        auto out = read_answer(json_answer(body));
        ASSERT_TRUE(std::holds_alternative<StoredTokens>(out)) << value.dump();
        EXPECT_FALSE(std::get<StoredTokens>(out).expires_at) << value.dump();
    }
    json body = token_body();
    body["expires_in"] = 1e300;
    auto out = read_answer(json_answer(body));
    ASSERT_TRUE(std::holds_alternative<StoredTokens>(out));
    EXPECT_EQ(std::get<StoredTokens>(out).expires_at, test_time() + OAuthDefaults::max_token_lifetime);
}

TEST(OAuthFlowTokens, RefusalsCarryTheErrorCodeOnly) {
    auto out =
        read_answer(json_answer(json{{"error", "invalid_grant"}, {"error_description", "PlantedDescription"}}, 400));
    ASSERT_TRUE(std::holds_alternative<FlowFailure>(out));
    EXPECT_EQ(std::get<FlowFailure>(out).problem, AuthorizationProblem::token_request_failed);
    EXPECT_EQ(std::get<FlowFailure>(out).error_code, "invalid_grant");
    expect_clean(std::get<FlowFailure>(out), {"PlantedDescription"});

    for (const FetchResult& result : {answer(500, "{\"error\":\"server_error\"}"), answer(307, "", "text/plain"),
                                      answer(201, token_body().dump()), answer(200, token_body().dump(), "text/html"),
                                      answer(200, "not json"), no_answer(FetchOutcome::transport_error)}) {
        EXPECT_EQ(refused(result), AuthorizationProblem::token_request_failed);
    }
    EXPECT_EQ(refused(no_answer(FetchOutcome::timed_out)), AuthorizationProblem::timed_out);
    auto server_error = read_answer(answer(500, "{\"error\":\"server_error\"}"));
    EXPECT_FALSE(std::get<FlowFailure>(server_error).error_code);
}

}  // namespace
