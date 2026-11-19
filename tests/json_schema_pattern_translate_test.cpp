// The translation of a `pattern` from the dialect of JSON Schema (ECMA-262, Unicode mode) to the syntax of the regular
// expression engine: what each construct becomes, and what is refused, where, and why. The translator is internal, so
// the test reaches it through the source directory.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "schema/pattern_translate.h"

namespace {

using neomcp::json_schema::detail::PatternRefusal;
using neomcp::json_schema::detail::translate_pattern;

struct Translation {
    std::string pattern;
    std::string expected;
};

struct Refusal {
    std::string pattern;
    std::size_t position;
    std::string construct;  // a part of the description
};

TEST(PatternTranslate, EveryConstructBecomesTheExpressionWithTheSameMeaning) {
    const std::vector<Translation> cases = {
        // Literals: only letters, digits and '_' are written as they are.
        {"abc", "abc"},
        {"a-b", "a\\x{2d}b"},
        {"a b", "a\\x{20}b"},
        {"a/b", "a\\x{2f}b"},
        {"\xC3\xA9", "\\x{e9}"},
        {"\xF0\x9F\x98\x80", "\\x{1f600}"},
        {"snake_case9", "snake_case9"},
        // Assertions.
        {"^a$", "^a$"},
        {"\\bfoo", "\\bfoo"},
        // "\\B" holds only between two characters, so the expression is read from a character boundary.
        {"\\B", "^[\\x{0}-\\x{10ffff}]*?(?:\\B)"},
        {"a\\B|\\bb", "^[\\x{0}-\\x{10ffff}]*?(?:a\\B|\\bb)"},
        // The dot is every character but the line terminators.
        {"a.b", "a[^\\x{a}\\x{d}\\x{2028}-\\x{2029}]b"},
        // Class escapes: ASCII digits and word characters, the white space and line terminators of ECMA-262.
        {"\\d", "[0-9]"},
        {"\\D", "[\\x{0}-\\x{2f}\\x{3a}-\\x{10ffff}]"},
        {"\\w", "[0-9A-Z_a-z]"},
        {"\\W", "[\\x{0}-\\x{2f}\\x{3a}-\\x{40}\\x{5b}-\\x{5e}\\x{60}\\x{7b}-\\x{10ffff}]"},
        {"\\s",
         "[\\x{9}-\\x{d}\\x{20}\\x{a0}\\x{1680}\\x{2000}-\\x{200a}\\x{2028}-\\x{2029}\\x{202f}\\x{205f}\\x{3000}\\x{feff}]"},
        {"\\S",
         "[\\x{0}-\\x{8}\\x{e}-\\x{1f}\\x{21}-\\x{9f}\\x{a1}-\\x{167f}\\x{1681}-\\x{1fff}\\x{200b}-\\x{2027}"
         "\\x{202a}-\\x{202e}\\x{2030}-\\x{205e}\\x{2060}-\\x{2fff}\\x{3001}-\\x{fefe}\\x{ff00}-\\x{10ffff}]"},
        // Character escapes.
        {"\\t\\n\\v\\f\\r", "\\x{9}\\x{a}\\x{b}\\x{c}\\x{d}"},
        {"\\cJ\\cj\\cA", "\\x{a}\\x{a}\\x{1}"},
        {"\\0", "\\x{0}"},
        {"\\x41\\x7a", "Az"},
        {"\\u0041\\u00e9", "A\\x{e9}"},
        {"\\u{1F600}", "\\x{1f600}"},
        {"\\u{41}", "A"},
        {"\\uD83D\\uDE00", "\\x{1f600}"},
        {"\\.\\*\\/\\\\\\(\\)\\[\\]\\{\\}\\|\\^\\$\\+\\?",
         "\\x{2e}\\x{2a}\\x{2f}\\x{5c}\\x{28}\\x{29}\\x{5b}\\x{5d}\\x{7b}\\x{7d}\\x{7c}\\x{5e}\\x{24}\\x{2b}\\x{3f}"},
        // Groups: none of them captures.
        {"(ab)", "(?:ab)"},
        {"(?:ab)", "(?:ab)"},
        {"(?<year>\\d{4})", "(?:[0-9]{4})"},
        {"a|b||c", "a|b||c"},
        {"()", "(?:)"},
        // Quantifiers, and their lazy forms.
        {"a*b+c?", "a*b+c?"},
        {"a*?b+?c??", "a*?b+?c??"},
        {"a{3}b{2,}c{1,4}", "a{3}b{2,}c{1,4}"},
        {"a{3}?", "a{3}?"},
        {"a{0}", "a{0}"},
        {"a{1000}", "a{1000}"},
        {"(?:ab){2,3}", "(?:ab){2,3}"},
        // Classes.
        {"[a-z]", "[a-z]"},
        {"[^a-z]", "[^a-z]"},
        {"[a-cx-z5]", "[5a-cx-z]"},
        {"[\\d-]", "[\\x{2d}0-9]"},
        {"[-a]", "[\\x{2d}a]"},
        {"[a-]", "[\\x{2d}a]"},
        {"[a-c\\d]", "[0-9a-c]"},
        {"[\\b]", "[\\x{8}]"},
        {"[\\]\\\\\\-\\^]", "[\\x{2d}\\x{5c}-\\x{5e}]"},
        {"[[]", "[\\x{5b}]"},
        {"[\\x41-\\u0043]", "[A-C]"},
        {"[\\0-\\x1f]", "[\\x{0}-\\x{1f}]"},
        {"[\\S]", "[\\x{0}-\\x{8}\\x{e}-\\x{1f}\\x{21}-\\x{9f}\\x{a1}-\\x{167f}\\x{1681}-\\x{1fff}\\x{200b}-\\x{2027}"
                  "\\x{202a}-\\x{202e}\\x{2030}-\\x{205e}\\x{2060}-\\x{2fff}\\x{3001}-\\x{fefe}\\x{ff00}-\\x{10ffff}]"},
        {"[^\\d]", "[^0-9]"},
        // A class that holds nothing matches nothing; a negated one matches everything.
        {"[]", "[^\\x{0}-\\x{10ffff}]"},
        {"[^]", "[\\x{0}-\\x{10ffff}]"},
        // General categories, by their short and long names and with the name of the property.
        {"\\p{Lu}", "\\p{Lu}"},
        {"\\p{Letter}", "\\p{L}"},
        {"\\P{Decimal_Number}", "\\P{Nd}"},
        {"\\p{digit}", "\\p{Nd}"},
        {"\\p{gc=Zs}", "\\p{Zs}"},
        {"\\P{General_Category=Space_Separator}", "\\P{Zs}"},
        {"[\\p{L}\\d_]", "[0-9_\\p{L}]"},
        {"[^\\P{Lu}a]", "[^a\\P{Lu}]"},
    };
    for (const Translation& entry : cases) {
        const auto result = translate_pattern(entry.pattern);
        const auto* text = std::get_if<std::string>(&result);
        ASSERT_TRUE(text != nullptr) << "refused: " << entry.pattern << " at byte "
                                     << std::get<PatternRefusal>(result).position << ": "
                                     << std::get<PatternRefusal>(result).construct;
        EXPECT_EQ(*text, entry.expected) << "pattern: " << entry.pattern;
    }
}

TEST(PatternTranslate, WhatTheEngineCannotMatchExactlyIsRefusedWhereItBegins) {
    const std::vector<Refusal> cases = {
        // Assertions and references that need to look back or around.
        {"(?=a)", 0, "lookahead"},
        {"a(?!b)", 1, "lookahead"},
        {"(?<=a)", 0, "lookbehind"},
        {"x(?<!a)", 1, "lookbehind"},
        {"\\1", 0, "back-reference"},
        {"(a)\\1", 3, "back-reference"},
        {"(?<n>a)\\k<n>", 7, "named back-reference"},
        {"[\\1]", 1, "back-reference"},
        // Other group syntax.
        {"(?i)a", 0, "group modifier"},
        {"(?i:a)", 0, "group modifier"},
        {"(?P<n>a)", 0, "group modifier"},
        {"(?<a>x)(?<a>y)", 7, "used twice"},
        {"(?<1a>x)", 0, "not an ASCII identifier"},
        {"(?<a", 0, "never closed"},
        // Properties other than the general categories, and the categories the engine defines differently.
        {"\\p{Script=Greek}", 0, "not a general category"},
        {"a\\p{Alphabetic}", 1, "not a general category"},
        {"\\p{Foo}", 0, "not a general category"},
        {"\\p{C}", 0, "defines differently"},
        {"x\\P{Cn}", 1, "defines differently"},
        {"\\p{LC}", 0, "defines differently"},
        {"\\p{L", 0, "incomplete property escape"},
        {"\\p", 0, "incomplete property escape"},
        // Repeat counts.
        {"a{1001}", 1, "repeat count above 1000"},
        {"a{1,1001}", 1, "repeat count above 1000"},
        {"ab{3,2}", 2, "upper bound is below"},
        // Syntax errors in Unicode mode.
        {"a**", 2, "nothing to repeat"},
        {"*a", 0, "nothing to repeat"},
        {"(*)", 1, "nothing to repeat"},
        {"a|+", 2, "nothing to repeat"},
        {"^*", 1, "after an assertion"},
        {"\\b+", 2, "after an assertion"},
        {"a{", 1, "does not begin a quantifier"},
        {"a{,3}", 1, "does not begin a quantifier"},
        {"a{2", 1, "does not begin a quantifier"},
        {"a]", 1, "unescaped ']'"},
        {"a}", 1, "unescaped '}'"},
        {"{", 0, "nothing to repeat"},
        {"\\a", 0, "not a syntax character"},
        {"x\\-", 1, "not a syntax character"},
        {"\\_", 0, "not a syntax character"},
        {"\\e", 0, "not a syntax character"},
        {"\\00", 0, "octal"},
        {"\\c1", 0, "'\\c' without a letter"},
        {"\\xZ1", 0, "incomplete \\x escape"},
        {"\\u12", 0, "incomplete \\u escape"},
        {"\\u{}", 0, "incomplete \\u{...} escape"},
        {"\\u{41", 0, "incomplete \\u{...} escape"},
        {"\\uD800", 0, "lone surrogate"},
        {"\\uD800\\u0041", 0, "lone surrogate"},
        {"\\uDC00", 0, "lone surrogate"},
        {"\\u{D800}", 0, "lone surrogate"},
        {"\\u{110000}", 0, "above U+10FFFF"},
        {"(", 0, "never closed"},
        {"a(b(c)", 1, "never closed"},
        {"a)", 1, "unmatched ')'"},
        {"[abc", 0, "never closed"},
        {"a\\", 1, "backslash at the end"},
        // Classes.
        {"[z-a]", 1, "end is before its start"},
        {"[\\d-a]", 1, "class escape at an end of a range"},
        {"[a-\\d]", 1, "class escape at an end of a range"},
        {"[\\B]", 1, "\\B inside a character class"},
        {"[\\k<a>]", 1, "named back-reference"},
        // Text that is not UTF-8.
        {"a\xff", 1, "not valid UTF-8"},
        {"\xC0\x80", 0, "not valid UTF-8"},
        {"\xED\xA0\x80", 0, "not valid UTF-8"},
        {"\xE2\x82", 0, "not valid UTF-8"},
    };
    for (const Refusal& entry : cases) {
        const auto result = translate_pattern(entry.pattern);
        const auto* refusal = std::get_if<PatternRefusal>(&result);
        ASSERT_TRUE(refusal != nullptr) << "accepted: " << entry.pattern << " as " << std::get<std::string>(result);
        EXPECT_EQ(refusal->position, entry.position) << "pattern: " << entry.pattern;
        EXPECT_NE(refusal->construct.find(entry.construct), std::string::npos)
            << "pattern: " << entry.pattern << ", construct: " << refusal->construct;
    }
}

TEST(PatternTranslate, TheSameCharactersInAHarmlessPlaceAreNotRefused) {
    const std::vector<std::string> patterns = {
        "\\\\p",       // an escaped backslash, then the letter p
        "\\\\u{2}",    // an escaped backslash, then "u" twice
        "[(?<x]",      // inside a character class
        "[\\\\]p",     // the class ends at the first unescaped bracket
        "\\(?<a",      // an optional escaped parenthesis
        "[}{)(]",      // brackets that a class holds as characters
        "[a-z-9]",     // a range, then '-' and '9'
        "(?:)",
        "a|",
        "(?:^)*",
    };
    for (const std::string& pattern : patterns) {
        const auto result = translate_pattern(pattern);
        EXPECT_TRUE(std::holds_alternative<std::string>(result))
            << pattern << ": " << (std::holds_alternative<PatternRefusal>(result)
                                       ? std::get<PatternRefusal>(result).construct
                                       : std::string());
    }
}

TEST(PatternTranslate, TheLongestPatternsDoNotRecurse) {
    // Groups nested a thousand deep, and a thousand classes: the scanner keeps its state in a vector, not on the stack.
    std::string nested(5000, '(');
    nested += "a";
    nested += std::string(5000, ')');
    EXPECT_TRUE(std::holds_alternative<std::string>(translate_pattern(nested)));
    std::string classes;
    for (int i = 0; i < 1000; ++i) classes += "[a-z]";
    EXPECT_TRUE(std::holds_alternative<std::string>(translate_pattern(classes)));
}

}  // namespace
