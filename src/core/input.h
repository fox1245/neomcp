// Input requests on the wire: the requests a call asks, written for a revision; what each needs of the client; and
// the answers read back. Shared by the sources of the core; not part of the public interface.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/mechanism.h"
#include "neomcp/profile.h"

namespace neomcp::detail {

// The method of the request: elicitation/create, sampling/createMessage or roots/list.
std::string_view method_of(const InputRequest& request) noexcept;

// The capabilities the request needs, as a ClientCapabilities object: {"elicitation": {"form": {}}} and so on.
json required_capabilities(const InputRequest& request);

// Whether the client's capabilities object declares what the request needs. An `elicitation` object without `form`
// and without `url` declares forms only, as the pages that define the modes say.
bool capabilities_declare(const InputRequest& request, const json& capabilities);

// The params of the request written for the profile, where `mechanism` names the revision's tables; or why the
// revision cannot carry it (a mode or a member it lacks), as a clause for a message. Every member the revision's
// params require is there. A URL elicitation without an id gets one where the revision's request has the member.
std::variant<json, std::string> write_params(const InputRequest& request, const profile::Profile& profile,
                                             const InputMechanism& mechanism);

// Whether the call may ask `request` under the profile: the revision carries it and the client declared what it
// needs. Used both before a call asks and by CallControl::client_supports.
bool input_supported(const InputRequest& request, const json& capabilities, const profile::Profile& profile,
                     const InputMechanism& mechanism);

// An answer to a request of `method`, read from the client's result: the answer, or what is wrong with its shape.
std::variant<InputResponse, std::string> read_answer(std::string_view method, const json& result);

}  // namespace neomcp::detail
