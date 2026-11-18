// A Model Context Protocol server: tools declared from C++ types, run on threads the server owns, and served to any
// number of sessions.
//
//   struct AddArgs { int a; int b; std::optional<std::string> note; };
//   NEOMCP_DESCRIBE(AddArgs, a, "first addend", b, "second addend", note, "a note to echo back")
//
//   neomcp::Server server({.name = "calc", .version = "1.0.0"});
//   server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });
//
// A transport then connects sessions: `server.connect(sink)` returns a Session that reads the client's lines and
// writes the server's through the sink.
//
// Registration. Every tool is registered before the server serves: the first connect() (or the first use of the
// server as a ToolHost) ends registration, and registering or changing a tool after that throws std::logic_error.
// A tool's input schema comes from its arguments type (neomcp::schema_of), and arguments are checked against it
// before the handler runs; a call whose arguments fail it never reaches the handler. The forms a handler can take
// are listed in neomcp/server/handler.h.
//
// Results. What a handler returns becomes the call's result:
//
//   std::string                    one text block
//   a described struct             structuredContent, the type's schema as outputSchema, and the same JSON as text
//   bool, a number, std::vector,   structuredContent {"result": value}, a schema of that object as outputSchema, and
//   std::map<std::string, T>,      the value's JSON as text
//   a described enum
//   neomcp::json                   an object: structuredContent and its JSON as text (no outputSchema); any other
//                                  value: its JSON as text
//   neomcp::ToolResult             as built
//   void                           no content
//
// A structured result is checked against the tool's output schema when the tool has one (unless the result is an
// error); a result that fails it is a defect of the server program and is answered with an internal error. Throwing
// neomcp::ToolError answers the call with a tool execution error carrying its message; any other exception, a
// failed decoding of arguments that passed the schema included, is answered with a tool execution error with a
// fixed message (ServerOptions::expose_error_details adds the exception's text).
//
// Threads. Plain-function handlers run on a pool of worker threads the server owns, bounded by
// ServerOptions::max_concurrent_calls and max_pending_calls. Coroutine handlers run on one thread the server owns,
// which drives an asio::io_context, and are bounded by max_coroutine_calls; a coroutine handler must not block, or
// it holds up every other coroutine handler. A handler may be called on several threads at once. Every handler gets
// a Context (see context.h).
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/describe.h"
#include "neomcp/json.h"
#include "neomcp/json_schema.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/server/context.h"
#include "neomcp/server/handler.h"
#include "neomcp/session.h"
#include "neomcp/tool_host.h"

namespace neomcp {

struct ServerOptions {
    // Who the server is (the members of Implementation), so that `{.name = "calc", .version = "1.0.0"}` works.
    std::string name{};
    std::string version{};
    std::optional<std::string> title{};
    std::optional<std::string> description{};
    std::optional<std::string> website_url{};
    std::vector<Icon> icons{};
    // Sent in the handshake and in server/discover.
    std::optional<std::string> instructions{};

    // The revisions served; empty: every built-in profile.
    std::vector<profile::ProfilePtr> profiles{};

    // Worker threads that run plain-function handlers, and so the plain-function calls that run at once. Zero is an
    // error. Default 4.
    std::size_t max_concurrent_calls = 4;
    // Plain-function calls that may wait for a worker; a call beyond both bounds is refused as busy. Default 64.
    // Coroutine calls are counted by max_coroutine_calls instead.
    std::size_t max_pending_calls = 64;
    // Coroutine calls in flight at once; a call beyond it is refused as busy. A coroutine call holds no thread, but
    // it holds its arguments and its coroutine frames until it ends, so without a bound a client could make the
    // server hold any number of them. A call keeps its place until its coroutine has ended, also after it was
    // cancelled. Zero is an error. Default 64: as many as the plain-function calls that may wait, which is far more
    // than a client keeps in flight, and it bounds what coroutine calls hold to 64 sets of arguments.
    std::size_t max_coroutine_calls = 64;

    // Whether an error answer carries the text of the exception (or of the defect) behind it. Off: tool failures
    // and internal errors have fixed messages, so that nothing about the server's internals reaches the client.
    bool expose_error_details = false;

    // Log messages through the protocol (Context::log). Off by default: the server declares no logging capability.
    bool protocol_logging = false;
    // The level in force on a connection before the client sets one.
    LogLevel default_log_level = SessionOptions{}.default_log_level;
    // Whether results carry the server's Implementation in `_meta`, where the revision defines that key.
    bool include_server_info = SessionOptions{}.include_server_info;
    // Caching hints on tools/list and server/discover, where the revision has them. Default: stale at once, private.
    CacheHints tools_cache{};
    // Tools per tools/list page; empty: one page with every tool. Zero is an error.
    std::optional<std::size_t> tools_page_size{};
    // Input bounds of a session (see SessionOptions): 8 MiB per line, 128 levels of nesting, and 1024 lines waiting
    // to be written before progress and log notifications are dropped.
    std::size_t max_message_bytes = SessionOptions{}.max_message_bytes;
    std::size_t max_nesting_depth = SessionOptions{}.max_nesting_depth;
    std::size_t max_queued_notifications = SessionOptions{}.max_queued_notifications;

