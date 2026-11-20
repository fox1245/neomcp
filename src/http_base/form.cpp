#include "neomcp/http/form.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "http_base/grammar.h"

namespace neomcp::http {

namespace {

using detail::is_alpha;
using detail::is_digit;

constexpr char field_separator = '&';
constexpr char name_separator = '=';
constexpr char escape = '%';
constexpr char encoded_space = '+';
constexpr char space = ' ';
constexpr std::string_view upper_hex = "0123456789ABCDEF";
constexpr std::string_view basic_prefix = "Basic ";
constexpr char credentials_separator = ':';

// The unreserved characters of RFC 3986, section 2.3: the ones that stay as they are.
constexpr bool is_unreserved(char c) noexcept {
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

std::optional<int> hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return std::nullopt;
}

// Whether the octets are UTF-8 (RFC 3629): shortest forms only, no surrogates, nothing above U+10FFFF.
bool is_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t point = 0;
        std::uint32_t lowest = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
            point = lead & 0x1fu;
            lowest = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            point = lead & 0x0fu;
            lowest = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            point = lead & 0x07u;
            lowest = 0x10000;
        } else {
            return false;
        }
        if (text.size() - i < length) return false;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0u) != 0x80u) return false;
            point = (point << 6) | (next & 0x3fu);
        }
        if (point < lowest || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
        i += length;
    }
    return true;
}

// One name or value, decoded; `base` is its offset in the whole text, for the error.
std::variant<std::string, FormError> decode_component(std::string_view text, std::size_t base) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == encoded_space) {
            out += space;
        } else if (c == escape) {
            const std::optional<int> high = i + 1 < text.size() ? hex_value(text[i + 1]) : std::nullopt;
            const std::optional<int> low = i + 2 < text.size() ? hex_value(text[i + 2]) : std::nullopt;
            if (!high || !low) return FormError{FormProblem::bad_escape, base + i};
            out += static_cast<char>((*high << 4) | *low);
            i += 2;
        } else {
            out += c;
        }
    }
    if (!is_utf8(out)) return FormError{FormProblem::invalid_utf8, base};
    return out;
}

// Base64 with the standard alphabet and padding (RFC 4648, section 4).
std::string encode_base64(std::string_view bytes) {
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static constexpr char pad = '=';
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t group = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
                                    static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
        out += alphabet[(group >> 18) & 0x3fu];
        out += alphabet[(group >> 12) & 0x3fu];
        out += alphabet[(group >> 6) & 0x3fu];
        out += alphabet[group & 0x3fu];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t group = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        out += alphabet[(group >> 18) & 0x3fu];
        out += alphabet[(group >> 12) & 0x3fu];
        out += pad;
        out += pad;
    } else if (rest == 2) {
        const std::uint32_t group = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        out += alphabet[(group >> 18) & 0x3fu];
        out += alphabet[(group >> 12) & 0x3fu];
        out += alphabet[(group >> 6) & 0x3fu];
        out += pad;
    }
    return out;
}

}  // namespace

std::string_view to_string(FormProblem problem) noexcept {
    switch (problem) {
        case FormProblem::bad_escape: return "a '%' is not followed by two hexadecimal digits";
        case FormProblem::invalid_utf8: return "a decoded name or value is not UTF-8";
    }
    return {};
}

std::string form_encode_component(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (is_unreserved(c)) {
            out += c;
        } else if (c == space) {
            out += encoded_space;
        } else {
            const auto octet = static_cast<unsigned char>(c);
            out += escape;
            out += upper_hex[octet >> 4];
            out += upper_hex[octet & 0x0fu];
        }
    }
    return out;
}

std::string form_encode(std::span<const FormField> fields) {
    std::string out;
    for (const FormField& field : fields) {
        if (!out.empty()) out += field_separator;
        out += form_encode_component(field.name);
        out += name_separator;
        out += form_encode_component(field.value);
    }
    return out;
}

std::variant<std::vector<FormField>, FormError> form_decode(std::string_view text) {
    std::vector<FormField> fields;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(field_separator, start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view piece = text.substr(start, end - start);
        if (!piece.empty()) {
            const std::size_t split = piece.find(name_separator);
            const std::string_view name = piece.substr(0, split);
            const std::string_view value =
                split == std::string_view::npos ? std::string_view() : piece.substr(split + 1);
            auto decoded_name = decode_component(name, start);
            if (auto* error = std::get_if<FormError>(&decoded_name)) return *error;
            auto decoded_value = decode_component(value, start + (split == std::string_view::npos ? 0 : split + 1));
            if (auto* error = std::get_if<FormError>(&decoded_value)) return *error;
            fields.push_back(FormField{std::move(std::get<std::string>(decoded_name)),
                                       std::move(std::get<std::string>(decoded_value))});
        }
        start = end + 1;
    }
    return fields;
}

FieldLookup find_unique_field(std::span<const FormField> fields, std::string_view name) {
    FieldLookup lookup;
    for (const FormField& field : fields) {
        if (field.name != name || field.value.empty()) continue;
        if (lookup.state == FieldLookup::State::present) {
            return FieldLookup{FieldLookup::State::repeated, {}};
        }
        lookup = FieldLookup{FieldLookup::State::present, field.value};
    }
    return lookup;
}

std::string oauth_basic_authorization(std::string_view client_id, std::string_view client_secret) {
    std::string credentials = form_encode_component(client_id);
    credentials += credentials_separator;
    credentials += form_encode_component(client_secret);
    return std::string(basic_prefix) + encode_base64(credentials);
}

}  // namespace neomcp::http
