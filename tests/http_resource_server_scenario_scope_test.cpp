// Scenarios of the scopes an endpoint that is an OAuth protected resource asks for, over loopback sockets: every kind
// of item and request that needs a scope, on every revision that has it, the list a 403 asks for, a batch that mixes
// allowed and refused requests, scopes that imply nothing unless the server declared it, lists that show everything,
// unknown and switched-off items, the change feed of the subscriptions, and a retried call under another principal.
//
// The tests that complement the other files of this feature are named ResourceServerScenario*.
#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/resource_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;
using Scopes = std::vector<std::string>;

// One request that needs scopes: how to send it in a session of a revision that has them, and on the 2026-07-28
// revision that has none; what it needs; how often the item it asks for ran.
struct Target {
    std::string label;
    std::string method;
    json params;
    std::optional<std::string> name;  // the Mcp-Name field of the 2026-07-28 revision
    Scopes needs;
    std::function<int(const Items&)> ran;
};

std::vector<Target> targets() {
    const json no_arguments = json::object();
    return {
        {"tools/call guarded", "tools/call", json{{"name", "guarded"}, {"arguments", no_arguments}}, "guarded",
         {"files:write"}, [](const Items& items) { return items.guarded.load(); }},
        {"tools/call audited", "tools/call", json{{"name", "audited"}, {"arguments", no_arguments}}, "audited",
         {"audit:read"}, [](const Items& items) { return items.audited.load(); }},
        {"resources/read of a resource", "resources/read", json{{"uri", "test://secret"}}, "test://secret",
         {"secret:read"}, [](const Items& items) { return items.secret.load(); }},
        {"resources/read of a template", "resources/read", json{{"uri", "doc://x"}}, "doc://x", {"docs:read"},
         [](const Items& items) { return items.document.load(); }},
        {"prompts/get", "prompts/get", json{{"name", "plan"}, {"arguments", json{{"topic", "t"}}}}, "plan",
         {"plans:read"}, [](const Items& items) { return items.plan.load(); }},
        {"completion/complete for a prompt", "completion/complete",
         json{{"ref", json{{"type", "ref/prompt"}, {"name", "plan"}}}, {"argument", json{{"name", "topic"}, {"value", ""}}}},
         std::nullopt, {"plans:read"}, [](const Items& items) { return items.completed.load(); }},
        {"completion/complete for a template", "completion/complete",
         json{{"ref", json{{"type", "ref/resource"}, {"uri", "doc://{name}"}}},
              {"argument", json{{"name", "name"}, {"value", ""}}}},
         std::nullopt, {"docs:read"}, [](const Items& items) { return items.completed.load(); }},
    };
}

// What the 403 asks for when the token holds `granted` and the request needs `needs`: what is missing, the scopes every
// request needs, and the granted ones the server names itself.
Scopes asked_for(const Scopes& needs, const Scopes& granted_declared) {
    Scopes scopes = needs;
    scopes.push_back("mcp:use");
    for (const std::string& scope : granted_declared) {
        if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) scopes.push_back(scope);
    }
    return scopes;
}

void expect_scope_refusal(const Reply& reply, const Scopes& expected, const std::string& label) {
    EXPECT_EQ(reply.status, 403) << label << " " << reply.body;
    if (reply.status != 403) return;
    EXPECT_EQ(challenge_of(reply).error, "insufficient_scope") << label;
    EXPECT_EQ(scopes_of(reply), expected) << label;
    EXPECT_EQ(error_of(reply), "insufficient_scope") << label;
    EXPECT_EQ(reply.header("Cache-Control"), "no-store") << label;
    EXPECT_EQ(challenge_of(reply).resource_metadata, "http://localhost/.well-known/oauth-protected-resource/mcp") << label;
}

// ---- Every kind of request that needs a scope ------------------------------------------------------------------------

