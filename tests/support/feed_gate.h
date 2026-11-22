// Holds the change feed of a Server for a test: a session of the server, one that opens with a handshake, watches one
// resource, and its sink holds the thread that writes the next update of that resource until the test releases it.
// That thread is the server's feed, so every change the test reports meanwhile waits in the server, and is told to the
// sessions once the gate is released.
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace neomcp::testing {

class FeedGate {
public:
    static constexpr std::chrono::seconds patience{30};

    FeedGate(Server& server, std::string uri, const std::string& revision) : uri_(std::move(uri)) {
        const profile::ProfilePtr profile = profile::builtin(revision);
        if (!profile) throw std::logic_error("no built-in profile " + revision);
        auto made = server.connect([this](std::string line) { take(std::move(line)); });
        if (auto* error = std::get_if<CreateError>(&made)) throw std::runtime_error(error->message);
        session_ = std::get<std::unique_ptr<Session>>(std::move(made));
        session_->receive(serialize(Peer::initialize_request(*profile, RequestId(1))));
        wait_answer(1);
        session_->receive(serialize(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
        session_->receive(serialize(Peer::make_request(*profile, RequestId(2), "resources/subscribe", json{{"uri", uri_}})));
        wait_answer(2);
    }
    ~FeedGate() {
        release();
        if (session_) session_->close();
    }
    FeedGate(const FeedGate&) = delete;
    FeedGate& operator=(const FeedGate&) = delete;

    // Reports an update of the resource through `server` and returns once the feed is held writing it.
    void hold(Server& server) {
        {
            std::lock_guard lock(mutex_);
            holding_ = true;
        }
        server.notify_resource_updated(uri_);
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, patience, [&] { return held_; })) throw std::runtime_error("the feed was not held");
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            held_ = false;
            holding_ = false;
        }
        changed_.notify_all();
    }

private:
    void take(std::string line) {
        const json message = json::parse(line);
        std::unique_lock lock(mutex_);
        if (message.contains("id")) answered_.push_back(message.at("id"));
        const bool update = message.value("method", std::string()) == "notifications/resources/updated" &&
                            message.at("params").at("uri") == uri_;
        changed_.notify_all();
        if (!update || !holding_) return;
        holding_ = false;
        held_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, patience, [&] { return !held_; });
    }
    void wait_answer(int id) {
        std::unique_lock lock(mutex_);
        const bool answered = changed_.wait_for(lock, patience, [&] {
            for (const json& seen : answered_) {
                if (seen == id) return true;
            }
            return false;
        });
        if (!answered) throw std::runtime_error("the gate's session did not answer request " + std::to_string(id));
    }

    std::string uri_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<json> answered_;
    bool holding_ = false;
    bool held_ = false;
    std::unique_ptr<Session> session_;  // last: closed before what its sink uses goes
};

}  // namespace neomcp::testing
