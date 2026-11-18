// Tests of the JSON Schema 2020-12 validator: the keywords one by one, the pointers of an Issue, everything
// `compile` refuses, and each bound of Limits just inside and just outside.
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <thread>
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

// Compile a schema that the test expects to be accepted.
std::optional<Validator> compiled(const json& schema, Limits limits = {}) {
    auto result = Validator::compile(schema, limits);
    if (auto* validator = std::get_if<Validator>(&result)) return *validator;
    const Issue& issue = std::get<Issue>(result);
    ADD_FAILURE() << "the schema is refused at " << issue.schema_pointer << ": " << issue.message;
    return std::nullopt;
}

// The Issue that compiling refuses a schema with.
Issue refusal(const json& schema, Limits limits = {}) {
    auto result = Validator::compile(schema, limits);
    if (auto* issue = std::get_if<Issue>(&result)) return *issue;
    ADD_FAILURE() << "the schema was accepted: " << schema.dump();
    return {};
}

// The first failure of a value, or nothing when it is valid.
std::optional<Issue> check(const json& schema, const json& value, Limits limits = {}) {
    const auto validator = compiled(schema, limits);
    if (!validator) return Issue{"", "", "the schema did not compile"};
    return validator->validate(value);
}

bool accepts(const json& schema, const json& value, Limits limits = {}) { return !check(schema, value, limits); }

json parse(const char* text) { return json::parse(text); }

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// `levels` arrays inside each other; the innermost is empty.
json nested_arrays(std::size_t levels) {
    json value = json::array();
    for (std::size_t level = 1; level < levels; ++level) value = json::array({value});
    return value;
}

// `levels` allOf wrappers around `leaf`: the leaf is evaluated at depth `levels`.
json all_of_chain(std::size_t levels, json leaf) {
    for (std::size_t level = 0; level < levels; ++level) leaf = json{{"allOf", json::array({leaf})}};
    return leaf;
}

// ---- keywords ----------------------------------------------------------------------------------------------------

TEST(JsonSchema, IntegerAcceptsAnIntegralNumber) {
    const json schema = {{"type", "integer"}};
    EXPECT_TRUE(accepts(schema, parse("1.0")));
    EXPECT_TRUE(accepts(schema, parse("-3")));
    EXPECT_TRUE(accepts(schema, parse("18446744073709551615")));
    EXPECT_FALSE(accepts(schema, parse("1.5")));
    EXPECT_FALSE(accepts(schema, json("1")));
}

TEST(JsonSchema, NumericBoundsAndMultipleOf) {
    const json schema = parse(R"({"type":"number","minimum":0,"exclusiveMaximum":10,"multipleOf":0.5})");
    EXPECT_TRUE(accepts(schema, parse("0")));
    EXPECT_TRUE(accepts(schema, parse("9.5")));
    EXPECT_FALSE(accepts(schema, parse("-1")));
    EXPECT_FALSE(accepts(schema, parse("10")));
    EXPECT_FALSE(accepts(schema, parse("2.25")));
    const auto issue = check(schema, parse("-1"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->schema_pointer, "/minimum");
    EXPECT_TRUE(contains(issue->message, "is less than the minimum 0"));
    EXPECT_FALSE(accepts(parse(R"({"multipleOf":3})"), parse("7")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":3})"), parse("9")));
}

TEST(JsonSchema, NumbersCompareExactlyWhateverTheirRepresentation) {
    // Signed, unsigned and floating point numbers of the same value are the same number.
    EXPECT_TRUE(accepts(parse(R"({"const":1})"), parse("1.0")));
    EXPECT_TRUE(accepts(parse(R"({"const":1.0})"), json(1)));
    EXPECT_TRUE(accepts(parse(R"({"const":0})"), parse("-0.0")));
    // A 64-bit integer does not lose its low bits against a double.
    EXPECT_FALSE(accepts(parse(R"({"maximum":9007199254740992})"), parse("9007199254740993")));
    EXPECT_TRUE(accepts(parse(R"({"maximum":9007199254740993})"), parse("9007199254740993")));
    EXPECT_FALSE(accepts(parse(R"({"const":9007199254740993})"), parse("9007199254740992.0")));
    EXPECT_TRUE(accepts(parse(R"({"maximum":18446744073709551615})"), parse("18446744073709551615")));
    EXPECT_FALSE(accepts(parse(R"({"maximum":18446744073709551614})"), parse("18446744073709551615")));
    EXPECT_FALSE(accepts(parse(R"({"minimum":18446744073709551615})"), json(-1)));
    EXPECT_TRUE(accepts(parse(R"({"exclusiveMaximum":18446744073709551616.0})"), parse("18446744073709551615")));
    EXPECT_FALSE(accepts(parse(R"({"minimum":-9223372036854775808})"), parse("-9.3e18")));
    EXPECT_TRUE(accepts(parse(R"({"minimum":-9223372036854775808})"), parse("-9223372036854775808")));
    // Unsigned 2^64-1 is not signed -1.
    EXPECT_FALSE(accepts(parse(R"({"const":-1})"), parse("18446744073709551615")));
    EXPECT_FALSE(accepts(parse(R"({"enum":[-1]})"), parse("18446744073709551615")));
}

TEST(JsonSchema, MultipleOfIsDecidedOnTheDecimalNumbers) {
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":0.0001})"), parse("0.0075")));
    EXPECT_FALSE(accepts(parse(R"({"multipleOf":0.0001})"), parse("0.00751")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":1.5})"), parse("4.5")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":1e-8})"), parse("12391239123")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":0.1})"), parse("0.3")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":2})"), parse("4.0")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":7})"), parse("0")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":7})"), parse("-14")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":3})"), parse("18446744073709551615")));
    EXPECT_FALSE(accepts(parse(R"({"multipleOf":2})"), parse("18446744073709551615")));
    // A quotient too large for a double is not a multiple.
    EXPECT_FALSE(accepts(parse(R"({"type":"integer","multipleOf":0.123456789})"), parse("1e308")));
    EXPECT_TRUE(accepts(parse(R"({"multipleOf":1e-300})"), parse("1e300")));
}

