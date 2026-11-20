// The typed layer of the Client: typed tool calls with the check of their structured content, lists with their pages
// and bounds, resources, prompts and completion, the budget and bounds of a server's schemas, and the tools left out
// for marks that break the header rules. Against the library's own Server on every revision and transport, and against
// scripted peers for what that server never sends.
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

#include "neomcp/client.h"
#include "neomcp/client/in_process.h"
#include "neomcp/json_schema.h"
#include "neomcp/server.h"
#include "support.h"

namespace typed_pair {

struct AddArgs {
    int a;
    int b;
};

struct Point {
    int x;
    int y;
};

struct ReviewArgs {
    std::string title;
    std::optional<std::string> focus;
};

struct FileParams {
    std::string path;
};

}  // namespace typed_pair

NEOMCP_DESCRIBE(typed_pair::AddArgs, a, "first addend", b, "second addend")
NEOMCP_DESCRIBE(typed_pair::Point, x, "horizontal position", y, "vertical position")
NEOMCP_DESCRIBE(typed_pair::ReviewArgs, title, "what to review", focus, "what to look at")
NEOMCP_DESCRIBE(typed_pair::FileParams, path, "the file's path")

namespace {

using namespace client_test;
using neomcp::ClientErrorKind;
using neomcp::OutputCheckKind;
using neomcp::profile::Transport;
using typed_pair::AddArgs;
using typed_pair::Point;

constexpr std::string_view handshake_revision = "2025-11-25";
constexpr std::string_view per_request_revision = "2026-07-28";

// ---------------------------------------------------------------------------------------------------------------
// The library's own server

constexpr std::size_t tool_count = 5;
constexpr std::size_t tools_page_size = 2;
constexpr std::size_t tool_pages = 3;  // 2 + 2 + 1
constexpr std::uint64_t list_ttl = 5000;
constexpr std::uint64_t read_ttl = 7000;

struct Counts {
    std::atomic<int> adds{0};
};

neomcp::ServerOptions served(std::string_view revision) {
    neomcp::ServerOptions options;
    options.name = "typed-server";
    options.version = "1.0.0";
    options.profiles.push_back(neomcp::profile::builtin(revision));
    options.tools_page_size = tools_page_size;
    options.tools_cache = neomcp::CacheHints{list_ttl, neomcp::CacheScope::public_scope};
    options.resources_page_size = 1;
    options.resources_cache = neomcp::CacheHints{list_ttl, neomcp::CacheScope::public_scope};
    options.resource_read_cache = neomcp::CacheHints{read_ttl, neomcp::CacheScope::public_scope};
    options.prompts_page_size = 1;
    return options;
}

void populate(neomcp::Server& server, const std::shared_ptr<Counts>& counts) {
    server.tool<AddArgs>("add", "Add two integers", [counts](const AddArgs& args) {
        ++counts->adds;
        return args.a + args.b;
    });
    server.tool<AddArgs>("point", "A point", [](const AddArgs& args) { return Point{args.a, args.b}; });
    server.tool("failing", "Reports a failure of its own", [] {
        neomcp::ToolResult result;
        neomcp::TextContent text;
        text.text = "it failed";
        result.content.emplace_back(text);
        result.is_error = true;
        return result;
    });
    server.tool("third", "A third tool", [] { return std::string("three"); });
    server.tool("fourth", "A fourth tool", [] { return std::string("four"); });

    server.resource("config://app", "App configuration", [] { return std::string("on"); });
    server.resource("config://other", "Other configuration", [] { return std::string("off"); });
    server
        .resource_template<typed_pair::FileParams>(
            "file:///{path}", "A file", [](const typed_pair::FileParams& file) { return "file " + file.path; })
        .complete("path", [](std::string_view typed) { return std::vector<std::string>{std::string(typed) + ".txt"}; });
    server
        .prompt<typed_pair::ReviewArgs>("review", "Review a change",
                                        [](const typed_pair::ReviewArgs& review) { return "Review " + review.title; })
        .complete("title", [](std::string_view typed, const std::map<std::string, std::string>& resolved) {
            return std::vector<std::string>{std::string(typed) + "-" + std::to_string(resolved.size())};
        });
}

class Pair {
public:
    Pair(std::string_view revision, Transport transport, neomcp::ClientOptions options)
        : counts(std::make_shared<Counts>()), server_(served(revision)) {
        populate(server_, counts);
        peer = neomcp::InProcessServer::create(server_, transport);
        auto opened = neomcp::Client::open(std::move(options), peer->transport());
        if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
        client.emplace(std::move(opened).value());
    }
    Pair(std::string_view revision, Transport transport) : Pair(revision, transport, only({revision})) {}

