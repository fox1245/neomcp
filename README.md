<div align="center">

<img src="assets/logo.svg" width="160">

# neomcp

**A C++ SDK for the Model Context Protocol: servers and clients.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](README.md)

</div>

---

neomcp is a C++ library for writing [Model Context Protocol](https://modelcontextprotocol.io) (MCP) servers and
clients. Work in progress: servers over stdio and Streamable HTTP work today, the rest is listed under Status, and
the interface may still change.

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
  returns what it needs and is entered again with the answers, and the library carries that in the way each
  revision defines. The state that travels between the rounds is signed by the server.
- Telling clients of changes: tools, prompts and resources can be switched on and off while the server serves, a
  resource can be reported as updated, and clients hear of it in the way their revision defines.
- The server is run in CI against the official conformance suite for the revisions 2025-11-25 and 2026-07-28, and
  against the official Python SDK's client over stdio and HTTP.

Still planned:

- The client.
- Authorization.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and network access at configure time (the pinned
dependencies are fetched: [neojson](https://github.com/fox1245/neojson), standalone Asio, cpp-httplib, and
GoogleTest when the tests are built).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `NEOMCP_BUILD_TESTS` (default ON when neomcp is the top-level project) and `NEOMCP_WARNINGS_AS_ERRORS`
(default OFF). Every `profiles/*.json` is built into the library.

`NEOMCP_INTEROP_TESTS` (default OFF) adds tests that drive a stdio server and an HTTP server with the official
Python SDK's client. Install `tests/interop/requirements.txt` into a Python environment and pass its interpreter as
`NEOMCP_INTEROP_PYTHON`.

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
`http://127.0.0.1:8000/mcp` until SIGINT or SIGTERM, or on the port given as its first argument:

```cpp
#include <cstdint>
#include <optional>
#include <string>

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
        options.port = static_cast<std::uint16_t>(std::stoi(argv[1]));
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

## License

MIT. See [LICENSE](LICENSE).

The `wire` member of every file in `profiles/` is derived from the schema of the Model Context Protocol
specification, whose repository licenses new contributions under the Apache License 2.0 and keeps older ones that
have not been relicensed under the MIT License. See [NOTICE](NOTICE).
