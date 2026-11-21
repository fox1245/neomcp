// The server of the application-error tests as a program over stdio (registrations.h), serving only the revisions named
// on its command line (every built-in one when none is named).
//
//   neomcp_application_errors_server [revision...]
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "neomcp/stdio.h"
#include "registrations.h"

int main(int argc, char** argv) {
    neomcp::ServerOptions options{.name = "application-errors", .version = "1.0.0"};
    for (int at = 1; at < argc; ++at) {
        auto profile = neomcp::profile::builtin(argv[at]);
        if (!profile) {
            std::fputs("neomcp_application_errors_server: no built-in profile of that revision\n", stderr);
            return 2;
        }
        options.profiles.push_back(std::move(profile));
    }
    neomcp::Server server(std::move(options));
    application_errors::register_failing(server);
    return neomcp::run_stdio(server);
}
