// One thread that runs jobs at given times. Internal to the HTTP endpoint, which uses a single one for every deadline
// it has (when a silent request gets an event stream, when an idle session is closed), so the number of threads does
// not grow with the requests.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace neomcp::http::detail {

class Timer {
public:
    using Clock = std::chrono::steady_clock;
    using Id = std::uint64_t;

    // Starts the thread; throws std::system_error when it cannot.
    Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    // Calls stop().
    ~Timer();

    // Runs `job` on the timer's thread at `when` or soon after, with no lock of the timer held. After stop() the job is
    // dropped and 0 returned; otherwise the id (never 0) for cancel().
    Id schedule(Clock::time_point when, std::function<void()> job);

    // Drops the job when it has not started; a job that is running is not waited for. An unknown id is ignored.
    void cancel(Id id);

    // Drops every job that has not started, waits for the one running, and joins the thread. Idempotent. Must not be
    // called from a job.
    void stop();

private:
    void run();

    std::mutex mutex_;
    std::condition_variable changed_;
    // By time, then by id, so that two jobs of the same time run in the order they were scheduled.
    std::map<std::pair<Clock::time_point, Id>, std::function<void()>> jobs_;
    std::map<Id, Clock::time_point> when_;  // the time of every job not yet started
    Id next_ = 1;
    bool stopped_ = false;
    std::thread thread_;
};

}  // namespace neomcp::http::detail
