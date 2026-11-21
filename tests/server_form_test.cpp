// Tests that the form of an elicitation is the form the revision defines: the keywords a member may have come from
// the types of the revision's profile, when a form is built, when a server registers it and when a call asks it. An
// annotation the revision's shape lacks (a default, say) is left out of the form; any other keyword it lacks refuses.
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace sft {

enum class Color { red, green, blue };

// A string with a default, which 2025-06-18 does not define.
struct Named {
    std::string name;
};
// Members every revision with forms defines.
struct Plain {
    std::string name;
    std::int32_t age;
    bool verified;
    Color color;
};
// A number and a boolean with defaults: the boolean has one in every revision, the number not in 2025-06-18.
struct Counted {
    std::int32_t count;
    bool verified;
};
struct Flagged {
    bool verified;
};
// A choice with titles written as oneOf, and a choice of several.
struct Email {
    std::string text;
};
// Read as the string it is written as.
inline void from_json(const neomcp::json& value, Email& email) { email.text = value.get<std::string>(); }
struct Titled {
    Email address;
};
struct Several {
    std::vector<Color> colors;
};
struct Nested {
    Plain inner;
};

}  // namespace sft

template <>
struct neomcp::Schema<sft::Email> {
    static neomcp::json make() {
        return neomcp::json::parse(R"({"type":"string","oneOf":[{"const":"a","title":"A"},{"const":"b","title":"B"}]})");
    }
};

NEOMCP_DESCRIBE_ENUM(sft::Color, red, green, blue)
NEOMCP_DESCRIBE(sft::Plain, name, "a name", age, "an age", verified, "whether", color, "a colour")
NEOMCP_DESCRIBE(sft::Titled, address, "which")
NEOMCP_DESCRIBE(sft::Several, colors, "some colours")
NEOMCP_DESCRIBE(sft::Nested, inner, "")

template <>
struct neomcp::Described<sft::Named> {
    static constexpr auto fields =
        neomcp::fields(neomcp::field(&sft::Named::name, "name", "Your name", neomcp::default_value("Ann")));
};
template <>
struct neomcp::Described<sft::Counted> {
    static constexpr auto fields = neomcp::fields(neomcp::field(&sft::Counted::count, "count", "", neomcp::default_value(3)),
                                                  neomcp::field(&sft::Counted::verified, "verified", "", neomcp::default_value(true)));
};
template <>
struct neomcp::Described<sft::Flagged> {
    static constexpr auto fields =
        neomcp::fields(neomcp::field(&sft::Flagged::verified, "verified", "", neomcp::default_value(true)));
};

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* older = "2025-06-18";  // a form without a default for a string, a titled choice or several
constexpr const char* newer = "2025-11-25";
constexpr const char* newest = "2026-07-28";
constexpr const char* without_forms[] = {"2024-11-05", "2025-03-26"};

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

