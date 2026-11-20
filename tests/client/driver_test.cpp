// The Client's driver against scripted peers: answers lost and asked for again, the backoff of a round that asks for
// its state alone, the threads (executors of awaiters, the client's own thread, the worker pool), and opening that is
// cancelled or never answered.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include "neomcp/client.h"
#include "neomcp/client/memory_transport.h"
#include "support.h"

namespace {

using namespace client_test;
using neomcp::ClientErrorKind;
using neomcp::profile::Transport;

constexpr std::string_view handshake_revision = "2025-11-25";
constexpr std::string_view per_request_revision = "2026-07-28";

// Answers what opening needs and pings; false for anything else.
bool answer_opening(ScriptedPeer& peer, const Seen& request, std::string_view revision) {
    if (request.method() == "initialize") {
        peer.answer(request, initialize_result(revision));
        return true;
    }
    if (request.method() == "server/discover") {
        peer.answer(request, discover_result(revision));
        return true;
    }
    if (request.method() == "ping") {
        peer.answer(request, json::object());
        return true;
    }
    return false;
}

neomcp::Client open_with(neomcp::ClientOptions options, const std::shared_ptr<ScriptedPeer>& peer,
                         Transport kind = Transport::stdio) {
    auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(kind, peer));
    if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
    return std::move(opened).value();
}

json empty_list() { return json{{"tools", json::array()}}; }

json tool_params() { return json{{"name", "t"}, {"arguments", json::object()}}; }

json state_only() { return json{{"resultType", "input_required"}, {"requestState", "s"}}; }

json completed() { return json{{"content", json::array()}}; }

json form_schema() {
    return json::parse(R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})");
}

// A result that asks for forms under these keys.
json asking_forms(std::initializer_list<std::string_view> keys) {
    json requests = json::object();
    for (const auto key : keys) {
        requests[std::string(key)] =
            json{{"method", "elicitation/create"},
                 {"params", json{{"mode", "form"}, {"message", "Sure?"}, {"requestedSchema", form_schema()}}}};
    }
    return json{{"resultType", "input_required"}, {"inputRequests", requests}, {"requestState", "st"}};
}

neomcp::ElicitationAnswer accepted() {
    return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
}

// Opens the latch when the test leaves, before the client is destroyed, so that no handler holds the destructor.
struct OpenOnExit {
    std::shared_ptr<Latch> latch;
    ~OpenOnExit() { latch->open(); }
};

// ---------------------------------------------------------------------------------------------------------------
// Lost answers

TEST(ClientDriver, ALostReadIsSentAgainOnce) {
    std::atomic<int> lists{0};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, handshake_revision)) return;
        if (request.method() == "tools/list") {
            if (++lists == 1) {
                side.event(neomcp::connection_unit, neomcp::ProcessExited{1});
            } else {
                side.answer(request, empty_list());
            }
        }
    });
    neomcp::Client client = open_with(only({handshake_revision}), peer);
    const auto listed = client.request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(peer->count("tools/list"), 2u);
    EXPECT_EQ(peer->restarts(), 1u);
    EXPECT_EQ(peer->count("initialize"), 2u);
}

TEST(ClientDriver, ALostReadOverHttpIsSentAgainOnce) {
    std::atomic<int> lists{0};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/list") {
            if (++lists == 1) {
                side.event(request.unit, neomcp::StreamLost{"the connection was reset"});
            } else {
                side.answer(request, empty_list());
            }
        }
    });
    peer->http = true;
    neomcp::Client client = open_with(only({per_request_revision}), peer, Transport::streamable_http);
    const auto listed = client.request("tools/list");
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(peer->count("tools/list"), 2u);
}

TEST(ClientDriver, ALostToolCallIsNeverSentAgain) {
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, handshake_revision)) return;
        if (request.method() == "tools/call") side.event(neomcp::connection_unit, neomcp::ProcessExited{1});
    });
    neomcp::Client client = open_with(only({handshake_revision}), peer);
    const auto called = client.call_tool("t", json::object());
    ASSERT_FALSE(called);
    EXPECT_EQ(called.error().kind, ClientErrorKind::server_exited);
    EXPECT_TRUE(called.error().may_have_run);
    EXPECT_EQ(peer->count("tools/call"), 1u);
    // The next request starts the server again.
    EXPECT_TRUE(client.ping());
    EXPECT_EQ(peer->count("tools/call"), 1u);
    EXPECT_EQ(peer->restarts(), 1u);
}

