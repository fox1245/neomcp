// What an authorizer fetches through its transport (AuthorizationHost::fetch) against scripted peers: a document of
// another origin without the token of the MCP requests, redirects only within the fetched URL's origin and only for a
// metadata GET, the bound on the body and on the time, the rule for the URLs, and the end of a fetch when the transport
// closes.
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include "auth_rig.h"
#include "neomcp/client/http.h"
#include "rig.h"
#include "scripted_http_server.h"

namespace {

using auth_rig::ScriptedAuthorizer;
using http_rig::Driven;
using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::AuthorizationFetch;
using neomcp::AuthorizationFetchError;
using neomcp::AuthorizationFetchMethod;
using neomcp::AuthorizationFetchPurpose;
using neomcp::AuthorizationFetchResult;

constexpr const char* token = "Bearer mcp-token";

// A transport for the MCP endpoint `mcp` with an authorizer whose host the test fetches through.
struct Rig {
    explicit Rig(const std::string& mcp) : authorizer(std::make_shared<ScriptedAuthorizer>(token, 0)) {
        auto described = http_rig::endpoint(mcp);
        described.authorizer = authorizer;
        driven = std::make_unique<Driven>(described);
    }

    // Starts a fetch on the client's thread.
    std::future<AuthorizationFetchResult> start(AuthorizationFetch request) {
        neomcp::AuthorizationHost* host = authorizer->host();
        return asio::co_spawn(
            driven->io().executor(),
            [host, request = std::move(request)]() mutable -> asio::awaitable<AuthorizationFetchResult> {
                AuthorizationFetchResult result = co_await host->fetch(std::move(request));
                co_return result;
            },
            asio::use_future);
    }

    AuthorizationFetchResult fetch(AuthorizationFetch request) {
        auto done = start(std::move(request));
        if (done.wait_for(http_rig::wait_bound) != std::future_status::ready) {
            ADD_FAILURE() << "the fetch did not end";
            return {};
        }
        return done.get();
    }

