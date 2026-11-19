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
// server as a ToolHost) ends registration, and registering or changing a tool after that throws std::logic_error
// (switching it on and off does not; see "Changes while serving" below).
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
// Resources and templates. A resource has a fixed URI, a template describes a family of them:
//
//   server.resource("config://app", "App configuration", [] { return load_text(); }).mime_type("text/plain");
//   server.resource_template<FileParams>("file:///{path}", "A file", [](const FileParams& p) { return read(p.path); });
//
// A resource handler takes nothing or a `Context&`, as a tool handler does, and is a plain function or a coroutine.
// What it returns becomes the contents of the read (see neomcp/server/feature_handler.h): a std::string is text, a
// neomcp::Bytes is a blob, a described struct or neomcp::json is JSON text with the MIME type application/json (unless
// the resource has its own), and neomcp::ResourceContents, a vector of them or a neomcp::ReadResult are as built.
// Throwing neomcp::ResourceNotFound answers the read as a resource that does not exist. The server declares the
// `resources` capability exactly when a resource or a template is registered.
//
// A template's variables are the members of its parameters struct, a described struct of strings, integers and bools
// (std::optional for a variable the URI may lack, which only a `{?query}` variable can). Registration refuses a
// template that cannot be parsed or matched (neomcp/uri_template.h states what is matched and how), a variable with
// no member and a member with no variable, each by name. A read is served by the resource registered at exactly that
// URI if there is one, else by the first registered template that matches the URI and whose variables decode as the
// parameters (a text that is not a number for an integer member is no match). A URI nothing serves is answered as the
// revision prescribes for a resource that does not exist. A matched value comes from the client and may hold any text,
// path separators and `..` included. A handler that uses it to reach a file or another store must check it against
// what it is willing to serve.
//
// Prompts. A prompt is a template of messages that a client offers its user:
//
//   struct ReviewArgs { std::string title; std::optional<std::string> focus; };
//   NEOMCP_DESCRIBE(ReviewArgs, title, "what to review", focus, "what to look at")
//   server.prompt<ReviewArgs>("review", "Review a change", [](const ReviewArgs& a) { return "Review " + a.title; });
//
// Arguments are strings on the wire: Args is a described struct of std::string members (required) and
// std::optional<std::string> members (optional), which is checked at compile time, and the argument list of the
// prompt (name, description, required) is derived from it. A prompt without arguments is registered without a type
// (server.prompt(name, description, handler)). A std::string a handler returns is one message of the user; a
// neomcp::PromptMessage, a vector of them or a neomcp::PromptResult are as built, and the content of a message may be
// text, an image, audio, a resource link or an embedded resource. prompts/get answers an unknown prompt and a
// missing required argument with Invalid params. The server declares the `prompts` capability exactly when a prompt
// is registered.
//
// Completion. A prompt's argument and a template's variable can have a completer, which suggests values for what the
// user has typed so far:
//
//   server.prompt<ReviewArgs>(...).complete("title", [](std::string_view typed) { return std::vector<std::string>{...}; });
//
// completion/complete for a `ref/prompt` or a `ref/resource` (the template's text) runs the completer of the argument
// named, and answers with an empty list when there is none; a reference to a prompt or template the server lacks is
// answered with Invalid params. At most neomcp::max_completion_values values are sent.
//
// Asking the client. A tool, resource, template or prompt handler that needs something only the client can give (a
// form the user fills in, a completion from the client's model, the client's roots; neomcp/input.h) returns
// neomcp::InputRequired instead of its result. Its return type is then std::variant<R, neomcp::InputRequired> for one
// of the results above (or neomcp::InputRequired alone), and the same for a coroutine handler:
//
//   server.tool<DeleteArgs>("delete_folder", "Delete a folder",
//       [](const DeleteArgs& a, neomcp::Context& ctx) -> std::variant<std::string, neomcp::InputRequired> {
//           const auto answer = ctx.input_responses().find("confirm");
//           if (answer == ctx.input_responses().end()) {
//               neomcp::InputRequired ask;
//               ask.requests.emplace("confirm", neomcp::ElicitationForm{"Delete " + a.path + "?", confirm_schema});
//               return ask;
//           }
//           const auto* confirm = std::get_if<neomcp::ElicitationAnswer>(&answer->second);
//           if (confirm == nullptr || confirm->action != neomcp::ElicitationAction::accept) return "Kept " + a.path;
//           remove_folder(a.path);
//           return "Deleted " + a.path;
//       });
//
// The handler is entered again, from its start, with the answers by the keys it chose (Context::input_responses) and
// the state it gave (Context::request_state). It is not resumed: no frame, thread or coroutine of it is kept while
// the client answers. Whatever it did before it asked happens again on every entry unless it guards that with its
// state, so a handler asks before it acts, and decides from the answers and its state what is still missing. Only
// the last round's answers come back; earlier ones a handler keeps in its state. Progress values a handler reports
// are compared across the rounds of one call when the session keeps the call open (a value not above the last one
// sent is dropped).
//
// How the client is asked follows the revision the call is served under. Where the result asks (2026-07-28), the
// client retries the call with the answers, and the state travels sealed: signed with
// ServerOptions::request_state_keys and bound to the call (not encrypted, so it must hold no secret). Where the server
// sends requests (2025-11-25 and earlier), the session sends them on the call's stream and waits at most
// ServerOptions::input_timeout per round and max_input_rounds rounds. Before asking, Context::client_supports tells
// whether the client can take a request; asking for one it cannot take fails the call as the revision prescribes.
//
// Changes while serving. Registration ends when the server serves, but every tool, resource, template and prompt can be
// switched off and on again at any time, from any thread, a handler included:
//
//   const auto beta = server.tool("beta", "A tool offered later", [] { return std::string("beta"); }).enable(false);
//   server.tool("offer_beta", "Offer beta", [beta] { beta.enable(); return std::string("offered"); });
//   server.notify_resource_updated("config://app");  // the content of a resource changed
//
// Something switched off is not listed and is answered as something that does not exist (a call that was admitted
// before still runs). A switch that changes what a list shows tells the clients that the list changed, and
// notify_resource_updated tells the clients that watch the resource. The server declares `listChanged` for the kinds it
// has and `subscribe` for resources, and each session tells its client the way the revision it is served under
// prescribes: notifications of the connection after resources/subscribe (2025-11-25 and earlier), or the subscriptions
// a client opens with subscriptions/listen (2026-07-28). The server tells the sessions on a thread of its own, so the
// thread that reports a change never waits for a client; a session whose client reads slowly drops the notification
// when its output queue is full. When the server shuts down, every open subscription ends gracefully.
//
// Threads. Plain-function handlers run on a pool of worker threads the server owns, bounded by
// ServerOptions::max_concurrent_calls and max_pending_calls. Coroutine handlers run on one thread the server owns,
// which drives an asio::io_context, and are bounded by max_coroutine_calls; a coroutine handler must not block, or
// it holds up every other coroutine handler. A handler may be called on several threads at once. Every handler gets
// a Context (see context.h).
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/change.h"
#include "neomcp/describe.h"
#include "neomcp/feature_host.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/json_schema.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/server/context.h"
#include "neomcp/server/feature_handler.h"
#include "neomcp/server/form_schema.h"
#include "neomcp/server/handler.h"
#include "neomcp/server/inputs.h"
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
    LogLevel default_log_level = SessionDefaults::log_level;
    // Whether results carry the server's Implementation in `_meta`, where the revision defines that key.
    bool include_server_info = SessionDefaults::include_server_info;
    // Caching hints on tools/list and server/discover, where the revision has them. Default: stale at once, private.
    CacheHints tools_cache{};
    // Tools per tools/list page; empty: one page with every tool. Zero is an error.
    std::optional<std::size_t> tools_page_size{};
    // Caching hints on resources/list and resources/templates/list, where the revision has them. Default: stale at
    // once, private.
    CacheHints resources_cache{};
    // Caching hints on resources/read results, where the revision has them, for a resource that sets none of its own
    // (ResourceHandle::cache). Default: stale at once, private.
    CacheHints resource_read_cache{};
    // Resources per resources/list page, and templates per resources/templates/list page; empty: one page with
    // everything. Zero is an error.
    std::optional<std::size_t> resources_page_size{};
    // Caching hints on prompts/list, where the revision has them. Default: stale at once, private.
    CacheHints prompts_cache{};
    // Prompts per prompts/list page; empty: one page with every prompt. Zero is an error.
    std::optional<std::size_t> prompts_page_size{};
    // Input bounds of a session (see SessionOptions): 8 MiB per line, 128 levels of nesting, and 1024 lines waiting
    // to be written before progress and log notifications are dropped.
    std::size_t max_message_bytes = SessionDefaults::max_message_bytes;
    std::size_t max_nesting_depth = SessionDefaults::max_nesting_depth;
    std::size_t max_queued_notifications = SessionDefaults::max_queued_notifications;

    // The bounds of the JSON Schema validator that checks arguments and structured results.
    json_schema::Limits schema_limits{};

    // ---- Asking the client (a handler that returns neomcp::InputRequired)
    // Where the server sends requests and waits: the longest wait for a round's answers, after which the call fails,
    // and the most rounds one call may ask. Both must be positive. Defaults 10 minutes and 10.
    std::chrono::milliseconds input_timeout = std::chrono::minutes(10);
    std::size_t max_input_rounds = SessionDefaults::max_input_rounds;
    // Where the state travels sealed: the keys (each at least min_request_state_key_size bytes; the first seals,
    // every one verifies) and how long a state is accepted. Empty keys: 32 bytes from the operating system's random
    // source, once per process; processes that serve the same clients must share a key given here.
    std::vector<std::string> request_state_keys{};
    std::chrono::seconds request_state_ttl = SessionDefaults::request_state_ttl;
    // The clock of that expiry; empty: the system clock.
    StateClock clock{};
    // Told why a state was refused, for the server's log; the client only learns that it was.
    RequestStateDiagnostic on_request_state_rejected{};

    // ---- Change notifications (see SessionOptions)
    // Where clients open subscriptions: the subscriptions one session keeps open at once. Zero is an error. Default 16.
    std::size_t max_subscriptions = FeedDefaults::max_subscriptions;
    // The resources a connection watches at once, and the resources one subscription names. Zero is an error.
    // Default 256.
    std::size_t max_watched_resources = FeedDefaults::max_watched_resources;
    // Told, with no lock held, after the number of sessions registered for the server's changes changed; read
    // Server::change_watchers() for it. For tests and diagnostics; it must not block.
    std::function<void()> on_change_watchers{};
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
    // Switches the tool on or off (see Server). Unlike the setters it may be called at any time, from any thread.
    ToolHandle enable(bool on = true) const;
    bool enabled() const;

    const std::string& name() const;

