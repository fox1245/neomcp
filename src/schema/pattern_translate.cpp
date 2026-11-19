#include "schema/pattern_translate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neomcp::json_schema::detail {

namespace {

using Rune = char32_t;

constexpr Rune max_rune = 0x10FFFF;
// The largest repeat count RE2 accepts.
constexpr std::size_t max_repeat = 1000;
// A repeat count that is larger than any limit; digits beyond it change nothing.
constexpr std::size_t saturated_count = 1'000'000;
// How much of the pattern a message quotes for one construct.
constexpr std::size_t quoted_bytes = 24;
// What a pattern that contains "\B" starts with. The engine searches a text byte by byte, so an expression that can
// match without consuming a character may begin inside the bytes of a character, and there "\B" holds, since both
// neighbours are bytes of a character that is not an ASCII word character. ECMA-262 asks "\B" only between two code
// points. Reading whole characters from the start of the text makes every place where the expression begins one.
constexpr std::string_view from_a_character_boundary = "^[\\x{0}-\\x{10ffff}]*?(?:";

struct Range {
    Rune lo;
    Rune hi;
};

// ECMA-262 \d, \w and \s. \s is WhiteSpace and LineTerminator: tab, line tab, form feed, space, no-break space, the
// byte order mark, the characters of category Zs, line feed, carriage return, line separator and paragraph separator.
constexpr std::array<Range, 1> digit_ranges = {{{0x30, 0x39}}};
constexpr std::array<Range, 4> word_ranges = {{{0x30, 0x39}, {0x41, 0x5A}, {0x5F, 0x5F}, {0x61, 0x7A}}};
constexpr std::array<Range, 10> space_ranges = {{{0x09, 0x0D},
                                                 {0x20, 0x20},
                                                 {0xA0, 0xA0},
                                                 {0x1680, 0x1680},
                                                 {0x2000, 0x200A},
                                                 {0x2028, 0x2029},
                                                 {0x202F, 0x202F},
                                                 {0x205F, 0x205F},
                                                 {0x3000, 0x3000},
                                                 {0xFEFF, 0xFEFF}}};
// The characters '.' does not match: the line terminators.
constexpr std::array<Range, 3> line_terminator_ranges = {{{0x0A, 0x0A}, {0x0D, 0x0D}, {0x2028, 0x2029}}};

// A general category as ECMA-262 names it. `exact` is false for the categories that RE2 defines differently.
struct Category {
    std::string_view short_name;
    std::string_view long_name;
    std::string_view alias;
    bool exact;
};

// The values of General_Category in the table of property value aliases that ECMA-262 refers to. RE2 knows the short
// names only. It has no "Cn" (unassigned) and no "LC" (cased letter), and its "C" lacks the unassigned characters
// that ECMA-262's "C" has, so those three are refused.
constexpr std::array<Category, 38> categories = {{
    {"C", "Other", "", false},
    {"Cc", "Control", "cntrl", true},
    {"Cf", "Format", "", true},
    {"Cn", "Unassigned", "", false},
    {"Co", "Private_Use", "", true},
    {"Cs", "Surrogate", "", true},
    {"L", "Letter", "", true},
    {"LC", "Cased_Letter", "", false},
    {"Ll", "Lowercase_Letter", "", true},
    {"Lm", "Modifier_Letter", "", true},
    {"Lo", "Other_Letter", "", true},
    {"Lt", "Titlecase_Letter", "", true},
    {"Lu", "Uppercase_Letter", "", true},
    {"M", "Mark", "Combining_Mark", true},
    {"Mc", "Spacing_Mark", "", true},
    {"Me", "Enclosing_Mark", "", true},
    {"Mn", "Nonspacing_Mark", "", true},
    {"N", "Number", "", true},
    {"Nd", "Decimal_Number", "digit", true},
    {"Nl", "Letter_Number", "", true},
    {"No", "Other_Number", "", true},
    {"P", "Punctuation", "punct", true},
    {"Pc", "Connector_Punctuation", "", true},
    {"Pd", "Dash_Punctuation", "", true},
    {"Pe", "Close_Punctuation", "", true},
    {"Pf", "Final_Punctuation", "", true},
    {"Pi", "Initial_Punctuation", "", true},
    {"Po", "Other_Punctuation", "", true},
    {"Ps", "Open_Punctuation", "", true},
    {"S", "Symbol", "", true},
    {"Sc", "Currency_Symbol", "", true},
    {"Sk", "Modifier_Symbol", "", true},
    {"Sm", "Math_Symbol", "", true},
    {"So", "Other_Symbol", "", true},
    {"Z", "Separator", "", true},
    {"Zl", "Line_Separator", "", true},
    {"Zp", "Paragraph_Separator", "", true},
    {"Zs", "Space_Separator", "", true},
}};

const Category* find_category(std::string_view name) {
    for (const Category& category : categories) {
        if (name == category.short_name || name == category.long_name || (!category.alias.empty() && name == category.alias)) {
            return &category;
        }
    }
    return nullptr;
}

// Reads the code point at `index` of `text`. False when the bytes there are not well-formed UTF-8.
bool decode(std::string_view text, std::size_t index, Rune& rune, std::size_t& length) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned>(static_cast<unsigned char>(text[at])); };
    const unsigned lead = byte(index);
    if (lead < 0x80) {
        rune = lead;
        length = 1;
        return true;
    }
    std::size_t extra = 0;
    Rune minimum = 0;
    Rune value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1;
        minimum = 0x80;
        value = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2;
        minimum = 0x800;
        value = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3;
        minimum = 0x10000;
        value = lead & 0x07u;
    } else {
        return false;
    }
    if (index + extra >= text.size()) return false;
    for (std::size_t i = 1; i <= extra; ++i) {
        const unsigned next = byte(index + i);
        if ((next & 0xC0u) != 0x80u) return false;
        value = (value << 6) | (next & 0x3Fu);
    }
    if (value < minimum || value > max_rune || (value >= 0xD800 && value <= 0xDFFF)) return false;
    rune = value;
    length = extra + 1;
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_decimal_digit(char c) { return c >= '0' && c <= '9'; }

