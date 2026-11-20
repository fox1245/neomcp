// A test-only server of the older transport of events over HTTP (the revision 2024-11-05), over the library's session
// engine and HTTP listener, on a loopback port the operating system chooses.
//
// One path is served. A GET opens an event stream and a session of the server for it (a session for the transport
// http_sse); the stream's first event names where the client posts its messages: the same path with a query that
// names the stream. A POST with that query is one message of the client for the stream's session, answered 202; the
// session's messages go out on the stream as events. A POST without such a query (such as a Streamable HTTP request)
// is answered 405, and one that names no open stream 404.
//
// For a client that meets a server that does not keep to the transport, the options make it misbehave: a first event
// of another type, an endpoint of another origin or with user information (any text as the endpoint event's data), no
// endpoint event at all; drop_streams() ends every open stream, as a server that goes away does.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "neomcp/server.h"

namespace http_sse_test {

struct HttpSseServerOptions {
    // The one path served.
    std::string path = "/sse/stream";
    // The data of the endpoint event, made from the query that names the stream (such as "stream=1"); empty: the
    // path and that query (an absolute-path reference).
    std::function<std::string(const std::string& query)> endpoint_reference;
    // The types of the two events; empty: those the built-in profile of 2024-11-05 names.
    std::string endpoint_event;
    std::string message_event;
    // The type the stream's first event goes out under; empty: endpoint_event.
    std::string first_event;
    // Whether the stream begins with the endpoint event at all.
    bool send_endpoint = true;
};

class HttpSseServer {
public:
    // The server must outlive this object. Throws std::runtime_error when the listener cannot start.
    explicit HttpSseServer(neomcp::Server& server, HttpSseServerOptions options = {});
    ~HttpSseServer();
    HttpSseServer(const HttpSseServer&) = delete;
    HttpSseServer& operator=(const HttpSseServer&) = delete;

    std::uint16_t port() const noexcept;
    // http://127.0.0.1:<port>
    std::string origin() const;
    // origin() and the path served.
    std::string url() const;

    // The streams opened so far (GETs answered with a stream).
    std::size_t streams() const;
    // Waits until at least `n` streams were opened; true unless the bound lapsed first.
    bool wait_for_streams(std::size_t n, std::chrono::milliseconds bound = std::chrono::seconds(20)) const;
    // The JSON-RPC methods of the messages posted to open streams, in order (an answer of the client has none and is
    // left out).
    std::vector<std::string> posted_methods() const;
    // How many of them were `method`.
    std::size_t posted(const std::string& method) const;
    // The POSTs answered 405 because they named no stream (such as the requests of Streamable HTTP).
    std::size_t refused_posts() const;

    // Ends every open stream and closes its session: the client's GET ends in order.
    void drop_streams();

    // Stops the listener and closes every session. Idempotent.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace http_sse_test
