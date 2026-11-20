// The bounds on the schemas a server sends, each driven to its edge through a tool listing: an output schema is
// compiled when the first structured result of its tool arrives, within the peer limits, against one budget of compiles
// per client. Also the bound on how deep a schema with header marks is searched, and the bounds of a listing. A form's
// requested schema reaches the same compile only after the restricted-form check, which admits neither patterns nor
// nesting, so the schema rows reach the validator through output schemas.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "neomcp/json_schema.h"
#include "support.h"

namespace {

using namespace client_test;
using neomcp::ClientDefaults;
using neomcp::ClientErrorKind;
using neomcp::OutputCheckKind;
using neomcp::profile::Transport;

constexpr std::string_view revision = "2026-07-28";

// ---- What the server sends

json tools_page(json tools, std::optional<json> next = std::nullopt) {
    json page = json{{"tools", std::move(tools)}, {"ttlMs", 0}, {"cacheScope", "private"}, {"resultType", "complete"}};
    if (next) page["nextCursor"] = std::move(*next);
    return page;
}

json tool(std::string_view name, std::optional<json> output_schema = std::nullopt, json input_schema = nullptr) {
    if (input_schema.is_null()) input_schema = json{{"type", "object"}};
    json definition = json{{"name", std::string(name)}, {"inputSchema", std::move(input_schema)}};
    if (output_schema) definition["outputSchema"] = std::move(*output_schema);
    return definition;
}

// A schema `levels` levels of `items` deep, the empty schema at the bottom.
json items_chain(std::size_t levels) {
    json schema = json::object();
    for (std::size_t i = 0; i < levels; ++i) schema = json{{"items", std::move(schema)}};
    return schema;
}

// An object schema whose deepest subschema is `depth` levels below the root (the root is level 0).
json schema_of_depth(std::size_t depth) {
    json properties = json::object();
    properties["a"] = items_chain(depth - 1);
    return json{{"type", "object"}, {"properties", std::move(properties)}};
}

json with_pattern(const std::string& pattern) {
    json properties = json::object();
    properties["s"] = json{{"type", "string"}, {"pattern", pattern}};
    return json{{"type", "object"}, {"properties", std::move(properties)}};
}

// A pattern of `count` word characters in counted repetitions of at most a thousand. The engine's program holds a few
// instructions for each (the class has four ranges), so its memory grows with `count` while its text stays short.
std::string word_characters(std::size_t count) {
    constexpr std::size_t most = 1000;
    std::string pattern;
    for (; count >= most; count -= most) pattern += "\\w{" + std::to_string(most) + "}";
    if (count > 0) pattern += "\\w{" + std::to_string(count) + "}";
    return pattern;
}

// The limits the client compiles a server's schemas under, from the defaults.
neomcp::json_schema::Limits peer_limits() {
    neomcp::json_schema::Limits limits;
    limits.max_schema_depth = ClientDefaults::peer_schema_depth;
    limits.max_pattern_length = ClientDefaults::peer_pattern_length;
    limits.max_pattern_memory = ClientDefaults::peer_pattern_memory;
    limits.max_evaluations = ClientDefaults::peer_max_evaluations;
    limits.max_patterns = ClientDefaults::peer_patterns_per_schema;
    return limits;
}

bool compiles(const json& schema) {
    return std::holds_alternative<neomcp::json_schema::Validator>(
        neomcp::json_schema::Validator::compile(schema, peer_limits()));
}

// The largest n in [low, high) for which `fits(n)` holds, given that it holds for `low` and not for `high`, and that
// it holds for every n below one for which it holds.
template <class Fits>
std::size_t largest_fitting(std::size_t low, std::size_t high, Fits fits) {
    while (high - low > 1) {
        const std::size_t middle = low + (high - low) / 2;
        if (fits(middle)) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

// A server that lists `tools` (in one page) and answers a call of a tool with the structured content `results` holds
// for it.
struct ToolServer {
    json tools = json::array();
    std::map<std::string, json> results;
};

bool answer_opening(ScriptedPeer& peer, const Seen& request) {
    if (request.method() == "server/discover") {
        peer.answer(request, discover_result(revision));
        return true;
    }
    return false;
}

std::shared_ptr<ScriptedPeer> tool_server(std::shared_ptr<ToolServer> script) {
    return std::make_shared<ScriptedPeer>([script](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request)) return;
        if (request.method() == "tools/list") {
            side.answer(request, tools_page(script->tools));
            return;
        }
        if (request.method() != "tools/call") return;
        const std::string name = request.message["params"]["name"].get<std::string>();
        const auto found = script->results.find(name);
        json content = found == script->results.end() ? json::object() : found->second;
        side.answer(request, json{{"content", json::array()}, {"structuredContent", std::move(content)},
                                  {"resultType", "complete"}});
    });
}

neomcp::Client open_with(neomcp::ClientOptions options, const std::shared_ptr<ScriptedPeer>& peer,
                         Transport kind = Transport::stdio) {
    if (kind == Transport::streamable_http) peer->http = true;
    auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(kind, peer));
    if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
    return std::move(opened).value();
}

