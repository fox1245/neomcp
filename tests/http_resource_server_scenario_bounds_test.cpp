// Scenarios of the bounds of an endpoint that is an OAuth protected resource, over loopback sockets, each at its edge
// and one past it: the verdicts remembered at once and for how long, the authorization servers named, and the size of
// the protected resource metadata document, at start and when the authorization servers are replaced.
//
// The tests that complement the other files of this feature are named ResourceServerScenario*.
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/resource_scenario.h"

namespace {

using namespace neomcp::scenario;
using namespace std::chrono_literals;
using neomcp::http::ResourceServerDefaults;

// ---- Remembered verdicts -----------------------------------------------------------------------------------------------

// The cache holds max_cache_entries verdicts; one more makes room by dropping the verdict nearest its end, here the one
// of the token that expires first.
TEST(ResourceServerScenarioBounds, TheCacheHoldsItsBoundOfVerdictsAndOneMoreDisplacesTheOneNearestItsEnd) {
    Harness harness([](neomcp::HttpServerOptions& options) {
        options.authorization->bounds.max_cache_entries = 2;
        options.authorization->bounds.leeway = 0s;
    });
    auto& clock = harness.clock;
    // Each ends with its token, before max_cache_age (60 s): the first nearest, the third last.
    const std::string first = harness.token("first", granted(clock, "a", {"mcp:use"}, 10s));
    const std::string second = harness.token("second", granted(clock, "a", {"mcp:use"}, 20s));
    const std::string third = harness.token("third", granted(clock, "a", {"mcp:use"}, 30s));
    EXPECT_EQ(list_tools(harness.port, first).status, 200);
    EXPECT_EQ(list_tools(harness.port, second).status, 200);
    EXPECT_EQ(harness.table->calls(), 2);
    // At the bound: both are remembered.
    EXPECT_EQ(list_tools(harness.port, first).status, 200);
    EXPECT_EQ(list_tools(harness.port, second).status, 200);
    EXPECT_EQ(harness.table->calls(), 2);
    // One past it: the third takes the place of the first, whose end is nearest; the second stays.
    EXPECT_EQ(list_tools(harness.port, third).status, 200);
    EXPECT_EQ(harness.table->calls(), 3);
    EXPECT_EQ(list_tools(harness.port, second).status, 200);
    EXPECT_EQ(list_tools(harness.port, third).status, 200);
    EXPECT_EQ(harness.table->calls(), 3);
    EXPECT_EQ(list_tools(harness.port, first).status, 200);
    EXPECT_EQ(harness.table->calls(), 4) << "the first was let go and asked about again";
}

TEST(ResourceServerScenarioBounds, ACacheOfNoEntriesOrAnAgeOfZeroRemembersNothing) {
    const auto calls_for_two_requests = [](const std::function<void(neomcp::http::ResourceServerBounds&)>& tweak,
                                           bool valid) {
        Harness harness([&](neomcp::HttpServerOptions& options) { tweak(options.authorization->bounds); });
        if (valid) harness.token("t", granted(harness.clock, "a", {"mcp:use"}));
        const int expected = valid ? 200 : 401;
        EXPECT_EQ(list_tools(harness.port, "t").status, expected);
        EXPECT_EQ(list_tools(harness.port, "t").status, expected);
        return harness.table->calls();
    };
    EXPECT_EQ(calls_for_two_requests([](auto& bounds) { bounds.max_cache_entries = 0; }, true), 2);
    EXPECT_EQ(calls_for_two_requests([](auto& bounds) { bounds.max_cache_entries = 0; }, false), 2);
    EXPECT_EQ(calls_for_two_requests([](auto& bounds) { bounds.max_cache_age = 0s; }, true), 2);
    EXPECT_EQ(calls_for_two_requests([](auto& bounds) { bounds.negative_cache_age = 0s; }, false), 2);
    // The same with one entry and an age of one second: the second request is answered from memory.
    EXPECT_EQ(calls_for_two_requests(
                  [](auto& bounds) {
                      bounds.max_cache_entries = 1;
                      bounds.max_cache_age = 1s;
                  },
                  true),
              1);
    EXPECT_EQ(calls_for_two_requests([](auto& bounds) { bounds.negative_cache_age = 1s; }, false), 1);
}

// ---- The authorization servers ----------------------------------------------------------------------------------------

std::vector<std::string> issuers(std::size_t count) {
    std::vector<std::string> list;
    for (std::size_t at = 0; at < count; ++at) list.push_back("https://auth-" + std::to_string(at) + ".example");
    return list;
}

TEST(ResourceServerScenarioBounds, AsManyAuthorizationServersAsAClientReadsAreServedAndOneMoreIsNot) {
    const std::size_t bound = ResourceServerDefaults::max_authorization_servers;
    {
        Harness harness([&](neomcp::HttpServerOptions& options) {
            auto list = issuers(bound - 1);
            list.insert(list.begin(), first_issuer);
            options.authorization->authorization_servers = list;
        });
        const Reply document = send(harness.port, "GET", {}, {}, document_path_of_endpoint);
        ASSERT_EQ(document.status, 200);
        EXPECT_EQ(document.message().at("authorization_servers").size(), bound);
        // A token of the last of them is served: each is one the endpoint accepts.
        harness.token("last", granted(harness.clock, "a", {"mcp:use"}, 3600s, issuers(bound - 1).back()));
        EXPECT_EQ(list_tools(harness.port, "last").status, 200);
        // Replacing them: the bound is accepted, one more is refused and changes nothing.
        EXPECT_TRUE(harness.http->update_authorization_servers(issuers(bound)));
        EXPECT_FALSE(harness.http->update_authorization_servers(issuers(bound + 1)));
        const Reply after = send(harness.port, "GET", {}, {}, document_path_of_endpoint);
        EXPECT_EQ(after.message().at("authorization_servers").size(), bound);
        EXPECT_EQ(after.message().at("authorization_servers").at(0).get<std::string>(), issuers(bound).front());
    }
    EXPECT_THROW(Harness([&](neomcp::HttpServerOptions& options) {
                     options.authorization->authorization_servers = issuers(bound + 1);
                 }),
                 std::runtime_error);
}

// ---- The metadata document ----------------------------------------------------------------------------------------------

// The document of the harness's options with a resource_name of `name_bytes` bytes; empty when the server does not start.
std::optional<std::string> document_with_name_of(std::size_t name_bytes) {
    try {
        Harness harness([&](neomcp::HttpServerOptions& options) {
            options.authorization->resource_name = std::string(name_bytes, 'n');
        });
        const Reply document = send(harness.port, "GET", {}, {}, document_path_of_endpoint);
        if (document.status != 200) return std::nullopt;
        return document.body;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

TEST(ResourceServerScenarioBounds, TheMetadataDocumentIsServedUpToTheBytesAClientReadsAndNotOneBytePast) {
    // The name is written as it is (its bytes need no escape), so the document grows with it byte for byte.
    const std::optional<std::string> baseline = document_with_name_of(1);
    ASSERT_TRUE(baseline);
    const std::size_t edge = ResourceServerDefaults::max_metadata_bytes - (baseline->size() - 1);
    const std::optional<std::string> largest = document_with_name_of(edge);
    ASSERT_TRUE(largest) << "a document of exactly the bound is served";
    EXPECT_EQ(largest->size(), ResourceServerDefaults::max_metadata_bytes);
    EXPECT_FALSE(document_with_name_of(edge + 1)) << "one byte more and the server does not start";
}

TEST(ResourceServerScenarioBounds, AuthorizationServersThatWouldMakeTheDocumentTooLargeAreRefusedAndChangeNothing) {
    const std::optional<std::string> baseline = document_with_name_of(1);
    ASSERT_TRUE(baseline);
    const std::size_t edge = ResourceServerDefaults::max_metadata_bytes - (baseline->size() - 1);
    // At the bound with the one issuer of the harness: an issuer one byte longer would take the document past it.
    Harness harness([&](neomcp::HttpServerOptions& options) {
        options.authorization->resource_name = std::string(edge, 'n');
    });
    const std::string longer = std::string(first_issuer) + "x";
    EXPECT_FALSE(harness.http->update_authorization_servers({longer}));
    const Reply document = send(harness.port, "GET", {}, {}, document_path_of_endpoint);
    ASSERT_EQ(document.status, 200);
    EXPECT_EQ(document.body.size(), ResourceServerDefaults::max_metadata_bytes);
    EXPECT_EQ(document.message().at("authorization_servers").at(0).get<std::string>(), first_issuer);
    // An issuer of the same length keeps the document at the bound, and is taken.
    const std::string same_length = std::string(first_issuer).substr(0, std::string(first_issuer).size() - 1) + "y";
    EXPECT_TRUE(harness.http->update_authorization_servers({same_length}));
    EXPECT_EQ(send(harness.port, "GET", {}, {}, document_path_of_endpoint).body.size(),
              ResourceServerDefaults::max_metadata_bytes);
}

}  // namespace