    std::shared_ptr<Counts> counts;

private:
    neomcp::Server server_;

public:
    std::shared_ptr<neomcp::InProcessServer> peer;
    std::optional<neomcp::Client> client;
};

struct Cell {
    std::string revision;
    Transport transport;
};

std::string cell_name(const testing::TestParamInfo<Cell>& info) {
    std::string name = info.param.revision + (info.param.transport == Transport::stdio ? "_stdio" : "_http");
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

std::vector<Cell> cells() {
    std::vector<Cell> all;
    for (const auto revision : neomcp::profile::builtin_revisions()) {
        const auto profile = neomcp::profile::builtin(revision);
        if (profile->stdio()) all.push_back(Cell{std::string(revision), Transport::stdio});
        if (profile->streamable_http()) all.push_back(Cell{std::string(revision), Transport::streamable_http});
    }
    return all;
}

// What the revision's wire tables say.
bool has_member(std::string_view revision, std::string_view type, std::string_view member) {
    const auto* found = neomcp::profile::builtin(revision)->type(type);
    return found != nullptr && found->has_property(member);
}
bool structured(std::string_view revision) { return has_member(revision, "CallToolResult", "structuredContent"); }
bool has_request(std::string_view revision, std::string_view method) {
    return neomcp::profile::builtin(revision)->request(method) != nullptr;
}
bool mirrors(std::string_view revision, Transport transport) {
    const auto& http = neomcp::profile::builtin(revision)->streamable_http();
    return transport == Transport::streamable_http && http && http->argument_headers;
}

std::string text_of(const neomcp::ContentBlock& block) {
    const auto* text = std::get_if<neomcp::TextContent>(&block);
    return text == nullptr ? "(not text)" : text->text;
}

class ClientTypedPair : public testing::TestWithParam<Cell> {};

TEST_P(ClientTypedPair, TypedCallsDecodeTheCheckedContent) {
    const Cell cell = GetParam();
    Pair pair(cell.revision, cell.transport);
    neomcp::Client& client = *pair.client;
    if (!structured(cell.revision)) {
        const auto sum = client.call<int>("add", AddArgs{1, 2});
        ASSERT_FALSE(sum);
        EXPECT_EQ(sum.error().kind, ClientErrorKind::no_structured_results);
        EXPECT_EQ(pair.counts->adds.load(), 0);
        return;
    }
    // The preview: the tool is listed first, its output schema compiled and the content checked and decoded.
    auto sum = pair.client->call<int>("add", AddArgs{1, 2});
    ASSERT_TRUE(sum) << sum.error().message;
    EXPECT_FALSE(sum->is_error);
    ASSERT_TRUE(sum->value);
    EXPECT_EQ(*sum->value, 3);
    EXPECT_EQ(sum->check.kind, OutputCheckKind::valid);

    const auto point = client.call<Point>("point", AddArgs{4, 5});
    ASSERT_TRUE(point) << point.error().message;
    ASSERT_TRUE(point->value);
    EXPECT_EQ(point->value->x, 4);
    EXPECT_EQ(point->value->y, 5);
    EXPECT_EQ(point->check.kind, OutputCheckKind::valid);

    // A tool's own failure is a value, with nothing decoded.
    const auto failing = client.call<int>("failing", neomcp::json::object());
    ASSERT_TRUE(failing) << failing.error().message;
    EXPECT_TRUE(failing->is_error);
    EXPECT_FALSE(failing->value);
    ASSERT_FALSE(failing->content.empty());
    EXPECT_EQ(text_of(failing->content[0]), "it failed");

    // The raw call gives the check as data.
    const auto raw = client.call_tool("add", json{{"a", 2}, {"b", 2}});
    ASSERT_TRUE(raw) << raw.error().message;
    EXPECT_EQ(raw->check.kind, OutputCheckKind::valid);
    ASSERT_TRUE(raw->structured_content);

    // The coroutine form, from another executor, resumes there.
    asio::io_context other;
    std::optional<neomcp::Result<neomcp::TypedCall<int>>> awaited;
    bool resumed_there = false;
    asio::co_spawn(
        other,
        [&]() -> asio::awaitable<void> {
            auto result = co_await pair.client->async_call<int>("add", AddArgs{20, 22});
            resumed_there = other.get_executor().running_in_this_thread();
            awaited.emplace(std::move(result));
        },
        asio::detached);
    other.run();
    ASSERT_TRUE(awaited);
    ASSERT_TRUE(*awaited) << awaited->error().message;
    ASSERT_TRUE((*awaited)->value);
    EXPECT_EQ(*(*awaited)->value, 42);
    EXPECT_TRUE(resumed_there);
}

TEST_P(ClientTypedPair, ARawCallIsCheckedAgainstTheLatestList) {
    const Cell cell = GetParam();
    Pair pair(cell.revision, cell.transport);
    neomcp::Client& client = *pair.client;
    const auto before = client.call_tool("add", json{{"a", 1}, {"b", 1}});
    ASSERT_TRUE(before) << before.error().message;
    if (!structured(cell.revision)) {
        EXPECT_EQ(before->check.kind, OutputCheckKind::not_applicable);
        return;
    }
    // A transport that mirrors arguments lists the tools before the first call of a tool; another does not.
    EXPECT_EQ(before->check.kind, mirrors(cell.revision, cell.transport) ? OutputCheckKind::valid
                                                                         : OutputCheckKind::not_listed);
    ASSERT_TRUE(client.list_all_tools());
    const auto after = client.call_tool("add", json{{"a", 1}, {"b", 1}});
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after->check.kind, OutputCheckKind::valid);
    // A tool without an output schema.
    const auto plain = client.call_tool("third", json::object());
    ASSERT_TRUE(plain) << plain.error().message;
    EXPECT_EQ(plain->check.kind, OutputCheckKind::not_applicable);
}

TEST_P(ClientTypedPair, ListsFollowEveryPage) {
    const Cell cell = GetParam();
    Pair pair(cell.revision, cell.transport);
    neomcp::Client& client = *pair.client;
    const bool hints = has_member(cell.revision, "ListToolsResult", "ttlMs");

    std::vector<std::string> names;
    std::optional<std::string> cursor;
    std::size_t pages = 0;
    do {
        auto page = client.list_tools(cursor);
        ASSERT_TRUE(page) << page.error().message;
        ++pages;
        EXPECT_LE(page->items.size(), tools_page_size);
        for (const auto& tool : page->items) names.push_back(tool.name);
        EXPECT_EQ(page->cache.has_value(), hints);
        if (page->cache) {
            EXPECT_EQ(page->cache->ttl_ms, list_ttl);
            EXPECT_EQ(page->cache->scope, neomcp::CacheScope::public_scope);
        }
        cursor = page->next_cursor;
    } while (cursor && pages <= tool_pages);
    EXPECT_EQ(pages, tool_pages);
    EXPECT_EQ(names.size(), tool_count);

    const auto all = client.list_all_tools();
    ASSERT_TRUE(all) << all.error().message;
    EXPECT_EQ(all->items.size(), tool_count);
    EXPECT_EQ(all->page_cache.size(), tool_pages);
    for (const auto& hint : all->page_cache) {
        EXPECT_EQ(hint.has_value(), hints);
    }
    bool output_schema = false;
    for (const auto& tool : all->items) {
        if (tool.name == "add") output_schema = tool.output_schema.has_value();
    }
    EXPECT_EQ(output_schema, has_member(cell.revision, "Tool", "outputSchema"));
}

TEST_P(ClientTypedPair, TheListBoundsAreExactAndGiveNoPartialList) {
    const Cell cell = GetParam();
    {
        auto options = only({cell.revision});
        options.max_list_pages = tool_pages;
        options.max_list_items = tool_count;
        Pair pair(cell.revision, cell.transport, std::move(options));
        const auto all = pair.client->list_all_tools();
        ASSERT_TRUE(all) << all.error().message;
        EXPECT_EQ(all->items.size(), tool_count);
    }
    {
        auto options = only({cell.revision});
        options.max_list_pages = tool_pages - 1;
        Pair pair(cell.revision, cell.transport, std::move(options));
        const auto all = pair.client->list_all_tools();
        ASSERT_FALSE(all);
        EXPECT_EQ(all.error().kind, ClientErrorKind::list_pages_exceeded);
    }
    {
        auto options = only({cell.revision});
        options.max_list_items = tool_count - 1;
        Pair pair(cell.revision, cell.transport, std::move(options));
        const auto all = pair.client->list_all_tools();
        ASSERT_FALSE(all);
        EXPECT_EQ(all.error().kind, ClientErrorKind::list_items_exceeded);
    }
}

TEST_P(ClientTypedPair, ResourcesPromptsAndCompletion) {
    const Cell cell = GetParam();
    Pair pair(cell.revision, cell.transport);
    neomcp::Client& client = *pair.client;

    const auto resources = client.list_all_resources();
    ASSERT_TRUE(resources) << resources.error().message;
    ASSERT_EQ(resources->items.size(), 2u);
    EXPECT_EQ(resources->page_cache.size(), 2u);
    EXPECT_EQ(resources->items[0].uri, "config://app");
    const bool list_hints = has_member(cell.revision, "ListResourcesResult", "ttlMs");
    for (const auto& hint : resources->page_cache) {
        EXPECT_EQ(hint.has_value(), list_hints);
    }

    const auto first = client.list_resources();
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->items.size(), 1u);
    EXPECT_TRUE(first->next_cursor);

