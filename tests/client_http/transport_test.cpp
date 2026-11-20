// The Streamable HTTP transport against a scripted peer that records the bytes of every request: what a POST carries,
// how each kind of answer is read and bounded, redirects, the authorizer's seam, proxies, the session of a handshake,
// and the failures of a connection.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "client_http/streamable.h"
#include "neomcp/client/http.h"
#include "neomcp/profile.h"
#include "rig.h"
#include "scripted_http_server.h"

namespace {

using http_rig::Delivered;
using http_rig::Driven;
using http_rig::endpoint;
using http_rig::events_of;
using http_rig::messages_of;
using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::HttpHeader;
using neomcp::ResponseHead;
using neomcp::SendFailed;
using neomcp::StreamEnded;
using neomcp::StreamLost;

constexpr const char* ping_line = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";

ScriptedHttpServer answering(json body = json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}}) {
    return ScriptedHttpServer([body](const Request&) { return http_test::json_reply(200, body); });
}

std::optional<ResponseHead> head_of(const std::vector<Delivered>& seen) {
    const auto heads = events_of<ResponseHead>(seen);
    if (heads.empty()) return std::nullopt;
    return heads.front();
}

bool ended(const std::vector<Delivered>& seen) { return !events_of<StreamEnded>(seen).empty(); }

// ---- What a POST carries

TEST(ClientHttp, APostCarriesTheMediaTypesAndTheEnginesFields) {
    auto server = answering();
    Driven driven(endpoint(server.url()));
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, ping_line, {{"Mcp-Method", "ping"}, {"MCP-Protocol-Version", "2026-07-28"}});
    const auto seen = driven.recorder().until_end(1);
    ASSERT_TRUE(server.wait_for(1));
    const Request request = server.requests().front();
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.target, "/mcp");
    EXPECT_EQ(request.body, ping_line);
    EXPECT_EQ(request.field("Content-Type"), "application/json");
    EXPECT_EQ(request.field("Accept"), "application/json, text/event-stream");
    EXPECT_EQ(request.field("Mcp-Method"), "ping");
    EXPECT_EQ(request.field("MCP-Protocol-Version"), "2026-07-28");
    EXPECT_FALSE(request.field("Expect"));
    EXPECT_FALSE(request.field("Authorization"));
    ASSERT_EQ(messages_of(seen).size(), 1u);
    EXPECT_EQ(json::parse(messages_of(seen).front()), (json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}}));
    EXPECT_TRUE(ended(seen));
}

TEST(ClientHttp, ALargeBodyGoesWithoutAnExpectation) {
    auto server = answering();
    Driven driven(endpoint(server.url()));
    const std::string large = R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"pad":")" +
                              std::string(std::size_t{2} << 20, 'x') + "\"}}";
    driven.post(1, large);
    driven.recorder().until_end(1);
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_FALSE(server.requests().front().field("Expect"));
    EXPECT_EQ(server.requests().front().body.size(), large.size());
}

TEST(ClientHttp, HeaderValuesReachThePeerAsGiven) {
    auto server = answering();
    Driven driven(endpoint(server.url()));
    const std::string non_ascii = "caf\xC3\xA9";
    driven.post(1, ping_line,
                {{"X-Empty", ""}, {"X-Spaces", "   "}, {"X-Tab", "\t"}, {"X-Inner", "us west 1"},
                 {"X-Encoded", "=?base64?SGVsbG8=?="}, {"X-Raw", non_ascii}});
    driven.recorder().until_end(1);
    ASSERT_TRUE(server.wait_for(1));
    const Request request = server.requests().front();
    // Present with an empty value: libcurl's form for an empty field, not the bare `Name:` that would remove it.
    EXPECT_EQ(request.fields_named("X-Empty"), std::vector<std::string>{""});
    EXPECT_EQ(request.fields_named("X-Spaces"), std::vector<std::string>{""});
    EXPECT_EQ(request.fields_named("X-Tab"), std::vector<std::string>{""});
    EXPECT_EQ(request.field("X-Inner"), "us west 1");
    EXPECT_EQ(request.field("X-Encoded"), "=?base64?SGVsbG8=?=");
    EXPECT_EQ(request.field("X-Raw"), non_ascii);
}