template <class Form>
std::string refusal_in(const char* revision) {
    try {
        static_cast<void>(neomcp::form_schema_of<Form>(*builtin(revision)));
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return std::string();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

TEST(FormOfARevision, AStringWithADefaultKeepsItWhereTheRevisionDefinesItAndLosesItWhereNot) {
    // The profile of 2025-06-18 gives a string no `default`; the later ones do.
    EXPECT_FALSE(builtin(older)->type("StringSchema")->has_property("default"));
    EXPECT_TRUE(builtin(newer)->type("StringSchema")->has_property("default"));
    const json without = neomcp::form_schema_of<sft::Named>(*builtin(older));
    const json& name = without.at("properties").at("name");
    EXPECT_FALSE(name.contains("default")) << name.dump();
    EXPECT_EQ(name.at("type"), "string");
    EXPECT_EQ(name.at("description"), "Your name") << "the annotations the shape has stay";
    for (const char* revision : {newer, newest}) {
        SCOPED_TRACE(revision);
        const json schema = neomcp::form_schema_of<sft::Named>(*builtin(revision));
        EXPECT_EQ(schema.at("properties").at("name").at("default"), "Ann");
    }
    // The form without the default is the form with it, but for the default.
    json with = neomcp::form_schema_of<sft::Named>(*builtin(newer));
    with["properties"]["name"].erase("default");
    EXPECT_EQ(without, with);
}

TEST(FormOfARevision, ANumberWithADefaultFollowsItsRevisionToo) {
    const json older_form = neomcp::form_schema_of<sft::Counted>(*builtin(older));
    EXPECT_FALSE(older_form.at("properties").at("count").contains("default"));
    EXPECT_EQ(older_form.at("properties").at("verified").at("default"), true) << "a boolean has its default here";
    for (const char* revision : {newer, newest}) {
        SCOPED_TRACE(revision);
        EXPECT_EQ(neomcp::form_schema_of<sft::Counted>(*builtin(revision)).at("properties").at("count").at("default"), 3);
    }
    for (const char* revision : {older, newer, newest}) {
        SCOPED_TRACE(revision);
        EXPECT_EQ(neomcp::form_schema_of<sft::Flagged>(*builtin(revision)).at("properties").at("verified").at("default"),
                  true);
    }
}

TEST(FormOfARevision, ATitledChoiceAndAChoiceOfSeveralAreFormsOfTheLaterRevisionsOnly) {
    for (const std::string& refused : {refusal_in<sft::Titled>(older), refusal_in<sft::Several>(older)}) {
        EXPECT_FALSE(refused.empty());
    }
    EXPECT_TRUE(contains(refusal_in<sft::Titled>(older), "'oneOf'"));
    EXPECT_TRUE(contains(refusal_in<sft::Several>(older), "'colors'"));
    for (const char* revision : {newer, newest}) {
        SCOPED_TRACE(revision);
        EXPECT_TRUE(refusal_in<sft::Titled>(revision).empty());
        EXPECT_TRUE(refusal_in<sft::Several>(revision).empty());
    }
}

TEST(FormOfARevision, WhatEveryRevisionDefinesIsAFormInEach) {
    for (const char* revision : {older, newer, newest}) {
        SCOPED_TRACE(revision);
        EXPECT_TRUE(refusal_in<sft::Plain>(revision).empty());
        EXPECT_TRUE(contains(refusal_in<sft::Nested>(revision), "'inner'"));
    }
}

TEST(FormOfARevision, ARevisionWithoutFormsHasNone) {
    for (const char* revision : without_forms) {
        SCOPED_TRACE(revision);
        EXPECT_TRUE(contains(refusal_in<sft::Plain>(revision), "defines no form")) << refusal_in<sft::Plain>(revision);
    }
}

TEST(FormOfARevision, WithoutARevisionAFormIsOneThatSomeBuiltInRevisionDefines) {
    EXPECT_EQ(neomcp::form_schema_of<sft::Named>().at("properties").at("name").at("default"), "Ann");
    EXPECT_NO_THROW(static_cast<void>(neomcp::form_schema_of<sft::Several>()));
    try {
        static_cast<void>(neomcp::form_schema_of<sft::Nested>());
        ADD_FAILURE() << "a nested form was built";
    } catch (const std::invalid_argument& error) {
        EXPECT_TRUE(contains(error.what(), "'inner'")) << error.what();
    }
}

TEST(FormOfARevision, ARevisionsKeywordsAreThoseOfItsProfileAndNoOthers) {
    const auto problem = [](const char* revision, const json& member) {
        const json schema{{"type", "object"}, {"properties", {{"m", member}}}};
        return neomcp::detail::form_schema_problem(schema, *builtin(revision));
    };
    const auto written = [](const char* revision, const json& member) {
        const json schema{{"type", "object"}, {"properties", {{"m", member}}}};
        auto form = neomcp::detail::form_schema_in(schema, *builtin(revision));
        return std::get<json>(form).at("properties").at("m");
    };
    // An annotation the shape lacks is left out, not refused.
    const json with_default{{"type", "string"}, {"default", "x"}};
    for (const char* revision : {older, newer, newest}) {
        SCOPED_TRACE(revision);
        EXPECT_FALSE(problem(revision, with_default));
    }
    EXPECT_EQ(written(older, with_default), (json{{"type", "string"}}));
    EXPECT_EQ(written(newer, with_default), with_default);
    // The annotations of the meta-data vocabulary that no shape has are left out in every revision; the ones it has stay.
    const json annotated{{"type", "boolean"}, {"title", "T"}, {"deprecated", true}, {"readOnly", true},
                         {"writeOnly", false}, {"examples", {true}}};
    for (const char* revision : {older, newer, newest}) {
        SCOPED_TRACE(revision);
        EXPECT_EQ(written(revision, annotated), (json{{"type", "boolean"}, {"title", "T"}}));
    }
    // Any other keyword the shape lacks is refused by name, whatever the revision: it changes the instances that are valid.
    const json with_pattern{{"type", "string"}, {"pattern", "^x$"}};
    for (const char* revision : {older, newer, newest}) {
        SCOPED_TRACE(revision);
        const auto refused = problem(revision, with_pattern);
        ASSERT_TRUE(refused);
        EXPECT_TRUE(contains(*refused, "'pattern'")) << *refused;
    }
    const json one_of_with_default = json::parse(R"({"type":"string","oneOf":[{"const":"a","title":"A"}],"default":"a"})");
    const auto no_one_of = problem(older, one_of_with_default);
    ASSERT_TRUE(no_one_of);
    EXPECT_TRUE(contains(*no_one_of, "'oneOf'")) << *no_one_of;
    EXPECT_FALSE(problem(newer, one_of_with_default));
    // A choice with names fits where the profile has enumNames (every revision with forms).
    const json named_choice{{"type", "string"}, {"enum", {"a", "b"}}, {"enumNames", {"A", "B"}}};
    for (const char* revision : {older, newer, newest}) {
        SCOPED_TRACE(revision);
        EXPECT_FALSE(problem(revision, named_choice));
    }
    // A choice with a default: the choice of 2025-06-18 has no default either, and is written without it.
    const json choice_with_default{{"type", "string"}, {"enum", {"a", "b"}}, {"default", "a"}};
    EXPECT_EQ(written(older, choice_with_default), (json{{"type", "string"}, {"enum", {"a", "b"}}}));
    EXPECT_EQ(written(newer, choice_with_default), choice_with_default);
    // A shape that requires a keyword: a list without items.
    const auto lacking = problem(newer, json{{"type", "array"}, {"minItems", 1}});
    ASSERT_TRUE(lacking);
    EXPECT_TRUE(contains(*lacking, "'items'")) << *lacking;
}

// ---------------------------------------------------------------------------------------------------------------
// A server registers a form that some revision it serves defines, and a call asks it only of a revision that does.

neomcp::ServerOptions serving(std::vector<profile::ProfilePtr> revisions) {
    neomcp::ServerOptions options{.name = "form-server", .version = "1.0.0"};
    options.profiles = std::move(revisions);
    return options;
}

const auto body = [](const neomcp::Answer<sft::Named>& named) {
    return std::string(named.accepted() ? "named " + named->name : named.asked() ? "not accepted" : "not asked");
};
const auto question_text = [] { return std::string("Your name?"); };

// A form whose choice is written as oneOf: 2025-06-18 has no such shape, and the keyword is not an annotation.
const auto titled_body = [](const neomcp::Answer<sft::Titled>& titled) {
    return std::string(titled.accepted() ? "chose" : titled.asked() ? "not accepted" : "not asked");
};

TEST(FormOfARevision, AServerRegistersAFormOnlyIfARevisionItServesDefinesIt) {
    neomcp::Server only_older(serving({builtin(older)}));
    try {
        only_older.tool("titled", "Asks a choice", titled_body, neomcp::elicit<sft::Titled>("address", question_text));
        ADD_FAILURE() << "a form that no served revision defines was registered";
    } catch (const std::invalid_argument& error) {
        const std::string text = error.what();
        EXPECT_TRUE(contains(text, "tool 'titled'")) << text;
        EXPECT_TRUE(contains(text, "'address'")) << text;
        EXPECT_TRUE(contains(text, "'oneOf'")) << text;
    }
    // A default the revision lacks is left out, so the form is a form of it.
    EXPECT_NO_THROW(only_older.tool("named", "Asks a name", body, neomcp::elicit<sft::Named>("name", question_text)));
    neomcp::Server both(serving({builtin(older), builtin(newer)}));
    EXPECT_NO_THROW(both.tool("titled", "Asks a choice", titled_body, neomcp::elicit<sft::Titled>("address", question_text)));
    neomcp::Server by_default(serving({}));
    EXPECT_NO_THROW(
        by_default.tool("titled", "Asks a choice", titled_body, neomcp::elicit<sft::Titled>("address", question_text)));
    neomcp::Server no_forms(serving({builtin(without_forms[1])}));
    EXPECT_NO_THROW(no_forms.tool("plain", "Asks a name", [](const neomcp::Answer<sft::Plain>&) { return std::string(); },
                                  neomcp::elicit<sft::Plain>("plain", question_text)))
        << "a form checked against the built-in revisions when the server serves none that has forms";
    EXPECT_THROW(no_forms.tool("nested", "Cannot be built", [](const neomcp::Answer<sft::Nested>&) { return std::string(); },
                               neomcp::elicit<sft::Nested>("nested", question_text)),
                 std::invalid_argument);
}

// A client of a revision that sends requests, with the capability to take forms.
struct Client {
    Client(neomcp::Server& server, const char* revision) : peer(server), profile(builtin(revision)) {
        const auto opened = peer.request(
            Peer::initialize_request(*profile, neomcp::RequestId("init"), json{{"elicitation", json::object()}}), patience);
        EXPECT_TRUE(opened && opened->contains("result"));
        peer.notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    }
    void call(int id, const std::string& tool) {
        peer.send(neomcp::serialize(
            json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}}}}));
    }
    Peer peer;
    profile::ProfilePtr profile;
};

