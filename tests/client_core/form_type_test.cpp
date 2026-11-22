// The client's check of the restricted form of an elicitation looks at each property's `type` as well as at its member
// names: the value must be one the profile lists for the alternative whose members the property has. The values come
// from the profile's data (Type::type_values), so a revision that lacks a value refuses it, and a profile that lists
// none for an alternative leaves the decision to the members.
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/client/mechanism.h"
#include "neomcp/json.h"
#include "neomcp/profile.h"

namespace {

using neomcp::json;

constexpr std::string_view first_form_revision = "2025-06-18";
constexpr std::string_view enum_revision = "2025-11-25";
constexpr std::string_view per_request_revision = "2026-07-28";

json J(const char* text) { return json::parse(text); }

// A form whose only property is `property`.
json form_of(const json& property) {
    json properties = json::object();
    properties["p"] = property;
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    return schema;
}

// What the client's check says of the form under the profile, through the half of the mechanism the profile selects.
bool refused(const neomcp::profile::ProfilePtr& profile, const json& property) {
    const auto half =
        neomcp::builtin_client_mechanisms().make_input(profile->mechanism(neomcp::profile::Slot::server_input));
    return half->form_problem(form_of(property), *profile).has_value();
}

neomcp::profile::ProfilePtr loaded(const json& document) {
    auto made = neomcp::profile::load(document.dump());
    EXPECT_TRUE(std::holds_alternative<neomcp::profile::ProfilePtr>(made));
    return std::holds_alternative<neomcp::profile::ProfilePtr>(made) ? std::get<neomcp::profile::ProfilePtr>(made)
                                                                    : nullptr;
}

json document_of(std::string_view revision) { return json::parse(neomcp::profile::builtin_text(revision)); }

struct Case {
    const char* property;
    std::vector<std::string_view> revisions;  // the builtin revisions whose form has this property
};

const std::vector<std::string_view> every_form_revision{first_form_revision, enum_revision, per_request_revision};
const std::vector<std::string_view> enum_revisions{enum_revision, per_request_revision};

}  // namespace

TEST(ClientFormType, EachPrimitiveTypeIsAccepted) {
    const std::vector<Case> cases{
        {R"({"type":"string"})", every_form_revision},
        {R"({"type":"string","minLength":1,"maxLength":5,"format":"email"})", every_form_revision},
        {R"({"type":"number","minimum":0,"maximum":1})", every_form_revision},
        {R"({"type":"integer","minimum":0})", every_form_revision},
        {R"({"type":"boolean"})", every_form_revision},
        {R"({"type":"string","enum":["a","b"]})", every_form_revision},
        {R"({"type":"string","enum":["a","b"],"enumNames":["A","B"]})", every_form_revision},
        {R"({"type":"string","oneOf":[{"const":"a","title":"A"}]})", enum_revisions},
        {R"({"type":"array","items":{"type":"string","enum":["a"]}})", enum_revisions},
        {R"({"type":"array","items":{"anyOf":[{"const":"a","title":"A"}]}})", enum_revisions},
    };
    for (const Case& one : cases) {
        for (const std::string_view revision : one.revisions) {
            SCOPED_TRACE(std::string(revision) + " " + one.property);
            EXPECT_FALSE(refused(neomcp::profile::builtin(revision), J(one.property)));
        }
    }
}

TEST(ClientFormType, ATypeTheProfileDoesNotListIsRefused) {
    for (const std::string_view revision : every_form_revision) {
        SCOPED_TRACE(std::string(revision));
        const auto profile = neomcp::profile::builtin(revision);
        // A name no revision defines, and a name of JSON that a form has no shape for.
        EXPECT_TRUE(refused(profile, J(R"({"type":"decimal"})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"null"})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"object"})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":""})")));
        // The names are compared as they are written.
        EXPECT_TRUE(refused(profile, J(R"({"type":"String"})")));
        // Not a name at all.
        EXPECT_TRUE(refused(profile, J(R"({"type":1})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":null})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":["string"]})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":{"x":"string"}})")));
    }
}

TEST(ClientFormType, ATypeThatIsNotTheOneOfTheShapeWhoseMembersThePropertyHasIsRefused) {
    for (const std::string_view revision : every_form_revision) {
        SCOPED_TRACE(std::string(revision));
        const auto profile = neomcp::profile::builtin(revision);
        // The members of a string with the type of a boolean, of a number or of an integer, and the other way round.
        EXPECT_TRUE(refused(profile, J(R"({"type":"boolean","minLength":1})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"number","minLength":1})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"integer","maxLength":1})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"string","minimum":1})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"boolean","minimum":1})")));
        // The members of a choice with a type that is not a string's.
        EXPECT_TRUE(refused(profile, J(R"({"type":"number","enum":["a"]})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"boolean","enum":["a"]})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","enum":["a"]})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","minLength":1})")));
    }
    // The members of a choice of several, with the type of a string.
    EXPECT_TRUE(refused(neomcp::profile::builtin(enum_revision),
                        J(R"({"type":"string","items":{"type":"string","enum":["a"]}})")));
}

