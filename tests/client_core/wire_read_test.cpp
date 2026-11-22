// The readers of wire_read.h: what they read back of what the writers write, in every revision; what they refuse; and
// the way they name the place that is wrong.
//
// Every expected value is written here, from the pages and from the profile's own tables (`profile.type(...)`), never
// from a list of revisions: a revision that adds a member to a type needs no change in this file.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/wire.h"
#include "neomcp/wire_read.h"
#include "support.h"

namespace {

using namespace neomcp;
using client_core_test::Fill;
using client_core_test::Generator;
using client_core_test::same;

std::vector<profile::ProfilePtr> all_profiles() {
    std::vector<profile::ProfilePtr> out;
    for (const std::string_view revision : profile::builtin_revisions()) out.push_back(profile::builtin(revision));
    return out;
}

const profile::Profile& newest() {
    static const profile::ProfilePtr latest = profile::builtin(profile::builtin_revisions().back());
    return *latest;
}

bool starts_with(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }

// The profile of the revision after `edit` changed its document; null (and a failure) when it no longer loads.
profile::ProfilePtr edited(std::string_view revision, const std::function<void(json&)>& edit) {
    json document = json::parse(profile::builtin_text(revision));
    edit(document);
    auto loaded = profile::load(document.dump());
    if (const auto* problem = std::get_if<profile::Error>(&loaded)) {
        ADD_FAILURE() << revision << ": " << problem->pointer << " " << problem->message;
        return nullptr;
    }
    return std::get<profile::ProfilePtr>(loaded);
}

// Generates values of every fill, many of them at random, from seeds that do not change.
constexpr std::size_t random_values = 120;

struct Source {
    Fill fill;
    std::uint64_t seed;
};

std::vector<Source> sources() {
    std::vector<Source> out{{Fill::none, 1}, {Fill::all, 2}};
    for (std::size_t i = 0; i < random_values; ++i) out.push_back({Fill::any, 1000 + i});
    return out;
}

// The value written, read back from the value and from its text, for one profile.
//   exact: the value only holds members the profile's type has, so the reader returns an equal value;
//   otherwise the members the revision does not have are dropped by the writer: the reader returns a value that
//   writes the same, and writes nothing more under the newest revision than under this one (so it has invented no
//   member that the revision lacks).
template <typename T>
void check_round_trip(const T& original, const profile::Profile& profile, bool exact) {
    const json written = to_wire(original, profile);
    for (const json& sent : {written, json::parse(written.dump())}) {
        auto read = from_wire<T>(sent, profile);
        const T* value = std::get_if<T>(&read);
        ASSERT_NE(value, nullptr) << std::get<std::string>(read) << "\nwire: " << written.dump();
        EXPECT_EQ(to_wire(*value, profile), written);
        if (exact) {
            EXPECT_TRUE(same(*value, original)) << "wire: " << written.dump();
        } else {
            EXPECT_EQ(to_wire(*value, profile), to_wire(*value, newest())) << "wire: " << written.dump();
        }
    }
}

template <typename T>
void round_trip_everywhere(const std::function<T(Generator&)>& make) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        for (const bool exact : {true, false}) {
            for (const Source& source : sources()) {
                Generator generator(*profile, source.seed, source.fill, exact);
                check_round_trip<T>(make(generator), *profile, exact);
                if (::testing::Test::HasFatalFailure()) return;
            }
        }
    }
}

TEST(WireRead, IconRoundTrips) {
    round_trip_everywhere<Icon>([](Generator& g) { return g.icon(); });
}
TEST(WireRead, ImplementationRoundTrips) {
    round_trip_everywhere<Implementation>([](Generator& g) { return g.implementation(); });
}
TEST(WireRead, ToolAnnotationsRoundTrip) {
    round_trip_everywhere<ToolAnnotations>([](Generator& g) { return g.tool_annotations(); });
}
TEST(WireRead, ToolDefinitionRoundTrips) {
    round_trip_everywhere<ToolDefinition>([](Generator& g) { return g.tool(); });
}
TEST(WireRead, ContentBlockRoundTrips) {
    round_trip_everywhere<ContentBlock>([](Generator& g) { return g.block(); });
}
TEST(WireRead, ResourceDefinitionRoundTrips) {
    round_trip_everywhere<ResourceDefinition>([](Generator& g) { return g.resource(); });
}
TEST(WireRead, ResourceTemplateRoundTrips) {
    round_trip_everywhere<ResourceTemplateDefinition>([](Generator& g) { return g.resource_template(); });
}
TEST(WireRead, ResourceContentsRoundTrip) {
    round_trip_everywhere<ResourceContents>([](Generator& g) { return g.resource_contents(); });
}
TEST(WireRead, ReadResultRoundTrips) {
    round_trip_everywhere<ReadResult>([](Generator& g) { return g.read_result(); });
}
TEST(WireRead, PromptDefinitionRoundTrips) {
    round_trip_everywhere<PromptDefinition>([](Generator& g) { return g.prompt(); });
}
TEST(WireRead, PromptMessageRoundTrips) {
    round_trip_everywhere<PromptMessage>([](Generator& g) { return g.prompt_message(); });
}
TEST(WireRead, PromptResultRoundTrips) {
    round_trip_everywhere<PromptResult>([](Generator& g) { return g.prompt_result(); });
}
TEST(WireRead, CompletionResultRoundTrips) {
    round_trip_everywhere<CompletionResult>([](Generator& g) { return g.completion(); });
}
TEST(WireRead, ToolResultRoundTrips) {
    round_trip_everywhere<ToolResult>([](Generator& g) { return g.tool_result(); });
}

// Every kind of content block, in every revision that defines it, with every member present and with none.
TEST(WireRead, EveryKindOfBlockRoundTrips) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        for (const ContentKind kind : {ContentKind::text, ContentKind::image, ContentKind::audio,
                                       ContentKind::resource_link, ContentKind::embedded_resource}) {
            if (!has_content_kind(kind, *profile)) continue;
            for (const Fill fill : {Fill::none, Fill::all}) {
                Generator generator(*profile, 77, fill, true);
                check_round_trip<ContentBlock>(generator.block(kind), *profile, true);
            }
        }
    }
}

