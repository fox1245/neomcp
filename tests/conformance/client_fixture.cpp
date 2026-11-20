// The client the official conformance runner tests (the CI job `conformance-client`): one flow per scenario of the
// runner's client suite, over Streamable HTTP to the URL the runner gives.
//
//   neomcp_conformance_client [--profiles=run|all] <url>
//
// The runner appends the URL of the scenario's server as the last argument; it is used exactly as given. The scenario
// is named by MCP_CONFORMANCE_SCENARIO, the revision the runner runs by MCP_CONFORMANCE_PROTOCOL_VERSION, and a
// scenario that hands the client data (the arguments of its tool calls) gives it as JSON in MCP_CONFORMANCE_CONTEXT.
//
// --profiles=run (the default) enables only the revision the runner names; --profiles=all enables every built-in
// revision, so that the client finds out by its probe which kind of server answers.
//
// The last line on standard output is one JSON object that says how the flow ended:
//   {"fixture":"neomcp-conformance-client","scenario":"...","profiles":"run","status":"ok"}
// with "status":"failed" and a "reason" when it did not. The exit status is 0 when the flow completed and 1 otherwise,
// also for a scenario this fixture does not know.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/profile.h"

namespace {

using neomcp::json;

constexpr std::string_view fixture_name = "neomcp-conformance-client";
constexpr std::string_view profiles_option = "--profiles=";
constexpr std::string_view profiles_run = "run";
constexpr std::string_view profiles_all = "all";
constexpr const char* scenario_variable = "MCP_CONFORMANCE_SCENARIO";
constexpr const char* revision_variable = "MCP_CONFORMANCE_PROTOCOL_VERSION";
constexpr const char* context_variable = "MCP_CONFORMANCE_CONTEXT";

struct Failure {
    std::string reason;
};

std::string describe(const neomcp::ClientError& error) {
    std::string text(neomcp::to_string(error.kind));
    if (error.code) text += " " + std::to_string(*error.code);
    if (!error.message.empty()) text += ": " + error.message;
    return text;
}

// Runs one scenario's flow against the client; throws Failure when a step fails.
struct Flow {
    neomcp::ClientOptions options;
    std::string url;
    json context;

    neomcp::Client open() {
        neomcp::HttpEndpoint endpoint;
        endpoint.url = url;
        // The runner's servers are on the loopback interface: no proxy of the environment may stand between.
        endpoint.proxy.mode = neomcp::ProxyMode::none;
        auto client = neomcp::Client::open(options, std::move(endpoint));
        if (!client) throw Failure{"open: " + describe(client.error())};
        return std::move(client).value();
    }

    static neomcp::ToolCall call(neomcp::Client& client, const std::string& name, json arguments = json::object()) {
        auto result = client.call_tool(name, std::move(arguments));
        if (!result) throw Failure{"tools/call " + name + ": " + describe(result.error())};
        return std::move(result).value();
    }

    static void close(neomcp::Client& client) {
        auto closed = client.close();
        if (!closed) throw Failure{"close: " + describe(closed.error())};
    }

    // Every tool the server lists, as the client keeps them: on Streamable HTTP without the tools whose input schema
    // marks parameters for headers against the rules.
    static std::vector<neomcp::ToolDefinition> tools(neomcp::Client& client) {
        auto listed = client.list_all_tools();
        if (!listed) throw Failure{"tools/list: " + describe(listed.error())};
        return std::move(listed).value().items;
    }

    // ---- the scenarios

    void initialize() {
        auto client = open();
        tools(client);
        close(client);
    }

    void tools_call() {
        auto client = open();
        tools(client);
        call(client, "add_numbers", json{{"a", 2}, {"b", 3}});
        close(client);
    }

    void elicitation_defaults() {
        options.handlers.form([](const neomcp::ElicitationForm& form) {
            return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, form.prefilled()};
        });
        auto client = open();
        call(client, "test_client_elicitation_defaults");
        close(client);
    }

    void sse_retry() {
        auto client = open();
        call(client, "test_reconnection");
        close(client);
    }

    void request_metadata() {
        auto client = open();
        tools(client);
        close(client);
    }

    void request_state() {
        options.handlers.form([](const neomcp::ElicitationForm&) {
            return neomcp::ElicitationAnswer{neomcp::ElicitationAction::accept, json{{"confirmed", true}}};
        });
        auto client = open();
        tools(client);
        // The call whose result asks for input, and an unrelated call at the same time: the answers go on the retry
        // of the first only.
        std::optional<Failure> echoed;
        std::thread asking([&] {
            try {
                call(client, "test_mrtr_echo_state");
            } catch (const Failure& failure) {
                echoed = failure;
            }
        });
        std::optional<Failure> unrelated;
        try {
            call(client, "test_mrtr_unrelated");
        } catch (const Failure& failure) {
            unrelated = failure;
        }
        asking.join();
        if (echoed) throw *echoed;
        if (unrelated) throw *unrelated;
        call(client, "test_mrtr_no_state");
        call(client, "test_mrtr_no_result_type");
        close(client);
    }

