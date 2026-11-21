#include "neomcp/client/oauth_loopback.h"

#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "neomcp/http/listener.h"
#include "neomcp/http/message.h"

namespace neomcp {

namespace {

constexpr int ok_status = 200;
constexpr int not_found_status = 404;
constexpr int method_not_allowed_status = 405;
constexpr int uri_too_long_status = 414;
constexpr int unavailable_status = 503;
constexpr std::string_view get_method = "GET";
constexpr std::string_view http_scheme = "http";

// What the browser is shown, whatever the redirect carried: the listener does not read the query, so it cannot tell a
// sign-in that worked from one that did not. Nothing in the page is fetched, so nothing leaves the browser.
constexpr std::string_view page_type = "text/html; charset=utf-8";
constexpr std::string_view page_body =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>Return to the application</title></head>"
    "<body><p>You can close this window and return to the application.</p></body></html>";
constexpr std::string_view cache_field = "Cache-Control";
constexpr std::string_view cache_value = "no-store";
constexpr std::string_view referrer_field = "Referrer-Policy";
constexpr std::string_view referrer_value = "no-referrer";
constexpr std::string_view policy_field = "Content-Security-Policy";
constexpr std::string_view policy_value = "default-src 'none'";
constexpr std::string_view sniff_field = "X-Content-Type-Options";
constexpr std::string_view sniff_value = "nosniff";
constexpr std::string_view connection_field = "Connection";
constexpr std::string_view connection_value = "close";
constexpr std::string_view allow_field = "Allow";

using Clock = std::chrono::steady_clock;

ClientError invalid(std::string message) {
    ClientError error;
    error.kind = ClientErrorKind::invalid_options;
    error.message = std::move(message);
    return error;
}

// What one prompt waits for, shared by the listener's worker threads, the prompt's own thread and the cancellation of
// the prompt.
class Wait {
public:
    // The first call takes the callback and returns true; a later one, and one after the wait was cancelled, returns
    // false. The prompt does not end because of it until announce(): the browser is answered first, so that closing
    // the listener cannot cut that answer.
    bool claim(std::string url) {
        std::lock_guard lock(mutex_);
        if (callback_ || cancelled_) return false;
        callback_ = std::move(url);
        return true;
    }

    // The callback taken by claim() is the prompt's answer.
    void announce() {
        std::function<void()> notify;
        {
            std::lock_guard lock(mutex_);
            announced_ = true;
            notify = wake_;
        }
        changed_.notify_all();
        if (notify) notify();
    }

    void cancel() {
        std::function<void()> notify;
        {
            std::lock_guard lock(mutex_);
            if (cancelled_) return;
            cancelled_ = true;
            notify = wake_;
        }
        changed_.notify_all();
        if (notify) notify();
    }

    // The listener is closed: the end of a coroutine prompt, which does not wait on a condition variable.
    void mark_closed() {
        std::function<void()> notify;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            notify = wake_;
        }
        changed_.notify_all();
        if (notify) notify();
    }

    bool done() const {
        std::lock_guard lock(mutex_);
        return announced_ || cancelled_;
    }
    bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }
    // The answer: the callback, unless the wait was cancelled before it was announced.
    std::optional<std::string> callback() const {
        std::lock_guard lock(mutex_);
        if (!announced_) return std::nullopt;
        return callback_;
    }

    void block() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return announced_ || cancelled_; });
    }

    // Called (from any thread, with no lock held) whenever the state changes; the coroutine prompt's way to be woken.
    void set_wake(std::function<void()> wake) {
        std::lock_guard lock(mutex_);
        wake_ = std::move(wake);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::string> callback_;
    bool announced_ = false;
    bool cancelled_ = false;
    bool closed_ = false;
    std::function<void()> wake_;
};

http::Response page() {
    http::Response response = http::make_response(ok_status, page_type, std::string(page_body));
    response.headers.set(std::string(cache_field), std::string(cache_value));
    response.headers.set(std::string(referrer_field), std::string(referrer_value));
    response.headers.set(std::string(policy_field), std::string(policy_value));
    response.headers.set(std::string(sniff_field), std::string(sniff_value));
    // One request is all this listener serves: the worker is free as soon as the page is written.
    response.headers.set(std::string(connection_field), std::string(connection_value));
    return response;
}

}  // namespace

struct LoopbackRedirect::Impl {
    LoopbackRedirectOptions options;
    std::uint16_t port = 0;
    std::string redirect_uri;

    mutable std::mutex mutex;
    bool active = false;  // a prompt waits
    std::string problem;

    http::ListenerOptions listener_options() const {
        http::ListenerOptions chosen;
        chosen.bind_address = LoopbackRedirectDefaults::host;
        chosen.port = port;
        chosen.path = options.path;
        // A callback is a request line; nothing it may carry is a body.
        chosen.max_body_bytes = 1;
        chosen.worker_threads = LoopbackRedirectDefaults::worker_threads;
        chosen.max_worker_threads = LoopbackRedirectDefaults::max_worker_threads;
        chosen.max_queued_connections = LoopbackRedirectDefaults::max_queued_connections;
        chosen.read_timeout = options.read_timeout;
        return chosen;
    }

