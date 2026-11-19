// Tests of describing types, the schemas that follow from the descriptions, and conversion.
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "neomcp/describe.h"

using neomcp::json;

namespace dt {

struct AddArgs {
    int a;
    int b;
    std::optional<std::string> note;
};

enum class Color { red, green, blue };

enum class Mode { fast, slow };

struct Inner {
    std::string name;
    double ratio;
};

struct Outer {
    Inner inner;
    std::vector<Inner> items;
    std::map<std::string, int> tally;
    Color color;
    bool flag;
};

struct Ints {
    std::int8_t i8;
    std::uint8_t u8;
    std::int16_t i16;
    std::uint16_t u16;
    std::int32_t i32;
    std::uint32_t u32;
    std::int64_t i64;
    std::uint64_t u64;
    float f;
    double d;
};

struct Opt {
    int count;
    double ratio;
    std::string text;
    std::vector<int> list;
    std::string mode;
    int level;
    Color color;
    bool verbose;
    std::optional<int> maybe;
};

// Members whose options carry a number that is not finite. The options are built as aggregates because their
// functions refuse such a number in a constant expression.
struct InfiniteMinimum {
    double ok;
    double bad;
};
struct NotANumberMaximum {
    float bad;
};
struct InfiniteChoice {
    double bad;
};
struct NotANumberDefault {
    double bad;
};
struct HugeButFinite {
    double d;
    float f;
};
struct HoldsBadBound {
    InfiniteMinimum inner;
};

struct AllOptional {
    std::optional<int> a;
    std::optional<std::string> b;
};

struct Empty {};

struct Weird {
    int slash;
    int tilde;
};

struct PointMacro {
    int x;
    int y;
};

struct PointHand {
    int x;
    int y;
};

struct Celsius {
    double degrees;
};

struct Holder {
    Celsius temperature;
    std::vector<Celsius> history;
};

struct Wide {
    int m01;
    int m02;
    int m03;
    int m04;
    int m05;
    int m06;
    int m07;
    int m08;
    int m09;
    int m10;
    int m11;
    int m12;
    int m13;
    int m14;
    int m15;
    int m16;
    int m17;
    int m18;
    int m19;
    int m20;
    int m21;
    int m22;
    int m23;
    int m24;
    int m25;
    int m26;
    int m27;
    int m28;
    int m29;
    int m30;
    int m31;
    int m32;
};

enum class Many32 { e01, e02, e03, e04, e05, e06, e07, e08, e09, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32 };
enum class Many63 { f01, f02, f03, f04, f05, f06, f07, f08, f09, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62, f63 };

struct WithOptionalVector {
    std::optional<std::vector<int>> numbers;
};

// Conversion of a type of one's own through neojson's customisation points.
inline void to_json(json& out, const Celsius& value) { out = value.degrees; }
inline void from_json(const json& in, Celsius& value) { value.degrees = in.get<double>(); }

}  // namespace dt

// A type that appears in an option (one_of, default_value) is described before the struct that uses it.
NEOMCP_DESCRIBE_ENUM(dt::Color, red, green, blue)

// The description written by hand, as a user would.
template <>
struct neomcp::Described<dt::AddArgs> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&dt::AddArgs::a, "a", "first addend"),
        neomcp::field(&dt::AddArgs::b, "b", "second addend", neomcp::minimum(0), neomcp::maximum(100)),
        neomcp::field(&dt::AddArgs::note, "note", "a note to echo back"));
};

template <>
struct neomcp::DescribedEnum<dt::Mode> {
    static constexpr neomcp::EnumEntry<dt::Mode> values[] = {{dt::Mode::fast, "fast"}, {dt::Mode::slow, "slow"}};
};

template <>
struct neomcp::Described<dt::PointHand> {
    static constexpr auto fields = neomcp::fields(neomcp::field(&dt::PointHand::x, "x", "horizontal"),
                                                  neomcp::field(&dt::PointHand::y, "y", "vertical"));
};

template <>
struct neomcp::Described<dt::Empty> {
    static constexpr auto fields = neomcp::fields();
};

template <>
struct neomcp::Described<dt::Weird> {
    static constexpr auto fields =
        neomcp::fields(neomcp::field(&dt::Weird::slash, "a/b", ""), neomcp::field(&dt::Weird::tilde, "m~n", ""));
};

template <>
struct neomcp::Described<dt::Opt> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&dt::Opt::count, "count", "how many", neomcp::minimum(1), neomcp::maximum(10),
                      neomcp::default_value(5)),
        neomcp::field(&dt::Opt::ratio, "ratio", "", neomcp::minimum(0.5), neomcp::maximum(1.5),
                      neomcp::default_value(1.0)),
        neomcp::field(&dt::Opt::text, "text", "", neomcp::min_length(1), neomcp::max_length(8),
                      neomcp::default_value("abc")),
        neomcp::field(&dt::Opt::list, "list", "", neomcp::min_items(1), neomcp::max_items(3)),
        neomcp::field(&dt::Opt::mode, "mode", "", neomcp::one_of("fast", "slow")),
        neomcp::field(&dt::Opt::level, "level", "", neomcp::one_of(1, 2, 3)),
        neomcp::field(&dt::Opt::color, "color", "", neomcp::one_of("red", "blue"), neomcp::default_value("green")),
        neomcp::field(&dt::Opt::verbose, "verbose", "", neomcp::default_value(true)),
        neomcp::field(&dt::Opt::maybe, "maybe", "", neomcp::minimum(0)));
};

