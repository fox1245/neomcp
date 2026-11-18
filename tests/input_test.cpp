// Tests of input requests in the session engine: one scripted host, served under both mechanisms of `server_input`.
//
// Under input_required_result (the 2026-07-28 profile) the call's result asks and the test retries the call with the
// answers and the state; under server_request (the 2025-11-25 profile) the session sends requests and the test
// answers them as a client would. The expectations of the result form follow the conformance scenarios for input
// required results, one test per scenario. Every wait is on an event a test controls; the bound on a wait only limits
// how long a broken build takes to fail.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "core/base64.h"
#include "core/request_state.h"
#include "neomcp/feature_host.h"
#include "neomcp/input.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/mechanism.h"
#include "neomcp/profile.h"
#include "neomcp/session.h"
#include "neomcp/testing/peer.h"
#include "neomcp/tool_host.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* legacy_revision = "2025-11-25";
constexpr const char* older_revision = "2025-06-18";

json J(const char* text) { return json::parse(text); }

profile::ProfilePtr builtin(const std::string& revision) {
    auto found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

// Every capability a client can declare for input requests.
json all_capabilities() {
    return J(R"({"elicitation":{"form":{},"url":{}},"sampling":{"tools":{},"context":{}},"roots":{}})");
}

json name_schema() { return J(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})"); }

neomcp::ToolResult text_result(std::string text) {
    neomcp::ToolResult result;
    neomcp::TextContent block;
    block.text = std::move(text);
    result.content.emplace_back(std::move(block));
    return result;
}

neomcp::SamplingRequest sampling(std::string prompt) {
    neomcp::SamplingRequest request;
    neomcp::SamplingMessage message;
    neomcp::TextContent text;
    text.text = std::move(prompt);
    message.content.emplace_back(std::move(text));
    request.messages.push_back(std::move(message));
    request.max_tokens = 100;
    return request;
}

const neomcp::ElicitationAnswer* elicited(const neomcp::InputResponses& answers, const std::string& key) {
    const auto found = answers.find(key);
    return found == answers.end() ? nullptr : std::get_if<neomcp::ElicitationAnswer>(&found->second);
}

template <class T>
bool answered(const neomcp::InputResponses& answers, const std::string& key) {
    const auto found = answers.find(key);
    return found != answers.end() && std::holds_alternative<T>(found->second);
}

// ---------------------------------------------------------------------------------------------------------------
// The scripted host: tools, a prompt and a resource that ask, each deciding from the answers and its state alone.

class ScriptedHost : public neomcp::ToolHost,
                     public neomcp::PromptHost,
                     public neomcp::ResourceHost,
                     public neomcp::CompletionHost {
public:
    struct Entry {
        std::string name;
        neomcp::InputResponses answers;
        std::optional<std::string> state;
    };
    struct Parked {
        neomcp::CallRequest request;
        neomcp::CallControl control;
        neomcp::Completion done;
    };

    ScriptedHost() {
        for (const char* name :
             {"test_input_required_result_elicitation", "test_input_required_result_sampling",
              "test_input_required_result_list_roots", "test_input_required_result_request_state",
              "test_input_required_result_multiple_inputs", "test_input_required_result_multi_round",
              "test_input_required_result_tampered_state", "test_input_required_result_capabilities", "echo_answer",
              "asks_url", "asks_sampling_with_tools", "asks_nothing", "always_asks", "asks_three", "parked"}) {
            neomcp::ToolDefinition tool;
            tool.name = name;
            tool.description = "a tool that asks";
            tools_.push_back(std::move(tool));
        }
        neomcp::PromptDefinition prompt;
        prompt.name = "test_input_required_result_prompt";
        prompts_.push_back(std::move(prompt));
        neomcp::ResourceDefinition resource;
        resource.uri = "test://input-required";
        resource.name = "a resource that asks";
        resources_.push_back(std::move(resource));
    }

    neomcp::ResourceHost* resource_host() noexcept override { return this; }
    neomcp::PromptHost* prompt_host() noexcept override { return this; }
    neomcp::CompletionHost* completion_host() noexcept override { return this; }
    std::span<const neomcp::ToolDefinition> tools() const override { return tools_; }
    std::span<const neomcp::PromptDefinition> prompts() const override { return prompts_; }
    std::span<const neomcp::ResourceDefinition> resources() const override { return resources_; }
    std::span<const neomcp::ResourceTemplateDefinition> resource_templates() const override { return {}; }

    void call(const neomcp::CallRequest& request, neomcp::CallControl control, neomcp::Completion done) override {
        record(request.name, request.input_responses, request.request_state);
        if (request.name == "parked") {
            const std::lock_guard<std::mutex> lock(mutex_);
            parked_.push_back(Parked{request, control, done});
            changed_.notify_all();
            return;
        }
        done(decide(request, control));
    }

    void get(const neomcp::PromptRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        record(request.name, request.input_responses, request.request_state);
        if (const auto* answer = elicited(request.input_responses, "user_context")) {
            neomcp::PromptResult result;
            neomcp::PromptMessage message;
            neomcp::TextContent text;
            text.text = "Context: " + answer->content.dump();
            message.content = std::move(text);
            result.messages.push_back(std::move(message));
            done(neomcp::FeatureOutcome(std::move(result)));
            return;
        }
        neomcp::InputRequired asked;
        asked.requests.emplace("user_context",
                               neomcp::ElicitationForm{"What is the context?",
                                                       J(R"({"type":"object","properties":{"context":{"type":"string"}}})")});
        done(std::move(asked));
    }

    void read(const neomcp::ReadRequest& request, neomcp::CallControl, neomcp::Completion done) override {
        record(request.uri, request.input_responses, request.request_state);
        const auto found = request.input_responses.find("roots");
        if (found != request.input_responses.end()) {
            if (const auto* roots = std::get_if<neomcp::RootsAnswer>(&found->second)) {
                neomcp::ReadResult result;
                result.contents.emplace_back(neomcp::TextResourceContents{
                    request.uri, "text/plain", std::to_string(roots->roots.size()) + " roots", std::nullopt});
                result.cache = neomcp::CacheHints{60000, neomcp::CacheScope::public_scope};
                done(neomcp::FeatureOutcome(std::move(result)));
                return;
            }
        }
        neomcp::InputRequired asked;
        asked.requests.emplace("roots", neomcp::RootsRequest{});
        done(std::move(asked));
    }

    void complete(const neomcp::CompletionRequest&, neomcp::CallControl, neomcp::Completion done) override {
        neomcp::InputRequired asked;
        asked.requests.emplace("roots", neomcp::RootsRequest{});
        done(std::move(asked));
    }

    void awaiting_input(neomcp::InputWait wait) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        waits_.push_back(std::move(wait));
        changed_.notify_all();
    }

    int entries(const std::string& name) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(log_.begin(), log_.end(), [&](const Entry& e) { return e.name == name; }));
    }
    std::optional<Entry> last(const std::string& name) {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
            if (it->name == name) return *it;
        }
        return std::nullopt;
    }
    std::optional<neomcp::InputWait> wait_for_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, patience, [&] { return !waits_.empty(); })) return std::nullopt;
        neomcp::InputWait wait = std::move(waits_.front());
        waits_.pop_front();
        return wait;
    }
    std::optional<Parked> wait_parked() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, patience, [&] { return !parked_.empty(); })) return std::nullopt;
        Parked parked = std::move(parked_.front());
        parked_.pop_front();
        return parked;
    }

