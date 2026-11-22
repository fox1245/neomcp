// Scenarios of the subscriptions to resources on an endpoint that is an OAuth protected resource, over loopback sockets.
//
// On every revision that keeps a session (2025-03-26 to 2025-11-25) a subscription lasts while the session's requests
// may read the resource: a later request of the session whose token (of the same principal) lacks the scopes ends it,
// whether that request is a POST or the GET that opens the session's stream, and the token holding them again does not
// bring it back (the client subscribes again). A session that keeps its scopes is told as before, the stream of a
// session carries only the updates its own token may read, and the event stream of a request in progress carries none.
// On 2026-07-28, where each subscription is a request of its own, a later subscription with a narrower token is told
// only of what it may read.
//
// The tests that complement the other files of this feature are named ResourceServerScenario*.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/sse_parser.h"
#include "support/resource_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr const char* secret = "test://secret";  // needs secret:read
constexpr const char* open_resource = "test://open";
constexpr const char* updated_method = "notifications/resources/updated";
constexpr const char* slow_result = "slow-done";

// The messages of an event stream as read so far (an event not yet complete is left out).
std::vector<json> messages_of(const std::string& body) {
    std::vector<json> messages;
    neomcp::http::SseParser parser(body.size() + 1);
    parser.feed(body, [&](neomcp::http::SseItem item) {
        if (const auto* event = std::get_if<neomcp::http::SseEvent>(&item)) messages.push_back(json::parse(event->data));
    });
    return messages;
}

// How often the stream was told that `uri` was updated.
int updates_of(const std::string& body, const std::string& uri) {
    int count = 0;
    for (const json& message : messages_of(body)) {
        if (message.value("method", "") == updated_method && message.at("params").at("uri") == uri) ++count;
    }
    return count;
}

int updates(const std::string& body) {
    int count = 0;
    for (const json& message : messages_of(body)) {
        if (message.value("method", "") == updated_method) ++count;
    }
    return count;
}

// The stream of a session, opened with `token`.
std::unique_ptr<Call> open_stream(std::uint16_t port, const std::string& session, const std::string& token,
                                  const std::string& revision) {
    httplib::Headers headers = legacy_headers(session, token, revision);
    headers.erase("Accept");
    headers.emplace("Accept", "text/event-stream");
    return std::make_unique<Call>(port, "GET", std::move(headers));
}

// The stream of a session opened again after the client closed the one before: the endpoint answers 409 until it saw
// that close, so each 409 is followed by another attempt.
std::unique_ptr<Call> reopen_stream(std::uint16_t port, const std::string& session, const std::string& token,
                                    const std::string& revision) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    auto stream = open_stream(port, session, token, revision);
    while (stream->wait_for_headers() && stream->now().status == 409 && std::chrono::steady_clock::now() < deadline) {
        stream->finish();
        stream = open_stream(port, session, token, revision);
    }
    return stream;
}

Reply subscribe(std::uint16_t port, const std::string& session, const std::string& token, const std::string& uri,
                const std::string& revision) {
    return session_request(port, session, token, "resources/subscribe", json{{"uri", uri}}, revision);
}

// The secret resource changes, then the open one: once a stream that watches the open one was told of it, it would
// have been told of the secret one before.
void both_change(Harness& harness) {
    harness.server.notify_resource_updated(secret);
    harness.server.notify_resource_updated(open_resource);
}

bool told_of_open(Call& stream, int times) {
    return stream.wait_until([times](const std::string& body) { return updates_of(body, open_resource) >= times; });
}

class SubscriptionScopes : public ::testing::TestWithParam<std::string> {};

struct Tokens {
    std::string broad;   // alice, may read the secret resource
    std::string narrow;  // alice too, may not
};

Tokens tokens_of(Harness& harness) {
    auto& clock = harness.clock;
    return {harness.token("broad", granted(clock, "alice", {"mcp:use", "secret:read"})),
            harness.token("narrow", granted(clock, "alice", {"mcp:use"}))};
}

