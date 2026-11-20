// Change notifications of the Client (neomcp/client/subscription.h): against the library's own Server in the same
// process on every revision, and against scripted peers for what that server never does (a stream that ends without
// its result, a notification of a subscription nobody opened, a burst faster than the application reads). Waits are
// bounded and wait for events; reopen delays are timers of a test timer service that the test fires.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include "neomcp/client.h"
#include "neomcp/client/in_process.h"
#include "neomcp/client/memory_transport.h"
#include "neomcp/server.h"
#include "support.h"

namespace {

using namespace client_test;
using namespace std::chrono_literals;
using neomcp::Change;
using neomcp::ChangeFilter;
using neomcp::ChangeKind;
using neomcp::ClientErrorKind;
using neomcp::FeedDelivery;
using neomcp::profile::Transport;

constexpr std::string_view modern = "2026-07-28";
constexpr std::string_view legacy = "2025-11-25";
constexpr std::string_view reads_arrays = "2025-03-26";
constexpr std::string_view subscription_key = "io.modelcontextprotocol/subscriptionId";
constexpr std::string_view watched_uri = "test://watched";
constexpr std::string_view other_uri = "test://other";
constexpr std::string_view missing_uri = "test://missing";

// ---------------------------------------------------------------------------------------------------------------
// Helpers

// The next change, or what ended the subscription; empty when nothing came within the bound. The wait is an
// async_next() raced against a timer on a context of the test's own, so a wait that gives up takes no change.
std::optional<neomcp::Result<Change>> next_within(neomcp::Subscription& subscription,
                                                  std::chrono::milliseconds bound = wait_bound) {
    using namespace asio::experimental::awaitable_operators;
    asio::io_context context;
    std::optional<neomcp::Result<Change>> got;
    asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor, bound);
            auto next = subscription.async_next();
            auto raced = co_await (std::move(next) || timer.async_wait(asio::as_tuple(asio::use_awaitable)));
            if (raced.index() == 0) got.emplace(std::move(std::get<0>(raced)));
        },
        asio::detached);
    context.run();
    return got;
}

// The next change, which must be one.
Change next_change(neomcp::Subscription& subscription) {
    auto got = next_within(subscription);
    if (!got) throw std::runtime_error("no change arrived in time");
    if (!*got) throw std::runtime_error("the subscription ended: " + got->error().message);
    return std::move(*got).value();
}

Change list_change(ChangeKind kind) { return Change{kind, {}}; }
Change update(std::string_view uri) { return Change{ChangeKind::resource_updated, std::string(uri)}; }

bool same(const Change& a, const Change& b) { return a.kind == b.kind && a.uri == b.uri; }

std::string describe(const Change& change) {
    switch (change.kind) {
        case ChangeKind::tools_list: return "tools_list";
        case ChangeKind::prompts_list: return "prompts_list";
        case ChangeKind::resources_list: return "resources_list";
        case ChangeKind::resource_updated: return "resource_updated " + change.uri;
    }
    return "?";
}

ChangeFilter every_kind(std::vector<std::string> resources = {}) {
    ChangeFilter filter;
    filter.tools_list = true;
    filter.prompts_list = true;
    filter.resources_list = true;
    filter.resources = std::move(resources);
    return filter;
}

ChangeFilter tools_only() {
    ChangeFilter filter;
    filter.tools_list = true;
    return filter;
}

// How the revision tells of changes, from its profile and the library's client half (never from its name).
FeedDelivery delivery_of(std::string_view revision) {
    const auto profile = neomcp::profile::builtin(revision);
    const auto half = neomcp::builtin_client_mechanisms().make_feed(profile->mechanism(neomcp::profile::Slot::change_feed));
    return half->delivery();
}

// ---------------------------------------------------------------------------------------------------------------
// The library's own server, in the same process

neomcp::ServerOptions served() {
    neomcp::ServerOptions options;
    options.name = "feed-server";
    options.version = "1.0.0";
    return options;
}

// A server whose lists and resources a test changes while it serves.
struct FeedServer {
    FeedServer() : server(served()) {
        server.tool("alpha", "Always there", [] { return std::string("alpha"); });
        beta.emplace(server.tool("beta", "Offered later", [] { return std::string("beta"); }).enable(false));
        prompt.emplace(server.prompt("greeting", "A greeting", [] { return std::string("Hello"); }));
        watched.emplace(server.resource(std::string(watched_uri), "Watched", [] { return std::string("content"); }));
        other.emplace(server.resource(std::string(other_uri), "Other", [] { return std::string("other"); }));
    }

    neomcp::Server server;
    std::optional<neomcp::ToolHandle> beta;
    std::optional<neomcp::PromptHandle> prompt;
    std::optional<neomcp::ResourceHandle> watched;
    std::optional<neomcp::ResourceHandle> other;
};

// Stands between the client and the in-process server and records what the client writes.
class RecordingPeer final : public neomcp::MemoryPeer {
public:
    explicit RecordingPeer(std::shared_ptr<neomcp::InProcessServer> inner) : inner_(std::move(inner)) {}

    std::optional<std::string> attach(neomcp::MemoryLink link) override { return inner_->attach(std::move(link)); }
    void receive(neomcp::Outgoing outgoing) override {
        {
            std::lock_guard lock(mutex_);
            kinds_.push_back(outgoing.kind);
            if (!outgoing.line.empty()) {
                const json message = json::parse(outgoing.line);
                if (message.contains("method")) methods_.push_back(message["method"].get<std::string>());
            }
        }
        inner_->receive(std::move(outgoing));
    }
    void detach(neomcp::CloseCause cause) override { inner_->detach(cause); }

    std::size_t count(std::string_view method) const {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count(methods_.begin(), methods_.end(), std::string(method)));
    }
    std::size_t count_kind(neomcp::OutgoingKind kind) const {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count(kinds_.begin(), kinds_.end(), kind));
    }

