// Base64 (RFC 4648, section 4) for what a server writes: the standard alphabet, padded with '=' to a multiple of
// four characters, without line breaks.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace neomcp {

// The Base64 text of the bytes. The text of no bytes is empty.
std::string base64_encode(std::span<const std::byte> bytes);
// The same for bytes held in a string.
std::string base64_encode(std::string_view bytes);

}  // namespace neomcp
