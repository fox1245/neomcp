// The client: what an application calls to speak to an MCP server.
//
//   neomcp::ClientOptions options;
//   options.client_info.name = "my-app";
//   options.client_info.version = "1.0.0";
//   neomcp::ChildProcess server;
//   server.program = "calc-server";
//   auto client = neomcp::Client::open(options, server);
//   if (!client) { /* client.error() */ }
//   auto sum = client->call_tool("add", {{"a", 1}, {"b", 2}});
//   if (!sum) { /* sum.error() */ }
//
// Every operation has two forms: a blocking one under its plain name, which returns Result<T> when the operation has
// ended, and a coroutine under the same name with the prefix `async_`, which an asio coroutine awaits:
//
//   neomcp::json arguments = {{"a", 1}, {"b", 2}};
//   auto sum = co_await client->async_call_tool("add", std::move(arguments));
//
// (GCC 13 stops with an internal failure on a braced list of JSON values written inside a co_await expression; build
// the value before the co_await, as above.)
//
// Opening finds out which kind of server answers and at which revision, by the probe or the handshake the profiles
// call for (neomcp/client/engine.h), and returns once contact is open; Client::verdict() gives what it found, for a
// later client to start from (ClientOptions::prior). From then on every call is written for the governing revision,
// waits for its answer under a clock of its own, and ends with the answer or a typed error (neomcp/client/result.h).
// An answer that was lost is asked for again once when the method is a read or a list (ClientOptions::resent_methods),
// and never for a tool call, which ends with the error and ClientError::may_have_run set. A stdio server that exits is
// started again at the next request (ClientOptions::max_restarts).
//
// Threads. Each Client owns one thread, which runs an asio::io_context; the protocol's state (the engine, the input
// dispatcher, the transport's events, the timers) is touched there only. The rest of the client may be used from any
// thread:
// - An async_ method may be awaited from any executor. Its work is posted to the client's thread, and the awaiting
//   coroutine resumes on its own executor when the operation ends. Every parameter is taken by value, and the method
//   is an ordinary function that captures the client's shared state before it returns the coroutine, so neither a
//   temporary argument nor the Client object itself needs to outlive the awaitable; a call still awaited when the
//   client closes ends `closed`.
// - A blocking method runs its async_ form on the client's thread and blocks the calling thread until it ends. Called
//   on the client's own thread (from a callback or a coroutine input handler), where it would wait for itself, it
//   returns ClientErrorKind::on_client_thread at once.
// - A call is cancelled through RequestOptions::stop, or by cancelling the coroutine that awaits it (asio's
//   cancellation, as a timeout of asio's awaitable operators does). Either way it is cancelled as its transport
//   prescribes and ends `cancelled`.
// - Callbacks (RequestOptions::on_progress, ClientOptions::on_log and on_diagnostic, a transport's callbacks) run on
//   the client's thread and must return promptly: while one runs, no answer of the server is read. What a callback
//   throws is dropped.
// - Input handlers: plain ones run on a worker pool of the client bounded by max_concurrent_input_handlers; coroutine
//   ones are spawned on the client's thread and must not block it (neomcp/client/input_handlers.h). When stop is
//   requested on a coroutine handler, what it awaits through asio is cancelled as well.
// - The client's thread belongs to the client's shared state and is never joined from itself: when a Client is
//   destroyed on that thread, or the state's last owner (an awaited call) lets it go there, the thread ends on its own
//   once the close has completed.
#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>

#include "neomcp/client/engine.h"
#include "neomcp/client/input_handlers.h"
#include "neomcp/client/methods.h"
#include "neomcp/client/options.h"
#include "neomcp/client/result.h"
#include "neomcp/client/timers.h"
#include "neomcp/client/transport.h"
#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail {
class ClientCore;
}  // namespace detail

// What the server said about itself when contact opened (its discovery result, or the handshake's result), as data.
// None of it changes what the client does: the instructions are for the application to give its model, the info for
// display and logs.
struct ServerDescription {
    // The governing revision, the kind of server and the transport it answered on (the same as the verdict's).
    std::string revision;
    ServerKind kind = ServerKind::per_request;
    profile::Transport transport = profile::Transport::stdio;
    // The server's Implementation, read for the governing revision; empty when it sent none or one that cannot be read
    // (`info_wire` keeps it as sent).
    std::optional<Implementation> info;
    json info_wire = nullptr;
    // Its ServerCapabilities object, as sent.
    json capabilities = json::object();
    std::optional<std::string> instructions;
    // The revisions a discovery result listed, in the server's order; empty when contact opened with a handshake.
    std::optional<std::vector<std::string>> revisions;
};

// One connection to one server. A handle: move-only; a moved-from Client answers every call with `closed`, its
// server() is a default ServerDescription and its verdict() is empty. Destroying a Client closes it, as close() does,
// and then waits for its thread and its running plain input handlers to end; on the client's own thread it starts the
// close and returns at once.
class Client {
public:
    // ---- Opening

