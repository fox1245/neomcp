// What `pattern`, `patternProperties` and `propertyNames` do with the regular expression dialect of JSON Schema:
// the meaning of each construct where it differs from other dialects, the patterns that are refused and how, and the
// bound on the memory a pattern may take.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/json_schema.h"

namespace {

using neomcp::json;
using neomcp::json_schema::Issue;
using neomcp::json_schema::Limits;
using neomcp::json_schema::Validator;

// The result of compiling {"pattern": pattern}.
std::variant<Validator, Issue> compile_pattern(const std::string& pattern, const Limits& limits = {}) {
    return Validator::compile(json{{"pattern", pattern}}, limits);
}

// Whether `text` contains a match of `pattern`. The pattern must be accepted.
bool matches(const std::string& pattern, const std::string& text) {
    auto result = compile_pattern(pattern);
    const auto* validator = std::get_if<Validator>(&result);
    if (!validator) {
        ADD_FAILURE() << "refused: " << pattern << ": " << std::get<Issue>(result).message;
        return false;
    }
    return !validator->validate(json(text));
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

struct Case {
    std::string pattern;
    std::string text;
    bool matched;
};

TEST(JsonSchemaPattern, ThePatternIsSearchedForAndNotAnchored) {
    EXPECT_TRUE(matches("b", "abc"));
    EXPECT_FALSE(matches("^b", "abc"));
    EXPECT_FALSE(matches("b$", "abc"));
    EXPECT_TRUE(matches("^abc$", "abc"));
    EXPECT_TRUE(matches("", "anything"));
    EXPECT_TRUE(matches("", ""));
}

TEST(JsonSchemaPattern, TheMeaningOfEachConstructIsThatOfEcmaScriptInUnicodeMode) {
    const std::string emoji = "\xF0\x9F\x98\x80";  // U+1F600
    const std::vector<Case> cases = {
        // A character is a code point, whatever the number of bytes.
        {"^.$", emoji, true},
        {"^..$", emoji, false},
        {"^[^a]$", emoji, true},
        {"^.$", "\xC3\xA9", true},
        {"^\\u{1F600}$", emoji, true},
        {"^\\uD83D\\uDE00$", emoji, true},
        {"^\\u00e9$", "\xC3\xA9", true},
        {"^[\\u{1F600}-\\u{1F64F}]$", emoji, true},
        {"^[\xC3\xA9]$", "\xC3\xA9", true},
        // The dot does not match a line terminator; a negated class does.
        {"^.$", "\n", false},
        {"^.$", "\r", false},
        {"^.$", "\xE2\x80\xA8", false},
        {"^.$", "\xE2\x80\xA9", false},
        {"^.$", "\x0B", true},
        {"^[^a]$", "\n", true},
        {"^[^]$", "\n", true},
        {"^[]$", "a", false},
        {"[]", "", false},
        // '$' is the end of the text and not the place before a final line feed.
        {"a$", "a\n", false},
        {"a$", "a", true},
        {"^a", "\na", false},
        // \d and \w are ASCII; \s is the white space and the line terminators of ECMA-262.
        {"^\\d+$", "0123456789", true},
        {"^\\d$", "\xD9\xA3", false},
        {"^\\w+$", "azAZ09_", true},
        {"^\\w$", "\xC3\xA9", false},
        {"^\\W$", "\xC3\xA9", true},
        {"^\\s$", "\xC2\xA0", true},
        {"^\\s$", "\xEF\xBB\xBF", true},
        {"^\\s$", "\xE3\x80\x80", true},
        {"^\\s$", "\xE2\x80\xA8", true},
        {"^\\s$", "\x0B", true},
        {"^\\s$", "\x0C", true},
        {"^\\s$", "\xE1\xA0\x8E", false},  // U+180E is not white space
        {"^\\S$", "\xC2\xA0", false},
        {"^\\S$", "a", true},
        {"^[\\s\\d]+$", " 1\t2", true},
        {"^[\\S]$", " ", false},
        // Word boundaries are ASCII.
        {"\\bfoo\\b", "a foo b", true},
        {"\\bfoo\\b", "afoob", false},
        {"\\Boo\\B", "afoob", true},
        // Escapes.
        {"^\\cJ$", "\n", true},
        {"^\\x41$", "A", true},
        {"^\\0$", std::string(1, '\0'), true},
        {"^[\\b]$", "\b", true},
        {"^\\/$", "/", true},
        {"^a\\.b$", "a.b", true},
        {"^a\\.b$", "axb", false},
        // Classes.
        {"^[a-c-]+$", "a-b-c", true},
        {"^[\\w-]+$", "a-b_c", true},
        {"^[a-z-9]+$", "ab-9", true},
        {"^[a-z-9]+$", "ab5", false},
        {"^[\\^\\]]+$", "^]", true},
        // General categories.
        {"^\\p{Lu}+$", "ABC", true},
        {"^\\p{Lu}+$", "ABc", false},
        {"^\\p{Uppercase_Letter}$", "\xC3\x89", true},
        {"^\\P{Lu}$", "a", true},
        {"^\\P{Lu}$", "A", false},
        {"^[\\p{L}\\d]+$", "a1\xC3\xA9", true},
        {"^[^\\p{L}]$", "1", true},
        {"^\\p{gc=Nd}+$", "123", true},
        {"^\\p{General_Category=Letter}+$", "ab\xC3\xA9", true},
        {"^\\p{Letter}cole", "\xC3\xA9" "cole", true},
        // Groups, alternation and quantifiers.
        {"^(?:ab|cd){2}$", "abcd", true},
        {"^(?:ab|cd){2}$", "abab", true},
        {"^(?:ab|cd){2}$", "ab", false},
        {"^(?<year>\\d{4})-(?<month>\\d{2})$", "2026-11", true},
        {"^a+?$", "aaa", true},
        {"^a{2,3}$", "aaaa", false},
        {"^a{2,}$", "aaaa", true},
        {"^(a|ab)(c|bcd)(d*)$", "abcd", true},
    };
    for (const Case& entry : cases) {
        EXPECT_EQ(matches(entry.pattern, entry.text), entry.matched)
            << "pattern " << entry.pattern << ", text " << json(entry.text).dump(-1, ' ', false, json::error_handler_t::replace);
    }
}

TEST(JsonSchemaPattern, PatternPropertiesAndPropertyNamesUseTheSameDialect) {
    const json schema = json::parse(R"({"patternProperties":{"^\\p{Lu}.$":{"type":"integer"}},"propertyNames":{"pattern":"^[^\\r\\n]+$"}})");
    auto result = Validator::compile(schema);
    ASSERT_TRUE(std::holds_alternative<Validator>(result));
    const Validator& validator = std::get<Validator>(result);
    EXPECT_FALSE(validator.validate(json{{"\xC3\x89x", 1}}));
    EXPECT_TRUE(validator.validate(json{{"\xC3\x89x", "text"}}));
    EXPECT_FALSE(validator.validate(json{{"\xC3\xA9x", "text"}}));
    EXPECT_TRUE(validator.validate(json{{"a\nb", 1}}));
}

TEST(JsonSchemaPattern, AStringThatIsNotUtf8EndsTheValidationInsteadOfMatching) {
    for (const char* wrapper : {"", "not", "if"}) {
        json schema = json{{"pattern", "^a.b$"}};
        if (std::string(wrapper) == "not") schema = json{{"not", schema}};
        if (std::string(wrapper) == "if") schema = json{{"if", schema}, {"then", true}};
        auto result = Validator::compile(schema);
        ASSERT_TRUE(std::holds_alternative<Validator>(result));
        const auto issue = std::get<Validator>(result).validate(json("a\xFF" "b"));
        ASSERT_TRUE(issue) << wrapper;
        EXPECT_TRUE(contains(issue->message, "not valid UTF-8")) << wrapper << ": " << issue->message;
        EXPECT_EQ(issue->schema_pointer, std::string(wrapper).empty() ? "/pattern" : "/" + std::string(wrapper) + "/pattern")
            << wrapper;
    }
    auto names = Validator::compile(json{{"patternProperties", json{{"^a", true}}}});
    ASSERT_TRUE(std::holds_alternative<Validator>(names));
    const auto issue = std::get<Validator>(names).validate(json{{"a\xC0\x80", 1}});
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "not valid UTF-8")) << issue->message;
    // Well-formed text of every length of sequence is matched.
    EXPECT_TRUE(matches("^a.b.c.d$", "a\xC3\xA9" "b\xE2\x82\xAC" "c\xF0\x9F\x98\x80" "d"));
}

