#include "flow.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <future>
#include <map>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <variant>

#include "../support/sampling_tool_loop.h"
#include "registrations.h"

namespace client_matrix {

namespace {

using neomcp::json;
using neomcp::profile::Era;
using neomcp::profile::Profile;
using neomcp::profile::Transport;

// How long the flow waits for a change the server was made to report. Reached only when something is broken.
constexpr auto change_bound = std::chrono::seconds(30);

// What the client's handlers answer, and what the asking tools must then return.
constexpr std::string_view model_answer = "Paris";
constexpr std::string_view user_email = "ada@example.com";
constexpr std::string_view root_uri = "file:///client-matrix/root";

bool defines(const Profile& profile, Transport transport) {
    switch (transport) {
        case Transport::stdio: return profile.stdio().has_value();
        case Transport::http_sse: return profile.http_sse().has_value();
        case Transport::streamable_http: return profile.streamable_http().has_value();
    }
    return false;
}

bool contains(const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Whether the revision lets a server ask the client `method`: as a request of its own, or inside a result.
bool can_ask(const Profile& profile, std::string_view method) {
    const neomcp::profile::Message* request = profile.request(method);
    return (request != nullptr && request->from_server) || profile.input_request(method) != nullptr;
}

bool client_requests(const Profile& profile, std::string_view method) {
    const neomcp::profile::Message* request = profile.request(method);
    return request != nullptr && request->from_client;
}

// Whether the revision's sampling carries tool uses and tool results: its union of sampling blocks lists both types.
bool samples_with_tools(const Profile& profile) {
    const neomcp::profile::Union* blocks = profile.union_type("SamplingMessageContentBlock");
    if (blocks == nullptr) return false;
    for (const char* type : {"ToolUseContent", "ToolResultContent"}) {
        if (profile.type(type) == nullptr || !contains(blocks->alternatives, type)) return false;
    }
    return true;
}

// Whether the revision's tool results carry structured content.
bool structured_results(const Profile& profile) {
    const neomcp::profile::Type* type = profile.type("CallToolResult");
    return type != nullptr && type->has_property("structuredContent");
}

std::string text_of(const neomcp::ToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<neomcp::TextContent>(&block)) return text->text;
    }
    return {};
}

bool mentions(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

template <class T>
std::vector<std::string> names_of(const std::vector<T>& items) {
    std::vector<std::string> names;
    for (const T& item : items) names.push_back(item.name);
    return names;
}

// ---- The steps. Each reports through GoogleTest; a fatal failure ends the step.

void check_lists(neomcp::Client& client, const Expected& expected) {
    const auto tools = client.list_all_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    const std::vector<std::string> tool_names = names_of(tools->items);
    for (const char* name : {"add", "test_simple_text", "test_tool_with_progress", "test_sampling", "wait",
                             "test_trigger_tool_change"}) {
        EXPECT_TRUE(contains(tool_names, name)) << name;
    }
    EXPECT_FALSE(contains(tool_names, "test_hidden_tool"));
    if (expected.paged) {
        // The library's own servers also serve the tool that samples with tools.
        EXPECT_TRUE(contains(tool_names, sampling_tool_loop::tool_name));
        EXPECT_EQ(tools->page_cache.size(), tools->items.size());
        const auto first = client.list_tools();
        ASSERT_TRUE(first) << first.error().message;
        EXPECT_EQ(first->items.size(), 1u);
        EXPECT_TRUE(first->next_cursor.has_value());
    }

    const auto resources = client.list_all_resources();
    ASSERT_TRUE(resources) << resources.error().message;
    std::vector<std::string> uris;
    for (const auto& resource : resources->items) uris.push_back(resource.uri);
    EXPECT_TRUE(contains(uris, "test://static-text"));

    const auto templates = client.list_all_resource_templates();
    ASSERT_TRUE(templates) << templates.error().message;
    std::vector<std::string> forms;
    for (const auto& family : templates->items) forms.push_back(family.uri_template);
    EXPECT_TRUE(contains(forms, "test://template/{id}/data"));

    const auto prompts = client.list_all_prompts();
    ASSERT_TRUE(prompts) << prompts.error().message;
    EXPECT_TRUE(contains(names_of(prompts->items), "test_prompt_with_arguments"));
}

void check_tool_calls(neomcp::Client& client, const Profile& governing) {
    const auto simple = client.call_tool("test_simple_text");
    ASSERT_TRUE(simple) << simple.error().message;
    EXPECT_FALSE(simple->is_error);
    EXPECT_EQ(text_of(*simple), "This is a simple text response for testing.");

    if (structured_results(governing)) {
        const auto sum = client.call<Sum>("add", AddArgs{2, 3});
        ASSERT_TRUE(sum) << sum.error().message;
        EXPECT_FALSE(sum->is_error);
        ASSERT_TRUE(sum->value.has_value());
        EXPECT_EQ(sum->value->sum, 5);
        return;
    }
    // Nothing is sent; the untyped call carries the same object as text.
    const auto refused = client.call<Sum>("add", AddArgs{2, 3});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, neomcp::ClientErrorKind::no_structured_results) << refused.error().message;
    json arguments = json::object();
    arguments["a"] = 2;
    arguments["b"] = 3;
    const auto untyped = client.call_tool("add", std::move(arguments));
    ASSERT_TRUE(untyped) << untyped.error().message;
    const json value = json::parse(text_of(*untyped), nullptr, false);
    ASSERT_TRUE(value.is_object()) << text_of(*untyped);
    ASSERT_TRUE(value.contains("sum")) << text_of(*untyped);
    EXPECT_EQ(value.at("sum").get<int>(), 5);
}

std::string text_contents(const neomcp::ReadResult& read) {
    if (read.contents.empty()) return {};
    if (const auto* text = std::get_if<neomcp::TextResourceContents>(&read.contents.front())) return text->text;
    return {};
}

void check_resources(neomcp::Client& client) {
    const auto fixed = client.read_resource("test://static-text");
    ASSERT_TRUE(fixed) << fixed.error().message;
    EXPECT_EQ(text_contents(*fixed), "This is the content of the static text resource.");

    const auto family = client.read_resource("test://template/7/data");
    ASSERT_TRUE(family) << family.error().message;
    const json value = json::parse(text_contents(*family), nullptr, false);
    ASSERT_TRUE(value.is_object()) << text_contents(*family);
    ASSERT_TRUE(value.contains("id")) << text_contents(*family);
    EXPECT_EQ(value.at("id").get<std::string>(), "7");
}

void check_prompt(neomcp::Client& client) {
    std::map<std::string, std::string> arguments{{"arg1", "alpha"}, {"arg2", "beta"}};
    const auto prompt = client.get_prompt("test_prompt_with_arguments", std::move(arguments));
    ASSERT_TRUE(prompt) << prompt.error().message;
    ASSERT_FALSE(prompt->messages.empty());
    const auto* text = std::get_if<neomcp::TextContent>(&prompt->messages.front().content);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "Prompt with arguments: arg1='alpha', arg2='beta'");
}