    const auto read = client.read_resource("config://app");
    ASSERT_TRUE(read) << read.error().message;
    ASSERT_EQ(read->contents.size(), 1u);
    const auto* text = std::get_if<neomcp::TextResourceContents>(&read->contents[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "on");
    EXPECT_EQ(read->cache.has_value(), has_member(cell.revision, "ReadResourceResult", "ttlMs"));
    if (read->cache) {
        EXPECT_EQ(read->cache->ttl_ms, read_ttl);
    }

    const auto from_template = client.read_resource("file:///notes");
    ASSERT_TRUE(from_template) << from_template.error().message;

    const auto missing = client.read_resource("config://none");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().kind, ClientErrorKind::server_error);

    const auto templates = client.list_all_resource_templates();
    ASSERT_TRUE(templates) << templates.error().message;
    ASSERT_EQ(templates->items.size(), 1u);
    EXPECT_EQ(templates->items[0].uri_template, "file:///{path}");

    const auto prompts = client.list_all_prompts();
    ASSERT_TRUE(prompts) << prompts.error().message;
    ASSERT_EQ(prompts->items.size(), 1u);
    EXPECT_EQ(prompts->items[0].name, "review");

    const auto prompt = client.get_prompt("review", {{"title", "the change"}});
    ASSERT_TRUE(prompt) << prompt.error().message;
    ASSERT_EQ(prompt->messages.size(), 1u);
    EXPECT_EQ(text_of(prompt->messages[0].content), "Review the change");

