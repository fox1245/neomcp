#include "core/base64.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "neomcp/base64.h"

namespace neomcp::detail {
namespace {

constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char padding = '=';
constexpr int not_a_digit = -1;
constexpr int bits_per_digit = 6;
constexpr int bits_per_byte = 8;

// The value of each byte as a digit of the alphabet, or not_a_digit.
constexpr std::array<int, 256> make_digit_values() {
    std::array<int, 256> values{};
    for (auto& value : values) value = not_a_digit;
    for (std::size_t i = 0; i < alphabet.size(); ++i) values[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    return values;
}

constexpr std::array<int, 256> digit_values = make_digit_values();

}  // namespace

std::optional<std::string> decode_base64(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    // A non-empty text has at least four characters here, so the last two can be read.
    std::size_t padded = 0;
    if (!text.empty() && text.back() == padding) padded = text[text.size() - 2] == padding ? 2 : 1;
    const std::size_t digits = text.size() - padded;

    std::string out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;  // only the low `bits` bits are pending
    int bits = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const int value = digit_values[static_cast<unsigned char>(text[i])];
        if (value == not_a_digit) return std::nullopt;  // also a padding character before the end
        buffer = (buffer << bits_per_digit) | static_cast<std::uint32_t>(value);
        bits += bits_per_digit;
        if (bits >= bits_per_byte) {
            bits -= bits_per_byte;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFFu));
        }
    }
    // Two or four bits are left over when the text is padded; another encoder could set them, so they must be zero.
    if (bits > 0 && (buffer & ((std::uint32_t{1} << bits) - 1)) != 0) return std::nullopt;
    return out;
}

}  // namespace neomcp::detail

namespace neomcp {

std::string base64_encode(std::span<const std::byte> bytes) {
    constexpr std::string_view digits = detail::alphabet;
    constexpr std::size_t group_bytes = 3;
    constexpr int bits_per_digit = 6;
    constexpr unsigned digit_mask = 0x3Fu;
    std::string out;
    out.reserve((bytes.size() + group_bytes - 1) / group_bytes * 4);
    std::size_t i = 0;
    for (; i + group_bytes <= bytes.size(); i += group_bytes) {
        const std::uint32_t group = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(bytes[i + 2]);
        for (int shift = 3 * bits_per_digit; shift >= 0; shift -= bits_per_digit) {
            out.push_back(digits[(group >> shift) & digit_mask]);
        }
    }
    const std::size_t left = bytes.size() - i;
    if (left > 0) {
        std::uint32_t group = static_cast<std::uint32_t>(bytes[i]) << 16;
        if (left == 2) group |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
        out.push_back(digits[(group >> 18) & digit_mask]);
        out.push_back(digits[(group >> 12) & digit_mask]);
        out.push_back(left == 2 ? digits[(group >> 6) & digit_mask] : '=');
        out.push_back('=');
    }
    return out;
}

std::string base64_encode(std::string_view bytes) {
    return base64_encode(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
}

}  // namespace neomcp
