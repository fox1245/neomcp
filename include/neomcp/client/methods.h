// The names of the requests a client sends, as the revisions name them. The client's options refer to methods by these
// constants (such as the requests whose lost answer is asked for again); the engine checks every name against the
// governing revision's wire tables before a request goes out.
#pragma once

#include <string_view>

namespace neomcp::client_method {

inline constexpr std::string_view ping = "ping";
inline constexpr std::string_view tools_list = "tools/list";
inline constexpr std::string_view tools_call = "tools/call";
inline constexpr std::string_view resources_list = "resources/list";
inline constexpr std::string_view resources_templates_list = "resources/templates/list";
inline constexpr std::string_view resources_read = "resources/read";
inline constexpr std::string_view prompts_list = "prompts/list";
inline constexpr std::string_view prompts_get = "prompts/get";
inline constexpr std::string_view completion_complete = "completion/complete";

}  // namespace neomcp::client_method
