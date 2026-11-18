// HMAC-SHA-256 (RFC 2104, FIPS 198-1) and a comparison of secrets in constant time. Shared by the sources of the
// library; not part of the public interface.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "crypto/sha256.h"

namespace neomcp::detail {

// A message authentication code computed over bytes given in pieces. The key may have any length: a key longer than
// a block is hashed first, as the standard says. What length a key must have to be safe is the caller's rule.
class HmacSha256 {
public:
    explicit HmacSha256(std::string_view key) noexcept;
    ~HmacSha256();
    HmacSha256(const HmacSha256&) = default;
    HmacSha256& operator=(const HmacSha256&) = default;

    void update(std::string_view bytes) noexcept;
    // The code of everything fed so far. The object starts over afterwards, with the same key.
    Sha256Digest finish() noexcept;

private:
    std::array<unsigned char, sha256_block_size> inner_pad_{};  // the key XOR ipad
    std::array<unsigned char, sha256_block_size> outer_pad_{};  // the key XOR opad
    Sha256 inner_;
};

Sha256Digest hmac_sha256(std::string_view key, std::string_view message) noexcept;

// Whether two byte strings are equal, in a time that depends on their lengths only, never on where they differ.
// Strings of different lengths are unequal (their lengths are not secret).
bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

}  // namespace neomcp::detail