// A completion past the limit: the writer cuts it to max_completion_values and says so.
TEST(WireRead, CompletionPastTheLimitReadsAsTheWriterCutIt) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        for (const std::size_t count : {max_completion_values - 1, max_completion_values, max_completion_values + 1,
                                        2 * max_completion_values + 5}) {
            CompletionResult original;
            for (std::size_t i = 0; i < count; ++i) original.values.push_back("v" + std::to_string(i));
            const json written = to_wire(original, *profile);
            auto read = from_wire<CompletionResult>(written, *profile);
            const auto* value = std::get_if<CompletionResult>(&read);
            ASSERT_NE(value, nullptr) << std::get<std::string>(read);
            EXPECT_EQ(value->values.size(), std::min(count, max_completion_values));
            const bool cut = count > max_completion_values;
            EXPECT_EQ(value->has_more, cut ? std::optional<bool>(true) : std::nullopt);
            EXPECT_EQ(value->total, cut ? std::optional<std::uint64_t>(count) : std::nullopt);
        }
    }
}

// ---- Fixed cases, written from the pages

json parsed(const std::string& text) {
    json value = json::parse(text, nullptr, false);
    EXPECT_FALSE(value.is_discarded()) << text;
    return value;
}

// Whether the profile's type has the member; the expectations of a fixed case follow the profile's tables.
bool has(const profile::Profile& profile, const std::string& type, const std::string& member) {
    const profile::Type* found = profile.type(type);
    return found != nullptr && found->has_property(member);
}

TEST(WireRead, AToolOfTheNewestPageIsReadMemberByMember) {
    const std::string text = R"({
        "name": "add", "title": "Add", "description": "Adds two numbers.",
        "inputSchema": {"type": "object", "properties": {"a": {"type": "integer"}}},
        "outputSchema": {"type": "object"},
        "annotations": {"title": "Adder", "readOnlyHint": true, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false},
        "icons": [{"src": "https://example.test/i.png", "mimeType": "image/png", "sizes": ["48x48", "any"], "theme": "dark"}],
        "_meta": {"k": 1},
        "execution": {"taskSupport": "optional"},
        "somethingNew": [1, 2]
    })";
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        auto read = from_wire<ToolDefinition>(parsed(text), *profile);
        const auto* tool = std::get_if<ToolDefinition>(&read);
        ASSERT_NE(tool, nullptr) << std::get<std::string>(read);
        EXPECT_EQ(tool->name, "add");
        EXPECT_EQ(tool->description, std::optional<std::string>("Adds two numbers."));
        EXPECT_EQ(tool->input_schema, parsed(R"({"type": "object", "properties": {"a": {"type": "integer"}}})"));
        EXPECT_EQ(tool->title.has_value(), has(*profile, "Tool", "title"));
        EXPECT_EQ(tool->output_schema.has_value(), has(*profile, "Tool", "outputSchema"));
        EXPECT_EQ(tool->annotations.has_value(), has(*profile, "Tool", "annotations"));
        EXPECT_EQ(tool->icons.size(), has(*profile, "Tool", "icons") ? 1U : 0U);
        EXPECT_EQ(tool->meta.has_value(), has(*profile, "Tool", "_meta"));
        if (tool->annotations) {
            EXPECT_EQ(tool->annotations->title, std::optional<std::string>("Adder"));
            EXPECT_EQ(tool->annotations->read_only_hint, std::optional<bool>(true));
            EXPECT_EQ(tool->annotations->destructive_hint, std::optional<bool>(false));
            EXPECT_EQ(tool->annotations->idempotent_hint, std::optional<bool>(true));
            EXPECT_EQ(tool->annotations->open_world_hint, std::optional<bool>(false));
        }
        if (!tool->icons.empty()) {
            EXPECT_EQ(tool->icons[0].src, "https://example.test/i.png");
            EXPECT_EQ(tool->icons[0].mime_type, std::optional<std::string>("image/png"));
            EXPECT_EQ(tool->icons[0].sizes, (std::vector<std::string>{"48x48", "any"}));
            EXPECT_EQ(tool->icons[0].theme, std::optional<IconTheme>(IconTheme::dark));
        }
    }
}