    void note_problem(std::string text) {
        std::lock_guard lock(mutex);
        problem = std::move(text);
    }
    void release() {
        std::lock_guard lock(mutex);
        active = false;
    }

    // The handler of one prompt's listener.
    http::Handler handler_for(std::shared_ptr<Wait> wait) const {
        const std::string base = std::string(http_scheme) + "://" + LoopbackRedirectDefaults::host + ":" +
                                 std::to_string(port) + options.path;
        const std::size_t bound = options.max_callback_bytes;
        return [wait = std::move(wait), base, bound](http::Request request, http::Responder responder) {
            // Only a GET is a browser coming back; anything else is refused and the wait goes on.
            if (request.method != get_method) {
                http::Response refused = http::make_response(method_not_allowed_status);
                refused.headers.set(std::string(allow_field), std::string(get_method));
                responder.complete(std::move(refused));
                return;
            }
            std::string callback = base;
            if (!request.query.empty()) {
                callback.push_back('?');
                callback += request.query;
            }
            if (callback.size() > bound) {
                responder.complete(http::make_response(uri_too_long_status));
                return;
            }
            if (!wait->claim(std::move(callback))) {
                // A second request, or one that arrived as the prompt ended.
                responder.complete(http::make_response(not_found_status));
                return;
            }
            responder.complete(page());
            wait->announce();
        };
    }
};

namespace {

// Stops the listener on a thread of its own, then runs `then`. The listener is kept alive by the thread; a thread that
// cannot be started stops it here.
void close_in_background(std::shared_ptr<http::Listener> listener, std::function<void()> then) {
    try {
        std::thread([listener, then]() mutable {
            listener->stop();
            listener.reset();
            if (then) then();
        }).detach();
    } catch (const std::system_error&) {
        listener->stop();
        listener.reset();
        if (then) then();
    }
}

// One prompt's listening: the listener, and what it waits for. Closing it is part of the prompt; one that is dropped
// without having been closed (a coroutine abandoned as the client shuts down) is closed on a thread of its own.
class Sitting {
public:
    Sitting(std::shared_ptr<LoopbackRedirect::Impl> impl, std::shared_ptr<Wait> wait,
            std::shared_ptr<http::Listener> listener)
        : impl_(std::move(impl)), wait_(std::move(wait)), listener_(std::move(listener)) {}
    Sitting(const Sitting&) = delete;
    Sitting& operator=(const Sitting&) = delete;
    ~Sitting() {
        if (listener_) {
            close_in_background(std::move(listener_), [impl = impl_] { impl->release(); });
        }
    }

    const std::shared_ptr<Wait>& wait() const { return wait_; }

    // Returns when the port is closed.
    void close() {
        if (!listener_) return;
        listener_->stop();
        listener_.reset();
        impl_->release();
    }

