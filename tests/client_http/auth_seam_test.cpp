// The authorization seam of both HTTP transports against scripted peers: a refused request is held and told to the
// authorizer once per set of credentials, sent again as it was (same bytes, same id) on retry, ended on fail; a reply
// reaches only the requests of its own sign-in; closing detaches the authorizer; and the token goes nowhere but the
// `Authorization` field of requests to the endpoint's origin.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "auth_rig.h"
#include "neomcp/client/http.h"
#include "neomcp/profile.h"
#include "rig.h"
#include "scripted_http_server.h"

namespace {

using auth_rig::ScriptedAuthorizer;
using http_rig::Driven;
using http_rig::endpoint;
using http_rig::events_of;
using http_rig::messages_of;
using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::AuthorizationDenied;
using neomcp::AuthorizationHold;
using neomcp::AuthorizationProblem;
using neomcp::StreamEnded;

constexpr const char* first = "Bearer first";
constexpr const char* second = "Bearer second";

std::string ping(std::uint64_t id) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"ping"})";
}

http_test::Reply refusal(int status = 401) {
    return http_test::empty_reply(status, {{"WWW-Authenticate", "Bearer realm=\"mcp\""}});
}

http_test::Reply answer(const Request& request) {
    return http_test::json_reply(200, http_test::result_for(request, json::object()));
}

// Answers a request that carries `Authorization: <accepted>` and refuses every other one with 401.
ScriptedHttpServer accepting(std::string accepted) {
    return ScriptedHttpServer([accepted](const Request& request) {
        if (request.field("Authorization") == accepted) return answer(request);
        return refusal();
    });
}

neomcp::HttpEndpoint authorized(const std::string& url, std::shared_ptr<neomcp::HttpAuthorizer> authorizer) {
    auto described = endpoint(url);
    described.authorizer = std::move(authorizer);
    return described;
}

std::vector<bool> holds_of(const std::vector<http_rig::Delivered>& seen) {
    std::vector<bool> begins;
    for (const auto& hold : events_of<AuthorizationHold>(seen)) begins.push_back(hold.begin);
    return begins;
}

std::map<std::uint64_t, std::vector<Request>> by_id(const std::vector<Request>& requests) {
    std::map<std::uint64_t, std::vector<Request>> found;
    for (const Request& request : requests) found[request.rpc_id().get<std::uint64_t>()].push_back(request);
    return found;
}

// An authorizer that renews its credentials and answers retry at once, inside challenge().
class RenewingAuthorizer final : public neomcp::HttpAuthorizer {
public:
    neomcp::Credential authorization(const neomcp::HttpRequestInfo&) override {
        return neomcp::Credential{generation_ == 0 ? first : second, generation_};
    }
    void challenge(const neomcp::HttpRequestInfo&, int, std::span<const std::string>, std::uint64_t,
                   neomcp::ChallengeReply reply) override {
        ++generation_;
        reply.retry();
    }

private:
    std::uint64_t generation_ = 0;
};

// ---- One sign-in for the requests refused together

