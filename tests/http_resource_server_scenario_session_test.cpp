// Scenarios of the Streamable HTTP endpoint as an OAuth protected resource, over loopback sockets: the metadata
// document (its exact bytes, where it is served, that no request can change it), the order of the checks before the gate
// and after it, the gate on the first request of every revision, the owner of a session (who is the same principal and
// what another one learns), and the end of a stream with its token and the way back in.
//
// The tests that complement the other files of this feature are named ResourceServerScenario*.
#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/resource_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr const char* public_document_url = "http://localhost/.well-known/oauth-protected-resource/mcp";

// What the document is, byte for byte, for the options of the harness.
constexpr const char* expected_document =
    R"({"resource":"http://localhost/mcp","authorization_servers":["https://auth.example"],)"
    R"("scopes_supported":["mcp:use","files:read","files:write"],"bearer_methods_supported":["header"],)"
    R"("resource_name":"Test resource"})";

std::string shape_of(const Reply& reply) {
    return std::to_string(reply.status) + "|" + reply.header("Content-Type") + "|" + reply.header("WWW-Authenticate") + "|" +
           reply.body;
}

// ---- The metadata document -------------------------------------------------------------------------------------------

TEST(ResourceServerScenarioMetadata, TheDocumentIsExactlyTheConfiguredBytesWhateverTheRequestSays) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.allowed_hosts = {"localhost", "127.0.0.1", "alias.test"}; });
    const std::string port = std::to_string(harness.port);
    harness.token("good", granted(harness.clock, "a", {"mcp:use"}));
    const std::vector<httplib::Headers> requests{
        {},
        {{"Host", "localhost"}},
        {{"Host", "alias.test"}},
        {{"Host", "alias.test:" + port}},
        {{"X-Forwarded-Host", "elsewhere.example"}, {"X-Forwarded-Proto", "https"}, {"X-Forwarded-Port", "8443"}},
        {{"Forwarded", "host=elsewhere.example;proto=https"}, {"X-Forwarded-For", "203.0.113.9"}},
        {{"X-Forwarded-Prefix", "/gateway"}, {"X-Original-URL", "https://elsewhere.example/mcp"}, {"X-Rewrite-URL", "/x"}},
    };
    for (const httplib::Headers& extra : requests) {
        const Reply document = send(harness.port, "GET", extra, {}, document_path_of_endpoint);
        ASSERT_EQ(document.status, 200);
        EXPECT_EQ(document.body, expected_document);
        EXPECT_EQ(document.header("Content-Type"), "application/json");
        EXPECT_EQ(document.header("Cache-Control"), "max-age=300");
        // The challenge points at the same address, however the request came.
        httplib::Headers headers = modern_headers("tools/list", std::nullopt);
        for (const auto& field : extra) headers.emplace(field.first, field.second);
        const Reply refused = send(harness.port, "POST", std::move(headers), rpc(1, "tools/list", modern_params()));
        ASSERT_EQ(refused.status, 401);
        EXPECT_EQ(challenge_of(refused).resource_metadata, public_document_url);
        // And the validator is told the resource the options name, never one made from the request.
        headers = modern_headers("tools/list", "good");
        for (const auto& field : extra) headers.emplace(field.first, field.second);
        EXPECT_EQ(send(harness.port, "POST", std::move(headers), rpc(1, "tools/list", modern_params())).status, 200);
    }
    for (const std::string& asked : harness.table->resources()) EXPECT_EQ(asked, configured_resource);
    EXPECT_FALSE(harness.table->resources().empty());
    // The same on every path the request can name it by: no other spelling of the address serves it.
    for (const char* other : {"/.well-known/oauth-protected-resource", "/.well-known/oauth-protected-resource/",
                              "/.well-known/oauth-protected-resource/mcp/", "/.well-known/oauth-protected-resource/MCP",
                              "/.well-known/OAuth-Protected-Resource/mcp"}) {
        EXPECT_EQ(send(harness.port, "GET", {}, {}, other).status, 404) << other;
    }
}

