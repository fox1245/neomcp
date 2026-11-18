#include "http/timer.h"

#include <exception>

namespace neomcp::http::detail {

Timer::Timer() : thread_([this] { run(); }) {}

Timer::~Timer() { stop(); }

Timer::Id Timer::schedule(Clock::time_point when, std::function<void()> job) {
    std::lock_guard lock(mutex_);
    if (stopped_) return 0;
    const Id id = next_++;
    jobs_.emplace(std::make_pair(when, id), std::move(job));
    when_.emplace(id, when);
    changed_.notify_all();
    return id;
}

void Timer::cancel(Id id) {
    std::lock_guard lock(mutex_);
    const auto found = when_.find(id);
    if (found == when_.end()) return;
    jobs_.erase(std::make_pair(found->second, id));
    when_.erase(found);
    changed_.notify_all();
}

void Timer::stop() {
    std::thread thread;
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        jobs_.clear();
        when_.clear();
        thread = std::move(thread_);
    }
    changed_.notify_all();
    if (thread.joinable()) thread.join();
}

void Timer::run() {
    std::unique_lock lock(mutex_);
    while (!stopped_) {
        if (jobs_.empty()) {
            changed_.wait(lock);
            continue;
        }
        const auto first = jobs_.begin();
        if (Clock::now() < first->first.first) {
            changed_.wait_until(lock, first->first.first);
            continue;
        }
        std::function<void()> job = std::move(first->second);
        when_.erase(first->first.second);
        jobs_.erase(first);
        lock.unlock();
        try {
            job();
        } catch (...) {
            // A job that throws must not end the thread that runs the others.
        }
        job = nullptr;  // whatever the job holds is released without the lock
        lock.lock();
    }
}

}  // namespace neomcp::http::detail