private:
    std::shared_ptr<neomcp::InProcessServer> inner_;
    mutable std::mutex mutex_;
    std::vector<neomcp::OutgoingKind> kinds_;
    std::vector<std::string> methods_;
};

// A feed server, a recording in-process peer of it, and a client of one revision.
class FeedPair {
public:
    FeedPair(std::string_view revision, Transport transport, neomcp::ClientOptions options = {}) {
        options.profiles = {neomcp::profile::builtin(revision)};
        options.client_info.name = "feed-test";
        options.client_info.version = "1.0.0";
        process = neomcp::InProcessServer::create(fixture.server, transport);
        recorder = std::make_shared<RecordingPeer>(process);
        auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(transport, recorder));
        if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
        client.emplace(std::move(opened).value());
    }

    FeedServer fixture;
    std::shared_ptr<neomcp::InProcessServer> process;
    std::shared_ptr<RecordingPeer> recorder;
    std::optional<neomcp::Client> client;
};

struct Cell {
    std::string revision;
    Transport transport;
};

std::string cell_name(const testing::TestParamInfo<Cell>& info) {
    std::string name = info.param.revision + (info.param.transport == Transport::stdio ? "_stdio" : "_http");
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

std::vector<Cell> cells() {
    std::vector<Cell> all;
    for (const auto revision : neomcp::profile::builtin_revisions()) {
        const auto profile = neomcp::profile::builtin(revision);
        if (profile->stdio()) all.push_back(Cell{std::string(revision), Transport::stdio});
        if (profile->streamable_http()) all.push_back(Cell{std::string(revision), Transport::streamable_http});
    }
    return all;
}

class ClientFeedEveryRevision : public testing::TestWithParam<Cell> {};

TEST_P(ClientFeedEveryRevision, HearsOfEachListAndOfAWatchedResource) {
    const Cell cell = GetParam();
    FeedPair pair(cell.revision, cell.transport);
    auto subscription =
        pair.client->listen(every_kind({std::string(watched_uri), std::string(missing_uri), std::string(watched_uri)}));
    ASSERT_TRUE(subscription) << subscription.error().message;
    EXPECT_EQ(subscription->delivery(), delivery_of(cell.revision));

    // Asked: each resource once. Honoured: every kind, and only the resource the server has.
    EXPECT_EQ(subscription->asked().resources,
              (std::vector<std::string>{std::string(watched_uri), std::string(missing_uri)}));
    const ChangeFilter& honoured = subscription->honoured();
    EXPECT_TRUE(honoured.tools_list);
    EXPECT_TRUE(honoured.prompts_list);
    EXPECT_TRUE(honoured.resources_list);
    EXPECT_EQ(honoured.resources, std::vector<std::string>{std::string(watched_uri)});

    pair.fixture.beta->enable();
    EXPECT_TRUE(same(next_change(*subscription), list_change(ChangeKind::tools_list)));
    pair.fixture.prompt->enable(false);
    EXPECT_TRUE(same(next_change(*subscription), list_change(ChangeKind::prompts_list)));
    pair.fixture.other->enable(false);
    EXPECT_TRUE(same(next_change(*subscription), list_change(ChangeKind::resources_list)));
    pair.fixture.server.notify_resource_updated(std::string(watched_uri));
    EXPECT_TRUE(same(next_change(*subscription), update(watched_uri)));

    if (subscription->delivery() == FeedDelivery::subscription) {
        EXPECT_EQ(pair.recorder->count("subscriptions/listen"), 1u);
    } else {
        // Each resource the filter names is watched once, the missing one too (the server refuses it).
        EXPECT_EQ(pair.recorder->count("resources/subscribe"), 2u);
    }
    EXPECT_TRUE(subscription->close());
    const auto after = subscription->next();
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().kind, ClientErrorKind::closed);
}

TEST_P(ClientFeedEveryRevision, ClosingTellsTheServer) {
    const Cell cell = GetParam();
    FeedPair pair(cell.revision, cell.transport);
    auto subscription = pair.client->listen(every_kind({std::string(watched_uri)}));
    ASSERT_TRUE(subscription) << subscription.error().message;
    const std::size_t cancels_before = pair.recorder->count("notifications/cancelled");
    const std::size_t ends_before = pair.recorder->count_kind(neomcp::OutgoingKind::end_unit);
    EXPECT_TRUE(subscription->close());
    if (subscription->delivery() == FeedDelivery::connection) {
        EXPECT_EQ(pair.recorder->count("resources/unsubscribe"), 1u);
    } else if (cell.transport == Transport::stdio) {
        EXPECT_EQ(pair.recorder->count("notifications/cancelled"), cancels_before + 1);
    } else {
        // The revision's HTTP cancellation ends the subscription's stream.
        EXPECT_GE(pair.recorder->count_kind(neomcp::OutgoingKind::end_unit), ends_before + 1);
    }
    // The client goes on serving its application.
    const auto listed = pair.client->list_tools();
    EXPECT_TRUE(listed) << listed.error().message;
}

TEST_P(ClientFeedEveryRevision, TwoSubscriptionsShareOneWatchOfAResource) {
    const Cell cell = GetParam();
    if (delivery_of(cell.revision) != FeedDelivery::connection) GTEST_SKIP() << "the revision watches nothing";
    FeedPair pair(cell.revision, cell.transport);
    ChangeFilter filter;
    filter.resources = {std::string(watched_uri)};
    auto first = pair.client->listen(filter);
    ASSERT_TRUE(first) << first.error().message;
    auto second = pair.client->listen(filter);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second->honoured().resources, filter.resources);
    EXPECT_EQ(pair.recorder->count("resources/subscribe"), 1u);

    // The first lets go: the resource is still watched for the second.
    EXPECT_TRUE(first->close());
    EXPECT_EQ(pair.recorder->count("resources/unsubscribe"), 0u);
    pair.fixture.server.notify_resource_updated(std::string(watched_uri));
    EXPECT_TRUE(same(next_change(*second), update(watched_uri)));

    EXPECT_TRUE(second->close());
    EXPECT_EQ(pair.recorder->count("resources/unsubscribe"), 1u);
}

