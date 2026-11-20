<div align="center">

<img src="assets/logo.svg" width="160">

# neomcp

**A C++ SDK for the Model Context Protocol: servers and clients.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](README.md)

</div>

---

neomcp is a C++ library for writing [Model Context Protocol](https://modelcontextprotocol.io) (MCP) servers and
clients. Work in progress: servers and clients over stdio and HTTP work today, authorization does not yet, and the
interface may still change.

## Status

What exists now:

- One programming model for every protocol revision. A revision is described by a profile, a JSON document that
  names the mechanisms the revision uses and lists its methods, fields, headers and error codes. Profiles exist for
  five revisions, 2024-11-05 to 2026-07-28.
- Tools declared from C++ types: the input and output schemas are derived from the types, and arguments are checked
  against the schema before the handler runs.
- Resources, resource templates (RFC 6570), prompts and argument completion declared from C++ types, served for
  every revision with the members and the caching hints each revision defines. A value matched by a resource template
  comes from the client and may hold any text, path separators and `..` included; a handler that uses it to reach a
  file or another store must check it against what it is willing to serve.
- A stdio server that speaks both eras of the protocol, the handshake of the older revisions and the per-request form
  of 2026-07-28, to the same program.
- A Streamable HTTP server that serves both eras on one endpoint: sessions for the revisions that have them, the
  per-request form of 2026-07-28 without one.
- Asking the client for input while a call is served (a form, a completion from its model, its roots): a handler
  declares what it needs and runs once the answers are there, or returns what it needs and is entered again with
  them, and the library carries that in the way each revision defines. The state that travels between the rounds is
  signed by the server.
- Telling clients of changes: tools, prompts and resources can be switched on and off while the server serves, a
  resource can be reported as updated, and clients hear of it in the way their revision defines.
- The server is run in CI against the official conformance suite for the revisions 2025-11-25 and 2026-07-28, and
  against the official Python SDK's client over stdio and HTTP.
- A client (`neomcp::Client`, in `neomcp/client.h`) for the same five revisions. It starts a server as a child
  process and speaks over its pipes (every revision), or speaks to an HTTP endpoint over Streamable HTTP (2025-03-26
  to 2026-07-28) and over the older transport of events over HTTP (2024-11-05). Opening finds out which kind of
  server answers and at which revision: one that names its revision on every request, one that opens with a
  handshake, or one of the older HTTP transport. Every operation has a blocking form and a coroutine form.
- What the client does on every transport, each where the governing revision defines it: tool calls, and typed ones
  whose structured result is checked against the tool's output schema and decoded into a C++ type (from 2025-06-18;
  on older revisions a typed call fails at once and sends nothing); lists followed over all their pages; resources
  and resource templates; prompts; completion; progress; cancellation; the server's requests for input (a form from
  2025-06-18, a completion from the client's model, the client's roots) answered by handlers the application
  registers, in whichever way the revision carries them; and changes of what the server offers, taken from a
  subscription. By default a tool call whose answer was lost is never sent again, and a read or a list is sent again
  once. A stdio server that exits is started again at the next request, up to a bound.
- A client that starts a server as a child process runs the command with the application's rights, and a variable
  of the application's environment reaches the server only when the application passes it. The HTTP client honours
  the proxy settings of the environment unless told otherwise, follows redirects only within the endpoint's origin,
  and always verifies certificates.
- The client is run in CI against the official conformance suite: every client scenario that needs no
  authorization passes at 2025-11-25 and 2026-07-28. It is also run against the library's own servers over every
  transport and revision, against servers that serve only one of the two eras, and against the official Python
  SDK's servers over stdio, Streamable HTTP and the older transport of events over HTTP.

Still planned:

- Authorization, for the server and the client.

[ROADMAP.md](ROADMAP.md) lists the work that is in progress and the work that has not been started.

## Build