TEST(JsonSchemaPattern, ARefusedPatternIsNamedWithItsConstructAndPosition) {
    struct Refused {
        std::string pattern;
        std::string construct;
        std::string position;
    };
    const std::vector<Refused> cases = {
        {"^(?=a)b", "lookahead assertion", "at byte 1"},
        {"x(?<=a)b", "lookbehind assertion", "at byte 1"},
        {"(a)\\1", "back-reference", "at byte 3"},
        {"(?<n>a)\\k<n>", "named back-reference", "at byte 7"},
        {"(?i)abc", "group modifier", "at byte 0"},
        {"\\p{Script=Greek}", "not a general category", "at byte 0"},
        {"a{2000}", "repeat count above 1000", "at byte 1"},
        {"[z-a]", "end is before its start", "at byte 1"},
        {"a]", "unescaped ']'", "at byte 1"},
    };
    for (const Refused& entry : cases) {
        // pattern
        auto in_pattern = Validator::compile(json{{"properties", json{{"a", json{{"pattern", entry.pattern}}}}}});
        ASSERT_TRUE(std::holds_alternative<Issue>(in_pattern)) << entry.pattern;
        const Issue& issue = std::get<Issue>(in_pattern);
        EXPECT_EQ(issue.schema_pointer, "/properties/a/pattern") << entry.pattern;
        EXPECT_TRUE(contains(issue.message, entry.construct)) << entry.pattern << ": " << issue.message;
        EXPECT_TRUE(contains(issue.message, entry.position)) << entry.pattern << ": " << issue.message;
        EXPECT_TRUE(contains(issue.message, "cannot be matched exactly")) << issue.message;
        // patternProperties
        auto in_key = Validator::compile(json{{"patternProperties", json{{entry.pattern, true}}}});
        ASSERT_TRUE(std::holds_alternative<Issue>(in_key)) << entry.pattern;
        EXPECT_TRUE(contains(std::get<Issue>(in_key).message, entry.construct)) << entry.pattern;
        // propertyNames
        auto in_names = Validator::compile(json{{"propertyNames", json{{"pattern", entry.pattern}}}});
        ASSERT_TRUE(std::holds_alternative<Issue>(in_names)) << entry.pattern;
        EXPECT_EQ(std::get<Issue>(in_names).schema_pointer, "/propertyNames/pattern") << entry.pattern;
    }
}

