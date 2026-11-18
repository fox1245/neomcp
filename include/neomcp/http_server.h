// The Streamable HTTP transport: a server speaks to any number of clients over HTTP, on one path, in both eras.
//
//   int main() {
//       neomcp::Server server({.name = "calc", .version = "1.0.0"});
//       server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });
//       neomcp::HttpServerOptions options;
//       options.port = 8000;
//       return neomcp::run_http(server, options);  // http://127.0.0.1:8000/mcp
//   }
//
// HttpServer runs neomcp::http::Endpoint (see neomcp/http/endpoint.h for what it answers) on the library's listener
// (neomcp/http/listener.h). It binds the loopback address unless told otherwise, and checks the Host and Origin
// headers of every request (neomcp/http/policy.h). There is no TLS: put a reverse proxy in front of a server that
// other machines reach.
//
// Sizing. Each request that waits for its answer and each open stream holds one worker thread of the listener for as
// long as it lasts; a connection that arrives while every worker is busy waits, up to max_queued_connections of them,
// and one beyond that is closed. So max_worker_threads bounds the requests in progress plus the open streams (a
// client of a session-based revision usually keeps one stream open, besides its requests).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "neomcp/http/endpoint.h"
#include "neomcp/http/listener.h"
#include "neomcp/http/outcome.h"
#include "neomcp/http/policy.h"
#include "neomcp/server.h"

namespace neomcp {

struct HttpServerOptions {
    // Where to listen. The loopback address by default: the server is reachable from other machines only when asked.
    std::string bind_address = http::ListenerDefaults::bind_address;
    // 0: any free port; start() reports the one chosen.
    std::uint16_t port = 0;
    // The one path served, as for http::ListenerOptions::path.
    std::string path = "/mcp";

    // The Host and Origin check (see http::PolicyOptions). On a loopback address with no allowed_hosts, the names
    // localhost, 127.0.0.1 and [::1] are allowed, with the bound port or without one. On any other address
    // allowed_hosts must name the hosts clients use, or host_check must be off; start() fails otherwise.
    std::vector<std::string> allowed_hosts{};
    std::vector<std::string> allowed_origins{};
    http::HostCheck host_check = http::HostCheck::on;

    // Sessions and streams.
    http::EndpointOptions endpoint{};

    // The listener (see http::ListenerOptions). The body bound is the server's max_message_bytes.
    std::size_t worker_threads = http::ListenerDefaults::worker_threads;
    std::size_t max_worker_threads = http::ListenerDefaults::max_worker_threads;
    std::chrono::seconds worker_idle_timeout = http::ListenerDefaults::worker_idle_timeout;
    std::size_t max_queued_connections = http::ListenerDefaults::max_queued_connections;
    std::chrono::milliseconds read_timeout = http::ListenerDefaults::read_timeout;
    std::chrono::milliseconds write_timeout = http::ListenerDefaults::write_timeout;
    std::chrono::seconds idle_connection_timeout = http::ListenerDefaults::idle_connection_timeout;
    std::chrono::milliseconds keep_alive_interval = http::ListenerDefaults::keep_alive_interval;
    std::chrono::milliseconds disconnect_poll_interval = http::ListenerDefaults::disconnect_poll_interval;

    // run_http only: write the library's diagnostics to standard error (where it listens, why it could not start).
    bool diagnostics = true;
    // run_http only: told the bound port once the server listens.
    std::function<void(std::uint16_t port)> on_listening{};
};

class HttpServer {
public:
    // The server must outlive this object.
    explicit HttpServer(Server& server, HttpServerOptions options = {});
    // Calls stop().
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds, starts serving and returns the bound port, or why it could not: an option out of range, an address or
    // port that cannot be bound, a Host check that cannot be built (a non-loopback address without allowed_hosts),
    // or revisions that cannot be served on Streamable HTTP. Starts once; a second call fails.
    http::Outcome<std::uint16_t> start();

    // Closes every session and stream (an open subscription ends gracefully, and its end is written out within the
    // write timeout), stops listening and returns when the listener's threads have left (within about the read and
    // write timeouts). Idempotent. Must not be called from a handler of the server.
    void stop() noexcept;

    // The bound port; 0 before start() and after stop().
    std::uint16_t port() const noexcept;

    // The sessions kept now (those a handshake opened and that have not ended).
    std::size_t session_count() const;

private:
    Server& server_;
    HttpServerOptions options_;
    std::unique_ptr<http::Endpoint> endpoint_;
    std::unique_ptr<http::Listener> listener_;
    bool started_ = false;
};

// Serves over HTTP until the process is asked to end (SIGINT or SIGTERM), then stops and returns 0; returns 1 when the
// server could not start (the reason goes to standard error when diagnostics are on). Writes nothing to standard
// output. The server must outlive the call.
int run_http(Server& server, HttpServerOptions options = {});

}  // namespace neomcp