TEST(ClientHttpAuth, TenRequestsRefusedTogetherShareOneSignIn) {
    constexpr std::uint64_t count = 10;
    auto server = accepting(second);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven driven(authorized(server.url(), authorizer));
    for (std::uint64_t unit = 1; unit <= count; ++unit) driven.post(unit, ping(unit));
    for (std::uint64_t unit = 1; unit <= count; ++unit) {
        ASSERT_TRUE(driven.recorder().until_count<AuthorizationHold>(unit, 1)) << unit;
    }
    ASSERT_TRUE(authorizer->wait_told(1));
    ASSERT_EQ(authorizer->told().size(), 1u);  // one sign-in for all ten
    EXPECT_EQ(authorizer->told().front().status, 401);
    EXPECT_EQ(authorizer->told().front().generation, 0u);
    EXPECT_EQ(authorizer->told().front().challenges, std::vector<std::string>{"Bearer realm=\"mcp\""});

    authorizer->set(second, 1);
    authorizer->retry(0);
    for (std::uint64_t unit = 1; unit <= count; ++unit) {
        const auto seen = driven.recorder().until_end(unit);
        EXPECT_EQ(messages_of(seen).size(), 1u) << unit;
        EXPECT_EQ(holds_of(seen), (std::vector<bool>{true, false})) << unit;
        EXPECT_TRUE(events_of<AuthorizationDenied>(seen).empty()) << unit;
    }
    EXPECT_EQ(authorizer->told().size(), 1u);
    ASSERT_TRUE(server.wait_for(2 * count));
    const auto requests = server.requests();
    EXPECT_EQ(requests.size(), 2 * count);
    // Each request went out twice: refused with the first value, then the same bytes, under the same id, with the
    // second; never with the token anywhere but its one field.
    for (const auto& [id, sent] : by_id(requests)) {
        ASSERT_EQ(sent.size(), 2u) << id;
        EXPECT_EQ(sent[0].body, sent[1].body) << id;
        EXPECT_EQ(sent[0].fields_named("Authorization"), std::vector<std::string>{first}) << id;
        EXPECT_EQ(sent[1].fields_named("Authorization"), std::vector<std::string>{second}) << id;
        EXPECT_EQ(sent[1].target, "/mcp") << id;
        EXPECT_EQ(sent[1].target.find("second"), std::string::npos) << id;
    }
}

// ---- Generations

// A refusal that comes after newer credentials exist is sent again with them at once: the authorizer is not asked.
TEST(ClientHttpAuth, ARefusalOfOlderCredentialsIsSentAgainAtOnce) {
    auto late = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([late](const Request& request) {
        if (request.field("Authorization") == second) return answer(request);
        if (request.rpc_id() == json(2)) {
            http_test::Reply waited;
            waited.wait(late);
            for (auto& step : refusal().steps) waited.steps.push_back(step);
            return waited;
        }
        return refusal();
    });
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven driven(authorized(server.url(), authorizer));
    driven.post(1, ping(1));
    driven.post(2, ping(2));
    ASSERT_TRUE(server.wait_for(2));
    ASSERT_TRUE(authorizer->wait_told(1));
    authorizer->set(second, 1);
    authorizer->retry(0);
    EXPECT_EQ(messages_of(driven.recorder().until_end(1)).size(), 1u);
    // The first request went out again with the credentials of generation 1; now the second one's refusal arrives.
    late->open();
    const auto seen = driven.recorder().until_end(2);
    EXPECT_EQ(messages_of(seen).size(), 1u);
    EXPECT_TRUE(holds_of(seen).empty());  // never held: sent again at once
    EXPECT_EQ(authorizer->told().size(), 1u);
    const auto sent = by_id(server.requests())[2];
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1].field("Authorization"), second);
    EXPECT_EQ(sent[0].body, sent[1].body);
}

// The authorizer renewed its credentials on its own while a sign-in for the older ones waits: a refusal of the newer
// ones opens a sign-in of its own, and each reply reaches only the requests of its own.
TEST(ClientHttpAuth, AReplyReachesOnlyTheRequestsOfItsSignIn) {
    const std::string third = "Bearer third";
    auto server = accepting(third);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven driven(authorized(server.url(), authorizer));
    driven.post(1, ping(1));
    ASSERT_TRUE(authorizer->wait_told(1));
    authorizer->set(second, 1);
    driven.post(2, ping(2));
    ASSERT_TRUE(authorizer->wait_told(2));
    EXPECT_EQ(authorizer->told()[0].generation, 0u);
    EXPECT_EQ(authorizer->told()[1].generation, 1u);
    EXPECT_EQ(authorizer->told()[1].request.unit, 2u);

    authorizer->fail(0, auth_rig::failed_with(AuthorizationProblem::prompt_cancelled));
    const auto ended = events_of<AuthorizationDenied>(driven.recorder().until_end(1));
    ASSERT_EQ(ended.size(), 1u);
    ASSERT_TRUE(ended.front().failure.authorization);
    EXPECT_EQ(ended.front().failure.authorization->problem, AuthorizationProblem::prompt_cancelled);

    authorizer->set(third, 2);
    authorizer->retry(1);
    const auto seen = driven.recorder().until_end(2);
    EXPECT_EQ(messages_of(seen).size(), 1u);
    EXPECT_TRUE(events_of<AuthorizationDenied>(seen).empty());
    EXPECT_EQ(holds_of(seen), (std::vector<bool>{true, false}));
}

