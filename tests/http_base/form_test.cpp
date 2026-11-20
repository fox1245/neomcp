#include "neomcp/http/form.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace neomcp::http;

std::vector<FormField> decoded(std::string_view text) {
    auto result = form_decode(text);
    auto* fields = std::get_if<std::vector<FormField>>(&result);
    EXPECT_NE(fields, nullptr) << "rejected: " << text;
    return fields != nullptr ? *fields : std::vector<FormField>{};
}

FormError rejected(std::string_view text) {
    auto result = form_decode(text);
    auto* error = std::get_if<FormError>(&result);
    EXPECT_NE(error, nullptr) << "accepted: " << text;
    return error != nullptr ? *error : FormError{};
}

// ---- Encoding

// OAuth 2.1, appendix B: SPACE, PERCENT SIGN, AMPERSAND, PLUS SIGN, POUND SIGN, EURO SIGN.
TEST(FormCodec, TheExampleOfOAuth21) {
    const std::string value = " %&+\xc2\xa3\xe2\x82\xac";
    EXPECT_EQ(form_encode_component(value), "+%25%26%2B%C2%A3%E2%82%AC");
    const auto fields = decoded("v=+%25%26%2B%C2%A3%E2%82%AC");
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0].value, value);
}

TEST(FormCodec, UnreservedCharactersStayAsTheyAre) {
    const std::string unreserved = "ABCXYZabcxyz0189-._~";
    EXPECT_EQ(form_encode_component(unreserved), unreserved);
}

TEST(FormCodec, ReservedAndOtherCharactersAreEscaped) {
    EXPECT_EQ(form_encode_component("&=+%/?#:@[]!$'()*,;"),
              "%26%3D%2B%25%2F%3F%23%3A%40%5B%5D%21%24%27%28%29%2A%2C%3B");
    EXPECT_EQ(form_encode_component("\"<>\\^`{|}"), "%22%3C%3E%5C%5E%60%7B%7C%7D");
    EXPECT_EQ(form_encode_component(std::string("\x00\x0a\x0d\x1f\x7f", 5)), "%00%0A%0D%1F%7F");
    EXPECT_EQ(form_encode_component("a b"), "a+b");
}

TEST(FormCodec, FieldsAreJoinedInOrder) {
    const std::vector<FormField> fields = {{"grant_type", "authorization_code"},
                                           {"code", "Splx/lOrzK&x=y"},
                                           {"redirect_uri", "http://127.0.0.1:3000/callback"},
                                           {"empty", ""}};
    EXPECT_EQ(form_encode(fields),
              "grant_type=authorization_code&code=Splx%2FlOrzK%26x%3Dy"
              "&redirect_uri=http%3A%2F%2F127.0.0.1%3A3000%2Fcallback&empty=");
    EXPECT_EQ(form_encode(std::vector<FormField>{}), "");
}

TEST(FormCodec, EveryCodePointClassRoundTrips) {
    std::string ascii;
    for (int c = 0; c < 0x80; ++c) ascii += static_cast<char>(c);
    const std::vector<FormField> fields = {
        {ascii, ascii},
        {"two", "\xc2\x80\xdf\xbf"},
        {"three", "\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf"},
        {"four", "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"},
        {"", "nameless"},
    };
    const std::string text = form_encode(fields);
    EXPECT_EQ(decoded(text), fields);
}

// ---- Decoding

TEST(FormCodec, DecodingReversesTheEscapesInOnePass) {
    const auto fields = decoded("a=x+y&b=x%2By&c=%25%32%42&d=%c2%a3");
    ASSERT_EQ(fields.size(), 4u);
    EXPECT_EQ(fields[0].value, "x y");
    EXPECT_EQ(fields[1].value, "x+y");
    EXPECT_EQ(fields[2].value, "%2B");  // decoded once, not twice
    EXPECT_EQ(fields[3].value, "\xc2\xa3");
}

TEST(FormCodec, PiecesNamesAndValues) {
    const auto fields = decoded("&&a=1&&b&c=&=v&d=x=y&");
    ASSERT_EQ(fields.size(), 5u);
    EXPECT_EQ(fields[0], (FormField{"a", "1"}));
    EXPECT_EQ(fields[1], (FormField{"b", ""}));
    EXPECT_EQ(fields[2], (FormField{"c", ""}));
    EXPECT_EQ(fields[3], (FormField{"", "v"}));
    EXPECT_EQ(fields[4], (FormField{"d", "x=y"}));
    EXPECT_TRUE(decoded("").empty());
    EXPECT_TRUE(decoded("&").empty());
}

