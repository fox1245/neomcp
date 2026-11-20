// A scripted HTTP peer for tests: a small server on a loopback port the operating system chooses, which records every
// request exactly as its bytes arrived and answers each by the test's script.
//
// A script is a function of the request that returns the reply as steps: bytes to write (the status line and the
// header fields are bytes too, so a reply may hold two Content-Type fields, an oversized header or a body that stops
// half way), a gate to wait at until the test opens it (the test's own clock), a pause of the peer's own (only where a
// test is about bytes that keep coming apart in time), and a drop of the connection. After the last step the connection is closed, so a body without a length ends there.
//
// Each connection is served by a thread of its own, one request per connection. stop() (and the destructor) opens
// every gate, ends every connection and joins every thread.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/json.h"

namespace http_test {

using neomcp::json;

// One request as it arrived.
struct Request {
    std::string method;
    std::string target;
    std::string head;  // the request line and the header lines, exactly as received, up to the blank line
    std::vector<std::pair<std::string, std::string>> fields;  // names as sent; values without surrounding blanks
    std::string body;

    // The value of the first field of that name (compared without regard to case), when there is one.
    std::optional<std::string> field(std::string_view name) const;
    // The values of every field of that name, in order.
    std::vector<std::string> fields_named(std::string_view name) const;
    // Whether the head holds this exact line (without its line end).
    bool has_line(std::string_view line) const;

    // The body read as JSON (discarded when it is none), its JSON-RPC method (empty when none) and id (null when none).
    json message() const;
    std::string rpc_method() const;
    json rpc_id() const;
};

// A point a reply waits at until the test opens it.
class Gate {
public:
    void open();
    // Waits until the gate is open; true unless the bound lapsed first.
    bool wait(std::chrono::milliseconds bound);
    bool is_open() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

struct Send {
    std::string bytes;
};
struct Wait {
    std::shared_ptr<Gate> gate;
};
struct Drop {};  // the connection ends at once, without the rest of the reply
// The peer's own pause before the next step, for a test of how a client treats a stream whose bytes keep coming apart
// in time; tests that need an order of events use a Wait.
struct Pause {
    std::chrono::milliseconds duration{0};
};
using Step = std::variant<Send, Wait, Drop, Pause>;

struct Reply {
    std::vector<Step> steps;

    Reply& send(std::string bytes);
    Reply& wait(std::shared_ptr<Gate> gate);
    Reply& drop();
    Reply& pause(std::chrono::milliseconds duration);
};

using Fields = std::vector<std::pair<std::string, std::string>>;

// The status line and header fields of a reply, with `Connection: close`, and a Content-Length when one is given.
std::string head(int status, const Fields& fields, std::optional<std::size_t> content_length);
// A complete reply: the status, the fields, and the body with its length.
Reply raw_reply(int status, const Fields& fields, std::string body);
// A JSON body with its media type.
Reply json_reply(int status, const json& body, Fields fields = {});
// A reply without a body.
Reply empty_reply(int status, Fields fields = {});
// An event stream: the head, then each piece as its own write; the stream ends when the connection closes.
Reply event_stream_reply(std::vector<std::string> pieces, Fields fields = {}, int status = 200);
// A JSON-RPC result for the request's id.
json result_for(const Request& request, json result);
// A JSON-RPC error for the request's id.
json error_for(const Request& request, int code, std::string message, json data = nullptr);

class ScriptedHttpServer {
public:
    using Script = std::function<Reply(const Request& request)>;

    explicit ScriptedHttpServer(Script script);
    ~ScriptedHttpServer();
    ScriptedHttpServer(const ScriptedHttpServer&) = delete;
    ScriptedHttpServer& operator=(const ScriptedHttpServer&) = delete;

    std::uint16_t port() const noexcept { return port_; }
    // http://127.0.0.1:<port><path>
    std::string url(std::string_view path = "/mcp") const;

    // Replaces the script, for the requests that arrive from now on.
    void set_script(Script script);

    // The requests received so far, in the order they arrived.
    std::vector<Request> requests() const;
    std::size_t count() const;
    // Waits until at least `n` requests arrived; true unless the bound lapsed first.
    bool wait_for(std::size_t n, std::chrono::milliseconds bound = std::chrono::seconds(20)) const;

    void stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::uint16_t port_ = 0;
};

}  // namespace http_test
