// The library's OAuth client against the library's own protected server, over loopback sockets in one process, for each
// revision the client speaks over Streamable HTTP and with every revision enabled: the first contact (the refusal, the
// protected resource metadata by the URL the challenge names, the authorization server's metadata, the registration,
// the token, the call that then passes); the scope the client asks for (from the challenge, from the metadata's
// supported scopes, none); a tool that declares a scope (the refusal, the union of scopes, the call that then passes) and
// a scope no authorization server grants (the client gives up within its bound); the server naming another
// authorization server; and a token issued for another resource.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "end_to_end_server_rig.h"

namespace {

using namespace server_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;
using oauth_rig::field_of;
using oauth_rig::grants_of;
using oauth_rig::query_of;

constexpr std::string_view modern_revision = "2026-07-28";

class ServerEndToEnd : public ::testing::TestWithParam<std::string> {
protected:
    void use(ServerWorld& world) { use_revision(world, GetParam()); }

    // The revision the client ends up speaking: the one enabled, or the newest when every revision is.
    void expect_revision(neomcp::Client& client) {
        const auto verdict = client.verdict();
        ASSERT_TRUE(verdict);
        EXPECT_EQ(verdict->revision, GetParam() == every_revision ? std::string(modern_revision) : GetParam());
    }
};

// The requests of the server's log that carried a token, in order.
std::vector<Seen> authorized(ServerWorld& world) {
    std::vector<Seen> out;
    for (Seen& one : world.log().all()) {
        if (one.authorization && one.path == endpoint_path) out.push_back(std::move(one));
    }
    return out;
}

// ---- The first contact

TEST_P(ServerEndToEnd, SignsInOnTheFirstRefusalAndTheCallPasses) {
    ServerWorld world;
    use(world);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    expect_revision(*client);
    const auto opened = client->call_tool("open");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_FALSE(opened->is_error);
    EXPECT_EQ(text_of(*opened), "open");
    EXPECT_EQ(world.items.open.load(), 1);

    // The order of what the server saw: a request without a token, answered 401; the document of the protected
    // resource at the URL its challenge names (the path form of the endpoint's path), with no token; and then the
    // token the authorization server issued.
    const std::vector<Seen> all = world.log().all();
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all.front().method, "POST");
    EXPECT_EQ(all.front().path, endpoint_path);
    EXPECT_FALSE(all.front().authorization);
    EXPECT_EQ(all.front().status, 401);
    const std::vector<Seen> documents = world.log().to("GET", document_path);
    ASSERT_EQ(documents.size(), 1u);
    EXPECT_FALSE(documents.front().authorization);
    EXPECT_EQ(documents.front().status, 200);
    const auto first_with_token = std::find_if(all.begin(), all.end(), [](const Seen& one) { return one.authorization; });
    const auto document = std::find_if(all.begin(), all.end(), [](const Seen& one) { return one.path == document_path; });
    ASSERT_NE(first_with_token, all.end());
    ASSERT_NE(document, all.end());
    EXPECT_LT(document, first_with_token);
    ASSERT_EQ(world.as.access_tokens().size(), 1u);
    const std::string token = world.as.access_tokens().front();
    EXPECT_EQ(first_with_token->authorization, "Bearer " + token);
    for (const Seen& one : authorized(world)) {
        EXPECT_EQ(one.authorization, "Bearer " + token);
        EXPECT_TRUE(one.status == 200 || one.status == 202) << one.rpc_method << " " << one.status;
    }

    // The authorization server met the client once for each step.
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);
    EXPECT_EQ(grants_of(world.as, "authorization_code"), 1u);
    ASSERT_EQ(world.person.prompts().size(), 1u);
    const std::string query = query_of(world.person.prompts().front().authorization_url);
    EXPECT_EQ(field_of(query, "client_id"), world.as.client_id());
    EXPECT_EQ(field_of(query, "resource"), world.resource_url);
    EXPECT_EQ(field_of(query, "code_challenge_method"), "S256");
    EXPECT_EQ(field_of(query, "scope"), std::string(required_scope));
    ASSERT_EQ(world.as.requests_to("/token").size(), 1u);
    EXPECT_EQ(field_of(world.as.requests_to("/token").front().body, "resource"), world.resource_url);

    // The server asked its validator about this resource, and what a handler sees is what the token said.
    EXPECT_GE(world.table->calls(), 1);
    for (const std::string& resource : world.table->resources()) EXPECT_EQ(resource, world.resource_url);
    const auto who = client->call_tool("whoami");
    ASSERT_TRUE(who) << who.error().message;
    const json seen = json::parse(text_of(*who));
    EXPECT_EQ(seen.at("issuer"), world.as.issuer());
    EXPECT_EQ(seen.at("subject"), std::string(AuthServer::subject_name));
    EXPECT_EQ(seen.at("client"), world.as.client_id());
    EXPECT_EQ(seen.at("scopes"), json::array({std::string(required_scope)}));
    EXPECT_EQ(seen.at("audiences"), json::array({world.resource_url}));
    expect_tokens_kept(world, {}, {text_of(*who)});
    EXPECT_TRUE(client->close());
}

