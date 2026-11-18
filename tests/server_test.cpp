// Tests of the server layer through the in-memory peer.
//
// Expected per-revision values are written here from the specification of each revision, not read back from the
// profiles. Every wait is on an event a test controls; the bound on a wait only limits how long a broken build takes
// to fail. "Nothing was written" is shown by a later answer that arrives (a session writes in order).
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <gtest/gtest.h>

#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

// The example at the top of neomcp/server.h, as a user writes it.
struct AddArgs {
    int a;
    int b;
    std::optional<std::string> note;
};
NEOMCP_DESCRIBE(AddArgs,
    a, "first addend",
    b, "second addend",
    note, "a note to echo back")

namespace {
namespace st {

struct Bounded {
    int count;
};

struct Point {
    int x;
    int y;
};

enum class Colour { red, green };

struct Painted {
    Colour colour;
};

struct Mismatch {
    int n;
};

struct Tag {
    std::string tag;
};

int add_numbers(const AddArgs& args) { return args.a + args.b; }

struct Multiplier {
    int factor;
    int operator()(const AddArgs& args) const { return factor * (args.a + args.b); }
};

}  // namespace st
}  // namespace

template <>
struct neomcp::Described<st::Bounded> {
    static constexpr auto fields =
        neomcp::fields(neomcp::field(&st::Bounded::count, "count", "how many", neomcp::minimum(0)));
};
NEOMCP_DESCRIBE(st::Point, x, "", y, "")
NEOMCP_DESCRIBE_ENUM(st::Colour, red, green)
NEOMCP_DESCRIBE(st::Painted, colour, "")
NEOMCP_DESCRIBE(st::Mismatch, n, "")
NEOMCP_DESCRIBE(st::Tag, tag, "")

// A schema that admits what the description cannot decode, so that the two disagree.
template <>
struct neomcp::Schema<st::Mismatch> {
    static neomcp::json make() { return neomcp::json{{"type", "object"}}; }
};

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

// Long enough for a sanitizer build on a loaded machine; reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;
constexpr std::chrono::milliseconds no_wait = 0ms;

// The revisions and their era, from the specification (versioning.mdx of 2026-07-28: 2025-11-25 and earlier are
// legacy).
const std::vector<std::string> all_revisions{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"};
const std::vector<std::string> legacy_revisions{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* legacy_revision = "2025-06-18";
constexpr const char* log_level_key = "io.modelcontextprotocol/logLevel";
constexpr const char* version_key = "io.modelcontextprotocol/protocolVersion";

bool is_legacy(const std::string& revision) {
    for (const auto& legacy : legacy_revisions) {
        if (legacy == revision) return true;
    }
    return false;
}

profile::ProfilePtr builtin(const std::string& revision) {
    profile::ProfilePtr found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

std::set<std::string> keys(const json& value) {
    std::set<std::string> out;
    if (const auto* members = value.get_ptr<const json::object_t*>()) {
        for (const auto& entry : *members) out.insert(entry.first);
    }
    return out;
}

std::string name_of(const ::testing::TestParamInfo<std::string>& info) {
    std::string name = info.param;
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

const json& result_of(const json& answer) { return answer.at("result"); }

int error_code_of(const json& answer) {
    if (!answer.contains("error")) return 0;
    return answer.at("error").at("code").get<int>();
}

std::string error_message_of(const json& answer) {
    if (!answer.contains("error")) return {};
    return answer.at("error").at("message").get<std::string>();
}

std::string first_text(const json& answer) {
    return result_of(answer).at("content").at(0).at("text").get<std::string>();
}

// A result (not an error) that is a tool execution error.
bool is_error_result(const json& answer) {
    if (!answer.contains("result")) return false;
    const json& result = answer.at("result");
    return result.contains("isError") && result.at("isError") == true;
}

bool contains_text(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

json cancelled(const json& id) {
    return json{{"jsonrpc", "2.0"},
                {"method", "notifications/cancelled"},
                {"params", {{"requestId", id}, {"reason", "test"}}}};
}

bool mentions(const json& message, const json& id, const json& token) {
    if (message.contains("id") && message.at("id") == id) return true;
    return message.contains("params") && message.at("params").contains("progressToken") &&
           message.at("params").at("progressToken") == token;
}

neomcp::ToolResult text_result(std::string text) {
    neomcp::ToolResult result;
    neomcp::TextContent block;
    block.text = std::move(text);
    result.content.emplace_back(std::move(block));
    return result;
}

// A flag the test raises once. A wait ends when it is raised, when a stop token is stopped, or after a bound that
// only a broken build reaches.
class Event {
public:
    void set() {
        const std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return set_; });
    }
    // Whether it was raised; false when the wait ended because stop was requested.
    bool wait(std::stop_token token) {
        // Registered before the lock is taken: the callback runs at once when stop was requested already.
        std::stop_callback wake(token, [this] {
            const std::lock_guard<std::mutex> lock(mutex_);
            changed_.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return set_ || token.stop_requested(); });
        return set_;
    }
    bool is_set() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return set_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

class Counter {
public:
    void add() {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
        changed_.notify_all();
    }
    bool wait_for(int target, std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return count_ >= target; });
    }
    int value() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int count_ = 0;
};

// A coroutine that ends when stop is requested on the token, the way a coroutine handler waits for something that
// only observes its stop token.
asio::awaitable<void> until_stopped(std::stop_token token) {
    const auto executor = co_await asio::this_coro::executor;
    // Shared with the callback's posted cancellation, which may run after this coroutine resumed for another reason.
    auto timer = std::make_shared<asio::steady_timer>(executor, asio::steady_timer::time_point::max());
    std::stop_callback wake(token, [timer] { asio::post(timer->get_executor(), [timer] { timer->cancel(); }); });
    auto outcome = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    static_cast<void>(outcome);
}

// One client speaking one revision to a server: a handshake for a legacy revision, nothing for the modern one.
struct Client {
    Client(neomcp::Server& server, const std::string& revision_name)
        : peer(server), revision(builtin(revision_name)) {
        if (is_legacy(revision_name)) {
            const auto opened = peer.initialize(*revision, patience);
            EXPECT_TRUE(opened && opened->contains("result")) << (opened ? opened->dump() : "no answer");
        }
    }

    json message(const neomcp::RequestId& id, std::string method, json params = json::object()) const {
        return Peer::make_request(*revision, id, std::move(method), std::move(params));
    }

    static json call_params(const std::string& tool, json arguments, json meta) {
        json params = json::object();
        params["name"] = tool;
        params["arguments"] = std::move(arguments);
        if (!meta.is_null()) params["_meta"] = std::move(meta);
        return params;
    }

    json ask(std::string method, json params = json::object()) {
        const auto answer = peer.request(message(next_id++, std::move(method), std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer within the patience");
        return *answer;
    }

    json call(const std::string& tool, json arguments = json::object(), json meta = nullptr) {
        return ask("tools/call", call_params(tool, std::move(arguments), std::move(meta)));
    }

    // Sends a call without waiting for its answer.
    void start(const neomcp::RequestId& id, const std::string& tool, json arguments = json::object(),
               json meta = nullptr) {
        peer.notify(message(id, "tools/call", call_params(tool, std::move(arguments), std::move(meta))));
    }

    std::vector<json> notifications() {
        std::vector<json> out;
        while (auto notification = peer.next_notification(no_wait)) out.push_back(std::move(*notification));
        return out;
    }

    Peer peer;
    profile::ProfilePtr revision;
    int next_id = 1;
};

neomcp::ServerOptions named(std::string name) {
    neomcp::ServerOptions options;
    options.name = std::move(name);
    options.version = "1.0.0";
    return options;
}

class PerRevision : public ::testing::TestWithParam<std::string> {};
INSTANTIATE_TEST_SUITE_P(Revisions, PerRevision, ::testing::ValuesIn(all_revisions), name_of);

// ---------------------------------------------------------------------------------------------------------------
// The documented example

// The schema of AddArgs by the rules of neomcp/describe/schema.h: int has the range of 32 bits.
constexpr const char* add_input_schema = R"({"type":"object","properties":{
    "a":{"type":"integer","minimum":-2147483648,"maximum":2147483647,"description":"first addend"},
    "b":{"type":"integer","minimum":-2147483648,"maximum":2147483647,"description":"second addend"},
    "note":{"type":"string","description":"a note to echo back"}},
    "required":["a","b"],"additionalProperties":false})";
constexpr const char* int_output_schema = R"({"type":"object","properties":{
    "result":{"type":"integer","minimum":-2147483648,"maximum":2147483647}},
    "required":["result"],"additionalProperties":false})";