void check_completion(neomcp::Client& client, const Profile& governing) {
    if (!client_requests(governing, "completion/complete")) return;
    // Where the revision has a capability for it, the server declares it.
    const neomcp::profile::Type* declared = governing.type("ServerCapabilities");
    if (declared != nullptr && declared->has_property("completions")) {
        const json capabilities = client.server().capabilities;
        ASSERT_TRUE(capabilities.is_object() && capabilities.contains("completions")) << capabilities.dump();
    }
    const auto completed =
        client.complete(neomcp::CompletionRef::prompt("test_prompt_with_arguments"), "arg1", "te");
    ASSERT_TRUE(completed) << completed.error().message;
    const std::vector<std::string> wanted{"test", "testing", "tested"};
    EXPECT_EQ(completed->values, wanted);
}

void check_ping_and_progress(neomcp::Client& client, const Profile& governing) {
    if (client_requests(governing, "ping")) {
        const auto pinged = client.ping();
        EXPECT_TRUE(pinged) << pinged.error().message;
    }
    std::atomic<int> reports{0};
    neomcp::RequestOptions call;
    call.on_progress = [&reports](const neomcp::Progress&) { ++reports; };
    const auto progressed = client.call_tool("test_tool_with_progress", json::object(), call);
    ASSERT_TRUE(progressed) << progressed.error().message;
    EXPECT_EQ(text_of(*progressed), "Progress test completed");
    EXPECT_EQ(reports.load(), 3);
}