TEST(JsonSchema, StringsCountCodePointsAndMatchPatterns) {
    const json schema = parse(R"({"type":"string","minLength":2,"maxLength":3,"pattern":"^[a-z가-힣]+$"})");
    EXPECT_TRUE(accepts(schema, json("가나")));  // 2 code points, 6 bytes
    EXPECT_FALSE(accepts(schema, json("a")));
    EXPECT_FALSE(accepts(schema, json("abcd")));
    EXPECT_FALSE(accepts(schema, json("AB")));
    // A pattern is a search, not an anchored match.
    EXPECT_TRUE(accepts(parse(R"({"pattern":"b"})"), json("abc")));
    EXPECT_FALSE(accepts(parse(R"({"pattern":"b"})"), json("xyz")));
    const auto issue = check(parse(R"({"properties":{"s":{"pattern":"^a"}}})"), parse(R"({"s":"b"})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/s");
    EXPECT_EQ(issue->schema_pointer, "/properties/s/pattern");
}

TEST(JsonSchema, EqualityIsNumericAndOrderInsensitive) {
    EXPECT_TRUE(accepts(parse(R"({"const":1})"), parse("1.0")));
    EXPECT_TRUE(accepts(parse(R"({"enum":[{"a":1,"b":[2]}]})"), parse(R"({"b":[2.0],"a":1})")));
    EXPECT_FALSE(accepts(parse(R"({"const":"1"})"), parse("1")));
    EXPECT_FALSE(accepts(parse(R"({"const":false})"), parse("0")));
    EXPECT_FALSE(accepts(parse(R"({"uniqueItems":true})"), parse("[1, 1.0]")));
    EXPECT_TRUE(accepts(parse(R"({"uniqueItems":true})"), parse("[1, \"1\"]")));
    EXPECT_FALSE(accepts(parse(R"({"uniqueItems":true})"), parse(R"([{"a":1,"b":2},{"b":2,"a":1}])")));
    EXPECT_TRUE(accepts(parse(R"({"uniqueItems":true})"), parse(R"([[1,2],[2,1]])")));
    EXPECT_TRUE(accepts(parse(R"({"uniqueItems":false})"), parse("[1, 1]")));
}

TEST(JsonSchema, UniqueItemsOfALargeArrayIsNotQuadratic) {
    json items = json::array();
    for (int i = 0; i < 50000; ++i) items.push_back(i);
    EXPECT_TRUE(accepts(parse(R"({"uniqueItems":true})"), items));
    items.push_back(49999.0);
    const auto issue = check(parse(R"({"uniqueItems":true})"), items);
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "49999 and 50000"));
}

TEST(JsonSchema, DefsAndRefs) {
    // The shape generators of schemas write: definitions under $defs, $ref to them.
    const json schema = parse(R"({
        "$defs": {"Item": {"type": "object", "properties": {"name": {"type": "string"},
                                                             "qty": {"type": "integer", "minimum": 1}},
                           "required": ["name"], "additionalProperties": false}},
        "type": "object",
        "properties": {"items": {"type": "array", "items": {"$ref": "#/$defs/Item"}},
                       "note": {"anyOf": [{"type": "string"}, {"type": "null"}], "default": null}},
        "required": ["items"]})");
    EXPECT_TRUE(accepts(schema, parse(R"({"items":[{"name":"a","qty":2}],"note":null})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"items":[{"name":"a","qty":0}]})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"items":[{"qty":1}]})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"items":[{"name":"a","extra":1}]})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"items":[],"note":3})")));
    const auto issue = check(schema, parse(R"({"items":[{"qty":1}]})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/items/0");
    EXPECT_EQ(issue->schema_pointer, "/$defs/Item/required");
    EXPECT_TRUE(contains(issue->message, "is missing required property \"name\""));
}

TEST(JsonSchema, RecursiveRefsAnchorsAndPointerEscapes) {
    const json tree = parse(R"({
        "$defs": {"node": {"$anchor": "node", "type": "object",
                           "properties": {"children": {"type": "array", "items": {"$ref": "#node"}},
                                          "value": {"type": "integer"}}},
                  "a/b": {"type": "string"}},
        "$ref": "#/$defs/node",
        "properties": {"label": {"$ref": "#/$defs/a~1b"}}})");
    EXPECT_TRUE(accepts(tree, parse(R"({"value":1,"label":"x","children":[{"value":2,"children":[{"value":3}]}]})")));
    EXPECT_FALSE(accepts(tree, parse(R"({"children":[{"children":[{"value":"x"}]}]})")));
    EXPECT_FALSE(accepts(tree, parse(R"({"label":7})")));
    const auto issue = check(tree, parse(R"({"children":[{"children":[{"value":"x"}]}]})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/children/0/children/0/value");
    EXPECT_EQ(issue->schema_pointer, "/$defs/node/properties/value/type");
}

TEST(JsonSchema, ReferencesResolveByPercentEncodingAndEscapes) {
    const json schema = parse(R"({
        "$defs": {"percent%field": {"type": "integer"}, "tilde~field": {"type": "string"},
                  "slash/field": {"type": "boolean"}},
        "properties": {"p": {"$ref": "#/$defs/percent%25field"}, "t": {"$ref": "#/$defs/tilde~0field"},
                       "s": {"$ref": "#/$defs/slash~1field"}}})");
    EXPECT_TRUE(accepts(schema, parse(R"({"p":1,"t":"x","s":true})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"p":"x"})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"t":1})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"s":1})")));
}

TEST(JsonSchema, ReferencesPointIntoArraysAndUnknownKeywords) {
    const json schema = parse(R"({
        "x-store": [{"type": "string"}, {"type": "integer"}],
        "properties": {"a": {"$ref": "#/x-store/1"}, "b": {"$ref": "#/properties/a"}}})");
    EXPECT_TRUE(accepts(schema, parse(R"({"a":1,"b":2})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"a":"s"})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"b":"s"})")));
    const auto issue = check(schema, parse(R"({"a":"s"})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->schema_pointer, "/x-store/1/type");
}

TEST(JsonSchema, EmbeddedResourcesResolveByTheirId) {
    const json schema = parse(R"({
        "$id": "https://example.com/root.json",
        "$defs": {"inner": {"$id": "https://example.com/inner.json",
                            "$defs": {"leaf": {"type": "integer"}},
                            "$ref": "#/$defs/leaf"}},
        "properties": {"n": {"$ref": "https://example.com/inner.json"}}})");
    EXPECT_TRUE(accepts(schema, parse(R"({"n":3})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"n":"3"})")));
}

TEST(JsonSchema, RelativeIdsAndRefsResolveAgainstTheirBase) {
    const json schema = parse(R"({
        "$id": "http://example.com/schemas/root.json",
        "$defs": {
            "numbers": {"$id": "numbers/", "$defs": {"int": {"$id": "int.json", "type": "integer"},
                                                     "up": {"$id": "../up.json", "type": "string"}}}},
        "properties": {"i": {"$ref": "numbers/int.json"}, "u": {"$ref": "up.json"},
                       "abs": {"$ref": "/schemas/numbers/int.json"}}})");
    EXPECT_TRUE(accepts(schema, parse(R"({"i":1,"u":"x","abs":2})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"i":"x"})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"u":1})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"abs":"x"})")));
}

