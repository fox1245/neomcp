// The requests that wait for one validation of their token, driven through the endpoint's handler (no sockets, so that
// the test decides when the verdict comes and sees the answers in the order the endpoint completes them): one
// validation for any number of requests, the order of a session's requests kept, including a request that arrives
// while the waiting ones are being handed on, and stop() while a validation is in progress.
//
// A ping is answered by the session while it reads the request, so the order the answers complete in is the order the
// requests were fed to the session.
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/endpoint.h"
#include "neomcp/http/resource_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::http::TokenRefusal;
using neomcp::http::TokenVerdict;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* legacy = "2025-11-25";
constexpr const char* resource = "http://localhost/mcp";
constexpr const char* issuer = "https://auth.example";

// A validator that answers held tokens only when the test releases them, and the others at once.
class Held final : public neomcp::http::TokenValidator {
public:
    void set(const std::string& token, TokenVerdict verdict, bool hold) {
        std::lock_guard lock(mutex_);
        verdicts_.insert_or_assign(token, std::move(verdict));
        if (hold) held_.insert_or_assign(token, std::vector<neomcp::http::TokenReply>{});
    }
    void validate(const neomcp::http::TokenCheck& check, neomcp::http::TokenReply reply) override {
        const std::string token(check.token);
        TokenVerdict verdict = TokenRefusal::invalid;
        {
            std::lock_guard lock(mutex_);
            ++calls_[token];
            if (const auto found = held_.find(token); found != held_.end()) {
                found->second.push_back(std::move(reply));
                changed_.notify_all();
                return;
            }
            if (const auto found = verdicts_.find(token); found != verdicts_.end()) verdict = found->second;
        }
        reply(std::move(verdict));
    }
    // Waits until a validation of `token` is held.
    bool wait_held(const std::string& token) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return !held_[token].empty(); });
    }
    // Answers what was held for `token`, on this thread.
    void release(const std::string& token) {
        std::vector<neomcp::http::TokenReply> replies;
        TokenVerdict verdict = TokenRefusal::invalid;
        {
            std::lock_guard lock(mutex_);
            replies = std::move(held_[token]);
            held_.erase(token);
            verdict = verdicts_.at(token);
        }
        for (auto& reply : replies) reply(verdict);
    }
    int calls(const std::string& token) {
        std::lock_guard lock(mutex_);
        return calls_[token];
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, TokenVerdict> verdicts_;
    std::map<std::string, std::vector<neomcp::http::TokenReply>> held_;
    std::map<std::string, int> calls_;
};

// The answers, in the order they were completed.
class Answers {
public:
    struct Answer {
        std::string label;
        neomcp::http::Response response;
    };

    neomcp::http::Responder responder(std::string label, std::function<void()> after = {}) {
        return neomcp::http::Responder([this, label = std::move(label), after = std::move(after)](
                                           neomcp::http::Response&& response) {
            {
                std::lock_guard lock(mutex_);
                answers_.push_back(Answer{label, std::move(response)});
            }
            changed_.notify_all();
            if (after) after();
            return true;
        });
    }
    bool wait_for(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return answers_.size() >= count; });
    }
    std::vector<Answer> all() {
        std::lock_guard lock(mutex_);
        return answers_;
    }
    std::vector<std::string> labels() {
        std::vector<std::string> out;
        for (const Answer& answer : all()) out.push_back(answer.label);
        return out;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Answer> answers_;
};

neomcp::Authorization alice(std::chrono::seconds lifetime = 3600s) {
    neomcp::Authorization authorization;
    authorization.issuer = issuer;
    authorization.subject = "alice";
    authorization.scopes = {"mcp:use"};
    authorization.audiences = {resource};
    authorization.expires = std::chrono::system_clock::now() + lifetime;
    return authorization;
}

struct Rig {
    Rig() : server(neomcp::ServerOptions{.name = "flight-test", .version = "0.0.1"}) {
        neomcp::http::EndpointOptions options;
        options.stream_after = 1h;
        neomcp::http::ResourceServerOptions authorization;
        authorization.resource = resource;
        authorization.authorization_servers = {issuer};
        authorization.validator = validator;
        options.authorization = std::move(authorization);
        auto policy = neomcp::http::HostPolicy::create({});
        if (!policy) throw std::runtime_error(policy.error);
        auto made = neomcp::http::Endpoint::create(server, std::move(*policy.value), std::move(options));
        if (!made) throw std::runtime_error(made.error);
        endpoint = std::move(*made.value);
    }
    ~Rig() { endpoint->stop(); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    neomcp::http::Request request(const std::string& token, const std::string& session, std::string body) const {
        neomcp::http::Request made;
        made.method = "POST";
        made.path = "/mcp";
        made.headers = {{"Host", "localhost"},
                        {"Content-Type", "application/json"},
                        {"Accept", "application/json, text/event-stream"},
                        {"MCP-Protocol-Version", legacy},
                        {"Authorization", "Bearer " + token}};
        if (!session.empty()) made.headers.add("Mcp-Session-Id", session);
        made.body = std::move(body);
        return made;
    }
    neomcp::http::Request ping(const std::string& token, const std::string& session, int id) const {
        return request(token, session, json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}}.dump());
    }

    // A legacy session opened with `token` (answered at once); its id.
    std::string open_session(const std::string& token) {
        Answers answers;
        const json params{{"protocolVersion", legacy},
                          {"capabilities", json::object()},
                          {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}}};
        endpoint->handle(request(token, {}, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                                 {"params", params}}
                                                .dump()),
                         answers.responder("initialize"));
        if (!answers.wait_for(1)) throw std::runtime_error("no answer to initialize");
        const auto opened = answers.all();
        const auto id = opened.front().response.headers.get("Mcp-Session-Id");
        if (!id) throw std::runtime_error("no session id");
        const std::string session(*id);
        endpoint->handle(request(token, session, json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump()),
                         answers.responder("initialized"));
        if (!answers.wait_for(2) || answers.all().back().response.status != 202) {
            const auto all = answers.all();
            throw std::runtime_error("the session was not opened: " +
                                     (all.size() < 2 ? std::string("no answer")
                                                     : std::to_string(all.back().response.status) + " " +
                                                           all.back().response.body));
        }
        return session;
    }

    std::shared_ptr<Held> validator = std::make_shared<Held>();
    neomcp::Server server;
    std::unique_ptr<neomcp::http::Endpoint> endpoint;
};

