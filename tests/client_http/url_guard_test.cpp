// The guard of URLs a server supplies: http and https only, no user information, and only the endpoint's origin.
#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "client_http/transfers.h"
#include "client_http/url_guard.h"

namespace {

using neomcp::detail::http_client::Origin;
using neomcp::detail::http_client::origin_of;
using neomcp::detail::http_client::resolve_reference;
using neomcp::detail::http_client::UrlGuard;
using neomcp::detail::http_client::UrlPurpose;

UrlGuard guard_for(const std::string& url) {
    auto made = UrlGuard::for_endpoint(url);
    if (auto* problem = std::get_if<std::string>(&made)) {
        ADD_FAILURE() << *problem;
    }
    return std::get<UrlGuard>(std::move(made));
}

bool admits(const UrlGuard& guard, const std::string& url) { return !guard.admit(UrlPurpose::redirect, url); }

TEST(ClientHttp, UrlGuardReadsTheOriginOfTheEndpoint) {
    const auto origin = origin_of("HTTP://Example.COM/mcp");
    ASSERT_TRUE(std::holds_alternative<Origin>(origin));
    EXPECT_EQ(std::get<Origin>(origin).scheme, "http");
    EXPECT_EQ(std::get<Origin>(origin).host, "example.com");
    EXPECT_EQ(std::get<Origin>(origin).port, 80);
    EXPECT_EQ(std::get<Origin>(origin_of("https://example.com/")).port, 443);
    EXPECT_EQ(std::get<Origin>(origin_of("http://127.0.0.1:8080")).port, 8080);
}

TEST(ClientHttp, UrlGuardRefusesEndpointsItCannotUse) {
    for (const char* url : {"ftp://example.com/", "file:///etc/hosts", "example.com/mcp", "/mcp", "",
                                  "http://user@example.com/", "http://user:secret@example.com/",
                                  "http://:secret@example.com/"}) {
        EXPECT_TRUE(std::holds_alternative<std::string>(UrlGuard::for_endpoint(url))) << url;
    }
    EXPECT_TRUE(std::holds_alternative<std::string>(UrlGuard::for_endpoint(std::string("http://a\0b/", 11))));
}

TEST(ClientHttp, UrlGuardAdmitsOnlyTheEndpointsOrigin) {
    const UrlGuard guard = guard_for("http://127.0.0.1:8080/mcp");
    EXPECT_TRUE(admits(guard, "http://127.0.0.1:8080/other"));
    EXPECT_TRUE(admits(guard, "HTTP://127.0.0.1:8080/other?x=1"));
    EXPECT_FALSE(admits(guard, "http://127.0.0.1:8081/mcp"));       // another port
    EXPECT_FALSE(admits(guard, "https://127.0.0.1:8080/mcp"));      // another scheme
    EXPECT_FALSE(admits(guard, "http://localhost:8080/mcp"));       // another host name for the same address
    EXPECT_FALSE(admits(guard, "http://127.0.0.2:8080/mcp"));
    EXPECT_FALSE(admits(guard, "http://user@127.0.0.1:8080/mcp"));  // user information
    EXPECT_FALSE(admits(guard, "ftp://127.0.0.1:8080/mcp"));
    EXPECT_FALSE(admits(guard, "/relative"));                       // only absolute URLs reach the guard
}

TEST(ClientHttp, UrlGuardComparesDefaultPortsAndHostCase) {
    const UrlGuard guard = guard_for("https://Example.com/mcp");
    EXPECT_TRUE(admits(guard, "https://example.com:443/next"));
    EXPECT_TRUE(admits(guard, "https://EXAMPLE.COM/next"));
    EXPECT_FALSE(admits(guard, "http://example.com/next"));
    EXPECT_FALSE(admits(guard, "https://example.com:8443/next"));
    EXPECT_FALSE(admits(guard, "https://example.com.evil.test/next"));
}

TEST(ClientHttp, UrlGuardNamesThePurposeOfARefusal) {
    const UrlGuard guard = guard_for("http://127.0.0.1:8080/mcp");
    const auto refusal = guard.admit(UrlPurpose::event_endpoint, "http://127.0.0.1:9/");
    ASSERT_TRUE(refusal);
    EXPECT_NE(refusal->find("endpoint for messages"), std::string::npos);
}

TEST(ClientHttp, ReferencesResolveAgainstTheBase) {
    EXPECT_EQ(resolve_reference("http://127.0.0.1:8080/a/b", "/c"), "http://127.0.0.1:8080/c");
    EXPECT_EQ(resolve_reference("http://127.0.0.1:8080/a/b", "c"), "http://127.0.0.1:8080/a/c");
    EXPECT_EQ(resolve_reference("http://127.0.0.1:8080/a/b", "http://other:1/x"), "http://other:1/x");
    EXPECT_FALSE(resolve_reference("not a url", "/c"));
}

TEST(ClientHttp, AHeaderValueIsSentInTheFormLibcurlNeeds) {
    using neomcp::detail::http_client::header_line;
    EXPECT_EQ(header_line("Mcp-Name", "add"), "Mcp-Name: add");
    // `Name:` with nothing after it would remove the field; `Name;` sends it empty.
    EXPECT_EQ(header_line("Mcp-Param-Empty", ""), "Mcp-Param-Empty;");
    EXPECT_EQ(header_line("Mcp-Param-Blank", "   "), "Mcp-Param-Blank;");
    EXPECT_EQ(header_line("Mcp-Param-Tab", "\t"), "Mcp-Param-Tab;");
    EXPECT_EQ(header_line("X", "caf\xC3\xA9"), "X: caf\xC3\xA9");
    EXPECT_FALSE(header_line("X", "a\r\nInjected: yes"));
    EXPECT_FALSE(header_line("X", "a\nb"));
    EXPECT_FALSE(header_line("X", std::string("a\0b", 3)));
    EXPECT_FALSE(header_line("Bad Name", "v"));
    EXPECT_FALSE(header_line("Bad:Name", "v"));
    EXPECT_FALSE(header_line("", "v"));
}

TEST(ClientHttp, ALibraryWithoutAnAsynchronousResolverIsRefusedUnlessAccepted) {
    using neomcp::detail::http_client::resolver_refusal;
    EXPECT_FALSE(resolver_refusal(true, false));
    EXPECT_FALSE(resolver_refusal(true, true));
    EXPECT_TRUE(resolver_refusal(false, false));
    EXPECT_FALSE(resolver_refusal(false, true));
}

TEST(ClientHttp, TheBodyOfAResponseIsPlannedFromItsHead) {
    using neomcp::detail::http_client::BodyKind;
    using neomcp::detail::http_client::plan_body;
    using Types = std::vector<std::string_view>;
    auto plan = plan_body(200, Types{"application/json; charset=utf-8"}, 10);
    EXPECT_EQ(plan.media_type, "application/json");
    EXPECT_EQ(plan.kind, BodyKind::json);
    plan = plan_body(200, Types{"Text/Event-Stream"}, -1);
    EXPECT_EQ(plan.media_type, "text/event-stream");
    EXPECT_EQ(plan.kind, BodyKind::event_stream);
    // An error answer may carry a JSON-RPC error; an event stream comes only with success.
    EXPECT_EQ(plan_body(400, Types{"application/json"}, 10).kind, BodyKind::json);
    EXPECT_EQ(plan_body(500, Types{"text/event-stream"}, -1).kind, BodyKind::discard);
    // Another type is named, so that the engine refuses it; its body is dropped.
    plan = plan_body(200, Types{"text/html"}, 10);
    EXPECT_EQ(plan.media_type, "text/html");
    EXPECT_EQ(plan.kind, BodyKind::discard);
    // A body whose type cannot be read: an engaged empty type.
    EXPECT_EQ(plan_body(200, Types{}, 10).media_type, std::optional<std::string>(""));
    EXPECT_EQ(plan_body(200, Types{}, -1).media_type, std::optional<std::string>(""));
    EXPECT_EQ(plan_body(200, Types{"application/json", "application/json"}, 10).media_type,
              std::optional<std::string>(""));
    EXPECT_EQ(plan_body(200, Types{"not a type"}, 10).media_type, std::optional<std::string>(""));
    // Nothing to read: no type at all.
    EXPECT_FALSE(plan_body(202, Types{}, -1).media_type);
    EXPECT_FALSE(plan_body(204, Types{}, -1).media_type);
    EXPECT_FALSE(plan_body(200, Types{}, 0).media_type);
    EXPECT_FALSE(plan_body(404, Types{}, 10).media_type);
}

}  // namespace
