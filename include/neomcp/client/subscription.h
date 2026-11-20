// A subscription to the changes of what a server offers (Client::listen).
//
//   neomcp::ChangeFilter filter;
//   filter.tools_list = true;
//   filter.resources = {"config://app"};
//   auto subscription = client->listen(filter);
//   if (!subscription) { /* subscription.error() */ }
//   while (auto change = subscription->next()) {
//       if (change->kind == neomcp::ChangeKind::tools_list) { /* list the tools again */ }
//   }
//   // The loop ends with the error that ended the subscription: subscription_ended (the server ended it),
//   // subscription_lost, closed, ...
//
// One call serves both ways a revision tells of changes (the client half of its `change_feed` mechanism, delivery()):
// - On subscriptions: listen() opens one with a request of its own, a long-lived call that has no clock once the
//   server acknowledged it, and returns when the acknowledgement arrived; honoured() is what of the filter the server
//   said it honours. The subscription's messages are told apart from those of every other subscription by the request
//   that opened it. close() ends it as the transport cancels a request (a cancellation notification on stdio, the end
//   of its stream on HTTP). A stream that ends without the server's final result is a loss: the subscription is opened
//   again after a wait drawn at random up to a bound that doubles from ClientOptions::subscription_reopen_first_delay
//   to subscription_reopen_max_delay, at most max_subscription_reopens times in a row (an acknowledgement starts the
//   count again), and then it ends with subscription_lost. A stdio server process that ended takes its subscriptions
//   with it: each is opened again at once with the new process, outside that count (max_restarts bounds the
//   restarts).
// - On the connection: the changes are notifications of the connection, which every subscription of the client
//   filters for itself. A resource is watched with a request when the filter names it and the server declared that
//   it reports updates, once for all subscriptions that name it, and is no longer watched when the last of them
//   closes. honoured() is what the server declared it reports (the list kinds whose changes it announces, the
//   resources when it reports updates), less each resource it refused to watch. When contact opens anew (a stdio
//   server process started again, a new session) the watched resources are watched again; on HTTP the client asks
//   for the stream on which the server speaks outside requests.
//
// Changes are merged: while the application has not taken them, a list's changes are one change of that list and a
// resource's updates one update of it, kept in the order of their first arrival. So a slow reader never holds more
// changes than the kinds and resources it asked for, and none it is owed is lost. When a subscription is opened again
// (after a loss, or with a new server process or session), what changed in between is not known: every kind and
// resource it honours is reported as changed once.
//
// Threads. A Subscription may be used from any thread, like its Client. next() blocks the calling thread until a
// change or the end arrives; on the client's own thread it returns on_client_thread at once. async_next() may be
// awaited from any executor and resumes there; cancelling the coroutine that awaits it ends that wait with
// `cancelled` and takes no change. Every parameter of the async_ forms is taken by value. A Subscription holds what it
// needs of its client: it outlives a closed or destroyed Client, and then ends with `closed`. Destroying a Subscription
// closes it without waiting.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <asio/awaitable.hpp>

#include "neomcp/change.h"
#include "neomcp/client/result.h"
#include "neomcp/mechanism.h"

namespace neomcp {

namespace detail {
class ClientCore;
struct SubscriptionAccess;
}  // namespace detail

class Subscription {
public:
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    // Closes the subscription, as close() does, without waiting for it.
    ~Subscription();

    // What the application asked for, with each resource once.
    const ChangeFilter& asked() const noexcept { return asked_; }
    // What of it the server honours, as it stood when listen() returned (a reopened subscription filters by what its
    // latest acknowledgement says). Never more than asked().
    const ChangeFilter& honoured() const noexcept { return honoured_; }
    // How the governing revision tells of changes.
    FeedDelivery delivery() const noexcept { return delivery_; }

    // The next change, oldest first; waits until one arrives. When none is left and the subscription ended, the error
    // that ended it: subscription_ended, subscription_lost, closed, or the error of the call that could not reopen it.
    Result<Change> next();
    asio::awaitable<Result<Change>> async_next();

    // Ends the subscription: the server is told as the revision prescribes (the subscription cancelled, or the
    // resources only it watched no longer watched), and returns once that went out and, for a resource no longer
    // watched, once the server answered. Changes not taken are dropped; a next() waiting elsewhere ends `closed`.
    // Closing a closed subscription succeeds at once. On the client's own thread: on_client_thread.
    Result<void> close();
    asio::awaitable<void> async_close();

private:
    friend struct detail::SubscriptionAccess;
    Subscription(std::shared_ptr<detail::ClientCore> core, std::uint64_t id, ChangeFilter asked_filter,
                 ChangeFilter honoured_filter, FeedDelivery how) noexcept;

    std::shared_ptr<detail::ClientCore> core_;
    std::uint64_t id_ = 0;
    ChangeFilter asked_;
    ChangeFilter honoured_;
    FeedDelivery delivery_ = FeedDelivery::subscription;
};

namespace detail {

// The longest wait before reopen `attempt` (from 1) of a lost subscription: min(most, first * 2^(attempt - 1)).
std::chrono::milliseconds subscription_reopen_bound(std::size_t attempt, std::chrono::milliseconds first,
                                                    std::chrono::milliseconds most) noexcept;
// The wait itself, drawn uniformly from [0, bound] by 64 random bits (full jitter).
std::chrono::milliseconds subscription_reopen_delay(std::chrono::milliseconds bound, std::uint64_t random) noexcept;

}  // namespace detail

}  // namespace neomcp