TEST(ClientFormType, ATypeOnlyAnotherRevisionListsIsRefusedWhereTheProfileLacksIt) {
    // The values of a shape come from the revision's profile. A profile of the same revision that lists one value
    // less for a shape refuses that value, and the revision as it is accepts it.
    for (const std::string_view revision : every_form_revision) {
        SCOPED_TRACE(std::string(revision));
        const auto builtin = neomcp::profile::builtin(revision);
        EXPECT_FALSE(refused(builtin, J(R"({"type":"integer"})")));
        EXPECT_FALSE(refused(builtin, J(R"({"type":"number"})")));
        json document = document_of(revision);
        json& values = document["wire"]["types"]["NumberSchema"]["type_values"];
        ASSERT_EQ(values, J(R"(["integer","number"])"));
        values = J(R"(["number"])");
        const auto without_integer = loaded(document);
        ASSERT_NE(without_integer, nullptr);
        EXPECT_TRUE(refused(without_integer, J(R"({"type":"integer"})")));
        EXPECT_FALSE(refused(without_integer, J(R"({"type":"number"})")));
        EXPECT_TRUE(refused(without_integer, J(R"({"type":"integer","minimum":0})")));
    }
}

TEST(ClientFormType, ATypeThatAnotherRevisionOnlyHasIsRefusedByTheRevisionWithoutTheShape) {
    // `array` is a type of the revisions that define a choice of several. Where the revision has no such shape, a
    // property of that type that has nothing but the members every shape has (a type, a title, a description) fits
    // the members of a boolean, and only its type keeps it out of the form.
    const auto first = neomcp::profile::builtin(first_form_revision);
    EXPECT_TRUE(refused(first, J(R"({"type":"array"})")));
    EXPECT_TRUE(refused(first, J(R"({"type":"array","description":"d"})")));
    EXPECT_TRUE(refused(first, J(R"({"type":"array","title":"t","default":true})")));
    EXPECT_FALSE(refused(first, J(R"({"type":"boolean","description":"d"})")));
    // The choice itself is refused there by its members too, and accepted where the revision defines it.
    const json choice = J(R"({"type":"array","items":{"type":"string","enum":["a"]},"minItems":1})");
    EXPECT_TRUE(refused(first, choice));
    for (const std::string_view revision : enum_revisions) {
        SCOPED_TRACE(std::string(revision));
        EXPECT_FALSE(refused(neomcp::profile::builtin(revision), choice));
    }
}

TEST(ClientFormType, TheObjectsAPropertyHoldsFitTheirTypesAsWell) {
    // The items of a choice of several are an object of a type of the profile: the members of a string with a list of
    // its values, or a list of titled values.
    for (const std::string_view revision : enum_revisions) {
        SCOPED_TRACE(std::string(revision));
        const auto profile = neomcp::profile::builtin(revision);
        EXPECT_FALSE(refused(profile, J(R"({"type":"array","items":{"type":"string","enum":["a"]}})")));
        EXPECT_FALSE(refused(profile, J(R"({"type":"array","items":{"anyOf":[{"const":"a","title":"A"}]}})")));
        // A type that is not the items' own, a member they lack, a member they need but do not have, and not an
        // object at all.
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":{"type":"number","enum":["a"]}})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":{"type":"decimal","enum":["a"]}})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":{"type":"string","enum":["a"],"minLength":1}})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":{"type":"string"}})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":{"anyOf":[],"type":"string"}})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":{}})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":"string"})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":[{"type":"string","enum":["a"]}]})")));
        EXPECT_TRUE(refused(profile, J(R"({"type":"array","items":null})")));
    }
}

TEST(ClientFormType, AShapeWhoseProfileListsNoValuesLeavesTheDecisionToItsMembers) {
    for (const std::string_view revision : every_form_revision) {
        SCOPED_TRACE(std::string(revision));
        json document = document_of(revision);
        document["wire"]["types"]["StringSchema"].erase("type_values");
        const auto unlisted = loaded(document);
        ASSERT_NE(unlisted, nullptr);
        EXPECT_TRUE(unlisted->type("StringSchema")->type_values.empty());
        // Its members are a string's and its type is not listed anywhere: the members decide.
        EXPECT_FALSE(refused(unlisted, J(R"({"type":"string","minLength":1})")));
        EXPECT_FALSE(refused(unlisted, J(R"({"type":"boolean","minLength":1})")));
        // With no value to compare, a type that no revision defines fits it too; the other shapes still list theirs.
        EXPECT_FALSE(refused(unlisted, J(R"({"type":"decimal"})")));
        EXPECT_TRUE(refused(unlisted, J(R"({"type":"string","minimum":1})")));
        EXPECT_TRUE(refused(unlisted, J(R"({"type":"decimal","minimum":1})")));
        EXPECT_TRUE(refused(unlisted, J(R"({"type":"string","enum":["a"],"minLength":1})")));
    }
}

TEST(ClientFormType, EveryPropertyIsCheckedNotOnlyTheFirst) {
    const auto profile = neomcp::profile::builtin(enum_revision);
    const auto half =
        neomcp::builtin_client_mechanisms().make_input(profile->mechanism(neomcp::profile::Slot::server_input));
    const json fitting = J(R"({"type":"object","properties":{"a":{"type":"string"},"b":{"type":"boolean"}}})");
    EXPECT_FALSE(half->form_problem(fitting, *profile));
    const json second = J(R"({"type":"object","properties":{"a":{"type":"string"},"b":{"type":"decimal"}}})");
    const auto problem = half->form_problem(second, *profile);
    ASSERT_TRUE(problem);
    EXPECT_NE(problem->find("property 2"), std::string::npos) << *problem;
    // The reason names the position, never a text of the server's.
    const json named = J(R"({"type":"object","properties":{"secret-name":{"type":"decimal"}}})");
    const auto of_named = half->form_problem(named, *profile);
    ASSERT_TRUE(of_named);
    EXPECT_EQ(of_named->find("secret-name"), std::string::npos) << *of_named;
}
