#!/usr/bin/env python3
"""Run the README's stdio server (calc) on one initialize and one tools/call and check its answers.

    python3 -I tests/consumer/check_stdio.py PATH_TO_CALC

The requests are written to standard input, which is then closed; the server must answer both, write nothing else
to standard output, and exit with 0.
"""

import json
import subprocess
import sys

REQUESTS = [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize",
     "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "consumer-check", "version": "1"}}},
    {"jsonrpc": "2.0", "method": "notifications/initialized"},
    {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "add", "arguments": {"a": 2, "b": 3}}},
]

# What each answer must hold. A dictionary names members the answer must have, with these values and perhaps more
# members; anything else must be equal.
EXPECTED = {
    1: {"jsonrpc": "2.0", "id": 1,
        "result": {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                   "serverInfo": {"name": "calc", "version": "1.0.0"}}},
    2: {"jsonrpc": "2.0", "id": 2,
        "result": {"content": [{"type": "text", "text": "5"}], "structuredContent": {"result": 5}}},
}


def holds(actual, expected):
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(key in actual and holds(actual[key], value)
                                                for key, value in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(
            holds(a, e) for a, e in zip(actual, expected))
    return actual == expected and type(actual) is type(expected)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    request_bytes = "".join(json.dumps(request) + "\n" for request in REQUESTS).encode()
    done = subprocess.run([sys.argv[1]], input=request_bytes, capture_output=True, timeout=60, check=False)
    errors = []
    if done.returncode != 0:
        errors.append(f"exit code {done.returncode}, expected 0")
    answers = {}
    for line in done.stdout.decode("utf-8", "replace").splitlines():
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            errors.append(f"a line of standard output is not JSON: {line!r}")
            continue
        answers[message.get("id") if isinstance(message, dict) else None] = message
    if len(answers) != len(EXPECTED):
        errors.append(f"expected {len(EXPECTED)} answers, got {len(answers)}")
    for identifier, expected in EXPECTED.items():
        if identifier not in answers:
            errors.append(f"no answer to request {identifier}")
        elif not holds(answers[identifier], expected):
            errors.append(f"answer to request {identifier} is {answers[identifier]}, expected {expected}")
    if errors:
        print("stdio check FAILED:\n  " + "\n  ".join(errors))
        print("standard error of the server: " + done.stderr.decode("utf-8", "replace"))
        return 1
    print("stdio check passed: initialize and tools/call answered, exit code 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