private:
    void record(const std::string& name, const neomcp::InputResponses& answers, const std::optional<std::string>& state) {
        const std::lock_guard<std::mutex> lock(mutex_);
        log_.push_back(Entry{name, answers, state});
    }

    neomcp::CallOutcome decide(const neomcp::CallRequest& request, const neomcp::CallControl& control) {
        const auto& answers = request.input_responses;
        const auto& state = request.request_state;
        const std::string& name = request.name;
        neomcp::InputRequired asked;
        if (name == "test_input_required_result_elicitation" || name == "echo_answer") {
            const auto* answer = elicited(answers, "user_name");
            if (answer != nullptr && answer->action == neomcp::ElicitationAction::accept && answer->content.contains("name")) {
                return text_result("Hello, " + answer->content.at("name").get<std::string>());
            }
            asked.requests.emplace("user_name", neomcp::ElicitationForm{"What is your name?", name_schema()});
        } else if (name == "test_input_required_result_sampling") {
            if (answered<neomcp::SamplingAnswer>(answers, "sample_request")) return text_result("sampled");
            asked.requests.emplace("sample_request", sampling("What is the capital of France?"));
        } else if (name == "test_input_required_result_list_roots") {
            if (answered<neomcp::RootsAnswer>(answers, "roots")) return text_result("listed");
            asked.requests.emplace("roots", neomcp::RootsRequest{});
        } else if (name == "test_input_required_result_request_state" ||
                   name == "test_input_required_result_tampered_state") {
            if (state == std::optional<std::string>("awaiting-confirmation") && elicited(answers, "confirm") != nullptr) {
                return text_result("confirmed");
            }
            asked.requests.emplace("confirm", neomcp::ElicitationForm{"Confirm?", name_schema()});
            asked.state = "awaiting-confirmation";
        } else if (name == "test_input_required_result_multiple_inputs") {
            if (state == std::optional<std::string>("multi") && elicited(answers, "user_name") != nullptr &&
                answered<neomcp::SamplingAnswer>(answers, "summary") && answered<neomcp::RootsAnswer>(answers, "roots")) {
                return text_result("all three");
            }
            asked.requests.emplace("user_name", neomcp::ElicitationForm{"Name?", name_schema()});
            asked.requests.emplace("summary", sampling("Summarise"));
            asked.requests.emplace("roots", neomcp::RootsRequest{});
            asked.state = "multi";
        } else if (name == "test_input_required_result_multi_round") {
            if (state == std::optional<std::string>("round=2") && elicited(answers, "age") != nullptr) {
                return text_result("both rounds done");
            }
            if (state == std::optional<std::string>("round=1") && elicited(answers, "name") != nullptr) {
                asked.requests.emplace("age", neomcp::ElicitationForm{"Age?", name_schema()});
                asked.state = "round=2";
            } else {
                asked.requests.emplace("name", neomcp::ElicitationForm{"Name?", name_schema()});
                asked.state = "round=1";
            }
        } else if (name == "test_input_required_result_capabilities") {
            const neomcp::InputRequest form = neomcp::ElicitationForm{"Name?", name_schema()};
            const neomcp::InputRequest sample = sampling("Guess the name");
            if (!answers.empty()) return text_result("answered");
            if (control.client_supports(form)) {
                asked.requests.emplace("user_name", form);
            } else if (control.client_supports(sample)) {
                asked.requests.emplace("guess", sample);
            } else {
                return text_result("nothing to ask with");
            }
        } else if (name == "asks_url") {
            if (elicited(answers, "visit") != nullptr) return text_result("visited");
            asked.requests.emplace("visit", neomcp::ElicitationUrl{"Set your key", "https://example.com/key", std::nullopt});
        } else if (name == "asks_sampling_with_tools") {
            if (answered<neomcp::SamplingAnswer>(answers, "agent")) return text_result("used tools");
            neomcp::SamplingRequest request = sampling("Use the tool");
            neomcp::ToolDefinition tool;
            tool.name = "lookup";
            request.tools.push_back(std::move(tool));
            asked.requests.emplace("agent", std::move(request));
        } else if (name == "asks_nothing") {
            if (state == std::optional<std::string>("later")) return text_result("served after shedding");
            asked.state = "later";
        } else if (name == "always_asks") {
            asked.requests.emplace("again", neomcp::ElicitationForm{"Again?", name_schema()});
        } else if (name == "asks_three") {
            if (answers.size() == 3) return text_result("three answers");
            asked.requests.emplace("a", neomcp::ElicitationForm{"A?", name_schema()});
            asked.requests.emplace("b", neomcp::ElicitationForm{"B?", name_schema()});
            asked.requests.emplace("c", neomcp::ElicitationForm{"C?", name_schema()});
        }
        return asked;
    }

    std::vector<neomcp::ToolDefinition> tools_;
    std::vector<neomcp::PromptDefinition> prompts_;
    std::vector<neomcp::ResourceDefinition> resources_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Entry> log_;
    std::deque<neomcp::InputWait> waits_;
    std::deque<Parked> parked_;
};

neomcp::SessionOptions input_options() {
    neomcp::SessionOptions options;
    options.server_info = neomcp::Implementation{"test-server", "1.0.0", std::nullopt, std::nullopt, std::nullopt, {}};
    options.request_state_keys = {std::string(32, 'k')};
    return options;
}

const json& result_of(const json& answer) { return answer.at("result"); }

bool asks(const json& answer) {
    return answer.contains("result") && result_of(answer).value("resultType", std::string()) == "input_required";
}

bool completes(const json& answer) {
    return answer.contains("result") && result_of(answer).value("resultType", std::string("complete")) == "complete";
}

// ---------------------------------------------------------------------------------------------------------------
// A client of the modern revision: every request names its revision and its capabilities.

struct Modern {
    explicit Modern(neomcp::SessionOptions options = input_options(), json capabilities = all_capabilities())
        : peer(host, std::move(options)), profile(builtin(modern_revision)), caps(std::move(capabilities)) {}

