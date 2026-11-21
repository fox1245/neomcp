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
| `mechanisms.tasks` | How a long-running request is answered with a task that the requester polls: `none` (the revision defines no tasks, or defines them in an extension that is not part of this profile), `augmented_request` (the requester asks for a task with a member of the request) or `server_directed` (the server decides, for a requester that declared the extension). |
| `transports` | The transports of the revision and, per transport, how a request is cancelled and which headers, streams and session rules apply. |
| `transports.http_sse.endpoint_event`, `transports.http_sse.message_event` | The older transport of events over HTTP: the type of the event that opens the client's event stream and whose data is the URI reference its messages are posted to, and the type of the events whose data is one message of the server. Two different, non-empty names. The transport reads them from here and holds none of its own. |
| `transports.streamable_http.status` | Optional. The HTTP status the revision prescribes for a reason: one member per reason, named after a reason the library declares (for example `header_mismatch`, `accepted` or `unknown_session`), with the status code. A reason the revision prescribes no status for is left out, and the transport answers it with a status of its own. |
| `errors` | Error codes the revision defines, and codes it forbids a sender to use. |
| `tools.invalid_arguments` | How a server reports tool arguments that fail the tool's input schema: `invalid_params_error` (the JSON-RPC error Invalid params, a protocol error) or `error_result` (a tool result with `isError` set, a tool execution error). The revision's `server/tools.mdx` says which. |
| `authorization.discovery` | How a client finds the authorization server of a server that refuses it: `protected_resource_metadata` (the resource's metadata of RFC 9728, found from the challenge or at its well-known places, names the authorization server, whose metadata of RFC 8414 follows) or `origin_metadata` (the authorization server's metadata on the origin of the server's URL, with the URL's path discarded, then default endpoints there). Authorization starts at the first refused request, before a version is agreed, so a client always tries protected resource metadata first; `origin_metadata` is the fallback it tries when no protected resource metadata is found and an enabled profile names it. 2024-11-05 defines no authorization; its profile names `protected_resource_metadata`, so a server of that revision that refuses a request is met as a later one is. The well-known names of protected resource metadata and of RFC 8414 do not vary between revisions and are not data here. |
| `authorization.origin_metadata` | Present exactly when `discovery` is `origin_metadata`. `metadata_path` is where the authorization server's metadata is on the server's origin; `default_endpoints` holds the paths of the `authorization`, `token` and `registration` endpoints on that origin when no metadata is found there. Each is an absolute path (RFC 3986 `path-absolute`): no query, no fragment, no authority. The client holds none of these names itself. |

## Extension profiles

A revision of the protocol can be extended by a document of its own that the protocol's specification names, an extension, which adds messages, types, a mechanism, headers and error codes to the revisions it is defined for. The core profile of the revision says nothing of it: `profiles/2026-07-28.json` selects `none` for `mechanisms.tasks`, and a server or client that does not take the extension up behaves as that profile says. An extension has one file per revision, `profiles/extensions/<folder>/<revision>.json`, where the folder is the extension's identifier with its `/` replaced by `.`. Every such file is built into the library.

`compose` (see `include/neomcp/profile.h`) makes the profile of a revision served with extensions: a copy of the core profile in which the extension's mechanism replaces the core's for the slots it names, its wire tables are added, its headers are added to the Streamable HTTP transport's (a header the revision already has, such as `Mcp-Name`, gets the extension's methods added; a revision without that transport takes none), and its error codes are added. A message, type, union or error code that both define must be defined in the same way, and a header may not list a method the revision already lists; any other difference is an error that names the place in the extension's document. `Profile::extensions()` lists the extensions a profile was composed with, and the library reads the extension's behaviour from the composed profile like any other profile data.

