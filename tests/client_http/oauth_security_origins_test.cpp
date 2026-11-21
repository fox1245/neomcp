// Where the client's secrets may go, over loopback against the scripted authorization server and resource: the access
// token only in the Authorization field of requests to the endpoint's origin (never after a redirect to another origin,
// never to an endpoint of events on another origin, never to a plain HTTP endpoint elsewhere, never to the documents
// and endpoints of authorization even where they share the resource's origin), the client secret only to the token
// endpoint wherever the metadata puts it, and no secret in any URL. And what a document may not make the client do: a
// protected resource metadata document named on another origin is never fetched, one that names another resource is
// not used, and an authorization server document that names another issuer is not used.
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "http_sse_server.h"
#include "neomcp/http/message.h"
#include "neomcp/server.h"
#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;

void expect_failure(neomcp::Result<neomcp::Client>& client, AuthorizationProblem problem) {
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed) << client.error().message;
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, problem);
}

bool carries_bearer(const Request& request) {
    const auto value = request.field("Authorization");
    return value && value->rfind("Bearer", 0) == 0;
}

// ---- The access token

TEST(OAuthSecurity, TheTokenNeverFollowsARedirectToAnotherOrigin) {
    World world;
    ScriptedHttpServer elsewhere([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json{{"tools", json::array()}}));
    });
    const std::string target = elsewhere.url("/mcp");
    world.resource.override_with([target](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target == "/mcp" && carries_bearer(request) && request.rpc_method() == "tools/list") {
            return http_test::empty_reply(307, {{"Location", target}});
        }
        return std::nullopt;
    });
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    const auto listed = client->request("tools/list");
    EXPECT_FALSE(listed);
    for (const Request& one : elsewhere.requests()) EXPECT_FALSE(one.field("Authorization")) << one.target;
    EXPECT_EQ(elsewhere.count(), 0u);
    expect_secrets_kept(world);
    if (!listed) expect_quiet(world, {listed.error()}, world.secrets());
    EXPECT_TRUE(client->close());
}

// The older transport's endpoint event names another origin: the client goes nowhere with the token.
TEST(OAuthSecurity, AnEndpointOfEventsOnAnotherOriginGetsNoToken) {
    World world;
    ScriptedHttpServer elsewhere([](const Request&) { return http_test::empty_reply(202); });
    http_sse_test::HttpSseServerOptions options;
    const std::string other = elsewhere.url("/sse/stream");
    options.endpoint_reference = [other](const std::string& query) { return other + "?" + query; };
    std::mutex mutex;
    std::string url;
    std::vector<std::string> carried;
    options.intercept = [&](const neomcp::http::Request& request) -> std::optional<neomcp::http::Response> {
        const auto value = request.headers.get("Authorization");
        std::string own;
        {
            std::lock_guard lock(mutex);
            if (value) carried.emplace_back(*value);
            own = url;
        }
        if (request.method == "GET" && request.query == "metadata") {
            const json document{{"resource", own}, {"authorization_servers", json::array({world.as.issuer()})}};
            return neomcp::http::make_response(200, "application/json", document.dump());
        }
        const std::string_view prefix = "Bearer ";
        if (value && value->substr(0, prefix.size()) == prefix &&
            world.ledger.scopes_of(std::string(value->substr(prefix.size())))) {
            return std::nullopt;
        }
        neomcp::http::Response refusal = neomcp::http::make_response(401);
        refusal.headers.set("WWW-Authenticate", "Bearer resource_metadata=\"" + own + "?metadata\"");
        return refusal;
    };
    neomcp::Server engine(neomcp::ServerOptions{.name = "oauth-events", .version = "1.0.0"});
    http_sse_test::HttpSseServer events(engine, std::move(options));
    {
        std::lock_guard lock(mutex);
        url = events.url();
    }
    auto described = http_rig::endpoint(events.url());
    described.authorizer = world.authorizer();
    auto client = neomcp::Client::open(world.client_options(), std::move(described));
    ASSERT_FALSE(client);
    EXPECT_EQ(elsewhere.count(), 0u) << "a request reached the other origin";
    expect_quiet(world, {client.error()}, world.secrets());
    // The token went to the endpoint's own origin only, and the stream was opened with it.
    {
        std::lock_guard lock(mutex);
        EXPECT_FALSE(carried.empty());
    }
    expect_secrets_kept(world);
    events.stop();
}