TEST(WireRead, ABlockOfEveryKindIsReadFromItsType) {
    const profile::Profile& profile = newest();
    auto text = from_wire<ContentBlock>(parsed(R"({"type":"text","text":"hi","annotations":{"audience":["user"],"priority":0.5,"lastModified":"2026-01-01T00:00:00Z"}})"), profile);
    ASSERT_TRUE(std::holds_alternative<ContentBlock>(text)) << std::get<std::string>(text);
    const auto* block = std::get_if<TextContent>(&std::get<ContentBlock>(text));
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->text, "hi");
    ASSERT_TRUE(block->annotations.has_value());
    EXPECT_EQ(block->annotations->audience, std::vector<Role>{Role::user});
    EXPECT_EQ(block->annotations->priority, std::optional<double>(0.5));
    EXPECT_EQ(block->annotations->last_modified, std::optional<std::string>("2026-01-01T00:00:00Z"));

    auto image = from_wire<ContentBlock>(parsed(R"({"type":"image","data":"AAAA","mimeType":"image/png"})"), profile);
    ASSERT_TRUE(std::holds_alternative<ContentBlock>(image));
    EXPECT_EQ(kind_of(std::get<ContentBlock>(image)), ContentKind::image);
    auto audio = from_wire<ContentBlock>(parsed(R"({"type":"audio","data":"AAAA","mimeType":"audio/wav"})"), profile);
    ASSERT_TRUE(std::holds_alternative<ContentBlock>(audio));
    EXPECT_EQ(kind_of(std::get<ContentBlock>(audio)), ContentKind::audio);
    auto link = from_wire<ContentBlock>(parsed(R"({"type":"resource_link","uri":"file:///a","name":"a","size":12})"), profile);
    ASSERT_TRUE(std::holds_alternative<ContentBlock>(link));
    EXPECT_EQ(std::get<ResourceLink>(std::get<ContentBlock>(link)).size, std::optional<std::uint64_t>(12));
    auto embedded = from_wire<ContentBlock>(parsed(R"({"type":"resource","resource":{"uri":"file:///a","blob":"AAAA"}})"), profile);
    ASSERT_TRUE(std::holds_alternative<ContentBlock>(embedded)) << std::get<std::string>(embedded);
    EXPECT_TRUE(std::holds_alternative<BlobResourceContents>(std::get<EmbeddedResource>(std::get<ContentBlock>(embedded)).resource));
}

TEST(WireRead, ABlockKindThatIsNotKnownOrNotDefinedIsAnErrorForThatBlock) {
    // The pages say nothing about how a client treats a block of a kind it does not know (server/tools.mdx lists the
    // kinds; no page asks a client to skip or to keep one), so such a block is refused rather than guessed at.
    const json unknown = parsed(R"({"type":"video","data":"AAAA"})");
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        auto read = from_wire<ToolResult>(parsed(R"({"content":[{"type":"text","text":"a"},{"type":"video"}]})"), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read));
        EXPECT_TRUE(starts_with(std::get<std::string>(read), "$.content[1].type:")) << std::get<std::string>(read);
    }
    // A kind that this library has a block for but a revision does not define.
    for (const auto& profile : all_profiles()) {
        for (const ContentKind kind : {ContentKind::audio, ContentKind::resource_link}) {
            if (has_content_kind(kind, *profile)) continue;
            json wire = json::object();
            wire["type"] = std::string(wire_name(kind));
            auto read = from_wire<ContentBlock>(wire, *profile);
            ASSERT_TRUE(std::holds_alternative<std::string>(read)) << profile->revision() << " " << wire_name(kind);
            EXPECT_TRUE(starts_with(std::get<std::string>(read), "$.type:"));
        }
    }
    auto read = from_wire<ContentBlock>(unknown, newest());
    EXPECT_TRUE(std::holds_alternative<std::string>(read));
}

TEST(WireRead, ThePathNamesTheMemberThatIsWrong) {
    const profile::Profile& profile = newest();
    auto lacking = from_wire<ToolResult>(
        parsed(R"({"content":[{"type":"text","text":"a"},{"type":"resource","resource":{"text":"x"}}]})"), profile);
    ASSERT_TRUE(std::holds_alternative<std::string>(lacking));
    EXPECT_EQ(std::get<std::string>(lacking), "$.content[1].resource.uri: is required");

    auto wrong = from_wire<ToolDefinition>(parsed(R"({"name":"a","inputSchema":{},"title":5})"), profile);
    ASSERT_TRUE(std::holds_alternative<std::string>(wrong));
    EXPECT_EQ(std::get<std::string>(wrong), "$.title: must be a string");

    auto nested = from_wire<PromptResult>(
        parsed(R"({"messages":[{"role":"user","content":{"type":"text","text":"a"}},{"role":"robot","content":{"type":"text","text":"b"}}]})"),
        profile, "$.result");
    ASSERT_TRUE(std::holds_alternative<std::string>(nested));
    EXPECT_EQ(std::get<std::string>(nested), "$.result.messages[1].role: must be user or assistant");

    auto list = from_wire<Icon>(parsed(R"({"src":"a","sizes":["1x1",2]})"), profile);
    ASSERT_TRUE(std::holds_alternative<std::string>(list));
    EXPECT_EQ(std::get<std::string>(list), "$.sizes[1]: must be a string");
}

TEST(WireRead, NothingThePeerSentIsRepeatedInAMessage) {
    const std::string secret = "the-secret-value-0451";
    for (const auto& profile : all_profiles()) {
        json wire = json::object();
        wire["name"] = secret;
        wire["inputSchema"] = secret;
        auto read = from_wire<ToolDefinition>(wire, *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read));
        EXPECT_EQ(std::get<std::string>(read).find(secret), std::string::npos);
        wire = json::object();
        wire["type"] = secret;
        auto block = from_wire<ContentBlock>(wire, *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(block));
        EXPECT_EQ(std::get<std::string>(block).find(secret), std::string::npos);
    }
}

TEST(WireRead, ObjectsOnlyWhereTheSchemasSayObject) {
    const profile::Profile& profile = newest();
    for (const char* text : {R"({"name":"a","inputSchema":true})", R"({"name":"a","inputSchema":{},"outputSchema":[]})",
                             R"({"name":"a","inputSchema":{},"_meta":"x"})", R"({"name":"a","inputSchema":[]})"}) {
        auto read = from_wire<ToolDefinition>(parsed(text), profile);
        EXPECT_TRUE(std::holds_alternative<std::string>(read)) << text;
    }
    auto structured = from_wire<ToolResult>(parsed(R"({"content":[],"structuredContent":[1]})"), profile);
    EXPECT_TRUE(std::holds_alternative<std::string>(structured));
    auto meta = from_wire<ContentBlock>(parsed(R"({"type":"text","text":"a","_meta":5})"), profile);
    EXPECT_TRUE(std::holds_alternative<std::string>(meta));
}

TEST(WireRead, ANumberMustBeTheKindTheTypeNames) {
    const profile::Profile& profile = newest();
    for (const char* text : {R"({"type":"resource_link","uri":"u","name":"n","size":-1})",
                             R"({"type":"resource_link","uri":"u","name":"n","size":1.5})",
                             R"({"type":"resource_link","uri":"u","name":"n","size":"1"})"}) {
        auto read = from_wire<ContentBlock>(parsed(text), profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read)) << text;
        EXPECT_TRUE(starts_with(std::get<std::string>(read), "$.size:")) << std::get<std::string>(read);
    }
    // Any JSON number is a priority, because the writer writes any double.
    for (const char* text : {"0", "1", "-3", "2.5", "1e2"}) {
        auto read = from_wire<ContentBlock>(
            parsed(std::string(R"({"type":"text","text":"a","annotations":{"priority":)") + text + "}}"), profile);
        EXPECT_TRUE(std::holds_alternative<ContentBlock>(read)) << text;
    }
}