TEST(ResourceServerScenarioScope, EveryKindOfRequestThatNeedsAScopeIsRefusedWith403BeforeItsItemRunsOn2026) {
    Harness harness;
    auto& clock = harness.clock;
    const std::string bare = harness.token("bare", granted(clock, "a", {"mcp:use"}));
    const std::string reader = harness.token("reader", granted(clock, "a", {"mcp:use", "files:read", "openid"}));
    std::size_t index = 0;
    for (const Target& target : targets()) {
        const auto send_as = [&](const std::string& token) {
            return modern_request(harness.port, token, target.method, target.params, target.name);
        };
        const int before = target.ran(harness.items);
        expect_scope_refusal(send_as(bare), asked_for(target.needs, {}), target.label);
        expect_scope_refusal(send_as(reader), asked_for(target.needs, {"files:read"}), target.label + " (a reader)");
        EXPECT_EQ(target.ran(harness.items), before) << target.label << ": nothing of the item ran";
        // With what it needs, the request is served and the item runs.
        Scopes full{"mcp:use"};
        full.insert(full.end(), target.needs.begin(), target.needs.end());
        const std::string name = "full-" + std::to_string(index++);
        harness.token(name, granted(clock, "a", full));
        const Reply served = send_as(name);
        EXPECT_EQ(served.status, 200) << target.label << " " << served.body;
        EXPECT_TRUE(json::parse(served.body).contains("result")) << target.label << " " << served.body;
        EXPECT_EQ(target.ran(harness.items), before + 1) << target.label;
    }
}

TEST(ResourceServerScenarioScope, EveryKindOfRequestThatNeedsAScopeIsRefusedWith403InASessionOfEachLegacyRevision) {
    for (const char* revision : {"2025-03-26", "2025-06-18", "2025-11-25"}) {
        Harness harness;
        auto& clock = harness.clock;
        const std::string bare = harness.token("bare", granted(clock, "a", {"mcp:use"}));
        const std::string wide = harness.token(
            "wide", granted(clock, "a", {"mcp:use", "files:write", "audit:read", "secret:read", "docs:read", "plans:read"}));
        const std::string session = open_session(harness.port, bare, revision);
        std::vector<Target> all = targets();
        all.push_back({"resources/subscribe", "resources/subscribe", json{{"uri", "test://secret"}}, std::nullopt,
                       {"secret:read"}, [](const Items&) { return 0; }});
        all.push_back({"resources/subscribe a template", "resources/subscribe", json{{"uri", "doc://x"}}, std::nullopt,
                       {"docs:read"}, [](const Items&) { return 0; }});
        all.push_back({"resources/unsubscribe", "resources/unsubscribe", json{{"uri", "test://secret"}}, std::nullopt,
                       {"secret:read"}, [](const Items&) { return 0; }});
        for (const Target& target : all) {
            const std::string label = std::string(revision) + " " + target.label;
            const int before = target.ran(harness.items);
            expect_scope_refusal(session_request(harness.port, session, bare, target.method, target.params, revision),
                                 asked_for(target.needs, {}), label);
            EXPECT_EQ(target.ran(harness.items), before) << label;
            const Reply served = session_request(harness.port, session, wide, target.method, target.params, revision);
            EXPECT_EQ(served.status, 200) << label << " " << served.body;
            EXPECT_TRUE(json::parse(served.body).contains("result")) << label << " " << served.body;
            const bool runs_an_item = target.method.find("subscribe") == std::string::npos;
            EXPECT_EQ(target.ran(harness.items), before + (runs_an_item ? 1 : 0)) << label;
        }
    }
}

TEST(ResourceServerScenarioScope, AnItemWithoutDeclarationsAndAnUnknownOrSwitchedOffOneAreNotRefusedForScopes) {
    Harness harness;
    const std::string bare = harness.token("bare", granted(harness.clock, "a", {"mcp:use"}));
    EXPECT_EQ(call_tool(harness.port, bare, "open_tool").status, 200);
    EXPECT_EQ(modern_request(harness.port, bare, "resources/read", json{{"uri", "test://open"}}, "test://open").status, 200);
    // Nothing is known of an item that does not exist, or is switched off: no scope is asked for, and the usual error
    // for a name that is not there is what comes back.
    for (const char* name : {"no-such-tool", "hidden"}) {
        const Reply reply = call_tool(harness.port, bare, name);
        EXPECT_NE(reply.status, 403) << name;
        EXPECT_NE(reply.status, 401) << name;
        EXPECT_EQ(reply.count("WWW-Authenticate"), 0u) << name;
        EXPECT_EQ(harness.items.hidden_runs.load(), 0) << name;
    }
    const Reply no_resource =
        modern_request(harness.port, bare, "resources/read", json{{"uri", "test://nothing"}}, "test://nothing");
    EXPECT_NE(no_resource.status, 403);
    EXPECT_EQ(no_resource.count("WWW-Authenticate"), 0u);
}

