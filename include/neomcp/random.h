// Random bytes from the operating system, and their hexadecimal text form. Used for secrets and for identifiers an
// attacker must not be able to guess, such as session ids and the keys that seal request state.
//
// The bytes come from the operating system's random source (getentropy on Linux and macOS, BCryptGenRandom on
// Windows) and from nothing else: a failure of the source is reported, never replaced by a library generator seeded
// from the clock.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace neomcp {

// `count` random bytes, or nothing when the operating system's source failed.
std::optional<std::string> random_bytes(std::size_t count);

// The lower-case hexadecimal text of `bytes`: two characters per byte, digits and 'a' to 'f'.
std::string hex_encode(std::string_view bytes);

// `byte_count` random bytes as hexadecimal text (2 * byte_count characters), or nothing when the source failed.
std::optional<std::string> random_hex(std::size_t byte_count);

}  // namespace neomcp
