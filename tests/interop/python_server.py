"""A server written with the official Python SDK, serving the names the client matrix's flow expects.

It is the peer of the C++ client in the interoperability job: the test neomcp_client_matrix_peer_test
(tests/client_matrix/peer_test.cpp) starts it once per transport and revision and runs the same flow as against the
library's own matrix server (tests/client_matrix/registrations.h): tools (a typed one among them), a resource and a
resource template, a prompt with a completer, tools that ask the client for a form, its model and its roots, a tool
that waits until it is cancelled, and a tool that changes the list of tools.

    python -I python_server.py stdio      over its standard input and output
    python -I python_server.py http       Streamable HTTP on 127.0.0.1, path /mcp
    python -I python_server.py sse        the transport of events over HTTP on 127.0.0.1, path /sse
    python -I python_server.py describe   what the installed SDK offers, as JSON on standard output

Over HTTP the port is chosen by the operating system and written to standard error on a line of its own before
anything else (the socket listens before the SDK is imported, so nothing the import prints comes first); the server
runs until SIGINT or SIGTERM.

The SDK serves both eras on every transport it has and offers no way to serve one era only, so the cells of one era
against a server of the other exist only for the library's own servers.
"""

import socket
import sys

MODES = ("stdio", "http", "sse", "describe")
MODE = sys.argv[1] if len(sys.argv) == 2 else ""
if MODE not in MODES:
    sys.exit("usage: python_server.py stdio|http|sse|describe")

LISTENER = None
if MODE in ("http", "sse"):
    LISTENER = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    LISTENER.bind(("127.0.0.1", 0))
    LISTENER.listen(128)
    print(LISTENER.getsockname()[1], file=sys.stderr, flush=True)

import importlib.metadata  # noqa: E402
import json  # noqa: E402
from typing import Annotated  # noqa: E402

import anyio  # noqa: E402
from pydantic import BaseModel, Field  # noqa: E402

from mcp.server import MCPServer, NotificationOptions  # noqa: E402
from mcp.server.mcpserver import (  # noqa: E402
    AcceptedElicitation,
    Context,
    Elicit,
    ElicitationResult,
    ListRoots,
    Resolve,
    Sample,
)
from mcp.types import (  # noqa: E402
    Completion,
    CreateMessageResult,
    ListRootsResult,
    PromptReference,
    SamplingMessage,
    TextContent,
)

mcp = MCPServer("neomcp-client-matrix-python", version="0.0.1", log_level="WARNING")


def say(text):
    print(f"python_server: {text}", file=sys.stderr, flush=True)


# The SDK declares the tools' `listChanged` on a connection with a handshake only when the options say so; the server
# reports changes of its tools, so it says so on every connection it opens.
_lowlevel = mcp._lowlevel_server
_plain_options = _lowlevel.create_initialization_options


def _options_with_tool_changes(notification_options=None, *args, **kwargs):
    return _plain_options(notification_options or NotificationOptions(tools_changed=True), *args, **kwargs)


_lowlevel.create_initialization_options = _options_with_tool_changes


def text_in(content):
    """The text of a content block, or of the first text block of a list of them."""
    blocks = content if isinstance(content, list) else [content]
    for block in blocks:
        if getattr(block, "type", None) == "text":
            return block.text
    return ""


# ---- Tools


class Sum(BaseModel):
    sum: int


@mcp.tool(description="Add two integers")
def add(a: int, b: int) -> Sum:
    return Sum(sum=a + b)


@mcp.tool(description="Return one text block")
def test_simple_text() -> str:
    return "This is a simple text response for testing."


@mcp.tool(description="Report progress three times, then answer")
async def test_tool_with_progress(ctx: Context) -> str:
    await ctx.report_progress(0, 100)
    await ctx.report_progress(50, 100)
    await ctx.report_progress(100, 100)
    return "Progress test completed"


@mcp.tool(description="Wait for a time, or until the call is cancelled")
async def wait(milliseconds: int, ctx: Context) -> str:
    await ctx.report_progress(0, None, "waiting")
    await anyio.sleep(milliseconds / 1000)
    return "waited"


# ---- Asking the client: the SDK asks with a request of its own or inside the result, as the revision prescribes.


def ask_model(prompt: str) -> Sample:
    message = SamplingMessage(role="user", content=TextContent(type="text", text=prompt))
    return Sample([message], max_tokens=100)