TEST(ResourceServerScenarioScope, ListsShowEveryEnabledItemWhateverScopesTheTokenHolds) {
    Harness harness;
    const std::string bare = harness.token("bare", granted(harness.clock, "a", {"mcp:use"}));
    const auto names = [&](const std::string& method, const char* member, const char* key) {
        const Reply reply = modern_request(harness.port, bare, method);
        EXPECT_EQ(reply.status, 200) << method;
        std::set<std::string> found;
        const json parsed = json::parse(reply.body);
        for (const json& item : parsed.at("result").at(member)) found.insert(item.at(key).get<std::string>());
        return found;
    };
    const auto tools = names("tools/list", "tools", "name");
    EXPECT_TRUE(tools.count("guarded"));
    EXPECT_TRUE(tools.count("audited"));
    EXPECT_FALSE(tools.count("hidden")) << "switched off";
    EXPECT_TRUE(names("resources/list", "resources", "uri").count("test://secret"));
    EXPECT_TRUE(names("resources/templates/list", "resourceTemplates", "uriTemplate").count("doc://{name}"));
    EXPECT_TRUE(names("prompts/list", "prompts", "name").count("plan"));
}

// ---- The scopes a 403 asks for ---------------------------------------------------------------------------------------

TEST(ResourceServerScenarioScope, AGrantedScopeTheServerDoesNotNameIsNeverEchoedInTheChallenge) {
    Harness harness;
    const std::string token = harness.token(
        "t", granted(harness.clock, "a",
                     {"openid", "profile", "offline_access", "mcp:use", "email", "files:read", "x:y", "files:read"}));
    const Reply reply = call_tool(harness.port, token, "guarded");
    // What the request lacks, what every request needs, then what the token holds that the server names itself; each
    // once; nothing else the authorization server put in the token.
    EXPECT_EQ(scopes_of(reply), (Scopes{"files:write", "mcp:use", "files:read"})) << reply.everything();
    EXPECT_EQ(reply.everything().find("offline_access"), std::string::npos);
    EXPECT_EQ(reply.everything().find("openid"), std::string::npos);
}

// The scopes the server declares on an item are declared by the server too: a token that holds one is not asked to
// leave it out when it steps up for another.
TEST(ResourceServerScenarioScope, AGrantedScopeThatOnlyAnItemDeclaresIsEchoedLikeOneTheMetadataNames) {
    Harness harness;
    const std::string token = harness.token("audit", granted(harness.clock, "a", {"mcp:use", "audit:read"}));
    const Reply reply = call_tool(harness.port, token, "guarded");
    // audit:read is declared on the tool `audited` and is not in scopes_supported.
    EXPECT_EQ(scopes_of(reply), (Scopes{"files:write", "mcp:use", "audit:read"})) << reply.everything();
}

TEST(ResourceServerScenarioScope, TheScopesOfAChallengeAreCutToWhatAClientReadsAtTheEdge) {
    Scopes declared;
    for (int at = 0; at < 70; ++at) declared.push_back("s" + std::to_string(100 + at));
    Harness harness([&](neomcp::HttpServerOptions& options) {
        Scopes supported{"mcp:use", "files:read", "files:write"};
        supported.insert(supported.end(), declared.begin(), declared.end());
        options.authorization->scopes_supported = supported;
    });
    const auto token_with = [&](const std::string& name, std::size_t count) {
        Scopes scopes{"mcp:use"};
        scopes.insert(scopes.end(), declared.begin(), declared.begin() + static_cast<std::ptrdiff_t>(count));
        return harness.token(name, granted(harness.clock, "a", scopes));
    };
    // The challenge asks for files:write and mcp:use, then the granted scopes the server names: 64 scopes fit.
    const Reply fits = call_tool(harness.port, token_with("fits", 62), "guarded");
    EXPECT_EQ(scopes_of(fits).size(), 64u);
    EXPECT_EQ(scopes_of(fits).front(), "files:write");
    // One more would be 65: the granted ones are left out, what the request needs stays.
    const Reply cut = call_tool(harness.port, token_with("cut", 63), "guarded");
    EXPECT_EQ(scopes_of(cut), (Scopes{"files:write", "mcp:use"}));
    EXPECT_EQ(call_tool(harness.port, token_with("all", 70), "guarded").status, 403);
    EXPECT_EQ(scopes_of(call_tool(harness.port, token_with("all", 70), "guarded")), (Scopes{"files:write", "mcp:use"}));
}

