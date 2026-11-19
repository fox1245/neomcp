// Checks that the time `validate` spends on a `pattern` (and on the patterns that property names are matched against)
// stays small for short strings, whatever the pattern. The patterns are ordinary ones whose repeated group can match
// the same text in many ways (a host-name-like pattern among them); the strings grow two bytes at a time up to 64
// bytes, far below Limits::max_pattern_input. A matcher whose work grows linearly with the input checks each of them
// in microseconds in any build. Each test stops at the first check that takes `slow` or more, so a matcher whose work
// multiplies with every byte is caught after one such check, however fast or slow the build is.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
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
using std::chrono::milliseconds;

// Far more than a linear-time check of a 65-byte string needs on a loaded machine, in any build.
constexpr milliseconds slow{200};
constexpr std::size_t shortest = 12;
constexpr std::size_t longest = 64;

Validator compiled(const char* schema) {
    auto made = Validator::compile(json::parse(schema));
    if (const auto* issue = std::get_if<neomcp::json_schema::Issue>(&made)) {
        throw std::runtime_error("the schema does not compile: " + issue->message);
    }
    return std::get<Validator>(std::move(made));
}

milliseconds time_of(const Validator& validator, const json& value) {
    const auto start = Clock::now();
    (void)validator.validate(value);
    return std::chrono::duration_cast<milliseconds>(Clock::now() - start);
}

// `n` letters followed by a character the pattern does not admit, so that no way of matching succeeds.
std::string almost(std::size_t n) { return std::string(n, 'a') + "!"; }

struct Slow {
    std::size_t letters;
    milliseconds took;
};

// The first length from `shortest` to `longest` (two bytes at a time) whose check takes `slow` or more.
std::optional<Slow> first_slow(const Validator& validator, const std::function<json(std::size_t)>& value_of) {
    for (std::size_t n = shortest; n <= longest; n += 2) {
        const milliseconds took = time_of(validator, value_of(n));
        if (took >= slow) return Slow{n, took};
    }
    return std::nullopt;
}

json as_string(std::size_t n) { return json(almost(n)); }

json as_property_name(std::size_t n) {
    json value = json::object();
    value[almost(n)] = 1;
    return value;
}

TEST(JsonSchemaPatternTime, AHostNamePatternChecksShortStringsPromptly) {
    const Validator validator = compiled(R"({"type":"string","pattern":"^([a-z0-9]+-?)+[a-z0-9]$"})");
    const auto found = first_slow(validator, as_string);
    EXPECT_FALSE(found) << "a string of " << found->letters + 1 << " bytes took " << found->took.count() << " ms";
}

TEST(JsonSchemaPatternTime, TheTimeGrowsAboutLinearlyWithTheString) {
    const Validator validator = compiled(R"({"type":"string","pattern":"^(a+)+$"})");
    const auto found = first_slow(validator, as_string);
    if (!found) return;  // never slow: nothing grows faster than the bound allows
    const milliseconds shorter = time_of(validator, as_string(found->letters - 8));
    // Eight bytes fewer: a linear matcher takes at most about the same time; the allowance is four times plus a margin.
    EXPECT_LT(found->took, shorter * 4 + milliseconds(20))
        << found->letters - 7 << " bytes took " << shorter.count() << " ms, " << found->letters + 1 << " bytes took "
        << found->took.count() << " ms";
}

TEST(JsonSchemaPatternTime, APropertyNamePatternChecksShortNamesPromptly) {
    // The client chooses the property names of the arguments it sends.
    const Validator validator = compiled(R"({"type":"object","propertyNames":{"pattern":"^(a+)+$"}})");
    const auto found = first_slow(validator, as_property_name);
    EXPECT_FALSE(found) << "a property name of " << found->letters + 1 << " bytes took " << found->took.count() << " ms";
}

TEST(JsonSchemaPatternTime, APatternPropertiesKeyChecksShortNamesPromptly) {
    const Validator validator = compiled(R"({"type":"object","patternProperties":{"^(a+)+$":{"type":"integer"}}})");
    const auto found = first_slow(validator, as_property_name);
    EXPECT_FALSE(found) << "a property name of " << found->letters + 1 << " bytes took " << found->took.count() << " ms";
}

}  // namespace