// The kind of check of one call of `name`, with its issue.
neomcp::OutputCheck check_of(neomcp::Client& client, const std::string& name) {
    const auto called = client.call_tool(name, json::object());
    if (!called) throw std::runtime_error("the call of " + name + " failed: " + called.error().message);
    return called->check;
}

struct Heard {
    std::mutex mutex;
    std::vector<neomcp::ClientDiagnostic> all;

    void listen(neomcp::ClientOptions& options) {
        options.on_diagnostic = [this](const neomcp::ClientDiagnostic& diagnostic) {
            std::lock_guard lock(mutex);
            all.push_back(diagnostic);
        };
    }
    std::size_t count(neomcp::DiagnosticKind kind) {
        std::lock_guard lock(mutex);
        std::size_t found = 0;
        for (const auto& one : all) {
            const auto* engine = std::get_if<neomcp::Diagnostic>(&one);
            found += engine != nullptr && engine->kind == kind ? 1 : 0;
        }
        return found;
    }
    std::vector<std::string> dropped() {
        std::lock_guard lock(mutex);
        std::vector<std::string> found;
        for (const auto& one : all) {
            if (const auto* drop = std::get_if<neomcp::ToolDropped>(&one)) found.push_back(drop->tool);
        }
        return found;
    }
};

// ---- The peer limits, one at a time

TEST(ClientBounds, AnOutputSchemaAtTheDepthBoundIsUsedAndOneLevelDeeperIsUnverified) {
    constexpr std::size_t bound = ClientDefaults::peer_schema_depth;
    auto script = std::make_shared<ToolServer>();
    script->tools.push_back(tool("at", schema_of_depth(bound)));
    script->tools.push_back(tool("past", schema_of_depth(bound + 1)));
    neomcp::Client client = open_with(only({revision}), tool_server(script));
    ASSERT_TRUE(client.list_all_tools());
    EXPECT_EQ(check_of(client, "at").kind, OutputCheckKind::valid);
    const auto past = check_of(client, "past");
    EXPECT_EQ(past.kind, OutputCheckKind::unverified);
    EXPECT_FALSE(past.budget_spent);
    ASSERT_TRUE(past.issue.has_value());
    EXPECT_FALSE(past.issue->schema_pointer.empty());
    // The tool within the bound is still checked.
    EXPECT_EQ(check_of(client, "at").kind, OutputCheckKind::valid);
}