TEST(ClientDriver, TheResendCanBeSwitchedOff) {
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, handshake_revision)) return;
        if (request.method() == "tools/list") side.event(neomcp::connection_unit, neomcp::ProcessExited{1});
    });
    auto options = only({handshake_revision});
    options.resent_methods.clear();
    neomcp::Client client = open_with(std::move(options), peer);
    const auto listed = client.request("tools/list");
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::server_exited);
    EXPECT_TRUE(listed.error().may_have_run);
    EXPECT_EQ(peer->count("tools/list"), 1u);
}

// ---------------------------------------------------------------------------------------------------------------
// A round that asks for its state alone

TEST(ClientDriver, AStateOnlyRoundIsRetriedAtOnceThenWithABackoff) {
    std::atomic<int> calls{0};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/call") side.answer(request, ++calls <= 6 ? state_only() : completed());
    });
    auto timers = std::make_shared<ManualTimers>();
    auto options = only({per_request_revision});
    options.timers = timers;
    neomcp::Client client = open_with(std::move(options), peer);
    std::optional<neomcp::Result<json>> result;
    std::thread caller([&] { result.emplace(client.request("tools/call", tool_params())); });
    const OnExit join_caller([&] { join(caller); });
    const OnExit close_first([&] { static_cast<void>(client.close()); });
    std::vector<std::chrono::milliseconds> delays;
    std::vector<std::size_t> retries;
    for (int i = 0; i < 5; ++i) {
        const auto timer = timers->live([](const ManualTimers::Started& one) {
            return std::holds_alternative<neomcp::StateRetryDelay>(one.purpose);
        });
        ASSERT_TRUE(timer);
        // The first retry went out at once, before any timer.
        if (i == 0) {
            EXPECT_EQ(calls.load(), 2);
        }
        delays.push_back(timer->after);
        retries.push_back(std::get<neomcp::StateRetryDelay>(timer->purpose).retry);
        EXPECT_TRUE(timers->fire(timer->id));
    }
    caller.join();
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result) << result->error().message;
    EXPECT_EQ(calls.load(), 7);
    using ms = std::chrono::milliseconds;
    EXPECT_EQ(delays, (std::vector<ms>{ms{50}, ms{100}, ms{200}, ms{250}, ms{250}}));
    EXPECT_EQ(retries, (std::vector<std::size_t>{2, 3, 4, 5, 6}));
    EXPECT_EQ(timers->started_for<neomcp::StateRetryDelay>().size(), 5u);
}

TEST(ClientDriver, ARoundThatAsksStartsTheBackoffAgain) {
    std::atomic<int> calls{0};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() != "tools/call") return;
        const int call = ++calls;
        if (call == 3) {
            side.answer(request, asking_forms({"k"}));
        } else {
            side.answer(request, call < 6 ? state_only() : completed());
        }
    });
    auto timers = std::make_shared<ManualTimers>();
    auto options = only({per_request_revision});
    options.timers = timers;
    options.handlers.form([](const neomcp::ElicitationForm&) { return accepted(); });
    neomcp::Client client = open_with(std::move(options), peer);
    std::optional<neomcp::Result<json>> result;
    std::thread caller([&] { result.emplace(client.request("tools/call", tool_params())); });
    const OnExit join_caller([&] { join(caller); });
    const OnExit close_first([&] { static_cast<void>(client.close()); });
    for (int i = 0; i < 2; ++i) {
        const auto timer = timers->live([](const ManualTimers::Started& one) {
            return std::holds_alternative<neomcp::StateRetryDelay>(one.purpose);
        });
        ASSERT_TRUE(timer);
        EXPECT_EQ(timer->after, std::chrono::milliseconds(50));
        EXPECT_EQ(std::get<neomcp::StateRetryDelay>(timer->purpose).retry, 2u);
        EXPECT_TRUE(timers->fire(timer->id));
    }
    caller.join();
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result) << result->error().message;
    EXPECT_EQ(calls.load(), 6);
    EXPECT_EQ(timers->started_for<neomcp::StateRetryDelay>().size(), 2u);
}

// ---------------------------------------------------------------------------------------------------------------
// Threads

