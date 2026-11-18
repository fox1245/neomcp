// Tests of the Host and Origin policy: the authority and Origin parsers, the loopback decision and the policy.
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/policy.h"

namespace {

using neomcp::http::Authority;
using neomcp::http::HostCheck;
using neomcp::http::HostPolicy;
using neomcp::http::Origin;
using neomcp::http::PolicyOptions;
using neomcp::http::parse_authority;
using neomcp::http::parse_origin;

std::string text(const std::optional<Authority>& authority) {
    return authority ? authority->to_string() : "<invalid>";
}

// ---- parse_authority -------------------------------------------------------------------------------------------------

TEST(HttpAuthority, AcceptsTheFormsOfTheGrammar) {
    struct Case {
        const char* input;
        const char* host;
        bool ipv6;
        std::optional<std::uint16_t> port;
    };
    const std::vector<Case> cases = {
        {"localhost", "localhost", false, std::nullopt},
        {"localhost:8080", "localhost", false, 8080},
        {"LocalHost:8080", "localhost", false, 8080},          // upper case is folded
        {"EXAMPLE.COM", "example.com", false, std::nullopt},
        {"example.com.", "example.com", false, std::nullopt},  // a fully qualified name loses its dot
        {"example.com.:443", "example.com", false, 443},
        {"127.0.0.1", "127.0.0.1", false, std::nullopt},
        {"127.0.0.1:1", "127.0.0.1", false, 1},
        {"127.0.0.1:65535", "127.0.0.1", false, 65535},
        {"host:80", "host", false, 80},                        // a default port is a port
        {"host:0080", "host", false, 80},                      // leading zeros are a number
        {"a-b_c~d.example", "a-b_c~d.example", false, std::nullopt},
        {"[::1]", "::1", true, std::nullopt},
        {"[::1]:3000", "::1", true, 3000},
        {"[0:0:0:0:0:0:0:1]", "::1", true, std::nullopt},      // canonical form
        {"[0000:0000::0001]:9", "::1", true, 9},
        {"[2001:DB8::A]", "2001:db8::a", true, std::nullopt},
        {"[::ffff:127.0.0.1]", "::ffff:127.0.0.1", true, std::nullopt},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.input);
        const std::optional<Authority> got = parse_authority(c.input);
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(got->host, c.host);
        EXPECT_EQ(got->ipv6, c.ipv6);
        EXPECT_EQ(got->port, c.port);
    }
}

TEST(HttpAuthority, RejectsWhatTheGrammarDoesNot) {
    const std::vector<std::string> cases = {
        "",                       // empty
        ":80",                    // empty host
        ".",                      // only a dot
        "..",                     //
        ".example.com",           // empty first label
        "a..b",                   // empty label
        "example.com..",          // two trailing dots
        "host:",                  // empty port
        "host:0",                 // port 0
        "host:65536",             // out of range
        "host:99999",             //
        "host:123456",            // too many digits
        "host:80a",               // not a number
        "host:-1",                //
        "host:+1",                //
        "host:8 0",               //
        "host: 80",               //
        " host",                  // whitespace
        "host ",                  //
        "user@host",              // userinfo
        "user:pw@host:80",        //
        "host/path",              // a path
        "host:80/",               //
        "host?x=1",               // a query
        "host#frag",              // a fragment
        "http://host",            // a scheme
        "ho st",                  //
        "host:80:81",             // two colons
        "::1",                    // IPv6 without brackets
        "::1:80",                 //
        "[::1",                   // unterminated
        "::1]",                   //
        "[]",                     // empty brackets
        "[]:80",                  //
        "[::1]:",                 //
        "[::1]:0",                //
        "[::1]x",                 // junk after the bracket
        "[::1]:80:81",            //
        "[::1%eth0]",             // a zone identifier
        "[::1%25eth0]",           //
        "[fe80::1%lo]:80",        //
        "[12345::1]",             // not an IPv6 address
        "[host]",                 // a name in brackets
        "[127.0.0.1]",            // an IPv4 address in brackets
        "ex%61mple.com",          // percent-encoding
        "exa!mple.com",           // a sub-delimiter
        "exa mple",               //
        "exämple.com",            // non-ASCII
        "exa\tmple",              // control character
        "host\r\nX: y",           // header injection
        "[::1]\n",                //
    };
    for (const std::string& input : cases) {
        SCOPED_TRACE(input);
        EXPECT_FALSE(parse_authority(input).has_value()) << text(parse_authority(input));
    }
}