private:
    friend class Server;
    ToolHandle(detail::ServerCore* core, std::size_t index) : core_(core), index_(index) {}
    detail::ServerCore* core_;
    std::size_t index_;
};

// A registered resource, for setting its optional members. Every setter returns the handle, so that calls chain, and
// throws std::logic_error once the server serves. A handle is valid as long as its server.
class ResourceHandle {
public:
    ResourceHandle title(std::string text) const;
    ResourceHandle description(std::string text) const;
    // The MIME type of the resource, in the list and on its contents.
    ResourceHandle mime_type(std::string type) const;
    // The size of the content in bytes, before any encoding.
    ResourceHandle size(std::uint64_t bytes) const;
    ResourceHandle annotations(ContentAnnotations value) const;
    ResourceHandle icons(std::vector<Icon> list) const;
    // `_meta` of the resource; an object, else std::invalid_argument.
    ResourceHandle meta(json value) const;
    // The caching hints of this resource's reads, where the revision has them, instead of
    // ServerOptions::resource_read_cache.
    ResourceHandle cache(CacheHints hints) const;
    // Switches the resource on or off (see Server). May be called at any time, from any thread. A read of a URI whose
    // resource is off is served by a template that matches it, if one is on.
    ResourceHandle enable(bool on = true) const;
    bool enabled() const;