    // Returns at once; the port is closed when the wait is marked closed.
    void close_later() {
        if (!listener_) return;
        close_in_background(std::move(listener_), [impl = impl_, wait = wait_] {
            impl->release();
            wait->mark_closed();
        });
    }

private:
    std::shared_ptr<LoopbackRedirect::Impl> impl_;
    std::shared_ptr<Wait> wait_;
    std::shared_ptr<http::Listener> listener_;
};

// Opens the listener for a sign-in whose redirect URI is `info`'s; nothing when it cannot (and the reason is noted).
std::unique_ptr<Sitting> open_sitting(const std::shared_ptr<LoopbackRedirect::Impl>& impl, const AuthorizationPromptInfo& info) {
    {
        std::lock_guard lock(impl->mutex);
        impl->problem.clear();
        if (impl->active) {
            impl->problem = "another sign-in is waiting on this redirect";
            return nullptr;
        }
        if (info.redirect_uri != impl->redirect_uri) {
            impl->problem = "the sign-in's redirect URI is not the one of this LoopbackRedirect";
            return nullptr;
        }
        impl->active = true;
    }
    auto wait = std::make_shared<Wait>();
    auto listener = std::make_shared<http::Listener>(impl->listener_options(), impl->handler_for(wait));
    const http::Outcome<std::uint16_t> started = listener->start();
    if (!started) {
        impl->note_problem(started.error);
        impl->release();
        return nullptr;
    }
    return std::make_unique<Sitting>(impl, std::move(wait), std::move(listener));
}

// Hands the URL to the application; false when the application's function threw.
bool show_browser(const std::shared_ptr<LoopbackRedirect::Impl>& impl, const AuthorizationPromptInfo& info) {
    try {
        impl->options.open_browser(info.authorization_url);
        return true;
    } catch (...) {
        impl->note_problem("open_browser threw");
        return false;
    }
}

AuthorizationPromptAnswer answer_of(const Wait& wait) {
    AuthorizationPromptAnswer answer;
    answer.callback_url = wait.callback();
    return answer;
}

AuthorizationPromptAnswer run_plain(const std::shared_ptr<LoopbackRedirect::Impl>& impl,
                                    const AuthorizationPromptInfo& info, AuthorizationPromptContext& context) {
    const auto sitting = open_sitting(impl, info);
    if (!sitting) return {};
    const std::shared_ptr<Wait> wait = sitting->wait();
    const std::stop_callback cancel(context.stop_token(), [wait] { wait->cancel(); });
    if (!wait->done() && !show_browser(impl, info)) wait->cancel();
    wait->block();
    sitting->close();
    return answer_of(*wait);
}

// Waits until `ready()`, woken by the timer the Wait's wake function sets to a time that has passed.
template <class Ready>
asio::awaitable<void> wait_until(const std::shared_ptr<asio::steady_timer>& timer, Ready ready) {
    for (;;) {
        timer->expires_at(Clock::time_point::max());
        if (ready()) co_return;
        std::error_code ignored;
        co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
}

asio::awaitable<AuthorizationPromptAnswer> run_coroutine(std::shared_ptr<LoopbackRedirect::Impl> impl,
                                                         AuthorizationPromptInfo info,
                                                         AuthorizationPromptContext& context) {
    const auto sitting = open_sitting(impl, info);
    if (!sitting) co_return AuthorizationPromptAnswer{};
    const std::shared_ptr<Wait> wait = sitting->wait();
    const auto executor = co_await asio::this_coro::executor;
    const auto timer = std::make_shared<asio::steady_timer>(executor);
    wait->set_wake([executor, timer] {
        asio::post(executor, [timer] { timer->expires_at(Clock::time_point::min()); });
    });
    const std::stop_callback cancel(context.stop_token(), [wait] { wait->cancel(); });
    if (!wait->done() && !show_browser(impl, info)) wait->cancel();
    co_await wait_until(timer, [&] { return wait->done(); });
    // Closing can take a moment (a connection that has not sent anything is let go first); not on the client's thread.
    sitting->close_later();
    co_await wait_until(timer, [&] { return wait->closed(); });
    co_return answer_of(*wait);
}

}  // namespace

LoopbackRedirect::LoopbackRedirect(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

LoopbackRedirect::~LoopbackRedirect() = default;

Result<std::shared_ptr<LoopbackRedirect>> LoopbackRedirect::create(LoopbackRedirectOptions options) {
    if (!options.open_browser) return invalid("LoopbackRedirectOptions::open_browser is empty");
    if (options.max_callback_bytes == 0) return invalid("LoopbackRedirectOptions::max_callback_bytes must be positive");
    if (options.read_timeout.count() <= 0) return invalid("LoopbackRedirectOptions::read_timeout must be positive");

    auto impl = std::make_shared<Impl>();
    impl->options = std::move(options);
    impl->port = impl->options.port;

    // Bind once to find out whether the options can work, and which port is the system's choice. The handler is never
    // reached: nobody learns this port before it is closed.
    http::Listener probe(impl->listener_options(), [](http::Request, http::Responder responder) {
        responder.complete(http::make_response(unavailable_status));
    });
    const http::Outcome<std::uint16_t> bound = probe.start();
    if (!bound) return invalid("LoopbackRedirectOptions: " + bound.error);
    impl->port = *bound.value;
    probe.stop();

    impl->redirect_uri = std::string(http_scheme) + "://" + LoopbackRedirectDefaults::host + ":" +
                         std::to_string(impl->port) + impl->options.path;
    return std::shared_ptr<LoopbackRedirect>(new LoopbackRedirect(std::move(impl)));
}

const std::string& LoopbackRedirect::redirect_uri() const noexcept { return impl_->redirect_uri; }

std::uint16_t LoopbackRedirect::port() const noexcept { return impl_->port; }

AuthorizationPrompt LoopbackRedirect::plain_prompt() const {
    AuthorizationPrompt prompt;
    prompt.handler([impl = impl_](const AuthorizationPromptInfo& info, AuthorizationPromptContext& context) {
        return run_plain(impl, info, context);
    });
    return prompt;
}

AuthorizationPrompt LoopbackRedirect::coroutine_prompt() const {
    AuthorizationPrompt prompt;
    // Not a coroutine itself: its captures live in the handler, which a coroutine frame must not refer to.
    prompt.handler([impl = impl_](AuthorizationPromptInfo info,
                                  AuthorizationPromptContext& context) -> asio::awaitable<AuthorizationPromptAnswer> {
        return run_coroutine(impl, std::move(info), context);
    });
    return prompt;
}

std::string LoopbackRedirect::last_problem() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->problem;
}

}  // namespace neomcp