    json request(const std::string& method, json params) {
        params["_meta"] = Peer::request_meta(*profile, caps);
        json message = json::object();
        message["jsonrpc"] = "2.0";
        message["id"] = next_id++;
        message["method"] = method;
        message["params"] = std::move(params);
        return message;
    }
    json ask(const std::string& method, json params) {
        const auto answer = peer.request(request(method, std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer within the patience");
        return *answer;
    }
    json call(const std::string& tool, json extra = json::object()) {
        json params = std::move(extra);
        params["name"] = tool;
        if (!params.contains("arguments")) params["arguments"] = json::object();
        return ask("tools/call", std::move(params));
    }
    // Retries a call with answers and the state the earlier answer carried.
    json retry(const std::string& tool, const json& earlier, json answers, json arguments = json::object()) {
        json params = json::object();
        params["arguments"] = std::move(arguments);
        params["inputResponses"] = std::move(answers);
        if (result_of(earlier).contains("requestState")) params["requestState"] = result_of(earlier).at("requestState");
        return call(tool, std::move(params));
    }

    ScriptedHost host;
    Peer peer;
    profile::ProfilePtr profile;
    json caps;
    int next_id = 1;
};

json accept(json content) {
    json answer = json::object();
    answer["action"] = "accept";
    answer["content"] = std::move(content);
    return answer;
}

json sampled() { return J(R"({"role":"assistant","content":{"type":"text","text":"Paris"},"model":"m1","stopReason":"endTurn"})"); }

json roots_answer() { return J(R"({"roots":[{"uri":"file:///home/user/project","name":"Project"}]})"); }

json one(const char* key, json value) {
    json out = json::object();
    out[key] = std::move(value);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// input_required_result: the fourteen scenarios of the result form

TEST(InputRequiredResult, Elicitation) {
    Modern client;
    const json first = client.call("test_input_required_result_elicitation");
    ASSERT_TRUE(asks(first)) << first.dump();
    const json& request = result_of(first).at("inputRequests").at("user_name");
    EXPECT_EQ(request.at("method"), "elicitation/create");
    EXPECT_EQ(request.at("params").at("mode"), "form");
    EXPECT_EQ(request.at("params").at("message"), "What is your name?");
    EXPECT_EQ(request.at("params").at("requestedSchema"), name_schema());
    ASSERT_TRUE(result_of(first).at("requestState").is_string());

    const json second =
        client.retry("test_input_required_result_elicitation", first, one("user_name", accept(J(R"({"name":"Alice"})"))));
    ASSERT_TRUE(completes(second)) << second.dump();
    EXPECT_EQ(result_of(second).at("resultType"), "complete");
    ASSERT_FALSE(result_of(second).at("content").empty());
    EXPECT_EQ(result_of(second).at("content").at(0).at("text"), "Hello, Alice");
    EXPECT_EQ(client.host.entries("test_input_required_result_elicitation"), 2) << "entered again from its start";
}

TEST(InputRequiredResult, Sampling) {
    Modern client;
    const json first = client.call("test_input_required_result_sampling");
    ASSERT_TRUE(asks(first)) << first.dump();
    const json& request = result_of(first).at("inputRequests").at("sample_request");
    EXPECT_EQ(request.at("method"), "sampling/createMessage");
    EXPECT_EQ(request.at("params").at("maxTokens"), 100);
    EXPECT_EQ(request.at("params").at("messages").at(0).at("content").at("text"), "What is the capital of France?");
    const json second = client.retry("test_input_required_result_sampling", first, one("sample_request", sampled()));
    EXPECT_TRUE(completes(second)) << second.dump();
}

TEST(InputRequiredResult, ListRoots) {
    Modern client;
    const json first = client.call("test_input_required_result_list_roots");
    ASSERT_TRUE(asks(first)) << first.dump();
    const json& request = result_of(first).at("inputRequests").at("roots");
    EXPECT_EQ(request.at("method"), "roots/list");
    EXPECT_FALSE(request.contains("params"));
    const json second = client.retry("test_input_required_result_list_roots", first, one("roots", roots_answer()));
    EXPECT_TRUE(completes(second)) << second.dump();
}

TEST(InputRequiredResult, RequestStateComesBackAsTheHostGaveIt) {
    Modern client;
    const json first = client.call("test_input_required_result_request_state");
    ASSERT_TRUE(asks(first)) << first.dump();
    ASSERT_GE(result_of(first).at("inputRequests").size(), 1u);
    const std::string sealed = result_of(first).at("requestState").get<std::string>();
    ASSERT_FALSE(sealed.empty());
    EXPECT_EQ(sealed.find("awaiting-confirmation"), std::string::npos) << "the state never travels as it is";
    const json second = client.retry("test_input_required_result_request_state", first,
                                     one("confirm", accept(J(R"({"name":"yes"})"))));
    ASSERT_TRUE(completes(second)) << second.dump();
    const auto entry = client.host.last("test_input_required_result_request_state");
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->state, std::optional<std::string>("awaiting-confirmation"));
}

TEST(InputRequiredResult, MultipleInputs) {
    Modern client;
    const json first = client.call("test_input_required_result_multiple_inputs");
    ASSERT_TRUE(asks(first)) << first.dump();
    std::set<std::string> methods;
    for (const auto& entry : *result_of(first).at("inputRequests").get_ptr<const json::object_t*>()) {
        methods.insert(entry.second.at("method").get<std::string>());
    }
    EXPECT_EQ(methods, (std::set<std::string>{"elicitation/create", "sampling/createMessage", "roots/list"}));
    json answers = json::object();
    answers["user_name"] = accept(J(R"({"name":"Alice"})"));
    answers["summary"] = sampled();
    answers["roots"] = roots_answer();
    const json second = client.retry("test_input_required_result_multiple_inputs", first, answers);
    EXPECT_TRUE(completes(second)) << second.dump();
}

TEST(InputRequiredResult, MultiRoundWithAChangedState) {
    Modern client;
    const std::string tool = "test_input_required_result_multi_round";
    const json first = client.call(tool);
    ASSERT_TRUE(asks(first)) << first.dump();
    const json second = client.retry(tool, first, one("name", accept(J(R"({"name":"Alice"})"))));
    ASSERT_TRUE(asks(second)) << second.dump();
    EXPECT_TRUE(result_of(second).at("inputRequests").contains("age"));
    EXPECT_NE(result_of(second).at("requestState"), result_of(first).at("requestState"));
    const json third = client.retry(tool, second, one("age", accept(J(R"({"name":"42"})"))));
    EXPECT_TRUE(completes(third)) << third.dump();
    // The retry carries only the latest round's answers.
    const auto entry = client.host.last(tool);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->answers.size(), 1u);
}

TEST(InputRequiredResult, TamperedStateIsAnError) {
    Modern client;
    const json first = client.call("test_input_required_result_tampered_state");
    ASSERT_TRUE(asks(first)) << first.dump();
    json params = json::object();
    params["arguments"] = json::object();
    params["inputResponses"] = one("confirm", accept(J(R"({"name":"yes"})")));
    params["requestState"] = result_of(first).at("requestState").get<std::string>() + "-TAMPERED";
    const json second = client.call("test_input_required_result_tampered_state", params);
    ASSERT_TRUE(second.contains("error")) << second.dump();
    EXPECT_EQ(second.at("error").at("code"), neomcp::error_code::invalid_params);
    EXPECT_EQ(second.at("error").at("message"), "Invalid or expired requestState");
    EXPECT_EQ(second.at("error").at("data"), J(R"({"reason":"invalid_request_state"})"));
    EXPECT_EQ(client.host.entries("test_input_required_result_tampered_state"), 1) << "refused before the host";
}

TEST(InputRequiredResult, CapabilitiesLetTheHostAskForSomethingElse) {
    Modern client(input_options(), J(R"({"sampling":{}})"));
    const json first = client.call("test_input_required_result_capabilities");
    ASSERT_TRUE(asks(first)) << first.dump();
    ASSERT_GE(result_of(first).at("inputRequests").size(), 1u);
    for (const auto& entry : *result_of(first).at("inputRequests").get_ptr<const json::object_t*>()) {
        EXPECT_NE(entry.second.at("method"), "elicitation/create") << entry.first;
    }
}

TEST(InputRequiredResult, APromptAsks) {
    Modern client;
    json params = json::object();
    params["name"] = "test_input_required_result_prompt";
    const json first = client.ask("prompts/get", params);
    ASSERT_TRUE(asks(first)) << first.dump();
    EXPECT_EQ(result_of(first).at("inputRequests").at("user_context").at("method"), "elicitation/create");
    params["inputResponses"] = one("user_context", accept(J(R"({"context":"testing"})")));
    params["requestState"] = result_of(first).at("requestState");
    const json second = client.ask("prompts/get", params);
    ASSERT_TRUE(completes(second)) << second.dump();
    EXPECT_FALSE(result_of(second).at("messages").empty());
}

TEST(InputRequiredResult, AResourceReadAsksAndItsRetriedResultIsNotCached) {
    Modern client;
    json params = json::object();
    params["uri"] = "test://input-required";
    const json first = client.ask("resources/read", params);
    ASSERT_TRUE(asks(first)) << first.dump();
    EXPECT_FALSE(result_of(first).contains("ttlMs")) << "an asking result carries no caching hints";
    params["inputResponses"] = one("roots", roots_answer());
    params["requestState"] = result_of(first).at("requestState");
    const json second = client.ask("resources/read", params);
    ASSERT_TRUE(completes(second)) << second.dump();
    EXPECT_EQ(result_of(second).at("contents").at(0).at("text"), "1 roots");
    EXPECT_EQ(result_of(second).at("ttlMs"), 0) << "the host's hints do not apply to a retry";
    EXPECT_EQ(result_of(second).at("cacheScope"), "private");
}

TEST(InputRequiredResult, ResultTypeIsAnOwnMember) {
    Modern client;
    const json first = client.call("test_input_required_result_elicitation");
    ASSERT_TRUE(first.contains("result"));
    const auto& members = *result_of(first).get_ptr<const json::object_t*>();
    ASSERT_NE(members.find("resultType"), members.end());
    EXPECT_EQ(members.find("resultType")->second, "input_required");
}

TEST(InputRequiredResult, OnlyTheRequestsThePageListsMayAsk) {
    // basic/patterns/mrtr.mdx, "Supported Requests": prompts/get, resources/read and tools/call, and no other.
    const std::set<std::string> page{"prompts/get", "resources/read", "tools/call"};
    const auto modern = builtin(modern_revision);
    const auto mechanism = neomcp::builtin_mechanisms().make_input(modern->mechanism(profile::Slot::server_input));
    ASSERT_TRUE(mechanism);
    std::set<std::string> asking;
    for (const auto& request : modern->requests()) {
        if (mechanism->asks_on(*modern, request.method)) asking.insert(request.method);
    }
    EXPECT_EQ(asking, page);

    Modern client;
    for (const char* method : {"tools/list", "prompts/list"}) {
        const json answer = client.ask(method, json::object());
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_EQ(result_of(answer).at("resultType"), "complete") << method;
    }
    // A host that asks on a completion is answered with an internal error, never with a result that asks.
    json params = J(R"({"ref":{"type":"ref/prompt","name":"test_input_required_result_prompt"},"argument":{"name":"a","value":"b"}})");
    const json completion = client.ask("completion/complete", params);
    ASSERT_TRUE(completion.contains("error")) << completion.dump();
    EXPECT_EQ(completion.at("error").at("code"), neomcp::error_code::internal_error);
}

TEST(InputRequiredResult, MissingAnswersAreAskedForAgain) {
    Modern client;
    const std::string tool = "test_input_required_result_elicitation";
    const json first = client.call(tool);
    ASSERT_TRUE(asks(first));
    // An answer under another key, with the state: the host asks again.
    const json second = client.retry(tool, first, one("wrong_key", accept(J(R"({"data":"wrong"})"))));
    ASSERT_TRUE(asks(second)) << second.dump();
    // The same without a state: answers that no state says were asked for are no answers.
    json params = json::object();
    params["inputResponses"] = one("wrong_key", accept(J(R"({"data":"wrong"})")));
    const json third = client.call(tool, params);
    EXPECT_TRUE(asks(third)) << third.dump();
}

TEST(InputRequiredResult, ExtraAnswersAreIgnored) {
    Modern client;
    const std::string tool = "test_input_required_result_elicitation";
    const json first = client.call(tool);
    json answers = one("user_name", accept(J(R"({"name":"Alice"})")));
    answers["unknown_extra_key"] = accept(J(R"({"foo":"bar"})"));
    answers["another_unexpected"] = 12345;  // not even the shape of an answer: ignored, not refused
    const json second = client.retry(tool, first, answers);
    ASSERT_TRUE(completes(second)) << second.dump();
    const auto entry = client.host.last(tool);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->answers.size(), 1u) << "only the asked key reaches the host";

    // Without a state nothing says what was asked: the host asks again (the stricter reading; see the session's
    // documentation).
    json params = json::object();
    params["inputResponses"] = answers;
    EXPECT_TRUE(asks(client.call(tool, params)));
}

TEST(InputRequiredResult, MalformedAnswersAreRefused) {
    Modern client;
    const std::string tool = "test_input_required_result_elicitation";
    const json first = client.call(tool);
    // An answer of the wrong shape for the request it answers.
    for (const json& wrong : {json(12345), J(R"({"action":"maybe"})"), J(R"({"action":"accept","content":{"n":{"x":1}}})"),
                              sampled()}) {
        const json answer = client.retry(tool, first, one("user_name", wrong));
        ASSERT_TRUE(answer.contains("error")) << wrong.dump() << " " << answer.dump();
        EXPECT_EQ(answer.at("error").at("code"), neomcp::error_code::invalid_params) << wrong.dump();
    }
    // inputResponses that is not an object at all.
    json params = json::object();
    params["inputResponses"] = nullptr;
    const json null_answers = client.call(tool, params);
    ASSERT_TRUE(null_answers.contains("error")) << null_answers.dump();
    EXPECT_EQ(null_answers.at("error").at("code"), neomcp::error_code::invalid_params);
    EXPECT_EQ(client.host.entries(tool), 1);
}

TEST(InputRequiredResult, AHostThatAsksForNothingGetsItsStateBack) {
    Modern client;
    const json first = client.call("asks_nothing");
    ASSERT_TRUE(asks(first)) << first.dump();
    EXPECT_FALSE(result_of(first).contains("inputRequests"));
    EXPECT_TRUE(result_of(first).at("requestState").is_string()) << "every asking result has at least one of the two";
    json params = json::object();
    params["requestState"] = result_of(first).at("requestState");
    EXPECT_TRUE(completes(client.call("asks_nothing", params)));
}

TEST(InputRequiredResult, ArgumentsBindTheStateWhateverTheirOrder) {
    Modern client;
    const std::string tool = "test_input_required_result_elicitation";
    const json arguments = J(R"({"b":1.0,"a":{"y":2,"x":[1,2]}})");
    json params = json::object();
    params["arguments"] = arguments;
    const json first = client.call(tool, params);
    ASSERT_TRUE(asks(first));
    const json answers = one("user_name", accept(J(R"({"name":"Alice"})")));
    EXPECT_TRUE(completes(client.retry(tool, first, answers, J(R"({"a":{"x":[1,2],"y":2},"b":1})"))));
    const json changed = client.retry(tool, first, answers, J(R"({"a":{"x":[2,1],"y":2},"b":1})"));
    ASSERT_TRUE(changed.contains("error")) << changed.dump();
    EXPECT_EQ(changed.at("error").at("message"), "Invalid or expired requestState");
}

// ---------------------------------------------------------------------------------------------------------------
// Capabilities

struct Tagged {
    void write(neomcp::Output output) {
        const std::lock_guard<std::mutex> lock(mutex);
        outputs.push_back(std::move(output));
        changed.notify_all();
    }
    std::optional<neomcp::Output> wait_kind(neomcp::OutputKind kind, std::size_t skip = 0) {
        std::unique_lock<std::mutex> lock(mutex);
        std::optional<neomcp::Output> found;
        changed.wait_for(lock, patience, [&] {
            std::size_t seen = 0;
            for (const auto& output : outputs) {
                if (output.kind == kind && seen++ == skip) {
                    found = output;
                    return true;
                }
            }
            return false;
        });
        return found;
    }
    std::vector<neomcp::Output> all() {
        const std::lock_guard<std::mutex> lock(mutex);
        return outputs;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<neomcp::Output> outputs;
};

std::unique_ptr<neomcp::Session> tagged(ScriptedHost& host, Tagged& sink, neomcp::SessionOptions options = input_options()) {
    auto made = neomcp::Session::create_tagged(std::move(options), host,
                                               [&sink](neomcp::Output output) { sink.write(std::move(output)); });
    if (auto* error = std::get_if<neomcp::CreateError>(&made)) throw std::runtime_error(error->message);
    return std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
}

json modern_call(const std::string& tool, int id, const json& capabilities) {
    json params = json::object();
    params["name"] = tool;
    params["arguments"] = json::object();
    params["_meta"] = Peer::request_meta(*builtin(modern_revision), capabilities);
    json message = json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["method"] = "tools/call";
    message["params"] = std::move(params);
    return message;
}

TEST(Capabilities, AMissingCapabilityIsTheRevisionsErrorWithItsReason) {
    ScriptedHost host;
    Tagged sink;
    auto session = tagged(host, sink);
    session->receive(1, neomcp::serialize(modern_call("test_input_required_result_elicitation", 1, J(R"({"sampling":{}})"))));
    const auto output = sink.wait_kind(neomcp::OutputKind::response);
    ASSERT_TRUE(output);
    EXPECT_EQ(output->refusal, neomcp::RefusalReason::missing_client_capability);
    const json answer = json::parse(output->line);
    ASSERT_TRUE(answer.contains("error")) << answer.dump();
    EXPECT_EQ(answer.at("error").at("code"), -32021);
    EXPECT_EQ(answer.at("error").at("data"), J(R"({"requiredCapabilities":{"elicitation":{"form":{}}}})"));
    EXPECT_EQ(neomcp::to_string(neomcp::RefusalReason::missing_client_capability), "missing_client_capability");
}

TEST(Capabilities, ElicitationModes) {
    struct Case {
        const char* capabilities;
        const char* tool;
        bool asks;
    };
    const Case cases[] = {
        {R"({"elicitation":{}})", "test_input_required_result_elicitation", true},  // bare: forms only
        {R"({"elicitation":{}})", "asks_url", false},
        {R"({"elicitation":{"url":{}}})", "asks_url", true},
        {R"({"elicitation":{"url":{}}})", "test_input_required_result_elicitation", false},  // a URL only
        {R"({"elicitation":{"form":{}}})", "test_input_required_result_elicitation", true},
        {R"({"elicitation":true})", "test_input_required_result_elicitation", false},
        {R"({"sampling":{}})", "asks_sampling_with_tools", false},
        {R"({"sampling":{"tools":{}}})", "asks_sampling_with_tools", true},
        {R"({"roots":{}})", "test_input_required_result_list_roots", true},
        {R"({})", "test_input_required_result_list_roots", false},
    };
    for (const auto& c : cases) {
        Modern client(input_options(), J(c.capabilities));
        const json answer = client.call(c.tool);
        if (c.asks) {
            EXPECT_TRUE(asks(answer)) << c.capabilities << " " << c.tool << " " << answer.dump();
        } else {
            ASSERT_TRUE(answer.contains("error")) << c.capabilities << " " << c.tool << " " << answer.dump();
            EXPECT_EQ(answer.at("error").at("code"), -32021) << c.capabilities << " " << c.tool;
        }
    }
    Modern url_client(input_options(), J(R"({"elicitation":{"url":{}}})"));
    const json url = url_client.call("asks_url");
    const json& params = result_of(url).at("inputRequests").at("visit").at("params");
    EXPECT_EQ(params.at("mode"), "url");
    EXPECT_EQ(params.at("url"), "https://example.com/key");
    EXPECT_FALSE(params.contains("elicitationId")) << "this revision's URL elicitation has no id";
}

// ---------------------------------------------------------------------------------------------------------------
// Sealing: every way a state can be wrong is answered the same way, and the real cause goes to the diagnostic.

struct Rejections {
    void add(const neomcp::RequestStateRejection& rejection) {
        const std::lock_guard<std::mutex> lock(mutex);
        problems.push_back(rejection.problem);
    }
    std::vector<neomcp::RequestStateProblem> taken() {
        const std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(problems, {});
    }
    std::mutex mutex;
    std::vector<neomcp::RequestStateProblem> problems;
};

TEST(Sealing, EveryTamperingIsAnsweredIdentically) {
    using neomcp::RequestStateProblem;
    Rejections rejections;
    auto clock_now = std::make_shared<std::atomic<std::int64_t>>(1800000000);
    neomcp::SessionOptions options = input_options();
    options.on_request_state_rejected = [&](const neomcp::RequestStateRejection& r) { rejections.add(r); };
    options.clock = [clock_now] { return std::chrono::system_clock::time_point(std::chrono::seconds(clock_now->load())); };
    Modern client(options);
    const std::string tool = "test_input_required_result_tampered_state";
    const json args = J(R"({"x":1})");
    json params = json::object();
    params["arguments"] = args;
    const json first = client.call(tool, params);
    ASSERT_TRUE(asks(first)) << first.dump();
    const std::string sealed = result_of(first).at("requestState").get<std::string>();
    const auto dot = sealed.find('.');
    ASSERT_NE(dot, std::string::npos);
    const json answers = one("confirm", accept(J(R"({"name":"yes"})")));

    // The state of another tool and the state of another key, both valid where they were issued.
    const json other_tool = client.call("test_input_required_result_request_state", params);
    neomcp::SessionOptions other_key = options;
    other_key.request_state_keys = {std::string(32, 'z')};
    Modern stranger(other_key);
    const json foreign = stranger.call(tool, params);

    const auto flip = [&](std::size_t at) {
        std::string changed = sealed;
        changed[at] = changed[at] == 'A' ? 'B' : 'A';
        return changed;
    };
    const auto low_bit = [&](std::size_t at) {
        std::string changed = sealed;
        changed[at] = static_cast<char>(changed[at] ^ 1);
        return changed;
    };
    struct Case {
        std::string name;
        json state;
        json arguments;
        RequestStateProblem problem;
    };
    const std::vector<Case> cases{
        {"payload first byte", flip(0), args, RequestStateProblem::signature},
        {"payload middle byte", flip(dot / 2), args, RequestStateProblem::signature},
        {"payload last byte", flip(dot - 1), args, RequestStateProblem::signature},
        {"the separator", sealed.substr(0, dot) + "," + sealed.substr(dot + 1), args, RequestStateProblem::malformed},
        {"code first byte", flip(dot + 1), args, RequestStateProblem::signature},
        {"code last byte, low bit", low_bit(sealed.size() - 1), args, RequestStateProblem::malformed},
        {"code last byte", flip(sealed.size() - 2), args, RequestStateProblem::signature},
        {"suffix", sealed + "-TAMPERED", args, RequestStateProblem::malformed},
        {"truncated by one", sealed.substr(0, sealed.size() - 1), args, RequestStateProblem::malformed},
        {"truncated at the separator", sealed.substr(0, dot + 1), args, RequestStateProblem::malformed},
        {"payload only", sealed.substr(0, dot), args, RequestStateProblem::malformed},
        {"empty", std::string(), args, RequestStateProblem::malformed},
        {"not a string", json(42), args, RequestStateProblem::malformed},
        {"another key", result_of(foreign).at("requestState"), args, RequestStateProblem::signature},
        {"another tool", result_of(other_tool).at("requestState"), args, RequestStateProblem::binding},
        {"other arguments", sealed, J(R"({"x":2})"), RequestStateProblem::binding},
    };
    std::optional<json> first_error;
    for (const auto& c : cases) {
        json retry = json::object();
        retry["arguments"] = c.arguments;
        retry["inputResponses"] = answers;
        retry["requestState"] = c.state;
        const json answer = client.call(tool, retry);
        ASSERT_TRUE(answer.contains("error")) << c.name << " " << answer.dump();
        if (!first_error) first_error = answer.at("error");
        EXPECT_EQ(answer.at("error"), *first_error) << c.name;
        const auto problems = rejections.taken();
        ASSERT_EQ(problems.size(), 1u) << c.name;
        EXPECT_EQ(problems.front(), c.problem) << c.name << ": " << neomcp::to_string(problems.front());
    }
    // Expired: the same state, one TTL later.
    clock_now->fetch_add(600);
    json retry = json::object();
    retry["arguments"] = args;
    retry["inputResponses"] = answers;
    retry["requestState"] = sealed;
    const json expired = client.call(tool, retry);
    ASSERT_TRUE(expired.contains("error"));
    EXPECT_EQ(expired.at("error"), *first_error);
    EXPECT_EQ(rejections.taken(), std::vector<RequestStateProblem>{RequestStateProblem::expired});
    // One second earlier it was still good.
    clock_now->fetch_sub(1);
    EXPECT_TRUE(completes(client.call(tool, retry)));
    EXPECT_TRUE(rejections.taken().empty());
    EXPECT_EQ(client.host.entries(tool), 2) << "no refused state reached the host";
}

TEST(Sealing, EveryKeyOfTheRingVerifiesAndTheFirstSeals) {
    neomcp::SessionOptions old_options = input_options();
    old_options.request_state_keys = {std::string(32, 'o')};
    Modern old_server(old_options);
    const json issued = old_server.call("test_input_required_result_request_state");
    neomcp::SessionOptions ring = input_options();
    ring.request_state_keys = {std::string(40, 'n'), std::string(32, 'o')};
    Modern new_server(ring);
    const json answers = one("confirm", accept(J(R"({"name":"yes"})")));
    EXPECT_TRUE(completes(new_server.retry("test_input_required_result_request_state", issued, answers)));
    const json renewed = new_server.call("test_input_required_result_request_state");
    EXPECT_TRUE(old_server.retry("test_input_required_result_request_state", renewed, answers).contains("error"))
        << "the new key seals";
}

TEST(Sealing, TheSealerOpensWhatItSealedAndNothingElse) {
    neomcp::detail::StateSealer sealer({std::string(32, 'k')});
    neomcp::detail::StateClaims claims;
    claims.expires = 100;
    claims.method = "tools/call";
    claims.subject = "t";
    claims.arguments = neomcp::detail::arguments_digest(json::object());
    claims.server = "s";
    claims.round = 3;
    claims.asked = {{"a", "roots/list"}};
    claims.state = "mine";
    const std::string sealed = sealer.seal(claims);
    auto opened = sealer.open(sealed);
    ASSERT_TRUE(std::holds_alternative<neomcp::detail::StateClaims>(opened));
    const auto& back = std::get<neomcp::detail::StateClaims>(opened);
    EXPECT_EQ(back.expires, 100);
    EXPECT_EQ(back.round, 3u);
    EXPECT_EQ(back.asked, claims.asked);
    EXPECT_EQ(back.state, claims.state);
    const neomcp::detail::StateBinding binding{"tools/call", "t", claims.arguments, "s", ""};
    EXPECT_FALSE(neomcp::detail::check_claims(back, binding, std::chrono::system_clock::time_point(std::chrono::seconds(99))));
    EXPECT_EQ(neomcp::detail::check_claims(back, binding, std::chrono::system_clock::time_point(std::chrono::seconds(100))),
              neomcp::RequestStateProblem::expired);
    const neomcp::detail::StateBinding other_server{"tools/call", "t", claims.arguments, "other", ""};
    EXPECT_EQ(neomcp::detail::check_claims(back, other_server, std::chrono::system_clock::time_point(std::chrono::seconds(1))),
              neomcp::RequestStateProblem::binding);
    // Signed, not encrypted: the payload is readable.
    const auto payload = neomcp::detail::decode_base64url(sealed.substr(0, sealed.find('.')));
    ASSERT_TRUE(payload);
    EXPECT_NE(payload->find("mine"), std::string::npos);
    EXPECT_THROW(neomcp::detail::StateSealer({std::string(31, 'k')}), std::invalid_argument);
}

TEST(Sealing, CanonicalArgumentsIgnoreMemberOrderAndIntegerSpelling) {
    using neomcp::detail::canonical_json;
    EXPECT_EQ(canonical_json(J(R"({"b":1,"a":[3,{"d":true,"c":null}]})")), R"({"a":[3,{"c":null,"d":true}],"b":1})");
    EXPECT_EQ(canonical_json(J("1.0")), "1");
    EXPECT_EQ(canonical_json(J("-0.0")), "0");
    EXPECT_NE(canonical_json(J("1.5")), canonical_json(J("1")));
    EXPECT_EQ(neomcp::detail::arguments_digest(J(R"({"x":1,"y":2})")), neomcp::detail::arguments_digest(J(R"({"y":2,"x":1.0})")));
    EXPECT_NE(neomcp::detail::arguments_digest(J(R"({"x":1})")), neomcp::detail::arguments_digest(J(R"({"x":"1"})")));
}

TEST(Sealing, Base64UrlIsStrict) {
    using neomcp::detail::decode_base64url;
    using neomcp::detail::encode_base64url;
    // RFC 4648, section 10, without padding, and the two characters of the URL-safe alphabet.
    EXPECT_EQ(encode_base64url(""), "");
    EXPECT_EQ(encode_base64url("f"), "Zg");
    EXPECT_EQ(encode_base64url("fo"), "Zm8");
    EXPECT_EQ(encode_base64url("foo"), "Zm9v");
    EXPECT_EQ(encode_base64url("foob"), "Zm9vYg");
    EXPECT_EQ(encode_base64url("fooba"), "Zm9vYmE");
    EXPECT_EQ(encode_base64url("foobar"), "Zm9vYmFy");
    EXPECT_EQ(encode_base64url("\xfb\xff"), "-_8");
    std::string all;
    for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
    EXPECT_EQ(decode_base64url(encode_base64url(all)), all);
    EXPECT_FALSE(decode_base64url("Zg=="));  // padding
    EXPECT_FALSE(decode_base64url("Zh"));    // bits beyond the data
    EXPECT_FALSE(decode_base64url("Z"));     // a length of 4k + 1
    EXPECT_FALSE(decode_base64url("+/8"));   // the standard alphabet
    EXPECT_FALSE(decode_base64url("Zm 9v"));
}

// ---------------------------------------------------------------------------------------------------------------
// server_request: the same host, answered through client responses

struct Legacy {
    explicit Legacy(neomcp::SessionOptions options = input_options(), json capabilities = all_capabilities())
        : peer(host, std::move(options)), profile(builtin(legacy_revision)) {
        const auto opened = peer.request(Peer::initialize_request(*profile, neomcp::RequestId("init"), capabilities), patience);
        EXPECT_TRUE(opened && opened->contains("result"));
        peer.notify(J(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }

    json call_message(const std::string& tool, json params = json::object()) {
        params["name"] = tool;
        json message = json::object();
        message["jsonrpc"] = "2.0";
        message["id"] = next_id++;
        message["method"] = "tools/call";
        message["params"] = std::move(params);
        return message;
    }
    // Answers the requests the session sends with `answer` until the call's response arrives.
    json serve(const json& message, const std::function<json(const json& request)>& answer) {
        const auto id = *neomcp::RequestId::from_json(message.at("id"));
        peer.send(neomcp::serialize(message));
        for (int rounds = 0; rounds < 64; ++rounds) {
            if (auto done = peer.await_response(id, 0ms)) return *done;
            const auto request = peer.next_request(patience);
            if (!request) break;
            json response = json::object();
            response["jsonrpc"] = "2.0";
            response["id"] = request->at("id");
            response["result"] = answer(*request);
            peer.send(neomcp::serialize(response));
        }
        if (auto done = peer.await_response(id, patience)) return *done;
        throw std::runtime_error("no answer");
    }
    json flush() { return *peer.request(J(R"({"jsonrpc":"2.0","id":"flush","method":"ping"})"), patience); }

    ScriptedHost host;
    Peer peer;
    profile::ProfilePtr profile;
    int next_id = 1;
};

// What a client answers each kind of request with.
json answer_any(const json& request) {
    const std::string method = request.at("method");
    if (method == "elicitation/create") return accept(J(R"({"name":"Alice"})"));
    if (method == "sampling/createMessage") return sampled();
    return roots_answer();
}

TEST(ServerRequest, TheSameHostIsServedThroughRequests) {
    for (const char* tool : {"test_input_required_result_elicitation", "test_input_required_result_sampling",
                             "test_input_required_result_list_roots", "test_input_required_result_request_state",
                             "test_input_required_result_multiple_inputs", "test_input_required_result_multi_round"}) {
        Legacy client;
        const json answer = client.serve(client.call_message(tool), answer_any);
        ASSERT_TRUE(answer.contains("result")) << tool << " " << answer.dump();
        EXPECT_FALSE(answer.at("result").value("isError", false)) << tool << " " << answer.dump();
        EXPECT_FALSE(answer.at("result").contains("resultType")) << "a legacy result has no resultType";
    }
}

TEST(ServerRequest, RequestsAreTheRevisionsMessagesWithIdsOfTheSessionsOwn) {
    Legacy client;
    client.peer.send(neomcp::serialize(client.call_message("test_input_required_result_multiple_inputs")));
    std::vector<json> requests;
    for (int i = 0; i < 3; ++i) {
        const auto request = client.peer.next_request(patience);
        ASSERT_TRUE(request);
        requests.push_back(*request);
    }
    std::set<json> ids;
    std::set<std::string> methods;
    for (const auto& request : requests) {
        EXPECT_EQ(request.at("jsonrpc"), "2.0");
        ids.insert(request.at("id"));
        methods.insert(request.at("method").get<std::string>());
        if (request.at("method") == "elicitation/create") {
            EXPECT_EQ(request.at("params").at("mode"), "form");  // this revision's form params have the member
            EXPECT_EQ(request.at("params").at("requestedSchema"), name_schema());
        }
        if (request.at("method") == "roots/list") {
            EXPECT_FALSE(request.contains("params"));
        }
    }
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(methods, (std::set<std::string>{"elicitation/create", "sampling/createMessage", "roots/list"}));
    // All three were written before anything else of the call: the round goes out together.
    const auto transcript = client.peer.transcript();
    ASSERT_GE(transcript.size(), 4u);
    for (std::size_t i = transcript.size() - 3; i < transcript.size(); ++i) EXPECT_TRUE(transcript[i].contains("method"));
}

TEST(ServerRequest, AnOlderRevisionWritesFormsWithoutAMode) {
    neomcp::SessionOptions options = input_options();
    options.profiles = {builtin(older_revision)};
    ScriptedHost host;
    Peer peer(host, options);
    const auto older = builtin(older_revision);
    ASSERT_TRUE(peer.request(Peer::initialize_request(*older, neomcp::RequestId("init"), J(R"({"elicitation":{}})")), patience));
    peer.send(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo_answer"}})");
    const auto request = peer.next_request(patience);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->at("method"), "elicitation/create");
    EXPECT_FALSE(request->at("params").contains("mode"));
    EXPECT_EQ(request->at("params").at("requestedSchema"), name_schema());
    // URL mode is not a mode of this revision.
    const auto url = peer.request(J(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"asks_url"}})"), patience);
    ASSERT_TRUE(url);
    EXPECT_TRUE(url->at("result").at("isError").get<bool>()) << url->dump();
}

TEST(ServerRequest, AUrlElicitationGetsAnIdWhereTheRevisionHasOne) {
    Legacy client;
    client.peer.send(neomcp::serialize(client.call_message("asks_url")));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->at("params").at("mode"), "url");
    ASSERT_TRUE(request->at("params").at("elicitationId").is_string());
    EXPECT_EQ(request->at("params").at("elicitationId").get<std::string>().size(), 32u);
}

TEST(ServerRequest, TheRoundLimitIsExact) {
    neomcp::SessionOptions options = input_options();
    options.max_input_rounds = 3;
    Legacy client(options);
    int asked = 0;
    const json answer = client.serve(client.call_message("always_asks"), [&](const json& request) {
        ++asked;
        return answer_any(request);
    });
    EXPECT_EQ(asked, 3) << "three rounds of requests went out";
    EXPECT_EQ(client.host.entries("always_asks"), 4) << "the host asked a fourth time and the call failed";
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_TRUE(answer.at("result").at("isError").get<bool>());
    EXPECT_NE(answer.at("result").at("content").at(0).at("text").get<std::string>().find("more than 3"), std::string::npos);
}

TEST(ServerRequest, AnErrorResponseOrAWrongShapeEndsTheCall) {
    {
        Legacy client;
        client.peer.send(neomcp::serialize(client.call_message("echo_answer")));
        const auto request = client.peer.next_request(patience);
        ASSERT_TRUE(request);
        json response = J(R"({"jsonrpc":"2.0","error":{"code":-1,"message":"User rejected"}})");
        response["id"] = request->at("id");
        client.peer.send(neomcp::serialize(response));
        const auto answer = client.peer.await_response(neomcp::RequestId(1), patience);
        ASSERT_TRUE(answer);
        EXPECT_TRUE(answer->at("result").at("isError").get<bool>()) << answer->dump();
        EXPECT_EQ(client.host.entries("echo_answer"), 1);
    }
    {
        Legacy client;
        const json answer = client.serve(client.call_message("echo_answer"), [](const json&) { return sampled(); });
        EXPECT_TRUE(answer.at("result").at("isError").get<bool>()) << answer.dump();
    }
    {
        // A prompt has no error result: its failure is an internal error.
        Legacy client;
        json message = J(R"({"jsonrpc":"2.0","id":"p","method":"prompts/get","params":{"name":"test_input_required_result_prompt"}})");
        const json answer = client.serve(message, [](const json&) { return J(R"({"action":"nope"})"); });
        ASSERT_TRUE(answer.contains("error")) << answer.dump();
        EXPECT_EQ(answer.at("error").at("code"), neomcp::error_code::internal_error);
    }
}

TEST(ServerRequest, AResponseWithAnUnknownIdIsIgnored) {
    Legacy client;
    client.peer.send(neomcp::serialize(client.call_message("echo_answer")));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request);
    json stray = J(R"({"jsonrpc":"2.0","result":{"action":"accept","content":{"name":"Mallory"}}})");
    stray["id"] = "not-ours";
    client.peer.send(neomcp::serialize(stray));
    EXPECT_TRUE(client.flush().contains("result"));
    EXPECT_EQ(client.host.entries("echo_answer"), 1);
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = request->at("id");
    response["result"] = accept(J(R"({"name":"Alice"})"));
    client.peer.send(neomcp::serialize(response));
    const auto answer = client.peer.await_response(neomcp::RequestId(1), patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->at("result").at("content").at(0).at("text"), "Hello, Alice");
    // The same response again matches nothing any more.
    client.peer.send(neomcp::serialize(response));
    EXPECT_TRUE(client.flush().contains("result"));
    EXPECT_EQ(client.host.entries("echo_answer"), 2);
}

TEST(ServerRequest, AnswersInParamsAreNotReadUnderThisMechanism) {
    Legacy client;
    json params = json::object();
    params["inputResponses"] = one("user_name", accept(J(R"({"name":"Injected"})")));
    params["requestState"] = "anything";
    client.peer.send(neomcp::serialize(client.call_message("echo_answer", params)));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request) << "the host was entered without answers and asked";
    const auto entry = client.host.last("echo_answer");
    ASSERT_TRUE(entry);
    EXPECT_TRUE(entry->answers.empty());
    EXPECT_FALSE(entry->state);
}

