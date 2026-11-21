// A client that starts the server named on its command line as a child process (neomcp::client_stdio).
#include <iostream>
#include <utility>

#include <neomcp/client.h>
#include <neomcp/client/stdio.h>

#include "call.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: stdio_client <server program>\n";
        return 2;
    }
    neomcp::ChildProcess process;
    process.program = argv[1];
    return call_add("stdio_client", neomcp::Client::open({}, std::move(process)));
}
