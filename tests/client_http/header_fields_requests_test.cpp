// The header fields of the application's own on the wire: every kind of request of both HTTP transports carries the
// fixed fields and what the callback supplies for its kind, beside the authorizer's field; the documents the authorizer
// fetches carry none; a redirect keeps them within the origin and no other origin gets them; and a callback that fails
// for one request leaves the next one alone.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include "auth_rig.h"
#include "client_http/http_sse.h"
#include "client_http/streamable.h"
#include "header_fields_rig.h"
#include "neomcp/client/http.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"
#include "rig.h"
#include "scripted_http_server.h"

namespace {

using namespace std::chrono_literals;
using fields_rig::fixed_field;
using fields_rig::kind_field;
using fields_rig::kind_name;
using fields_rig::SessionPeer;
using fields_rig::Supplier;
using http_rig::Driven;
using http_rig::endpoint;
using http_rig::events_of;
using http_rig::messages_of;
using http_test::Gate;
using http_test::json;
using http_test::Reply;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::HttpHeader;
using neomcp::HttpRequestKind;
using neomcp::OutgoingKind;
using neomcp::SendFailed;

constexpr const char* ping_line = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
constexpr const char* second_fixed_field = "X-App-Second";
constexpr const char* token = "Bearer mcp-token";

neomcp::detail::http_client::HttpTransportSettings legacy_settings() {
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {fields_rig::legacy_profile()};
    return settings;
}

neomcp::detail::http_client::HttpTransportSettings older_settings() {
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {neomcp::profile::builtin("2024-11-05")};
    return settings;
}

std::string call_line(int id, std::string_view method) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string(method)}, {"params", json::object()}}.dump();
}

void handshake(Driven& driven) {
    driven.post(100, R"({"jsonrpc":"2.0","id":100,"method":"initialize","params":{}})", {}, OutgoingKind::request,
                fields_rig::legacy_profile());
    driven.recorder().until_end(100);
}

Reply held_stream(std::shared_ptr<Gate> hold) {
    Reply reply;
    reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
    reply.wait(std::move(hold));
    return reply;
}

neomcp::HttpEndpoint described(const std::string& url, Supplier& supplier) {
    neomcp::HttpEndpoint endpoint_of = endpoint(url);
    endpoint_of.header_fields = {{fixed_field, "fixed"}, {second_fixed_field, "two"}};
    endpoint_of.header_fields_for = supplier.callback();
    return endpoint_of;
}

void expect_fields(const Request& request, std::string_view kind) {
    EXPECT_EQ(request.fields_named(fixed_field), std::vector<std::string>{"fixed"}) << request.method << " " << kind;
    EXPECT_EQ(request.fields_named(second_fixed_field), std::vector<std::string>{"two"}) << request.method << " " << kind;
    EXPECT_EQ(request.fields_named(kind_field), std::vector<std::string>{std::string(kind)}) << request.method;
}

// ---- Streamable HTTP

TEST(ClientHeaderFields, EveryRequestOfStreamableHttpCarriesTheFixedFieldsAndTheCallbacks) {
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([hold](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET") return held_stream(hold);
        return std::nullopt;
    });
    Supplier supplier;
    Driven driven(described(peer.url(), supplier), legacy_settings());
    ASSERT_EQ(driven.problem(), "");
    handshake(driven);
    driven.open_stream({{"MCP-Protocol-Version", "2025-11-25"}}, fields_rig::legacy_profile());
    driven.post(2, call_line(2, "tools/list"), {{"MCP-Protocol-Version", "2025-11-25"}}, OutgoingKind::request,
                fields_rig::legacy_profile());
    driven.recorder().until_end(2);
    hold->open();
    driven.close();

    const auto requests = peer.server().requests();
    ASSERT_EQ(requests.size(), 4u);
    std::size_t posts = 0;
    std::size_t gets = 0;
    std::size_t deletes = 0;
    for (const Request& request : requests) {
        if (request.method == "POST") {
            expect_fields(request, kind_name(HttpRequestKind::message));
            ++posts;
        } else if (request.method == "GET") {
            expect_fields(request, kind_name(HttpRequestKind::connection_stream));
            EXPECT_FALSE(request.field("Last-Event-ID"));
            ++gets;
        } else if (request.method == "DELETE") {
            expect_fields(request, kind_name(HttpRequestKind::end_session));
            EXPECT_EQ(request.fields_named("MCP-Session-Id"), std::vector<std::string>{"session-1"});
            ++deletes;
        }
        // What the transport writes is written once, with what the engine gave.
        EXPECT_LE(request.fields_named("Accept").size(), 1u);
        EXPECT_LE(request.fields_named("Content-Type").size(), 1u);
        EXPECT_LE(request.fields_named("MCP-Session-Id").size(), 1u);
        EXPECT_LE(request.fields_named("MCP-Protocol-Version").size(), 1u);
    }
    EXPECT_EQ(posts, 2u);
    EXPECT_EQ(gets, 1u);
    EXPECT_EQ(deletes, 1u);
    EXPECT_EQ(supplier.kinds(), (std::vector<HttpRequestKind>{HttpRequestKind::message, HttpRequestKind::connection_stream,
                                                              HttpRequestKind::message, HttpRequestKind::end_session}));
}

