// Tests of the form a declared input asks with, built from a described struct.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/server.h"

namespace sdt {

struct Folder {
    std::string path;
};

struct Topic {
    std::string topic;
};

struct Item {
    std::string id;
};

struct Confirm {
    bool ok;
};

struct Destination {
    std::string city;
};

struct Note {
    std::string text;
};

struct Rating {
    std::int32_t stars;
};

enum class Color { red, green, blue };

struct Survey {
    std::string name;
    std::int32_t age;
    double score;
    bool verified;
    Color color;
    std::vector<Color> extras;
    std::optional<std::string> nickname;
    std::string level;
};

// Members a form cannot hold.
struct Nested {
    Confirm inner;
};
struct Counts {
    std::map<std::string, int> counts;
};
struct Tags {
    std::vector<std::string> tags;
};
struct Anything {
    neomcp::json anything;
};
struct Level {
    int level;
};

// A string with a format, through a schema of its own.
struct Email {
    std::string text;
};
struct Contact {
    Email address;
};
struct Host {
    std::string text;
};
struct Machine {
    Host host;
};

}  // namespace sdt

template <>
struct neomcp::Schema<sdt::Email> {
    static neomcp::json make() { return neomcp::json::parse(R"({"type":"string","format":"email"})"); }
};
template <>
struct neomcp::Schema<sdt::Host> {
    static neomcp::json make() { return neomcp::json::parse(R"({"type":"string","format":"hostname"})"); }
};

NEOMCP_DESCRIBE(sdt::Folder, path, "the folder")
NEOMCP_DESCRIBE(sdt::Topic, topic, "what the prompt is about")
NEOMCP_DESCRIBE(sdt::Item, id, "the item")
NEOMCP_DESCRIBE(sdt::Confirm, ok, "Delete it?")
NEOMCP_DESCRIBE(sdt::Destination, city, "where to")
NEOMCP_DESCRIBE(sdt::Note, text, "a note")
NEOMCP_DESCRIBE_ENUM(sdt::Color, red, green, blue)
NEOMCP_DESCRIBE(sdt::Nested, inner, "")
NEOMCP_DESCRIBE(sdt::Counts, counts, "")
NEOMCP_DESCRIBE(sdt::Tags, tags, "")
NEOMCP_DESCRIBE(sdt::Anything, anything, "")
NEOMCP_DESCRIBE(sdt::Contact, address, "where to write")
NEOMCP_DESCRIBE(sdt::Machine, host, "")

template <>
struct neomcp::Described<sdt::Survey> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&sdt::Survey::name, "name", "Your name", neomcp::title("Name"), neomcp::min_length(1),
                      neomcp::max_length(40), neomcp::default_value("Ann")),
        neomcp::field(&sdt::Survey::age, "age", "", neomcp::minimum(0), neomcp::maximum(150)),
        neomcp::field(&sdt::Survey::score, "score", "A score", neomcp::default_value(1.5)),
        neomcp::field(&sdt::Survey::verified, "verified", "", neomcp::default_value(true)),
        neomcp::field(&sdt::Survey::color, "color", "A colour"),
        neomcp::field(&sdt::Survey::extras, "extras", "", neomcp::min_items(0), neomcp::max_items(2)),
        neomcp::field(&sdt::Survey::nickname, "nickname", ""),
        neomcp::field(&sdt::Survey::level, "level", "", neomcp::one_of("low", "high")));
};

template <>
struct neomcp::Described<sdt::Rating> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&sdt::Rating::stars, "stars", "How many stars", neomcp::minimum(1), neomcp::maximum(5)));
};

template <>
struct neomcp::Described<sdt::Level> {
    static constexpr auto fields = neomcp::fields(neomcp::field(&sdt::Level::level, "level", "", neomcp::one_of(1, 2, 3)));
};

