// The transfer thread: work handed over from other threads and the wakeup that brings it there, removal and stop
// while exchanges run, and many exchanges at once.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "client_http/transfers.h"
#include "client_http/url_guard.h"
#include "scripted_http_server.h"

namespace {

using neomcp::UnitId;
using neomcp::detail::http_client::TransferItem;
using neomcp::detail::http_client::TransferLimits;
using neomcp::detail::http_client::TransferLoop;
using neomcp::detail::http_client::TransferRequest;
using neomcp::detail::http_client::UrlGuard;

constexpr auto bound = std::chrono::seconds(20);

// What the loop posted, per unit, and whether anything came after the test said it must not.
class Sink {
public:
    void take(UnitId unit, TransferItem item) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) late_ = true;
            items_[unit].push_back(std::move(item));
        }
        changed_.notify_all();
    }

    bool ended(UnitId unit) const {
        const auto found = items_.find(unit);
        if (found == items_.end()) return false;
        for (const auto& item : found->second) {
            if (std::holds_alternative<neomcp::StreamEnded>(item) || std::holds_alternative<neomcp::StreamLost>(item) ||
                std::holds_alternative<neomcp::SendFailed>(item)) {
                return true;
            }
        }
        return false;
    }

    bool wait_ended(const std::vector<UnitId>& units) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] {
            for (const UnitId unit : units) {
                if (!ended(unit)) return false;
            }
            return true;
        });
    }

    bool succeeded(UnitId unit) const {
        std::lock_guard lock(mutex_);
        const auto found = items_.find(unit);
        if (found == items_.end()) return false;
        return !found->second.empty() && std::holds_alternative<neomcp::StreamEnded>(found->second.back());
    }

    std::size_t units() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    // Anything posted from now on is late.
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    bool late() const {
        std::lock_guard lock(mutex_);
        return late_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::map<UnitId, std::vector<TransferItem>> items_;
    bool closed_ = false;
    bool late_ = false;
};

UrlGuard guard_for(const std::string& url) { return std::get<UrlGuard>(UrlGuard::for_endpoint(url)); }

TransferLimits limits() {
    TransferLimits limits;
    limits.proxy.mode = neomcp::ProxyMode::none;
    // Long enough that a command which did not wake the thread is not taken up before the test gives up.
    limits.idle_wait = std::chrono::minutes(10);
    return limits;
}

TransferRequest request(UnitId unit, const std::string& url) {
    TransferRequest request;
    request.unit = unit;
    request.url = url;
    request.header_lines = {"Content-Type: application/json"};
    request.body = R"({"jsonrpc":"2.0","method":"ping","id":)" + std::to_string(unit) + "}";
    return request;
}

http_test::ScriptedHttpServer answering() {
    return http_test::ScriptedHttpServer([](const http_test::Request& request) {
        return http_test::json_reply(200, http_test::result_for(request, neomcp::json::object()));
    });
}

TEST(ClientHttp, TransferLoopTakesWorkFromAnotherThreadAndWakesForIt) {
    auto server = answering();
    neomcp::detail::http_client::CurlGlobal global;
    Sink sink;
    TransferLoop loop(limits(), guard_for(server.url()), [&](UnitId unit, TransferItem item) {
        sink.take(unit, std::move(item));
    });
    ASSERT_FALSE(loop.start());
    // The first exchange ends; the thread is then idle in its wait, with no timer of libcurl's to end it.
    loop.submit(request(1, server.url()));
    ASSERT_TRUE(sink.wait_ended({1}));
    // The next one comes from another thread while the loop waits: only the wakeup brings it there in time.
    std::thread other([&] { loop.submit(request(2, server.url())); });
    other.join();
    ASSERT_TRUE(sink.wait_ended({2}));
    EXPECT_TRUE(sink.succeeded(1));
    EXPECT_TRUE(sink.succeeded(2));
    loop.stop();
}

