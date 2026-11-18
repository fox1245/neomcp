"""Interoperability of the stdio server with the official Python SDK's client.

The test starts the fixture server (tests/fixtures/stdio_server.cpp) as a subprocess, once per era the client can
speak, and drives it through the SDK's `Client`:

* legacy: `mode="legacy"`, the client sends `initialize`;
* modern: `mode="auto"`, the client probes `server/discover` first and adopts the newest modern revision.

The path of the fixture binary comes from the environment variable NEOMCP_STDIO_FIXTURE. Run it with the interpreter
that has tests/interop/requirements.txt installed.
"""

import json
import os
import subprocess
import sys
import unittest

import anyio
from mcp import Client, MCPError, StdioServerParameters

FIXTURE = os.environ.get("NEOMCP_STDIO_FIXTURE", "")

# Long enough for a loaded machine; reached only when something is broken.
PATIENCE_SECONDS = 60

TOOL_NAMES = ["add", "echo", "content", "fail", "crash", "count", "wait", "stats", "bounded"]
# The kinds of content block, by their `type` on the wire, in the order the fixture's `content` tool writes them.
CONTENT_KINDS = ["text", "image", "audio", "resource_link", "resource"]


def text_of(result):
    """The text of the first content block."""
    return result.content[0].text


class EraChecks:
    """The checks that hold in both eras. A subclass says how the client opens and what it must negotiate."""

    mode = None  # the SDK's connect mode
    version = None  # the revision the client must end up with
    # What the negotiated revision prescribes for arguments that fail the input schema: "error_result" is a result
    # with isError (2025-11-25 and 2026-07-28, tools.mdx "Input validation errors"); "invalid_params_error" is the
    # JSON-RPC error -32602 (2025-06-18 and earlier, "Invalid arguments").
    invalid_arguments = "error_result"

    def with_client(self, body):
        async def main():
            params = StdioServerParameters(command=FIXTURE)
            with anyio.fail_after(PATIENCE_SECONDS):
                async with Client(params, mode=self.mode) as client:
                    await body(client)

        anyio.run(main)

    def test_negotiates_the_revision(self):
        async def body(client):
            self.assertEqual(client.protocol_version, self.version)
            info = client.server_info
            self.assertIsNotNone(info)
            self.assertEqual(info.name, "neomcp-stdio-fixture")
            self.assertIsNotNone(client.server_capabilities.tools)

        self.with_client(body)

    def test_lists_the_tools_and_their_schemas(self):
        async def body(client):
            listing = await client.list_tools()
            self.assertEqual([tool.name for tool in listing.tools], TOOL_NAMES)
            tools = {tool.name: tool for tool in listing.tools}

            add = tools["add"]
            self.assertEqual(add.description, "Add two integers")
            self.assertEqual(add.input_schema["type"], "object")
            self.assertEqual(sorted(add.input_schema["required"]), ["a", "b"])
            self.assertEqual(add.input_schema["properties"]["a"]["type"], "integer")
            self.assertEqual(add.input_schema["properties"]["a"]["description"], "first addend")
            self.assertEqual(add.input_schema["properties"]["note"]["type"], "string")
            self.assertNotIn("note", add.input_schema["required"])
            self.assertFalse(add.input_schema["additionalProperties"])

            self.assertIsNotNone(add.output_schema)
            self.assertEqual(add.output_schema["type"], "object")
            self.assertEqual(add.output_schema["required"], ["sum"])
            self.assertEqual(add.output_schema["properties"]["sum"]["type"], "integer")
            self.assertEqual(add.output_schema["properties"]["note"]["type"], "string")

            self.assertIsNone(tools["echo"].output_schema)
            self.assertEqual(tools["bounded"].input_schema["properties"]["count"]["minimum"], 0)
            self.assertEqual(tools["stats"].output_schema["required"], ["waits_started", "waits_cancelled"])

        self.with_client(body)

    def test_a_typed_call_returns_structured_content_and_the_same_text(self):
        async def body(client):
            # The SDK also validates the structured content against the tool's output schema.
            result = await client.call_tool("add", {"a": 2, "b": 3, "note": "hello"})
            self.assertFalse(result.is_error)
            self.assertEqual(result.structured_content, {"sum": 5, "note": "hello"})
            self.assertEqual(json.loads(text_of(result)), {"sum": 5, "note": "hello"})

            result = await client.call_tool("add", {"a": -4, "b": 1})
            self.assertEqual(result.structured_content, {"sum": -3})
            self.assertEqual(json.loads(text_of(result)), {"sum": -3})

        self.with_client(body)

    def test_a_text_call(self):
        async def body(client):
            result = await client.call_tool("echo", {"text": "grüße 世界"})
            self.assertFalse(result.is_error)
            self.assertEqual(text_of(result), "grüße 世界")
            self.assertIsNone(result.structured_content)

        self.with_client(body)

    def test_every_kind_of_content(self):
        async def body(client):
            result = await client.call_tool("content", {})
            self.assertFalse(result.is_error)
            self.assertEqual([block.type for block in result.content], CONTENT_KINDS)
            text, image, audio, link, embedded = result.content
            self.assertEqual(text.text, "a text block")
            self.assertEqual((image.data, image.mime_type), ("aW1hZ2U=", "image/png"))
            self.assertEqual((audio.data, audio.mime_type), ("YXVkaW8=", "audio/wav"))
            self.assertEqual((link.uri, link.name), ("file:///fixture/link.txt", "link.txt"))
            self.assertEqual(embedded.resource.uri, "file:///fixture/embedded.txt")
            self.assertEqual(embedded.resource.text, "embedded text")

        self.with_client(body)

    def test_failures_are_results_with_is_error(self):
        async def body(client):
            result = await client.call_tool("fail", {})
            self.assertTrue(result.is_error)
            self.assertEqual(text_of(result), "the fixture fails on purpose")

            result = await client.call_tool("crash", {})
            self.assertTrue(result.is_error)
            self.assertTrue(text_of(result))
            # The text of an exception that is not a ToolError stays on the server.
            self.assertNotIn("must stay on the server", text_of(result))

        self.with_client(body)

    def test_arguments_that_fail_the_input_schema(self):
        cases = [
            ("bounded", {"count": -1}),  # a field option: minimum 0
            ("add", {"a": 1}),  # a required member is missing
            ("add", {"a": "one", "b": 2}),  # wrong type
            ("add", {"a": 1, "b": 2, "surplus": True}),  # additionalProperties is false
        ]

        async def body(client):
            for name, arguments in cases:
                with self.subTest(tool=name, arguments=arguments):
                    if self.invalid_arguments == "error_result":
                        result = await client.call_tool(name, arguments)
                        self.assertTrue(result.is_error)
                        self.assertTrue(text_of(result))
                    else:
                        with self.assertRaises(MCPError) as raised:
                            await client.call_tool(name, arguments)
                        self.assertEqual(raised.exception.code, -32602)
            # Arguments that pass reach the handler.
            result = await client.call_tool("bounded", {"count": 0})
            self.assertFalse(result.is_error)
            self.assertEqual(text_of(result), "count 0")

        self.with_client(body)

    def test_an_unknown_tool_is_an_invalid_params_error(self):
        async def body(client):
            with self.assertRaises(MCPError) as raised:
                await client.call_tool("no_such_tool", {})
            self.assertEqual(raised.exception.code, -32602)
            self.assertIn("no_such_tool", raised.exception.message)

        self.with_client(body)

    def test_progress(self):
        async def body(client):
            seen = []

            async def on_progress(progress, total, message):
                seen.append((progress, total, message))

            result = await client.call_tool("count", {"steps": 3}, progress_callback=on_progress)
            self.assertEqual(text_of(result), "counted 3")
            self.assertEqual(seen, [(1, 3, "step 1"), (2, 3, "step 2"), (3, 3, "step 3")])

        self.with_client(body)

    def test_cancelling_a_call_ends_it_and_the_server_stops_the_tool(self):
        async def body(client):
            started = anyio.Event()

            async def on_progress(progress, total, message):
                started.set()

            with anyio.CancelScope() as scope:

                async def cancel_when_waiting():
                    await started.wait()
                    scope.cancel()

                async with anyio.create_task_group() as tasks:
                    tasks.start_soon(cancel_when_waiting)
                    # The tool reports once it waits, then waits for far longer than the test lasts.
                    await client.call_tool("wait", {"milliseconds": 600000}, progress_callback=on_progress)
                    self.fail("the call returned although it was cancelled")
            self.assertTrue(scope.cancelled_caught)

            # The client sent notifications/cancelled; the server's tool saw it and ended. The tool runs on another
            # thread than the one that reads input, so it is asked until it reports having been cancelled.
            stats = None
            for _ in range(PATIENCE_SECONDS * 20):
                stats = (await client.call_tool("stats", {})).structured_content
                if stats["waits_cancelled"] >= 1:
                    break
                await anyio.sleep(0.05)
            self.assertEqual(stats, {"waits_started": 1, "waits_cancelled": 1})

            # The connection is still good.
            result = await client.call_tool("echo", {"text": "still here"})
            self.assertEqual(text_of(result), "still here")

        self.with_client(body)


