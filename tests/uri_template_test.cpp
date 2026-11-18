// Tests of the URI template parser and matcher. The examples marked RFC 6570 are the ones of section 3.2 of that
// document (levels 1 to 3), read here as URIs to match rather than as expansions to produce.
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/uri_template.h"

namespace {

using neomcp::UriTemplate;
using Bindings = UriTemplate::Bindings;

UriTemplate parsed(const std::string& text) {
    auto result = UriTemplate::parse(text);
    if (const auto* error = std::get_if<neomcp::UriTemplateError>(&result)) {
        ADD_FAILURE() << "'" << text << "' did not parse: " << error->message << " at " << error->position;
        return std::get<UriTemplate>(UriTemplate::parse("x"));
    }
    return std::get<UriTemplate>(std::move(result));
}

std::optional<neomcp::UriTemplateError> parse_error(const std::string& text) {
    auto result = UriTemplate::parse(text);
    if (auto* error = std::get_if<neomcp::UriTemplateError>(&result)) return *error;
    return std::nullopt;
}

struct MatchCase {
    std::string tpl;
    std::string uri;
    std::optional<Bindings> expected;  // empty: no match
};

// ---------------------------------------------------------------------------------------------------------------
// Parsing

TEST(UriTemplateParse, ReadsLiteralsAndExpressionsInOrder) {
    const UriTemplate tpl = parsed("file:///{+path}/x{?a,b}");
    ASSERT_EQ(tpl.parts().size(), 4u);
    EXPECT_EQ(std::get<std::string>(tpl.parts()[0]), "file:///");
    const auto& reserved = std::get<UriTemplate::Expression>(tpl.parts()[1]);
    EXPECT_EQ(reserved.op, UriTemplate::Operator::reserved);
    ASSERT_EQ(reserved.variables.size(), 1u);
    EXPECT_EQ(reserved.variables[0].name, "path");
    EXPECT_EQ(std::get<std::string>(tpl.parts()[2]), "/x");
    const auto& query = std::get<UriTemplate::Expression>(tpl.parts()[3]);
    EXPECT_EQ(query.op, UriTemplate::Operator::query);
    ASSERT_EQ(query.variables.size(), 2u);
    EXPECT_EQ(query.variables[1].name, "b");
    EXPECT_EQ(tpl.variables(), (std::vector<std::string>{"path", "a", "b"}));
    EXPECT_EQ(tpl.text(), "file:///{+path}/x{?a,b}");
}

TEST(UriTemplateParse, EveryOperatorOfTheRfcParses) {
    const std::vector<std::pair<std::string, UriTemplate::Operator>> table{
        {"{v}", UriTemplate::Operator::simple},          {"{+v}", UriTemplate::Operator::reserved},
        {"{#v}", UriTemplate::Operator::fragment},       {"{.v}", UriTemplate::Operator::label},
        {"{/v}", UriTemplate::Operator::path},           {"{;v}", UriTemplate::Operator::path_parameter},
        {"{?v}", UriTemplate::Operator::query},          {"{&v}", UriTemplate::Operator::query_continuation},
    };
    for (const auto& [text, op] : table) {
        SCOPED_TRACE(text);
        const UriTemplate tpl = parsed(text);
        ASSERT_EQ(tpl.parts().size(), 1u);
        EXPECT_EQ(std::get<UriTemplate::Expression>(tpl.parts()[0]).op, op);
    }
}

TEST(UriTemplateParse, ModifiersAndNamesParse) {
    const UriTemplate tpl = parsed("{a:3,b*,c.d,%41x,_y}");
    const auto& expression = std::get<UriTemplate::Expression>(tpl.parts()[0]);
    ASSERT_EQ(expression.variables.size(), 5u);
    EXPECT_EQ(expression.variables[0].prefix_length, std::optional<std::size_t>(3));
    EXPECT_TRUE(expression.variables[1].explode);
    EXPECT_EQ(expression.variables[2].name, "c.d");
    EXPECT_EQ(expression.variables[3].name, "%41x");
    EXPECT_EQ(expression.variables[4].name, "_y");
    EXPECT_TRUE(parse_error("{a:9999}") == std::nullopt);
}

TEST(UriTemplateParse, MalformedTemplatesAreErrorsWithAPosition) {
    struct Case {
        std::string text;
        std::size_t position;
    };
    const std::vector<Case> table{
        {"a{b", 1},            // an expression is not closed
        {"a}b", 1},            // a closing brace alone
        {"a{b{c}}", 1},        // a brace inside an expression
        {"{}", 0},             // empty expression
        {"{=v}", 1},           // reserved for future extensions
        {"{,v}", 1},           //
        {"{!v}", 1},           //
        {"{@v}", 1},           //
        {"{|v}", 1},           //
        {"{v,}", 3},           // a variable name is missing
        {"{,}", 1},            //
        {"{a b}", 2},          // a space in a name
        {"{a-b}", 2},          // a character no name may hold
        {"{a..b}", 3},         // two dots
        {"{a.}", 2},           // a name ending in a dot
        {"{a:0}", 3},          // prefix length 0
        {"{a:}", 3},           // prefix length missing
        {"{a:10000}", 3},      // five digits
        {"{a:01}", 3},         // leading zero
        {"{a%4}", 2},          // truncated percent-encoded octet
        {"{a%zz}", 2},         // not hexadecimal
        {"a b", 1},            // space in a literal
        {"a\"b", 1},           //
        {"a<b", 1},            //
        {"a\\b", 1},           //
        {"a%", 1},             // truncated octet in a literal
        {"a%2", 1},            //
        {"a%2g", 1},           //
        {"a|b", 1},            //
        {"a^b", 1},            //
        {"a`b", 1},            //
        {"a'b", 1},            //
    };
    for (const auto& entry : table) {
        SCOPED_TRACE(entry.text);
        const auto error = parse_error(entry.text);
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->position, entry.position) << error->message;
        EXPECT_FALSE(error->message.empty());
    }
}

