// What every cell of the application-error tests checks: each handler of registrations.h, called through a Client with
// codes an application may use (the error comes back as the handler gave it, in ClientError::code, ::message and
// ::data) and with codes it may not (an internal error with a fixed message, and nothing of the handler's).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "neomcp/client.h"

namespace application_errors {

// The built-in revisions that a Client of `transport` can speak with a server over it.
std::vector<std::string> revisions_defining(neomcp::profile::Transport transport);

// The options of a Client that speaks only `revision`.
neomcp::ClientOptions client_options(const std::string& revision);

// Runs the checks on an open client whose governing revision must be `revision`. Failures are reported
// through GoogleTest, each under the name of the handler and the code.
void run_checks(neomcp::Client& client, const std::string& revision);

// A name for a test parameter: letters, digits and underscores.
std::string test_name(std::string text);

}  // namespace application_errors
