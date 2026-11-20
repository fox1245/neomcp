// encode_header_value and encode_header_text: the value a client puts in the header that mirrors a tool parameter.
//
// The expectations are written from the 2026-07-28 Streamable HTTP page ("Encoding examples" and the rules above
// them) and from the conformance scenario for custom headers, not read back from the encoder. The framing is the one
// the profile gives. The checker of a server (detail::check_argument_headers) is the second reader of what the encoder
// writes: it must accept every value and decode it to the original.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "core/base64.h"
#include "core/headers.h"
#include "neomcp/argument_headers.h"
#include "neomcp/profile.h"
#include "support.h"

namespace {

using namespace neomcp;
using client_core_test::Random;

// The profiles that define argument headers, with the framing of the encoded form.
struct Framing {
    profile::ProfilePtr profile;
    const profile::ArgumentHeaders* headers = nullptr;
    const profile::EncodedHeaderValue* encoded = nullptr;
};

std::vector<Framing> framings() {
    std::vector<Framing> out;
    for (const std::string_view revision : profile::builtin_revisions()) {
        Framing framing;
        framing.profile = profile::builtin(revision);
        const auto& transport = framing.profile->streamable_http();
        if (!transport || !transport->argument_headers || !transport->encoded_header_value) continue;
        framing.headers = &*transport->argument_headers;
        framing.encoded = &*transport->encoded_header_value;
        out.push_back(std::move(framing));
    }
    return out;
}

bool begins(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }
bool ends(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// The rule of the page, as a test states it: plain when every byte is visible ASCII or a space, there is no space at
// either end, and the text does not both begin with the prefix and end with the suffix. A tab is never plain.
bool should_be_plain(std::string_view text, const profile::EncodedHeaderValue& framing) {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7E) return false;
    }
    if (!text.empty() && (text.front() == ' ' || text.back() == ' ')) return false;
    return !(begins(text, framing.prefix) && ends(text, framing.suffix));
}

std::string encoded_text(std::string_view text, const profile::EncodedHeaderValue& framing) {
    return encode_header_text(text, framing);
}

// What the checker makes of a header that mirrors the string parameter `p` of a call whose argument is `argument`.
std::optional<detail::ArgumentHeaderFailure> check(const json& argument, const std::string& header_value,
                                                   const Framing& framing, const char* type = "string") {
    const json schema = json{{"type", "object"},
                             {"properties", json{{"p", json{{"type", type}, {framing.headers->schema_keyword, "P"}}}}}};
    const json arguments = json{{"p", argument}};
    const std::string name = framing.headers->header_prefix + "P";
    const HeaderField field{name, header_value};
    return detail::check_argument_headers(arguments, schema, std::span<const HeaderField>(&field, 1), *framing.headers,
                                          framing.encoded);
}

TEST(HeaderValue, TheProfilesThatHaveArgumentHeadersHaveTheFraming) {
    ASSERT_FALSE(framings().empty());
    for (const Framing& framing : framings()) {
        EXPECT_FALSE(framing.encoded->prefix.empty());
        EXPECT_FALSE(framing.encoded->suffix.empty());
    }
}

// The five rows of "Encoding examples".
TEST(HeaderValue, ThePagesExamplesAreEncodedAsTheyShow) {
    struct Row {
        std::string original;
        std::string encoded;
    };
    const std::vector<Row> rows{
        {"us-west1", "us-west1"},
        {"Hello, \xE4\xB8\x96\xE7\x95\x8C", "=?base64?SGVsbG8sIOS4lueVjA==?="},
        {" padded ", "=?base64?IHBhZGRlZCA=?="},
        {"line1\nline2", "=?base64?bGluZTEKbGluZTI=?="},
        {"=?base64?literal?=", "=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?="},
    };
    for (const Framing& framing : framings()) {
        for (const Row& row : rows) {
            EXPECT_EQ(encoded_text(row.original, *framing.encoded), row.encoded) << row.original;
            const auto value = encode_header_value(json(row.original), *framing.encoded);
            ASSERT_TRUE(std::holds_alternative<std::optional<std::string>>(value));
            EXPECT_EQ(std::get<std::optional<std::string>>(value), std::optional<std::string>(row.encoded));
            EXPECT_FALSE(check(json(row.original), row.encoded, framing).has_value()) << row.original;
        }
    }
}