    const std::string& uri() const;

private:
    friend class Server;
    ResourceHandle(detail::ServerCore* core, std::size_t index) : core_(core), index_(index) {}
    detail::ServerCore* core_;
    std::size_t index_;
};

// A registered resource template; the setters are those of ResourceHandle, without `size`.
class ResourceTemplateHandle {
public:
    ResourceTemplateHandle title(std::string text) const;
    ResourceTemplateHandle description(std::string text) const;
    // The MIME type of the contents, only when every resource of the template has it.
    ResourceTemplateHandle mime_type(std::string type) const;
    ResourceTemplateHandle annotations(ContentAnnotations value) const;
    ResourceTemplateHandle icons(std::vector<Icon> list) const;
    ResourceTemplateHandle meta(json value) const;
    ResourceTemplateHandle cache(CacheHints hints) const;
    // Switches the template on or off (see Server), which changes the list of templates; its completers are off with
    // it. May be called at any time, from any thread.
    ResourceTemplateHandle enable(bool on = true) const;
    bool enabled() const;

    // Suggests values for the variable `variable` of the template; the handler is as PromptHandle::complete's. A
    // completion refers to the template by its URI template text.
    template <class Handler>
    ResourceTemplateHandle complete(std::string variable, Handler&& handler) const;

    const std::string& uri_template() const;

private:
    friend class Server;
    ResourceTemplateHandle(detail::ServerCore* core, std::size_t index) : core_(core), index_(index) {}
    void add_completer(std::string variable, detail::FeatureBody<CompletionResult> body) const;
    detail::ServerCore* core_;
    std::size_t index_;
};