bool is_ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool is_syntax_character(Rune c) {
    switch (c) {
        case U'^': case U'$': case U'\\': case U'.': case U'*': case U'+': case U'?':
        case U'(': case U')': case U'[': case U']': case U'{': case U'}': case U'|':
            return true;
        default:
            return false;
    }
}

std::string hex_of(Rune rune) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    do {
        out.insert(out.begin(), digits[rune & 0xFu]);
        rune >>= 4;
    } while (rune != 0);
    return out;
}

// One character as RE2 reads it, whatever its place in the expression.
std::string literal_of(Rune rune) {
    const bool plain = (rune >= U'0' && rune <= U'9') || (rune >= U'A' && rune <= U'Z') ||
                       (rune >= U'a' && rune <= U'z') || rune == U'_';
    if (plain) return std::string(1, static_cast<char>(rune));
    return "\\x{" + hex_of(rune) + "}";
}

// Sorted, with overlapping and adjacent ranges joined.
std::vector<Range> normalized(std::vector<Range> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
    std::vector<Range> out;
    for (const Range& range : ranges) {
        if (!out.empty() && range.lo <= out.back().hi + 1) {
            out.back().hi = std::max(out.back().hi, range.hi);
        } else {
            out.push_back(range);
        }
    }
    return out;
}

// What the code points 0 to 0x10FFFF contain beyond `ranges`.
std::vector<Range> complement(const std::vector<Range>& ranges) {
    std::vector<Range> out;
    Rune next = 0;
    for (const Range& range : normalized(ranges)) {
        if (range.lo > next) out.push_back(Range{next, range.lo - 1});
        next = range.hi + 1;
    }
    if (next <= max_rune) out.push_back(Range{next, max_rune});
    return out;
}