// The values the conformance scenario for custom headers sends, and what it expects of each.
TEST(HeaderValue, TheConformanceScenarioValuesAreEncodedAsItExpects) {
    struct Row {
        std::string original;
        bool plain;
    };
    const std::vector<Row> rows{
        {"us-west1", true},
        {"", true},
        {"test-method", true},
        {"us west 1", true},  // inner spaces stay plain
        {"SELECT * FROM users", true},
        {"Hello, \xE4\xB8\x96\xE7\x95\x8C", false},
        {" padded ", false},
        {" us-west1", false},
        {"us-west1 ", false},
        {"line1\nline2", false},
        {"line1\r\nline2", false},
        {"\tindented", false},  // the scenario expects a leading tab encoded
        {"tab\tinside", false},
    };
    for (const Framing& framing : framings()) {
        for (const Row& row : rows) {
            const std::string encoded = encoded_text(row.original, *framing.encoded);
            EXPECT_EQ(encoded == row.original, row.plain) << row.original;
            EXPECT_FALSE(check(json(row.original), encoded, framing).has_value()) << row.original;
        }
    }
}

TEST(HeaderValue, AValueThatLooksLikeTheEncodedFormIsEncodedEvenWhenTheyOverlap) {
    // "=?base64?=" begins with the prefix and ends with the suffix, which share their question mark; the page asks for
    // it to be encoded, and the checker, which reads a text that short as plain, still decodes it to the original.
    for (const Framing& framing : framings()) {
        const std::string sentinel = framing.encoded->prefix + framing.encoded->suffix.substr(1);
        EXPECT_NE(encoded_text(sentinel, *framing.encoded), sentinel);
        for (const std::string& value : {framing.encoded->prefix + framing.encoded->suffix,
                                         framing.encoded->prefix + "abcd" + framing.encoded->suffix,
                                         framing.encoded->prefix + "!" + framing.encoded->suffix, sentinel}) {
            const std::string encoded = encoded_text(value, *framing.encoded);
            EXPECT_NE(encoded, value) << value;
            EXPECT_FALSE(check(json(value), encoded, framing).has_value()) << value;
        }
        // Only the whole pattern counts: with one half it is plain.
        EXPECT_EQ(encoded_text(framing.encoded->prefix + "x", *framing.encoded), framing.encoded->prefix + "x");
        EXPECT_EQ(encoded_text("x" + framing.encoded->suffix, *framing.encoded), "x" + framing.encoded->suffix);
    }
}

TEST(HeaderValue, IntegersAreDecimalAndBooleansLowerCase) {
    for (const Framing& framing : framings()) {
        const auto text = [&](const json& value) {
            const auto result = encode_header_value(value, *framing.encoded);
            const auto* made = std::get_if<std::optional<std::string>>(&result);
            return made != nullptr && made->has_value() ? **made : std::string("<none>");
        };
        EXPECT_EQ(text(json(42)), "42");
        EXPECT_EQ(text(json(-7)), "-7");
        EXPECT_EQ(text(json(0)), "0");
        EXPECT_EQ(text(json(max_safe_integer)), "9007199254740991");
        EXPECT_EQ(text(json(-max_safe_integer)), "-9007199254740991");
        EXPECT_EQ(text(json(static_cast<std::uint64_t>(max_safe_integer))), "9007199254740991");
        EXPECT_EQ(text(json(3.0)), "3");  // a whole number is an integer
        EXPECT_EQ(text(json(true)), "true");
        EXPECT_EQ(text(json(false)), "false");
        EXPECT_EQ(text(json(nullptr)), "<none>");  // null has no header
        for (const std::int64_t value : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, std::int64_t{42},
                                         max_safe_integer, -max_safe_integer}) {
            EXPECT_FALSE(check(json(value), text(json(value)), framing, "integer").has_value()) << value;
        }
        EXPECT_FALSE(check(json(true), "true", framing, "boolean").has_value());
        EXPECT_FALSE(check(json(false), "false", framing, "boolean").has_value());
    }
}

