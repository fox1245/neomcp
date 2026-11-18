// Tests of the change feed of Server: items switched off and on while it serves, resources reported as updated, and
// what the sessions of each revision tell their clients (on the stdio transport). Every wait is on an event a test
// controls; the bound on a wait only limits how long a broken build takes to fail. A check that something is NOT sent
// makes a later change the client must hear of and looks at what came before it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

struct ItemParams {
    std::string id;
};
NEOMCP_DESCRIBE(ItemParams, id, "the item")

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* legacy_revision = "2025-11-25";
constexpr const char* subscription_key = "io.modelcontextprotocol/subscriptionId";

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

bool opens_with_handshake(const profile::Profile& profile) {
    return profile.mechanism(profile::Slot::open) == "initialize";
}

// A server with two tools of which one starts switched off, tools that switch things from inside a handler, a
// prompt, a resource and a template.
struct Fixture {
    explicit Fixture(neomcp::ServerOptions options = {.name = "feed-server", .version = "1.0.0"})
        : server(std::move(options)) {
        server.tool("alpha", "Always there", [] { return std::string("alpha"); });
        beta = std::make_unique<neomcp::ToolHandle>(
            server.tool("beta", "Offered later", [] { return std::string("beta"); }).enable(false));
        server.tool("toggle_beta", "Switch beta", [this] {
            beta->enable(!beta->enabled());
            return std::string(beta->enabled() ? "on" : "off");
        });
        prompt = std::make_unique<neomcp::PromptHandle>(
            server.prompt("greeting", "A greeting", [] { return std::string("Hello"); }));
        server.tool("toggle_prompt", "Switch the prompt", [this] {
            prompt->enable(!prompt->enabled());
            return std::string("switched");
        });
        resource = std::make_unique<neomcp::ResourceHandle>(
            server.resource("test://watched", "Watched", [] { return std::string("content"); }));
        item = std::make_unique<neomcp::ResourceTemplateHandle>(server.resource_template<ItemParams>(
            "test://item/{id}", "An item", [](const ItemParams& params) { return "item " + params.id; }));
        server.tool("touch", "Report the watched resource as updated", [this] {
            server.notify_resource_updated("test://watched");
            return std::string("touched");
        });
    }

    neomcp::Server server;
    std::unique_ptr<neomcp::ToolHandle> beta;
    std::unique_ptr<neomcp::PromptHandle> prompt;
    std::unique_ptr<neomcp::ResourceHandle> resource;
    std::unique_ptr<neomcp::ResourceTemplateHandle> item;
};

// A client of one revision on the stdio transport; a legacy one has opened its connection.
struct Client {
    Client(neomcp::Server& server, const std::string& revision) : profile(builtin(revision)), peer(server) {
        if (opens_with_handshake(*profile)) {
            opened = peer.initialize(*profile, patience);
            if (!opened) throw std::runtime_error("the handshake was not answered");
        }
    }