// A character class of RE2: ranges, then properties that RE2 spells out itself. Nothing at all in a class matches
// nothing, and nothing in a negated class matches everything; RE2 does not read "[]" or "[^]" that way.
void append_class(std::string& out, const std::vector<Range>& ranges, const std::string& properties, bool negated) {
    if (ranges.empty() && properties.empty()) {
        out += negated ? "[\\x{0}-\\x{10ffff}]" : "[^\\x{0}-\\x{10ffff}]";
        return;
    }
    out += negated ? "[^" : "[";
    for (const Range& range : normalized(ranges)) {
        out += literal_of(range.lo);
        if (range.hi > range.lo) {
            out += '-';
            out += literal_of(range.hi);
        }
    }
    out += properties;
    out += ']';
}

// A construct of the pattern that comes out as something else, or none.
struct Item {
    enum class Kind { character, set, property, assertion };
    Kind kind = Kind::character;
    Rune rune = 0;              // character
    std::vector<Range> ranges;  // set
    std::string text;           // property, assertion: the RE2 spelling
};

class Translator {
public:
    explicit Translator(std::string_view pattern) : pattern_(pattern) {}

    std::variant<std::string, PatternRefusal> run() {
        try {
            translate();
        } catch (PatternRefusal& refusal) {
            return std::move(refusal);
        }
        if (non_boundary_) return std::string(from_a_character_boundary) + out_ + ")";
        return std::move(out_);
    }

private:
    // What came last in the expression decides whether a quantifier may follow.
    enum class Previous { nothing, atom, assertion, quantifier };

    [[noreturn]] void refuse(std::size_t position, std::string construct) const {
        throw PatternRefusal{position, std::move(construct)};
    }

    // The text of the pattern from `start` up to here, shortened for a message.
    std::string quoted(std::size_t start) const {
        std::string_view text = pattern_.substr(start, pos_ - start);
        if (text.size() > quoted_bytes) {
            std::size_t cut = quoted_bytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
            return std::string(text.substr(0, cut)) + "...";
        }
        return std::string(text);
    }

    bool more() const { return pos_ < pattern_.size(); }

    // The byte `ahead` bytes after the current position, or 0 past the end. Syntax characters are ASCII and no byte
    // of a longer UTF-8 sequence is, so a byte is enough to recognise them.
    char peek(std::size_t ahead = 0) const { return pos_ + ahead < pattern_.size() ? pattern_[pos_ + ahead] : '\0'; }

    Rune next() {
        Rune rune = 0;
        std::size_t length = 0;
        if (!decode(pattern_, pos_, rune, length)) refuse(pos_, "text that is not valid UTF-8");
        pos_ += length;
        return rune;
    }

    void translate() {
        while (more()) {
            const std::size_t start = pos_;
            const Rune c = next();
            switch (c) {
                case U'\\': escape_outside_class(start); break;
                case U'^':
                    out_ += '^';
                    previous_ = Previous::assertion;
                    break;
                case U'$':
                    out_ += '$';
                    previous_ = Previous::assertion;
                    break;
                case U'.':
                    append_class(out_, std::vector<Range>(line_terminator_ranges.begin(), line_terminator_ranges.end()),
                                 {}, true);
                    previous_ = Previous::atom;
                    break;
                case U'(': open_group(start); break;
                case U')':
                    if (opened_.empty()) refuse(start, "an unmatched ')'");
                    opened_.pop_back();
                    out_ += ')';
                    previous_ = Previous::atom;
                    break;
                case U'|':
                    out_ += '|';
                    previous_ = Previous::nothing;
                    break;
                case U'[': character_class(start); break;
                case U']': refuse(start, "an unescaped ']'");
                case U'}': refuse(start, "an unescaped '}'");
                case U'*':
                case U'+':
                case U'?':
                case U'{': quantifier(start, c); break;
                default:
                    out_ += literal_of(c);
                    previous_ = Previous::atom;
                    break;
            }
        }
        if (!opened_.empty()) refuse(opened_.back(), "a group that is never closed");
    }