json response_to(const json& request, json result) {
    json response = json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = request.at("id");
    response["result"] = std::move(result);
    return response;
}

TEST(ServerRequest, CancellationWhileWaitingEndsTheWait) {
    Legacy client;
    client.peer.send(neomcp::serialize(client.call_message("echo_answer")));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request);
    const auto wait = client.host.wait_for_wait();
    ASSERT_TRUE(wait);
    EXPECT_FALSE(wait->ended().stop_requested());
    client.peer.notify(J(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})"));
    EXPECT_TRUE(wait->ended().stop_requested());
    client.peer.send(neomcp::serialize(response_to(*request, accept(J(R"({"name":"Late"})")))));
    EXPECT_TRUE(client.flush().contains("result"));
    EXPECT_EQ(client.host.entries("echo_answer"), 1) << "an answer after the cancellation reaches nobody";
    EXPECT_FALSE(client.peer.await_response(neomcp::RequestId(1), 0ms)) << "nothing is written for a cancelled call";
    wait->expire();  // stale: ignored
    EXPECT_TRUE(client.flush().contains("result"));
    EXPECT_FALSE(client.peer.await_response(neomcp::RequestId(1), 0ms));
}

TEST(ServerRequest, TheTimeOutEntryPoint) {
    Legacy client;
    client.peer.send(neomcp::serialize(client.call_message("echo_answer")));
    const auto request = client.peer.next_request(patience);
    ASSERT_TRUE(request);
    const auto wait = client.host.wait_for_wait();
    ASSERT_TRUE(wait);
    wait->expire();
    EXPECT_TRUE(wait->ended().stop_requested());
    const auto answer = client.peer.await_response(neomcp::RequestId(1), patience);
    ASSERT_TRUE(answer);
    EXPECT_TRUE(answer->at("result").at("isError").get<bool>()) << answer->dump();
    client.peer.send(neomcp::serialize(response_to(*request, accept(J(R"({"name":"Late"})")))));
    EXPECT_TRUE(client.flush().contains("result"));
    EXPECT_EQ(client.host.entries("echo_answer"), 1);
}

