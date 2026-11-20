// The clock and the timers of a client.
//
// The engine keeps its deadlines as data (DeadlineChange); the client sets a timer for each running one and calls the
// engine back when it lapses. The client also waits between the retries of a call whose result asks for nothing but
// its state. Both go through a TimerService: by default one on the client's own thread over a steady clock; a test
// gives its own, sees every timer the client sets with what it is for, and fires them when it chooses, so that no test
// waits for real time.
#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <variant>

#include "neomcp/client/engine.h"

namespace neomcp {

// The client waits before it sends again a call whose result asked for nothing but its state (`retry` counts those
// retries of the call in a row, from 1).
struct StateRetryDelay {
    RequestRef call;
    std::size_t retry = 1;
    friend bool operator==(const StateRetryDelay&, const StateRetryDelay&) = default;
};

// What a timer is for: one running stretch of an engine clock, or a wait before a retry.
using TimerPurpose = std::variant<Deadline, StateRetryDelay>;

// One timer. Never used twice by a service.
struct TimerId {
    std::uint64_t value = 0;
    friend bool operator==(const TimerId&, const TimerId&) = default;
    friend auto operator<=>(const TimerId&, const TimerId&) = default;
};

// Called by the client on its own thread only. A service may call `fire` on any thread; the client brings it to its own
// thread, and ignores a timer it cancelled or replaced, so a fire that races a cancel is harmless.
class TimerService {
public:
    virtual ~TimerService() = default;

    // The time the client measures what is left of a paused clock in.
    virtual std::chrono::steady_clock::time_point now() const = 0;

    // Calls `fire` once, `after` from now(), unless the timer is cancelled first. `after` is never negative.
    virtual TimerId start(std::chrono::milliseconds after, TimerPurpose purpose, std::function<void()> fire) = 0;

    // The timer is no longer wanted. Ignored for a timer that fired or was cancelled.
    virtual void cancel(TimerId timer) = 0;
};

}  // namespace neomcp