TEST(WireRead, TextOrBlobButNotBothAndNotNeither) {
    const profile::Profile& profile = newest();
    for (const char* text : {R"({"uri":"u","text":"a","blob":"AAAA"})", R"({"uri":"u"})"}) {
        auto read = from_wire<ResourceContents>(parsed(text), profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read)) << text;
        EXPECT_EQ(std::get<std::string>(read), "$: must have exactly one of text and blob");
    }
    auto text = from_wire<ResourceContents>(parsed(R"({"uri":"u","text":"a"})"), profile);
    EXPECT_TRUE(std::holds_alternative<TextResourceContents>(std::get<ResourceContents>(text)));
}

TEST(WireRead, ACompletionMayCarryNoMoreThanTheLimit) {
    json values = json::array();
    for (std::size_t i = 0; i <= max_completion_values; ++i) values.push_back("v");
    json wire = json::object();
    wire["completion"] = json{{"values", values}};
    for (const auto& profile : all_profiles()) {
        auto read = from_wire<CompletionResult>(wire, *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read)) << profile->revision();
        EXPECT_EQ(std::get<std::string>(read), "$.completion.values: has more values than a completion may carry");
    }
}

TEST(WireRead, MembersThatAnotherRevisionHasAreNotRead) {
    // `title` and `_meta` of a tool are in the newest revisions' type; a revision whose type lacks them ignores them
    // whatever the value, even a value that would be wrong.
    for (const auto& profile : all_profiles()) {
        const json wire = parsed(R"({"name":"a","inputSchema":{},"title":5,"_meta":7,"icons":"x","annotations":3})");
        auto read = from_wire<ToolDefinition>(wire, *profile);
        const bool reads_a_wrong_member = has(*profile, "Tool", "title") || has(*profile, "Tool", "_meta") ||
                                          has(*profile, "Tool", "icons") || has(*profile, "Tool", "annotations");
        EXPECT_EQ(std::holds_alternative<std::string>(read), reads_a_wrong_member) << profile->revision();
    }
}

TEST(WireRead, AnExtraMemberOfABlockIsKeptOnlyWhereTheTypeHasItAndTheModelDoesNot) {
    // Under the profile's own tables a member of the block type that the model has no member for would come back in
    // `extra`; none of the current revisions has one, so extra stays empty whatever the peer adds.
    for (const auto& profile : all_profiles()) {
        auto read = from_wire<ContentBlock>(parsed(R"({"type":"text","text":"a","x-vendor":1,"annotations":{}})"), *profile);
        ASSERT_TRUE(std::holds_alternative<ContentBlock>(read));
        EXPECT_TRUE(std::get<TextContent>(std::get<ContentBlock>(read)).extra.empty());
    }
}

// ---- Caching hints

TEST(WireRead, CachingHintsAreReadFromTheFormTheSessionWrites) {
    const profile::Profile& profile = newest();
    ASSERT_TRUE(has(profile, "ReadResourceResult", "ttlMs"));
    auto read = read_cache_hints(parsed(R"({"contents":[],"resultType":"complete","ttlMs":300000,"cacheScope":"public"})"),
                                 "ReadResourceResult", profile);
    const auto* hints = std::get_if<std::optional<CacheHints>>(&read);
    ASSERT_NE(hints, nullptr) << std::get<std::string>(read);
    ASSERT_TRUE(hints->has_value());
    EXPECT_EQ((*hints)->ttl_ms, 300000U);
    EXPECT_EQ((*hints)->scope, CacheScope::public_scope);

    auto zero = read_cache_hints(parsed(R"({"ttlMs":0,"cacheScope":"private"})"), "ReadResourceResult", profile);
    ASSERT_TRUE(std::holds_alternative<std::optional<CacheHints>>(zero));
    EXPECT_EQ(std::get<std::optional<CacheHints>>(zero)->scope, CacheScope::private_scope);

    // caching.mdx: a negative ttlMs is to be ignored and treated as 0.
    auto negative = read_cache_hints(parsed(R"({"ttlMs":-5,"cacheScope":"public"})"), "ReadResourceResult", profile);
    ASSERT_TRUE(std::holds_alternative<std::optional<CacheHints>>(negative));
    EXPECT_EQ(std::get<std::optional<CacheHints>>(negative)->ttl_ms, 0U);
}

TEST(WireRead, CachingHintsThatAreWrongOrMissingAreErrors) {
    const profile::Profile& profile = newest();
    for (const char* text : {R"({"ttlMs":"5","cacheScope":"public"})", R"({"ttlMs":1.5,"cacheScope":"public"})",
                             R"({"ttlMs":5,"cacheScope":"shared"})", R"({"ttlMs":5,"cacheScope":3})",
                             R"({"ttlMs":5})", R"({"cacheScope":"public"})", R"({})"}) {
        auto read = read_cache_hints(parsed(text), "ReadResourceResult", profile);
        EXPECT_TRUE(std::holds_alternative<std::string>(read)) << text;
    }
    auto not_an_object = read_cache_hints(json(5), "ReadResourceResult", profile);
    EXPECT_TRUE(std::holds_alternative<std::string>(not_an_object));
}

TEST(WireRead, ARevisionWithoutCachingHintsHasNone) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        auto read = read_cache_hints(parsed(R"({"contents":[],"ttlMs":5,"cacheScope":"public"})"), "ReadResourceResult", *profile);
        ASSERT_TRUE(std::holds_alternative<std::optional<CacheHints>>(read)) << std::get<std::string>(read);
        EXPECT_EQ(std::get<std::optional<CacheHints>>(read).has_value(), has(*profile, "ReadResourceResult", "ttlMs"));
        auto result = from_wire<ReadResult>(parsed(R"({"contents":[],"ttlMs":5,"cacheScope":"public"})"), *profile);
        ASSERT_TRUE(std::holds_alternative<ReadResult>(result));
        EXPECT_EQ(std::get<ReadResult>(result).cache.has_value(), has(*profile, "ReadResourceResult", "ttlMs"));
    }
}

