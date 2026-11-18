#include "crypto/sha256.h"

namespace neomcp::detail {
namespace {

// The round constants: the first 32 bits of the fractional parts of the cube roots of the first 64 primes.
constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

// The initial hash value: the first 32 bits of the fractional parts of the square roots of the first 8 primes.
constexpr std::array<std::uint32_t, 8> initial_state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32u - bits));
}

constexpr std::uint32_t load_big_endian(const unsigned char* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

constexpr unsigned char byte_of(std::uint64_t value, unsigned shift) noexcept {
    return static_cast<unsigned char>((value >> shift) & 0xFFu);
}

}  // namespace

Sha256::Sha256() noexcept { reset(); }

Sha256::~Sha256() {
    wipe(state_.data(), sizeof(state_));
    wipe(buffer_.data(), buffer_.size());
}

void Sha256::reset() noexcept {
    state_ = initial_state;
    buffered_ = 0;
    length_ = 0;
}

void Sha256::compress(const unsigned char* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i) schedule[i] = load_big_endian(block + 4 * i);
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            rotate_right(schedule[i - 15], 7) ^ rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        const std::uint32_t s1 =
            rotate_right(schedule[i - 2], 17) ^ rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t first = h + sum1 + choose + round_constants[i] + schedule[i];
        const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
    wipe(schedule.data(), sizeof(schedule));
}

void Sha256::update(std::string_view bytes) noexcept {
    update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

void Sha256::update(const unsigned char* bytes, std::size_t count) noexcept {
    length_ += count;
    if (buffered_ > 0) {
        const std::size_t room = sha256_block_size - buffered_;
        const std::size_t take = count < room ? count : room;
        for (std::size_t i = 0; i < take; ++i) buffer_[buffered_ + i] = bytes[i];
        buffered_ += take;
        bytes += take;
        count -= take;
        if (buffered_ < sha256_block_size) return;
        compress(buffer_.data());
        buffered_ = 0;
    }
    while (count >= sha256_block_size) {
        compress(bytes);
        bytes += sha256_block_size;
        count -= sha256_block_size;
    }
    for (std::size_t i = 0; i < count; ++i) buffer_[i] = bytes[i];
    buffered_ = count;
}

Sha256Digest Sha256::finish() noexcept {
    const std::uint64_t bits = length_ * 8u;
    // The padding: one 1 bit, zeros up to 56 bytes into a block, then the length in bits as 64 bits big-endian.
    constexpr std::size_t length_offset = sha256_block_size - 8;
    buffer_[buffered_++] = 0x80u;
    if (buffered_ > length_offset) {
        while (buffered_ < sha256_block_size) buffer_[buffered_++] = 0;
        compress(buffer_.data());
        buffered_ = 0;
    }
    while (buffered_ < length_offset) buffer_[buffered_++] = 0;
    for (unsigned i = 0; i < 8; ++i) buffer_[length_offset + i] = byte_of(bits, 56u - 8u * i);
    compress(buffer_.data());

    Sha256Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[4 * i] = byte_of(state_[i], 24);
        digest[4 * i + 1] = byte_of(state_[i], 16);
        digest[4 * i + 2] = byte_of(state_[i], 8);
        digest[4 * i + 3] = byte_of(state_[i], 0);
    }
    wipe(buffer_.data(), buffer_.size());
    reset();
    return digest;
}

Sha256Digest sha256(std::string_view bytes) noexcept {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

void wipe(void* bytes, std::size_t count) noexcept {
    volatile unsigned char* target = static_cast<volatile unsigned char*>(bytes);
    for (std::size_t i = 0; i < count; ++i) target[i] = 0;
}

}  // namespace neomcp::detail
