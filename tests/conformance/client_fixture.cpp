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
// The scenarios of authorization (auth/...) sign in with an OAuthAuthorizer. The runner's authorization server answers
// its authorization endpoint at once with a redirect to the redirect URI, so the fixture's prompt handler stands in for
// the person's browser: it requests the authorization URL, does not follow the redirect, and hands back the URL the
// redirect names. Credentials registered beforehand come from the context (client_id, client_secret and issuer).
//
// The last line on standard output is one JSON object that says how the flow ended:
//   {"fixture":"neomcp-conformance-client","scenario":"...","profiles":"run","status":"ok"}
// with "status":"failed" and a "reason" when it did not. The exit status is 0 when the flow completed and 1 otherwise,
// also for a scenario this fixture does not know. A scenario whose server or authorization server is built so that a
// correct client must refuse to go on completes when the client stopped with exactly the problem of authorization
// that scenario expects (expected_problems() below), and fails when it went on or stopped for another reason.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <curl/curl.h>

#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/client/oauth.h"
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

// The sign-in. The redirect URI is never listened on: the prompt handler reads where the authorization server sends
// the browser instead. The client ID metadata document URL is the one the runner's authorization servers accept as
// such without fetching it.
constexpr std::string_view redirect_uri = "http://127.0.0.1:3000/callback";
constexpr std::string_view client_id_metadata_url = "https://conformance-test.local/client-metadata.json";
// The members of the context that name credentials registered beforehand.
constexpr const char* context_client_id = "client_id";
constexpr const char* context_client_secret = "client_secret";
constexpr const char* context_issuer = "issuer";
// The request of the authorization URL, on the loopback interface.
constexpr std::chrono::milliseconds authorization_request_timeout{10000};
constexpr const char* authorization_request_protocols = "http,https";
constexpr const char* location_field = "Location";
constexpr long first_redirect_status = 300;
constexpr long last_redirect_status = 399;

struct Failure {
    std::string reason;
    // The client's error, when a step of the client failed.
    std::optional<neomcp::ClientError> error;
};

std::string describe(const neomcp::ClientError& error) {
    std::string text(neomcp::to_string(error.kind));
    if (error.code) text += " " + std::to_string(*error.code);
    if (error.authorization) {
        text += " (" + std::string(neomcp::to_string(error.authorization->problem));
        if (error.authorization->error_code) text += ", " + *error.authorization->error_code;
        text += ")";
    }
    if (!error.message.empty()) text += ": " + error.message;
    return text;
}

Failure failure_of(const std::string& step, const neomcp::ClientError& error) {
    return Failure{step + ": " + describe(error), error};
}

// ---- The prompt: the authorization request, as the person's browser would make it

struct CurlEasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};
using CurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;

std::size_t discard_body(char*, std::size_t size, std::size_t count, void*) { return size * count; }

// Stops the request once the sign-in no longer waits for the answer.
int stop_when_cancelled(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const neomcp::AuthorizationPromptContext*>(data)->cancelled() ? 1 : 0;
}

void prompt_trouble(const std::string& text) {
    std::fputs(("authorization request: " + text + "\n").c_str(), stderr);
}

// Requests the authorization URL without following its redirect and answers with the URL the redirect names, as it
// came; no answer when the authorization server sent no redirect.
neomcp::AuthorizationPromptAnswer follow_authorization(const neomcp::AuthorizationPromptInfo& prompt,
                                                       neomcp::AuthorizationPromptContext& context) {
    CurlEasy handle(curl_easy_init());
    if (!handle) {
        prompt_trouble("libcurl could not make a handle");
        return {};
    }
    CURL* easy = handle.get();
    curl_easy_setopt(easy, CURLOPT_URL, prompt.authorization_url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, authorization_request_protocols);
    // The runner's servers are on the loopback interface: no proxy of the environment may stand between.
    curl_easy_setopt(easy, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(authorization_request_timeout.count()));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &discard_body);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &stop_when_cancelled);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &context);
    const CURLcode performed = curl_easy_perform(easy);
    if (performed != CURLE_OK) {
        prompt_trouble(curl_easy_strerror(performed));
        return {};
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (status < first_redirect_status || status > last_redirect_status) {
        prompt_trouble("the authorization server answered " + std::to_string(status) + ", not a redirect");
        return {};
    }
    curl_header* location = nullptr;
    if (curl_easy_header(easy, location_field, 0, CURLH_HEADER, -1, &location) != CURLHE_OK || location == nullptr ||
        location->value == nullptr) {
        prompt_trouble("the redirect names no location");
        return {};
    }
    return neomcp::AuthorizationPromptAnswer{std::string(location->value)};
}

