// The resources a connection watches, in the session engine, where units come with an authorization: a later unit (or
// Session::present) whose authorization cannot read a watched resource ends the watch before anything of the unit is
// read, and an update of it that was queued before, behind an output another thread is writing, is not written after.
// The session serves 2025-11-25 on stdio and is fed units directly, as a transport that authorizes feeds it; the host
// reports changes on the thread a test chooses.
#include <chrono>
#include <condition_variable>
#include <cstddef>
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

#include "neomcp/authorization.h"
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
using Granted = std::shared_ptr<const neomcp::Authorization>;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* revision = "2025-11-25";
constexpr const char* secret = "test://secret";  // needs secret:read
constexpr const char* open_resource = "test://open";
constexpr const char* updated_method = "notifications/resources/updated";

profile::ProfilePtr builtin(const std::string& name) {
    auto found = profile::builtin(name);
    if (!found) throw std::logic_error("no built-in profile " + name);
    return found;
}

Granted grant(std::vector<std::string> scopes) {
    auto made = std::make_shared<neomcp::Authorization>();
    made->issuer = "https://as.example";
    made->subject = "alice";
    made->client_id = "client-1";
    made->scopes = std::move(scopes);
    return made;
}

// Two resources, one of which needs secret:read; changes are reported on the caller's thread.
class Host final : public neomcp::ToolHost, public neomcp::ResourceHost {
public:
    Host() {
        for (const char* uri : {secret, open_resource}) {
            neomcp::ResourceDefinition resource;
            resource.uri = uri;
            resource.name = uri;
            resources_.push_back(std::move(resource));
        }
    }

    void report(const std::string& uri) {
        std::shared_ptr<neomcp::ChangeListener> listener;
        {
            std::lock_guard lock(mutex_);
            listener = listener_.lock();
        }
        if (listener) listener->changed(neomcp::Change{neomcp::ChangeKind::resource_updated, uri});
    }

    // ToolHost
    neomcp::ResourceHost* resource_host() noexcept override { return this; }
    std::span<const neomcp::ToolDefinition> tools() const override { return {}; }
    void call(const neomcp::CallRequest&, neomcp::CallControl, neomcp::Completion) override {}
    neomcp::ChangeSupport change_support() const override { return {false, false, false, true}; }
    neomcp::ChangeRegistration watch(std::weak_ptr<neomcp::ChangeListener> listener) override {
        std::lock_guard lock(mutex_);
        listener_ = std::move(listener);
        return neomcp::ChangeRegistration([this] {
            std::lock_guard released(mutex_);
            listener_.reset();
        });
    }

    // ResourceHost
    std::span<const neomcp::ResourceDefinition> resources() const override { return resources_; }
    std::span<const neomcp::ResourceTemplateDefinition> resource_templates() const override { return {}; }
    std::span<const std::string> read_scopes(std::string_view uri) const override {
        if (uri == secret) return secret_scopes_;
        return {};
    }
    void read(const neomcp::ReadRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        neomcp::ReadResult result;
        result.contents.emplace_back(neomcp::TextResourceContents{request.uri, std::nullopt, "content", std::nullopt});
        done(neomcp::FeatureOutcome(std::move(result)));
    }

private:
    std::vector<neomcp::ResourceDefinition> resources_;
    const std::vector<std::string> secret_scopes_{"secret:read"};
    std::mutex mutex_;
    std::weak_ptr<neomcp::ChangeListener> listener_;
};

// What the session wrote, in order; the sink can be held on the next update of the open resource until released.
class Written {
public:
    void take(neomcp::Output output) {
        if (output.kind == neomcp::OutputKind::end) return;  // the end of a unit has no line
        std::unique_lock lock(mutex_);
        const bool hold = holding_ && is_update_of(output.line, open_resource);
        scopes_.push_back(std::move(output.read_scopes));
        lines_.push_back(std::move(output.line));
        changed_.notify_all();
        if (!hold) return;
        holding_ = false;
        held_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, patience, [&] { return !held_; });
    }
    void hold_next_open_update() {
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
    // Waits until a line answers request `id`.
    bool wait_answer(int id) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] {
            for (const std::string& line : lines_) {
                const json message = json::parse(line);
                if (message.contains("id") && message.at("id") == id) return true;
            }
            return false;
        });
    }
    int updates_of(const std::string& uri) {
        std::lock_guard lock(mutex_);
        int count = 0;
        for (const std::string& line : lines_) {
            if (is_update_of(line, uri)) ++count;
        }
        return count;
    }

    // What each written update of `uri` said reading the resource needs (Output::read_scopes).
    std::vector<std::vector<std::string>> scopes_of(const std::string& uri) {
        std::lock_guard lock(mutex_);
        std::vector<std::vector<std::string>> out;
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            if (is_update_of(lines_[i], uri)) out.push_back(scopes_[i]);
        }
        return out;
    }

private:
    static bool is_update_of(const std::string& line, const std::string& uri) {
        if (line.empty()) return false;
        const json message = json::parse(line);
        return message.value("method", "") == updated_method && message.at("params").at("uri") == uri;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> lines_;
    std::vector<std::vector<std::string>> scopes_;  // each line's Output::read_scopes
    bool holding_ = false;
    bool held_ = false;
};

// A session with its connection open, which watches both resources with an authorization that may read them.
struct Rig {
    Rig() {
        neomcp::SessionOptions options;
        options.profiles = {builtin(revision)};
        options.server_info.name = "subscription-test";
        options.server_info.version = "1.0.0";
        auto made = neomcp::Session::create_tagged(std::move(options), host,
                                                   [this](neomcp::Output output) { written.take(std::move(output)); });
        if (auto* error = std::get_if<neomcp::CreateError>(&made)) throw std::runtime_error(error->message);
        session = std::get<std::unique_ptr<neomcp::Session>>(std::move(made));
        send(Peer::initialize_request(*profile, neomcp::RequestId(1)), broad);
        send(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, broad);
        subscribe(secret, broad, 2);
        subscribe(open_resource, broad, 3);
    }

