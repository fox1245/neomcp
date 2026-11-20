// The scripted HTTP peer (scripted_http_server.h).
#include "scripted_http_server.h"

#include <atomic>
#include <charconv>
#include <list>
#include <system_error>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/buffers_iterator.hpp>
#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

namespace http_test {

namespace {

constexpr std::string_view line_end = "\r\n";
constexpr std::string_view head_end = "\r\n\r\n";
constexpr std::size_t max_head_bytes = std::size_t{1} << 20;
constexpr std::size_t max_body_bytes = std::size_t{64} << 20;

bool same_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (fold(left[i]) != fold(right[i])) return false;
    }
    return true;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string reason_of(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

// Splits a received head into the request line and its fields.
void parse_head(Request& request) {
    std::string_view rest = request.head;
    bool first = true;
    while (!rest.empty()) {
        const auto end = rest.find(line_end);
        const std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + line_end.size());
        if (first) {
            first = false;
            const auto space = line.find(' ');
            const auto second = line.find(' ', space == std::string_view::npos ? line.size() : space + 1);
            if (space != std::string_view::npos) {
                request.method = std::string(line.substr(0, space));
                request.target = std::string(line.substr(space + 1, second == std::string_view::npos
                                                                         ? std::string_view::npos
                                                                         : second - space - 1));
            }
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        request.fields.emplace_back(std::string(line.substr(0, colon)), std::string(trim(line.substr(colon + 1))));
    }
}

}  // namespace

// ---- Request

std::optional<std::string> Request::field(std::string_view name) const {
    for (const auto& [key, value] : fields) {
        if (same_name(key, name)) return value;
    }
    return std::nullopt;
}

std::vector<std::string> Request::fields_named(std::string_view name) const {
    std::vector<std::string> values;
    for (const auto& [key, value] : fields) {
        if (same_name(key, name)) values.push_back(value);
    }
    return values;
}

bool Request::has_line(std::string_view line) const {
    std::string_view rest = head;
    while (!rest.empty()) {
        const auto end = rest.find(line_end);
        if (rest.substr(0, end) == line) return true;
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end + line_end.size());
    }
    return false;
}

json Request::message() const { return json::parse(body, nullptr, false); }

std::string Request::rpc_method() const {
    const json value = message();
    const auto* members = value.get_ptr<const json::object_t*>();
    if (members == nullptr) return {};
    const auto found = members->find("method");
    if (found == members->end() || !found->second.is_string()) return {};
    return found->second.get<std::string>();
}

json Request::rpc_id() const {
    const json value = message();
    const auto* members = value.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find("id");
    return found == members->end() ? json(nullptr) : found->second;
}

// ---- Gate

void Gate::open() {
    {
        std::lock_guard lock(mutex_);
        open_ = true;
    }
    changed_.notify_all();
}

bool Gate::wait(std::chrono::milliseconds bound) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, bound, [this] { return open_; });
}

bool Gate::is_open() const {
    std::lock_guard lock(mutex_);
    return open_;
}

// ---- Replies

Reply& Reply::send(std::string bytes) {
    steps.emplace_back(Send{std::move(bytes)});
    return *this;
}
Reply& Reply::wait(std::shared_ptr<Gate> gate) {
    steps.emplace_back(Wait{std::move(gate)});
    return *this;
}
Reply& Reply::drop() {
    steps.emplace_back(Drop{});
    return *this;
}
Reply& Reply::pause(std::chrono::milliseconds duration) {
    steps.emplace_back(Pause{duration});
    return *this;
}

std::string head(int status, const Fields& fields, std::optional<std::size_t> content_length) {
    std::string text = "HTTP/1.1 " + std::to_string(status) + " " + reason_of(status) + "\r\n";
    for (const auto& [name, value] : fields) text += name + ": " + value + "\r\n";
    if (content_length) text += "Content-Length: " + std::to_string(*content_length) + "\r\n";
    text += "Connection: close\r\n\r\n";
    return text;
}

Reply raw_reply(int status, const Fields& fields, std::string body) {
    Reply reply;
    reply.send(head(status, fields, body.size()) + body);
    return reply;
}

Reply json_reply(int status, const json& body, Fields fields) {
    fields.emplace_back("Content-Type", "application/json");
    return raw_reply(status, fields, body.dump());
}

Reply empty_reply(int status, Fields fields) { return raw_reply(status, fields, std::string()); }

Reply event_stream_reply(std::vector<std::string> pieces, Fields fields, int status) {
    fields.emplace_back("Content-Type", "text/event-stream");
    Reply reply;
    reply.send(head(status, fields, std::nullopt));
    for (std::string& piece : pieces) reply.send(std::move(piece));
    return reply;
}

json result_for(const Request& request, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", request.rpc_id()}, {"result", std::move(result)}};
}

json error_for(const Request& request, int code, std::string message, json data) {
    json error{{"code", code}, {"message", std::move(message)}};
    if (!data.is_null()) error["data"] = std::move(data);
    return json{{"jsonrpc", "2.0"}, {"id", request.rpc_id()}, {"error", std::move(error)}};
}

// ---- The server

struct ScriptedHttpServer::Impl {
    asio::io_context context;
    asio::ip::tcp::acceptor acceptor{context};
    std::thread accepting;
    std::atomic<bool> stopping{false};

