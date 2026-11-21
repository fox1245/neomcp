// The HTTP names the requests of the OAuth flow use.
#pragma once

#include <string_view>

namespace neomcp::detail::oauth {

inline constexpr std::string_view method_get = "GET";
inline constexpr std::string_view method_post = "POST";

inline constexpr std::string_view header_accept = "Accept";
inline constexpr std::string_view header_content_type = "Content-Type";
inline constexpr std::string_view header_authorization = "Authorization";

inline constexpr std::string_view media_type_json = "application/json";
inline constexpr std::string_view media_type_form = "application/x-www-form-urlencoded";

inline constexpr int status_unauthorized = 401;
inline constexpr int status_forbidden = 403;
inline constexpr int status_not_found = 404;

}  // namespace neomcp::detail::oauth