TEST(HttpAuthority, ToStringBracketsIpv6AndAppendsThePort) {
    EXPECT_EQ(text(parse_authority("[::1]:80")), "[::1]:80");
    EXPECT_EQ(text(parse_authority("Host:5")), "host:5");
    EXPECT_EQ(text(parse_authority("host.")), "host");
}

// ---- parse_origin ----------------------------------------------------------------------------------------------------

TEST(HttpOrigin, AcceptsSchemeAndAuthority) {
    struct Case {
        const char* input;
        const char* scheme;
        const char* authority;
    };
    const std::vector<Case> cases = {
        {"http://localhost", "http", "localhost"},
        {"https://example.com:8443", "https", "example.com:8443"},
        {"HTTP://Example.COM", "http", "example.com"},
        {"http://[::1]:5173", "http", "[::1]:5173"},
        {"http://127.0.0.1:80", "http", "127.0.0.1:80"},  // a default port is kept as written
        {"vscode-webview://abcdef", "vscode-webview", "abcdef"},
        {"app+x.y-z://host", "app+x.y-z", "host"},
        {"http://example.com.", "http", "example.com"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.input);
        const std::optional<Origin> got = parse_origin(c.input);
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(got->scheme, c.scheme);
        EXPECT_EQ(got->authority.to_string(), c.authority);
    }
}

TEST(HttpOrigin, RejectsNullAndEverythingThatIsNotAnOrigin) {
    const std::vector<std::string> cases = {
        "",                       //
        "null",                   // an opaque origin is never accepted
        "NULL",                   //
        "http://",                // no host
        "://host",                // no scheme
        "host",                   // no scheme
        "host:80",                //
        "//host",                 //
        "http:/host",             //
        "http:host",              //
        "http://host/",           // a path, even "/"
        "http://host/path",       //
        "http://host?x",          //
        "http://host#x",          //
        "http://user@host",       // userinfo
        "http://host:",           // empty port
        "http://host:0",          //
        "http://host:70000",      //
        "http://host:x",          //
        "http://[::1",            //
        "http://::1",             //
        "1http://host",           // a scheme starts with a letter
        "ht tp://host",           //
        "ht_tp://host",           //
        "http://ho st",           //
        " http://host",           //
        "http://host ",           //
        "http://host, http://other",  // a list
        "http://host\r\n",        //
    };
    for (const std::string& input : cases) {
        SCOPED_TRACE(input);
        EXPECT_FALSE(parse_origin(input).has_value());
    }
}

// ---- is_loopback_address ---------------------------------------------------------------------------------------------