// ---- What ends a hold

TEST(ClientHttpAuth, FailEndsEveryHeldRequestWithTheAuthorizersError) {
    auto server = accepting(second);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven driven(authorized(server.url(), authorizer));
    driven.post(1, ping(1));
    driven.post(2, ping(2));
    ASSERT_TRUE(driven.recorder().until_count<AuthorizationHold>(1, 1));
    ASSERT_TRUE(driven.recorder().until_count<AuthorizationHold>(2, 1));
    ASSERT_EQ(authorizer->told().size(), 1u);
    authorizer->fail(0, auth_rig::failed_with(AuthorizationProblem::token_request_failed));
    for (const neomcp::UnitId unit : {neomcp::UnitId{1}, neomcp::UnitId{2}}) {
        const auto ended = events_of<AuthorizationDenied>(driven.recorder().until_end(unit));
        ASSERT_EQ(ended.size(), 1u) << unit;
        EXPECT_EQ(ended.front().status, 401);
        EXPECT_EQ(ended.front().failure.kind, neomcp::CallFailureKind::authorization_failed);
        ASSERT_TRUE(ended.front().failure.authorization);
        EXPECT_EQ(ended.front().failure.authorization->problem, AuthorizationProblem::token_request_failed);
    }
    EXPECT_EQ(server.count(), 2u);  // nothing was sent again
}

TEST(ClientHttpAuth, ClosingWhileARequestIsHeldDetachesTheAuthorizer) {
    auto server = accepting(second);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    {
        Driven driven(authorized(server.url(), authorizer));
        driven.post(1, ping(1));
        ASSERT_TRUE(driven.recorder().until_count<AuthorizationHold>(1, 1));
        ASSERT_TRUE(authorizer->wait_told(1));
        EXPECT_EQ(authorizer->attaches(), 1);
        driven.close();
        EXPECT_EQ(authorizer->detaches(), 1);
        // A reply that outlived its transport changes nothing.
        authorizer->set(second, 1);
        authorizer->retry(0);
        EXPECT_FALSE(server.wait_for(2, std::chrono::milliseconds(300)));
    }
    EXPECT_EQ(server.count(), 1u);
    // The authorizer is free for another transport.
    Driven again(authorized(server.url(), authorizer));
    EXPECT_EQ(again.problem(), "");
    EXPECT_EQ(authorizer->attaches(), 2);
}

// What the refused exchange still carries (here a JSON-RPC error with the request's id) never counts as the answer of
// the request sent again on the same unit.
TEST(ClientHttpAuth, WhatARefusalStillCarriesNeverReachesTheResentRequest) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.field("Authorization") == second) return answer(request);
        return http_test::json_reply(401, http_test::error_for(request, -32001, "refused"),
                                     {{"WWW-Authenticate", "Bearer"}});
    });
    Driven driven(authorized(server.url(), std::make_shared<RenewingAuthorizer>()));
    driven.post(1, ping(1));
    const auto seen = driven.recorder().until_end(1);
    const auto messages = messages_of(seen);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_TRUE(json::parse(messages.front()).contains("result"));
    EXPECT_EQ(server.count(), 2u);
}