TEST(ResourceServerScenarioMetadata, TheDocumentIsServedWithoutATokenWhateverTheAuthorizationFieldSaysAndOnlyForGetAndHead) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.metadata_max_age = 17s; });
    const std::vector<std::vector<std::string>> fields{
        {}, {"Bearer garbage"}, {"Bearer"}, {"Basic dXNlcjpwYXNz"}, {"Bearer a", "Bearer b"}, {std::string(5000, 'x')}};
    for (const std::vector<std::string>& values : fields) {
        httplib::Headers headers;
        for (const std::string& value : values) headers.emplace("Authorization", value);
        const Reply document = send(harness.port, "GET", std::move(headers), {}, document_path_of_endpoint);
        ASSERT_EQ(document.status, 200) << values.size();
        EXPECT_EQ(document.body, expected_document);
        EXPECT_EQ(document.header("Cache-Control"), "max-age=17");
        EXPECT_EQ(document.count("WWW-Authenticate"), 0u);
    }
    EXPECT_EQ(harness.table->calls(), 0);
    const Reply head = send(harness.port, "HEAD", {}, {}, document_path_of_endpoint);
    EXPECT_EQ(head.status, 200);
    EXPECT_TRUE(head.body.empty());
    EXPECT_EQ(head.header("Cache-Control"), "max-age=17");
    for (const char* method : {"PUT", "DELETE", "PATCH", "POST"}) {
        const Reply reply = send(harness.port, method, {{"Content-Type", json_media_type}}, "{}", document_path_of_endpoint);
        EXPECT_EQ(reply.status, 405) << method;
        EXPECT_EQ(reply.header("Allow"), "GET, HEAD") << method;
    }
}

TEST(ResourceServerScenarioMetadata, ADocumentServedOnARewrittenPathStillNamesThePublicAddressInChallenges) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->metadata_path = "/internal/prm"; });
    const Reply document = send(harness.port, "GET", {}, {}, "/internal/prm");
    ASSERT_EQ(document.status, 200);
    EXPECT_EQ(document.body, expected_document);
    EXPECT_EQ(send(harness.port, "GET", {}, {}, document_path_of_endpoint).status, 404) << "only where the proxy sends it";
    const Reply refused = list_tools(harness.port, std::nullopt);
    ASSERT_EQ(refused.status, 401);
    EXPECT_EQ(challenge_of(refused).resource_metadata, public_document_url);
}

TEST(ResourceServerScenarioMetadata, AnEndpointAtTheRootOfItsOriginServesTheDocumentAtTheRootForm) {
    for (const char* resource : {"http://localhost", "http://localhost/"}) {
        Harness harness([&](neomcp::HttpServerOptions& options) {
            options.path = "/";
            options.authorization->resource = resource;
        });
        const Reply document = send(harness.port, "GET", {}, {}, "/.well-known/oauth-protected-resource");
        ASSERT_EQ(document.status, 200) << resource;
        EXPECT_EQ(json::parse(document.body).at("resource"), resource) << "verbatim, as configured";
        httplib::Headers headers = modern_headers("tools/list", std::nullopt);
        const Reply refused = send(harness.port, "POST", std::move(headers), rpc(1, "tools/list", modern_params()), "/");
        ASSERT_EQ(refused.status, 401) << resource;
        EXPECT_EQ(challenge_of(refused).resource_metadata, "http://localhost/.well-known/oauth-protected-resource") << resource;
    }
}

// ---- The order of the checks ---------------------------------------------------------------------------------------------