// Tool per revision (schema.json of each revision), for a tool with a description and an output schema: the output
// schema exists from 2025-06-18.
const std::map<std::string, std::set<std::string>> example_tool_members{
    {"2024-11-05", {"description", "inputSchema", "name"}},
    {"2025-03-26", {"description", "inputSchema", "name"}},
    {"2025-06-18", {"description", "inputSchema", "name", "outputSchema"}},
    {"2025-11-25", {"description", "inputSchema", "name", "outputSchema"}},
    {"2026-07-28", {"description", "inputSchema", "name", "outputSchema"}},
};
// CallToolResult has structuredContent from 2025-06-18.
const std::map<std::string, bool> has_structured_content{
    {"2024-11-05", false}, {"2025-03-26", false}, {"2025-06-18", true}, {"2025-11-25", true}, {"2026-07-28", true},
};

TEST_P(PerRevision, TheDocumentedExample) {
    const std::string revision = GetParam();
    ASSERT_EQ(example_tool_members.size(), profile::builtin_revisions().size())
        << "a new revision needs its values here";

    neomcp::Server server({.name = "calc", .version = "1.0.0"});
    server.tool<AddArgs>("add", "Add two integers",
                         [](const AddArgs& args) { return args.a + args.b; });

    Client client(server, revision);
    const json list = result_of(client.ask("tools/list"));
    ASSERT_EQ(list.at("tools").size(), 1u);
    const json& tool = list.at("tools").at(0);
    EXPECT_EQ(keys(tool), example_tool_members.at(revision)) << tool.dump();
    EXPECT_EQ(tool.at("name"), "add");
    EXPECT_EQ(tool.at("description"), "Add two integers");
    EXPECT_EQ(tool.at("inputSchema"), json::parse(add_input_schema));
    if (tool.contains("outputSchema")) {
        EXPECT_EQ(tool.at("outputSchema"), json::parse(int_output_schema));
    }

    const json answer = client.call("add", {{"a", 2}, {"b", 3}, {"note", "hi"}});
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    const json& result = result_of(answer);
    ASSERT_EQ(result.at("content").size(), 1u);
    EXPECT_EQ(result.at("content").at(0), (json{{"type", "text"}, {"text", "5"}}));
    EXPECT_FALSE(result.contains("isError"));
    EXPECT_EQ(result.contains("structuredContent"), has_structured_content.at(revision)) << result.dump();
    if (result.contains("structuredContent")) {
        EXPECT_EQ(result.at("structuredContent"), (json{{"result", 5}}));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Handler forms and results

constexpr const char* word_schema =
    R"({"type":"object","properties":{"word":{"type":"string"}},"required":["word"]})";

TEST(Forms, EveryHandlerFormIsCalled) {
    neomcp::Server server(named("forms"));
    server.tool<AddArgs>("plain", "", [](const AddArgs& args) { return args.a + args.b; });
    server.tool<AddArgs>("plain_by_value", "", [](AddArgs args) { return args.a + args.b; });
    server.tool<AddArgs>("plain_context", "", [](const AddArgs& args, neomcp::Context& context) {
        return context.revision() + " " + std::to_string(args.a + args.b);
    });
    server.tool<AddArgs>("coroutine", "", [](AddArgs args) -> asio::awaitable<int> { co_return args.a + args.b; });
    server.tool<AddArgs>("coroutine_reference", "",
                         [](const AddArgs& args) -> asio::awaitable<int> { co_return args.a * args.b; });
    server.tool<AddArgs>("coroutine_context", "",
                         [](AddArgs args, neomcp::Context& context) -> asio::awaitable<std::string> {
                             co_return context.revision() + " " + std::to_string(args.a + args.b);
                         });
    server.tool("none", "", [] { return std::string("none"); });
    server.tool("none_context", "", [](neomcp::Context& context) { return context.revision(); });
    server.tool("none_coroutine", "", []() -> asio::awaitable<std::string> { co_return std::string("none co"); });
    server.tool("none_coroutine_context", "",
                [](neomcp::Context& context) -> asio::awaitable<std::string> { co_return context.revision(); });
    neomcp::ToolDefinition word;
    word.name = "untyped";
    word.input_schema = json::parse(word_schema);
    server.tool(word, [](const json& arguments, neomcp::Context&) {
        return text_result(arguments.at("word").get<std::string>());
    });
    word.name = "untyped_coroutine";
    server.tool(word, [](json arguments, neomcp::Context&) -> asio::awaitable<neomcp::ToolResult> {
        co_return text_result(arguments.at("word").get<std::string>() + " co");
    });
    server.tool<AddArgs>("pointer", "", &st::add_numbers);
    server.tool<AddArgs>("function", "", st::add_numbers);
    server.tool<AddArgs>("object", "", st::Multiplier{10});
    server.tool<AddArgs>("noexcept", "", [](const AddArgs& args) noexcept { return args.a - args.b; });
    server.tool<AddArgs>("mutable", "", [calls = 0](const AddArgs&) mutable { return ++calls; });

    struct Case {
        std::string tool;
        json arguments;
        std::string text;
    };
    const json typed = {{"a", 2}, {"b", 3}};
    const json words = {{"word", "hi"}};
    const std::vector<Case> cases{
        {"plain", typed, "5"},
        {"plain_by_value", typed, "5"},
        {"plain_context", typed, "2026-07-28 5"},
        {"coroutine", typed, "5"},
        {"coroutine_reference", typed, "6"},
        {"coroutine_context", typed, "2026-07-28 5"},
        {"none", json::object(), "none"},
        {"none_context", json::object(), "2026-07-28"},
        {"none_coroutine", json::object(), "none co"},
        {"none_coroutine_context", json::object(), "2026-07-28"},
        {"untyped", words, "hi"},
        {"untyped_coroutine", words, "hi co"},
        {"pointer", typed, "5"},
        {"function", typed, "5"},
        {"object", typed, "50"},
        {"noexcept", typed, "-1"},
        {"mutable", typed, "1"},
    };

    Client client(server, modern_revision);
    const json tools = result_of(client.ask("tools/list")).at("tools");
    ASSERT_EQ(tools.size(), cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        SCOPED_TRACE(cases[i].tool);
        EXPECT_EQ(tools.at(i).at("name"), cases[i].tool) << "registration order";
        const json answer = client.call(cases[i].tool, cases[i].arguments);
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_FALSE(result_of(answer).contains("isError")) << answer.dump();
        EXPECT_EQ(first_text(answer), cases[i].text);
    }
    EXPECT_EQ(tools.at(6).at("inputSchema"), (json{{"type", "object"}, {"additionalProperties", false}}))
        << "a tool without parameters takes an empty object";
    EXPECT_EQ(tools.at(10).at("inputSchema"), json::parse(word_schema)) << "a definition written by hand is kept";
}

// What each result type becomes (the table in neomcp/server.h), for 2026-07-28, which has structuredContent and
// outputSchema. Absent: null.
struct ResultCase {
    const char* tool;
    const char* content;
    const char* structured;
    const char* output_schema;
};

const std::vector<ResultCase> result_cases{
    {"string", R"([{"type":"text","text":"text"}])", nullptr, nullptr},
    {"struct", R"([{"type":"text","text":"{\"x\":1,\"y\":2}"}])", R"({"x":1,"y":2})",
     R"({"type":"object","properties":{
         "x":{"type":"integer","minimum":-2147483648,"maximum":2147483647},
         "y":{"type":"integer","minimum":-2147483648,"maximum":2147483647}},
         "required":["x","y"],"additionalProperties":false})"},
    {"bool", R"([{"type":"text","text":"true"}])", R"({"result":true})",
     R"({"type":"object","properties":{"result":{"type":"boolean"}},
         "required":["result"],"additionalProperties":false})"},
    {"integer", R"([{"type":"text","text":"42"}])", R"({"result":42})", int_output_schema},
    {"double", R"([{"type":"text","text":"1.5"}])", R"({"result":1.5})",
     R"({"type":"object","properties":{"result":{"type":"number"}},
         "required":["result"],"additionalProperties":false})"},
    {"vector", R"([{"type":"text","text":"[1,2]"}])", R"({"result":[1,2]})",
     R"({"type":"object","properties":{"result":{"type":"array","items":
         {"type":"integer","minimum":-2147483648,"maximum":2147483647}}},
         "required":["result"],"additionalProperties":false})"},
    {"map", R"([{"type":"text","text":"{\"a\":1}"}])", R"({"result":{"a":1}})",
     R"({"type":"object","properties":{"result":{"type":"object","additionalProperties":
         {"type":"integer","minimum":-2147483648,"maximum":2147483647}}},
         "required":["result"],"additionalProperties":false})"},
    {"enum", R"([{"type":"text","text":"\"green\""}])", R"({"result":"green"})",
     R"({"type":"object","properties":{"result":{"type":"string","enum":["red","green"]}},
         "required":["result"],"additionalProperties":false})"},
    {"json_object", R"([{"type":"text","text":"{\"k\":\"v\"}"}])", R"({"k":"v"})", nullptr},
    {"json_array", R"([{"type":"text","text":"[1,2]"}])", nullptr, nullptr},
    {"tool_result", R"([{"type":"image","data":"aW1hZ2U=","mimeType":"image/png"}])", R"({"free":true})", nullptr},
    {"void", "[]", nullptr, nullptr},
};

