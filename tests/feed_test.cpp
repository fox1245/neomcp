// Tests of the change feed in the session engine: one scripted host, served under both mechanisms of `change_feed`.
//
// Under connection_notifications (the 2025-11-25 profile) the client opens a connection, watches resources with
// resources/subscribe and hears of changes as notifications of the connection; under subscriptions_listen (the
// 2026-07-28 profile) the client opens subscriptions with subscriptions/listen and hears of changes on each one. The
// host switches items on and off and reports changes when a test tells it to. Every wait is on an event a test
// controls; the bound on a wait only limits how long a broken build takes to fail. A check that something is NOT sent
// sends a later message the client must get and looks at what came before it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/change.h"
#include "neomcp/feature_host.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/mechanism.h"
#include "neomcp/profile.h"
#include "neomcp/session.h"
#include "neomcp/testing/peer.h"
#include "neomcp/tool_host.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* legacy_revision = "2025-11-25";

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

class Counter {
public:
    void add() {
        {
            std::lock_guard lock(mutex_);
            ++value_;
        }
        changed_.notify_all();
    }
    bool wait_for_at_least(int n) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return value_ >= n; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int value_ = 0;
};

neomcp::ToolResult text_result(std::string text) {
    neomcp::ToolResult result;
    neomcp::TextContent block;
    block.text = std::move(text);
    result.content.emplace_back(std::move(block));
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// The scripted host: three tools, two prompts, two resources and a template, each of which a test can switch off,
// and changes a test reports.

class FeedHost final : public neomcp::ToolHost, public neomcp::ResourceHost, public neomcp::PromptHost {
public:
    explicit FeedHost(neomcp::ChangeSupport reports = everything()) : support_(reports) {
        for (const char* name : {"alpha", "beta", "gamma"}) {
            neomcp::ToolDefinition tool;
            tool.name = name;
            tool.description = std::string("The tool ") + name;
            tools_.push_back(std::move(tool));
            tool_on_.emplace_back(true);
        }
        for (const char* name : {"greeting", "farewell"}) {
            neomcp::PromptDefinition prompt;
            prompt.name = name;
            prompt.description = std::string("The prompt ") + name;
            prompts_.push_back(std::move(prompt));
            prompt_on_.emplace_back(true);
        }
        for (const char* uri : {"test://one", "test://two"}) {
            neomcp::ResourceDefinition resource;
            resource.uri = uri;
            resource.name = uri;
            resources_.push_back(std::move(resource));
            resource_on_.emplace_back(true);
        }
        neomcp::ResourceTemplateDefinition item;
        item.uri_template = "test://item/{id}";
        item.name = "item";
        templates_.push_back(std::move(item));
        template_on_.emplace_back(true);
    }

    static neomcp::ChangeSupport everything() { return {true, true, true, true}; }

    void switch_tool(std::size_t index, bool on) { tool_on_.at(index).store(on); }
    void switch_prompt(std::size_t index, bool on) { prompt_on_.at(index).store(on); }
    void switch_resource(std::size_t index, bool on) { resource_on_.at(index).store(on); }
    void switch_template(std::size_t index, bool on) { template_on_.at(index).store(on); }

    // Tells every registered listener.
    void report(const neomcp::Change& change) {
        for (const auto& listener : listeners()) listener->changed(change);
    }
    void report(neomcp::ChangeKind kind, std::string uri = {}) { report(neomcp::Change{kind, std::move(uri)}); }
    // Stops, as a server that shuts down does.
    void end_feed() {
        std::vector<std::shared_ptr<neomcp::ChangeListener>> told = listeners();
        {
            std::lock_guard lock(mutex_);
            ended_ = true;
            listeners_.clear();
        }
        changed_.notify_all();
        for (const auto& listener : told) listener->ended();
    }

    std::size_t watchers() const {
        std::lock_guard lock(mutex_);
        return listeners_.size();
    }
    std::size_t registrations() const {
        std::lock_guard lock(mutex_);
        return registrations_;
    }
    bool wait_for_watchers(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return listeners_.size() == count; });
    }

    // ToolHost
    neomcp::ResourceHost* resource_host() noexcept override { return this; }
    neomcp::PromptHost* prompt_host() noexcept override { return this; }
    std::span<const neomcp::ToolDefinition> tools() const override { return tools_; }
    bool tool_enabled(std::size_t index) const override { return tool_on_.at(index).load(); }
    void call(const neomcp::CallRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        done(text_result("ran " + request.name));
    }
    neomcp::ChangeSupport change_support() const override { return support_; }
    neomcp::ChangeRegistration watch(std::weak_ptr<neomcp::ChangeListener> listener) override {
        std::uint64_t id = 0;
        {
            std::lock_guard lock(mutex_);
            if (ended_) return {};
            id = next_++;
            listeners_.emplace(id, std::move(listener));
            ++registrations_;
        }
        changed_.notify_all();
        return neomcp::ChangeRegistration([this, id] {
            {
                std::lock_guard lock(mutex_);
                listeners_.erase(id);
            }
            changed_.notify_all();
        });
    }

    // ResourceHost
    std::span<const neomcp::ResourceDefinition> resources() const override { return resources_; }
    std::span<const neomcp::ResourceTemplateDefinition> resource_templates() const override { return templates_; }
    bool resource_enabled(std::size_t index) const override { return resource_on_.at(index).load(); }
    bool template_enabled(std::size_t index) const override { return template_on_.at(index).load(); }
    bool offers(std::string_view uri) const override {
        constexpr std::string_view item_prefix = "test://item/";
        if (uri.substr(0, item_prefix.size()) == item_prefix && uri.size() > item_prefix.size()) {
            return template_on_.at(0).load();
        }
        return ResourceHost::offers(uri);
    }
    void read(const neomcp::ReadRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        if (!offers(request.uri)) {
            done(neomcp::FeatureOutcome(neomcp::NotFound{request.uri}));
            return;
        }
        neomcp::ReadResult result;
        result.contents.emplace_back(neomcp::TextResourceContents{request.uri, std::nullopt, "content", std::nullopt});
        done(neomcp::FeatureOutcome(std::move(result)));
    }

    // PromptHost
    std::span<const neomcp::PromptDefinition> prompts() const override { return prompts_; }
    bool prompt_enabled(std::size_t index) const override { return prompt_on_.at(index).load(); }
    void get(const neomcp::PromptRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        neomcp::PromptResult result;
        neomcp::TextContent text;
        text.text = "prompt " + request.name;
        result.messages.push_back(neomcp::PromptMessage{neomcp::Role::user, std::move(text)});
        done(neomcp::FeatureOutcome(std::move(result)));
    }

