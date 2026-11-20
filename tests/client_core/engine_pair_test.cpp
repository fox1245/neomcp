// The client engine paired in process with the server's session (pair.h): every revision, over stdio and the header
// rules of Streamable HTTP, without sockets.
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine_rig.h"
#include "neomcp/server.h"
#include "pair.h"
#include "neomcp/wire_read.h"

namespace pair_test {

struct AddArgs {
    int a;
    int b;
};

struct ReviewArgs {
    std::string title;
    std::optional<std::string> focus;
};

struct Folder {
    std::string path;
};

struct Confirm {
    bool ok;
};

}  // namespace pair_test

NEOMCP_DESCRIBE(pair_test::AddArgs, a, "first addend", b, "second addend")
NEOMCP_DESCRIBE(pair_test::ReviewArgs, title, "what to review", focus, "what to look at")
NEOMCP_DESCRIBE(pair_test::Folder, path, "the folder")
NEOMCP_DESCRIBE(pair_test::Confirm, ok, "Delete it?")

namespace {

using namespace client_core_test;
using neomcp::ContactPhase;
using neomcp::profile::Transport;
// A tool whose arguments are marked for headers, where a revision mirrors arguments into headers.
neomcp::ToolDefinition marked_tool() {
    neomcp::ToolDefinition tool;
    tool.name = "echo";
    tool.description = "Echo the arguments";
    tool.input_schema = json::parse(R"({"type":"object","properties":{
        "region":{"type":"string","x-mcp-header":"Region"},
        "count":{"type":"integer","x-mcp-header":"Count"}}})");
    return tool;
}

void populate(neomcp::Server& server) {
    server.tool<pair_test::AddArgs>("add", "Add two integers",
                                    [](const pair_test::AddArgs& args) { return args.a + args.b; });
    server.tool(marked_tool(), [](const json& arguments) {
        neomcp::ToolResult result;
        neomcp::TextContent text;
        text.text = arguments.dump();
        result.content.emplace_back(text);
        return result;
    });
    server.tool<pair_test::Folder>(
        "delete_folder", "Delete a folder",
        [](const pair_test::Folder& folder, const neomcp::Answer<pair_test::Confirm>& confirm) {
            if (!confirm.accepted() || !confirm->ok) return std::string("kept");
            return "deleted " + folder.path;
        },
        neomcp::elicit<pair_test::Confirm>(
            "confirm", [](const pair_test::Folder& folder) { return "Delete " + folder.path + "?"; }));
    server.resource("config://app", "App configuration", [] { return std::string("on"); });
    server.prompt<pair_test::ReviewArgs>("review", "Review a change",
                                         [](const pair_test::ReviewArgs& args) { return "Review " + args.title; })
        .complete("title", [](std::string_view typed) { return std::vector<std::string>{std::string(typed) + "-1"}; });
}

neomcp::ServerOptions server_options(std::vector<neomcp::profile::ProfilePtr> profiles) {
    neomcp::ServerOptions options;
    options.name = "pair-server";
    options.version = "1.0.0";
    options.profiles = std::move(profiles);
    return options;
}

std::vector<neomcp::profile::ProfilePtr> revisions(std::initializer_list<std::string_view> names) {
    std::vector<neomcp::profile::ProfilePtr> profiles;
    for (const auto name : names) profiles.push_back(neomcp::profile::builtin(name));
    return profiles;
}