TEST(Results, EveryResultTypeOfTheTable) {
    neomcp::Server server(named("results"));
    server.tool("string", "", [] { return std::string("text"); });
    server.tool("struct", "", [] { return st::Point{1, 2}; });
    server.tool("bool", "", [] { return true; });
    server.tool("integer", "", [] { return 42; });
    server.tool("double", "", [] { return 1.5; });
    server.tool("vector", "", [] { return std::vector<int>{1, 2}; });
    server.tool("map", "", [] { return std::map<std::string, int>{{"a", 1}}; });
    server.tool("enum", "", [] { return st::Colour::green; });
    server.tool("json_object", "", [] { return json{{"k", "v"}}; });
    server.tool("json_array", "", [] { return json::array({1, 2}); });
    server.tool("tool_result", "", [] {
        neomcp::ToolResult result;
        neomcp::ImageContent image;
        image.data = "aW1hZ2U=";
        image.mime_type = "image/png";
        result.content.emplace_back(std::move(image));
        result.structured_content = json{{"free", true}};
        result.meta = json{{"trace", "t1"}};
        return result;
    });
    server.tool("void", "", [] {});

    Client client(server, modern_revision);
    const json tools = result_of(client.ask("tools/list")).at("tools");
    ASSERT_EQ(tools.size(), result_cases.size());
    for (std::size_t i = 0; i < result_cases.size(); ++i) {
        const ResultCase& item = result_cases[i];
        SCOPED_TRACE(item.tool);
        const json& tool = tools.at(i);
        ASSERT_EQ(tool.at("name"), item.tool);
        if (item.output_schema != nullptr) {
            ASSERT_TRUE(tool.contains("outputSchema")) << tool.dump();
            EXPECT_EQ(tool.at("outputSchema"), json::parse(item.output_schema));
        } else {
            EXPECT_FALSE(tool.contains("outputSchema")) << tool.dump();
        }
        const json answer = client.call(item.tool);
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        const json& result = result_of(answer);
        EXPECT_FALSE(result.contains("isError")) << result.dump();
        EXPECT_EQ(result.at("content"), json::parse(item.content));
        if (item.structured != nullptr) {
            ASSERT_TRUE(result.contains("structuredContent")) << result.dump();
            EXPECT_EQ(result.at("structuredContent"), json::parse(item.structured));
        } else {
            EXPECT_FALSE(result.contains("structuredContent")) << result.dump();
        }
    }
    EXPECT_EQ(result_of(client.call("tool_result")).at("_meta").at("trace"), "t1");
}

