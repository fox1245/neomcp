// The subscriptions of a session on the endpoint protected by fixed tokens, over loopback sockets and on every revision
// that keeps a session: a later request with a narrower token of the same subject ends the subscriptions it cannot
// read, in its own session only; and a replacement of the table that takes a scope from the token a session
// subscribed with ends its stream, and the stream opened again with that token ends the subscription, which a later
// replacement that gives the scope back does not bring back.
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/sse_parser.h"
#include "support/fixed_tokens_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr const char* secret = "test://secret";  // needs secret:read
constexpr const char* open_resource = "test://open";
constexpr const char* updated_method = "notifications/resources/updated";
// Long enough for a stream that is to end to have ended; a stream that is to stay open is shown open by what it
// carries, never by this time passing.
constexpr std::chrono::milliseconds ends_within = 10s;

std::vector<json> messages_of(const std::string& body) {
    std::vector<json> messages;
    neomcp::http::SseParser parser(body.size() + 1);
    parser.feed(body, [&](neomcp::http::SseItem item) {
        if (const auto* event = std::get_if<neomcp::http::SseEvent>(&item)) messages.push_back(json::parse(event->data));
    });
    return messages;
}

int updates_of(const std::string& body, const std::string& uri) {
    int count = 0;
    for (const json& message : messages_of(body)) {
        if (message.value("method", "") == updated_method && message.at("params").at("uri") == uri) ++count;
    }
    return count;
}

std::unique_ptr<Call> open_stream(std::uint16_t port, const std::string& session, const std::string& token,
                                  const std::string& revision) {
    httplib::Headers headers = legacy_headers(session, token, revision);
    headers.erase("Accept");
    headers.emplace("Accept", "text/event-stream");
    return std::make_unique<Call>(port, "GET", std::move(headers));
}

Reply subscribe(std::uint16_t port, const std::string& session, const std::string& token, const std::string& uri,
                const std::string& revision) {
    return session_request(port, session, token, "resources/subscribe", json{{"uri", uri}}, revision);
}

void both_change(FixedHarness& harness) {
    harness.server.notify_resource_updated(secret);
    harness.server.notify_resource_updated(open_resource);
}

bool told_of_open(Call& stream, int times) {
    return stream.wait_until([times](const std::string& body) { return updates_of(body, open_resource) >= times; });
}

bool ended(Call& stream) {
    stream.finish(ends_within);
    return stream.finished();
}

class FixedTokensSubscriptions : public ::testing::TestWithParam<std::string> {};

TEST_P(FixedTokensSubscriptions, ALaterRequestWithANarrowerTokenEndsTheSubscriptionsOfItsOwnSessionOnly) {
    const std::string revision = GetParam();
    FixedHarness harness(
        {fixed("broad", "alice", {"mcp:use", "secret:read"}), fixed("narrow", "alice", {"mcp:use"})});
    const std::string narrowed = open_session(harness.port, "broad", revision);
    const std::string kept = open_session(harness.port, "broad", revision);
    for (const std::string& session : {narrowed, kept}) {
        ASSERT_EQ(subscribe(harness.port, session, "broad", secret, revision).status, 200);
        ASSERT_EQ(subscribe(harness.port, session, "broad", open_resource, revision).status, 200);
    }
    auto narrowed_stream = open_stream(harness.port, narrowed, "broad", revision);
    auto kept_stream = open_stream(harness.port, kept, "broad", revision);
    ASSERT_TRUE(narrowed_stream->wait_for_headers());
    ASSERT_TRUE(kept_stream->wait_for_headers());
    ASSERT_EQ(narrowed_stream->now().status, 200);
    ASSERT_EQ(kept_stream->now().status, 200);

    ASSERT_EQ(ping(harness.port, kept, "broad", revision).status, 200);
    ASSERT_EQ(ping(harness.port, narrowed, "narrow", revision).status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrowed_stream, 1));
    ASSERT_TRUE(told_of_open(*kept_stream, 1));
    EXPECT_EQ(updates_of(narrowed_stream->now().body, secret), 0);
    EXPECT_EQ(updates_of(kept_stream->now().body, secret), 1);
    narrowed_stream->disconnect();
    kept_stream->disconnect();
}

TEST_P(FixedTokensSubscriptions, AReplacementThatTakesTheScopeEndsTheSubscriptionOnceTheStreamOpensAgain) {
    const std::string revision = GetParam();
    FixedHarness harness({fixed("token", "alice", {"mcp:use", "secret:read"})});
    const std::string session = open_session(harness.port, "token", revision);
    ASSERT_EQ(subscribe(harness.port, session, "token", secret, revision).status, 200);
    ASSERT_EQ(subscribe(harness.port, session, "token", open_resource, revision).status, 200);
    auto stream = open_stream(harness.port, session, "token", revision);
    ASSERT_TRUE(stream->wait_for_headers());
    ASSERT_EQ(stream->now().status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*stream, 1));
    EXPECT_EQ(updates_of(stream->now().body, secret), 1);

    // The token loses the scope: its stream ends, and the stream opened again with it is not told of the resource.
    harness.replace({fixed("token", "alice", {"mcp:use"})});
    EXPECT_TRUE(ended(*stream));
    auto narrowed = open_stream(harness.port, session, "token", revision);
    ASSERT_TRUE(narrowed->wait_for_headers());
    ASSERT_EQ(narrowed->now().status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrowed, 1));
    EXPECT_EQ(updates_of(narrowed->now().body, secret), 0);

    // The scope given back: the stream ends again, and the one opened after is still not told, since the subscription
    // ended when the narrowed token opened the stream.
    harness.replace({fixed("token", "alice", {"mcp:use", "secret:read"})});
    EXPECT_TRUE(ended(*narrowed));
    auto restored = open_stream(harness.port, session, "token", revision);
    ASSERT_TRUE(restored->wait_for_headers());
    ASSERT_EQ(restored->now().status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*restored, 1));
    EXPECT_EQ(updates_of(restored->now().body, secret), 0);
    restored->disconnect();
}

INSTANTIATE_TEST_SUITE_P(FixedTokens, FixedTokensSubscriptions,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25"), [](const auto& info) {
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

}  // namespace
