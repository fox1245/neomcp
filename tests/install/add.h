// The tool every program of this project serves or calls.
#pragma once

#include <neomcp/server.h>

struct AddArgs {
    int a;
    int b;
};
NEOMCP_DESCRIBE(AddArgs, a, "first addend", b, "second addend")

inline void add_tool(neomcp::Server& server) {
    server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });
}