// ---------------------------------------------------------------------------------------------------------------
// Registration

constexpr const char* expected_annotations =
    R"({"title":"Annotated","readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false})";

TEST(Registration, HandleSettersReachToolsList) {
    neomcp::Server server(named("handle"));
    const neomcp::ToolHandle handle =
        server.tool("annotated", "every optional member", [] { return std::string("x"); })
            .title("Annotated")
            .read_only()
            .destructive(false)
            .idempotent()
            .open_world(false)
            .meta(json{{"k", "v"}})
            .icons({neomcp::Icon{"https://example.com/tool.png", "image/png", {"48x48"}, neomcp::IconTheme::light}});
    EXPECT_EQ(handle.name(), "annotated");
    server.tool("bare", "no optional member", [] { return std::string("y"); });

    Client modern(server, modern_revision);
    const json tools = result_of(modern.ask("tools/list")).at("tools");
    ASSERT_EQ(tools.size(), 2u);
    const json& tool = tools.at(0);
    EXPECT_EQ(keys(tool), (std::set<std::string>{"_meta", "annotations", "description", "icons", "inputSchema", "name",
                                                 "title"}));
    EXPECT_EQ(tool.at("title"), "Annotated");
    EXPECT_EQ(tool.at("annotations"), json::parse(expected_annotations));
    EXPECT_EQ(tool.at("_meta"), (json{{"k", "v"}}));
    EXPECT_EQ(tool.at("icons").at(0).at("src"), "https://example.com/tool.png");
    EXPECT_EQ(keys(tools.at(1)), (std::set<std::string>{"description", "inputSchema", "name"}));

    // 2025-03-26 has the annotations, the title among them, and no title of the tool itself.
    Client older(server, "2025-03-26");
    const json old_tool = result_of(older.ask("tools/list")).at("tools").at(0);
    EXPECT_EQ(keys(old_tool), (std::set<std::string>{"annotations", "description", "inputSchema", "name"}));
    EXPECT_EQ(old_tool.at("annotations").at("title"), "Annotated");
}

template <class Error, class Action>
void expect_error_naming(Action action, const std::string& name) {
    try {
        action();
        ADD_FAILURE() << "no exception for " << name;
    } catch (const Error& error) {
        EXPECT_TRUE(contains_text(error.what(), name)) << error.what();
    }
}

constexpr const char* unresolvable_schema =
    R"({"type":"object","properties":{"x":{"$ref":"#/nowhere"}}})";

