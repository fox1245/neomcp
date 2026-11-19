// The deadline of a request body (ListenerOptions::body_timeout) at its edges: an honest client that sends slowly but
// in time is served; a body read in part when the time is up is refused, as is one whose last byte comes after it; a
// body without a declared length, and a GET that frames a body, are refused before anything of the body is read; and
// stop() does not wait for a body that is still arriving.
//
// The peers here write raw bytes, so that a test decides exactly when each byte of a body leaves; they use POSIX
// sockets and are skipped on Windows. The deadlines are short and the margins generous: the subject is a deadline.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/http/listener.h"
#include "neomcp/http/message.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using neomcp::http::Listener;
using neomcp::http::ListenerOptions;
using neomcp::http::Request;
using neomcp::http::Responder;

constexpr const char* path = "/mcp";
// Reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;

// A listener whose handler answers 200 with the body it got, and remembers every body it was called with.
class Echo {
public:
    explicit Echo(ListenerOptions options)
        : listener_(std::move(options), [this](Request request, Responder responder) {
              {
                  std::lock_guard lock(mutex_);
                  bodies_.insert(request.body);
              }
              responder.complete(neomcp::http::make_response(200, "text/plain", request.body));
          }) {
        const auto started = listener_.start();
        if (started) port_ = *started.value;
    }

    std::uint16_t port() const { return port_; }
    Listener& listener() { return listener_; }
    bool saw(const std::string& body) {
        std::lock_guard lock(mutex_);
        return bodies_.count(body) != 0;
    }
    std::size_t calls() {
        std::lock_guard lock(mutex_);
        return bodies_.size();
    }

private:
    std::mutex mutex_;
    std::set<std::string> bodies_;
    Listener listener_;
    std::uint16_t port_ = 0;
};

ListenerOptions options_with(std::chrono::milliseconds read_timeout, std::chrono::milliseconds body_timeout) {
    ListenerOptions options;
    options.path = path;
    options.read_timeout = read_timeout;
    options.body_timeout = body_timeout;
    options.disconnect_poll_interval = 20ms;
    return options;
}

#if !defined(_WIN32)

std::chrono::milliseconds since(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

// The request line and header fields; the connection closes after the answer, so that a peer reads to its end.
std::string head(std::string_view method, std::size_t length, bool chunked = false) {
    std::string text = std::string(method) + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\n" +
                       "Connection: close\r\n";
    if (chunked) {
        text += "Transfer-Encoding: chunked\r\n";
    } else {
        text += "Content-Length: " + std::to_string(length) + "\r\n";
    }
    return text + "\r\n";
}

// The status of the HTTP response at the start of `text`; 0 when there is none.
int status_of(const std::string& text) {
    constexpr std::string_view prefix = "HTTP/1.1 ";
    constexpr std::size_t digits = 3;
    if (text.size() < prefix.size() + digits || text.compare(0, prefix.size(), prefix) != 0) return 0;
    int status = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const char c = text[prefix.size() + i];
        if (c < '0' || c > '9') return 0;
        status = status * 10 + (c - '0');
    }
    return status;
}