TEST(ClientBounds, APatternAtTheLengthBoundIsUsedAndOneByteLongerIsUnverified) {
    constexpr std::size_t bound = ClientDefaults::peer_pattern_length;
    const std::string at = "^" + std::string(bound - 2, 'a') + "$";
    const std::string past = "^" + std::string(bound - 1, 'a') + "$";
    ASSERT_EQ(at.size(), bound);
    ASSERT_EQ(past.size(), bound + 1);
    auto script = std::make_shared<ToolServer>();
    script->tools.push_back(tool("at", with_pattern(at)));
    script->tools.push_back(tool("past", with_pattern(past)));
    script->results["at"] = json{{"s", std::string(bound - 2, 'a')}};
    script->results["past"] = json{{"s", std::string(bound - 1, 'a')}};
    neomcp::Client client = open_with(only({revision}), tool_server(script));
    ASSERT_TRUE(client.list_all_tools());
    EXPECT_EQ(check_of(client, "at").kind, OutputCheckKind::valid);
    const auto refused = check_of(client, "past");
    EXPECT_EQ(refused.kind, OutputCheckKind::unverified);
    ASSERT_TRUE(refused.issue.has_value());
    EXPECT_EQ(refused.issue->schema_pointer, "/properties/s/pattern");
}

// The memory of a pattern has no edge a test can name in bytes: the edge is found as the longest run of word characters
// whose program fits, with the client's limits, and the run one longer, still within the length bound, is refused.
TEST(ClientBounds, APatternWhoseProgramFitsTheMemoryBoundIsUsedAndOneRepetitionMoreIsUnverified) {
    constexpr std::size_t high = 10'000;
    ASSERT_LE(word_characters(high).size(), ClientDefaults::peer_pattern_length);
    ASSERT_TRUE(compiles(with_pattern(word_characters(1))));
    ASSERT_FALSE(compiles(with_pattern(word_characters(high))));
    const std::size_t edge =
        largest_fitting(1, high, [](std::size_t n) { return compiles(with_pattern(word_characters(n))); });
    auto script = std::make_shared<ToolServer>();
    script->tools.push_back(tool("fits", with_pattern(word_characters(edge))));
    script->tools.push_back(tool("outgrows", with_pattern(word_characters(edge + 1))));
    neomcp::Client client = open_with(only({revision}), tool_server(script));
    ASSERT_TRUE(client.list_all_tools());
    EXPECT_EQ(check_of(client, "fits").kind, OutputCheckKind::valid);
    const auto refused = check_of(client, "outgrows");
    EXPECT_EQ(refused.kind, OutputCheckKind::unverified);
    ASSERT_TRUE(refused.issue.has_value());
    EXPECT_EQ(refused.issue->schema_pointer, "/properties/s/pattern");
    EXPECT_NE(refused.issue->message.find("max_pattern_memory"), std::string::npos) << refused.issue->message;
    RecordProperty("repetitions_at_the_memory_bound", static_cast<int>(edge));
}

// One validation evaluates the root, the property and each item: an array of bound - 2 items is checked to its end;
// one more item stops the check, which leaves the content unverified rather than invalid.
TEST(ClientBounds, ContentThatTakesTheBoundOfEvaluationsIsCheckedAndOneMoreLeavesItUnverified) {
    constexpr std::size_t bound = 1000;
    const json schema = json::parse(
        R"({"type":"object","properties":{"a":{"type":"array","items":{"type":"integer"}}}})");
    auto script = std::make_shared<ToolServer>();
    script->tools.push_back(tool("at", schema));
    script->tools.push_back(tool("past", schema));
    script->results["at"] = json{{"a", json(std::vector<int>(bound - 2, 0))}};
    script->results["past"] = json{{"a", json(std::vector<int>(bound - 1, 0))}};
    auto options = only({revision});
    options.peer_max_evaluations = bound;
    neomcp::Client client = open_with(std::move(options), tool_server(script));
    ASSERT_TRUE(client.list_all_tools());
    EXPECT_EQ(check_of(client, "at").kind, OutputCheckKind::valid);
    const auto stopped = check_of(client, "past");
    EXPECT_EQ(stopped.kind, OutputCheckKind::unverified);
    EXPECT_FALSE(stopped.budget_spent);
    ASSERT_TRUE(stopped.issue.has_value());
    EXPECT_TRUE(stopped.issue->unchecked);
    // A typed call of the same content is unverified too, not invalid.
    const auto typed = client.call<json>("past", json::object());
    ASSERT_FALSE(typed);
    EXPECT_EQ(typed.error().kind, ClientErrorKind::unverified);
    // Content that fails within the bound is still invalid.
    script->results["past"] = json{{"a", json::array({"x"})}};
    EXPECT_EQ(check_of(client, "past").kind, OutputCheckKind::invalid);
}

