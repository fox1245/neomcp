// How a client reaches a server: the Transport interface, and the descriptions of the transports the library offers.
//
// A Transport carries what the client's engine writes (Outgoing) and tells the client what it read (TransportEvents).
// It knows nothing of the protocol beyond its framing: on stdio one message per line, on HTTP one exchange per message
// and the stream the client opens to hear the server. Which messages go out, when, and what an answer means is the
// engine's (neomcp/client/engine.h).
//
// Which transport a client uses is given to Client::open as one of:
// - ChildProcess: the client starts the server as a child process and speaks to it over its standard input and
//   output. Implemented by the target neomcp::client_stdio.
// - HttpEndpoint: the client speaks Streamable HTTP to a URL, or the older transport of events over HTTP where the
//   server serves only that. Implemented by the target neomcp::client_http (built with NEOMCP_HTTP_CLIENT).
// - a std::unique_ptr<Transport>: an in-process pair (neomcp/client/memory_transport.h), or the application's own.
// make_transport() turns a description into a Transport; each is defined by the target that implements it, so a
// program links the target of every description it opens.
//
// Threads. Every method of a Transport is called on the client's own thread. A transport delivers every event through
// the TransportEvents it was started with, on the client's thread (TransportEvents::executor()), and always by posting
// it there: never from inside start(), send() or close(), so that the client is never entered from inside its own call
// into the transport. Its own threads (a reader, a transfer thread, a process reaper) touch only what the transport
// owns.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include "neomcp/client/engine.h"
#include "neomcp/client/result.h"
#include "neomcp/profile.h"

namespace neomcp {

// ---- The interface

// What a transport tells the client. Implemented by the client; every method is called on the client's thread.
class TransportEvents {
public:
    virtual ~TransportEvents() = default;

    // The client's executor: post every event there.
    virtual asio::any_io_executor executor() const noexcept = 0;

    // One message read on a unit: a line on stdio, a JSON body or the data of one event on HTTP. What the client made
    // of it, so that the transport can count what it must bound (stdio counts the lines that are not messages).
    virtual Reception received(UnitId unit, std::string_view message) = 0;

    // Something about a unit's exchange other than a message: the head of an HTTP response, an event id, the server's
    // advice on reconnecting, the end or the loss of the stream, a failed send, the end of the server process.
    virtual void stream_event(UnitId unit, StreamEvent event) = 0;

    // stdio: a new server process runs, after a `restart` output or on the transport's own account.
    virtual void restarted() = 0;
};

// Why a transport is closed.
enum class CloseCause {
    client_closed,  // the application closed the client: end in order (stdio: close the server's input, then wait for
                    // it to exit before ending it; HTTP: end the session where the revision does)
    contact_ended   // contact cannot go on (a handshake that was never answered): end at once, without asking the
                    // server for anything
};

class Transport {
public:
    virtual ~Transport() = default;

    // The transport the profiles' entries are read for.
    virtual profile::Transport kind() const noexcept = 0;

    // When the client asks this transport for the stream of the connection (EngineOptions::connection_stream). A
    // transport that offers such a stream lets the application choose; the default is when the client needs one.
    virtual ConnectionStreamMode connection_stream() const noexcept { return ConnectionStreamMode::when_needed; }

    // Called once, before anything else. The events object outlives the transport. Returns why the transport could not
    // start (a program that cannot be started, a URL it cannot reach), or nothing when it started; the client then
    // fails to open with ClientErrorKind::transport and the message.
    virtual std::optional<std::string> start(TransportEvents& events) = 0;

    // One output of the engine, of any kind but OutgoingKind::close (which the client turns into close()). A message
    // that cannot be delivered is reported as SendFailed on its unit, never thrown.
    virtual void send(Outgoing outgoing) = 0;