void check_input(neomcp::Client& client, const Profile& governing) {
    if (can_ask(governing, "sampling/createMessage")) {
        SCOPED_TRACE("sampling");
        json arguments = json::object();
        arguments["prompt"] = "The capital of France?";
        const auto sampled = client.call_tool("test_sampling", std::move(arguments));
        ASSERT_TRUE(sampled) << sampled.error().message;
        EXPECT_FALSE(sampled->is_error) << text_of(*sampled);
        EXPECT_EQ(text_of(*sampled), "LLM response: " + std::string(model_answer));
    }
    if (can_ask(governing, "sampling/createMessage") && samples_with_tools(governing)) {
        SCOPED_TRACE("sampling with tools");
        // Served by the library's own servers (check_lists); another server may not have it.
        const auto tools = client.list_all_tools();
        ASSERT_TRUE(tools) << tools.error().message;
        if (contains(names_of(tools->items), sampling_tool_loop::tool_name)) {
            const auto looped = client.call_tool(std::string(sampling_tool_loop::tool_name));
            ASSERT_TRUE(looped) << looped.error().message;
            EXPECT_FALSE(looped->is_error) << text_of(*looped);
            EXPECT_EQ(text_of(*looped), sampling_tool_loop::expected);
        }
    }
    if (can_ask(governing, "elicitation/create")) {
        SCOPED_TRACE("elicitation");
        json arguments = json::object();
        arguments["message"] = "Who are you?";
        const auto asked = client.call_tool("test_elicitation", std::move(arguments));
        ASSERT_TRUE(asked) << asked.error().message;
        EXPECT_FALSE(asked->is_error) << text_of(*asked);
        EXPECT_TRUE(mentions(text_of(*asked), user_email)) << text_of(*asked);
    }
    if (can_ask(governing, "roots/list")) {
        SCOPED_TRACE("roots");
        const auto listed = client.call_tool("test_input_required_result_list_roots");
        ASSERT_TRUE(listed) << listed.error().message;
        EXPECT_FALSE(listed->is_error) << text_of(*listed);
        EXPECT_TRUE(mentions(text_of(*listed), root_uri)) << text_of(*listed);
    }
}

// The call is cancelled once the tool reports that it waits; the connection serves the next call.
void check_cancellation(neomcp::Client& client) {
    std::stop_source stop;
    neomcp::RequestOptions call;
    call.stop = stop.get_token();
    call.on_progress = [&stop](const neomcp::Progress&) { stop.request_stop(); };
    json arguments = json::object();
    arguments["milliseconds"] = 30'000;
    const auto began = std::chrono::steady_clock::now();
    const auto cancelled = client.call_tool("wait", std::move(arguments), call);
    ASSERT_FALSE(cancelled) << text_of(*cancelled);
    EXPECT_EQ(cancelled.error().kind, neomcp::ClientErrorKind::cancelled) << cancelled.error().message;
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(20));
    const auto after = client.call_tool("test_simple_text");
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_FALSE(after->is_error);
}

bool declares_tool_changes(const json& capabilities) {
    if (!capabilities.is_object() || !capabilities.contains("tools")) return false;
    const json& tools = capabilities.at("tools");
    return tools.is_object() && tools.contains("listChanged") && tools.at("listChanged").is_boolean() &&
           tools.at("listChanged").get<bool>();
}

// The server is made to change its tools; the change arrives on the subscription, and the tool it offered is listed.
void check_change(neomcp::Client& client) {
    const json capabilities = client.server().capabilities;
    ASSERT_TRUE(declares_tool_changes(capabilities)) << capabilities.dump();
    neomcp::ChangeFilter filter;
    filter.tools_list = true;
    auto subscription = client.listen(filter);
    ASSERT_TRUE(subscription) << subscription.error().message;
    EXPECT_TRUE(subscription->honoured().tools_list);

    const auto triggered = client.call_tool("test_trigger_tool_change");
    ASSERT_TRUE(triggered) << triggered.error().message;
    EXPECT_FALSE(triggered->is_error) << text_of(*triggered);

    auto waiting = std::async(std::launch::async, [&subscription] { return subscription->next(); });
    if (waiting.wait_for(change_bound) != std::future_status::ready) {
        ADD_FAILURE() << "no change arrived within the bound";
        EXPECT_TRUE(subscription->close());
    }
    const auto changed = waiting.get();
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(changed->kind, neomcp::ChangeKind::tools_list);

    const auto tools = client.list_all_tools();
    ASSERT_TRUE(tools) << tools.error().message;
    EXPECT_TRUE(contains(names_of(tools->items), "test_hidden_tool"));
    EXPECT_TRUE(subscription->close());
}

