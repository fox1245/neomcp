# Changelog

Versions follow the rule of the package configuration: before 1.0, a release with a new minor number may change the
interface, and a new patch number does not.

## Unreleased

### Server

- HTTP event-stream buffering (`http::BodyStream`, bounded by `max_stream_buffer_bytes`): pending list-change
  notifications (`tools`, `prompts`, `resources`) are no longer dropped when the stream buffer is full. They are
  coalesced per kind in FIFO order, block later ordinary appends from overtaking them, and close the stream fail-closed
  if a single uncoalesced list change exceeds `max_buffered_bytes`.

### Client

- Tasks of revision 2025-11-25 (`augmented_request`): parses and serializes `Tool.execution.taskSupport` (`forbidden`,
  `optional`, `required`) on `ToolDefinition`, gates on server `capabilities.tasks` and `requests.tools.call`, lists
  tools once when needed to check `taskSupport`, requests tasks per call for `optional` and `required` tools via
  `params.task`, polls with `tasks/get`, fetches terminal or interactive `input_required` streams via `tasks/result`,
  rejects unexpected task replies on unaugmented calls, preserves progress callbacks across task steps, and maps
  `tasks/cancel` `-32602` to `server_error` while keeping `tasks/get` `-32602` as `task_lost`.

## 0.3.0

The interface changed in places (see "Changed"); a project built against 0.2 is rebuilt against 0.3.

### Server

- Change notifications after a burst of changes, on every revision: a list change (of tools, prompts or resources)
  is never dropped for a full output queue, and is written before the resource updates waiting with it, never before
  an answer or an earlier list change, nor before its subscription's acknowledgement. A change waiting to be written,
  in the server's feed or in a session's queue, stands for every later change of the same list or of the same
  resource on the same session or subscription, so a burst of changes to one of them is told once. The server's
  feed keeps one pending update per resource and drops none; updates of distinct resources are still dropped beyond
  a session's `max_queued_notifications`, for that session. An update that waits for a later one of the same resource
  is written with what reading the resource needs under the current watch.
- Authorization: a resource subscription of a session (2025-03-26 to 2025-11-25) lasts while the session's requests
  may read the resource. A later request of the session, a POST or the GET that opens its stream, whose token (of the
  same principal) lacks a scope reading a subscribed resource needs ends that subscription before the request is
  served, and an update of it not yet written is dropped; the client subscribes again with a token that holds the
  scopes. The stream of a session carries only the updates its own token may read. With fixed tokens, the stream a
  replacement ended and the client opens again counts as such a request.