    const auto unknown = client.get_prompt("no-such-prompt");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().kind, ClientErrorKind::server_error);

    if (!has_request(cell.revision, "completion/complete")) return;
    const auto paths = client.complete(neomcp::CompletionRef::resource("file:///{path}"), "path", "no");
    ASSERT_TRUE(paths) << paths.error().message;
    ASSERT_EQ(paths->values.size(), 1u);
    EXPECT_EQ(paths->values[0], "no.txt");

    // The resolved arguments go where the revision's request has a place for them.
    const auto titles = client.complete(neomcp::CompletionRef::prompt("review"), "title", "ab", {{"focus", "tests"}});
    ASSERT_TRUE(titles) << titles.error().message;
    ASSERT_EQ(titles->values.size(), 1u);
    EXPECT_EQ(titles->values[0], has_member(cell.revision, "CompleteRequestParams", "context") ? "ab-1" : "ab-0");
}

INSTANTIATE_TEST_SUITE_P(ClientTypedEveryRevision, ClientTypedPair, testing::ValuesIn(cells()), cell_name);

// The form of the approved preview, blocking and as a coroutine.
TEST(ClientTyped, ThePreviewCompilesAndRuns) {
    neomcp::Server server(served(handshake_revision));
    populate(server, std::make_shared<Counts>());
    auto peer = neomcp::InProcessServer::create(server);
    neomcp::ClientOptions options = only({handshake_revision});
    auto client = neomcp::Client::open(options, peer->transport());
    ASSERT_TRUE(client) << client.error().message;
    auto sum = client->call<int>("add", AddArgs{1, 2});
    ASSERT_TRUE(sum) << sum.error().message;
    EXPECT_EQ(sum->value, 3);
    asio::io_context context;
    std::optional<int> awaited;
    asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            auto again = co_await client->async_call<int>("add", AddArgs{1, 2});
            if (again && again->value) awaited = *again->value;
        },
        asio::detached);
    context.run();
    EXPECT_EQ(awaited, 3);
}

TEST(ClientTyped, ListBoundsMustBePositive) {
    for (const bool pages : {true, false}) {
        neomcp::Server server(served(handshake_revision));
        auto peer = neomcp::InProcessServer::create(server);
        auto options = only({handshake_revision});
        if (pages) {
            options.max_list_pages = 0;
        } else {
            options.max_list_items = 0;
        }
        const auto opened = neomcp::Client::open(std::move(options), peer->transport());
        ASSERT_FALSE(opened);
        EXPECT_EQ(opened.error().kind, ClientErrorKind::invalid_options);
        EXPECT_NE(opened.error().message.find(pages ? "max_list_pages" : "max_list_items"), std::string::npos);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Scripted peers: what the library's server never sends

// Answers what opening needs; false for anything else.
bool answer_opening(ScriptedPeer& peer, const Seen& request, std::string_view revision) {
    if (request.method() == "initialize") {
        peer.answer(request, initialize_result(revision));
        return true;
    }
    if (request.method() == "server/discover") {
        peer.answer(request, discover_result(revision));
        return true;
    }
    if (request.method() == "ping") {
        peer.answer(request, json::object());
        return true;
    }
    return false;
}

neomcp::Client open_with(neomcp::ClientOptions options, const std::shared_ptr<ScriptedPeer>& peer,
                         Transport kind = Transport::stdio) {
    if (kind == Transport::streamable_http) peer->http = true;
    auto opened = neomcp::Client::open(std::move(options), neomcp::make_memory_transport(kind, peer));
    if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
    return std::move(opened).value();
}

// A page of tools at the per-request revision, which requires the caching hints.
json tools_page(json tools, std::optional<json> next = std::nullopt) {
    json page = json{{"tools", std::move(tools)}, {"ttlMs", 0}, {"cacheScope", "private"}, {"resultType", "complete"}};
    if (next) page["nextCursor"] = std::move(*next);
    return page;
}

json tool(std::string_view name, std::optional<json> output_schema = std::nullopt) {
    json definition = json{{"name", std::string(name)}, {"inputSchema", json{{"type", "object"}}}};
    if (output_schema) definition["outputSchema"] = std::move(*output_schema);
    return definition;
}

json one_tool(std::string_view name) {
    json list = json::array();
    list.push_back(tool(name));
    return list;
}

json structured_result(json content) {
    return json{{"content", json::array()}, {"structuredContent", std::move(content)}, {"resultType", "complete"}};
}

json integer_result_schema() {
    return json::parse(R"({"type":"object","properties":{"result":{"type":"integer"}},"required":["result"],)"
                       R"("additionalProperties":false})");
}

std::string cursor_of(const Seen& request) {
    const json& params = request.message["params"];
    return params.contains("cursor") ? params["cursor"].get<std::string>() : std::string("(none)");
}

// The diagnostics of a client, as they arrive.
struct Heard {
    std::mutex mutex;
    std::vector<neomcp::ClientDiagnostic> all;

    void listen(neomcp::ClientOptions& options) {
        options.on_diagnostic = [this](const neomcp::ClientDiagnostic& diagnostic) {
            std::lock_guard lock(mutex);
            all.push_back(diagnostic);
        };
    }
    std::vector<neomcp::ToolDropped> dropped() {
        std::lock_guard lock(mutex);
        std::vector<neomcp::ToolDropped> found;
        for (const auto& one : all) {
            if (const auto* drop = std::get_if<neomcp::ToolDropped>(&one)) found.push_back(*drop);
        }
        return found;
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
};

TEST(ClientTyped, AnEmptyCursorContinuesAndANullOneEnds) {
    std::vector<std::string> cursors;
    std::mutex mutex;
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() != "tools/list") return;
        const std::string cursor = cursor_of(request);
        {
            std::lock_guard lock(mutex);
            cursors.push_back(cursor);
        }
        if (cursor == "(none)") {
            side.answer(request, tools_page(one_tool("first"), json("")));
        } else {
            side.answer(request, tools_page(one_tool("second"), json(nullptr)));
        }
    });
    neomcp::Client client = open_with(only({per_request_revision}), peer);
    const auto first = client.list_tools();
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(first->next_cursor);
    EXPECT_EQ(*first->next_cursor, "");
    const auto all = client.list_all_tools();
    ASSERT_TRUE(all) << all.error().message;
    ASSERT_EQ(all->items.size(), 2u);
    EXPECT_EQ(all->items[1].name, "second");
    std::lock_guard lock(mutex);
    ASSERT_EQ(cursors.size(), 3u);
    EXPECT_EQ(cursors[1], "(none)");
    EXPECT_EQ(cursors[2], "");
}

