// URLs as the flow reads them: the strict reader, origins, loopback hosts, the well-known URLs, and the identity of
// resource identifiers at its edges.
#include <gtest/gtest.h>

#include "support.h"

namespace {

using namespace oauth_test;

TEST(OAuthFlowUrl, ReadsThePartsWithoutChangingThem) {
    const auto url = read_url("HTTPS://Example.COM:8443/a/B%2Fc?x=1&y#frag");
    ASSERT_TRUE(url);
    EXPECT_EQ(url->scheme, "HTTPS");
    EXPECT_EQ(url->authority, "Example.COM:8443");
    EXPECT_EQ(url->host, "Example.COM");
    EXPECT_EQ(url->port, "8443");
    EXPECT_EQ(url->path, "/a/B%2Fc");
    EXPECT_EQ(url->query, "x=1&y");
    EXPECT_EQ(url->fragment, "frag");
    EXPECT_EQ(url->parsed.host, "example.com");
    EXPECT_EQ(without_fragment(*url), "HTTPS://Example.COM:8443/a/B%2Fc?x=1&y");
    EXPECT_EQ(origin_text(*url), "HTTPS://Example.COM:8443");
}

TEST(OAuthFlowUrl, ReadsAnIpv6Host) {
    const auto url = read_url("http://[::1]:3000/cb");
    ASSERT_TRUE(url);
    EXPECT_EQ(url->host, "[::1]");
    EXPECT_EQ(url->port, "3000");
    EXPECT_TRUE(is_loopback(*url));
}

TEST(OAuthFlowUrl, RefusesWhatCouldBeReadTwoWays) {
    for (const char* text : {"", "example.com/x", "https:/x", "https://", "https://user@example.com/",
                             "https://user:pw@example.com/", "https://exa mple.com/", "https://example.com/a b",
                             "https://example.com/a\\b", "https://example.com/%zz", "https://example.com/%4",
                             "https://example.com:0/", "https://example.com:99999/", "https://example.com/\x01",
                             "https://example.com/?a=\"b\"", "https://example.com/#a#b",
                             "https://evil.example\\@good.example/", "https://[::1/"}) {
        EXPECT_FALSE(read_url(text)) << text;
    }
}

TEST(OAuthFlowUrl, OriginsCompareSchemeAndHostWithoutCaseAndTheDefaultPort) {
    const auto a = read_url("https://Example.com/x");
    const auto b = read_url("HTTPS://example.COM:443/y?z");
    const auto c = read_url("https://example.com:8443/x");
    const auto d = read_url("http://example.com/x");
    ASSERT_TRUE(a && b && c && d);
    EXPECT_TRUE(same_origin(*a, *b));
    EXPECT_FALSE(same_origin(*a, *c));
    EXPECT_FALSE(same_origin(*a, *d));
}

TEST(OAuthFlowUrl, LoopbackIsDecidedFromTheParsedHost) {
    for (const char* yes : {"http://localhost/", "http://LOCALHOST:1/", "http://127.0.0.1/", "http://127.255.0.9:8/",
                            "http://[::1]/", "http://[0:0:0:0:0:0:0:1]/"}) {
        const auto url = read_url(yes);
        ASSERT_TRUE(url) << yes;
        EXPECT_TRUE(is_loopback(*url)) << yes;
    }
    for (const char* no : {"http://127.0.0.1.evil.example/", "http://localhost.evil.example/",
                           "http://evil.localhost/", "http://[::ffff:127.0.0.1]/", "http://128.0.0.1/",
                           "http://10.0.0.1/", "http://example.com/"}) {
        const auto url = read_url(no);
        ASSERT_TRUE(url) << no;
        EXPECT_FALSE(is_loopback(*url)) << no;
    }
}

TEST(OAuthFlowUrl, CredentialsGoOnlyOverHttpsOrToLoopback) {
    EXPECT_TRUE(is_secure_or_loopback(*read_url("https://example.com/")));
    EXPECT_TRUE(is_secure_or_loopback(*read_url("http://localhost:3000/")));
    EXPECT_FALSE(is_secure_or_loopback(*read_url("http://example.com/")));
    EXPECT_FALSE(is_secure_or_loopback(*read_url("ftp://example.com/")));
}

TEST(OAuthFlowUrl, WellKnownUrlsInsertTheSuffixBeforeThePath) {
    EXPECT_EQ(insert_well_known(*read_url("https://example.com"), "x", true), "https://example.com/.well-known/x");
    EXPECT_EQ(insert_well_known(*read_url("https://example.com/"), "x", true), "https://example.com/.well-known/x");
    EXPECT_EQ(insert_well_known(*read_url("https://example.com/r1"), "x", true),
              "https://example.com/.well-known/x/r1");
    EXPECT_EQ(insert_well_known(*read_url("https://example.com/r1/"), "x", true),
              "https://example.com/.well-known/x/r1");
    EXPECT_EQ(insert_well_known(*read_url("https://Example.com:8443/r1?t=1#f"), "x", true),
              "https://Example.com:8443/.well-known/x/r1?t=1");
    EXPECT_EQ(insert_well_known(*read_url("https://example.com/r1?t=1"), "x", false),
              "https://example.com/.well-known/x/r1");
    EXPECT_EQ(append_well_known(*read_url("https://example.com/tenant1/"), "y"),
              "https://example.com/tenant1/.well-known/y");
}

TEST(OAuthFlowUrl, ResourceIdentityIgnoresTheCaseOfSchemeAndHostOnly) {
    EXPECT_TRUE(same_resource_identifier("https://example.com/mcp", "https://example.com/mcp"));
    EXPECT_TRUE(same_resource_identifier("https://example.com/mcp", "HTTPS://EXAMPLE.com/mcp"));
    EXPECT_TRUE(same_resource_identifier("http://localhost:3001", "http://LocalHost:3001"));
    // Everything else compares character by character.
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.com/MCP"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.com/mcp/"));
    EXPECT_FALSE(same_resource_identifier("https://example.com", "https://example.com/"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.com:443/mcp"));
    EXPECT_FALSE(same_resource_identifier("https://example.com:443/mcp", "https://example.com/mcp"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.com/%6Dcp"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/a%2fb", "https://example.com/a%2Fb"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.com/mcp?"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.com/mcp#x"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.org/mcp"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "http://example.com/mcp"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "https://example.com./mcp"));
    EXPECT_FALSE(same_resource_identifier("https://example.com/mcp", "not a url"));
}

TEST(OAuthFlowUrl, WithoutQueryDropsQueryAndFragment) {
    EXPECT_EQ(without_query("https://a.example/t?code=1#x"), "https://a.example/t");
    EXPECT_EQ(without_query("https://a.example/t#x?y"), "https://a.example/t");
    EXPECT_EQ(without_query("https://a.example/t"), "https://a.example/t");
}

}  // namespace
