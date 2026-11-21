// The listener on cpp-httplib. This is the only file that includes the HTTP library: nothing of it appears in a
// public header, so another server can replace it without touching code that speaks through Handler and Responder.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "neomcp/http/body_stream.h"
#include "neomcp/http/listener.h"
#include "neomcp/http/sse.h"

namespace neomcp::http {

namespace {

constexpr int lowest_status = 100;
constexpr int highest_status = 599;
constexpr int lowest_error_status = 400;
constexpr int bad_request_status = 400;
constexpr int request_timeout_status = 408;
constexpr int content_too_large_status = 413;
constexpr int internal_error_status = 500;
constexpr int unavailable_status = 503;

constexpr std::string_view content_type_field = "Content-Type";
constexpr std::string_view content_length_field = "Content-Length";
constexpr std::string_view transfer_encoding_field = "Transfer-Encoding";
constexpr std::string_view connection_field = "Connection";
constexpr std::string_view close_token = "close";
constexpr std::string_view cache_control_field = "Cache-Control";
constexpr std::string_view proxy_buffering_field = "X-Accel-Buffering";
constexpr std::string_view no_store = "no-store";
constexpr std::string_view no = "no";
constexpr std::string_view default_content_type = "application/octet-stream";
constexpr std::string_view get_method = "GET";
constexpr std::string_view head_method = "HEAD";
constexpr std::string_view post_method = "POST";
constexpr std::string_view put_method = "PUT";
constexpr std::string_view patch_method = "PATCH";
constexpr std::string_view delete_method = "DELETE";
constexpr std::string_view allow_field = "Allow";
constexpr std::string_view route_methods = "GET, HEAD";
constexpr int method_not_allowed_status = 405;

// The methods whose requests carry a body here: the library reads their bodies through the listener (Impl::receive),
// which bounds the time a body takes. The others (GET, HEAD, OPTIONS) take none.
bool takes_body(const std::string& method) {
    return method == post_method || method == put_method || method == patch_method || method == delete_method;
}

// Whether the request's framing announces a body: a transfer coding (the library takes only `chunked`), or a
// Content-Length above zero (the library refused a malformed one before routing).
bool frames_body(const httplib::Request& incoming) {
    return incoming.has_header(std::string(transfer_encoding_field)) ||
           incoming.get_header_value_u64(std::string(content_length_field)) > 0;
}

// The route whose path is the path as received, or none: an exact comparison, not a pattern.
const Route* find_route(const ListenerOptions& options, const std::string& path) {
    for (const Route& extra : options.routes) {
        if (extra.path == path) return &extra;
    }
    return nullptr;
}

// The answer a worker waits for: filled once, by the responder, by the destruction of the last responder copy, or by
// stop().
class Exchange {
public:
    // False when the exchange was settled already; the response is then dropped.
    bool deliver(Response&& response) {
        {
            std::lock_guard lock(mutex_);
            if (response_) return false;
            response_ = std::move(response);
        }
        settled_.notify_all();
        return true;
    }

    // The response once it is there; empty when nothing settled the exchange within `timeout`.
    std::optional<Response> wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!settled_.wait_for(lock, timeout, [this] { return response_.has_value(); })) return std::nullopt;
        return std::move(*response_);
    }

private:
    std::mutex mutex_;
    std::condition_variable settled_;
    std::optional<Response> response_;
};

// The stream of the connection the worker thread serves now; null outside a request. The whole of one request is
// processed on one worker thread (DeadlineServer below), so a handler finds its connection here.
thread_local httplib::Stream* serving_stream = nullptr;