private:
    std::vector<std::shared_ptr<neomcp::ChangeListener>> listeners() {
        std::vector<std::shared_ptr<neomcp::ChangeListener>> out;
        std::lock_guard lock(mutex_);
        for (const auto& entry : listeners_) {
            if (auto held = entry.second.lock()) out.push_back(std::move(held));
        }
        return out;
    }

    neomcp::ChangeSupport support_;
    std::vector<neomcp::ToolDefinition> tools_;
    std::vector<neomcp::PromptDefinition> prompts_;
    std::vector<neomcp::ResourceDefinition> resources_;
    std::vector<neomcp::ResourceTemplateDefinition> templates_;
    // In deques: an atomic cannot be moved, and a deque never moves its elements when it grows.
    std::deque<std::atomic<bool>> tool_on_;
    std::deque<std::atomic<bool>> prompt_on_;
    std::deque<std::atomic<bool>> resource_on_;
    std::deque<std::atomic<bool>> template_on_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::uint64_t, std::weak_ptr<neomcp::ChangeListener>> listeners_;
    std::uint64_t next_ = 1;
    std::size_t registrations_ = 0;
    bool ended_ = false;
};

neomcp::SessionOptions options_for(const std::string& revision) {
    neomcp::SessionOptions options;
    options.profiles = {builtin(revision)};
    options.server_info.name = "feed-test";
    options.server_info.version = "1.0.0";
    return options;
}

