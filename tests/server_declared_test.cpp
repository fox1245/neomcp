// Tests of declared inputs, through Server and the in-memory peer: the form built from a described struct, the rounds
// under the revision whose result asks and under one that sends requests, what a handler gets, and the answers that
// are refused. Every wait is on an event a test controls; the bound on a wait only limits how long a broken build
// takes to fail.
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

#include <asio/awaitable.hpp>
#include <gtest/gtest.h>

#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

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

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* legacy_revision = "2025-11-25";

json J(const char* text) { return json::parse(text); }

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

json all_capabilities() {
    return J(R"({"elicitation":{"form":{},"url":{}},"sampling":{"tools":{},"context":{}},"roots":{}})");
}

neomcp::ServerOptions options_with_key() {
    neomcp::ServerOptions options{.name = "declared-server", .version = "1.0.0"};
    options.request_state_keys = {std::string(32, 'd')};
    return options;
}

neomcp::SamplingRequest sampling_of(std::string prompt) {
    neomcp::SamplingMessage message;
    neomcp::TextContent text;
    text.text = std::move(prompt);
    message.content.emplace_back(std::move(text));
    neomcp::SamplingRequest request;
    request.messages.push_back(std::move(message));
    request.max_tokens = 50;
    return request;
}

json accept(json content) {
    json answer = json::object();
    answer["action"] = "accept";
    answer["content"] = std::move(content);
    return answer;
}

json sampled(const char* text) {
    json answer = J(R"({"role":"assistant","content":{"type":"text","text":""},"model":"m1","stopReason":"endTurn"})");
    answer["content"]["text"] = text;
    return answer;
}