TEST(ClientBounds, TheBudgetOfCompilesIsSpentAtItsBoundAndToldOnce) {
    constexpr std::size_t bound = ClientDefaults::max_peer_schema_compiles;
    auto script = std::make_shared<ToolServer>();
    for (std::size_t i = 0; i <= bound; ++i) {
        script->tools.push_back(tool("t" + std::to_string(i), json::parse(R"({"type":"object"})")));
    }
    Heard heard;
    auto options = only({revision});
    heard.listen(options);
    neomcp::Client client = open_with(std::move(options), tool_server(script));
    ASSERT_TRUE(client.list_all_tools());
    for (std::size_t i = 0; i < bound; ++i) {
        ASSERT_EQ(check_of(client, "t" + std::to_string(i)).kind, OutputCheckKind::valid) << i;
    }
    EXPECT_EQ(heard.count(neomcp::DiagnosticKind::schema_budget_spent), 0u);
    const auto past = check_of(client, "t" + std::to_string(bound));
    EXPECT_EQ(past.kind, OutputCheckKind::unverified);
    EXPECT_TRUE(past.budget_spent);
    EXPECT_FALSE(past.issue.has_value());
    // Listing again gives nothing back; the schemas compiled stay compiled.
    ASSERT_TRUE(client.list_all_tools());
    EXPECT_TRUE(check_of(client, "t" + std::to_string(bound)).budget_spent);
    EXPECT_EQ(check_of(client, "t0").kind, OutputCheckKind::valid);
    EXPECT_EQ(heard.count(neomcp::DiagnosticKind::schema_budget_spent), 1u);
}

// ---- A schema at every peer limit at once

// The costliest schema the client compiles for a server: the patterns per schema, each as long as the length bound
// allows and as large as the memory bound allows, at the depth bound. The time it takes is printed (it is a
// measurement, not a bound of the test). One past each bound is refused, as the tests above show.
TEST(ClientBounds, ASchemaAtEveryPeerLimitCompiles) {
    constexpr std::size_t patterns = ClientDefaults::peer_patterns_per_schema;
    constexpr std::size_t length = ClientDefaults::peer_pattern_length;
    // Pattern i: a literal that makes it distinct, padding up to the length bound, and the longest run of word
    // characters whose program then fits the memory bound. The literal and the padding together keep one length, so
    // every pattern takes the same run.
    const auto pattern = [](std::size_t index, std::size_t count) {
        const std::string run = word_characters(count);
        std::string text = "i" + std::to_string(index) + "-";
        constexpr std::size_t whole = ClientDefaults::peer_pattern_length;
        if (text.size() + run.size() < whole) text += std::string(whole - text.size() - run.size(), 'p');
        return text + run;
    };
    const std::size_t count = largest_fitting(
        1, 10'000, [&](std::size_t n) { return compiles(with_pattern(pattern(patterns - 1, n))); });
    std::vector<std::string> chosen;
    for (std::size_t i = 0; i < patterns; ++i) {
        chosen.push_back(pattern(i, count));
        ASSERT_EQ(chosen.back().size(), length) << i;
    }
    // At the bottom of an `items` chain, so that the patterns sit at the depth bound.
    json properties = json::object();
    for (std::size_t i = 0; i < patterns; ++i) {
        properties["p" + std::to_string(i)] = json{{"type", "string"}, {"pattern", chosen[i]}};
    }
    json bottom = json{{"type", "object"}, {"properties", std::move(properties)}};
    for (std::size_t level = 0; level + 2 < ClientDefaults::peer_schema_depth; ++level) {
        bottom = json{{"items", std::move(bottom)}};
    }
    json root_properties = json::object();
    root_properties["a"] = std::move(bottom);
    const json schema{{"type", "object"}, {"properties", std::move(root_properties)}};

    const auto started = std::chrono::steady_clock::now();
    const auto compiled = neomcp::json_schema::Validator::compile(schema, peer_limits());
    const auto took = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(std::holds_alternative<neomcp::json_schema::Validator>(compiled))
        << std::get<neomcp::json_schema::Issue>(compiled).message;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(took).count();
    std::printf("one schema at every peer limit compiled in %lld us\n", static_cast<long long>(micros));
    RecordProperty("compile_microseconds", static_cast<int>(micros));
}

// ---- Header marks

// On a transport that mirrors marked arguments, a tool's input schema is searched for marks to the depth bound: a
// schema that nests deeper is left out (with or without marks), as one whose marks cannot be checked.
TEST(ClientBounds, AnInputSchemaAtTheHeaderDepthBoundIsKeptAndOneLevelDeeperIsLeftOut) {
    constexpr std::size_t bound = neomcp::default_header_schema_depth;
    auto script = std::make_shared<ToolServer>();
    script->tools.push_back(tool("at", std::nullopt, schema_of_depth(bound)));
    script->tools.push_back(tool("past", std::nullopt, schema_of_depth(bound + 1)));
    Heard heard;
    auto options = only({revision});
    heard.listen(options);
    neomcp::Client client = open_with(std::move(options), tool_server(script), Transport::streamable_http);
    const auto listed = client.list_all_tools();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed->items.size(), 1u);
    EXPECT_EQ(listed->items[0].name, "at");
    EXPECT_EQ(heard.dropped(), std::vector<std::string>{"past"});
}