// A session of one revision on the host; a legacy one has its connection opened (initialize, then initialized).
struct Client {
    Client(FeedHost& host, const std::string& revision, neomcp::SessionOptions options)
        : profile(builtin(revision)), peer(host, std::move(options)) {
        if (profile->mechanism(profile::Slot::open) == "initialize") {
            opened = peer.initialize(*profile, patience);
            if (!opened) throw std::runtime_error("the handshake was not answered");
        }
    }
    Client(FeedHost& host, const std::string& revision) : Client(host, revision, options_for(revision)) {}

    json request(const std::string& method, json params = json::object()) {
        const neomcp::RequestId id(next_id++);
        auto answer = peer.request(Peer::make_request(*profile, id, method, std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer to " + method);
        return *answer;
    }
    json result(const std::string& method, json params = json::object()) {
        json answer = request(method, std::move(params));
        if (!answer.contains("result")) throw std::runtime_error(method + " failed: " + answer.dump());
        return answer.at("result");
    }
    // The server's capabilities: from the handshake, or from server/discover.
    json capabilities() {
        if (opened) return opened->at("result").at("capabilities");
        return result("server/discover").at("capabilities");
    }

    profile::ProfilePtr profile;
    Peer peer;
    std::optional<json> opened;
    std::uint64_t next_id = 100;
};

std::vector<std::string> names(const json& list, const char* key) {
    std::vector<std::string> out;
    for (const auto& item : list) out.push_back(item.at(key).get<std::string>());
    return out;
}

class EveryRevision : public ::testing::TestWithParam<const char*> {};

INSTANTIATE_TEST_SUITE_P(Feed, EveryRevision, ::testing::Values(legacy_revision, modern_revision));

// ---------------------------------------------------------------------------------------------------------------
// Items switched off

TEST_P(EveryRevision, AnItemSwitchedOffIsNotListed) {
    FeedHost host;
    Client client(host, GetParam());
    host.switch_tool(1, false);
    host.switch_prompt(0, false);
    host.switch_resource(1, false);
    EXPECT_EQ(names(client.result("tools/list").at("tools"), "name"), (std::vector<std::string>{"alpha", "gamma"}));
    EXPECT_EQ(names(client.result("prompts/list").at("prompts"), "name"), (std::vector<std::string>{"farewell"}));
    EXPECT_EQ(names(client.result("resources/list").at("resources"), "uri"), (std::vector<std::string>{"test://one"}));
    EXPECT_EQ(client.result("resources/templates/list").at("resourceTemplates").size(), 1u);
    host.switch_template(0, false);
    EXPECT_TRUE(client.result("resources/templates/list").at("resourceTemplates").empty());
    host.switch_tool(1, true);
    EXPECT_EQ(names(client.result("tools/list").at("tools"), "name"),
              (std::vector<std::string>{"alpha", "beta", "gamma"}));
}

TEST_P(EveryRevision, AToolSwitchedOffIsAnsweredAsOneThatDoesNotExist) {
    FeedHost host;
    Client client(host, GetParam());
    EXPECT_EQ(client.result("tools/call", json{{"name", "beta"}}).at("content").at(0).at("text"), "ran beta");
    host.switch_tool(1, false);
    const json refused = client.request("tools/call", json{{"name", "beta"}});
    const json unknown = client.request("tools/call", json{{"name", "delta"}});
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused.at("error").at("code"), unknown.at("error").at("code"));
    EXPECT_EQ(refused.at("error").at("message"), "Unknown tool: beta");
}

TEST_P(EveryRevision, APromptSwitchedOffIsAnsweredAsOneThatDoesNotExist) {
    FeedHost host;
    Client client(host, GetParam());
    EXPECT_FALSE(client.result("prompts/get", json{{"name", "greeting"}}).at("messages").empty());
    host.switch_prompt(0, false);
    const json refused = client.request("prompts/get", json{{"name", "greeting"}});
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused.at("error").at("code"), neomcp::error_code::invalid_params);
}

