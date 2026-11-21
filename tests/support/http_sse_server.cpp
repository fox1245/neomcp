// The test-only server of the transport of events over HTTP (http_sse_server.h).
#include "http_sse_server.h"

#include <charconv>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "neomcp/http/body_stream.h"
#include "neomcp/http/listener.h"
#include "neomcp/http/message.h"
#include "neomcp/http/sse.h"
#include "neomcp/json.h"
#include "neomcp/profile.h"
#include "neomcp/session.h"

namespace http_sse_test {

namespace {

constexpr std::string_view revision = "2024-11-05";
constexpr std::string_view stream_key = "stream";
constexpr std::string_view get_method = "GET";
constexpr std::string_view post_method = "POST";
constexpr int status_ok = 200;
constexpr int status_accepted = 202;
constexpr int status_not_found = 404;
constexpr int status_method_not_allowed = 405;
constexpr int status_server_error = 500;

// One event of the given type.
std::string frame(const std::string& type, const std::string& data) {
    return "event: " + type + "\n" + neomcp::http::sse_event(data);
}

// The stream a query names ("stream=<n>"); empty when it names none.
std::optional<std::uint64_t> stream_of(std::string_view query) {
    const auto equals = query.find('=');
    if (equals == std::string_view::npos || query.substr(0, equals) != stream_key) return std::nullopt;
    const std::string_view digits = query.substr(equals + 1);
    std::uint64_t id = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (error != std::errc() || end != digits.data() + digits.size()) return std::nullopt;
    return id;
}

std::string method_of(const std::string& body) {
    const neomcp::json message = neomcp::json::parse(body, nullptr, false);
    const auto* members = message.get_ptr<const neomcp::json::object_t*>();
    if (members == nullptr) return {};
    const auto found = members->find("method");
    if (found == members->end() || !found->second.is_string()) return {};
    return found->second.get<std::string>();
}

}  // namespace

struct HttpSseServer::Impl {
    struct Connection {
        std::shared_ptr<neomcp::http::BodyStream> stream;
        std::unique_ptr<neomcp::Session> session;
        std::mutex receiving;  // a session takes its input on one thread at a time
    };

    Impl(neomcp::Server& server_in, HttpSseServerOptions options_in)
        : server(server_in), options(std::move(options_in)) {
        const neomcp::profile::ProfilePtr builtin = neomcp::profile::builtin(revision);
        if (options.endpoint_event.empty()) options.endpoint_event = builtin->http_sse()->endpoint_event;
        if (options.message_event.empty()) options.message_event = builtin->http_sse()->message_event;
        if (options.first_event.empty()) options.first_event = options.endpoint_event;
    }

    void handle(neomcp::http::Request request, neomcp::http::Responder responder) {
        if (options.intercept) {
            if (auto replaced = options.intercept(request)) {
                responder.complete(std::move(*replaced));
                return;
            }
        }
        if (request.method == get_method) {
            open_stream(responder);
            return;
        }
        if (request.method == post_method) {
            deliver(request, responder);
            return;
        }
        responder.complete(neomcp::http::make_response(status_method_not_allowed));
    }

    void open_stream(const neomcp::http::Responder& responder) {
        auto connection = std::make_shared<Connection>();
        connection->stream = std::make_shared<neomcp::http::BodyStream>();
        const std::weak_ptr<neomcp::http::BodyStream> weak = connection->stream;
        const std::string message_event = options.message_event;
        auto made = server.connect_tagged(neomcp::profile::Transport::http_sse,
                                          [weak, message_event](neomcp::Output output) {
                                              if (output.line.empty()) return;
                                              if (const auto stream = weak.lock()) {
                                                  stream->append(frame(message_event, output.line));
                                              }
                                          });
        if (!std::holds_alternative<std::unique_ptr<neomcp::Session>>(made)) {
            responder.complete(neomcp::http::make_response(status_server_error));
            return;
        }
        connection->session = std::get<std::unique_ptr<neomcp::Session>>(std::move(made));
        std::uint64_t id = 0;
        {
            std::lock_guard lock(mutex);
            id = next++;
        }
        const std::string query = std::string(stream_key) + "=" + std::to_string(id);
        if (options.send_endpoint) {
            const std::string reference =
                options.endpoint_reference ? options.endpoint_reference(query) : options.path + "?" + query;
            connection->stream->append(frame(options.first_event, reference));
        }
        {
            std::lock_guard lock(mutex);
            connections.emplace(id, connection);
            ++opened;
        }
        changed.notify_all();
        // The client went away: its session goes too.
        connection->stream->on_close([this, id] { forget(id); });
        neomcp::http::Response response;
        response.status = status_ok;
        response.headers.set("Content-Type", std::string(neomcp::http::event_stream_media_type));
        response.headers.set("Cache-Control", "no-cache");
        response.stream = connection->stream;
        responder.complete(std::move(response));
    }