    std::shared_ptr<ScriptedAuthorizer> authorizer;
    std::unique_ptr<Driven> driven;
};

ScriptedHttpServer mcp_peer() {
    return ScriptedHttpServer([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
}

AuthorizationFetch get(std::string url) {
    AuthorizationFetch request;
    request.purpose = AuthorizationFetchPurpose::server_metadata;
    request.method = AuthorizationFetchMethod::get;
    request.url = std::move(url);
    request.headers = {{"Accept", "application/json"}};
    return request;
}

TEST(ClientHttpAuth, FetchGetsADocumentOfAnotherOriginWithoutTheMcpToken) {
    auto mcp = mcp_peer();
    ScriptedHttpServer issuer([](const Request&) {
        return http_test::json_reply(200, json{{"issuer", "x"}});
    });
    Rig rig(mcp.url());
    const auto result = rig.fetch(get(issuer.url("/.well-known/oauth-authorization-server")));
    ASSERT_FALSE(result.error) << result.message;
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.media_type, "application/json");
    EXPECT_EQ(json::parse(result.body), (json{{"issuer", "x"}}));
    ASSERT_TRUE(issuer.wait_for(1));
    const Request request = issuer.requests().front();
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.field("Accept"), "application/json");
    EXPECT_FALSE(request.field("Authorization"));
    EXPECT_EQ(mcp.count(), 0u);
}

TEST(ClientHttpAuth, FetchKeepsABodyOfAnyTypeWhole) {
    auto mcp = mcp_peer();
    ScriptedHttpServer issuer([](const Request&) {
        return http_test::raw_reply(404, {{"Content-Type", "text/html"}}, "<p>not here</p>");
    });
    Rig rig(mcp.url());
    const auto result = rig.fetch(get(issuer.url("/x")));
    ASSERT_FALSE(result.error) << result.message;
    EXPECT_EQ(result.status, 404);
    EXPECT_EQ(result.media_type, "text/html");
    EXPECT_EQ(result.body, "<p>not here</p>");
}

TEST(ClientHttpAuth, FetchFollowsARedirectOfAMetadataGetOnlyWithinItsOrigin) {
    auto mcp = mcp_peer();
    ScriptedHttpServer other([](const Request&) { return http_test::json_reply(200, json::object()); });
    const std::string away = other.url("/doc");
    ScriptedHttpServer issuer([away](const Request& request) {
        if (request.target == "/moved") return http_test::empty_reply(307, {{"Location", "/doc"}});
        if (request.target == "/away") return http_test::empty_reply(308, {{"Location", away}});
        return http_test::json_reply(200, json{{"at", request.target}});
    });
    Rig rig(mcp.url());
    const auto followed = rig.fetch(get(issuer.url("/moved")));
    ASSERT_FALSE(followed.error) << followed.message;
    EXPECT_EQ(followed.status, 200);
    EXPECT_EQ(json::parse(followed.body), (json{{"at", "/doc"}}));
    const auto refused = rig.fetch(get(issuer.url("/away")));
    ASSERT_FALSE(refused.error) << refused.message;
    EXPECT_EQ(refused.status, 308);
    EXPECT_EQ(other.count(), 0u);
}

TEST(ClientHttpAuth, FetchOfAPostFollowsNoRedirect) {
    auto mcp = mcp_peer();
    ScriptedHttpServer issuer([](const Request& request) {
        if (request.target == "/token") return http_test::empty_reply(307, {{"Location", "/elsewhere"}});
        return http_test::json_reply(200, json::object());
    });
    Rig rig(mcp.url());
    AuthorizationFetch request;
    request.purpose = AuthorizationFetchPurpose::token;
    request.method = AuthorizationFetchMethod::post;
    request.url = issuer.url("/token");
    request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Authorization", "Basic aWQ6c2VjcmV0"}};
    request.body = "grant_type=authorization_code&code=c";
    const auto result = rig.fetch(std::move(request));
    ASSERT_FALSE(result.error) << result.message;
    EXPECT_EQ(result.status, 307);
    ASSERT_TRUE(issuer.wait_for(1));
    EXPECT_EQ(issuer.count(), 1u);
    const Request sent = issuer.requests().front();
    EXPECT_EQ(sent.body, "grant_type=authorization_code&code=c");
    // The authorizer's own field for the token endpoint goes as written; the MCP token does not.
    EXPECT_EQ(sent.fields_named("Authorization"), std::vector<std::string>{"Basic aWQ6c2VjcmV0"});
}

TEST(ClientHttpAuth, FetchOfABodyOverItsBoundFails) {
    auto mcp = mcp_peer();
    ScriptedHttpServer issuer([](const Request&) {
        return http_test::json_reply(200, json{{"padding", std::string(200, 'x')}});
    });
    Rig rig(mcp.url());
    AuthorizationFetch request = get(issuer.url("/doc"));
    request.max_body_bytes = 64;
    const auto result = rig.fetch(std::move(request));
    EXPECT_EQ(result.error, AuthorizationFetchError::too_large);
}

TEST(ClientHttpAuth, FetchThatTakesTooLongTimesOut) {
    auto mcp = mcp_peer();
    auto never = std::make_shared<http_test::Gate>();
    ScriptedHttpServer issuer([never](const Request&) {
        http_test::Reply reply;
        reply.wait(never);
        return reply;
    });
    Rig rig(mcp.url());
    AuthorizationFetch request = get(issuer.url("/doc"));
    request.timeout = std::chrono::milliseconds(200);
    const auto result = rig.fetch(std::move(request));
    EXPECT_EQ(result.error, AuthorizationFetchError::timed_out);
    never->open();
}

TEST(ClientHttpAuth, FetchOfARefusedUrlGoesNowhere) {
    auto mcp = mcp_peer();
    ScriptedHttpServer issuer([](const Request&) { return http_test::json_reply(200, json::object()); });
    Rig rig(mcp.url());
    for (const std::string& url : {std::string("http://auth.example.test/doc"),  // plain HTTP elsewhere
                                   issuer.url("/doc") + "#fragment",
                                   "http://user@127.0.0.1:" + std::to_string(issuer.port()) + "/doc",
                                   std::string("/relative")}) {
        const auto result = rig.fetch(get(url));
        EXPECT_EQ(result.error, AuthorizationFetchError::refused_url) << url;
    }
    AuthorizationFetch split = get(issuer.url("/doc"));
    split.headers = {{"X-Split", "a\r\nInjected: 1"}};
    EXPECT_EQ(rig.fetch(std::move(split)).error, AuthorizationFetchError::invalid_request);
    EXPECT_EQ(issuer.count(), 0u);
}

TEST(ClientHttpAuth, FetchInFlightEndsClosedWhenTheTransportCloses) {
    auto mcp = mcp_peer();
    auto never = std::make_shared<http_test::Gate>();
    ScriptedHttpServer issuer([never](const Request&) {
        http_test::Reply reply;
        reply.wait(never);
        return reply;
    });
    Rig rig(mcp.url());
    auto pending = rig.start(get(issuer.url("/doc")));
    ASSERT_TRUE(issuer.wait_for(1));
    rig.driven->close();
    ASSERT_EQ(pending.wait_for(http_rig::wait_bound), std::future_status::ready);
    EXPECT_EQ(pending.get().error, AuthorizationFetchError::closed);
    EXPECT_EQ(rig.authorizer->detaches(), 1);
    never->open();
}

}  // namespace