std::string text_of(const json& answer) { return answer.at("result").at("content").at(0).at("text").get<std::string>(); }

// Runs a call of `tool` for a client of `revision` that accepts the form it is asked with a name. Returns the schema of
// the form it was asked, or nothing when none was; `done` is the answer to the call. A client that is not to be asked
// waits for the answer to the call and then finds no request; one that is waits for the request.
std::optional<json> ask_and_answer(neomcp::Server& server, const char* revision, const std::string& tool, bool asked,
                                   json& done) {
    Client client(server, revision);
    client.call(1, tool);
    if (!asked) {
        const auto ended = client.peer.await_response(neomcp::RequestId(1), patience);
        done = ended ? *ended : json();
        EXPECT_FALSE(client.peer.next_request(0ms)) << "something was asked";
        return std::nullopt;
    }
    const auto request = client.peer.next_request(patience);
    if (!request) {
        ADD_FAILURE() << "nothing was asked";
        return std::nullopt;
    }
    client.peer.send(neomcp::serialize(json{{"jsonrpc", "2.0"},
                                            {"id", request->at("id")},
                                            {"result", {{"action", "accept"}, {"content", {{"name", "Bo"}}}}}}));
    const auto ended = client.peer.await_response(neomcp::RequestId(1), patience);
    done = ended ? *ended : json();
    return request->at("params").at("requestedSchema");
}

