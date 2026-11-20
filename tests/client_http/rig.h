// Shared by the tests of the HTTP transport: a thread that stands for the client's, events a transport delivers,
// recorded, and a transport made and driven on that thread.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include "client_http/streamable.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"

namespace http_rig {

inline constexpr auto wait_bound = std::chrono::seconds(20);

// One thread that runs an io_context, standing for the client's thread.
class IoThread {
public:
    IoThread() : work_(asio::make_work_guard(context_)), thread_([this] { context_.run(); }) {}
    ~IoThread() {
        work_.reset();
        context_.stop();
        if (thread_.joinable()) thread_.join();
    }
    IoThread(const IoThread&) = delete;
    IoThread& operator=(const IoThread&) = delete;

    asio::any_io_executor executor() { return context_.get_executor(); }

    // Runs `work` on the thread and waits for it.
    template <class F>
    auto run(F work) -> decltype(work()) {
        std::packaged_task<decltype(work())()> task(std::move(work));
        auto result = task.get_future();
        asio::post(context_, [&task] { task(); });
        return result.get();
    }

private:
    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread thread_;
};

struct Received {
    neomcp::UnitId unit = neomcp::connection_unit;
    std::string text;
};
struct Event {
    neomcp::UnitId unit = neomcp::connection_unit;
    neomcp::StreamEvent event;
};
using Delivered = std::variant<Received, Event>;

// Records what a transport delivers. Every method of TransportEvents runs on the io thread; the records are read from
// the test's thread under the lock.
class Recorder final : public neomcp::TransportEvents {
public:
    explicit Recorder(asio::any_io_executor executor) : executor_(std::move(executor)) {}

    asio::any_io_executor executor() const noexcept override { return executor_; }

    neomcp::Reception received(neomcp::UnitId unit, std::string_view message) override {
        add(Received{unit, std::string(message)});
        return neomcp::Reception::accepted;
    }
    void stream_event(neomcp::UnitId unit, neomcp::StreamEvent event) override { add(Event{unit, std::move(event)}); }
    void restarted() override {}

    std::vector<Delivered> all() const {
        std::lock_guard lock(mutex_);
        return seen_;
    }

    // What one unit received, in order.
    std::vector<Delivered> of(neomcp::UnitId unit) const {
        std::vector<Delivered> found;
        for (const auto& one : all()) {
            const neomcp::UnitId at = std::holds_alternative<Received>(one) ? std::get<Received>(one).unit
                                                                             : std::get<Event>(one).unit;
            if (at == unit) found.push_back(one);
        }
        return found;
    }

    // Waits until `done` holds over the records; whether it did within the bound.
    bool wait(const std::function<bool(const std::vector<Delivered>&)>& done,
              std::chrono::milliseconds bound = wait_bound) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return done(seen_); });
    }

    // Waits until a unit's exchange ended (StreamEnded, StreamLost or SendFailed) or received a message, and returns
    // what the unit received.
    std::vector<Delivered> until_end(neomcp::UnitId unit) const {
        wait([&](const std::vector<Delivered>& seen) {
            for (const auto& one : seen) {
                const auto* event = std::get_if<Event>(&one);
                if (event == nullptr || event->unit != unit) continue;
                if (std::holds_alternative<neomcp::StreamEnded>(event->event) ||
                    std::holds_alternative<neomcp::StreamLost>(event->event) ||
                    std::holds_alternative<neomcp::SendFailed>(event->event)) {
                    return true;
                }
            }
            return false;
        });
        return of(unit);
    }

private:
    void add(Delivered one) {
        {
            std::lock_guard lock(mutex_);
            seen_.push_back(std::move(one));
        }
        changed_.notify_all();
    }

    asio::any_io_executor executor_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<Delivered> seen_;
};

template <class E>
const E* event_of(const Delivered& one) {
    const auto* event = std::get_if<Event>(&one);
    return event == nullptr ? nullptr : std::get_if<E>(&event->event);
}

template <class E>
std::vector<E> events_of(const std::vector<Delivered>& seen) {
    std::vector<E> found;
    for (const auto& one : seen) {
        if (const auto* event = event_of<E>(one)) found.push_back(*event);
    }
    return found;
}

inline std::vector<std::string> messages_of(const std::vector<Delivered>& seen) {
    std::vector<std::string> found;
    for (const auto& one : seen) {
        if (const auto* received = std::get_if<Received>(&one)) found.push_back(received->text);
    }
    return found;
}

// An endpoint that goes to `url` without any proxy, so that a proxy the environment names cannot reroute a test.
inline neomcp::HttpEndpoint endpoint(std::string url) {
    neomcp::HttpEndpoint endpoint;
    endpoint.url = std::move(url);
    endpoint.proxy.mode = neomcp::ProxyMode::none;
    return endpoint;
}

// A transport made, started and driven on the io thread, with its events recorded.
class Driven {
public:
    explicit Driven(neomcp::HttpEndpoint endpoint, neomcp::detail::http_client::HttpTransportSettings settings = {})
        : recorder_(io_.executor()) {
        auto made = neomcp::detail::http_client::make_http_transport(std::move(endpoint), std::move(settings));
        if (!made) {
            problem_ = made.error().message;
            return;
        }
        transport_ = std::move(made).value();
        problem_ = io_.run([this] { return transport_->start(recorder_); }).value_or(std::string());
    }

    ~Driven() { close(); }

    // Empty when the transport was made and started.
    const std::string& problem() const noexcept { return problem_; }

    void send(neomcp::Outgoing outgoing) {
        io_.run([this, &outgoing] { transport_->send(std::move(outgoing)); });
    }

    // A POST of `line` on `unit`, with the engine's header fields `headers`, under `profile` (none: no revision in
    // particular).
    void post(neomcp::UnitId unit, std::string line, std::vector<neomcp::HttpHeader> headers = {},
              neomcp::OutgoingKind kind = neomcp::OutgoingKind::request, neomcp::profile::ProfilePtr profile = nullptr) {
        send(neomcp::Outgoing{std::move(line), unit, std::move(headers), kind, std::move(profile)});
    }

    void end(neomcp::UnitId unit) {
        send(neomcp::Outgoing{std::string(), unit, {}, neomcp::OutgoingKind::end_unit, nullptr});
    }

    // The engine asks for the stream of the connection, with its fields, under `profile`.
    void open_stream(std::vector<neomcp::HttpHeader> headers, neomcp::profile::ProfilePtr profile) {
        send(neomcp::Outgoing{std::string(), neomcp::connection_unit, std::move(headers),
                              neomcp::OutgoingKind::open_connection_stream, std::move(profile)});
    }

    void close() {
        if (!transport_ || closed_) return;
        closed_ = true;
        auto done = asio::co_spawn(io_.executor(), transport_->close(neomcp::CloseCause::client_closed),
                                   asio::use_future);
        done.get();
        io_.run([this] { transport_.reset(); });
    }

    Recorder& recorder() noexcept { return recorder_; }
    IoThread& io() noexcept { return io_; }

private:
    IoThread io_;
    Recorder recorder_;
    std::unique_ptr<neomcp::Transport> transport_;
    std::string problem_;
    bool closed_ = false;
};

}  // namespace http_rig