    // The bounds of the JSON Schema validator that checks arguments and structured results.
    json_schema::Limits schema_limits{};
};

class Server;

// A registered tool, for setting its optional members. Every setter returns the handle, so that calls chain, and
// throws std::logic_error once the server serves. A handle is valid as long as its server.
class ToolHandle {
public:
    // The display name, as `title` and as the annotation `title` (each where the revision has it).
    ToolHandle title(std::string text) const;
    // Annotation hints.
    ToolHandle read_only(bool value = true) const;
    ToolHandle destructive(bool value = true) const;
    ToolHandle idempotent(bool value = true) const;
    ToolHandle open_world(bool value = true) const;
    // `_meta` of the tool; an object, else std::invalid_argument.
    ToolHandle meta(json value) const;
    ToolHandle icons(std::vector<Icon> list) const;

    const std::string& name() const;

private:
    friend class Server;
    ToolHandle(detail::ServerCore* core, std::size_t index) : core_(core), index_(index) {}
    detail::ServerCore* core_;
    std::size_t index_;
};

class Server final : public ToolHost {
public:
    // Starts the server's threads. Throws std::invalid_argument for options that cannot work.
    explicit Server(ServerOptions options);
    // Requests stop on every call in flight, answers the calls still waiting for a worker with an internal error,
    // lets the running handlers finish, and joins the server's threads; no handler runs after it returned. It must
    // not run on a thread of the server (inside a handler). A session may outlive the server: it then answers tool
    // calls with an internal error.
    ~Server() override;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // A tool whose arguments are the described struct Args: the input schema is schema_of<Args>() and arguments are
    // decoded with from_json<Args>. Throws std::invalid_argument for an empty or duplicate name and std::logic_error
    // once the server serves; each message names the tool.
    template <class Args, class Handler>
    ToolHandle tool(std::string name, std::string description, Handler&& handler);

    // A tool without parameters: the input schema is {"type":"object","additionalProperties":false}.
    template <class Handler>
    ToolHandle tool(std::string name, std::string description, Handler&& handler);

    // A tool with a definition written by hand; the handler takes the arguments object and returns a ToolResult.
    // The input schema must be an object schema the validator accepts, and so must the output schema when there is
    // one; std::invalid_argument otherwise.
    template <class Handler>
    ToolHandle tool(ToolDefinition definition, Handler&& handler);

    // A session on this server, writing through `sink`; several may be open at once and share the tools and the
    // threads. The first call ends registration. The session's options come from the server's.
    std::variant<std::unique_ptr<Session>, CreateError> connect(Sink sink);

    // ToolHost. The first use ends registration.
    std::span<const ToolDefinition> tools() const override;
    void call(const CallRequest& request, CallControl control, Completion done) override;

    const ServerOptions& options() const noexcept;

private:
    ToolHandle add(ToolDefinition definition, detail::Body body);

    std::shared_ptr<detail::ServerCore> core_;
};

// ---------------------------------------------------------------------------------------------------------------

template <class Args, class Handler>
ToolHandle Server::tool(std::string name, std::string description, Handler&& handler) {
    using F = std::decay_t<Handler>;
    if constexpr (!described<Args>) {
        static_assert(detail::dependent_false<Args>,
                      "neomcp: server.tool<Args>(...) needs Args to be a described struct (NEOMCP_DESCRIBE or a "
                      "neomcp::Described specialisation)");
        return ToolHandle(nullptr, 0);
    } else if constexpr (!detail::check_handler<detail::Input::typed, Args, F>()) {
        return ToolHandle(nullptr, 0);
    } else {
        ToolDefinition definition;
        definition.name = std::move(name);
        definition.description = std::move(description);
        definition.input_schema = schema_of<Args>();
        definition.output_schema = detail::output_schema_of<detail::handler_value_t<detail::Input::typed, F>>();
        return add(std::move(definition),
                   detail::make_body<detail::Input::typed, Args, F>(std::forward<Handler>(handler)));
    }
}

template <class Handler>
ToolHandle Server::tool(std::string name, std::string description, Handler&& handler) {
    using F = std::decay_t<Handler>;
    if constexpr (!detail::check_handler<detail::Input::none, detail::NoArguments, F>()) {
        return ToolHandle(nullptr, 0);
    } else {
        ToolDefinition definition;
        definition.name = std::move(name);
        definition.description = std::move(description);
        definition.input_schema = detail::empty_object_schema();
        definition.output_schema = detail::output_schema_of<detail::handler_value_t<detail::Input::none, F>>();
        return add(std::move(definition),
                   detail::make_body<detail::Input::none, detail::NoArguments, F>(std::forward<Handler>(handler)));
    }
}

template <class Handler>
ToolHandle Server::tool(ToolDefinition definition, Handler&& handler) {
    using F = std::decay_t<Handler>;
    if constexpr (!detail::check_handler<detail::Input::untyped, json, F>()) {
        return ToolHandle(nullptr, 0);
    } else {
        return add(std::move(definition),
                   detail::make_body<detail::Input::untyped, json, F>(std::forward<Handler>(handler)));
    }
}

}  // namespace neomcp