json roots_answer() { return J(R"({"roots":[{"uri":"file:///a","name":"A"},{"uri":"file:///b"}]})"); }

json one(const char* input_key, json answer) {
    json out = json::object();
    out[input_key] = std::move(answer);
    return out;
}

std::string text_of(const json& answer) { return answer.at("result").at("content").at(0).at("text").get<std::string>(); }

bool asks(const json& answer) {
    return answer.contains("result") && answer.at("result").value("resultType", std::string()) == "input_required";
}

std::set<std::string> asked_keys(const json& answer) {
    std::set<std::string> keys;
    for (const auto& item : answer.at("result").at("inputRequests").items()) keys.insert(item.key());
    return keys;
}

json tool_params(const std::string& tool, json arguments = json::object()) {
    json params = json::object();
    params["name"] = tool;
    params["arguments"] = std::move(arguments);
    return params;
}

// A client of the revision whose result asks: every request names its revision and the client's capabilities.
struct ModernClient {
    explicit ModernClient(neomcp::Server& server, json capabilities = all_capabilities())
        : peer(server), profile(builtin(modern_revision)), caps(std::move(capabilities)) {}

    json ask(const std::string& method, json params) {
        params["_meta"] = Peer::request_meta(*profile, caps);
        json message = json::object();
        message["jsonrpc"] = "2.0";
        message["id"] = next_id++;
        message["method"] = method;
        message["params"] = std::move(params);
        const auto answer = peer.request(message, patience);
        if (!answer) throw std::runtime_error("no answer within the patience");
        return *answer;
    }
    // The call again, with answers and the state of `earlier` (when it has one).
    json retry(const std::string& method, json params, const json& earlier, json answers) {
        params["inputResponses"] = std::move(answers);
        if (earlier.contains("result") && earlier.at("result").contains("requestState")) {
            params["requestState"] = earlier.at("result").at("requestState");
        }
        return ask(method, std::move(params));
    }

    Peer peer;
    profile::ProfilePtr profile;
    json caps;
    int next_id = 1;
};

// A client of a revision that sends requests.
struct LegacyClient {
    explicit LegacyClient(neomcp::Server& server, json capabilities = all_capabilities())
        : peer(server), profile(builtin(legacy_revision)) {
        const auto opened =
            peer.request(Peer::initialize_request(*profile, neomcp::RequestId("init"), std::move(capabilities)), patience);
        EXPECT_TRUE(opened && opened->contains("result"));
        peer.notify(J(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    void send_call(int call_id, json params) {
        json message = json::object();
        message["jsonrpc"] = "2.0";
        message["id"] = call_id;
        message["method"] = "tools/call";
        message["params"] = std::move(params);
        peer.send(neomcp::serialize(message));
    }
    void answer(const json& request, json result) {
        json response = json::object();
        response["jsonrpc"] = "2.0";
        response["id"] = request.at("id");
        response["result"] = std::move(result);
        peer.send(neomcp::serialize(response));
    }

    Peer peer;
    profile::ProfilePtr profile;
};

// What the handlers did: how often each body ran, and what happened only once.
struct Effects {
    std::atomic<int> bodies{0};
    std::atomic<int> deleted{0};
    std::atomic<int> questions{0};
};

void register_delete(neomcp::Server& server, Effects& effects) {
    server.tool<sdt::Folder>(
        "delete_folder", "Delete a folder",
        [&effects](const sdt::Folder& a, const neomcp::Answer<sdt::Confirm>& confirm) {
            ++effects.bodies;
            if (!confirm.accepted() || !confirm->ok) return std::string("kept");
            ++effects.deleted;
            return "deleted " + a.path;
        },
        neomcp::elicit<sdt::Confirm>("confirm", [&effects](const sdt::Folder& a) {
            ++effects.questions;  // counts the entries; a real question function has no effects
            return "Delete " + a.path + "?";
        }));
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

// ---------------------------------------------------------------------------------------------------------------
// The declarations

TEST(Declarations, AnAnswerSaysWhatBecameOfItsInput) {
    const neomcp::Answer<sdt::Confirm> accepted(sdt::Confirm{true});
    EXPECT_TRUE(accepted.accepted());
    EXPECT_TRUE(accepted.asked());
    EXPECT_TRUE(accepted->ok);
    EXPECT_TRUE((*accepted).ok);
    for (const auto status : {neomcp::AnswerStatus::declined, neomcp::AnswerStatus::cancelled, neomcp::AnswerStatus::not_asked}) {
        const neomcp::Answer<sdt::Confirm> other(status);
        EXPECT_EQ(other.status(), status);
        EXPECT_FALSE(other.accepted());
        EXPECT_EQ(other.asked(), status != neomcp::AnswerStatus::not_asked);
        EXPECT_THROW(static_cast<void>(other.value()), std::logic_error);
    }
    EXPECT_EQ(neomcp::Answer<sdt::Confirm>().status(), neomcp::AnswerStatus::not_asked);
    EXPECT_THROW(static_cast<void>(neomcp::Answer<sdt::Confirm>(neomcp::AnswerStatus::accepted)), std::invalid_argument);
}

TEST(Declarations, AQuestionAsksOrSaysWhatTheHandlerGets) {
    const neomcp::Question<sdt::Confirm> literal = "Delete it?";
    ASSERT_TRUE(literal.asks());
    EXPECT_EQ(literal.payload(), "Delete it?");
    const neomcp::Question<sdt::Confirm> none = neomcp::no_question(sdt::Confirm{false});
    ASSERT_FALSE(none.asks());
    EXPECT_TRUE(none.answer().accepted());
    EXPECT_FALSE(none.answer()->ok);
    const neomcp::Question<neomcp::RootsAnswer> skipped = neomcp::no_question<neomcp::RootsAnswer>();
    EXPECT_EQ(skipped.answer().status(), neomcp::AnswerStatus::not_asked);
    const neomcp::Question<neomcp::SamplingAnswer> sampling = sampling_of("Summarise");
    ASSERT_TRUE(sampling.asks());
    EXPECT_EQ(sampling.payload().max_tokens, 50U);

    const auto form = neomcp::elicit<sdt::Confirm>("confirm", [] { return std::string("?"); });
    EXPECT_EQ(form.input_key(), "confirm");
    EXPECT_FALSE(form.is_optional());
    EXPECT_TRUE(form.optional().is_optional());
    EXPECT_FALSE(form.is_optional()) << "optional() makes a copy";
    EXPECT_TRUE(neomcp::roots("roots").optional().is_optional());
    EXPECT_EQ(neomcp::sample("s", [] { return sampling_of("x"); }).input_key(), "s");
}

TEST(Declared, ARegistrationWithInputsThatCannotWorkIsRefusedByName) {
    neomcp::Server server(options_with_key());
    const auto body = [](const neomcp::Answer<sdt::Confirm>&, const neomcp::Answer<sdt::Confirm>&) { return std::string(); };
    const auto question = [] { return std::string("?"); };
    try {
        server.tool("twice", "Two inputs with one key", body, neomcp::elicit<sdt::Confirm>("same", question),
                    neomcp::elicit<sdt::Confirm>("same", question));
        ADD_FAILURE() << "two inputs with one key were registered";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("'same'"), std::string::npos) << error.what();
        EXPECT_NE(std::string(error.what()).find("tool 'twice'"), std::string::npos) << error.what();
    }
    EXPECT_THROW(server.tool("nameless", "An input without a key",
                             [](const neomcp::Answer<sdt::Confirm>&) { return std::string(); },
                             neomcp::elicit<sdt::Confirm>("", question)),
                 std::invalid_argument);
    try {
        server.tool("nested", "A form that cannot be built", [](const neomcp::Answer<sdt::Nested>&) { return std::string(); },
                    neomcp::elicit<sdt::Nested>("form", question));
        ADD_FAILURE() << "a nested form was registered";
    } catch (const std::invalid_argument& error) {
        const std::string text = error.what();
        EXPECT_NE(text.find("tool 'nested'"), std::string::npos) << text;
        EXPECT_NE(text.find("'form'"), std::string::npos) << text;
        EXPECT_NE(text.find("'inner'"), std::string::npos) << text;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Where the result asks

TEST(Declared, TheBodyRunsOnceWhenTheAnswerIsThere) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_delete(server, effects);
    ModernClient client(server);
    const json params = tool_params("delete_folder", J(R"({"path":"/tmp/x"})"));
    const json first = client.ask("tools/call", params);
    ASSERT_TRUE(asks(first)) << first.dump();
    const json& request = first.at("result").at("inputRequests").at("confirm");
    EXPECT_EQ(request.at("method"), "elicitation/create");
    EXPECT_EQ(request.at("params").at("message"), "Delete /tmp/x?");
    EXPECT_EQ(request.at("params").at("requestedSchema"), neomcp::form_schema_of<sdt::Confirm>());
    EXPECT_EQ(effects.bodies.load(), 0) << "the body does not run in a round that asks";

    const json second = client.retry("tools/call", params, first, one("confirm", accept(J(R"({"ok":true})"))));
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_EQ(text_of(second), "deleted /tmp/x");
    EXPECT_EQ(effects.bodies.load(), 1);
    EXPECT_EQ(effects.deleted.load(), 1);
    EXPECT_EQ(effects.questions.load(), 2) << "the question runs on every entry";

    // A declined form reaches the body as declined.
    const json again = client.ask("tools/call", params);
    const json declined = client.retry("tools/call", params, again, one("confirm", J(R"({"action":"decline"})")));
    EXPECT_EQ(text_of(declined), "kept");
    EXPECT_EQ(effects.bodies.load(), 2);
    EXPECT_EQ(effects.deleted.load(), 1);
}

TEST(Declared, AnAnswerToAQuestionThatChangedIsAskedAgain) {
    std::atomic<int> version{1};
    std::atomic<int> bodies{0};
    neomcp::Server server(options_with_key());
    server.tool(
        "versioned", "Its first question changes when the test says so",
        [&bodies](const neomcp::Answer<sdt::Note>& first, const neomcp::Answer<sdt::Note>& second) {
            ++bodies;
            return first->text + "+" + second->text;
        },
        neomcp::elicit<sdt::Note>("first", [&version] { return "First, version " + std::to_string(version.load()); }),
        neomcp::elicit<sdt::Note>("second", [] { return std::string("Second"); }));
    ModernClient client(server);
    const json params = tool_params("versioned");

    // An answer of this round to a question that changed since it was asked.
    const json asked = client.ask("tools/call", params);
    ASSERT_EQ(asked_keys(asked), (std::set<std::string>{"first", "second"}));
    version = 2;
    json both = one("first", accept(J(R"({"text":"a"})")));
    both["second"] = accept(J(R"({"text":"b"})"));
    const json stale = client.retry("tools/call", params, asked, both);
    ASSERT_TRUE(asks(stale)) << stale.dump();
    EXPECT_EQ(asked_keys(stale), std::set<std::string>{"first"});
    EXPECT_EQ(stale.at("result").at("inputRequests").at("first").at("params").at("message"), "First, version 2");
    EXPECT_EQ(bodies.load(), 0);

}

TEST(Declared, ContentThatDoesNotFitTheFormIsInvalidParams) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_delete(server, effects);
    std::atomic<int> rated{0};
    server.tool(
        "rate", "Rate between one and five stars",
        [&rated](const neomcp::Answer<sdt::Rating>& rating) {
            ++rated;
            return std::to_string(rating->stars);
        },
        neomcp::elicit<sdt::Rating>("rating", [] { return std::string("How good?"); }));
    ModernClient client(server);
    const json params = tool_params("delete_folder", J(R"({"path":"/srv"})"));
    // Content that its schema refuses, and an answer of another kind (which the session refuses, knowing from the
    // state what was asked).
    for (const json& wrong :
         {accept(J(R"({"ok":"yes"})")), accept(json::object()), J(R"({"action":"accept"})"), sampled("yes")}) {
        const json first = client.ask("tools/call", params);
        const json answer = client.retry("tools/call", params, first, one("confirm", wrong));
        ASSERT_TRUE(answer.contains("error")) << wrong.dump() << " " << answer.dump();
        EXPECT_EQ(answer.at("error").at("code"), neomcp::error_code::invalid_params) << wrong.dump();
    }
    // Content that decodes as the struct but breaks a bound of its schema.
    const json rate = tool_params("rate");
    const json asked = client.ask("tools/call", rate);
    const json out_of_bounds = client.retry("tools/call", rate, asked, one("rating", accept(J(R"({"stars":9})"))));
    ASSERT_TRUE(out_of_bounds.contains("error")) << out_of_bounds.dump();
    EXPECT_EQ(out_of_bounds.at("error").at("code"), neomcp::error_code::invalid_params);
    EXPECT_EQ(rated.load(), 0);
    EXPECT_EQ(text_of(client.retry("tools/call", rate, asked, one("rating", accept(J(R"({"stars":4})"))))), "4");

    EXPECT_EQ(effects.bodies.load(), 0);
}

TEST(Declared, AnOptionalInputTheClientCannotTakeIsNotAsked) {
    const auto body = [](const neomcp::Answer<sdt::Confirm>& confirm, const neomcp::Answer<neomcp::SamplingAnswer>& summary) {
        return std::string(confirm.accepted() ? "confirmed" : "not confirmed") +
               (summary.status() == neomcp::AnswerStatus::not_asked ? ", no summary" : ", a summary");
    };
    const auto form = [] { return std::string("Go on?"); };
    const auto summary = [] { return sampling_of("Summarise"); };
    neomcp::Server server(options_with_key());
    server.tool("optional_summary", "A summary only where the client has a model", body,
                neomcp::elicit<sdt::Confirm>("confirm", form), neomcp::sample("summary", summary).optional());
    server.tool("required_summary", "A summary the call needs", body, neomcp::elicit<sdt::Confirm>("confirm", form),
                neomcp::sample("summary", summary));
    ModernClient forms_only(server, J(R"({"elicitation":{}})"));
    const json params = tool_params("optional_summary");
    const json first = forms_only.ask("tools/call", params);
    ASSERT_TRUE(asks(first)) << first.dump();
    EXPECT_EQ(asked_keys(first), std::set<std::string>{"confirm"});
    const json second = forms_only.retry("tools/call", params, first, one("confirm", accept(J(R"({"ok":true})"))));
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_EQ(text_of(second), "confirmed, no summary");

    // Required: the call ends with the revision's error for a missing capability, before anything is asked.
    const json refused = forms_only.ask("tools/call", tool_params("required_summary"));
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(refused.at("error").at("code"), -32021);
    EXPECT_TRUE(refused.at("error").at("data").at("requiredCapabilities").at("sampling").is_object());

    // A client that has the model is asked both.
    ModernClient everything(server);
    EXPECT_EQ(asked_keys(everything.ask("tools/call", params)), (std::set<std::string>{"confirm", "summary"}));
}

TEST(Declared, AQuestionThatIsNotNeededGivesTheHandlerWhatItSays) {
    neomcp::Server server(options_with_key());
    server.tool<sdt::Folder>(
        "maybe_delete", "Asks only for a folder that is given",
        [](const sdt::Folder& a, neomcp::Answer<sdt::Confirm> confirm) {
            if (!confirm.asked()) return std::string("nothing to delete");
            return std::string(confirm.accepted() && confirm->ok ? "deleted " : "kept ") + a.path;
        },
        neomcp::elicit<sdt::Confirm>("confirm", [](const sdt::Folder& a) -> neomcp::Question<sdt::Confirm> {
            if (a.path == "/scratch") return neomcp::no_question(sdt::Confirm{true});
            if (a.path.empty()) return neomcp::no_question<sdt::Confirm>();
            return "Delete " + a.path + "?";
        }));
    ModernClient client(server);
    EXPECT_EQ(text_of(client.ask("tools/call", tool_params("maybe_delete", J(R"({"path":"/scratch"})")))),
              "deleted /scratch");
    EXPECT_EQ(text_of(client.ask("tools/call", tool_params("maybe_delete", J(R"({"path":""})")))), "nothing to delete");
    EXPECT_TRUE(asks(client.ask("tools/call", tool_params("maybe_delete", J(R"({"path":"/home"})")))));
}

TEST(Declared, PromptsResourcesTemplatesAndCoroutinesDeclareInputs) {
    neomcp::Server server(options_with_key());
    const auto confirm_question = [] { return std::string("Go on?"); };
    server.prompt<sdt::Topic>(
        "topic_prompt", "A prompt that asks for a note",
        [](const sdt::Topic& a, const neomcp::Answer<sdt::Note>& note) { return "Talk about " + a.topic + ": " + note->text; },
        neomcp::elicit<sdt::Note>("note", [](const sdt::Topic& a) { return "A note on " + a.topic + "?"; }));
    server.prompt(
        "plain_prompt", "A prompt without arguments that asks",
        [](const neomcp::Answer<sdt::Confirm>& confirm) { return std::string(confirm->ok ? "yes" : "no"); },
        neomcp::elicit<sdt::Confirm>("confirm", confirm_question));
    server.resource(
        "test://roots", "The client's roots, counted",
        [](const neomcp::Answer<neomcp::RootsAnswer>& listed) { return std::to_string(listed->roots.size()) + " roots"; },
        neomcp::roots("roots"));
    server.resource_template<sdt::Item>(
        "test://items/{id}", "An item, after a confirmation",
        [](const sdt::Item& item, const neomcp::Answer<sdt::Confirm>&, neomcp::Context&) { return "item " + item.id; },
        neomcp::elicit<sdt::Confirm>("confirm", [](const sdt::Item& item) { return "Show " + item.id + "?"; }));
    server.tool(
        "coroutine", "A coroutine with an input",
        [](const neomcp::Answer<sdt::Confirm>& confirm) -> asio::awaitable<std::string> {
            co_return std::string(confirm->ok ? "went on" : "stopped");
        },
        neomcp::elicit<sdt::Confirm>("confirm", confirm_question));
    ModernClient client(server);
    const json yes = one("confirm", accept(J(R"({"ok":true})")));

    json prompt = json::object();
    prompt["name"] = "topic_prompt";
    prompt["arguments"] = J(R"({"topic":"tea"})");
    const json asked = client.ask("prompts/get", prompt);
    ASSERT_TRUE(asks(asked)) << asked.dump();
    EXPECT_EQ(asked.at("result").at("inputRequests").at("note").at("params").at("message"), "A note on tea?");
    const json got = client.retry("prompts/get", prompt, asked, one("note", accept(J(R"({"text":"green"})"))));
    ASSERT_TRUE(got.contains("result")) << got.dump();
    EXPECT_EQ(got.at("result").at("messages").at(0).at("content").at("text"), "Talk about tea: green");

    json plain = json::object();
    plain["name"] = "plain_prompt";
    const json plain_got = client.retry("prompts/get", plain, client.ask("prompts/get", plain), yes);
    EXPECT_EQ(plain_got.at("result").at("messages").at(0).at("content").at("text"), "yes") << plain_got.dump();

    json read = json::object();
    read["uri"] = "test://roots";
    const json contents = client.retry("resources/read", read, client.ask("resources/read", read), one("roots", roots_answer()));
    ASSERT_TRUE(contents.contains("result")) << contents.dump();
    EXPECT_EQ(contents.at("result").at("contents").at(0).at("text"), "2 roots");

    json item = json::object();
    item["uri"] = "test://items/42";
    const json item_asked = client.ask("resources/read", item);
    ASSERT_TRUE(asks(item_asked)) << item_asked.dump();
    EXPECT_EQ(item_asked.at("result").at("inputRequests").at("confirm").at("params").at("message"), "Show 42?");
    const json item_got = client.retry("resources/read", item, item_asked, yes);
    EXPECT_EQ(item_got.at("result").at("contents").at(0).at("text"), "item 42") << item_got.dump();

    const json call = tool_params("coroutine");
    EXPECT_EQ(text_of(client.retry("tools/call", call, client.ask("tools/call", call), yes)), "went on");
}

TEST(Declared, KeysNobodyDeclaredAreIgnored) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_delete(server, effects);
    ModernClient client(server);
    const json params = tool_params("delete_folder", J(R"({"path":"/opt"})"));
    const json first = client.ask("tools/call", params);
    json answers = one("confirm", accept(J(R"({"ok":true})")));
    answers["unknown_extra_key"] = accept(J(R"({"foo":"bar"})"));
    answers["another_unexpected"] = 12345;
    const json second = client.retry("tools/call", params, first, answers);
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_EQ(text_of(second), "deleted /opt");
}

// ---------------------------------------------------------------------------------------------------------------
// Where the server sends requests

TEST(Declared, InputsAreAskedThroughRequestsWhereTheRevisionSendsThem) {
    Effects effects;
    neomcp::Server server(options_with_key());
    register_delete(server, effects);
    LegacyClient client(server);
    client.send_call(1, tool_params("delete_folder", J(R"({"path":"/var"})")));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->at("method"), "elicitation/create");
    EXPECT_EQ(request->at("params").at("message"), "Delete /var?");
    client.answer(*request, accept(J(R"({"ok":true})")));
    const auto done = client.peer.await_response(neomcp::RequestId(1), patience);
    ASSERT_TRUE(done);
    EXPECT_EQ(text_of(*done), "deleted /var");
    EXPECT_EQ(effects.bodies.load(), 1);

    // Content that does not fit the form ends the call with Invalid params.
    client.send_call(3, tool_params("delete_folder", J(R"({"path":"/etc"})")));
    const auto again = client.peer.next_request(patience);
    ASSERT_TRUE(again);
    client.answer(*again, accept(J(R"({"ok":"yes"})")));
    const auto refused = client.peer.await_response(neomcp::RequestId(3), patience);
    ASSERT_TRUE(refused);
    ASSERT_TRUE(refused->contains("error")) << refused->dump();
    EXPECT_EQ(refused->at("error").at("code"), neomcp::error_code::invalid_params);
    EXPECT_EQ(effects.deleted.load(), 1);
}

}  // namespace
