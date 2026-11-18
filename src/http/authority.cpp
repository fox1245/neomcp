#include <asio/ip/address.hpp>

#include <string>
#include <system_error>
#include <utility>

#include "neomcp/http/policy.h"

namespace neomcp::http {

namespace {

constexpr std::size_t max_port_digits = 5;
constexpr unsigned long max_port = 65535;

constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool is_ascii_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool is_name_character(char c) noexcept {
    return is_ascii_alpha(c) || is_ascii_digit(c) || c == '-' || c == '_' || c == '~' || c == '.';
}

// A host name: name characters only, no empty label. `name` has had its one trailing dot removed already.
bool valid_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    bool label_has_character = false;
    for (char c : name) {
        if (!is_name_character(c)) return false;
        if (c == '.') {
            if (!label_has_character) return false;
            label_has_character = false;
        } else {
            label_has_character = true;
        }
    }
    return label_has_character;  // a name does not end in '.' here: that is an empty label
}

std::optional<std::uint16_t> parse_port(std::string_view digits) noexcept {
    if (digits.empty() || digits.size() > max_port_digits) return std::nullopt;
    unsigned long value = 0;
    for (char c : digits) {
        if (!is_ascii_digit(c)) return std::nullopt;
        value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    if (value == 0 || value > max_port) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::string Authority::to_string() const {
    std::string text;
    if (ipv6) {
        text += '[';
        text += host;
        text += ']';
    } else {
        text = host;
    }
    if (port) {
        text += ':';
        text += std::to_string(*port);
    }
    return text;
}

std::optional<Authority> parse_authority(std::string_view text) {
    if (text.empty()) return std::nullopt;
    Authority authority;
    std::string_view port_text;
    bool has_port = false;

    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        const std::string_view inside = text.substr(1, close - 1);
        if (inside.empty() || inside.find('%') != std::string_view::npos) return std::nullopt;
        std::error_code error;
        const asio::ip::address_v6 address = asio::ip::make_address_v6(std::string(inside), error);
        if (error) return std::nullopt;
        authority.host = address.to_string();
        authority.ipv6 = true;
        std::string_view rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return std::nullopt;
            has_port = true;
            port_text = rest.substr(1);
        }
    } else {
        const std::size_t colon = text.find(':');
        std::string_view name = text.substr(0, colon);
        if (colon != std::string_view::npos) {
            has_port = true;
            port_text = text.substr(colon + 1);
        }
        if (!name.empty() && name.back() == '.') name.remove_suffix(1);
        if (!valid_name(name)) return std::nullopt;  // also rejects '@', '/', '?', '#', '[' and a second ':'
        authority.host.reserve(name.size());
        for (char c : name) authority.host += ascii_lower(c);
    }

    if (has_port) {
        const std::optional<std::uint16_t> port = parse_port(port_text);
        if (!port) return std::nullopt;
        authority.port = port;
    }
    return authority;
}

std::optional<Origin> parse_origin(std::string_view text) {
    constexpr std::string_view separator = "://";
    const std::size_t at = text.find(separator);
    if (at == std::string_view::npos || at == 0) return std::nullopt;
    const std::string_view scheme = text.substr(0, at);
    if (!is_ascii_alpha(scheme.front())) return std::nullopt;
    Origin origin;
    for (char c : scheme) {
        if (!(is_ascii_alpha(c) || is_ascii_digit(c) || c == '+' || c == '-' || c == '.')) return std::nullopt;
        origin.scheme += ascii_lower(c);
    }
    std::optional<Authority> authority = parse_authority(text.substr(at + separator.size()));
    if (!authority) return std::nullopt;
    origin.authority = std::move(*authority);
    return origin;
}

}  // namespace neomcp::http
