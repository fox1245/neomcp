<div align="center">

<img src="assets/logo.svg" width="160">

# neomcp

**A C++ SDK for the Model Context Protocol: servers and clients.**

</div>

---

neomcp is a C++ library for writing [Model Context Protocol](https://modelcontextprotocol.io) (MCP) servers and
clients. Work in progress. Nothing here is usable yet.

## Planned

- One programming model for every protocol revision. A revision is described by a profile, a JSON document that
  names the mechanisms the revision uses and lists its methods, fields, headers and error codes.
- Revisions 2024-11-05 to 2026-07-28, over stdio and Streamable HTTP.
- Tools, resources and prompts declared from C++ types.