TEST(ClientHttpAuth, ARequestRefusedMoreOftenThanItsBoundEnds) {
    ScriptedHttpServer server([](const Request&) { return refusal(); });
    auto described = authorized(server.url(), std::make_shared<RenewingAuthorizer>());
    described.max_authorizations_per_request = 2;
    Driven driven(described);
    driven.post(1, ping(1));
    const auto ended = events_of<AuthorizationDenied>(driven.recorder().until_end(1));
    ASSERT_EQ(ended.size(), 1u);
    ASSERT_TRUE(ended.front().failure.authorization);
    EXPECT_EQ(ended.front().failure.authorization->problem, AuthorizationProblem::attempts_exceeded);
    EXPECT_EQ(server.count(), 3u);  // held twice, refused a third time
}

// A request refused as often as its bound is held each time and served afterwards; one refusal more ends it, and the
// server was asked once more than the bound.
TEST(ClientHttpAuth, ARequestIsHeldAsOftenAsItsBoundAndNoMore) {
    for (const std::size_t bound : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        for (const std::size_t refusals : {bound - 1, bound, bound + 1}) {
            SCOPED_TRACE("bound " + std::to_string(bound) + ", refused " + std::to_string(refusals) + " times");
            auto served = std::make_shared<std::atomic<std::size_t>>(0);
            ScriptedHttpServer server([served, refusals](const Request& request) {
                if (served->fetch_add(1) < refusals) return refusal();
                return answer(request);
            });
            auto described = authorized(server.url(), std::make_shared<RenewingAuthorizer>());
            described.max_authorizations_per_request = bound;
            Driven driven(described);
            driven.post(1, ping(1));
            const auto seen = driven.recorder().until_end(1);
            if (refusals <= bound) {
                EXPECT_EQ(messages_of(seen).size(), 1u);
                EXPECT_EQ(server.count(), refusals + 1);
            } else {
                const auto ended = events_of<AuthorizationDenied>(seen);
                ASSERT_EQ(ended.size(), 1u);
                ASSERT_TRUE(ended.front().failure.authorization);
                EXPECT_EQ(ended.front().failure.authorization->problem, AuthorizationProblem::attempts_exceeded);
                EXPECT_EQ(server.count(), bound + 1);
            }
        }
    }
}

TEST(ClientHttpAuth, AnAuthorizerServesOneTransportAtATime) {
    auto server = accepting(first);
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven one(authorized(server.url(), authorizer));
    ASSERT_EQ(one.problem(), "");
    auto made = neomcp::detail::http_client::make_http_transport(authorized(server.url(), authorizer), {});
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error().kind, neomcp::ClientErrorKind::invalid_options);
}

// ---- Where the token goes

TEST(ClientHttpAuth, NoTokenGoesToAPlainHttpEndpointElsewhere) {
    ScriptedHttpServer proxy([](const Request&) { return refusal(); });
    auto authorizer = std::make_shared<ScriptedAuthorizer>("Bearer secret", 0);
    auto described = authorized("http://mcp.example.test/mcp", authorizer);
    described.proxy.mode = neomcp::ProxyMode::named;
    described.proxy.url = proxy.url("");
    Driven driven(described);
    driven.post(1, ping(1));
    const auto ended = events_of<AuthorizationDenied>(driven.recorder().until_end(1));
    ASSERT_EQ(ended.size(), 1u);
    ASSERT_TRUE(ended.front().failure.authorization);
    EXPECT_EQ(ended.front().failure.authorization->problem, AuthorizationProblem::insecure_endpoint);
    ASSERT_TRUE(proxy.wait_for(1));
    EXPECT_FALSE(proxy.requests().front().field("Authorization"));
    EXPECT_EQ(authorizer->asked(), 0u);
    EXPECT_TRUE(authorizer->told().empty());
}

