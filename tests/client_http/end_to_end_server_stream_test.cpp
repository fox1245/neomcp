// A stream the server holds for the client ends when the token that opened it expires, and the client opens it again
// with a token it refreshed: the subscription of the revision that has them (a request that stays open), and the stream
// of the connection of a revision with sessions.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "end_to_end_server_rig.h"

namespace {

using namespace server_rig;
using oauth_rig::grants_of;

// The requests of the server's log that open a stream: the call that subscribes (the revision with subscriptions), or
// the GET of the endpoint (the stream of a connection).
std::vector<Seen> stream_openings(ServerWorld& world, bool subscriptions) {
    std::vector<Seen> out;
    for (Seen& one : world.log().all()) {
        if (one.path != endpoint_path) continue;
        const bool opens = subscriptions ? one.rpc_method == "subscriptions/listen" : one.method == "GET";
        if (opens) out.push_back(std::move(one));
    }
    return out;
}

class ServerEndToEndStream : public ::testing::TestWithParam<std::string> {};

TEST_P(ServerEndToEndStream, IsClosedWhenItsTokenExpiresAndOpenedAgainWithARefreshedToken) {
    ServerSetup setup;
    // The stream ends when the token expires, not a leeway later.
    setup.authorization = [](neomcp::http::ResourceServerOptions& options) { options.bounds.leeway = 0s; };
    ServerWorld world(std::move(setup));
    use_revision(world, GetParam());
    world.as.issue_refresh_tokens(true);
    // The first token is short; the ones after it last.
    world.as.token_lifetime(3s);
    auto client = world.connect(world.authorizer(), [](neomcp::ClientOptions& options) {
        options.subscription_reopen_first_delay = 20ms;
        options.subscription_reopen_max_delay = 100ms;
    });
    ASSERT_TRUE(client) << client.error().message;

    neomcp::ChangeFilter filter;
    filter.tools_list = true;
    auto subscription = client->listen(filter);
    ASSERT_TRUE(subscription) << subscription.error().message;
    // How the revision tells of changes decides which request holds the stream: a request of its own for each
    // subscription, or the GET of the connection.
    const bool subscriptions = subscription->delivery() == neomcp::FeedDelivery::subscription;
    ChangeCollector changes(std::move(*subscription));
    world.as.token_lifetime(3600s);
    ASSERT_EQ(world.as.access_tokens().size(), 1u);
    const std::string first = world.as.access_tokens().front();

    // The server ends the stream when the first token expires; the client opens it again, is refused for the expired
    // token, refreshes it without the person, and is let in.
    ASSERT_TRUE(wait_until([&] { return stream_openings(world, subscriptions).size() >= 3; }))
        << "the stream was not opened again";
    ASSERT_TRUE(wait_until([&] { return world.as.access_tokens().size() >= 2; }));
    const std::string second = world.as.access_tokens()[1];
    const std::vector<Seen> openings = stream_openings(world, subscriptions);
    ASSERT_GE(openings.size(), 3u);
    EXPECT_EQ(openings[0].authorization, "Bearer " + first);
    EXPECT_EQ(openings[0].status, 200);
    EXPECT_EQ(openings[1].authorization, "Bearer " + first);
    EXPECT_EQ(openings[1].status, 401);
    EXPECT_EQ(openings[2].authorization, "Bearer " + second);
    EXPECT_EQ(openings[2].status, 200);
    EXPECT_EQ(grants_of(world.as, "refresh_token"), 1u);
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_EQ(world.as.requests_to("/register").size(), 1u);

    // The new stream is alive: a change of the list reaches the client through it, and the subscription has not ended.
    if (subscriptions) {
        // A subscription that was opened again reports what it honours as changed once.
        ASSERT_TRUE(changes.wait_for(1));
    }
    const std::size_t before = changes.count();
    ASSERT_TRUE(world.items.switched);
    world.items.switched->enable(true);
    EXPECT_TRUE(changes.wait_for(before + 1)) << "no change arrived on the stream opened again";
    EXPECT_FALSE(changes.ended());
    expect_tokens_kept(world);
}

INSTANTIATE_TEST_SUITE_P(ServerEndToEndStreams, ServerEndToEndStream, ::testing::Values("2026-07-28", "2025-11-25"),
                         [](const auto& info) { return revision_name(info.param); });

}  // namespace