TEST(ClientTyped, ACursorLoopEndsAtThePageBound) {
    constexpr std::size_t bound = 4;
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/list") side.answer(request, tools_page(one_tool("again"), json("same")));
    });
    auto options = only({per_request_revision});
    options.max_list_pages = bound;
    neomcp::Client client = open_with(std::move(options), peer);
    const auto all = client.list_all_tools();
    ASSERT_FALSE(all);
    EXPECT_EQ(all.error().kind, ClientErrorKind::list_pages_exceeded);
    EXPECT_EQ(peer->count("tools/list"), bound);
}

TEST(ClientTyped, AMalformedPageIsAMalformedAnswer) {
    std::atomic<int> variant{0};
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() != "tools/list") return;
        switch (variant.load()) {
            case 0: side.answer(request, tools_page(one_tool("t"), json(5))); break;
            case 1: side.answer(request, tools_page(json("not a list"))); break;
            case 2: {
                json list = json::array();
                list.push_back(json{{"inputSchema", json{{"type", "object"}}}});
                side.answer(request, tools_page(std::move(list)));
                break;
            }
            default: {
                json page = tools_page(one_tool("t"));
                page["ttlMs"] = "soon";
                side.answer(request, std::move(page));
                break;
            }
        }
    });
    neomcp::Client client = open_with(only({per_request_revision}), peer);
    for (int one = 0; one < 4; ++one) {
        SCOPED_TRACE(one);
        variant = one;
        const auto page = client.list_tools();
        ASSERT_FALSE(page);
        EXPECT_EQ(page.error().kind, ClientErrorKind::malformed_answer);
    }
}

// A peer that lists `tools` and answers each call of a tool with the structured content of `results`; any other call
// with a result that asks for a form.
struct ToolServer {
    json tools;
    std::map<std::string, json> results;
};

std::shared_ptr<ScriptedPeer> tool_server(std::shared_ptr<ToolServer> script) {
    return std::make_shared<ScriptedPeer>([script](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/list") {
            side.answer(request, tools_page(script->tools));
            return;
        }
        if (request.method() != "tools/call") return;
        const std::string name = request.message["params"]["name"].get<std::string>();
        const auto found = script->results.find(name);
        if (found != script->results.end()) {
            side.answer(request, structured_result(found->second));
            return;
        }
        const json form = json::parse(R"({"type":"object","properties":{"ok":{"type":"boolean"}}})");
        json requests = json::object();
        requests["k"] = json{{"method", "elicitation/create"},
                             {"params", json{{"mode", "form"}, {"message", "Sure?"}, {"requestedSchema", form}}}};
        side.answer(request,
                    json{{"resultType", "input_required"}, {"inputRequests", requests}, {"requestState", "st"}});
    });
}

TEST(ClientTyped, ContentThatFailsItsOutputSchemaFailsATypedCall) {
    auto script = std::make_shared<ToolServer>();
    script->tools = json::array();
    script->tools.push_back(tool("count", integer_result_schema()));
    script->results["count"] = json{{"result", "three"}};
    auto peer = tool_server(script);
    neomcp::Client client = open_with(only({per_request_revision}), peer);

    const auto typed = client.call<int>("count", json::object());
    ASSERT_FALSE(typed);
    EXPECT_EQ(typed.error().kind, ClientErrorKind::output_invalid);
    EXPECT_EQ(peer->count("tools/list"), 1u);

    const auto raw = client.call_tool("count", json::object());
    ASSERT_TRUE(raw) << raw.error().message;
    EXPECT_EQ(raw->check.kind, OutputCheckKind::invalid);
    ASSERT_TRUE(raw->check.issue);
    EXPECT_EQ(raw->check.issue->instance_pointer, "/result");
    // The tools were listed once: the typed call found the tool known.
    EXPECT_EQ(peer->count("tools/list"), 1u);
}

