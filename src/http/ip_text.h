// Whether a text can be an IP address at all, decided before the platform's address parser reads it. The platform
// parsers differ in what else they accept (brackets, a port after the address, shortened IPv4 forms), and an address
// must read the same on every platform.
#pragma once

#include <cstddef>
#include <string_view>

namespace neomcp::http::detail {

// Four decimal numbers from 0 to 255 separated by dots, each without a leading zero.
constexpr bool is_ipv4_text(std::string_view text) noexcept {
    constexpr std::size_t parts_expected = 4;
    constexpr std::size_t max_digits = 3;
    constexpr unsigned max_part = 255;
    std::size_t parts = 0;
    std::size_t at = 0;
    while (true) {
        std::size_t digits = 0;
        unsigned value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (digits == 1 && value == 0) return false;  // a leading zero
            value = value * 10 + static_cast<unsigned>(text[at] - '0');
            ++digits;
            ++at;
            if (digits > max_digits) return false;
        }
        if (digits == 0 || value > max_part) return false;
        ++parts;
        if (at == text.size()) return parts == parts_expected;
        if (text[at] != '.' || parts == parts_expected) return false;
        ++at;
    }
}

// Only the characters an IPv6 address has: hexadecimal digits, ':' and, for an IPv4 address inside it, '.'; and at
// least one ':'. Whether the text is a valid address is then the address parser's decision.
constexpr bool has_ipv6_characters(std::string_view text) noexcept {
    bool colon = false;
    for (char c : text) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (c == ':') {
            colon = true;
        } else if (!hex && c != '.') {
            return false;
        }
    }
    return colon;
}

}  // namespace neomcp::http::detail