// A plain HTTP endpoint on another host (reached through a proxy here) gets no token, and no sign-in starts for it.
TEST(OAuthSecurity, APlainHttpEndpointElsewhereGetsNoToken) {
    World world;
    const std::string endpoint = "http://mcp.example.test/mcp";
    ScriptedHttpServer proxy([](const Request&) {
        return http_test::empty_reply(
            401, {{"WWW-Authenticate",
                   "Bearer resource_metadata=\"http://mcp.example.test/.well-known/oauth-protected-resource/mcp\""}});
    });
    // Tokens for that endpoint are in the store already: they are not sent either.
    world.store->put_tokens({endpoint, world.as.issuer()}, neomcp::StoredTokens{"stored-access", {}, {}, {}, {}});
    auto described = http_rig::endpoint(endpoint);
    described.authorizer = world.authorizer();
    described.proxy.mode = neomcp::ProxyMode::named;
    described.proxy.url = proxy.url("");
    auto client = neomcp::Client::open(world.client_options(), std::move(described));
    expect_failure(client, AuthorizationProblem::insecure_endpoint);
    ASSERT_TRUE(proxy.wait_for(1));
    for (const Request& one : proxy.requests()) {
        EXPECT_FALSE(one.field("Authorization")) << one.target;
        EXPECT_NE(one.target.find("mcp.example.test/mcp"), std::string::npos) << one.target;
    }
    EXPECT_EQ(world.as.server().count(), 0u);
    EXPECT_TRUE(world.person.prompts().empty());
    std::vector<std::string> secrets = world.secrets();
    secrets.emplace_back("stored-access");
    expect_quiet(world, {client.error()}, secrets);
}

// The token and registration endpoints share the resource's origin. The documents and endpoints there get no bearer
// though one exists (a refresh, then a step-up, each fetches them again while a token is held).
TEST(OAuthSecurity, TheResourcesOwnOriginGetsNoBearerOutsideTheEndpoint) {
    World world;
    world.as.issue_refresh_tokens(true);
    const std::string origin = world.resource.origin();
    world.as.edit_metadata([origin](json& metadata) {
        metadata["token_endpoint"] = origin + "/token";
        metadata["registration_endpoint"] = origin + "/register";
    });
    AuthServer& as = world.as;
    world.resource.override_with([&as](const Request& request) -> std::optional<http_test::Reply> {
        if (request.target == "/token" || request.target == "/register") return as.answer(request);
        return std::nullopt;
    });
    world.resource.challenge_scope("mcp:basic");
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    world.ledger.expire_all();
    ASSERT_TRUE(client->request("tools/list"));
    world.resource.require_scopes({"mcp:write"});
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;

    std::size_t elsewhere = 0;
    for (const Request& one : world.resource.requests()) {
        if (one.target == "/mcp") continue;
        ++elsewhere;
        EXPECT_FALSE(one.field("Authorization")) << one.target;
    }
    // Three metadata requests, a registration, and three token requests (code, refresh, code).
    EXPECT_GE(elsewhere, 7u);
    EXPECT_EQ(world.person.prompts().size(), 2u);
    expect_secrets_kept(world);
    EXPECT_TRUE(client->close());
}

// ---- The client secret

