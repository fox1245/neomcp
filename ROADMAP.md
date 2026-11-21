# Roadmap

What is done, what is in progress and what has not been started. The README's Status section says what a user can
rely on today; this file is the list of work.

Last updated: 2026-11-21.

## Done

- **Profiles.** One JSON profile per protocol revision (2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25, 2026-07-28):
  the mechanisms a revision uses and its wire data. CI compares the wire data of every profile with the schema the
  specification publishes.
- **Server core.** Tools, resources, resource templates, prompts and completion declared from C++ types; JSON Schema
  2020-12 validation of arguments; one session engine without input or output of its own for both eras of the
  protocol.
- **stdio server**, both eras.
- **Streamable HTTP server**, both eras on one endpoint: sessions, the connection stream, event streams, host and
  origin checks, mirrored parameter headers, bounds on request bodies, sessions and watched resources.
- **Asking the client for input**: declared inputs (`neomcp::elicit`, `neomcp::sample`, `neomcp::roots`) and the
  low-level form (`neomcp::InputRequired`), carried in the way each revision defines.
- **Change notifications**, in the way each revision defines.
- **Patterns in schemas** are matched in time proportional to the string (RE2 behind a translation from the dialect
  of JSON Schema); CI compares the translation with V8 on generated patterns.
- **Conformance.** The official conformance suite passes for the server at 2025-11-25 and 2026-07-28 with empty
  lists of expected failures; the official Python SDK's client is run against the stdio and HTTP servers.
- **Errors of the application's own.** A handler of any kind (tool, resource, resource template, prompt, completer;
  plain or coroutine) throws `neomcp::ApplicationError` and the request is answered with a JSON-RPC error of the
  code, message and data it chose, on every transport and revision; for a task, the task fails with them. The codes
  of the range JSON-RPC reserves for itself and the codes the governing revision names are refused (the request is
  answered with an internal error), and so is an answer longer than `max_message_bytes`. The client hands the code and
  the data of every error answer to the application.
- **Build preparation for the client**: the targets `neomcp::io` and `neomcp::http_base` hold what a client shares
  with the server.
- **Shared declarations for the client**: `ByteReader`/`ByteWriter` (`neomcp/byte_stream.h`), `Headers`
  (`neomcp/http/headers.h`) and the authority parser (`neomcp/http/authority.h`) in headers that include nothing of
  the server.
- **The reading half of the wire code** (`neomcp/wire_read.h`): readers for everything a server writes, the inverses
  for input requests, and the encoder for mirrored parameter headers.
- **The client halves of the mechanisms** (`neomcp/client/mechanism.h`, target `neomcp::client`): one client half per
  name the profiles declare, each tested against its server half and against what a session writes, and the
  registry that selects them per profile and transport.
- **The client engine** (`neomcp/client/engine.h`): request ids and the pending table, both ways of opening and the
  stdio probe, the corrective retry of a refused revision, clocks as data, cancellation, progress, pings and the
  server's notifications as events; tested alone and paired in process with the server's session on every revision.
- **Input handlers on the client** (`neomcp/client/input_handlers.h`): one handler per kind (form, URL, sampling,
  roots), plain or coroutine, for both ways a server asks; capabilities declared only for the handlers that exist,
  refusals by what was declared, forms checked against the restricted form before a handler sees them.
- **`neomcp::Client`** (`neomcp/client.h`): every operation as a blocking method and an `async_` coroutine twin;
  typed tool calls with C++ types and output schema checks, lists with pagination, resources, prompts, completion;
  one thread per client, a bounded pool for plain input handlers, an in-memory transport and an in-process server;
  a lost read or list sent again once, a tool call never; a budget and limits for the schemas a server sends.
- **The stdio transport of the client** (`neomcp::client_stdio`): a server started as a child process on POSIX and
  Windows without a shell, a short named environment plus what the application adds, stderr inherited, captured or
  discarded, the shutdown ladder (stdin closed, then the group ended), and a server that exits started again at most
  `max_restarts` times in a row.
- **Change notifications on the client** (`Client::listen`, `neomcp/client/subscription.h`): one subscription call
  for both mechanisms, changes merged per kind and per resource, a lost subscription reopened with backoff and jitter.
- **The Streamable HTTP transport of the client** (`neomcp::client_http`, libcurl from the system behind
  `NEOMCP_HTTP_CLIENT`): JSON and event-stream answers, detection of the kind of server over HTTP, mirrored headers,
  sessions renewed when the server forgets them, the connection stream, resumption of a broken stream, redirects only
  within the endpoint's origin.
- **Client conformance.** The official conformance suite's client scenarios that need no authorization pass at
  2025-11-25 and 2026-07-28, with only the authorization scenarios listed as expected failures.
- **The HTTP+SSE transport of 2024-11-05 on the client**: found when a Streamable HTTP endpoint refuses the
  handshake, the endpoint event's URL resolved and held to the endpoint's origin, event names from the profile.