TEST(ClientDriver, AsyncCallsFromASecondContextAndAThreadPoolCompleteOnTheirOwnExecutors) {
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, handshake_revision)) return;
        if (request.method() == "tools/list") side.answer(request, empty_list());
    });
    neomcp::Client client = open_with(only({handshake_revision}), peer);
    asio::io_context other;
    auto keep = asio::make_work_guard(other);
    std::thread other_thread([&] { other.run(); });
    const OnExit stop_other([&] {
        other.stop();
        join(other_thread);
    });
    asio::thread_pool pool(3);
    constexpr int coroutines = 4;
    constexpr int calls_each = 5;
    std::atomic<int> done{0};
    std::atomic<int> answered{0};
    std::atomic<int> on_own{0};
    const std::function<asio::awaitable<void>(std::function<bool()>)> run =
        [&](std::function<bool()> mine) -> asio::awaitable<void> {
        for (int i = 0; i < calls_each; ++i) {
            auto listed = co_await client.async_request("tools/list");
            if (listed) ++answered;
            if (mine()) ++on_own;
        }
        ++done;
    };
    for (int i = 0; i < coroutines; ++i) {
        asio::co_spawn(other, run([&] { return other.get_executor().running_in_this_thread(); }), asio::detached);
        asio::co_spawn(pool, run([&] { return pool.get_executor().running_in_this_thread(); }), asio::detached);
    }
    EXPECT_TRUE(eventually([&] { return done.load() == 2 * coroutines; }));
    keep.reset();
    other_thread.join();
    pool.join();
    EXPECT_EQ(answered.load(), 2 * coroutines * calls_each);
    EXPECT_EQ(on_own.load(), 2 * coroutines * calls_each);
}

TEST(ClientDriver, TheTransportIsUsedOnTheClientsOwnThreadOnly) {
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, handshake_revision)) return;
        if (request.method() == "tools/list") side.answer(request, empty_list());
    });
    neomcp::Client client = open_with(only({handshake_revision}), peer);
    asio::io_context other;
    std::atomic<int> answered{0};
    asio::co_spawn(
        other,
        [&]() -> asio::awaitable<void> {
            for (int i = 0; i < 3; ++i) {
                auto listed = co_await client.async_request("tools/list");
                if (listed) ++answered;
            }
        },
        asio::detached);
    std::thread::id other_id;
    std::thread other_thread([&] {
        other_id = std::this_thread::get_id();
        other.run();
    });
    other_thread.join();
    EXPECT_EQ(answered.load(), 3);
    const auto threads = peer->threads();
    ASSERT_FALSE(threads.empty());
    for (const auto id : threads) {
        EXPECT_EQ(id, threads.front());
        EXPECT_NE(id, other_id);
        EXPECT_NE(id, std::this_thread::get_id());
    }
}

TEST(ClientDriver, ABlockingCallOnTheClientsThreadReturnsTheTypedError) {
    std::atomic<int> calls{0};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/call") side.answer(request, ++calls == 1 ? asking_forms({"k"}) : completed());
        if (request.method() == "tools/list") side.answer(request, empty_list());
    });
    std::atomic<neomcp::Client*> self{nullptr};
    std::mutex mutex;
    std::optional<ClientErrorKind> blocked_ping;
    std::optional<ClientErrorKind> blocked_close;
    std::optional<bool> awaited_list;
    auto options = only({per_request_revision});
    options.handlers.form([&](neomcp::ElicitationForm) -> asio::awaitable<neomcp::ElicitationAnswer> {
        neomcp::Client* client = self.load();
        const neomcp::Result<void> pinged = client->ping();
        const neomcp::Result<void> closing = client->close();
        const neomcp::Result<json> awaited = co_await client->async_request("tools/list");
        {
            std::lock_guard lock(mutex);
            if (!pinged) blocked_ping = pinged.error().kind;
            if (!closing) blocked_close = closing.error().kind;
            awaited_list = awaited.has_value();
        }
        co_return accepted();
    });
    neomcp::Client client = open_with(std::move(options), peer);
    self = &client;
    const auto result = client.request("tools/call", tool_params());
    ASSERT_TRUE(result) << result.error().message;
    std::lock_guard lock(mutex);
    EXPECT_EQ(blocked_ping, ClientErrorKind::on_client_thread);
    EXPECT_EQ(blocked_close, ClientErrorKind::on_client_thread);
    EXPECT_EQ(awaited_list, true);
}