TEST(ClientHttp, AFieldThatWouldSplitTheHeadIsNeverSent) {
    auto server = answering();
    Driven driven(endpoint(server.url()));
    driven.post(1, ping_line, {{"X-Bad", "a\r\nInjected: yes"}});
    const auto seen = driven.recorder().until_end(1);
    EXPECT_EQ(events_of<SendFailed>(seen).size(), 1u);
    driven.post(2, ping_line);
    driven.recorder().until_end(2);
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_EQ(server.count(), 1u);  // only the second request went out
    EXPECT_FALSE(server.requests().front().field("Injected"));
}

// ---- How answers are read

TEST(ClientHttp, AcceptedAnswersCarryNoType) {
    ScriptedHttpServer server([](const Request&) { return http_test::empty_reply(202); });
    Driven driven(endpoint(server.url()));
    driven.post(1, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", {},
                neomcp::OutgoingKind::notification);
    const auto seen = driven.recorder().until_end(1);
    const auto head = head_of(seen);
    ASSERT_TRUE(head);
    EXPECT_EQ(head->status, 202);
    EXPECT_FALSE(head->media_type);
    EXPECT_TRUE(messages_of(seen).empty());
    EXPECT_TRUE(ended(seen));
}

TEST(ClientHttp, TheTypeOfAnAnswerIsReadFromItsHead) {
    struct Case {
        http_test::Fields fields;
        std::optional<std::string> media;
    };
    const std::vector<Case> cases{
        {{{"Content-Type", "application/json; charset=utf-8"}}, std::string("application/json")},
        {{{"Content-Type", "text/html"}}, std::string("text/html")},
        {{{"Content-Type", "application/json"}, {"Content-Type", "application/json"}}, std::string()},
        {{{"Content-Type", "application/"}}, std::string()},
        {{}, std::string()},
    };
    for (const Case& one : cases) {
        ScriptedHttpServer server([&one](const Request&) {
            return http_test::raw_reply(200, one.fields, R"({"jsonrpc":"2.0","id":1,"result":{}})");
        });
        Driven driven(endpoint(server.url()));
        driven.post(1, ping_line);
        const auto head = head_of(driven.recorder().until_end(1));
        ASSERT_TRUE(head);
        EXPECT_EQ(head->media_type, one.media);
    }
}

TEST(ClientHttp, AnErrorStatusWithAJsonBodyDeliversTheBody) {
    const json refusal{{"jsonrpc", "2.0"}, {"id", 1}, {"error", {{"code", -32022}, {"message", "no"}}}};
    ScriptedHttpServer server([&](const Request&) { return http_test::json_reply(400, refusal); });
    Driven driven(endpoint(server.url()));
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    ASSERT_TRUE(head_of(seen));
    EXPECT_EQ(head_of(seen)->status, 400);
    EXPECT_EQ(messages_of(seen), std::vector<std::string>{refusal.dump()});
}

TEST(ClientHttp, AnErrorStatusWithAnotherBodyDeliversNoMessage) {
    ScriptedHttpServer server([](const Request&) {
        return http_test::raw_reply(404, {{"Content-Type", "text/html"}}, "<html>not here</html>");
    });
    Driven driven(endpoint(server.url()));
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    EXPECT_EQ(head_of(seen)->status, 404);
    EXPECT_TRUE(messages_of(seen).empty());
    EXPECT_TRUE(ended(seen));
}