- **Every bound on what a server controls**, tested exactly at the bound and one past it.
- **Header fields of the application's own on the client's HTTP transport**: fields, or a callback that supplies them
  per request, sent with every request to the endpoint on both HTTP transports beside what the authorizer writes, and
  not to the authorizer's fetches; the transport's own fields cannot be replaced, and a collision is refused.
- **The client's loopback listener** for the browser's redirect (`neomcp::client_oauth_loopback`, optional).
- **Examples** (`examples/`), built and partly run in CI.
- **The client against other servers**: this library's servers over every transport and revision, and the official
  Python SDK's servers over stdio, Streamable HTTP and SSE.

## The client without authorization: done

The client is built as one engine without input or output plus transports, like the server. Its methods come in two
forms: a blocking one under the plain name and a coroutine one with the prefix `async_`
(`client->call<int>("add", args)` and `co_await client->async_call<int>("add", args)`).

Decided for the client: an answer that was lost is asked for again once for reads and lists and never for a tool
call; a server started as a child process gets a short named environment plus what the application adds; proxy
settings of the environment are honoured; a stdio server that exited is started again at the next request, three
times in a row at most; certificate verification cannot be switched off; libcurl comes from the system
(`find_package(CURL)`), behind an option, so that a server-only build needs none.

## The client's authorization: done

`neomcp::OAuthAuthorizer` signs the HTTP client in where a server asks for OAuth authorization: discovery through
protected resource metadata (and, for 2025-03-26 servers, the authorization server metadata on the server's own
origin with its default endpoints), registration by a client ID metadata document, by dynamic registration or with
credentials registered beforehand, PKCE, the issuer of the callback, refresh, step-up of scope and a change of
authorization server. Every scored client authorization scenario of the official conformance suite passes at
2025-11-25 and 2026-07-28, and the two 2025-03-26 scenarios pass as well; the six extension scenarios
(client credentials, enterprise-managed authorization, DPoP, workload identity) are not supported.

## Packaging: done

Install rules and a CMake package configuration (`find_package(neomcp CONFIG COMPONENTS ...)`), static and, on
Linux, shared libraries; versioned releases from 0.1.0.

## In progress

- **Authorization on the server.** Done: reading bearer credentials and writing challenges, declared scopes on
  tools, resources, templates and prompts checked before a handler runs, the identity a handler sees, and the
  Streamable HTTP endpoint as a protected resource (protected resource metadata, 401 and 403 answers, a validator
  interface the application implements, a cache of verdicts, a stream closed when its token expires). Still to come:
  the introspection validator (RFC 7662), the JWT validator (RFC 9068), and tests against the library's own client.
- **The tasks extension.** Done: the profile data of tasks for 2025-11-25 and of the extension
  `io.modelcontextprotocol/tasks` for 2026-07-28, and the task store. Still to come: running tools as tasks, the
  wire of both revisions on the server, the client, and the conformance suite's `tasks-*` scenarios.

## Not started: later steps

- **A validator of fixed bearer tokens** for a server without an authorization server: a table of tokens and the scopes
  each grants, whose scopes reach the handler and the declared scopes of items as an OAuth token's do, without
  protected resource metadata.

## Open items in what exists

- Sampling messages of 2025-11-25 and 2026-07-28 may hold tool use and tool result blocks (`ToolUseContent`,
  `ToolResultContent`); the content model has no such blocks, so a server cannot write them and the reader of a
  sampling request refuses a request that holds them.
- The client checks the restricted form of an elicitation by member names only; the profiles now carry the `type`
  values of the primitive schema types (`type_values`), which that check could use. Accepted content is still
  validated against the schema.
- Change notifications: many distinct updated URIs in a burst can delay a list change.
- A request of 2026-07-28 that carries a session header is routed to that session; the pages say to ignore the
  header.
- The table of handler forms at the top of `include/neomcp/server/handler.h` omits a `ToolDefinition` handler
  without `Context&`.
- The HTTP listener holds one worker thread per request in progress and per open stream. The bounds limit how long
  one connection holds a worker, not how many connections arrive; a listener that reads requests without holding a
  worker would remove that limit.
- Unicode property escapes in patterns (`\p{..}`) follow the Unicode tables of the regular expression engine
  (15.1), which can differ from a JavaScript engine's for recently assigned characters.
- Measurements not yet taken: the cost of creating a session per request under 2026-07-28, and of computing the
  mirrored parameter headers per call.
- Windows: the tests that drive raw sockets are skipped.
- Client: the bounds of the HTTP+SSE transport (its endpoint event's time, its stream's size, idle and connect
  bounds) are not yet tested at their edges.
- Client: a client fixture of the conformance suite over stdio (the suite's client scenarios run over HTTP).
- Client: very large values of the delays of a retried round that asks only with its state are not bounded at
  creation.
