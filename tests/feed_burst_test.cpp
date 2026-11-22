// Tests of the change feed in the session engine under a burst of changes while the writer is held: on every built-in
// revision, what a session queues for its connection (resources/subscribe) or for a subscription
// (subscriptions/listen) when more changes arrive than max_queued_notifications lets wait.
//
// A thread reports an update of the gate resource and is held inside the sink while it writes it; the test reports
// the burst meanwhile (each report returns once the session queued or dropped the change), lets the writer go, and
// reports a last update. What was written between the gate's update and the last one is what the burst left.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/change.h"
#include "neomcp/feature_host.h"
#include "neomcp/jsonrpc.h"
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
constexpr std::size_t bound = 4;      // max_queued_notifications of every session here
constexpr std::size_t items = 2 * bound;  // a burst of distinct updates twice the bound
constexpr const char* gate = "test://gate";
constexpr const char* last = "test://last";
constexpr const char* subscription_feed = "subscriptions_listen";
constexpr const char* subscription_key = "io.modelcontextprotocol/subscriptionId";

std::string item(std::size_t index) { return "test://item/" + std::to_string(index); }

std::vector<std::string> watched_uris() {
    std::vector<std::string> uris{gate, last};
    for (std::size_t i = 0; i < items; ++i) uris.push_back(item(i));
    return uris;
}

// A host with one tool and the resources above, which reports a change on the thread that asks it to.
class BurstHost final : public neomcp::ToolHost, public neomcp::ResourceHost {
public:
    BurstHost() {
        neomcp::ToolDefinition tool;
        tool.name = "alpha";
        tool.description = "A tool";
        tools_.push_back(std::move(tool));
        for (const std::string& uri : watched_uris()) {
            neomcp::ResourceDefinition resource;
            resource.uri = uri;
            resource.name = uri;
            resources_.push_back(std::move(resource));
        }
    }

    void report(neomcp::ChangeKind kind, std::string uri = {}) {
        std::shared_ptr<neomcp::ChangeListener> listener;
        {
            std::lock_guard lock(mutex_);
            listener = listener_.lock();
        }
        if (listener) listener->changed(neomcp::Change{kind, std::move(uri)});
    }

    neomcp::ResourceHost* resource_host() noexcept override { return this; }
    std::span<const neomcp::ToolDefinition> tools() const override { return tools_; }
    void call(const neomcp::CallRequest&, neomcp::CallControl, neomcp::Completion done) override {
        done(neomcp::ToolResult{});
    }
    neomcp::ChangeSupport change_support() const override { return {true, false, true, true}; }
    neomcp::ChangeRegistration watch(std::weak_ptr<neomcp::ChangeListener> listener) override {
        std::lock_guard lock(mutex_);
        listener_ = std::move(listener);
        return neomcp::ChangeRegistration([this] {
            std::lock_guard held(mutex_);
            listener_.reset();
        });
    }

    std::span<const neomcp::ResourceDefinition> resources() const override { return resources_; }
    std::span<const neomcp::ResourceTemplateDefinition> resource_templates() const override { return {}; }
    void read(const neomcp::ReadRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        neomcp::ReadResult result;
        result.contents.emplace_back(neomcp::TextResourceContents{request.uri, std::nullopt, "content", std::nullopt});
        done(neomcp::FeatureOutcome(std::move(result)));
    }

private:
    std::vector<neomcp::ToolDefinition> tools_;
    std::vector<neomcp::ResourceDefinition> resources_;
    std::mutex mutex_;
    std::weak_ptr<neomcp::ChangeListener> listener_;
};

// One written message in short: "tools" and "resources" for the list changes, "updated <uri>", "ack" for an
// acknowledgement, "answer <id>" for a response; with "@<subscription>" when the message carries a subscription's id
// and `tagged` asks for it.
std::string brief(const json& message, bool tagged) {
    std::string text;
    if (message.contains("method")) {
        const std::string method = message.at("method").get<std::string>();
        if (method == "notifications/tools/list_changed") {
            text = "tools";
        } else if (method == "notifications/resources/list_changed") {
            text = "resources";
        } else if (method == "notifications/resources/updated") {
            text = "updated " + message.at("params").at("uri").get<std::string>();
        } else if (method == "notifications/subscriptions/acknowledged") {
            text = "ack";
        } else {
            text = method;
        }
    } else {
        text = "answer " + message.at("id").dump();
    }
    if (tagged && message.contains("params") && message.at("params").contains("_meta")) {
        const json& meta = message.at("params").at("_meta");
        if (meta.contains(subscription_key)) text += "@" + meta.at(subscription_key).get<std::string>();
    }
    return text;
}