TEST(ResourceServerScenarioOrder, TheChecksBeforeTheGateAnswerWithoutAChallengeAndTheGateComesBeforeTheRest) {
    Harness harness;
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}));
    const std::string body = rpc(1, "tools/list", modern_params());
    const auto request_with = [&](const std::string& method, std::optional<std::string> credentials, httplib::Headers extra,
                                  const std::string& content_type = json_media_type, const std::string& accept = both_media_types) {
        httplib::Headers headers{{"Content-Type", content_type},
                                 {"Accept", accept},
                                 {"MCP-Protocol-Version", modern_revision},
                                 {"Mcp-Method", "tools/list"}};
        if (credentials) headers.emplace("Authorization", "Bearer " + *credentials);
        for (const auto& field : extra) headers.emplace(field.first, field.second);
        return send(harness.port, method, std::move(headers), method == "POST" ? body : std::string());
    };
    const auto unchallenged = [](const Reply& reply, int status, const std::string& what) {
        EXPECT_EQ(reply.status, status) << what;
        EXPECT_EQ(reply.count("WWW-Authenticate"), 0u) << what << ": not about authorization";
    };
    const auto challenged = [](const Reply& reply, const std::string& what) {
        EXPECT_EQ(reply.status, 401) << what;
        EXPECT_EQ(reply.count("WWW-Authenticate"), 1u) << what;
    };
    // The Host and Origin policy comes first, token or none.
    unchallenged(request_with("POST", std::nullopt, {{"Host", "unlisted.example"}}), 403, "bad Host");
    unchallenged(request_with("POST", token, {{"Host", "unlisted.example"}}), 403, "bad Host with a token");
    unchallenged(request_with("POST", std::nullopt, {{"Origin", "https://unlisted.example"}}), 403, "bad Origin");
    unchallenged(request_with("POST", token, {{"Origin", "https://unlisted.example"}}), 403, "bad Origin with a token");
    // So does the choice of method: one the endpoint does not serve is not asked for a token.
    for (const char* method : {"PUT", "PATCH", "OPTIONS"}) {
        const Reply reply = request_with(method, std::nullopt, {});
        unchallenged(reply, 405, method);
        EXPECT_FALSE(reply.header("Allow").empty()) << method;
    }
    // Then the gate: before the media type, the Accept field, and whether a session is named or can be had.
    challenged(request_with("POST", std::nullopt, {}, "text/plain"), "no token, wrong media type");
    unchallenged(request_with("POST", token, {}, "text/plain"), 415, "token, wrong media type");
    challenged(request_with("POST", std::nullopt, {}, json_media_type, "image/png"), "no token, wrong Accept");
    unchallenged(request_with("POST", token, {}, json_media_type, "image/png"), 406, "token, wrong Accept");
    challenged(request_with("POST", std::nullopt, {{"Mcp-Session-Id", "no-such-session"}}), "no token, a session id");
    for (const char* method : {"GET", "DELETE"}) {
        challenged(request_with(method, std::nullopt, {}, json_media_type, "text/event-stream"), std::string(method) + " without a session");
        const Reply with_token = request_with(method, token, {}, json_media_type, "text/event-stream");
        unchallenged(with_token, 405, std::string(method) + " without a session, with a token");
    }
    // The gate comes before the body is read, so before the revision is known: a revision the server does not serve is
    // reported to whoever has a token, and asked for a token before.
    httplib::Headers odd{{"Content-Type", json_media_type}, {"Accept", both_media_types}, {"MCP-Protocol-Version", "1999-01-01"}};
    challenged(send(harness.port, "POST", odd, body), "an unserved revision, no token");
    EXPECT_EQ(harness.table->calls(), 1) << "the validator was asked about the one token that was presented, and once";
}

