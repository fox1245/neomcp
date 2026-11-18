#include "neomcp/http_server.h"

#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

namespace neomcp {

namespace {

http::PolicyOptions policy_options(const HttpServerOptions& options, std::uint16_t port) {
    http::PolicyOptions policy;
    policy.bind_address = options.bind_address;
    policy.bound_port = port;
    policy.allowed_hosts = options.allowed_hosts;
    policy.allowed_origins = options.allowed_origins;
    policy.host_check = options.host_check;
    return policy;
}

http::ListenerOptions listener_options(const HttpServerOptions& options, std::size_t max_body_bytes) {
    http::ListenerOptions listener;
    listener.bind_address = options.bind_address;
    listener.port = options.port;
    listener.path = options.path;
    listener.max_body_bytes = max_body_bytes;
    listener.worker_threads = options.worker_threads;
    listener.max_worker_threads = options.max_worker_threads;
    listener.worker_idle_timeout = options.worker_idle_timeout;
    listener.max_queued_connections = options.max_queued_connections;
    listener.read_timeout = options.read_timeout;
    listener.write_timeout = options.write_timeout;
    listener.idle_connection_timeout = options.idle_connection_timeout;
    listener.keep_alive_interval = options.keep_alive_interval;
    listener.disconnect_poll_interval = options.disconnect_poll_interval;
    return listener;
}

void say(bool diagnostics, const std::string& text) {
    if (!diagnostics) return;
    std::fputs(("neomcp: " + text + "\n").c_str(), stderr);
}

}  // namespace

HttpServer::HttpServer(Server& server, HttpServerOptions options) : server_(server), options_(std::move(options)) {}

HttpServer::~HttpServer() { stop(); }

http::Outcome<std::uint16_t> HttpServer::start() {
    using Result = http::Outcome<std::uint16_t>;
    if (started_) return Result::failure("the server already started");
    started_ = true;

    // The Host check is built before anything is bound, so that a server it could not protect (a non-loopback address
    // without named hosts) never listens. It is built again once the port is known; until then a Host that names a
    // port is refused.
    auto provisional = http::HostPolicy::create(policy_options(options_, 0));
    if (!provisional) return Result::failure(provisional.error);
    auto endpoint = http::Endpoint::create(server_, std::move(*provisional.value), options_.endpoint);
    if (!endpoint) return Result::failure(endpoint.error);
    endpoint_ = std::move(*endpoint.value);

    listener_ = std::make_unique<http::Listener>(listener_options(options_, server_.options().max_message_bytes),
                                                 endpoint_->handler());
    auto bound = listener_->start();
    if (!bound) {
        stop();
        return Result::failure(bound.error);
    }
    auto policy = http::HostPolicy::create(policy_options(options_, *bound.value));
    if (!policy) {
        stop();
        return Result::failure(policy.error);
    }
    endpoint_->set_policy(std::move(*policy.value));
    return Result::success(*bound.value);
}

void HttpServer::stop() noexcept {
    // The sessions first, so that the requests waiting for answers are answered and the streams end; then the listener,
    // whose workers are free by then.
    if (endpoint_) endpoint_->stop();
    if (listener_) listener_->stop();
}

std::uint16_t HttpServer::port() const noexcept { return listener_ ? listener_->port() : 0; }

std::size_t HttpServer::session_count() const { return endpoint_ ? endpoint_->session_count() : 0; }

int run_http(Server& server, HttpServerOptions options) {
    const bool diagnostics = options.diagnostics;
    const auto on_listening = options.on_listening;
    const std::string path = options.path;
    const std::string address = options.bind_address;
    try {
        HttpServer http(server, std::move(options));
        // Asked to end: the handlers are in place before the server serves, so no signal is missed once it does.
        asio::io_context io;
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([](const std::error_code&, int) {});

        const auto started = http.start();
        if (!started) {
            say(diagnostics, "cannot serve over HTTP: " + started.error);
            return 1;
        }
        say(diagnostics, "serving MCP over HTTP on " + address + " port " + std::to_string(*started.value) + ", path " +
                             path);
        if (on_listening) on_listening(*started.value);
        io.run();  // returns once a signal arrived
        http.stop();
        return 0;
    } catch (const std::exception& error) {
        say(diagnostics, std::string("cannot serve over HTTP: ") + error.what());
        return 1;
    }
}

}  // namespace neomcp
