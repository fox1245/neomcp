// Scenarios of the Streamable HTTP endpoint protected by fixed tokens, over loopback sockets: a token of the table
// reaches the handler with its subject and scopes on every revision; the plain challenges (no resource_metadata) for
// no token, an unreadable field, and a token that is unknown, expired, removed or replaced; no metadata document;
// required_scopes and the scopes items declare; the owner of a session; the end of a stream; what start() refuses;
// and the endpoint on its own: both kinds of tokens refused, fixed tokens in its options, a request while it stops.
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/endpoint.h"
#include "support/fixed_tokens_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr const char* plain_challenge = R"(Bearer scope="mcp:use")";
constexpr const char* invalid_token_challenge = R"(Bearer error="invalid_token", scope="mcp:use")";

std::string shape_of(const Reply& reply) {
    return std::to_string(reply.status) + "|" + reply.header("Content-Type") + "|" + reply.header("WWW-Authenticate") +
           "|" + reply.header("Cache-Control") + "|" + reply.body;
}

// A refusal of the gate: the status, the one challenge, no-store, and no resource_metadata anywhere.
void expect_refusal(const Reply& reply, int status, const std::string& challenge) {
    ASSERT_TRUE(reply.ok);
    EXPECT_EQ(reply.status, status) << reply.body;
    EXPECT_EQ(reply.count("WWW-Authenticate"), 1u);
    EXPECT_EQ(reply.header("WWW-Authenticate"), challenge);
    EXPECT_EQ(reply.header("Cache-Control"), "no-store");
    EXPECT_EQ(reply.everything().find("resource_metadata"), std::string::npos);
    EXPECT_EQ(reply.everything().find(".well-known"), std::string::npos);
}

void expect_invalid_token(const Reply& reply) {
    expect_refusal(reply, 401, invalid_token_challenge);
    EXPECT_EQ(error_of(reply), "invalid_token");
}

// whoami over the revision: a request of 2026-07-28, or one inside a session the token opened.
Reply whoami(std::uint16_t port, const std::string& token, const std::string& revision) {
    if (revision == modern_revision) return call_tool(port, token, "whoami");
    const std::string session = open_session(port, token, revision);
    return session_request(port, session, token, "tools/call", json{{"name", "whoami"}, {"arguments", json::object()}},
                           revision);
}

class FixedTokensRevision : public ::testing::TestWithParam<std::string> {};

TEST_P(FixedTokensRevision, ATableTokenReachesTheHandlerWithItsSubjectAndScopesAndNoIssuer) {
    FixedHarness harness({fixed("alice-token", "alice", {"mcp:use", "files:read"}),
                          fixed("bob-token", "bob", {"mcp:use"})});
    const Reply reply = whoami(harness.port, "alice-token", GetParam());
    ASSERT_EQ(reply.status, 200) << reply.body;
    const json who = who_of(reply);
    EXPECT_EQ(who.at("issuer"), "");
    EXPECT_EQ(who.at("subject"), "alice");
    EXPECT_TRUE(who.at("client").is_null());
    EXPECT_EQ(who.at("scopes"), json::array({"mcp:use", "files:read"}));
    EXPECT_EQ(who.at("audiences"), json::array());
    EXPECT_EQ(who_of(whoami(harness.port, "bob-token", GetParam())).at("subject"), "bob");
    EXPECT_EQ(harness.items.whoami.load(), 2);
    // A token the table does not hold is refused on this revision too, before anything runs.
    expect_invalid_token(call_tool(harness.port, "carol-token", "whoami"));
    EXPECT_EQ(harness.items.whoami.load(), 2);
}

INSTANTIATE_TEST_SUITE_P(FixedTokens, FixedTokensRevision,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"),
                         [](const auto& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return name;
                         });

