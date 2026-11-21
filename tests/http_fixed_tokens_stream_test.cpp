// The open streams of fixed tokens when the table is replaced, over loopback sockets and on every revision the
// endpoint serves: the stream of a session (GET) on the revisions that keep one, a subscription on 2026-07-28, and the
// event stream of a request in progress on all of them. A stream of a token that is gone, or whose grant changed, ends;
// its reopening is answered with what the new table says; a stream of a token whose entry stays the same stays open
// and still carries what the server sends.
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/fixed_tokens_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr const char* list_changed = "notifications/tools/list_changed";
constexpr const char* invalid_token_challenge = R"(Bearer error="invalid_token", scope="mcp:use")";
constexpr const char* slow_result = "slow-done";
// Long enough for a stream that is to end to have ended; a stream that is to stay open is shown open by what it
// carries, never by this time passing.
constexpr std::chrono::milliseconds ends_within = 10s;

bool is_modern(const std::string& revision) { return revision == modern_revision; }

httplib::Headers stream_headers(const std::string& session, const std::string& token, const std::string& revision) {
    httplib::Headers headers = legacy_headers(session, token, revision);
    headers.erase("Accept");
    headers.emplace("Accept", "text/event-stream");
    return headers;
}

std::string listen_body() {
    return rpc(5, "subscriptions/listen", modern_params({{"notifications", {{"toolsListChanged", true}}}}));
}

// A stream that stays open until the server ends it: the stream of a session opened with `token`, or a subscription
// on 2026-07-28.
struct OpenStream {
    std::string session;  // the revisions that keep one
    std::unique_ptr<Call> call;
};

OpenStream open_stream(std::uint16_t port, const std::string& token, const std::string& revision) {
    OpenStream opened;
    if (is_modern(revision)) {
        opened.call = std::make_unique<Call>(port, "POST", modern_headers("subscriptions/listen", token), listen_body());
    } else {
        opened.session = open_session(port, token, revision);
        opened.call = std::make_unique<Call>(port, "GET", stream_headers(opened.session, token, revision));
    }
    return opened;
}

// The same request again with `token`, answered at once when the gate refuses it.
Reply reopen(std::uint16_t port, const OpenStream& stream, const std::string& token, const std::string& revision) {
    if (is_modern(revision)) return send(port, "POST", modern_headers("subscriptions/listen", token), listen_body());
    return send(port, "GET", stream_headers(stream.session, token, revision));
}

int occurrences(const std::string& text, const std::string& what) {
    int count = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++count;
    return count;
}

bool ended(Call& call) {
    call.finish(ends_within);
    return call.finished();
}

void expect_open(OpenStream& stream) {
    ASSERT_TRUE(stream.call->wait_for_headers());
    ASSERT_EQ(stream.call->now().status, 200);
}

class FixedTokensStreams : public ::testing::TestWithParam<std::string> {};

TEST_P(FixedTokensStreams, AStreamOfARemovedTokenEndsAndItsReopeningGets401WhileAKeptTokensStreamCarriesOn) {
    const std::string revision = GetParam();
    FixedHarness harness({fixed("keep", "alice", {"mcp:use"}), fixed("gone", "bob", {"mcp:use"})});
    OpenStream kept = open_stream(harness.port, "keep", revision);
    OpenStream removed = open_stream(harness.port, "gone", revision);
    expect_open(kept);
    expect_open(removed);

    // "keep" stays with the same grant; "gone" is removed.
    harness.replace({fixed("keep", "alice", {"mcp:use"}), fixed("new", "carol", {"mcp:use"})});
    EXPECT_TRUE(ended(*removed.call)) << "the stream of a removed token is still open";
    EXPECT_FALSE(kept.call->finished());
    const Reply again = reopen(harness.port, removed, "gone", revision);
    EXPECT_EQ(again.status, 401);
    EXPECT_EQ(again.header("WWW-Authenticate"), invalid_token_challenge);

    // The kept stream is open, as what the server sends still reaches it; and so after a second replacement that keeps
    // the entry again.
    harness.items.hidden->enable(true);
    EXPECT_TRUE(kept.call->wait_until([](const std::string& body) { return occurrences(body, list_changed) >= 1; }));
    harness.replace({fixed("keep", "alice", {"mcp:use"}), fixed("other", "dave", {"mcp:use"})});
    harness.items.hidden->enable(false);
    EXPECT_TRUE(kept.call->wait_until([](const std::string& body) { return occurrences(body, list_changed) >= 2; }));
    EXPECT_FALSE(kept.call->finished());
}

