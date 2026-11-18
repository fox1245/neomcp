#include "crypto/hmac.h"

namespace neomcp::detail {
namespace {

constexpr unsigned char inner_byte = 0x36u;
constexpr unsigned char outer_byte = 0x5cu;

}  // namespace

HmacSha256::HmacSha256(std::string_view key) noexcept {
    std::array<unsigned char, sha256_block_size> block{};
    if (key.size() > sha256_block_size) {
        const Sha256Digest hashed = sha256(key);
        for (std::size_t i = 0; i < hashed.size(); ++i) block[i] = hashed[i];
    } else {
        for (std::size_t i = 0; i < key.size(); ++i) block[i] = static_cast<unsigned char>(key[i]);
    }
    for (std::size_t i = 0; i < sha256_block_size; ++i) {
        inner_pad_[i] = static_cast<unsigned char>(block[i] ^ inner_byte);
        outer_pad_[i] = static_cast<unsigned char>(block[i] ^ outer_byte);
    }
    wipe(block.data(), block.size());
    inner_.update(inner_pad_.data(), inner_pad_.size());
}

HmacSha256::~HmacSha256() {
    wipe(inner_pad_.data(), inner_pad_.size());
    wipe(outer_pad_.data(), outer_pad_.size());
}

void HmacSha256::update(std::string_view bytes) noexcept { inner_.update(bytes); }

Sha256Digest HmacSha256::finish() noexcept {
    const Sha256Digest inner = inner_.finish();
    Sha256 outer;
    outer.update(outer_pad_.data(), outer_pad_.size());
    outer.update(inner.data(), inner.size());
    inner_.update(inner_pad_.data(), inner_pad_.size());  // ready for the next message
    return outer.finish();
}

Sha256Digest hmac_sha256(std::string_view key, std::string_view message) noexcept {
    HmacSha256 mac(key);
    mac.update(message);
    return mac.finish();
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    // Every byte is looked at, and the differences are gathered in a value the compiler must read and write each time.
    volatile unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference = static_cast<unsigned char>(difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return difference == 0;
}

}  // namespace neomcp::detail