TEST(JsonSchema, ReferencesResolveLikeRfc3986) {
    // The examples of RFC 3986 section 5.4, with the base http://a/b/c/d;p?q: a schema that names itself by the
    // reference is found by the absolute URI the RFC resolves it to.
    const std::vector<std::pair<const char*, const char*>> cases = {
        {"g:h", "g:h"},
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {";x", "http://a/b/c/;x"},
        {"g;x", "http://a/b/c/g;x"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"},
        {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."},
        {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
    };
    for (const auto& [relative, absolute] : cases) {
        const json schema = json{{"$id", "http://a/b/c/d;p?q"},
                                 {"$defs", json{{"target", json{{"$id", relative}, {"type", "integer"}}}}},
                                 {"properties", json{{"v", json{{"$ref", absolute}}}}}};
        const auto validator = compiled(schema);
        ASSERT_TRUE(validator) << relative;
        EXPECT_FALSE(validator->validate(parse(R"({"v":1})"))) << relative;
        EXPECT_TRUE(validator->validate(parse(R"({"v":"s"})"))) << relative;
    }
}

TEST(JsonSchema, TextThatIsNotUtf8DoesNotBreakMessages) {
    const std::string bad = "bad\xff" "name";
    const Issue dialect = refusal(json{{"$schema", bad}});
    EXPECT_EQ(dialect.schema_pointer, "/$schema");
    const auto missing = check(json{{"required", json::array({bad})}}, json::object());
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->schema_pointer, "/required");
    EXPECT_TRUE(contains(missing->message, "is missing required property"));
    const Issue pattern = refusal(json{{"pattern", "(" + bad}});
    EXPECT_EQ(pattern.schema_pointer, "/pattern");
}

TEST(JsonSchema, SignedAndUnsignedIntegersBuiltInCodeAreTheSameNumber) {
    const json schema = parse(R"({"const":5,"type":"integer","minimum":5,"maximum":5,"multipleOf":5})");
    EXPECT_TRUE(accepts(schema, json(5)));
    EXPECT_TRUE(accepts(schema, json(5U)));
    EXPECT_TRUE(accepts(schema, json(5LL)));
    EXPECT_TRUE(accepts(schema, json(5ULL)));
    EXPECT_TRUE(accepts(schema, json(5.0)));
    EXPECT_FALSE(accepts(schema, json(6U)));
    EXPECT_TRUE(accepts(json{{"const", 5U}}, json(5)));
}

TEST(JsonSchema, UrnIdsAndAnchorsPerResource) {
    const json schema = parse(R"({
        "$id": "urn:example:root",
        "$defs": {"a": {"$id": "urn:example:a", "$anchor": "x", "type": "integer"},
                  "b": {"$id": "urn:example:b", "$anchor": "x", "type": "string"}},
        "properties": {"a": {"$ref": "urn:example:a#x"}, "b": {"$ref": "urn:example:b#x"},
                       "c": {"$ref": "urn:example:root#/$defs/a"}}})");
    EXPECT_TRUE(accepts(schema, parse(R"({"a":1,"b":"s","c":2})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"a":"s"})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"b":1})")));
}

TEST(JsonSchema, IdsInsideUnknownKeywordsAndEnumsDoNotCount) {
    // The same identifier appears in an unknown keyword, in an enum and once as a real $id: only the real one counts,
    // so there is no duplicate and the reference reaches the real schema.
    const json schema = parse(R"({
        "x-note": {"$id": "https://example.com/hidden.json", "$anchor": "hidden", "type": "null"},
        "enum": [{"$id": "https://example.com/hidden.json"}, 1],
        "$defs": {"real": {"$id": "https://example.com/hidden.json", "type": "string"}},
        "properties": {"v": {"$ref": "https://example.com/hidden.json"}}})");
    EXPECT_TRUE(accepts(schema, parse(R"(1)")));
    EXPECT_TRUE(accepts(json{{"properties", json{{"v", json{{"$ref", "#/$defs/x"}}}}},
                             {"$defs", json{{"x", json{{"type", "string"}}}}}},
                        parse(R"({"v":"s"})")));
    const json plain = parse(R"({
        "x-note": {"$id": "https://example.com/hidden.json", "type": "null"},
        "$defs": {"real": {"$id": "https://example.com/hidden.json", "type": "string"}},
        "properties": {"v": {"$ref": "https://example.com/hidden.json"}}})");
    EXPECT_TRUE(accepts(plain, parse(R"({"v":"s"})")));
    EXPECT_FALSE(accepts(plain, parse(R"({"v":null})")));
}

TEST(JsonSchema, ADynamicAnchorNamesASchemaForAPlainRef) {
    const json schema = parse(R"({"$defs": {"s": {"$dynamicAnchor": "name", "type": "string"}},
                                  "properties": {"v": {"$ref": "#name"}}})");
    EXPECT_TRUE(accepts(schema, parse(R"({"v":"s"})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"v":1})")));
}

TEST(JsonSchema, RefsApplyAlongsideSiblingKeywords) {
    const json schema = parse(R"({"$defs": {"big": {"minimum": 10}}, "$ref": "#/$defs/big", "maximum": 20})");
    EXPECT_TRUE(accepts(schema, parse("15")));
    EXPECT_FALSE(accepts(schema, parse("5")));
    EXPECT_FALSE(accepts(schema, parse("25")));
}

TEST(JsonSchema, CompositionKeywords) {
    const json schema = parse(R"({
        "oneOf": [{"type": "integer", "multipleOf": 2}, {"type": "integer", "multipleOf": 3}],
        "not": {"const": 12}})");
    EXPECT_TRUE(accepts(schema, parse("4")));
    EXPECT_TRUE(accepts(schema, parse("9")));
    EXPECT_FALSE(accepts(schema, parse("6")));   // both branches
    EXPECT_FALSE(accepts(schema, parse("7")));   // neither branch
    EXPECT_FALSE(accepts(schema, parse("12")));  // not

    const json conditional = parse(R"({
        "type": "object",
        "if": {"properties": {"kind": {"const": "file"}}, "required": ["kind"]},
        "then": {"required": ["path"]},
        "else": {"required": ["url"]}})");
    EXPECT_TRUE(accepts(conditional, parse(R"({"kind":"file","path":"/x"})")));
    EXPECT_FALSE(accepts(conditional, parse(R"({"kind":"file","url":"u"})")));
    EXPECT_TRUE(accepts(conditional, parse(R"({"url":"u"})")));
}

