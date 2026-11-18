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

// Base64 with the URL and filename safe alphabet (RFC 4648, section 5), without padding: what the library writes in
// tokens that travel inside JSON strings and URLs.
std::string encode_base64url(std::string_view bytes);
// The bytes such a text encodes, or empty when the text is not in that strict form: the URL-safe alphabet only, no
// padding, a length that is not one more than a multiple of four, and zero in the bits the last digit carries beyond
// the data. So every byte string has exactly one text this accepts.
std::optional<std::string> decode_base64url(std::string_view text);

}  // namespace neomcp::detail
