// Checking a string against a pattern takes time proportional to the length of the string, whatever the pattern:
// patterns with nested quantifiers and alternations that can match one text in very many ways are checked against
// strings that double in length, and the time must not grow faster than the length. The test compares ratios, not
// seconds, so it does not depend on the speed of the machine; it is registered only with NEOMCP_TEST_TIME_BOUNDS,
// because instrumented builds slow different parts of a run by different factors.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/json_schema.h"

namespace {

using neomcp::json;
using neomcp::json_schema::Issue;
using neomcp::json_schema::Limits;
using neomcp::json_schema::Validator;
using Clock = std::chrono::steady_clock;

constexpr std::size_t shortest = std::size_t{1} << 16;
constexpr std::size_t doublings = 4;  // up to 1 MiB
constexpr int repeats = 5;

struct Case {
    std::string pattern;
    std::string unit;    // the text is this, repeated
    std::string suffix;  // and then this, so that no way of matching succeeds
};

// The shortest time of a few runs, in seconds.
double best_time(const Validator& validator, const json& value) {
    double best = 0;
    for (int run = 0; run < repeats; ++run) {
        const auto start = Clock::now();
        const auto issue = validator.validate(value);
        const double took = std::chrono::duration<double>(Clock::now() - start).count();
        // Every text is built to contain no match.
        EXPECT_TRUE(issue.has_value());
        if (run == 0 || took < best) best = took;
    }
    return best;
}

std::string repeated(const std::string& unit, std::size_t bytes, const std::string& suffix) {
    std::string text;
    text.reserve(bytes + suffix.size());
    while (text.size() < bytes) text += unit;
    return text + suffix;
}

TEST(JsonSchemaPatternLinear, TheTimeDoesNotGrowFasterThanTheStringDoubles) {
    const std::vector<Case> cases = {
        {"^(a+)+$", "a", "!"},
        {"^([a-z0-9]+-?)+[a-z0-9]$", "a", "!"},
        {"(x+x+)+y", "x", ""},
        {"^(a|aa)+$", "a", "!"},
        {"^(a*)*b$", "a", ""},
        {"^(([a-z])+.)+[A-Z]([a-z])+$", "a", "!"},
        {"(.*a){20}", "b", ""},
        {"^(a?){60}a{60}$", "a", ""},
        {"^(?:(?:a|b)*)*c", "ab", ""},
        {"(?:[a-z]+\\d?)+\\p{Lu}", "ab", ""},
    };
    Limits limits;
    limits.max_pattern_input = shortest << doublings;
    limits.max_pattern_length = 1024;
    for (const Case& entry : cases) {
        auto result = Validator::compile(json{{"pattern", entry.pattern}}, limits);
        ASSERT_TRUE(std::holds_alternative<Validator>(result)) << entry.pattern;
        const Validator& validator = std::get<Validator>(result);
        // The first run builds the tables the engine fills while it matches.
        (void)validator.validate(json(repeated(entry.unit, shortest, entry.suffix)));
        std::vector<double> times;
        for (std::size_t step = 0; step <= doublings; ++step) {
            const std::size_t bytes = shortest << step;
            times.push_back(best_time(validator, json(repeated(entry.unit, bytes, entry.suffix))));
        }
        // Linear: 16 times the text takes about 16 times as long. The allowance is three times that, and a floor
        // under the first time keeps a single scheduling delay from making the ratio meaningless. A matcher whose time
        // grows with the square of the length takes 256 times as long.
        const double first = std::max(times.front(), 2e-4);
        const double growth = times.back() / first;
        const double length_ratio = static_cast<double>(std::size_t{1} << doublings);
        EXPECT_LT(growth, length_ratio * 3.0) << entry.pattern << ": " << times.front() << " s for " << shortest
                                              << " bytes, " << times.back() << " s for " << (shortest << doublings)
                                              << " bytes";
        // And no single doubling costs more than four times the one before.
        for (std::size_t step = 1; step < times.size(); ++step) {
            EXPECT_LT(times[step], std::max(times[step - 1], 2e-4) * 4.0)
                << entry.pattern << ", doubling " << step << ": " << times[step - 1] << " s then " << times[step] << " s";
        }
    }
}

TEST(JsonSchemaPatternLinear, TheLongestAllowedPatternAgainstTheLongestAllowedStringIsPrompt) {
    // The defaults: a pattern of 512 bytes with a repeated group, and a string of 256 bytes.
    const Limits limits;
    std::string pattern = "^";
    while (pattern.size() + 8 <= limits.max_pattern_length) pattern += "(a+)+b?";
    pattern += "$";
    auto result = Validator::compile(json{{"pattern", pattern}}, limits);
    ASSERT_TRUE(std::holds_alternative<Validator>(result));
    const auto start = Clock::now();
    (void)std::get<Validator>(result).validate(json(std::string(limits.max_pattern_input - 1, 'a') + "!"));
    EXPECT_LT(std::chrono::duration<double>(Clock::now() - start).count(), 1.0);
}

}  // namespace
