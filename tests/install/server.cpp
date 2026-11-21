// A stdio server (neomcp::server, neomcp::stdio). The stdio client of this project starts it as a child process.
#include <neomcp/server.h>
#include <neomcp/stdio.h>

#include "add.h"

int main() {
    neomcp::Server server({.name = "installed-server", .version = "1.0.0"});
    add_tool(server);
    return neomcp::run_stdio(server);
}