TEST_P(EveryRevision, PagesSkipItemsSwitchedOffAndKeepTheirCursors) {
    FeedHost host;
    neomcp::SessionOptions options = options_for(GetParam());
    options.tools_page_size = 1;
    Client client(host, GetParam(), std::move(options));
    host.switch_tool(1, false);
    const json first = client.result("tools/list");
    EXPECT_EQ(names(first.at("tools"), "name"), (std::vector<std::string>{"alpha"}));
    // The cursor names the next tool offered, past the one switched off.
    ASSERT_EQ(first.at("nextCursor"), "2");
    const json second = client.result("tools/list", json{{"cursor", "2"}});
    EXPECT_EQ(names(second.at("tools"), "name"), (std::vector<std::string>{"gamma"}));
    EXPECT_FALSE(second.contains("nextCursor")) << "no tool is offered after the last page";
    // A cursor stays valid while the tools it passes are switched on and off.
    host.switch_tool(1, true);
    host.switch_tool(2, false);
    const json again = client.result("tools/list", json{{"cursor", "1"}});
    EXPECT_EQ(names(again.at("tools"), "name"), (std::vector<std::string>{"beta"}));
    EXPECT_FALSE(again.contains("nextCursor"));
}

// ---------------------------------------------------------------------------------------------------------------
// Capabilities

TEST_P(EveryRevision, TheCapabilitiesDeclareTheChangesTheHostReports) {
    FeedHost host;
    Client client(host, GetParam());
    const json capabilities = client.capabilities();
    EXPECT_EQ(capabilities.at("tools"), (json{{"listChanged", true}}));
    EXPECT_EQ(capabilities.at("prompts"), (json{{"listChanged", true}}));
    EXPECT_EQ(capabilities.at("resources"), (json{{"listChanged", true}, {"subscribe", true}}));
}

TEST_P(EveryRevision, AHostThatReportsNoChangesDeclaresNone) {
    FeedHost host(neomcp::ChangeSupport{});
    Client client(host, GetParam());
    const json capabilities = client.capabilities();
    EXPECT_EQ(capabilities.at("tools"), (json{{"listChanged", false}}));
    EXPECT_EQ(capabilities.at("prompts"), json::object());
    EXPECT_EQ(capabilities.at("resources"), json::object());
}

TEST_P(EveryRevision, OnlyTheKindsReportedAreDeclared) {
    neomcp::ChangeSupport updates_only;
    updates_only.resource_updates = true;
    FeedHost host(updates_only);
    Client client(host, GetParam());
    const json capabilities = client.capabilities();
    EXPECT_EQ(capabilities.at("tools"), (json{{"listChanged", false}}));
    EXPECT_EQ(capabilities.at("prompts"), json::object());
    EXPECT_EQ(capabilities.at("resources"), (json{{"subscribe", true}}));
}

TEST(FeedMechanism, ARevisionWhoseChangeFeedHasNoImplementationIsNotServed) {
    FeedHost host;
    neomcp::MechanismRegistry registry = neomcp::builtin_mechanisms();
    ASSERT_TRUE(registry.remove(neomcp::MechanismSlot::change_feed, "connection_notifications"));
    neomcp::SessionOptions options = options_for(legacy_revision);
    options.mechanisms = &registry;
    auto made = neomcp::Session::create(std::move(options), host, [](std::string) {});
    ASSERT_TRUE(std::holds_alternative<neomcp::CreateError>(made));
    const auto& error = std::get<neomcp::CreateError>(made);
    ASSERT_EQ(error.missing.size(), 1u) << error.message;
    EXPECT_EQ(error.missing[0].slot, neomcp::MechanismSlot::change_feed);
    EXPECT_EQ(error.missing[0].name, "connection_notifications");
}