TEST(ClientHeaderFields, TheRequestThatResumesAStreamCarriesThem) {
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([hold](const Request& request) -> std::optional<Reply> {
        if (request.method == "POST" && request.rpc_method() == "tools/call") {
            Reply reply;
            reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
            reply.send("id: e1\ndata: \n\n");
            return reply;  // the stream ends before its answer
        }
        if (request.method == "GET") {
            Reply reply;
            reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
            const json answer{{"jsonrpc", "2.0"}, {"id", 2}, {"result", json::object()}};
            reply.send("id: e2\ndata: " + answer.dump() + "\n\n");
            reply.wait(hold);
            return reply;
        }
        return std::nullopt;
    });
    Supplier supplier;
    neomcp::HttpEndpoint endpoint_of = described(peer.url(), supplier);
    endpoint_of.retry_delay = 10ms;
    Driven driven(endpoint_of, legacy_settings());
    handshake(driven);
    driven.post(2, call_line(2, "tools/call"), {}, OutgoingKind::request, fields_rig::legacy_profile());
    ASSERT_TRUE(driven.recorder().wait([](const std::vector<http_rig::Delivered>& seen) {
        for (const auto& one : seen) {
            const auto* received = std::get_if<http_rig::Received>(&one);
            if (received != nullptr && received->unit == 2) return true;
        }
        return false;
    }));
    hold->open();
    std::optional<Request> resumed;
    for (const Request& request : peer.server().requests()) {
        if (request.method == "GET") resumed = request;
    }
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->field("Last-Event-ID"), "e1");
    expect_fields(*resumed, kind_name(HttpRequestKind::stream_resumption));
    EXPECT_EQ(supplier.kinds(), (std::vector<HttpRequestKind>{HttpRequestKind::message, HttpRequestKind::message,
                                                              HttpRequestKind::stream_resumption}));
    driven.end(2);
}

TEST(ClientHeaderFields, ARequestSentAgainAfterARedirectCarriesThemOnceMore) {
    auto moved = std::make_shared<std::atomic<int>>(0);
    ScriptedHttpServer server([moved](const Request& request) {
        if (request.target == "/mcp" && moved->fetch_add(1) == 0) {
            return http_test::empty_reply(307, {{"Location", "/moved"}});
        }
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    Supplier supplier;
    Driven driven(described(server.url(), supplier));
    driven.post(1, ping_line);
    driven.recorder().until_end(1);
    ASSERT_TRUE(server.wait_for(2));
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1].target, "/moved");
    expect_fields(requests[0], "message");
    expect_fields(requests[1], "message");
    EXPECT_EQ(supplier.calls(), 1u);
}

