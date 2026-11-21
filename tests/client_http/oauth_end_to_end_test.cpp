// OAuthAuthorizer end to end over loopback, a Client against a scripted resource and authorization server, for each
// revision the client speaks over Streamable HTTP (the handshake revisions and the one without): signing in with a
// dynamic registration, with a client ID metadata document, and with credentials registered beforehand (Basic and in
// the body); a refresh without the person; a step-up for scope; a changed authorization server; two clients that share
// one store. Then the older transport of events, reached after the Streamable handshake is refused, with the token sent
// only to the endpoint's origin. Every test also checks where the secrets went: the bearer only to the resource, no
// secret in any URL, the protocol version only to the resource's origin.
#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "http_sse_server.h"
#include "neomcp/base64.h"
#include "neomcp/http/message.h"
#include "neomcp/server.h"
#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::ClientErrorKind;

class OAuthEndToEndEra : public ::testing::TestWithParam<std::string> {
protected:
    OAuthEndToEndEra() { world.revisions = {GetParam()}; }

    void expect_era(neomcp::Client& client) {
        const auto verdict = client.verdict();
        ASSERT_TRUE(verdict);
        EXPECT_EQ(verdict->revision, GetParam());
    }

    World world;
};

// ---- Signing in

TEST_P(OAuthEndToEndEra, SignsInWithADynamicRegistration) {
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    expect_era(*client);
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;

    ASSERT_EQ(world.person.prompts().size(), 1u);
    const auto registrations = world.as.requests_to("/register");
    ASSERT_EQ(registrations.size(), 1u);
    const json registration = json::parse(registrations.front().body);
    EXPECT_EQ(registration["token_endpoint_auth_method"], "none");
    EXPECT_FALSE(registration.contains("client_secret"));
    EXPECT_EQ(grants_of(world.as, "authorization_code"), 1u);
    EXPECT_EQ(field_of(world.as.requests_to("/token").front().body, "client_id"), world.as.client_id());
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer one-access-1");
    expect_secrets_kept(world);
    EXPECT_TRUE(client->close());
}

TEST_P(OAuthEndToEndEra, SignsInWithAClientIdMetadataDocument) {
    const std::string document = "https://app.example/oauth/client.json";
    world.as.edit_metadata([](json& metadata) { metadata["client_id_metadata_document_supported"] = true; });
    neomcp::OAuthOptions options = world.options();
    options.client_id_metadata_url = document;
    auto client = world.open(world.authorizer(std::move(options)));
    ASSERT_TRUE(client) << client.error().message;
    expect_era(*client);

    EXPECT_TRUE(world.as.requests_to("/register").empty());
    ASSERT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(field_of(query_of(world.person.prompts().front().authorization_url), "client_id"), document);
    const auto tokens = world.as.requests_to("/token");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(field_of(tokens.front().body, "client_id"), document);
    EXPECT_FALSE(field_of(tokens.front().body, "client_secret"));
    EXPECT_FALSE(tokens.front().field("Authorization"));
    expect_secrets_kept(world);
    EXPECT_TRUE(client->close());
}

// The identifier and the secret are form-encoded before they are joined for Basic: a space, ':' and '&' in either
// survive the trip.
TEST_P(OAuthEndToEndEra, SignsInWithCredentialsRegisteredBeforehandByBasic) {
    const std::string id = "pre id:1&x";
    const std::string secret = "s p:c&t" + std::string(planted_secret);
    world.as.edit_metadata([](json& metadata) {
        metadata["token_endpoint_auth_methods_supported"] = json::array({"client_secret_basic"});
    });
    neomcp::OAuthOptions options = world.options();
    options.dynamic_registration = false;
    options.pre_registered.push_back({world.as.issuer(), id, secret, std::nullopt});
    auto client = world.open(world.authorizer(std::move(options)));
    ASSERT_TRUE(client) << client.error().message;
    expect_era(*client);

    EXPECT_TRUE(world.as.requests_to("/register").empty());
    EXPECT_EQ(field_of(query_of(world.person.prompts().front().authorization_url), "client_id"), id);
    const auto tokens = world.as.requests_to("/token");
    ASSERT_EQ(tokens.size(), 1u);
    // form("pre id:1&x") ":" form(secret), each encoded by hand.
    const std::string joined = "pre+id%3A1%26x:s+p%3Ac%26t" + std::string(planted_secret);
    EXPECT_EQ(tokens.front().field("Authorization"), "Basic " + neomcp::base64_encode(joined));
    EXPECT_FALSE(field_of(tokens.front().body, "client_secret"));
    EXPECT_EQ(tokens.front().body.find(std::string(planted_secret)), std::string::npos);
    // The secret went to the token endpoint only.
    for (const Request& one : world.as.server().requests()) {
        if (one.target == "/token") continue;
        EXPECT_EQ(one.head.find(neomcp::base64_encode(joined)), std::string::npos) << one.target;
        EXPECT_EQ(one.body.find(std::string(planted_secret)), std::string::npos) << one.target;
    }
    expect_secrets_kept(world, {}, {secret, neomcp::base64_encode(joined)});
    EXPECT_TRUE(client->close());
}