// ---- The scope the client asks for

// The refusal names the scope: the client asks for it, and not for all that the metadata lists.
TEST_P(ServerEndToEnd, AsksForTheScopeOfTheChallenge) {
    ServerSetup setup;
    setup.required_scopes = {"mcp:use"};
    setup.scopes_supported = std::vector<std::string>{"mcp:use", "files:read", "files:write"};
    ServerWorld world(std::move(setup));
    use(world);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(asked_scope(world.person.prompts().front()), "mcp:use");
    const auto opened = client->call_tool("open");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(text_of(*opened), "open");
    expect_tokens_kept(world);
    EXPECT_TRUE(client->close());
}

// The refusal names no scope (the server needs none of its own): the client asks for those the metadata lists.
TEST_P(ServerEndToEnd, AsksForTheScopesTheMetadataListsWhenTheChallengeNamesNone) {
    ServerSetup setup;
    setup.required_scopes = {};
    setup.scopes_supported = std::vector<std::string>{"files:read", "files:write"};
    ServerWorld world(std::move(setup));
    use(world);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(asked_scope(world.person.prompts().front()), "files:read files:write");
    const auto opened = client->call_tool("open");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto who = client->call_tool("whoami");
    ASSERT_TRUE(who) << who.error().message;
    EXPECT_EQ(json::parse(text_of(*who)).at("scopes"), json::array({"files:read", "files:write"}));
    expect_tokens_kept(world, {}, {text_of(*who)});
    EXPECT_TRUE(client->close());
}

// Neither names a scope: the client asks for none, and the server takes a token that holds none.
TEST_P(ServerEndToEnd, AsksForNoScopeWhenNeitherTheChallengeNorTheMetadataNamesOne) {
    ServerSetup setup;
    setup.required_scopes = {};
    ServerWorld world(std::move(setup));
    use(world);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_EQ(world.person.prompts().size(), 1u);
    EXPECT_FALSE(asked_scope(world.person.prompts().front()));
    const auto who = client->call_tool("whoami");
    ASSERT_TRUE(who) << who.error().message;
    EXPECT_EQ(json::parse(text_of(*who)).at("scopes"), json::array());
    expect_tokens_kept(world, {}, {text_of(*who)});
    EXPECT_TRUE(client->close());
}

// ---- A tool that declares a scope

TEST_P(ServerEndToEnd, StepsUpForTheScopeOfATool) {
    ServerWorld world;
    use(world);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(asked_scope(world.person.prompts().front()), "mcp:use");

    const auto written = client->call_tool("write");
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(text_of(*written), "written");
    EXPECT_EQ(world.items.write.load(), 1);

    // The refusal for want of scope made one more sign-in, for the union of what was asked and what the server said.
    ASSERT_EQ(world.person.prompts().size(), 2u);
    EXPECT_EQ(asked_scope(world.person.prompts()[1]), "mcp:use files:write");
    ASSERT_EQ(world.as.access_tokens().size(), 2u);
    const std::string stepped = world.as.access_tokens()[1];
    EXPECT_EQ(authorized(world).back().authorization, "Bearer " + stepped);
    EXPECT_EQ(authorized(world).back().status, 200);
    // The server answered 403 to the first token for that call, and the registration served both sign-ins.
    const std::vector<int> early = statuses_for(world, world.as.access_tokens().front());
    EXPECT_NE(std::find(early.begin(), early.end(), 403), early.end());
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);

    const auto who = client->call_tool("whoami");
    ASSERT_TRUE(who) << who.error().message;
    EXPECT_EQ(json::parse(text_of(*who)).at("scopes"), json::array({"mcp:use", "files:write"}));
    expect_tokens_kept(world, {}, {text_of(*who)});
    EXPECT_TRUE(client->close());
}

// An authorization server that never grants the scope a tool needs: the client asks again once, sees the same refusal,
// and ends the call within its bound; the tool does not run, and the client goes on with what it may do.
TEST_P(ServerEndToEnd, GivesUpOnAScopeNoAuthorizationServerGrants) {
    ServerWorld world;
    use(world);
    world.as.grant_only({std::string(required_scope)});
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;

    const auto started = std::chrono::steady_clock::now();
    const auto refused = client->call_tool("ungranted");
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::authorization_failed) << refused.error().message;
    ASSERT_TRUE(refused.error().authorization);
    EXPECT_EQ(refused.error().authorization->problem, AuthorizationProblem::insufficient_scope);
    EXPECT_LT(took, std::chrono::seconds(20));
    EXPECT_EQ(world.items.ungranted.load(), 0);
    // One sign-in at the first contact and one for the step-up; the union was asked, and the second refusal asked for
    // nothing the client had not asked for already.
    ASSERT_EQ(world.person.prompts().size(), 2u);
    EXPECT_EQ(asked_scope(world.person.prompts()[1]), "mcp:use never:granted");

    const auto opened = client->call_tool("open");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(text_of(*opened), "open");
    expect_tokens_kept(world, {refused.error()});
    EXPECT_TRUE(client->close());
}

