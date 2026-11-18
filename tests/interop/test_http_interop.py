"""Interoperability of the Streamable HTTP server with the official Python SDK's client.

The test starts the conformance fixture server (tests/conformance/everything_server.cpp) once, on a free port of
127.0.0.1, and drives it through the SDK's `Client` with the endpoint's URL, in both eras against the same process:

* legacy: `mode="legacy"`, the client sends `initialize` and keeps the session the server gives it;
* modern: `mode="auto"`, the client probes `server/discover` first and adopts the newest modern revision.

The path of the fixture binary comes from the environment variable NEOMCP_HTTP_FIXTURE. Run it with the interpreter
that has tests/interop/requirements.txt installed.
"""

import os
import subprocess
import sys
import unittest

import anyio
from mcp import Client

FIXTURE = os.environ.get("NEOMCP_HTTP_FIXTURE", "")

# Long enough for a loaded machine; reached only when something is broken.
PATIENCE_SECONDS = 60

# The tools these tests rely on. The server lists more: it is the one the conformance suite runs against.
TOOL_NAMES = [
    "test_simple_text",
    "test_image_content",
    "test_audio_content",
    "test_embedded_resource",
    "test_multiple_content_types",
    "test_error_handling",
    "test_tool_with_progress",
    "test_tool_with_logging",
    "test_logging_tool",
]


def text_of(result):
    """The text of the first content block."""
    return result.content[0].text


class Fixture:
    """The fixture server process, started once for every test of the module."""

    process = None
    url = None

    @classmethod
    def start(cls):
        if cls.process is not None:
            return
        cls.process = subprocess.Popen([FIXTURE, "0"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        # The first line on standard error is the port it chose.
        line = cls.process.stderr.readline().decode().strip()
        if not line.isdigit():
            cls.stop()
            raise RuntimeError(f"the fixture did not report its port: {line!r}")
        cls.url = f"http://127.0.0.1:{line}/mcp"

    @classmethod
    def stop(cls):
        if cls.process is None:
            return
        cls.process.terminate()
        try:
            cls.process.wait(timeout=PATIENCE_SECONDS)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait()
        cls.process.stderr.close()
        cls.process = None


def setUpModule():
    Fixture.start()


def tearDownModule():
    Fixture.stop()


class EraChecks:
    """The checks that hold in both eras. A subclass says how the client opens and what it must negotiate."""

    mode = None  # the SDK's connect mode
    version = None  # the revision the client must end up with

    def with_client(self, body):
        async def main():
            with anyio.fail_after(PATIENCE_SECONDS):
                async with Client(Fixture.url, mode=self.mode) as client:
                    await body(client)

        anyio.run(main)

    def test_negotiates_the_revision(self):
        async def body(client):
            self.assertEqual(client.protocol_version, self.version)
            self.assertEqual(client.server_info.name, "neomcp-conformance-fixture")
            self.assertIsNotNone(client.server_capabilities.tools)

        self.with_client(body)

    def test_lists_the_tools(self):
        async def body(client):
            listing = await client.list_tools()
            names = [tool.name for tool in listing.tools]
            self.assertEqual(len(names), len(set(names)))
            self.assertEqual([name for name in TOOL_NAMES if name not in names], [])
            for tool in listing.tools:
                self.assertTrue(tool.description)
                self.assertEqual(tool.input_schema["type"], "object")

        self.with_client(body)

    def test_a_text_call(self):
        async def body(client):
            result = await client.call_tool("test_simple_text", {})
            self.assertFalse(result.is_error)
            self.assertEqual(text_of(result), "This is a simple text response for testing.")

        self.with_client(body)

    def test_content_of_several_kinds(self):
        async def body(client):
            result = await client.call_tool("test_multiple_content_types", {})
            self.assertEqual([block.type for block in result.content], ["text", "image", "resource"])
            self.assertEqual(result.content[1].mime_type, "image/png")
            self.assertEqual(result.content[2].resource.uri, "test://embedded-resource")

            audio = await client.call_tool("test_audio_content", {})
            self.assertEqual(audio.content[0].type, "audio")
            self.assertEqual(audio.content[0].mime_type, "audio/wav")

        self.with_client(body)

    def test_a_tool_execution_error_is_a_result(self):
        async def body(client):
            result = await client.call_tool("test_error_handling", {})
            self.assertTrue(result.is_error)
            self.assertTrue(text_of(result))

        self.with_client(body)

    def test_progress_arrives_before_the_answer(self):
        async def body(client):
            seen = []

            async def on_progress(progress, total, message):
                seen.append((progress, total))

            result = await client.call_tool("test_tool_with_progress", {}, progress_callback=on_progress)
            self.assertEqual(text_of(result), "Progress test completed")
            self.assertEqual(seen, [(0, 100), (50, 100), (100, 100)])

        self.with_client(body)

    def test_two_clients_at_once(self):
        async def body(client):
            async def other():
                async with Client(Fixture.url, mode=self.mode) as second:
                    result = await second.call_tool("test_simple_text", {})
                    self.assertFalse(result.is_error)

            async with anyio.create_task_group() as tasks:
                tasks.start_soon(other)
                result = await client.call_tool("test_simple_text", {})
                self.assertFalse(result.is_error)

        self.with_client(body)


class LegacyEra(EraChecks, unittest.TestCase):
    mode = "legacy"
    version = "2025-11-25"


class ModernEra(EraChecks, unittest.TestCase):
    mode = "auto"
    version = "2026-07-28"


if __name__ == "__main__":
    if not FIXTURE or not os.path.exists(FIXTURE):
        sys.exit("NEOMCP_HTTP_FIXTURE must name the fixture server binary")
    unittest.main(verbosity=2)