TEST(FixedTokensServer, WithoutABearerTokenTheChallengeIsPlainBearerWithTheRequiredScopes) {
    FixedHarness harness({fixed("t", "alice", {"mcp:use"})});
    const Reply none = list_tools(harness.port, std::nullopt);
    expect_refusal(none, 401, plain_challenge);
    EXPECT_TRUE(none.body.empty());
    httplib::Headers basic = modern_headers("tools/list", std::nullopt);
    basic.emplace("Authorization", "Basic dXNlcjpwYXNz");
    expect_refusal(send(harness.port, "POST", std::move(basic), rpc(1, "tools/list", modern_params())), 401,
                   plain_challenge);
    EXPECT_EQ(harness.items.total(), 0);

    // Without required_scopes the challenge is the scheme alone (RFC 6750, section 3).
    FixedHarness bare({fixed("t", "alice", {})}, [](neomcp::HttpServerOptions& options) {
        options.fixed_tokens->required_scopes.clear();
    });
    expect_refusal(list_tools(bare.port, std::nullopt), 401, "Bearer");
    expect_refusal(list_tools(bare.port, "unknown"), 401, R"(Bearer error="invalid_token")");
    EXPECT_EQ(list_tools(bare.port, "t").status, 200);
}

TEST(FixedTokensServer, AFieldThatCannotBeReadGets400WithAPlainChallenge) {
    FixedHarness harness({fixed("t", "alice", {"mcp:use"})});
    httplib::Headers empty = modern_headers("tools/list", std::nullopt);
    empty.emplace("Authorization", "Bearer");
    const Reply unreadable = send(harness.port, "POST", std::move(empty), rpc(1, "tools/list", modern_params()));
    expect_refusal(unreadable, 400, R"(Bearer error="invalid_request")");
    EXPECT_EQ(error_of(unreadable), "invalid_request");
    httplib::Headers twice = modern_headers("tools/list", "t");
    twice.emplace("Authorization", "Bearer t");
    expect_refusal(send(harness.port, "POST", std::move(twice), rpc(1, "tools/list", modern_params())), 400,
                   R"(Bearer error="invalid_request")");
    EXPECT_EQ(harness.items.total(), 0);
}

TEST(FixedTokensServer, UnknownExpiredRemovedAndReplacedTokensGetThePlain401FromTheNextRequestOn) {
    FixedHarness harness({fixed("kept", "alice", {"mcp:use"}), fixed("gone", "bob", {"mcp:use"}),
                          fixed("old", "carol", {"mcp:use"})});
    auto& clock = harness.clock;
    // Unknown, and texts near a known token.
    for (const char* unknown : {"never-given", "kep", "keptt", "Kept"}) {
        expect_invalid_token(call_tool(harness.port, unknown, "whoami"));
    }
    EXPECT_EQ(harness.items.total(), 0);

    // Expiry: accepted one second before it, refused at it (there is no leeway), and after it.
    harness.replace({fixed("kept", "alice", {"mcp:use"}), fixed("gone", "bob", {"mcp:use"}),
                     fixed("old", "carol", {"mcp:use"}), fixed("brief", "dave", {"mcp:use"}, clock.now() + 10s)});
    EXPECT_EQ(call_tool(harness.port, "brief", "whoami").status, 200);
    clock.advance(9s);
    EXPECT_EQ(call_tool(harness.port, "brief", "whoami").status, 200);
    clock.advance(1s);
    expect_invalid_token(call_tool(harness.port, "brief", "whoami"));
    clock.advance(3600s);
    expect_invalid_token(call_tool(harness.port, "brief", "whoami"));

    // Each was accepted right before the replacement.
    EXPECT_EQ(call_tool(harness.port, "gone", "whoami").status, 200);
    EXPECT_EQ(who_of(call_tool(harness.port, "kept", "whoami")).at("subject"), "alice");
    EXPECT_EQ(call_tool(harness.port, "old", "whoami").status, 200);
    // Removed (gone), given other grants (kept), and replaced by another token for the same subject (old, new).
    harness.replace({fixed("kept", "erin", {"mcp:use", "files:write"}), fixed("new", "carol", {"mcp:use"})});
    expect_invalid_token(call_tool(harness.port, "gone", "whoami"));
    expect_invalid_token(call_tool(harness.port, "old", "whoami"));
    const json kept = who_of(call_tool(harness.port, "kept", "whoami"));
    EXPECT_EQ(kept.at("subject"), "erin");
    EXPECT_EQ(kept.at("scopes"), json::array({"mcp:use", "files:write"}));
    EXPECT_EQ(who_of(call_tool(harness.port, "new", "whoami")).at("subject"), "carol");
    // An empty table refuses every token.
    harness.replace({});
    expect_invalid_token(call_tool(harness.port, "kept", "whoami"));
    expect_invalid_token(call_tool(harness.port, "new", "whoami"));
}

