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

// The text of an IPv6 address: groups of one to four hexadecimal digits separated by ':', eight of them, or fewer
// with one "::" standing for the missing ones; the last two groups may be written as a dotted quad. Nothing else:
// no brackets, no zone identifier, no port.
constexpr bool is_ipv6_text(std::string_view text) noexcept {
    constexpr std::size_t groups_total = 8;
    constexpr std::size_t groups_of_a_dotted_quad = 2;
    constexpr std::size_t max_digits = 4;
    if (text.empty()) return false;
    std::size_t groups = 0;
    bool compressed = false;
    std::size_t at = 0;
    if (text.size() >= 2 && text[0] == ':' && text[1] == ':') {
        compressed = true;
        at = 2;
    } else if (text[0] == ':') {
        return false;
    }
    while (at < text.size()) {
        const std::size_t start = at;
        while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || (text[at] >= 'a' && text[at] <= 'f') ||
                                    (text[at] >= 'A' && text[at] <= 'F'))) {
            ++at;
        }
        if (at < text.size() && text[at] == '.') {
            // A dotted quad is the rest of the text.
            if (!is_ipv4_text(text.substr(start))) return false;
            groups += groups_of_a_dotted_quad;
            break;
        }
        if (at == start || at - start > max_digits) return false;
        ++groups;
        if (at == text.size()) break;
        if (text[at] != ':') return false;
        ++at;
        if (at == text.size()) return false;  // a single ':' at the end
        if (text[at] == ':') {
            if (compressed) return false;
            compressed = true;
            ++at;
        }
    }
    return compressed ? groups < groups_total : groups == groups_total;
}

}  // namespace neomcp::http::detail