TEST(ClientHttp, AJsonBodyOverTheBoundIsCutAndTheExchangeEnds) {
    const std::string big = R"({"jsonrpc":"2.0","id":1,"result":{"pad":")" + std::string(4096, 'x') + "\"}}";
    ScriptedHttpServer server([&](const Request&) {
        return http_test::raw_reply(200, {{"Content-Type", "application/json"}}, big);
    });
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.max_message_bytes = 1024;
    Driven driven(endpoint(server.url()), settings);
    driven.post(1, ping_line);
    ASSERT_TRUE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return !messages_of(seen).empty(); }));
    const auto seen = driven.recorder().of(1);
    // One byte beyond the bound reaches the engine, which refuses it as too long; nothing more comes.
    ASSERT_EQ(messages_of(seen).size(), 1u);
    EXPECT_EQ(messages_of(seen).front().size(), 1025u);
    // A later exchange that ends in order: by then anything the first one still had would have come.
    server.set_script([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    driven.post(2, ping_line);
    ASSERT_FALSE(events_of<StreamEnded>(driven.recorder().until_end(2)).empty());
    EXPECT_TRUE(events_of<StreamEnded>(driven.recorder().of(1)).empty());
    EXPECT_TRUE(events_of<StreamLost>(driven.recorder().of(1)).empty());
}

TEST(ClientHttp, AJsonBodyAtTheBoundIsRead) {
    const std::string body = R"({"jsonrpc":"2.0","id":1,"result":{}})";
    ScriptedHttpServer server([&](const Request&) {
        return http_test::raw_reply(200, {{"Content-Type", "application/json"}}, body);
    });
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.max_message_bytes = body.size();
    Driven driven(endpoint(server.url()), settings);
    driven.post(1, ping_line);
    EXPECT_EQ(messages_of(driven.recorder().until_end(1)), std::vector<std::string>{body});
}

TEST(ClientHttp, ResponseHeadersOverTheBoundAbortTheExchange) {
    ScriptedHttpServer server([](const Request&) {
        return http_test::raw_reply(200, {{"Content-Type", "application/json"}, {"X-Pad", std::string(2048, 'p')}},
                                    R"({"jsonrpc":"2.0","id":1,"result":{}})");
    });
    auto narrow = endpoint(server.url());
    narrow.max_response_header_bytes = 1024;
    Driven driven(narrow);
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    EXPECT_FALSE(head_of(seen));
    EXPECT_TRUE(messages_of(seen).empty());
    EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);

    // Many small fields count together.
    http_test::Fields many;
    for (int i = 0; i < 100; ++i) many.emplace_back("X-Field-" + std::to_string(i), "0123456789");
    server.set_script([&](const Request&) {
        return http_test::raw_reply(200, many, R"({"jsonrpc":"2.0","id":2,"result":{}})");
    });
    driven.post(2, ping_line);
    EXPECT_EQ(events_of<StreamLost>(driven.recorder().until_end(2)).size(), 1u);

    // Within the bound the same answer is read.
    auto wide = endpoint(server.url());
    Driven roomy(wide);
    roomy.post(3, ping_line);
    EXPECT_TRUE(head_of(roomy.recorder().until_end(3)));
}

TEST(ClientHttp, AnEventStreamIsReadEventByEvent) {
    ScriptedHttpServer server([](const Request&) {
        return http_test::event_stream_reply({"id: e1\nretry: 500\ndata:\n\n",
                                              ": keep-alive\n\nevent: message\ndata: {\"jsonrpc\":\"2.0\",",
                                              "\"method\":\"notifications/progress\"}\n\n",
                                              "event: other\ndata: ignored\n\n",
                                              "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\r\n\r\n"});
    });
    Driven driven(endpoint(server.url()));
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    EXPECT_EQ(head_of(seen)->media_type, "text/event-stream");
    const auto ids = events_of<neomcp::StreamEventId>(seen);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front().id, "e1");
    const auto retries = events_of<neomcp::StreamRetryAdvice>(seen);
    ASSERT_EQ(retries.size(), 1u);
    EXPECT_EQ(retries.front().delay.count(), 500);
    EXPECT_EQ(messages_of(seen), (std::vector<std::string>{R"({"jsonrpc":"2.0","method":"notifications/progress"})",
                                                           R"({"jsonrpc":"2.0","id":1,"result":{}})"}));
    EXPECT_TRUE(ended(seen));
}

TEST(ClientHttp, AnEventOverTheBoundEndsTheStream) {
    ScriptedHttpServer server([](const Request&) {
        return http_test::event_stream_reply({"data: " + std::string(2048, 'x') + "\n\n", "data: {}\n\n"});
    });
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.max_message_bytes = 1024;
    Driven driven(endpoint(server.url()), settings);
    driven.post(1, ping_line);
    ASSERT_TRUE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return !messages_of(seen).empty(); }));
    const auto seen = driven.recorder().of(1);
    ASSERT_EQ(messages_of(seen).size(), 1u);
    EXPECT_GT(messages_of(seen).front().size(), 1024u);
}

