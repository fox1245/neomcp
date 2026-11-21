// Shared by the tests of LoopbackRedirect: a prompt that runs on a thread of its own, a browser that is a plain HTTP
// request, and what a test needs to know of the port.
#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <gtest/gtest.h>

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "neomcp/client/oauth.h"
#include "neomcp/client/oauth_loopback.h"

namespace loopback_test {

// Long enough for a loaded machine; reached only when something is broken.
inline constexpr std::chrono::seconds patience{30};
// How long a thing that must not happen is watched for.
inline constexpr std::chrono::milliseconds window{300};

// The answer of an HTTP request the test made as the browser, or the reason there is none.
struct Reply {
    int status = 0;
    std::string body;
    std::string allow;
    std::string error;
};

inline Reply request(std::uint16_t port, const std::string& method, const std::string& target) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(20, 0);
    client.set_write_timeout(20, 0);
    // The target goes out as written: a browser does not re-encode what an authorization server put in the redirect.
    client.set_path_encode(false);
    httplib::Result result(nullptr, httplib::Error::Unknown);
    if (method == "GET") {
        result = client.Get(target);
    } else if (method == "POST") {
        result = client.Post(target, "x", "text/plain");
    } else if (method == "PUT") {
        result = client.Put(target, "x", "text/plain");
    } else if (method == "DELETE") {
        result = client.Delete(target);
    } else {
        result = client.Head(target);
    }
    Reply reply;
    if (!result) {
        reply.error = httplib::to_string(result.error());
        return reply;
    }
    reply.status = result->status;
    reply.body = result->body;
    reply.allow = result->get_header_value("Allow");
    return reply;
}

inline Reply get(std::uint16_t port, const std::string& target) { return request(port, "GET", target); }

// A TCP connection to `address`:`port` is made: something listens there.
inline bool connects(const asio::ip::address& address, std::uint16_t port) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    std::error_code error;
    socket.connect(asio::ip::tcp::endpoint(address, port), error);
    return !error;
}
inline bool connects(std::uint16_t port) { return connects(asio::ip::make_address("127.0.0.1"), port); }

// The address of this machine that traffic to another network leaves from, when it has one that is not loopback. The
// route is looked up, nothing is sent.
inline std::optional<asio::ip::address> outbound_address() {
    asio::io_context io;
    asio::ip::udp::socket socket(io);
    std::error_code error;
    socket.connect(asio::ip::udp::endpoint(asio::ip::make_address("192.0.2.1"), 9), error);
    if (error) return std::nullopt;
    const auto local = socket.local_endpoint(error);
    if (error) return std::nullopt;
    const asio::ip::address address = local.address();
    if (address.is_loopback() || address.is_unspecified()) return std::nullopt;
    return address;
}

inline neomcp::AuthorizationPromptInfo prompt_info(const neomcp::LoopbackRedirect& redirect,
                                                   std::string authorization_url = "https://as.example/authorize?s=1") {
    neomcp::AuthorizationPromptInfo info;
    info.authorization_url = std::move(authorization_url);
    info.redirect_uri = redirect.redirect_uri();
    info.issuer = "https://as.example";
    return info;
}

// What the application's function that opens the browser saw.
class Opened {
public:
    std::function<void(const std::string&)> function() {
        return [this](const std::string& url) {
            std::function<void()> action;
            {
                std::lock_guard lock(mutex_);
                urls_.push_back(url);
                threads_.push_back(std::this_thread::get_id());
                action = then_;
            }
            changed_.notify_all();
            if (action) action();
        };
    }
    // Run on the prompt's thread after the URL was noted.
    void then(std::function<void()> action) {
        std::lock_guard lock(mutex_);
        then_ = std::move(action);
    }
    std::vector<std::string> urls() const {
        std::lock_guard lock(mutex_);
        return urls_;
    }
    // The threads the function was called on.
    std::vector<std::thread::id> threads() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }
    bool wait(std::size_t count) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, patience, [&] { return urls_.size() >= count; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<std::string> urls_;
    std::vector<std::thread::id> threads_;
    std::function<void()> then_;
};

// A plain prompt running on a thread of its own, with a stop of its own.
class RunningPrompt {
public:
    RunningPrompt(neomcp::AuthorizationPrompt prompt, neomcp::AuthorizationPromptInfo info)
        : context_(stop_.get_token()) {
        body_ = prompt.plain();
        result_ = std::async(std::launch::async, [this, info = std::move(info)] { return body_->run(info, context_); });
    }
    ~RunningPrompt() {
        stop_.request_stop();
        if (result_.valid()) result_.wait();
    }
    RunningPrompt(const RunningPrompt&) = delete;
    RunningPrompt& operator=(const RunningPrompt&) = delete;

    void stop() { stop_.request_stop(); }
    // The answer, or nothing when the prompt did not end within the bound.
    std::optional<neomcp::AuthorizationPromptAnswer> answer() {
        if (result_.wait_for(patience) != std::future_status::ready) return std::nullopt;
        return result_.get();
    }
    // Whether the prompt has not ended after the window.
    bool still_waiting() const { return result_.wait_for(window) != std::future_status::ready; }

private:
    std::stop_source stop_;
    neomcp::AuthorizationPromptContext context_;
    std::shared_ptr<neomcp::detail::PlainPromptBody> body_;
    std::future<neomcp::AuthorizationPromptAnswer> result_;
};

// A redirect whose browser is opened by `opened`.
inline std::shared_ptr<neomcp::LoopbackRedirect> make_redirect(Opened& opened, neomcp::LoopbackRedirectOptions options = {}) {
    options.open_browser = opened.function();
    auto made = neomcp::LoopbackRedirect::create(std::move(options));
    EXPECT_TRUE(made) << (made ? "" : made.error().message);
    return made ? *made : nullptr;
}

}  // namespace loopback_test