TEST(Registration, RefusedDefinitionsNameTheTool) {
    neomcp::Server server(named("registration"));
    const auto first = server.tool("dup", "", [] {});
    expect_error_naming<std::invalid_argument>([&] { server.tool("dup", "", [] {}); }, "dup");
    EXPECT_THROW(server.tool("", "", [] {}), std::invalid_argument);

    const auto untyped = [](const json&, neomcp::Context&) { return neomcp::ToolResult{}; };
    const auto refused = [&](const std::string& name, json input, std::optional<json> output) {
        neomcp::ToolDefinition definition;
        definition.name = name;
        definition.input_schema = std::move(input);
        definition.output_schema = std::move(output);
        expect_error_naming<std::invalid_argument>([&] { server.tool(definition, untyped); }, name);
    };
    refused("input_refused", json::parse(unresolvable_schema), std::nullopt);
    refused("input_array", json{{"type", "array"}}, std::nullopt);
    refused("input_untyped", json::object(), std::nullopt);
    refused("output_refused", json{{"type", "object"}}, json::parse(unresolvable_schema));
    refused("output_string", json{{"type", "object"}}, json{{"type", "string"}});

    neomcp::ToolDefinition bad_meta;
    bad_meta.name = "meta_array";
    bad_meta.meta = json::array();
    expect_error_naming<std::invalid_argument>([&] { server.tool(bad_meta, untyped); }, "meta_array");
    const auto handle = server.tool("meta_handle", "", [] {});
    expect_error_naming<std::invalid_argument>([&] { handle.meta(json::array()); }, "meta_handle");

    auto made = server.connect([](std::string) {});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    const auto after_connect = [&](auto action, const std::string& name) {
        try {
            action();
            ADD_FAILURE() << "no exception for " << name;
        } catch (const std::invalid_argument& error) {
            ADD_FAILURE() << "expected std::logic_error, got std::invalid_argument: " << error.what();
        } catch (const std::logic_error& error) {
            EXPECT_TRUE(contains_text(error.what(), name)) << error.what();
        }
    };
    after_connect([&] { server.tool("late", "", [] {}); }, "late");
    after_connect([&] { first.title("too late"); }, "dup");

    std::vector<std::string> names;
    for (const auto& tool : server.tools()) names.push_back(tool.name);
    EXPECT_EQ(names, (std::vector<std::string>{"dup", "meta_handle"})) << "no refused tool was registered";
}

TEST(Registration, OptionsThatCannotWork) {
    EXPECT_THROW(neomcp::Server({.name = "x", .version = "1", .max_concurrent_calls = 0}), std::invalid_argument);
    neomcp::Server server({.name = "x", .version = "1", .tools_page_size = 0});
    auto made = server.connect([](std::string) {});
    ASSERT_TRUE(std::holds_alternative<neomcp::CreateError>(made));
    EXPECT_TRUE(contains_text(std::get<neomcp::CreateError>(made).message, "tools_page_size"));
}

// ---------------------------------------------------------------------------------------------------------------
// Arguments

// server/tools.mdx of each revision: arguments that fail the input schema are a protocol error (-32602) up to
// 2025-06-18 and a tool execution error (a result with isError) from 2025-11-25.
const std::map<std::string, bool> invalid_arguments_are_results{
    {"2024-11-05", false}, {"2025-03-26", false}, {"2025-06-18", false}, {"2025-11-25", true}, {"2026-07-28", true},
};

TEST_P(PerRevision, ArgumentsAreCheckedBeforeTheHandlerRuns) {
    const std::string revision = GetParam();
    ASSERT_EQ(invalid_arguments_are_results.size(), profile::builtin_revisions().size());
    Counter ran;
    neomcp::Server server(named("checked"));
    server.tool<AddArgs>("add", "", [&ran](const AddArgs& args) {
        ran.add();
        return args.a + args.b;
    });
    server.tool<st::Bounded>("bounded", "", [&ran](const st::Bounded& args) {
        ran.add();
        return args.count;
    });
    Client client(server, revision);

    struct Case {
        std::string what;
        std::string tool;
        json arguments;
        std::string said;  // a part of the message that says what failed
    };
    const std::vector<Case> cases{
        {"a wrong type", "add", {{"a", "two"}, {"b", 3}}, "arguments/a"},
        {"a missing required member", "add", {{"a", 2}}, "\"b\""},
        {"an extra member", "add", {{"a", 2}, {"b", 3}, {"c", 4}}, "arguments/c"},
        {"a field option", "bounded", {{"count", -1}}, "minimum"},
    };
    for (const Case& item : cases) {
        SCOPED_TRACE(item.what);
        const json answer = client.call(item.tool, item.arguments);
        if (invalid_arguments_are_results.at(revision)) {
            ASSERT_TRUE(answer.contains("result")) << answer.dump();
            EXPECT_TRUE(is_error_result(answer)) << answer.dump();
            EXPECT_TRUE(contains_text(first_text(answer), item.said)) << first_text(answer);
        } else {
            EXPECT_EQ(error_code_of(answer), -32602) << answer.dump();
            EXPECT_TRUE(contains_text(error_message_of(answer), item.said)) << answer.dump();
        }
    }
    EXPECT_EQ(ran.value(), 0) << "a handler ran with arguments that fail the input schema";
    EXPECT_EQ(first_text(client.call("add", {{"a", 2}, {"b", 3}})), "5");
    EXPECT_EQ(first_text(client.call("bounded", {{"count", 0}})), "0");
    EXPECT_EQ(ran.value(), 2);
}

// ---------------------------------------------------------------------------------------------------------------
// Failures

constexpr const char* count_schema =
    R"({"type":"object","properties":{"count":{"type":"integer"}},"required":["count"]})";

std::unique_ptr<neomcp::Server> failing_server(bool expose) {
    neomcp::ServerOptions options = named("failing");
    options.expose_error_details = expose;
    auto server = std::make_unique<neomcp::Server>(std::move(options));
    server->tool("tool_error", "", []() -> std::string { throw neomcp::ToolError("no such city"); });
    server->tool("throws", "", []() -> std::string { throw std::runtime_error("secret detail"); });
    server->tool("throws_other", "", []() -> std::string { throw 42; });
    server->tool("coroutine_throws", "", [fail = true]() -> asio::awaitable<std::string> {
        if (fail) throw std::runtime_error("secret detail");
        co_return std::string();
    });
    server->tool("bad_encoding", "", [] { return st::Painted{static_cast<st::Colour>(7)}; });
    server->tool<st::Mismatch>("mismatch", "", [](const st::Mismatch& value) { return value.n; });
    neomcp::ToolDefinition shaped;
    shaped.output_schema = json::parse(count_schema);
    shaped.name = "wrong_shape";
    server->tool(shaped, [](const json&, neomcp::Context&) {
        neomcp::ToolResult result = text_result("many");
        result.structured_content = json{{"count", "many"}};
        return result;
    });
    shaped.name = "no_structure";
    server->tool(shaped, [](const json&, neomcp::Context&) { return text_result("no structure"); });
    shaped.name = "error_without_structure";
    server->tool(shaped, [](const json&, neomcp::Context&) {
        neomcp::ToolResult result = text_result("failed as a tool");
        result.is_error = true;
        return result;
    });
    shaped.name = "right_shape";
    server->tool(shaped, [](const json&, neomcp::Context&) -> asio::awaitable<neomcp::ToolResult> {
        neomcp::ToolResult result = text_result("3");
        result.structured_content = json{{"count", 3}};
        co_return result;
    });
    return server;
}