TEST(ResourceServerScenarioScope, TheBytesOfAChallengesScopesAreCutToWhatAClientReadsToo) {
    Scopes wide;
    for (int at = 0; at < 40; ++at) wide.push_back("w" + std::to_string(100 + at) + std::string(116, 'x'));  // 120 bytes
    Harness harness([&](neomcp::HttpServerOptions& options) {
        Scopes supported{"mcp:use", "files:write"};
        supported.insert(supported.end(), wide.begin(), wide.end());
        options.authorization->scopes_supported = supported;
    });
    const auto token_with = [&](const std::string& name, std::size_t count) {
        Scopes scopes{"mcp:use"};
        scopes.insert(scopes.end(), wide.begin(), wide.begin() + static_cast<std::ptrdiff_t>(count));
        return harness.token(name, granted(harness.clock, "a", scopes));
    };
    const Reply fits = call_tool(harness.port, token_with("thirty", 30), "guarded");
    EXPECT_EQ(scopes_of(fits).size(), 32u) << "30 granted, and the two every challenge has";
    const Reply cut = call_tool(harness.port, token_with("forty", 40), "guarded");
    EXPECT_EQ(scopes_of(cut), (Scopes{"files:write", "mcp:use"})) << "40 x 120 bytes is more than a client reads";
}

// What one item needs is asked for in one challenge: the server does not start when that challenge (the item's scopes
// and required_scopes, here mcp:use) would be more than a client reads, in scopes or in bytes, and starts at the edge.
bool starts_with_one_item_declaring(const Scopes& declared) {
    FakeClock clock;
    auto table = std::make_shared<Table>();
    neomcp::Server server(server_options());
    server.tool("needs-many", "Needs a lot", [] { return std::string("ok"); }).scopes(declared);
    neomcp::HttpServerOptions options = quick_options();
    options.authorization = resource_options(table, clock);
    neomcp::HttpServer http(server, std::move(options));
    const bool started = static_cast<bool>(http.start());
    http.stop();
    return started;
}

TEST(ResourceServerScenarioScope, AnItemThatDeclaresMoreScopesThanAChallengeCanCarryIsRefusedWhenTheServerStarts) {
    using neomcp::http::ResourceServerDefaults;
    const auto numbered = [](std::size_t count) {
        Scopes scopes;
        for (std::size_t at = 0; at < count; ++at) scopes.push_back("m" + std::to_string(100 + at));
        return scopes;
    };
    // With mcp:use, max_challenge_scopes in all: the edge; one more is past it.
    EXPECT_TRUE(starts_with_one_item_declaring(numbered(ResourceServerDefaults::max_challenge_scopes - 1)));
    EXPECT_FALSE(starts_with_one_item_declaring(numbered(ResourceServerDefaults::max_challenge_scopes)));
    // A scope the item shares with required_scopes is asked for once, so it does not count twice.
    Scopes shared = numbered(ResourceServerDefaults::max_challenge_scopes - 1);
    shared.push_back("mcp:use");
    EXPECT_TRUE(starts_with_one_item_declaring(shared));
    // Bytes: the item's scope, a space and mcp:use, max_challenge_scope_bytes in all; one byte more is past it.
    const std::size_t rest = std::string_view(" mcp:use").size();
    const std::string widest(ResourceServerDefaults::max_challenge_scope_bytes - rest, 'w');
    EXPECT_TRUE(starts_with_one_item_declaring({widest}));
    EXPECT_FALSE(starts_with_one_item_declaring({widest + "w"}));
}

