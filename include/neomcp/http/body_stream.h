// A response body that is produced over time.
//
// One object is shared by the producer (the code that answers a request) and the consumer (the listener that writes
// the bytes to the peer). It is safe to use from any number of threads.
//
// The producer appends chunks and ends the stream. Appending never blocks: the buffer is bounded, and an append that
// would exceed the bound is refused with Append::full and changes nothing, so a producer that outruns the peer learns
// of it instead of growing memory. The bound counts bytes that were appended and not yet read by the consumer.
//
// The consumer reads what has accumulated, waiting for more for a limited time, and closes the stream when the peer
// is gone or the listener stops. A closed stream discards its buffer, refuses later appends with Append::closed, and
// reports the close once to the callback registered with on_close(). The producer uses that callback to stop work
// that nobody will read.
//
// A stream that the consumer has read to its end is finished: closing it afterwards is not a close, and the callback
// is not called. A stream that was ended by the producer but not yet read to its end is not finished: closing it
// discards the unread bytes and calls the callback.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace neomcp::http {

class BodyStream {
public:
    // The default bound on the bytes that wait for the consumer. 1 MiB.
    static constexpr std::size_t default_max_buffered_bytes = std::size_t{1} << 20;

    enum class Append {
        accepted,  // the bytes were buffered (an empty chunk is accepted and changes nothing)
        full,      // the bytes would exceed the bound; nothing was buffered, the stream is still open
        ended,     // the producer already ended the stream
        closed,    // the consumer closed the stream; the bytes were dropped
    };

    enum class Read {
        data,     // `out` holds the bytes that accumulated since the last read
        timeout,  // nothing arrived within the time; the stream is still open
        end,      // the producer ended the stream and everything was read; the stream is finished
        closed,   // the stream was closed (by close())
    };

    explicit BodyStream(std::size_t max_buffered_bytes = default_max_buffered_bytes);
    BodyStream(const BodyStream&) = delete;
    BodyStream& operator=(const BodyStream&) = delete;

    // ---- Producer side ----

    // Buffers `chunk` for the consumer. Thread-safe.
    Append append(std::string_view chunk);

    // No more bytes will be appended. Idempotent. Bytes already buffered are still delivered.
    void end();

    // Registers the function called once when the consumer closes the stream before it was read to its end. It runs
    // on the thread that closes the stream, never under the stream's lock, so it may call back into the stream. If the
    // stream is already closed, it runs at once on the calling thread. A second registration replaces the first when
    // that has not run yet. Must not throw; an exception that escapes is swallowed.
    void on_close(std::function<void()> callback);

    // True once the consumer closed the stream.
    bool closed() const;

    // Waits until a stream the producer has ended is read to its end, until it is closed, or until the deadline.
    // Returns at once for a stream that was not ended. True when the stream is finished.
    bool wait_finished(std::chrono::steady_clock::time_point deadline);

    // ---- Consumer side ----

    // Moves the accumulated bytes into `out` (replacing its content) and returns Read::data; if there are none, waits
    // up to `timeout` for the producer to append, end or the stream to close.
    Read read(std::string& out, std::chrono::milliseconds timeout);

    // The consumer will read no more (the peer is gone, or the listener stops). Idempotent; does nothing on a
    // finished stream. Discards buffered bytes and calls the on_close callback, if any, once.
    void close() noexcept;

private:
    const std::size_t max_buffered_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable activity_;
    std::string buffer_;
    bool ended_ = false;
    bool closed_ = false;
    bool finished_ = false;
    std::function<void()> on_close_;
};

}  // namespace neomcp::http