TEST(HttpLoopback, IsDecidedByParsingTheAddress) {
    using neomcp::http::is_loopback_address;
    EXPECT_TRUE(is_loopback_address("127.0.0.1"));
    EXPECT_TRUE(is_loopback_address("127.1.2.3"));        // all of 127.0.0.0/8
    EXPECT_TRUE(is_loopback_address("::1"));
    EXPECT_TRUE(is_loopback_address("0:0:0:0:0:0:0:1"));  // another spelling of the same address
    EXPECT_TRUE(is_loopback_address("::ffff:127.0.0.1")); // an IPv4-mapped loopback address
    EXPECT_TRUE(is_loopback_address("localhost"));
    EXPECT_TRUE(is_loopback_address("LOCALHOST"));

    EXPECT_FALSE(is_loopback_address(""));
    EXPECT_FALSE(is_loopback_address("0.0.0.0"));
    EXPECT_FALSE(is_loopback_address("::"));
    EXPECT_FALSE(is_loopback_address("192.168.1.10"));
    EXPECT_FALSE(is_loopback_address("10.0.0.1"));
    EXPECT_FALSE(is_loopback_address("128.0.0.1"));
    EXPECT_FALSE(is_loopback_address("::ffff:10.0.0.1"));
    EXPECT_FALSE(is_loopback_address("example.com"));      // a name is not resolved
    EXPECT_FALSE(is_loopback_address("localhost.example.com"));
    EXPECT_FALSE(is_loopback_address("127.0.0.1.example.com"));
    EXPECT_FALSE(is_loopback_address("[::1]"));            // brackets are not part of an address
    EXPECT_FALSE(is_loopback_address("127.0.0.1 "));
    EXPECT_FALSE(is_loopback_address("127.0.0.1:80"));     // a port is not part of an address
    EXPECT_FALSE(is_loopback_address("127.1"));            // shortened forms are not read
    EXPECT_FALSE(is_loopback_address("0127.0.0.1"));
    EXPECT_FALSE(is_loopback_address("::1%lo"));           // a zone identifier is not accepted
    EXPECT_FALSE(is_loopback_address("::1:"));
    EXPECT_FALSE(is_loopback_address(":1"));
    EXPECT_FALSE(is_loopback_address("0:0:0:0:0:0:0:0:1")); // nine groups
    EXPECT_FALSE(is_loopback_address("::00001"));           // five digits in a group
    EXPECT_FALSE(is_loopback_address("::ffff:127.0.0.1:80"));
    EXPECT_TRUE(is_loopback_address("0::1"));
    EXPECT_TRUE(is_loopback_address("::FFFF:127.0.0.1"));
}

// ---- HostPolicy ------------------------------------------------------------------------------------------------------

PolicyOptions loopback_at(std::uint16_t port) {
    PolicyOptions options;
    options.bind_address = "127.0.0.1";
    options.bound_port = port;
    return options;
}

HostPolicy build(const PolicyOptions& options) {
    auto outcome = HostPolicy::create(options);
    if (!outcome) throw std::runtime_error(outcome.error);
    return std::move(*outcome.value);
}

TEST(HttpPolicy, ALoopbackBindAllowsTheThreeNamesWithTheBoundPortOrNone) {
    const HostPolicy policy = build(loopback_at(5000));
    for (const char* host : {"localhost", "localhost:5000", "127.0.0.1", "127.0.0.1:5000", "[::1]", "[::1]:5000",
                             "LOCALHOST:5000", "localhost.", "[0:0:0:0:0:0:0:1]:5000"}) {
        EXPECT_TRUE(policy.host_allowed(host)) << host;
    }
    for (const char* host : {"localhost:5001", "127.0.0.1:80", "[::1]:1", "example.com", "example.com:5000",
                             "127.0.0.2", "0.0.0.0:5000", "localhost.evil.com", "evil.com#localhost", "",
                             "localhost:", "user@localhost", "::1", "127.0.0.1.nip.io"}) {
        EXPECT_FALSE(policy.host_allowed(host)) << host;
    }
}

TEST(HttpPolicy, WithoutABoundPortOnlyHostsWithoutAPortPass) {
    const HostPolicy policy = build(loopback_at(0));
    EXPECT_TRUE(policy.host_allowed("localhost"));
    EXPECT_FALSE(policy.host_allowed("localhost:5000"));
}

TEST(HttpPolicy, AnIpv6LoopbackBindAllowsTheSameDefaults) {
    PolicyOptions options = loopback_at(7);
    options.bind_address = "::1";
    const HostPolicy policy = build(options);
    EXPECT_TRUE(policy.host_allowed("[::1]:7"));
    EXPECT_TRUE(policy.host_allowed("localhost:7"));
}