// ---- The bounds of a listing, with their defaults

TEST(ClientBounds, AListingOfThePageBoundIsWholeAndOnePageMoreIsAnError) {
    constexpr std::size_t bound = ClientDefaults::max_list_pages;
    auto pages = std::make_shared<std::size_t>(bound);
    auto peer = std::make_shared<ScriptedPeer>([pages](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request)) return;
        if (request.method() != "tools/list") return;
        const json& params = request.message["params"];
        const std::size_t page = params.contains("cursor") ? std::stoul(params["cursor"].get<std::string>()) : 1;
        json list = json::array();
        list.push_back(tool("t" + std::to_string(page)));
        if (page < *pages) {
            side.answer(request, tools_page(std::move(list), json(std::to_string(page + 1))));
        } else {
            side.answer(request, tools_page(std::move(list)));
        }
    });
    neomcp::Client client = open_with(only({revision}), peer);
    const auto whole = client.list_all_tools();
    ASSERT_TRUE(whole) << whole.error().message;
    EXPECT_EQ(whole->items.size(), bound);
    EXPECT_EQ(peer->count("tools/list"), bound);
    *pages = bound + 1;
    const auto refused = client.list_all_tools();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::list_pages_exceeded);
    EXPECT_EQ(peer->count("tools/list"), 2 * bound);  // the page past the bound is never asked for
}

TEST(ClientBounds, AListingOfTheItemBoundIsWholeAndOneItemMoreIsAnError) {
    constexpr std::size_t bound = ClientDefaults::max_list_items;
    auto items = std::make_shared<std::size_t>(bound);
    auto peer = std::make_shared<ScriptedPeer>([items](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request)) return;
        if (request.method() != "tools/list") return;
        json list = json::array();
        for (std::size_t i = 0; i < *items; ++i) list.push_back(tool("t" + std::to_string(i)));
        side.answer(request, tools_page(std::move(list)));
    });
    neomcp::Client client = open_with(only({revision}), peer);
    const auto whole = client.list_all_tools();
    ASSERT_TRUE(whole) << whole.error().message;
    EXPECT_EQ(whole->items.size(), bound);
    *items = bound + 1;
    const auto refused = client.list_all_tools();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ClientErrorKind::list_items_exceeded);
    // The client goes on.
    *items = 1;
    EXPECT_TRUE(client.list_all_tools());
}

}  // namespace
