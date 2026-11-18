// Base64 (RFC 4648, section 4) in its strict form. Shared by the sources of the core; not part of the public
// interface.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace neomcp::detail {

// The bytes a Base64 text encodes, or empty when the text is not in the strict form: the standard alphabet only (no
// whitespace, no line breaks, no URL-safe digits), padded with '=' to a multiple of four characters, and zero in the
// bits the last digit carries beyond the data. So every byte string has exactly one text this accepts.
std::optional<std::string> decode_base64(std::string_view text);

}  // namespace neomcp::detail