TEST_P(ClientFeedEveryRevision, ASubscriptionOutlivesARestartOfTheServerProcess) {
    const Cell cell = GetParam();
    if (cell.transport != Transport::stdio) GTEST_SKIP() << "only a child process is started again";
    FeedPair pair(cell.revision, cell.transport);
    auto subscription = pair.client->listen(every_kind({std::string(watched_uri)}));
    ASSERT_TRUE(subscription) << subscription.error().message;
    const bool on_connection = subscription->delivery() == FeedDelivery::connection;

    pair.process->end_process(0);
    ASSERT_TRUE(eventually([&] { return pair.process->processes() == 2; }));
    // What changed while no process ran is not known: every honoured kind and resource is reported once.
    std::vector<Change> gap;
    for (int i = 0; i < 4; ++i) gap.push_back(next_change(*subscription));
    std::vector<std::string> seen;
    for (const auto& change : gap) seen.push_back(describe(change));
    EXPECT_EQ(seen, (std::vector<std::string>{"tools_list", "prompts_list", "resources_list",
                                              "resource_updated " + std::string(watched_uri)}));

    // Opened again with the new process: watched again, or listened to again.
    if (on_connection) {
        ASSERT_TRUE(eventually([&] { return pair.recorder->count("resources/subscribe") == 2; }));
    } else {
        ASSERT_TRUE(eventually([&] { return pair.recorder->count("subscriptions/listen") == 2; }));
    }
    // A request after those makes sure the new process has taken them.
    ASSERT_TRUE(pair.client->list_tools());
    pair.fixture.beta->enable();
    EXPECT_TRUE(same(next_change(*subscription), list_change(ChangeKind::tools_list)));
    pair.fixture.server.notify_resource_updated(std::string(watched_uri));
    EXPECT_TRUE(same(next_change(*subscription), update(watched_uri)));
}

INSTANTIATE_TEST_SUITE_P(ClientFeed, ClientFeedEveryRevision, testing::ValuesIn(cells()), cell_name);

TEST(ClientFeed, OneSubscriptionMoreThanTheBoundFailsAtOnce) {
    for (const auto revision : {modern, legacy}) {
        neomcp::ClientOptions options;
        options.max_subscriptions = 2;
        FeedPair pair(revision, Transport::stdio, options);
        auto first = pair.client->listen(tools_only());
        auto second = pair.client->listen(tools_only());
        ASSERT_TRUE(first && second) << revision;
        const std::size_t written = pair.recorder->count("subscriptions/listen");
        auto third = pair.client->listen(tools_only());
        ASSERT_FALSE(third) << revision;
        EXPECT_EQ(third.error().kind, ClientErrorKind::too_many_subscriptions) << revision;
        EXPECT_EQ(pair.recorder->count("subscriptions/listen"), written) << revision;
        EXPECT_TRUE(first->close());
        auto again = pair.client->listen(tools_only());
        EXPECT_TRUE(again) << revision;
    }
}

TEST(ClientFeed, AClosedClientEndsItsSubscriptions) {
    FeedPair pair(modern, Transport::stdio);
    auto subscription = pair.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    EXPECT_TRUE(pair.client->close());
    const auto after = subscription->next();
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().kind, ClientErrorKind::closed);
    auto refused = pair.client->listen(tools_only());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::closed);
}

// ---------------------------------------------------------------------------------------------------------------
// Scripted peers

json full_capabilities() {
    return json::parse(
        R"({"tools":{"listChanged":true},"prompts":{"listChanged":true},"resources":{"listChanged":true,"subscribe":true}})");
}

json handshake_result(std::string_view revision) {
    json result = initialize_result(revision);
    result["capabilities"] = full_capabilities();
    return result;
}

json meta_of(const json& subscription) {
    json meta = json::object();
    meta[std::string(subscription_key)] = subscription;
    return meta;
}

json notification(std::string method, json params) {
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["method"] = std::move(method);
    message["params"] = std::move(params);
    return message;
}

json acknowledgement(const json& subscription, json filter) {
    json params = json::object();
    params["notifications"] = std::move(filter);
    params["_meta"] = meta_of(subscription);
    return notification("notifications/subscriptions/acknowledged", std::move(params));
}

// A change notification; tagged with the subscription when one is given.
json change_note(std::string_view method, std::optional<json> subscription = std::nullopt,
                 std::optional<std::string> uri = std::nullopt) {
    json params = json::object();
    if (uri) params["uri"] = *uri;
    if (subscription) params["_meta"] = meta_of(*subscription);
    return notification(std::string(method), std::move(params));
}

json tools_changed(std::optional<json> subscription = std::nullopt) {
    return change_note("notifications/tools/list_changed", std::move(subscription));
}
json prompts_changed(std::optional<json> subscription = std::nullopt) {
    return change_note("notifications/prompts/list_changed", std::move(subscription));
}
json resources_changed(std::optional<json> subscription = std::nullopt) {
    return change_note("notifications/resources/list_changed", std::move(subscription));
}
json updated(std::string_view uri, std::optional<json> subscription = std::nullopt) {
    return change_note("notifications/resources/updated", std::move(subscription), std::string(uri));
}

// What a scripted server does: opens contact, acknowledges the listens it is told to, answers the watches, and runs
// the test's hook before it answers a list of tools (the barrier a test sends its changes before).
struct FeedScript {
    std::string revision;
    std::atomic<int> acknowledged_listens{std::numeric_limits<int>::max()};
    std::function<void(ScriptedPeer&, const Seen&)> before_list;

