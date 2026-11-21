// Which codes an application may answer a request with, under the revision that governs the request. Shared by the
// session and the task runner; not part of the public interface.
#pragma once

#include <string>

#include "neomcp/profile.h"
#include "neomcp/tool_host.h"

namespace neomcp::detail {

// Why an application may not use `code` under `profile`; empty when it may. The code is refused when it lies in the
// range JSON-RPC reserves for itself (the protocol's own codes among them) unless it is one of the codes that range
// leaves to implementations, and then still when the profile (extensions composed into it included) names it as one of
// its error codes or lists it among the codes a sender must not emit. The text says why, without the application's
// message or data.
std::string refused_application_code(const profile::Profile& profile, int code);

// The text that notes an application error whose answer would be longer than `limit` bytes.
std::string oversized_application_error(const ApplicationFailure& failure, std::size_t size, std::size_t limit);

}  // namespace neomcp::detail
