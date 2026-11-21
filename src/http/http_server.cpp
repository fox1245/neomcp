#include "neomcp/http_server.h"

#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include "http/resource_server.h"

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
    listener.body_timeout = options.body_timeout;
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
    http::EndpointOptions endpoint_options = options_.endpoint;
    if (options_.authorization) {
        if (endpoint_options.authorization) {
            return Result::failure("authorization is set twice: in the options and in their endpoint options");
        }
        endpoint_options.authorization = options_.authorization;
    }
    if (endpoint_options.authorization) {
        if (endpoint_options.authorization->bounds.max_token_bytes > http::ResourceServerDefaults::max_token_bytes) {
            return Result::failure("authorization: max_token_bytes is more than the listener lets through (" +
                                   std::to_string(http::ResourceServerDefaults::max_token_bytes) + " bytes)");
        }
    } else if (server_.declares_scopes()) {
        return Result::failure("items declare scopes, which need authorization: set the authorization options");
    }
    auto endpoint = http::Endpoint::create(server_, std::move(*provisional.value), std::move(endpoint_options));
    if (!endpoint) return Result::failure(endpoint.error);
    endpoint_ = std::move(*endpoint.value);

    http::ListenerOptions listening = listener_options(options_, server_.options().max_message_bytes);
    if (const std::optional<std::string> document_path = endpoint_->metadata_path()) {
        listening.routes.push_back(http::Route{*document_path, endpoint_->metadata_handler()});
    }
    listener_ = std::make_unique<http::Listener>(std::move(listening), endpoint_->handler());
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
    const http::ResourceServerOptions* authorization =
        options_.authorization ? &*options_.authorization
                               : (options_.endpoint.authorization ? &*options_.endpoint.authorization : nullptr);
    if (authorization != nullptr) {
        const std::optional<std::string> host = http::detail::url_authority(authorization->resource);
        if (!host || !policy.value->host_allowed(*host)) {
            stop();
            return Result::failure("authorization: the host of resource does not pass the Host check, so every client "
                                   "request would be refused");
        }
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

http::Outcome<bool> HttpServer::update_authorization_servers(std::vector<std::string> issuers) {
    if (!endpoint_) return http::Outcome<bool>::failure("the server has not started");
    return endpoint_->update_authorization_servers(std::move(issuers));
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