TEST(ServerRequest, TheWaitEndsWhenItsAnswersArrive) {
    Legacy client;
    client.peer.send(neomcp::serialize(client.call_message("echo_answer")));
    const auto request = client.peer.next_request(patience);
    const auto wait = client.host.wait_for_wait();
    ASSERT_TRUE(request && wait);
    client.peer.send(neomcp::serialize(response_to(*request, accept(J(R"({"name":"Alice"})")))));
    EXPECT_TRUE(wait->ended().stop_requested());
    ASSERT_TRUE(client.peer.await_response(neomcp::RequestId(1), patience));
    wait->expire();  // after the answer: nothing changes
    EXPECT_TRUE(client.flush().contains("result"));
}

TEST(ServerRequest, AbandonAndCloseWhileWaiting) {
    for (const bool close : {false, true}) {
        ScriptedHost host;
        Tagged sink;
        neomcp::SessionOptions options = input_options();
        options.transport = profile::Transport::streamable_http;
        options.profiles = {builtin(legacy_revision)};
        auto session = tagged(host, sink, options);
        const json init = Peer::initialize_request(*builtin(legacy_revision), neomcp::RequestId("init"), all_capabilities());
        std::vector<neomcp::HeaderField> none;
        session->receive(1, neomcp::serialize(init), none);
        ASSERT_TRUE(sink.wait_kind(neomcp::OutputKind::response));
        session->receive(2, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", none);
        session->receive(3, R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo_answer"}})", none);
        const auto request = sink.wait_kind(neomcp::OutputKind::request);
        ASSERT_TRUE(request);
        EXPECT_EQ(request->unit, std::optional<neomcp::UnitId>(3));
        EXPECT_FALSE(request->last);
        const auto wait = host.wait_for_wait();
        ASSERT_TRUE(wait);
        if (close) {
            session->close();
        } else {
            session->abandon(3);
        }
        EXPECT_TRUE(wait->ended().stop_requested()) << close;
        const std::size_t before = sink.all().size();
        session->receive(4, neomcp::serialize(response_to(json::parse(request->line), accept(J(R"({"name":"Late"})")))), none);
        EXPECT_EQ(host.entries("echo_answer"), 1) << close;
        for (std::size_t i = before; i < sink.all().size(); ++i) {
            EXPECT_NE(sink.all()[i].unit, std::optional<neomcp::UnitId>(3)) << "nothing more for the unit";
        }
    }
}

TEST(ServerRequest, AMissingCapabilityIsAToolErrorWhereTheRevisionHasNoCode) {
    Legacy client(input_options(), J(R"({"roots":{}})"));
    const auto answer = client.peer.request(client.call_message("echo_answer"), patience);
    ASSERT_TRUE(answer);
    ASSERT_TRUE(answer->contains("result")) << answer->dump();
    EXPECT_TRUE(answer->at("result").at("isError").get<bool>());
    EXPECT_FALSE(client.peer.next_request(0ms)) << "nothing was sent";
    // The host can ask beforehand and do without.
    const auto other = client.peer.request(client.call_message("test_input_required_result_capabilities"), patience);
    ASSERT_TRUE(other);
    EXPECT_EQ(other->at("result").at("content").at(0).at("text"), "nothing to ask with");
}

TEST(ServerRequest, TwoWaitingCallsAnsweredInTheOtherOrder) {
    Legacy client;
    client.peer.send(neomcp::serialize(client.call_message("echo_answer")));  // id 1
    const auto first = client.peer.next_request(patience);
    client.peer.send(neomcp::serialize(client.call_message("echo_answer")));  // id 2
    const auto second = client.peer.next_request(patience);
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->at("id"), second->at("id"));
    client.peer.send(neomcp::serialize(response_to(*second, accept(J(R"({"name":"Second"})")))));
    const auto answer_two = client.peer.await_response(neomcp::RequestId(2), patience);
    ASSERT_TRUE(answer_two);
    EXPECT_EQ(answer_two->at("result").at("content").at(0).at("text"), "Hello, Second");
    EXPECT_FALSE(client.peer.await_response(neomcp::RequestId(1), 0ms));
    client.peer.send(neomcp::serialize(response_to(*first, accept(J(R"({"name":"First"})")))));
    const auto answer_one = client.peer.await_response(neomcp::RequestId(1), patience);
    ASSERT_TRUE(answer_one);
    EXPECT_EQ(answer_one->at("result").at("content").at(0).at("text"), "Hello, First");
}