// The member `name` of the context when it is a string.
std::optional<std::string> context_string(const json& context, const char* name) {
    if (!context.is_object() || !context.contains(name) || !context.at(name).is_string()) return std::nullopt;
    return context.at(name).get<std::string>();
}

// Runs one scenario's flow against the client; throws Failure when a step fails.
struct Flow {
    neomcp::ClientOptions options;
    std::string url;
    json context;
    std::shared_ptr<neomcp::OAuthAuthorizer> authorizer;

    neomcp::Client open() {
        neomcp::HttpEndpoint endpoint;
        endpoint.url = url;
        // The runner's servers are on the loopback interface: no proxy of the environment may stand between.
        endpoint.proxy.mode = neomcp::ProxyMode::none;
        endpoint.authorizer = authorizer;
        auto client = neomcp::Client::open(options, std::move(endpoint));
        if (!client) throw failure_of("open", client.error());
        return std::move(client).value();
    }

    static neomcp::ToolCall call(neomcp::Client& client, const std::string& name, json arguments = json::object()) {
        auto result = client.call_tool(name, std::move(arguments));
        if (!result) throw failure_of("tools/call " + name, result.error());
        return std::move(result).value();
    }

    static void close(neomcp::Client& client) {
        auto closed = client.close();
        if (!closed) throw failure_of("close", closed.error());
    }

    // Every tool the server lists, as the client keeps them: on Streamable HTTP without the tools whose input schema
    // marks parameters for headers against the rules.
    static std::vector<neomcp::ToolDefinition> tools(neomcp::Client& client) {
        auto listed = client.list_all_tools();
        if (!listed) throw failure_of("tools/list", listed.error());
        return std::move(listed).value().items;
    }

    // The authorizer of the sign-in scenarios: the prompt above, the client ID metadata document URL (used where the
    // authorization server supports such documents), dynamic registration otherwise, and credentials registered
    // beforehand when the context names them.
    void sign_in_options() {
        neomcp::OAuthOptions oauth;
        oauth.redirect_uri = std::string(redirect_uri);
        oauth.client_id_metadata_url = std::string(client_id_metadata_url);
        oauth.client_name = std::string(fixture_name);
        oauth.prompt.handler(&follow_authorization);
        const auto client_id = context_string(context, context_client_id);
        const auto issuer = context_string(context, context_issuer);
        if (client_id && issuer) {
            neomcp::PreRegisteredClient registered;
            registered.issuer = *issuer;
            registered.client_id = *client_id;
            registered.client_secret = context_string(context, context_client_secret);
            oauth.pre_registered.push_back(std::move(registered));
        }
        auto created = neomcp::OAuthAuthorizer::create(std::move(oauth));
        if (!created) throw failure_of("the authorizer", created.error());
        authorizer = std::move(created).value();
        // What the authorizer reports while it works, for the run's log; it never holds a secret.
        options.on_diagnostic = [](const neomcp::ClientDiagnostic& diagnostic) {
            if (const auto* said = std::get_if<neomcp::Diagnostic>(&diagnostic)) {
                std::fputs(("diagnostic: " + said->message + "\n").c_str(), stderr);
            }
        };
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
        if (!resources) throw failure_of("resources/list", resources.error());
        for (const neomcp::ResourceDefinition& resource : resources->items) {
            auto read = client.read_resource(resource.uri);
            if (!read) throw failure_of("resources/read " + resource.uri, read.error());
        }
        auto prompts = client.list_all_prompts();
        if (!prompts) throw failure_of("prompts/list", prompts.error());
        for (const neomcp::PromptDefinition& prompt : prompts->items) {
            auto got = client.get_prompt(prompt.name);
            if (!got) throw failure_of("prompts/get " + prompt.name, got.error());
        }
        close(client);
    }

