// URLs written to be read two ways: the guard refuses every one that names another origin, carries user information,
// or holds bytes a request line cannot carry, whatever form the trick takes.
#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "client_http/url_guard.h"

namespace {

using neomcp::detail::http_client::Origin;
using neomcp::detail::http_client::origin_of;
using neomcp::detail::http_client::resolve_reference;
using neomcp::detail::http_client::UrlGuard;
using neomcp::detail::http_client::UrlPurpose;

constexpr const char* endpoint = "http://127.0.0.1:8080/mcp";

UrlGuard endpoint_guard() {
    auto made = UrlGuard::for_endpoint(endpoint);
    if (auto* problem = std::get_if<std::string>(&made)) {
        ADD_FAILURE() << *problem;
    }
    return std::get<UrlGuard>(std::move(made));
}

bool admits(const UrlGuard& guard, const std::string& url) { return !guard.admit(UrlPurpose::redirect, url); }

TEST(ClientHttp, UrlGuardRefusesUserInformationInEveryPlaceItCanHide) {
    const UrlGuard guard = endpoint_guard();
    for (const char* url : {
             "http://127.0.0.1:8080@other.test/",       // the endpoint's authority as user information
             "http://127.0.0.1:8080@127.0.0.1:8080/",   // the same origin, still with user information
             "http://other.test@127.0.0.1:8080/",
             "http://:@127.0.0.1:8080/",
             "http://%3a@127.0.0.1:8080/",
             "http://127.0.0.1:8080;x@other.test/",
         }) {
        EXPECT_FALSE(admits(guard, url)) << url;
    }
}

TEST(ClientHttp, UrlGuardRefusesAnotherHostBehindAFragmentOrABackslash) {
    const UrlGuard guard = endpoint_guard();
    for (const char* url : {
             "http://other.test#@127.0.0.1:8080/",
             "http://other.test?@127.0.0.1:8080/",
             "http://other.test\\@127.0.0.1:8080/",
             "http://127.0.0.1:8080\\@other.test/",
             "http://other.test/\\127.0.0.1:8080/",
         }) {
        // Whatever the parser makes of these, what it admits must be the endpoint's origin as the parser reads it.
        const auto origin = origin_of(url);
        const bool same = std::holds_alternative<Origin>(origin) &&
                          std::get<Origin>(origin) == std::get<Origin>(origin_of(endpoint));
        EXPECT_EQ(admits(guard, url), same) << url;
        if (std::holds_alternative<Origin>(origin)) {
            EXPECT_NE(std::get<Origin>(origin).host, "127.0.0.1") << url;
        }
    }
}

TEST(ClientHttp, UrlGuardRefusesBytesARequestCannotCarry) {
    const UrlGuard guard = endpoint_guard();
    for (const std::string& url : {
             std::string("http://127.0.0.1:8080/mcp\r\nX-Injected: 1"),
             std::string("http://127.0.0.1:8080/mcp\nX-Injected: 1"),
             std::string("http://127.0.0.1:8080/m cp"),
             std::string("http://127.0.0.1:8080/mcp\t"),
             std::string("http://127.0.0.1:8080/\x7f"),
             std::string("http://127.0.0.1:8080/a\0b", 26),
             std::string("http://127.0.0.1\r\n:8080/"),
         }) {
        EXPECT_FALSE(admits(guard, url)) << "a URL with a control byte or a space";
    }
}

TEST(ClientHttp, UrlGuardRefusesPortsThatWrapOrRepeatAndHostsThatAreEmpty) {
    const UrlGuard guard = endpoint_guard();
    for (const char* url : {
             "http://127.0.0.1:73616/",   // 8080 + 65536
             "http://127.0.0.1:4294975376/",
             "http://127.0.0.1:8080:8080/",
             "http://127.0.0.1:-8080/",
             "http://127.0.0.1:+8080/",
             "http://127.0.0.1:0/",
             "http:///mcp",
             "http://:8080/mcp",
             "http://[::ffff:127.0.0.1]:8080/",  // the same address in another form is another origin
             "http://[::1]:8080/",
             "http://127.0.0.1./",
         }) {
        EXPECT_FALSE(admits(guard, url)) << url;
    }
}

TEST(ClientHttp, UrlGuardJudgesAResolvedReferenceNotTheTextAServerSent) {
    const UrlGuard guard = endpoint_guard();
    // Network-path references keep the scheme and take another authority.
    for (const char* reference : {"//other.test/mcp", "//127.0.0.1:8081/mcp", "///other.test/mcp",
                                  "https://127.0.0.1:8080/mcp", "http:other.test/mcp"}) {
        const auto resolved = resolve_reference(endpoint, reference);
        if (!resolved) continue;  // a reference that cannot be resolved is refused by its caller
        const auto origin = origin_of(*resolved);
        const bool same = std::holds_alternative<Origin>(origin) &&
                          std::get<Origin>(origin) == std::get<Origin>(origin_of(endpoint));
        EXPECT_EQ(admits(guard, *resolved), same) << reference << " -> " << *resolved;
        if (same) {
            EXPECT_EQ(std::get<Origin>(origin).host, "127.0.0.1") << reference;
            EXPECT_EQ(std::get<Origin>(origin).port, 8080) << reference;
        }
    }
    // A path-relative reference that looks like an authority stays on the endpoint.
    const auto relative = resolve_reference(endpoint, "other.test:8081/mcp");
    if (relative) {
        const auto origin = origin_of(*relative);
        if (std::holds_alternative<Origin>(origin) && std::get<Origin>(origin).host != "127.0.0.1") {
            EXPECT_FALSE(admits(guard, *relative)) << *relative;
        }
    }
}

}  // namespace