// The answer and the time-out race; exactly one of them wins, and the host is entered again only when the answer won.
TEST(ServerRequest, AnAnswerRacingTheTimeOut) {
    int answered = 0;
    int expired = 0;
    for (int round = 0; round < 200; ++round) {
        Legacy client;
        client.peer.send(neomcp::serialize(client.call_message("echo_answer")));
        const auto request = client.peer.next_request(patience);
        const auto wait = client.host.wait_for_wait();
        ASSERT_TRUE(request && wait);
        const std::string line = neomcp::serialize(response_to(*request, accept(J(R"({"name":"Alice"})"))));
        std::atomic<bool> go{false};
        std::thread timer([&] {
            while (!go.load()) std::this_thread::yield();
            wait->expire();
        });
        go.store(true);
        client.peer.send(line);
        timer.join();
        const auto answer = client.peer.await_response(neomcp::RequestId(1), patience);
        ASSERT_TRUE(answer);
        const bool failed = answer->at("result").value("isError", false);
        const int entries = client.host.entries("echo_answer");
        if (failed) {
            ++expired;
            EXPECT_EQ(entries, 1) << "the host is not entered again after the time-out won";
        } else {
            ++answered;
            EXPECT_EQ(entries, 2);
        }
        EXPECT_FALSE(client.peer.await_response(neomcp::RequestId(1), 0ms)) << "one answer only";
    }
    EXPECT_EQ(answered + expired, 200);
}