TEST(ResourceServerScenarioOrder, EveryRevisionIsAskedForATokenOnItsFirstRequestAndServedWithOne) {
    Harness harness;
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}));
    std::set<std::string> challenges;
    // The revisions that open a session with an initialize request.
    for (const char* revision : {"2025-03-26", "2025-06-18", "2025-11-25"}) {
        const json params{{"protocolVersion", revision},
                          {"capabilities", json::object()},
                          {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
        httplib::Headers headers{{"Content-Type", json_media_type}, {"Accept", both_media_types}};
        if (std::string(revision) != first_session_revision) headers.emplace("MCP-Protocol-Version", revision);
        const Reply refused = send(harness.port, "POST", headers, rpc(1, "initialize", params));
        EXPECT_EQ(refused.status, 401) << revision;
        challenges.insert(refused.header("WWW-Authenticate"));
        EXPECT_EQ(harness.items.total(), 0);
        const Reply opened = send(harness.port, "POST", legacy_headers({}, token, revision), rpc(1, "initialize", params));
        EXPECT_EQ(opened.status, 200) << revision << " " << opened.body;
        EXPECT_FALSE(opened.header("Mcp-Session-Id").empty()) << revision;
    }
    // The revision without sessions starts with a request that discovers the server.
    const Reply refused = modern_request(harness.port, std::nullopt, "server/discover");
    EXPECT_EQ(refused.status, 401);
    challenges.insert(refused.header("WWW-Authenticate"));
    EXPECT_EQ(challenges.size(), 1u) << "the same challenge whatever the revision";
    EXPECT_EQ(modern_request(harness.port, token, "server/discover").status, 200);
}

// ---- The owner of a session ----------------------------------------------------------------------------------------

TEST(ResourceServerScenarioSession, APrincipalIsTheIssuerTheSubjectAndTheClientAndNotTheToken) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->authorization_servers = {first_issuer, second_issuer}; });
    auto& clock = harness.clock;
    const auto principal = [&](const std::string& name, std::string subject, std::string client, std::string issuer,
                               std::vector<std::string> scopes = {"mcp:use"}) {
        return harness.token(name, granted(clock, std::move(subject), std::move(scopes), 3600s, std::move(issuer),
                                           {configured_resource}, std::move(client)));
    };
    // The subject and the client are written in a way that no shift of the boundary between them gives the same
    // principal.
    const std::string owner = principal("owner", "ab", "c", first_issuer);
    const std::string session = open_session(harness.port, owner);
    for (const auto& other_principal : {principal("other-client", "ab", "d", first_issuer),
                                 principal("other-issuer", "ab", "c", second_issuer),
                                 principal("shifted", "a", "bc", first_issuer),
                                 principal("other-subject", "abc", "c", first_issuer),
                                 principal("no-subject", "", "c", first_issuer),
                                 principal("no-client", "ab", "", first_issuer)}) {
        EXPECT_EQ(ping(harness.port, session, other_principal).status, 404) << other_principal;
        EXPECT_EQ(send(harness.port, "GET", legacy_headers(session, other_principal)).status, 404) << other_principal;
        EXPECT_EQ(send(harness.port, "DELETE", legacy_headers(session, other_principal)).status, 404) << other_principal;
    }
    // The session is still there for its owner, whose new token has other scopes and another expiry.
    const std::string renewed = principal("renewed", "ab", "c", first_issuer, {"mcp:use", "files:read"});
    EXPECT_EQ(ping(harness.port, session, owner).status, 200);
    EXPECT_EQ(ping(harness.port, session, renewed).status, 200);
}

TEST(ResourceServerScenarioSession, AnotherPrincipalLearnsNothingThatNoSessionAtAllWouldNotTellAndNoTokenLearnsNothing) {
    for (const char* revision : {"2025-03-26", "2025-06-18", "2025-11-25"}) {
        Harness harness;
        auto& clock = harness.clock;
        const std::string alice = harness.token("alice", granted(clock, "alice", {"mcp:use"}));
        const std::string bob = harness.token("bob", granted(clock, "bob", {"mcp:use"}));
        const std::string session = open_session(harness.port, alice, revision);
        const std::string nobody(session.size(), '0');
        const auto as = [&](const std::string& token, const std::string& id) {
            std::vector<Reply> replies;
            replies.push_back(ping(harness.port, id, token, revision));
            replies.push_back(send(harness.port, "GET", legacy_headers(id, token, revision)));
            replies.push_back(send(harness.port, "DELETE", legacy_headers(id, token, revision)));
            return replies;
        };
        const auto about_alices = as(bob, session);
        const auto about_nothing = as(bob, nobody);
        for (std::size_t at = 0; at < about_alices.size(); ++at) {
            EXPECT_EQ(about_alices[at].status, 404) << revision << " " << at;
            EXPECT_EQ(shape_of(about_alices[at]), shape_of(about_nothing[at])) << revision << " " << at;
        }
        // Without a token, or with one that is no good, a session id is not looked at: the answer is the gate's.
        const auto no_token = [&](const std::string& method) {
            httplib::Headers headers = legacy_headers(session, "unused", revision);
            headers.erase("Authorization");
            return send(harness.port, method, std::move(headers), method == "POST" ? rpc(2, "ping") : std::string());
        };
        for (const char* method : {"POST", "GET", "DELETE"}) {
            const Reply reply = no_token(method);
            EXPECT_EQ(reply.status, 401) << revision << " " << method;
            EXPECT_EQ(reply.count("WWW-Authenticate"), 1u);
            EXPECT_EQ(ping(harness.port, session, "never-issued", revision).status, 401);
        }
        httplib::Headers unreadable = legacy_headers(session, "x", revision);
        unreadable.erase("Authorization");
        unreadable.emplace("Authorization", "Bearer");
        EXPECT_EQ(send(harness.port, "POST", std::move(unreadable), rpc(2, "ping")).status, 400);
        // None of it touched the session: it answers its owner, and a DELETE that was refused did not end it.
        EXPECT_EQ(ping(harness.port, session, alice, revision).status, 200) << revision;
        EXPECT_EQ(ping(harness.port, session, alice, revision).status, 200) << revision;
    }
}