TEST(JsonSchemaPattern, RepeatCountsThatMultiplyPastTheEngineLimitAreRefused) {
    auto result = compile_pattern("(?:a{100}){100}");
    ASSERT_TRUE(std::holds_alternative<Issue>(result));
    EXPECT_TRUE(contains(std::get<Issue>(result).message, "repeat counts multiply")) << std::get<Issue>(result).message;
}

TEST(JsonSchemaLimits, MaxPatternMemoryRefusesAPatternWhoseProgramDoesNotFit) {
    Limits small;
    small.max_pattern_memory = 8 * 1024;
    // A short pattern fits, and one that repeats a character a thousand times does not.
    EXPECT_TRUE(std::holds_alternative<Validator>(compile_pattern("^ab+c$", small)));
    const auto repeated = compile_pattern("^a{1000}$", small);
    ASSERT_TRUE(std::holds_alternative<Issue>(repeated));
    const Issue& issue = std::get<Issue>(repeated);
    EXPECT_EQ(issue.schema_pointer, "/pattern");
    EXPECT_TRUE(contains(issue.message, "max_pattern_memory")) << issue.message;
    EXPECT_TRUE(contains(issue.message, "8192")) << issue.message;
    // The same pattern is accepted with the default.
    EXPECT_TRUE(std::holds_alternative<Validator>(compile_pattern("^a{1000}$")));
    // A key of patternProperties is refused alike.
    const auto key = Validator::compile(json{{"patternProperties", json{{"^a{1000}$", true}}}}, small);
    ASSERT_TRUE(std::holds_alternative<Issue>(key));
    EXPECT_TRUE(contains(std::get<Issue>(key).message, "max_pattern_memory"));
    // A class that lists many characters, a general category, makes a large program when it is repeated.
    Limits medium;
    medium.max_pattern_memory = 64 * 1024;
    EXPECT_TRUE(std::holds_alternative<Validator>(compile_pattern("^\\p{L}$", medium)));
    const auto large = compile_pattern("^\\p{L}{40}$", medium);
    ASSERT_TRUE(std::holds_alternative<Issue>(large));
    EXPECT_TRUE(contains(std::get<Issue>(large).message, "max_pattern_memory")) << std::get<Issue>(large).message;
    EXPECT_TRUE(contains(std::get<Issue>(large).message, "65536")) << std::get<Issue>(large).message;
    EXPECT_EQ(Limits{}.max_pattern_memory, 8U * 1024U * 1024U);
}

}  // namespace