// The library's server, with what the body deadline needs: a handler reaches the stream of its connection, whose total
// time limit (Stream::set_max_timeout) also covers the chunk-size lines, chunk extensions and trailer fields of a
// chunked body, where a content reader is never called; and every read of the connection gives up once the listener
// stops (Stream::set_read_interrupt), so stop() does not wait for a peer that sends slowly.
//
// process_and_close_socket is copied from httplib::Server::process_and_close_socket of the cpp-httplib version named by
// NEOMCP_HTTPLIB_VERSION in cmake/NeomcpHttplib.cmake; the copy differs only in the lines marked as additions. When
// that version changes, compare this function with the library's again: the static_assert below stops the build until
// the version named there is the header's.
static_assert(std::string_view(CPPHTTPLIB_VERSION) == std::string_view(NEOMCP_HTTPLIB_VERSION),
              "the pinned cpp-httplib changed: compare DeadlineServer::process_and_close_socket in "
              "src/http/listener.cpp with httplib::Server::process_and_close_socket of the new version, then set "
              "NEOMCP_HTTPLIB_VERSION in cmake/NeomcpHttplib.cmake to it");

class DeadlineServer final : public httplib::Server {
public:
    explicit DeadlineServer(const std::atomic<bool>& closing) : closing_(closing) {}

private:
    bool process_and_close_socket(socket_t sock) override {
        std::string remote_addr;
        int remote_port = 0;
        httplib::detail::get_remote_ip_and_port(sock, remote_addr, remote_port);

        std::string local_addr;
        int local_port = 0;
        httplib::detail::get_local_ip_and_port(sock, local_addr, local_port);

        bool websocket_upgraded = false;
        auto ret = serve_guarded([&]() {
            return httplib::detail::process_server_socket(
                svr_sock_, sock, keep_alive_max_count_, keep_alive_timeout_sec_, read_timeout_sec_, read_timeout_usec_,
                write_timeout_sec_, write_timeout_usec_,
                [&](httplib::Stream& strm, bool close_connection, bool& connection_closed) {
                    // The additions: the stream is marked for the request on this thread, and its reads end once the
                    // listener stops.
                    strm.set_read_interrupt([&closing = closing_] { return closing.load(); });
                    serving_stream = &strm;
                    struct Unmark {
                        ~Unmark() { serving_stream = nullptr; }
                    } unmark;
                    return process_request(strm, remote_addr, remote_port, local_addr, local_port, close_connection,
                                           connection_closed, nullptr, &websocket_upgraded);
                });
        });

        httplib::detail::drain_and_close_socket(sock);
        return ret;
    }

    const std::atomic<bool>& closing_;
};

// The library's pool refuses a connection when its list of jobs not yet taken by a worker is full, which also happens
// in a burst while workers are idle and about to take them. This queue counts the connections that are open instead
// (served or waiting) and refuses one only when that number would exceed the workers' maximum plus the waiting bound.
class BoundedPool final : public httplib::TaskQueue {
public:
    BoundedPool(std::size_t base, std::size_t maximum, std::size_t waiting, time_t idle_seconds)
        : pool_(base, maximum, 0, idle_seconds), limit_(maximum + waiting) {}

    bool enqueue(std::function<void()> job) override {
        if (open_.fetch_add(1) >= limit_) {
            open_.fetch_sub(1);
            return false;
        }
        const bool taken = pool_.enqueue([this, job = std::move(job)] {
            struct Done {
                std::atomic<std::size_t>& open;
                ~Done() { open.fetch_sub(1); }
            } done{open_};
            job();
        });
        if (!taken) open_.fetch_sub(1);
        return taken;
    }

    void shutdown() override { pool_.shutdown(); }

private:
    httplib::ThreadPool pool_;
    const std::size_t limit_;
    std::atomic<std::size_t> open_{0};
};

bool valid_path(const std::string& path) {
    if (path.empty() || path.front() != '/') return false;
    return std::all_of(path.begin(), path.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
               c == '~' || c == '/';
    });
}

// A path of a route: '/' and one or more segments, none empty, none "." or "..", each of letters, digits and - _ ~ .
bool valid_route_path(const std::string& path) {
    if (path.size() < 2 || path.front() != '/') return false;
    std::string_view rest = std::string_view(path).substr(1);
    for (;;) {
        const std::size_t slash = rest.find('/');
        const std::string_view segment = rest.substr(0, slash);
        if (segment.empty() || segment == "." || segment == "..") return false;
        const bool allowed = std::all_of(segment.begin(), segment.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                   c == '_' || c == '~' || c == '.';
        });
        if (!allowed) return false;
        if (slash == std::string_view::npos) return true;
        rest.remove_prefix(slash + 1);
    }
}