// ---------------------------------------------------------------------------------------------------------------
// connection_notifications (2025-11-25): notifications of the connection

json notification(const std::string& method) { return json{{"jsonrpc", "2.0"}, {"method", method}}; }

json updated(const std::string& uri) {
    return json{{"jsonrpc", "2.0"}, {"method", "notifications/resources/updated"}, {"params", {{"uri", uri}}}};
}

TEST(ConnectionFeed, ListChangesReachTheConnectionOnceItsClientIsReady) {
    FeedHost host;
    const auto profile = builtin(legacy_revision);
    Peer peer(host, options_for(legacy_revision));
    ASSERT_TRUE(peer.request(Peer::initialize_request(*profile, 1), patience));
    EXPECT_EQ(host.watchers(), 0u) << "no registration before the client is ready";
    host.report(neomcp::ChangeKind::tools_list);  // reaches nobody
    peer.notify(notification("notifications/initialized"));
    EXPECT_EQ(host.watchers(), 1u);
    host.report(neomcp::ChangeKind::prompts_list);
    host.report(neomcp::ChangeKind::resources_list);
    host.report(neomcp::ChangeKind::tools_list);
    EXPECT_EQ(peer.next_notification(patience), notification("notifications/prompts/list_changed"))
        << "nothing of the change before the client was ready";
    EXPECT_EQ(peer.next_notification(patience), notification("notifications/resources/list_changed"));
    EXPECT_EQ(peer.next_notification(patience), notification("notifications/tools/list_changed"));
}

TEST(ConnectionFeed, AWatchedResourceIsUpdatedAndNoOther) {
    FeedHost host;
    Client client(host, legacy_revision);
    EXPECT_EQ(client.result("resources/subscribe", json{{"uri", "test://one"}}), json::object());
    EXPECT_EQ(client.result("resources/subscribe", json{{"uri", "test://item/7"}}), json::object())
        << "a URI a template matches names a resource";
    host.report(neomcp::ChangeKind::resource_updated, "test://two");  // not watched
    host.report(neomcp::ChangeKind::resource_updated, "test://one");
    EXPECT_EQ(client.peer.next_notification(patience), updated("test://one"));
    host.report(neomcp::ChangeKind::resource_updated, "test://item/7");
    EXPECT_EQ(client.peer.next_notification(patience), updated("test://item/7"));

    EXPECT_EQ(client.result("resources/unsubscribe", json{{"uri", "test://one"}}), json::object());
    EXPECT_EQ(client.result("resources/unsubscribe", json{{"uri", "test://never"}}), json::object())
        << "removing a URI that is not watched changes nothing and is not an error";
    host.report(neomcp::ChangeKind::resource_updated, "test://one");
    host.report(neomcp::ChangeKind::resource_updated, "test://item/7");
    EXPECT_EQ(client.peer.next_notification(patience), updated("test://item/7")) << "test://one is no longer watched";
}

TEST(ConnectionFeed, AUriThatNamesNoResourceIsRefusedAsAReadOfItIs) {
    FeedHost host;
    Client client(host, legacy_revision);
    const json read = client.request("resources/read", json{{"uri", "test://missing"}});
    const json refused = client.request("resources/subscribe", json{{"uri", "test://missing"}});
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused.at("error").at("code"), read.at("error").at("code"));
    EXPECT_EQ(refused.at("error").at("code"), *client.profile->error_code("resource_not_found"));
    EXPECT_EQ(refused.at("error").at("data"), (json{{"uri", "test://missing"}}));
    // A resource switched off names nothing either.
    host.switch_resource(0, false);
    EXPECT_TRUE(client.request("resources/subscribe", json{{"uri", "test://one"}}).contains("error"));
}

