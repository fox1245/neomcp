#!/usr/bin/env python3
"""Run the README's HTTP server (calc_http) on a free port, call its tool once, and stop it with SIGTERM.

    python3 -I tests/consumer/check_http.py PATH_TO_CALC_HTTP

First the server is started with arguments that are not a port: it must refuse each with a line on standard error and
a nonzero exit code, and serve nothing. Then the call is a tools/call in the form of revision 2026-07-28: no handshake, the revision and the client named in the
request's _meta and in the headers. The server must answer it and, on SIGTERM, exit with 0. POSIX only: on Windows a
process cannot be sent SIGTERM from Python (terminate() ends it without letting it exit by itself).
"""

import json
import socket
import subprocess
import sys
import time
import urllib.request

REVISION = "2026-07-28"
# Arguments that are not a port: out of the range of a port, not a number, empty, signed, followed by text, or too
# long for any integer.
NOT_A_PORT = ("65536", "70000", "-1", "+8080", "http", "", " 80", "8080x", "99999999999999999999")
START_TIMEOUT = 30.0
STOP_TIMEOUT = 15.0
REQUEST = {
    "jsonrpc": "2.0", "id": 7, "method": "tools/call",
    "params": {"name": "add", "arguments": {"a": 2, "b": 3},
               "_meta": {"io.modelcontextprotocol/protocolVersion": REVISION,
                         "io.modelcontextprotocol/clientInfo": {"name": "consumer-check", "version": "1"},
                         "io.modelcontextprotocol/clientCapabilities": {}}},
}
EXPECTED = {"jsonrpc": "2.0", "id": 7,
            "result": {"content": [{"type": "text", "text": "5"}], "structuredContent": {"result": 5},
                       "resultType": "complete"}}


def holds(actual, expected):
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(key in actual and holds(actual[key], value)
                                                for key, value in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(
            holds(a, e) for a, e in zip(actual, expected))
    return actual == expected and type(actual) is type(expected)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_until_listening(process, port):
    deadline = time.monotonic() + START_TIMEOUT
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise SystemExit(f"the server ended with code {process.returncode} before it listened")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit("the server did not listen in time")


def call(port):
    body = json.dumps(REQUEST).encode()
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/mcp", data=body, method="POST",
        headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                 "MCP-Protocol-Version": REVISION, "Mcp-Method": "tools/call", "Mcp-Name": "add"})
    with urllib.request.urlopen(request, timeout=30) as reply:
        return reply.status, reply.headers.get("Content-Type", ""), json.loads(reply.read())


def refusals(program):
    """The ways in which the program does not refuse each argument that is not a port."""
    errors = []
    for argument in NOT_A_PORT:
        process = subprocess.Popen([program, argument], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            out, err = process.communicate(timeout=START_TIMEOUT)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            errors.append(f"the argument {argument!r} was not refused: the server kept running")
            continue
        if process.returncode == 0:
            errors.append(f"the argument {argument!r} was refused with exit code 0")
        if not err.strip():
            errors.append(f"the argument {argument!r} was refused without a line on standard error")
        if out:
            errors.append(f"the argument {argument!r} made the server write to standard output")
    return errors


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    refused = refusals(sys.argv[1])
    if refused:
        print("HTTP check FAILED:\n  " + "\n  ".join(refused))
        return 1
    port = free_port()
    process = subprocess.Popen([sys.argv[1], str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    errors = []
    try:
        wait_until_listening(process, port)
        status, content_type, answer = call(port)
        if status != 200:
            errors.append(f"HTTP status {status}, expected 200")
        if not content_type.startswith("application/json"):
            errors.append(f"content type {content_type!r}, expected application/json")
        if not holds(answer, EXPECTED):
            errors.append(f"answer is {answer}, expected {EXPECTED}")
        process.terminate()  # SIGTERM
        try:
            code = process.wait(timeout=STOP_TIMEOUT)
        except subprocess.TimeoutExpired:
            errors.append(f"the server did not stop within {STOP_TIMEOUT} s of SIGTERM")
            code = None
        if code is not None and code != 0:
            errors.append(f"exit code {code} after SIGTERM, expected 0")
        if process.stdout.read():
            errors.append("the server wrote to standard output")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    if errors:
        print("HTTP check FAILED:\n  " + "\n  ".join(errors))
        print("standard error of the server: " + process.stderr.read().decode("utf-8", "replace"))
        return 1
    print(f"HTTP check passed: {len(NOT_A_PORT)} arguments that are not a port were refused, tools/call answered on port {port}, SIGTERM stopped the server with exit code 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