TEST(ClientHeaderFields, ARedirectToAnotherOriginIsNotFollowedAndItsPeerGetsNoField) {
    ScriptedHttpServer other([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    const std::string away = other.url("/elsewhere");
    ScriptedHttpServer server([away](const Request&) { return http_test::empty_reply(307, {{"Location", away}}); });
    Supplier supplier;
    Driven driven(described(server.url(), supplier));
    driven.post(1, ping_line);
    driven.recorder().until_end(1);
    EXPECT_EQ(server.count(), 1u);
    EXPECT_EQ(other.count(), 0u);
}

TEST(ClientHeaderFields, TheStreamOfTheConnectionIsOpenedAgainAfterARequestWhoseFieldsFailed) {
    auto hold = std::make_shared<Gate>();
    SessionPeer peer([hold](const Request& request) -> std::optional<Reply> {
        if (request.method == "GET") return held_stream(hold);
        return std::nullopt;
    });
    auto failed = std::make_shared<std::atomic<int>>(0);
    Supplier supplier([failed](HttpRequestKind kind, std::size_t) {
        if (kind == HttpRequestKind::connection_stream && failed->fetch_add(1) == 0) {
            return std::vector<HttpHeader>{{"Accept", "text/plain"}};
        }
        return std::vector<HttpHeader>{{kind_field, kind_name(kind)}};
    });
    neomcp::HttpEndpoint endpoint_of = endpoint(peer.url());
    endpoint_of.header_fields_for = supplier.callback();
    endpoint_of.retry_delay = 10ms;
    Driven driven(endpoint_of, legacy_settings());
    handshake(driven);
    driven.open_stream({}, fields_rig::legacy_profile());
    ASSERT_TRUE(peer.server().wait_for(2));
    hold->open();
    std::vector<Request> gets;
    for (const Request& request : peer.server().requests()) {
        if (request.method == "GET") gets.push_back(request);
    }
    ASSERT_EQ(gets.size(), 1u);  // the first GET was never sent
    EXPECT_EQ(gets[0].field("Accept"), "text/event-stream");
    EXPECT_EQ(gets[0].fields_named(kind_field), std::vector<std::string>{"connection_stream"});
    EXPECT_EQ(supplier.kinds(), (std::vector<HttpRequestKind>{HttpRequestKind::message, HttpRequestKind::connection_stream,
                                                              HttpRequestKind::connection_stream}));
}

TEST(ClientHeaderFields, TheRequestThatEndsASessionIsNotSentWhenItsFieldsCannotBe) {
    SessionPeer peer;
    Supplier supplier([](HttpRequestKind kind, std::size_t) {
        if (kind == HttpRequestKind::end_session) return std::vector<HttpHeader>{{"MCP-Session-Id", "other"}};
        return std::vector<HttpHeader>{};
    });
    neomcp::HttpEndpoint endpoint_of = endpoint(peer.url());
    endpoint_of.header_fields_for = supplier.callback();
    Driven driven(endpoint_of, legacy_settings());
    handshake(driven);
    driven.close();
    for (const Request& request : peer.server().requests()) EXPECT_NE(request.method, "DELETE");
    EXPECT_EQ(supplier.kinds().back(), HttpRequestKind::end_session);
}

// ---- The older transport of events over HTTP

TEST(ClientHeaderFields, EveryRequestOfTheOlderTransportCarriesTheFixedFieldsAndTheCallbacks) {
    auto open = std::make_shared<Gate>();
    ScriptedHttpServer server([open](const Request& request) {
        if (request.method == "POST") return http_test::empty_reply(202);
        Reply stream = http_test::event_stream_reply({"event: endpoint\ndata: /messages\n\n"});
        stream.wait(open);
        return stream;
    });
    Supplier supplier;
    Driven driven(described(server.url(), supplier), older_settings(), http_rig::Kind::events);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, ping_line);
    driven.recorder().until_end(1);
    driven.post(2, ping_line);
    driven.recorder().until_end(2);
    open->open();
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_EQ(requests[0].method, "GET");
    expect_fields(requests[0], kind_name(HttpRequestKind::connection_stream));
    for (std::size_t index = 1; index < requests.size(); ++index) {
        EXPECT_EQ(requests[index].method, "POST");
        EXPECT_EQ(requests[index].target, "/messages");
        expect_fields(requests[index], kind_name(HttpRequestKind::message));
    }
    EXPECT_EQ(supplier.kinds(), (std::vector<HttpRequestKind>{HttpRequestKind::connection_stream, HttpRequestKind::message,
                                                              HttpRequestKind::message}));
}

TEST(ClientHeaderFields, AMessageOfTheOlderTransportWhoseFieldsFailedFailsAloneAndTheNextGoesOut) {
    auto open = std::make_shared<Gate>();
    ScriptedHttpServer server([open](const Request& request) {
        if (request.method == "POST") return http_test::empty_reply(202);
        Reply stream = http_test::event_stream_reply({"event: endpoint\ndata: /messages\n\n"});
        stream.wait(open);
        return stream;
    });
    auto sent = std::make_shared<std::atomic<int>>(0);
    Supplier supplier([sent](HttpRequestKind kind, std::size_t) {
        if (kind == HttpRequestKind::message && sent->fetch_add(1) == 0) {
            return std::vector<HttpHeader>{{"Content-Type", "text/plain"}};
        }
        return std::vector<HttpHeader>{{kind_field, kind_name(kind)}};
    });
    neomcp::HttpEndpoint endpoint_of = endpoint(server.url());
    endpoint_of.header_fields_for = supplier.callback();
    Driven driven(endpoint_of, older_settings(), http_rig::Kind::events);
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 1u);
    driven.post(2, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(2)).size(), 0u);
    open->open();
    std::size_t posts = 0;
    for (const Request& request : server.requests()) {
        if (request.method == "POST") ++posts;
    }
    EXPECT_EQ(posts, 1u);
}

