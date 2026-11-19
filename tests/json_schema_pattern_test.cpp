// What `pattern`, `patternProperties` and `propertyNames` do with the regular expression dialect of JSON Schema:
// the meaning of each construct where it differs from other dialects, the patterns that are refused and how, and the
// bound on the memory a pattern may take.
#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
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

TEST(JsonSchemaPattern, ThePatternIsSearchedForAndNotAnchored) {
    EXPECT_TRUE(matches("b", "abc"));
    EXPECT_FALSE(matches("^b", "abc"));
    EXPECT_FALSE(matches("b$", "abc"));
    EXPECT_TRUE(matches("^abc$", "abc"));
    EXPECT_TRUE(matches("", "anything"));
    EXPECT_TRUE(matches("", ""));
}

// The cases of tests/data/pattern_cases.json: a pattern and either the answer of a search for each of its texts or the
// fact that the pattern is a syntax error in Unicode mode. The same file is checked against a JavaScript engine by
// tests/pattern_oracle/generate.js, so the expectations have one source and two engines agree with it.
json read_cases() {
    std::ifstream stream(NEOMCP_PATTERN_CASES_FILE, std::ios::binary);
    if (!stream) throw std::runtime_error(std::string("cannot open ") + NEOMCP_PATTERN_CASES_FILE);
    std::ostringstream text;
    text << stream.rdbuf();
    return json::parse(text.str());
}

TEST(JsonSchemaPattern, TheMeaningOfEachConstructIsThatOfEcmaScriptInUnicodeMode) {
    const json document = read_cases();
    std::size_t compared = 0;
    for (const json& entry : document.at("cases")) {
        if (entry.contains("syntax_error")) continue;
        const std::string pattern = entry.at("pattern").get<std::string>();
        for (const json& item : entry.at("texts")) {
            const std::string text = item.at("text").get<std::string>();
            EXPECT_EQ(matches(pattern, text), item.at("match").get<bool>())
                << "pattern " << pattern << ", text " << json(text).dump(-1, ' ', false, json::error_handler_t::replace);
            ++compared;
        }
    }
    EXPECT_GE(compared, 100U);
}

TEST(JsonSchemaPattern, APatternThatIsASyntaxErrorInUnicodeModeIsRefused) {
    const json document = read_cases();
    std::size_t refused = 0;
    for (const json& entry : document.at("cases")) {
        if (!entry.contains("syntax_error")) continue;
        const std::string pattern = entry.at("pattern").get<std::string>();
        const auto result = compile_pattern(pattern);
        EXPECT_TRUE(std::holds_alternative<Issue>(result)) << "accepted: " << pattern;
        ++refused;
    }
    EXPECT_GE(refused, 50U);
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
