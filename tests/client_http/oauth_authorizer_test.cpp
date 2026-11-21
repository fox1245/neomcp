// OAuthAuthorizer against a scripted authorization server and resource on loopback: the full sign-in, one sign-in for
// requests refused together, a refresh without the person, a changed authorization server, and the step-up for scope
// with its bound.
#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <string>
#include <vector>

#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;

std::size_t count_with(const std::vector<Request>& requests, std::string_view name, std::string_view value) {
    std::size_t count = 0;
    for (const Request& one : requests) {
        if (field_of(one.body, name) == std::optional<std::string>(std::string(value))) ++count;
    }
    return count;
}

// ---- The full sign-in

TEST(OAuthAuthorizer, TheFullSignInEndToEnd) {
    World world;
    auto authorizer = world.authorizer();
    ASSERT_TRUE(authorizer);
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;

    // The probe was refused, the documents read, the client registered, the person asked once, the code exchanged.
    ASSERT_EQ(world.person.prompts().size(), 1u);
    const std::vector<neomcp::AuthorizationPromptInfo> prompts = world.person.prompts();
    const neomcp::AuthorizationPromptInfo& prompt = prompts.front();
    EXPECT_EQ(prompt.issuer, world.as.issuer());
    EXPECT_EQ(prompt.redirect_uri, redirect_uri);
    const std::string query = query_of(prompt.authorization_url);
    EXPECT_EQ(field_of(query, "response_type"), "code");
    EXPECT_EQ(field_of(query, "client_id"), world.as.client_id());
    EXPECT_EQ(field_of(query, "code_challenge_method"), "S256");
    EXPECT_EQ(field_of(query, "resource"), world.resource.url());
    EXPECT_TRUE(field_of(query, "state"));
    EXPECT_EQ(prompt.authorization_url.rfind(world.as.issuer() + "/authorize?", 0), 0u);

    EXPECT_EQ(world.as.requests_to("/.well-known/oauth-authorization-server").size(), 1u);
    const auto registrations = world.as.requests_to("/register");
    ASSERT_EQ(registrations.size(), 1u);
    const json registration = json::parse(registrations.front().body);
    EXPECT_EQ(registration["redirect_uris"], json::array({std::string(redirect_uri)}));
    EXPECT_EQ(registration["application_type"], "native");
    const auto tokens = world.as.requests_to("/token");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(field_of(tokens.front().body, "grant_type"), "authorization_code");
    EXPECT_EQ(field_of(tokens.front().body, "resource"), world.resource.url());
    EXPECT_EQ(field_of(tokens.front().body, "redirect_uri"), redirect_uri);
    EXPECT_TRUE(field_of(tokens.front().body, "code_verifier"));
    // No request to the authorization server carries the resource's Authorization.
    for (const Request& one : world.as.server().requests()) EXPECT_FALSE(one.field("Authorization")) << one.target;

    // The refused probe went out again with the bearer, and so does every later call.
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    const auto sent = world.resource.authorizations();
    ASSERT_GE(sent.size(), 3u);
    EXPECT_EQ(sent.front(), "");
    EXPECT_EQ(sent.back(), "Bearer one-access-1");
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_TRUE(client->close());
}

// ---- One sign-in at a time

// Two calls refused while the person signs in wait for the same sign-in: one prompt, and both go through.
TEST(OAuthAuthorizer, TwoRefusalsDuringOneSignInShareOnePrompt) {
    World world;
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_EQ(world.person.prompts().size(), 1u);

    // The tokens expire; no refresh token exists, so both refusals need the person, who comes back only once both
    // were refused.
    world.ledger.expire_all();
    const std::size_t refused_before = world.resource.refusals();
    world.person.hold_until([&] { return world.resource.refusals() >= refused_before + 2; });
    auto first = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    auto second = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    const auto one = first.get();
    const auto two = second.get();
    ASSERT_TRUE(one) << one.error().message;
    ASSERT_TRUE(two) << two.error().message;
    EXPECT_EQ(world.resource.refusals(), refused_before + 2);
    EXPECT_EQ(world.person.prompts().size(), 2u);
    EXPECT_EQ(world.as.requests_to("/token").size(), 2u);
    EXPECT_TRUE(client->close());
}

// ---- Refresh

TEST(OAuthAuthorizer, ARefusedTokenIsRefreshedWithoutAPrompt) {
    World world;
    world.as.issue_refresh_tokens(true);
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    world.ledger.expire_all();

    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(world.person.prompts().size(), 1u);
    const auto tokens = world.as.requests_to("/token");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(field_of(tokens[1].body, "grant_type"), "refresh_token");
    EXPECT_EQ(field_of(tokens[1].body, "refresh_token"), "one-refresh-1");
    EXPECT_EQ(field_of(tokens[1].body, "resource"), world.resource.url());
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer one-access-2");
    // The rotated refresh token replaced the old one in the store.
    const auto stored = world.store->tokens({world.resource.url(), world.as.issuer()});
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->refresh_token, "one-refresh-2");
    EXPECT_TRUE(client->close());
}

// A refresh the authorization server refuses erases the refresh token, and the person is asked; when the person gives
// up, the sign-in fails and the erasure is kept.
TEST(OAuthAuthorizer, AFailedSignInKeepsItsChangesToTheStore) {
    World world;
    world.as.issue_refresh_tokens(true);
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    const neomcp::CredentialKey key{world.resource.url(), world.as.issuer()};
    ASSERT_TRUE(world.store->tokens(key));

    world.ledger.expire_all();
    world.as.refuse_next_token("invalid_grant");
    world.person.set_mode(Person::Mode::give_up);
    const auto listed = client->request("tools/list");
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(listed.error().authorization);
    EXPECT_EQ(listed.error().authorization->problem, AuthorizationProblem::prompt_cancelled);
    EXPECT_EQ(world.person.prompts().size(), 2u);
    EXPECT_FALSE(world.store->tokens(key));
    EXPECT_TRUE(client->close());
}