// An answer racing the cancellation of its call from another thread, through the transport's abandon().
TEST(ServerRequest, AnAnswerRacingAnAbandon) {
    for (int round = 0; round < 200; ++round) {
        ScriptedHost host;
        Tagged sink;
        neomcp::SessionOptions options = input_options();
        options.transport = profile::Transport::streamable_http;
        options.profiles = {builtin(legacy_revision)};
        auto session = tagged(host, sink, options);
        std::vector<neomcp::HeaderField> none;
        session->receive(1, neomcp::serialize(Peer::initialize_request(*builtin(legacy_revision), neomcp::RequestId("i"),
                                                                       all_capabilities())),
                         none);
        session->receive(3, R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo_answer"}})", none);
        const auto request = sink.wait_kind(neomcp::OutputKind::request);
        ASSERT_TRUE(request);
        const std::string line = neomcp::serialize(response_to(json::parse(request->line), accept(J(R"({"name":"A"})"))));
        std::atomic<bool> go{false};
        std::thread transport([&] {
            while (!go.load()) std::this_thread::yield();
            session->abandon(3);
        });
        go.store(true);
        session->receive(4, line, none);
        transport.join();
        // Whoever won, the unit got at most one answer and nothing after its end.
        int responses = 0;
        bool ended = false;
        for (const auto& output : sink.all()) {
            if (output.unit != std::optional<neomcp::UnitId>(3)) continue;
            EXPECT_FALSE(ended) << "an output after the unit's last one";
            if (output.kind == neomcp::OutputKind::response) ++responses;
            if (output.last) ended = true;
        }
        EXPECT_LE(responses, 1);
        EXPECT_LE(host.entries("echo_answer"), 2);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Creation

TEST(InputCreation, BadOptionsAreRefused) {
    ScriptedHost host;
    for (int which = 0; which < 3; ++which) {
        neomcp::SessionOptions options = input_options();
        if (which == 0) options.max_input_rounds = 0;
        if (which == 1) options.request_state_ttl = std::chrono::seconds(0);
        if (which == 2) options.request_state_keys = {std::string(31, 'x')};
        auto made = neomcp::Session::create(options, host, [](std::string) {});
        EXPECT_TRUE(std::holds_alternative<neomcp::CreateError>(made)) << which;
    }
    neomcp::SessionOptions defaults;
    defaults.server_info.name = "s";
    auto made = neomcp::Session::create(defaults, host, [](std::string) {});
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made)) << "a key of the process by default";
}

