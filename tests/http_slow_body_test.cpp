// Checks that a client sending a request body slowly holds a worker of the listener no longer than its body deadline
// allows. The request line and the headers are bounded as a whole by the read timeout; this checks the body. The
// slow client sends one byte of a 64-byte body every 300 ms, inside a read timeout of 500 ms, and gives up after four
// seconds; the body deadline is one read timeout here (its default, two minutes, is sized for large bodies on slow
// links). The listener has one worker, so a second client's request shows at once whether that worker is held.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "neomcp/http/listener.h"
#include "neomcp/http/message.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr const char* path = "/mcp";
constexpr std::chrono::milliseconds read_timeout = 500ms;

std::unique_ptr<httplib::Client> client_for(std::uint16_t port) {
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_keep_alive(false);
    client->set_read_timeout(60, 0);
    client->set_write_timeout(60, 0);
    return client;
}

TEST(HttpSlowBody, ABodySentSlowlyDoesNotHoldTheOnlyWorkerMuchLongerThanTheReadTimeout) {
    neomcp::http::ListenerOptions options;
    options.path = path;
    options.worker_threads = 1;
    options.max_worker_threads = 1;
    options.max_queued_connections = 4;
    options.read_timeout = read_timeout;
    options.body_timeout = read_timeout;
    options.disconnect_poll_interval = 20ms;
    neomcp::http::Listener listener(options, [](neomcp::http::Request, neomcp::http::Responder responder) {
        responder.complete(neomcp::http::make_response(200, "application/json", "{}"));
    });
    const auto started = listener.start();
    ASSERT_TRUE(started) << started.error;
    const std::uint16_t port = *started.value;

    std::atomic<bool> give_up{false};
    std::atomic<int> sent{0};
    auto slow = std::async(std::launch::async, [&] {
        constexpr std::size_t body_bytes = 64;
        return client_for(port)->Post(
            path, body_bytes,
            [&](std::size_t, std::size_t, httplib::DataSink& sink) {
                if (give_up.load()) return false;  // the client leaves: the connection is cut
                std::this_thread::sleep_for(300ms);
                ++sent;
                return sink.write("a", 1);
            },
            "application/json");
    });
    auto stopper = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(4s);
        give_up = true;
    });
    std::this_thread::sleep_for(700ms);  // the slow client's headers are read and its body is being read

    const auto start = Clock::now();
    const auto quick = client_for(port)->Post(path, "{}", "application/json");
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    ASSERT_TRUE(quick);
    EXPECT_EQ(quick->status, 200);
    EXPECT_LT(waited, 3 * read_timeout) << "the second request waited " << waited.count() << " ms while the slow "
                                        << "client had sent " << sent.load() << " bytes of its body";
    stopper.get();
    (void)slow.get();
    listener.stop();
}

}  // namespace