// A registered prompt, for setting its optional members; it behaves as a ToolHandle does.
class PromptHandle {
public:
    PromptHandle title(std::string text) const;
    PromptHandle icons(std::vector<Icon> list) const;
    // `_meta` of the prompt; an object, else std::invalid_argument.
    PromptHandle meta(json value) const;
    // Switches the prompt on or off (see Server); its completers are off with it. May be called at any time, from any
    // thread.
    PromptHandle enable(bool on = true) const;
    bool enabled() const;

    // Suggests values for the argument `argument` of the prompt (a member of its Args). The handler takes the text
    // typed so far (std::string_view, const std::string& or std::string), optionally the arguments the client has
    // resolved already (const std::map<std::string, std::string>&), and optionally a Context&, and returns a
    // std::vector<std::string> or a neomcp::CompletionResult; it is a plain function or a coroutine. At most
    // neomcp::max_completion_values values are sent. Throws std::invalid_argument for an argument the prompt lacks and
    // one that has a completer already, std::logic_error once the server serves. The server declares the
    // `completions` capability exactly when a completer is registered.
    template <class Handler>
    PromptHandle complete(std::string argument, Handler&& handler) const;

    const std::string& name() const;

private:
    friend class Server;
    PromptHandle(detail::ServerCore* core, std::size_t index) : core_(core), index_(index) {}
    void add_completer(std::string argument, detail::FeatureBody<CompletionResult> body) const;
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
    //
    // A member described with neomcp::mirror_in_header(name) is marked in the input schema for the revisions that let
    // a client mirror a parameter into an HTTP header: for each of them the schema keyword the revision defines is
    // written on the member's property, and a call over Streamable HTTP must then carry the header that mirrors it
    // (see neomcp/argument_headers.h). The marks have to obey the rules of those revisions, else std::invalid_argument
    // names the tool, the place and the rule. Only a struct described with a Described specialisation can carry
    // options; NEOMCP_DESCRIBE cannot.
    template <class Args, class Handler>
    ToolHandle tool(std::string name, std::string description, Handler&& handler);

    // A tool without parameters: the input schema is {"type":"object","additionalProperties":false}.
    template <class Handler>
    ToolHandle tool(std::string name, std::string description, Handler&& handler);

    // A tool with a definition written by hand; the handler takes the arguments object and returns a ToolResult.
    // The input schema must be an object schema the validator accepts, and so must the output schema when there is
    // one; std::invalid_argument otherwise. The schema may mark integer, string and boolean parameters for HTTP
    // headers with the keyword the revision defines (header_parameters in neomcp/argument_headers.h states the rules);
    // for a server that serves a revision that defines it, a mark that breaks a rule is std::invalid_argument too.
    template <class Handler>
    ToolHandle tool(ToolDefinition definition, Handler&& handler);