TEST(ClientDriver, PlainHandlersAreBoundedByThePool) {
    std::atomic<int> calls{0};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/call") {
            side.answer(request, ++calls == 1 ? asking_forms({"k1", "k2", "k3"}) : completed());
        }
    });
    auto latch = std::make_shared<Latch>();
    auto started = std::make_shared<std::atomic<int>>(0);
    auto running = std::make_shared<std::atomic<int>>(0);
    auto most = std::make_shared<std::atomic<int>>(0);
    auto options = only({per_request_revision});
    options.max_concurrent_input_handlers = 2;
    options.handlers.form([latch, started, running, most](const neomcp::ElicitationForm&,
                                                          neomcp::InputContext& context) {
        ++*started;
        const int now = ++*running;
        int seen = most->load();
        while (now > seen && !most->compare_exchange_weak(seen, now)) {
        }
        latch->wait(context.stop_token());
        --*running;
        return accepted();
    });
    neomcp::Client client = open_with(std::move(options), peer);
    const OpenOnExit open_on_exit{latch};
    std::optional<neomcp::Result<json>> result;
    std::thread caller([&] { result.emplace(client.request("tools/call", tool_params())); });
    const OnExit join_caller([&] { join(caller); });
    const OnExit close_first([&] { static_cast<void>(client.close()); });
    EXPECT_TRUE(eventually([&] { return started->load() == 2; }));
    // A third handler does not start while two run.
    EXPECT_FALSE(eventually([&] { return started->load() == 3; }, std::chrono::milliseconds(300)));
    latch->open();
    caller.join();
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result) << result->error().message;
    EXPECT_EQ(started->load(), 3);
    EXPECT_EQ(most->load(), 2);
}

TEST(ClientDriver, CloseWhileHandlersRunStopsThemAndEndsTheCall) {
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/call") side.answer(request, asking_forms({"k"}));
    });
    auto latch = std::make_shared<Latch>();
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    auto options = only({per_request_revision});
    options.handlers.form([latch, entered, stopped](const neomcp::ElicitationForm&, neomcp::InputContext& context) {
        *entered = true;
        latch->wait(context.stop_token());
        *stopped = context.cancelled();
        return accepted();
    });
    neomcp::Client client = open_with(std::move(options), peer);
    const OpenOnExit open_on_exit{latch};
    std::optional<neomcp::Result<json>> result;
    std::thread caller([&] { result.emplace(client.request("tools/call", tool_params())); });
    const OnExit join_caller([&] { join(caller); });
    const OnExit close_first([&] { static_cast<void>(client.close()); });
    ASSERT_TRUE(eventually([&] { return entered->load(); }));
    EXPECT_TRUE(client.close());
    caller.join();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().kind, ClientErrorKind::closed);
    EXPECT_TRUE(eventually([&] { return stopped->load(); }));
    EXPECT_TRUE(peer->detached());
}

// ---------------------------------------------------------------------------------------------------------------
// Opening