// ---- A stream ends with the token that opened it -------------------------------------------------------------------

TEST(ResourceServerScenarioStreams, AStreamOutlivesItsTokenByTheLeewayAndNotLongerThanThat) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.leeway = 2s; });
    const std::string opener = harness.token("long", granted(harness.clock, "alice", {"mcp:use"}));
    const std::string session = open_session(harness.port, opener);
    const std::string brief = harness.token("brief", granted(harness.clock, "alice", {"mcp:use"}, 1s));
    httplib::Headers headers = legacy_headers(session, brief);
    headers.erase("Accept");
    headers.emplace("Accept", "text/event-stream");
    const auto started = std::chrono::steady_clock::now();
    const Reply stream = send(harness.port, "GET", std::move(headers));
    const auto lived = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(stream.ok) << "ended by the server";
    EXPECT_EQ(stream.status, 200);
    EXPECT_GE(lived, 2500ms) << "the token expires after 1 s, and the leeway is 2 s";
    EXPECT_LT(lived, 20s);

    const auto listening_since = std::chrono::steady_clock::now();
    const Reply subscription =
        send(harness.port, "POST", modern_headers("subscriptions/listen", brief),
             rpc(5, "subscriptions/listen", modern_params({{"notifications", {{"toolsListChanged", true}}}})));
    const auto listened = std::chrono::steady_clock::now() - listening_since;
    EXPECT_TRUE(subscription.ok);
    EXPECT_EQ(subscription.status, 200);
    EXPECT_GE(listened, 2500ms);
    EXPECT_LT(listened, 20s);
}

TEST(ResourceServerScenarioStreams, AfterItsStreamEndedAnExpiredTokenIsRefusedAndAFreshOneOpensAStreamThatItsOwnTokenKeeps) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.leeway = 0s; });
    auto& clock = harness.clock;
    const std::string opener = harness.token("long", granted(clock, "alice", {"mcp:use"}));
    const std::string session = open_session(harness.port, opener);
    const std::string brief = harness.token("brief", granted(clock, "alice", {"mcp:use"}, 1s));
    const auto stream_headers = [&](const std::string& token) {
        httplib::Headers headers = legacy_headers(session, token);
        headers.erase("Accept");
        headers.emplace("Accept", "text/event-stream");
        return headers;
    };
    const Reply first = send(harness.port, "GET", stream_headers(brief));
    EXPECT_TRUE(first.ok);
    EXPECT_EQ(first.status, 200);

    // The client's clock has moved past the token's end: the same token is no longer good.
    clock.advance(2s);
    const Reply stale = send(harness.port, "GET", stream_headers(brief));
    EXPECT_EQ(stale.status, 401);
    EXPECT_EQ(challenge_of(stale).error, "invalid_token");
    EXPECT_EQ(ping(harness.port, session, opener).status, 200) << "the session goes on; only the stream ended";

    // A fresh token reopens the stream, and it is not cut when the old token's time passes.
    const std::string fresh = harness.token("fresh", granted(clock, "alice", {"mcp:use"}, 3600s));
    Call reopened(harness.port, "GET", stream_headers(fresh));
    ASSERT_TRUE(reopened.wait_for_headers());
    EXPECT_EQ(reopened.now().status, 200) << "not 409: the ended stream was let go";
    std::this_thread::sleep_for(1500ms);
    EXPECT_FALSE(reopened.finished()) << "the new stream lives as long as its own token";
    reopened.disconnect();
}

}  // namespace
