// The header fields of the application's own on the HTTP transports: what is refused when the endpoint is opened (a
// name the transport or the protocol writes, a name or value that is not HTTP field syntax, a name given twice, a
// bound exceeded), and what happens to one request whose fields from the callback cannot be sent.
#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "client_http/http_sse.h"
#include "client_http/streamable.h"
#include "header_fields_rig.h"
#include "neomcp/client.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"
#include "rig.h"
#include "scripted_http_server.h"

namespace {

using fields_rig::Supplier;
using http_rig::Driven;
using http_rig::endpoint;
using http_rig::events_of;
using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;
using neomcp::HttpClientDefaults;
using neomcp::HttpHeader;
using neomcp::HttpRequestKind;
using neomcp::SendFailed;

constexpr const char* ping_line = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";

ScriptedHttpServer answering() {
    return ScriptedHttpServer([](const Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, json::object()));
    });
}

// Why the endpoint is refused by the Streamable HTTP transport, or an empty string where it is not; with every built-in
// revision enabled the older transport must agree.
std::string refusal(const neomcp::HttpEndpoint& described,
                    neomcp::detail::http_client::HttpTransportSettings settings = {}) {
    auto streamable = neomcp::detail::http_client::make_http_transport(described, settings);
    // The older transport is refused when no enabled revision defines it, whatever the fields are.
    if (settings.profiles.empty()) {
        auto older = neomcp::detail::http_client::make_http_sse_transport(described, settings);
        EXPECT_EQ(static_cast<bool>(streamable), static_cast<bool>(older));
    }
    if (streamable) return {};
    EXPECT_EQ(streamable.error().kind, neomcp::ClientErrorKind::invalid_options);
    return streamable.error().message;
}

neomcp::HttpEndpoint with_fields(std::vector<HttpHeader> fields) {
    neomcp::HttpEndpoint described = endpoint("http://127.0.0.1:1/mcp");
    described.header_fields = std::move(fields);
    return described;
}