TEST(ClientHttpAuth, ARedirectToAnotherOriginTakesNoToken) {
    ScriptedHttpServer elsewhere([](const Request& request) { return answer(request); });
    const std::string target = elsewhere.url("/mcp");
    ScriptedHttpServer server([target](const Request& request) {
        if (request.target == "/same") return answer(request);
        if (request.target == "/mcp" && request.rpc_id() == json(1)) {
            return http_test::empty_reply(307, {{"Location", "/same"}});
        }
        return http_test::empty_reply(307, {{"Location", target}});
    });
    Driven driven(authorized(server.url(), std::make_shared<ScriptedAuthorizer>("Bearer secret", 0)));
    driven.post(1, ping(1));  // to the same origin: followed, with the token
    EXPECT_EQ(messages_of(driven.recorder().until_end(1)).size(), 1u);
    driven.post(2, ping(2));  // to another origin: not followed
    EXPECT_TRUE(messages_of(driven.recorder().until_end(2)).empty());
    EXPECT_EQ(elsewhere.count(), 0u);
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3u);
    for (const Request& request : requests) {
        EXPECT_EQ(request.fields_named("Authorization"), std::vector<std::string>{"Bearer secret"});
        EXPECT_EQ(request.target.find("secret"), std::string::npos);
    }
}

TEST(ClientHttpAuth, TheEnginesFieldsNeverCarryAnAuthorization) {
    ScriptedHttpServer server([](const Request& request) { return answer(request); });
    Driven driven(authorized(server.url(), std::make_shared<ScriptedAuthorizer>("Bearer real", 0)));
    driven.post(1, ping(1), {{"Authorization", "Bearer from-the-engine"}, {"authorization", "Basic x"}});
    driven.recorder().until_end(1);
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_EQ(server.requests().front().fields_named("Authorization"), std::vector<std::string>{"Bearer real"});
}

// ---- The stream of the connection

TEST(ClientHttpAuth, AStreamOfTheConnectionRefusedIsHeldAndOpenedAnew) {
    auto open = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([open](const Request& request) {
        if (request.method != "GET") return answer(request);
        if (request.field("Authorization") != second) return refusal();
        http_test::Reply stream = http_test::event_stream_reply({": open\n\n"});
        stream.wait(open);
        return stream;
    });
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven driven(authorized(server.url(), authorizer));
    driven.open_stream({}, neomcp::profile::builtin("2025-11-25"));
    ASSERT_TRUE(authorizer->wait_told(1));
    EXPECT_EQ(authorizer->told().front().request.method, "GET");
    EXPECT_EQ(authorizer->told().front().request.unit, neomcp::connection_unit);
    authorizer->set(second, 1);
    authorizer->retry(0);
    ASSERT_TRUE(server.wait_for(2));
    const auto requests = server.requests();
    EXPECT_EQ(requests[1].method, "GET");
    EXPECT_EQ(requests[1].field("Authorization"), second);
    open->open();
}

// The bound counts refusals in a row: a stream of the connection whose credentials expire again after it was
// accepted is held again, however often that happens in the transport's life.
TEST(ClientHttpAuth, ARefusalAfterAnAcceptedAnswerStartsTheCountAgain) {
    auto gets = std::make_shared<std::atomic<int>>(0);
    auto open = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([gets, open](const Request& request) {
        if (request.method != "GET") return answer(request);
        const int index = ++*gets;
        if (index == 1 || index == 3) return refusal();
        http_test::Reply stream = http_test::event_stream_reply({": open\n\n"});
        if (index != 2) stream.wait(open);  // the second ends at once, and the stream is opened again
        return stream;
    });
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    auto described = authorized(server.url(), authorizer);
    described.max_authorizations_per_request = 1;
    described.retry_delay = std::chrono::milliseconds(10);
    Driven driven(described);
    driven.open_stream({}, neomcp::profile::builtin("2025-11-25"));
    ASSERT_TRUE(authorizer->wait_told(1));
    authorizer->set(second, 1);
    authorizer->retry(0);
    ASSERT_TRUE(authorizer->wait_told(2));  // the third GET was held, not given up
    authorizer->set("Bearer third", 2);
    authorizer->retry(1);
    ASSERT_TRUE(server.wait_for(4));
    EXPECT_EQ(server.requests()[3].field("Authorization"), "Bearer third");
    open->open();
}

