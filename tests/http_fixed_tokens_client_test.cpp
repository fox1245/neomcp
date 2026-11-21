// The library's own Client against the server protected by fixed tokens, with StaticBearer, the authorizer that sends
// one token the application has: on every revision the server serves, the handler sees the token's subject, an item
// whose scope the token grants runs and one whose scope it lacks is refused; a token the table does not hold, or no
// longer holds, is refused.
#include <memory>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "support/fixed_tokens_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

neomcp::ClientOptions options_for(const std::string& revision) {
    neomcp::ClientOptions options;
    if (revision != "all") options.profiles.push_back(neomcp::profile::builtin(revision));
    options.client_info.name = "fixed-tokens-client";
    options.client_info.version = "1.0.0";
    return options;
}

neomcp::HttpEndpoint endpoint_of(const FixedHarness& harness, const std::string& token) {
    neomcp::HttpEndpoint endpoint;
    endpoint.url = "http://127.0.0.1:" + std::to_string(harness.port) + endpoint_path;
    endpoint.proxy.mode = neomcp::ProxyMode::none;
    endpoint.authorizer = std::make_shared<neomcp::StaticBearer>(token);
    return endpoint;
}

std::string text_of_call(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

class FixedTokensClient : public ::testing::TestWithParam<std::string> {};

TEST_P(FixedTokensClient, AStaticBearerOfTheTableReachesTheHandlerWithItsGrants) {
    FixedHarness harness({fixed("alice-token", "alice", {"mcp:use", "files:write"})});
    auto client = neomcp::Client::open(options_for(GetParam()), endpoint_of(harness, "alice-token"));
    ASSERT_TRUE(client) << client.error().message;
    const auto who = client->call_tool("whoami");
    ASSERT_TRUE(who) << who.error().message;
    const json seen = json::parse(text_of_call(*who));
    EXPECT_EQ(seen.at("subject"), "alice");
    EXPECT_EQ(seen.at("issuer"), "");
    EXPECT_EQ(seen.at("scopes"), json::array({"mcp:use", "files:write"}));
    const auto guarded = client->call_tool("guarded");
    ASSERT_TRUE(guarded) << guarded.error().message;
    EXPECT_EQ(text_of_call(*guarded), "written");
    // audit:read is not granted: the 403 reaches the authorizer, which has no other token.
    const auto audited = client->call_tool("audited");
    ASSERT_FALSE(audited);
    EXPECT_EQ(audited.error().kind, neomcp::ClientErrorKind::authorization_required);
    EXPECT_EQ(harness.items.audited.load(), 0);

    // Removed from the table: refused from the next request on; given back: accepted again.
    harness.replace({fixed("bob-token", "bob", {"mcp:use"})});
    const auto removed = client->call_tool("whoami");
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().kind, neomcp::ClientErrorKind::authorization_required);
    harness.replace({fixed("alice-token", "alice", {"mcp:use"})});
    const auto back = client->call_tool("whoami");
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(json::parse(text_of_call(*back)).at("scopes"), json::array({"mcp:use"}));
    EXPECT_TRUE(client->close());
}

TEST_P(FixedTokensClient, AStaticBearerTheTableDoesNotHoldIsRefusedBeforeAnythingRuns) {
    FixedHarness harness({fixed("alice-token", "alice", {"mcp:use"})});
    auto client = neomcp::Client::open(options_for(GetParam()), endpoint_of(harness, "not-in-the-table"));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, neomcp::ClientErrorKind::authorization_required);
    EXPECT_EQ(client.error().message.find("not-in-the-table"), std::string::npos);
    EXPECT_EQ(harness.items.total(), 0);
}

INSTANTIATE_TEST_SUITE_P(FixedTokens, FixedTokensClient,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28", "all"),
                         [](const auto& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return name;
                         });

}  // namespace