TEST(ClientTyped, AnOutputSchemaThatCannotBeCompiledLeavesTheResultUnverified) {
    auto script = std::make_shared<ToolServer>();
    script->tools = json::array();
    script->tools.push_back(tool("elsewhere", json::parse(R"({"$ref":"https://example.com/schemas/result.json"})")));
    script->results["elsewhere"] = json{{"result", 3}};
    auto peer = tool_server(script);
    neomcp::Client client = open_with(only({per_request_revision}), peer);

    const auto typed = client.call<int>("elsewhere", json::object());
    ASSERT_FALSE(typed);
    EXPECT_EQ(typed.error().kind, ClientErrorKind::unverified);

    const auto raw = client.call_tool("elsewhere", json::object());
    ASSERT_TRUE(raw) << raw.error().message;
    EXPECT_EQ(raw->check.kind, OutputCheckKind::unverified);
    EXPECT_FALSE(raw->check.budget_spent);
    EXPECT_TRUE(raw->check.issue);
}

TEST(ClientTyped, ATypedCallOfAToolTheServerDoesNotListIsUnverified) {
    auto script = std::make_shared<ToolServer>();
    script->tools = json::array();
    script->results["hidden"] = json{{"result", 3}};
    auto peer = tool_server(script);
    neomcp::Client client = open_with(only({per_request_revision}), peer);
    const auto typed = client.call<int>("hidden", json::object());
    ASSERT_FALSE(typed);
    EXPECT_EQ(typed.error().kind, ClientErrorKind::unverified);
    const auto raw = client.call_tool("hidden", json::object());
    ASSERT_TRUE(raw);
    EXPECT_EQ(raw->check.kind, OutputCheckKind::not_listed);
}

TEST(ClientTyped, ContentWithoutTheResultMemberIsUndecodable) {
    auto script = std::make_shared<ToolServer>();
    script->tools = json::array();
    script->tools.push_back(tool("loose"));
    script->results["loose"] = json{{"value", 3}};
    auto peer = tool_server(script);
    neomcp::Client client = open_with(only({per_request_revision}), peer);
    const auto typed = client.call<int>("loose", json::object());
    ASSERT_FALSE(typed);
    EXPECT_EQ(typed.error().kind, ClientErrorKind::output_undecodable);
    const auto whole = client.call<neomcp::json>("loose", json::object());
    ASSERT_TRUE(whole) << whole.error().message;
    ASSERT_TRUE(whole->value);
    EXPECT_EQ(whole->check.kind, OutputCheckKind::no_schema);
    EXPECT_EQ((*whole->value)["value"].get<int>(), 3);
}

TEST(ClientTyped, TheCompileBudgetCountsRefusedSchemasAndFormsAndIsNeverGivenBack) {
    auto script = std::make_shared<ToolServer>();
    script->tools = json::array();
    script->tools.push_back(tool("refused", json::parse(R"({"$ref":"https://example.com/other.json"})")));
    script->tools.push_back(tool("fine", integer_result_schema()));
    script->results["refused"] = json{{"result", 1}};
    script->results["fine"] = json{{"result", 2}};
    auto peer = tool_server(script);
    Heard heard;
    std::atomic<int> forms{0};
    auto options = only({per_request_revision});
    options.max_peer_schema_compiles = 1;
    options.handlers.form([&](const neomcp::ElicitationForm&) {
        ++forms;
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
    });
    heard.listen(options);
    neomcp::Client client = open_with(std::move(options), peer);
    ASSERT_TRUE(client.list_all_tools());

    // The refused compile spends the one compile there is.
    const auto refused = client.call_tool("refused", json::object());
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->check.kind, OutputCheckKind::unverified);
    EXPECT_FALSE(refused->check.budget_spent);

    const auto fine = client.call_tool("fine", json::object());
    ASSERT_TRUE(fine);
    EXPECT_EQ(fine->check.kind, OutputCheckKind::unverified);
    EXPECT_TRUE(fine->check.budget_spent);
    const auto typed = client.call<int>("fine", json::object());
    ASSERT_FALSE(typed);
    EXPECT_EQ(typed.error().kind, ClientErrorKind::unverified);

    // Listing again gives nothing back.
    ASSERT_TRUE(client.list_all_tools());
    const auto again = client.call_tool("fine", json::object());
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->check.budget_spent);

    // A form of a round draws on the same budget: it is refused before any handler sees it.
    const auto asking = client.request("tools/call", json{{"name", "asker"}, {"arguments", json::object()}});
    ASSERT_FALSE(asking);
    EXPECT_EQ(asking.error().kind, ClientErrorKind::form_not_restricted);
    EXPECT_EQ(forms.load(), 0);

    EXPECT_EQ(heard.count(neomcp::DiagnosticKind::schema_budget_spent), 1u);
}

TEST(ClientTyped, AFormIsAnsweredWhileTheBudgetLasts) {
    auto script = std::make_shared<ToolServer>();
    script->tools = json::array();
    std::atomic<int> forms{0};
    auto peer = tool_server(script);
    auto options = only({per_request_revision});
    options.max_peer_schema_compiles = 1;
    options.handlers.form([&](const neomcp::ElicitationForm&) {
        ++forms;
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"ok", true}}};
    });
    neomcp::Client client = open_with(std::move(options), peer);
    // The scripted server asks again on every round: the call ends at the bound of rounds, after one compile per
    // round; the first round is within the budget.
    const auto asking = client.request("tools/call", json{{"name", "asker"}, {"arguments", json::object()}});
    ASSERT_FALSE(asking);
    EXPECT_EQ(asking.error().kind, ClientErrorKind::form_not_restricted);
    EXPECT_EQ(forms.load(), 1);
}

json patterned_schema(std::size_t distinct, std::size_t properties) {
    json all = json::object();
    for (std::size_t index = 0; index < properties; ++index) {
        all["p" + std::to_string(index)] =
            json{{"type", "string"}, {"pattern", "^x" + std::to_string(index % distinct) + "$"}};
    }
    return json{{"type", "object"}, {"properties", std::move(all)}};
}

TEST(ClientTyped, AnOutputSchemaWithMorePatternsThanTheBoundIsUnverified) {
    constexpr std::size_t bound = neomcp::ClientDefaults::peer_patterns_per_schema;
    auto script = std::make_shared<ToolServer>();
    script->tools = json::array();
    script->tools.push_back(tool("at_bound", patterned_schema(bound, bound)));
    script->tools.push_back(tool("past_bound", patterned_schema(bound + 1, bound + 1)));
    script->tools.push_back(tool("repeated", patterned_schema(1, bound + 1)));
    for (const auto* name : {"at_bound", "past_bound", "repeated"}) script->results[name] = json::object();
    auto peer = tool_server(script);
    neomcp::Client client = open_with(only({per_request_revision}), peer);
    ASSERT_TRUE(client.list_all_tools());
    const auto at = client.call_tool("at_bound", json::object());
    ASSERT_TRUE(at);
    EXPECT_EQ(at->check.kind, OutputCheckKind::valid);
    const auto past = client.call_tool("past_bound", json::object());
    ASSERT_TRUE(past);
    EXPECT_EQ(past->check.kind, OutputCheckKind::unverified);
    EXPECT_FALSE(past->check.budget_spent);
    const auto repeated = client.call_tool("repeated", json::object());
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->check.kind, OutputCheckKind::valid);
}

