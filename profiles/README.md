# Revision profiles

One JSON file per MCP specification revision. A profile says how that revision behaves by naming mechanisms that
the library implements in code, and lists what the revision puts on the wire. The library reads the profile of the
revision a peer speaks and never branches on a revision string.

A revision that only combines existing mechanisms needs a new profile and no new code. A revision that introduces
a new way of working needs one new mechanism in code, and then a profile that names it.

## Members written by hand

They come from the specification's prose. `evidence` records the pages, the commit of the specification
repository and the date they were read.

| Member | Meaning |
|---|---|
| `profile_version` | Version of this file format. |
| `revision` | The revision string, for example `2026-07-28`. |
| `era` | `legacy` (2025-11-25 and earlier) or `modern`, the specification's own terms. |
| `evidence` | Where the hand-written members were read. |
| `mechanisms.open` | How a peer starts: `initialize` (a handshake) or `request_meta` (every request carries its version and capabilities). |
| `mechanisms.state` | What the server may remember: `connection` (the handshake's result, per stdio process or HTTP session) or `none`. |
| `mechanisms.server_input` | How a server asks its client for something: `server_request` (a JSON-RPC request to the client) or `input_required_result` (a result that asks, and a retried call that answers). Under `input_required_result` the requests that may ask are read from `wire`: those whose params type has the members `inputResponses` and `requestState`. |
| `mechanisms.change_feed` | How change notifications reach a client: `connection_notifications` or `subscriptions_listen`. |
| `mechanisms.batch` | `array` when a JSON-RPC batch is allowed, else `none`. |
| `transports` | The transports of the revision and, per transport, how a request is cancelled and which headers, streams and session rules apply. |
| `transports.streamable_http.status` | Optional. The HTTP status the revision prescribes for a reason: one member per reason, named after a reason the library declares (for example `header_mismatch`, `accepted` or `unknown_session`), with the status code. A reason the revision prescribes no status for is left out, and the transport answers it with a status of its own. |
| `errors` | Error codes the revision defines, and codes it forbids a sender to use. |
| `tools.invalid_arguments` | How a server reports tool arguments that fail the tool's input schema: `invalid_params_error` (the JSON-RPC error Invalid params, a protocol error) or `error_result` (a tool result with `isError` set, a tool execution error). The revision's `server/tools.mdx` says which. |

## The `wire` member

`wire` is generated from the revision's official `schema.json` and must not be edited:

```sh
tools/profile_wire.py path/to/schema/2026-07-28/schema.json --write profiles/2026-07-28.json
tools/profile_wire.py path/to/schema/2026-07-28/schema.json --check profiles/2026-07-28.json
```

| Member | Meaning |
|---|---|
| `schema` | The schema's dialect and the SHA-256 of the file the tables were derived from. |
| `requests`, `notifications` | Each method with who sends it (`client`, `server` or both), its message type, its parameter type and that type's required members. |
| `input_requests` | The requests a server may place in a result for the client to answer. |
| `unlisted_message_types` | Message types the schema defines but assigns to no sender. It is empty for every profile here, and a new entry needs a decision. |
| `types` | Every object type with its properties and its required properties. A type the schema composes from others (allOf) or defines as a reference has the combined members. |
| `unions` | Every type that is a union of other types, with its alternatives. A message whose parameters are a union lists as required the members every alternative requires. |