class Failures : public ::testing::TestWithParam<bool> {};
INSTANTIATE_TEST_SUITE_P(Details, Failures, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "exposed" : "hidden"; });

TEST_P(Failures, AreAnsweredAsTheTableSays) {
    const bool expose = GetParam();
    auto server = failing_server(expose);
    for (const std::string revision : {legacy_revision, modern_revision}) {
        SCOPED_TRACE(revision);
        Client client(*server, revision);
        const auto tool_error = [&](const std::string& tool) {
            const json answer = client.call(tool);
            EXPECT_TRUE(is_error_result(answer)) << answer.dump();
            if (!answer.contains("result")) return std::string();
            return first_text(answer);
        };
        EXPECT_EQ(tool_error("tool_error"), "no such city");
        EXPECT_EQ(tool_error("throws"), expose ? "The tool failed: secret detail" : "The tool failed");
        EXPECT_EQ(tool_error("coroutine_throws"), expose ? "The tool failed: secret detail" : "The tool failed");
        EXPECT_EQ(tool_error("throws_other"),
                  expose ? "The tool failed: an exception that is not a std::exception" : "The tool failed");
        const std::string encoding = tool_error("bad_encoding");
        if (expose) {
            EXPECT_TRUE(contains_text(encoding, "enum value")) << encoding;
        } else {
            EXPECT_EQ(encoding, "The tool failed");
        }
        const std::string decoding = tool_error("mismatch");
        if (expose) {
            EXPECT_TRUE(contains_text(decoding, "decoding failed")) << decoding;
        } else {
            EXPECT_EQ(decoding, "The tool failed");
        }
        EXPECT_EQ(tool_error("error_without_structure"), "failed as a tool") << "an error result is not checked";

        for (const auto& [tool, detail] : std::vector<std::pair<std::string, std::string>>{
                 {"wrong_shape", "does not match its output schema"}, {"no_structure", "no structured content"}}) {
            SCOPED_TRACE(tool);
            const json answer = client.call(tool);
            EXPECT_EQ(error_code_of(answer), -32603) << answer.dump();
            if (expose) {
                EXPECT_TRUE(contains_text(error_message_of(answer), detail)) << answer.dump();
            } else {
                EXPECT_EQ(error_message_of(answer), "Internal error: the tool call failed");
            }
        }
        const json right = client.call("right_shape");
        ASSERT_TRUE(right.contains("result")) << right.dump();
        EXPECT_FALSE(result_of(right).contains("isError"));
        if (revision == modern_revision) {
            EXPECT_EQ(result_of(right).at("structuredContent"), (json{{"count", 3}}));
        }
        const json unknown = client.call("nope");
        EXPECT_EQ(error_code_of(unknown), -32602);
        EXPECT_EQ(error_message_of(unknown), "Unknown tool: nope");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Context

TEST(Context, TellsWhoAsked) {
    neomcp::Server server(named("who"));
    server.tool("who", "", [](neomcp::Context& context) {
        json who = json::object();
        who["revision"] = context.revision();
        who["legacy"] = context.era() == profile::Era::legacy;
        who["client"] = context.client_info();
        who["capabilities"] = context.client_capabilities();
        who["meta"] = context.request_meta();
        return who;
    });
    Client legacy(server, legacy_revision);
    const json old = result_of(legacy.call("who", json::object(), {{"trace", "x"}})).at("structuredContent");
    EXPECT_EQ(old.at("revision"), legacy_revision);
    EXPECT_EQ(old.at("legacy"), true);
    EXPECT_EQ(old.at("client").at("name"), "neomcp-test-peer") << "from the handshake";
    EXPECT_EQ(old.at("capabilities"), json::object());
    EXPECT_EQ(old.at("meta"), (json{{"trace", "x"}}));

    Client modern(server, modern_revision);
    const json now = result_of(modern.call("who", json::object(), {{"trace", "y"}})).at("structuredContent");
    EXPECT_EQ(now.at("revision"), modern_revision);
    EXPECT_EQ(now.at("legacy"), false);
    EXPECT_TRUE(now.at("client").is_null()) << "the request named no client";
    EXPECT_EQ(now.at("capabilities"), json::object());
    EXPECT_EQ(now.at("meta").at("trace"), "y");
    EXPECT_EQ(now.at("meta").at(version_key), modern_revision);
}

TEST(Context, ProgressAndLogFromBothKindsAndFromAThreadTheHandlerStarted) {
    neomcp::ServerOptions options = named("reports");
    options.protocol_logging = true;
    neomcp::Server server(std::move(options));
    server.tool("plain", "", [](neomcp::Context& context) {
        context.progress(1, 3, "one");
        context.info("plain");
        std::thread helper([&context] {
            context.progress(2, 3, "two");
            context.warning("from a thread", "helper");
        });
        helper.join();
        return std::string("done");
    });
    server.tool("coroutine", "", [](neomcp::Context& context) -> asio::awaitable<std::string> {
        context.progress(1, 3, "one");
        context.info("coroutine");
        std::thread helper([&context] {
            context.progress(2, 3, "two");
            context.warning("from a thread", "helper");
        });
        helper.join();
        co_return std::string("done");
    });
    Client client(server, modern_revision);
    for (const std::string tool : {"plain", "coroutine"}) {
        SCOPED_TRACE(tool);
        const json answer = client.call(tool, json::object(), {{"progressToken", tool}, {log_level_key, "debug"}});
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        const auto sent = client.notifications();
        ASSERT_EQ(sent.size(), 4u);
        EXPECT_EQ(sent[0].at("method"), "notifications/progress");
        EXPECT_EQ(sent[0].at("params").at("progressToken"), tool);
        EXPECT_EQ(sent[0].at("params").at("progress"), 1);
        EXPECT_EQ(sent[0].at("params").at("message"), "one");
        EXPECT_EQ(sent[1].at("method"), "notifications/message");
        EXPECT_EQ(sent[1].at("params").at("level"), "info");
        EXPECT_EQ(sent[1].at("params").at("data"), tool);
        EXPECT_EQ(sent[2].at("params").at("progress"), 2);
        EXPECT_EQ(sent[3].at("params").at("level"), "warning");
        EXPECT_EQ(sent[3].at("params").at("logger"), "helper");
        EXPECT_EQ(sent[3].at("params").at("data"), "from a thread");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Cancellation

TEST(Cancellation, ReachesAPlainHandlerAndNothingIsWrittenAfter) {
    Event entered;
    Event observed;
    bool saw_stop = false;
    neomcp::ServerOptions options = named("cancel");
    options.protocol_logging = true;
    options.max_concurrent_calls = 1;  // a later call runs after the cancelled one finished
    neomcp::Server server(std::move(options));
    server.tool("wait", "", [&](neomcp::Context& context) {
        context.progress(1);
        entered.set();
        Event never;
        static_cast<void>(never.wait(context.stop_token()));
        saw_stop = context.cancelled();
        observed.set();
        context.progress(2);
        context.error("after the cancellation");
        return std::string("late");
    });
    server.tool("echo", "", [] { return std::string("echo"); });

    Client client(server, modern_revision);
    client.start("c1", "wait", json::object(), {{"progressToken", "t1"}, {log_level_key, "debug"}});
    ASSERT_TRUE(entered.wait(patience));
    const auto first = client.peer.next_notification(patience);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->at("params").at("progressToken"), "t1");

    const std::size_t before = client.peer.transcript().size();
    client.peer.notify(cancelled("c1"));
    ASSERT_TRUE(observed.wait(patience));
    EXPECT_TRUE(saw_stop);
    EXPECT_EQ(first_text(client.call("echo")), "echo");
    const auto transcript = client.peer.transcript();
    ASSERT_EQ(transcript.size(), before + 1) << transcript.back().dump();
    for (std::size_t i = before; i < transcript.size(); ++i) EXPECT_FALSE(mentions(transcript[i], "c1", "t1"));
}

TEST(Cancellation, ReachesACoroutineHandlerAndTheOperationItAwaits) {
    Event entered;
    Event observed;
    bool saw_stop = false;
    bool aborted = false;
    neomcp::ServerOptions options = named("cancel");
    options.protocol_logging = true;
    neomcp::Server server(std::move(options));
    server.tool("wait", "", [&](neomcp::Context& context) -> asio::awaitable<std::string> {
        context.progress(1);
        entered.set();
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor, asio::steady_timer::time_point::max());
        const auto outcome = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        aborted = std::get<0>(outcome) == asio::error::operation_aborted;
        saw_stop = context.cancelled();
        observed.set();
        context.progress(2);
        context.error("after the cancellation");
        co_return std::string("late");
    });
    // Coroutines run on one thread: this one runs after the cancelled one finished.
    server.tool("echo", "", []() -> asio::awaitable<std::string> { co_return std::string("echo"); });

    Client client(server, modern_revision);
    client.start("c1", "wait", json::object(), {{"progressToken", "t1"}, {log_level_key, "debug"}});
    ASSERT_TRUE(entered.wait(patience));
    const auto first = client.peer.next_notification(patience);
    ASSERT_TRUE(first);

    const std::size_t before = client.peer.transcript().size();
    client.peer.notify(cancelled("c1"));
    ASSERT_TRUE(observed.wait(patience));
    EXPECT_TRUE(saw_stop);
    EXPECT_TRUE(aborted) << "the timer the coroutine awaited was not cancelled";
    EXPECT_EQ(first_text(client.call("echo")), "echo");
    const auto transcript = client.peer.transcript();
    ASSERT_EQ(transcript.size(), before + 1) << transcript.back().dump();
    for (std::size_t i = before; i < transcript.size(); ++i) EXPECT_FALSE(mentions(transcript[i], "c1", "t1"));
}

// ---------------------------------------------------------------------------------------------------------------
// The worker pool and the end of the server

TEST(Pool, ACallBeyondBothBoundsIsRefusedAsBusy) {
    Counter entered;
    Event release;
    neomcp::ServerOptions options = named("pool");
    options.max_concurrent_calls = 1;
    options.max_pending_calls = 1;
    neomcp::Server server(std::move(options));
    server.tool("hold", "", [&](neomcp::Context& context) {
        entered.add();
        static_cast<void>(release.wait(context.stop_token()));
        return std::string("held");
    });
    server.tool("co_echo", "", []() -> asio::awaitable<std::string> { co_return std::string("co_echo"); });

    Client client(server, modern_revision);
    client.start("h1", "hold");
    ASSERT_TRUE(entered.wait_for(1, patience));
    client.start("h2", "hold");  // waits for the one worker
    const auto busy = client.peer.request(
        client.message("h3", "tools/call", Client::call_params("hold", json::object(), nullptr)), patience);
    ASSERT_TRUE(busy) << "the third call was not answered while two were held";
    EXPECT_EQ(error_code_of(*busy), -32603) << busy->dump();
    EXPECT_EQ(busy->at("error").at("data"), (json{{"reason", "busy"}}));
    EXPECT_EQ(first_text(client.call("co_echo")), "co_echo") << "coroutine handlers do not wait for the pool";
    EXPECT_EQ(entered.value(), 1) << "the second call waits for the worker";

    release.set();
    const auto first = client.peer.await_response("h1", patience);
    const auto second = client.peer.await_response("h2", patience);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first_text(*first), "held");
    EXPECT_EQ(first_text(*second), "held");
    EXPECT_EQ(entered.value(), 2);
    EXPECT_EQ(first_text(client.call("hold")), "held") << "the capacity is back";
}

TEST(Shutdown, DestroyingTheServerStopsBlockedHandlersAndReturns) {
    Counter entered;
    Event plain_stopped;
    Event coroutine_stopped;
    neomcp::ServerOptions options = named("shutdown");
    options.max_concurrent_calls = 1;
    auto server = std::make_unique<neomcp::Server>(std::move(options));
    server->tool("block", "", [&](neomcp::Context& context) {
        entered.add();
        Event never;
        static_cast<void>(never.wait(context.stop_token()));
        plain_stopped.set();
        return std::string("stopped");
    });
    server->tool("co_block", "", [&](neomcp::Context& context) -> asio::awaitable<std::string> {
        entered.add();
        try {
            co_await until_stopped(context.stop_token());
        } catch (const std::system_error&) {
            // The awaited operation was cancelled as well.
        }
        coroutine_stopped.set();
        co_return std::string("stopped");
    });
    server->tool("co_timer", "", [&](neomcp::Context&) -> asio::awaitable<std::string> {
        entered.add();
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor, asio::steady_timer::time_point::max());
        co_await timer.async_wait(asio::use_awaitable);  // throws when cancelled
        co_return std::string("the timer expired");
    });

    // The session outlives the server.
    auto client = std::make_unique<Client>(*server, modern_revision);
    client->start("b1", "block");
    client->start("b2", "block");  // waits for the one worker
    client->start("c1", "co_block");
    client->start("c2", "co_timer");
    ASSERT_TRUE(entered.wait_for(3, patience));

    server.reset();
    EXPECT_TRUE(plain_stopped.is_set());
    EXPECT_TRUE(coroutine_stopped.is_set());
    EXPECT_EQ(entered.value(), 3) << "the waiting call never ran";

    const auto b1 = client->peer.await_response("b1", patience);
    const auto b2 = client->peer.await_response("b2", patience);
    const auto c1 = client->peer.await_response("c1", patience);
    const auto c2 = client->peer.await_response("c2", patience);
    ASSERT_TRUE(b1 && b2 && c1 && c2);
    EXPECT_EQ(first_text(*b1), "stopped");
    EXPECT_EQ(error_code_of(*b2), -32603) << b2->dump();
    EXPECT_EQ(first_text(*c1), "stopped");
    EXPECT_TRUE(is_error_result(*c2)) << "the cancellation escaped the handler: " << c2->dump();

    EXPECT_EQ(result_of(client->ask("tools/list")).at("tools").size(), 3u) << "the session still has its tools";
    EXPECT_EQ(error_code_of(client->call("block")), -32603) << "no handler runs after the server is gone";
    client.reset();
}