// ---- The idle bound

// The bytes of a complete JSON answer, for a reply that sends them after a gate.
std::string json_bytes(const Request& request) {
    const auto reply = http_test::json_reply(200, http_test::result_for(request, json::object()));
    return std::get<http_test::Send>(reply.steps.front()).bytes;
}

// An event stream that has begun and then falls silent is lost after stream_idle_timeout, while a POST that waits for
// its JSON answer for longer than that bound is not: only the client's clock of the request bounds it.
TEST(ClientHttp, TheIdleBoundBeginsWithAnEventStream) {
    auto answer = std::make_shared<http_test::Gate>();
    auto silence = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([answer, silence](const Request& request) {
        http_test::Reply reply;
        if (request.rpc_method() == "slow") {
            reply.wait(answer);
            reply.send(json_bytes(request));
            return reply;
        }
        reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
        reply.send(": begun\n\n");
        reply.wait(silence);
        return reply;
    });
    auto described = endpoint(server.url());
    described.stream_idle_timeout = std::chrono::milliseconds(200);
    Driven driven(described);
    // The JSON answer is held first, so that it has waited longer than the bound once the stream is found idle.
    driven.post(1, R"({"jsonrpc":"2.0","id":1,"method":"slow"})");
    ASSERT_TRUE(server.wait_for(1));
    driven.post(2, R"({"jsonrpc":"2.0","id":2,"method":"stream"})");
    const auto stream = driven.recorder().until_end(2);
    ASSERT_TRUE(head_of(stream));
    EXPECT_EQ(head_of(stream)->media_type, "text/event-stream");
    EXPECT_EQ(events_of<StreamLost>(stream).size(), 1u);
    // The held request is still waiting: nothing ended it.
    EXPECT_TRUE(driven.recorder().of(1).empty());
    answer->open();
    const auto waited = driven.recorder().until_end(1);
    EXPECT_EQ(messages_of(waited).size(), 1u);
    EXPECT_TRUE(ended(waited));
    EXPECT_TRUE(events_of<StreamLost>(waited).empty());
    silence->open();
}

TEST(ClientHttp, AnEventStreamThatKeepsCarryingBytesIsNotIdle) {
    // Comments every 20 ms for well over the idle bound: each byte starts the bound again, so the stream lives to its
    // answer.
    ScriptedHttpServer server([](const Request&) {
        const auto gap = std::chrono::milliseconds(20);
        const int ticks = 60;
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
        for (int i = 0; i < ticks; ++i) {
            reply.pause(gap);
            reply.send(": tick\n\n");
        }
        reply.send("data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n\n");
        return reply;
    });
    auto described = endpoint(server.url());
    described.stream_idle_timeout = std::chrono::milliseconds(500);
    Driven driven(described);
    const auto started = std::chrono::steady_clock::now();
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    // The stream lasted longer than the bound (else this test would show nothing).
    EXPECT_GT(std::chrono::steady_clock::now() - started, described.stream_idle_timeout);
    EXPECT_TRUE(events_of<StreamLost>(seen).empty());
    EXPECT_EQ(messages_of(seen).size(), 1u);
    EXPECT_TRUE(ended(seen));
}

// ---- Failures of the connection

TEST(ClientHttp, NoServerAtThePortIsAFailedSend) {
    std::string url;
    {
        auto gone = answering();
        url = gone.url();
    }
    Driven driven(endpoint(url));
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    EXPECT_EQ(events_of<SendFailed>(seen).size(), 1u);
    EXPECT_FALSE(head_of(seen));
}

TEST(ClientHttp, AConnectionThatDropsMidBodyIsLost) {
    ScriptedHttpServer server([](const Request&) {
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "application/json"}}, 100));
        reply.send(R"({"jsonrpc":"2.0",)");
        reply.drop();
        return reply;
    });
    Driven driven(endpoint(server.url()));
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    ASSERT_TRUE(head_of(seen));
    EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);
    EXPECT_TRUE(messages_of(seen).empty());
}

