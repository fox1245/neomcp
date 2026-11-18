<div align="center">

<img src="assets/logo.svg" width="160">

# neomcp

**A C++ SDK for the Model Context Protocol: servers and clients.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](README.md)

</div>

---

neomcp is a C++ library for writing [Model Context Protocol](https://modelcontextprotocol.io) (MCP) servers and
clients. Work in progress. Nothing here is usable yet.

## Planned

- One programming model for every protocol revision. A revision is described by a profile, a JSON document that
  names the mechanisms the revision uses and lists its methods, fields, headers and error codes.
- Revisions 2024-11-05 to 2026-07-28, over stdio and Streamable HTTP.
- Tools, resources and prompts declared from C++ types.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and network access at configure time (the pinned
dependencies are fetched: [neojson](https://github.com/fox1245/neojson), and GoogleTest when the tests are built).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `NEOMCP_BUILD_TESTS` (default ON when neomcp is the top-level project) and `NEOMCP_WARNINGS_AS_ERRORS`
(default OFF). Every `profiles/*.json` is built into the library.

## License

MIT. See [LICENSE](LICENSE).
