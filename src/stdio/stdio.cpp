// The stdio transport.
//
// Threads and ownership:
// - The thread that calls serve_streams reads, cuts lines and calls Session::receive. The session writes through the
//   sink, on that thread or on a tool's thread, one at a time.
// - Everything the sink and the idle callback share lives in one State held by shared pointers. A callback that began
//   before close() can still be running on a tool's thread when serve_streams returns, so the State must outlive the
//   call; the writer is not in it: the session never calls the sink after close() returned, and the sink is the only
//   user of the writer.
// - State::mutex guards nothing but the wait: the idle callback and the failure path take it only to signal, and the
//   waiter holds it while it asks Session::idle(). The session calls the callback without a lock of its own held, so
//   the two locks are never taken in the opposite order.
#include "neomcp/stdio.h"

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "stdio/fd_streams.h"
#include "stdio/line_splitter.h"

namespace neomcp {
namespace {

// What the sink throws to close the session when a write failed.
struct WriteFailed : std::runtime_error {
    WriteFailed() : std::runtime_error("neomcp: the output stream could not be written") {}
};

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    bool write_failed = false;  // guarded by mutex
};

void say(bool diagnostics, const std::string& text) {
    if (!diagnostics) return;
    std::fputs(("neomcp: " + text + "\n").c_str(), stderr);
}

}  // namespace

int serve_streams(Server& server, ByteReader& input, ByteWriter& output, StdioOptions options) {
    if (options.read_chunk_bytes == 0) {
        throw std::invalid_argument("neomcp: StdioOptions::read_chunk_bytes is zero");
    }
    const detail::SigpipeGuard sigpipe(options.ignore_sigpipe);

    const auto state = std::make_shared<State>();
    const bool diagnostics = options.diagnostics;
    const auto sink = [state, &input, &output, diagnostics](std::string line) {
        line.push_back('\n');
        if (output.write(line)) return;
        bool first = false;
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            first = !state->write_failed;
            state->write_failed = true;
        }
        if (first) say(diagnostics, "writing to the output stream failed; closing the session");
        input.interrupt();
        state->changed.notify_all();
        throw WriteFailed();
    };
    const auto on_idle = [state] {
        const std::lock_guard<std::mutex> lock(state->mutex);
        state->changed.notify_all();
    };

    auto created = server.connect(sink, on_idle);
    if (auto* error = std::get_if<CreateError>(&created)) {
        say(options.diagnostics, "cannot serve a session: " + error->message);
        return 1;
    }
    const std::unique_ptr<Session> session = std::move(std::get<std::unique_ptr<Session>>(created));

    const auto failed = [&] {
        const std::lock_guard<std::mutex> lock(state->mutex);
        return state->write_failed;
    };

    detail::LineSplitter lines(server.options().max_message_bytes);
    std::string chunk(options.read_chunk_bytes, '\0');
    const auto deliver = [&](std::string_view line) { session->receive(line); };
    try {
        for (;;) {
            if (failed()) break;
            const std::size_t count = input.read(chunk.data(), chunk.size());
            if (count == 0) {
                lines.finish(deliver);
                break;
            }
            if (failed()) break;
            lines.feed(std::string_view(chunk.data(), count), deliver);
        }
        if (!failed() && options.drain_timeout > std::chrono::milliseconds::zero()) {
            // The input ended: wait for the calls in flight to be answered and written.
            std::unique_lock<std::mutex> lock(state->mutex);
            state->changed.wait_for(lock, options.drain_timeout,
                                    [&] { return state->write_failed || session->idle(); });
        }
    } catch (const WriteFailed&) {
        // The session is closed already; there is nothing left to drain.
    }
    session->close();
    return 0;
}

int run_stdio(Server& server, StdioOptions options) {
    const detail::BinaryMode binary_input(0);
    const detail::BinaryMode binary_output(1);
    detail::FdReader input(0);
    detail::FdWriter output(1);
    return serve_streams(server, input, output, std::move(options));
}

}  // namespace neomcp