TEST(JsonSchema, ObjectKeywords) {
    const json schema = parse(R"({
        "type": "object",
        "properties": {"a": {"type": "integer"}},
        "patternProperties": {"^x-": {"type": "string"}},
        "additionalProperties": false,
        "propertyNames": {"maxLength": 5},
        "dependentRequired": {"a": ["x-b"]},
        "dependentSchemas": {"x-c": {"required": ["a"]}},
        "minProperties": 1, "maxProperties": 3})");
    EXPECT_TRUE(accepts(schema, parse(R"({"a":1,"x-b":"s"})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"a":1})")));            // dependentRequired
    EXPECT_FALSE(accepts(schema, parse(R"({"x-c":"s"})")));        // dependentSchemas
    EXPECT_FALSE(accepts(schema, parse(R"({"other":1})")));        // additionalProperties
    EXPECT_FALSE(accepts(schema, parse(R"({"x-long1":"s"})")));    // propertyNames
    EXPECT_FALSE(accepts(schema, parse(R"({})")));                 // minProperties
    EXPECT_FALSE(accepts(schema, parse(R"({"x-b":1})")));          // patternProperties
    EXPECT_FALSE(accepts(schema, parse(R"({"a":1,"x-1":"s","x-2":"s","x-3":"s"})")));  // maxProperties
    const auto issue = check(schema, parse(R"({"other":1})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/other");
    EXPECT_EQ(issue->schema_pointer, "/additionalProperties");
    EXPECT_TRUE(contains(issue->message, "additional property"));
}

TEST(JsonSchema, UnevaluatedPropertiesSeeThroughApplicators) {
    const json schema = parse(R"({
        "allOf": [{"properties": {"a": {"type": "string"}}}],
        "anyOf": [{"properties": {"b": {"type": "integer"}}, "required": ["b"]},
                  {"properties": {"c": {"type": "boolean"}}, "required": ["c"]}],
        "unevaluatedProperties": false})");
    EXPECT_TRUE(accepts(schema, parse(R"({"a":"x","b":1})")));
    EXPECT_TRUE(accepts(schema, parse(R"({"c":true})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"b":1,"d":0})")));
    // A failed branch evaluates nothing: "c" is not evaluated when its branch fails.
    EXPECT_FALSE(accepts(schema, parse(R"({"b":1,"c":"no"})")));
    const auto issue = check(schema, parse(R"({"b":1,"d":0})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/d");
    EXPECT_EQ(issue->schema_pointer, "/unevaluatedProperties");
}

TEST(JsonSchema, UnevaluatedSeesThroughReferencesButNotIntoNot) {
    const json schema = parse(R"({
        "$defs": {"base": {"properties": {"a": true}}},
        "$ref": "#/$defs/base",
        "not": {"not": {"properties": {"b": true}}},
        "unevaluatedProperties": false})");
    EXPECT_TRUE(accepts(schema, parse(R"({"a":1})")));
    EXPECT_FALSE(accepts(schema, parse(R"({"a":1,"b":1})")));
}