// ---------------------------------------------------------------------------------------------------------------
// Sessions

TEST(Sessions, TwoSessionsShareOneServer) {
    Counter entered;
    Event open;
    neomcp::Server server(named("shared"));
    server.tool<st::Tag>("tagged", "", [&](const st::Tag& args, neomcp::Context& context) {
        context.progress(1);
        entered.add();
        static_cast<void>(open.wait(context.stop_token()));
        return args.tag;
    });
    Client modern(server, modern_revision);
    Client legacy(server, legacy_revision);
    modern.start("m", "tagged", {{"tag", "modern"}}, {{"progressToken", "pm"}});
    legacy.start("l", "tagged", {{"tag", "legacy"}}, {{"progressToken", "pl"}});
    ASSERT_TRUE(entered.wait_for(2, patience)) << "both calls run at once";
    open.set();
    const auto m = modern.peer.await_response("m", patience);
    const auto l = legacy.peer.await_response("l", patience);
    ASSERT_TRUE(m && l);
    EXPECT_EQ(first_text(*m), "modern");
    EXPECT_EQ(first_text(*l), "legacy");
    const auto modern_sent = modern.notifications();
    const auto legacy_sent = legacy.notifications();
    ASSERT_EQ(modern_sent.size(), 1u);
    ASSERT_EQ(legacy_sent.size(), 1u);
    EXPECT_EQ(modern_sent[0].at("params").at("progressToken"), "pm");
    EXPECT_EQ(legacy_sent[0].at("params").at("progressToken"), "pl");
}