bool mentions(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// Fields named "X-F0", "X-F1", ... with values of one byte, `count` of them.
std::vector<HttpHeader> numbered(std::size_t count) {
    std::vector<HttpHeader> fields;
    for (std::size_t index = 0; index < count; ++index) fields.push_back({"X-F" + std::to_string(index), "v"});
    return fields;
}

// ---- What the endpoint refuses

TEST(ClientHeaderFields, ValidFixedFieldsAreAccepted) {
    EXPECT_EQ(refusal(with_fields({{"X-Tenant", "acme"}, {"x-trace", "a b\tc"}, {"X-Empty", ""}, {"X-Raw", "caf\xC3\xA9"}})),
              "");
    EXPECT_EQ(refusal(with_fields({})), "");
}

TEST(ClientHeaderFields, EveryNameTheProfilesWriteIsRefusedWhateverItsCase) {
    std::size_t checked = 0;
    for (const auto revision : neomcp::profile::builtin_revisions()) {
        const auto profile = neomcp::profile::builtin(revision);
        if (!profile->streamable_http()) continue;
        const auto& http = *profile->streamable_http();
        std::vector<std::string> names;
        if (http.session_header) names.push_back(*http.session_header);
        if (http.version_header) names.push_back(http.version_header->name);
        for (const auto& header : http.request_headers) names.push_back(header.name);
        if (http.argument_headers) names.push_back(http.argument_headers->header_prefix + "Region");
        for (const std::string& name : names) {
            std::string lower = name;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const std::string& spelling : {name, lower}) {
                const std::string why = refusal(with_fields({{spelling, "x"}}));
                EXPECT_TRUE(mentions(why, spelling)) << revision << " " << spelling << ": " << why;
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 10u);
}

TEST(ClientHeaderFields, EveryFieldOfHttpTheTransportWritesIsRefused) {
    for (const std::string& name : std::vector<std::string>{
         "Accept", "Content-Type", "Authorization", "Last-Event-ID", "Host", "Content-Length", "Transfer-Encoding",
          "Connection", "Expect", "TE", "Trailer", "Upgrade", "Proxy-Authorization"}) {
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const std::string& spelling : {name, lower}) {
            const std::string why = refusal(with_fields({{spelling, "x"}}));
            EXPECT_TRUE(mentions(why, spelling)) << spelling << ": " << why;
        }
    }
}

TEST(ClientHeaderFields, ANameOfAnEnabledRevisionIsRefusedAndOneOfAnotherRevisionIsFree) {
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {fields_rig::legacy_profile()};
    EXPECT_TRUE(mentions(refusal(with_fields({{"MCP-Session-Id", "x"}}), settings), "MCP-Session-Id"));
    EXPECT_TRUE(mentions(refusal(with_fields({{"mcp-protocol-version", "x"}}), settings), "mcp-protocol-version"));
    // 2025-11-25 writes no field that mirrors the method, so with only that revision enabled the name is free.
    EXPECT_EQ(refusal(with_fields({{"Mcp-Method", "x"}}), settings), "");
}

TEST(ClientHeaderFields, ANameThatIsNotATokenIsRefused) {
    const std::vector<std::string> bad{std::string(),           std::string("two words"), std::string("a:b"),
                                       std::string("a\r\nb"),   std::string("caf\xC3\xA9"), std::string("(x)"),
                                       std::string("a\0b", 3), std::string("x/y")};
    for (const std::string& name : bad) {
        const std::string why = refusal(with_fields({{name, "v"}}));
        EXPECT_FALSE(why.empty()) << "a name of " << name.size() << " bytes was accepted";
        EXPECT_TRUE(mentions(why, "token")) << why;
    }
}

TEST(ClientHeaderFields, AValueWithAControlCharacterIsRefused) {
    const std::vector<std::string> bad{std::string("a\rb"), std::string("a\nb"), std::string("a\0b", 3),
                                       std::string("a\x01"
                                                   "b"),
                                       std::string("a\x1F"
                                                   "b"),
                                       std::string("a\x7F"
                                                   "b"),
                                       std::string("ok\r\nInjected: yes")};
    for (const std::string& value : bad) {
        const std::string why = refusal(with_fields({{"X-Value", value}}));
        EXPECT_TRUE(mentions(why, "X-Value")) << "a value of " << value.size() << " bytes was accepted";
        EXPECT_FALSE(mentions(why, "Injected")) << "a value is never echoed";
    }
    // A tab, a space, visible characters and obs-text are the field syntax.
    EXPECT_EQ(refusal(with_fields({{"X-Value", " a\tb\x80\xFF~ "}})), "");
}

TEST(ClientHeaderFields, ANameGivenTwiceIsRefusedWhateverItsCase) {
    const std::string why = refusal(with_fields({{"X-Tenant", "a"}, {"x-TENANT", "b"}}));
    EXPECT_TRUE(mentions(why, "twice")) << why;
}

// ---- The bounds, at the edge and one past

TEST(ClientHeaderFields, TheCountOfFieldsIsBoundedAtTheEdgeAndOnePast) {
    neomcp::HttpEndpoint at = with_fields(numbered(5));
    at.max_header_fields = 5;
    EXPECT_EQ(refusal(at), "");
    neomcp::HttpEndpoint past = with_fields(numbered(6));
    past.max_header_fields = 5;
    EXPECT_TRUE(mentions(refusal(past), "max_header_fields"));
}

TEST(ClientHeaderFields, TheDefaultCountIsBoundedAtTheEdgeAndOnePast) {
    EXPECT_EQ(refusal(with_fields(numbered(HttpClientDefaults::max_header_fields))), "");
    EXPECT_TRUE(mentions(refusal(with_fields(numbered(HttpClientDefaults::max_header_fields + 1))), "max_header_fields"));
}

TEST(ClientHeaderFields, TheBytesOfFieldsAreBoundedAtTheEdgeAndOnePast) {
    // "X-A" and a value: the bytes are those of names and values together.
    neomcp::HttpEndpoint at = with_fields({{"X-A", std::string(97, 'v')}});
    at.max_header_field_bytes = 100;
    EXPECT_EQ(refusal(at), "");
    neomcp::HttpEndpoint past = with_fields({{"X-A", std::string(98, 'v')}});
    past.max_header_field_bytes = 100;
    EXPECT_TRUE(mentions(refusal(past), "max_header_field_bytes"));
    // Over several fields.
    neomcp::HttpEndpoint split = with_fields({{"X-A", std::string(47, 'v')}, {"X-B", std::string(47, 'v')}});
    split.max_header_field_bytes = 100;
    EXPECT_EQ(refusal(split), "");
    split.header_fields.back().value.push_back('v');
    EXPECT_TRUE(mentions(refusal(split), "max_header_field_bytes"));
}

TEST(ClientHeaderFields, TheDefaultBytesAreBoundedAtTheEdgeAndOnePast) {
    const std::size_t room = HttpClientDefaults::max_header_field_bytes - std::string("X-A").size();
    EXPECT_EQ(refusal(with_fields({{"X-A", std::string(room, 'v')}})), "");
    EXPECT_TRUE(mentions(refusal(with_fields({{"X-A", std::string(room + 1, 'v')}})), "max_header_field_bytes"));
}

TEST(ClientHeaderFields, ABoundOfZeroIsAnError) {
    neomcp::HttpEndpoint fields = with_fields({});
    fields.max_header_fields = 0;
    EXPECT_TRUE(mentions(refusal(fields), "max_header_fields"));
    neomcp::HttpEndpoint bytes = with_fields({});
    bytes.max_header_field_bytes = 0;
    EXPECT_TRUE(mentions(refusal(bytes), "max_header_field_bytes"));
}

TEST(ClientHeaderFields, ARefusedEndpointFailsClientOpen) {
    neomcp::ClientOptions options;
    auto client = neomcp::Client::open(options, with_fields({{"Authorization", "Bearer x"}}));
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, neomcp::ClientErrorKind::invalid_options);
}

