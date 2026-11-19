// Argument headers: the marks a tool's input schema carries, the rules they obey when a tool is registered, and the
// check a call undergoes against the headers of its request.
//
// Every expected value is written here from the specification of the revision that defines argument headers, not
// read back from its profile; the profile supplies the keyword and the header prefix only to the code under test.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/argument_headers.h"
#include "neomcp/profile.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace {

using neomcp::json;

constexpr const char* modern_revision = "2026-07-28";
constexpr const char* keyword = "x-mcp-header";

json parsed(const std::string& text) {
    json value = json::parse(text, nullptr, false);
    if (value.is_discarded()) throw std::runtime_error("the test's schema is not JSON: " + text);
    return value;
}

// The schema of an object with one property, which carries `property` (JSON text).
json one_property(const std::string& property) {
    return parsed("{\"type\":\"object\",\"properties\":{\"p\":" + property + "}}");
}

// ---- The marks of a schema --------------------------------------------------------------------------------------

struct Accepted {
    std::string name;
    std::string schema;
    std::vector<std::string> marks;  // "Name=path/to/it", in the order of the paths
};

std::vector<std::string> marks_of(const std::string& schema) {
    const auto found = neomcp::header_parameters(parsed(schema), keyword);
    const auto* parameters = std::get_if<std::vector<neomcp::HeaderParameter>>(&found);
    if (parameters == nullptr) {
        const auto& issue = std::get<neomcp::HeaderMarkIssue>(found);
        return {"REFUSED " + issue.pointer + " " + issue.rule};
    }
    std::vector<std::string> marks;
    for (const auto& parameter : *parameters) {
        std::string path;
        for (const auto& step : parameter.path) path += (path.empty() ? "" : "/") + step;
        marks.push_back(parameter.name + "=" + path);
    }
    return marks;
}

TEST(ArgumentHeaderMarks, AcceptedSchemas) {
    const std::vector<Accepted> cases{
        {"no mark", R"({"type":"object","properties":{"a":{"type":"string"}}})", {}},
        {"a string", R"({"type":"object","properties":{"region":{"type":"string","x-mcp-header":"Region"}}})",
         {"Region=region"}},
        {"a boolean", R"({"type":"object","properties":{"b":{"type":"boolean","x-mcp-header":"B"}}})", {"B=b"}},
        {"an integer with both bounds",
         R"({"type":"object","properties":{"n":{"type":"integer","minimum":0,"maximum":9007199254740991,"x-mcp-header":"N"}}})",
         {"N=n"}},
        {"an integer with exclusive bounds",
         R"({"type":"object","properties":{"n":{"type":"integer","exclusiveMinimum":-5,"exclusiveMaximum":5,"x-mcp-header":"N"}}})",
         {"N=n"}},
        {"an integer without bounds",
         R"({"type":"object","properties":{"n":{"type":"integer","x-mcp-header":"N"}}})", {"N=n"}},
        {"an integer with one bound",
         R"({"type":"object","properties":{"n":{"type":"integer","minimum":0,"x-mcp-header":"N"}}})", {"N=n"}},
        {"an integer whose declared bounds are past the safe range",
         R"({"type":"object","properties":{"n":{"type":"integer","minimum":-9007199254740993,"maximum":9007199254740993,"x-mcp-header":"N"}}})",
         {"N=n"}},
        {"an integer that is a constant",
         R"({"type":"object","properties":{"n":{"type":"integer","const":7,"x-mcp-header":"N"}}})", {"N=n"}},
        {"an integer from a list",
         R"({"type":"object","properties":{"n":{"type":"integer","enum":[1,2,-9007199254740991],"x-mcp-header":"N"}}})",
         {"N=n"}},
        {"a string that may be null",
         R"({"type":"object","properties":{"s":{"type":["string","null"],"x-mcp-header":"S"}}})", {"S=s"}},
        {"nested objects",
         R"({"type":"object","properties":{"a":{"type":"object","properties":{"b":{"type":"string","x-mcp-header":"Deep"}}}}})",
         {"Deep=a/b"}},
        {"a name with every kind of token character",
         R"({"type":"object","properties":{"s":{"type":"string","x-mcp-header":"A-b_c.d!#$%&'*+^`|~9"}}})",
         {"A-b_c.d!#$%&'*+^`|~9=s"}},
        {"several, in the order of their paths",
         R"({"type":"object","properties":{"z":{"type":"string","x-mcp-header":"Z"},"a":{"type":"string","x-mcp-header":"A"}}})",
         {"A=a", "Z=z"}},
        {"a property that is called like the keyword",
         R"({"type":"object","properties":{"x-mcp-header":{"type":"string"}}})", {}},
        {"the keyword inside the data keywords",
         R"({"type":"object","properties":{"s":{"type":"string","default":{"x-mcp-header":"D"},"examples":[{"x-mcp-header":"E"}]},"c":{"const":{"x-mcp-header":"C"}},"e":{"enum":[{"x-mcp-header":"E"}]}}})",
         {}},
        {"required and unrelated keywords beside the mark",
         R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","required":["s"],"additionalProperties":false,"properties":{"s":{"type":"string","description":"d","x-mcp-header":"S"}}})",
         {"S=s"}},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        EXPECT_EQ(marks_of(item.schema), item.marks);
    }
}

struct Refused {
    std::string name;
    std::string schema;
    std::string pointer;  // where the rule is broken
    std::string rule;     // a part of the rule's words
};

