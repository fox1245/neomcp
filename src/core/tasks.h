// The built-in mechanisms of the slot `tasks` (neomcp/mechanism.h). Shared by the sources of the core; not part of the
// public interface.
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "neomcp/mechanism.h"

namespace neomcp::detail {

// `none`: no call is answered with a task.
std::unique_ptr<TaskMechanism> make_no_tasks();
// `augmented_request`: the requester asks for a task in the call, of a tool whose listing says it may run as one.
std::unique_ptr<TaskMechanism> make_augmented_request_tasks();
// `server_directed`: the server decides, per call, for a requester that declares the extension in the request.
std::unique_ptr<TaskMechanism> make_server_directed_tasks();

// The time as ISO 8601 in UTC, to the millisecond: 2026-07-28T10:30:00.000Z.
std::string iso_8601(std::chrono::system_clock::time_point at);

}  // namespace neomcp::detail