// ---- The server names another authorization server

TEST_P(ServerEndToEnd, SignsInAtTheNewAuthorizationServerAfterTheServerChangedIt) {
    ServerWorld world;
    use(world);
    world.as.issue_refresh_tokens(true);
    auto client = world.connect();
    ASSERT_TRUE(client) << client.error().message;
    const auto before = client->call_tool("open");
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(world.as.access_tokens().size(), 1u);
    const std::string old_token = world.as.access_tokens().front();

    world.name_authorization_servers({world.second.issuer()});
    world.person.sign_in_at(world.second);
    const auto after = client->call_tool("open");
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(text_of(*after), "open");

    // The old token is refused at once, though the server remembered it: its issuer is no longer named. The client did
    // not refresh it at the old server, but registered at the new one and signed in there.
    const std::vector<int> old_statuses = statuses_for(world, old_token);
    ASSERT_GE(old_statuses.size(), 2u);
    EXPECT_EQ(old_statuses.front(), 200);
    EXPECT_EQ(old_statuses.back(), 401);
    EXPECT_EQ(grants_of(world.as, "refresh_token"), 0u);
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);
    EXPECT_EQ(world.second.requests_to("/register").size(), 1u);
    EXPECT_EQ(grants_of(world.second, "authorization_code"), 1u);
    ASSERT_EQ(world.second.access_tokens().size(), 1u);
    const std::string new_token = world.second.access_tokens().front();
    EXPECT_EQ(authorized(world).back().authorization, "Bearer " + new_token);
    EXPECT_EQ(authorized(world).back().status, 200);
    EXPECT_EQ(world.person.prompts().size(), 2u);
    // Where the revision keeps sessions (the client made a handshake), the session belongs to the principal of the token
    // that opened it: the new token is another principal, so the server answers the old session 404 and the client
    // opens a new one. Where it does not, there is nothing to open.
    const std::vector<Seen> requests = world.log().all();
    const auto handshakes = static_cast<std::size_t>(std::count_if(requests.begin(), requests.end(), [](const Seen& one) {
        return one.rpc_method == "initialize" && one.status == 200;
    }));
    const std::vector<int> new_statuses = statuses_for(world, new_token);
    const bool sessions = handshakes > 0;
    EXPECT_EQ(handshakes, sessions ? 2u : 0u);
    EXPECT_EQ(std::count(new_statuses.begin(), new_statuses.end(), 404), sessions ? 1 : 0);
    // The new authorization server never saw the old one's tokens.
    for (const auto& one : world.second.server().requests()) {
        EXPECT_EQ(one.head.find(old_token), std::string::npos) << one.target;
        EXPECT_EQ(one.body.find(old_token), std::string::npos) << one.target;
    }

    // The new token is for this resource, from the new authorization server.
    const auto who = client->call_tool("whoami");
    ASSERT_TRUE(who) << who.error().message;
    EXPECT_EQ(json::parse(text_of(*who)).at("issuer"), world.second.issuer());
    expect_tokens_kept(world, {}, {text_of(*who)});
    EXPECT_TRUE(client->close());
}

// ---- A token for another resource

TEST_P(ServerEndToEnd, SignsInAgainForATokenForAnotherResourceAndGivesUpWithinItsBounds) {
    ServerWorld world;
    use(world);
    world.as.issue_for_audience("https://other.example/mcp");
    const auto started = std::chrono::steady_clock::now();
    auto client = world.connect();
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed) << client.error().message;
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::attempts_exceeded);
    EXPECT_LT(took, std::chrono::seconds(20));

    // Each refusal of a token for another resource made the client sign in again, until the bound on the sign-ins one
    // operation starts in a row (the first, and the step-ups the bound allows); the server answered 401 to every token
    // and ran no tool.
    constexpr std::size_t sign_ins = neomcp::OAuthDefaults::max_step_ups_per_operation + 1;
    EXPECT_EQ(world.person.prompts().size(), sign_ins);
    EXPECT_EQ(world.as.access_tokens().size(), sign_ins);
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);
    for (const std::string& token : world.as.access_tokens()) {
        const std::vector<int> statuses = statuses_for(world, token);
        ASSERT_FALSE(statuses.empty());
        for (const int status : statuses) EXPECT_EQ(status, 401);
    }
    EXPECT_EQ(world.items.open.load(), 0);
    expect_tokens_kept(world, {client.error()});
}

INSTANTIATE_TEST_SUITE_P(ServerEndToEndRevisions, ServerEndToEnd,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28", "all"),
                         [](const auto& info) { return revision_name(info.param); });

}  // namespace