// A Client destroyed on its own thread while a plain handler runs: the client's thread and its io_context go once the
// close is done, and the handler returns only afterwards, so its worker must not reach the gone context. A peer that
// speaks after the client is gone reaches nothing either.
TEST(ClientDriver, AClientDestroyedOnItsOwnThreadOutlivesNothingItsWorkersUse) {
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/call") side.answer(request, asking_forms({"k"}));
    });
    auto latch = std::make_shared<Latch>();
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto returned = std::make_shared<std::atomic<bool>>(false);
    std::optional<neomcp::Client> holder;
    std::atomic<bool> destroyed{false};
    auto options = only({per_request_revision});
    // The handler ignores its stop token, so it holds its worker until the test lets it go.
    options.handlers.form([latch, entered, returned](const neomcp::ElicitationForm&) {
        *entered = true;
        latch->wait(std::stop_token());
        *returned = true;
        return accepted();
    });
    // A log message of the server is the moment to let the client go, on the client's own thread.
    options.on_log = [&holder, &destroyed](const neomcp::LogEvent&) {
        if (!holder) return;
        holder.reset();
        destroyed = true;
    };
    holder.emplace(open_with(std::move(options), peer));
    const OpenOnExit open_on_exit{latch};
    // The call is awaited on another context, so that no thread is inside a method of the Client when it goes.
    asio::io_context other;
    std::optional<ClientErrorKind> ended;
    asio::co_spawn(
        other,
        [&]() -> asio::awaitable<void> {
            json params = tool_params();
            auto called = co_await holder->async_request("tools/call", std::move(params));
            if (!called) ended = called.error().kind;
        },
        asio::detached);
    std::thread runner([&] { other.run(); });
    const OnExit stop_runner([&] {
        other.stop();
        join(runner);
    });
    ASSERT_TRUE(eventually([&] { return entered->load(); }));
    const std::string log_line =
        R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"x"}})";
    peer->send(neomcp::connection_unit, log_line);
    ASSERT_TRUE(eventually([&] { return destroyed.load(); }));
    runner.join();
    EXPECT_EQ(ended, ClientErrorKind::closed);
    EXPECT_TRUE(eventually([&] { return peer->detached(); }));
    // Nothing tells when the client's thread has ended; it ends right after the close, well within this.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    latch->open();
    EXPECT_TRUE(eventually([&] { return returned->load(); }));
    // The worker hands its outcome on after the handler returned; and the peer speaks once more.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    peer->send(neomcp::connection_unit, log_line);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST(ClientDriver, APriorVerdictOpensWithoutAProbe) {
    const auto script = [](ScriptedPeer& side, const Seen& request) {
        if (request.method() == "server/discover") {
            side.refuse(request, -32601, "Method not found");
            return;
        }
        answer_opening(side, request, handshake_revision);
    };
    auto first = std::make_shared<ScriptedPeer>(script);
    std::optional<neomcp::ContactVerdict> verdict;
    {
        neomcp::Client client = open_with(only({}), first);
        verdict = client.verdict();
    }
    EXPECT_EQ(first->count("server/discover"), 1u);
    ASSERT_TRUE(verdict);
    EXPECT_EQ(verdict->kind, neomcp::ServerKind::handshake);
    auto second = std::make_shared<ScriptedPeer>(script);
    auto options = only({});
    options.prior = verdict;
    neomcp::Client client = open_with(std::move(options), second);
    EXPECT_EQ(second->count("server/discover"), 0u);
    EXPECT_EQ(second->count("initialize"), 1u);
    EXPECT_EQ(client.verdict(), verdict);
}

TEST(ClientDriver, CancellingAsyncOpenEndsItAndClosesTheTransport) {
    using namespace asio::experimental::awaitable_operators;
    std::atomic<bool> asked{false};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer&, const Seen& request) {
        if (request.method() == "initialize") asked = true;
    });
    asio::io_context other;
    asio::steady_timer trigger(other, asio::steady_timer::time_point::max());
    std::atomic<int> winner{-1};
    asio::co_spawn(
        other,
        [&]() -> asio::awaitable<void> {
            auto opening = neomcp::Client::async_open(only({handshake_revision}),
                                                      neomcp::make_memory_transport(Transport::stdio, peer));
            auto raced = co_await (std::move(opening) || trigger.async_wait(asio::as_tuple(asio::use_awaitable)));
            winner = static_cast<int>(raced.index());
        },
        asio::detached);
    std::thread runner([&] { other.run(); });
    const OnExit stop_runner([&] {
        other.stop();
        join(runner);
    });
    ASSERT_TRUE(eventually([&] { return asked.load(); }));
    asio::post(other, [&] { trigger.cancel(); });
    runner.join();
    EXPECT_EQ(winner.load(), 1);
    EXPECT_TRUE(peer->detached());
}

TEST(ClientDriver, AHandshakeNeverAnsweredFailsWhenItsClockLapses) {
    auto peer = std::make_shared<ScriptedPeer>([](ScriptedPeer&, const Seen&) {});
    auto timers = std::make_shared<ManualTimers>();
    auto options = only({handshake_revision});
    options.timers = timers;
    std::optional<neomcp::Result<neomcp::Client>> opened;
    std::thread opener([&] {
        opened.emplace(neomcp::Client::open(std::move(options), neomcp::make_memory_transport(Transport::stdio, peer)));
    });
    const OnExit join_opener([&] { join(opener); });
    const OnExit lapse_first([&] { timers->fire_all(); });
    const auto timer = timers->live([](const ManualTimers::Started& one) {
        return std::holds_alternative<neomcp::Deadline>(one.purpose);
    });
    ASSERT_TRUE(timer);
    EXPECT_TRUE(timers->fire(timer->id));
    opener.join();
    ASSERT_TRUE(opened);
    ASSERT_FALSE(*opened);
    EXPECT_EQ(opened->error().kind, ClientErrorKind::handshake_timed_out);
    EXPECT_TRUE(peer->detached());
}

}  // namespace