// ---- The older transport of events over HTTP

neomcp::detail::http_client::HttpTransportSettings older() {
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {neomcp::profile::builtin("2024-11-05")};
    return settings;
}

// The stream's GET, and the messages that wait for it, are held; on retry a new GET opens the connection.
TEST(ClientHttpAuth, EventsAStreamRefusedIsHeldWithTheMessagesThatWait) {
    auto open = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([open](const Request& request) {
        if (request.field("Authorization") != second) return refusal();
        if (request.method == "POST") return http_test::empty_reply(202);
        http_test::Reply stream = http_test::event_stream_reply({"event: endpoint\ndata: /messages\n\n"});
        stream.wait(open);
        return stream;
    });
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven driven(authorized(server.url(), authorizer), older(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, ping(1));
    ASSERT_TRUE(authorizer->wait_told(1));
    ASSERT_TRUE(driven.recorder().until_count<AuthorizationHold>(1, 1));
    authorizer->set(second, 1);
    authorizer->retry(0);
    const auto seen = driven.recorder().until_end(1);
    EXPECT_FALSE(events_of<StreamEnded>(seen).empty());
    EXPECT_EQ(holds_of(seen), (std::vector<bool>{true, false}));
    ASSERT_TRUE(server.wait_for(3));
    const auto requests = server.requests();
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[1].method, "GET");
    EXPECT_EQ(requests[1].field("Authorization"), second);
    EXPECT_EQ(requests[2].method, "POST");
    EXPECT_EQ(requests[2].target, "/messages");
    EXPECT_EQ(requests[2].field("Authorization"), second);
    open->open();
}

TEST(ClientHttpAuth, EventsAMessageRefusedIsSentAgainAsItWas) {
    auto open = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([open](const Request& request) {
        if (request.method == "POST") {
            if (request.field("Authorization") != second) return refusal();
            return http_test::empty_reply(202);
        }
        http_test::Reply stream = http_test::event_stream_reply({"event: endpoint\ndata: /messages\n\n"});
        stream.wait(open);
        return stream;
    });
    auto authorizer = std::make_shared<ScriptedAuthorizer>(first, 0);
    Driven driven(authorized(server.url(), authorizer), older(), http_rig::Kind::events);
    driven.post(1, ping(1));
    ASSERT_TRUE(authorizer->wait_told(1));
    EXPECT_EQ(authorizer->told().front().request.method, "POST");
    authorizer->set(second, 1);
    authorizer->retry(0);
    const auto seen = driven.recorder().until_end(1);
    EXPECT_FALSE(events_of<StreamEnded>(seen).empty());
    EXPECT_EQ(holds_of(seen), (std::vector<bool>{true, false}));
    std::vector<Request> posts;
    for (const Request& request : server.requests()) {
        if (request.method == "POST") posts.push_back(request);
    }
    ASSERT_EQ(posts.size(), 2u);
    EXPECT_EQ(posts[0].body, posts[1].body);
    EXPECT_EQ(posts[1].field("Authorization"), second);
    open->open();
}

TEST(ClientHttpAuth, EventsAnEndpointOfAnotherOriginTakesNoToken) {
    ScriptedHttpServer elsewhere([](const Request&) { return http_test::empty_reply(202); });
    const std::string target = elsewhere.url("/messages");
    auto open = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([open, target](const Request&) {
        http_test::Reply stream = http_test::event_stream_reply({"event: endpoint\ndata: " + target + "\n\n"});
        stream.wait(open);
        return stream;
    });
    Driven driven(authorized(server.url(), std::make_shared<ScriptedAuthorizer>("Bearer secret", 0)), older(),
                  http_rig::Kind::events);
    driven.post(1, ping(1));
    driven.recorder().until_end(1);
    EXPECT_EQ(elsewhere.count(), 0u);
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_EQ(server.requests().front().field("Authorization"), "Bearer secret");
    open->open();
}

}  // namespace
