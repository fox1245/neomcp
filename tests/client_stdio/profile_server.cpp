// The library's own server over stdio, serving only the revisions named on its command line (all built-in ones when
// none is named), with one tool. The tests of the child-process transport start it to open contact with a server of
// one kind only.
//
// Usage: neomcp_client_stdio_profile_server [revision...]
#include <iostream>
#include <string>

#include "neomcp/describe.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "neomcp/stdio.h"

struct SumArgs {
    int a;
    int b;
};
NEOMCP_DESCRIBE(SumArgs,
    a, "first addend",
    b, "second addend")

int main(int argc, char** argv) {
    neomcp::ServerOptions options;
    options.name = "neomcp-profile-fixture";
    options.version = "0.0.1";
    for (int at = 1; at < argc; ++at) {
        auto profile = neomcp::profile::builtin(argv[at]);
        if (!profile) {
            std::cerr << "no built-in profile of that revision\n";
            return 2;
        }
        options.profiles.push_back(std::move(profile));
    }
    neomcp::Server server(std::move(options));
    server.tool<SumArgs>("sum", "Add two integers", [](const SumArgs& args) { return std::to_string(args.a + args.b); });
    return neomcp::run_stdio(server);
}
