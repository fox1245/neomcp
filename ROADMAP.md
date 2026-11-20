# Roadmap

What is done, what is in progress and what has not been started. The README's Status section says what a user can
rely on today; this file is the list of work.

Last updated: 2026-11-20.

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

## In progress

Nothing is on a branch that is not on `main`.

## Not started: the client without authorization

The client is built as one engine without input or output plus transports, like the server. Its methods come in two
forms: a blocking one under the plain name and a coroutine one with the prefix `async_`
(`client->call<int>("add", args)` and `co_await client->async_call<int>("add", args)`).

In order; items on one line can be done side by side.

1. `neomcp::Client`: the typed interface (tools with C++ types, lists with pagination, resources, prompts,
   completion), results checked against output schemas, limits for schemas a server sends, an in-memory transport.
2. Change notifications for the client | the stdio transport (a child process, POSIX and Windows) | the HTTP transport
   on libcurl with JSON answers, detection of the kind of server over HTTP, mirrored headers, and a client fixture
   for the official conformance suite in CI.
3. Event streams for the HTTP transport: answers as streams, server requests on streams, sessions, the connection
   stream, resumption.
4. The HTTP+SSE transport of 2024-11-05 (client only) | every bound on what a server controls tested at its edge |
   the official Python SDK's servers as peers, and the matrix of this client against this server over every
   transport and revision.

Decided for the client: an answer that was lost is asked for again once for reads and lists and never for a tool
call; a server started as a child process gets a short named environment plus what the application adds; proxy
settings of the environment are honoured; a stdio server that exited is started again at the next request, three
times in a row at most; certificate verification cannot be switched off; libcurl comes from the system
(`find_package(CURL)`), behind an option, so that a server-only build needs none.

Acceptance: the official conformance suite's client scenarios that need no authorization, at 2025-11-25 and
2026-07-28, with empty lists of expected failures.

## Not started: later steps

- **Authorization on the client** (OAuth: discovery, registration, scopes, token handling) and the conformance
  suite's authorization scenarios.
- **Authorization on the server** (validation of access tokens, protected resource metadata).
- **The tasks extension.** The conformance suite's `tasks-*` scenarios are not scored and do not pass.
- **Examples** (`examples/`): after everything above.
- **Packaging**: install rules and a package configuration file; versioned releases.

## Open items in what exists

- `ServerOptions` does not carry the newer bounds of `SessionOptions` (`max_watched_uri_bytes`, `max_watched_bytes`,
  `max_client_info_bytes`, `max_token_bytes`); a `Server`'s sessions use the defaults.
- The keywords a form of elicitation may use are written out in `src/server/form_schema.cpp` for the newest
  revision; they should come from the profile's types, so that a revision whose types differ (2025-06-18 has no
  `default` for strings) gets the form it defines.
- Where the request state is required (`answers_need_state`), a retry whose answers are dropped is still treated as
  a retry when caching hints are written.
- stdio: cancelling a request by an id that is in use under both eras at once.
- `lastModified` of a resource is written under 2024-11-05, whose types have no annotations.
- Sampling messages of 2025-11-25 and 2026-07-28 may hold tool use and tool result blocks (`ToolUseContent`,
  `ToolResultContent`); the content model has no such blocks, so a server cannot write them and the reader of a
  sampling request refuses a request that holds them.
- The client checks the restricted form of an elicitation by member names only, because the profiles do not carry
  the `type` values of the primitive schema types; accepted content is still validated against the schema.
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
