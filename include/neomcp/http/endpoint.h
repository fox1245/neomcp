// The Model Context Protocol endpoint on one HTTP path: the Streamable HTTP transport of every enabled revision that
// defines it, both eras on the same path.
//
// The endpoint is a Handler: it takes a Request and completes a Responder, and does no I/O of its own, so any HTTP
// server can carry it (neomcp::HttpServer runs it on this library's Listener). Which way a request goes is decided by
// the revision profiles (their Streamable HTTP entries: the session header, the version header, whether a GET stream
// and a DELETE are offered, the statuses they prescribe) and by the session engine, never by a revision string.
//
// What it does with a request, in order:
//   1. The Host and Origin headers must pass the HostPolicy; otherwise 403.
//   2. POST is served. GET and DELETE are served for a live session whose revision offers them; any other method,
//      and GET or DELETE without such a session, is answered 405 with an Allow header.
//   3. A POST must have the media type application/json (parameters such as a charset do not matter; anything else
//      415), an Accept header that admits application/json or text/event-stream when it has one (406 otherwise), and
//      a body within the server's max_message_bytes (413).
//   4. A POST that names a session (by the session header of any enabled revision) is fed to that session; a session
//      id that names no live session is answered 404. A POST without one gets a fresh session of its own. When its
//      input opens a connection under a revision that has a session header, the session is kept under a new id: 32
//      random bytes from the operating system, in hexadecimal, sent in that header with the answer. Otherwise the
//      session ends with the request.
//   5. The answer: the endpoint waits for the request's first output. When it is the answer, the response is
//      application/json. When something comes first (a progress or log notification of the call), or nothing has come
//      after EndpointOptions::stream_after, the response is an event stream (when the client accepts one): one event
//      per message, ending after the answer. A request that awaits no answer (a notification, a response) is answered
//      202 without a body. A refusal is answered with the status its revision prescribes, or the library's own, and
//      the JSON-RPC error as body.
//   6. The client closing the response stream, or leaving before the answer, abandons the request: the revision's
//      cancellation rule decides whether the call is stopped (a closed stream cancels it in some revisions, not in
//      others), and nothing more is written for it.
//   7. A subscription (subscriptions/listen, where the revision has it) is answered as an event stream at once, its
//      acknowledgement the first event, and stays open (with keep-alive comments) until the client closes it, which
//      ends it, or the server ends it; a client that takes only JSON is answered 406. Each such request is a session
//      of its own, so it holds one worker of the listener for as long as it is open.
//
// A kept session: GET opens its connection stream (the messages that belong to no request go there, one event each,
// such as the change notifications of the connection; at most one such stream at a time, a second GET is answered 409;
// without one they are dropped), DELETE ends it
// (204). It also ends after EndpointOptions::session_idle_timeout without a request in progress or an open stream, and
// when the endpoint stops; later requests that name it are answered 404. Requests of one session may arrive on many
// threads; the endpoint feeds them to the session one at a time.
//
// Threads: the endpoint has one thread of its own, which runs its deadlines. It never holds a lock while it calls
// into a session, a responder or a stream.
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>

#include "neomcp/http/body_stream.h"
#include "neomcp/http/message.h"
#include "neomcp/http/outcome.h"
#include "neomcp/http/policy.h"

namespace neomcp {
class Server;
}  // namespace neomcp

namespace neomcp::http {

struct EndpointOptions {
    // Sessions kept at once; a handshake beyond it is refused with 503. At least 1. Default 1024.
    std::size_t max_sessions = 1024;

    // A kept session without a request in progress and without an open stream for this long is closed. Positive.
    // Default 30 minutes.
    std::chrono::milliseconds session_idle_timeout = std::chrono::minutes(30);

    // A request that has had no output for this long is answered with an event stream (when the client accepts one),
    // so that keep-alive comments can flow while it runs. Positive. Default 5 seconds.
    std::chrono::milliseconds stream_after{5000};

    // The bytes an event stream holds for a reader that is slower than the server (see BodyStream). A notification
    // that does not fit is dropped; an answer that does not fit ends the stream and abandons the request. At least 1.
    std::size_t max_stream_buffer_bytes = BodyStream::default_max_buffered_bytes;
};

class Endpoint {
public:
    // Fails, with a message, when an option is out of range or a session of the server cannot be served on Streamable
    // HTTP (no enabled revision defines the transport, or a mechanism has no implementation). The server must outlive
    // the endpoint. Creating it ends the registration of the server's tools.
    static Outcome<std::unique_ptr<Endpoint>> create(Server& server, HostPolicy policy, EndpointOptions options = {});

    // Calls stop().
    ~Endpoint();
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    // Answers one request (see above). Thread-safe.
    void handle(Request request, Responder responder);

    // A Handler that calls handle(); the endpoint must outlive every use of it.
    Handler handler();

    // Replaces the Host and Origin policy for the requests that arrive from now on (a listener bound to port 0 learns
    // its port only once it serves).
    void set_policy(HostPolicy policy);

    // Closes every session and every stream: requests waiting for an answer are answered 503, an open subscription
    // ends gracefully (its stream gets the end and then ends), the other open streams are cut, and a request that
    // arrives from now on is answered 503. Returns without waiting for anything a client does. Idempotent. Must not be
    // called from inside a handler of the server.
    void stop() noexcept;

    // The sessions kept now.
    std::size_t session_count() const;

private:
    struct Impl;
    explicit Endpoint(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace neomcp::http