    void send(const json& message, const Granted& who) {
        session->receive(++unit, neomcp::serialize(message), {}, who);
    }
    void subscribe(const std::string& uri, const Granted& who, int id) {
        send(Peer::make_request(*profile, neomcp::RequestId(id), "resources/subscribe", json{{"uri", uri}}), who);
        if (!written.wait_answer(id)) throw std::runtime_error("no answer to resources/subscribe");
    }
    void ping(const Granted& who, int id) {
        send(Peer::make_request(*profile, neomcp::RequestId(id), "ping"), who);
        if (!written.wait_answer(id)) throw std::runtime_error("no answer to ping");
    }

    profile::ProfilePtr profile = builtin(revision);
    const Granted broad = grant({"secret:read"});
    const Granted narrow = grant({});
    Host host;
    Written written;
    neomcp::UnitId unit = 0;
    std::unique_ptr<neomcp::Session> session;  // last: closed before what its sink uses goes
};

TEST(SessionSubscription, AUnitWhoseAuthorizationCannotReadAWatchedResourceEndsTheWatch) {
    Rig rig;
    rig.host.report(secret);
    rig.host.report(open_resource);
    EXPECT_EQ(rig.written.updates_of(secret), 1);

    rig.ping(rig.narrow, 10);
    rig.host.report(secret);
    rig.host.report(open_resource);
    EXPECT_EQ(rig.written.updates_of(open_resource), 2);
    EXPECT_EQ(rig.written.updates_of(secret), 1);

    // The broad authorization again does not bring the watch back; subscribing again does.
    rig.ping(rig.broad, 11);
    rig.host.report(secret);
    EXPECT_EQ(rig.written.updates_of(secret), 1);
    rig.subscribe(secret, rig.broad, 12);
    rig.host.report(secret);
    EXPECT_EQ(rig.written.updates_of(secret), 2);
}

TEST(SessionSubscription, PresentingAnAuthorizationThatCannotReadAWatchedResourceEndsTheWatch) {
    Rig rig;
    rig.session->present(nullptr);
    rig.host.report(secret);
    EXPECT_EQ(rig.written.updates_of(secret), 1) << "a null authorization ends nothing";
    rig.session->present(rig.narrow);
    rig.host.report(secret);
    rig.host.report(open_resource);
    EXPECT_EQ(rig.written.updates_of(open_resource), 1);
    EXPECT_EQ(rig.written.updates_of(secret), 1);
}

TEST(SessionSubscription, AnUpdateQueuedBeforeTheWatchEndedIsNotWrittenAfter) {
    Rig rig;
    // Another thread writes the open resource's update and is held inside the sink; the secret resource's update is
    // queued behind it; then a unit with the narrow authorization arrives. The writer, released, finds the secret
    // resource no longer watched.
    rig.written.hold_next_open_update();
    std::thread writer([&rig] { rig.host.report(open_resource); });
    struct Join {
        std::thread& thread;
        Written& written;
        ~Join() {
            written.release();
            if (thread.joinable()) thread.join();
        }
    } const join{writer, rig.written};
    ASSERT_TRUE(rig.written.wait_held());
    rig.host.report(secret);
    rig.send(Peer::make_request(*rig.profile, neomcp::RequestId(20), "ping"), rig.narrow);
    rig.written.release();
    ASSERT_TRUE(rig.written.wait_answer(20));
    EXPECT_EQ(rig.written.updates_of(open_resource), 1);
    EXPECT_EQ(rig.written.updates_of(secret), 0) << "the update queued for the ended watch is not written";
}

// An update reported again while one of the same resource waits to be written adds nothing: the one that waits stands
// for it, and is not written once a unit ended the watch.
TEST(SessionSubscription, AnUpdateRepeatedBeforeTheWatchEndedIsNotWrittenAfter) {
    Rig rig;
    rig.written.hold_next_open_update();
    std::thread writer([&rig] { rig.host.report(open_resource); });
    struct Join {
        std::thread& thread;
        Written& written;
        ~Join() {
            written.release();
            if (thread.joinable()) thread.join();
        }
    } const join{writer, rig.written};
    ASSERT_TRUE(rig.written.wait_held());
    for (int round = 0; round < 3; ++round) rig.host.report(secret);
    rig.send(Peer::make_request(*rig.profile, neomcp::RequestId(20), "ping"), rig.narrow);
    rig.written.release();
    ASSERT_TRUE(rig.written.wait_answer(20));
    EXPECT_EQ(rig.written.updates_of(secret), 0) << "the update that stands for the others is not written either";
}

// The update that stands for later ones is written once, with what reading the resource needs, for the transport to
// check against the stream it writes on.
TEST(SessionSubscription, AnUpdateThatStandsForLaterOnesCarriesWhatReadingTheResourceNeeds) {
    Rig rig;
    rig.written.hold_next_open_update();
    std::thread writer([&rig] { rig.host.report(open_resource); });
    struct Join {
        std::thread& thread;
        Written& written;
        ~Join() {
            written.release();
            if (thread.joinable()) thread.join();
        }
    } const join{writer, rig.written};
    ASSERT_TRUE(rig.written.wait_held());
    for (int round = 0; round < 3; ++round) rig.host.report(secret);
    rig.written.release();
    rig.ping(rig.broad, 21);
    const auto scopes = rig.written.scopes_of(secret);
    ASSERT_EQ(scopes.size(), 1u);
    EXPECT_EQ(scopes.front(), (std::vector<std::string>{"secret:read"}));
}

}  // namespace