    std::vector<Seen> listens() const {
        std::lock_guard lock(mutex);
        return seen_listens;
    }
    json last_listen_id() const {
        std::lock_guard lock(mutex);
        return seen_listens.back().message["id"];
    }

    void on_request(ScriptedPeer& peer, const Seen& request) {
        const std::string method = request.method();
        if (method == "initialize") {
            peer.answer(request, handshake_result(revision));
        } else if (method == "server/discover") {
            json result = discover_result(revision);
            result["capabilities"] = full_capabilities();
            peer.answer(request, result);
        } else if (method == "subscriptions/listen") {
            {
                std::lock_guard lock(mutex);
                seen_listens.push_back(request);
            }
            if (acknowledged_listens.fetch_sub(1) > 0) {
                peer.send(request.unit,
                          neomcp::serialize(acknowledgement(request.message["id"],
                                                            request.message["params"]["notifications"])));
            } else if (peer.http) {
                peer.event(request.unit, neomcp::StreamEnded{});  // the stream ends without anything
            }
        } else if (method == "resources/subscribe" || method == "resources/unsubscribe" || method == "ping") {
            peer.answer(request, json::object());
        } else if (method == "tools/list") {
            if (before_list) before_list(peer, request);
            json result = json::object();
            result["tools"] = json::array();
            // The caching hints a revision's list result requires, from its wire tables.
            const auto* type = neomcp::profile::builtin(revision)->type("ListToolsResult");
            if (type != nullptr && type->has_property("ttlMs")) {
                result["ttlMs"] = 0;
                result["cacheScope"] = "private";
                result["resultType"] = "complete";
            }
            peer.answer(request, result);
        }
    }

    mutable std::mutex mutex;
    std::vector<Seen> seen_listens;
};

struct Scripted {
    Scripted(std::string_view revision, Transport transport, neomcp::ClientOptions options = {})
        : script(std::make_shared<FeedScript>()) {
        script->revision = std::string(revision);
        peer = std::make_shared<ScriptedPeer>(
            [shared = script](ScriptedPeer& one, const Seen& request) { shared->on_request(one, request); });
        peer->http = transport == Transport::streamable_http;
        options.profiles = {neomcp::profile::builtin(revision)};
        options.client_info.name = "feed-test";
        options.client_info.version = "1.0.0";
        auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(transport, peer));
        if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
        client.emplace(std::move(opened).value());
    }

    // Sends `messages` before the answer of a list of tools, and waits for that answer: once it is back, the client
    // has read every one of them.
    void burst(std::vector<json> messages) {
        script->before_list = [messages = std::move(messages)](ScriptedPeer& one, const Seen& request) {
            for (const auto& message : messages) one.send(request.unit, neomcp::serialize(message));
        };
        const auto listed = client->list_tools();
        if (!listed) throw std::runtime_error("the barrier failed: " + listed.error().message);
        script->before_list = nullptr;
    }

    std::shared_ptr<FeedScript> script;
    std::shared_ptr<ScriptedPeer> peer;
    std::optional<neomcp::Client> client;
};

TEST(ClientFeed, ASlowReaderHoldsOneChangePerKindAndResource) {
    for (const auto revision : {modern, legacy}) {
        Scripted scripted(revision, Transport::stdio);
        auto subscription = scripted.client->listen(every_kind({"res://a", "res://b"}));
        ASSERT_TRUE(subscription) << subscription.error().message;
        const bool tagged = subscription->delivery() == FeedDelivery::subscription;
        const std::optional<json> tag =
            tagged ? std::optional<json>(scripted.script->last_listen_id()) : std::nullopt;

        // Many changes while the application reads none.
        std::vector<json> burst;
        for (int i = 0; i < 50; ++i) {
            burst.push_back(tools_changed(tag));
            burst.push_back(updated("res://a", tag));
            burst.push_back(updated("res://b", tag));
            burst.push_back(updated("res://a", tag));
        }
        scripted.burst(std::move(burst));
        std::vector<std::string> taken;
        for (int i = 0; i < 3; ++i) taken.push_back(describe(next_change(*subscription)));
        EXPECT_EQ(taken, (std::vector<std::string>{"tools_list", "resource_updated res://a", "resource_updated res://b"}))
            << revision;

        // Nothing else was held: the next change is a new one.
        scripted.burst({prompts_changed(tag)});
        EXPECT_EQ(describe(next_change(*subscription)), "prompts_list") << revision;
    }
}

TEST(ClientFeed, ChangesOfOneArrayAreNeverDroppedFromTheEnginesQueue) {
    // One queued event at most: the four changes of one array still reach the subscription.
    neomcp::ClientOptions options;
    options.max_queued_events = 1;
    Scripted scripted(reads_arrays, Transport::stdio, options);
    auto subscription = scripted.client->listen(every_kind({"res://a"}));
    ASSERT_TRUE(subscription) << subscription.error().message;
    json array = json::array();
    array.push_back(tools_changed());
    array.push_back(prompts_changed());
    array.push_back(resources_changed());
    array.push_back(updated("res://a"));
    scripted.burst({array});
    std::vector<std::string> taken;
    for (int i = 0; i < 4; ++i) taken.push_back(describe(next_change(*subscription)));
    EXPECT_EQ(taken, (std::vector<std::string>{"tools_list", "prompts_list", "resources_list",
                                               "resource_updated res://a"}));
}

TEST(ClientFeed, AChangeNobodyAskedForIsNotDelivered) {
    for (const auto revision : {modern, legacy}) {
        Scripted scripted(revision, Transport::stdio);
        auto subscription = scripted.client->listen(tools_only());
        ASSERT_TRUE(subscription) << subscription.error().message;
        const bool tagged = subscription->delivery() == FeedDelivery::subscription;
        const std::optional<json> tag =
            tagged ? std::optional<json>(scripted.script->last_listen_id()) : std::nullopt;
        scripted.burst({prompts_changed(tag), updated("res://a", tag), tools_changed(tag)});
        EXPECT_EQ(describe(next_change(*subscription)), "tools_list") << revision;
        EXPECT_FALSE(next_within(*subscription, 50ms)) << revision;
    }
}