TEST(WireRead, WhenTheTypeDoesNotRequireTheHintsAResultWithoutThemHasNone) {
    // The newest document with the two hints taken out of the required members: a result that gives neither has no hints.
    const std::string_view revision = profile::builtin_revisions().back();
    const profile::ProfilePtr lenient = edited(revision, [](json& document) {
        json& type = document["wire"]["types"]["ReadResourceResult"];
        json kept = json::array();
        for (const json& name : type["required"].get_ref<const json::array_t&>()) {
            const auto* text = name.get_ptr<const std::string*>();
            if (text != nullptr && *text != "ttlMs" && *text != "cacheScope") kept.push_back(name);
        }
        type["required"] = kept;
    });
    ASSERT_NE(lenient, nullptr);
    ASSERT_FALSE(lenient->type("ReadResourceResult")->is_required("ttlMs"));
    auto none = read_cache_hints(parsed(R"({"contents":[]})"), "ReadResourceResult", *lenient);
    ASSERT_TRUE(std::holds_alternative<std::optional<CacheHints>>(none)) << std::get<std::string>(none);
    EXPECT_FALSE(std::get<std::optional<CacheHints>>(none).has_value());
    auto ttl_only = read_cache_hints(parsed(R"({"ttlMs":7})"), "ReadResourceResult", *lenient);
    ASSERT_TRUE(std::holds_alternative<std::optional<CacheHints>>(ttl_only));
    ASSERT_TRUE(std::get<std::optional<CacheHints>>(ttl_only).has_value());
    EXPECT_EQ(std::get<std::optional<CacheHints>>(ttl_only)->ttl_ms, 7U);
}

TEST(WireRead, AReadResultWithoutHintsIsLeftToTheEngineToJudge) {
    // The writer does not write the hints (the engine adds them), so a result as the writer wrote it has none.
    const profile::Profile& profile = newest();
    auto read = from_wire<ReadResult>(parsed(R"({"contents":[{"uri":"u","text":"a"}]})"), profile);
    ASSERT_TRUE(std::holds_alternative<ReadResult>(read)) << std::get<std::string>(read);
    EXPECT_FALSE(std::get<ReadResult>(read).cache.has_value());
    auto half = from_wire<ReadResult>(parsed(R"({"contents":[],"ttlMs":5})"), profile);
    EXPECT_TRUE(std::holds_alternative<std::string>(half));
}

// ---- A value that is not an object, and members of the wrong kind, never throw

template <typename T>
void expect_error_for_every_non_object(const profile::Profile& profile) {
    for (const json& wire : {json(nullptr), json(5), json("s"), json::array(), json(true)}) {
        auto read = from_wire<T>(wire, profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read)) << wire.dump();
        EXPECT_TRUE(starts_with(std::get<std::string>(read), "$")) << std::get<std::string>(read);
    }
}

TEST(WireRead, NoReaderAcceptsAValueThatIsNotAnObject) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        expect_error_for_every_non_object<Icon>(*profile);
        expect_error_for_every_non_object<Implementation>(*profile);
        expect_error_for_every_non_object<ToolAnnotations>(*profile);
        expect_error_for_every_non_object<ToolDefinition>(*profile);
        expect_error_for_every_non_object<ContentBlock>(*profile);
        expect_error_for_every_non_object<ResourceDefinition>(*profile);
        expect_error_for_every_non_object<ResourceTemplateDefinition>(*profile);
        expect_error_for_every_non_object<ResourceContents>(*profile);
        expect_error_for_every_non_object<ReadResult>(*profile);
        expect_error_for_every_non_object<PromptDefinition>(*profile);
        expect_error_for_every_non_object<PromptMessage>(*profile);
        expect_error_for_every_non_object<PromptResult>(*profile);
        expect_error_for_every_non_object<CompletionResult>(*profile);
        expect_error_for_every_non_object<ToolResult>(*profile);
    }
}

// ---- What a reader says about a value of the wrong shape, pinned member by member

TEST(WireRead, ANonObjectIsNamedAtTheGivenPlace) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        const auto tool = from_wire<ToolDefinition>(json(5), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(tool));
        EXPECT_EQ(std::get<std::string>(tool), "$: must be an object");
        const auto icon = from_wire<Icon>(json::array(), *profile, "$.icon");
        ASSERT_TRUE(std::holds_alternative<std::string>(icon));
        EXPECT_EQ(std::get<std::string>(icon), "$.icon: must be an object");
        const auto block = from_wire<ContentBlock>(json("s"), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(block));
        EXPECT_EQ(std::get<std::string>(block), "$: must be an object");
        const auto contents = from_wire<ResourceContents>(json(nullptr), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(contents));
        EXPECT_EQ(std::get<std::string>(contents), "$: must be an object");
    }
}

TEST(WireRead, ABlockNamesItsTypeMemberWhenThatIsMissingOrWrong) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        const auto missing = from_wire<ContentBlock>(parsed(R"({"text":"a"})"), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(missing));
        EXPECT_EQ(std::get<std::string>(missing), "$.type: is required");
        const auto wrong = from_wire<ContentBlock>(parsed(R"({"type":5,"text":"a"})"), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(wrong));
        EXPECT_EQ(std::get<std::string>(wrong), "$.type: must be a string");
    }
}

TEST(WireRead, AnIconThemeThatIsNeitherLightNorDarkIsAnError) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        const auto read = from_wire<Icon>(parsed(R"({"src":"a","theme":"blue"})"), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read));
        EXPECT_EQ(std::get<std::string>(read), "$.theme: must be light or dark");
    }
}

TEST(WireRead, AnAudienceEntryThatIsNotARoleIsAnErrorAtItsIndex) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        const auto read = from_wire<ContentBlock>(
            parsed(R"({"type":"text","text":"a","annotations":{"audience":["user","robot"]}})"), *profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read));
        EXPECT_EQ(std::get<std::string>(read), "$.annotations.audience[1]: must be user or assistant");
    }
}

