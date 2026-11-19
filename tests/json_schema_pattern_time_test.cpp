// Checks that the time `validate` spends on a `pattern` (and on the patterns that property names are matched against)
// is bounded for short strings, whatever the pattern. The strings here are 12 to 21 bytes long, far below
// Limits::max_pattern_input; the patterns are ordinary ones whose repeated group can match the same text in many ways
// (a host-name-like pattern among them). A matcher whose work grows linearly with the input checks such a string in
// microseconds.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "neomcp/json.h"
#include "neomcp/json_schema.h"

namespace {

using neomcp::json;
using neomcp::json_schema::Validator;
using Clock = std::chrono::steady_clock;

// Far more than a linear-time check of a 21-byte string needs on a loaded machine, in any build.
constexpr std::chrono::milliseconds generous{250};

Validator compiled(const char* schema) {
    auto made = Validator::compile(json::parse(schema));
    if (const auto* issue = std::get_if<neomcp::json_schema::Issue>(&made)) {
        throw std::runtime_error("the schema does not compile: " + issue->message);
    }
    return std::get<Validator>(std::move(made));
}

// How long one validation takes.
std::chrono::milliseconds time_of(const Validator& validator, const json& value) {
    const auto start = Clock::now();
    (void)validator.validate(value);
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

// `n` letters followed by a character the pattern does not admit, so that no way of matching succeeds.
std::string almost(std::size_t n) { return std::string(n, 'a') + "!"; }

TEST(JsonSchemaPatternTime, AHostNamePatternChecksAShortStringPromptly) {
    const Validator validator = compiled(R"({"type":"string","pattern":"^([a-z0-9]+-?)+[a-z0-9]$"})");
    const auto took = time_of(validator, json(almost(20)));
    EXPECT_LT(took, generous) << "a 21-byte string took " << took.count() << " ms";
}

TEST(JsonSchemaPatternTime, TheTimeGrowsAboutLinearlyWithTheString) {
    const Validator validator = compiled(R"({"type":"string","pattern":"^(a+)+$"})");
    const auto shorter = time_of(validator, json(almost(12)));
    const auto longer = time_of(validator, json(almost(20)));
    // 21 bytes against 13: a linear matcher takes under twice as long; the allowance is ten times plus a fixed margin.
    EXPECT_LT(longer, shorter * 10 + std::chrono::milliseconds(50))
        << "13 bytes took " << shorter.count() << " ms, 21 bytes took " << longer.count() << " ms";
}

TEST(JsonSchemaPatternTime, APropertyNamePatternChecksAShortNamePromptly) {
    // The client chooses the property names of the arguments it sends.
    const Validator validator = compiled(R"({"type":"object","propertyNames":{"pattern":"^(a+)+$"}})");
    json value = json::object();
    value[almost(20)] = 1;
    const auto took = time_of(validator, value);
    EXPECT_LT(took, generous) << "a 21-byte property name took " << took.count() << " ms";
}

TEST(JsonSchemaPatternTime, APatternPropertiesKeyChecksAShortNamePromptly) {
    const Validator validator = compiled(R"({"type":"object","patternProperties":{"^(a+)+$":{"type":"integer"}}})");
    json value = json::object();
    value[almost(20)] = 1;
    const auto took = time_of(validator, value);
    EXPECT_LT(took, generous) << "a 21-byte property name took " << took.count() << " ms";
}

}  // namespace
