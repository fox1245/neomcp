// The transfer thread of an HTTP transport: one thread that runs every exchange of the transport on one libcurl multi
// handle, and the pieces of HTTP it reads that do not depend on the protocol (header lines, the plan for a body).
//
// Model: libcurl's multi interface in a thread of its own. The thread waits in curl_multi_poll; another thread hands
// it work through a queue of commands (submit, remove, stop) and wakes it with curl_multi_wakeup, which any thread may
// call. Every easy handle is made, driven, removed and freed on the transfer thread alone, so no handle is ever used by
// two threads, and a handle is never removed from inside one of its own callbacks. What the thread reads goes to the
// sink, on the transfer thread, in the order it was read per unit; the sink must not block and must not call back
// into the loop.
//
// The loop knows nothing of the protocol: it posts bytes, bounds them and reports how each exchange ended.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "client_http/url_guard.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/transport.h"

namespace neomcp::detail::http_client {

// ---- libcurl's global state, initialized once for every transport of the process

// Holds libcurl's global state initialized while any holder lives: curl_global_init runs when the first holder is
// made, curl_global_cleanup when the last one goes. Thread-safe.
class CurlGlobal {
public:
    CurlGlobal();
    ~CurlGlobal();
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    // Whether the initialization succeeded.
    bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

// Why a libcurl whose name resolution blocks is refused; empty when it resolves asynchronously, or when the
// application accepted a blocking one. `asynchronous`: whether the library reports an asynchronous resolver.
std::optional<std::string> resolver_refusal(bool asynchronous, bool allow_blocking);
// The same for the libcurl the program runs with.
std::optional<std::string> resolver_refusal(bool allow_blocking);

// ---- header lines

// The line libcurl sends for a header field: "Name: value", or "Name;" for an empty value (libcurl drops a field
// written "Name:" with nothing after the colon instead of sending it empty). A value of spaces and tabs only is sent
// empty: HTTP drops that whitespace around a value anyway. Empty for a name that is not an HTTP token and for a value
// with CR, LF or NUL, which would end the field and start another.
std::optional<std::string> header_line(std::string_view name, std::string_view value);

// Whether `c` may be in the name of a header field (a token character, RFC 9110, section 5.6.2).
bool is_token_char(char c);

// ---- the body of a response

enum class BodyKind {
    json,          // one JSON body, kept whole within the bound
    event_stream,  // an event stream, read event by event
    discard        // read and dropped
};

struct BodyPlan {
    // For ResponseHead::media_type: the lower-case type and subtype. Engaged but empty when the response has a body
    // whose type cannot be read (no Content-Type, an unreadable one, or more than one), so that the engine refuses it
    // as of a type the protocol does not use; disengaged when the response has no type and carries no content.
    std::optional<std::string> media_type;
    BodyKind kind = BodyKind::discard;
};

// What a response's body is, from its status, the values of its Content-Type fields and the length it announced (-1
// when it announced none). JSON is kept with any status (an error answer may come with one); an event stream only with
// a status of success.
BodyPlan plan_body(int status, const std::vector<std::string_view>& content_types, long long content_length);

// ---- the loop

// The HTTP methods the loop makes requests with.
enum class TransferMethod {
    post,    // with the request's body
    get,     // without a body
    remove   // DELETE, without a body
};

// Which redirects an exchange follows (always 307 and 308 only, with the method and the body kept, at most
// TransferLimits::max_redirects times).
enum class RedirectRule {
    endpoint_origin,  // those the guard admits as a redirect: the endpoint's origin
    request_origin,   // those that keep the origin of the URL the request went to, and that the guard admits as a
                      // document for authorization
    none              // none: a redirect is handed over as the answer
};

// One HTTP request to make.
struct TransferRequest {
    UnitId unit = connection_unit;
    std::string url;
    std::vector<std::string> header_lines;  // in libcurl's form (header_line)
    std::string body;                       // sent as the body of a POST
    TransferMethod method = TransferMethod::post;
    // A bound on the whole exchange, connecting included; zero: none (the client's clocks decide).
    std::chrono::milliseconds total_timeout{0};
    // The exchange starts no earlier than this (the wait before a stream is resumed). A request that waits is ended
    // by remove() and replaced by a later submit() of its unit as a running exchange is.
    std::chrono::steady_clock::time_point not_before{};
    // The event stream of the answer is read with the type of each event: every event that carries data comes as a
    // TransferEvent, whatever its type, and the caller decides what the type means. Otherwise only the events of the
    // default type carry messages (TransferMessage), and the others are dropped.
    bool typed_events = false;
    // The body of the answer is read and dropped whatever its type: only the head and the end of the exchange come.
    bool discard_body = false;
    // The body of the answer is kept whole whatever its type, and comes as one TransferMessage before the end.
    bool whole_body = false;
    // The bound of a message of this exchange; empty: TransferLimits::max_message_bytes.
    std::optional<std::size_t> max_body_bytes;
    RedirectRule redirects = RedirectRule::endpoint_origin;
};

// The head of a response, with the plan for its body.
struct TransferHead {
    int status = 0;
    std::optional<std::string> media_type;
    std::vector<HttpHeader> headers;
};

// One message: a JSON body, or the data of one event of type message. Longer than the bound when the bound was
// exceeded (the exchange was then aborted, and nothing more comes for its unit).
struct TransferMessage {
    std::string text;
};

// One event of a stream read with typed events (TransferRequest::typed_events): its type and its data, which is not
// empty. Longer than the bound when the bound was exceeded, as a TransferMessage is.
struct TransferEvent {
    std::string type;
    std::string data;
};

// Posted by withdraw() once the unit's exchange was ended: every item posted for the unit before it belongs to that
// exchange, and every one after it to a later request of the unit.
struct TransferWithdrawn {};

// The last two are never posted by the loop: a transport queues them itself, so that the client hears of a request it
// holds for authorization in order with the rest, and never from inside a call into the transport.
using TransferItem = std::variant<TransferHead, TransferMessage, TransferEvent, StreamEventId, StreamRetryAdvice,
                                  StreamEnded, StreamLost, SendFailed, TransferWithdrawn, AuthorizationHold,
                                  AuthorizationDenied>;

// Called on the transfer thread for every item of a unit's exchange.
using TransferSink = std::function<void(UnitId unit, TransferItem item)>;

struct TransferLimits {
    std::size_t max_message_bytes = ClientDefaults::max_message_bytes;
    std::size_t max_response_header_bytes = HttpClientDefaults::max_response_header_bytes;
    std::chrono::milliseconds connect_timeout = HttpClientDefaults::connect_timeout;
    // An event stream that has begun (its head arrived) and then carries no byte for this long is ended and reported
    // lost. Until a stream begins nothing bounds the wait here: a request waiting for its answer is bounded by the
    // client's clock of that request alone.
    std::chrono::milliseconds stream_idle_timeout = HttpClientDefaults::stream_idle_timeout;
    std::size_t max_redirects = HttpClientDefaults::max_redirects;
    HttpProxy proxy;
    std::optional<std::filesystem::path> ca_file;
    // The longest the thread waits in one call to curl_multi_poll. libcurl shortens the wait to its own timers and a
    // command wakes the thread at once, so this only bounds a wait that nothing else ends (a test sets it long, so
    // that a command that did not wake the thread shows).
    std::chrono::milliseconds idle_wait{1000};
};

class TransferLoop {
public:
    // `guard` decides which redirects are followed (TransferRequest::redirects).
    TransferLoop(TransferLimits limits, UrlGuard guard, TransferSink sink);
    // Stops the loop (stop()).
    ~TransferLoop();
    TransferLoop(const TransferLoop&) = delete;
    TransferLoop& operator=(const TransferLoop&) = delete;

    // Starts the thread; why it could not, otherwise. Called once.
    std::optional<std::string> start();

    // Any thread. Hands a request to the thread and wakes it. Ignored after stop().
    void submit(TransferRequest request);
    // Any thread. Ends the exchange of a unit (and the one a redirect of it made); nothing more comes for it.
    void remove(UnitId unit);
    // Any thread. Ends the exchange of a unit as remove() does, then posts TransferWithdrawn for the unit, so that what
    // the ended exchange posted before can be told from what a request submitted afterwards on the same unit posts.
    void withdraw(UnitId unit);
    // Any thread but the loop's own. Ends every exchange, stops the thread and waits for it; nothing more reaches the
    // sink afterwards. Idempotent.
    void stop();

    // How many exchanges the thread runs now, for tests; requests that wait for their start are not counted.
    std::size_t running() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::thread thread_;
    std::mutex stop_mutex_;
    bool stopped_ = false;
};

}  // namespace neomcp::detail::http_client