TEST(WireRead, ASizeIsReadWhateverIntegerKindTheValueHolds) {
    // A size built in code is a signed integer, one parsed from text an unsigned one; both are sizes, also zero.
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        for (const std::int64_t size : {std::int64_t{0}, std::int64_t{12}}) {
            json built = parsed(R"({"type":"resource_link","uri":"u","name":"n"})");
            built["size"] = size;
            for (const json& wire : {built, json::parse(built.dump())}) {
                const auto read = from_wire<ContentBlock>(wire, *profile);
                if (!has_content_kind(ContentKind::resource_link, *profile)) continue;
                ASSERT_TRUE(std::holds_alternative<ContentBlock>(read)) << std::get<std::string>(read);
                EXPECT_EQ(std::get<ResourceLink>(std::get<ContentBlock>(read)).size,
                          std::optional<std::uint64_t>(static_cast<std::uint64_t>(size)));
            }
        }
    }
}

TEST(WireRead, ACachingTtlOfZeroIsReadWhateverIntegerKindItHolds) {
    const profile::Profile& profile = newest();
    json built = parsed(R"({"cacheScope":"public"})");
    built["ttlMs"] = std::int64_t{0};
    for (const json& wire : {built, json::parse(built.dump())}) {
        const auto read = read_cache_hints(wire, "ReadResourceResult", profile);
        ASSERT_TRUE(std::holds_alternative<std::optional<CacheHints>>(read)) << std::get<std::string>(read);
        ASSERT_TRUE(std::get<std::optional<CacheHints>>(read).has_value());
        EXPECT_EQ(std::get<std::optional<CacheHints>>(read)->ttl_ms, 0U);
    }
    // A negative ttl is read as 0 and not as a huge unsigned number.
    json negative = parsed(R"({"cacheScope":"public"})");
    negative["ttlMs"] = std::int64_t{-1};
    const auto read = read_cache_hints(negative, "ReadResourceResult", profile);
    ASSERT_TRUE(std::holds_alternative<std::optional<CacheHints>>(read));
    EXPECT_EQ(std::get<std::optional<CacheHints>>(read)->ttl_ms, 0U);
}

// ---- Members the profile's type has and the model has no member for

// The profile of a revision whose text block type also has the member `x-note`.
profile::ProfilePtr with_text_note(std::string_view revision) {
    return edited(revision, [](json& document) {
        document["wire"]["types"]["TextContent"]["properties"].push_back("x-note");
    });
}

TEST(WireRead, AMemberTheTypeHasAndTheModelDoesNotIsKeptAsExtra) {
    for (const std::string_view revision : profile::builtin_revisions()) {
        SCOPED_TRACE(std::string(revision));
        const profile::ProfilePtr profile = with_text_note(revision);
        ASSERT_NE(profile, nullptr);
        const json wire = parsed(R"({"type":"text","text":"a","x-note":[1,2],"x-other":3})");
        const auto read = from_wire<ContentBlock>(wire, *profile);
        ASSERT_TRUE(std::holds_alternative<ContentBlock>(read)) << std::get<std::string>(read);
        const auto& block = std::get<TextContent>(std::get<ContentBlock>(read));
        // The member of the type is kept; one that the type does not have is not.
        EXPECT_EQ(block.extra, parsed(R"({"x-note":[1,2]})"));
        // And the writer puts it back.
        EXPECT_EQ(to_wire(std::get<ContentBlock>(read), *profile).at("x-note"), parsed("[1,2]"));
        // Another kind of block, whose type lacks the member, keeps nothing.
        const auto image = from_wire<ContentBlock>(
            parsed(R"({"type":"image","data":"AAAA","mimeType":"image/png","x-note":1})"), *profile);
        ASSERT_TRUE(std::holds_alternative<ContentBlock>(image));
        EXPECT_TRUE(std::get<ImageContent>(std::get<ContentBlock>(image)).extra.empty());
    }
}

TEST(WireRead, AMemberTheProfileRequiresIsRequiredWhateverTheModelSays) {
    // `title` of a tool is optional in the model and in every revision; a revision that required it would have it so.
    const std::string_view revision = profile::builtin_revisions().back();
    ASSERT_TRUE(has(newest(), "Tool", "title"));
    const profile::ProfilePtr strict = edited(revision, [](json& document) {
        document["wire"]["types"]["Tool"]["required"].push_back("title");
    });
    ASSERT_NE(strict, nullptr);
    ASSERT_TRUE(strict->type("Tool")->is_required("title"));
    const auto without = from_wire<ToolDefinition>(parsed(R"({"name":"a","inputSchema":{}})"), *strict);
    ASSERT_TRUE(std::holds_alternative<std::string>(without));
    EXPECT_EQ(std::get<std::string>(without), "$.title: is required");
    EXPECT_TRUE(std::holds_alternative<ToolDefinition>(
        from_wire<ToolDefinition>(parsed(R"({"name":"a","inputSchema":{},"title":"t"})"), *strict)));
    EXPECT_TRUE(std::holds_alternative<ToolDefinition>(
        from_wire<ToolDefinition>(parsed(R"({"name":"a","inputSchema":{}})"), newest())));
}

// ---- Sweeps over the members of generated values
//
// For every member of the wire form of a full value: replacing it by a value of another kind must give an error that
// names the member; removing it must give an error exactly when the profile's type requires it.

struct Member {
    std::vector<std::string> steps;  // object members and array indices, from the root
    std::string path;                // as the reader writes it
    std::string owner_type;          // the profile type of the object that holds the member; empty: no profile type
    std::string name;                // the member's name; empty for an array element
    bool element = false;
    std::string object_path;
};

// The type of the object a member holds, where the member leads to one; empty when it leads to none.
std::string content_type(const json& block) {
    const auto* members = block.get_ptr<const json::object_t*>();
    if (members == nullptr) return {};
    const auto found = members->find("type");
    const auto* kind = found == members->end() ? nullptr : found->second.get_ptr<const std::string*>();
    if (kind == nullptr) return {};
    for (const ContentKind candidate : {ContentKind::text, ContentKind::image, ContentKind::audio,
                                        ContentKind::resource_link, ContentKind::embedded_resource}) {
        if (wire_name(candidate) == *kind) return std::string(type_name(candidate));
    }
    return {};
}

std::string contents_type(const json& contents) {
    return contents.contains("text") ? "TextResourceContents" : "BlobResourceContents";
}