TEST(JsonSchema, ArrayKeywords) {
    const json tuple = parse(R"({"prefixItems":[{"type":"string"},{"type":"integer"}],"items":false})");
    EXPECT_TRUE(accepts(tuple, parse(R"(["a",1])")));
    EXPECT_FALSE(accepts(tuple, parse(R"(["a",1,2])")));
    EXPECT_FALSE(accepts(tuple, parse(R"([1,"a"])")));

    const json contains_schema =
        parse(R"({"contains":{"type":"integer"},"minContains":2,"maxContains":3,"minItems":2})");
    EXPECT_TRUE(accepts(contains_schema, parse(R"([1,"a",2])")));
    EXPECT_FALSE(accepts(contains_schema, parse(R"([1,"a"])")));
    EXPECT_FALSE(accepts(contains_schema, parse(R"([1,2,3,4])")));
    EXPECT_TRUE(accepts(parse(R"({"contains":{"type":"integer"},"minContains":0})"), parse(R"(["a"])")));
    EXPECT_FALSE(accepts(parse(R"({"contains":{"type":"integer"}})"), parse(R"(["a"])")));

    const json unevaluated = parse(R"({"prefixItems":[{"type":"string"}],"contains":{"type":"integer"},
                                      "unevaluatedItems":false})");
    EXPECT_TRUE(accepts(unevaluated, parse(R"(["a",1,2])")));
    EXPECT_FALSE(accepts(unevaluated, parse(R"(["a",1,true])")));

    const auto issue = check(parse(R"({"items":{"type":"string"}})"), parse(R"(["a",1])"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/1");
    EXPECT_EQ(issue->schema_pointer, "/items/type");
}

TEST(JsonSchema, BooleanSchemas) {
    EXPECT_TRUE(accepts(json(true), parse(R"({"anything":[1]})")));
    EXPECT_FALSE(accepts(json(false), parse("null")));
    const json schema = parse(R"({"type":"object","properties":{"forbidden":false}})");
    EXPECT_TRUE(accepts(schema, parse(R"({"ok":1})")));
    const auto issue = check(schema, parse(R"({"forbidden":1})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/forbidden");
    EXPECT_EQ(issue->schema_pointer, "/properties/forbidden");
}

TEST(JsonSchema, FormatAndUnknownKeywordsAreAnnotations) {
    EXPECT_TRUE(accepts(parse(R"({"format":"email"})"), json("not an email")));
    EXPECT_TRUE(accepts(parse(R"({"x-vendor":1,"format":"uri","contentEncoding":"base64","title":"t"})"), json(3)));
    EXPECT_TRUE(accepts(parse(R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object"})"),
                        parse("{}")));
    EXPECT_TRUE(accepts(parse(R"({"$schema":"https://json-schema.org/draft/2020-12/schema#"})"), parse("{}")));
}

// ---- Issue pointers ----------------------------------------------------------------------------------------------

TEST(JsonSchema, IssuePointersOfANestedFailureNameBothPlaces) {
    const json schema = parse(R"({
        "properties": {"users": {"type": "array", "items": {"properties": {"tags": {"items": {"maxLength": 3}}}}}}})");
    const auto issue = check(schema, parse(R"({"users":[{"tags":["ok"]},{"tags":["ok","toolong"]}]})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/users/1/tags/1");
    EXPECT_EQ(issue->schema_pointer, "/properties/users/items/properties/tags/items/maxLength");
}

TEST(JsonSchema, IssuePointersEscapeTildeAndSlash) {
    const json schema = parse(R"({"properties": {"a/b": {"properties": {"m~n": {"type": "string"}}}}})");
    const auto issue = check(schema, parse(R"({"a/b":{"m~n":1}})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "/a~1b/m~0n");
    EXPECT_EQ(issue->schema_pointer, "/properties/a~1b/properties/m~0n/type");
}

TEST(JsonSchema, IssuePointersOfTheRootAreEmpty) {
    const auto issue = check(parse(R"({"type":"string"})"), json(1));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "");
    EXPECT_EQ(issue->schema_pointer, "/type");
}

TEST(JsonSchema, IssuePointerOfAnyOfAndOneOfAndNot) {
    const auto any = check(parse(R"({"properties":{"v":{"anyOf":[{"type":"string"},{"type":"null"}]}}})"),
                           parse(R"({"v":1})"));
    ASSERT_TRUE(any);
    EXPECT_EQ(any->instance_pointer, "/v");
    EXPECT_EQ(any->schema_pointer, "/properties/v/anyOf");
    const auto one = check(parse(R"({"oneOf":[true,true]})"), json(1));
    ASSERT_TRUE(one);
    EXPECT_EQ(one->schema_pointer, "/oneOf");
    const auto negated = check(parse(R"({"not":true})"), json(1));
    ASSERT_TRUE(negated);
    EXPECT_EQ(negated->schema_pointer, "/not");
}

TEST(JsonSchema, IssuePointersOfPropertyNames) {
    const auto issue = check(parse(R"({"propertyNames":{"maxLength":2}})"), parse(R"({"abc":1})"));
    ASSERT_TRUE(issue);
    EXPECT_EQ(issue->instance_pointer, "");
    EXPECT_EQ(issue->schema_pointer, "/propertyNames");
    EXPECT_TRUE(contains(issue->message, "\"abc\""));
}

// ---- what compile refuses ----------------------------------------------------------------------------------------

TEST(JsonSchema, AnotherDialectIsRefusedAtTheKeyword) {
    const Issue issue = refusal(parse(R"({"properties":{"a":{"$schema":"http://json-schema.org/draft-07/schema#"}}})"));
    EXPECT_EQ(issue.schema_pointer, "/properties/a/$schema");
    EXPECT_EQ(issue.instance_pointer, "");
    EXPECT_EQ(refusal(parse(R"({"$schema":7})")).schema_pointer, "/$schema");
}

TEST(JsonSchema, DynamicAndRecursiveReferencesAreRefused) {
    EXPECT_EQ(refusal(parse(R"({"$dynamicRef":"#meta"})")).schema_pointer, "/$dynamicRef");
    EXPECT_EQ(refusal(parse(R"({"items":{"$recursiveRef":"#"}})")).schema_pointer, "/items/$recursiveRef");
}

TEST(JsonSchema, ReferencesToOtherDocumentsAreRefused) {
    EXPECT_EQ(refusal(parse(R"({"$ref":"https://example.com/other.json"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"properties":{"a":{"$ref":"other.json#/$defs/x"}}})")).schema_pointer,
              "/properties/a/$ref");
    EXPECT_EQ(refusal(parse(R"({"$ref":"file:///etc/passwd"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"$id":"https://example.com/a.json","$ref":"https://example.com/b.json"})")).schema_pointer,
              "/$ref");
}

TEST(JsonSchema, ReferencesThatDoNotResolveAreRefused) {
    EXPECT_EQ(refusal(parse(R"({"$ref":"#/$defs/missing"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"$ref":"#nowhere"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"$defs":{"a":{"type":"string"}},"$ref":"#/$defs/a/x"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"x-store":[1,2],"$ref":"#/x-store/01"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"x-store":[1,2],"$ref":"#/x-store/99999999999999999999999"})")).schema_pointer,
              "/$ref");
    EXPECT_EQ(refusal(parse(R"({"x-store":[1,2],"$ref":"#/x-store/2"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"$ref":"#/$defs/bad~2escape"})")).schema_pointer, "/$ref");
    EXPECT_EQ(refusal(parse(R"({"$ref":7})")).schema_pointer, "/$ref");
    // A target that is not a schema is refused where it sits.
    const Issue not_a_schema = refusal(parse(R"({"x-store":[3],"$ref":"#/x-store/0"})"));
    EXPECT_EQ(not_a_schema.schema_pointer, "/x-store/0");
}

TEST(JsonSchema, IdsAreChecked) {
    EXPECT_EQ(refusal(parse(R"({"$id":3})")).schema_pointer, "/$id");
    EXPECT_EQ(refusal(parse(R"({"properties":{"a":{"$id":"https://example.com/x#frag"}}})")).schema_pointer,
              "/properties/a/$id");
    const Issue duplicate = refusal(parse(R"({"$defs":{"a":{"$id":"https://example.com/x"},"b":{"$id":"https://example.com/x"}}})"));
    EXPECT_EQ(duplicate.schema_pointer, "/$defs/b/$id");
    const Issue anchors = refusal(parse(R"({"$defs":{"a":{"$anchor":"x"},"b":{"$anchor":"x"}}})"));
    EXPECT_EQ(anchors.schema_pointer, "/$defs/b/$anchor");
    EXPECT_EQ(refusal(parse(R"({"$anchor":3})")).schema_pointer, "/$anchor");
}

TEST(JsonSchema, PatternsThatCannotCompileAreRefusedAtTheKeyword) {
    EXPECT_EQ(refusal(parse(R"({"pattern":"(unclosed"})")).schema_pointer, "/pattern");
    EXPECT_EQ(refusal(parse(R"j({"properties":{"a":{"pattern":"a)"}}})j")).schema_pointer, "/properties/a/pattern");
    EXPECT_EQ(refusal(parse(R"({"patternProperties":{"(x":true}})")).schema_pointer, "/patternProperties/(x");
    EXPECT_EQ(refusal(parse(R"({"patternProperties":{"a/(b":true}})")).schema_pointer, "/patternProperties/a~1(b");
    EXPECT_EQ(refusal(parse(R"({"pattern":3})")).schema_pointer, "/pattern");
}

TEST(JsonSchema, RegexConstructsOfNewerEcmaScriptAreRefusedOnEveryPlatform) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {R"(\p{L})", "Unicode property escape"},
        {R"(^\P{Lu}+$)", "Unicode property escape"},
        {R"([\p{L}0-9])", "Unicode property escape"},
        {R"(\u{1F600})", "code point escape"},
        {R"(a(?<=b)c)", "lookbehind"},
        {R"(a(?<!b)c)", "lookbehind"},
        {R"((?<year>[0-9]+))", "named group"},
        {R"((?<n>a)\k<n>)", "named group"},
        {R"(\k<n>)", "named back-reference"},
    };
    for (const auto& [pattern, construct] : cases) {
        const Issue in_pattern = refusal(json{{"properties", json{{"a", json{{"pattern", pattern}}}}}});
        EXPECT_EQ(in_pattern.schema_pointer, "/properties/a/pattern") << pattern;
        EXPECT_TRUE(contains(in_pattern.message, construct)) << pattern << ": " << in_pattern.message;
        EXPECT_TRUE(contains(in_pattern.message, "does not have")) << pattern;
        const Issue in_key = refusal(json{{"patternProperties", json{{pattern, true}}}});
        EXPECT_EQ(in_key.schema_pointer, "/patternProperties/" + pattern) << pattern;
        EXPECT_TRUE(contains(in_key.message, construct)) << pattern;
    }
}

TEST(JsonSchema, TheSameCharactersInAHarmlessPlaceAreNotRefused) {
    const std::vector<std::string> patterns = {
        R"(\\p)",       // an escaped backslash, then the letter p
        R"(\\u{2})",    // an escaped backslash, then "u" repeated twice
        R"([(?<x])",    // inside a character class
        R"([\\]p)",     // the class ends at the first unescaped bracket
        R"(\(?<a)",     // an optional escaped parenthesis
        R"((?=a)b)",    // lookahead exists in the grammar
        R"((?!a)b)",
        R"((?:a)b)",
    };
    for (const std::string& pattern : patterns) {
        EXPECT_TRUE(compiled(json{{"pattern", pattern}})) << pattern;
        EXPECT_TRUE(compiled(json{{"patternProperties", json{{pattern, true}}}})) << pattern;
    }
}

TEST(JsonSchema, ARangeThatEnginesReadDifferentlyIsRefusedOrHarmless) {
    // ECMAScript rejects the reversed range [z-a]; libstdc++ does too, libc++ accepts it. What holds on every
    // platform: the schema is refused at the keyword, or it compiles and validation answers without a crash.
    const json schema = parse(R"({"properties":{"a":{"pattern":"[z-a]"}}})");
    auto result = Validator::compile(schema);
    if (const auto* issue = std::get_if<Issue>(&result)) {
        EXPECT_EQ(issue->schema_pointer, "/properties/a/pattern");
        return;
    }
    const Validator validator = std::get<Validator>(result);
    for (const char* text : {"", "a", "m", "z", "-"}) {
        (void)validator.validate(json{{"a", text}});
    }
}

TEST(JsonSchema, KeywordsOfTheWrongShapeAreRefused) {
    const std::vector<std::pair<const char*, const char*>> cases = {
        {R"({"items":[{"type":"string"}]})", "/items"},
        {R"({"type":"imaginary"})", "/type"},
        {R"({"type":[]})", "/type"},
        {R"({"type":["string",3]})", "/type"},
        {R"({"minimum":"0"})", "/minimum"},
        {R"({"exclusiveMaximum":null})", "/exclusiveMaximum"},
        {R"({"multipleOf":0})", "/multipleOf"},
        {R"({"multipleOf":-1})", "/multipleOf"},
        {R"({"multipleOf":"2"})", "/multipleOf"},
        {R"({"minLength":-1})", "/minLength"},
        {R"({"maxItems":1.5})", "/maxItems"},
        {R"({"minProperties":"1"})", "/minProperties"},
        {R"({"minContains":-2})", "/minContains"},
        {R"({"enum":3})", "/enum"},
        {R"({"required":"a"})", "/required"},
        {R"({"required":["a",1]})", "/required"},
        {R"({"uniqueItems":1})", "/uniqueItems"},
        {R"({"dependentRequired":[]})", "/dependentRequired"},
        {R"({"dependentRequired":{"a":"b"}})", "/dependentRequired/a"},
        {R"({"dependentRequired":{"a":[1]}})", "/dependentRequired/a"},
        {R"({"anyOf":[]})", "/anyOf"},
        {R"({"allOf":{}})", "/allOf"},
        {R"({"properties":[]})", "/properties"},
        {R"({"$defs":3})", "/$defs"},
        {R"({"properties":{"a":3}})", "/properties/a"},
        {R"({"allOf":[true,"x"]})", "/allOf/1"},
        {R"({"not":null})", "/not"},
        {"3", ""},
        {R"("string")", ""},
        {"null", ""},
        {"[]", ""},
    };
    for (const auto& [text, pointer] : cases) {
        EXPECT_EQ(refusal(parse(text)).schema_pointer, pointer) << text;
    }
}

TEST(JsonSchema, ReferencesThatNeverMakeProgressAreRefused) {
    EXPECT_EQ(refusal(parse(R"({"$ref":"#"})")).schema_pointer, "/$ref");
    const Issue loop = refusal(parse(R"({"$defs":{"a":{"$ref":"#/$defs/b"},"b":{"$ref":"#/$defs/a"}},"$ref":"#/$defs/a"})"));
    EXPECT_TRUE(loop.schema_pointer == "/$defs/a/$ref" || loop.schema_pointer == "/$defs/b/$ref") << loop.schema_pointer;
    EXPECT_TRUE(contains(loop.message, "never makes progress"));
    // The same value is checked again through an applicator.
    EXPECT_FALSE(refusal(parse(R"({"anyOf":[{"$ref":"#"},{"$ref":"#"}]})")).schema_pointer.empty());
    EXPECT_FALSE(refusal(parse(R"({"allOf":[{"not":{"$ref":"#"}}]})")).schema_pointer.empty());
    EXPECT_FALSE(refusal(parse(R"({"if":true,"then":{"$ref":"#"}})")).schema_pointer.empty());
    EXPECT_FALSE(refusal(parse(R"({"dependentSchemas":{"a":{"$ref":"#"}}})")).schema_pointer.empty());
}

TEST(JsonSchema, RecursionThatStepsIntoTheValueIsAccepted) {
    const json tree = parse(R"({"type":"object","properties":{"children":{"type":"array","items":{"$ref":"#"}}}})");
    EXPECT_TRUE(accepts(tree, parse(R"({"children":[{"children":[{}]}]})")));
    EXPECT_FALSE(accepts(tree, parse(R"({"children":[{"children":3}]})")));
    const json list = parse(R"({"anyOf":[{"type":"null"},{"properties":{"next":{"$ref":"#"}}}]})");
    EXPECT_TRUE(accepts(list, parse(R"({"next":{"next":null}})")));
    // A reference to a schema nested in a boolean-valued position stays acyclic.
    EXPECT_TRUE(accepts(parse(R"({"$defs":{"t":true},"$ref":"#/$defs/t"})"), json(1)));
}

// ---- Limits ------------------------------------------------------------------------------------------------------

TEST(JsonSchemaLimits, MaxSchemaDepthBoundsTheDocument) {
    Limits limits;
    limits.max_schema_depth = 4;
    json inside = parse(R"({"type":"string"})");
    for (int i = 0; i < 4; ++i) inside = json{{"not", inside}};
    EXPECT_TRUE(compiled(inside, limits));
    json outside = json{{"not", inside}};
    const Issue issue = refusal(outside, limits);
    EXPECT_EQ(issue.schema_pointer, "/not/not/not/not/not");
    EXPECT_TRUE(contains(issue.message, "nested deeper than 4"));
    // The default allows 64 levels and not 65; a schema of 80 levels is refused.
    json eighty = parse(R"({"type":"object"})");
    for (int i = 0; i < 80; ++i) eighty = json{{"properties", json{{"x", eighty}}}};
    EXPECT_FALSE(refusal(eighty).schema_pointer.empty());
    json deep = parse(R"({"type":"string"})");
    for (int i = 0; i < 64; ++i) deep = json{{"not", deep}};
    EXPECT_TRUE(compiled(deep));
    EXPECT_FALSE(refusal(json{{"not", deep}}).schema_pointer.empty());
}

TEST(JsonSchemaLimits, MaxDepthBoundsEvaluation) {
    const json leaf = parse(R"({"type":"string"})");
    Limits limits;
    limits.max_depth = 3;
    // The leaf of three wrappers is evaluated at depth 3: inside the bound.
    EXPECT_FALSE(accepts(all_of_chain(3, leaf), json(1), limits));
    EXPECT_TRUE(accepts(all_of_chain(3, leaf), json("x"), limits));
    // Four wrappers put it at depth 4: outside.
    const auto issue = check(all_of_chain(4, leaf), json("x"), limits);
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "max_depth"));
    // The default is 128.
    Limits deep_schemas;
    deep_schemas.max_schema_depth = 1000;
    EXPECT_TRUE(accepts(all_of_chain(128, parse("true")), json(1), deep_schemas));
    const auto beyond = check(all_of_chain(129, parse("true")), json(1), deep_schemas);
    ASSERT_TRUE(beyond);
    EXPECT_TRUE(contains(beyond->message, "max_depth"));
}

TEST(JsonSchemaLimits, ALimitIsNotTurnedIntoAPassByNotAnyOfOrIf) {
    Limits limits;
    limits.max_depth = 2;
    const json deep = parse(R"({"allOf":[{"allOf":[{"type":"string"}]}]})");
    // Within the bound `not` sees the inner schema fail and passes.
    EXPECT_TRUE(accepts(json{{"not", deep}}, json(1), Limits{}));
    // Outside it the validation ends with the limit's Issue.
    for (const char* wrapper : {"not", "if"}) {
        const auto issue = check(json{{wrapper, deep}}, json(1), limits);
        ASSERT_TRUE(issue) << wrapper;
        EXPECT_TRUE(contains(issue->message, "max_depth")) << wrapper;
    }
    const auto any = check(json{{"anyOf", json::array({deep, true})}}, json(1), limits);
    ASSERT_TRUE(any);
    EXPECT_TRUE(contains(any->message, "max_depth"));
    const auto one = check(json{{"oneOf", json::array({deep, true})}}, json(1), limits);
    ASSERT_TRUE(one);
    EXPECT_TRUE(contains(one->message, "max_depth"));
    const auto contains_issue = check(json{{"contains", deep}}, parse("[1]"), limits);
    ASSERT_TRUE(contains_issue);
    EXPECT_TRUE(contains(contains_issue->message, "max_depth"));
}

TEST(JsonSchemaLimits, ARecursiveSchemaStopsAtMaxDepth) {
    const json tree = parse(R"({"type":"object","properties":{"c":{"$ref":"#"}}})");
    json value = json::object();
    for (int i = 0; i < 20; ++i) value = json{{"c", value}};
    Limits limits;
    limits.max_depth = 40;  // each level of the value costs a property and a reference: 2 levels
    EXPECT_TRUE(accepts(tree, value, limits));
    limits.max_depth = 39;
    const auto issue = check(tree, value, limits);
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "max_depth"));
}

TEST(JsonSchemaLimits, MaxValueDepthBoundsEqualityAndHashing) {
    Limits limits;
    limits.max_value_depth = 5;
    for (const char* keyword : {"const", "enum"}) {
        const auto make = [&](const json& operand) {
            return std::string(keyword) == "const" ? json{{"const", operand}}
                                                  : json{{"enum", json::array({operand})}};
        };
        EXPECT_TRUE(accepts(make(nested_arrays(5)), nested_arrays(5), limits)) << keyword;
        const auto issue = check(make(nested_arrays(6)), nested_arrays(6), limits);
        ASSERT_TRUE(issue) << keyword;
        EXPECT_TRUE(contains(issue->message, "max_value_depth")) << keyword;
        EXPECT_EQ(issue->schema_pointer, std::string("/") + keyword);
        // A value that differs before the limit is reached is still answered.
        EXPECT_FALSE(accepts(make(json::array({1})), nested_arrays(6), limits)) << keyword;
    }
    const json unique = parse(R"({"uniqueItems":true})");
    EXPECT_TRUE(accepts(unique, json::array({nested_arrays(4), nested_arrays(3)}), limits));
    const auto issue = check(unique, json::array({nested_arrays(5), nested_arrays(5)}), limits);
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "duplicate"));
    const auto deep = check(unique, json::array({nested_arrays(6), nested_arrays(6)}), limits);
    ASSERT_TRUE(deep);
    EXPECT_TRUE(contains(deep->message, "max_value_depth"));
    // The default is 256 levels.
    EXPECT_TRUE(accepts(json{{"const", nested_arrays(256)}}, nested_arrays(256)));
    EXPECT_TRUE(check(json{{"const", nested_arrays(257)}}, nested_arrays(257)));
}

TEST(JsonSchemaLimits, MaxPatternInputBoundsStringsAndPropertyNames) {
    Limits limits;
    limits.max_pattern_input = 8;
    const json schema = parse(R"({"pattern":"a"})");
    EXPECT_TRUE(accepts(schema, json("aaaaaaaa"), limits));
    const auto issue = check(schema, json("aaaaaaaaa"), limits);
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "max_pattern_input"));
    EXPECT_EQ(issue->schema_pointer, "/pattern");

    const json names = parse(R"({"patternProperties":{"^a":true}})");
    EXPECT_TRUE(accepts(names, json{{"aaaaaaaa", 1}}, limits));
    const auto name_issue = check(names, json{{"aaaaaaaaa", 1}}, limits);
    ASSERT_TRUE(name_issue);
    EXPECT_TRUE(contains(name_issue->message, "max_pattern_input"));
    EXPECT_EQ(name_issue->schema_pointer, "/patternProperties");

    // The limit is not a failed match: no wrapper turns it into a pass.
    for (const char* wrapper : {"not", "if"}) {
        const auto wrapped = check(json{{wrapper, schema}}, json("aaaaaaaaa"), limits);
        ASSERT_TRUE(wrapped) << wrapper;
        EXPECT_TRUE(contains(wrapped->message, "max_pattern_input")) << wrapper;
    }
    // A string of any other type is not matched against anything.
    EXPECT_TRUE(accepts(schema, json(123456789012LL), limits));
    // The default is 256 bytes.
    EXPECT_TRUE(accepts(schema, json(std::string(256, 'a'))));
    EXPECT_TRUE(check(schema, json(std::string(257, 'a'))));
}

TEST(JsonSchemaLimits, MaxPatternLengthBoundsThePatternSource) {
    Limits limits;
    limits.max_pattern_length = 8;
    const std::string inside = "^a{1,5}$";  // 8 bytes
    const std::string outside = "^a{1,56}$";  // 9 bytes
    EXPECT_TRUE(compiled(json{{"pattern", inside}}, limits));
    const Issue pattern = refusal(json{{"pattern", outside}}, limits);
    EXPECT_EQ(pattern.schema_pointer, "/pattern");
    EXPECT_TRUE(contains(pattern.message, "max_pattern_length"));
    EXPECT_EQ(refusal(json{{"properties", json{{"p", json{{"pattern", outside}}}}}}, limits).schema_pointer,
              "/properties/p/pattern");

    EXPECT_TRUE(compiled(json{{"patternProperties", json{{inside, true}}}}, limits));
    const Issue key = refusal(json{{"patternProperties", json{{outside, true}}}}, limits);
    EXPECT_EQ(key.schema_pointer, "/patternProperties/" + std::string("^a{1,56}$"));
    EXPECT_TRUE(contains(key.message, "max_pattern_length"));

    // The default is 512 bytes.
    EXPECT_TRUE(compiled(json{{"pattern", std::string(512, 'a')}}));
    EXPECT_FALSE(refusal(json{{"pattern", std::string(513, 'a')}}).schema_pointer.empty());
    EXPECT_EQ(Limits{}.max_pattern_length, 512U);
}

TEST(JsonSchemaLimits, MaxEvaluationsBoundsTheWorkOfOneValidation) {
    // Each level doubles the work: 2 members that both refer to the next level.
    const json schema = parse(R"({
        "$defs": {"l0": {"type": "string"},
                  "l1": {"allOf": [{"$ref": "#/$defs/l0"}, {"$ref": "#/$defs/l0"}]},
                  "l2": {"allOf": [{"$ref": "#/$defs/l1"}, {"$ref": "#/$defs/l1"}]},
                  "l3": {"allOf": [{"$ref": "#/$defs/l2"}, {"$ref": "#/$defs/l2"}]}},
        "$ref": "#/$defs/l3"})");
    std::size_t smallest = 0;
    for (std::size_t bound = 1; bound < 1000 && smallest == 0; ++bound) {
        Limits limits;
        limits.max_evaluations = bound;
        if (!check(schema, json("x"), limits)) smallest = bound;
    }
    ASSERT_GT(smallest, 8U);
    Limits limits;
    limits.max_evaluations = smallest - 1;
    const auto issue = check(schema, json("x"), limits);
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "max_evaluations"));
    limits.max_evaluations = smallest;
    EXPECT_FALSE(check(schema, json("x"), limits));
}

TEST(JsonSchemaLimits, AnExponentialSchemaEndsAtTheDefaultBound) {
    // Thirty levels of two members that both refer to the next level would take 2^30 evaluations.
    json defs = json::object();
    defs["l0"] = json{{"type", "string"}};
    for (int level = 1; level <= 30; ++level) {
        const json ref = json{{"$ref", "#/$defs/l" + std::to_string(level - 1)}};
        defs["l" + std::to_string(level)] = json{{"allOf", json::array({ref, ref})}};
    }
    const json schema = json{{"$defs", defs}, {"$ref", "#/$defs/l30"}};
    Limits limits;
    limits.max_evaluations = 100000;
    const auto issue = check(schema, json("x"), limits);
    ASSERT_TRUE(issue);
    EXPECT_TRUE(contains(issue->message, "max_evaluations"));
}

TEST(JsonSchemaLimits, DefaultsAreDocumented) {
    const Limits limits;
    EXPECT_EQ(limits.max_depth, 128U);
    EXPECT_EQ(limits.max_schema_depth, 64U);
    EXPECT_EQ(limits.max_value_depth, 256U);
    EXPECT_EQ(limits.max_pattern_input, 256U);
    EXPECT_EQ(limits.max_evaluations, 10000000U);
}

// ---- robustness --------------------------------------------------------------------------------------------------

TEST(JsonSchema, AValidatorIsCopyableAndOutlivesTheSchemaItWasMadeFrom) {
    std::optional<Validator> copy;
    {
        json schema = parse(R"({"$defs":{"n":{"type":"integer"}},"properties":{"x":{"$ref":"#/$defs/n"}}})");
        const auto original = compiled(schema);
        ASSERT_TRUE(original);
        copy = *original;
        schema = json();
    }
    EXPECT_FALSE(copy->validate(parse(R"({"x":1})")));
    EXPECT_TRUE(copy->validate(parse(R"({"x":"a"})")));
}

TEST(JsonSchema, ValidationFromSeveralThreadsAgrees) {
    const auto validator = compiled(parse(R"({
        "$defs": {"node": {"type": "object", "properties": {"v": {"type": "integer", "minimum": 0},
                                                           "kids": {"type": "array", "items": {"$ref": "#/$defs/node"}}}}},
        "$ref": "#/$defs/node", "unevaluatedProperties": false})"));
    ASSERT_TRUE(validator);
    const json good = parse(R"({"v":1,"kids":[{"v":2},{"v":3,"kids":[{"v":4}]}]})");
    const json bad = parse(R"({"v":1,"kids":[{"v":-2}]})");
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 500; ++i) {
                if (validator->validate(good)) ++wrong;
                const auto issue = validator->validate(bad);
                if (!issue || issue->instance_pointer != "/kids/0/v") ++wrong;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(wrong.load(), 0);
}

TEST(JsonSchema, ValuesOfEveryShapeAreAnsweredNotThrown) {
    const json schema = parse(R"({"type":["object","array","string","number","boolean","null"],
                                  "properties":{"a":{"minimum":1}},"items":{"maxLength":1},
                                  "pattern":"x","minLength":1,"multipleOf":2})");
    const std::vector<json> values = {json(), json(true), json(1), json(-1), json(2.5), json("x"), json(""),
                                      json::array(), json::object(), parse("[[[1]]]"), parse(R"({"a":{"b":[1]}})"),
                                      json(std::numeric_limits<double>::quiet_NaN()),
                                      json(std::numeric_limits<double>::infinity())};
    for (const json& value : values) {
        (void)check(schema, value);  // the answer does not matter, only that there is one
    }
    SUCCEED();
}

}  // namespace
