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
        // The range of an integer.
        {"an integer without bounds", R"({"type":"object","properties":{"p":{"type":"integer","x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "bounded"},
        {"an integer without an upper bound",
         R"({"type":"object","properties":{"p":{"type":"integer","minimum":0,"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "bounded"},
        {"an integer without a lower bound",
         R"({"type":"object","properties":{"p":{"type":"integer","maximum":0,"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "bounded"},
        {"an integer whose maximum is past the safe range",
         R"({"type":"object","properties":{"p":{"type":"integer","minimum":0,"maximum":9007199254740992,"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "maximum"},
        {"an integer whose minimum is past the safe range",
         R"({"type":"object","properties":{"p":{"type":"integer","minimum":-9007199254740992,"maximum":0,"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "minimum"},
        {"an integer whose exclusive bound is past the safe range",
         R"({"type":"object","properties":{"p":{"type":"integer","minimum":0,"exclusiveMaximum":1e300,"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "exclusiveMaximum"},
        {"an integer constant past the safe range",
         R"({"type":"object","properties":{"p":{"type":"integer","const":9007199254740992,"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "const"},
        {"an integer list with a value past the safe range",
         R"({"type":"object","properties":{"p":{"type":"integer","enum":[1,9007199254740992],"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "enum"},
        {"an integer list that is empty",
         R"({"type":"object","properties":{"p":{"type":"integer","enum":[],"x-mcp-header":"H"}}})",
         "/properties/p/x-mcp-header", "enum"},
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
        {"an unbounded integer", one_property(R"({"type":"integer","x-mcp-header":"H"})")},
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

TEST(DescribedHeaderMarks, MarksThatBreakARuleRefuseTheTool) {
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
    EXPECT_NE(wide.find("wide"), std::string::npos) << wide;
    EXPECT_NE(wide.find("/properties/count"), std::string::npos) << wide;
    EXPECT_NE(wide.find("bounded"), std::string::npos) << wide;
    const std::string bad = message_of(
        [&] { server.tool<described::BadName>("bad", "d", [](const described::BadName&) { return std::string("done"); }); });
    EXPECT_NE(bad.find("token"), std::string::npos) << bad;
    const std::string twice = message_of(
        [&] { server.tool<described::Twice>("twice", "d", [](const described::Twice&) { return std::string("done"); }); });
    EXPECT_NE(twice.find("without regard to case"), std::string::npos) << twice;
    EXPECT_TRUE(server.tools().empty());
}

}  // namespace