// The profile type of the object found at `member` of an object of `owner`; "?" when it is not an object with a type.
std::string child_type(const std::string& owner, const std::string& member, const json& child) {
    if (member == "icons") return "Icon";
    if (member == "annotations") return owner == "Tool" ? "ToolAnnotations" : "Annotations";
    if (member == "resource") return contents_type(child);
    if (member == "arguments") return "PromptArgument";
    if (member == "messages") return "PromptMessage";
    if (member == "contents" && owner == "ReadResourceResult") return "contents";
    if (member == "content" && owner == "PromptMessage") return content_type(child);
    if (member == "content" && owner == "CallToolResult") return "content";
    if (member == "completion") return "";
    return "?";
}

void collect(const json& node, const std::string& owner, const std::string& path, std::vector<std::string> steps,
             std::vector<Member>& out) {
    const auto* members = node.get_ptr<const json::object_t*>();
    if (members == nullptr) return;
    for (const auto& [name, child] : *members) {
        Member entry;
        entry.steps = steps;
        entry.steps.push_back(name);
        entry.path = path + "." + name;
        entry.owner_type = owner;
        entry.name = name;
        entry.object_path = path;
        out.push_back(entry);
        if (name == "_meta" || name == "inputSchema" || name == "outputSchema" || name == "structuredContent") continue;
        const std::string below = child_type(owner, name, child);
        if (below == "?") {
            // A list of strings: its elements can be replaced too.
            if (const auto* list = child.get_ptr<const json::array_t*>()) {
                for (std::size_t i = 0; i < list->size(); ++i) {
                    Member element;
                    element.steps = entry.steps;
                    element.steps.push_back(std::to_string(i));
                    element.path = entry.path + "[" + std::to_string(i) + "]";
                    element.element = true;
                    element.owner_type = owner;
                    element.object_path = entry.path;
                    out.push_back(element);
                }
            }
            continue;
        }
        if (const auto* list = child.get_ptr<const json::array_t*>()) {
            for (std::size_t i = 0; i < list->size(); ++i) {
                const json& item = (*list)[i];
                const std::string item_path = entry.path + "[" + std::to_string(i) + "]";
                std::vector<std::string> item_steps = entry.steps;
                item_steps.push_back(std::to_string(i));
                std::string item_type = below;
                if (below == "contents") item_type = contents_type(item);
                if (below == "content") item_type = content_type(item);
                Member element;
                element.steps = item_steps;
                element.path = item_path;
                element.element = true;
                element.owner_type = owner;
                element.object_path = entry.path;
                out.push_back(element);
                collect(item, item_type, item_path, item_steps, out);
            }
        } else {
            collect(child, below, entry.path, entry.steps, out);
        }
    }
}

json* navigate(json& root, const std::vector<std::string>& steps) {
    json* at = &root;
    for (const std::string& step : steps) {
        if (at->is_array()) {
            const std::size_t index = static_cast<std::size_t>(std::stoul(step));
            if (index >= at->size()) return nullptr;
            at = &(*at)[index];
        } else if (at->is_object()) {
            auto* members = at->get_ptr<json::object_t*>();
            const auto found = members->find(step);
            if (found == members->end()) return nullptr;
            at = &found->second;
        } else {
            return nullptr;
        }
    }
    return at;
}

json other_kind(const json& original) { return original.is_number() ? json("x") : json(5); }

template <typename T>
void sweep_one(const json& wire, const std::string& root_type, const profile::Profile& profile) {
    std::vector<Member> members;
    collect(wire, root_type, "$", {}, members);
    ASSERT_FALSE(members.empty());
    for (const Member& member : members) {
        // Replaced by a value of another kind.
        json changed = wire;
        json* target = navigate(changed, member.steps);
        ASSERT_NE(target, nullptr) << member.path;
        *target = other_kind(*target);
        auto read = from_wire<T>(changed, profile);
        ASSERT_TRUE(std::holds_alternative<std::string>(read)) << profile.revision() << " " << member.path << "\n" << wire.dump();
        EXPECT_TRUE(starts_with(std::get<std::string>(read), member.path + ":"))
            << profile.revision() << " " << member.path << ": " << std::get<std::string>(read);
        if (member.element) continue;
        // Removed.
        json removed = wire;
        json* holder = navigate(removed, std::vector<std::string>(member.steps.begin(), member.steps.end() - 1));
        ASSERT_NE(holder, nullptr);
        holder->get_ptr<json::object_t*>()->erase(member.name);
        auto reread = from_wire<T>(removed, profile);
        const profile::Type* type = profile.type(member.owner_type);
        // Where the profile has no type of the object, the model's own needs decide: the inline completion needs its
        // values, and an icon its source.
        const bool model_required = (member.owner_type.empty() && member.name == "values") ||
                                    (type == nullptr && member.owner_type == "Icon" && member.name == "src");
        const bool required = (type != nullptr && type->is_required(member.name)) || model_required;
        if (required) {
            ASSERT_TRUE(std::holds_alternative<std::string>(reread))
                << profile.revision() << " without " << member.path << "\n" << removed.dump();
            EXPECT_TRUE(starts_with(std::get<std::string>(reread), member.object_path))
                << std::get<std::string>(reread);
        } else {
            EXPECT_TRUE(std::holds_alternative<T>(reread))
                << profile.revision() << " without " << member.path << ": "
                << (std::holds_alternative<std::string>(reread) ? std::get<std::string>(reread) : std::string());
        }
    }
}

template <typename T>
void sweep_everywhere(const std::string& root_type, const std::function<T(Generator&)>& make,
                      const std::function<std::string(const json&)>& type_of = {}) {
    for (const auto& profile : all_profiles()) {
        SCOPED_TRACE(profile->revision());
        for (std::uint64_t seed = 1; seed <= 3; ++seed) {
            Generator generator(*profile, seed, Fill::all, true);
            const T value = make(generator);
            const json wire = to_wire(value, *profile);
            sweep_one<T>(wire, type_of ? type_of(wire) : root_type, *profile);
            if (::testing::Test::HasFatalFailure()) return;
        }
    }
}