TEST(ConnectionFeed, AConnectionWatchesNoMoreResourcesThanTheBound) {
    FeedHost host;
    neomcp::SessionOptions options = options_for(legacy_revision);
    options.max_watched_resources = 2;
    Client client(host, legacy_revision, std::move(options));
    EXPECT_EQ(client.result("resources/subscribe", json{{"uri", "test://one"}}), json::object());
    EXPECT_EQ(client.result("resources/subscribe", json{{"uri", "test://two"}}), json::object());
    const json refused = client.request("resources/subscribe", json{{"uri", "test://item/1"}});
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused.at("error").at("code"), neomcp::error_code::internal_error);
    EXPECT_EQ(refused.at("error").at("data"), (json{{"reason", "limit"}}));
    EXPECT_EQ(client.result("resources/subscribe", json{{"uri", "test://one"}}), json::object())
        << "a URI watched already takes no more room";
    EXPECT_EQ(client.result("resources/unsubscribe", json{{"uri", "test://two"}}), json::object());
    EXPECT_EQ(client.result("resources/subscribe", json{{"uri", "test://item/1"}}), json::object());
}

TEST(ConnectionFeed, WithoutUpdatesReportedWatchingIsNotOffered) {
    FeedHost host(neomcp::ChangeSupport{true, true, true, false});
    Client client(host, legacy_revision);
    const json refused = client.request("resources/subscribe", json{{"uri", "test://one"}});
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused.at("error").at("code"), neomcp::error_code::method_not_found);
}

TEST(ConnectionFeed, TheSessionRegistersOnceAndLetsGoWhenItCloses) {
    FeedHost host;
    Client client(host, legacy_revision);
    EXPECT_EQ(host.watchers(), 1u);
    client.result("resources/subscribe", json{{"uri", "test://one"}});
    client.result("resources/subscribe", json{{"uri", "test://two"}});
    EXPECT_EQ(host.registrations(), 1u);
    client.peer.close();
    EXPECT_EQ(host.watchers(), 0u);
}

TEST(ConnectionFeed, AHostThatStopsTakesTheRegistrationAndNothingComesAfter) {
    FeedHost host;
    Client client(host, legacy_revision);
    client.result("resources/subscribe", json{{"uri", "test://one"}});
    host.end_feed();
    EXPECT_EQ(host.watchers(), 0u);
    host.report(neomcp::ChangeKind::tools_list);
    EXPECT_EQ(client.result("tools/list").at("tools").size(), 3u) << "the session serves on";
    const auto transcript = client.peer.transcript();
    EXPECT_TRUE(std::none_of(transcript.begin(), transcript.end(),
                             [](const json& message) { return message.contains("method"); }));
}

// The race the registration is for: a change reported on one thread while the session closes on another. Nothing is
// written after close() returned, and the host holds no registration of the closed session.
TEST(ConnectionFeed, AChangeRacingCloseIsNeverWrittenAfterIt) {
    const auto profile = builtin(legacy_revision);
    for (int round = 0; round < 200; ++round) {
        FeedHost host;
        std::atomic<bool> closed{false};
        Counter written;
        std::atomic<int> late{0};
        auto created = neomcp::Session::create(options_for(legacy_revision), host, [&](std::string) {
            if (closed.load()) ++late;
            written.add();
        });
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(created));
        auto session = std::get<std::unique_ptr<neomcp::Session>>(std::move(created));
        session->receive(Peer::initialize_request(*profile, 1).dump());
        session->receive(notification("notifications/initialized").dump());
        ASSERT_EQ(host.watchers(), 1u);
        std::atomic<bool> stop{false};
        std::thread reporter([&] {
            while (!stop.load()) host.report(neomcp::ChangeKind::tools_list);
        });
        ASSERT_TRUE(written.wait_for_at_least(2));  // the handshake's answer and one notification
        if (round % 2 == 0) {
            session->close();
            closed = true;
        } else {
            session.reset();  // destroying closes too, and the reporter may hold the listener meanwhile
            closed = true;
        }
        EXPECT_EQ(host.watchers(), 0u);
        stop = true;
        reporter.join();
        EXPECT_EQ(late.load(), 0) << "round " << round;
    }
}

}  // namespace