TEST(UriTemplateParse, ReservedForFutureOperatorIsNamed) {
    const auto error = parse_error("{=v}");
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->message.find("'='"), std::string::npos) << error->message;
}

TEST(UriTemplateParse, PercentEncodedOctetsAndNonAsciiBytesAreLiteralText) {
    EXPECT_FALSE(parse_error("a%20b/\xc3\xa9"));
    EXPECT_EQ(std::get<std::string>(parsed("a%20b/\xc3\xa9").parts()[0]), "a%20b/\xc3\xa9");
}

// ---------------------------------------------------------------------------------------------------------------
// What matching serves

TEST(UriTemplateSupport, TheMatchedFormsAreSupported) {
    for (const char* text : {"x", "{v}", "{+v}", "{a,b}", "{+a,b}", "a/{v}/b", "{v}{?q}", "s:{a}-{b}{?c,d}", "{?q}", "{+a}{?q}"}) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(parsed(text).unsupported().has_value()) << *parsed(text).unsupported();
    }
}

TEST(UriTemplateSupport, OtherFormsParseAndAreReportedByName) {
    const std::vector<std::pair<std::string, std::string>> table{
        // RFC 6570 level 2: fragment expansion.
        {"X{#var}", "'#'"},
        {"X{#hello}", "'#'"},
        {"{.v}", "'.'"},
        {"{/v}", "'/'"},
        {"{;v}", "';'"},
        {"{&v}", "'&'"},
        {"{v*}", "'*'"},
        {"{v:3}", "':3'"},
        {"{a}{b}", "adjacent"},
        {"{+a}{+b}", "adjacent"},
        {"{?a}/x", "not the end"},
        {"{?a}{?b}", "not the end"},
        {"x?y={?a}", "literal '?'"},
        {"{a}/{a}", "'a' appears more than once"},
        {"{a,a}", "'a' appears more than once"},
        {"{?a,a}", "'a' appears more than once"},
    };
    for (const auto& [text, part] : table) {
        SCOPED_TRACE(text);
        const UriTemplate tpl = parsed(text);
        ASSERT_TRUE(tpl.unsupported().has_value());
        EXPECT_NE(tpl.unsupported()->find(part), std::string::npos) << *tpl.unsupported();
        EXPECT_FALSE(tpl.match("anything")) << "a template that cannot be matched matches nothing";
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Matching

void expect_cases(const std::vector<MatchCase>& cases) {
    for (const auto& entry : cases) {
        SCOPED_TRACE(entry.tpl + " against " + entry.uri);
        const UriTemplate tpl = parsed(entry.tpl);
        ASSERT_FALSE(tpl.unsupported().has_value()) << *tpl.unsupported();
        EXPECT_EQ(tpl.match(entry.uri), entry.expected);
    }
}

// Level 1 and 2 expansions of section 3.2.1 and 3.2.2, read the other way.
TEST(UriTemplateMatch, Rfc6570LevelsOneAndTwo) {
    expect_cases({
        {"{var}", "value", Bindings{{"var", "value"}}},
        {"{hello}", "Hello%20World%21", Bindings{{"hello", "Hello World!"}}},
        {"{+var}", "value", Bindings{{"var", "value"}}},
        {"{+hello}", "Hello%20World!", Bindings{{"hello", "Hello World!"}}},
        {"{+path}/here", "/foo/bar/here", Bindings{{"path", "/foo/bar"}}},
        {"here?ref={+path}", "here?ref=/foo/bar", Bindings{{"path", "/foo/bar"}}},
        // Level 3 multiple variables in one expression, and the form-style query.
        {"map?{x,y}", "map?1024,768", Bindings{{"x", "1024"}, {"y", "768"}}},
        {"{x,hello,y}", "1024,Hello%20World%21,768",
         Bindings{{"x", "1024"}, {"hello", "Hello World!"}, {"y", "768"}}},
        {"{+x,hello,y}", "1024,Hello%20World!,768",
         Bindings{{"x", "1024"}, {"hello", "Hello World!"}, {"y", "768"}}},
        {"{?x,y}", "?x=1024&y=768", Bindings{{"x", "1024"}, {"y", "768"}}},
        {"{?x,y,empty}", "?x=1024&y=768&empty=",
         Bindings{{"x", "1024"}, {"y", "768"}, {"empty", ""}}},
    });
}

// What a resource template is used for in practice, and the case the official conformance suite reads.
TEST(UriTemplateMatch, TypicalResourceTemplates) {
    expect_cases({
        {"test://template/{id}/data", "test://template/123/data", Bindings{{"id", "123"}}},
        {"test://template/{id}/data", "test://template/123/other", std::nullopt},
        {"test://template/{id}/data", "test://template/1/2/data", std::nullopt},  // {id} never holds '/'
        {"file:///{path}", "file:///readme", Bindings{{"path", "readme"}}},
        {"file:///{path}", "file:///dir/readme", std::nullopt},
        {"file:///{+path}", "file:///dir/readme", Bindings{{"path", "dir/readme"}}},
        {"users/{id}.json", "users/42.json", Bindings{{"id", "42"}}},
        {"users/{id}.json", "users/a.b.json", Bindings{{"id", "a.b"}}},
        {"users/{id}.json", "users/42.txt", std::nullopt},
        {"db://{schema}/{table}", "db://public/orders", Bindings{{"schema", "public"}, {"table", "orders"}}},
        {"db://{schema}/{table}", "db://public", std::nullopt},
        {"db://{schema}/{table}", "db://public/orders/extra", std::nullopt},
        {"a/{x}/b/{y}/c", "a/1/b/2/c", Bindings{{"x", "1"}, {"y", "2"}}},
        {"plain", "plain", Bindings{}},
        {"plain", "plainer", std::nullopt},
        {"plain", "Plain", std::nullopt},  // literal text is case-sensitive
    });
}

TEST(UriTemplateMatch, TheRuleForVariablesFollowedByTextIsShortestThenRest) {
    expect_cases({
        {"{a}-{b}", "x-y-z", Bindings{{"a", "x"}, {"b", "y-z"}}},
        {"{a}-{b}-{c}", "1-2-3-4", Bindings{{"a", "1"}, {"b", "2"}, {"c", "3-4"}}},
        {"{a}-{b}", "x-", Bindings{{"a", "x"}, {"b", ""}}},
        {"{a}-{b}", "xy", std::nullopt},
    });
}

TEST(UriTemplateMatch, ReservedExpansionMayHoldReservedCharacters) {
    expect_cases({
        {"s://{+p}", "s://a/b?c=d#e", Bindings{{"p", "a/b?c=d#e"}}},
        {"s://{p}", "s://a?c", std::nullopt},
        {"s://{p}", "s://a:b", std::nullopt},
        {"s://{+p}/end", "s://a/b/end", Bindings{{"p", "a/b"}}},
        {"{+a}/{+b}", "x/y/z", Bindings{{"a", "x"}, {"b", "y/z"}}},
    });
}

TEST(UriTemplateMatch, ValuesArePercentDecodedAndMalformedEscapesRefused) {
    expect_cases({
        {"f/{n}", "f/a%20b", Bindings{{"n", "a b"}}},
        {"f/{n}", "f/a%2Fb", Bindings{{"n", "a/b"}}},  // an encoded '/' is a value, not a separator
        {"f/{n}", "f/%C3%A9", Bindings{{"n", "\xc3\xa9"}}},
        {"f/{n}", "f/%2", std::nullopt},
        {"f/{n}", "f/%2g", std::nullopt},
        {"f/{n}", "f/%", std::nullopt},
        {"f/{n}", "f/%zz", std::nullopt},
        {"f/{n}/g", "f/%4/g", std::nullopt},
        {"f/{+n}", "f/a%2", std::nullopt},
        {"f/{n}", "f/a b", std::nullopt},        // a space is not a value character
        {"f/{n}", "f/a\"b", std::nullopt},
        {"f/{n}", "f/\xc3\xa9", Bindings{{"n", "\xc3\xa9"}}},  // bytes above 0x7F are taken as they are
    });
}

TEST(UriTemplateMatch, QueryPairsAreUnorderedAndOptional) {
    expect_cases({
        {"search://items{?q,limit}", "search://items", Bindings{}},
        {"search://items{?q,limit}", "search://items?q=cat", Bindings{{"q", "cat"}}},
        {"search://items{?q,limit}", "search://items?limit=5&q=cat", Bindings{{"q", "cat"}, {"limit", "5"}}},
        {"search://items{?q,limit}", "search://items?q=a%20b", Bindings{{"q", "a b"}}},
        {"search://items{?q,limit}", "search://items?q=cat&q=dog", std::nullopt},  // a name once
        {"search://items{?q,limit}", "search://items?other=1", std::nullopt},      // a name the template lacks
        {"search://items{?q,limit}", "search://items?q", std::nullopt},            // no '='
        {"search://items{?q,limit}", "search://items?", std::nullopt},             // nothing after '?'
        {"search://items{?q,limit}", "search://items?q=1&", std::nullopt},         // an empty pair
        {"search://items{?q,limit}", "search://items?q=a/b", std::nullopt},        // '/' is not a value character
        {"search://items{?q,limit}", "search://itemsX?q=1", std::nullopt},
        {"search://{kind}{?q}", "search://books?q=x", Bindings{{"kind", "books"}, {"q", "x"}}},
        {"search://{kind}{?q}", "search://books", Bindings{{"kind", "books"}}},
        {"{?q}", "?q=1", Bindings{{"q", "1"}}},
        {"{+path}{?v}", "a/b?v=1", Bindings{{"path", "a/b"}, {"v", "1"}}},
    });
}

TEST(UriTemplateMatch, TemplatesWithoutVariablesMatchOnlyTheirOwnText) {
    expect_cases({
        {"", "", Bindings{}},
        {"", "x", std::nullopt},
        {"a:b", "a:b", Bindings{}},
    });
}

TEST(UriTemplateMatch, AnEmptyValueMatches) {
    expect_cases({
        {"file:///{path}", "file:///", Bindings{{"path", ""}}},
        {"a/{x}/b", "a//b", Bindings{{"x", ""}}},
    });
}

TEST(UriTemplateMatch, AValueNeverStretchesPastWhereTheNextTextFirstStarts) {
    // {a} is the whole run before the final "/b"; a '/' inside it is not a value character, and nothing is retried.
    const UriTemplate tpl = parsed("{a}/b");
    EXPECT_EQ(tpl.match("x/b"), (std::optional<Bindings>(Bindings{{"a", "x"}})));
    EXPECT_FALSE(tpl.match("x/y/b"));
    EXPECT_FALSE(tpl.match("x/bb"));
}

TEST(UriTemplateMatch, LongInputsTakeLinearWork) {
    const UriTemplate tpl = parsed("{a}-{b}-{c}-{d}-{e}-{f}");
    std::string uri;
    for (int i = 0; i < 20000; ++i) uri += "ab";
    EXPECT_FALSE(tpl.match(uri));  // no '-' at all: one pass, then done
    uri = "a-b-c-d-e-" + uri;
    EXPECT_TRUE(tpl.match(uri));
}

}  // namespace