TEST(FormOfARevision, ACallAsksTheFormOfItsRevisionWithoutTheAnnotationsItsShapeLacks) {
    neomcp::Server server(serving({builtin(older), builtin(newer)}));
    server.tool("named", "Asks a name", body, neomcp::elicit<sft::Named>("name", question_text));
    json done;
    const auto newer_form = ask_and_answer(server, newer, "named", true, done);
    ASSERT_TRUE(newer_form);
    EXPECT_EQ(newer_form->at("properties").at("name").at("default"), "Ann");
    EXPECT_EQ(text_of(done), "named Bo");
    const auto older_form = ask_and_answer(server, older, "named", true, done);
    ASSERT_TRUE(older_form) << done.dump();
    EXPECT_FALSE(older_form->at("properties").at("name").contains("default")) << older_form->dump();
    EXPECT_EQ(older_form->at("properties").at("name").at("description"), "Your name");
    EXPECT_EQ(text_of(done), "named Bo");
}

TEST(FormOfARevision, ACallEndsForARevisionThatDoesNotDefineTheFormsShape) {
    neomcp::Server server(serving({builtin(older), builtin(newer)}));
    server.tool("titled", "Asks a choice", titled_body, neomcp::elicit<sft::Titled>("address", question_text));
    json done;
    EXPECT_TRUE(ask_and_answer(server, newer, "titled", true, done)) << "a revision that defines the shape is asked";
    EXPECT_FALSE(ask_and_answer(server, older, "titled", false, done)) << "nothing was asked";
    ASSERT_TRUE(done.contains("result")) << done.dump();
    EXPECT_TRUE(done.at("result").at("isError").get<bool>()) << done.dump();
}

TEST(FormOfARevision, AnOptionalFormThatTheRevisionDoesNotDefineIsNotAsked) {
    neomcp::Server server(serving({builtin(older), builtin(newer)}));
    server.tool("titled", "Asks a choice if it can", titled_body,
                neomcp::elicit<sft::Titled>("address", question_text).optional());
    json done;
    EXPECT_FALSE(ask_and_answer(server, older, "titled", false, done));
    ASSERT_TRUE(done.contains("result")) << done.dump();
    EXPECT_EQ(text_of(done), "not asked");
}

}  // namespace
