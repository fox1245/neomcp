// WWW-Authenticate values a server controls, at the edges of the grammar and at random: the parser never throws,
// reads nothing past its bound, and what it gives back keeps its own rules.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/http/challenge.h"

namespace {

using neomcp::http::Challenge;
using neomcp::http::ChallengeError;
using neomcp::http::ChallengeLimits;
using neomcp::http::ChallengeProblem;
using neomcp::http::find_bearer_challenge;
using neomcp::http::parse_challenges;

bool parsed(const std::variant<std::vector<Challenge>, ChallengeError>& result) {
    return std::holds_alternative<std::vector<Challenge>>(result);
}

bool lower_case(std::string_view text) {
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') return false;
    }
    return true;
}

// What every parsed result must hold, whatever the input was.
void expect_well_formed(const std::vector<Challenge>& challenges, const ChallengeLimits& limits) {
    for (const auto& challenge : challenges) {
        EXPECT_FALSE(challenge.scheme.empty());
        EXPECT_TRUE(lower_case(challenge.scheme)) << challenge.scheme;
        EXPECT_LE(challenge.params.size(), limits.max_params);
        if (challenge.token68) {
            EXPECT_TRUE(challenge.params.empty());
        }
        for (const auto& param : challenge.params) {
            EXPECT_FALSE(param.name.empty());
            EXPECT_TRUE(lower_case(param.name)) << param.name;
            for (const char c : param.value) {
                const auto byte = static_cast<unsigned char>(c);
                // Field values allow HTAB, SP and visible bytes; a control byte never reaches a parameter.
                EXPECT_TRUE(byte == '\t' || (byte >= 0x20 && byte != 0x7f)) << static_cast<int>(byte);
            }
        }
    }
    // The Bearer reading of any result is defined: a challenge or a reason, never a throw.
    static_cast<void>(find_bearer_challenge(challenges));
}

TEST(ChallengeEdges, BrokenValuesNeverYieldControlBytesOrBrokenNames) {
    for (const char* value : {
             R"(Bearer realm="unterminated)",
             R"(Bearer realm="ends with an escape\)",
             R"(Bearer realm=)",
             R"(Bearer =value)",
             R"(Bearer realm="a" "b")",
             R"(Bearer realm="a",,,, scope=)",
             R"(Bearer realm ="a")",
             R"(Bearer, , ,)",
             R"(, Bearer)",
             "Bearer realm=\"a\x01"
             "b\"",
             "Bearer realm=\"a\r\nX-Injected: 1\"",
         }) {
        const auto result = parse_challenges(std::string_view(value));
        if (parsed(result)) {
            expect_well_formed(std::get<std::vector<Challenge>>(result), ChallengeLimits{});
            // A value with a control byte never comes back inside a parameter.
            for (const auto& challenge : std::get<std::vector<Challenge>>(result)) {
                for (const auto& param : challenge.params) {
                    EXPECT_EQ(param.value.find('\r'), std::string::npos) << value;
                    EXPECT_EQ(param.value.find('\n'), std::string::npos) << value;
                    EXPECT_EQ(param.value.find('\x01'), std::string::npos) << value;
                }
            }
        }
    }
}

TEST(ChallengeEdges, ANulInsideAValueIsNeverKept) {
    const std::string value("Bearer resource_metadata=\"https://a.test/\0/x\"", 45);
    const auto result = parse_challenges(std::string_view(value));
    if (parsed(result)) {
        for (const auto& challenge : std::get<std::vector<Challenge>>(result)) {
            for (const auto& param : challenge.params) {
                EXPECT_EQ(param.value.find('\0'), std::string::npos);
            }
        }
    }
}

