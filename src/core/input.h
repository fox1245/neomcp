// Input requests on the wire: the requests a call asks, written for a revision; what each needs of the client; and
// the answers read back. Shared by the sources of the core; not part of the public interface.
#pragma once

#include <optional>
#include <span>
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

// The notification that tells a server the client's roots changed, where the profile lets a client send it.
std::optional<std::string_view> roots_changed_notification(const profile::Profile& profile);

// The ClientCapabilities object that declares the kinds of `kinds` (each one as required_capabilities gives it) under
// the profile, with that the roots change when asked: a nested member only where the profile carries what it declares
// (a mode of elicitation where the form params have a mode, URLs where the profile has URL params, tools where the
// sampling params have tools, the change of roots where the client may send its notification), kept to the members of
// the profile's ClientCapabilities type.
json declared_capabilities(std::span<const InputRequest> kinds, bool roots_list_changed,
                           const profile::Profile& profile);

// The member of ClientCapabilities that declares the kind of the request at all: elicitation, sampling or roots.
std::string_view capability_of(const InputRequest& request);

// The params of the request written for the profile, where `mechanism` names the revision's tables; or why the
// revision cannot carry it (a mode or a member it lacks), as a clause for a message. Every member the revision's
// params require is there. A URL elicitation without an id gets one where the revision's request has the member.
std::variant<json, std::string> write_params(const InputRequest& request, const profile::Profile& profile,
                                             const InputMechanism& mechanism);

// Whether the call may ask `request` under the profile: the revision carries it and the client declared what it
// needs. Used both before a call asks and by CallControl::client_supports.
bool input_supported(const InputRequest& request, const json& capabilities, const profile::Profile& profile,
                     const InputMechanism& mechanism);

// The request a server asked with, read from its params: the inverse of write_params. `method` is the request's method
// and `params` its params (null when it has none). Every member the revision's params require must be there; members
// that the revision's params do not have are not read; an `elicitation/create` request without a mode is a form. An
// error says what is wrong: a method or a mode the revision does not define, a member that is missing or of the wrong
// kind, a content block of a kind that sampling does not carry.
std::variant<InputRequest, std::string> read_request(std::string_view method, const json& params,
                                                     const profile::Profile& profile, const InputMechanism& mechanism);

// The result a client answers a request with: the inverse of read_answer. Only the members the revision's result type
// has are written, and every member it requires is there; the fields of an elicitation are written for an acceptance
// only. An error when the revision does not carry the request or the answer is malformed.
std::variant<json, std::string> write_answer(const InputResponse& response, const profile::Profile& profile,
                                             const InputMechanism& mechanism);

// An answer to a request of `method`, read from the client's result: the answer, or what is wrong with its shape.
std::variant<InputResponse, std::string> read_answer(std::string_view method, const json& result);

// The method of the request whose answer `result` is, decided from its members alone: of the requests the mechanism
// lets a server ask with under the profile, the one whose result type in the profile's wire tables has all its
// required members in `result`. Empty when no request's result type fits, or more than one does.
std::optional<std::string_view> answer_method(const json& result, const profile::Profile& profile,
                                              const InputMechanism& mechanism);

}  // namespace neomcp::detail
