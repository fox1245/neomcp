// SHA-256 (FIPS 180-4). Shared by the sources of the library; not part of the public interface.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace neomcp::detail {

inline constexpr std::size_t sha256_size = 32;        // bytes of a digest
inline constexpr std::size_t sha256_block_size = 64;  // bytes of a block

using Sha256Digest = std::array<unsigned char, sha256_size>;

// A digest computed over bytes given in pieces. Feeding a message in any split gives the digest of the whole.
class Sha256 {
public:
    Sha256() noexcept;
    ~Sha256();
    Sha256(const Sha256&) = default;
    Sha256& operator=(const Sha256&) = default;

    void update(std::string_view bytes) noexcept;
    void update(const unsigned char* bytes, std::size_t count) noexcept;
    // The digest of everything fed so far. The object starts over afterwards.
    Sha256Digest finish() noexcept;

private:
    void reset() noexcept;
    void compress(const unsigned char* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, sha256_block_size> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;  // bytes fed
};

// The digest of the bytes.
Sha256Digest sha256(std::string_view bytes) noexcept;

// Overwrites the bytes with zeros in a way the compiler does not remove, for buffers that held key material.
void wipe(void* bytes, std::size_t count) noexcept;

}  // namespace neomcp::detail