TEST(ClientHttp, EndingAUnitStopsItsExchangeAndItsEvents) {
    auto gate = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([gate](const Request&) {
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
        reply.wait(gate);
        reply.send("data: late\n\n");
        return reply;
    });
    Driven driven(endpoint(server.url()));
    driven.post(1, ping_line);
    ASSERT_TRUE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return head_of(seen).has_value(); }));
    driven.end(1);
    gate->open();
    driven.post(2, R"({"jsonrpc":"2.0","method":"x"})", {}, neomcp::OutgoingKind::notification);
    driven.recorder().until_end(2);
    EXPECT_TRUE(messages_of(driven.recorder().of(1)).empty());
    EXPECT_FALSE(ended(driven.recorder().of(1)));
}

TEST(ClientHttp, ClosingWhileExchangesRunDeliversNothingAfterwards) {
    auto gate = std::make_shared<http_test::Gate>();
    ScriptedHttpServer server([gate](const Request&) {
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
        reply.send("data: {}\n\n");
        reply.wait(gate);
        reply.send("data: {}\n\n");
        return reply;
    });
    Driven driven(endpoint(server.url()));
    for (neomcp::UnitId unit = 1; unit <= 4; ++unit) driven.post(unit, ping_line);
    ASSERT_TRUE(server.wait_for(4));
    ASSERT_TRUE(driven.recorder().wait([](const std::vector<Delivered>& seen) { return messages_of(seen).size() == 4; }));
    driven.close();
    const std::size_t before = driven.recorder().all().size();
    gate->open();
    server.stop();
    EXPECT_EQ(driven.recorder().all().size(), before);
}

TEST(ClientHttp, TheConnectionStreamIsNotOpenedByThisTransport) {
    auto server = answering();
    Driven driven(endpoint(server.url()));
    driven.send(neomcp::Outgoing{std::string(), neomcp::connection_unit, {}, neomcp::OutgoingKind::open_connection_stream,
                                 nullptr});
    const auto seen = driven.recorder().until_end(neomcp::connection_unit);
    EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);
    EXPECT_EQ(server.count(), 0u);
}

// ---- Redirects

// A peer that redirects `hops` times to `location`, then answers.
std::unique_ptr<ScriptedHttpServer> redirect_peer(int status, std::shared_ptr<std::string> location, int hops) {
    auto count = std::make_shared<int>(0);
    return std::make_unique<ScriptedHttpServer>([status, location, hops, count](const Request&) {
        if ((*count)++ < hops) return http_test::empty_reply(status, {{"Location", *location}});
        return http_test::json_reply(200, json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}});
    });
}

TEST(ClientHttp, RedirectsOf307And308ToTheSameOriginAreFollowedWithTheMethodKept) {
    for (const int status : {307, 308}) {
        auto location = std::make_shared<std::string>("/moved");
        auto server = redirect_peer(status, location, 1);
        Driven driven(endpoint(server->url()));
        driven.post(1, ping_line, {{"Mcp-Method", "ping"}});
        const auto seen = driven.recorder().until_end(1);
        ASSERT_TRUE(head_of(seen)) << status;
        EXPECT_EQ(head_of(seen)->status, 200) << status;
        ASSERT_EQ(server->count(), 2u);
        const auto again = server->requests().back();
        EXPECT_EQ(again.method, "POST");
        EXPECT_EQ(again.target, "/moved");
        EXPECT_EQ(again.body, ping_line);
        EXPECT_EQ(again.field("Mcp-Method"), "ping");
        EXPECT_EQ(messages_of(seen).size(), 1u);
    }
}

TEST(ClientHttp, RedirectsToAnotherOriginAreNotFollowed) {
    auto other = answering();
    auto location = std::make_shared<std::string>(other.url("/elsewhere"));
    auto server = redirect_peer(307, location, 1);
    Driven driven(endpoint(server->url()));
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    ASSERT_TRUE(head_of(seen));
    EXPECT_EQ(head_of(seen)->status, 307);
    EXPECT_EQ(other.count(), 0u);
    EXPECT_EQ(server->count(), 1u);
}

TEST(ClientHttp, RedirectsOtherThan307And308AreNotFollowed) {
    for (const int status : {301, 302, 303}) {
        auto location = std::make_shared<std::string>("/moved");
        auto server = redirect_peer(status, location, 1);
        Driven driven(endpoint(server->url()));
        driven.post(1, ping_line);
        const auto seen = driven.recorder().until_end(1);
        ASSERT_TRUE(head_of(seen)) << status;
        EXPECT_EQ(head_of(seen)->status, status);
        EXPECT_EQ(server->count(), 1u) << status;
    }
}