TEST(ClientFeed, TheConnectionStreamIsAskedForOnceWhenAListenerNeedsIt) {
    Scripted scripted(legacy, Transport::streamable_http);
    EXPECT_EQ(scripted.peer->count_kind(neomcp::OutgoingKind::open_connection_stream), 0u);
    auto first = scripted.client->listen(tools_only());
    ASSERT_TRUE(first) << first.error().message;
    auto second = scripted.client->listen(tools_only());
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(scripted.peer->count_kind(neomcp::OutgoingKind::open_connection_stream), 1u);
    // Its changes come on that stream.
    scripted.peer->send(neomcp::connection_unit, neomcp::serialize(tools_changed()));
    EXPECT_EQ(describe(next_change(*first)), "tools_list");
    EXPECT_EQ(describe(next_change(*second)), "tools_list");
}

TEST(ClientFeed, AResourceTheServerRefusesToWatchIsNotHonoured) {
    // The scripted server refuses the second resource.
    auto script = std::make_shared<FeedScript>();
    script->revision = std::string(legacy);
    auto peer = std::make_shared<ScriptedPeer>([script](ScriptedPeer& one, const Seen& request) {
        if (request.method() == "resources/subscribe" && request.message["params"]["uri"] == "res://refused") {
            one.refuse(request, -32602, "no such resource");
            return;
        }
        script->on_request(one, request);
    });
    neomcp::ClientOptions options = only({legacy});
    auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(Transport::stdio, peer));
    ASSERT_TRUE(opened) << opened.error().message;
    auto subscription = opened->listen(every_kind({"res://kept", "res://refused"}));
    ASSERT_TRUE(subscription) << subscription.error().message;
    EXPECT_EQ(subscription->honoured().resources, std::vector<std::string>{"res://kept"});
    EXPECT_TRUE(subscription->close());
    // Only what was watched is let go.
    EXPECT_EQ(peer->count("resources/unsubscribe"), 1u);
    const auto written = peer->written();
    const auto unwatch = std::find_if(written.begin(), written.end(),
                                      [](const Seen& one) { return one.method() == "resources/unsubscribe"; });
    ASSERT_NE(unwatch, written.end());
    EXPECT_EQ(unwatch->message["params"]["uri"], "res://kept");
}

TEST(ClientFeed, AListenStoppedBeforeItsWatchesLetsGoOfNoOtherSubscriptionsResource) {
    // The scripted server never answers the watch of res://slow.
    auto script = std::make_shared<FeedScript>();
    script->revision = std::string(legacy);
    auto peer = std::make_shared<ScriptedPeer>([script](ScriptedPeer& one, const Seen& request) {
        if (request.method() == "resources/subscribe" && request.message["params"]["uri"] == "res://slow") return;
        script->on_request(one, request);
    });
    auto opened = neomcp::Client::open(only({legacy}), neomcp::make_memory_transport(Transport::stdio, peer));
    ASSERT_TRUE(opened) << opened.error().message;
    ChangeFilter shared_resource;
    shared_resource.resources = {"res://shared"};
    auto first = opened->listen(shared_resource);
    ASSERT_TRUE(first) << first.error().message;

    // The second names the slow resource before the shared one, and is stopped while the slow watch waits.
    ChangeFilter both;
    both.resources = {"res://slow", "res://shared"};
    std::stop_source stop;
    neomcp::RequestOptions options;
    options.stop = stop.get_token();
    std::optional<neomcp::Result<neomcp::Subscription>> second;
    std::thread listening([&] { second.emplace(opened->listen(both, options)); });
    const OnExit joined([&] {
        stop.request_stop();
        join(listening);
    });
    ASSERT_TRUE(eventually([&] { return peer->count("resources/subscribe") == 2; }));
    stop.request_stop();
    join(listening);
    ASSERT_TRUE(second);
    ASSERT_FALSE(*second);
    EXPECT_EQ(second->error().kind, ClientErrorKind::cancelled);

    // The shared resource is still watched for the first, and nothing let it go.
    ASSERT_TRUE(opened->list_tools());
    for (const auto& one : peer->written()) {
        if (one.method() == "resources/unsubscribe") {
            EXPECT_NE(one.message["params"]["uri"], "res://shared");
        }
    }
    peer->send(neomcp::connection_unit, neomcp::serialize(updated("res://shared")));
    EXPECT_EQ(describe(next_change(*first)), "resource_updated res://shared");
    EXPECT_TRUE(first->close());
    EXPECT_EQ(peer->count("resources/unsubscribe"), 1u);
}

// ---- Losses and reopens

using Reopen = neomcp::SubscriptionReopenDelay;

std::optional<ManualTimers::Started> live_reopen(ManualTimers& timers, std::size_t attempt) {
    return timers.live([attempt](const ManualTimers::Started& one) {
        const auto* reopen = std::get_if<Reopen>(&one.purpose);
        return reopen != nullptr && reopen->attempt == attempt;
    });
}