TEST_P(SubscriptionScopes, ALaterRequestWithANarrowerTokenEndsTheSubscriptionsOfItsOwnSessionOnly) {
    const std::string revision = GetParam();
    Harness harness;
    const Tokens tokens = tokens_of(harness);
    const std::string narrowed = open_session(harness.port, tokens.broad, revision);
    const std::string kept = open_session(harness.port, tokens.broad, revision);
    for (const std::string& session : {narrowed, kept}) {
        ASSERT_EQ(subscribe(harness.port, session, tokens.broad, secret, revision).status, 200);
        ASSERT_EQ(subscribe(harness.port, session, tokens.broad, open_resource, revision).status, 200);
    }
    auto narrowed_stream = open_stream(harness.port, narrowed, tokens.broad, revision);
    auto kept_stream = open_stream(harness.port, kept, tokens.broad, revision);
    ASSERT_TRUE(narrowed_stream->wait_for_headers());
    ASSERT_TRUE(kept_stream->wait_for_headers());
    ASSERT_EQ(narrowed_stream->now().status, 200);
    ASSERT_EQ(kept_stream->now().status, 200);

    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrowed_stream, 1));
    ASSERT_TRUE(told_of_open(*kept_stream, 1));
    EXPECT_EQ(updates_of(narrowed_stream->now().body, secret), 1) << "before any narrower token";
    EXPECT_EQ(updates_of(kept_stream->now().body, secret), 1);

    // The kept session's request keeps the scopes; the other session's request comes after it, with the narrower
    // token of the same principal.
    ASSERT_EQ(ping(harness.port, kept, tokens.broad, revision).status, 200);
    ASSERT_EQ(ping(harness.port, narrowed, tokens.narrow, revision).status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrowed_stream, 2));
    ASSERT_TRUE(told_of_open(*kept_stream, 2));
    EXPECT_EQ(updates_of(narrowed_stream->now().body, secret), 1)
        << "a session whose latest token cannot read the resource is not told of its updates";
    EXPECT_EQ(updates_of(kept_stream->now().body, secret), 2) << "a session that kept its scopes is told as before";

    // The broader token again does not bring the subscription back: it ended.
    ASSERT_EQ(ping(harness.port, narrowed, tokens.broad, revision).status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrowed_stream, 3));
    EXPECT_EQ(updates_of(narrowed_stream->now().body, secret), 1) << "the ended subscription stays ended";

    // Subscribing again, with a token that may read the resource, is told again.
    ASSERT_EQ(subscribe(harness.port, narrowed, tokens.broad, secret, revision).status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrowed_stream, 4));
    ASSERT_TRUE(told_of_open(*kept_stream, 4));
    EXPECT_EQ(updates_of(narrowed_stream->now().body, secret), 2);
    EXPECT_EQ(updates_of(kept_stream->now().body, secret), 4);
    narrowed_stream->disconnect();
    kept_stream->disconnect();
}

TEST_P(SubscriptionScopes, TheStreamOpenedWithANarrowerTokenEndsTheSubscriptionsItCannotRead) {
    const std::string revision = GetParam();
    Harness harness;
    const Tokens tokens = tokens_of(harness);
    const std::string session = open_session(harness.port, tokens.broad, revision);
    ASSERT_EQ(subscribe(harness.port, session, tokens.broad, secret, revision).status, 200);
    ASSERT_EQ(subscribe(harness.port, session, tokens.broad, open_resource, revision).status, 200);

    auto narrow_stream = open_stream(harness.port, session, tokens.narrow, revision);
    ASSERT_TRUE(narrow_stream->wait_for_headers());
    ASSERT_EQ(narrow_stream->now().status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrow_stream, 1));
    EXPECT_EQ(updates_of(narrow_stream->now().body, secret), 0);
    narrow_stream->disconnect();

    // The stream opened again with the broader token: the subscription the narrower stream ended stays ended.
    auto broad_stream = reopen_stream(harness.port, session, tokens.broad, revision);
    ASSERT_TRUE(broad_stream->wait_for_headers());
    ASSERT_EQ(broad_stream->now().status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*broad_stream, 1));
    EXPECT_EQ(updates_of(broad_stream->now().body, secret), 0) << "the GET's token ended the subscription";
    broad_stream->disconnect();
}

TEST_P(SubscriptionScopes, TheStreamOfASessionCarriesOnlyTheUpdatesItsOwnTokenMayRead) {
    const std::string revision = GetParam();
    Harness harness;
    const Tokens tokens = tokens_of(harness);
    const std::string session = open_session(harness.port, tokens.broad, revision);
    // The stream is opened with the narrower token before anything is watched; the subscriptions come after, with
    // the broader one, and may read what they name.
    auto narrow_stream = open_stream(harness.port, session, tokens.narrow, revision);
    ASSERT_TRUE(narrow_stream->wait_for_headers());
    ASSERT_EQ(narrow_stream->now().status, 200);
    ASSERT_EQ(subscribe(harness.port, session, tokens.broad, secret, revision).status, 200);
    ASSERT_EQ(subscribe(harness.port, session, tokens.broad, open_resource, revision).status, 200);

    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrow_stream, 1));
    EXPECT_EQ(updates_of(narrow_stream->now().body, secret), 0)
        << "the stream's token cannot read the resource, whatever the subscription's could";
    narrow_stream->disconnect();
}