TEST(HeaderValue, ValuesThatCannotBeMirroredAreIssues) {
    for (const Framing& framing : framings()) {
        for (const json& value : {json(max_safe_integer + 1), json(-max_safe_integer - 1),
                                  json(static_cast<std::uint64_t>(max_safe_integer) + 1), json(18446744073709551615ULL),
                                  json(3.5), json(1e300), json::array(), json::object()}) {
            const auto result = encode_header_value(value, *framing.encoded);
            ASSERT_TRUE(std::holds_alternative<HeaderValueIssue>(result)) << value.dump();
            EXPECT_FALSE(std::get<HeaderValueIssue>(result).rule.empty());
        }
    }
}

TEST(HeaderValue, NullHasNoHeaderAndIsNotAnIssue) {
    for (const Framing& framing : framings()) {
        const auto result = encode_header_value(json(nullptr), *framing.encoded);
        const auto* made = std::get_if<std::optional<std::string>>(&result);
        ASSERT_NE(made, nullptr);
        EXPECT_FALSE(made->has_value());
    }
}

TEST(HeaderValue, WholeFloatsAreMirroredUpToTheSafeIntegerLimitAndNoFurther) {
    for (const Framing& framing : framings()) {
        const auto text = [&](double value) {
            const auto result = encode_header_value(json(value), *framing.encoded);
            const auto* made = std::get_if<std::optional<std::string>>(&result);
            return made != nullptr && made->has_value() ? **made : std::string("<issue>");
        };
        EXPECT_EQ(text(9007199254740991.0), "9007199254740991");
        EXPECT_EQ(text(-9007199254740991.0), "-9007199254740991");
        EXPECT_EQ(text(9007199254740992.0), "<issue>");
        EXPECT_EQ(text(-9007199254740992.0), "<issue>");
        EXPECT_EQ(text(0.5), "<issue>");
    }
}

TEST(HeaderValue, TextThatIsExactlyTheFramingIsEncodedWhateverTheFraming) {
    // The sentinel rule holds for any framing, also when the text is no longer than the prefix or the suffix.
    const profile::EncodedHeaderValue prefix_is_longer{"ab", "b"};
    EXPECT_EQ(encode_header_text("ab", prefix_is_longer), "abYWI=b");
    EXPECT_EQ(encode_header_text("a", prefix_is_longer), "a");
    EXPECT_EQ(encode_header_text("b", prefix_is_longer), "b");
    const profile::EncodedHeaderValue suffix_is_longer{"a", "ab"};
    EXPECT_EQ(encode_header_text("ab", suffix_is_longer), "aYWI=ab");
    EXPECT_EQ(encode_header_text("a", suffix_is_longer), "a");
}

TEST(HeaderValue, AnIssueNeverRepeatsTheValue) {
    const Framing framing = framings().front();
    const auto result = encode_header_value(json::array({"the-secret-value-0451"}), *framing.encoded);
    ASSERT_TRUE(std::holds_alternative<HeaderValueIssue>(result));
    EXPECT_EQ(std::get<HeaderValueIssue>(result).rule.find("secret"), std::string::npos);
}

// ---- Generated values

// The context bytes the other positions of a generated value are chosen from: the ones that make the decision
// interesting (a space, a tab, the characters of the sentinel).
constexpr std::string_view context = "a =?\t";

