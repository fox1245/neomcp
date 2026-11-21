// The Client with an authorizer: a held request's clocks wait for the sign-in (the probe, the handshake and a call
// alike), the fixed token of StaticBearer, an authorizer's failure as the application sees it, closing while a call
// is held, one client at a time per authorizer (and the hand-over to the older transport, which takes it along), and
// what an authorizer reports.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "../conformance/everything.h"
#include "auth_rig.h"
#include "http_sse_server.h"
#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/server.h"
#include "rig.h"
#include "scripted_http_server.h"
#include "support.h"

namespace {

using auth_rig::ScriptedAuthorizer;
using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;

constexpr const char* first = "Bearer first";
constexpr const char* second = "Bearer second";
constexpr std::string_view modern = "2026-07-28";
constexpr auto short_timeout = std::chrono::milliseconds(300);

json discover_result() {
    return json{{"supportedVersions", json::array({std::string(modern)})},
                {"capabilities", json::object()},
                {"resultType", "complete"},
                {"ttlMs", 0},
                {"cacheScope", "private"}};
}

// A modern server that takes only requests that carry `accepted` (which the test may change) and refuses the rest
// with 401 or `refusal_status`.
class Peer {
public:
    explicit Peer(std::string accepted, int refusal_status = 401)
        : accepted_(std::move(accepted)), refusal_(refusal_status), server_([this](const Request& request) {
              return reply(request);
          }) {}

    void accept(std::string value) {
        std::lock_guard lock(mutex_);
        accepted_ = std::move(value);
    }
    // The next `count` requests are refused whatever they carry.
    void refuse_next(std::size_t count) {
        std::lock_guard lock(mutex_);
        refuse_next_ = count;
    }
    std::string url() const { return server_.url(); }
    ScriptedHttpServer& server() { return server_; }

private:
    http_test::Reply reply(const Request& request) {
        bool refused = false;
        {
            std::lock_guard lock(mutex_);
            if (refuse_next_ > 0) {
                --refuse_next_;
                refused = true;
            }
            refused = refused || request.field("Authorization").value_or(std::string()) != accepted_;
        }
        if (refused) {
            return http_test::empty_reply(refusal_, {{"WWW-Authenticate",
                                                      "Bearer error=\"insufficient_scope\", scope=\"mcp:write\""}});
        }
        if (request.rpc_method() == "server/discover") {
            return http_test::json_reply(200, http_test::result_for(request, discover_result()));
        }
        if (request.rpc_id().is_null()) return http_test::empty_reply(202);
        return http_test::json_reply(200, http_test::result_for(request, json{{"tools", json::array()}}));
    }

    std::mutex mutex_;
    std::string accepted_;
    int refusal_;
    std::size_t refuse_next_ = 0;
    ScriptedHttpServer server_;
};

neomcp::ClientOptions every_revision() {
    neomcp::ClientOptions options;
    options.client_info.name = "client-http-auth-test";
    options.client_info.version = "1.0.0";
    return options;
}

neomcp::HttpEndpoint authorized(const std::string& url, std::shared_ptr<neomcp::HttpAuthorizer> authorizer) {
    auto described = http_rig::endpoint(url);
    described.authorizer = std::move(authorizer);
    return described;
}

// ---- Clocks

// The probe, and later a call, are refused and held for longer than the request's timeout: neither times out, since
// what takes the time is the client's own sign-in.
TEST(ClientHttpAuth, AHeldRequestsClocksWaitForTheSignIn) {
    Peer peer(second);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    neomcp::ClientOptions options = every_revision();
    options.request_timeout = short_timeout;
    auto opening = std::async(std::launch::async, [&] {
        return neomcp::Client::open(std::move(options), authorized(peer.url(), authorizer));
    });
    const auth_rig::Finally release_opening{[&] { authorizer->fail_all(); }};
    ASSERT_TRUE(authorizer->wait_told(1));
    EXPECT_EQ(authorizer->told().front().request.method, "POST");
    std::this_thread::sleep_for(3 * short_timeout);
    authorizer->set(second, 1);
    authorizer->retry(0);
    auto client = opening.get();
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->revision, modern);

    peer.accept("Bearer third");
    auto listing = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    const auth_rig::Finally release_listing{[&] { authorizer->fail_all(); }};
    ASSERT_TRUE(authorizer->wait_told(2));
    std::this_thread::sleep_for(3 * short_timeout);
    authorizer->set("Bearer third", 2);
    authorizer->retry(1);
    const auto listed = listing.get();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(client->close());
}