    mutable std::mutex mutex;
    mutable std::condition_variable arrived;
    Script script;
    std::vector<Request> received;
    std::list<std::shared_ptr<asio::ip::tcp::socket>> sockets;
    std::vector<std::shared_ptr<Gate>> gates;
    std::vector<std::thread> connections;

    void accept_loop() {
        while (!stopping.load()) {
            auto socket = std::make_shared<asio::ip::tcp::socket>(context);
            std::error_code error;
            acceptor.accept(*socket, error);
            if (error || stopping.load()) return;
            std::lock_guard lock(mutex);
            sockets.push_back(socket);
            connections.emplace_back([this, socket] { serve(socket); });
        }
    }

    // Ends the connection when serve() returns, by whichever path. The socket is only shut down here and in stop(),
    // never closed, so that the two threads never write the socket object; it is closed when the server goes.
    struct Ending {
        asio::ip::tcp::socket& socket;
        ~Ending() {
            std::error_code error;
            socket.shutdown(asio::ip::tcp::socket::shutdown_both, error);
        }
    };

    void serve(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        const Ending ending{*socket};
        std::error_code error;
        asio::streambuf buffer(max_head_bytes);
        const std::size_t head_size = asio::read_until(*socket, buffer, head_end, error);
        if (error) return;
        Request request;
        {
            const auto data = buffer.data();
            const std::string all(asio::buffers_begin(data), asio::buffers_end(data));
            request.head = all.substr(0, head_size - head_end.size());
            request.body = all.substr(head_size);
        }
        parse_head(request);
        std::size_t length = 0;
        if (const auto text = request.field("Content-Length")) {
            const auto [stop, problem] = std::from_chars(text->data(), text->data() + text->size(), length);
            if (problem != std::errc() || length > max_body_bytes) return;
        }
        while (request.body.size() < length) {
            std::string more(length - request.body.size(), '\0');
            const std::size_t read = socket->read_some(asio::buffer(more), error);
            if (error) return;
            request.body.append(more, 0, read);
        }
        Script current;
        {
            std::lock_guard lock(mutex);
            current = script;
        }
        Reply reply = current ? current(request) : empty_reply(500);
        {
            std::lock_guard lock(mutex);
            received.push_back(request);
            for (const Step& step : reply.steps) {
                if (const auto* wait = std::get_if<Wait>(&step)) gates.push_back(wait->gate);
            }
            if (stopping.load()) {
                for (const auto& gate : gates) gate->open();
            }
        }
        arrived.notify_all();
        for (const Step& step : reply.steps) {
            if (const auto* send = std::get_if<Send>(&step)) {
                asio::write(*socket, asio::buffer(send->bytes), error);
                if (error) return;
            } else if (const auto* wait = std::get_if<Wait>(&step)) {
                while (!wait->gate->wait(std::chrono::milliseconds(100))) {
                    if (stopping.load()) return;
                }
            } else if (const auto* pause = std::get_if<Pause>(&step)) {
                std::unique_lock lock(mutex);
                if (arrived.wait_for(lock, pause->duration, [this] { return stopping.load(); })) return;
            } else {
                break;
            }
        }
    }

    void stop() {
        if (stopping.exchange(true)) return;
        // Wake the accepting thread with a connection of its own.
        {
            std::error_code error;
            asio::io_context waking;
            asio::ip::tcp::socket socket(waking);
            socket.connect(acceptor.local_endpoint(error), error);
        }
        if (accepting.joinable()) accepting.join();
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex);
            for (const auto& gate : gates) gate->open();
            for (const auto& socket : sockets) {
                std::error_code error;
                socket->shutdown(asio::ip::tcp::socket::shutdown_both, error);
            }
            threads.swap(connections);
        }
        for (std::thread& thread : threads) {
            if (thread.joinable()) thread.join();
        }
        std::error_code error;
        acceptor.close(error);
        std::lock_guard lock(mutex);
        sockets.clear();
    }
};

ScriptedHttpServer::ScriptedHttpServer(Script script) : impl_(std::make_shared<Impl>()) {
    impl_->script = std::move(script);
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
    impl_->acceptor.open(endpoint.protocol());
    impl_->acceptor.bind(endpoint);
    impl_->acceptor.listen();
    port_ = impl_->acceptor.local_endpoint().port();
    impl_->accepting = std::thread([impl = impl_.get()] { impl->accept_loop(); });
}

ScriptedHttpServer::~ScriptedHttpServer() { stop(); }

std::string ScriptedHttpServer::url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
}

void ScriptedHttpServer::set_script(Script script) {
    std::lock_guard lock(impl_->mutex);
    impl_->script = std::move(script);
}

std::vector<Request> ScriptedHttpServer::requests() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->received;
}

std::size_t ScriptedHttpServer::count() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->received.size();
}

bool ScriptedHttpServer::wait_for(std::size_t n, std::chrono::milliseconds bound) const {
    std::unique_lock lock(impl_->mutex);
    return impl_->arrived.wait_for(lock, bound, [&] { return impl_->received.size() >= n; });
}

void ScriptedHttpServer::stop() { impl_->stop(); }

}  // namespace http_test