TEST(ClientTyped, TheValidatorBoundsDistinctPatternsOnlyWhenAsked) {
    const json schema = patterned_schema(40, 40);
    EXPECT_TRUE(
        std::holds_alternative<neomcp::json_schema::Validator>(neomcp::json_schema::Validator::compile(schema)));
    neomcp::json_schema::Limits limits;
    limits.max_patterns = 39;
    const auto refused = neomcp::json_schema::Validator::compile(schema, limits);
    ASSERT_TRUE(std::holds_alternative<neomcp::json_schema::Issue>(refused));
    EXPECT_NE(std::get<neomcp::json_schema::Issue>(refused).message.find("max_patterns"), std::string::npos);
    limits.max_patterns = 40;
    EXPECT_TRUE(std::holds_alternative<neomcp::json_schema::Validator>(
        neomcp::json_schema::Validator::compile(schema, limits)));
    // patternProperties keys count too.
    json keyed = json::object();
    keyed["^a"] = json{{"type", "string"}};
    keyed["^b"] = json{{"type", "string"}};
    limits.max_patterns = 1;
    EXPECT_TRUE(std::holds_alternative<neomcp::json_schema::Issue>(
        neomcp::json_schema::Validator::compile(json{{"patternProperties", keyed}}, limits)));
}

// ---------------------------------------------------------------------------------------------------------------
// Header marks

json marked_tools(const neomcp::profile::ArgumentHeaders& marks) {
    json good_properties = json::object();
    good_properties["region"] = json{{"type", "string"}, {marks.schema_keyword, "Region"}};
    json bad_properties = json::object();
    bad_properties["ratio"] = json{{"type", "number"}, {marks.schema_keyword, "Ratio"}};
    json list = json::array();
    list.push_back(json{{"name", "good"}, {"inputSchema", json{{"type", "object"}, {"properties", good_properties}}}});
    list.push_back(json{{"name", "bad"}, {"inputSchema", json{{"type", "object"}, {"properties", bad_properties}}}});
    return list;
}