// A request refused, held and sent again is accepted: the authorizer is told so once, of that request; a request that
// was never refused is not reported.
TEST(ClientHttpAuth, TheAuthorizerHearsOfARefusedRequestAccepted) {
    Peer peer(first);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    auto client = neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_TRUE(authorizer->accepted_requests().empty());
    peer.refuse_next(1);
    auto listing = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    const auth_rig::Finally release_listing{[&] { authorizer->fail_all(); }};
    ASSERT_TRUE(authorizer->wait_told(1));
    authorizer->retry(0);
    const auto listed = listing.get();
    ASSERT_TRUE(listed) << listed.error().message;
    const auto accepted = authorizer->accepted_requests();
    ASSERT_EQ(accepted.size(), 1u);
    EXPECT_EQ(accepted.front().unit, authorizer->told().front().request.unit);
    EXPECT_EQ(accepted.front().method, "POST");
    const auto again = client->request("tools/list");
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(authorizer->accepted_requests().size(), 1u);
    EXPECT_TRUE(client->close());
}

// ---- StaticBearer

TEST(ClientHttpAuth, StaticBearerSendsItsTokenAndFailsARefusal) {
    Peer peer("Bearer fixed-token");
    auto client = neomcp::Client::open(every_revision(),
                                       authorized(peer.url(), std::make_shared<neomcp::StaticBearer>("fixed-token")));
    ASSERT_TRUE(client) << client.error().message;
    for (const Request& request : peer.server().requests()) {
        EXPECT_EQ(request.fields_named("Authorization"), std::vector<std::string>{"Bearer fixed-token"});
    }
    peer.refuse_next(1);
    const auto refused = client->request("tools/list");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::authorization_required);
    EXPECT_EQ(refused.error().message.find("fixed-token"), std::string::npos);
    EXPECT_FALSE(refused.error().authorization);
    // The next call carries the same token and goes through.
    const auto listed = client->request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(client->close());
}

// What StaticBearer is told of a 403, through an authorizer that wraps it: the challenge as the server sent it.
class Wrapped final : public neomcp::HttpAuthorizer {
public:
    neomcp::Credential authorization(const neomcp::HttpRequestInfo& request) override {
        return inner_.authorization(request);
    }
    void challenge(const neomcp::HttpRequestInfo& request, int status, std::span<const std::string> challenges,
                   std::uint64_t generation, neomcp::ChallengeReply reply) override {
        {
            std::lock_guard lock(mutex_);
            statuses_.push_back(status);
            seen_.assign(challenges.begin(), challenges.end());
        }
        inner_.challenge(request, status, challenges, generation, std::move(reply));
    }
    std::vector<int> statuses() const {
        std::lock_guard lock(mutex_);
        return statuses_;
    }
    std::vector<std::string> seen() const {
        std::lock_guard lock(mutex_);
        return seen_;
    }

private:
    neomcp::StaticBearer inner_{"fixed-token"};
    mutable std::mutex mutex_;
    std::vector<int> statuses_;
    std::vector<std::string> seen_;
};

TEST(ClientHttpAuth, StaticBearerIsToldA403sChallengeAsSent) {
    Peer peer("Bearer fixed-token", 403);
    auto authorizer = std::make_shared<Wrapped>();
    auto client = neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    ASSERT_TRUE(client) << client.error().message;
    peer.refuse_next(1);
    const auto refused = client->request("tools/list");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::authorization_required);
    EXPECT_EQ(authorizer->statuses(), std::vector<int>{403});
    EXPECT_EQ(authorizer->seen(),
              std::vector<std::string>{"Bearer error=\"insufficient_scope\", scope=\"mcp:write\""});
    EXPECT_TRUE(client->close());
}

TEST(ClientHttpAuth, AProbeRefusedWithAFixedTokenStopsWithoutAHandshake) {
    Peer peer("Bearer another-token");
    auto client = neomcp::Client::open(every_revision(),
                                       authorized(peer.url(), std::make_shared<neomcp::StaticBearer>("fixed-token")));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_required);
    for (const Request& request : peer.server().requests()) EXPECT_NE(request.rpc_method(), "initialize");
}

// ---- Failures as the application sees them

TEST(ClientHttpAuth, AnAuthorizersFailureNamesItsProblemAndNoSecret) {
    Peer peer(first);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    auto client = neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    ASSERT_TRUE(client) << client.error().message;
    peer.accept(second);
    auto listing = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    const auth_rig::Finally release_listing{[&] { authorizer->fail_all(); }};
    ASSERT_TRUE(authorizer->wait_told(1));
    authorizer->fail(0, auth_rig::failed_with(AuthorizationProblem::issuer_mismatch));
    const auto listed = listing.get();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::authorization_failed);
    EXPECT_EQ(neomcp::to_string(listed.error().kind), "authorization_failed");
    ASSERT_TRUE(listed.error().authorization);
    EXPECT_EQ(listed.error().authorization->problem, AuthorizationProblem::issuer_mismatch);
    EXPECT_EQ(listed.error().authorization->issuer, "https://issuer.test");
    EXPECT_EQ(listed.error().message.find("Bearer"), std::string::npos);
    EXPECT_FALSE(listed.error().may_have_run);
    EXPECT_TRUE(client->close());
}