TEST(Sessions, AHandlerThatOutlivesItsSessionCompletesWithoutWriting) {
    Event entered;
    Event release;
    Event done;
    bool saw_stop = false;
    auto server = std::make_unique<neomcp::Server>(named("outlive"));
    server->tool("slow", "", [&](neomcp::Context& context) {
        entered.set();
        static_cast<void>(release.wait(patience));  // not the stop token: it goes on after its session closed
        saw_stop = context.cancelled();
        context.progress(1);
        context.error("nobody listens");
        done.set();
        return std::string("too late");
    });

    std::mutex lines_mutex;
    std::vector<std::string> lines;
    auto made = server->connect([&](std::string line) {
        const std::lock_guard<std::mutex> lock(lines_mutex);
        lines.push_back(std::move(line));
    });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    auto session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
    const auto modern = builtin(modern_revision);
    session->receive(neomcp::serialize(
        Peer::make_request(*modern, 1, "tools/call", {{"name", "slow"}, {"_meta", {{"progressToken", "t"}}}})));
    ASSERT_TRUE(entered.wait(patience));
    session.reset();
    std::size_t written = 0;
    {
        const std::lock_guard<std::mutex> lock(lines_mutex);
        written = lines.size();
    }
    release.set();
    ASSERT_TRUE(done.wait(patience));
    server.reset();  // joins the worker, so the handler's completion has been made
    EXPECT_TRUE(saw_stop) << "closing the session requests stop";
    const std::lock_guard<std::mutex> lock(lines_mutex);
    EXPECT_EQ(lines.size(), written);
}

}  // namespace
