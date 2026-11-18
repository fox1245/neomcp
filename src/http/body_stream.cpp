#include "neomcp/http/body_stream.h"

#include <utility>

namespace neomcp::http {

namespace {

void call_quietly(std::function<void()>& callback) noexcept {
    if (!callback) return;
    try {
        callback();
    } catch (...) {
        // A callback that throws must not take down the thread that closes the stream.
    }
}

}  // namespace

BodyStream::BodyStream(std::size_t max_buffered_bytes) : max_buffered_bytes_(max_buffered_bytes) {}

BodyStream::Append BodyStream::append(std::string_view chunk) {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return Append::closed;
        if (ended_) return Append::ended;
        if (chunk.empty()) return Append::accepted;
        if (chunk.size() > max_buffered_bytes_ - buffer_.size()) return Append::full;
        buffer_.append(chunk);
    }
    activity_.notify_all();
    return Append::accepted;
}

void BodyStream::end() {
    {
        std::lock_guard lock(mutex_);
        if (ended_ || closed_) return;
        ended_ = true;
    }
    activity_.notify_all();
}

void BodyStream::on_close(std::function<void()> callback) {
    {
        std::lock_guard lock(mutex_);
        if (!closed_) {
            on_close_ = std::move(callback);
            return;
        }
    }
    call_quietly(callback);
}

bool BodyStream::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

BodyStream::Read BodyStream::read(std::string& out, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!activity_.wait_for(lock, timeout, [this] { return closed_ || !buffer_.empty() || ended_; })) {
        return Read::timeout;
    }
    if (closed_) return Read::closed;
    if (!buffer_.empty()) {
        out = std::move(buffer_);
        buffer_.clear();
        return Read::data;
    }
    finished_ = true;  // ended and drained
    lock.unlock();
    activity_.notify_all();  // wait_finished() waits for this
    return Read::end;
}

bool BodyStream::wait_finished(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    activity_.wait_until(lock, deadline, [this] { return finished_ || closed_ || !ended_; });
    return finished_;
}

void BodyStream::close() noexcept {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        if (closed_ || finished_) return;
        closed_ = true;
        buffer_.clear();
        callback = std::move(on_close_);
        on_close_ = nullptr;
    }
    activity_.notify_all();
    call_quietly(callback);
}

}  // namespace neomcp::http