    void open_group(std::size_t start) {
        if (peek() != '?') {
            out_ += "(?:";
            opened_.push_back(start);
            previous_ = Previous::nothing;
            return;
        }
        const char kind = peek(1);
        if (kind == ':') {
            pos_ += 2;
            out_ += "(?:";
        } else if (kind == '=' || kind == '!') {
            pos_ += 2;
            refuse(start, std::string("a lookahead assertion (?") + kind + "...)");
        } else if (kind == '<' && (peek(2) == '=' || peek(2) == '!')) {
            const char sign = peek(2);
            pos_ += 3;
            refuse(start, std::string("a lookbehind assertion (?<") + sign + "...)");
        } else if (kind == '<') {
            pos_ += 2;
            const std::size_t name_start = pos_;
            while (more() && peek() != '>') ++pos_;
            if (!more()) refuse(start, "a group name that is never closed");
            const std::string name(pattern_.substr(name_start, pos_ - name_start));
            ++pos_;  // '>'
            if (!is_group_name(name)) {
                refuse(start, "a group name that is not an ASCII identifier (?<" + name + ">...)");
            }
            if (!names_.insert(name).second) refuse(start, "a group name used twice (?<" + name + ">...)");
            out_ += "(?:";
        } else {
            ++pos_;  // '?'
            if (more()) next();
            refuse(start, "a group modifier or other group syntax (" + quoted(start + 1) + ")");
        }
        opened_.push_back(start);
        previous_ = Previous::nothing;
    }