template <>
struct neomcp::Described<dt::InfiniteMinimum> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&dt::InfiniteMinimum::ok, "ok", "", neomcp::minimum(-1.5)),
        neomcp::field(&dt::InfiniteMinimum::bad, "bad", "",
                      neomcp::Minimum<double>{-std::numeric_limits<double>::infinity()}));
};

template <>
struct neomcp::Described<dt::NotANumberMaximum> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&dt::NotANumberMaximum::bad, "bad", "",
                      neomcp::Maximum<float>{std::numeric_limits<float>::quiet_NaN()}));
};

template <>
struct neomcp::Described<dt::InfiniteChoice> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&dt::InfiniteChoice::bad, "bad", "",
                      neomcp::OneOf<double, 2>{{1.0, std::numeric_limits<double>::infinity()}}));
};

template <>
struct neomcp::Described<dt::NotANumberDefault> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&dt::NotANumberDefault::bad, "bad", "",
                      neomcp::DefaultValue<double>{std::numeric_limits<double>::quiet_NaN()}));
};

template <>
struct neomcp::Described<dt::HugeButFinite> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&dt::HugeButFinite::d, "d", "", neomcp::minimum(std::numeric_limits<double>::lowest()),
                      neomcp::maximum(std::numeric_limits<double>::max())),
        neomcp::field(&dt::HugeButFinite::f, "f", "", neomcp::one_of(-std::numeric_limits<float>::max(), 0.0f),
                      neomcp::default_value(std::numeric_limits<float>::max())));
};

template <>
struct neomcp::Described<dt::HoldsBadBound> {
    static constexpr auto fields = neomcp::fields(neomcp::field(&dt::HoldsBadBound::inner, "inner", ""));
};

template <>
struct neomcp::Schema<dt::Celsius> {
    static neomcp::json make() { return neomcp::json::parse(R"({"type":"number","minimum":-273.15})"); }
};

NEOMCP_DESCRIBE(dt::PointMacro, x, "horizontal", y, "vertical")
NEOMCP_DESCRIBE(dt::Inner, name, "the name", ratio, "")
NEOMCP_DESCRIBE(dt::Outer, inner, "", items, "", tally, "", color, "", flag, "")
NEOMCP_DESCRIBE(dt::Ints, i8, "", u8, "", i16, "", u16, "", i32, "", u32, "", i64, "", u64, "", f, "", d, "")
NEOMCP_DESCRIBE(dt::AllOptional, a, "", b, "")
NEOMCP_DESCRIBE(dt::Holder, temperature, "body temperature", history, "")
NEOMCP_DESCRIBE(dt::WithOptionalVector, numbers, "")
NEOMCP_DESCRIBE(dt::Wide, m01, "", m02, "", m03, "", m04, "", m05, "", m06, "", m07, "", m08, "", m09, "", m10, "", m11, "", m12, "", m13, "", m14, "", m15, "", m16, "", m17, "", m18, "", m19, "", m20, "", m21, "", m22, "", m23, "", m24, "", m25, "", m26, "", m27, "", m28, "", m29, "", m30, "", m31, "", m32, "")
NEOMCP_DESCRIBE_ENUM(dt::Many32, e01, e02, e03, e04, e05, e06, e07, e08, e09, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32)
NEOMCP_DESCRIBE_ENUM(dt::Many63, f01, f02, f03, f04, f05, f06, f07, f08, f09, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62, f63)