// ---- One request whose fields from the callback cannot be sent

class CallbackCollision : public ::testing::TestWithParam<std::string> {};

TEST_P(CallbackCollision, FailsThatRequestAndTheNextOneGoesOut) {
    ScriptedHttpServer server = answering();
    Supplier supplier([name = GetParam()](HttpRequestKind, std::size_t call) {
        if (call == 0) return std::vector<HttpHeader>{{name, "x"}, {"X-Good", "1"}};
        return std::vector<HttpHeader>{{"X-Good", "2"}};
    });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields_for = supplier.callback();
    Driven driven(described);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, ping_line);
    const auto failed = driven.recorder().until_end(1);
    ASSERT_EQ(events_of<SendFailed>(failed).size(), 1u);
    EXPECT_TRUE(mentions(events_of<SendFailed>(failed).front().cause, "header field"));
    EXPECT_EQ(server.count(), 0u) << "the request went out with " << GetParam();
    driven.post(2, ping_line);
    driven.recorder().until_end(2);
    ASSERT_TRUE(server.wait_for(1));
    ASSERT_EQ(server.count(), 1u);
    const Request request = server.requests().front();
    EXPECT_EQ(request.fields_named("X-Good"), std::vector<std::string>{"2"});
    EXPECT_EQ(supplier.calls(), 2u);
}

INSTANTIATE_TEST_SUITE_P(ClientHeaderFields, CallbackCollision,
                         ::testing::Values("Accept", "accept", "Content-Type", "Authorization", "AUTHORIZATION", "Host",
                                           "Content-Length", "Transfer-Encoding", "Connection", "Expect",
                                           "Last-Event-ID", "MCP-Session-Id", "mcp-protocol-version", "Mcp-Method",
                                           "Mcp-Name", "Mcp-Param-Region"),
                         [](const auto& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return name + "_" + std::to_string(info.index);
                         });