TEST(FormCodec, RepeatedNamesAreAllKept) {
    const auto fields = decoded("state=a&state=b");
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[1].value, "b");
}

TEST(FormCodec, BadEscapes) {
    EXPECT_EQ(rejected("%").offset, 0u);
    EXPECT_EQ(rejected("a=%4").offset, 2u);
    EXPECT_EQ(rejected("a=1&b=%G1").offset, 6u);
    EXPECT_EQ(rejected("a=1&%zz=b").offset, 4u);
    EXPECT_EQ(rejected("a=%%41").problem, FormProblem::bad_escape);
    EXPECT_EQ(rejected("a=%4 ").problem, FormProblem::bad_escape);
}

TEST(FormCodec, DecodedOctetsMustBeUtf8) {
    const std::vector<std::string> texts = {
        "a=%FF",           // never in UTF-8
        "a=%C0%AF",        // an overlong form
        "a=%E0%80%AF",     // an overlong form
        "a=%ED%A0%80",     // a surrogate
        "a=%F4%90%80%80",  // above U+10FFFF
        "a=%C2",           // a sequence cut short
        "a=%E2%82",        // a sequence cut short
        "a=%80",           // a continuation byte alone
        "%C3%28=x",        // in a name
        "a=\xff",          // a raw octet
    };
    for (const auto& text : texts) {
        SCOPED_TRACE(text);
        EXPECT_EQ(rejected(text).problem, FormProblem::invalid_utf8);
    }
    EXPECT_EQ(rejected("x=1&a=%FF").offset, 6u);
}

TEST(FormCodec, AnErrorCarriesNoTextOfThePeer) {
    const std::string planted = "planted-code-0123";
    const FormError error = rejected("code=" + planted + "&state=%zz");
    EXPECT_EQ(std::string(to_string(error.problem)).find(planted), std::string::npos);
    EXPECT_EQ(std::string(to_string(FormProblem::invalid_utf8)).find(planted), std::string::npos);
}

// ---- Looking a name up

TEST(FormCodec, FindUniqueField) {
    const auto fields = decoded("code=abc&state=&iss=https%3A%2F%2Fas.example&error=x&error=y&scope=a&scope=");
    const FieldLookup code = find_unique_field(fields, "code");
    EXPECT_EQ(code.state, FieldLookup::State::present);
    EXPECT_EQ(code.value, "abc");
    EXPECT_EQ(find_unique_field(fields, "iss").value, "https://as.example");
    // A field without a value counts as absent.
    EXPECT_EQ(find_unique_field(fields, "state").state, FieldLookup::State::absent);
    EXPECT_EQ(find_unique_field(fields, "scope").state, FieldLookup::State::present);
    EXPECT_EQ(find_unique_field(fields, "error").state, FieldLookup::State::repeated);
    EXPECT_EQ(find_unique_field(fields, "missing").state, FieldLookup::State::absent);
    // Names compare exactly.
    EXPECT_EQ(find_unique_field(fields, "Code").state, FieldLookup::State::absent);
}

// ---- Basic authentication of an OAuth client

TEST(FormCodec, BasicCredentialsOfOAuth21) {
    EXPECT_EQ(oauth_basic_authorization("s6BhdRkqt3", "7Fjfp0ZBr1KtDRbnfVdmIw"),
              "Basic czZCaGRSa3F0Mzo3RmpmcDBaQnIxS3REUmJuZlZkbUl3");
}

TEST(FormCodec, BasicCredentialsEncodeIdAndSecret) {
    // "a%3Ab:c+d": the ':' of the identifier is escaped, so only the separator is a ':'.
    EXPECT_EQ(oauth_basic_authorization("a:b", "c d"), "Basic YSUzQWI6Yytk");
    // "id~:p%40ss+w%2Frd": Base64 with '+' and one '=' of padding, the standard alphabet.
    EXPECT_EQ(oauth_basic_authorization("id~", "p@ss w/rd"), "Basic aWR+OnAlNDBzcyt3JTJGcmQ=");
    // "id~:sec", "id~:s" and "id~:se": one, two and no octets left over.
    EXPECT_EQ(oauth_basic_authorization("id~", "sec"), "Basic aWR+OnNlYw==");
    EXPECT_EQ(oauth_basic_authorization("id~", "s"), "Basic aWR+OnM=");
    EXPECT_EQ(oauth_basic_authorization("id~", "se"), "Basic aWR+OnNl");
}

}  // namespace