TEST(ArgumentHeaderMarks, RefusedSchemas) {
    const std::string s = R"({"type":"string","x-mcp-header":"H"})";
    const std::string reach = "statically reachable";
    const std::vector<Refused> cases{
        // Where a mark may sit.
        {"under items", R"({"type":"object","properties":{"l":{"type":"array","items":{"type":"string","x-mcp-header":"H"}}}})",
         "/properties/l/items/x-mcp-header", reach},
        {"under prefixItems",
         R"({"type":"object","properties":{"l":{"type":"array","prefixItems":[{"type":"string","x-mcp-header":"H"}]}}})",
         "/properties/l/prefixItems/0/x-mcp-header", reach},
        {"under allOf", R"({"type":"object","allOf":[{"properties":{"p":)" + s + "}}]}", "/allOf/0/properties/p/x-mcp-header", reach},
        {"under anyOf", R"({"type":"object","anyOf":[{"properties":{"p":)" + s + "}}]}", "/anyOf/0/properties/p/x-mcp-header", reach},
        {"under oneOf", R"({"type":"object","oneOf":[{"properties":{"p":)" + s + "}}]}", "/oneOf/0/properties/p/x-mcp-header", reach},
        {"under not", R"({"type":"object","not":{"properties":{"p":)" + s + "}}}", "/not/properties/p/x-mcp-header", reach},
        {"under if", R"({"type":"object","if":{"properties":{"p":)" + s + "}}}", "/if/properties/p/x-mcp-header", reach},
        {"under then", R"({"type":"object","then":{"properties":{"p":)" + s + "}}}", "/then/properties/p/x-mcp-header", reach},
        {"under else", R"({"type":"object","else":{"properties":{"p":)" + s + "}}}", "/else/properties/p/x-mcp-header", reach},
        {"in a definition that a reference reaches",
         R"({"type":"object","properties":{"p":{"$ref":"#/$defs/d"}},"$defs":{"d":)" + s + "}}", "/$defs/d/x-mcp-header", reach},
        {"in definitions of the older spelling",
         R"({"type":"object","definitions":{"d":)" + s + "}}", "/definitions/d/x-mcp-header", reach},
        {"under additionalProperties",
         R"({"type":"object","additionalProperties":{"type":"string","x-mcp-header":"H"}})",
         "/additionalProperties/x-mcp-header", reach},
        {"under patternProperties", R"({"type":"object","patternProperties":{"^a":)" + s + "}}",
         "/patternProperties/^a/x-mcp-header", reach},
        {"under dependentSchemas",
         R"({"type":"object","dependentSchemas":{"a":{"properties":{"p":)" + s + "}}}}",
         "/dependentSchemas/a/properties/p/x-mcp-header", reach},
        {"under a keyword nobody defined",
         R"({"type":"object","x-other":{"properties":{"p":)" + s + "}}}", "/x-other/properties/p/x-mcp-header", reach},
        {"below an array property",
         R"({"type":"object","properties":{"a":{"type":"array","items":{"type":"object","properties":{"p":)" + s + "}}}}}",
         "/properties/a/items/properties/p/x-mcp-header", reach},
        {"a property of an object that a composition adds",
         R"({"type":"object","properties":{"o":{"type":"object","allOf":[{"properties":{"p":)" + s + "}}]}}}",
         "/properties/o/allOf/0/properties/p/x-mcp-header", reach},
        {"on the root", R"({"type":"string","x-mcp-header":"H"})", "/x-mcp-header", "root"},
        // The value of the mark.
        {"an empty name", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":""}}})",
         "/properties/p/x-mcp-header", "empty"},
        {"a name with a space", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":"A B"}}})",
         "/properties/p/x-mcp-header", "token"},
        {"a name with a colon", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":"A:B"}}})",
         "/properties/p/x-mcp-header", "token"},
        {"a name with a line feed", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":"A\nB"}}})",
         "/properties/p/x-mcp-header", "token"},
        {"a name with a carriage return", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":"A\rB"}}})",
         "/properties/p/x-mcp-header", "token"},
        {"a name with a non-ASCII letter", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":"Région"}}})",
         "/properties/p/x-mcp-header", "token"},
        {"a name that is not a string", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":5}}})",
         "/properties/p/x-mcp-header", "string"},
        {"a name that is an object", R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":{"a":1}}}})",
         "/properties/p/x-mcp-header", "string"},
        {"names that differ in case only",
         R"({"type":"object","properties":{"a":{"type":"string","x-mcp-header":"Region"},"b":{"type":"string","x-mcp-header":"rEGION"}}})",
         "/properties/b/x-mcp-header", "without regard to case"},
        {"the same name twice",
         R"({"type":"object","properties":{"a":{"type":"string","x-mcp-header":"R"},"b":{"type":"boolean","x-mcp-header":"R"}}})",
         "/properties/b/x-mcp-header", "unique"},
        {"the same name at two depths",
         R"({"type":"object","properties":{"a":{"type":"object","properties":{"c":{"type":"string","x-mcp-header":"R"}}},"b":{"type":"string","x-mcp-header":"r"}}})",
         "/properties/b/x-mcp-header", "unique"},
        // The type of the parameter.
        {"a number", R"({"type":"object","properties":{"p":{"type":"number","x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "not number"},
        {"an object", R"({"type":"object","properties":{"p":{"type":"object","x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "type"},
        {"an array", R"({"type":"object","properties":{"p":{"type":"array","x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "type"},
        {"null alone", R"({"type":"object","properties":{"p":{"type":"null","x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "type"},
        {"two primitives", R"({"type":"object","properties":{"p":{"type":["string","boolean"],"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "type"},
        {"no type", R"({"type":"object","properties":{"p":{"enum":["a"],"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "type"},
        {"a reference instead of a type",
         R"({"type":"object","properties":{"p":{"$ref":"#/$defs/t","x-mcp-header":"H"}},"$defs":{"t":{"type":"string"}}})",
         "/properties/p/x-mcp-header", "type"},
        {"a type of the wrong shape", R"({"type":"object","properties":{"p":{"type":5,"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "type"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        const auto found = neomcp::header_parameters(parsed(item.schema), keyword);
        const auto* issue = std::get_if<neomcp::HeaderMarkIssue>(&found);
        ASSERT_NE(issue, nullptr) << "accepted";
        EXPECT_EQ(issue->pointer, item.pointer);
        EXPECT_NE(issue->rule.find(item.rule), std::string::npos) << issue->rule;
    }
}

TEST(ArgumentHeaderMarks, ThePointerEscapesTheNamesOfProperties) {
    const auto found = neomcp::header_parameters(
        parsed(R"({"type":"object","properties":{"a/b~c":{"type":"array","items":{"x-mcp-header":"H"}}}})"), keyword);
    const auto* issue = std::get_if<neomcp::HeaderMarkIssue>(&found);
    ASSERT_NE(issue, nullptr);
    EXPECT_EQ(issue->pointer, "/properties/a~1b~0c/items/x-mcp-header");
}

TEST(ArgumentHeaderMarks, ASchemaNestedTooDeepIsRefusedNotFollowedWithoutEnd) {
    json schema = parsed(R"({"type":"object","properties":{"p":{"type":"string","x-mcp-header":"H"}}})");
    for (int level = 0; level < 200; ++level) {
        schema = json{{"type", "object"}, {"properties", json{{"n", std::move(schema)}}}};
    }
    const auto found = neomcp::header_parameters(schema, keyword);
    const auto* issue = std::get_if<neomcp::HeaderMarkIssue>(&found);
    ASSERT_NE(issue, nullptr);
    EXPECT_NE(issue->rule.find("deeper"), std::string::npos) << issue->rule;
    EXPECT_TRUE(std::holds_alternative<std::vector<neomcp::HeaderParameter>>(
        neomcp::header_parameters(schema, keyword, 1000)));
}

TEST(ArgumentHeaderMarks, ParametersCarryTheirTypeAndPlace) {
    const auto found = neomcp::header_parameters(
        parsed(R"({"type":"object","properties":{"o":{"type":"object","properties":{"n":{"type":"integer","minimum":0,"maximum":9,"x-mcp-header":"N"}}},"b":{"type":"boolean","x-mcp-header":"B"}}})"),
        keyword);
    const auto& parameters = std::get<std::vector<neomcp::HeaderParameter>>(found);
    ASSERT_EQ(parameters.size(), 2U);
    EXPECT_EQ(parameters[0].name, "B");
    EXPECT_EQ(parameters[0].type, neomcp::HeaderParameterType::boolean);
    EXPECT_EQ(parameters[0].pointer, "/properties/b");
    EXPECT_EQ(parameters[1].name, "N");
    EXPECT_EQ(parameters[1].type, neomcp::HeaderParameterType::integer);
    EXPECT_EQ(parameters[1].pointer, "/properties/o/properties/n");
    EXPECT_EQ(parameters[1].path, (std::vector<std::string>{"o", "n"}));
}

// ---- Registration -----------------------------------------------------------------------------------------------

neomcp::ToolDefinition tool_of(std::string name, json schema) {
    neomcp::ToolDefinition definition;
    definition.name = std::move(name);
    definition.description = "a tool";
    definition.input_schema = std::move(schema);
    return definition;
}

std::string registration_error(neomcp::Server& server, const neomcp::ToolDefinition& definition) {
    try {
        server.tool(definition, [](const json&) { return neomcp::ToolResult{}; });
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

std::vector<neomcp::profile::ProfilePtr> profiles_of(std::initializer_list<const char*> revisions) {
    std::vector<neomcp::profile::ProfilePtr> out;
    for (const char* revision : revisions) out.push_back(neomcp::profile::builtin(revision));
    return out;
}

TEST(ArgumentHeaderRegistration, AToolWithValidMarksIsRegisteredAndListedWithThem) {
    neomcp::Server server({.name = "t", .version = "1"});
    const json schema = parsed(R"({"type":"object","properties":{"region":{"type":"string","x-mcp-header":"Region"}}})");
    EXPECT_NO_THROW(server.tool(tool_of("spanner", schema), [](const json&) { return neomcp::ToolResult{}; }));
    ASSERT_EQ(server.tools().size(), 1U);
    EXPECT_EQ(server.tools()[0].input_schema, schema) << "the schema is listed as it was written";
}

TEST(ArgumentHeaderRegistration, AToolWithAMarkThatBreaksARuleIsRefusedWithItsNameItsPlaceAndTheRule) {
    neomcp::Server server({.name = "t", .version = "1"});
    const json schema = parsed(R"({"type":"object","properties":{"l":{"type":"array","items":{"type":"string","x-mcp-header":"H"}}}})");
    const std::string message = registration_error(server, tool_of("spanner", schema));
    EXPECT_NE(message.find("spanner"), std::string::npos) << message;
    EXPECT_NE(message.find("/properties/l/items/x-mcp-header"), std::string::npos) << message;
    EXPECT_NE(message.find("statically reachable"), std::string::npos) << message;
    EXPECT_NE(message.find(keyword), std::string::npos) << message;
    EXPECT_TRUE(server.tools().empty()) << "nothing was registered";
}

TEST(ArgumentHeaderRegistration, EveryRuleIsEnforcedWhenAToolIsRegistered) {
    neomcp::Server server({.name = "t", .version = "1"});
    const std::vector<std::pair<std::string, json>> broken{
        {"a number", one_property(R"({"type":"number","x-mcp-header":"H"})")},
        {"an empty name", one_property(R"({"type":"string","x-mcp-header":""})")},
        {"a name that is not a token", one_property(R"({"type":"string","x-mcp-header":"a b"})")},
        {"a duplicate",
         parsed(R"({"type":"object","properties":{"a":{"type":"string","x-mcp-header":"H"},"b":{"type":"string","x-mcp-header":"h"}}})")},
        {"a mark on the root", parsed(R"({"type":"object","x-mcp-header":"H"})")},
    };
    for (const auto& [name, schema] : broken) {
        SCOPED_TRACE(name);
        EXPECT_FALSE(registration_error(server, tool_of("t_" + name, schema)).empty());
    }
    EXPECT_TRUE(server.tools().empty());
}

TEST(ArgumentHeaderRegistration, RevisionsThatDefineNoArgumentHeadersCheckNothing) {
    neomcp::ServerOptions options{.name = "t", .version = "1"};
    options.profiles = profiles_of({"2025-11-25", "2025-06-18"});
    neomcp::Server server(std::move(options));
    const json schema = parsed(R"({"type":"object","properties":{"l":{"type":"array","items":{"type":"number","x-mcp-header":""}}}})");
    EXPECT_NO_THROW(server.tool(tool_of("loose", schema), [](const json&) { return neomcp::ToolResult{}; }));
    ASSERT_EQ(server.tools().size(), 1U);
    EXPECT_EQ(server.tools()[0].input_schema, schema) << "the keyword stays in the schema the tool lists";
}

TEST(ArgumentHeaderRegistration, ServingTheRevisionThatDefinesThemAmongOthersChecksTheMarks) {
    neomcp::ServerOptions options{.name = "t", .version = "1"};
    options.profiles = profiles_of({modern_revision, "2025-11-25"});
    neomcp::Server server(std::move(options));
    const json number = one_property(R"({"type":"number","x-mcp-header":"H"})");
    EXPECT_FALSE(registration_error(server, tool_of("t", number)).empty());
}

}  // namespace

// ---- Marks written with the describe layer ----------------------------------------------------------------------

namespace described {

struct Place {
    std::string city;
    std::optional<std::string> zone;
};

struct Spanner {
    std::string region;
    std::int32_t priority;
    bool verbose;
    std::optional<std::string> note;
    Place place;
    std::string query;
};

struct Wide {
    std::int64_t count;
};

struct BadName {
    std::string name;
};

struct Twice {
    std::string a;
    std::string b;
};

struct Doubled {
    std::string a;
};

struct Listed {
    std::vector<Place> places;
};

}  // namespace described

template <>
struct neomcp::Described<described::Place> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&described::Place::city, "city", "the city", neomcp::mirror_in_header("City")),
        neomcp::field(&described::Place::zone, "zone", "the zone", neomcp::mirror_in_header("Zone")));
};