    json request(const std::string& method, json params = json::object()) {
        auto answer = peer.request(Peer::make_request(*profile, next_id++, method, std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer to " + method);
        return *answer;
    }
    json result(const std::string& method, json params = json::object()) {
        json answer = request(method, std::move(params));
        if (!answer.contains("result")) throw std::runtime_error(method + " failed: " + answer.dump());
        return answer.at("result");
    }
    json call(const std::string& tool) { return request("tools/call", json{{"name", tool}}); }
    std::vector<std::string> tool_names() {
        std::vector<std::string> names;
        const json listed = result("tools/list");
        for (const auto& tool : listed.at("tools")) names.push_back(tool.at("name").get<std::string>());
        return names;
    }
    json capabilities() {
        if (opened) return opened->at("result").at("capabilities");
        return result("server/discover").at("capabilities");
    }

    // What the client asks to hear of: the connection is told of every list and of the resources it watches; a
    // modern client opens one subscription (id "feed") for the same.
    void listen(const std::vector<std::string>& resources) {
        if (opens_with_handshake(*profile)) {
            for (const auto& uri : resources) result("resources/subscribe", json{{"uri", uri}});
            return;
        }
        json filter{{"toolsListChanged", true}, {"promptsListChanged", true}, {"resourcesListChanged", true}};
        if (!resources.empty()) filter["resourceSubscriptions"] = resources;
        peer.notify(Peer::make_request(*profile, "feed", "subscriptions/listen", json{{"notifications", filter}}));
        const auto acknowledged = peer.next_notification(patience);
        if (!acknowledged || acknowledged->at("method") != "notifications/subscriptions/acknowledged") {
            throw std::runtime_error("the subscription was not acknowledged");
        }
    }

    // The next change notification, without the subscription's tag (which is checked here).
    std::optional<json> next_change() {
        auto message = peer.next_notification(patience);
        if (!message) return std::nullopt;
        if (!opens_with_handshake(*profile)) {
            EXPECT_EQ(message->at("params").at("_meta").at(subscription_key), "feed") << message->dump();
            message->at("params").erase("_meta");
            if (message->at("params").empty()) message->erase("params");
        }
        return message;
    }

    profile::ProfilePtr profile;
    Peer peer;
    std::optional<json> opened;
    std::uint64_t next_id = 100;
};

json changed(const std::string& method) { return json{{"jsonrpc", "2.0"}, {"method", method}}; }

json updated(const std::string& uri) {
    return json{{"jsonrpc", "2.0"}, {"method", "notifications/resources/updated"}, {"params", {{"uri", uri}}}};
}

std::string text_of(const json& answer) {
    if (!answer.contains("result")) return "(error) " + answer.dump();
    return answer.at("result").at("content").at(0).at("text").get<std::string>();
}

class EveryRevision : public ::testing::TestWithParam<const char*> {};

INSTANTIATE_TEST_SUITE_P(ServerFeed, EveryRevision, ::testing::Values(legacy_revision, modern_revision));

TEST_P(EveryRevision, TheServerDeclaresTheChangesOfEveryKindItHas) {
    Fixture fixture;
    Client client(fixture.server, GetParam());
    const json capabilities = client.capabilities();
    EXPECT_EQ(capabilities.at("tools"), (json{{"listChanged", true}}));
    EXPECT_EQ(capabilities.at("prompts"), (json{{"listChanged", true}}));
    EXPECT_EQ(capabilities.at("resources"), (json{{"listChanged", true}, {"subscribe", true}}));
}

TEST_P(EveryRevision, AToolSwitchedOnFromAHandlerIsListedAndCallableAndTheClientIsTold) {
    Fixture fixture;
    Client client(fixture.server, GetParam());
    client.listen({});
    EXPECT_EQ(client.tool_names(),
              (std::vector<std::string>{"alpha", "toggle_beta", "toggle_prompt", "touch"}));
    EXPECT_EQ(client.call("beta").at("error").at("message"), "Unknown tool: beta");
    EXPECT_EQ(text_of(client.call("toggle_beta")), "on");
    EXPECT_EQ(client.next_change(), changed("notifications/tools/list_changed"));
    EXPECT_EQ(client.tool_names(),
              (std::vector<std::string>{"alpha", "beta", "toggle_beta", "toggle_prompt", "touch"}));
    EXPECT_EQ(text_of(client.call("beta")), "beta");
    EXPECT_TRUE(fixture.beta->enabled());
    EXPECT_EQ(text_of(client.call("toggle_beta")), "off");
    EXPECT_EQ(client.next_change(), changed("notifications/tools/list_changed"));
    EXPECT_EQ(client.call("beta").at("error").at("message"), "Unknown tool: beta");
}

TEST_P(EveryRevision, ASwitchThatChangesNothingTellsNothing) {
    Fixture fixture;
    Client client(fixture.server, GetParam());
    client.listen({});
    fixture.beta->enable(false);  // off already
    fixture.prompt->enable(false);
    EXPECT_EQ(client.next_change(), changed("notifications/prompts/list_changed"))
        << "switching off what is off came first and told nothing";
    EXPECT_TRUE(client.result("prompts/list").at("prompts").empty());
    const json refused = client.request("prompts/get", json{{"name", "greeting"}});
    EXPECT_TRUE(refused.contains("error")) << refused.dump();
}

TEST_P(EveryRevision, ResourcesAndTemplatesSwitchedOffAreNotFound) {
    Fixture fixture;
    Client client(fixture.server, GetParam());
    client.listen({});
    EXPECT_EQ(client.result("resources/read", json{{"uri", "test://item/4"}}).at("contents").at(0).at("text"),
              "item 4");
    fixture.resource->enable(false);
    EXPECT_EQ(client.next_change(), changed("notifications/resources/list_changed"));
    fixture.item->enable(false);
    EXPECT_EQ(client.next_change(), changed("notifications/resources/list_changed")) << "a template is listed too";
    EXPECT_TRUE(client.result("resources/list").at("resources").empty());
    EXPECT_TRUE(client.result("resources/templates/list").at("resourceTemplates").empty());
    const json missing = client.request("resources/read", json{{"uri", "test://watched"}});
    const json unknown = client.request("resources/read", json{{"uri", "test://nothing"}});
    ASSERT_TRUE(missing.contains("error")) << missing.dump();
    EXPECT_EQ(missing.at("error").at("code"), unknown.at("error").at("code"));
    EXPECT_TRUE(client.request("resources/read", json{{"uri", "test://item/4"}}).contains("error"));
    fixture.item->enable();
    EXPECT_EQ(client.next_change(), changed("notifications/resources/list_changed"));
    EXPECT_EQ(client.result("resources/read", json{{"uri", "test://item/5"}}).at("contents").at(0).at("text"),
              "item 5");
}

TEST_P(EveryRevision, AnUpdatedResourceIsToldToTheClientsThatWatchIt) {
    Fixture fixture;
    Client watching(fixture.server, GetParam());
    Client other(fixture.server, GetParam());
    watching.listen({"test://watched"});
    other.listen({"test://item/1"});
    EXPECT_EQ(text_of(watching.call("touch")), "touched");
    EXPECT_EQ(watching.next_change(), updated("test://watched"));
    fixture.server.notify_resource_updated("test://item/1");
    EXPECT_EQ(other.next_change(), updated("test://item/1")) << "the update of test://watched came first, not to it";
    fixture.server.notify_resource_updated("test://item/1");
    fixture.server.notify_resource_updated("test://watched");
    EXPECT_EQ(watching.next_change(), updated("test://watched"));
}

TEST_P(EveryRevision, TheWatchersAreCountedAndLetGo) {
    std::mutex mutex;
    std::condition_variable changed_count;
    int calls = 0;
    neomcp::ServerOptions options{.name = "feed-server", .version = "1.0.0"};
    options.on_change_watchers = [&] {
        {
            std::lock_guard lock(mutex);
            ++calls;
        }
        changed_count.notify_all();
    };
    Fixture fixture(std::move(options));
    {
        Client client(fixture.server, GetParam());
        // A legacy session registers once its client said it is ready (the handshake did), a modern one when it
        // opens a subscription.
        EXPECT_EQ(fixture.server.change_watchers(), opens_with_handshake(*client.profile) ? 1u : 0u);
        client.listen({"test://watched"});
        EXPECT_EQ(fixture.server.change_watchers(), 1u);
        client.peer.close();
        EXPECT_EQ(fixture.server.change_watchers(), 0u);
    }
    std::unique_lock lock(mutex);
    EXPECT_TRUE(changed_count.wait_for(lock, patience, [&] { return calls >= 2; }));
}

TEST_P(EveryRevision, TheCompletersOfAPromptOrTemplateSwitchedOffAreOffWithIt) {
    neomcp::Server server(neomcp::ServerOptions{.name = "feed-server", .version = "1.0.0"});
    const auto suggest = [](std::string_view typed) { return std::vector<std::string>{std::string(typed) + "1"}; };
    const auto lookup =
        server.prompt<ItemParams>("lookup", "Look up an item", [](const ItemParams& params) { return params.id; })
            .complete("id", suggest);
    const auto item = server.resource_template<ItemParams>("test://item/{id}", "An item",
                                                           [](const ItemParams& params) { return params.id; })
                          .complete("id", suggest);
    Client client(server, GetParam());
    const json prompt_ref{{"ref", {{"type", "ref/prompt"}, {"name", "lookup"}}},
                          {"argument", {{"name", "id"}, {"value", "a"}}}};
    const json template_ref{{"ref", {{"type", "ref/resource"}, {"uri", "test://item/{id}"}}},
                            {"argument", {{"name", "id"}, {"value", "b"}}}};
    EXPECT_EQ(client.result("completion/complete", prompt_ref).at("completion").at("values"), (json{"a1"}));
    EXPECT_EQ(client.result("completion/complete", template_ref).at("completion").at("values"), (json{"b1"}));
    lookup.enable(false);
    item.enable(false);
    EXPECT_TRUE(client.request("completion/complete", prompt_ref).contains("error"));
    EXPECT_TRUE(client.request("completion/complete", template_ref).contains("error"));
    lookup.enable();
    EXPECT_EQ(client.result("completion/complete", prompt_ref).at("completion").at("values"), (json{"a1"}));
}

TEST(ServerFeed, ASubscriptionEndsGracefullyWhenTheServerShutsDown) {
    auto fixture = std::make_unique<Fixture>();
    Client client(fixture->server, modern_revision);
    client.listen({"test://watched"});
    fixture.reset();  // the session outlives the server
    const auto notice = client.peer.next_notification(patience);
    ASSERT_TRUE(notice);
    EXPECT_EQ(notice->at("method"), "notifications/cancelled");
    EXPECT_EQ(notice->at("params").at("requestId"), "feed");
    EXPECT_EQ(notice->at("params").at("_meta").at(subscription_key), "feed");
    const auto end = client.peer.await_response("feed", patience);
    ASSERT_TRUE(end);
    EXPECT_EQ(end->at("result").at("resultType"), "complete");
    EXPECT_EQ(end->at("result").at("_meta").at(subscription_key), "feed");
}

TEST(ServerFeed, OptionsThatCannotWorkAreRefused) {
    EXPECT_THROW(neomcp::Server(neomcp::ServerOptions{.name = "x", .version = "1", .max_subscriptions = 0}),
                 std::invalid_argument);
    EXPECT_THROW(neomcp::Server(neomcp::ServerOptions{.name = "x", .version = "1", .max_watched_resources = 0}),
                 std::invalid_argument);
}

// Switching a tool on and off on one thread while clients list it and call it on others: every list is whole, a call
// either runs or is answered as one of a tool that does not exist, and the legacy client hears of the changes.
TEST(ServerFeed, SwitchingRacesListsAndCalls) {
    Fixture fixture;
    Client watcher(fixture.server, legacy_revision);
    watcher.listen({});
    std::atomic<bool> stop{false};
    std::thread switcher([&] {
        bool on = false;
        while (!stop.load()) {
            on = !on;
            fixture.beta->enable(on);
        }
    });
    std::vector<std::thread> clients;
    std::atomic<int> problems{0};
    std::atomic<int> ran{0};
    std::atomic<int> refused{0};
    for (const char* revision : {modern_revision, legacy_revision}) {
        clients.emplace_back([&, revision] {
            Client client(fixture.server, revision);
            for (int i = 0; i < 300; ++i) {
                const auto names = client.tool_names();
                if (std::find(names.begin(), names.end(), "alpha") == names.end()) ++problems;
                const json answer = client.call("beta");
                if (answer.contains("result") && text_of(answer) == "beta") {
                    ++ran;
                } else if (answer.contains("error") && answer.at("error").at("message") == "Unknown tool: beta") {
                    ++refused;
                } else {
                    ++problems;
                }
            }
        });
    }
    for (auto& thread : clients) thread.join();
    stop = true;
    switcher.join();
    EXPECT_EQ(problems.load(), 0);
    EXPECT_EQ(ran.load() + refused.load(), 600);
    // The watcher heard of at least one change (the switcher changed the list many times).
    const auto heard = watcher.next_change();
    EXPECT_EQ(heard, changed("notifications/tools/list_changed"));
}

}  // namespace