    // A session on this server, writing through `sink`; several may be open at once and share the tools and the
    // threads. The first call ends registration. The session's options come from the server's. `on_idle`, when given,
    // is told whenever the session becomes idle (see IdleCallback in neomcp/session.h).
    std::variant<std::unique_ptr<Session>, CreateError> connect(Sink sink, IdleCallback on_idle = {});
    // The same for a transport that feeds input units (see Session::receive): the session serves `transport` and
    // writes every output whole, tagged with the unit it belongs to, through `sink`.
    std::variant<std::unique_ptr<Session>, CreateError> connect_tagged(profile::Transport transport, OutputSink sink,
                                                                       IdleCallback on_idle = {});

    // ToolHost. The first use ends registration.
    std::span<const ToolDefinition> tools() const override;
    bool tool_enabled(std::size_t index) const override;
    void call(const CallRequest& request, CallControl control, Completion done) override;
    ChangeSupport change_support() const override;
    ChangeRegistration watch(std::weak_ptr<ChangeListener> listener) override;

    // The content of the resource at `uri` changed: the clients that watch it are told. Thread-safe; it returns at
    // once (the sessions are told on the server's own thread), and a change reported again before the sessions were
    // told of it is told once.
    void notify_resource_updated(std::string uri);
    // The sessions registered for the server's changes now. For tests and diagnostics.
    std::size_t change_watchers() const;

    const ServerOptions& options() const noexcept;

    // A resource at a fixed URI, with a human-readable name. Throws std::invalid_argument for an empty URI or name and
    // a URI registered already, and std::logic_error once the server serves.
    template <class Handler>
    ResourceHandle resource(std::string uri, std::string name, Handler&& handler);

    // A family of resources whose URIs `uri_template` (RFC 6570) describes; Params is a described struct whose
    // members are the template's variables. Throws std::invalid_argument for an empty template or name, a template
    // registered already, one that cannot be parsed or matched, and a variable and a member that do not pair up;
    // std::logic_error once the server serves. Each message names the template. A matched value comes from the client
    // and may hold any text, path separators and `..` included; a handler that uses it to reach a file or another
    // store must check it against what it is willing to serve.
    template <class Params, class Handler>
    ResourceTemplateHandle resource_template(std::string uri_template, std::string name, Handler&& handler);

    // A prompt whose arguments are the described struct Args (see above). Throws std::invalid_argument for an empty or
    // duplicate name and std::logic_error once the server serves; each message names the prompt.
    template <class Args, class Handler>
    PromptHandle prompt(std::string name, std::string description, Handler&& handler);

    // A prompt without arguments.
    template <class Handler>
    PromptHandle prompt(std::string name, std::string description, Handler&& handler);

    // ToolHost: the resources and the prompts, which the first use ends registration for. Null when none is
    // registered.
    ResourceHost* resource_host() noexcept override;
    PromptHost* prompt_host() noexcept override;
    CompletionHost* completion_host() noexcept override;

private:
    ToolHandle add(ToolDefinition definition, detail::Body body, std::vector<HeaderMark> marks = {});
    ResourceHandle add_resource(ResourceDefinition definition, detail::FeatureBody<ReadResult> body);
    PromptHandle add_prompt(PromptDefinition definition, detail::FeatureBody<PromptResult> body);
    ResourceTemplateHandle add_resource_template(ResourceTemplateDefinition definition,
                                                 detail::FeatureBody<ReadResult> body,
                                                 std::vector<detail::TemplateMember> members,
                                                 detail::TemplateCheck check);

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
                   detail::make_body<detail::Input::typed, Args, F>(std::forward<Handler>(handler)),
                   header_marks_of<Args>());
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

template <class Handler>
ResourceHandle Server::resource(std::string uri, std::string name, Handler&& handler) {
    using F = std::decay_t<Handler>;
    if constexpr (!detail::check_feature_handler<detail::ResourceKind, F>()) {
        return ResourceHandle(nullptr, 0);
    } else {
        ResourceDefinition definition;
        definition.uri = std::move(uri);
        definition.name = std::move(name);
        return add_resource(std::move(definition), detail::make_feature_body<detail::ResourceKind, detail::NoArguments, F>(
                                                       std::forward<Handler>(handler)));
    }
}