TEST(ClientHttp, RedirectsStopAtTheirBound) {
    auto location = std::make_shared<std::string>("/again");
    auto server = redirect_peer(307, location, 3);
    auto bounded = endpoint(server->url());
    bounded.max_redirects = 2;
    Driven driven(bounded);
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    ASSERT_TRUE(head_of(seen));
    EXPECT_EQ(head_of(seen)->status, 307);
    EXPECT_EQ(server->count(), 3u);  // the first request and two redirects

    // Within the bound the same chain is followed to its end.
    auto chain = redirect_peer(307, location, 2);
    auto enough = endpoint(chain->url());
    enough.max_redirects = 2;
    Driven followed(enough);
    followed.post(1, ping_line);
    EXPECT_EQ(head_of(followed.recorder().until_end(1))->status, 200);
}

TEST(ClientHttp, ARedirectToAUrlWithUserInformationIsNotFollowed) {
    auto location = std::make_shared<std::string>();
    auto server = redirect_peer(307, location, 1);
    *location = "http://user:secret@127.0.0.1:" + std::to_string(server->port()) + "/mcp";
    Driven driven(endpoint(server->url()));
    driven.post(1, ping_line);
    EXPECT_EQ(head_of(driven.recorder().until_end(1))->status, 307);
    EXPECT_EQ(server->count(), 1u);
}

// ---- Authorization

class RecordingAuthorizer final : public neomcp::HttpAuthorizer {
public:
    explicit RecordingAuthorizer(std::string value) : value_(std::move(value)) {}
    std::string authorization(const neomcp::HttpRequestInfo& request) override {
        asked.push_back(request);
        return value_;
    }
    void challenge(const neomcp::HttpRequestInfo& request, int status,
                   std::span<const std::string> challenges) override {
        challenged.push_back(request);
        statuses.push_back(status);
        seen.assign(challenges.begin(), challenges.end());
    }
    std::vector<neomcp::HttpRequestInfo> asked;
    std::vector<neomcp::HttpRequestInfo> challenged;
    std::vector<int> statuses;
    std::vector<std::string> seen;

private:
    std::string value_;
};

TEST(ClientHttp, TheAuthorizersValueGoesOnTheAuthorizationFieldOnly) {
    auto server = answering();
    auto authorizer = std::make_shared<RecordingAuthorizer>("Bearer token-value");
    auto described = endpoint(server.url());
    described.authorizer = authorizer;
    Driven driven(described);
    driven.post(5, ping_line);
    driven.recorder().until_end(5);
    ASSERT_TRUE(server.wait_for(1));
    const Request request = server.requests().front();
    EXPECT_EQ(request.fields_named("Authorization"), std::vector<std::string>{"Bearer token-value"});
    EXPECT_EQ(request.target.find("token"), std::string::npos);
    EXPECT_EQ(request.body.find("token"), std::string::npos);
    std::size_t carriers = 0;
    for (const auto& [name, value] : request.fields) {
        if (value.find("token-value") != std::string::npos) ++carriers;
    }
    EXPECT_EQ(carriers, 1u);
    driven.close();
    ASSERT_EQ(authorizer->asked.size(), 1u);
    EXPECT_EQ(authorizer->asked.front().method, "POST");
    EXPECT_EQ(authorizer->asked.front().url, server.url());
    EXPECT_EQ(authorizer->asked.front().unit, 5u);
}

TEST(ClientHttp, AnEmptyAuthorizationSendsNoField) {
    auto server = answering();
    auto described = endpoint(server.url());
    described.authorizer = std::make_shared<RecordingAuthorizer>("");
    Driven driven(described);
    driven.post(1, ping_line);
    driven.recorder().until_end(1);
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_FALSE(server.requests().front().field("Authorization"));
}

TEST(ClientHttp, AnAuthorizationThatWouldSplitTheHeadIsNeverSent) {
    auto server = answering();
    auto described = endpoint(server.url());
    described.authorizer = std::make_shared<RecordingAuthorizer>("Bearer a\r\nX-Injected: 1");
    Driven driven(described);
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 1u);
    EXPECT_EQ(server.count(), 0u);
}