// The pair with this file's server.
class EnginePair : public Pair {
public:
    EnginePair(neomcp::EngineOptions options, Transport transport, std::vector<neomcp::profile::ProfilePtr> served = {})
        : Pair(std::move(options), transport, server_options(std::move(served)), populate) {}
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

class ClientEnginePair : public testing::TestWithParam<Cell> {};

const json* find_named(const json& list, std::string_view name) {
    for (const auto& item : list) {
        if (item.contains("name") && item["name"] == std::string(name)) return &item;
    }
    return nullptr;
}

TEST_P(ClientEnginePair, OpensAndServesEveryKindOfRequest) {
    const Cell cell = GetParam();
    EnginePair pair(only({cell.revision}), cell.transport);
    pair.open();
    const auto& contact = pair.rig().engine->contact();
    EXPECT_EQ(contact.verdict->revision, cell.revision);
    EXPECT_EQ(contact.verdict->transport, cell.transport);
    const auto& profile = pair.profile();

    const json tools = pair.result("tools/list", json::object());
    const json* add = find_named(tools["tools"], "add");
    ASSERT_NE(add, nullptr);
    auto definition = neomcp::from_wire<neomcp::ToolDefinition>(*add, profile);
    ASSERT_TRUE(std::holds_alternative<neomcp::ToolDefinition>(definition));
    const json called = pair.result("tools/call", json{{"name", "add"}, {"arguments", json{{"a", 2}, {"b", 3}}}},
                                    std::get<neomcp::ToolDefinition>(definition).input_schema);
    auto read = neomcp::from_wire<neomcp::ToolResult>(called, profile);
    ASSERT_TRUE(std::holds_alternative<neomcp::ToolResult>(read)) << std::get<std::string>(read);
    const auto& tool_result = std::get<neomcp::ToolResult>(read);
    EXPECT_FALSE(tool_result.is_error);
    ASSERT_FALSE(tool_result.content.empty());
    EXPECT_EQ(std::get<neomcp::TextContent>(tool_result.content[0]).text, "5");

    // Arguments mirrored into headers, where the revision does that: the session checks every header.
    const json* echo = find_named(tools["tools"], "echo");
    ASSERT_NE(echo, nullptr);
    const json echoed = pair.result("tools/call",
                                    json{{"name", "echo"}, {"arguments", json{{"region", " eu\twest"}, {"count", 7}}}},
                                    (*echo)["inputSchema"]);
    EXPECT_FALSE(echoed.contains("isError") && echoed["isError"] == true) << echoed.dump();

    const json resources = pair.result("resources/list", json::object());
    ASSERT_EQ(resources["resources"].size(), 1u);
    const json contents = pair.result("resources/read", json{{"uri", "config://app"}});
    auto read_result = neomcp::from_wire<neomcp::ReadResult>(contents, profile);
    ASSERT_TRUE(std::holds_alternative<neomcp::ReadResult>(read_result));
    const auto& first_contents = std::get<neomcp::ReadResult>(read_result).contents.at(0);
    EXPECT_EQ(std::get<neomcp::TextResourceContents>(first_contents).text, "on");

    const json prompts = pair.result("prompts/list", json::object());
    EXPECT_NE(find_named(prompts["prompts"], "review"), nullptr);
    const json prompt = pair.result("prompts/get", json{{"name", "review"}, {"arguments", json{{"title", "x"}}}});
    auto prompt_result = neomcp::from_wire<neomcp::PromptResult>(prompt, profile);
    ASSERT_TRUE(std::holds_alternative<neomcp::PromptResult>(prompt_result));
    EXPECT_EQ(std::get<neomcp::PromptResult>(prompt_result).messages.size(), 1u);

    if (profile.request("completion/complete") != nullptr) {
        const json completed =
            pair.result("completion/complete", json{{"ref", json{{"type", "ref/prompt"}, {"name", "review"}}},
                                                    {"argument", json{{"name", "title"}, {"value", "ab"}}}});
        auto completion = neomcp::from_wire<neomcp::CompletionResult>(completed, profile);
        ASSERT_TRUE(std::holds_alternative<neomcp::CompletionResult>(completion));
        EXPECT_EQ(std::get<neomcp::CompletionResult>(completion).values, std::vector<std::string>{"ab-1"});
    }

    const auto* ping = profile.request("ping");
    if (ping != nullptr && ping->from_client) {
        EXPECT_EQ(pair.result("ping", json::object()), json::object());
    }
    EXPECT_TRUE(pair.rig().diagnostics(neomcp::DiagnosticKind::server_defect).empty());
    EXPECT_EQ(pair.rig().engine->pending(), 0u);
}

TEST_P(ClientEnginePair, InputRequestsReachTheSeamAndTheirAnswersComplete) {
    const Cell cell = GetParam();
    const auto revision = neomcp::profile::builtin(cell.revision);
    const auto* sent = revision->request("elicitation/create");
    if (revision->input_request("elicitation/create") == nullptr && (sent == nullptr || !sent->from_server)) {
        GTEST_SKIP() << "the revision cannot ask for a form";
    }
    auto options = only({cell.revision});
    options.input.form = true;
    EnginePair pair(options, cell.transport);
    pair.open();
    neomcp::OutgoingCall outgoing;
    outgoing.message.method = "tools/call";
    outgoing.message.params = json{{"name", "delete_folder"}, {"arguments", json{{"path", "/srv/old"}}}};
    const auto ref = pair.rig().send(outgoing);
    auto& rig = pair.rig();
    ASSERT_TRUE(pair.pump([&] {
        return !rig.all<neomcp::ServerAsked>().empty() || !rig.all<neomcp::InputRoundStarted>().empty() ||
               rig.ended(ref).has_value();
    }));
    ASSERT_FALSE(rig.ended(ref));
    const neomcp::ElicitationAnswer yes{neomcp::ElicitationAction::accept, json{{"ok", true}}};
    if (const auto asked = rig.all<neomcp::ServerAsked>(); !asked.empty()) {
        EXPECT_TRUE(std::holds_alternative<neomcp::ElicitationForm>(asked[0].request));
        if (cell.transport != Transport::stdio) {
            EXPECT_EQ(asked[0].call, ref);
        }
        rig.engine->answer(asked[0].ref, neomcp::InputResponse(yes));
    } else {
        const auto round = rig.all<neomcp::InputRoundStarted>().at(0);
        ASSERT_EQ(round.asking.requests.size(), 1u);
        neomcp::InputResponses answers;
        answers.emplace(round.asking.requests[0].key, yes);
        rig.engine->answer_round(ref, answers);
    }
    std::optional<neomcp::RequestEnded> ended;
    ASSERT_TRUE(pair.pump([&] { return (ended = rig.ended(ref)).has_value(); }));
    ASSERT_TRUE(std::holds_alternative<neomcp::Answered>(ended->outcome));
    const json& result = std::get<neomcp::Answered>(ended->outcome).result;
    auto read = neomcp::from_wire<neomcp::ToolResult>(result, pair.profile());
    ASSERT_TRUE(std::holds_alternative<neomcp::ToolResult>(read));
    EXPECT_EQ(std::get<neomcp::TextContent>(std::get<neomcp::ToolResult>(read).content.at(0)).text,
              "deleted /srv/old");
}

INSTANTIATE_TEST_SUITE_P(ClientEngineEveryRevision, ClientEnginePair, testing::ValuesIn(cells()), cell_name);

// ---- a client of every revision against servers of one kind, of the other, or of both

struct Era {
    const char* name;
    std::vector<neomcp::profile::ProfilePtr> served;
    neomcp::ServerKind expected;
    std::string revision;
};

TEST(ClientEnginePairDualEra, AClientOfEveryRevisionFindsWhichKindOfServerAnswers) {
    const std::vector<Era> eras = {
        {"handshake only", revisions({"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}),
         neomcp::ServerKind::handshake, "2025-11-25"},
        {"per request only", revisions({"2026-07-28"}), neomcp::ServerKind::per_request, "2026-07-28"},
        {"both", {}, neomcp::ServerKind::per_request, "2026-07-28"},
    };
    for (const auto transport : {Transport::stdio, Transport::streamable_http}) {
        for (const auto& era : eras) {
            SCOPED_TRACE(std::string(era.name) + (transport == Transport::stdio ? " over stdio" : " over HTTP"));
            neomcp::EngineOptions options;
            options.client_info.name = "engine-test";
            options.client_info.version = "1.0.0";
            EnginePair pair(options, transport, era.served);
            pair.open();
            const auto& contact = pair.rig().engine->contact();
            ASSERT_TRUE(contact.verdict);
            EXPECT_EQ(contact.verdict->kind, era.expected);
            EXPECT_EQ(contact.verdict->revision, era.revision);
            EXPECT_EQ(contact.steps.front().method, "server/discover");
            const json tools = pair.result("tools/list", json::object());
            EXPECT_NE(find_named(tools["tools"], "add"), nullptr);
        }
    }
}

}  // namespace