// A peer on a plain socket.
class RawPeer {
public:
    explicit RawPeer(std::uint16_t port) : fd_(socket(AF_INET, SOCK_STREAM, 0)) {
        if (fd_ < 0) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connected_ = connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;
    ~RawPeer() {
        if (fd_ >= 0) close(fd_);
    }

    bool connected() const { return connected_; }

    bool write(std::string_view text) {
        while (!text.empty()) {
            const auto sent = send(fd_, text.data(), text.size(), 0);
            if (sent <= 0) return false;
            text.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    // Whether bytes (or the end) can be read within `timeout`.
    bool readable(std::chrono::milliseconds timeout) {
        pollfd watched{fd_, POLLIN, 0};
        return poll(&watched, 1, static_cast<int>(timeout.count())) > 0;
    }

    // Everything until the peer closes, or until `timeout` passes.
    std::string read_all(std::chrono::milliseconds timeout = patience) {
        std::string got;
        const auto until = Clock::now() + timeout;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
            if (left <= 0ms || !readable(left)) break;
            char buffer[4096];
            const auto n = recv(fd_, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                closed_ = true;
                break;
            }
            got.append(buffer, static_cast<std::size_t>(n));
        }
        return got;
    }

    // Whether the last read_all() ended because the peer closed the connection.
    bool closed() const { return closed_; }

private:
    int fd_ = -1;
    bool connected_ = false;
    bool closed_ = false;
};

// Sends `body` one byte every `pace` (after `fast` bytes sent at once), stopping early when an answer arrives; then
// reads the answer.
std::string trickle(RawPeer& peer, const std::string& body, std::size_t fast, std::chrono::milliseconds pace) {
    if (!peer.write(body.substr(0, fast))) return {};
    for (std::size_t i = fast; i < body.size(); ++i) {
        if (peer.readable(pace)) break;  // answered before the body was complete
        if (!peer.write(body.substr(i, 1))) break;
    }
    return peer.read_all();
}

#define NEOMCP_SKIP_WITHOUT_RAW_SOCKETS() (void)0
#else
#define NEOMCP_SKIP_WITHOUT_RAW_SOCKETS() GTEST_SKIP() << "the peers of these tests use POSIX sockets"
#endif

TEST(HttpBodyDeadline, AnHonestClientThatSendsSlowlyButInTimeIsServed) {
    Echo echo(options_with(2s, 5s));
    ASSERT_NE(echo.port(), 0);
    const std::string body(32, 'h');
    std::size_t sent = 0;
    httplib::Client client("127.0.0.1", echo.port());
    client.set_keep_alive(false);
    client.set_read_timeout(30, 0);
    const auto answered = client.Post(
        path, body.size(),
        [&](std::size_t, std::size_t, httplib::DataSink& sink) {
            std::this_thread::sleep_for(20ms);  // about 0.7 s in all, far inside the five seconds
            return sink.write(body.data() + sent++, 1);
        },
        "text/plain");
    ASSERT_TRUE(answered);
    EXPECT_EQ(answered->status, 200);
    EXPECT_EQ(answered->body, body);
    EXPECT_TRUE(echo.saw(body));
}

TEST(HttpBodyDeadline, ABodyReadInPartWhenTheTimeIsUpIsRefusedAndItsConnectionClosed) {
    NEOMCP_SKIP_WITHOUT_RAW_SOCKETS();
#if !defined(_WIN32)
    // Half the body at once, then a byte every 150 ms: each read is far inside the read timeout of two seconds, and
    // the whole body would take almost five seconds against a deadline of 600 ms.
    constexpr auto body_timeout = 600ms;
    constexpr auto read_timeout = 2s;
    Echo echo(options_with(read_timeout, body_timeout));
    ASSERT_NE(echo.port(), 0);
    const std::string body(64, 'p');
    RawPeer peer(echo.port());
    ASSERT_TRUE(peer.connected());
    const auto start = Clock::now();
    ASSERT_TRUE(peer.write(head("POST", body.size())));
    const std::string answer = trickle(peer, body, body.size() / 2, 150ms);
    const auto took = since(start);
    EXPECT_EQ(status_of(answer), 408) << answer.substr(0, 200);
    EXPECT_TRUE(peer.closed()) << "the connection stays open after the refusal";
    EXPECT_EQ(echo.calls(), 0u);
    EXPECT_GE(took, body_timeout);
    EXPECT_LT(took, body_timeout + read_timeout) << "refused after " << took.count() << " ms";
#endif
}

TEST(HttpBodyDeadline, ABodyWithoutADeclaredLengthIsRefusedBeforeItIsRead) {
    NEOMCP_SKIP_WITHOUT_RAW_SOCKETS();
#if !defined(_WIN32)
    constexpr auto read_timeout = 2s;
    Echo echo(options_with(read_timeout, 60s));
    ASSERT_NE(echo.port(), 0);
    RawPeer peer(echo.port());
    ASSERT_TRUE(peer.connected());
    const auto start = Clock::now();
    // The header fields only: an answer that waits for a chunk line would come after a read timeout.
    ASSERT_TRUE(peer.write(head("POST", 0, true)));
    const std::string answer = peer.read_all();
    EXPECT_EQ(status_of(answer), 411) << answer.substr(0, 200);
    EXPECT_TRUE(peer.closed());
    EXPECT_LT(since(start), read_timeout / 2);
    EXPECT_EQ(echo.calls(), 0u);
#endif
}

TEST(HttpBodyDeadline, AGetThatFramesABodyIsRefusedBeforeTheBodyIsRead) {
    NEOMCP_SKIP_WITHOUT_RAW_SOCKETS();
#if !defined(_WIN32)
    constexpr auto read_timeout = 2s;
    Echo echo(options_with(read_timeout, 60s));
    ASSERT_NE(echo.port(), 0);
    for (const std::string_view method : {"GET", "OPTIONS"}) {
        SCOPED_TRACE(std::string(method));
        RawPeer peer(echo.port());
        ASSERT_TRUE(peer.connected());
        const auto start = Clock::now();
        ASSERT_TRUE(peer.write(head(method, 1000)));
        const std::string answer = peer.read_all();
        EXPECT_EQ(status_of(answer), 400) << answer.substr(0, 200);
        EXPECT_TRUE(peer.closed());
        EXPECT_LT(since(start), read_timeout / 2);
    }
    EXPECT_EQ(echo.calls(), 0u);
#endif
}

TEST(HttpBodyDeadline, StopDoesNotWaitForABodyThatIsStillArriving) {
    NEOMCP_SKIP_WITHOUT_RAW_SOCKETS();
#if !defined(_WIN32)
    // A body that would take a minute to arrive at a byte every 100 ms, with a deadline of a minute.
    constexpr auto read_timeout = 1s;
    Echo echo(options_with(read_timeout, 60s));
    ASSERT_NE(echo.port(), 0);
    RawPeer peer(echo.port());
    ASSERT_TRUE(peer.connected());
    ASSERT_TRUE(peer.write(head("POST", 600)));
    std::atomic<bool> done{false};
    std::thread slow([&] {
        for (int i = 0; i < 600 && !done.load(); ++i) {
            if (peer.readable(100ms)) break;
            if (!peer.write("s")) break;
        }
    });
    std::this_thread::sleep_for(300ms);  // the body is being read
    const auto start = Clock::now();
    echo.listener().stop();
    const auto took = since(start);
    done = true;
    slow.join();
    EXPECT_LT(took, 3 * read_timeout) << "stop() took " << took.count() << " ms";
    EXPECT_EQ(echo.calls(), 0u);
#endif
}

// The race below: a short deadline, a read timeout far above it, and how far from the deadline the answer is known.
constexpr std::chrono::milliseconds race_body_timeout = 600ms;
constexpr std::chrono::milliseconds race_read_timeout = 2s;
constexpr std::chrono::milliseconds race_clearly = 300ms;

// The last byte of a body is sent just before or just after the deadline. Whichever comes first, the request is
// answered once: 200 with the whole body (the handler was called with it), or 408 (it was not). Far from the deadline
// the answer is known; near it, both are right.
TEST(HttpBodyDeadline, TheLastByteRacingTheDeadlineGetsOneConsistentAnswer) {
    NEOMCP_SKIP_WITHOUT_RAW_SOCKETS();
#if !defined(_WIN32)
    ListenerOptions options = options_with(race_read_timeout, race_body_timeout);
    options.worker_threads = 4;
    Echo echo(options);
    ASSERT_NE(echo.port(), 0);

    struct Trial {
        std::chrono::milliseconds offset;  // when the last byte leaves, against the deadline
        std::string body;
        int status = 0;
        bool closed = false;
        std::chrono::milliseconds took{0};
    };
    std::vector<Trial> trials;
    const std::vector<std::chrono::milliseconds> offsets = {-race_clearly, -10ms, -5ms, 0ms, 5ms, 10ms, race_clearly};
    for (int repeat = 0; repeat < 4; ++repeat) {
        for (const auto offset : offsets) {
            const int number = static_cast<int>(trials.size());
            trials.push_back(Trial{offset, "trial-" + std::to_string(number) + "-" + std::string(40, 'r')});
        }
    }
    // Four peers at a time, on the listener's four workers.
    constexpr std::size_t together = 4;
    for (std::size_t first = 0; first < trials.size(); first += together) {
        std::vector<std::thread> peers;
        for (std::size_t i = first; i < trials.size() && i < first + together; ++i) {
            peers.emplace_back([&echo, &trial = trials[i]] {
                RawPeer peer(echo.port());
                if (!peer.connected()) return;
                const auto start = Clock::now();
                if (!peer.write(head("POST", trial.body.size()))) return;
                if (!peer.write(std::string_view(trial.body).substr(0, trial.body.size() - 1))) return;
                std::this_thread::sleep_until(start + race_body_timeout + trial.offset);
                if (!peer.write(std::string_view(trial.body).substr(trial.body.size() - 1))) return;
                const std::string answer = peer.read_all();
                trial.status = status_of(answer);
                trial.closed = peer.closed();
                trial.took = since(start);
            });
        }
        for (auto& peer : peers) peer.join();
    }

    for (const Trial& trial : trials) {
        SCOPED_TRACE(trial.body.substr(0, 9) + " at " + std::to_string(trial.offset.count()) + " ms");
        ASSERT_TRUE(trial.status == 200 || trial.status == 408) << "answered " << trial.status;
        EXPECT_EQ(trial.status == 200, echo.saw(trial.body)) << "the answer and the handler's call disagree";
        if (trial.offset <= -race_clearly) {
            EXPECT_EQ(trial.status, 200);
        }
        if (trial.offset >= race_clearly) {
            EXPECT_EQ(trial.status, 408);
        }
        if (trial.status == 408) {
            EXPECT_TRUE(trial.closed);
        }
        EXPECT_LT(trial.took, race_body_timeout + race_read_timeout);
    }
#endif
}

}  // namespace