- `Session::present` takes the authorization of a request that belongs to the connection but brings the session no
  input (the GET that opens a session's stream), with the same effect on subscriptions as a unit that comes with it;
  `Output::read_scopes` names, on an update notification of the connection, the scopes reading the resource needs.
- A request whose version header names 2026-07-28, which has no sessions, is served on its own whatever session header
  it carries: the header does not route it to a session (a live one included), and its answer carries none. A GET or
  DELETE of that revision is answered 405. The revisions that keep sessions still route by the header.

### Client

- The check of the restricted form of an elicitation compares each property's `type` with the values its revision's
  profile lists for the shape (`type_values`), and the objects a property holds (the items of a choice of several) with
  their types, as it compared member names before. A property whose `type` is none of the values, or whose members
  are those of one shape and whose `type` is another's, is outside the form and is refused like any other property
  that does not fit.
- `ClientOptions::state_retry_first_delay` and `state_retry_max_delay` may not exceed
  `ClientDefaults::state_retry_delay_limit` (1 hour); `Client::open` fails with `invalid_options` for more.
- Older transport of events over HTTP: an endpoint event longer than `max_message_bytes` ends the connection with
  that reason (it was reported as a first event of another type).

### Sampling with tools

- Sampling messages and results carry tool uses and tool results (`neomcp::ToolUseContent`,
  `neomcp::ToolResultContent`) under the revisions whose sampling defines them (2025-11-25 and 2026-07-28): a server
  offers tools, reads the model's tool uses, and asks again with the history and a result for each; a client's
  sampling handler receives them typed and answers with tool uses. The members written and read come from the
  revision's types.
- The rules the pages give a conversation with tools are checked on both sides: a message that holds tool results is
  the user's and holds nothing else, and the tool uses of a message of the assistant are answered, by id, by the
  message that follows it. A server does not send a request that breaks them, and a client refuses one.
- A request whose messages hold tool uses or results is a request with tools: it is asked only of a client that
  declared `sampling.tools`, and never under a revision whose sampling has no tools; the call then fails at once and
  nothing is sent.

### Changed

- `neomcp::SamplingMessage::content` is a list of `neomcp::SamplingContent` (text, image, audio, tool use, tool
  result) instead of `neomcp::ContentBlock`. Text, image and audio blocks convert as before.
- `neomcp::SamplingAnswer::content` is a list of `neomcp::SamplingContent` read for the call's revision instead of
  the JSON the client sent; a handler that set it to a JSON block adds a `neomcp::TextContent` instead. One block is
  written as itself and several as an array, which only a revision that allows one takes (an array is also read only
  there). Named stop reasons are in `neomcp::stop_reasons`.
- A sampling block of a kind the revision does not define (audio under 2024-11-05) makes the request fail instead of
  being written as a text block.

## 0.2.0

The interface changed in places (see "Changed"); a project built against 0.1 is rebuilt against 0.2.

### Tasks

- Tools that run as tasks: `ToolHandle::task_support` declares that a tool may (or must) run as one; the server keeps
  tasks in `ServerOptions::tasks.store`, a `neomcp::TaskStore` (`neomcp::make_memory_task_store`, or the
  application's own), runs them on its pools and bounds them by the store's limits, and lets the application start,
  read, cancel and answer tasks itself (`Server::start_task`, `task`, `cancel_task`, `answer_task`, `watch_task`).
  A handler knows that it runs as a task (`Context::task_id`).
- The extension `io.modelcontextprotocol/tasks` of 2026-07-28 on the server (`tasks/get`, `tasks/update`,
  `tasks/cancel`, a task created when the client declares the extension and the tool may run as one) and tasks of
  2025-11-25 (`params.task`, `tasks/get`, `tasks/result` that waits for the end and carries the task's requests for
  input, `tasks/cancel`, `tasks/list` per owner, status notifications). The server passes the official conformance
  suite's `tasks-*` scenarios.
- The client takes tasks of 2026-07-28 when `ClientOptions::tasks.accept` is set: `call_tool` follows a task to its
  end, `start_tool` returns the tool's result or a `neomcp::TaskHandle` (`status`, `wait`, `cancel`), and
  `Client::task` takes up a task by its id. The waits between reads, the whole wait, the number of reads and the
  length of an id are bounded (`ClientTaskOptions`). A task's requests for input reach the client's input handlers.

### Server

- Authorization: a validator that asks the authorization server's introspection endpoint (RFC 7662,
  `neomcp::server_auth_introspection`), and one that checks JWT access tokens with the authorization server's
  published keys (RFC 9068, `neomcp::server_auth_jwt`, behind `NEOMCP_JWT`).
- Authorization: a 403 also asks again for the granted scopes that a tool, resource, template or prompt declares, as
  it does for those in `required_scopes` and `scopes_supported`; `Server::scope_declarations` lists what the items
  declare.
- Authorization: the HTTP server does not start when one item declares more scopes than a client reads in one
  challenge (with `required_scopes`, 64 scopes or 4 KiB); what several items need together is cut to fit and keeps
  `required_scopes`.
- The client and the server check scope values with one function, `neomcp::is_scope_token`;
  `neomcp::http::is_scope_token` is the same function under the name of the HTTP blocks.
- `neomcp::ApplicationError`: a handler of any kind (tool, resource, resource template, prompt, completer; plain or
  coroutine) answers its request with a JSON-RPC error of its own code, message and data, on every transport and
  revision, and a call that runs as a task fails with them. A code outside the range JSON-RPC reserves, or one of the
  codes that range leaves to implementations, is available unless the governing revision names it (its profile's
  `errors`, the extensions in force and the codes it forbids a sender to use); a request whose handler uses another
  code, or whose answer is longer than `max_message_bytes`, is answered with an internal error whose message is fixed.
- A host (`neomcp::ToolHost`) completes a call with `neomcp::ApplicationFailure` for the same.
- Fixed bearer tokens for a server without an authorization server: `HttpServerOptions::fixed_tokens` (and
  `EndpointOptions::fixed_tokens`) take a `neomcp::http::FixedTokenTable` of tokens, each with its subject, scopes and
  optional expiry (`neomcp/http/fixed_tokens.h`). The grants reach `Context::authorization()` and the declared scopes
  of items as an OAuth token's do; no protected resource metadata is served and the challenges carry no
  `resource_metadata`. The table keeps SHA-256 digests only and compares in constant time over every entry;
  `FixedTokenTable::create` refuses what it cannot serve with a `FixedTokenError`; `replace_fixed_tokens` swaps the
  table while the server runs and ends the open streams of tokens that are gone or whose grants changed.

### Client

- Header fields of the application's own on the HTTP transports: `HttpEndpoint::header_fields` (fixed fields) and
  `HttpEndpoint::header_fields_for` (a callback told the kind of each request), bounded by `max_header_fields` and
  `max_header_field_bytes`. They go with every request to the endpoint, on Streamable HTTP and on the older transport
  of events over HTTP, and not to the documents the authorizer fetches. A field the transport or the enabled revisions
  write (the media types, `Authorization`, the session and version headers, the mirrored fields) is refused.

### Examples

- `examples/`: servers over stdio and HTTP, clients over the in-process, stdio and HTTP transports, a tool that asks
  the client for input, a client that signs in with OAuth and a protected server; built with
  `NEOMCP_BUILD_EXAMPLES`, and those that need no person run in CI.

### Changed

- neojson is pinned at 5ee2793. Its `json::array_t` is now a vector with its own allocator: code that holds an
  array of the library's `json` values as `std::vector<neomcp::json>` names `neomcp::json::array_t` instead.
- `neomcp::TaskHost` has two more pure virtual functions (`current_task`, `list_tasks`).
- The introspection and the JWT validators can be linked into one program.
- The answers of the protected endpoint for a token that cannot be validated now (503) carry
  `Cache-Control: no-store`, as its other answers do.

## 0.1.0

The first release.

### Protocol

- Five protocol revisions, 2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25 and 2026-07-28, each described by a JSON
  profile built into the library; the handshake era and the per-request era of 2026-07-28 are served by the same
  programs.

### Server

- Tools, resources, resource templates (RFC 6570), prompts and argument completion declared from C++ types, with
  schemas derived from the types and arguments checked before a handler runs.
- A stdio server and a Streamable HTTP server, each serving both eras.
- Asking the client for input while a call is served (forms, completions from its model, its roots), in the way each
  revision carries it, with the state between rounds signed by the server.
- Change notifications for tools, prompts and resources.
- Authorization: the Streamable HTTP endpoint as an OAuth protected resource, with protected resource metadata,
  401 and 403 challenges, a token validator interface the application implements, checks of audience, issuer,
  lifetime and scope on every request, declared scopes on tools, resources, templates and prompts, and streams that
  close when their token expires.
- Passes the official conformance suite's server scenarios at 2025-11-25 and 2026-07-28, and works with the official
  Python SDK's client over stdio and HTTP.

### Client

- `neomcp::Client` for the same five revisions over stdio (a server started as a child process), Streamable HTTP and
  the older transport of events over HTTP, with a blocking and a coroutine form of every operation.
- Tool calls (typed ones checked against the tool's output schema), lists over all their pages, resources, prompts,
  completion, progress, cancellation, the server's requests for input answered by handlers the application
  registers, and subscriptions to changes.
- OAuth sign-in (`neomcp::OAuthAuthorizer`): discovery, registration by a client ID metadata document, by dynamic
  registration or with credentials registered beforehand, PKCE, the issuer of the callback, refresh, step-up of
  scope and a change of authorization server, with tokens kept in memory or in a store the application supplies.
- Passes every scored client scenario of the official conformance suite at 2025-11-25 and 2026-07-28, the
  authorization scenarios and the two 2025-03-26 authorization scenarios included, and works with the official
  Python SDK's servers over stdio, Streamable HTTP and the older HTTP transport.

### Build

- CMake 3.22 or newer and a C++20 compiler; install rules and a package configuration
  (`find_package(neomcp CONFIG COMPONENTS ...)`), static libraries everywhere and shared ones on Linux.
- The HTTP client needs libcurl 8.5.0 or newer from the system, behind `NEOMCP_HTTP_CLIENT`.

### Not yet

- Token introspection and JWT validators for the server, the tasks extension, and the optional loopback listener for
  the client's browser redirect.