TEST(FixedTokensServer, NoMetadataDocumentIsServedItsPathIsAnsweredAsAnyUnknownPath) {
    FixedHarness harness({fixed("t", "alice", {"mcp:use"})});
    const Reply unknown = send(harness.port, "GET", {}, {}, "/no/such/path");
    ASSERT_TRUE(unknown.ok);
    EXPECT_EQ(unknown.status, 404);
    for (const char* path : {document_path_of_endpoint, "/.well-known/oauth-protected-resource"}) {
        for (const char* method : {"GET", "HEAD"}) {
            const Reply bare = send(harness.port, method, {}, {}, path);
            const Reply other = send(harness.port, method, {}, {}, "/no/such/path");
            EXPECT_EQ(shape_of(bare), shape_of(other)) << method << " " << path;
            const Reply with_token = send(harness.port, method, {{"Authorization", "Bearer t"}}, {}, path);
            EXPECT_EQ(shape_of(with_token), shape_of(other)) << method << " " << path;
        }
    }
    // The server has no authorization servers to replace.
    const auto updated = harness.http->update_authorization_servers({first_issuer});
    EXPECT_FALSE(updated);
}

TEST(FixedTokensServer, RequiredScopesAndTheScopesItemsDeclareGive403WithAPlainChallenge) {
    FixedHarness harness({fixed("no-use", "alice", {"files:write"}), fixed("use", "bob", {"mcp:use"}),
                          fixed("writer", "carol", {"mcp:use", "files:write"}), fixed("admin", "dave", {"admin"})});
    const Reply lacking = call_tool(harness.port, "no-use", "whoami");
    // What the request lacks, then the granted scopes the server names (files:write, which an item declares).
    expect_refusal(lacking, 403, R"(Bearer error="insufficient_scope", scope="mcp:use files:write")");
    EXPECT_EQ(error_of(lacking), "insufficient_scope");
    const Reply guarded = call_tool(harness.port, "use", "guarded");
    expect_refusal(guarded, 403, R"(Bearer error="insufficient_scope", scope="files:write mcp:use")");
    const Reply secret = modern_request(harness.port, "use", "resources/read", json{{"uri", "test://secret"}},
                                        "test://secret");
    expect_refusal(secret, 403, R"(Bearer error="insufficient_scope", scope="secret:read mcp:use")");
    EXPECT_EQ(harness.items.total(), 0);
    EXPECT_EQ(text_of(call_tool(harness.port, "writer", "guarded")), "written");
    // A scope the server declares to imply others (admin implies mcp:use and files:write) is followed.
    EXPECT_EQ(text_of(call_tool(harness.port, "admin", "guarded")), "written");
    EXPECT_EQ(harness.items.guarded.load(), 2);
}

TEST(FixedTokensServer, ASessionBelongsToTheSubjectOfTheTokenThatOpenedIt) {
    FixedHarness harness({fixed("alice-1", "alice", {"mcp:use"}), fixed("alice-2", "alice", {"mcp:use", "files:read"}),
                          fixed("bob", "bob", {"mcp:use"})});
    for (const char* revision : {"2025-03-26", "2025-06-18", "2025-11-25"}) {
        const std::string session = open_session(harness.port, "alice-1", revision);
        EXPECT_EQ(ping(harness.port, session, "bob", revision).status, 404) << revision;
        EXPECT_EQ(ping(harness.port, session, "alice-2", revision).status, 200) << revision;
        EXPECT_EQ(ping(harness.port, session, "alice-1", revision).status, 200) << revision;
    }
}

