// The HTTP request and response model, and the handler interface.
//
// Nothing here depends on an HTTP library or on a socket. A listener turns what it receives into a Request and hands
// it, with a Responder, to a Handler; the answer is whatever the handler completes the Responder with. The same
// Handler can therefore be driven by this library's listener, by a test, or by another HTTP server.
#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "neomcp/http/body_stream.h"

namespace neomcp::http {

// True when the two strings are equal after folding ASCII letters to one case. Header field names compare this way.
bool equal_ignoring_case(std::string_view left, std::string_view right) noexcept;

// Header fields in the order they were added. Names compare without regard to ASCII case; values are kept as given.
class Headers {
public:
    struct Field {
        std::string name;
        std::string value;
    };

    Headers() = default;
    Headers(std::initializer_list<Field> fields);

    // Appends a field. A name may occur more than once.
    Headers& add(std::string name, std::string value);

    // Replaces every field of that name by one with the new value, at the position of the first; appends when there
    // is none.
    Headers& set(std::string name, std::string value);

    // Removes every field of that name and returns how many there were.
    std::size_t remove(std::string_view name);

    // The value of the first field of that name. The view stays valid until the headers are changed.
    std::optional<std::string_view> get(std::string_view name) const noexcept;

    // The values of every field of that name, in order. Views as for get().
    std::vector<std::string_view> get_all(std::string_view name) const;

    std::size_t count(std::string_view name) const noexcept;
    bool contains(std::string_view name) const noexcept { return count(name) > 0; }

    const std::vector<Field>& fields() const noexcept { return fields_; }
    std::size_t size() const noexcept { return fields_.size(); }
    bool empty() const noexcept { return fields_.empty(); }

private:
    std::vector<Field> fields_;
};

struct Request {
    std::string method;              // as received, e.g. "POST"
    std::string path;                // the target without its query, percent-decoding applied
    std::string query;               // the raw query without the '?'; empty when there is none
    Headers headers;
    std::string body;                // the whole body, bounded by the listener's body limit
    std::optional<std::string> peer_address;  // the peer's IP address when the listener knows it
};

struct Response {
    int status = 200;
    Headers headers;

    // A complete body. The listener sets Content-Length itself.
    std::string body;

    // When set, the body is this stream and `body` is ignored: the response is sent chunked, as the stream
    // produces bytes, and ends when the stream ends. The stream may still be empty when the response is completed.
    std::shared_ptr<BodyStream> stream;
};

// A response with a complete body and, when `content_type` is not empty, that Content-Type.
Response make_response(int status, std::string_view content_type = {}, std::string body = {});

// The completion side of one request. It may be completed at once, or later from any thread; copies share one
// completion. The first complete() wins and every later one has no effect. When the last copy is destroyed without
// a completion, the request is answered 500 (a handler that fails to answer must not leave the peer waiting).
//
// The peer may go away before the request is completed. A listener that watches its connections (this library's
// does) reports that through a Watch, and the handler learns of it through on_disconnect(): the request is then
// settled, and a completion that arrives later is dropped (complete() returns false).
class Responder {
    struct State;

public:
    // What a listener supplies: receives the response; returns false when it can no longer use it (the request was
    // abandoned, or the listener stopped). Called at most once, on the thread that completes. Must not throw.
    using Sink = std::function<bool(Response&&)>;

    // The listener's side of a responder: it reports a disconnect without counting as a copy, so a handler that drops
    // every copy still has the request answered 500.
    class Watch {
    public:
        Watch() = default;

        // The peer went away. When the request was not completed yet, this settles it (later completions are
        // dropped), runs the callback registered with on_disconnect() once on this thread, and returns true. Returns
        // false when the request was completed, or settled, already; the callback is not called then.
        bool report_disconnect() const;

    private:
        friend class Responder;
        explicit Watch(std::weak_ptr<State> state) : state_(std::move(state)) {}
        std::weak_ptr<State> state_;
    };

    explicit Responder(Sink sink);

    // True when this call completed the request and the response was taken; false when the request was already
    // completed or settled by a disconnect, or the listener could not take the response.
    bool complete(Response response) const;

    // True once any copy completed the request, or a disconnect settled it.
    bool completed() const noexcept;

    // Registers the function told that the peer went away before the request was completed. It runs at most once, on
    // the thread that reports the disconnect (a thread of the listener), with no lock of the listener or of the
    // responder held, so it may call back into the responder. It does not run when the request was completed first.
    // When the disconnect was reported already, it runs at once on the calling thread. A second registration replaces
    // the first when that has not run yet. Must not throw; what it throws is swallowed.
    void on_disconnect(std::function<void()> callback) const;

    // The handle a listener keeps to report a disconnect.
    Watch watch() const;

private:
    std::shared_ptr<State> state_;
};

// Answers one request. Called by the listener on a worker thread; it may complete `responder` before it returns or
// keep it and complete it from another thread. A listener that waits for the completion on the worker thread (this
// library's does) keeps that thread busy until then.
using Handler = std::function<void(Request request, Responder responder)>;

}  // namespace neomcp::http
