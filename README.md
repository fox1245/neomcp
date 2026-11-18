<div align="center">

<img src="assets/logo.svg" width="160">

# neomcp

**A C++ SDK for the Model Context Protocol: servers and clients.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](README.md)

</div>

---

neomcp is a C++ library for writing [Model Context Protocol](https://modelcontextprotocol.io) (MCP) servers and
clients. Work in progress: servers over stdio work today, the rest is listed under Status, and the interface
may still change.

## Status

What exists now:

- One programming model for every protocol revision. A revision is described by a profile, a JSON document that
  names the mechanisms the revision uses and lists its methods, fields, headers and error codes. Profiles exist for
  five revisions, 2024-11-05 to 2026-07-28.
- Tools declared from C++ types: the input and output schemas are derived from the types, and arguments are checked
  against the schema before the handler runs.
- Resources, resource templates (RFC 6570), prompts and argument completion declared from C++ types, served for
  every revision with the members and the caching hints each revision defines.
- A stdio server that speaks both eras of the protocol, the handshake of the older revisions and the per-request form
  of 2026-07-28, to the same program.

Still planned:

- Streamable HTTP.
- The client.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and network access at configure time (the pinned
dependencies are fetched: [neojson](https://github.com/fox1245/neojson), standalone Asio, and GoogleTest when the
tests are built).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `NEOMCP_BUILD_TESTS` (default ON when neomcp is the top-level project) and `NEOMCP_WARNINGS_AS_ERRORS`
(default OFF). Every `profiles/*.json` is built into the library.

`NEOMCP_INTEROP_TESTS` (default OFF) adds a test that drives a stdio server with the official Python SDK's client.
Install `tests/interop/requirements.txt` into a Python environment and pass its interpreter as
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

## License

MIT. See [LICENSE](LICENSE).
