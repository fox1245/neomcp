// The client: what an application calls to speak to an MCP server.
//
//   neomcp::ClientOptions options;
//   options.client_info.name = "my-app";
//   options.client_info.version = "1.0.0";
//   neomcp::ChildProcess server;
//   server.program = "calc-server";
//   auto client = neomcp::Client::open(options, server);
//   if (!client) { /* client.error() */ }
//   auto sum = client->call<int>("add", AddArgs{1, 2});
//   if (!sum) { /* sum.error() */ }
//
// A typed call serializes its arguments with neomcp::to_json, checks the result's structured content against the
// tool's output schema and decodes it with neomcp::from_json (neomcp/describe.h, neomcp/client/calls.h); call_tool()
// takes and gives JSON.
//
// Every operation has two forms: a blocking one under its plain name, which returns Result<T> when the operation has
// ended, and a coroutine under the same name with the prefix `async_`, which an asio coroutine awaits:
//
//   auto sum = co_await client->async_call<int>("add", AddArgs{1, 2});
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
// - Changes of what the server offers are not callbacks: the application takes them from a Subscription (listen()),
//   with next() or async_next(), at its own pace; they are merged while it does not.
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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/awaitable.hpp>

#include "neomcp/client/calls.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/input_handlers.h"
#include "neomcp/client/methods.h"
#include "neomcp/client/options.h"
#include "neomcp/client/result.h"
#include "neomcp/client/subscription.h"
#include "neomcp/client/timers.h"
#include "neomcp/client/transport.h"
#include "neomcp/describe.h"
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
    // returned as sent, with the outcome of its check against the tool's output schema (ToolCall::check), which never
    // fails the call. The tool's definition is the one of the latest list of tools; on a transport that mirrors
    // arguments into headers, a tool the client has not seen listed makes it list the tools first (list_all_tools),
    // and a failure of that list fails the call.
    Result<ToolCall> call_tool(std::string name, json arguments = json::object(), RequestOptions options = {});
    asio::awaitable<Result<ToolCall>> async_call_tool(std::string name, json arguments = json::object(),
                                                      RequestOptions options = {});

    // Calls a tool with arguments of a described type and decodes its result as R:
    //
    //   auto sum = client->call<int>("add", AddArgs{1, 2});
    //   if (sum && !sum->is_error) std::cout << *sum->value;
    //
    // `args` is serialized with neomcp::to_json. A tool the client has not seen listed makes it list the tools first
    // (list_all_tools; a failure of that list fails the call). The result's structured content must fit the tool's
    // output schema: content that fails it is output_invalid, and content that cannot be checked (the schema cannot be
    // compiled within the peer bounds or the budget of compiles, or the tool is not listed) is unverified, never taken
    // as unchecked. A tool that declares no output schema has its content decoded as it came. R is decoded from the
    // whole structured content when it is a described struct or neomcp::json, and from its member "result" when it is
    // a bool, a number, a described enum, or a vector or map of such values (the form in which a server wraps a result
    // that is not an object). A result without structured content, or one that does not decode as R, is
    // output_undecodable; a revision whose tool results have no structured content fails at once with
    // no_structured_results, and nothing is sent. A tool's own failure (`is_error`) is a value, with `value` empty.
    template <class R, class Args>
    Result<TypedCall<R>> call(std::string name, Args args, RequestOptions options = {});
    template <class R, class Args>
    asio::awaitable<Result<TypedCall<R>>> async_call(std::string name, Args args, RequestOptions options = {});

    // ---- Lists
    //
    // One page, from the start or from a cursor of an earlier page (opaque, sent back exactly as it came; a cursor
    // that cannot be is cursor_not_echoable). A page ends the list when its `nextCursor` is absent or null; any string,
    // the empty one too, names a next page. A page that cannot be read (an item, the cursor or the caching hints) is
    // malformed_answer. On a transport that mirrors tool arguments into headers, a tool whose input schema marks its
    // parameters against the rules is left out of the page, and the diagnostics callback is told (ToolDropped); on
    // another transport the marks are ignored. The tools listed become the definitions the client checks results of
    // and mirrors arguments from.
    //
    // The list_all_ forms follow the pages until the last, bounded by ClientOptions::max_list_pages and
    // max_list_items: past either, the call fails (list_pages_exceeded, list_items_exceeded) and gives no partial
    // list. Each page is a request with the call's options (its clock is per page; its stop ends the whole list).

    Result<Page<ToolDefinition>> list_tools(std::optional<std::string> cursor = {}, RequestOptions options = {});
    asio::awaitable<Result<Page<ToolDefinition>>> async_list_tools(std::optional<std::string> cursor = {},
                                                                   RequestOptions options = {});
    Result<Listing<ToolDefinition>> list_all_tools(RequestOptions options = {});
    asio::awaitable<Result<Listing<ToolDefinition>>> async_list_all_tools(RequestOptions options = {});

    Result<Page<ResourceDefinition>> list_resources(std::optional<std::string> cursor = {},
                                                    RequestOptions options = {});
    asio::awaitable<Result<Page<ResourceDefinition>>> async_list_resources(std::optional<std::string> cursor = {},
                                                                           RequestOptions options = {});
    Result<Listing<ResourceDefinition>> list_all_resources(RequestOptions options = {});
    asio::awaitable<Result<Listing<ResourceDefinition>>> async_list_all_resources(RequestOptions options = {});

    Result<Page<ResourceTemplateDefinition>> list_resource_templates(std::optional<std::string> cursor = {},
                                                                     RequestOptions options = {});
    asio::awaitable<Result<Page<ResourceTemplateDefinition>>> async_list_resource_templates(
        std::optional<std::string> cursor = {}, RequestOptions options = {});
    Result<Listing<ResourceTemplateDefinition>> list_all_resource_templates(RequestOptions options = {});
    asio::awaitable<Result<Listing<ResourceTemplateDefinition>>> async_list_all_resource_templates(
        RequestOptions options = {});

    Result<Page<PromptDefinition>> list_prompts(std::optional<std::string> cursor = {}, RequestOptions options = {});
    asio::awaitable<Result<Page<PromptDefinition>>> async_list_prompts(std::optional<std::string> cursor = {},
                                                                       RequestOptions options = {});
    Result<Listing<PromptDefinition>> list_all_prompts(RequestOptions options = {});
    asio::awaitable<Result<Listing<PromptDefinition>>> async_list_all_prompts(RequestOptions options = {});

    // ---- Resources, prompts, completion

    // Reads a resource: its contents and, where the revision has them, its caching hints (ReadResult::cache).
    Result<ReadResult> read_resource(std::string uri, RequestOptions options = {});
    asio::awaitable<Result<ReadResult>> async_read_resource(std::string uri, RequestOptions options = {});

    // Gets a prompt with its arguments (strings by name).
    Result<PromptResult> get_prompt(std::string name, std::map<std::string, std::string> arguments = {},
                                    RequestOptions options = {});
    asio::awaitable<Result<PromptResult>> async_get_prompt(std::string name,
                                                           std::map<std::string, std::string> arguments = {},
                                                           RequestOptions options = {});

    // Asks for suggestions for the argument `argument` of a prompt or a resource template, of which `value` has been
    // typed. `resolved` holds the arguments already chosen; it is sent where the revision's request has a place for
    // it, and left out elsewhere.
    Result<CompletionResult> complete(CompletionRef ref, std::string argument, std::string value,
                                      std::map<std::string, std::string> resolved = {}, RequestOptions options = {});
    asio::awaitable<Result<CompletionResult>> async_complete(CompletionRef ref, std::string argument,
                                                             std::string value,
                                                             std::map<std::string, std::string> resolved = {},
                                                             RequestOptions options = {});

    // ---- Changes

    // Subscribes to the changes `filter` asks for, under whichever way the governing revision tells of them
    // (neomcp/client/subscription.h), and returns once the server acknowledged the subscription (on subscriptions) or
    // the resources it names are watched (on the connection). `options.timeout` bounds the wait for that
    // acknowledgement (and each request that watches a resource); once acknowledged, a subscription has no clock.
    // `options.stop`, or cancelling the awaiter, before then ends the subscription and gives `cancelled`. Fails at once
    // with too_many_subscriptions when ClientOptions::max_subscriptions are open; with the server's error when it
    // refuses the subscription; with subscription_ended when it ends it before acknowledging it; and with the error of
    // the request when one fails otherwise before the acknowledgement (no reopen happens before it).
    Result<Subscription> listen(ChangeFilter filter, RequestOptions options = {});
    asio::awaitable<Result<Subscription>> async_listen(ChangeFilter filter, RequestOptions options = {});

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

    // The tool call of call<R>: the tool's definition known first (listing the tools when it is not), a revision with
    // structured results, and the check of the content. The typed forms decode what these give back.
    Result<ToolCall> checked_call(std::string name, json arguments, RequestOptions options);
    asio::awaitable<Result<ToolCall>> async_checked_call(std::string name, json arguments, RequestOptions options);

    std::shared_ptr<detail::ClientCore> core_;
};

// ---- Typed calls. Each form serializes its arguments before anything waits (the coroutine form in the ordinary
// function that returns it, so that it keeps no reference to the Client or to `args`).

template <class R, class Args>
Result<TypedCall<R>> Client::call(std::string name, Args args, RequestOptions options) {
    static_assert(detail::typed_result_supported<R>(),
                  "neomcp: a typed call decodes a described struct, neomcp::json, a bool, a number, a described enum, "
                  "or a vector or string-keyed map of such values; read text results with call_tool");
    json arguments = neomcp::to_json(args);
    return detail::decode_typed<R>(checked_call(std::move(name), std::move(arguments), std::move(options)));
}

template <class R, class Args>
asio::awaitable<Result<TypedCall<R>>> Client::async_call(std::string name, Args args, RequestOptions options) {
    static_assert(detail::typed_result_supported<R>(),
                  "neomcp: a typed call decodes a described struct, neomcp::json, a bool, a number, a described enum, "
                  "or a vector or string-keyed map of such values; read text results with call_tool");
    json arguments = neomcp::to_json(args);
    return detail::decode_typed_async<R>(async_checked_call(std::move(name), std::move(arguments), std::move(options)));
}

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