@mcp.tool(description="Ask the client's model for a completion of a prompt")
async def test_sampling(prompt: str, answer: Annotated[CreateMessageResult, Resolve(ask_model)]) -> str:
    return f"LLM response: {text_in(answer.content)}"


class Contact(BaseModel):
    username: str = Field(description="User's response")
    email: str = Field(description="User's email address")


def ask_user(message: str) -> Elicit[Contact]:
    return Elicit(message, Contact)


@mcp.tool(description="Ask the user for a user name and an email address")
async def test_elicitation(
    message: str, answer: Annotated[ElicitationResult[Contact], Resolve(ask_user)]
) -> str:
    if isinstance(answer, AcceptedElicitation):
        return f"User response: action=accept, content={json.dumps(answer.data.model_dump())}"
    return f"User response: action={answer.action}"


def ask_roots() -> ListRoots:
    return ListRoots()


@mcp.tool(description="Ask the client for its roots")
async def test_input_required_result_list_roots(roots: Annotated[ListRootsResult, Resolve(ask_roots)]) -> str:
    return f"Roots ({len(roots.roots)}): " + " ".join(str(root.uri) for root in roots.roots)


# ---- A change of the tools: the trigger offers the hidden tool, and withdraws it the next time.

hidden_offered = False


def test_hidden_tool() -> str:
    return "hidden"


@mcp.tool(description="Changes the list of tools")
async def test_trigger_tool_change(ctx: Context) -> str:
    global hidden_offered
    if hidden_offered:
        mcp.remove_tool("test_hidden_tool")
    else:
        mcp.add_tool(test_hidden_tool, description="A tool the trigger offers and withdraws")
    hidden_offered = not hidden_offered
    # A client of 2026-07-28 hears of it on its subscription, a client with a handshake on its connection.
    await ctx.notify_tools_changed()
    try:
        await ctx.session.send_tool_list_changed()
    except Exception as failure:  # the connection may have no way to tell it; the subscription has
        say(f"send_tool_list_changed: {failure!r}")
    return "The list of tools changed."


# ---- Resources, prompts, completion


@mcp.resource("test://static-text", name="static_text", description="A resource with a fixed text", mime_type="text/plain")
def static_text() -> str:
    return "This is the content of the static text resource."


@mcp.resource("test://template/{id}/data", name="template_data", description="Data for one identifier")
def template_data(id: str) -> str:
    return json.dumps({"id": id, "templateTest": True, "data": f"Data for ID: {id}"})


@mcp.prompt(description="A prompt that takes two arguments")
def test_prompt_with_arguments(arg1: str, arg2: str) -> str:
    return f"Prompt with arguments: arg1='{arg1}', arg2='{arg2}'"


@mcp.completion()
async def complete(ref, argument, context):
    if isinstance(ref, PromptReference) and ref.name == "test_prompt_with_arguments" and argument.name == "arg1":
        return Completion(values=[value for value in ("test", "testing", "tested") if value.startswith(argument.value)])
    return None


# ---- Serving


def describe():
    """What the installed SDK offers: its version, the revisions it knows, and the transports this script uses."""
    import mcp_types.version as versions

    offered = {
        "mcp": importlib.metadata.version("mcp"),
        "known_revisions": list(versions.KNOWN_PROTOCOL_VERSIONS),
        "handshake_revisions": list(versions.HANDSHAKE_PROTOCOL_VERSIONS),
        "modern_revisions": list(versions.MODERN_PROTOCOL_VERSIONS),
        "server_has": {name: callable(getattr(mcp, name, None)) for name in ("run_stdio_async", "streamable_http_app", "sse_app")},
        "tools": sorted(tool.name for tool in anyio.run(mcp.list_tools)),
    }
    print(json.dumps(offered, indent=2))


async def serve_http(app):
    import uvicorn

    config = uvicorn.Config(app, log_level="warning", lifespan="on")
    await uvicorn.Server(config).serve(sockets=[LISTENER])


def main():
    if MODE == "describe":
        describe()
    elif MODE == "stdio":
        anyio.run(mcp.run_stdio_async)
    elif MODE == "http":
        anyio.run(serve_http, mcp.streamable_http_app(streamable_http_path="/mcp"))
    else:
        anyio.run(serve_http, mcp.sse_app(sse_path="/sse", message_path="/messages/"))


main()
