// A client and a server in one process (neomcp::client_in_process).
#include <neomcp/client.h>
#include <neomcp/client/in_process.h>
#include <neomcp/server.h>

#include "add.h"
#include "call.h"

int main() {
    neomcp::Server server({.name = "installed-in-process", .version = "1.0.0"});
    add_tool(server);
    auto peer = neomcp::InProcessServer::create(server);
    return call_add("in_process", neomcp::Client::open({}, peer->transport()));
}
