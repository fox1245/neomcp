#include "neomcp/http/media_type.h"

#include <algorithm>
#include <cstddef>

#include "neomcp/http/message.h"

namespace neomcp::http {

namespace {

constexpr int full_weight = 1000;
constexpr std::string_view weight_name = "q";
constexpr std::string_view any = "*";

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

struct Parameter {
    std::string name;  // lower case
    std::string value;
    bool quoted = false;
};

// Reads one value from left to right; every reader either consumes what it read or reports that nothing fits.
class Cursor {
public:
    explicit Cursor(std::string_view text) : text_(text) {}

    bool done() const noexcept { return at_ >= text_.size(); }
    char peek() const noexcept { return text_[at_]; }
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

std::string lowered(std::string text) {
    for (char& c : text) c = lower(c);
    return text;
}

// `*( OWS ";" OWS [ parameter ] )`, up to the end of the text or, when `in_list` is set, up to a ',' (which it does
// not consume). False when what follows is not that.
bool read_parameters(Cursor& cursor, std::vector<Parameter>& out, bool in_list) {
    for (;;) {
        cursor.skip_whitespace();
        if (cursor.done()) return true;
        if (in_list && cursor.peek() == ',') return true;
        if (!cursor.take(';')) return false;
        cursor.skip_whitespace();
        if (cursor.done() || cursor.peek() == ';' || (in_list && cursor.peek() == ',')) continue;  // empty parameter
        Parameter parameter;
        parameter.name = lowered(cursor.token());
        if (parameter.name.empty() || !cursor.take('=')) return false;
        if (!cursor.done() && cursor.peek() == '"') {
            auto value = cursor.quoted_string();
            if (!value) return false;
            parameter.value = std::move(*value);
            parameter.quoted = true;
        } else {
            parameter.value = cursor.token();
            if (parameter.value.empty()) return false;
        }
        out.push_back(std::move(parameter));
    }
}

// qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] ), times 1000.
std::optional<int> read_weight(std::string_view text) {
    if (text.empty() || (text.front() != '0' && text.front() != '1')) return std::nullopt;
    const int whole = text.front() - '0';
    if (text.size() == 1) return whole * full_weight;
    if (text[1] != '.') return std::nullopt;
    const std::string_view digits = text.substr(2);
    if (digits.size() > 3) return std::nullopt;
    int fraction = 0;
    int scale = full_weight / 10;
    for (const char digit : digits) {
        if (!is_digit(digit)) return std::nullopt;
        fraction += (digit - '0') * scale;
        scale /= 10;
    }
    if (whole == 1 && fraction != 0) return std::nullopt;
    return whole * full_weight + fraction;
}

}  // namespace

std::optional<MediaType> parse_media_type(std::string_view text) {
    Cursor cursor(text);
    cursor.skip_whitespace();
    MediaType media;
    media.type = lowered(cursor.token());
    if (media.type.empty() || !cursor.take('/')) return std::nullopt;
    media.subtype = lowered(cursor.token());
    if (media.subtype.empty()) return std::nullopt;
    std::vector<Parameter> parameters;
    if (!read_parameters(cursor, parameters, false) || !cursor.done()) return std::nullopt;
    for (Parameter& parameter : parameters) {
        media.parameters.emplace_back(std::move(parameter.name), std::move(parameter.value));
    }
    return media;
}

bool is_media_type(std::string_view text, std::string_view type, std::string_view subtype) {
    const std::optional<MediaType> media = parse_media_type(text);
    return media && equal_ignoring_case(media->type, type) && equal_ignoring_case(media->subtype, subtype);
}

std::optional<std::vector<MediaRange>> parse_accept(std::string_view text) {
    Cursor cursor(text);
    std::vector<MediaRange> ranges;
    for (;;) {
        cursor.skip_whitespace();
        if (cursor.done()) return ranges;
        if (cursor.take(',')) continue;  // an empty list element
        MediaRange range;
        range.type = lowered(cursor.token());
        if (range.type.empty() || !cursor.take('/')) return std::nullopt;
        range.subtype = lowered(cursor.token());
        if (range.subtype.empty()) return std::nullopt;
        if (range.type == any && range.subtype != any) return std::nullopt;
        std::vector<Parameter> parameters;
        if (!read_parameters(cursor, parameters, true)) return std::nullopt;
        bool weighted = false;
        for (const Parameter& parameter : parameters) {
            if (parameter.name != weight_name) continue;
            const std::optional<int> weight = parameter.quoted ? std::nullopt : read_weight(parameter.value);
            if (weighted || !weight) return std::nullopt;  // one weight, written as a qvalue
            range.weight = *weight;
            weighted = true;
        }
        ranges.push_back(std::move(range));
        cursor.skip_whitespace();
        if (cursor.done()) return ranges;
        if (!cursor.take(',')) return std::nullopt;
    }
}

bool accepts(std::span<const MediaRange> ranges, std::string_view type, std::string_view subtype) {
    int best_specificity = -1;
    int best_weight = 0;
    for (const MediaRange& range : ranges) {
        int specificity = 0;
        if (range.type == any) {
            specificity = 0;
        } else if (!equal_ignoring_case(range.type, type)) {
            continue;
        } else if (range.subtype == any) {
            specificity = 1;
        } else if (equal_ignoring_case(range.subtype, subtype)) {
            specificity = 2;
        } else {
            continue;
        }
        if (specificity > best_specificity) {
            best_specificity = specificity;
            best_weight = range.weight;
        } else if (specificity == best_specificity) {
            best_weight = std::max(best_weight, range.weight);
        }
    }
    return best_specificity >= 0 && best_weight > 0;
}

}  // namespace neomcp::http