| Member | Meaning |
|---|---|
| `profile_version` | Version of this file format. |
| `extension` | The extension's identifier, for example `io.modelcontextprotocol/tasks`. |
| `revisions` | The revisions of the protocol the extension is defined for. |
| `evidence` | As for a revision. The repository and commit are those of the extension's own repository. |
| `mechanisms` | The slots the extension changes, with the mechanism it selects for each. |
| `senders` | Who sends each message, by the name of its type: `requests` and `notifications`, each an object of type name to a list of `client` and `server`. The schema of an extension does not group its messages by sender, so it is written here by hand (see the `wire` member), and the loader checks it against the wire tables. A message type that the schema defines with a method and that is listed in neither is in `wire.unlisted_message_types`: for the tasks extension that is its status notification, whose parameters are a union of objects written inline, a shape the tool does not record; the message is left out until the library uses it. |
| `transports.streamable_http.request_headers` | Optional. Headers of the Streamable HTTP transport, in the form of the revision's own; see above for how they join the transport's. |
| `errors` | Optional. Error codes the extension uses, by name. |
| `wire` | Generated, as below. |

## The `wire` member

`wire` is generated from the revision's official `schema.json` and must not be edited. The schema is part of the
Model Context Protocol specification, which carries its own licence terms; [NOTICE](../NOTICE) at the root of this
repository gives the attribution that goes with data derived from it. To regenerate or check it:

```sh
tools/profile_wire.py path/to/schema/2026-07-28/schema.json --write profiles/2026-07-28.json
tools/profile_wire.py path/to/schema/2026-07-28/schema.json --check profiles/2026-07-28.json
```

The schema of an extension has no union of the requests a client or a server sends. For it the senders are the hand-written member `senders` of the extension's own file, which the tool reads with `--senders`:

```sh
tools/profile_wire.py path/to/extension/schema.json --senders profiles/extensions/<folder>/<revision>.json --write profiles/extensions/<folder>/<revision>.json
tools/profile_wire.py path/to/extension/schema.json --senders profiles/extensions/<folder>/<revision>.json --check profiles/extensions/<folder>/<revision>.json
```

An extension may write the method of a message in a part of an `allOf` and the parameters of a request inline; the tool reads both, and an inline parameter object has no name in `params` (the request type's `property_types` gives it, as for any inline object). A definition that is a union of objects written inline, such as the task states joined as `DetailedTask`, is not recorded as a union (its alternatives have no names); the objects it joins are recorded under the names the schema gives them elsewhere (`WorkingTask`, `CompletedTask` and the others), and a definition of that shape that has no named counterpart is not in `wire` at all.

| Member | Meaning |
|---|---|
| `schema` | The schema's dialect and the SHA-256 of the file the tables were derived from. |
| `requests`, `notifications` | Each method with who sends it (`client`, `server` or both), its message type, its parameter type and that type's required members. |
| `input_requests` | The requests a server may place in a result for the client to answer. |
| `unlisted_message_types` | Message types the schema defines but assigns to no sender. It is empty for every profile here, and a new entry needs a decision. |
| `types` | Every object type with its properties and its required properties. An object that the schema writes inline as a property of a definition (with `properties`, as `Resource.annotations` is in 2024-11-05, whose definitions have no `Annotations` type) is a type of its own, named after the definition that writes it, a `.` and the property (`Resource.annotations`); one written inside such an object is named after it in the same way. A name of the schema never holds a `.`, and the tool stops if a derived name is one the schema defines. `property_types`, present only when the type has such properties, maps a property to the type of its value: the schema's own type where the property refers to an object type, the derived name where the object is written inline. A property of any other shape (a union, an array, a string) is not listed. The library finds the type of a property through it and never by composing a name. A type the schema composes from others (allOf) or defines as a reference has the combined members. `type_values`, present only when the schema lists them (a constant or an enumeration of strings), holds the values of the type's `type` property: the member tells which alternative of a union an object is, for example which primitive schema definition of a form a property has. |
| `unions` | Every type that is a union of other types, with its alternatives. A message whose parameters are a union lists as required the members every alternative requires. |