// What the session wrote, in order; the sink is held on the next update of the gate once armed, until released.
class Written {
public:
    void take(neomcp::Output output) {
        if (output.kind == neomcp::OutputKind::end) return;
        std::unique_lock lock(mutex_);
        json message = json::parse(output.line);
        const bool hold = holding_ && brief(message, false) == std::string("updated ") + gate;
        messages_.push_back(std::move(message));
        changed_.notify_all();
        if (!hold) return;
        holding_ = false;
        held_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, patience, [&] { return !held_; });
    }
    void arm() {
        std::lock_guard lock(mutex_);
        holding_ = true;
    }
    bool wait_held() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return held_; });
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            held_ = false;
        }
        changed_.notify_all();
    }
    // Waits until a written message reads `text` in short.
    bool wait_for(const std::string& text, bool tagged = false) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] {
            return std::any_of(messages_.begin(), messages_.end(),
                               [&](const json& message) { return brief(message, tagged) == text; });
        });
    }
    std::vector<std::string> briefs(bool tagged = false) {
        std::lock_guard lock(mutex_);
        std::vector<std::string> out;
        for (const json& message : messages_) out.push_back(brief(message, tagged));
        return out;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<json> messages_;
    bool holding_ = false;
    bool held_ = false;
};

// A session of one revision, told of the lists and of every resource above: through its connection, or through one
// subscription with the id "burst".
class Rig {
public:
    explicit Rig(const std::string& revision) : profile_(profile::builtin(revision)) {
        if (!profile_) throw std::logic_error("no built-in profile " + revision);
        neomcp::SessionOptions options;
        options.profiles = {profile_};
        options.server_info.name = "burst-test";
        options.server_info.version = "1.0.0";
        options.max_queued_notifications = bound;
        auto made = neomcp::Session::create_tagged(std::move(options), host_,
                                                   [this](neomcp::Output output) { written_.take(std::move(output)); });
        if (auto* error = std::get_if<neomcp::CreateError>(&made)) throw std::runtime_error(error->message);
        session_ = std::get<std::unique_ptr<neomcp::Session>>(std::move(made));
        if (subscribes()) {
            json filter{{"toolsListChanged", true}, {"resourcesListChanged", true}, {"resourceSubscriptions", watched_uris()}};
            listen("burst", std::move(filter));
            return;
        }
        request(1, "initialize", Peer::initialize_request(*profile_, neomcp::RequestId(1)));
        send(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        int id = 10;
        for (const std::string& uri : watched_uris()) {
            request(id, "resources/subscribe",
                    Peer::make_request(*profile_, neomcp::RequestId(id), "resources/subscribe", json{{"uri", uri}}));
            ++id;
        }
    }
    ~Rig() {
        written_.release();
        if (writer_.joinable()) writer_.join();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    bool subscribes() const { return profile_->mechanism(profile::Slot::change_feed) == subscription_feed; }

    // Opens a subscription and waits for its acknowledgement.
    void listen(const std::string& id, json filter) {
        send(Peer::make_request(*profile_, neomcp::RequestId(id), "subscriptions/listen",
                                json{{"notifications", std::move(filter)}}));
        if (!written_.wait_for("ack@" + id, true)) throw std::runtime_error("the subscription was not acknowledged");
    }
    void send(const json& message) { session_->receive(++unit_, neomcp::serialize(message)); }
    void request(int id, const std::string& method, const json& message) {
        send(message);
        if (!written_.wait_for("answer " + std::to_string(id))) throw std::runtime_error("no answer to " + method);
    }

    // A thread reports an update of the gate and is held while it writes it.
    void hold() {
        written_.arm();
        writer_ = std::thread([this] { host_.report(neomcp::ChangeKind::resource_updated, gate); });
        if (!written_.wait_held()) throw std::runtime_error("the writer was not held");
    }
    // Lets the writer go, waits until it wrote what was queued, and reports an update of `last`.
    void release() {
        written_.release();
        writer_.join();
        host_.report(neomcp::ChangeKind::resource_updated, last);
        if (!written_.wait_for(std::string("updated ") + last)) throw std::runtime_error("the last update was not written");
    }
    // What was written after the gate's update, up to the last update (included).
    std::vector<std::string> after_gate(bool tagged = false) {
        const std::vector<std::string> all = written_.briefs(tagged);
        const std::string held = std::string("updated ") + gate + (tagged && subscribes() ? "@burst" : "");
        const auto from = std::find(all.begin(), all.end(), held);
        if (from == all.end()) return {};
        return std::vector<std::string>(std::next(from), all.end());
    }

    BurstHost& host() { return host_; }
    const profile::Profile& profile() const { return *profile_; }

private:
    profile::ProfilePtr profile_;
    BurstHost host_;
    Written written_;
    neomcp::UnitId unit_ = 0;
    std::thread writer_;
    std::unique_ptr<neomcp::Session> session_;  // last: closed before what its sink uses goes
};

std::vector<std::string> updates(std::size_t from, std::size_t to) {
    std::vector<std::string> out;
    for (std::size_t i = from; i < to; ++i) out.push_back("updated " + item(i));
    return out;
}

std::vector<std::string> joined(std::initializer_list<std::vector<std::string>> parts) {
    std::vector<std::string> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

const std::vector<std::string> last_only{std::string("updated ") + last};

std::vector<std::string> every_revision() {
    std::vector<std::string> out;
    for (const std::string_view revision : profile::builtin_revisions()) out.emplace_back(revision);
    return out;
}

std::string test_name(const ::testing::TestParamInfo<std::string>& info) {
    std::string name;
    for (const char c : info.param) name += (c >= '0' && c <= '9') ? c : '_';
    return "r" + name;
}

class ChangeBurst : public ::testing::TestWithParam<std::string> {};

INSTANTIATE_TEST_SUITE_P(EveryRevision, ChangeBurst, ::testing::ValuesIn(every_revision()), test_name);

// Twice the bound of distinct updates, then a list change: the list change is written, before the updates that were
// queued; the updates beyond the bound are dropped as they were.
TEST_P(ChangeBurst, AListChangeAfterABurstOfUpdatesIsWrittenFirstAndTheBoundStays) {
    Rig rig(GetParam());
    rig.hold();
    for (std::size_t i = 0; i < items; ++i) rig.host().report(neomcp::ChangeKind::resource_updated, item(i));
    rig.host().report(neomcp::ChangeKind::tools_list);
    rig.release();
    EXPECT_EQ(rig.after_gate(), joined({{"tools"}, updates(0, bound), last_only}));
}

// List changes of one kind that wait to be written come out as one; kinds keep the order they came in.
TEST_P(ChangeBurst, ListChangesOfOneKindWaitingToBeWrittenComeOutAsOne) {
    Rig rig(GetParam());
    rig.hold();
    for (std::size_t i = 0; i < bound; ++i) rig.host().report(neomcp::ChangeKind::resource_updated, item(i));
    for (int round = 0; round < 3; ++round) {
        rig.host().report(neomcp::ChangeKind::tools_list);
        rig.host().report(neomcp::ChangeKind::resources_list);
    }
    rig.release();
    EXPECT_EQ(rig.after_gate(), joined({{"tools", "resources"}, updates(0, bound), last_only}));

    // Once written, the next change of the list is written again.
    rig.host().report(neomcp::ChangeKind::tools_list);
    rig.host().report(neomcp::ChangeKind::resource_updated, item(0));
    rig.host().report(neomcp::ChangeKind::resource_updated, last);
    const auto tail = rig.after_gate();
    ASSERT_GE(tail.size(), 3u);
    EXPECT_EQ(std::vector<std::string>(tail.end() - 3, tail.end()),
              (std::vector<std::string>{"tools", "updated " + item(0), std::string("updated ") + last}));
}

// Updates of one resource that wait to be written come out as one, and take one place of the bound.
TEST_P(ChangeBurst, UpdatesOfOneResourceWaitingToBeWrittenComeOutAsOne) {
    Rig rig(GetParam());
    rig.hold();
    // Each resource many times in a row, more often than the bound: had the repeats taken places, the first resource
    // would fill the queue and the others be dropped.
    for (std::size_t i = 0; i < bound; ++i) {
        for (std::size_t round = 0; round < 3 * bound; ++round) {
            rig.host().report(neomcp::ChangeKind::resource_updated, item(i));
        }
    }
    rig.release();
    EXPECT_EQ(rig.after_gate(), joined({updates(0, bound), last_only}));
}

// A list change passes the updates queued before it, never a response.
TEST_P(ChangeBurst, AListChangeStaysBehindAResponseQueuedBeforeIt) {
    Rig rig(GetParam());
    rig.hold();
    rig.host().report(neomcp::ChangeKind::resource_updated, item(0));
    rig.send(Peer::make_request(rig.profile(), neomcp::RequestId(50), "ping"));
    rig.host().report(neomcp::ChangeKind::resource_updated, item(1));
    rig.host().report(neomcp::ChangeKind::tools_list);
    rig.release();
    EXPECT_EQ(rig.after_gate(), joined({{"updated " + item(0), "answer 50", "tools", "updated " + item(1)}, last_only}));
}

// Under subscriptions: a list change of a subscription opened while the writer is held comes after its
// acknowledgement, and the held subscription's list change comes before its own updates queued earlier.
TEST_P(ChangeBurst, AListChangeOfASubscriptionComesAfterItsAcknowledgement) {
    Rig rig(GetParam());
    if (!rig.subscribes()) return;  // the revision tells the connection, which has no acknowledgement
    rig.hold();
    for (std::size_t i = 0; i < bound; ++i) rig.host().report(neomcp::ChangeKind::resource_updated, item(i));
    rig.send(Peer::make_request(rig.profile(), neomcp::RequestId("second"), "subscriptions/listen",
                                json{{"notifications", {{"toolsListChanged", true}}}}));
    rig.host().report(neomcp::ChangeKind::tools_list);
    rig.release();
    const auto tail = rig.after_gate(true);
    const auto at = [&](const std::string& text) { return std::find(tail.begin(), tail.end(), text) - tail.begin(); };
    const auto end = static_cast<std::ptrdiff_t>(tail.size());
    ASSERT_LT(at("tools@second"), end) << ::testing::PrintToString(tail);
    ASSERT_LT(at("tools@burst"), end) << ::testing::PrintToString(tail);
    EXPECT_LT(at("ack@second"), at("tools@second")) << ::testing::PrintToString(tail);
    EXPECT_LT(at("tools@burst"), at("updated " + item(0) + "@burst")) << ::testing::PrintToString(tail);
}

}  // namespace