TEST(ClientTyped, ToolsWithBadHeaderMarksAreLeftOutWhereArgumentsAreMirrored) {
    const auto profile = neomcp::profile::builtin(per_request_revision);
    ASSERT_TRUE(profile->streamable_http() && profile->streamable_http()->argument_headers);
    const neomcp::profile::ArgumentHeaders marks = *profile->streamable_http()->argument_headers;
    for (const Transport kind : {Transport::streamable_http, Transport::stdio}) {
        SCOPED_TRACE(kind == Transport::stdio ? "stdio" : "http");
        auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
            if (answer_opening(side, request, per_request_revision)) return;
            if (request.method() == "tools/list") side.answer(request, tools_page(marked_tools(marks)));
            if (request.method() == "tools/call") side.answer(request, structured_result(json::object()));
        });
        Heard heard;
        auto options = only({per_request_revision});
        heard.listen(options);
        neomcp::Client client = open_with(std::move(options), peer, kind);

        // The first call of a tool lists the tools first where its arguments are mirrored.
        const auto called = client.call_tool("good", json{{"region", "eu"}});
        ASSERT_TRUE(called) << called.error().message;
        const bool mirrored = kind == Transport::streamable_http;
        EXPECT_EQ(peer->count("tools/list"), mirrored ? 1u : 0u);
        std::optional<std::string> header;
        for (const auto& seen : peer->written()) {
            if (seen.method() != "tools/call") continue;
            for (const auto& field : seen.headers) {
                if (field.name == marks.header_prefix + "Region") header = field.value;
            }
        }
        EXPECT_EQ(header.has_value(), mirrored);
        if (header) {
            EXPECT_EQ(*header, "eu");
        }

        const auto listed = client.list_tools();
        ASSERT_TRUE(listed) << listed.error().message;
        std::vector<std::string> names;
        for (const auto& one : listed->items) names.push_back(one.name);
        if (mirrored) {
            EXPECT_EQ(names, std::vector<std::string>{"good"});
            const auto dropped = heard.dropped();
            ASSERT_FALSE(dropped.empty());
            EXPECT_EQ(dropped[0].tool, "bad");
            EXPECT_FALSE(dropped[0].issue.rule.empty());
        } else {
            EXPECT_EQ(names, (std::vector<std::string>{"good", "bad"}));
            EXPECT_TRUE(heard.dropped().empty());
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Threads

TEST(ClientTyped, TheBlockingFormsOnTheClientsThreadReturnTheTypedError) {
    const auto profile = neomcp::profile::builtin(per_request_revision);
    const neomcp::profile::ArgumentHeaders marks = *profile->streamable_http()->argument_headers;
    auto peer = std::make_shared<ScriptedPeer>([&](ScriptedPeer& side, const Seen& request) {
        if (answer_opening(side, request, per_request_revision)) return;
        if (request.method() == "tools/list") side.answer(request, tools_page(marked_tools(marks)));
    });
    std::vector<ClientErrorKind> kinds;
    std::mutex mutex;
    neomcp::Client* self = nullptr;
    auto options = only({per_request_revision});
    // A tool left out is reported on the client's thread, where the blocking forms must not wait for themselves.
    options.on_diagnostic = [&](const neomcp::ClientDiagnostic& diagnostic) {
        if (!std::holds_alternative<neomcp::ToolDropped>(diagnostic) || self == nullptr) return;
        const auto page = self->list_tools();
        const auto all = self->list_all_prompts();
        const auto typed = self->call<int>("good", AddArgs{1, 2});
        const auto read = self->read_resource("config://app");
        std::lock_guard lock(mutex);
        for (const auto* error : {page ? nullptr : &page.error(), all ? nullptr : &all.error(),
                                  typed ? nullptr : &typed.error(), read ? nullptr : &read.error()}) {
            kinds.push_back(error == nullptr ? ClientErrorKind::closed : error->kind);
        }
    };
    neomcp::Client client = open_with(std::move(options), peer, Transport::streamable_http);
    self = &client;
    ASSERT_TRUE(client.list_tools());
    std::lock_guard lock(mutex);
    ASSERT_EQ(kinds.size(), 4u);
    for (const auto kind : kinds) {
        EXPECT_EQ(kind, ClientErrorKind::on_client_thread);
    }
}

TEST(ClientTyped, ListsAwaitedFromAnotherContextResumeThere) {
    Pair pair(per_request_revision, Transport::stdio);
    neomcp::Client& client = *pair.client;
    asio::io_context other;
    std::optional<neomcp::Result<neomcp::Listing<neomcp::ToolDefinition>>> tools;
    std::optional<neomcp::Result<neomcp::CompletionResult>> completed;
    int resumed_there = 0;
    asio::co_spawn(
        other,
        [&]() -> asio::awaitable<void> {
            auto listed = co_await client.async_list_all_tools();
            resumed_there += other.get_executor().running_in_this_thread() ? 1 : 0;
            tools.emplace(std::move(listed));
            auto suggested = co_await client.async_complete(neomcp::CompletionRef::prompt("review"), "title", "x");
            resumed_there += other.get_executor().running_in_this_thread() ? 1 : 0;
            completed.emplace(std::move(suggested));
        },
        asio::detached);
    other.run();
    ASSERT_TRUE(tools && *tools);
    EXPECT_EQ((*tools)->items.size(), tool_count);
    ASSERT_TRUE(completed && *completed);
    EXPECT_EQ(resumed_there, 2);
}

TEST(ClientTyped, AMovedFromClientAnswersEveryTypedCallClosed) {
    Pair pair(per_request_revision, Transport::stdio);
    neomcp::Client moved = std::move(*pair.client);
    const auto typed = pair.client->call<int>("add", AddArgs{1, 2});
    ASSERT_FALSE(typed);
    EXPECT_EQ(typed.error().kind, ClientErrorKind::closed);
    const auto listed = pair.client->list_all_tools();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().kind, ClientErrorKind::closed);
    EXPECT_TRUE(moved.call<int>("add", AddArgs{1, 2}));
}

}  // namespace