TEST(HttpPolicy, ANonLoopbackBindWithoutHostsFailsClosedWithAClearMessage) {
    for (const char* address : {"0.0.0.0", "::", "192.168.1.5", "example.com", ""}) {
        PolicyOptions options;
        options.bind_address = address;
        options.bound_port = 1234;
        auto outcome = HostPolicy::create(options);
        EXPECT_FALSE(outcome) << address;
        EXPECT_FALSE(outcome.value.has_value());
        EXPECT_NE(outcome.error.find("allowed_hosts"), std::string::npos) << outcome.error;
        EXPECT_NE(outcome.error.find("HostCheck::off"), std::string::npos) << outcome.error;
        EXPECT_NE(outcome.error.find(address), std::string::npos) << outcome.error;
    }
}

TEST(HttpPolicy, ANonLoopbackBindWithHostsOrWithTheCheckOffBuilds) {
    PolicyOptions named;
    named.bind_address = "0.0.0.0";
    named.bound_port = 8080;
    named.allowed_hosts = {"mcp.example.com", "10.0.0.5:9000"};
    const HostPolicy with_names = build(named);
    EXPECT_TRUE(with_names.host_allowed("mcp.example.com"));
    EXPECT_TRUE(with_names.host_allowed("mcp.example.com:8080"));  // a name alone allows any port
    EXPECT_TRUE(with_names.host_allowed("MCP.Example.COM:443"));
    EXPECT_TRUE(with_names.host_allowed("10.0.0.5:9000"));         // an entry with a port allows exactly that port
    EXPECT_FALSE(with_names.host_allowed("10.0.0.5"));
    EXPECT_FALSE(with_names.host_allowed("10.0.0.5:9001"));
    EXPECT_FALSE(with_names.host_allowed("localhost"));             // the defaults do not apply when hosts are named
    EXPECT_FALSE(with_names.host_allowed("evil.example.com"));

    PolicyOptions off;
    off.bind_address = "0.0.0.0";
    off.host_check = HostCheck::off;
    const HostPolicy unchecked = build(off);
    EXPECT_TRUE(unchecked.host_allowed("anything.example"));
    EXPECT_TRUE(unchecked.host_allowed(""));
}

TEST(HttpPolicy, NamedHostsReplaceTheLoopbackDefaults) {
    PolicyOptions options = loopback_at(5000);
    options.allowed_hosts = {"app.local"};
    const HostPolicy policy = build(options);
    EXPECT_TRUE(policy.host_allowed("app.local:5000"));
    EXPECT_FALSE(policy.host_allowed("localhost"));
}

TEST(HttpPolicy, InvalidEntriesAreErrorsThatNameThem) {
    PolicyOptions bad_host = loopback_at(1);
    bad_host.allowed_hosts = {"good.example", "bad host"};
    auto host = HostPolicy::create(bad_host);
    EXPECT_FALSE(host);
    EXPECT_NE(host.error.find("bad host"), std::string::npos);

    PolicyOptions bad_origin = loopback_at(1);
    bad_origin.allowed_origins = {"https://good.example", "https://bad.example/path"};
    auto origin = HostPolicy::create(bad_origin);
    EXPECT_FALSE(origin);
    EXPECT_NE(origin.error.find("https://bad.example/path"), std::string::npos);

    PolicyOptions null_origin = loopback_at(1);
    null_origin.allowed_origins = {"null"};
    EXPECT_FALSE(HostPolicy::create(null_origin));
}