// An empty string when the options are usable, else what is wrong.
std::string check_options(const ListenerOptions& o) {
    using std::chrono::milliseconds;
    if (o.bind_address.empty()) return "bind_address is empty";
    if (!valid_path(o.path)) {
        return "path \"" + o.path + "\" must start with '/' and hold only letters, digits and - _ ~ /";
    }
    for (std::size_t at = 0; at < o.routes.size(); ++at) {
        const Route& route = o.routes[at];
        if (!valid_route_path(route.path)) {
            return "route path \"" + route.path +
                   "\" must start with '/' and hold non-empty segments of letters, digits and - _ ~ . that are not . or ..";
        }
        if (!route.handler) return "the handler of route \"" + route.path + "\" is empty";
        if (route.path == o.path) return "route path \"" + route.path + "\" is the path";
        for (std::size_t before = 0; before < at; ++before) {
            if (o.routes[before].path == route.path) return "route path \"" + route.path + "\" is repeated";
        }
    }
    if (o.max_body_bytes < 1) return "max_body_bytes must be at least 1";
    if (o.worker_threads < 1) return "worker_threads must be at least 1";
    if (o.max_worker_threads < o.worker_threads) return "max_worker_threads must be at least worker_threads";
    if (o.worker_idle_timeout.count() < 1) return "worker_idle_timeout must be at least 1 second";
    if (o.max_queued_connections < 1) return "max_queued_connections must be at least 1";
    if (o.read_timeout <= milliseconds::zero()) return "read_timeout must be positive";
    if (o.body_timeout <= milliseconds::zero()) return "body_timeout must be positive";
    if (o.write_timeout <= milliseconds::zero()) return "write_timeout must be positive";
    if (o.idle_connection_timeout.count() < 1) return "idle_connection_timeout must be at least 1 second";
    if (o.keep_alive_interval <= milliseconds::zero()) return "keep_alive_interval must be positive";
    if (o.disconnect_poll_interval <= milliseconds::zero()) return "disconnect_poll_interval must be positive";
    return {};
}

}  // namespace

struct Listener::Impl {
    Impl(ListenerOptions opts, Handler h) : options(std::move(opts)), handler(std::move(h)) {}

    ~Impl() { stop(); }

    Outcome<std::uint16_t> start();
    void stop() noexcept;
    void receive(const httplib::Request& incoming, httplib::Response& outgoing, const httplib::ContentReader& reader);
    void handle(const httplib::Request& incoming, std::string body, httplib::Response& outgoing,
                const Handler& target);
    void answer_route(const Route& route, const httplib::Request& incoming, httplib::Response& outgoing);
    void write(Response&& response, const httplib::Request& incoming, httplib::Response& outgoing);
    void write_stream(std::shared_ptr<BodyStream> stream, const std::string& content_type,
                      httplib::Response& outgoing);

    const ListenerOptions options;
    Handler handler;

    std::mutex lifecycle_mutex;  // start() and stop()
    bool started = false;
    bool stopped = false;
    std::unique_ptr<httplib::Server> server;
    std::thread serve_thread;
    std::atomic<std::uint16_t> bound_port{0};
    // Set by stop() before it waits for the workers; a body being read stops at its next piece.
    std::atomic<bool> closing{false};

    std::mutex mutex;  // guards the three below
    bool stopping = false;
    std::unordered_set<Exchange*> pending;
    std::set<std::shared_ptr<BodyStream>> streams;
};