TEST(ClientHttp, TransferLoopRemovesARunningExchange) {
    auto gate = std::make_shared<http_test::Gate>();
    http_test::ScriptedHttpServer server([gate](const http_test::Request&) {
        http_test::Reply reply;
        reply.wait(gate);
        return reply;
    });
    neomcp::detail::http_client::CurlGlobal global;
    Sink sink;
    TransferLoop loop(limits(), guard_for(server.url()), [&](UnitId unit, TransferItem item) {
        sink.take(unit, std::move(item));
    });
    ASSERT_FALSE(loop.start());
    loop.submit(request(7, server.url()));
    ASSERT_TRUE(server.wait_for(1));
    EXPECT_EQ(loop.running(), 1u);
    loop.remove(7);
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (loop.running() != 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_EQ(loop.running(), 0u);
    gate->open();
    loop.stop();
    EXPECT_EQ(sink.units(), 0u);  // a removed exchange posts nothing
}

TEST(ClientHttp, TransferLoopStopsWhileExchangesRun) {
    auto gate = std::make_shared<http_test::Gate>();
    http_test::ScriptedHttpServer server([gate](const http_test::Request&) {
        http_test::Reply reply;
        reply.send(http_test::head(200, {{"Content-Type", "text/event-stream"}}, std::nullopt));
        reply.send("data: {}\n\n");
        reply.wait(gate);
        return reply;
    });
    neomcp::detail::http_client::CurlGlobal global;
    Sink sink;
    auto loop = std::make_unique<TransferLoop>(limits(), guard_for(server.url()), [&](UnitId unit, TransferItem item) {
        sink.take(unit, std::move(item));
    });
    ASSERT_FALSE(loop->start());
    constexpr UnitId exchanges = 8;
    for (UnitId unit = 1; unit <= exchanges; ++unit) loop->submit(request(unit, server.url()));
    ASSERT_TRUE(server.wait_for(exchanges));
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (loop->running() != exchanges && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_EQ(loop->running(), exchanges);
    // Stopped from another thread than the one that made it, while every exchange is open.
    std::thread stopping([&] { loop->stop(); });
    stopping.join();
    sink.close();
    loop->submit(request(99, server.url()));  // ignored after the stop
    loop.reset();
    gate->open();
    server.stop();
    EXPECT_FALSE(sink.late());
}

TEST(ClientHttp, TransferLoopRunsManyExchangesHandedOverFromSeveralThreads) {
    auto server = answering();
    neomcp::detail::http_client::CurlGlobal global;
    Sink sink;
    TransferLoop loop(limits(), guard_for(server.url()), [&](UnitId unit, TransferItem item) {
        sink.take(unit, std::move(item));
    });
    ASSERT_FALSE(loop.start());
    constexpr int threads = 4;
    constexpr int each = 10;
    std::vector<std::thread> senders;
    for (int t = 0; t < threads; ++t) {
        senders.emplace_back([&, t] {
            for (int i = 0; i < each; ++i) loop.submit(request(static_cast<UnitId>(t * each + i + 1), server.url()));
        });
    }
    for (auto& sender : senders) sender.join();
    std::vector<UnitId> units;
    for (UnitId unit = 1; unit <= threads * each; ++unit) units.push_back(unit);
    ASSERT_TRUE(sink.wait_ended(units));
    for (const UnitId unit : units) {
        EXPECT_TRUE(sink.succeeded(unit)) << unit;
    }
    loop.stop();
}

TEST(ClientHttp, TransferLoopStopIsIdempotentAndWorksWithoutStart) {
    neomcp::detail::http_client::CurlGlobal global;
    TransferLoop never(limits(), guard_for("http://127.0.0.1:1/"), [](UnitId, TransferItem) {});
    never.stop();
    never.stop();
    TransferLoop started(limits(), guard_for("http://127.0.0.1:1/"), [](UnitId, TransferItem) {});
    ASSERT_FALSE(started.start());
    started.stop();
    started.stop();
}

}  // namespace
