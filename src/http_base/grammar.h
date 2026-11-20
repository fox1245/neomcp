// Pieces of the grammar of RFC 9110 (section 5.6) that more than one parser of this target reads: the characters of
// a token, and a cursor over one value that reads tokens, whitespace and quoted strings. Not part of the public
// interface.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace neomcp::http::detail {

constexpr bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// tchar of RFC 9110 section 5.6.2.
constexpr bool is_tchar(char c) noexcept {
    if (is_alpha(c) || is_digit(c)) return true;
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~': return true;
        default: return false;
    }
}

constexpr char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string lowered(std::string text) {
    for (char& c : text) c = lower(c);
    return text;
}

// Reads one value from left to right; every reader either consumes what it read or reports that nothing fits.
class Cursor {
public:
    explicit Cursor(std::string_view text) : text_(text) {}

    bool done() const noexcept { return at_ >= text_.size(); }
    char peek() const noexcept { return text_[at_]; }
    std::size_t position() const noexcept { return at_; }
    void reset(std::size_t position) noexcept { at_ = position; }
    bool take(char c) noexcept {
        if (done() || text_[at_] != c) return false;
        ++at_;
        return true;
    }
    void skip_whitespace() noexcept {  // OWS
        while (!done() && (text_[at_] == ' ' || text_[at_] == '\t')) ++at_;
    }
    // A token, as written; empty when there is none here.
    std::string token() {
        const std::size_t start = at_;
        while (!done() && is_tchar(text_[at_])) ++at_;
        return std::string(text_.substr(start, at_ - start));
    }
    // A quoted-string starting here, without its quotes and with its escapes resolved; nothing when it is malformed.
    std::optional<std::string> quoted_string() {
        if (!take('"')) return std::nullopt;
        std::string out;
        while (!done()) {
            const auto c = static_cast<unsigned char>(text_[at_++]);
            if (c == '"') return out;
            if (c == '\\') {
                if (done()) return std::nullopt;
                const auto escaped = static_cast<unsigned char>(text_[at_++]);
                // quoted-pair: HTAB, SP, VCHAR or obs-text
                if (!(escaped == '\t' || (escaped >= 0x20 && escaped <= 0x7e) || escaped >= 0x80)) return std::nullopt;
                out += static_cast<char>(escaped);
                continue;
            }
            // qdtext: HTAB, SP, 0x21, 0x23-0x5B, 0x5D-0x7E, obs-text
            if (c == '\t' || c == ' ' || c == 0x21 || (c >= 0x23 && c <= 0x5b) || (c >= 0x5d && c <= 0x7e) ||
                c >= 0x80) {
                out += static_cast<char>(c);
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;  // no closing quote
    }

private:
    std::string_view text_;
    std::size_t at_ = 0;
};

}  // namespace neomcp::http::detail