class LegacyEra(EraChecks, unittest.TestCase):
    mode = "legacy"
    version = "2025-11-25"


class ModernEra(EraChecks, unittest.TestCase):
    mode = "auto"
    version = "2026-07-28"


class ProcessChecks(unittest.TestCase):
    """What a client that is not an SDK sees of the process: a pipe from a shell."""

    LEGACY_INITIALIZE = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "shell", "version": "0"},
        },
    }
    TOOLS_LIST = {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}

    def run_with_input(self, messages):
        data = "".join(json.dumps(message) + "\n" for message in messages)
        done = subprocess.run(
            [FIXTURE], input=data.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=PATIENCE_SECONDS
        )
        lines = done.stdout.decode().splitlines()
        return done, [json.loads(line) for line in lines]

    def test_a_pipe_gets_every_answer_and_the_process_exits_when_input_ends(self):
        done, answers = self.run_with_input([self.LEGACY_INITIALIZE, self.TOOLS_LIST])
        self.assertEqual(done.returncode, 0)
        self.assertEqual([answer["id"] for answer in answers], [1, 2])
        self.assertEqual(answers[0]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual([tool["name"] for tool in answers[1]["result"]["tools"]], TOOL_NAMES)
        # Nothing but messages on standard output, and nothing on standard error in a normal run.
        self.assertEqual(done.stderr, b"")

    def test_a_slow_call_is_answered_before_the_process_exits(self):
        call = {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {"name": "wait", "arguments": {"milliseconds": 300}},
        }
        done, answers = self.run_with_input([self.LEGACY_INITIALIZE, call])
        self.assertEqual(done.returncode, 0)
        self.assertEqual({answer["id"] for answer in answers}, {1, 3})
        answer = next(answer for answer in answers if answer["id"] == 3)
        self.assertEqual(answer["result"]["content"][0]["text"], "waited")


if __name__ == "__main__":
    if not FIXTURE or not os.path.exists(FIXTURE):
        sys.exit("NEOMCP_STDIO_FIXTURE must name the fixture server binary")
    unittest.main(verbosity=2)