TEST(FixedTokensServer, AStreamEndsWhenItsTokenExpiresWithoutLeeway) {
    FixedHarness harness({fixed("long", "alice", {"mcp:use"})});
    harness.replace({fixed("long", "alice", {"mcp:use"}), fixed("brief", "alice", {"mcp:use"}, harness.clock.now() + 1s)});
    const std::string session = open_session(harness.port, "long");
    httplib::Headers headers = legacy_headers(session, "brief");
    headers.erase("Accept");
    headers.emplace("Accept", "text/event-stream");
    const auto started = std::chrono::steady_clock::now();
    const Reply stream = send(harness.port, "GET", std::move(headers));
    const auto lived = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(stream.ok) << "ended by the server";
    EXPECT_EQ(stream.status, 200);
    EXPECT_LT(lived, 20s) << "an OAuth token's default leeway is 60 s; fixed tokens have none";
}

// ---- What start() refuses ---------------------------------------------------------------------------------------------

neomcp::http::FixedTokenOptions some_tokens() {
    neomcp::http::FixedTokenOptions options;
    options.tokens = fixed_table({fixed("t", "alice", {"mcp:use"})});
    options.required_scopes = {"mcp:use"};
    return options;
}

std::string start_error(neomcp::Server& server, neomcp::HttpServerOptions options) {
    neomcp::HttpServer http(server, std::move(options));
    auto started = http.start();
    if (started) return {};
    return started.error;
}

TEST(FixedTokensServer, StartRefusesBothKindsOfTokensTwoTablesAndRequiredScopesThatCannotBeServed) {
    neomcp::Server server(server_options());
    const auto table = std::make_shared<Table>();
    FakeClock clock;
    {
        neomcp::HttpServerOptions options = quick_options();
        options.fixed_tokens = some_tokens();
        options.authorization = resource_options(table, clock);
        EXPECT_NE(start_error(server, std::move(options)).find("both set"), std::string::npos);
    }
    {
        neomcp::HttpServerOptions options = quick_options();
        options.fixed_tokens = some_tokens();
        options.endpoint.authorization = resource_options(table, clock);
        EXPECT_NE(start_error(server, std::move(options)).find("both set"), std::string::npos);
    }
    {
        neomcp::HttpServerOptions options = quick_options();
        options.fixed_tokens = some_tokens();
        options.endpoint.fixed_tokens = some_tokens();
        EXPECT_NE(start_error(server, std::move(options)).find("set twice"), std::string::npos);
    }
    for (const std::vector<std::string>& required :
         {std::vector<std::string>{"has space"}, std::vector<std::string>{"offline_access"},
          std::vector<std::string>{""}}) {
        neomcp::HttpServerOptions options = quick_options();
        options.fixed_tokens = some_tokens();
        options.fixed_tokens->required_scopes = required;
        EXPECT_NE(start_error(server, std::move(options)).find("required_scopes"), std::string::npos) << required.front();
    }
    // Before start() and without fixed tokens, nothing is replaced.
    neomcp::HttpServer idle(server, quick_options());
    EXPECT_FALSE(idle.replace_fixed_tokens(neomcp::http::FixedTokenTable()));
    Harness oauth;
    EXPECT_FALSE(oauth.http->replace_fixed_tokens(neomcp::http::FixedTokenTable()));
}

TEST(FixedTokensServer, RequiredScopesWithWhatOneItemDeclaresFitOneChallengeAtTheEdgeAndNotOnePast) {
    const auto required_of = [](std::size_t count) {
        std::vector<std::string> scopes;
        for (std::size_t at = 0; at < count; ++at) scopes.push_back("s" + std::to_string(at));
        return scopes;
    };
    const auto start_with = [&](std::size_t required, bool with_item) {
        neomcp::Server server(server_options());
        if (with_item) server.tool("guarded", "Needs files:write", [] { return std::string("w"); }).scopes({"files:write"});
        neomcp::HttpServerOptions options = quick_options();
        options.fixed_tokens = some_tokens();
        options.fixed_tokens->required_scopes = required_of(required);
        return start_error(server, std::move(options));
    };
    const std::size_t most = neomcp::http::ResourceServerDefaults::max_challenge_scopes;
    EXPECT_EQ(start_with(most, false), "");
    EXPECT_NE(start_with(most + 1, false), "");
    EXPECT_EQ(start_with(most - 1, true), "");
    EXPECT_NE(start_with(most, true).find("declares scopes"), std::string::npos);
    // Items that declare scopes start with fixed tokens, and not without any tokens.
    neomcp::Server server(server_options());
    server.tool("guarded", "Needs files:write", [] { return std::string("w"); }).scopes({"files:write"});
    EXPECT_NE(start_error(server, quick_options()).find("fixed_tokens"), std::string::npos);
}