TEST(InputCreation, TheRegistryHasBothMechanisms) {
    const auto& registry = neomcp::builtin_mechanisms();
    for (const auto name : profile::declared_mechanisms(profile::Slot::server_input)) {
        EXPECT_TRUE(registry.contains(neomcp::MechanismSlot::server_input, name)) << name;
        EXPECT_TRUE(registry.make_input(name)) << name;
    }
    EXPECT_EQ(registry.make_input("input_required_result")->delivery(), neomcp::InputDelivery::result);
    EXPECT_EQ(registry.make_input("server_request")->delivery(), neomcp::InputDelivery::requests);
    neomcp::MechanismRegistry without = registry;
    ASSERT_TRUE(without.remove(neomcp::MechanismSlot::server_input, "server_request"));
    ScriptedHost host;
    neomcp::SessionOptions options = input_options();
    options.mechanisms = &without;
    auto made = neomcp::Session::create(options, host, [](std::string) {});
    ASSERT_TRUE(std::holds_alternative<neomcp::CreateError>(made));
    EXPECT_FALSE(std::get<neomcp::CreateError>(made).missing.empty());
    EXPECT_EQ(std::get<neomcp::CreateError>(made).missing.front().slot, neomcp::MechanismSlot::server_input);
}

}  // namespace
