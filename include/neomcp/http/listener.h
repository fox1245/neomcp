// An HTTP/1.1 listener: accepts connections, turns each request on one path into a Request, hands it to a Handler and
// writes what the handler's Responder is completed with. It knows nothing of the protocol that runs over it.
//
// The listener is plain HTTP (no TLS) on blocking sockets, one worker thread per connection that is being served. Two
// consequences matter for sizing:
//
//   - A worker thread serves a connection until it ends. A response that streams (Response::stream) therefore holds
//     its worker for as long as the stream is open, and so does a request whose Responder has not been completed yet
//     (the worker waits for the completion). The number of such responses that can be open at once is
//     ListenerOptions::max_worker_threads. Size it for the number of streams you expect plus the requests in flight.
//   - A connection that arrives when every worker is busy is accepted and kept waiting, up to max_queued_connections
//     of them; it gets no answer until a worker is free. So at most max_worker_threads + max_queued_connections
//     connections are open at once, and a connection that arrives beyond that is closed at once.
//
// The peer's disconnect is noticed by polling (ListenerOptions::disconnect_poll_interval), because a blocking
// socket gives the worker no wake-up: a client that went away is noticed within about one interval. While a stream is
// open the stream's on_close callback runs then. While the worker waits for the Responder to be completed, the
// Responder's on_disconnect callback runs then (on the worker, with no lock of the listener held), and a completion
// that arrives later is dropped. The look at the connection reads without consuming: a peer that sends more bytes
// (a pipelined request) is not taken for gone, and a peer that shuts down its sending side is.
//
// Constructing the first listener ignores SIGPIPE for the whole process (the HTTP library does that) and nothing
// restores it, so a write to a closed socket fails with an error instead of ending the process.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "neomcp/http/message.h"
#include "neomcp/http/outcome.h"

namespace neomcp::http {

// The defaults of ListenerOptions that HttpServerOptions repeats. They are constants and not read from a
// ListenerOptions object in a default member initializer, for the reason given at SessionDefaults (neomcp/session.h).
struct ListenerDefaults {
    static constexpr const char* bind_address = "127.0.0.1";
    static constexpr std::size_t worker_threads = 8;
    static constexpr std::size_t max_worker_threads = 32;
    static constexpr std::chrono::seconds worker_idle_timeout{3};
    static constexpr std::size_t max_queued_connections = 64;
    static constexpr std::chrono::milliseconds read_timeout{5000};
    static constexpr std::chrono::milliseconds write_timeout{5000};
    static constexpr std::chrono::seconds idle_connection_timeout{5};
    static constexpr std::chrono::milliseconds keep_alive_interval{15000};
    static constexpr std::chrono::milliseconds disconnect_poll_interval{250};
};

struct ListenerOptions {
    // The address to bind: an IP address or a name the system resolves. Loopback by default: a listener is reachable
    // from other machines only when the caller asks for it.
    std::string bind_address = ListenerDefaults::bind_address;

    // The port to bind. 0: any free port; start() reports the one chosen. A port that is in use is an error: the
    // listener does not share addresses.
    std::uint16_t port = 0;

    // The one path served. It starts with '/' and holds only letters, digits and '-', '_', '~', '/'. Every method on
    // it goes to the handler (GET, HEAD, POST, PUT, DELETE, PATCH and OPTIONS; a method the HTTP library does not know is
    // answered 400 by it). Any other path is answered 404, and the handler is not called.
    std::string path = "/";

    // The largest request body in bytes, at least 1. A larger body is answered 413 before the handler runs. Default
    // 8 MiB.
    std::size_t max_body_bytes = std::size_t{8} * 1024 * 1024;

    // Worker threads: `worker_threads` always exist; up to `max_worker_threads` (at least that many) exist while
    // connections are waiting, and the extra ones leave after `worker_idle_timeout` without work.
    std::size_t worker_threads = ListenerDefaults::worker_threads;
    std::size_t max_worker_threads = ListenerDefaults::max_worker_threads;
    std::chrono::seconds worker_idle_timeout = ListenerDefaults::worker_idle_timeout;

    // Connections kept waiting for a worker while every worker is busy; at least 1.
    std::size_t max_queued_connections = ListenerDefaults::max_queued_connections;

    // How long the listener waits for the next bytes of a request, and for the peer to accept bytes it writes. A
    // stream whose reader stops reading ends after the write timeout.
    std::chrono::milliseconds read_timeout = ListenerDefaults::read_timeout;
    std::chrono::milliseconds write_timeout = ListenerDefaults::write_timeout;

    // How long an idle connection is kept for another request (whole seconds, at least 1).
    std::chrono::seconds idle_connection_timeout = ListenerDefaults::idle_connection_timeout;

    // On an event stream (Content-Type text/event-stream) that has been idle this long the listener writes a
    // keep-alive comment. Other streams get none. Must be positive.
    std::chrono::milliseconds keep_alive_interval = ListenerDefaults::keep_alive_interval;

    // How often the listener looks for a disconnected peer while a stream is idle, and while a request waits for its
    // Responder to be completed. Must be positive.
    std::chrono::milliseconds disconnect_poll_interval = ListenerDefaults::disconnect_poll_interval;
};

class Listener {
public:
    // The handler is called on worker threads, possibly several at once.
    Listener(ListenerOptions options, Handler handler);

    // Calls stop().
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Binds, starts serving and returns the bound port, or the reason it could not (an option out of range, a port in
    // use, an address that cannot be bound). Does not throw for those. A listener starts once: a second call, and a
    // call after stop(), fail.
    Outcome<std::uint16_t> start();

    // Stops serving and returns when every worker has left. Idempotent and safe to call from any thread except a
    // worker (a handler, or a stream's on_close callback). Requests whose Responder is still uncompleted are answered
    // 503; an open stream that its producer has ended is given until the write timeout to be written out; every other
    // open stream is closed (so its producer is told through on_close, and the connection is cut without the
    // terminating chunk), and the listening socket is closed. It returns within about the read and write timeouts,
    // except while a handler itself blocks: a handler that does not return keeps its worker.
    void stop() noexcept;

    // True between a successful start() and stop().
    bool running() const noexcept;

    // The bound port; 0 before start().
    std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neomcp::http
