// The in-memory transport (neomcp/client/memory_transport.h).
#include "neomcp/client/memory_transport.h"

#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <asio/post.hpp>

namespace neomcp {

namespace detail {

namespace {

struct LinkMessage {
    UnitId unit = connection_unit;
    std::string text;
};
struct LinkEvent {
    UnitId unit = connection_unit;
    StreamEvent event;
};
struct LinkRestarted {};

using LinkItem = std::variant<LinkMessage, LinkEvent, LinkRestarted>;

}  // namespace

// What a link shares with its transport: the client's events while the transport is open, and what waits to be
// delivered. Items are delivered on the client's thread in the order they were given, by one drain at a time.
class MemoryLinkState : public std::enable_shared_from_this<MemoryLinkState> {
public:
    explicit MemoryLinkState(TransportEvents& events) : events_(&events), executor_(events.executor()) {}

    static MemoryLink link(std::shared_ptr<MemoryLinkState> state) { return MemoryLink(std::move(state)); }

    void give(LinkItem item) {
        std::lock_guard lock(mutex_);
        if (events_ == nullptr) return;
        items_.push_back(std::move(item));
        if (drain_posted_) return;
        drain_posted_ = true;
        asio::post(executor_, [self = shared_from_this()] { self->drain(); });
    }

    bool open() const noexcept {
        std::lock_guard lock(mutex_);
        return events_ != nullptr;
    }

    // On the client's thread.
    void close() {
        std::lock_guard lock(mutex_);
        events_ = nullptr;
        items_.clear();
    }

private:
    // On the client's thread, where alone the events object is used; it is cleared there too, so a delivery never
    // races the close.
    void drain() {
        while (true) {
            LinkItem item;
            TransportEvents* events = nullptr;
            {
                std::lock_guard lock(mutex_);
                if (items_.empty() || events_ == nullptr) {
                    drain_posted_ = false;
                    return;
                }
                item = std::move(items_.front());
                items_.pop_front();
                events = events_;
            }
            deliver(*events, std::move(item));
        }
    }

    static void deliver(TransportEvents& events, LinkItem item) {
        if (auto* message = std::get_if<LinkMessage>(&item)) {
            static_cast<void>(events.received(message->unit, message->text));
        } else if (auto* event = std::get_if<LinkEvent>(&item)) {
            events.stream_event(event->unit, std::move(event->event));
        } else {
            events.restarted();
        }
    }

    mutable std::mutex mutex_;
    TransportEvents* events_;
    asio::any_io_executor executor_;
    std::deque<LinkItem> items_;
    bool drain_posted_ = false;
};

namespace {

class MemoryTransport final : public Transport {
public:
    MemoryTransport(profile::Transport transport_kind, std::shared_ptr<MemoryPeer> peer)
        : kind_(transport_kind), peer_(std::move(peer)) {}

    profile::Transport kind() const noexcept override { return kind_; }

    std::optional<std::string> start(TransportEvents& events) override {
        if (state_) throw std::logic_error("the in-memory transport was started twice");
        state_ = std::make_shared<MemoryLinkState>(events);
        return peer_->attach(MemoryLinkState::link(state_));
    }

    void send(Outgoing outgoing) override {
        if (closed_ || !state_) return;
        peer_->receive(std::move(outgoing));
    }

    asio::awaitable<void> close(CloseCause cause) override {
        if (!closed_) {
            closed_ = true;
            if (state_) state_->close();
            peer_->detach(cause);
        }
        co_return;
    }

private:
    profile::Transport kind_;
    std::shared_ptr<MemoryPeer> peer_;
    std::shared_ptr<MemoryLinkState> state_;
    bool closed_ = false;
};

}  // namespace

}  // namespace detail

void MemoryLink::message(UnitId unit, std::string message) const {
    if (state_) state_->give(detail::LinkMessage{unit, std::move(message)});
}

void MemoryLink::event(UnitId unit, StreamEvent event) const {
    if (state_) state_->give(detail::LinkEvent{unit, std::move(event)});
}

void MemoryLink::restarted() const {
    if (state_) state_->give(detail::LinkRestarted{});
}

bool MemoryLink::open() const noexcept { return state_ && state_->open(); }

std::unique_ptr<Transport> make_memory_transport(profile::Transport kind, std::shared_ptr<MemoryPeer> peer) {
    if (!peer) throw std::invalid_argument("an in-memory transport needs a peer");
    return std::make_unique<detail::MemoryTransport>(kind, std::move(peer));
}

}  // namespace neomcp