namespace {

// Schemas are compared as text: the order of the keys is part of the result.
void expect_json(const json& actual, const char* expected) {
    EXPECT_EQ(actual.dump(), json::parse(expected).dump());
}

template <class T>
neomcp::DecodeError decode_failure(const json& document) {
    try {
        static_cast<void>(neomcp::from_json<T>(document));
    } catch (const neomcp::DecodeError& error) {
        return error;
    }
    ADD_FAILURE() << "no DecodeError for " << document.dump();
    return neomcp::DecodeError("<none>", "<none>");
}

template <class T>
neomcp::DecodeError decode_failure(const char* text) {
    return decode_failure<T>(json::parse(text));
}

dt::Outer sample_outer() {
    dt::Outer outer;
    outer.inner = {"first", 0.5};
    outer.items = {{"a", 1.0}, {"b", 2.5}};
    outer.tally = {{"x", 1}, {"y", -2}};
    outer.color = dt::Color::green;
    outer.flag = true;
    return outer;
}

constexpr const char* kIntRange32 = R"("minimum":-2147483648,"maximum":2147483647)";

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Concepts and constant evaluation

static_assert(neomcp::described<dt::AddArgs>);
static_assert(!neomcp::described<int>);
static_assert(!neomcp::described<dt::Color>);
static_assert(neomcp::described_enum<dt::Color>);
static_assert(neomcp::described_enum<dt::Mode>);
static_assert(!neomcp::described_enum<int>);
static_assert(!neomcp::described_enum<dt::AddArgs>);
static_assert(neomcp::Described<dt::AddArgs>::fields.size == 3);
static_assert(std::get<1>(neomcp::Described<dt::AddArgs>::fields.items).name == "b");
static_assert(std::get<1>(neomcp::Described<dt::AddArgs>::fields.items).description == "second addend");
static_assert(std::get<0>(neomcp::Described<dt::PointMacro>::fields.items).name == "x");
static_assert(neomcp::detail::unique_names(neomcp::Described<dt::AddArgs>::fields));
static_assert(!neomcp::detail::unique_names(neomcp::fields(neomcp::field(&dt::PointHand::x, "same", ""),
                                                           neomcp::field(&dt::PointHand::y, "same", ""))));
static_assert(neomcp::detail::has_schema<int>());
static_assert(neomcp::detail::has_schema<std::vector<std::map<std::string, std::vector<dt::Color>>>>());
static_assert(!neomcp::detail::has_schema<char>());
static_assert(!neomcp::detail::has_schema<char32_t>());
static_assert(!neomcp::detail::has_schema<std::optional<int>>());
static_assert(!neomcp::detail::has_schema<std::vector<std::optional<int>>>());
static_assert(!neomcp::detail::has_schema<std::map<int, int>>());
static_assert(!neomcp::detail::has_schema<std::string_view>());
static_assert(neomcp::detail::has_schema<dt::Celsius>());
static_assert(neomcp::detail::has_schema<std::uint8_t>());
static_assert(neomcp::detail::has_schema<neomcp::json>());

// ---------------------------------------------------------------------------------------------------------------
// Schemas

TEST(DescribeSchema, ScalarTypes) {
    expect_json(neomcp::schema_of<bool>(), R"({"type":"boolean"})");
    expect_json(neomcp::schema_of<float>(), R"({"type":"number"})");
    expect_json(neomcp::schema_of<double>(), R"({"type":"number"})");
    expect_json(neomcp::schema_of<std::string>(), R"({"type":"string"})");
    expect_json(neomcp::schema_of<neomcp::json>(), R"({})");
}

TEST(DescribeSchema, IntegerTypesCarryTheirRange) {
    expect_json(neomcp::schema_of<std::int8_t>(), R"({"type":"integer","minimum":-128,"maximum":127})");
    expect_json(neomcp::schema_of<std::uint8_t>(), R"({"type":"integer","minimum":0,"maximum":255})");
    expect_json(neomcp::schema_of<std::int16_t>(), R"({"type":"integer","minimum":-32768,"maximum":32767})");
    expect_json(neomcp::schema_of<std::uint16_t>(), R"({"type":"integer","minimum":0,"maximum":65535})");
    expect_json(neomcp::schema_of<std::int32_t>(),
                (std::string(R"({"type":"integer",)") + kIntRange32 + "}").c_str());
    expect_json(neomcp::schema_of<std::uint32_t>(), R"({"type":"integer","minimum":0,"maximum":4294967295})");
    expect_json(neomcp::schema_of<std::int64_t>(), R"({"type":"integer"})");
    expect_json(neomcp::schema_of<std::uint64_t>(), R"({"type":"integer","minimum":0})");
}

TEST(DescribeSchema, ContainersAndEnums) {
    expect_json(neomcp::schema_of<std::vector<std::string>>(), R"({"type":"array","items":{"type":"string"}})");
    expect_json(neomcp::schema_of<std::map<std::string, double>>(),
                R"({"type":"object","additionalProperties":{"type":"number"}})");
    expect_json(neomcp::schema_of<std::vector<std::vector<bool>>>(),
                R"({"type":"array","items":{"type":"array","items":{"type":"boolean"}}})");
    expect_json(neomcp::schema_of<dt::Color>(), R"({"type":"string","enum":["red","green","blue"]})");
    expect_json(neomcp::schema_of<dt::Mode>(), R"({"type":"string","enum":["fast","slow"]})");
}

TEST(DescribeSchema, StructWrittenByHand) {
    expect_json(neomcp::schema_of<dt::AddArgs>(),
                (std::string(R"({"type":"object","properties":{"a":{"type":"integer",)") + kIntRange32 +
                 R"(,"description":"first addend"},"b":{"type":"integer","minimum":0,"maximum":100,)" +
                 R"("description":"second addend"},"note":{"type":"string","description":"a note to echo back"}},)" +
                 R"("required":["a","b"],"additionalProperties":false})")
                    .c_str());
}

TEST(DescribeSchema, NestedStructVectorMapAndEnum) {
    const std::string inner =
        R"({"type":"object","properties":{"name":{"type":"string","description":"the name"},"ratio":{"type":"number"}},)"
        R"("required":["name","ratio"],"additionalProperties":false})";
    const std::string expected =
        R"({"type":"object","properties":{"inner":)" + inner + R"(,"items":{"type":"array","items":)" + inner +
        R"(},"tally":{"type":"object","additionalProperties":{"type":"integer",)" + kIntRange32 + R"(}},)" +
        R"("color":{"type":"string","enum":["red","green","blue"]},"flag":{"type":"boolean"}},)" +
        R"("required":["inner","items","tally","color","flag"],"additionalProperties":false})";
    expect_json(neomcp::schema_of<dt::Outer>(), expected.c_str());
}

TEST(DescribeSchema, EveryIntegerWidthAsMember) {
    expect_json(neomcp::schema_of<dt::Ints>(),
                R"({"type":"object","properties":{)"
                R"("i8":{"type":"integer","minimum":-128,"maximum":127},)"
                R"("u8":{"type":"integer","minimum":0,"maximum":255},)"
                R"("i16":{"type":"integer","minimum":-32768,"maximum":32767},)"
                R"("u16":{"type":"integer","minimum":0,"maximum":65535},)"
                R"("i32":{"type":"integer","minimum":-2147483648,"maximum":2147483647},)"
                R"("u32":{"type":"integer","minimum":0,"maximum":4294967295},)"
                R"("i64":{"type":"integer"},"u64":{"type":"integer","minimum":0},)"
                R"("f":{"type":"number"},"d":{"type":"number"}},)"
                R"("required":["i8","u8","i16","u16","i32","u32","i64","u64","f","d"],"additionalProperties":false})");
}

TEST(DescribeSchema, EveryOptionKeyword) {
    expect_json(neomcp::schema_of<dt::Opt>(),
                (std::string(R"({"type":"object","properties":{)") +
                 // minimum, maximum and default on an integer; the type's own bounds are replaced in place
                 R"("count":{"type":"integer","minimum":1,"maximum":10,"description":"how many","default":5},)" +
                 R"("ratio":{"type":"number","minimum":0.5,"maximum":1.5,"default":1.0},)" +
                 R"("text":{"type":"string","minLength":1,"maxLength":8,"default":"abc"},)" +
                 R"("list":{"type":"array","items":{"type":"integer",)" + kIntRange32 +
                 R"(},"minItems":1,"maxItems":3},)" +
                 R"("mode":{"type":"string","enum":["fast","slow"]},)" +
                 R"("level":{"type":"integer",)" + kIntRange32 + R"(,"enum":[1,2,3]},)" +
                 // the enum member's own `enum` is replaced by the option
                 R"("color":{"type":"string","enum":["red","blue"],"default":"green"},)" +
                 R"("verbose":{"type":"boolean","default":true},)" +
                 // an option on an optional member applies to the member's type; the member is not required
                 R"("maybe":{"type":"integer","minimum":0,"maximum":2147483647}},)" +
                 R"("required":["count","ratio","text","list","mode","level","color","verbose"],)" +
                 R"("additionalProperties":false})")
                    .c_str());
}

// A bound that is not a number would be written as null. The schema derivation (which is when a tool is registered)
// refuses it with the member and the option named.
template <class T>
void expect_refused_option(const char* member, const char* option) {
    try {
        static_cast<void>(neomcp::schema_of<T>());
    } catch (const std::invalid_argument& error) {
        const std::string text = error.what();
        EXPECT_NE(text.find(std::string("\"") + member + "\""), std::string::npos) << text;
        EXPECT_NE(text.find(option), std::string::npos) << text;
        return;
    }
    ADD_FAILURE() << "no std::invalid_argument for the option " << option << " of " << member;
}

TEST(DescribeSchema, ANumberThatIsNotFiniteIsRefusedWhenTheSchemaIsDerived) {
    expect_refused_option<dt::InfiniteMinimum>("bad", "neomcp::minimum");
    expect_refused_option<dt::NotANumberMaximum>("bad", "neomcp::maximum");
    expect_refused_option<dt::InfiniteChoice>("bad", "neomcp::one_of");
    expect_refused_option<dt::NotANumberDefault>("bad", "neomcp::default_value");
    // Through a member that is a described struct: the member that holds the number is the one named.
    expect_refused_option<dt::HoldsBadBound>("bad", "neomcp::minimum");
}

TEST(DescribeSchema, TheLargestFiniteNumbersAreAcceptedByEveryNumericOption) {
    const json schema = neomcp::schema_of<dt::HugeButFinite>();
    EXPECT_FALSE(schema.at("properties").at("d").at("minimum").is_null());
    EXPECT_FALSE(schema.at("properties").at("d").at("maximum").is_null());
    EXPECT_EQ(schema.at("properties").at("f").at("enum").at(0).get<double>(),
              -static_cast<double>(std::numeric_limits<float>::max()));
    EXPECT_EQ(schema.at("properties").at("f").at("default").get<double>(),
              static_cast<double>(std::numeric_limits<float>::max()));
}

TEST(DescribeSchema, AnOptionMadeAtRunTimeFromANumberThatIsNotFiniteReportsIt) {
    // The functions refuse such a number only in a constant expression (tests/compile_check/describe_bounds.cpp);
    // at run time the option is made and says it is not finite, which is what schema_of looks at.
    const volatile double infinity = std::numeric_limits<double>::infinity();
    const volatile double not_a_number = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(neomcp::minimum(infinity).finite());
    EXPECT_FALSE(neomcp::maximum(not_a_number).finite());
    EXPECT_FALSE(neomcp::one_of(1.0, infinity).finite());
    EXPECT_FALSE(neomcp::default_value(-infinity).finite());
    EXPECT_TRUE(neomcp::minimum(3).finite());
    EXPECT_TRUE(neomcp::default_value("text").finite());
}

TEST(DescribeSchema, RequiredIsOmittedWhenEveryMemberIsOptional) {
    expect_json(neomcp::schema_of<dt::AllOptional>(),
                (std::string(R"({"type":"object","properties":{"a":{"type":"integer",)") + kIntRange32 +
                 R"(},"b":{"type":"string"}},"additionalProperties":false})")
                    .c_str());
    expect_json(neomcp::schema_of<dt::Empty>(), R"({"type":"object","properties":{},"additionalProperties":false})");
}

TEST(DescribeSchema, OptionalVectorMemberHasTheSchemaOfTheVector) {
    expect_json(neomcp::schema_of<dt::WithOptionalVector>(),
                (std::string(R"({"type":"object","properties":{"numbers":{"type":"array","items":{"type":"integer",)") +
                 kIntRange32 + R"(}}},"additionalProperties":false})")
                    .c_str());
}

TEST(DescribeSchema, UserSchemaIsUsedForTheTypeAndWhereverItAppears) {
    expect_json(neomcp::schema_of<dt::Celsius>(), R"({"type":"number","minimum":-273.15})");
    expect_json(neomcp::schema_of<dt::Holder>(),
                R"({"type":"object","properties":{)"
                R"("temperature":{"type":"number","minimum":-273.15,"description":"body temperature"},)"
                R"("history":{"type":"array","items":{"type":"number","minimum":-273.15}}},)"
                R"("required":["temperature","history"],"additionalProperties":false})");
}

TEST(DescribeSchema, MacroAndHandWrittenDescriptionsGiveEqualSchemas) {
    const json macro = neomcp::schema_of<dt::PointMacro>();
    const json hand = neomcp::schema_of<dt::PointHand>();
    EXPECT_EQ(macro.dump(), hand.dump());
    EXPECT_TRUE(macro == hand);
    expect_json(macro,
                (std::string(R"({"type":"object","properties":{"x":{"type":"integer",)") + kIntRange32 +
                 R"(,"description":"horizontal"},"y":{"type":"integer",)" + kIntRange32 +
                 R"(,"description":"vertical"}},"required":["x","y"],"additionalProperties":false})")
                    .c_str());
}

TEST(DescribeSchema, ThirtyTwoMembersAndEnumerators) {
    const json wide = neomcp::schema_of<dt::Wide>();
    ASSERT_TRUE(wide["properties"].is_object());
    EXPECT_EQ(wide["properties"].size(), 32u);
    EXPECT_EQ(wide["required"].size(), 32u);
    EXPECT_EQ(wide["required"][0].get<std::string>(), "m01");
    EXPECT_EQ(wide["required"][31].get<std::string>(), "m32");

    const json many32 = neomcp::schema_of<dt::Many32>();
    ASSERT_EQ(many32["enum"].size(), 32u);
    EXPECT_EQ(many32["enum"][0].get<std::string>(), "e01");
    EXPECT_EQ(many32["enum"][31].get<std::string>(), "e32");

    const json many63 = neomcp::schema_of<dt::Many63>();
    ASSERT_EQ(many63["enum"].size(), 63u);
    EXPECT_EQ(many63["enum"][62].get<std::string>(), "f63");
}

TEST(DescribeSchema, SameTypeGivesTheSameBytes) {
    EXPECT_EQ(neomcp::schema_of<dt::Outer>().dump(), neomcp::schema_of<dt::Outer>().dump());
    EXPECT_EQ(neomcp::schema_of<dt::Opt>().dump(), neomcp::schema_of<dt::Opt>().dump());
    EXPECT_EQ(neomcp::schema_of<dt::AddArgs>().dump(2), neomcp::schema_of<dt::AddArgs>().dump(2));
}

// ---------------------------------------------------------------------------------------------------------------
// Conversion

TEST(DescribeConvert, OptionalAbsentNullAndPresent) {
    const dt::AddArgs absent = neomcp::from_json<dt::AddArgs>(json::parse(R"({"a":1,"b":2})"));
    EXPECT_EQ(absent.a, 1);
    EXPECT_EQ(absent.b, 2);
    EXPECT_FALSE(absent.note.has_value());
    EXPECT_EQ(neomcp::to_json(absent).dump(), R"({"a":1,"b":2})");

    const dt::AddArgs null_note = neomcp::from_json<dt::AddArgs>(json::parse(R"({"a":1,"b":2,"note":null})"));
    EXPECT_FALSE(null_note.note.has_value());
    EXPECT_EQ(neomcp::to_json(null_note).dump(), R"({"a":1,"b":2})");

    const dt::AddArgs present = neomcp::from_json<dt::AddArgs>(json::parse(R"({"note":"hi","b":2,"a":1})"));
    ASSERT_TRUE(present.note.has_value());
    EXPECT_EQ(*present.note, "hi");
    // Members are written in declaration order whatever order the document had.
    EXPECT_EQ(neomcp::to_json(present).dump(), R"({"a":1,"b":2,"note":"hi"})");
}

TEST(DescribeConvert, NestedValueRoundTripsInDeclarationOrder) {
    const dt::Outer outer = sample_outer();
    const json document = neomcp::to_json(outer);
    EXPECT_EQ(document.dump(),
              R"({"inner":{"name":"first","ratio":0.5},"items":[{"name":"a","ratio":1.0},{"name":"b","ratio":2.5}],)"
              R"("tally":{"x":1,"y":-2},"color":"green","flag":true})");
    const dt::Outer back = neomcp::from_json<dt::Outer>(document);
    EXPECT_EQ(neomcp::to_json(back).dump(), document.dump());
    EXPECT_EQ(back.inner.name, "first");
    EXPECT_EQ(back.items.size(), 2u);
    EXPECT_EQ(back.items[1].ratio, 2.5);
    EXPECT_EQ(back.tally.at("y"), -2);
    EXPECT_EQ(back.color, dt::Color::green);
    EXPECT_TRUE(back.flag);
}

TEST(DescribeConvert, IntegerLimitsRoundTrip) {
    const char* text =
        R"({"i8":-128,"u8":255,"i16":-32768,"u16":65535,"i32":-2147483648,"u32":4294967295,)"
        R"("i64":-9223372036854775808,"u64":18446744073709551615,"f":1.5,"d":0.25})";
    const dt::Ints ints = neomcp::from_json<dt::Ints>(json::parse(text));
    EXPECT_EQ(ints.i8, -128);
    EXPECT_EQ(ints.u8, 255);
    EXPECT_EQ(ints.u32, 4294967295u);
    EXPECT_EQ(ints.i64, std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(ints.u64, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ints.f, 1.5f);
    EXPECT_EQ(ints.d, 0.25);
    EXPECT_EQ(neomcp::to_json(ints).dump(), json::parse(text).dump());

    const dt::Ints top = neomcp::from_json<dt::Ints>(json::parse(
        R"({"i8":127,"u8":0,"i16":32767,"u16":0,"i32":2147483647,"u32":0,)"
        R"("i64":9223372036854775807,"u64":0,"f":0,"d":0})"));
    EXPECT_EQ(top.i64, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(top.i8, 127);
}

TEST(DescribeConvert, NumbersAcrossKinds) {
    EXPECT_EQ(neomcp::from_json<double>(json::parse("3")), 3.0);
    EXPECT_EQ(neomcp::from_json<double>(json::parse("-3")), -3.0);
    EXPECT_EQ(neomcp::from_json<float>(json::parse("2")), 2.0f);
    EXPECT_EQ(neomcp::from_json<int>(json::parse("3.0")), 3);
    EXPECT_EQ(neomcp::from_json<std::uint8_t>(json::parse("255.0")), 255);
    EXPECT_EQ(neomcp::from_json<std::uint64_t>(json::parse("18446744073709551615")),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(DescribeConvert, EnumsByName) {
    EXPECT_EQ(neomcp::to_json(dt::Color::blue).dump(), R"("blue")");
    EXPECT_EQ(neomcp::from_json<dt::Color>(json("red")), dt::Color::red);
    EXPECT_EQ(neomcp::to_json(dt::Mode::slow).dump(), R"("slow")");
    EXPECT_EQ(neomcp::from_json<dt::Mode>(json("fast")), dt::Mode::fast);
    EXPECT_THROW(neomcp::to_json(static_cast<dt::Color>(9)), std::invalid_argument);
    EXPECT_EQ(neomcp::from_json<dt::Many32>(json("e32")), dt::Many32::e32);
    EXPECT_EQ(neomcp::from_json<dt::Many63>(json("f63")), dt::Many63::f63);
}

TEST(DescribeConvert, ContainersAtTheTop) {
    const std::vector<std::vector<int>> grid = {{1, 2}, {}, {3}};
    EXPECT_EQ(neomcp::to_json(grid).dump(), "[[1,2],[],[3]]");
    EXPECT_EQ(neomcp::from_json<std::vector<std::vector<int>>>(json::parse("[[1,2],[],[3]]")), grid);

    using Lists = std::map<std::string, std::vector<std::string>>;
    const Lists map = {{"k", {"a", "b"}}, {"l", {}}};
    EXPECT_EQ(neomcp::to_json(map).dump(), R"({"k":["a","b"],"l":[]})");
    EXPECT_EQ(neomcp::from_json<Lists>(json::parse(R"({"l":[],"k":["a","b"]})")), map);

    const std::vector<bool> flags = {true, false, true};
    EXPECT_EQ(neomcp::to_json(flags).dump(), "[true,false,true]");
    EXPECT_EQ(neomcp::from_json<std::vector<bool>>(json::parse("[true,false,true]")), flags);

    const json any = json::parse(R"({"free":[1,"form"]})");
    EXPECT_EQ(neomcp::to_json(any), any);
    EXPECT_EQ(neomcp::from_json<json>(any), any);
}

TEST(DescribeConvert, EmptyStructAndOptionalVector) {
    EXPECT_EQ(neomcp::to_json(dt::Empty{}).dump(), "{}");
    static_cast<void>(neomcp::from_json<dt::Empty>(json::object()));

    dt::WithOptionalVector value;
    EXPECT_EQ(neomcp::to_json(value).dump(), "{}");
    value.numbers = std::vector<int>{4, 5};
    EXPECT_EQ(neomcp::to_json(value).dump(), R"({"numbers":[4,5]})");
    EXPECT_EQ(neomcp::from_json<dt::WithOptionalVector>(json::parse(R"({"numbers":[4,5]})")).numbers, value.numbers);
    EXPECT_FALSE(neomcp::from_json<dt::WithOptionalVector>(json::parse(R"({"numbers":null})")).numbers.has_value());
}

TEST(DescribeConvert, UnknownMembersAreIgnoredAndOptionsAreNotEnforced) {
    const dt::AddArgs args = neomcp::from_json<dt::AddArgs>(json::parse(R"({"a":1,"b":2,"extra":[1,2,3]})"));
    EXPECT_EQ(args.a, 1);
    // `b` is declared with maximum(100); validating against the schema enforces that, decoding does not.
    EXPECT_EQ(neomcp::from_json<dt::AddArgs>(json::parse(R"({"a":1,"b":1000})")).b, 1000);
}

TEST(DescribeConvert, ThirtyTwoMembersRoundTrip) {
    json document = json::object();
    for (int i = 1; i <= 32; ++i) {
        const std::string name = std::string("m") + (i < 10 ? "0" : "") + std::to_string(i);
        document[name] = i * 3;
    }
    const dt::Wide wide = neomcp::from_json<dt::Wide>(document);
    EXPECT_EQ(wide.m01, 3);
    EXPECT_EQ(wide.m32, 96);
    EXPECT_EQ(neomcp::to_json(wide).dump(), document.dump());
}

TEST(DescribeConvert, UserTypeConvertsThroughNeojsonCustomisation) {
    const dt::Holder holder = neomcp::from_json<dt::Holder>(json::parse(R"({"temperature":36.5,"history":[1,2.5]})"));
    EXPECT_EQ(holder.temperature.degrees, 36.5);
    ASSERT_EQ(holder.history.size(), 2u);
    EXPECT_EQ(holder.history[1].degrees, 2.5);
    EXPECT_EQ(neomcp::to_json(holder).dump(), R"({"temperature":36.5,"history":[1.0,2.5]})");
}

// ---------------------------------------------------------------------------------------------------------------
// Decoding failures

TEST(DescribeDecodeError, WrongTypeAtTheRoot) {
    const auto error = decode_failure<int>(R"("x")");
    EXPECT_EQ(error.pointer(), "");
    EXPECT_EQ(error.expected(), "integer");
    EXPECT_EQ(decode_failure<bool>("1").expected(), "boolean");
    EXPECT_EQ(decode_failure<std::string>("1").expected(), "string");
    EXPECT_EQ(decode_failure<double>(R"("x")").expected(), "number");
}

TEST(DescribeDecodeError, WrongTypeOfMember) {
    const auto error = decode_failure<dt::AddArgs>(R"({"a":"x","b":2})");
    EXPECT_EQ(error.pointer(), "/a");
    EXPECT_EQ(error.expected(), "integer");
    EXPECT_EQ(decode_failure<dt::AddArgs>(R"({"a":null,"b":2})").pointer(), "/a");
}

TEST(DescribeDecodeError, MissingRequiredMemberIsNamedByThePlaceItShouldBeAt) {
    const auto error = decode_failure<dt::AddArgs>(R"({"a":1})");
    EXPECT_EQ(error.pointer(), "/b");
    EXPECT_EQ(error.expected(), "required member of type integer");
    EXPECT_EQ(decode_failure<dt::AddArgs>(R"({})").pointer(), "/a");
}

TEST(DescribeDecodeError, NestedTwoLevelsDeep) {
    json document = neomcp::to_json(sample_outer());
    document["inner"]["ratio"] = "fast";
    const auto wrong = decode_failure<dt::Outer>(document);
    EXPECT_EQ(wrong.pointer(), "/inner/ratio");
    EXPECT_EQ(wrong.expected(), "number");

    document = neomcp::to_json(sample_outer());
    document["inner"].erase("name");
    const auto missing = decode_failure<dt::Outer>(document);
    EXPECT_EQ(missing.pointer(), "/inner/name");
    EXPECT_EQ(missing.expected(), "required member of type string");
}

TEST(DescribeDecodeError, InsideAVectorElement) {
    json document = neomcp::to_json(sample_outer());
    document["items"][1]["name"] = 5;
    EXPECT_EQ(decode_failure<dt::Outer>(document).pointer(), "/items/1/name");

    document = neomcp::to_json(sample_outer());
    document["items"][0].erase("ratio");
    EXPECT_EQ(decode_failure<dt::Outer>(document).pointer(), "/items/0/ratio");

    EXPECT_EQ(decode_failure<std::vector<std::vector<int>>>("[[1],[2,\"x\"]]").pointer(), "/1/1");
}

TEST(DescribeDecodeError, PointerEscapesSlashAndTilde) {
    EXPECT_EQ(decode_failure<dt::Weird>(R"({"m~n":1})").pointer(), "/a~1b");
    EXPECT_EQ(decode_failure<dt::Weird>(R"({"a/b":1})").pointer(), "/m~0n");
    EXPECT_EQ(decode_failure<dt::Weird>(R"({"a/b":"s","m~n":1})").pointer(), "/a~1b");
    EXPECT_EQ(decode_failure<dt::Weird>(R"({"a/b":1,"m~n":"s"})").pointer(), "/m~0n");

    json document = neomcp::to_json(sample_outer());
    document["tally"]["x/y"] = "s";
    EXPECT_EQ(decode_failure<dt::Outer>(document).pointer(), "/tally/x~1y");
}

TEST(DescribeDecodeError, IntegerOutOfRange) {
    auto with = [](const char* member, const char* value) {
        json document = json::parse(
            R"({"i8":0,"u8":0,"i16":0,"u16":0,"i32":0,"u32":0,"i64":0,"u64":0,"f":0,"d":0})");
        document[member] = json::parse(value);
        return decode_failure<dt::Ints>(document);
    };
    const auto i8 = with("i8", "128");
    EXPECT_EQ(i8.pointer(), "/i8");
    EXPECT_EQ(i8.expected(), "integer from -128 to 127");
    EXPECT_EQ(with("i8", "-129").pointer(), "/i8");
    EXPECT_EQ(with("u8", "256").expected(), "integer from 0 to 255");
    EXPECT_EQ(with("u8", "-1").pointer(), "/u8");
    EXPECT_EQ(with("i16", "32768").pointer(), "/i16");
    EXPECT_EQ(with("u16", "65536").pointer(), "/u16");
    EXPECT_EQ(with("i32", "2147483648").pointer(), "/i32");
    EXPECT_EQ(with("i32", "-2147483649").pointer(), "/i32");
    EXPECT_EQ(with("u32", "4294967296").expected(), "integer from 0 to 4294967295");
    EXPECT_EQ(with("i64", "9223372036854775808").pointer(), "/i64");
    EXPECT_EQ(with("u64", "-1").pointer(), "/u64");
    EXPECT_EQ(with("u64", "-1").expected(), "integer from 0 to 18446744073709551615");
    EXPECT_EQ(with("i32", "1e30").pointer(), "/i32");
    EXPECT_EQ(with("i32", "1e30").expected(), "integer from -2147483648 to 2147483647");
    EXPECT_EQ(with("u64", "18446744073709551616.0").pointer(), "/u64");
}

TEST(DescribeDecodeError, NonIntegralNumberForInteger) {
    const auto error = decode_failure<dt::AddArgs>(R"({"a":1.5,"b":2})");
    EXPECT_EQ(error.pointer(), "/a");
    EXPECT_EQ(error.expected(), "integer");
    EXPECT_EQ(decode_failure<int>("0.1").expected(), "integer");
}

TEST(DescribeDecodeError, FloatThatCannotBeHeld) {
    const auto error = decode_failure<dt::Ints>(
        R"({"i8":0,"u8":0,"i16":0,"u16":0,"i32":0,"u32":0,"i64":0,"u64":0,"f":1e300,"d":0})");
    EXPECT_EQ(error.pointer(), "/f");
}

TEST(DescribeDecodeError, EnumNameNotDeclared) {
    json document = neomcp::to_json(sample_outer());
    document["color"] = "purple";
    const auto error = decode_failure<dt::Outer>(document);
    EXPECT_EQ(error.pointer(), "/color");
    EXPECT_EQ(error.expected(), R"(one of "red", "green", "blue")");

    document["color"] = 5;
    const auto wrong_type = decode_failure<dt::Outer>(document);
    EXPECT_EQ(wrong_type.pointer(), "/color");
    EXPECT_EQ(wrong_type.expected(), "string");
}

TEST(DescribeDecodeError, NonObjectWhereAStructIsExpected) {
    const auto root = decode_failure<dt::AddArgs>("[1,2]");
    EXPECT_EQ(root.pointer(), "");
    EXPECT_EQ(root.expected(), "object");

    json document = neomcp::to_json(sample_outer());
    document["inner"] = 3;
    const auto inner = decode_failure<dt::Outer>(document);
    EXPECT_EQ(inner.pointer(), "/inner");
    EXPECT_EQ(inner.expected(), "object");
}

TEST(DescribeDecodeError, NonArrayAndNonObjectForContainers) {
    json document = neomcp::to_json(sample_outer());
    document["items"] = "none";
    EXPECT_EQ(decode_failure<dt::Outer>(document).pointer(), "/items");
    EXPECT_EQ(decode_failure<dt::Outer>(document).expected(), "array");

    document = neomcp::to_json(sample_outer());
    document["tally"] = json::array();
    EXPECT_EQ(decode_failure<dt::Outer>(document).pointer(), "/tally");
    EXPECT_EQ(decode_failure<dt::Outer>(document).expected(), "object");
}

TEST(DescribeDecodeError, OptionalMemberOfTheWrongType) {
    const auto error = decode_failure<dt::AddArgs>(R"({"a":1,"b":2,"note":5})");
    EXPECT_EQ(error.pointer(), "/note");
    EXPECT_EQ(error.expected(), "string");
}

TEST(DescribeDecodeError, ConversionOfAUserTypeThatThrows) {
    const auto error = decode_failure<dt::Holder>(R"({"temperature":"hot","history":[]})");
    EXPECT_EQ(error.pointer(), "/temperature");
    const auto inside = decode_failure<dt::Holder>(R"({"temperature":1,"history":[1,"hot"]})");
    EXPECT_EQ(inside.pointer(), "/history/1");
}

TEST(DescribeDecodeError, MessageCombinesPointerAndExpectation) {
    const auto error = decode_failure<dt::AddArgs>(R"({"a":1})");
    const std::string message = error.what();
    EXPECT_NE(message.find("/b"), std::string::npos);
    EXPECT_NE(message.find("required member of type integer"), std::string::npos);
    const std::runtime_error& base = error;
    EXPECT_EQ(std::string(base.what()), message);
}