    void standard_headers() {
        auto client = open();
        for (const neomcp::ToolDefinition& tool : tools(client)) call(client, tool.name);
        auto resources = client.list_all_resources();
        if (!resources) throw Failure{"resources/list: " + describe(resources.error())};
        for (const neomcp::ResourceDefinition& resource : resources->items) {
            auto read = client.read_resource(resource.uri);
            if (!read) throw Failure{"resources/read " + resource.uri + ": " + describe(read.error())};
        }
        auto prompts = client.list_all_prompts();
        if (!prompts) throw Failure{"prompts/list: " + describe(prompts.error())};
        for (const neomcp::PromptDefinition& prompt : prompts->items) {
            auto got = client.get_prompt(prompt.name);
            if (!got) throw Failure{"prompts/get " + prompt.name + ": " + describe(got.error())};
        }
        close(client);
    }

    // The tool calls the scenario's context names, with their arguments as given: the client mirrors the marked
    // arguments into headers from the tools' listed input schemas.
    void custom_headers() {
        if (!context.is_object() || !context.contains("toolCalls") || !context["toolCalls"].is_array()) {
            throw Failure{"the scenario's context names no tool calls"};
        }
        auto client = open();
        tools(client);
        for (const json& one : context["toolCalls"]) {
            if (!one.is_object() || !one.contains("name") || !one["name"].is_string()) {
                throw Failure{"a tool call of the context has no name"};
            }
            call(client, one["name"].get<std::string>(), one.contains("arguments") ? one["arguments"] : json::object());
        }
        close(client);
    }

    // Every tool the client kept (the listing leaves out those whose header marks break the rules) is called.
    void invalid_tool_headers() {
        auto client = open();
        for (const neomcp::ToolDefinition& tool : tools(client)) call(client, tool.name, json{{"region", "us-west1"}});
        close(client);
    }

    void schema_ref() {
        auto client = open();
        tools(client);
        close(client);
    }

    // The input schema of the scenario's focal tool, as the client keeps it, sent back through its echo tool: the
    // scenario compares it with the schema it served.
    void schema_preservation() {
        auto client = open();
        std::optional<json> kept;
        for (const neomcp::ToolDefinition& tool : tools(client)) {
            if (tool.name == "json_schema_2020_12_tool") kept = tool.input_schema;
        }
        if (!kept) throw Failure{"the listing holds no tool json_schema_2020_12_tool"};
        call(client, "json_schema_echo", json{{"schema", std::move(*kept)}});
        close(client);
    }
};

using Scenario = void (Flow::*)();

const std::map<std::string, Scenario, std::less<>>& scenarios() {
    static const std::map<std::string, Scenario, std::less<>> table{
        {"initialize", &Flow::initialize},
        {"tools_call", &Flow::tools_call},
        {"elicitation-sep1034-client-defaults", &Flow::elicitation_defaults},
        {"sse-retry", &Flow::sse_retry},
        {"request-metadata", &Flow::request_metadata},
        {"sep-2322-client-request-state", &Flow::request_state},
        {"http-standard-headers", &Flow::standard_headers},
        {"http-custom-headers", &Flow::custom_headers},
        {"http-invalid-tool-headers", &Flow::invalid_tool_headers},
        {"json-schema-ref-no-deref", &Flow::schema_ref},
        {"json-schema-2020-12-preservation", &Flow::schema_preservation},
    };
    return table;
}

int report(std::string_view scenario, std::string_view profiles, const std::optional<std::string>& failure) {
    json line{{"fixture", std::string(fixture_name)},
              {"scenario", std::string(scenario)},
              {"profiles", std::string(profiles)},
              {"status", failure ? "failed" : "ok"}};
    if (failure) line["reason"] = *failure;
    std::fputs((line.dump() + "\n").c_str(), stdout);
    std::fflush(stdout);
    return failure ? 1 : 0;
}

// The value of an environment variable; empty when it is not set.
std::string variable(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::string();
    std::string copy(value);
    std::free(value);
    return copy;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::string profiles(profiles_run);
    std::optional<std::string> url;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument.substr(0, profiles_option.size()) == profiles_option) {
            profiles = std::string(argument.substr(profiles_option.size()));
        } else if (i == argc - 1) {
            url = std::string(argument);
        } else {
            return report(variable(scenario_variable), profiles, "unknown argument " + std::string(argument));
        }
    }
    const std::string scenario = variable(scenario_variable);
    if (!url) return report(scenario, profiles, std::string("no URL was given"));
    if (profiles != profiles_run && profiles != profiles_all) {
        return report(scenario, profiles, "--profiles takes run or all, not " + profiles);
    }
    const auto found = scenarios().find(scenario);
    if (found == scenarios().end()) return report(scenario, profiles, std::string("unknown scenario"));

    Flow flow;
    flow.url = *url;
    flow.options.client_info.name = std::string(fixture_name);
    flow.options.client_info.version = "0.0.1";
    if (profiles == profiles_run) {
        const std::string revision = variable(revision_variable);
        auto profile = neomcp::profile::builtin(revision);
        if (!profile) return report(scenario, profiles, "no built-in profile for the revision " + revision);
        flow.options.profiles.push_back(std::move(profile));
    }
    const std::string context = variable(context_variable);
    if (!context.empty()) {
        flow.context = json::parse(context, nullptr, false);
        if (flow.context.is_discarded()) return report(scenario, profiles, std::string("the context is not JSON"));
    }
    try {
        (flow.*(found->second))();
    } catch (const Failure& failure) {
        return report(scenario, profiles, failure.reason);
    }
    return report(scenario, profiles, std::nullopt);
}