TEST_P(OAuthEndToEndEra, SignsInWithCredentialsRegisteredBeforehandInTheBody) {
    world.as.edit_metadata([](json& metadata) {
        metadata["token_endpoint_auth_methods_supported"] = json::array({"client_secret_post"});
    });
    neomcp::OAuthOptions options = world.options();
    options.dynamic_registration = false;
    options.pre_registered.push_back({world.as.issuer(), "pre-client", std::string(planted_secret), std::nullopt});
    auto client = world.open(world.authorizer(std::move(options)));
    ASSERT_TRUE(client) << client.error().message;
    expect_era(*client);

    const auto tokens = world.as.requests_to("/token");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(field_of(tokens.front().body, "client_id"), "pre-client");
    EXPECT_EQ(field_of(tokens.front().body, "client_secret"), planted_secret);
    EXPECT_FALSE(tokens.front().field("Authorization"));
    for (const Request& one : world.as.server().requests()) {
        if (one.target == "/token") continue;
        EXPECT_EQ(one.head.find(std::string(planted_secret)), std::string::npos) << one.target;
        EXPECT_EQ(one.body.find(std::string(planted_secret)), std::string::npos) << one.target;
    }
    expect_secrets_kept(world);
    EXPECT_TRUE(client->close());
}

// ---- After signing in

TEST_P(OAuthEndToEndEra, RefreshesAfterExpiryWithoutAPrompt) {
    world.as.issue_refresh_tokens(true);
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    expect_era(*client);
    world.ledger.expire_all();

    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(grants_of(world.as, "refresh_token"), 1u);
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer one-access-2");
    expect_secrets_kept(world);
    EXPECT_TRUE(client->close());
}

TEST_P(OAuthEndToEndEra, StepsUpWhenRefusedForWantOfScope) {
    world.resource.challenge_scope("mcp:basic");
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    expect_era(*client);
    world.resource.require_scopes({"mcp:write"});

    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(world.person.prompts().size(), 2u);
    EXPECT_EQ(field_of(query_of(world.person.prompts()[1].authorization_url), "scope"), "mcp:basic mcp:write");
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer one-access-2");
    expect_secrets_kept(world);
    EXPECT_TRUE(client->close());
}

TEST_P(OAuthEndToEndEra, SignsInAnewWhereTheAuthorizationServerChanged) {
    world.as.issue_refresh_tokens(true);
    AuthServer second(world.ledger, "two");
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    expect_era(*client);

    world.resource.use(second);
    world.person.sign_in_at(second);
    world.ledger.expire_all();
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(grants_of(world.as, "refresh_token"), 0u);
    EXPECT_EQ(second.requests_to("/register").size(), 1u);
    for (const Request& one : second.server().requests()) {
        EXPECT_EQ(one.head.find("one-"), std::string::npos) << one.target;
        EXPECT_EQ(one.body.find("one-"), std::string::npos) << one.target;
    }
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer two-access-1");
    // The first server's tokens and registration are gone from the store, so nothing can send or refresh them later.
    EXPECT_FALSE(world.store->tokens({world.resource.url(), world.as.issuer()}));
    EXPECT_FALSE(world.store->client(world.as.issuer()));
    EXPECT_TRUE(world.store->tokens({world.resource.url(), second.issuer()}));
    expect_secrets_kept(world, {&second});
    EXPECT_TRUE(client->close());
}

// Two clients of one store: the second signs in with what the first obtained, and after the first refreshed, the
// second sends the new token without a refresh of its own.
TEST_P(OAuthEndToEndEra, TwoClientsShareOneStore) {
    world.as.issue_refresh_tokens(true);
    auto first = world.open(world.authorizer());
    ASSERT_TRUE(first) << first.error().message;
    auto second = world.open(world.authorizer());
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);

    // Both at once, from two threads.
    auto one = std::async(std::launch::async, [&] { return first->request("tools/list"); });
    auto two = std::async(std::launch::async, [&] { return second->request("tools/list"); });
    const auto one_listed = one.get();
    const auto two_listed = two.get();
    ASSERT_TRUE(one_listed) << one_listed.error().message;
    ASSERT_TRUE(two_listed) << two_listed.error().message;

    world.ledger.expire_all();
    const auto refreshed = first->request("tools/list");
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    const auto after = second->request("tools/list");
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(world.resource.authorizations().back(), "Bearer one-access-2");
    EXPECT_EQ(grants_of(world.as, "refresh_token"), 1u);
    EXPECT_EQ(world.person.prompts().size(), 1u);
    expect_secrets_kept(world);
    EXPECT_TRUE(first->close());
    EXPECT_TRUE(second->close());
}