// The authorization server's document puts its endpoints on another origin. That origin gets the registration and
// the token requests, the secret in the token requests only; the server that published the document gets nothing but
// the request for it; the access token goes to the resource alone.
TEST(OAuthSecurity, TheSecretGoesOnlyToTheTokenEndpointWhereverItIs) {
    World world;
    AuthServer endpoints(world.ledger, "two");
    endpoints.register_with_secret(true);
    const std::string elsewhere = endpoints.issuer();
    world.as.edit_metadata([elsewhere](json& metadata) {
        metadata["authorization_endpoint"] = elsewhere + "/authorize";
        metadata["token_endpoint"] = elsewhere + "/token";
        metadata["registration_endpoint"] = elsewhere + "/register";
    });
    world.person.sign_in_at(endpoints);
    world.as.issue_refresh_tokens(true);
    endpoints.issue_refresh_tokens(true);
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    world.ledger.expire_all();
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;

    for (const Request& one : world.as.server().requests()) {
        EXPECT_EQ(one.target, "/.well-known/oauth-authorization-server");
    }
    const std::string secret(planted_secret);
    std::size_t with_secret = 0;
    for (const Request& one : endpoints.server().requests()) {
        const bool holds = one.body.find(secret) != std::string::npos || one.head.find(secret) != std::string::npos;
        if (one.target == "/token") {
            EXPECT_EQ(field_of(one.body, "client_secret"), secret);
            ++with_secret;
        } else {
            EXPECT_FALSE(holds) << one.target;
        }
    }
    EXPECT_EQ(with_secret, 2u);  // the code, then the refresh
    for (const Request& one : world.resource.requests()) {
        EXPECT_EQ(one.body.find(secret), std::string::npos);
        EXPECT_EQ(one.head.find(secret), std::string::npos);
    }
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer two-access-2");
    expect_secrets_kept(world, {&endpoints});
    EXPECT_TRUE(client->close());
}

// ---- What a document may not make the client do

TEST(OAuthSecurity, ResourceMetadataNamedOnAnotherOriginIsNeverFetched) {
    World world;
    ScriptedHttpServer elsewhere([&world](const Request&) {
        return http_test::json_reply(200, json{{"resource", world.resource.url()},
                                               {"authorization_servers", json::array({world.as.issuer()})}});
    });
    world.resource.challenge_names(elsewhere.url("/.well-known/oauth-protected-resource/mcp"));
    auto client = world.open(world.authorizer());
    expect_failure(client, AuthorizationProblem::refused_url);
    EXPECT_EQ(elsewhere.count(), 0u);
    EXPECT_EQ(world.as.server().count(), 0u);
    EXPECT_TRUE(world.person.prompts().empty());
    expect_quiet(world, {client.error()}, world.secrets());
}

// The resource a document names must be the endpoint itself: no other path, no slash more, no other case of the path,
// not the origin alone (named by the challenge), not another host.
TEST(OAuthSecurity, ResourceMetadataNamingAnotherResourceIsNotUsed) {
    const std::vector<std::function<std::string(const World&)>> others = {
        [](const World& w) { return w.resource.url() + "/"; },
        [](const World& w) { return w.resource.origin() + "/MCP"; },
        [](const World& w) { return w.resource.origin(); },
        [](const World& w) { return w.resource.origin() + "/mcp/other"; },
        [](const World&) { return std::string("https://mcp.example/mcp"); },
        [](const World& w) { return w.resource.url() + "%2F"; },
    };
    for (const auto& other : others) {
        World world;
        const std::string named = other(world);
        SCOPED_TRACE(named);
        world.resource.edit_metadata([named](json& document) { document["resource"] = named; });
        auto client = world.open(world.authorizer());
        expect_failure(client, AuthorizationProblem::resource_mismatch);
        EXPECT_EQ(world.as.server().count(), 0u);
        EXPECT_TRUE(world.person.prompts().empty());
        if (!client) expect_quiet(world, {client.error()}, world.secrets());
    }
}

// The issuer an authorization server's document names must be the one it was found by: nothing of a document that
// names another is used, and no other address of it is tried.
TEST(OAuthSecurity, AnAuthorizationServerDocumentNamingAnotherIssuerIsNotUsed) {
    const std::vector<std::function<std::string(const std::string&)>> others = {
        [](const std::string& issuer) { return issuer + "/"; },
        [](const std::string& issuer) { return "HTTP" + issuer.substr(std::string("http").size()); },
        [](const std::string&) { return std::string("https://as.example"); },
    };
    for (const auto& other : others) {
        World world;
        const std::string named = other(world.as.issuer());
        SCOPED_TRACE(named);
        world.as.edit_metadata([named](json& document) { document["issuer"] = named; });
        auto client = world.open(world.authorizer());
        expect_failure(client, AuthorizationProblem::metadata_issuer_mismatch);
        EXPECT_EQ(world.as.server().count(), 1u);
        EXPECT_TRUE(world.person.prompts().empty());
        if (!client) expect_quiet(world, {client.error()}, world.secrets());
    }
}

}  // namespace