Outcome<std::uint16_t> Listener::Impl::start() {
    std::lock_guard lifecycle(lifecycle_mutex);
    if (stopped) return Outcome<std::uint16_t>::failure("the listener was stopped");
    if (started) return Outcome<std::uint16_t>::failure("the listener already started");
    if (std::string problem = check_options(options); !problem.empty()) {
        return Outcome<std::uint16_t>::failure(std::move(problem));
    }
    if (!handler) return Outcome<std::uint16_t>::failure("the handler is empty");

    auto candidate = std::make_unique<DeadlineServer>(closing);
    candidate->set_payload_max_length(options.max_body_bytes);
    candidate->set_read_timeout(options.read_timeout);
    candidate->set_write_timeout(options.write_timeout);
    candidate->set_keep_alive_timeout(options.idle_connection_timeout);
    candidate->set_tcp_nodelay(true);
    // The library's default shares a port between listeners (SO_REUSEPORT where it exists); a port in use must be an
    // error here.
    candidate->set_socket_options([](auto sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    const std::size_t base = options.worker_threads;
    const std::size_t maximum = options.max_worker_threads;
    const std::size_t queued = options.max_queued_connections;
    const auto idle = static_cast<time_t>(options.worker_idle_timeout.count());
    candidate->new_task_queue = [base, maximum, queued, idle]() -> httplib::TaskQueue* {
        return new BoundedPool(base, maximum, queued, idle);
    };

    // The library reads a body it routes to an ordinary handler before the handler runs, with a bound on each read
    // only, and the deadline cannot be set for it. So the methods that take a body are routed to a handler that reads
    // it itself (receive), under the deadline, and a request of another method (GET, HEAD, OPTIONS) that frames a body
    // is refused before that body is read.
    candidate->set_pre_request_handler([](const httplib::Request& incoming, httplib::Response& outgoing) {
        if (takes_body(incoming.method) || !frames_body(incoming)) return httplib::Server::HandlerResponse::Unhandled;
        outgoing.status = bad_request_status;
        outgoing.set_header(std::string(connection_field), std::string(close_token));
        return httplib::Server::HandlerResponse::Handled;
    });
    const httplib::Server::Handler route = [this](const httplib::Request& incoming, httplib::Response& outgoing) {
        handle(incoming, std::string(), outgoing, handler);
    };
    const httplib::Server::HandlerWithContentReader reading =
        [this](const httplib::Request& incoming, httplib::Response& outgoing, const httplib::ContentReader& reader) {
            receive(incoming, outgoing, reader);
        };
    candidate->Get(options.path, route);  // HEAD is routed with GET
    candidate->Options(options.path, route);
    candidate->Post(options.path, reading);
    candidate->Put(options.path, reading);
    candidate->Delete(options.path, reading);
    candidate->Patch(options.path, reading);

    // The routes are answered before the library's own routing (a route is never one of its patterns: the library
    // reads a pattern with a '.' in it as an expression, and a path here is compared as the text it is).
    candidate->set_pre_routing_handler([this](const httplib::Request& incoming, httplib::Response& outgoing) {
        const Route* extra = find_route(options, incoming.path);
        if (extra == nullptr) return httplib::Server::HandlerResponse::Unhandled;
        answer_route(*extra, incoming, outgoing);
        return httplib::Server::HandlerResponse::Handled;
    });

    int port = 0;
    if (options.port == 0) {
        port = candidate->bind_to_any_port(options.bind_address);
    } else if (candidate->bind_to_port(options.bind_address, options.port)) {
        port = options.port;
    } else {
        port = -1;
    }
    if (port <= 0) {
        return Outcome<std::uint16_t>::failure("cannot listen on " + options.bind_address + ":" +
                                               std::to_string(options.port) +
                                               " (the port may be in use, or the address unavailable)");
    }

    server = std::move(candidate);
    try {
        serve_thread = std::thread([this] { server->listen_after_bind(); });
    } catch (const std::system_error& error) {
        server->stop();
        server.reset();
        return Outcome<std::uint16_t>::failure(std::string("cannot start the listener thread: ") + error.what());
    }
    server->wait_until_ready();
    started = true;
    bound_port = static_cast<std::uint16_t>(port);
    return Outcome<std::uint16_t>::success(static_cast<std::uint16_t>(port));
}

void Listener::Impl::stop() noexcept {
    std::lock_guard lifecycle(lifecycle_mutex);
    if (stopped) return;
    stopped = true;
    if (!started) return;
    closing = true;

    std::vector<Exchange*> waiting;
    std::vector<std::shared_ptr<BodyStream>> open;
    {
        std::lock_guard lock(mutex);
        stopping = true;
        waiting.assign(pending.begin(), pending.end());
        open.assign(streams.begin(), streams.end());
        // Settle the waits under the lock: an Exchange leaves `pending` under the same lock, after it was settled or
        // while it is not yet waited on, so the pointers are valid here.
        for (Exchange* exchange : waiting) exchange->deliver(make_response(unavailable_status));
    }
    // A stream its producer has ended (an answer, the graceful end of a subscription) is given until the write timeout
    // to be written out; every other one is cut.
    const auto deadline = std::chrono::steady_clock::now() + options.write_timeout;
    for (const auto& stream : open) stream->wait_finished(deadline);
    for (const auto& stream : open) stream->close();
    server->stop();
    if (serve_thread.joinable()) serve_thread.join();
    bound_port = 0;
}

void Listener::Impl::receive(const httplib::Request& incoming, httplib::Response& outgoing,
                             const httplib::ContentReader& reader) {
    using clock = std::chrono::steady_clock;
    // The time runs from the end of the header fields, which the library read within one read timeout.
    const clock::time_point deadline = clock::now() + options.body_timeout;
    // Every read of the body, its framing included, waits no longer than what is left of that time. The stream is
    // always there (DeadlineServer serves every connection); without it the deadline could not hold, so the request
    // is refused.
    httplib::Stream* const stream = serving_stream;
    if (stream == nullptr) {
        outgoing.status = internal_error_status;
        outgoing.set_header(std::string(connection_field), std::string(close_token));
        return;
    }
    enum class Cut { none, late, too_large, stopping };
    Cut cut = Cut::none;
    std::string body;
    bool complete = false;
    {
        // At least one millisecond: zero means no limit.
        const auto limit = std::max<std::chrono::milliseconds::rep>(options.body_timeout.count(), 1);
        stream->set_max_timeout(static_cast<time_t>(limit));
        // Cleared once the body is read, whatever ended it: the rest of the exchange has the library's own limits.
        struct Unlimit {
            httplib::Stream& limited;
            ~Unlimit() { limited.set_max_timeout(0); }
        } unlimit{*stream};
        complete = reader([&](const char* data, std::size_t size) {
            // stop() joins the workers: a body that is still arriving does not keep it waiting.
            if (closing.load()) {
                cut = Cut::stopping;
                return false;
            }
            // Looked at for every piece, the last one included: a body that is not complete by the deadline is
            // refused however much of it came in time.
            if (clock::now() >= deadline) {
                cut = Cut::late;
                return false;
            }
            // The library bounds the body as well; this keeps the bound where the bytes are kept. body.size() never
            // exceeds max_body_bytes, so the difference does not wrap.
            if (size > options.max_body_bytes - body.size()) {
                cut = Cut::too_large;
                return false;
            }
            body.append(data, size);
            return true;
        });
    }
    if (!complete) {
        // The handler is not called. An error status closes the connection after the answer (the rest of the body
        // cannot be told from a next request); the field says so to the peer as well.
        switch (cut) {
            case Cut::late: outgoing.status = request_timeout_status; break;
            case Cut::too_large: outgoing.status = content_too_large_status; break;
            case Cut::stopping: outgoing.status = unavailable_status; break;
            case Cut::none:
                if (closing.load()) {
                    outgoing.status = unavailable_status;  // a read the listener's stop interrupted
                } else if (clock::now() >= deadline) {
                    // A read the time limit ended: between pieces of data too, in a chunk line or a trailer field.
                    outgoing.status = request_timeout_status;
                } else if (outgoing.status < lowest_error_status) {
                    // The library's own reason (a body over the bound, a malformed chunk, a read that timed out).
                    outgoing.status = bad_request_status;
                }
                break;
        }
        outgoing.set_header(std::string(connection_field), std::string(close_token));
        return;
    }
    handle(incoming, std::move(body), outgoing, handler);
}

// GET and HEAD go to the route's handler, unless the request frames a body (refused before anything is read, as on the
// main path). Every other method is refused 405: a body it announced is not read, so the connection ends with the
// answer.
void Listener::Impl::answer_route(const Route& route, const httplib::Request& incoming, httplib::Response& outgoing) {
    const bool reading = incoming.method == get_method || incoming.method == head_method;
    if (reading && !frames_body(incoming)) {
        handle(incoming, std::string(), outgoing, route.handler);
        return;
    }
    if (reading) {
        outgoing.status = bad_request_status;
    } else {
        outgoing.status = method_not_allowed_status;
        outgoing.set_header(std::string(allow_field), std::string(route_methods));
    }
    if (takes_body(incoming.method) || frames_body(incoming)) {
        outgoing.set_header(std::string(connection_field), std::string(close_token));
    }
}

void Listener::Impl::handle(const httplib::Request& incoming, std::string body, httplib::Response& outgoing,
                            const Handler& target) {
    // A responder the handler keeps may outlive this call, so the exchange is shared with it.
    const auto exchange = std::make_shared<Exchange>();
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            outgoing.status = unavailable_status;
            return;
        }
        pending.insert(exchange.get());
    }

    Responder::Watch watch;
    try {
        Request request;
        request.method = incoming.method;
        request.path = incoming.path;
        const std::size_t question = incoming.target.find('?');
        if (question != std::string::npos) request.query = incoming.target.substr(question + 1);
        for (const auto& field : incoming.headers) request.headers.add(field.first, field.second);
        request.body = std::move(body);
        if (!incoming.remote_addr.empty()) request.peer_address = incoming.remote_addr;

        // The handler gets the only copy kept here: when it drops it uncompleted, the exchange is answered 500. The
        // watch does not count as a copy.
        Responder responder([exchange](Response&& response) { return exchange->deliver(std::move(response)); });
        watch = responder.watch();
        target(std::move(request), std::move(responder));
    } catch (...) {
        exchange->deliver(make_response(internal_error_status));
    }

    // Wait for the answer, and look at the connection every poll interval meanwhile: a peer that went away before the
    // answer settles the request (the handler is told through the responder, with no lock held here), and an answer
    // that arrives later is dropped.
    std::optional<Response> settled;
    for (;;) {
        settled = exchange->wait_for(options.disconnect_poll_interval);
        if (settled) break;
        if (incoming.is_connection_closed() && watch.report_disconnect()) {
            exchange->deliver(make_response(unavailable_status));  // nobody reads it; it only ends the wait
        }
    }
    Response response = std::move(*settled);
    {
        std::lock_guard lock(mutex);
        pending.erase(exchange.get());
    }
    write(std::move(response), incoming, outgoing);
}