// What several items need together (here a subscription that names three resources) may be more than one challenge
// carries: the challenge asks for what fits, in order, and keeps the scopes every request needs.
TEST(ResourceServerScenarioScope, WhatSeveralItemsNeedTogetherIsCutToOneChallengeAClientReads) {
    using neomcp::http::ResourceServerDefaults;
    FakeClock clock;
    auto table = std::make_shared<Table>();
    neomcp::Server server(server_options());
    const std::size_t each = ResourceServerDefaults::max_challenge_scopes / 2;
    Scopes all;
    for (int resource = 0; resource < 3; ++resource) {
        Scopes declared;
        for (std::size_t at = 0; at < each; ++at) declared.push_back("r" + std::to_string(resource) + "-" + std::to_string(at));
        all.insert(all.end(), declared.begin(), declared.end());
        server.resource("test://r" + std::to_string(resource), "Needs its own scopes", [] { return std::string("r"); })
            .scopes(declared);
    }
    neomcp::HttpServerOptions options = quick_options();
    options.authorization = resource_options(table, clock);
    neomcp::HttpServer http(server, std::move(options));
    const auto started = http.start();
    ASSERT_TRUE(started) << started.error;
    table->set("t", granted(clock, "a", {"mcp:use"}));
    const Reply reply =
        send(*started.value, "POST", modern_headers("subscriptions/listen", "t"),
             rpc(5, "subscriptions/listen",
                 modern_params({{"notifications", {{"resourceSubscriptions", {"test://r0", "test://r1", "test://r2"}}}}})));
    ASSERT_EQ(reply.status, 403) << reply.body;
    const Scopes asked = scopes_of(reply);
    EXPECT_EQ(asked.size(), ResourceServerDefaults::max_challenge_scopes);
    EXPECT_LE(challenge_of(reply).scope.value_or("").size(), ResourceServerDefaults::max_challenge_scope_bytes);
    // The first resources' scopes in their order, then what every request needs.
    EXPECT_EQ(asked.front(), all.front());
    EXPECT_EQ(asked.back(), "mcp:use");
    EXPECT_TRUE(std::equal(asked.begin(), asked.end() - 1, all.begin()));
    http.stop();
}

// ---- Batches ---------------------------------------------------------------------------------------------------------

TEST(ResourceServerScenarioScope, ABatchMixingAllowedAndRefusedRequestsRunsNoneAndAsksForTheUnionOnce) {
    Harness harness;
    auto& clock = harness.clock;
    const char* revision = "2025-03-26";
    const std::string bare = harness.token("bare", granted(clock, "a", {"mcp:use"}));
    const std::string wide = harness.token("wide", granted(clock, "a", {"mcp:use", "files:write", "audit:read"}));
    const std::string session = open_session(harness.port, bare, revision);
    int next_id = 0;
    const auto call_of = [&next_id](const char* tool) {
        return json{{"jsonrpc", "2.0"},
                    {"id", ++next_id},
                    {"method", "tools/call"},
                    {"params", {{"name", tool}, {"arguments", json::object()}}}};
    };
    json batch = json::array({call_of("whoami"), call_of("guarded"), call_of("open_tool"), call_of("audited"), call_of("guarded")});
    const Reply refused = send(harness.port, "POST", legacy_headers(session, bare, revision), batch.dump());
    expect_scope_refusal(refused, (Scopes{"files:write", "audit:read", "mcp:use"}), "a batch");
    EXPECT_EQ(harness.items.total(), 0) << "not even the requests that were allowed ran";

    // An unknown tool asks for nothing, and does not hide the request that is refused.
    json with_unknown = json::array({json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                         {"params", {{"name", "no-such-tool"}, {"arguments", json::object()}}}},
                                    call_of("audited")});
    expect_scope_refusal(send(harness.port, "POST", legacy_headers(session, bare, revision), with_unknown.dump()),
                         (Scopes{"audit:read", "mcp:use"}), "a batch with an unknown tool");
    EXPECT_EQ(harness.items.total(), 0);

    // With what every request of it needs, the whole batch is served, and every request runs.
    const Reply served = send(harness.port, "POST", legacy_headers(session, wide, revision), batch.dump());
    EXPECT_EQ(served.status, 200) << served.body;
    ASSERT_TRUE(json::parse(served.body).is_array()) << served.body;
    EXPECT_EQ(json::parse(served.body).size(), 5u);
    EXPECT_EQ(harness.items.whoami.load(), 1);
    EXPECT_EQ(harness.items.open_tool.load(), 1);
    EXPECT_EQ(harness.items.guarded.load(), 2);
    EXPECT_EQ(harness.items.audited.load(), 1);
}

// ---- Scopes imply nothing unless the server declared it --------------------------------------------------------------

