// Sampling with tools between the Client and the library's own Server in the same process: on every revision and
// transport where the revision's sampling carries tool uses and tool results, a tool's two rounds (a tool use, then
// the tool's result and a final text); and where the client cannot take them, that nothing reaches its handler.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../support/sampling_tool_loop.h"
#include "neomcp/client.h"
#include "neomcp/client/in_process.h"
#include "neomcp/server.h"
#include "support.h"

namespace {

using namespace client_test;
using neomcp::profile::Transport;
namespace loop = sampling_tool_loop;

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

bool server_asks_sampling(const neomcp::profile::Profile& profile) {
    constexpr std::string_view method = "sampling/createMessage";
    const auto* sent = profile.request(method);
    return profile.input_request(method) != nullptr || (sent != nullptr && sent->from_server);
}

// Whether the revision's sampling carries tool uses and tool results: its union of sampling blocks lists both.
bool carries_tool_blocks(const neomcp::profile::Profile& profile) {
    const auto* blocks = profile.union_type("SamplingMessageContentBlock");
    if (blocks == nullptr) return false;
    const auto lists = [&](std::string_view type) {
        return profile.type(type) != nullptr &&
               std::find(blocks->alternatives.begin(), blocks->alternatives.end(), type) != blocks->alternatives.end();
    };
    return lists("ToolUseContent") && lists("ToolResultContent");
}

// Every revision and transport where a server can ask for sampling, split by whether its sampling carries tool blocks.
std::vector<Cell> cells(bool tool_blocks) {
    std::vector<Cell> all;
    for (const auto revision : neomcp::profile::builtin_revisions()) {
        const auto profile = neomcp::profile::builtin(revision);
        if (!server_asks_sampling(*profile) || carries_tool_blocks(*profile) != tool_blocks) continue;
        if (profile->stdio()) all.push_back(Cell{std::string(revision), Transport::stdio});
        if (profile->streamable_http()) all.push_back(Cell{std::string(revision), Transport::streamable_http});
    }
    return all;
}

// A server with the tool, its in-process peer and a client of it.
class Pair {
public:
    Pair(neomcp::ClientOptions options, const Cell& cell) : server_(served(cell.revision)) {
        loop::register_tool(server_);
        peer = neomcp::InProcessServer::create(server_, cell.transport);
        auto opened = neomcp::Client::open(std::move(options), peer->transport());
        if (!opened) throw std::runtime_error("the client did not open: " + opened.error().message);
        client.emplace(std::move(opened).value());
    }

private:
    static neomcp::ServerOptions served(const std::string& revision) {
        neomcp::ServerOptions options;
        options.name = "sampling-tools-server";
        options.version = "1.0.0";
        options.profiles.push_back(neomcp::profile::builtin(revision));
        return options;
    }

    neomcp::Server server_;

public:
    std::shared_ptr<neomcp::InProcessServer> peer;
    std::optional<neomcp::Client> client;
};

std::string first_text(const neomcp::ToolResult& result) {
    if (result.content.empty()) return "(empty)";
    const auto* text = std::get_if<neomcp::TextContent>(&result.content[0]);
    return text == nullptr ? "(not text)" : text->text;
}

// The sampling handler's declaration that it takes requests with tools.
neomcp::SamplingSupport with_tools() {
    neomcp::SamplingSupport support;
    support.tools = true;
    return support;
}

class SamplingToolsPair : public testing::TestWithParam<Cell> {};

TEST_P(SamplingToolsPair, AToolUseAndItsResultMakeTwoRounds) {
    const Cell cell = GetParam();
    std::atomic<int> rounds{0};
    std::atomic<int> results_seen{0};
    auto options = only({cell.revision});
    options.handlers.sampling(
        [&](const neomcp::SamplingRequest& request) {
            ++rounds;
            for (const auto& message : request.messages) {
                for (const auto& block : message.content) {
                    if (std::holds_alternative<neomcp::ToolResultContent>(block)) ++results_seen;
                }
            }
            return loop::model(request);
        },
        with_tools());
    Pair pair(std::move(options), cell);
    const auto result = pair.client->call_tool(std::string(loop::tool_name));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_FALSE(result->is_error) << first_text(*result);
    EXPECT_EQ(first_text(*result), loop::expected);
    EXPECT_EQ(rounds.load(), 2);
    EXPECT_EQ(results_seen.load(), 1);
}

TEST_P(SamplingToolsPair, AHandlerThatTakesNoToolsIsNotAsked) {
    const Cell cell = GetParam();
    std::atomic<int> rounds{0};
    auto options = only({cell.revision});
    options.handlers.sampling([&](const neomcp::SamplingRequest& request) {
        ++rounds;
        return loop::model(request);
    });
    Pair pair(std::move(options), cell);
    const auto result = pair.client->call_tool(std::string(loop::tool_name));
    EXPECT_TRUE(!result || result->is_error) << (result ? first_text(*result) : std::string());
    EXPECT_EQ(rounds.load(), 0) << "the server sends no request with tools to a client that did not declare them";
}

INSTANTIATE_TEST_SUITE_P(SamplingToolsEveryRevision, SamplingToolsPair, testing::ValuesIn(cells(true)), cell_name);

// Where the revision's sampling has no tool blocks, the server asks nothing, whatever the handler declares.
class SamplingToolsWithout : public testing::TestWithParam<Cell> {};

TEST_P(SamplingToolsWithout, TheCallFailsAndTheHandlerIsNeverAsked) {
    const Cell cell = GetParam();
    std::atomic<int> rounds{0};
    auto options = only({cell.revision});
    options.handlers.sampling(
        [&](const neomcp::SamplingRequest& request) {
            ++rounds;
            return loop::model(request);
        },
        with_tools());
    Pair pair(std::move(options), cell);
    const auto result = pair.client->call_tool(std::string(loop::tool_name));
    EXPECT_TRUE(!result || result->is_error) << (result ? first_text(*result) : std::string());
    EXPECT_EQ(rounds.load(), 0);
}

INSTANTIATE_TEST_SUITE_P(SamplingToolsOlderRevisions, SamplingToolsWithout, testing::ValuesIn(cells(false)), cell_name);

}  // namespace