    // Creates the client (its thread, the engine, the input dispatcher), starts the transport and opens contact.
    // Fails with invalid_options for options that cannot work, transport when the transport cannot start, and with
    // the kind of the contact failure when contact cannot be made. Returns once contact is open.
    static Result<Client> open(ClientOptions options, std::unique_ptr<Transport> transport);
    // The same with a child process or an HTTP endpoint (make_transport). Defined by neomcp::client_stdio and
    // neomcp::client_http, which a program that uses the form links.
    static Result<Client> open(ClientOptions options, ChildProcess process);
    static Result<Client> open(ClientOptions options, HttpEndpoint endpoint);
    // The same with a transport chosen at run time; links the targets of both descriptions.
    template <std::same_as<TransportSpec> Spec>
    static Result<Client> open(ClientOptions options, Spec transport);

    static asio::awaitable<Result<Client>> async_open(ClientOptions options, std::unique_ptr<Transport> transport);
    static asio::awaitable<Result<Client>> async_open(ClientOptions options, ChildProcess process);
    static asio::awaitable<Result<Client>> async_open(ClientOptions options, HttpEndpoint endpoint);
    template <std::same_as<TransportSpec> Spec>
    static asio::awaitable<Result<Client>> async_open(ClientOptions options, Spec transport);

    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();

    // ---- Closing

    // Ends every pending call `closed`, requests stop on every running input handler (and drops what it returns),
    // stops every clock and closes the transport; returns once the transport is closed. Plain handlers still running
    // are not waited for here (they see their stop token); the destructor waits for them to return, except on the
    // client's own thread. Later calls end `closed` at once. Closing a closed client succeeds at once. On the client's
    // own thread: on_client_thread (await async_close there, or let the Client be destroyed).
    Result<void> close();
    asio::awaitable<void> async_close();

    // ---- Calls

    // Any request of the governing revision, by its method and params (null, or an object): the server's result as it
    // sent it. The client adds what the revision puts on every request; the engine refuses a method the revision has
    // no client request for (unsupported_method). A result that asks for input is answered through the handlers and
    // the request sent again, as for every call.
    Result<json> request(std::string method, json params = nullptr, RequestOptions options = {});
    asio::awaitable<Result<json>> async_request(std::string method, json params = nullptr,
                                                RequestOptions options = {});

    // Asks the server whether it is there; unsupported_method under a revision that has no such request.
    Result<void> ping(RequestOptions options = {});
    asio::awaitable<Result<void>> async_ping(RequestOptions options = {});

    // Calls a tool and returns its result, read for the governing revision: a result with `is_error` is a value (the
    // tool failed), never a ClientError. A result that cannot be read is malformed_answer. The structured content is
    // returned as sent, without a check against the tool's output schema.
    Result<ToolResult> call_tool(std::string name, json arguments = json::object(), RequestOptions options = {});
    asio::awaitable<Result<ToolResult>> async_call_tool(std::string name, json arguments = json::object(),
                                                        RequestOptions options = {});

    // ---- Roots

    // Tells the server that the client's roots changed, where the governing revision has a notification for that and
    // the roots handler was registered with `notify_changes`. False, and nothing is sent, otherwise and when the
    // client is closed. Callable from any thread: it waits for nothing the server does, so on the client's own thread
    // it runs there at once.
    bool roots_changed();

    // ---- What contact found

    // What the server said about itself. A copy: after a restart under a handshake it may differ from an earlier one.
    // Callable from any thread.
    ServerDescription server() const;
    // Which kind of server answered, on which transport, at which revision, for ClientOptions::prior of a later client
    // of the same server. Empty only for a moved-from Client. Callable from any thread.
    std::optional<ContactVerdict> verdict() const;

private:
    explicit Client(std::shared_ptr<detail::ClientCore> core) noexcept;
    // An awaitable that yields `error` at once: async_open of a description that could not be made into a transport
    // (for the targets that define those forms).
    static asio::awaitable<Result<Client>> failed_open(ClientError error);

    std::shared_ptr<detail::ClientCore> core_;
};

// ---- The forms that choose at run time. The forms for a ChildProcess and an HttpEndpoint are defined by the targets
// of those transports, so that a program links only the transports it opens.

template <std::same_as<TransportSpec> Spec>
Result<Client> Client::open(ClientOptions options, Spec transport) {
    return std::visit([&options](auto& alternative) { return open(std::move(options), std::move(alternative)); },
                      transport);
}

template <std::same_as<TransportSpec> Spec>
asio::awaitable<Result<Client>> Client::async_open(ClientOptions options, Spec transport) {
    return std::visit(
        [&options](auto& alternative) { return async_open(std::move(options), std::move(alternative)); }, transport);
}

}  // namespace neomcp