template <>
struct neomcp::Described<described::Spanner> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&described::Spanner::region, "region", "the region", neomcp::mirror_in_header("Region")),
        neomcp::field(&described::Spanner::priority, "priority", "the priority", neomcp::mirror_in_header("Priority")),
        neomcp::field(&described::Spanner::verbose, "verbose", "chatty", neomcp::mirror_in_header("Verbose")),
        neomcp::field(&described::Spanner::note, "note", "a note"),
        neomcp::field(&described::Spanner::place, "place", "where"),
        neomcp::field(&described::Spanner::query, "query", "the query"));
};

template <>
struct neomcp::Described<described::Wide> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&described::Wide::count, "count", "how many", neomcp::mirror_in_header("Count")));
};

template <>
struct neomcp::Described<described::BadName> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&described::BadName::name, "name", "a name", neomcp::mirror_in_header("not a token")));
};

template <>
struct neomcp::Described<described::Twice> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&described::Twice::a, "a", "", neomcp::mirror_in_header("Same")),
        neomcp::field(&described::Twice::b, "b", "", neomcp::mirror_in_header("sAME")));
};

template <>
struct neomcp::Described<described::Doubled> {
    static constexpr auto fields = neomcp::fields(
        neomcp::field(&described::Doubled::a, "a", "", neomcp::mirror_in_header("First"), neomcp::mirror_in_header("Second")));
};