void Listener::Impl::write(Response&& response, const httplib::Request& incoming, httplib::Response& outgoing) {
    outgoing.status = (response.status >= lowest_status && response.status <= highest_status) ? response.status
                                                                                             : internal_error_status;
    std::string content_type;
    for (const Headers::Field& field : response.headers.fields()) {
        if (equal_ignoring_case(field.name, content_type_field)) {
            if (content_type.empty()) content_type = field.value;
        } else if (!equal_ignoring_case(field.name, content_length_field) &&
                   !equal_ignoring_case(field.name, transfer_encoding_field)) {
            outgoing.headers.emplace(field.name, field.value);
        }
    }

    if (response.stream) {
        if (incoming.method == head_method) {
            response.stream->close();  // a HEAD answer has no body; the producer is told nobody reads
            return;
        }
        write_stream(std::move(response.stream), content_type, outgoing);
        return;
    }
    if (content_type.empty() && !response.body.empty()) content_type = std::string(default_content_type);
    if (!content_type.empty() || !response.body.empty()) outgoing.set_content(std::move(response.body), content_type);
}

void Listener::Impl::write_stream(std::shared_ptr<BodyStream> stream, const std::string& content_type,
                                  httplib::Response& outgoing) {
    {
        std::lock_guard lock(mutex);
        if (stopping) {
            stream->close();
            outgoing.status = unavailable_status;
            return;
        }
        streams.insert(stream);
    }

    const bool event_stream = is_event_stream_content_type(content_type);
    if (event_stream) {
        if (!outgoing.has_header(std::string(cache_control_field))) {
            outgoing.set_header(std::string(cache_control_field), std::string(no_store));
        }
        if (!outgoing.has_header(std::string(proxy_buffering_field))) {
            outgoing.set_header(std::string(proxy_buffering_field), std::string(no));
        }
    }

    struct Pull {
        std::shared_ptr<BodyStream> stream;
        bool event_stream;
        std::chrono::milliseconds poll;
        std::chrono::milliseconds keep_alive;
        std::chrono::steady_clock::time_point last_write;
        std::string chunk;
    };
    auto pull = std::make_shared<Pull>(Pull{stream, event_stream, options.disconnect_poll_interval,
                                            options.keep_alive_interval, std::chrono::steady_clock::now(), {}});

    // The worker thread calls this again and again while the response is open (the library pulls). Each call waits for
    // the stream for at most one poll interval, so a disconnect is noticed within about that time, and never returns
    // without having waited, so the library does not spin.
    auto provider = [pull](std::size_t, httplib::DataSink& sink) -> bool {
        using clock = std::chrono::steady_clock;
        auto wait = pull->poll;
        if (pull->event_stream) {
            const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - pull->last_write);
            wait = std::min(wait, std::max(std::chrono::milliseconds::zero(), pull->keep_alive - idle));
        }
        switch (pull->stream->read(pull->chunk, wait)) {
            case BodyStream::Read::data:
                if (!sink.write(pull->chunk.data(), pull->chunk.size())) {
                    pull->stream->close();
                    return false;
                }
                pull->last_write = clock::now();
                return true;
            case BodyStream::Read::end:
                sink.done();
                return true;
            case BodyStream::Read::closed:
                return false;  // the listener is stopping (or the stream was closed): cut the connection
            case BodyStream::Read::timeout:
                break;
        }
        if (!sink.is_writable()) {
            pull->stream->close();  // the peer is gone
            return false;
        }
        if (pull->event_stream &&
            clock::now() - pull->last_write >= pull->keep_alive) {
            const std::string comment = sse_keep_alive();
            if (!sink.write(comment.data(), comment.size())) {
                pull->stream->close();
                return false;
            }
            pull->last_write = clock::now();
        }
        return true;
    };

    // Whatever ends the response (completion, a failed write, a cut connection, a HEAD answer), the stream is closed
    // here. That is a no-op for a stream the consumer read to its end, and reports every other end to the producer.
    auto release = [this, stream](bool) {
        stream->close();
        std::lock_guard lock(mutex);
        streams.erase(stream);
    };

    outgoing.set_chunked_content_provider(content_type.empty() ? std::string(default_content_type) : content_type,
                                          std::move(provider), std::move(release));
}

Listener::Listener(ListenerOptions options, Handler handler)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(handler))) {}

Listener::~Listener() { stop(); }

Outcome<std::uint16_t> Listener::start() { return impl_->start(); }

void Listener::stop() noexcept { impl_->stop(); }

bool Listener::running() const noexcept {
    std::lock_guard lock(impl_->lifecycle_mutex);
    return impl_->started && !impl_->stopped && impl_->server && impl_->server->is_running();
}

std::uint16_t Listener::port() const noexcept { return impl_->bound_port.load(); }

}  // namespace neomcp::http
