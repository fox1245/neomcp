#include "schema/pattern_scan.h"

namespace neomcp::json_schema::detail {

std::optional<std::string> find_unsupported_construct(std::string_view pattern) {
    bool in_class = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        const char next = i + 1 < pattern.size() ? pattern[i + 1] : '\0';
        const char after = i + 2 < pattern.size() ? pattern[i + 2] : '\0';
        if (c == '\\') {
            if ((next == 'p' || next == 'P') && after == '{') {
                return std::string("a Unicode property escape (\\") + next + "{...})";
            }
            if (next == 'u' && after == '{') return std::string("a code point escape (\\u{...})");
            if (next == 'k' && after == '<' && !in_class) return std::string("a named back-reference (\\k<...>)");
            ++i;  // the escaped character is not read again
            continue;
        }
        if (in_class) {
            if (c == ']') in_class = false;
            continue;
        }
        if (c == '[') {
            in_class = true;
        } else if (c == '(' && next == '?' && after == '<') {
            const char kind = i + 3 < pattern.size() ? pattern[i + 3] : '\0';
            if (kind == '=' || kind == '!') return std::string("a lookbehind assertion (?<") + kind + "...)";
            return std::string("a named group (?<name>...)");
        }
    }
    return std::nullopt;
}

}  // namespace neomcp::json_schema::detail
