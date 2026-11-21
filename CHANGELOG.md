# Changelog

Versions follow the rule of the package configuration: before 1.0, a release with a new minor number may change the
interface, and a new patch number does not.

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
