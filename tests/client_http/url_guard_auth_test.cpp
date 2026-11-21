// The guard's rule for documents of authorization: another origin is allowed; https, or http only to a loopback host;
// a loopback host only from a loopback endpoint; never user information or a fragment; hosts that reach this machine
// without being loopback names never. Loopback is read from the parsed host, so that no spelling of an address passes
// for another.
#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "client_http/url_guard.h"

namespace {

using neomcp::detail::http_client::HostReach;
using neomcp::detail::http_client::reach_of;
using neomcp::detail::http_client::UrlGuard;
using neomcp::detail::http_client::UrlPurpose;

UrlGuard guard_for(const std::string& url) {
    auto made = UrlGuard::for_endpoint(url);
    if (auto* problem = std::get_if<std::string>(&made)) {
        ADD_FAILURE() << *problem;
    }
    return std::get<UrlGuard>(std::move(made));
}

bool admits(const UrlGuard& guard, const std::string& url) {
    return !guard.admit(UrlPurpose::authorization, url);
}

TEST(ClientHttpAuth, UrlGuardAdmitsAnAuthorizationServerOfAnotherOrigin) {
    const UrlGuard guard = guard_for("https://mcp.example.com/mcp");
    for (const char* url : {
             "https://auth.example.com/.well-known/oauth-authorization-server",
             "https://auth.example.com:8443/token?x=1",
             "HTTPS://AUTH.example.com/authorize",  // the scheme and host in another case
             "https://mcp.example.com/.well-known/oauth-protected-resource/mcp",
             "https://127.0.0.1.evil.example/",  // a name, not an address
         }) {
        EXPECT_TRUE(admits(guard, url)) << url;
    }
}

TEST(ClientHttpAuth, UrlGuardRefusesPlainHttpToAHostElsewhere) {
    for (const char* endpoint : {"https://mcp.example.com/mcp", "http://127.0.0.1:8080/mcp"}) {
        const UrlGuard guard = guard_for(endpoint);
        for (const char* url : {"http://auth.example.com/token", "http://127.0.0.1.evil.example/",
                                "http://[2001:db8::1]/"}) {
            EXPECT_FALSE(admits(guard, url)) << endpoint << " -> " << url;
        }
    }
}

// A server elsewhere cannot point the client at the machine it runs on, by any spelling of a loopback host.
TEST(ClientHttpAuth, UrlGuardRefusesThisMachineFromAnEndpointElsewhere) {
    const UrlGuard guard = guard_for("https://mcp.example.com/mcp");
    for (const char* url : {
             "https://127.0.0.1/", "http://127.0.0.1:9000/", "https://127.255.255.254/", "https://localhost/",
             "https://LOCALHOST/", "https://localhost./", "http://localhost:9000/", "https://[::1]/",
             "https://[0:0:0:0:0:0:0:1]/", "https://[::ffff:127.0.0.1]/", "https://[::ffff:7f00:1]/",
             "https://2130706433/", "https://127.1/", "https://0x7f.1/", "https://0177.0.0.1/", "http://2130706433/",
             "http://127.1/", "http://0x7f.1/",
             // Hosts that reach this machine without being loopback names are refused from anywhere.
             "https://0.0.0.0/", "https://[::]/", "https://foo.localhost/", "https://a.b.localhost./",
         }) {
        EXPECT_FALSE(admits(guard, url)) << url;
    }
}

TEST(ClientHttpAuth, UrlGuardAdmitsThisMachineFromALoopbackEndpoint) {
    for (const char* endpoint : {"http://127.0.0.1:8080/mcp", "http://localhost:8080/mcp", "http://[::1]:8080/mcp"}) {
        const UrlGuard guard = guard_for(endpoint);
        for (const char* url : {"http://127.0.0.1:9000/", "http://localhost:9000/token", "http://[::1]:9000/",
                                "https://127.0.0.1/", "https://auth.example.com/"}) {
            EXPECT_TRUE(admits(guard, url)) << endpoint << " -> " << url;
        }
        for (const char* url : {"http://0.0.0.0:9000/", "http://[::]:9000/", "http://foo.localhost:9000/",
                                "https://foo.localhost/"}) {
            EXPECT_FALSE(admits(guard, url)) << endpoint << " -> " << url;
        }
    }
}

TEST(ClientHttpAuth, UrlGuardRefusesUserInformationFragmentsAndRelativeUrls) {
    const UrlGuard guard = guard_for("http://127.0.0.1:8080/mcp");
    for (const char* url : {
             "https://user@auth.example.com/", "https://user:secret@auth.example.com/", "http://user@127.0.0.1:9000/",
             "https://auth.example.com/doc#fragment", "/relative", "auth.example.com/x",
             "ftp://auth.example.com/", "file:///etc/hosts", "",
         }) {
        EXPECT_FALSE(admits(guard, url)) << url;
    }
}

// The rule for authorization leaves the other purposes as they were: the endpoint's origin only.
TEST(ClientHttpAuth, UrlGuardKeepsRedirectsAndEventEndpointsOnTheEndpointsOrigin) {
    const UrlGuard guard = guard_for("https://mcp.example.com/mcp");
    EXPECT_TRUE(guard.admit(UrlPurpose::redirect, "https://auth.example.com/"));
    EXPECT_TRUE(guard.admit(UrlPurpose::event_endpoint, "https://auth.example.com/"));
    EXPECT_FALSE(guard.admit(UrlPurpose::redirect, "https://mcp.example.com/other"));
}

TEST(ClientHttpAuth, UrlGuardSaysWhetherAnEndpointCarriesTokens) {
    EXPECT_TRUE(guard_for("https://mcp.example.com/mcp").carries_tokens());
    EXPECT_TRUE(guard_for("http://127.0.0.1:8080/mcp").carries_tokens());
    EXPECT_TRUE(guard_for("http://localhost:8080/mcp").carries_tokens());
    EXPECT_TRUE(guard_for("http://[::1]:8080/mcp").carries_tokens());
    EXPECT_FALSE(guard_for("http://mcp.example.com/mcp").carries_tokens());
    EXPECT_FALSE(guard_for("http://0.0.0.0:8080/mcp").carries_tokens());
    EXPECT_FALSE(guard_for("http://foo.localhost:8080/mcp").carries_tokens());
}

TEST(ClientHttpAuth, UrlGuardReadsWhatAHostIsToThisMachine) {
    EXPECT_EQ(reach_of("127.0.0.1"), HostReach::loopback);
    EXPECT_EQ(reach_of("127.10.20.30"), HostReach::loopback);
    EXPECT_EQ(reach_of("localhost"), HostReach::loopback);
    EXPECT_EQ(reach_of("localhost."), HostReach::loopback);
    EXPECT_EQ(reach_of("[::1]"), HostReach::loopback);
    EXPECT_EQ(reach_of("[::ffff:127.0.0.1]"), HostReach::loopback);
    EXPECT_EQ(reach_of("0.0.0.0"), HostReach::local);
    EXPECT_EQ(reach_of("0.1.2.3"), HostReach::local);
    EXPECT_EQ(reach_of("[::]"), HostReach::local);
    EXPECT_EQ(reach_of("app.localhost"), HostReach::local);
    EXPECT_EQ(reach_of("128.0.0.1"), HostReach::other);
    EXPECT_EQ(reach_of("localhost.example.com"), HostReach::other);
    EXPECT_EQ(reach_of("notlocalhost"), HostReach::other);
    EXPECT_EQ(reach_of("[::2]"), HostReach::other);
    EXPECT_EQ(reach_of("[::ffff:10.0.0.1]"), HostReach::other);
    EXPECT_EQ(reach_of("example.com"), HostReach::other);
}

}  // namespace