    // Ends the transport. Completes, on the client's thread, once nothing of the transport runs any more and no event
    // will be delivered; bounded by the transport's own limits (the grace periods of a child process, the bound of the
    // request that ends an HTTP session). Called at most once; nothing is sent after it.
    virtual asio::awaitable<void> close(CloseCause cause) = 0;
};

// ---- A child process over stdio (neomcp::client_stdio)

// What becomes of the server's standard error.
enum class StderrMode {
    inherit,  // it is the client process's standard error
    capture,  // the client reads it: every line goes to ChildProcess::on_stderr_line, and the last bytes are kept
    discard   // it goes nowhere
};

// The defaults of ChildProcess, as constants (see SessionDefaults in neomcp/session.h for why).
struct ChildProcessDefaults {
    static constexpr std::size_t stderr_tail_bytes = std::size_t{64} * 1024;
    static constexpr std::size_t max_stderr_line_bytes = std::size_t{4} * 1024;
    static constexpr std::size_t max_ignored_stdout_lines = 100;
    static constexpr std::size_t max_ignored_stdout_bytes = std::size_t{64} * 1024;
    static constexpr std::chrono::milliseconds exit_grace{2'000};
    static constexpr std::chrono::milliseconds term_grace{2'000};
};

// The names of the environment variables a child process gets from the client's environment unless
// ChildProcess::inherited names others: a short list per platform, so that what the client's environment holds
// (credentials among it) does not reach a server unless the application passes it.
std::span<const std::string_view> inherited_environment() noexcept;

struct ChildProcess {
    // The program, found through PATH when it names no directory, and its arguments. No shell is involved.
    std::string program;
    std::vector<std::string> arguments;
    // The working directory of the process; empty: the client's.
    std::optional<std::filesystem::path> working_directory;

    // The variables taken from the client's environment, by name; empty: inherited_environment().
    std::optional<std::vector<std::string>> inherited;
    // Variables set for the process, after the inherited ones (a name given in both takes this value).
    std::vector<std::pair<std::string, std::string>> environment;

    StderrMode stderr_mode = StderrMode::inherit;
    // StderrMode::capture: each line the server writes to its standard error, without its terminator and cut at
    // max_stderr_line_bytes, on the client's thread. It must return promptly.
    std::function<void(std::string_view line)> on_stderr_line;
    // StderrMode::capture: the last bytes of standard error kept, for the message of a failure. Default 64 KiB.
    std::size_t stderr_tail_bytes = ChildProcessDefaults::stderr_tail_bytes;
    std::size_t max_stderr_line_bytes = ChildProcessDefaults::max_stderr_line_bytes;

    // Lines on the server's standard output that are not JSON are skipped and reported, up to this many lines and
    // bytes in all; then the transport fails. Defaults 100 lines, 64 KiB.
    std::size_t max_ignored_stdout_lines = ChildProcessDefaults::max_ignored_stdout_lines;
    std::size_t max_ignored_stdout_bytes = ChildProcessDefaults::max_ignored_stdout_bytes;

    // Ending the process: its input is closed; after exit_grace it is asked to terminate; after term_grace it is
    // ended, with every process it started. Defaults 2 s each.
    std::chrono::milliseconds exit_grace = ChildProcessDefaults::exit_grace;
    std::chrono::milliseconds term_grace = ChildProcessDefaults::term_grace;
};

// ---- An HTTP endpoint (neomcp::client_http)

// Gives the requests of an HTTP transport the authorization a server asks for. Declared in neomcp/client/http.h.
class HttpAuthorizer;

// Which proxy the HTTP transport uses.
enum class ProxyMode {
    environment,  // the one the environment names (http_proxy, https_proxy, ALL_PROXY, NO_PROXY), if any
    none,         // none, whatever the environment says
    named         // HttpProxy::url
};

struct HttpProxy {
    ProxyMode mode = ProxyMode::environment;
    std::string url;  // ProxyMode::named only
};

// ConnectionStreamMode (when the client opens the stream of the connection) is declared in neomcp/client/engine.h.

// The defaults of HttpEndpoint, as constants.
struct HttpClientDefaults {
    static constexpr std::size_t max_response_header_bytes = std::size_t{64} * 1024;
    static constexpr std::size_t max_session_id_bytes = 1024;
    static constexpr std::chrono::milliseconds connect_timeout{10'000};
    static constexpr std::chrono::milliseconds stream_idle_timeout{300'000};
    static constexpr std::chrono::milliseconds max_retry_delay{60'000};
    static constexpr std::size_t max_resumptions = 3;
    static constexpr std::size_t max_redirects = 5;
    static constexpr std::chrono::milliseconds endpoint_timeout{10'000};
    static constexpr std::chrono::milliseconds delete_timeout{2'000};
};

struct HttpEndpoint {
    // The endpoint's URL, used exactly as given.
    std::string url;
    // Asked for the authorization of every request and told of every refusal for want of it; null: a request the
    // server refuses so fails with authorization_required.
    std::shared_ptr<HttpAuthorizer> authorizer;
    HttpProxy proxy;
    // A file of certificate authorities to verify the server with, instead of the system's. Verification itself cannot
    // be switched off.
    std::optional<std::filesystem::path> ca_file;
    ConnectionStreamMode connection_stream = ConnectionStreamMode::when_needed;
    // Accept an HTTP library whose name resolution blocks the transfer thread (and every transfer on it). Off: such a
    // library is refused when the transport starts.
    bool allow_blocking_resolver = false;

    // Response headers beyond this many bytes abort the exchange. Default 64 KiB.
    std::size_t max_response_header_bytes = HttpClientDefaults::max_response_header_bytes;
    // A session id longer than this fails the transport. Default 1024.
    std::size_t max_session_id_bytes = HttpClientDefaults::max_session_id_bytes;
    std::chrono::milliseconds connect_timeout = HttpClientDefaults::connect_timeout;
    // A stream that carries no byte for this long is taken as broken. Default 5 min.
    std::chrono::milliseconds stream_idle_timeout = HttpClientDefaults::stream_idle_timeout;
    // A server that advises waiting longer than this before a stream is resumed is not waited for: the request fails
    // stream_lost. Default 60 s.
    std::chrono::milliseconds max_retry_delay = HttpClientDefaults::max_retry_delay;
    // Resumptions of one stream that bring no new event. Default 3.
    std::size_t max_resumptions = HttpClientDefaults::max_resumptions;
    // Redirects followed for one request (only to the same origin, with the method kept). Default 5.
    std::size_t max_redirects = HttpClientDefaults::max_redirects;
    // The older transport: how long the first event, which names where messages go, may take. Default 10 s.
    std::chrono::milliseconds endpoint_timeout = HttpClientDefaults::endpoint_timeout;
    // The request that ends a session when the client closes. Default 2 s.
    std::chrono::milliseconds delete_timeout = HttpClientDefaults::delete_timeout;
};

// ---- Making a transport

// A child process transport; defined by neomcp::client_stdio. Fails (ClientErrorKind::transport, invalid_options)
// for a description that cannot work. The process is started by Transport::start.
Result<std::unique_ptr<Transport>> make_transport(ChildProcess process);
// An HTTP transport; defined by neomcp::client_http. Fails for a URL it cannot use and for options that cannot work.
Result<std::unique_ptr<Transport>> make_transport(HttpEndpoint endpoint);

// Any of the three, for an application that chooses at run time. Opening one through Client::open links the targets of
// both descriptions.
using TransportSpec = std::variant<ChildProcess, HttpEndpoint, std::unique_ptr<Transport>>;

}  // namespace neomcp