template <>
struct neomcp::Described<described::Listed> {
    static constexpr auto fields = neomcp::fields(neomcp::field(&described::Listed::places, "places", "the places"));
};

namespace {

std::string mark_of(const json& schema, std::initializer_list<const char*> properties) {
    const json* at = &schema;
    for (const char* name : properties) {
        if (!at->contains("properties") || !at->at("properties").contains(name)) return "(no property)";
        at = &at->at("properties").at(name);
    }
    return at->contains(keyword) ? at->at(keyword).get<std::string>() : "(none)";
}

TEST(DescribedHeaderMarks, TheMarksOfAStructAreCollectedWithTheirPlaces) {
    const auto marks = neomcp::header_marks_of<described::Spanner>();
    ASSERT_EQ(marks.size(), 5U);
    EXPECT_EQ(marks[0].name, "Region");
    EXPECT_EQ(marks[0].path, (std::vector<std::string>{"region"}));
    EXPECT_EQ(marks[1].name, "Priority");
    EXPECT_EQ(marks[2].name, "Verbose");
    EXPECT_EQ(marks[3].name, "City");
    EXPECT_EQ(marks[3].path, (std::vector<std::string>{"place", "city"}));
    EXPECT_EQ(marks[4].name, "Zone");
    EXPECT_EQ(marks[4].path, (std::vector<std::string>{"place", "zone"}));
    EXPECT_TRUE(neomcp::header_marks_of<described::Listed>().empty()) << "nothing below a vector is reached";
}

TEST(DescribedHeaderMarks, TheSchemaOfTheTypeAloneCarriesNoMark) {
    const json schema = neomcp::schema_of<described::Spanner>();
    EXPECT_EQ(mark_of(schema, {"region"}), "(none)");
}

TEST(DescribedHeaderMarks, ARegisteredToolListsTheKeywordOnTheMarkedMembersOnly) {
    neomcp::Server server({.name = "t", .version = "1"});
    server.tool<described::Spanner>("spanner", "d", [](const described::Spanner&) { return std::string("done"); });
    ASSERT_EQ(server.tools().size(), 1U);
    const json& schema = server.tools()[0].input_schema;
    EXPECT_EQ(mark_of(schema, {"region"}), "Region");
    EXPECT_EQ(mark_of(schema, {"priority"}), "Priority");
    EXPECT_EQ(mark_of(schema, {"verbose"}), "Verbose");
    EXPECT_EQ(mark_of(schema, {"place", "city"}), "City");
    EXPECT_EQ(mark_of(schema, {"place", "zone"}), "Zone") << "an optional member may carry a mark";
    EXPECT_EQ(mark_of(schema, {"note"}), "(none)");
    EXPECT_EQ(mark_of(schema, {"place"}), "(none)");
    EXPECT_EQ(mark_of(schema, {"query"}), "(none)");
    // What the schema of the type says is kept.
    EXPECT_EQ(schema.at("properties").at("priority").at("maximum"), 2147483647);
    EXPECT_EQ(schema.at("properties").at("region").at("description"), "the region");
}

TEST(DescribedHeaderMarks, ARevisionWithoutArgumentHeadersGetsNoKeyword) {
    neomcp::ServerOptions options{.name = "t", .version = "1"};
    options.profiles = profiles_of({"2025-11-25"});
    neomcp::Server server(std::move(options));
    server.tool<described::Spanner>("spanner", "d", [](const described::Spanner&) { return std::string("done"); });
    EXPECT_EQ(mark_of(server.tools()[0].input_schema, {"region"}), "(none)");
}

TEST(DescribedHeaderMarks, TheKeywordOfEveryServedRevisionIsWritten) {
    // Two profiles that name different keywords: each is written, so a client of either finds its own.
    const auto text = std::string(neomcp::profile::builtin_text(modern_revision));
    json second = parsed(text);
    second["revision"] = "2099-01-01";
    second["transports"]["streamable_http"]["argument_headers"]["schema_keyword"] = "x-other-header";
    const auto loaded = neomcp::profile::load(second.dump());
    if (const auto* error = std::get_if<neomcp::profile::Error>(&loaded)) {
        FAIL() << "the profile did not load: " << error->pointer << " " << error->message;
    }
    neomcp::ServerOptions options{.name = "t", .version = "1"};
    options.profiles = {neomcp::profile::builtin(modern_revision), std::get<neomcp::profile::ProfilePtr>(loaded)};
    neomcp::Server server(std::move(options));
    server.tool<described::Place>("place", "d", [](const described::Place&) { return std::string("done"); });
    const json& city = server.tools()[0].input_schema.at("properties").at("city");
    EXPECT_EQ(city.at(keyword), "City");
    EXPECT_EQ(city.at("x-other-header"), "City");
}

TEST(DescribedHeaderMarks, MarksThatBreakARuleRefuseTheToolAndAnIntegerNeedsNoBounds) {
    neomcp::Server server({.name = "t", .version = "1"});
    const auto message_of = [&](auto tool_registration) {
        try {
            tool_registration();
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    const std::string wide = message_of(
        [&] { server.tool<described::Wide>("wide", "d", [](const described::Wide&) { return std::string("done"); }); });
    EXPECT_TRUE(wide.empty()) << "a 64-bit integer member needs no bounds: " << wide;
    const std::string bad = message_of(
        [&] { server.tool<described::BadName>("bad", "d", [](const described::BadName&) { return std::string("done"); }); });
    EXPECT_NE(bad.find("token"), std::string::npos) << bad;
    const std::string twice = message_of(
        [&] { server.tool<described::Twice>("twice", "d", [](const described::Twice&) { return std::string("done"); }); });
    EXPECT_NE(twice.find("without regard to case"), std::string::npos) << twice;
    ASSERT_EQ(server.tools().size(), 1U) << "only the tool with the integer was registered";
    EXPECT_EQ(mark_of(server.tools()[0].input_schema, {"count"}), "Count");
}

TEST(DescribedHeaderMarks, AMemberWithTwoMarksRefusesTheToolInsteadOfKeepingOne) {
    neomcp::Server server({.name = "t", .version = "1"});
    EXPECT_THROW(
        server.tool<described::Doubled>("doubled", "d", [](const described::Doubled&) { return std::string("done"); }),
        std::invalid_argument);
    EXPECT_EQ(server.tools().size(), 0U);
}

}  // namespace

namespace {

// ---- The check of a call -----------------------------------------------------------------------------------------

using Fields = std::vector<std::pair<std::string, std::string>>;

// Collects the outputs of a session by unit and waits for a unit's last one.
class Recorder {
public:
    void operator()(neomcp::Output output) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const neomcp::UnitId unit = output.unit.value_or(0);
        const bool last = output.last;
        units_[unit].push_back(std::move(output));
        if (last) ended_[unit] = true;
        changed_.notify_all();
    }

    std::vector<neomcp::Output> wait(neomcp::UnitId unit) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(30), [&] { return ended_[unit]; })) {
            throw std::runtime_error("unit " + std::to_string(unit) + " did not end");
        }
        return units_[unit];
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<neomcp::UnitId, std::vector<neomcp::Output>> units_;
    std::map<neomcp::UnitId, bool> ended_;
};