Requirements: CMake 3.22 or newer, a C++20 compiler, and network access at configure time (the pinned
dependencies are fetched: [neojson](https://github.com/fox1245/neojson), standalone Asio, cpp-httplib,
[RE2](https://github.com/google/re2) with [Abseil](https://github.com/abseil/abseil-cpp) for the regular expressions
of JSON Schema, and GoogleTest when the tests are built).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `NEOMCP_BUILD_TESTS` (default ON when neomcp is the top-level project) and `NEOMCP_WARNINGS_AS_ERRORS`
(default OFF). Every `profiles/*.json` is built into the library.

`NEOMCP_HTTP_CLIENT` (default ON) builds the client's HTTP transports, which need libcurl 8.5.0 or newer from the
system (with its headers); configure stops with a message when it is missing. A program that only serves, or a
client that only starts servers as child processes, can be built with the option OFF and needs no libcurl.

`NEOMCP_INTEROP_TESTS` (default OFF) adds tests that drive a stdio server and an HTTP server with the official
Python SDK's client, and the official Python SDK's servers with this library's client. Install
`tests/interop/requirements.txt` into a Python environment and pass its interpreter as `NEOMCP_INTEROP_PYTHON`.

## A first server

```cpp
#include <optional>
#include <string>

#include <neomcp/server.h>
#include <neomcp/stdio.h>

struct AddArgs {
    int a;
    int b;
    std::optional<std::string> note;
};
NEOMCP_DESCRIBE(AddArgs, a, "first addend", b, "second addend", note, "a note to echo back")

int main() {
    neomcp::Server server({.name = "calc", .version = "1.0.0"});
    server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });
    return neomcp::run_stdio(server);
}
```

This is a complete MCP server with one tool, `add`. It reads requests from standard input and writes answers to
standard output, one JSON message per line, and exits when its input closes. To build it, link the two targets:

```cmake
add_subdirectory(neomcp)
target_link_libraries(calc PRIVATE neomcp::server neomcp::stdio)
```

## Serving over HTTP

The same server speaks Streamable HTTP with `neomcp::run_http` from `<neomcp/http_server.h>` (target
`neomcp::http`). This program is the first server with `run_http` in place of `run_stdio`. It serves
`http://127.0.0.1:8000/mcp` until SIGINT or SIGTERM, or on the port given as its first argument. An argument that is
not a port (a number from 0 to 65535, written in digits only) is refused with a message on standard error and a
nonzero exit code:

```cpp
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <neomcp/http_server.h>
#include <neomcp/server.h>

struct AddArgs {
    int a;
    int b;
    std::optional<std::string> note;
};
NEOMCP_DESCRIBE(AddArgs, a, "first addend", b, "second addend", note, "a note to echo back")

int main(int argc, char** argv) {
    neomcp::Server server({.name = "calc", .version = "1.0.0"});
    server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });

    neomcp::HttpServerOptions options;
    options.port = 8000;
    if (argc > 1) {
        const std::string_view text = argv[1];
        const char* const end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, options.port);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            std::cerr << "calc_http: \"" << text << "\" is not a port (0 to 65535)\n";
            return EXIT_FAILURE;
        }
    }
    return neomcp::run_http(server, options);
}
```

To build it, link `neomcp::server` and `neomcp::http`:

```cmake
add_subdirectory(neomcp)
target_link_libraries(calc_http PRIVATE neomcp::server neomcp::http)
```

It binds the loopback address unless told otherwise and checks the `Host` and `Origin` headers of every request
(a browser page must not reach a local server through DNS rebinding). On any other address, name the hosts clients
use in `allowed_hosts`, or switch the check off explicitly; otherwise the server does not start. There is no TLS: put
a reverse proxy in front of a server that other machines reach.

Sizing: each request waiting for its answer and each open response stream holds one worker thread of the listener
for as long as it lasts. `max_worker_threads` (default 32) bounds them together; connections beyond it wait, up to
`max_queued_connections`, and one beyond that is closed. A client of a session-based revision usually keeps one
stream open besides its requests, so allow one worker per such client plus the requests you expect in flight.

Three bounds keep a client from holding these resources longer than it uses them:

- A request body must arrive within `body_timeout` (default 2 minutes, enough for the largest body over a slow
  link) of its headers, whether it has a `Content-Length` or comes in chunks; a slower one is answered 408 and its
  worker is free again.
- Of the sessions kept for session-based revisions (`endpoint.max_sessions`, default 1024), one that nobody used
  after its handshake ends after `endpoint.unused_session_timeout` (default 30 seconds), and gives its place to a
  new handshake when the table is full; a session that was used keeps its place.
- The resource URIs a session watches may be at most 8 KiB each and 128 KiB together
  (`SessionOptions::max_watched_uri_bytes` and `max_watched_bytes`; a `Server`'s sessions keep these defaults); a
  longer one is refused.

These bounds limit how long one connection holds a worker, not how many connections arrive. A server that clients
outside its own machine or network reach belongs behind a reverse proxy that buffers request bodies and limits
connections, as it does for TLS.

## Asking the client

A tool, a prompt or a resource can ask the client for something while a call is served: a form the user fills in, a
completion from the client's model, the client's roots. Declare it after the handler, and the handler takes the
answer:

```cpp
struct Confirm {
    bool ok;
};
NEOMCP_DESCRIBE(Confirm, ok, "Delete it?")

server.tool<DeleteArgs>("delete_folder", "Delete a folder",
    [](const DeleteArgs& a, const neomcp::Answer<Confirm>& confirm) {
        if (!confirm.accepted() || !confirm->ok) return std::string("kept");
        remove_folder(a.path);
        return std::string("deleted");
    },
    neomcp::elicit<Confirm>("confirm", [](const DeleteArgs& a) { return "Delete " + a.path + "?"; }));
```

The form is built from `Confirm`, and what the user submits is checked against it before the handler sees it. The
question function runs on every round of the call, so it must have no effects and must ask the same question for the
same arguments. The handler runs exactly once, when every declared input has its answer, and never in a round that
asks. Whether the client is asked by a result and a retried call or by requests sent while the call waits follows the
revision it speaks, and the same code serves both. A handler that needs more control returns `neomcp::InputRequired`
itself and is entered again with the answers, as `neomcp/server.h` describes.

## License

MIT. See [LICENSE](LICENSE).

The `wire` member of every file in `profiles/` is derived from the schema of the Model Context Protocol
specification, whose repository licenses new contributions under the Apache License 2.0 and keeps older ones that
have not been relicensed under the MIT License. See [NOTICE](NOTICE).
