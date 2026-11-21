// Example 1: an MCP server over standard input and output (stdio).
//
// The simplest way to run a server is as a child process of the program that uses it, which is how most desktop MCP
// clients start one. The client writes its requests to this program's standard input, one JSON message per line, and
// reads the answers from its standard output. Nothing else may be written to standard output, so a server that wants
// to say something to a person writes to standard error.
//
// The tools, the resource and the prompt are registered in demo_server.h. To try it from a shell, write these three
// lines to a file, requests.jsonl, and run ./stdio_server < requests.jsonl (the server answers the first and the
// third):
//
//   {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"shell","version":"1"}}}
//   {"jsonrpc":"2.0","method":"notifications/initialized"}
//   {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"add","arguments":{"a":2,"b":3}}}
//
// or let example 3 (client_stdio) start it and talk to it.
#include <neomcp/server.h>
#include <neomcp/stdio.h>

#include "demo_server.h"

int main() {
    // Declared before the server, so that it is destroyed after it: the handlers use it until the server is gone.
    Notes notes;
    neomcp::Server server({.name = "notes-and-calc", .version = "1.0.0"});
    add_demo_features(server, notes);

    // Serves one client until it closes our standard input, then returns 0.
    return neomcp::run_stdio(server);
}