TEST(ClientHttp, ARefusalForWantOfAuthorizationIsToldToTheAuthorizer) {
    for (const int status : {401, 403}) {
        ScriptedHttpServer server([status](const Request&) {
            return http_test::empty_reply(status, {{"WWW-Authenticate", "Bearer resource_metadata=\"x\""},
                                                   {"WWW-Authenticate", "Basic realm=\"y\""}});
        });
        auto authorizer = std::make_shared<RecordingAuthorizer>("");
        auto described = endpoint(server.url());
        described.authorizer = authorizer;
        Driven driven(described);
        driven.post(1, ping_line);
        const auto seen = driven.recorder().until_end(1);
        EXPECT_EQ(head_of(seen)->status, status);
        driven.close();
        ASSERT_EQ(authorizer->statuses, std::vector<int>{status});
        EXPECT_EQ(authorizer->seen,
                  (std::vector<std::string>{"Bearer resource_metadata=\"x\"", "Basic realm=\"y\""}));
        EXPECT_EQ(authorizer->challenged.front().unit, 1u);
    }
}

// ---- Proxies

class Environment {
public:
    Environment(const char* name, std::optional<std::string> value) : name_(name) {
#if defined(_MSC_VER)
        char* old = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&old, &size, name) == 0 && old != nullptr) {
            old_ = std::string(old);
            std::free(old);
        }
#else
        if (const char* old = std::getenv(name)) old_ = std::string(old);
#endif
        apply(value);
    }
    ~Environment() { apply(old_); }

private:
    void apply(const std::optional<std::string>& value) {
#ifdef _WIN32
        _putenv_s(name_, value ? value->c_str() : "");
#else
        if (value) {
            setenv(name_, value->c_str(), 1);
        } else {
            unsetenv(name_);
        }
#endif
    }
    const char* name_;
    std::optional<std::string> old_;
};

TEST(ClientHttp, ANamedProxyCarriesTheRequests) {
    auto proxy = answering();
    auto described = endpoint("http://127.0.0.1:9/mcp");  // never reached directly
    described.proxy.mode = neomcp::ProxyMode::named;
    described.proxy.url = proxy.url("");
    Driven driven(described);
    driven.post(1, ping_line);
    driven.recorder().until_end(1);
    ASSERT_TRUE(proxy.wait_for(1));
    EXPECT_EQ(proxy.requests().front().target, "http://127.0.0.1:9/mcp");
}

TEST(ClientHttp, TheEnvironmentsProxyIsUsedUnlessSwitchedOff) {
    auto proxy = answering();
    auto direct = answering();
    const Environment http_proxy("http_proxy", proxy.url(""));
    const Environment no_proxy_lower("no_proxy", std::nullopt);
    const Environment no_proxy_upper("NO_PROXY", std::nullopt);
    {
        auto described = endpoint(direct.url());
        described.proxy.mode = neomcp::ProxyMode::environment;
        Driven driven(described);
        driven.post(1, ping_line);
        driven.recorder().until_end(1);
    }
    EXPECT_EQ(proxy.count(), 1u);
    EXPECT_EQ(direct.count(), 0u);
    {
        auto described = endpoint(direct.url());
        described.proxy.mode = neomcp::ProxyMode::none;
        Driven driven(described);
        driven.post(1, ping_line);
        driven.recorder().until_end(1);
    }
    EXPECT_EQ(proxy.count(), 1u);
    EXPECT_EQ(direct.count(), 1u);
}

// ---- Options