// Every byte of a UTF-8 string as the probe: 0x00 to 0x7F as themselves, and the code points U+0080 to U+00FF as their
// two-byte UTF-8 form (a lone byte above 0x7F is not UTF-8, and a JSON string holds nothing else). Larger code points
// are probed through their own sequences.
std::vector<std::string> probes() {
    std::vector<std::string> out;
    for (int byte = 0; byte < 0x80; ++byte) out.emplace_back(1, static_cast<char>(byte));
    for (int code = 0x80; code <= 0xFF; ++code) {
        out.push_back(std::string{static_cast<char>(0xC0 | (code >> 6)), static_cast<char>(0x80 | (code & 0x3F))});
    }
    out.push_back("\xE0\xA0\x80");          // U+0800
    out.push_back("\xEF\xBF\xBF");          // U+FFFF
    out.push_back("\xF0\x90\x80\x80");      // U+10000
    out.push_back("\xF4\x8F\xBF\xBF");      // U+10FFFF
    return out;
}

// Strings of length 1 to 4 (in code points) that have the probe first, in the middle and last, with the other
// positions taken from the context bytes; the empty string besides.
std::vector<std::string> generated_values() {
    Random random(20260719);
    std::vector<std::string> out{""};
    constexpr std::size_t variants = 6;
    for (const std::string& probe : probes()) {
        out.push_back(probe);
        for (std::size_t length = 2; length <= 4; ++length) {
            for (std::size_t position = 0; position < length; ++position) {
                for (std::size_t variant = 0; variant < variants; ++variant) {
                    std::string value;
                    for (std::size_t i = 0; i < length; ++i) {
                        if (i == position) {
                            value += probe;
                        } else if (variant == 0) {
                            value += 'a';
                        } else if (variant == 1) {
                            value += ' ';
                        } else {
                            value += context[random.below(context.size())];
                        }
                    }
                    out.push_back(std::move(value));
                }
            }
        }
    }
    return out;
}

TEST(HeaderValue, EveryGeneratedValueIsPlainOnlyWhereThePageAllowsAndTheCheckerDecodesItBack) {
    const auto values = generated_values();
    EXPECT_GT(values.size(), 10000U);
    for (const Framing& framing : framings()) {
        for (const std::string& value : values) {
            const std::string encoded = encoded_text(value, *framing.encoded);
            const bool plain = should_be_plain(value, *framing.encoded);
            ASSERT_EQ(encoded == value, plain) << "value: [" << value << "] sent as [" << encoded << "]";
            if (!plain) {
                // Framed, with strict standard Base64 of the bytes between.
                ASSERT_TRUE(begins(encoded, framing.encoded->prefix) && ends(encoded, framing.encoded->suffix));
                const std::string_view middle = std::string_view(encoded).substr(
                    framing.encoded->prefix.size(),
                    encoded.size() - framing.encoded->prefix.size() - framing.encoded->suffix.size());
                const auto decoded = detail::decode_base64(middle);
                ASSERT_TRUE(decoded.has_value()) << encoded;
                ASSERT_EQ(*decoded, value);
            }
            // The checker accepts what is written, and so decodes it to the original.
            const auto failure = check(json(value), encoded, framing);
            ASSERT_FALSE(failure.has_value()) << "value: [" << value << "] sent as [" << encoded << "]: "
                                              << (failure ? failure->message : std::string());
            // And it would refuse the same header for another argument.
            const auto mismatch = check(json(value + "x"), encoded, framing);
            ASSERT_TRUE(mismatch.has_value()) << "value: [" << value << "]";
        }
    }
}

TEST(HeaderValue, LongValuesAreHandledAlike) {
    for (const Framing& framing : framings()) {
        for (const std::size_t length : {std::size_t{100}, std::size_t{4096}, std::size_t{70000}}) {
            const std::string body(length, 'x');
            const std::string sentinel_body(length - length % 4, 'A');
            for (const std::string& value :
                 {body, " " + body, body + " ", body + "\t", body + "\xE6\x97\xA5", "\n" + body,
                  framing.encoded->prefix + sentinel_body + framing.encoded->suffix,
                  framing.encoded->prefix + body + framing.encoded->suffix}) {
                const std::string encoded = encoded_text(value, *framing.encoded);
                ASSERT_EQ(encoded == value, should_be_plain(value, *framing.encoded)) << value.size();
                ASSERT_FALSE(check(json(value), encoded, framing).has_value()) << value.size();
            }
        }
    }
}

}  // namespace