TEST(ClientHeaderFields, AnEventStreamOfTheOlderTransportWhoseFieldsFailedFailsTheMessagesThatWaitForIt) {
    ScriptedHttpServer server([](const Request&) { return http_test::empty_reply(202); });
    Supplier supplier([](HttpRequestKind kind, std::size_t) {
        if (kind == HttpRequestKind::connection_stream) return std::vector<HttpHeader>{{"Host", "elsewhere"}};
        return std::vector<HttpHeader>{};
    });
    neomcp::HttpEndpoint endpoint_of = endpoint(server.url());
    endpoint_of.header_fields_for = supplier.callback();
    Driven driven(endpoint_of, older_settings(), http_rig::Kind::events);
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 1u);
    EXPECT_EQ(server.count(), 0u);
}

// ---- Beside the authorizer's field, and not to its documents

TEST(ClientHeaderFields, TheFieldsGoBesideTheAuthorizersAndNeverInsteadOfIt) {
    ScriptedHttpServer server([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    auto authorizer = std::make_shared<auth_rig::ScriptedAuthorizer>(token, 0);
    Supplier supplier([](HttpRequestKind kind, std::size_t call) {
        if (call == 0) return std::vector<HttpHeader>{{"Authorization", "Bearer forged"}};
        return std::vector<HttpHeader>{{kind_field, kind_name(kind)}};
    });
    neomcp::HttpEndpoint endpoint_of = endpoint(server.url());
    endpoint_of.authorizer = authorizer;
    endpoint_of.header_fields = {{fixed_field, "fixed"}};
    endpoint_of.header_fields_for = supplier.callback();
    Driven driven(endpoint_of);
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 1u);
    EXPECT_EQ(server.count(), 0u);
    driven.post(2, ping_line);
    driven.recorder().until_end(2);
    ASSERT_TRUE(server.wait_for(1));
    const Request request = server.requests().front();
    EXPECT_EQ(request.fields_named("Authorization"), std::vector<std::string>{token});
    EXPECT_EQ(request.fields_named(fixed_field), std::vector<std::string>{"fixed"});
    EXPECT_EQ(request.fields_named(kind_field), std::vector<std::string>{"message"});
}

TEST(ClientHeaderFields, TheDocumentsTheAuthorizerFetchesCarryNoneOfThem) {
    ScriptedHttpServer mcp([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
    ScriptedHttpServer issuer([](const Request&) { return http_test::json_reply(200, json{{"issuer", "x"}}); });
    auto authorizer = std::make_shared<auth_rig::ScriptedAuthorizer>(token, 0);
    Supplier supplier;
    neomcp::HttpEndpoint endpoint_of = described(mcp.url(), supplier);
    endpoint_of.authorizer = authorizer;
    Driven driven(endpoint_of);
    ASSERT_EQ(driven.problem(), "");
    neomcp::AuthorizationHost* host = authorizer->host();
    ASSERT_NE(host, nullptr);
    for (const std::string& url : {issuer.url("/.well-known/oauth-authorization-server"), mcp.url("/.well-known/x")}) {
        neomcp::AuthorizationFetch request;
        request.purpose = neomcp::AuthorizationFetchPurpose::server_metadata;
        request.method = neomcp::AuthorizationFetchMethod::get;
        request.url = url;
        request.headers = {{"Accept", "application/json"}};
        auto done = asio::co_spawn(
            driven.io().executor(),
            [host, request]() mutable -> asio::awaitable<neomcp::AuthorizationFetchResult> {
                neomcp::AuthorizationFetchResult result = co_await host->fetch(std::move(request));
                co_return result;
            },
            asio::use_future);
        ASSERT_EQ(done.wait_for(http_rig::wait_bound), std::future_status::ready);
        const auto result = done.get();
        EXPECT_FALSE(result.error) << result.message;
    }
    ASSERT_TRUE(issuer.wait_for(1));
    ASSERT_TRUE(mcp.wait_for(1));
    for (const Request& request : {issuer.requests().front(), mcp.requests().front()}) {
        EXPECT_FALSE(request.field(fixed_field));
        EXPECT_FALSE(request.field(second_fixed_field));
        EXPECT_FALSE(request.field(kind_field));
    }
    EXPECT_FALSE(issuer.requests().front().field("Authorization"));
    EXPECT_EQ(supplier.calls(), 0u);
}

}  // namespace
