// What the transfer thread of an HTTP transport read, carried to the client's thread: one queue, drained there by one
// drain at a time, in the order it was read. Shared by the transports of this target.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>

#include "client_http/transfers.h"

namespace neomcp::detail::http_client {

// What the transfer thread read, carried to the client's thread: one queue, drained there by one drain at a time.
class Delivery : public std::enable_shared_from_this<Delivery> {
public:
    using Handler = std::function<void(UnitId unit, TransferItem item)>;

    // On the client's thread.
    void attach(asio::any_io_executor executor, Handler handler) {
        std::lock_guard lock(mutex_);
        executor_ = std::move(executor);
        handler_ = std::move(handler);
        attached_ = true;
    }

    // On the client's thread: nothing more is delivered.
    void detach() {
        std::lock_guard lock(mutex_);
        attached_ = false;
        handler_ = nullptr;
        items_.clear();
    }

    // Any thread.
    void push(UnitId unit, TransferItem item) {
        std::lock_guard lock(mutex_);
        if (!attached_) return;
        items_.emplace_back(unit, std::move(item));
        if (drain_posted_) return;
        drain_posted_ = true;
        asio::post(executor_, [self = shared_from_this()] { self->drain(); });
    }

private:
    // On the client's thread, where alone the handler runs and is cleared, so a delivery never races detach().
    void drain() {
        while (true) {
            std::pair<UnitId, TransferItem> next{connection_unit, StreamEnded{}};
            Handler handler;  // a copy: the handler may detach the delivery while it runs
            {
                std::lock_guard lock(mutex_);
                if (!attached_ || items_.empty()) {
                    drain_posted_ = false;
                    return;
                }
                next = std::move(items_.front());
                items_.pop_front();
                handler = handler_;
            }
            handler(next.first, std::move(next.second));
        }
    }

    std::mutex mutex_;
    asio::any_io_executor executor_;
    Handler handler_;
    bool attached_ = false;
    bool drain_posted_ = false;
    std::deque<std::pair<UnitId, TransferItem>> items_;
};

}  // namespace neomcp::detail::http_client