TEST(ResourceServerScenarioScope, ScopesImplyNothingUnlessDeclaredAndADeclaredImplicationIsFollowedAllTheWay) {
    Harness harness([](neomcp::HttpServerOptions&) {}, [](neomcp::ServerOptions& options) {
        options.scope_implications = neomcp::ScopeImplications(
            neomcp::ScopeImplications::Declared{{"super", {"admin"}}, {"admin", {"mcp:use", "files:write"}}});
    });
    auto& clock = harness.clock;
    // Text implies nothing: no wildcard, no prefix, no case folding, no suffix.
    std::size_t index = 0;
    for (const char* held : {"files:*", "files", "files:", "FILES:WRITE", "files:write:extra", "files:wri", "*", "files:write "}) {
        const std::string name = "t" + std::to_string(index++);
        harness.token(name, granted(clock, "a", {"mcp:use", held}));
        const Reply reply = call_tool(harness.port, name, "guarded");
        EXPECT_EQ(reply.status, 403) << held;
    }
    EXPECT_EQ(harness.items.guarded.load(), 0);
    harness.token("admin", granted(clock, "a", {"admin"}));
    harness.token("super", granted(clock, "a", {"super"}));
    harness.token("implied-only-down", granted(clock, "a", {"files:write"}));
    EXPECT_EQ(call_tool(harness.port, "admin", "guarded").status, 200);
    EXPECT_EQ(call_tool(harness.port, "super", "guarded").status, 200) << "two steps";
    EXPECT_EQ(call_tool(harness.port, "super", "audited").status, 403) << "super implies nothing about audit:read";
    EXPECT_EQ(call_tool(harness.port, "implied-only-down", "open_tool").status, 403)
        << "files:write does not imply mcp:use: the implications run one way";
    EXPECT_EQ(harness.items.guarded.load(), 2);
}

// ---- The change feed of the subscriptions ----------------------------------------------------------------------------

// The messages of an event stream body: the JSON of each `data:` line, in order.
std::vector<json> messages_of(const std::string& body) {
    std::vector<json> messages;
    std::size_t at = 0;
    while (at < body.size()) {
        std::size_t end = body.find('\n', at);
        if (end == std::string::npos) end = body.size();
        const std::string line = body.substr(at, end - at);
        at = end + 1;
        if (line.rfind("data:", 0) != 0) continue;
        std::string text = line.substr(5);
        if (!text.empty() && text.front() == ' ') text.erase(0, 1);
        if (text.empty()) continue;
        messages.push_back(json::parse(text));
    }
    return messages;
}

std::vector<std::string> updated_resources(const std::string& body) {
    std::vector<std::string> uris;
    for (const json& message : messages_of(body)) {
        if (message.value("method", "") == "notifications/resources/updated") {
            uris.push_back(message.at("params").at("uri").get<std::string>());
        }
    }
    return uris;
}

bool has_method(const std::string& body, const std::string& method) {
    for (const json& message : messages_of(body)) {
        if (message.value("method", "") == method) return true;
    }
    return false;
}