// The sampling handler's declaration that it takes requests with tools.
neomcp::SamplingSupport with_tools() {
    neomcp::SamplingSupport support;
    support.tools = true;
    return support;
}

}  // namespace

neomcp::ClientOptions client_options(const std::vector<std::string>& revisions) {
    neomcp::ClientOptions options;
    options.client_info.name = "client-matrix";
    options.client_info.version = "1.0.0";
    options.profiles = profiles_of(revisions);
    options.handlers.form([](const neomcp::ElicitationForm&) {
        json content = json::object();
        content["username"] = "ada";
        content["email"] = std::string(user_email);
        return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, std::move(content)};
    });
    // The model answers a plain request with model_answer, and plays its part in the rounds with a tool.
    options.handlers.sampling(
        [](const neomcp::SamplingRequest& request) { return sampling_tool_loop::model(request, model_answer); },
        with_tools());
    options.handlers.roots([] {
        neomcp::RootsAnswer answer;
        answer.roots.push_back(neomcp::Root{std::string(root_uri), std::string("root"), std::nullopt});
        return answer;
    });
    return options;
}

std::vector<std::string> revisions_defining(Transport transport) {
    std::vector<std::string> found;
    for (const std::string_view revision : neomcp::profile::builtin_revisions()) {
        if (defines(*neomcp::profile::builtin(revision), transport)) found.emplace_back(revision);
    }
    return found;
}

std::vector<std::string> revisions_of_era(Transport transport, Era era) {
    std::vector<std::string> found;
    for (const std::string& revision : revisions_defining(transport)) {
        if (neomcp::profile::builtin(revision)->era() == era) found.push_back(revision);
    }
    return found;
}

Expected expected_contact(Transport transport, const std::vector<std::string>& client_revisions,
                          const std::vector<std::string>& server_revisions) {
    Expected expected;
    expected.transport = transport;
    // The built-in revisions are sorted ascending: the last shared one is the newest.
    for (const std::string& revision : revisions_defining(transport)) {
        const bool client_has = client_revisions.empty() || contains(client_revisions, revision);
        const bool server_has = server_revisions.empty() || contains(server_revisions, revision);
        if (client_has && server_has) expected.revision = revision;
    }
    if (!expected.revision.empty()) {
        expected.kind = neomcp::profile::builtin(expected.revision)->era() == Era::modern
                            ? neomcp::ServerKind::per_request
                            : neomcp::ServerKind::handshake;
    }
    return expected;
}

void run_flow(neomcp::Client& client, const Expected& expected) {
    const auto verdict = client.verdict();
    ASSERT_TRUE(verdict.has_value());
    EXPECT_EQ(verdict->revision, expected.revision);
    EXPECT_EQ(verdict->kind, expected.kind);
    EXPECT_EQ(verdict->transport, expected.transport);
    const neomcp::profile::ProfilePtr governing = neomcp::profile::builtin(verdict->revision);
    ASSERT_TRUE(governing != nullptr) << verdict->revision;

    // Each step runs even when an earlier one failed, so that one run names every step that does not work.
    {
        SCOPED_TRACE("step: lists");
        check_lists(client, expected);
    }
    {
        SCOPED_TRACE("step: tool calls");
        check_tool_calls(client, *governing);
    }
    {
        SCOPED_TRACE("step: resources");
        check_resources(client);
    }
    {
        SCOPED_TRACE("step: prompt");
        check_prompt(client);
    }
    {
        SCOPED_TRACE("step: completion");
        check_completion(client, *governing);
    }
    {
        SCOPED_TRACE("step: ping and progress");
        check_ping_and_progress(client, *governing);
    }
    {
        SCOPED_TRACE("step: input requests");
        check_input(client, *governing);
    }
    {
        SCOPED_TRACE("step: cancellation");
        check_cancellation(client);
    }
    {
        SCOPED_TRACE("step: change of the tools");
        check_change(client);
    }
    {
        SCOPED_TRACE("step: close");
        const auto closed = client.close();
        EXPECT_TRUE(closed) << closed.error().message;
    }
}

std::string test_name(std::string text) {
    for (char& c : text) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return text;
}

}  // namespace client_matrix
