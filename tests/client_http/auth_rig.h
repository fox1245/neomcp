// Shared by the tests of the authorization seam: an authorizer the test drives. It gives the value and generation the
// test sets, records what it is asked and told, and keeps every reply until the test answers it (from the test's
// thread: a reply may be answered from any thread).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "neomcp/client/http.h"
#include "rig.h"

namespace auth_rig {

struct Told {
    neomcp::HttpRequestInfo request;
    int status = 0;
    std::vector<std::string> challenges;
    std::uint64_t generation = 0;
};

class ScriptedAuthorizer final : public neomcp::HttpAuthorizer {
public:
    explicit ScriptedAuthorizer(std::string value = "Bearer first", std::uint64_t generation = 0)
        : value_(std::move(value)), generation_(generation) {}

    void attach(neomcp::AuthorizationHost& host) override {
        std::lock_guard lock(mutex_);
        host_ = &host;
        ++attaches_;
        changed_.notify_all();
    }
    void detach() override {
        std::lock_guard lock(mutex_);
        host_ = nullptr;
        ++detaches_;
        changed_.notify_all();
    }
    neomcp::Credential authorization(const neomcp::HttpRequestInfo& request) override {
        std::lock_guard lock(mutex_);
        asked_.push_back(request);
        return neomcp::Credential{value_, generation_};
    }
    void challenge(const neomcp::HttpRequestInfo& request, int status, std::span<const std::string> challenges,
                   std::uint64_t generation, neomcp::ChallengeReply reply) override {
        std::lock_guard lock(mutex_);
        told_.push_back(Told{request, status, std::vector<std::string>(challenges.begin(), challenges.end()),
                             generation});
        replies_.push_back(std::move(reply));
        changed_.notify_all();
    }

    // From the test's thread.
    void set(std::string value, std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        value_ = std::move(value);
        generation_ = generation;
    }
    bool wait_told(std::size_t count, std::chrono::milliseconds bound = http_rig::wait_bound) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return told_.size() >= count; });
    }
    bool wait_detached(std::chrono::milliseconds bound = http_rig::wait_bound) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return detaches_ > 0; });
    }
    void retry(std::size_t index) {
        std::lock_guard lock(mutex_);
        replies_.at(index).retry();
    }
    void fail(std::size_t index, neomcp::ClientError error) {
        std::lock_guard lock(mutex_);
        replies_.at(index).fail(std::move(error));
    }
    // Fails every reply still unanswered, so that nothing stays held.
    void fail_all() {
        std::lock_guard lock(mutex_);
        for (auto& reply : replies_) reply.fail(failed_kind());
    }
    std::vector<Told> told() const {
        std::lock_guard lock(mutex_);
        return told_;
    }
    std::size_t asked() const {
        std::lock_guard lock(mutex_);
        return asked_.size();
    }
    int attaches() const {
        std::lock_guard lock(mutex_);
        return attaches_;
    }
    int detaches() const {
        std::lock_guard lock(mutex_);
        return detaches_;
    }
    // The host, while attached; used on the client's thread only.
    neomcp::AuthorizationHost* host() const {
        std::lock_guard lock(mutex_);
        return host_;
    }

private:
    static neomcp::ClientError failed_kind() {
        neomcp::ClientError error;
        error.kind = neomcp::ClientErrorKind::authorization_required;
        return error;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string value_;
    std::uint64_t generation_ = 0;
    neomcp::AuthorizationHost* host_ = nullptr;
    int attaches_ = 0;
    int detaches_ = 0;
    std::vector<neomcp::HttpRequestInfo> asked_;
    std::vector<Told> told_;
    std::vector<neomcp::ChallengeReply> replies_;
};

// Runs its action when it goes out of scope: a test that stops at a failed assertion while a call is held releases the
// call before the future that waits for it is destroyed.
template <class F>
class Finally {
public:
    explicit Finally(F action) : action_(std::move(action)) {}
    ~Finally() { action_(); }
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;

private:
    F action_;
};

inline neomcp::ClientError failed_with(neomcp::AuthorizationProblem problem) {
    neomcp::ClientError error;
    error.kind = neomcp::ClientErrorKind::authorization_failed;
    error.message = "the sign-in did not complete";
    error.authorization = neomcp::AuthorizationFailure{problem, "https://issuer.test", "https://issuer.test/token"};
    return error;
}

}  // namespace auth_rig