// What a tools/call came to.
struct Outcome {
    json message;                                  // the one message the call was answered with
    neomcp::RefusalReason refusal = neomcp::RefusalReason::none;
    int status = 0;                                // the status the profile prescribes for the refusal; 0 for none

    bool refused() const { return message.contains("error"); }
    int code() const { return refused() ? message.at("error").at("code").get<int>() : 0; }
    std::string text() const { return refused() ? message.at("error").at("message").get<std::string>() : std::string(); }
    // A tool result with isError set, or an error answer.
    bool failed() const {
        return refused() || (message.at("result").contains("isError") && message.at("result").at("isError").get<bool>());
    }
};

const char* const spanner_schema = R"({
    "type": "object",
    "properties": {
        "region": {"type": "string", "x-mcp-header": "Region"},
        "priority": {"type": ["integer", "null"], "minimum": 0, "maximum": 100, "x-mcp-header": "Priority"},
        "verbose": {"type": ["boolean", "null"], "x-mcp-header": "Verbose"},
        "place": {"type": ["object", "null"], "properties": {"city": {"type": ["string", "null"], "x-mcp-header": "City"}}},
        "query": {"type": "string"}
    },
    "required": ["region"]
})";

class CallRig {
public:
    explicit CallRig(neomcp::profile::Transport transport = neomcp::profile::Transport::streamable_http)
        : server({.name = "t", .version = "1"}) {
        neomcp::ToolDefinition spanner = tool_of("spanner", parsed(spanner_schema));
        server.tool(spanner, [this](const json&) {
            ++runs;
            return neomcp::ToolResult{};
        });
        neomcp::ToolDefinition plain = tool_of("plain", parsed(R"({"type":"object","properties":{"query":{"type":"string"}}})"));
        server.tool(plain, [this](const json&) {
            ++runs;
            return neomcp::ToolResult{};
        });
        neomcp::ToolDefinition counter = tool_of("counter", parsed(R"({"type":"object","properties":{"n":{"type":"integer","x-mcp-header":"N"}}})"));
        server.tool(counter, [this](const json&) {
            ++runs;
            return neomcp::ToolResult{};
        });
        auto made = server.connect_tagged(transport, [this](neomcp::Output output) { recorder(std::move(output)); });
        session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
    }

    // A tools/call of the 2026-07-28 revision, with the standard headers and `extra`.
    Outcome call(const std::string& tool, const json& arguments, const Fields& extra, bool standard = true) {
        const auto modern = neomcp::profile::builtin(modern_revision);
        const json request = neomcp::testing::Peer::make_request(*modern, neomcp::RequestId(next_id),
                                                                 "tools/call", json{{"name", tool}, {"arguments", arguments}});
        Fields fields;
        if (standard) fields = {{"MCP-Protocol-Version", modern_revision}, {"Mcp-Method", "tools/call"}, {"Mcp-Name", tool}};
        fields.insert(fields.end(), extra.begin(), extra.end());
        return send(request, fields);
    }

    Outcome send(const json& request, const Fields& fields) {
        const neomcp::UnitId unit = next_id++;
        std::vector<neomcp::HeaderField> headers;
        for (const auto& [name, value] : fields) headers.push_back(neomcp::HeaderField{name, value});
        session->receive(unit, request.dump(), headers);
        const auto outputs = recorder.wait(unit);
        Outcome outcome;
        for (const auto& output : outputs) {
            if (output.kind != neomcp::OutputKind::response) continue;
            outcome.message = json::parse(output.line);
            outcome.refusal = output.refusal;
            if (output.profile && output.profile->streamable_http()) {
                outcome.status = output.profile->streamable_http()->status_for(neomcp::to_string(output.refusal)).value_or(0);
            }
        }
        return outcome;
    }

    neomcp::Server server;
    Recorder recorder;
    std::unique_ptr<neomcp::Session> session;
    std::atomic<int> runs{0};
    int next_id = 1;
};

// The header the pages give for a region.
Fields region(const std::string& value) { return {{"Mcp-Param-Region", value}}; }

struct CallCase {
    std::string name;
    json arguments;
    Fields headers;
    std::string refused;  // empty: the call runs; else a part of the refusal's message
};

