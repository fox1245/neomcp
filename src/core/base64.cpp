#include "core/base64.h"

#include <array>
#include <cstddef>
#include <cstdint>

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