TEST(ClientFeed, AStreamThatEndsWithoutItsResultIsReopenedAfterAJitteredWait) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    Scripted scripted(modern, Transport::streamable_http, options);
    auto subscription = scripted.client->listen(every_kind({"res://a"}));
    ASSERT_TRUE(subscription) << subscription.error().message;
    ASSERT_EQ(scripted.script->listens().size(), 1u);

    for (int loss = 1; loss <= 2; ++loss) {
        const Seen listen = scripted.script->listens().back();
        scripted.peer->event(listen.unit, neomcp::StreamEnded{});
        // An acknowledgement came before each loss: every loss is the first in a row.
        const auto timer = live_reopen(*timers, 1);
        ASSERT_TRUE(timer) << loss;
        const auto& reopen = std::get<Reopen>(timer->purpose);
        EXPECT_EQ(reopen.bound, 1000ms) << loss;
        EXPECT_LE(timer->after, reopen.bound) << loss;
        EXPECT_GE(timer->after, 0ms) << loss;
        EXPECT_EQ(scripted.script->listens().size(), static_cast<std::size_t>(loss)) << "nothing before the wait";
        ASSERT_TRUE(timers->fire(timer->id));
        ASSERT_TRUE(eventually([&] { return scripted.script->listens().size() == static_cast<std::size_t>(loss) + 1; }));
        // What changed during the gap is not known: everything honoured is reported once.
        std::vector<std::string> gap;
        for (int i = 0; i < 4; ++i) gap.push_back(describe(next_change(*subscription)));
        EXPECT_EQ(gap, (std::vector<std::string>{"tools_list", "prompts_list", "resources_list",
                                                 "resource_updated res://a"}))
            << loss;
    }
    // The reopened subscription delivers on the new request's stream, tagged with the new request.
    const Seen latest = scripted.script->listens().back();
    scripted.peer->send(latest.unit, neomcp::serialize(tools_changed(latest.message["id"])));
    EXPECT_EQ(describe(next_change(*subscription)), "tools_list");
}

TEST(ClientFeed, ReopensInARowEndWithSubscriptionLost) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    Scripted scripted(modern, Transport::streamable_http, options);
    scripted.script->acknowledged_listens = 1;  // only the first listen is acknowledged; later streams just end
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    scripted.peer->event(scripted.script->listens().back().unit, neomcp::StreamEnded{});

    std::vector<std::chrono::milliseconds> bounds;
    for (std::size_t attempt = 1; attempt <= neomcp::ClientDefaults::max_subscription_reopens; ++attempt) {
        const auto timer = live_reopen(*timers, attempt);
        ASSERT_TRUE(timer) << attempt;
        const auto& reopen = std::get<Reopen>(timer->purpose);
        bounds.push_back(reopen.bound);
        EXPECT_LE(timer->after, reopen.bound) << attempt;
        ASSERT_TRUE(timers->fire(timer->id));
        ASSERT_TRUE(eventually([&] { return scripted.script->listens().size() == attempt + 1; })) << attempt;
    }
    EXPECT_EQ(bounds, (std::vector<std::chrono::milliseconds>{1000ms, 2000ms, 4000ms, 8000ms, 16000ms}));
    const auto ended = next_within(*subscription);
    ASSERT_TRUE(ended);
    ASSERT_FALSE(*ended);
    EXPECT_EQ(ended->error().kind, ClientErrorKind::subscription_lost);
    // No reopen beyond the bound.
    EXPECT_EQ(scripted.script->listens().size(), neomcp::ClientDefaults::max_subscription_reopens + 1);
    EXPECT_EQ(timers->started_for<Reopen>().size(), neomcp::ClientDefaults::max_subscription_reopens);
}

TEST(ClientFeed, ZeroReopensEndsTheSubscriptionAtTheFirstLoss) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    options.max_subscription_reopens = 0;
    Scripted scripted(modern, Transport::streamable_http, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    scripted.peer->event(scripted.script->listens().back().unit, neomcp::StreamEnded{});
    const auto ended = next_within(*subscription);
    ASSERT_TRUE(ended);
    ASSERT_FALSE(*ended);
    EXPECT_EQ(ended->error().kind, ClientErrorKind::subscription_lost);
    EXPECT_TRUE(timers->started_for<Reopen>().empty());
}

TEST(ClientFeed, ClosingCancelsAReopenThatIsDue) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    Scripted scripted(modern, Transport::streamable_http, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    scripted.peer->event(scripted.script->listens().back().unit, neomcp::StreamEnded{});
    const auto timer = live_reopen(*timers, 1);
    ASSERT_TRUE(timer);
    EXPECT_TRUE(subscription->close());
    // The timer is gone, and firing it anyway sends nothing.
    const auto started = timers->started_for<Reopen>();
    ASSERT_EQ(started.size(), 1u);
    EXPECT_TRUE(started[0].cancelled);
    timers->fire_all();
    ASSERT_TRUE(scripted.client->list_tools());
    EXPECT_EQ(scripted.script->listens().size(), 1u);
}

TEST(ClientFeed, ClosingTheClientCancelsAReopenThatIsDue) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    Scripted scripted(modern, Transport::streamable_http, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    scripted.peer->event(scripted.script->listens().back().unit, neomcp::StreamEnded{});
    ASSERT_TRUE(live_reopen(*timers, 1));
    EXPECT_TRUE(scripted.client->close());
    const auto started = timers->started_for<Reopen>();
    ASSERT_EQ(started.size(), 1u);
    EXPECT_TRUE(started[0].cancelled);
    const auto after = subscription->next();
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().kind, ClientErrorKind::closed);
}

TEST(ClientFeed, AServerProcessThatEndsTakesTheSubscriptionAlongAndItIsOpenedAgainAtOnce) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    options.max_subscription_reopens = 0;  // a restart is no reopen of a lost stream
    Scripted scripted(modern, Transport::stdio, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    for (int exit = 1; exit <= 3; ++exit) {
        scripted.peer->event(neomcp::connection_unit, neomcp::ProcessExited{exit});
        ASSERT_TRUE(eventually([&] { return scripted.script->listens().size() == static_cast<std::size_t>(exit) + 1; }))
            << exit;
        EXPECT_EQ(describe(next_change(*subscription)), "tools_list") << exit;  // the gap
    }
    EXPECT_EQ(scripted.peer->restarts(), 3u);
    EXPECT_TRUE(timers->started_for<Reopen>().empty());
}