TEST(ClientHttpAuth, AnAuthorizerThatFailsTheProbeFailsOpeningWithItsProblem) {
    Peer peer(second);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    auto opening = std::async(std::launch::async, [&] {
        return neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    });
    const auth_rig::Finally release_opening{[&] { authorizer->fail_all(); }};
    ASSERT_TRUE(authorizer->wait_told(1));
    authorizer->fail(0, auth_rig::failed_with(AuthorizationProblem::discovery_failed));
    const auto client = opening.get();
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::discovery_failed);
    for (const Request& request : peer.server().requests()) EXPECT_NE(request.rpc_method(), "initialize");
}

// ---- Closing, sharing

TEST(ClientHttpAuth, ClosingTheClientEndsAHeldCallClosed) {
    Peer peer(first);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    auto client = neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    ASSERT_TRUE(client) << client.error().message;
    peer.accept(second);
    auto listing = std::async(std::launch::async, [&] { return client->request("tools/list"); });
    const auth_rig::Finally release_listing{[&] { authorizer->fail_all(); }};
    ASSERT_TRUE(authorizer->wait_told(1));
    const std::size_t before = peer.server().count();
    EXPECT_TRUE(client->close());
    const auto listed = listing.get();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::closed);
    EXPECT_TRUE(authorizer->wait_detached());
    authorizer->set(second, 1);
    authorizer->retry(0);
    EXPECT_FALSE(peer.server().wait_for(before + 1, std::chrono::milliseconds(300)));
}

TEST(ClientHttpAuth, AnAuthorizerServesOneClientAtATime) {
    Peer peer("Bearer fixed-token");
    auto authorizer = std::make_shared<neomcp::StaticBearer>("fixed-token");
    auto one = neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    ASSERT_TRUE(one) << one.error().message;
    auto two = neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    ASSERT_FALSE(two);
    EXPECT_EQ(two.error().kind, ClientErrorKind::invalid_options);
    EXPECT_TRUE(one->close());
    auto three = neomcp::Client::open(every_revision(), authorized(peer.url(), authorizer));
    ASSERT_TRUE(three) << three.error().message;
    EXPECT_TRUE(three->close());
}

// Streamable HTTP is refused, then the older transport is spoken with the same authorizer.
TEST(ClientHttpAuth, TheHandOverToTheOlderTransportTakesTheAuthorizerAlong) {
    neomcp::Server server(conformance::server_options());
    conformance::register_everything(server);
    http_sse_test::HttpSseServer sse(server);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    auto client = neomcp::Client::open(every_revision(), authorized(sse.url(), authorizer));
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(client->verdict()->transport, neomcp::profile::Transport::http_sse);
    EXPECT_EQ(authorizer->attaches(), 2);
    EXPECT_EQ(authorizer->detaches(), 1);
    EXPECT_TRUE(client->close());
    EXPECT_EQ(authorizer->detaches(), 2);
}

// ---- Reports

class Reporting final : public neomcp::HttpAuthorizer {
public:
    void attach(neomcp::AuthorizationHost& host) override { host.diagnostic("a scope was dropped"); }
    neomcp::Credential authorization(const neomcp::HttpRequestInfo&) override { return {}; }
    void challenge(const neomcp::HttpRequestInfo&, int, std::span<const std::string>, std::uint64_t,
                   neomcp::ChallengeReply) override {}
};

TEST(ClientHttpAuth, AnAuthorizersReportReachesTheApplication) {
    Peer peer("");
    std::mutex mutex;
    std::vector<neomcp::Diagnostic> reports;
    neomcp::ClientOptions options = every_revision();
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (const auto* report = std::get_if<neomcp::Diagnostic>(&diagnostic)) {
            std::lock_guard lock(mutex);
            reports.push_back(*report);
        }
    };
    auto client = neomcp::Client::open(std::move(options), authorized(peer.url(), std::make_shared<Reporting>()));
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_TRUE(client->close());
    std::lock_guard lock(mutex);
    bool found = false;
    for (const auto& report : reports) {
        found = found || (report.kind == neomcp::DiagnosticKind::authorization && report.message == "a scope was dropped");
    }
    EXPECT_TRUE(found);
}

}  // namespace