    static bool is_group_name(const std::string& name) {
        if (name.empty()) return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            const bool start_ok = is_ascii_letter(c) || c == '_' || c == '$';
            if (i == 0 ? !start_ok : !(start_ok || is_decimal_digit(c))) return false;
        }
        return true;
    }

    std::size_t read_count() {
        std::size_t value = 0;
        while (is_decimal_digit(peek())) {
            if (value < saturated_count) value = value * 10 + static_cast<std::size_t>(peek() - '0');
            ++pos_;
        }
        return value;
    }

    void quantifier(std::size_t start, Rune c) {
        if (previous_ != Previous::atom) {
            refuse(start, previous_ == Previous::assertion ? "a quantifier after an assertion"
                                                           : "a quantifier with nothing to repeat");
        }
        if (c == U'{') {
            if (!is_decimal_digit(peek())) refuse(start, "a '{' that does not begin a quantifier");
            const std::size_t low = read_count();
            bool has_high = false;
            bool open_ended = false;
            std::size_t high = 0;
            if (peek() == ',') {
                ++pos_;
                if (is_decimal_digit(peek())) {
                    high = read_count();
                    has_high = true;
                } else {
                    open_ended = true;
                }
            }
            if (peek() != '}') refuse(start, "a '{' that does not begin a quantifier");
            ++pos_;
            if (low > max_repeat || (has_high && high > max_repeat)) {
                refuse(start, "a repeat count above " + std::to_string(max_repeat) + " (" + quoted(start) + ")");
            }
            if (has_high && high < low) refuse(start, "a quantifier whose upper bound is below its lower bound");
            out_ += "{" + std::to_string(low);
            if (has_high) out_ += "," + std::to_string(high);
            if (open_ended) out_ += ",";
            out_ += "}";
        } else {
            out_ += static_cast<char>(c);
        }
        if (peek() == '?') {  // the lazy form; which match is found first does not change whether there is one
            ++pos_;
            out_ += '?';
        }
        previous_ = Previous::quantifier;
    }

    void escape_outside_class(std::size_t start) {
        const Item item = escape(start, false);
        switch (item.kind) {
            case Item::Kind::character:
                out_ += literal_of(item.rune);
                previous_ = Previous::atom;
                break;
            case Item::Kind::set:
                append_class(out_, item.ranges, {}, false);
                previous_ = Previous::atom;
                break;
            case Item::Kind::property:
                out_ += item.text;
                previous_ = Previous::atom;
                break;
            case Item::Kind::assertion:
                out_ += item.text;
                previous_ = Previous::assertion;
                break;
        }
    }

    void character_class(std::size_t start) {
        bool negated = false;
        if (peek() == '^') {
            negated = true;
            ++pos_;
        }
        std::vector<Range> ranges;
        std::string properties;
        for (;;) {
            if (!more()) refuse(start, "a character class that is never closed");
            if (peek() == ']') {
                ++pos_;
                break;
            }
            const std::size_t atom_start = pos_;
            const Item first = class_atom();
            if (peek() == '-' && pos_ + 1 < pattern_.size() && peek(1) != ']') {
                ++pos_;
                const Item last = class_atom();
                if (first.kind != Item::Kind::character || last.kind != Item::Kind::character) {
                    refuse(atom_start, "a class escape at an end of a range (" + quoted(atom_start) + ")");
                }
                if (first.rune > last.rune) refuse(atom_start, "a range whose end is before its start (" + quoted(atom_start) + ")");
                ranges.push_back(Range{first.rune, last.rune});
                continue;
            }
            switch (first.kind) {
                case Item::Kind::character: ranges.push_back(Range{first.rune, first.rune}); break;
                case Item::Kind::set: ranges.insert(ranges.end(), first.ranges.begin(), first.ranges.end()); break;
                case Item::Kind::property: properties += first.text; break;
                case Item::Kind::assertion: break;  // not produced inside a class
            }
        }
        append_class(out_, ranges, properties, negated);
        previous_ = Previous::atom;
    }

    Item class_atom() {
        const std::size_t start = pos_;
        const Rune c = next();
        if (c == U'\\') return escape(start, true);
        Item item;
        item.rune = c;
        return item;
    }

    static Item character(Rune rune) {
        Item item;
        item.rune = rune;
        return item;
    }

    template <typename Ranges>
    static Item set_of(const Ranges& ranges, bool negated) {
        Item item;
        item.kind = Item::Kind::set;
        const std::vector<Range> given(ranges.begin(), ranges.end());
        item.ranges = negated ? complement(given) : given;
        return item;
    }

    // The escape that starts at `start`, with the backslash already read.
    Item escape(std::size_t start, bool in_class) {
        if (!more()) refuse(start, "a backslash at the end of the pattern");
        const Rune c = next();
        switch (c) {
            case U'd': return set_of(digit_ranges, false);
            case U'D': return set_of(digit_ranges, true);
            case U'w': return set_of(word_ranges, false);
            case U'W': return set_of(word_ranges, true);
            case U's': return set_of(space_ranges, false);
            case U'S': return set_of(space_ranges, true);
            case U'b':
                if (in_class) return character(0x08);
                return assertion("\\b");
            case U'B':
                if (in_class) refuse(start, "\\B inside a character class");
                non_boundary_ = true;
                return assertion("\\B");
            case U'f': return character(0x0C);
            case U'n': return character(0x0A);
            case U'r': return character(0x0D);
            case U't': return character(0x09);
            case U'v': return character(0x0B);
            case U'0':
                if (is_decimal_digit(peek())) refuse(start, "an octal escape (" + quoted_escape(start) + ")");
                return character(0);
            case U'1': case U'2': case U'3': case U'4': case U'5': case U'6': case U'7': case U'8': case U'9':
                refuse(start, "a back-reference (" + quoted(start) + ")");
            case U'k': refuse(start, "a named back-reference (\\k<...>)");
            case U'c': {
                if (!is_ascii_letter(peek())) refuse(start, "'\\c' without a letter after it");
                const Rune letter = static_cast<Rune>(static_cast<unsigned char>(peek()));
                ++pos_;
                return character(letter % 32);
            }
            case U'x': {
                const int high = hex_value(peek());
                const int low = hex_value(peek(1));
                if (high < 0 || low < 0) refuse(start, "an incomplete \\x escape");
                pos_ += 2;
                return character(static_cast<Rune>(high * 16 + low));
            }
            case U'u': return unicode_escape(start);
            case U'p':
            case U'P': return property(start, c == U'P');
            default:
                if (is_syntax_character(c) || c == U'/' || (in_class && c == U'-')) return character(c);
                refuse(start, "an escape of a character that is not a syntax character (" + quoted(start) + ")");
        }
    }

    static Item assertion(const char* text) {
        Item item;
        item.kind = Item::Kind::assertion;
        item.text = text;
        return item;
    }

    // The escape after "\0", for a message.
    std::string quoted_escape(std::size_t start) const {
        return std::string(pattern_.substr(start, std::min<std::size_t>(pos_ + 1, pattern_.size()) - start));
    }

    // "\uHHHH", "\u{H...}", and a pair of "\uHHHH" escapes that stand for one character outside the basic plane.
    Item unicode_escape(std::size_t start) {
        if (peek() == '{') {
            ++pos_;
            std::size_t value = 0;
            std::size_t digits = 0;
            while (hex_value(peek()) >= 0) {
                if (value <= max_rune) value = value * 16 + static_cast<std::size_t>(hex_value(peek()));
                ++pos_;
                ++digits;
            }
            if (digits == 0 || peek() != '}') refuse(start, "an incomplete \\u{...} escape");
            ++pos_;
            if (value > max_rune) refuse(start, "a \\u{...} escape above U+10FFFF (" + quoted(start) + ")");
            if (value >= 0xD800 && value <= 0xDFFF) refuse(start, "a lone surrogate escape (" + quoted(start) + ")");
            return character(static_cast<Rune>(value));
        }
        const std::optional<Rune> first = four_hex_digits();
        if (!first) refuse(start, "an incomplete \\u escape");
        if (*first >= 0xD800 && *first <= 0xDBFF && peek() == '\\' && peek(1) == 'u') {
            const std::size_t saved = pos_;
            pos_ += 2;
            const std::optional<Rune> second = four_hex_digits();
            if (second && *second >= 0xDC00 && *second <= 0xDFFF) {
                return character(0x10000 + ((*first - 0xD800) << 10) + (*second - 0xDC00));
            }
            pos_ = saved;
        }
        if (*first >= 0xD800 && *first <= 0xDFFF) refuse(start, "a lone surrogate escape (" + quoted(start) + ")");
        return character(*first);
    }

    std::optional<Rune> four_hex_digits() {
        Rune value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const int digit = hex_value(peek(i));
            if (digit < 0) return std::nullopt;
            value = value * 16 + static_cast<Rune>(digit);
        }
        pos_ += 4;
        return value;
    }

    // "\p{...}" and "\P{...}" with a general category, written as RE2 spells it.
    Item property(std::size_t start, bool negated) {
        if (peek() != '{') refuse(start, "an incomplete property escape");
        const std::size_t open = pos_;
        while (more() && peek() != '}') ++pos_;
        if (!more()) refuse(start, "an incomplete property escape");
        const std::string_view body = pattern_.substr(open + 1, pos_ - open - 1);
        ++pos_;  // '}'
        std::string_view value = body;
        const std::size_t equals = body.find('=');
        if (equals != std::string_view::npos) {
            const std::string_view name = body.substr(0, equals);
            if (name != "General_Category" && name != "gc") {
                refuse(start, "a Unicode property that is not a general category (" + quoted(start) + ")");
            }
            value = body.substr(equals + 1);
        }
        const Category* category = find_category(value);
        if (!category) refuse(start, "a Unicode property that is not a general category (" + quoted(start) + ")");
        if (!category->exact) {
            refuse(start, "a general category that RE2 defines differently (" + quoted(start) + ")");
        }
        Item item;
        item.kind = Item::Kind::property;
        item.text = std::string(negated ? "\\P{" : "\\p{") + std::string(category->short_name) + "}";
        return item;
    }

    std::string_view pattern_;
    std::size_t pos_ = 0;
    std::string out_;
    bool non_boundary_ = false;  // "\B" occurs
    Previous previous_ = Previous::nothing;
    std::vector<std::size_t> opened_;  // where each open group begins
    std::set<std::string> names_;
};

}  // namespace

std::variant<std::string, PatternRefusal> translate_pattern(std::string_view pattern) {
    return Translator(pattern).run();
}

bool is_valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        Rune rune = 0;
        std::size_t length = 0;
        if (!decode(text, index, rune, length)) return false;
        index += length;
    }
    return true;
}

}  // namespace neomcp::json_schema::detail