TEST(WireReadSweep, ToolDefinition) {
    sweep_everywhere<ToolDefinition>("Tool", [](Generator& g) { return g.tool(); });
}
TEST(WireReadSweep, Implementation) {
    sweep_everywhere<Implementation>("Implementation", [](Generator& g) { return g.implementation(); });
}
TEST(WireReadSweep, Icon) {
    sweep_everywhere<Icon>("Icon", [](Generator& g) { return g.icon(); });
}
TEST(WireReadSweep, ContentBlock) {
    sweep_everywhere<ContentBlock>("", [](Generator& g) { return g.block(); }, content_type);
}
TEST(WireReadSweep, ResourceDefinition) {
    sweep_everywhere<ResourceDefinition>("Resource", [](Generator& g) { return g.resource(); });
}
TEST(WireReadSweep, ResourceTemplate) {
    sweep_everywhere<ResourceTemplateDefinition>("ResourceTemplate", [](Generator& g) { return g.resource_template(); });
}
TEST(WireReadSweep, ResourceContents) {
    sweep_everywhere<ResourceContents>("", [](Generator& g) { return g.resource_contents(); }, contents_type);
}
TEST(WireReadSweep, ReadResult) {
    sweep_everywhere<ReadResult>("ReadResourceResult", [](Generator& g) { return g.read_result(); });
}
TEST(WireReadSweep, Prompt) {
    sweep_everywhere<PromptDefinition>("Prompt", [](Generator& g) { return g.prompt(); });
}
TEST(WireReadSweep, PromptMessage) {
    sweep_everywhere<PromptMessage>("PromptMessage", [](Generator& g) { return g.prompt_message(); });
}
TEST(WireReadSweep, PromptResult) {
    sweep_everywhere<PromptResult>("GetPromptResult", [](Generator& g) { return g.prompt_result(); });
}
TEST(WireReadSweep, CompletionResult) {
    sweep_everywhere<CompletionResult>("CompleteResult", [](Generator& g) { return g.completion(); });
}
TEST(WireReadSweep, ToolResult) {
    sweep_everywhere<ToolResult>("CallToolResult", [](Generator& g) { return g.tool_result(); });
}

TEST(WireRead, ToolExecutionTaskSupportIsReadAndWritten) {
    const profile::ProfilePtr tasks_2025 = profile::builtin("2025-11-25");
    ASSERT_NE(tasks_2025, nullptr);

    for (const auto& [text, expected] : {std::pair<const char*, TaskSupport>{"forbidden", TaskSupport::forbidden},
                                         std::pair<const char*, TaskSupport>{"optional", TaskSupport::optional},
                                         std::pair<const char*, TaskSupport>{"required", TaskSupport::required}}) {
        const json wire = json{{"name", "work"},
                               {"inputSchema", {{"type", "object"}}},
                               {"execution", {{"taskSupport", text}}}};
        const auto read = from_wire<ToolDefinition>(wire, *tasks_2025);
        ASSERT_TRUE(std::holds_alternative<ToolDefinition>(read)) << std::get<std::string>(read);
        const ToolDefinition& tool = std::get<ToolDefinition>(read);
        EXPECT_EQ(tool.task_support, std::optional<TaskSupport>(expected));

        const json rewritten = to_wire(tool, *tasks_2025);
        EXPECT_EQ(rewritten.at("execution").at("taskSupport"), text);
        // Revisions without `Tool.execution` ignore it on read and drop it on write.
        EXPECT_FALSE(to_wire(tool, newest()).contains("execution"));
        const auto older_read = from_wire<ToolDefinition>(wire, newest());
        ASSERT_TRUE(std::holds_alternative<ToolDefinition>(older_read)) << std::get<std::string>(older_read);
        EXPECT_EQ(std::get<ToolDefinition>(older_read).task_support, std::nullopt);
    }

    const json absent = json{{"name", "plain"}, {"inputSchema", {{"type", "object"}}}};
    const auto read_absent = from_wire<ToolDefinition>(absent, *tasks_2025);
    ASSERT_TRUE(std::holds_alternative<ToolDefinition>(read_absent));
    EXPECT_FALSE(std::get<ToolDefinition>(read_absent).task_support.has_value());

    const json empty_exec = json{{"name", "plain"}, {"inputSchema", {{"type", "object"}}}, {"execution", json::object()}};
    const auto read_empty = from_wire<ToolDefinition>(empty_exec, *tasks_2025);
    ASSERT_TRUE(std::holds_alternative<ToolDefinition>(read_empty));
    EXPECT_FALSE(std::get<ToolDefinition>(read_empty).task_support.has_value());

    const json bad_value =
        json{{"name", "bad"}, {"inputSchema", {{"type", "object"}}}, {"execution", {{"taskSupport", "always"}}}};
    const auto failed_value = from_wire<ToolDefinition>(bad_value, *tasks_2025, "tool");
    ASSERT_TRUE(std::holds_alternative<std::string>(failed_value));
    EXPECT_NE(std::get<std::string>(failed_value).find("tool.execution.taskSupport"), std::string::npos);

    const json bad_object = json{{"name", "bad"}, {"inputSchema", {{"type", "object"}}}, {"execution", 5}};
    const auto failed_object = from_wire<ToolDefinition>(bad_object, *tasks_2025, "tool");
    ASSERT_TRUE(std::holds_alternative<std::string>(failed_object));
    EXPECT_NE(std::get<std::string>(failed_object).find("tool.execution"), std::string::npos);

    for (const json& bad_kind : {json(nullptr), json(1), json(true), json(""), json("Optional")}) {
        const json invalid_kind = json{{"name", "bad"},
                                       {"inputSchema", {{"type", "object"}}},
                                       {"execution", {{"taskSupport", bad_kind}}}};
        const auto rejected = from_wire<ToolDefinition>(invalid_kind, *tasks_2025, "tool");
        ASSERT_TRUE(std::holds_alternative<std::string>(rejected)) << bad_kind.dump();
        EXPECT_NE(std::get<std::string>(rejected).find("tool.execution.taskSupport"), std::string::npos);
    }
}

}  // namespace