INSTANTIATE_TEST_SUITE_P(OAuthEndToEnd, OAuthEndToEndEra,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"),
                         [](const auto& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return "Revision_" + name;
                         });

// ---- The older transport of events

// The library's session engine behind the test-only server of events, which refuses every request without a bearer
// the authorization server issued and serves its protected resource metadata at its own path with a query.
class EventsResource {
public:
    EventsResource(Ledger& ledger, const AuthServer& as, http_sse_test::HttpSseServerOptions options = {})
        : ledger_(ledger), issuer_(as.issuer()) {
        options.intercept = [this](const neomcp::http::Request& request) { return intercept(request); };
        sse_ = std::make_unique<http_sse_test::HttpSseServer>(engine_, std::move(options));
        std::lock_guard lock(mutex_);
        url_ = sse_->url();
    }

    std::string url() const {
        std::lock_guard lock(mutex_);
        return url_;
    }
    http_sse_test::HttpSseServer& sse() { return *sse_; }

    struct Seen {
        std::string method;
        std::string query;
        std::string authorization;
    };
    std::vector<Seen> seen() const {
        std::lock_guard lock(mutex_);
        return seen_;
    }

private:
    std::optional<neomcp::http::Response> intercept(const neomcp::http::Request& request) {
        const auto value = request.headers.get("Authorization");
        const std::string authorization = value ? std::string(*value) : std::string();
        std::string own_url;
        {
            std::lock_guard lock(mutex_);
            seen_.push_back({request.method, request.query, authorization});
            own_url = url_;
        }
        if (request.method == "GET" && request.query == "metadata") {
            const json document{{"resource", own_url}, {"authorization_servers", json::array({issuer_})}};
            return neomcp::http::make_response(200, "application/json", document.dump());
        }
        const std::string prefix = "Bearer ";
        const bool good = authorization.rfind(prefix, 0) == 0 && ledger_.scopes_of(authorization.substr(prefix.size()));
        if (good) return std::nullopt;
        neomcp::http::Response refusal = neomcp::http::make_response(401);
        refusal.headers.set("WWW-Authenticate", "Bearer resource_metadata=\"" + own_url + "?metadata\"");
        return refusal;
    }

    Ledger& ledger_;
    const std::string issuer_;
    // Declared before the server, which a request may still be reaching while it stops.
    mutable std::mutex mutex_;
    std::string url_;
    std::vector<Seen> seen_;
    neomcp::Server engine_{neomcp::ServerOptions{.name = "oauth-events", .version = "1.0.0"}};
    std::unique_ptr<http_sse_test::HttpSseServer> sse_;
};

TEST(OAuthEndToEnd, TheOlderTransportAfterTheStreamableHandshakeIsRefusedCarriesTheTokenToItsOriginOnly) {
    World world;
    EventsResource events(world.ledger, world.as);
    auto authorizer = world.authorizer();
    auto described = http_rig::endpoint(events.url());
    described.authorizer = authorizer;
    auto client = neomcp::Client::open(world.client_options(), std::move(described));
    ASSERT_TRUE(client) << client.error().message;
    ASSERT_TRUE(client->verdict());
    EXPECT_EQ(client->verdict()->transport, neomcp::profile::Transport::http_sse);
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;

    // One sign-in served both transports.
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(grants_of(world.as, "authorization_code"), 1u);
    EXPECT_EQ(events.sse().streams(), 1u);
    EXPECT_GE(events.sse().posted("initialize"), 1u);
    EXPECT_GE(events.sse().posted("tools/list"), 1u);

    // Before the sign-in the requests went without a token; after it, every request to the endpoint's origin carried
    // the bearer, the stream and the messages of the older transport included.
    const auto seen = events.seen();
    std::size_t carried = 0;
    bool signed_in = false;
    for (const EventsResource::Seen& one : seen) {
        if (!one.authorization.empty()) signed_in = true;
        if (one.query == "metadata") {
            EXPECT_EQ(one.authorization, "") << "the metadata request carried a token";
            continue;
        }
        if (signed_in) {
            EXPECT_EQ(one.authorization, "Bearer one-access-1") << one.method << " ?" << one.query;
            ++carried;
        }
    }
    EXPECT_GE(carried, 4u);  // the probe again, the handshake of Streamable HTTP, the stream, the messages
    expect_secrets_kept(world);
    EXPECT_TRUE(client->close());
}

}  // namespace