TEST(ClientFeed, AResultThatEndsTheSubscriptionIsNotReopened) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    Scripted scripted(modern, Transport::streamable_http, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    const Seen listen = scripted.script->listens().back();
    scripted.peer->send(listen.unit, neomcp::serialize(tools_changed(listen.message["id"])));
    json result = json::object();
    result["resultType"] = "complete";
    result["_meta"] = meta_of(listen.message["id"]);
    scripted.peer->answer(listen, result);
    // What came before the end is still delivered, then the end.
    EXPECT_EQ(describe(next_change(*subscription)), "tools_list");
    const auto ended = next_within(*subscription);
    ASSERT_TRUE(ended);
    ASSERT_FALSE(*ended);
    EXPECT_EQ(ended->error().kind, ClientErrorKind::subscription_ended);
    EXPECT_TRUE(timers->started_for<Reopen>().empty());
    EXPECT_EQ(scripted.script->listens().size(), 1u);
}

TEST(ClientFeed, AnEndingNoticeEndsTheSubscriptionAndTheResultAfterItIsExpected) {
    std::mutex mutex;
    std::vector<neomcp::DiagnosticKind> diagnostics;
    neomcp::ClientOptions options;
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (const auto* engine = std::get_if<neomcp::Diagnostic>(&diagnostic)) {
            std::lock_guard lock(mutex);
            diagnostics.push_back(engine->kind);
        }
    };
    Scripted scripted(modern, Transport::stdio, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    const Seen listen = scripted.script->listens().back();
    json params = json::object();
    params["requestId"] = listen.message["id"];
    scripted.peer->send(neomcp::connection_unit,
                        neomcp::serialize(notification("notifications/cancelled", std::move(params))));
    json result = json::object();
    result["resultType"] = "complete";
    result["_meta"] = meta_of(listen.message["id"]);
    scripted.peer->send(neomcp::connection_unit,
                        neomcp::serialize(neomcp::make_result(neomcp::RequestId(listen.message["id"].get<std::uint64_t>()),
                                                              result)));
    const auto ended = next_within(*subscription);
    ASSERT_TRUE(ended);
    ASSERT_FALSE(*ended);
    EXPECT_EQ(ended->error().kind, ClientErrorKind::subscription_ended);
    ASSERT_TRUE(scripted.client->list_tools());  // the result has been read by now
    std::lock_guard lock(mutex);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(ClientFeed, ANotificationOfASubscriptionNobodyOpenedIsReported) {
    std::atomic<int> unknown{0};
    neomcp::ClientOptions options;
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (const auto* engine = std::get_if<neomcp::Diagnostic>(&diagnostic);
            engine != nullptr && engine->kind == neomcp::DiagnosticKind::unknown_id) {
            ++unknown;
        }
    };
    Scripted scripted(modern, Transport::stdio, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    scripted.burst({tools_changed(json(999)), tools_changed(json("not-a-listen"))});
    EXPECT_EQ(unknown.load(), 2);
    EXPECT_FALSE(next_within(*subscription, 50ms));
}

TEST(ClientFeed, AListenTheServerRefusesFails) {
    auto script = std::make_shared<FeedScript>();
    script->revision = std::string(modern);
    auto peer = std::make_shared<ScriptedPeer>([script](ScriptedPeer& one, const Seen& request) {
        if (request.method() == "subscriptions/listen") {
            one.refuse(request, -32602, "no subscriptions here");
            return;
        }
        script->on_request(one, request);
    });
    auto opened = neomcp::Client::open(only({modern}), neomcp::make_memory_transport(Transport::stdio, peer));
    ASSERT_TRUE(opened) << opened.error().message;
    auto subscription = opened->listen(tools_only());
    ASSERT_FALSE(subscription);
    EXPECT_EQ(subscription.error().kind, ClientErrorKind::server_error);
    EXPECT_EQ(subscription.error().code, -32602);
}

TEST(ClientFeed, AnAcknowledgementThatNeverComesIsBoundedByTheTimeout) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    Scripted scripted(modern, Transport::stdio, options);
    scripted.script->acknowledged_listens = 0;
    std::optional<neomcp::Result<neomcp::Subscription>> result;
    std::thread listening([&] { result.emplace(scripted.client->listen(tools_only())); });
    const OnExit joined([&] {
        timers->fire_all();
        join(listening);
    });
    ASSERT_TRUE(eventually([&] { return scripted.script->listens().size() == 1; }));
    const auto clock = timers->live([](const ManualTimers::Started& one) {
        const auto* deadline = std::get_if<neomcp::Deadline>(&one.purpose);
        return deadline != nullptr && deadline->kind == neomcp::DeadlineKind::wait;
    });
    ASSERT_TRUE(clock);
    ASSERT_TRUE(timers->fire(clock->id));
    join(listening);
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().kind, ClientErrorKind::timed_out);
    // Cancelled as the transport cancels a request.
    EXPECT_TRUE(eventually([&] { return scripted.peer->count("notifications/cancelled") == 1; }));
}