// ---- A changed authorization server

TEST(OAuthAuthorizer, AChangedAuthorizationServerDropsTheOldTokens) {
    World world;
    world.as.issue_refresh_tokens(true);
    AuthServer second(world.ledger, "two");
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    const std::size_t first_tokens = world.as.requests_to("/token").size();

    // The resource now names the second authorization server, and the first one's tokens are no longer good there.
    world.resource.use(second);
    world.person.sign_in_at(second);
    world.ledger.expire_all();
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;

    // No refresh anywhere: the first server was not asked again, and nothing of it reached the second.
    EXPECT_EQ(world.as.requests_to("/token").size(), first_tokens);
    EXPECT_EQ(second.requests_to("/register").size(), 1u);
    for (const Request& one : second.server().requests()) {
        EXPECT_EQ(one.body.find("one-refresh"), std::string::npos) << one.target;
        EXPECT_EQ(one.body.find(world.as.client_id()), std::string::npos) << one.target;
        EXPECT_EQ(one.head.find("one-"), std::string::npos) << one.target;
    }
    ASSERT_EQ(world.person.prompts().size(), 2u);
    EXPECT_EQ(world.person.prompts()[1].issuer, second.issuer());
    EXPECT_EQ(field_of(query_of(world.person.prompts()[1].authorization_url), "client_id"), second.client_id());
    EXPECT_EQ(count_with(second.requests_to("/token"), "grant_type", "authorization_code"), 1u);
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer two-access-1");
    // The store holds the second server's tokens only, and no registration at the first.
    EXPECT_FALSE(world.store->tokens({world.resource.url(), world.as.issuer()}));
    EXPECT_TRUE(world.store->tokens({world.resource.url(), second.issuer()}));
    EXPECT_FALSE(world.store->client(world.as.issuer()));
    EXPECT_TRUE(client->close());
}

// With credentials registered beforehand at the new server, a refresh there would be possible: it is still not made
// with the old server's refresh token, which never reaches the new server.
TEST(OAuthAuthorizer, TheOldRefreshTokenNeverReachesANewAuthorizationServer) {
    World world;
    world.as.issue_refresh_tokens(true);
    AuthServer second(world.ledger, "two");
    neomcp::OAuthOptions options = world.options();
    options.pre_registered.push_back({second.issuer(), "two-preregistered", std::nullopt, std::nullopt});
    auto authorizer = world.authorizer(std::move(options));
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;

    world.resource.use(second);
    world.person.sign_in_at(second);
    world.ledger.expire_all();
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    for (const Request& one : second.server().requests()) {
        EXPECT_EQ(one.body.find("one-refresh"), std::string::npos) << one.target;
        EXPECT_EQ(one.head.find("one-"), std::string::npos) << one.target;
    }
    EXPECT_EQ(count_with(second.requests_to("/token"), "grant_type", "refresh_token"), 0u);
    EXPECT_EQ(count_with(second.requests_to("/token"), "client_id", "two-preregistered"), 1u);
    EXPECT_TRUE(second.requests_to("/register").empty());
    EXPECT_EQ(world.person.prompts().size(), 2u);
    EXPECT_TRUE(client->close());
}

// ---- Step-up

TEST(OAuthAuthorizer, AStepUpAsksForTheUnionOfScopes) {
    World world;
    world.resource.challenge_scope("mcp:basic");
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(world.person.prompts()[0].scopes, (std::vector<std::string>{"mcp:basic"}));

    world.resource.require_scopes({"mcp:write"});
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(world.person.prompts().size(), 2u);
    EXPECT_EQ(field_of(query_of(world.person.prompts()[1].authorization_url), "scope"), "mcp:basic mcp:write");
    EXPECT_TRUE(client->close());
}

// A server that refuses for want of a scope the client already asked for gets no new sign-in: the call fails at once.
TEST(OAuthAuthorizer, AStepUpForNothingNewIsNotMade) {
    World world;
    world.resource.challenge_scope("mcp:basic");
    auto authorizer = world.authorizer();
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    world.resource.require_scopes({"mcp:write"});
    world.resource.refuse_for_scope_always(true);

    const auto listed = client->request("tools/list");
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(listed.error().authorization);
    EXPECT_EQ(listed.error().authorization->problem, AuthorizationProblem::insufficient_scope);
    // One step-up, then the refusal for the same scopes ends it.
    EXPECT_EQ(world.person.prompts().size(), 2u);
    EXPECT_TRUE(client->close());
}

// Step-ups of one operation are bounded even when each asks for more: a server that keeps asking for a new scope
// gets at most max_step_ups_per_operation + 1 sign-ins in a row for one request (here one).
TEST(OAuthAuthorizer, StepUpsInARowAreBounded) {
    World world;
    auto authorizer = world.authorizer([&] {
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_step_ups_per_operation = 0;
        return options;
    }());
    auto client = world.open(authorizer);
    ASSERT_TRUE(client) << client.error().message;
    world.resource.escalate_scopes(true);
    const auto listed = client->request("tools/list");
    ASSERT_FALSE(listed);
    ASSERT_TRUE(listed.error().authorization);
    EXPECT_EQ(listed.error().authorization->problem, AuthorizationProblem::attempts_exceeded);
    // The sign-in of the open, and one step-up of the call.
    EXPECT_EQ(world.person.prompts().size(), 2u);
    EXPECT_TRUE(client->close());
}

}  // namespace