    void deliver(const neomcp::http::Request& request, const neomcp::http::Responder& responder) {
        const auto id = stream_of(request.query);
        if (!id) {
            {
                std::lock_guard lock(mutex);
                ++refused;
            }
            responder.complete(neomcp::http::make_response(status_method_not_allowed));
            return;
        }
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(mutex);
            const auto found = connections.find(*id);
            if (found != connections.end()) connection = found->second;
            if (connection) {
                const std::string method = method_of(request.body);
                if (!method.empty()) methods.push_back(method);
            }
        }
        if (!connection) {
            responder.complete(neomcp::http::make_response(status_not_found));
            return;
        }
        {
            std::lock_guard lock(connection->receiving);
            connection->session->receive(request.body);
        }
        responder.complete(neomcp::http::make_response(status_accepted));
    }

    void forget(std::uint64_t id) {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(mutex);
            const auto found = connections.find(id);
            if (found == connections.end()) return;
            connection = found->second;
            connections.erase(found);
        }
        connection->session->close();
    }

    void drop_all() {
        std::map<std::uint64_t, std::shared_ptr<Connection>> taken;
        {
            std::lock_guard lock(mutex);
            taken.swap(connections);
        }
        for (auto& [id, connection] : taken) {
            connection->session->close();
            connection->stream->end();
        }
    }

    neomcp::Server& server;
    HttpSseServerOptions options;
    std::unique_ptr<neomcp::http::Listener> listener;
    std::uint16_t port = 0;

    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::map<std::uint64_t, std::shared_ptr<Connection>> connections;
    std::uint64_t next = 1;
    std::size_t opened = 0;
    std::size_t refused = 0;
    std::vector<std::string> methods;
    bool stopped = false;
};

HttpSseServer::HttpSseServer(neomcp::Server& server, HttpSseServerOptions options)
    : impl_(std::make_unique<Impl>(server, std::move(options))) {
    neomcp::http::ListenerOptions listening;
    listening.path = impl_->options.path;
    listening.keep_alive_interval = std::chrono::hours(1);
    Impl* impl = impl_.get();
    impl_->listener = std::make_unique<neomcp::http::Listener>(
        std::move(listening),
        [impl](neomcp::http::Request request, neomcp::http::Responder responder) {
            impl->handle(std::move(request), std::move(responder));
        });
    const auto started = impl_->listener->start();
    if (!started) throw std::runtime_error("the server of events over HTTP did not start: " + started.error);
    impl_->port = *started.value;
}

HttpSseServer::~HttpSseServer() { stop(); }

std::uint16_t HttpSseServer::port() const noexcept { return impl_->port; }

std::string HttpSseServer::origin() const { return "http://127.0.0.1:" + std::to_string(impl_->port); }

std::string HttpSseServer::url() const { return origin() + impl_->options.path; }

std::size_t HttpSseServer::streams() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->opened;
}

bool HttpSseServer::wait_for_streams(std::size_t n, std::chrono::milliseconds bound) const {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, bound, [&] { return impl_->opened >= n; });
}

std::vector<std::string> HttpSseServer::posted_methods() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->methods;
}

std::size_t HttpSseServer::posted(const std::string& method) const {
    std::size_t count = 0;
    for (const auto& one : posted_methods()) count += one == method ? 1 : 0;
    return count;
}

std::size_t HttpSseServer::refused_posts() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->refused;
}

void HttpSseServer::drop_streams() { impl_->drop_all(); }

void HttpSseServer::stop() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopped) return;
        impl_->stopped = true;
    }
    impl_->drop_all();
    impl_->listener->stop();
}

}  // namespace http_sse_test
