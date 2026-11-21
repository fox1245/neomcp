#!/usr/bin/env bash
# Runs the examples that need no person, and fails at the first one that does not exit with 0:
#
#   5 client_in_process   a server and a client in one process
#   6 elicitation         a tool that asks, and a client that answers
#   3 client_stdio        starts 1 (stdio_server) as a child process
#   4 client_http         talks to 2 (http_server), which this script starts in the background and stops at the end
#
# Examples 7 (oauth_client) and 8 (protected_server) are not run: they need a person and an authorization server.
#
# Usage: examples/run_unattended.sh <the directory that holds the built programs>
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <the directory that holds the built programs>" >&2
    exit 2
fi
programs="$1"

# A program of the examples, which must have been built.
required() {
    if [ ! -x "$programs/$1" ]; then
        echo "run_unattended: $programs/$1 was not built" >&2
        exit 1
    fi
}

run() {
    echo "== $*"
    "$@"
}

required client_in_process
required elicitation
required stdio_server
required http_server

run "$programs/client_in_process"
run "$programs/elicitation"

# The clients are built only where their transport is; a build without it leaves them out on purpose.
if [ -x "$programs/client_stdio" ]; then
    run "$programs/client_stdio" "$programs/stdio_server"
else
    echo "== client_stdio was not built; skipped"
fi

if [ -x "$programs/client_http" ]; then
    work="$(mktemp -d)"
    server_pid=""
    cleanup() {
        if [ -n "$server_pid" ]; then
            kill "$server_pid" 2>/dev/null || true
        fi
        rm -f "$work/address" "$work/server.log"
        rmdir "$work"
    }
    trap cleanup EXIT

    # Port 0: the server picks a free one and writes its address, one line, to standard output.
    echo "== http_server 0 (in the background)"
    "$programs/http_server" 0 >"$work/address" 2>"$work/server.log" &
    server_pid=$!

    waited=0
    while [ ! -s "$work/address" ]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "run_unattended: http_server ended before it listened:" >&2
            cat "$work/server.log" >&2
            exit 1
        fi
        waited=$((waited + 1))
        if [ "$waited" -gt 300 ]; then
            echo "run_unattended: http_server did not listen within 30 seconds" >&2
            exit 1
        fi
        sleep 0.1
    done
    read -r url <"$work/address"

    run "$programs/client_http" "$url"

    # SIGTERM ends the server, which then returns 0.
    echo "== stopping http_server"
    kill -TERM "$server_pid"
    status=0
    wait "$server_pid" || status=$?
    server_pid=""
    if [ "$status" -ne 0 ]; then
        echo "run_unattended: http_server exited with $status after SIGTERM" >&2
        cat "$work/server.log" >&2
        exit 1
    fi
else
    echo "== client_http was not built; skipped"
fi

echo "run_unattended: ok"