namespace {

using neomcp::json;
namespace profile = neomcp::profile;

constexpr const char* modern_revision = "2026-07-28";

json J(const char* text) { return json::parse(text); }

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

// ---------------------------------------------------------------------------------------------------------------
// The form

std::string member_type_name(const json& member) {
    const std::string type = member.at("type").get<std::string>();
    if (type == "string") return member.contains("enum") ? "UntitledSingleSelectEnumSchema" : "StringSchema";
    if (type == "integer" || type == "number") return "NumberSchema";
    if (type == "boolean") return "BooleanSchema";
    return "UntitledMultiSelectEnumSchema";
}

std::set<std::string> keys_of(const json& object) {
    std::set<std::string> keys;
    for (const auto& item : object.items()) keys.insert(item.key());
    return keys;
}

std::int64_t integer_of(const json& number) { return number.get<std::int64_t>(); }

TEST(FormSchema, EveryMemberIsWrittenWithTheKeywordsOfItsShape) {
    const json schema = neomcp::form_schema_of<sdt::Survey>();
    EXPECT_EQ(keys_of(schema), (std::set<std::string>{"type", "properties", "required"})) << schema.dump();
    EXPECT_EQ(schema.at("type"), "object");
    std::set<std::string> required;
    for (const json& entry : schema.at("required")) required.insert(entry.get<std::string>());
    EXPECT_EQ(required, (std::set<std::string>{"name", "age", "score", "verified", "color", "extras", "level"}));
    const json& members = schema.at("properties");
    EXPECT_EQ(keys_of(members),
              (std::set<std::string>{"name", "age", "score", "verified", "color", "extras", "nickname", "level"}));

    const json& name = members.at("name");
    EXPECT_EQ(keys_of(name),
              (std::set<std::string>{"type", "description", "title", "minLength", "maxLength", "default"}));
    EXPECT_EQ(name.at("type"), "string");
    EXPECT_EQ(name.at("title"), "Name");
    EXPECT_EQ(name.at("description"), "Your name");
    EXPECT_EQ(integer_of(name.at("minLength")), 1);
    EXPECT_EQ(integer_of(name.at("maxLength")), 40);
    EXPECT_EQ(name.at("default"), "Ann");

    const json& age = members.at("age");
    EXPECT_EQ(keys_of(age), (std::set<std::string>{"type", "minimum", "maximum"}));
    EXPECT_EQ(age.at("type"), "integer");
    EXPECT_EQ(integer_of(age.at("minimum")), 0);
    EXPECT_EQ(integer_of(age.at("maximum")), 150);

    const json& score = members.at("score");
    EXPECT_EQ(keys_of(score), (std::set<std::string>{"type", "description", "default"}));
    EXPECT_EQ(score.at("type"), "number");
    EXPECT_DOUBLE_EQ(score.at("default").get<double>(), 1.5);

    const json& verified = members.at("verified");
    EXPECT_EQ(keys_of(verified), (std::set<std::string>{"type", "default"}));
    EXPECT_EQ(verified.at("type"), "boolean");
    EXPECT_EQ(verified.at("default"), true);

    const json& color = members.at("color");
    EXPECT_EQ(keys_of(color), (std::set<std::string>{"type", "enum", "description"}));
    EXPECT_EQ(color.at("type"), "string");
    EXPECT_EQ(color.at("enum"), J(R"(["red","green","blue"])"));

    const json& extras = members.at("extras");
    EXPECT_EQ(keys_of(extras), (std::set<std::string>{"type", "items", "minItems", "maxItems"}));
    EXPECT_EQ(extras.at("type"), "array");
    EXPECT_EQ(keys_of(extras.at("items")), (std::set<std::string>{"type", "enum"}));
    EXPECT_EQ(extras.at("items").at("type"), "string");
    EXPECT_EQ(extras.at("items").at("enum"), J(R"(["red","green","blue"])"));
    EXPECT_EQ(integer_of(extras.at("minItems")), 0);
    EXPECT_EQ(integer_of(extras.at("maxItems")), 2);

    EXPECT_EQ(keys_of(members.at("nickname")), std::set<std::string>{"type"});
    const json& level = members.at("level");
    EXPECT_EQ(keys_of(level), (std::set<std::string>{"type", "enum"}));
    EXPECT_EQ(level.at("enum"), J(R"(["low","high"])"));

    // Every keyword is a property of the type of the restricted schema the revision defines for the member's shape.
    const auto modern = builtin(modern_revision);
    for (const auto& item : schema.at("properties").items()) {
        const profile::Type* type = modern->type(member_type_name(item.value()));
        ASSERT_NE(type, nullptr) << item.key();
        for (const auto& keyword : item.value().items()) {
            EXPECT_TRUE(type->has_property(keyword.key())) << item.key() << ": " << keyword.key() << " in " << type->name;
        }
        for (const std::string& required : type->required) {
            EXPECT_TRUE(item.value().contains(required)) << item.key() << " lacks " << required;
        }
    }

    // A string with a format of its own type fits.
    EXPECT_EQ(neomcp::form_schema_of<sdt::Contact>().at("properties").at("address"),
              J(R"({"type":"string","format":"email","description":"where to write"})"));
}

template <class Form>
std::string refusal_of() {
    try {
        static_cast<void>(neomcp::form_schema_of<Form>());
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return std::string();
}

TEST(FormSchema, AMemberThatDoesNotFitIsRefusedByName) {
    const std::pair<std::string, std::string> refused[] = {
        {refusal_of<sdt::Nested>(), "'inner'"}, {refusal_of<sdt::Counts>(), "'counts'"},
        {refusal_of<sdt::Tags>(), "'tags'"},    {refusal_of<sdt::Anything>(), "'anything'"},
        {refusal_of<sdt::Level>(), "'level'"},  {refusal_of<sdt::Machine>(), "'host'"},
    };
    for (const auto& [message, member] : refused) {
        EXPECT_NE(message.find(member), std::string::npos) << "refused naming " << member << ": " << message;
    }
}

}  // namespace