TEST(ClientHeaderFields, ACallbackFieldOfTheSessionHeaderIsRefusedOnARequestOfTheSession) {
    fields_rig::SessionPeer peer;
    Supplier supplier([](HttpRequestKind, std::size_t call) {
        if (call == 1) return std::vector<HttpHeader>{{"mcp-session-id", "forged"}};
        return std::vector<HttpHeader>{};
    });
    neomcp::HttpEndpoint described = endpoint(peer.url());
    described.header_fields_for = supplier.callback();
    neomcp::detail::http_client::HttpTransportSettings settings;
    settings.profiles = {fields_rig::legacy_profile()};
    Driven driven(described, settings);
    ASSERT_EQ(driven.problem(), "");
    driven.post(100, R"({"jsonrpc":"2.0","id":100,"method":"initialize","params":{}})", {},
                neomcp::OutgoingKind::request, fields_rig::legacy_profile());
    driven.recorder().until_end(100);
    driven.post(2, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", {}, neomcp::OutgoingKind::request,
                fields_rig::legacy_profile());
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(2)).size(), 1u);
    for (const Request& request : peer.server().requests()) {
        EXPECT_NE(request.field("MCP-Session-Id"), "forged");
        EXPECT_NE(request.rpc_method(), "tools/list");
    }
}

TEST(ClientHeaderFields, ACallbackFieldThatIsAlsoAFixedFieldFailsThatRequest) {
    ScriptedHttpServer server = answering();
    Supplier supplier([](HttpRequestKind, std::size_t) { return std::vector<HttpHeader>{{"x-tenant", "b"}}; });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields = {{"X-Tenant", "a"}};
    described.header_fields_for = supplier.callback();
    Driven driven(described);
    driven.post(1, ping_line);
    const auto seen = driven.recorder().until_end(1);
    ASSERT_EQ(events_of<SendFailed>(seen).size(), 1u);
    EXPECT_TRUE(mentions(events_of<SendFailed>(seen).front().cause, "twice"));
    EXPECT_EQ(server.count(), 0u);
}

TEST(ClientHeaderFields, ACallbackFieldThatIsNotFieldSyntaxFailsThatRequest) {
    ScriptedHttpServer server = answering();
    Supplier supplier([](HttpRequestKind, std::size_t call) {
        switch (call) {
            case 0: return std::vector<HttpHeader>{{"bad name", "v"}};
            case 1: return std::vector<HttpHeader>{{"X-Value", "a\r\nInjected: yes"}};
            default: return std::vector<HttpHeader>{{"X-Fine", "v"}};
        }
    });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields_for = supplier.callback();
    Driven driven(described);
    for (const neomcp::UnitId unit : {1, 2}) {
        driven.post(unit, ping_line);
        EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(unit)).size(), 1u) << unit;
    }
    driven.post(3, ping_line);
    driven.recorder().until_end(3);
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_EQ(server.count(), 1u);
    EXPECT_FALSE(server.requests().front().field("Injected"));
    EXPECT_EQ(server.requests().front().field("X-Fine"), "v");
}

TEST(ClientHeaderFields, ACallbackFieldThatTheRequestAlreadyHoldsFailsThatRequest) {
    ScriptedHttpServer server = answering();
    Supplier supplier([](HttpRequestKind, std::size_t) { return std::vector<HttpHeader>{{"x-engine", "b"}}; });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields_for = supplier.callback();
    Driven driven(described);
    driven.post(1, ping_line, {{"X-Engine", "a"}});
    const auto seen = driven.recorder().until_end(1);
    ASSERT_EQ(events_of<SendFailed>(seen).size(), 1u);
    EXPECT_TRUE(mentions(events_of<SendFailed>(seen).front().cause, "x-engine"));
    EXPECT_EQ(server.count(), 0u);
}