// A tool that sends progress at once, so that its answer is an event stream from the start, then waits for the test.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;

    void enter() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait_for(lock, patience, [&] { return released; });
    }
    bool wait_entered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, patience, [&] { return entered; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

TEST_P(SubscriptionScopes, TheEventStreamOfARequestInProgressCarriesNoUpdateWhateverItsToken) {
    const std::string revision = GetParam();
    Gate gate;
    Harness harness({}, {}, [&gate](neomcp::Server& server) {
        server.tool("streaming", "Sends progress, then waits", [&gate](neomcp::Context& context) {
            context.progress(1, 2);
            gate.enter();
            return std::string(slow_result);
        });
    });
    struct Release {
        Gate& gate;
        ~Release() { gate.release(); }
    } const release{gate};
    const Tokens tokens = tokens_of(harness);
    const std::string session = open_session(harness.port, tokens.broad, revision);
    ASSERT_EQ(subscribe(harness.port, session, tokens.broad, secret, revision).status, 200);
    ASSERT_EQ(subscribe(harness.port, session, tokens.broad, open_resource, revision).status, 200);
    auto stream = open_stream(harness.port, session, tokens.broad, revision);
    ASSERT_TRUE(stream->wait_for_headers());
    ASSERT_EQ(stream->now().status, 200);

    json params{{"name", "streaming"}, {"arguments", json::object()}, {"_meta", {{"progressToken", "p"}}}};
    Call in_progress(harness.port, "POST", legacy_headers(session, tokens.broad, revision),
                     rpc(7, "tools/call", std::move(params)));
    ASSERT_TRUE(gate.wait_entered());
    ASSERT_TRUE(in_progress.wait_until([](const std::string& body) { return !messages_of(body).empty(); }));

    both_change(harness);
    ASSERT_TRUE(told_of_open(*stream, 1));
    ASSERT_EQ(ping(harness.port, session, tokens.narrow, revision).status, 200);
    both_change(harness);
    ASSERT_TRUE(told_of_open(*stream, 2));
    EXPECT_EQ(updates_of(stream->now().body, secret), 1);

    gate.release();
    const Reply answered = in_progress.finish();
    EXPECT_NE(answered.body.find(slow_result), std::string::npos) << answered.body;
    EXPECT_EQ(updates(answered.body), 0) << "the updates of the connection go to its stream only";
    stream->disconnect();
}

INSTANTIATE_TEST_SUITE_P(ResourceServerScenarioSubscription, SubscriptionScopes,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25"), [](const auto& info) {
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

// ---- 2026-07-28 ---------------------------------------------------------------------------------------------------

std::unique_ptr<Call> listen(std::uint16_t port, const std::string& token, std::vector<std::string> uris) {
    return std::make_unique<Call>(
        port, "POST", modern_headers("subscriptions/listen", token),
        rpc(5, "subscriptions/listen", modern_params({{"notifications", {{"resourceSubscriptions", std::move(uris)}}}})));
}

TEST(ResourceServerScenarioSubscription, ALaterSubscriptionWithANarrowerTokenIsToldOnlyOfWhatItMayRead) {
    Harness harness;
    const Tokens tokens = tokens_of(harness);
    const auto acknowledged = [](const std::string& body) {
        for (const json& message : messages_of(body)) {
            if (message.value("method", "") == "notifications/subscriptions/acknowledged") return true;
        }
        return false;
    };
    auto broad = listen(harness.port, tokens.broad, {secret, open_resource});
    ASSERT_TRUE(broad->wait_until(acknowledged));

    // The same principal's narrower token cannot subscribe to the secret resource, and is told only of the open one.
    const Reply refused =
        send(harness.port, "POST", modern_headers("subscriptions/listen", tokens.narrow),
             rpc(5, "subscriptions/listen",
                 modern_params({{"notifications", {{"resourceSubscriptions", {secret, open_resource}}}}})));
    EXPECT_EQ(refused.status, 403) << refused.body;
    auto narrow = listen(harness.port, tokens.narrow, {open_resource});
    ASSERT_TRUE(narrow->wait_until(acknowledged));

    both_change(harness);
    ASSERT_TRUE(told_of_open(*narrow, 1));
    ASSERT_TRUE(told_of_open(*broad, 1));
    EXPECT_EQ(updates_of(narrow->now().body, secret), 0);
    EXPECT_EQ(narrow->now().body.find(secret), std::string::npos);
    EXPECT_EQ(updates_of(broad->now().body, secret), 1) << "the earlier subscription keeps what its token may read";
    narrow->disconnect();
    broad->disconnect();
}

}  // namespace