TEST(ResourceServerScenarioScope, ASubscriptionIsOnlyToldOfTheResourcesItsAuthorizationCanRead) {
    Harness harness;
    auto& clock = harness.clock;
    const std::string plain = harness.token("plain", granted(clock, "a", {"mcp:use"}));
    const std::string reader = harness.token("reader", granted(clock, "b", {"mcp:use", "secret:read"}));
    const auto open_listen = [&](const std::string& token, json notifications) {
        return std::make_unique<Call>(harness.port, "POST", modern_headers("subscriptions/listen", token),
                                      rpc(5, "subscriptions/listen", modern_params({{"notifications", std::move(notifications)}})));
    };
    // Naming a resource that needs a scope the token lacks is refused (403), and a stream is never opened.
    const Reply refused =
        send(harness.port, "POST", modern_headers("subscriptions/listen", plain),
             rpc(5, "subscriptions/listen",
                 modern_params({{"notifications", {{"resourceSubscriptions", {"test://open", "test://secret"}}}}})));
    expect_scope_refusal(refused, (Scopes{"secret:read", "mcp:use"}), "a subscription that names a secret");

    auto names_open = open_listen(plain, json{{"resourceSubscriptions", {"test://open"}}, {"toolsListChanged", true}});
    auto names_none = open_listen(plain, json{{"toolsListChanged", true}});
    auto names_both = open_listen(reader, json{{"resourceSubscriptions", {"test://secret", "test://open"}}});
    const auto acknowledged = [](const std::string& body) { return body.find("acknowledged") != std::string::npos; };
    ASSERT_TRUE(names_open->wait_until(acknowledged));
    ASSERT_TRUE(names_none->wait_until(acknowledged));
    ASSERT_TRUE(names_both->wait_until(acknowledged));

    // The secret resource changes, then the open one, then a tool is switched on (which every stream that asked for the
    // list is told, whatever its scopes: lists are not filtered).
    harness.server.notify_resource_updated("test://secret");
    harness.server.notify_resource_updated("test://open");
    ASSERT_TRUE(harness.items.hidden.has_value());
    harness.items.hidden->enable(true);
    const auto list_changed = [](const std::string& body) {
        return has_method(body, "notifications/tools/list_changed");
    };
    ASSERT_TRUE(names_open->wait_until(list_changed));
    ASSERT_TRUE(names_none->wait_until(list_changed));
    ASSERT_TRUE(names_both->wait_until([](const std::string& body) { return updated_resources(body).size() >= 2; }));

    EXPECT_EQ(updated_resources(names_open->now().body), (std::vector<std::string>{"test://open"}));
    EXPECT_TRUE(updated_resources(names_none->now().body).empty())
        << "a stream that names no resource is told of no update, so it learns nothing of a resource it cannot read";
    const auto both = updated_resources(names_both->now().body);
    EXPECT_EQ(std::set<std::string>(both.begin(), both.end()), (std::set<std::string>{"test://secret", "test://open"}));
    for (auto* stream : {names_open.get(), names_none.get(), names_both.get()}) {
        EXPECT_EQ(stream->now().body.find("test://secret") != std::string::npos, stream == names_both.get());
        stream->disconnect();
    }
}

// ---- A retried call under another principal --------------------------------------------------------------------------

TEST(ResourceServerScenarioScope, AToolsAnswerToItsQuestionIsTakenOnlyFromThePrincipalThatWasAsked) {
    Harness harness;
    auto& clock = harness.clock;
    const std::string alice = harness.token("alice", granted(clock, "alice", {"mcp:use"}));
    const std::string refreshed = harness.token("alice-refreshed", granted(clock, "alice", {"mcp:use", "files:read"}));
    const std::string bob = harness.token("bob", granted(clock, "bob", {"mcp:use"}));
    const std::string other_client = harness.token("other-client", granted(clock, "alice", {"mcp:use"}, 3600s, first_issuer,
                                                                           {configured_resource}, "client-2"));
    const json forms{{"elicitation", json{{"form", json::object()}}}};
    const auto ask = [&](const std::string& token) {
        return modern_request(harness.port, token, "tools/call", json{{"name", "confirm"}, {"arguments", json::object()}},
                              "confirm", forms);
    };
    const auto retry = [&](const std::optional<std::string>& token, const json& state) {
        json params{{"name", "confirm"},
                    {"arguments", json::object()},
                    {"inputResponses", json{{"confirm", json{{"action", "accept"}, {"content", json{{"ok", true}}}}}}},
                    {"requestState", state}};
        return modern_request(harness.port, token, "tools/call", std::move(params), "confirm", forms);
    };
    const Reply asked = ask(alice);
    ASSERT_EQ(asked.status, 200) << asked.body;
    const json result = json::parse(asked.body).at("result");
    ASSERT_EQ(result.at("resultType"), "input_required") << asked.body;
    const json state = result.at("requestState");

    const auto refused = [](const Reply& reply) {
        const json body = json::parse(reply.body.empty() ? "{}" : reply.body);
        return body.contains("error") && body.at("error").contains("data") &&
               body.at("error").at("data").value("reason", "") == "invalid_request_state";
    };
    EXPECT_TRUE(refused(retry(bob, state))) << "another subject";
    EXPECT_TRUE(refused(retry(other_client, state))) << "the same subject through another client";
    EXPECT_EQ(retry(std::nullopt, state).status, 401) << "no token at all never gets as far as the state";
    EXPECT_EQ(harness.items.confirm.load(), 1) << "the handler ran for the first round only";
    // The same principal with a refreshed token (other scopes, another expiry) goes on.
    const Reply done = retry(refreshed, state);
    EXPECT_EQ(done.status, 200) << done.body;
    EXPECT_EQ(text_of(done), "confirmed");
    EXPECT_EQ(harness.items.confirm.load(), 2);
}

}  // namespace
