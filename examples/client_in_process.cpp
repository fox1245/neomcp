// Example 5: a server and a client in one process, with no I/O. The smallest complete program.
//
// neomcp::InProcessServer connects a client to a server through memory: no pipe, no socket, no child process. The
// server sees exactly what it would see over a real transport, so this is the way to test a server, and a client
// against a server, without starting anything. The program has no command line and exits with 0 when the call
// returns what it should.
#include <iostream>
#include <utility>

#include <neomcp/client.h>
#include <neomcp/client/in_process.h>
#include <neomcp/server.h>

struct AddArgs {
    int a;
    int b;
};
NEOMCP_DESCRIBE(AddArgs, a, "first addend", b, "second addend")

int main() {
    // The server, with one tool.
    neomcp::Server server({.name = "calc", .version = "1.0.0"});
    server.tool<AddArgs>("add", "Add two integers", [](const AddArgs& args) { return args.a + args.b; });

    // The peer that connects a client to it, and the client that opens through the peer's transport. The server is
    // declared first, so it is destroyed last.
    auto peer = neomcp::InProcessServer::create(server);
    auto opened = neomcp::Client::open({}, peer->transport());
    if (!opened) {
        std::cerr << "client_in_process: the client did not open: " << opened.error().message << '\n';
        return 1;
    }

    // A typed call: AddArgs goes out as the tool's arguments, and the number that comes back is decoded as an int.
    const auto sum = opened->call<int>("add", AddArgs{2, 3});
    if (!sum) {
        std::cerr << "client_in_process: the call failed: " << sum.error().message << '\n';
        return 1;
    }
    if (sum->is_error || !sum->value || *sum->value != 5) {
        std::cerr << "client_in_process: the tool did not answer 5\n";
        return 1;
    }
    std::cout << "2 + 3 = " << *sum->value << '\n';
    return 0;
}
