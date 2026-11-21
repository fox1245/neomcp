// One thread that runs the introspection requests of a validator on a libcurl multi handle. Internal to
// neomcp::server_auth_introspection.
//
// Model: the thread waits in curl_multi_poll; another thread hands it a request through a queue and wakes it with
// curl_multi_wakeup. Every easy handle is made, driven, removed and freed on that thread alone. The callback that tells
// of an end runs on that thread, after the handle left the multi handle and outside every lock; it must return
// promptly and must not call into the loop.
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace neomcp::http::detail {

// One POST to make.
struct ExchangeRequest {
    std::string url;
    std::vector<std::string> header_lines;  // "Name: value"
    std::string body;
    std::chrono::milliseconds total_timeout{0};
    std::chrono::milliseconds connect_timeout{0};
    std::size_t max_body_bytes = 0;
    std::size_t max_header_bytes = 0;
    bool use_proxy_from_environment = false;
    std::optional<std::string> ca_file;
};

enum class ExchangeEnd {
    response,        // a complete response; status and body are set
    body_too_large,  // the body, or the header fields, exceeded their bound (also when announced in advance)
    failed           // anything else: not reachable, timed out, a transfer error
};

struct ExchangeResult {
    ExchangeEnd end = ExchangeEnd::failed;
    int status = 0;
    std::string body;
};

using ExchangeDone = std::function<void(ExchangeResult)>;

// Why the HTTP library cannot serve (its resolver blocks and the application did not accept that); empty when it can.
std::optional<std::string> exchange_resolver_problem(bool allow_blocking);

class ExchangeLoop {
public:
    ExchangeLoop();
    // Stops the loop; the requests still running end without a callback.
    ~ExchangeLoop();
    ExchangeLoop(const ExchangeLoop&) = delete;
    ExchangeLoop& operator=(const ExchangeLoop&) = delete;

    // Starts the thread; why it could not, otherwise. Called once.
    std::optional<std::string> start();
    // Any thread. Hands a request to the thread.
    void submit(ExchangeRequest request, ExchangeDone done);
    // Any thread but the loop's own. Stops the thread and waits for it. Idempotent.
    void stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::thread thread_;
    std::mutex stop_mutex_;
    bool stopped_ = false;
};

}  // namespace neomcp::http::detail