TEST(ArgumentHeaderCall, StringsAreComparedExactlyAndTheEncodedFormIsDecodedFirst) {
    const std::vector<CallCase> cases{
        {"a plain value", {{"region", "us-west1"}}, region("us-west1"), ""},
        {"the empty string", {{"region", ""}}, region(""), ""},
        {"a name in other cases", {{"region", "us"}}, {{"mcp-param-REGION", "us"}}, ""},
        {"non-ASCII text in the encoded form", {{"region", "Hello, \xe4\xb8\x96\xe7\x95\x8c"}},
         region("=?base64?SGVsbG8sIOS4lueVjA==?="), ""},
        {"padding in the encoded form", {{"region", " padded "}}, region("=?base64?IHBhZGRlZCA=?="), ""},
        {"a line break in the encoded form", {{"region", "line1\nline2"}}, region("=?base64?bGluZTEKbGluZTI=?="), ""},
        {"a value that looks encoded, encoded", {{"region", "=?base64?literal?="}},
         region("=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?="), ""},
        {"digits of Base64 without the wrapper are literal", {{"region", "SGVsbG8="}}, region("SGVsbG8="), ""},
        {"a prefix without the suffix is literal", {{"region", "=?base64?SGVsbG8="}}, region("=?base64?SGVsbG8="), ""},
        {"a suffix without the prefix is literal", {{"region", "SGVsbG8=?="}}, region("SGVsbG8=?="), ""},
        {"a space inside", {{"region", "a b"}}, region("a b"), ""},
        {"the header is missing", {{"region", "us"}}, {}, "lacks the Mcp-Param-Region"},
        {"the header differs", {{"region", "us"}}, region("eu"), "does not match"},
        {"the header differs in case", {{"region", "us"}}, region("US"), "does not match"},
        {"the header holds the encoded digits of the value", {{"region", "Hello"}}, region("SGVsbG8="), "does not match"},
        {"the encoded header differs", {{"region", "Hello"}}, region("=?base64?SGVsbG8h?="), "does not match"},
        {"the value has outer whitespace the header lost", {{"region", " us "}}, region("us"), "does not match"},
        {"the padding of the encoded form is missing", {{"region", "Hello"}}, region("=?base64?SGVsbG8?="), "encoded form"},
        {"the encoded form has characters outside the alphabet", {{"region", "Hello"}}, region("=?base64?SGVs!!!bG8=?="),
         "encoded form"},
        {"the encoded form is the URL-safe alphabet", {{"region", "~~~"}}, region("=?base64?fn5-?="), "encoded form"},
        {"the encoded form in the standard alphabet", {{"region", "~~~"}}, region("=?base64?fn5+?="), ""},
        {"the encoded form has stray bits", {{"region", "Hello"}}, region("=?base64?SGVsbG9=?="), "encoded form"},
        {"the header is twice there", {{"region", "us"}}, {{"Mcp-Param-Region", "us"}, {"mcp-param-region", "us"}},
         "more than once"},
        {"a control character in a plain value", {{"region", "a\x01" "b"}}, region(std::string("a\x01" "b")), "character"},
        {"a delete character in a plain value", {{"region", "a\x7f" "b"}}, region("a\x7f" "b"), "character"},
        {"a byte above ASCII in a plain value", {{"region", "Hello, \xe4\xb8\x96"}}, region("Hello, \xe4\xb8\x96"),
         "character"},
        {"a line break in a plain value", {{"region", "a\nb"}}, region("a\nb"), "character"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        CallRig rig;
        const Outcome outcome = rig.call("spanner", item.arguments, item.headers);
        if (item.refused.empty()) {
            EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
            EXPECT_EQ(rig.runs.load(), 1);
            continue;
        }
        EXPECT_EQ(outcome.code(), -32020) << outcome.message.dump();
        EXPECT_EQ(outcome.refusal, neomcp::RefusalReason::header_mismatch);
        EXPECT_EQ(outcome.status, 400);
        EXPECT_NE(outcome.text().find(item.refused), std::string::npos) << outcome.text();
        EXPECT_EQ(rig.runs.load(), 0) << "the tool did not run";
    }
}

TEST(ArgumentHeaderCall, IntegersAreComparedAsNumbersAndBooleansAsWords) {
    const json region_only = {{"region", "us"}};
    const auto with = [&](const char* name, json value) {
        json arguments = region_only;
        arguments[name] = std::move(value);
        return arguments;
    };
    const auto both = [](Fields first, Fields second) {
        first.insert(first.end(), second.begin(), second.end());
        return first;
    };
    const Fields r = region("us");
    const std::vector<CallCase> cases{
        {"an integer", with("priority", 42), both(r, {{"Mcp-Param-Priority", "42"}}), ""},
        {"zero", with("priority", 0), both(r, {{"Mcp-Param-Priority", "0"}}), ""},
        {"a header that spells a whole number with a fraction", with("priority", 42), both(r, {{"Mcp-Param-Priority", "42.0"}}), ""},
        {"a header with more zeros than needed", with("priority", 7), both(r, {{"Mcp-Param-Priority", "007.000"}}), ""},
        {"minus zero", with("priority", 0), both(r, {{"Mcp-Param-Priority", "-0"}}), ""},
        {"an argument written with a fraction of zero", with("priority", json(42.0)), both(r, {{"Mcp-Param-Priority", "42"}}), ""},
        {"an integer in the encoded form", with("priority", 42), both(r, {{"Mcp-Param-Priority", "=?base64?NDI=?="}}), ""},
        {"a different integer", with("priority", 42), both(r, {{"Mcp-Param-Priority", "43"}}), "does not match"},
        {"a fraction that is not zero", with("priority", 42), both(r, {{"Mcp-Param-Priority", "42.5"}}), "does not match"},
        {"an exponent", with("priority", 100), both(r, {{"Mcp-Param-Priority", "1e2"}}), "does not match"},
        {"a hexadecimal number", with("priority", 42), both(r, {{"Mcp-Param-Priority", "0x2a"}}), "does not match"},
        {"a plus sign", with("priority", 42), both(r, {{"Mcp-Param-Priority", "+42"}}), "does not match"},
        {"a leading space", with("priority", 42), both(r, {{"Mcp-Param-Priority", " 42"}}), "does not match"},
        {"a trailing dot", with("priority", 42), both(r, {{"Mcp-Param-Priority", "42."}}), "does not match"},
        {"an empty value", with("priority", 42), both(r, {{"Mcp-Param-Priority", ""}}), "does not match"},
        {"only a sign", with("priority", 42), both(r, {{"Mcp-Param-Priority", "-"}}), "does not match"},
        {"a word", with("priority", 42), both(r, {{"Mcp-Param-Priority", "forty-two"}}), "does not match"},
        {"the opposite sign", with("priority", -5), both(r, {{"Mcp-Param-Priority", "5"}}), "does not match"},
        {"a header past the range of a header number", with("priority", 7),
         both(r, {{"Mcp-Param-Priority", "99999999999999999999999999"}}), "does not match"},
        {"the integer is missing its header", with("priority", 42), r, "lacks the Mcp-Param-Priority"},
        {"an integer past the safe range cannot be mirrored", with("priority", 9007199254740993LL),
         both(r, {{"Mcp-Param-Priority", "9007199254740993"}}), "cannot mirror"},
        {"a number that is not whole cannot be mirrored", with("priority", 4.5), both(r, {{"Mcp-Param-Priority", "4.5"}}), "cannot mirror"},
        {"an object cannot be mirrored", with("priority", json::object()), both(r, {{"Mcp-Param-Priority", "{}"}}), "cannot mirror"},
        {"an array cannot be mirrored", with("priority", json::array()), both(r, {{"Mcp-Param-Priority", "[]"}}), "cannot mirror"},
        {"true", with("verbose", true), both(r, {{"Mcp-Param-Verbose", "true"}}), ""},
        {"false", with("verbose", false), both(r, {{"Mcp-Param-Verbose", "false"}}), ""},
        {"true against false", with("verbose", true), both(r, {{"Mcp-Param-Verbose", "false"}}), "does not match"},
        {"a capital", with("verbose", true), both(r, {{"Mcp-Param-Verbose", "True"}}), "does not match"},
        {"one", with("verbose", true), both(r, {{"Mcp-Param-Verbose", "1"}}), "does not match"},
        {"a boolean without its header", with("verbose", false), r, "lacks the Mcp-Param-Verbose"},
        {"a boolean given as a string is compared as the string it is", with("verbose", "true"),
         both(r, {{"Mcp-Param-Verbose", "true"}}), ""},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        CallRig rig;
        const Outcome outcome = rig.call("spanner", item.arguments, item.headers);
        if (item.refused.empty()) {
            // Whether the schema accepts the arguments is not this test's question: they were not refused for a header.
            EXPECT_NE(outcome.code(), -32020) << outcome.message.dump();
            EXPECT_EQ(outcome.refusal, neomcp::RefusalReason::none);
            continue;
        }
        EXPECT_EQ(outcome.code(), -32020) << outcome.message.dump();
        EXPECT_EQ(outcome.status, 400);
        EXPECT_NE(outcome.text().find(item.refused), std::string::npos) << outcome.text();
        EXPECT_EQ(rig.runs.load(), 0);
    }
}

TEST(ArgumentHeaderCall, AnIntegerOutsideTheSafeRangeIsRefusedWhateverTheSchemaDeclares) {
    // The schema of the tool declares no bounds; the range is enforced on the values.
    const std::vector<std::pair<json, std::string>> inside{
        {9007199254740991LL, "9007199254740991"}, {-9007199254740991LL, "-9007199254740991"}};
    for (const auto& [value, text] : inside) {
        SCOPED_TRACE(text);
        CallRig rig;
        const Outcome outcome = rig.call("counter", json{{"n", value}}, {{"Mcp-Param-N", text}});
        EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
        EXPECT_EQ(rig.runs.load(), 1);
    }
    const std::vector<std::pair<json, std::string>> outside{
        {9007199254740992LL, "9007199254740992"},
        {-9007199254740992LL, "-9007199254740992"},
        {json(std::uint64_t{18446744073709551615ULL}), "18446744073709551615"},
        {json(1e300), "1e300"}};
    for (const auto& [value, text] : outside) {
        SCOPED_TRACE(text);
        CallRig rig;
        const Outcome outcome = rig.call("counter", json{{"n", value}}, {{"Mcp-Param-N", text}});
        EXPECT_EQ(outcome.code(), -32020) << outcome.message.dump();
        EXPECT_EQ(outcome.status, 400);
        EXPECT_NE(outcome.text().find("cannot mirror"), std::string::npos) << outcome.text();
        EXPECT_EQ(rig.runs.load(), 0) << "the tool did not run";
    }
}

TEST(ArgumentHeaderCall, NestedParametersAreReadAtTheirPath) {
    CallRig rig;
    const json arguments = {{"region", "us"}, {"place", {{"city", "Oslo"}}}};
    Outcome outcome = rig.call("spanner", arguments, {{"Mcp-Param-Region", "us"}, {"Mcp-Param-City", "Oslo"}});
    EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    outcome = rig.call("spanner", arguments, {{"Mcp-Param-Region", "us"}, {"Mcp-Param-City", "Rome"}});
    EXPECT_EQ(outcome.code(), -32020);
    EXPECT_NE(outcome.text().find("params.arguments.place.city"), std::string::npos) << outcome.text();
    outcome = rig.call("spanner", arguments, {{"Mcp-Param-Region", "us"}});
    EXPECT_EQ(outcome.code(), -32020);
    EXPECT_NE(outcome.text().find("Mcp-Param-City"), std::string::npos) << outcome.text();
    EXPECT_EQ(rig.runs.load(), 1);
}

TEST(ArgumentHeaderCall, AnArgumentThatIsAbsentOrNullHasNoHeaderToExpect) {
    CallRig rig;
    const Fields r = region("us");
    // Absent, and null, and a parent that is not an object: the header is not expected.
    for (const json& arguments :
         {json{{"region", "us"}}, json{{"region", "us"}, {"priority", nullptr}, {"verbose", nullptr}},
          json{{"region", "us"}, {"place", nullptr}}, json{{"region", "us"}, {"place", {{"city", nullptr}}}}}) {
        SCOPED_TRACE(arguments.dump());
        const Outcome outcome = rig.call("spanner", arguments, r);
        EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    }
    // Its presence is ignored when the header is well formed ...
    Fields extra = r;
    extra.emplace_back("Mcp-Param-Priority", "5");
    Outcome outcome = rig.call("spanner", json{{"region", "us"}}, extra);
    EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    extra.back().second = "not even a number";
    outcome = rig.call("spanner", json{{"region", "us"}, {"priority", nullptr}}, extra);
    EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    // ... and a header that is malformed is refused whether or not the argument is there.
    extra.back().second = "=?base64?!!!!?=";
    outcome = rig.call("spanner", json{{"region", "us"}}, extra);
    EXPECT_EQ(outcome.code(), -32020);
    EXPECT_NE(outcome.text().find("encoded form"), std::string::npos) << outcome.text();
    extra.back().second = "a\x01";
    outcome = rig.call("spanner", json{{"region", "us"}}, extra);
    EXPECT_EQ(outcome.code(), -32020);
    extra.back().second = "5";
    extra.emplace_back("Mcp-Param-Priority", "5");
    outcome = rig.call("spanner", json{{"region", "us"}}, extra);
    EXPECT_EQ(outcome.code(), -32020);
    EXPECT_NE(outcome.text().find("more than once"), std::string::npos) << outcome.text();
}

TEST(ArgumentHeaderCall, TheHeadersAreCheckedBeforeTheArgumentsAreCheckedAgainstTheSchema) {
    CallRig rig;
    // The priority is out of the range the schema allows, and the header says something else: the header is refused.
    Outcome outcome = rig.call("spanner", json{{"region", "us"}, {"priority", 500}},
                               {{"Mcp-Param-Region", "us"}, {"Mcp-Param-Priority", "7"}});
    EXPECT_EQ(outcome.code(), -32020) << outcome.message.dump();
    EXPECT_EQ(outcome.status, 400);
    // The header agrees: the arguments are then refused by the schema, as a result that says so.
    outcome = rig.call("spanner", json{{"region", "us"}, {"priority", 500}},
                       {{"Mcp-Param-Region", "us"}, {"Mcp-Param-Priority", "500"}});
    EXPECT_EQ(outcome.code(), 0) << outcome.message.dump();
    EXPECT_TRUE(outcome.failed());
    EXPECT_EQ(outcome.refusal, neomcp::RefusalReason::none);
    EXPECT_EQ(rig.runs.load(), 0);
}

TEST(ArgumentHeaderCall, AMessageNamesTheHeaderAndEchoesNoValue) {
    CallRig rig;
    const std::string secret(4000, 's');
    Outcome outcome = rig.call("spanner", json{{"region", secret}}, region(secret + "x"));
    EXPECT_EQ(outcome.code(), -32020);
    EXPECT_NE(outcome.text().find("Mcp-Param-Region"), std::string::npos);
    EXPECT_EQ(outcome.text().find("ssss"), std::string::npos) << "no part of a value is repeated";
    EXPECT_LT(outcome.text().size(), 300U);
    outcome = rig.call("spanner", json{{"region", "us"}}, region(std::string(4000, '\x01')));
    EXPECT_EQ(outcome.code(), -32020);
    EXPECT_LT(outcome.text().size(), 300U);
    outcome = rig.call("spanner", json{{"region", "us"}}, region("=?base64?" + std::string(4000, '!') + "?="));
    EXPECT_EQ(outcome.code(), -32020);
    EXPECT_LT(outcome.text().size(), 300U);
}

TEST(ArgumentHeaderCall, AToolWithoutMarksNeedsNoHeader) {
    CallRig rig;
    const Outcome outcome = rig.call("plain", json{{"query", "x"}}, {{"Mcp-Param-Query", "different"}});
    EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    EXPECT_EQ(rig.runs.load(), 1) << "a header no mark names is not examined";
}

TEST(ArgumentHeaderCall, UnderALegacyRevisionNothingIsExpected) {
    CallRig rig;
    const auto legacy = neomcp::profile::builtin("2025-11-25");
    rig.session->receive(1, neomcp::testing::Peer::initialize_request(*legacy, 100).dump(), {});
    rig.recorder.wait(1);
    rig.next_id = 2;
    const json request = neomcp::testing::Peer::make_request(*legacy, neomcp::RequestId(7), "tools/call",
                                                             json{{"name", "spanner"}, {"arguments", json{{"region", "us"}}}});
    Outcome outcome = rig.send(request, {{"MCP-Protocol-Version", "2025-11-25"}});
    EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    // A header that contradicts the body is not looked at either.
    outcome = rig.send(request, {{"MCP-Protocol-Version", "2025-11-25"}, {"Mcp-Param-Region", "eu"}});
    EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    EXPECT_EQ(rig.runs.load(), 2);
}

TEST(ArgumentHeaderCall, OverStdioNoHeaderExists) {
    CallRig rig(neomcp::profile::Transport::stdio);
    const auto modern = neomcp::profile::builtin(modern_revision);
    const json request = neomcp::testing::Peer::make_request(*modern, neomcp::RequestId(7), "tools/call",
                                                             json{{"name", "spanner"}, {"arguments", json{{"region", "us"}}}});
    const Outcome outcome = rig.send(request, {});
    EXPECT_FALSE(outcome.failed()) << outcome.message.dump();
    EXPECT_EQ(rig.runs.load(), 1);
}

// A host that never checked the marks of its tools.
class UncheckedHost : public neomcp::ToolHost {
public:
    UncheckedHost() {
        definitions_.push_back(tool_of("bad", parsed(R"({"type":"object","properties":{"l":{"type":"array","items":{"type":"string","x-mcp-header":"H"}}}})")));
    }
    std::span<const neomcp::ToolDefinition> tools() const override { return definitions_; }
    void call(const neomcp::CallRequest&, neomcp::CallControl, neomcp::Completion done) override {
        ++runs;
        done(neomcp::ToolResult{});
    }
    std::atomic<int> runs{0};

private:
    std::vector<neomcp::ToolDefinition> definitions_;
};

TEST(ArgumentHeaderCall, AToolWhoseMarksAreInvalidIsNotCalled) {
    UncheckedHost host;
    Recorder recorder;
    neomcp::SessionOptions options;
    options.transport = neomcp::profile::Transport::streamable_http;
    auto made = neomcp::Session::create_tagged(options, host, [&](neomcp::Output output) { recorder(std::move(output)); });
    auto session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
    const auto modern = neomcp::profile::builtin(modern_revision);
    const json request = neomcp::testing::Peer::make_request(*modern, neomcp::RequestId(7), "tools/call", json{{"name", "bad"}});
    const Fields fields{{"MCP-Protocol-Version", modern_revision}, {"Mcp-Method", "tools/call"}, {"Mcp-Name", "bad"}};
    std::vector<neomcp::HeaderField> headers;
    for (const auto& [name, value] : fields) headers.push_back(neomcp::HeaderField{name, value});
    session->receive(1, request.dump(), headers);
    const auto outputs = recorder.wait(1);
    ASSERT_FALSE(outputs.empty());
    const json answer = json::parse(outputs[0].line);
    ASSERT_TRUE(answer.contains("error")) << answer.dump();
    EXPECT_EQ(answer.at("error").at("code"), -32603);
    EXPECT_NE(answer.at("error").at("message").get<std::string>().find("bad"), std::string::npos);
    EXPECT_EQ(host.runs.load(), 0);
}

}  // namespace