int id_of(const Answers::Answer& answer) { return json::parse(answer.response.body).at("id").get<int>(); }

TEST(ResourceServerFlight, ManyRequestsWithOneNewTokenCauseOneValidationAndGoOnInArrivalOrder) {
    for (const int count : {2, 10}) {
        Rig rig;
        rig.validator->set("opener", alice(), false);
        const std::string session = rig.open_session("opener");
        rig.validator->set("fresh", alice(), true);
        Answers answers;
        for (int i = 1; i <= count; ++i) {
            rig.endpoint->handle(rig.ping("fresh", session, i), answers.responder(std::to_string(i)));
        }
        ASSERT_TRUE(rig.validator->wait_held("fresh"));
        EXPECT_TRUE(answers.all().empty()) << "nothing goes on before the verdict";
        rig.validator->release("fresh");
        ASSERT_TRUE(answers.wait_for(static_cast<std::size_t>(count)));
        const auto all = answers.all();
        for (int i = 0; i < count; ++i) {
            EXPECT_EQ(all[static_cast<std::size_t>(i)].response.status, 200);
            EXPECT_EQ(id_of(all[static_cast<std::size_t>(i)]), i + 1) << "count " << count;
        }
        EXPECT_EQ(rig.validator->calls("fresh"), 1) << "count " << count;
        // Remembered from now on.
        rig.endpoint->handle(rig.ping("fresh", session, 99), answers.responder("later"));
        ASSERT_TRUE(answers.wait_for(static_cast<std::size_t>(count) + 1));
        EXPECT_EQ(rig.validator->calls("fresh"), 1);
    }
}

TEST(ResourceServerFlight, ARequestArrivingWhileTheWaitersAreHandedOnGoesAfterThem) {
    Rig rig;
    rig.validator->set("opener", alice(), false);
    const std::string session = rig.open_session("opener");
    rig.validator->set("fresh", alice(), true);
    Answers answers;
    // Once the first is answered, the third arrives: the second is still waiting to be handed on.
    rig.endpoint->handle(rig.ping("fresh", session, 1), answers.responder("1", [&] {
        rig.endpoint->handle(rig.ping("fresh", session, 3), answers.responder("3"));
    }));
    rig.endpoint->handle(rig.ping("fresh", session, 2), answers.responder("2"));
    ASSERT_TRUE(rig.validator->wait_held("fresh"));
    rig.validator->release("fresh");
    ASSERT_TRUE(answers.wait_for(3));
    EXPECT_EQ(answers.labels(), (std::vector<std::string>{"1", "2", "3"}));
    EXPECT_EQ(rig.validator->calls("fresh"), 1);
}

TEST(ResourceServerFlight, StopAnswersTheWaitingRequests503AndALaterVerdictChangesNothing) {
    Rig rig;
    rig.validator->set("fresh", alice(), true);
    Answers answers;
    for (int i = 1; i <= 3; ++i) {
        rig.endpoint->handle(rig.request("fresh", {}, json{{"jsonrpc", "2.0"}, {"id", i}, {"method", "ping"}}.dump()),
                             answers.responder(std::to_string(i)));
    }
    ASSERT_TRUE(rig.validator->wait_held("fresh"));
    rig.endpoint->stop();
    ASSERT_TRUE(answers.wait_for(3));
    for (const auto& answer : answers.all()) {
        EXPECT_EQ(answer.response.status, 503);
        EXPECT_FALSE(answer.response.headers.contains("WWW-Authenticate"));
    }
    rig.validator->release("fresh");
    EXPECT_EQ(answers.all().size(), 3u);
    // A request after stop() is answered at once, without a validation.
    rig.endpoint->handle(rig.ping("other", {}, 4), answers.responder("after"));
    ASSERT_TRUE(answers.wait_for(4));
    EXPECT_EQ(answers.all().back().response.status, 503);
    EXPECT_EQ(rig.validator->calls("other"), 0);
}

TEST(ResourceServerFlight, TwoTokensAreValidatedApartAndEachRequestGetsItsOwnVerdict) {
    Rig rig;
    rig.validator->set("good", alice(), true);
    rig.validator->set("bad", TokenRefusal::invalid, true);
    Answers answers;
    rig.endpoint->handle(rig.ping("bad", {}, 1), answers.responder("bad"));
    rig.endpoint->handle(rig.ping("good", {}, 2), answers.responder("good"));
    ASSERT_TRUE(rig.validator->wait_held("bad"));
    ASSERT_TRUE(rig.validator->wait_held("good"));
    rig.validator->release("good");
    ASSERT_TRUE(answers.wait_for(1));
    rig.validator->release("bad");
    ASSERT_TRUE(answers.wait_for(2));
    const auto all = answers.all();
    EXPECT_EQ(all[0].label, "good");
    EXPECT_NE(all[0].response.status, 401) << "past the gate (the session answers what a ping without one gets)";
    EXPECT_NE(all[0].response.status, 503);
    EXPECT_EQ(all[1].label, "bad");
    EXPECT_EQ(all[1].response.status, 401);
}

}  // namespace
