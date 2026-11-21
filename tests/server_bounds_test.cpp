// Tests that the bounds of ServerOptions that mirror bounds of SessionOptions reach the sessions a Server makes and
// take effect there: each at its limit and one past it, and a zero bound refused when the server is made.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

struct ItemParams {
    std::string id;
};
NEOMCP_DESCRIBE(ItemParams, id, "the item")

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;
// A revision that opens with a handshake, and one that does not; the bounds apply to both where they apply at all.
constexpr const char* handshake_revision = "2025-11-25";
constexpr const char* request_revision = "2026-07-28";

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

neomcp::ServerOptions bounded(neomcp::ServerOptions options) {
    options.name = "bounds-server";
    options.version = "1.0.0";
    return options;
}

// A server with a tool, a resource template for the URIs a client watches, and the options a test sets.
struct Fixture {
    explicit Fixture(neomcp::ServerOptions options) : server(bounded(std::move(options))) {
        server.tool("echo", "Answers", [] { return std::string("echo"); });
        server.resource_template<ItemParams>("test://item/{id}", "An item",
                                             [](const ItemParams& params) { return "item " + params.id; });
    }
    neomcp::Server server;
};

// A client of one revision; one that handshakes has done so, with the given capabilities.
struct Client {
    Client(neomcp::Server& server, const std::string& revision) : profile(builtin(revision)), peer(server) {
        if (profile->mechanism(profile::Slot::open) == "initialize") {
            if (!peer.initialize(*profile, patience)) throw std::runtime_error("the handshake was not answered");
        }
    }
    json request(const std::string& method, json params = json::object()) {
        auto answer = peer.request(Peer::make_request(*profile, next_id++, method, std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer to " + method);
        return *answer;
    }
    profile::ProfilePtr profile;
    Peer peer;
    std::uint64_t next_id = 100;
};

std::string item_uri(std::size_t length, char fill = 'u') {
    const std::string prefix = "test://item/";
    return prefix + std::string(length - prefix.size(), fill);
}

bool refused_for_bound(const json& answer) {
    return answer.contains("error") && answer.at("error").at("code") == neomcp::error_code::internal_error &&
           answer.at("error").at("data") == json{{"reason", "limit"}};
}

int error_code_of(const json& answer) {
    return answer.contains("error") ? answer.at("error").at("code").get<int>() : 0;
}

TEST(ServerBounds, TheDefaultsAreThoseOfASession) {
    const neomcp::ServerOptions options{.name = "x", .version = "1"};
    const neomcp::SessionOptions session;
    EXPECT_EQ(options.max_client_info_bytes, session.max_client_info_bytes);
    EXPECT_EQ(options.max_token_bytes, session.max_token_bytes);
    EXPECT_EQ(options.max_watched_uri_bytes, session.max_watched_uri_bytes);
    EXPECT_EQ(options.max_watched_bytes, session.max_watched_bytes);
}

TEST(ServerBounds, ABoundOfZeroIsRefusedWhenTheServerIsMade) {
    EXPECT_THROW(neomcp::Server(neomcp::ServerOptions{.name = "x", .version = "1", .max_client_info_bytes = 0}),
                 std::invalid_argument);
    EXPECT_THROW(neomcp::Server(neomcp::ServerOptions{.name = "x", .version = "1", .max_token_bytes = 0}),
                 std::invalid_argument);
    EXPECT_THROW(neomcp::Server(neomcp::ServerOptions{.name = "x", .version = "1", .max_watched_uri_bytes = 0}),
                 std::invalid_argument);
    EXPECT_THROW(neomcp::Server(neomcp::ServerOptions{.name = "x", .version = "1", .max_watched_bytes = 0}),
                 std::invalid_argument);
}

TEST(ServerBounds, WhatAHandshakeSaysAboutTheClientIsBoundedByTheOptionOfTheServer) {
    const auto legacy = builtin(handshake_revision);
    constexpr std::size_t bound = 512;
    const auto info_of = [](std::size_t name_bytes) {
        return json{{"name", std::string(name_bytes, 'n')}, {"version", "1"}};
    };
    const json capabilities = json{{"roots", json::object()}};
    const std::size_t fixed = neomcp::serialize(info_of(0)).size() + neomcp::serialize(capabilities).size();
    ASSERT_LT(fixed, bound);
    for (const std::size_t over : {std::size_t{0}, std::size_t{1}}) {
        SCOPED_TRACE(over);
        Fixture fixture(neomcp::ServerOptions{.max_client_info_bytes = bound});
        Peer peer(fixture.server);
        json request = Peer::initialize_request(*legacy, 1, capabilities);
        request["params"]["clientInfo"] = info_of(bound - fixed + over);
        const auto answer = peer.request(request, patience);
        ASSERT_TRUE(answer);
        if (over == 0) {
            EXPECT_TRUE(answer->contains("result")) << "at the bound: " << answer->dump().substr(0, 200);
        } else {
            EXPECT_EQ(error_code_of(*answer), neomcp::error_code::invalid_params) << answer->dump().substr(0, 200);
        }
    }
}

TEST(ServerBounds, AStringProgressTokenIsBoundedByTheOptionOfTheServer) {
    constexpr std::size_t bound = 16;
    for (const char* revision : {handshake_revision, request_revision}) {
        SCOPED_TRACE(revision);
        Fixture fixture(neomcp::ServerOptions{.max_token_bytes = bound});
        Client client(fixture.server, revision);
        const auto call = [&](const std::string& token) {
            return client.request("tools/call", json{{"name", "echo"}, {"_meta", {{"progressToken", token}}}});
        };
        const json fits = call(std::string(bound, 't'));
        EXPECT_TRUE(fits.contains("result")) << "at the bound: " << fits.dump().substr(0, 200);
        const json refused = call(std::string(bound + 1, 't'));
        EXPECT_EQ(error_code_of(refused), neomcp::error_code::invalid_params) << refused.dump().substr(0, 200);
    }
}

TEST(ServerBounds, TheIdOfASubscriptionRequestIsBoundedByTheOptionOfTheServer) {
    constexpr std::size_t bound = 24;
    Fixture fixture(neomcp::ServerOptions{.max_token_bytes = bound});
    Client client(fixture.server, request_revision);
    const json filter{{"notifications", {{"toolsListChanged", true}}}};
    const std::string longer(bound + 1, 'i');
    const auto answer = client.peer.request(Peer::make_request(*client.profile, longer, "subscriptions/listen", filter),
                                            patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), neomcp::error_code::invalid_params) << answer->dump().substr(0, 200);
    const std::string fit(bound, 'i');
    client.peer.notify(Peer::make_request(*client.profile, fit, "subscriptions/listen", filter));
    const auto acknowledged = client.peer.next_notification(patience);
    ASSERT_TRUE(acknowledged);
    EXPECT_EQ(acknowledged->at("method"), "notifications/subscriptions/acknowledged") << "at the bound";
}

TEST(ServerBounds, AWatchedUriIsBoundedByTheOptionOfTheServer) {
    constexpr std::size_t bound = 64;
    Fixture fixture(neomcp::ServerOptions{.max_watched_uri_bytes = bound});
    Client client(fixture.server, handshake_revision);
    const json fits = client.request("resources/subscribe", json{{"uri", item_uri(bound)}});
    EXPECT_TRUE(fits.contains("result")) << "at the bound: " << fits.dump().substr(0, 200);
    const std::string longer = item_uri(bound + 1, '8');
    const json refused = client.request("resources/subscribe", json{{"uri", longer}});
    EXPECT_TRUE(refused_for_bound(refused)) << refused.dump().substr(0, 200);
    EXPECT_EQ(refused.dump().find(longer), std::string::npos) << "the answer repeats the URI";
}

TEST(ServerBounds, TheBytesOfTheWatchedUrisAreBoundedByTheOptionOfTheServer) {
    constexpr std::size_t uri_bytes = 40;
    constexpr std::size_t bound = 3 * uri_bytes;
    Fixture fixture(neomcp::ServerOptions{.max_watched_bytes = bound});
    Client client(fixture.server, handshake_revision);
    const auto subscribe = [&](const std::string& uri) {
        return client.request("resources/subscribe", json{{"uri", uri}});
    };
    const std::string first = item_uri(uri_bytes, '1');
    EXPECT_TRUE(subscribe(first).contains("result"));
    EXPECT_TRUE(subscribe(item_uri(uri_bytes, '2')).contains("result"));
    // 80 bytes are watched: a URI of 41 bytes would make 121, one past the bound, and one of 40 makes 120, the bound.
    const json past = subscribe(item_uri(uri_bytes + 1, '3'));
    EXPECT_TRUE(refused_for_bound(past)) << "one byte past the bound: " << past.dump().substr(0, 200);
    const json at = subscribe(item_uri(uri_bytes, '3'));
    EXPECT_TRUE(at.contains("result")) << "at the bound: " << at.dump().substr(0, 200);
    EXPECT_TRUE(refused_for_bound(subscribe("test://item/9"))) << "a short URI does not fit either";
    // Unwatching one gives its bytes back, so that a URI of the same length fits again.
    EXPECT_TRUE(client.request("resources/unsubscribe", json{{"uri", first}}).contains("result"));
    EXPECT_TRUE(subscribe(item_uri(uri_bytes, '4')).contains("result"));
}

}  // namespace