    // The tool calls the scenario's context names, with their arguments as given: the client mirrors the marked
    // arguments into headers from the tools' listed input schemas.
    void custom_headers() {
        if (!context.is_object() || !context.contains("toolCalls") || !context["toolCalls"].is_array()) {
            throw Failure{"the scenario's context names no tool calls", std::nullopt};
        }
        auto client = open();
        tools(client);
        for (const json& one : context["toolCalls"]) {
            if (!one.is_object() || !one.contains("name") || !one["name"].is_string()) {
                throw Failure{"a tool call of the context has no name", std::nullopt};
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
        if (!kept) throw Failure{"the listing holds no tool json_schema_2020_12_tool", std::nullopt};
        call(client, "json_schema_echo", json{{"schema", std::move(*kept)}});
        close(client);
    }

    // A server that asks for authorization: the client signs in where it refuses a request, lists the tools and calls
    // the scenario's tool with the token.
    void sign_in() {
        sign_in_options();
        auto client = open();
        tools(client);
        call(client, "test-tool");
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
        {"auth/metadata-default", &Flow::sign_in},
        {"auth/metadata-var1", &Flow::sign_in},
        {"auth/metadata-var2", &Flow::sign_in},
        {"auth/metadata-var3", &Flow::sign_in},
        {"auth/basic-cimd", &Flow::sign_in},
        {"auth/scope-from-www-authenticate", &Flow::sign_in},
        {"auth/scope-from-scopes-supported", &Flow::sign_in},
        {"auth/scope-omitted-when-undefined", &Flow::sign_in},
        {"auth/scope-step-up", &Flow::sign_in},
        {"auth/scope-retry-limit", &Flow::sign_in},
        {"auth/token-endpoint-auth-basic", &Flow::sign_in},
        {"auth/token-endpoint-auth-post", &Flow::sign_in},
        {"auth/token-endpoint-auth-none", &Flow::sign_in},
        {"auth/pre-registration", &Flow::sign_in},
        {"auth/resource-mismatch", &Flow::sign_in},
        {"auth/offline-access-scope", &Flow::sign_in},
        {"auth/offline-access-not-supported", &Flow::sign_in},
        {"auth/authorization-server-migration", &Flow::sign_in},
        {"auth/iss-supported", &Flow::sign_in},
        {"auth/iss-not-advertised", &Flow::sign_in},
        {"auth/iss-supported-missing", &Flow::sign_in},
        {"auth/iss-wrong-issuer", &Flow::sign_in},
        {"auth/iss-unexpected", &Flow::sign_in},
        {"auth/iss-normalized", &Flow::sign_in},
        {"auth/metadata-issuer-mismatch", &Flow::sign_in},
        {"auth/2025-03-26-oauth-metadata-backcompat", &Flow::sign_in},
        {"auth/2025-03-26-oauth-endpoint-fallback", &Flow::sign_in},
    };
    return table;
}

// The scenarios in which a correct client stops, each with the one problem of authorization it must stop with: the
// server asks again for the scope it was granted (scope-retry-limit), the protected resource metadata names another
// resource, the callback carries a wrong or no issuer where one is due, the authorization server's metadata names
// another issuer than its address.
const std::map<std::string, neomcp::AuthorizationProblem, std::less<>>& expected_problems() {
    using Problem = neomcp::AuthorizationProblem;
    static const std::map<std::string, Problem, std::less<>> table{
        {"auth/scope-retry-limit", Problem::insufficient_scope},
        {"auth/resource-mismatch", Problem::resource_mismatch},
        {"auth/iss-supported-missing", Problem::issuer_mismatch},
        {"auth/iss-wrong-issuer", Problem::issuer_mismatch},
        {"auth/iss-unexpected", Problem::issuer_mismatch},
        {"auth/iss-normalized", Problem::issuer_mismatch},
        {"auth/metadata-issuer-mismatch", Problem::metadata_issuer_mismatch},
    };
    return table;
}

// How a flow ended against what its scenario expects: empty when it ended as expected, otherwise why not.
std::optional<std::string> verdict(std::string_view scenario, const std::optional<Failure>& ended) {
    const auto expected = expected_problems().find(scenario);
    if (expected == expected_problems().end()) {
        if (ended) return ended->reason;
        return std::nullopt;
    }
    const std::string wanted =
        "the scenario expects the sign-in to fail with " + std::string(neomcp::to_string(expected->second));
    if (!ended) return wanted + ", but the flow completed";
    const bool matches = ended->error && ended->error->kind == neomcp::ClientErrorKind::authorization_failed &&
                         ended->error->authorization && ended->error->authorization->problem == expected->second;
    if (matches) return std::nullopt;
    return wanted + ", but " + ended->reason;
}

// libcurl's global state, for the prompt's requests, set up before any thread of the client starts.
class CurlGlobal {
public:
    CurlGlobal() : ok_(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
    ~CurlGlobal() {
        if (ok_) curl_global_cleanup();
    }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    bool ok() const noexcept { return ok_; }

private:
    bool ok_;
};

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
    const CurlGlobal curl_global;
    if (!curl_global.ok()) return report(scenario, profiles, std::string("libcurl could not be set up"));

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
    std::optional<Failure> ended;
    try {
        (flow.*(found->second))();
    } catch (const Failure& failure) {
        ended = failure;
    }
    return report(scenario, profiles, verdict(scenario, ended));
}