TEST_P(FixedTokensStreams, AStreamOfATokenWhoseGrantChangedEndsAndItsReopeningGetsWhatTheNewGrantGives) {
    const std::string revision = GetParam();
    const auto expiry = SystemClock::now() + 3600s;
    FixedHarness harness({fixed("narrowed", "alice", {"mcp:use", "files:read"}),
                          fixed("below", "bob", {"mcp:use", "files:read"}),
                          fixed("later", "carol", {"mcp:use"}, expiry)});
    OpenStream narrowed = open_stream(harness.port, "narrowed", revision);
    OpenStream below = open_stream(harness.port, "below", revision);
    OpenStream later = open_stream(harness.port, "later", revision);
    expect_open(narrowed);
    expect_open(below);
    expect_open(later);

    // Fewer scopes, still with mcp:use; scopes without mcp:use; the same scopes with another expiry.
    harness.replace({fixed("narrowed", "alice", {"mcp:use"}), fixed("below", "bob", {"files:read"}),
                     fixed("later", "carol", {"mcp:use"}, expiry + 3600s)});
    EXPECT_TRUE(ended(*narrowed.call)) << "narrowed scopes";
    EXPECT_TRUE(ended(*below.call)) << "scopes without the required one";
    EXPECT_TRUE(ended(*later.call)) << "another expiry";

    // Reopened: the narrowed grant still serves, the one below required_scopes is asked for the scope it lacks.
    OpenStream renewed = open_stream(harness.port, "narrowed", revision);
    expect_open(renewed);
    const Reply refused = reopen(harness.port, below, "below", revision);
    EXPECT_EQ(refused.status, 403);
    EXPECT_EQ(refused.header("WWW-Authenticate").find("insufficient_scope") != std::string::npos, true);
    OpenStream relived = open_stream(harness.port, "later", revision);
    expect_open(relived);
    // The reopened streams carry on.
    harness.items.hidden->enable(true);
    EXPECT_TRUE(renewed.call->wait_until([](const std::string& body) { return occurrences(body, list_changed) >= 1; }));
    EXPECT_TRUE(relived.call->wait_until([](const std::string& body) { return occurrences(body, list_changed) >= 1; }));
}

// A gate a tool waits at until the test opens it.
class Latch {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    bool wait() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

// The tools of the next test. "streaming" sends progress at once, so its answer is an event stream from the start, and
// waits; "late" waits first, so its request has no stream yet, then sends progress, which opens one, and waits again.
struct Gates {
    Latch streaming_entered;
    Latch late_entered;
    Latch late_may_progress;
    Latch finish;

    void open_all() {
        late_may_progress.open();
        finish.open();
    }
};

void register_slow_tools(neomcp::Server& server, Gates& gates) {
    server.tool("streaming", "Sends progress, then waits", [&gates](neomcp::Context& context) {
        context.progress(1, 2);
        gates.streaming_entered.open();
        gates.finish.wait();
        return std::string(slow_result);
    });
    server.tool("late", "Waits, sends progress, waits", [&gates](neomcp::Context& context) {
        gates.late_entered.open();
        gates.late_may_progress.wait();
        context.progress(1, 2);
        gates.finish.wait();
        return std::string(slow_result);
    });
}

// A tools/call that asks for progress, as a request of `revision` (inside `session` on the revisions that keep one).
std::unique_ptr<Call> slow_call(std::uint16_t port, const std::string& token, const std::string& tool,
                                const std::string& revision, const std::string& session) {
    json params{{"name", tool}, {"arguments", json::object()}};
    if (is_modern(revision)) {
        json modern = modern_params(std::move(params));
        modern["_meta"]["progressToken"] = "p-" + tool;
        return std::make_unique<Call>(port, "POST", modern_headers("tools/call", token, tool),
                                      rpc(7, "tools/call", std::move(modern)));
    }
    params["_meta"] = json{{"progressToken", "p-" + tool}};
    return std::make_unique<Call>(port, "POST", legacy_headers(session, token, revision),
                                  rpc(7, "tools/call", std::move(params)));
}

struct Finally {
    std::function<void()> run;
    ~Finally() { run(); }
};

TEST_P(FixedTokensStreams, TheEventStreamOfARequestInProgressEndsWhetherItOpenedBeforeTheReplacementOrAfter) {
    const std::string revision = GetParam();
    Gates gates;
    FixedHarness harness({fixed("gone", "bob", {"mcp:use"}), fixed("keep", "alice", {"mcp:use"})}, {},
                         [&gates](neomcp::Server& server) { register_slow_tools(server, gates); });
    const Finally release{[&gates] { gates.open_all(); }};
    // One session for each request (a session runs one call at a time), made in this order.
    const auto session_of = [&] {
        return is_modern(revision) ? std::string() : open_session(harness.port, "gone", revision);
    };
    const std::string first_session = session_of();
    const std::string second_session = session_of();

    // A request already streaming, and one without a stream yet; then a stream of the same token, opened after both,
    // so that once it has ended the replacement's sweep has gone past both requests (it visits sessions in the order
    // they were made, and a request of 2026-07-28 has a session of its own).
    const auto streaming = slow_call(harness.port, "gone", "streaming", revision, first_session);
    ASSERT_TRUE(gates.streaming_entered.wait());
    ASSERT_TRUE(streaming->wait_until([](const std::string& body) { return body.find("progress") != std::string::npos; }));
    const auto late = slow_call(harness.port, "gone", "late", revision, second_session);
    ASSERT_TRUE(gates.late_entered.wait());
    OpenStream witness = open_stream(harness.port, "gone", revision);
    expect_open(witness);

    harness.replace({fixed("keep", "alice", {"mcp:use"})});
    EXPECT_TRUE(ended(*witness.call));
    EXPECT_TRUE(ended(*streaming)) << "a request's stream open at the replacement";
    EXPECT_EQ(streaming->now().body.find(slow_result), std::string::npos);

    // The late request opens its stream now, after the sweep: the check made when a stream opens ends it.
    gates.late_may_progress.open();
    EXPECT_TRUE(ended(*late)) << "a request's stream opened after the replacement";
    EXPECT_EQ(late->now().body.find(slow_result), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(FixedTokens, FixedTokensStreams,
                         ::testing::Values("2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"),
                         [](const auto& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return name;
                         });

}  // namespace