// ---- The endpoint on its own ----------------------------------------------------------------------------------------

neomcp::http::HostPolicy any_policy() {
    auto policy = neomcp::http::HostPolicy::create({});
    if (!policy) throw std::runtime_error(policy.error);
    return std::move(*policy.value);
}

TEST(FixedTokensEndpoint, CreateRefusesAuthorizationAndFixedTokensTogether) {
    neomcp::Server server(server_options());
    const auto table = std::make_shared<Table>();
    FakeClock clock;
    neomcp::http::EndpointOptions options;
    options.fixed_tokens = some_tokens();
    options.authorization = resource_options(table, clock);
    auto made = neomcp::http::Endpoint::create(server, any_policy(), std::move(options));
    ASSERT_FALSE(made);
    EXPECT_NE(made.error.find("both set"), std::string::npos);
}

TEST(FixedTokensEndpoint, FixedTokensInTheEndpointOptionsAloneServeAsInTheServerOptions) {
    neomcp::Server server(server_options());
    Items items;
    register_items(server, items);
    neomcp::HttpServerOptions options = quick_options();
    options.endpoint.fixed_tokens = some_tokens();
    neomcp::HttpServer http(server, std::move(options));
    auto started = http.start();
    ASSERT_TRUE(started) << started.error;
    const Reply reply = call_tool(*started.value, "t", "whoami");
    ASSERT_EQ(reply.status, 200) << reply.body;
    EXPECT_EQ(who_of(reply).at("subject"), "alice");
    expect_refusal(list_tools(*started.value, std::nullopt), 401, plain_challenge);
    expect_invalid_token(list_tools(*started.value, "other"));
    EXPECT_EQ(send(*started.value, "GET", {}, {}, document_path_of_endpoint).status, 404);
    EXPECT_TRUE(http.replace_fixed_tokens(fixed_table({fixed("u", "bob", {"mcp:use"})})));
    expect_invalid_token(list_tools(*started.value, "t"));
    EXPECT_EQ(list_tools(*started.value, "u").status, 200);
    http.stop();
}

TEST(FixedTokensEndpoint, ARequestWithATableTokenWhileTheEndpointStopsGets503) {
    neomcp::Server server(server_options());
    neomcp::http::EndpointOptions options;
    options.fixed_tokens = some_tokens();
    auto made = neomcp::http::Endpoint::create(server, any_policy(), std::move(options));
    ASSERT_TRUE(made) << made.error;
    neomcp::http::Endpoint& endpoint = **made.value;
    endpoint.stop();
    neomcp::http::Request request;
    request.method = "POST";
    request.path = endpoint_path;
    request.headers = {{"Host", "localhost"},
                       {"Content-Type", json_media_type},
                       {"Accept", both_media_types},
                       {"MCP-Protocol-Version", modern_revision},
                       {"Mcp-Method", "tools/list"},
                       {"Authorization", "Bearer t"}};
    request.body = rpc(1, "tools/list", modern_params());
    std::optional<neomcp::http::Response> answer;
    endpoint.handle(std::move(request), neomcp::http::Responder([&answer](neomcp::http::Response&& response) {
                        answer = std::move(response);
                        return true;
                    }));
    ASSERT_TRUE(answer) << "answered at once";
    EXPECT_EQ(answer->status, 503);
    const json body = json::parse(answer->body);
    EXPECT_EQ(body.at("jsonrpc"), "2.0");
    EXPECT_TRUE(body.contains("error"));
    EXPECT_FALSE(answer->headers.get("WWW-Authenticate"));
}

}  // namespace
