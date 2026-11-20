// The client engine paired in process with the server's session: every revision, over stdio and the header rules of
// Streamable HTTP, without sockets. The server's handlers run on its own threads, so what the session writes is queued
// and the test waits for it with a bound; the engine is touched on the test's thread only.
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
using Clock = std::chrono::steady_clock;

constexpr auto wait_bound = std::chrono::seconds(20);

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

// What the session wrote, as the transport would hand it on.
struct Written {
    std::optional<neomcp::UnitId> unit;
    std::string line;
    bool last = false;
    bool end = false;
};

class Pair {
public:
    Pair(neomcp::EngineOptions options, Transport transport, std::vector<neomcp::profile::ProfilePtr> served = {})
        : server_(server_options(std::move(served))), rig_(std::move(options), transport), transport_(transport) {
        populate(server_);
        const auto lines = [this](std::string line) { push(Written{std::nullopt, line, false, false}); };
        const auto outputs = [this](neomcp::Output output) {
            push(Written{output.unit, output.line, output.last, output.kind == neomcp::OutputKind::end});
        };
        auto connected =
            transport == Transport::stdio ? server_.connect(lines) : server_.connect_tagged(transport, outputs);
        if (auto* error = std::get_if<neomcp::CreateError>(&connected)) throw std::runtime_error(error->message);
        session_ = std::move(std::get<std::unique_ptr<neomcp::Session>>(connected));
    }

    Rig& rig() { return rig_; }

    // Hands what the engine wrote to the session and what the session wrote to the engine, until `done` holds or
    // the bound passes.
    bool pump(const std::function<bool()>& done) {
        const auto deadline = Clock::now() + wait_bound;
        while (true) {
            forward();
            if (done()) return true;
            Written written;
            {
                std::unique_lock lock(mutex_);
                if (!ready_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return done();
                written = std::move(queue_.front());
                queue_.pop_front();
            }
            deliver(written);
        }
    }

    void open() {
        rig_.engine->open();
        ASSERT_TRUE(pump([&] {
            const auto phase = rig_.engine->contact().phase;
            return phase == ContactPhase::open || phase == ContactPhase::failed;
        }));
        ASSERT_EQ(rig_.engine->contact().phase, ContactPhase::open);
        // The ready notification of a handshake reaches the server before anything else does.
        pump([] { return true; });
    }

    neomcp::RequestEnded call(neomcp::OutgoingCall outgoing) {
        const auto ref = rig_.send(std::move(outgoing));
        std::optional<neomcp::RequestEnded> ended;
        pump([&] { return (ended = rig_.ended(ref)).has_value(); });
        if (!ended) throw std::runtime_error("the call did not end in time");
        return *ended;
    }

    neomcp::RequestEnded call(std::string method, json params, json input_schema = nullptr) {
        neomcp::OutgoingCall outgoing;
        outgoing.message.method = std::move(method);
        outgoing.message.params = std::move(params);
        outgoing.input_schema = std::move(input_schema);
        return call(std::move(outgoing));
    }

    // The result of a call that must be answered with one.
    json result(std::string method, json params, json input_schema = nullptr) {
        const auto ended = call(method, std::move(params), std::move(input_schema));
        if (const auto* answered = std::get_if<neomcp::Answered>(&ended.outcome)) return answered->result;
        if (const auto* error = std::get_if<neomcp::ErrorAnswered>(&ended.outcome)) {
            throw std::runtime_error(method + " was answered with an error: " + error->error.message);
        }
        if (const auto* failure = std::get_if<neomcp::CallFailure>(&ended.outcome)) {
            throw std::runtime_error(method + " failed: " + failure->message);
        }
        throw std::runtime_error(method + " asked for input");
    }

    const neomcp::profile::Profile& profile() const { return *rig_.engine->contact().profile; }

private:
    void push(Written written) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(written));
        }
        ready_.notify_all();
    }

    void forward() {
        for (; forwarded_ < rig_.out.size(); ++forwarded_) {
            const neomcp::Outgoing outgoing = rig_.out[forwarded_];
            switch (outgoing.kind) {
                case neomcp::OutgoingKind::request:
                case neomcp::OutgoingKind::notification:
                case neomcp::OutgoingKind::response:
                    if (transport_ == Transport::stdio) {
                        session_->receive(outgoing.line);
                    } else {
                        std::vector<neomcp::HeaderField> fields;
                        for (const auto& header : outgoing.headers) fields.push_back({header.name, header.value});
                        session_->receive(outgoing.unit, outgoing.line, fields);
                    }
                    break;
                case neomcp::OutgoingKind::end_unit:
                    if (transport_ != Transport::stdio) session_->abandon(outgoing.unit);
                    break;
                default: break;
            }
        }
    }

    void deliver(const Written& written) {
        if (transport_ == Transport::stdio) {
            rig_.engine->received(neomcp::connection_unit, written.line);
            return;
        }
        const auto unit = written.unit.value_or(neomcp::connection_unit);
        if (!written.end) rig_.engine->received(unit, written.line);
        if (written.end || written.last) rig_.engine->stream_event(unit, neomcp::StreamEnded{});
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Written> queue_;
    neomcp::Server server_;
    Rig rig_;
    Transport transport_;
    std::size_t forwarded_ = 0;
    std::unique_ptr<neomcp::Session> session_;
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
    Pair pair(only({cell.revision}), cell.transport);
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
    Pair pair(options, cell.transport);
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
        if (cell.transport != Transport::stdio) EXPECT_EQ(asked[0].call, ref);
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
            Pair pair(options, transport, era.served);
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