template <class Params, class Handler>
ResourceTemplateHandle Server::resource_template(std::string uri_template, std::string name, Handler&& handler) {
    using F = std::decay_t<Handler>;
    if constexpr (!described<Params>) {
        static_assert(detail::dependent_false<Params>,
                      "neomcp: server.resource_template<Params>(...) needs Params to be a described struct "
                      "(NEOMCP_DESCRIBE or a neomcp::Described specialisation)");
        return ResourceTemplateHandle(nullptr, 0);
    } else if constexpr (!detail::all_template_members_decodable<Params>()) {
        static_assert(detail::dependent_false<Params>,
                      "neomcp: the members of the Params of server.resource_template<Params>(...) are std::string, "
                      "bool or integers, or std::optional of one of them");
        return ResourceTemplateHandle(nullptr, 0);
    } else if constexpr (!detail::check_feature_handler<detail::TemplateKind<Params>, F>()) {
        return ResourceTemplateHandle(nullptr, 0);
    } else {
        ResourceTemplateDefinition definition;
        definition.uri_template = std::move(uri_template);
        definition.name = std::move(name);
        detail::TemplateCheck check = [](const json& variables) {
            try {
                static_cast<void>(neomcp::from_json<Params>(variables));
                return true;
            } catch (const DecodeError&) {
                return false;
            }
        };
        return add_resource_template(
            std::move(definition),
            detail::make_feature_body<detail::TemplateKind<Params>, Params, F>(std::forward<Handler>(handler)),
            detail::template_members_of<Params>(), std::move(check));
    }
}

template <class Args, class Handler>
PromptHandle Server::prompt(std::string name, std::string description, Handler&& handler) {
    using F = std::decay_t<Handler>;
    if constexpr (!described<Args>) {
        static_assert(detail::dependent_false<Args>,
                      "neomcp: server.prompt<Args>(...) needs Args to be a described struct (NEOMCP_DESCRIBE or a "
                      "neomcp::Described specialisation)");
        return PromptHandle(nullptr, 0);
    } else if constexpr (!detail::all_prompt_arguments_are_strings<Args>()) {
        static_assert(detail::dependent_false<Args>,
                      "neomcp: the arguments of a prompt are strings on the wire: the members of the Args of "
                      "server.prompt<Args>(...) are std::string or std::optional<std::string>");
        return PromptHandle(nullptr, 0);
    } else if constexpr (!detail::check_feature_handler<detail::PromptKind<Args>, F>()) {
        return PromptHandle(nullptr, 0);
    } else {
        PromptDefinition definition;
        definition.name = std::move(name);
        definition.description = std::move(description);
        definition.arguments = detail::prompt_arguments_of<Args>();
        return add_prompt(std::move(definition),
                          detail::make_feature_body<detail::PromptKind<Args>, Args, F>(std::forward<Handler>(handler)));
    }
}

template <class Handler>
PromptHandle Server::prompt(std::string name, std::string description, Handler&& handler) {
    using F = std::decay_t<Handler>;
    if constexpr (!detail::check_feature_handler<detail::PromptNoArgumentsKind, F>()) {
        return PromptHandle(nullptr, 0);
    } else {
        PromptDefinition definition;
        definition.name = std::move(name);
        definition.description = std::move(description);
        return add_prompt(std::move(definition),
                          detail::make_feature_body<detail::PromptNoArgumentsKind, detail::NoArguments, F>(
                              std::forward<Handler>(handler)));
    }
}

template <class Handler>
PromptHandle PromptHandle::complete(std::string argument, Handler&& handler) const {
    using F = std::decay_t<Handler>;
    if constexpr (!detail::check_feature_handler<detail::CompleterKind, F>()) {
        return *this;
    } else {
        add_completer(std::move(argument), detail::make_feature_body<detail::CompleterKind, detail::CompleterInput, F>(
                                               std::forward<Handler>(handler)));
        return *this;
    }
}

template <class Handler>
ResourceTemplateHandle ResourceTemplateHandle::complete(std::string variable, Handler&& handler) const {
    using F = std::decay_t<Handler>;
    if constexpr (!detail::check_feature_handler<detail::CompleterKind, F>()) {
        return *this;
    } else {
        add_completer(std::move(variable), detail::make_feature_body<detail::CompleterKind, detail::CompleterInput, F>(
                                               std::forward<Handler>(handler)));
        return *this;
    }
}

}  // namespace neomcp