TEST(ClientFeed, AnAcknowledgedSubscriptionHasNoClock) {
    auto timers = std::make_shared<ManualTimers>();
    neomcp::ClientOptions options;
    options.timers = timers;
    Scripted scripted(modern, Transport::stdio, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    // Every clock the listen started is gone: none of them can end it.
    for (const auto& one : timers->started()) {
        if (std::holds_alternative<neomcp::Deadline>(one.purpose)) {
            EXPECT_TRUE(one.cancelled);
        }
    }
    timers->fire_all();
    scripted.burst({tools_changed(scripted.script->last_listen_id())});
    EXPECT_EQ(describe(next_change(*subscription)), "tools_list");
}

// ---- Threads

TEST(ClientFeed, ABlockingNextOnTheClientsThreadReturnsTheTypedError) {
    std::atomic<neomcp::Subscription*> shared{nullptr};
    std::atomic<int> refused{-1};
    neomcp::ClientOptions options;
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        neomcp::Subscription* subscription = shared.load();
        if (subscription == nullptr || !std::holds_alternative<neomcp::Diagnostic>(diagnostic)) return;
        const auto next = subscription->next();
        const auto closed = subscription->close();
        refused = !next && next.error().kind == ClientErrorKind::on_client_thread && !closed &&
                          closed.error().kind == ClientErrorKind::on_client_thread
                      ? 1
                      : 0;
    };
    Scripted scripted(modern, Transport::stdio, options);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    shared = &subscription.value();
    // An answer to no request makes a diagnostic, whose callback runs on the client's thread.
    scripted.peer->send(neomcp::connection_unit, R"({"jsonrpc":"2.0","id":4242,"result":{}})");
    ASSERT_TRUE(eventually([&] { return refused.load() != -1; }));
    shared = nullptr;
    EXPECT_EQ(refused.load(), 1);
}

TEST(ClientFeed, AsyncNextCompletesOnTheAwaitersExecutor) {
    Scripted scripted(modern, Transport::stdio);
    auto subscription = scripted.client->listen(every_kind());
    ASSERT_TRUE(subscription) << subscription.error().message;

    asio::io_context second;
    auto guard = asio::make_work_guard(second);
    std::thread running([&] { second.run(); });
    asio::thread_pool pool(2);
    const OnExit stopped([&] {
        guard.reset();
        second.stop();
        join(running);
        pool.join();
    });

    std::atomic<int> on_second{-1};
    std::atomic<int> on_pool{-1};
    std::atomic<int> changes{0};
    asio::co_spawn(
        second,
        [&]() -> asio::awaitable<void> {
            auto got = co_await subscription->async_next();
            if (got) ++changes;
            on_second = second.get_executor().running_in_this_thread() ? 1 : 0;
        },
        asio::detached);
    asio::co_spawn(
        pool,
        [&]() -> asio::awaitable<void> {
            auto got = co_await subscription->async_next();
            if (got) ++changes;
            on_pool = pool.get_executor().running_in_this_thread() ? 1 : 0;
        },
        asio::detached);
    const json tag = scripted.script->last_listen_id();
    scripted.burst({tools_changed(tag), prompts_changed(tag)});
    ASSERT_TRUE(eventually([&] { return on_second.load() != -1 && on_pool.load() != -1; }));
    EXPECT_EQ(on_second.load(), 1);
    EXPECT_EQ(on_pool.load(), 1);
    EXPECT_EQ(changes.load(), 2);
}

TEST(ClientFeed, CancellingTheAwaiterOfAsyncNextTakesNoChange) {
    Scripted scripted(modern, Transport::stdio);
    auto subscription = scripted.client->listen(tools_only());
    ASSERT_TRUE(subscription) << subscription.error().message;
    // Waits that give up (next_within races a timer) take nothing.
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(next_within(*subscription, 5ms));
    }
    scripted.burst({tools_changed(scripted.script->last_listen_id())});
    EXPECT_EQ(describe(next_change(*subscription)), "tools_list");
}

TEST(ClientFeed, ADestroyedSubscriptionIsClosed) {
    Scripted scripted(modern, Transport::stdio);
    {
        auto subscription = scripted.client->listen(tools_only());
        ASSERT_TRUE(subscription) << subscription.error().message;
    }
    EXPECT_TRUE(eventually([&] { return scripted.peer->count("notifications/cancelled") == 1; }));
}

// ---- The waits before a reopen

TEST(ClientFeedBackoff, TheBoundDoublesFromTheFirstDelayUpToTheMost) {
    using neomcp::detail::subscription_reopen_bound;
    std::vector<std::chrono::milliseconds> bounds;
    for (std::size_t attempt = 1; attempt <= 8; ++attempt) bounds.push_back(subscription_reopen_bound(attempt, 1s, 60s));
    EXPECT_EQ(bounds, (std::vector<std::chrono::milliseconds>{1000ms, 2000ms, 4000ms, 8000ms, 16000ms, 32000ms,
                                                              60000ms, 60000ms}));
    EXPECT_EQ(subscription_reopen_bound(1000, 1s, 60s), 60000ms);
    EXPECT_EQ(subscription_reopen_bound(std::numeric_limits<std::size_t>::max(), 1s, 60s), 60000ms);
    EXPECT_EQ(subscription_reopen_bound(1, 5s, 3s), 3000ms);
}

TEST(ClientFeedBackoff, TheWaitIsDrawnFromZeroToTheBound) {
    using neomcp::detail::subscription_reopen_delay;
    EXPECT_EQ(subscription_reopen_delay(1000ms, 0), 0ms);
    EXPECT_EQ(subscription_reopen_delay(1000ms, 1000), 1000ms);
    EXPECT_EQ(subscription_reopen_delay(1000ms, 1001), 0ms);
    EXPECT_EQ(subscription_reopen_delay(1000ms, 1500), 499ms);
    EXPECT_LE(subscription_reopen_delay(1000ms, std::numeric_limits<std::uint64_t>::max()), 1000ms);
    EXPECT_EQ(subscription_reopen_delay(0ms, 12345), 0ms);
}

TEST(ClientFeedBackoff, OptionsThatCannotWorkAreRefused) {
    neomcp::ClientOptions options = only({modern});
    options.max_subscriptions = 0;
    options.subscription_reopen_first_delay = 2s;
    options.subscription_reopen_max_delay = 1s;
    auto peer = std::make_shared<ScriptedPeer>(nullptr);
    auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(Transport::stdio, peer));
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().kind, ClientErrorKind::invalid_options);
    EXPECT_NE(opened.error().message.find("max_subscriptions"), std::string::npos);
    EXPECT_NE(opened.error().message.find("subscription_reopen_first_delay"), std::string::npos);
}

}  // namespace