TEST(HttpPolicy, OriginsPassWhenTheirHostIsAllowedOrTheyAreListed) {
    PolicyOptions options = loopback_at(5000);
    options.allowed_origins = {"https://app.example.com", "http://localhost:5173"};
    const HostPolicy policy = build(options);

    EXPECT_TRUE(policy.origin_allowed("http://localhost:5000"));
    EXPECT_TRUE(policy.origin_allowed("http://127.0.0.1:5000"));
    EXPECT_TRUE(policy.origin_allowed("http://[::1]:5000"));
    EXPECT_TRUE(policy.origin_allowed("https://localhost"));            // any scheme; the host decides
    EXPECT_TRUE(policy.origin_allowed("https://app.example.com"));      // listed
    EXPECT_TRUE(policy.origin_allowed("HTTPS://APP.example.com"));
    EXPECT_TRUE(policy.origin_allowed("http://localhost:5173"));        // listed with its port

    EXPECT_FALSE(policy.origin_allowed("https://app.example.com:8443"));  // listed without a port: not this one
    EXPECT_FALSE(policy.origin_allowed("http://app.example.com"));        // another scheme
    EXPECT_FALSE(policy.origin_allowed("http://localhost:5174"));
    EXPECT_FALSE(policy.origin_allowed("http://evil.com"));
    EXPECT_FALSE(policy.origin_allowed("null"));
    EXPECT_FALSE(policy.origin_allowed(""));
    EXPECT_FALSE(policy.origin_allowed("localhost"));                   // not in origin form
    EXPECT_FALSE(policy.origin_allowed("http://localhost:5000/"));
}

TEST(HttpPolicy, NullIsNeverAllowedEvenWithTheHostCheckOff) {
    PolicyOptions options;
    options.bind_address = "0.0.0.0";
    options.host_check = HostCheck::off;
    options.allowed_origins = {"https://app.example.com"};
    const HostPolicy policy = build(options);
    EXPECT_FALSE(policy.origin_allowed("null"));
    EXPECT_TRUE(policy.origin_allowed("https://app.example.com"));
    EXPECT_FALSE(policy.origin_allowed("https://other.example.com"));  // no hosts are allowed, only the listed origin
}

TEST(HttpPolicy, CheckReadsTheHeadersOfARequest) {
    const HostPolicy policy = build(loopback_at(5000));

    neomcp::http::Headers ok{{"Host", "localhost:5000"}};
    EXPECT_TRUE(policy.check(ok).allowed);

    neomcp::http::Headers with_origin{{"host", "localhost:5000"}, {"ORIGIN", "http://localhost:5000"}};
    EXPECT_TRUE(policy.check(with_origin).allowed);

    neomcp::http::Headers none;
    auto missing = policy.check(none);
    EXPECT_FALSE(missing.allowed);
    EXPECT_NE(missing.reason.find("Host"), std::string::npos);

    neomcp::http::Headers wrong_host{{"Host", "evil.com"}};
    auto wrong = policy.check(wrong_host);
    EXPECT_FALSE(wrong.allowed);
    EXPECT_NE(wrong.reason.find("evil.com"), std::string::npos);

    neomcp::http::Headers two_hosts{{"Host", "localhost"}, {"Host", "localhost"}};
    EXPECT_FALSE(policy.check(two_hosts).allowed);

    neomcp::http::Headers bad_origin{{"Host", "localhost:5000"}, {"Origin", "http://evil.com"}};
    auto origin = policy.check(bad_origin);
    EXPECT_FALSE(origin.allowed);
    EXPECT_NE(origin.reason.find("Origin"), std::string::npos);

    neomcp::http::Headers null_origin{{"Host", "localhost:5000"}, {"Origin", "null"}};
    EXPECT_FALSE(policy.check(null_origin).allowed);

    neomcp::http::Headers two_origins{{"Host", "localhost:5000"},
                                      {"Origin", "http://localhost:5000"},
                                      {"Origin", "http://localhost:5000"}};
    EXPECT_FALSE(policy.check(two_origins).allowed);
}

TEST(HttpPolicy, WithTheHostCheckOffAMissingHostPassesButABadOriginDoesNot) {
    PolicyOptions options = loopback_at(5000);
    options.host_check = HostCheck::off;
    const HostPolicy policy = build(options);
    neomcp::http::Headers none;
    EXPECT_TRUE(policy.check(none).allowed);
    neomcp::http::Headers bad_origin{{"Origin", "http://evil.com"}};
    EXPECT_FALSE(policy.check(bad_origin).allowed);
}

}  // namespace