TEST(ClientHttp, OptionsThatCannotWorkAreRefused) {
    const auto refused = [](neomcp::HttpEndpoint described) {
        auto made = neomcp::detail::http_client::make_http_transport(std::move(described), {});
        return !made && made.error().kind == neomcp::ClientErrorKind::invalid_options;
    };
    EXPECT_TRUE(refused(endpoint("ftp://127.0.0.1/")));
    EXPECT_TRUE(refused(endpoint("http://user:secret@127.0.0.1/mcp")));
    EXPECT_TRUE(refused(endpoint("not a url")));
    auto named = endpoint("http://127.0.0.1:9/mcp");
    named.proxy.mode = neomcp::ProxyMode::named;
    EXPECT_TRUE(refused(named));
    auto authorities = endpoint("https://127.0.0.1:9/mcp");
    authorities.ca_file = std::filesystem::path("no-such-file-of-authorities.pem");
    EXPECT_TRUE(refused(authorities));
    auto zero = endpoint("http://127.0.0.1:9/mcp");
    zero.max_response_header_bytes = 0;
    EXPECT_TRUE(refused(zero));
    auto never = endpoint("http://127.0.0.1:9/mcp");
    never.connect_timeout = std::chrono::milliseconds(0);
    EXPECT_TRUE(refused(never));
    EXPECT_TRUE(neomcp::detail::http_client::make_http_transport(endpoint("http://127.0.0.1:9/mcp"), {}));
    EXPECT_TRUE(neomcp::make_transport(endpoint("https://example.com/mcp")));
}

// ---- The session of a handshake

neomcp::detail::http_client::HttpTransportSettings handshake_settings() {
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {neomcp::profile::builtin("2025-11-25")};
    return settings;
}

constexpr const char* initialize_line =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})";

TEST(ClientHttp, TheSessionOfAHandshakeGoesOnLaterRequests) {
    ScriptedHttpServer server([](const Request& request) {
        if (request.rpc_method() == "initialize") {
            return http_test::json_reply(200, http_test::result_for(request, json::object()),
                                         {{"MCP-Session-Id", "session-1"}});
        }
        return http_test::json_reply(200, http_test::result_for(request, json::object()),
                                     {{"MCP-Session-Id", "ignored-later"}});
    });
    Driven driven(endpoint(server.url()), handshake_settings());
    driven.post(1, initialize_line);
    driven.recorder().until_end(1);
    driven.post(2, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", {}, neomcp::OutgoingKind::notification);
    driven.recorder().until_end(2);
    driven.post(3, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    driven.recorder().until_end(3);
    driven.post(4, R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
    driven.recorder().until_end(4);
    ASSERT_TRUE(server.wait_for(4));
    const auto requests = server.requests();
    EXPECT_FALSE(requests[0].field("MCP-Session-Id"));
    EXPECT_EQ(requests[1].fields_named("MCP-Session-Id"), std::vector<std::string>{"session-1"});
    EXPECT_EQ(requests[2].fields_named("MCP-Session-Id"), std::vector<std::string>{"session-1"});
    // Only the handshake's answer names the session.
    EXPECT_EQ(requests[3].fields_named("MCP-Session-Id"), std::vector<std::string>{"session-1"});
}

TEST(ClientHttp, WithoutAProfileThatKeepsASessionNoSessionIsSent) {
    ScriptedHttpServer server([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()), {{"MCP-Session-Id", "s"}});
    });
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {neomcp::profile::builtin("2026-07-28")};
    Driven driven(endpoint(server.url()), settings);
    driven.post(1, initialize_line);
    driven.recorder().until_end(1);
    driven.post(2, ping_line);
    driven.recorder().until_end(2);
    ASSERT_TRUE(server.wait_for(2));
    EXPECT_FALSE(server.requests()[1].field("MCP-Session-Id"));
}

TEST(ClientHttp, ASessionIdThatCannotBeUsedEndsTheHandshake) {
    const std::vector<http_test::Fields> bad{
        {{"MCP-Session-Id", std::string("a\x7F" "b")}},
        {{"MCP-Session-Id", std::string(2000, 'x')}},
        {{"MCP-Session-Id", "one"}, {"mcp-session-id", "two"}},
        {{"MCP-Session-Id", "caf\xC3\xA9"}},
    };
    for (const auto& fields : bad) {
        ScriptedHttpServer server([&fields](const Request& request) {
            return http_test::json_reply(200, http_test::result_for(request, json::object()), fields);
        });
        Driven driven(endpoint(server.url()), handshake_settings());
        driven.post(1, initialize_line);
        const auto seen = driven.recorder().until_end(1);
        EXPECT_FALSE(head_of(seen));
        EXPECT_TRUE(messages_of(seen).empty());
        EXPECT_EQ(events_of<StreamLost>(seen).size(), 1u);
    }
}

}  // namespace