TEST(ChallengeEdges, TheByteBoundCountsEveryFieldLineAndIsExact) {
    ChallengeLimits limits;
    limits.max_bytes = 64;
    const std::string head = "Bearer realm=\"";
    const std::string tail = "\"";
    const std::string at_bound = head + std::string(limits.max_bytes - head.size() - tail.size(), 'a') + tail;
    ASSERT_EQ(at_bound.size(), limits.max_bytes);
    EXPECT_TRUE(parsed(parse_challenges(std::string_view(at_bound), limits)));
    const std::string past = head + std::string(limits.max_bytes - head.size() - tail.size() + 1, 'a') + tail;
    const auto refused = parse_challenges(std::string_view(past), limits);
    ASSERT_FALSE(parsed(refused));
    EXPECT_EQ(std::get<ChallengeError>(refused).problem, ChallengeProblem::too_large);
    // Split over field lines, the same bytes are counted together.
    const std::vector<std::string> lines{past.substr(0, 30), past.substr(30)};
    const auto split = parse_challenges(std::span<const std::string>(lines), limits);
    ASSERT_FALSE(parsed(split));
    EXPECT_EQ(std::get<ChallengeError>(split).problem, ChallengeProblem::too_large);
}

TEST(ChallengeEdges, ManyCommasAndManyParametersStayWithinTheBounds) {
    const ChallengeLimits limits;
    std::string commas = "Bearer";
    commas += std::string(20000, ',');
    const auto first = parse_challenges(std::string_view(commas), limits);
    if (parsed(first)) {
        expect_well_formed(std::get<std::vector<Challenge>>(first), limits);
    }
    std::string params = "Bearer ";
    for (std::size_t i = 0; i < limits.max_params + 10; ++i) {
        if (i != 0) params += ", ";
        params += "p" + std::to_string(i) + "=v";
    }
    const auto second = parse_challenges(std::string_view(params), limits);
    ASSERT_TRUE(parsed(second));
    const auto& challenges = std::get<std::vector<Challenge>>(second);
    ASSERT_EQ(challenges.size(), 1u);
    EXPECT_TRUE(challenges[0].too_many_parameters);
    expect_well_formed(challenges, limits);
}

TEST(ChallengeEdges, ACommaInsideAQuotedValueDoesNotEndTheChallenge) {
    const auto result =
        parse_challenges(std::string_view(R"(Bearer scope="a, b=c", resource_metadata="https://a.test/x")"));
    ASSERT_TRUE(parsed(result));
    const auto& challenges = std::get<std::vector<Challenge>>(result);
    ASSERT_EQ(challenges.size(), 1u);
    ASSERT_NE(challenges[0].param("scope"), nullptr);
    EXPECT_EQ(*challenges[0].param("scope"), "a, b=c");
    ASSERT_NE(challenges[0].param("resource_metadata"), nullptr);
    EXPECT_EQ(*challenges[0].param("resource_metadata"), "https://a.test/x");
}

TEST(ChallengeEdges, RandomValuesNeverBreakTheParser) {
    // An alphabet of the bytes the grammar gives a meaning to, plus a few it does not.
    const std::string alphabet = std::string("Bb aA=,\"\\\t;/:.-_~+!#$%&'*^`|0123456789") + "\x01\x7f\xc3\xa9\r\n" +
                                 std::string(1, '\0');
    std::mt19937 random(20261121u);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::uniform_int_distribution<std::size_t> length(0, 200);
    ChallengeLimits limits;
    limits.max_bytes = 160;
    limits.max_params = 8;
    std::size_t parsed_count = 0;
    for (int round = 0; round < 20000; ++round) {
        std::string value;
        const std::size_t n = length(random);
        // Start most values like a challenge, so the parser gets past the first token.
        if (round % 3 != 0) value = "Bearer ";
        for (std::size_t i = 0; i < n; ++i) value += alphabet[pick(random)];
        const auto result = parse_challenges(std::string_view(value), limits);
        if (value.size() > limits.max_bytes) {
            ASSERT_FALSE(parsed(result)) << "read past its bound";
            EXPECT_EQ(std::get<ChallengeError>(result).problem, ChallengeProblem::too_large);
            continue;
        }
        if (parsed(result)) {
            ++parsed_count;
            expect_well_formed(std::get<std::vector<Challenge>>(result), limits);
        } else {
            EXPECT_LE(std::get<ChallengeError>(result).offset, value.size());
        }
    }
    EXPECT_GT(parsed_count, 0u);
}

}  // namespace