TEST(ClientHeaderFields, ACallbackThatThrowsFailsThatRequestOnly) {
    ScriptedHttpServer server = answering();
    Supplier supplier([](HttpRequestKind, std::size_t call) -> std::vector<HttpHeader> {
        if (call == 0) throw std::runtime_error("no value yet");
        return {{"X-Fine", "v"}};
    });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields_for = supplier.callback();
    Driven driven(described);
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 1u);
    driven.post(2, ping_line);
    driven.recorder().until_end(2);
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_EQ(server.requests().front().field("X-Fine"), "v");
}

TEST(ClientHeaderFields, TheFixedFieldsAndTheCallbackShareTheBounds) {
    ScriptedHttpServer server = answering();
    // Two fixed fields and a callback that returns 3 (the 5th of 5) and then 4 (one past).
    Supplier supplier([](HttpRequestKind, std::size_t call) {
        std::vector<HttpHeader> fields;
        const std::size_t count = call == 0 ? 3 : 4;
        for (std::size_t index = 0; index < count; ++index) fields.push_back({"X-C" + std::to_string(index), "v"});
        return fields;
    });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields = {{"X-A", "v"}, {"X-B", "v"}};
    described.header_fields_for = supplier.callback();
    described.max_header_fields = 5;
    Driven driven(described);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 0u);
    driven.post(2, ping_line);
    const auto past = driven.recorder().until_end(2);
    ASSERT_EQ(events_of<SendFailed>(past).size(), 1u);
    EXPECT_TRUE(mentions(events_of<SendFailed>(past).front().cause, "max_header_fields"));
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_EQ(server.count(), 1u);
    EXPECT_EQ(server.requests().front().fields_named("X-C2"), std::vector<std::string>{"v"});
}

TEST(ClientHeaderFields, TheFieldsOfTheCallbackAloneAreBoundedAtTheEdgeAndOnePast) {
    ScriptedHttpServer server = answering();
    Supplier supplier([](HttpRequestKind, std::size_t call) {
        std::vector<HttpHeader> fields;
        const std::size_t count = call == 0 ? 3 : 4;
        for (std::size_t index = 0; index < count; ++index) fields.push_back({"X-C" + std::to_string(index), "v"});
        return fields;
    });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields_for = supplier.callback();
    described.max_header_fields = 3;
    Driven driven(described);
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 0u);
    driven.post(2, ping_line);
    const auto past = driven.recorder().until_end(2);
    ASSERT_EQ(events_of<SendFailed>(past).size(), 1u);
    EXPECT_TRUE(mentions(events_of<SendFailed>(past).front().cause, "max_header_fields"));
}

TEST(ClientHeaderFields, TheBytesOfFixedFieldsAndTheCallbackShareTheBound) {
    ScriptedHttpServer server = answering();
    // "X-A" with 7 bytes of value is 10 bytes; the callback's "X-B" with 87 or 88 makes 100 or 101.
    Supplier supplier([](HttpRequestKind, std::size_t call) {
        return std::vector<HttpHeader>{{"X-B", std::string(call == 0 ? 87 : 88, 'v')}};
    });
    neomcp::HttpEndpoint described = endpoint(server.url());
    described.header_fields = {{"X-A", std::string(7, 'v')}};
    described.header_fields_for = supplier.callback();
    described.max_header_field_bytes = 100;
    Driven driven(described);
    ASSERT_EQ(driven.problem(), "");
    driven.post(1, ping_line);
    EXPECT_EQ(events_of<SendFailed>(driven.recorder().until_end(1)).size(), 0u);
    driven.post(2, ping_line);
    const auto past = driven.recorder().until_end(2);
    ASSERT_EQ(events_of<SendFailed>(past).size(), 1u);
    EXPECT_TRUE(mentions(events_of<SendFailed>(past).front().cause, "max_header_field_bytes"));
}

}  // namespace
