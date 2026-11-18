// The requests that the official conformance runner's scenarios send to the conformance fixture's registrations
// (everything.h), sent over real sockets on the loopback interface in both eras, with the assertions that the
// scenarios make about what comes back. The runner needs a package manager and the network; this test needs neither
// and runs with the rest of the suite.
//
// The scenarios covered are those of resources (list, read of text, of binary and of a template, a URI nothing
// serves), prompts (list and the four gets), completion, and the caching hints of the revision that has them. One
// connection per test; the HTTP library's client is used here, in the tests only.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "everything.h"
#include "neomcp/http_server.h"
#include "neomcp/server.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;

constexpr const char* modern_revision = "2026-07-28";
constexpr const char* legacy_revision = "2025-11-25";
constexpr const char* path = "/mcp";
constexpr const char* json_type = "application/json";
constexpr const char* both_types = "application/json, text/event-stream";

// The code a read of a URI nothing serves is answered with: Invalid params in the revision that has moved the code
// there, Resource not found in the ones before it.
int not_found_code(const std::string& revision) { return revision == modern_revision ? -32602 : -32002; }

// The fixture's server, listening on a free port of the loopback interface.
struct Fixture {
    Fixture() : server(conformance::server_options()) {
        conformance::register_everything(server);
        neomcp::HttpServerOptions options;
        options.endpoint.stream_after = 1h;  // a silent request is answered in JSON
        options.keep_alive_interval = 1h;
        options.worker_threads = 4;
        options.max_worker_threads = 32;
        http = std::make_unique<neomcp::HttpServer>(server, std::move(options));
        const auto started = http->start();
        if (!started) throw std::runtime_error("the fixture did not start: " + started.error);
        port = *started.value;
    }

    neomcp::Server server;
    std::unique_ptr<neomcp::HttpServer> http;
    std::uint16_t port = 0;
};

// A client of one era: the legacy one opens a session first, as the runner's SDK client does; the modern one names
// its revision in every request, as the runner's stateless connection does.
class Client {
public:
    Client(std::uint16_t port, std::string revision) : revision_(std::move(revision)), http_("127.0.0.1", port) {
        http_.set_keep_alive(false);
        http_.set_connection_timeout(30, 0);
        http_.set_read_timeout(60, 0);
        http_.set_write_timeout(60, 0);
        if (revision_ == modern_revision) {
            opening_ = call("server/discover");
        } else {
            httplib::Headers headers{{"Accept", both_types}};
            const auto answer = http_.Post(path, headers, rpc(next_id_++, "initialize", initialize_params()), json_type);
            if (!answer || answer->status != 200) throw std::runtime_error("initialize was not answered with 200");
            session_ = answer->get_header_value("Mcp-Session-Id");
            if (session_.empty()) throw std::runtime_error("initialize gave no session id");
            opening_ = json::parse(answer->body);
            const auto ready = http_.Post(path, legacy_headers(), notification("notifications/initialized"), json_type);
            if (!ready || ready->status != 202) throw std::runtime_error("notifications/initialized was not answered with 202");
        }
    }

    const std::string& revision() const { return revision_; }
    bool modern() const { return revision_ == modern_revision; }

    // The answer to the handshake (initialize) or to the discovery (server/discover).
    const json& opening() const { return opening_; }

    // Sends a request and returns the whole JSON-RPC answer, a result or an error.
    json call(const std::string& method, json params = json::object()) {
        httplib::Headers headers = modern() ? modern_headers(method, params) : legacy_headers();
        if (modern()) params["_meta"] = modern_meta();
        const auto answer = http_.Post(path, headers, rpc(next_id_++, method, std::move(params)), json_type);
        if (!answer) throw std::runtime_error(method + " was not answered");
        last_status_ = answer->status;
        const std::string type = answer->get_header_value("Content-Type");
        if (type.rfind(json_type, 0) != 0) throw std::runtime_error(method + " was answered with " + type);
        return json::parse(answer->body);
    }

    int last_status() const { return last_status_; }

    // The result of a request that must have one.
    json result(const std::string& method, json params = json::object()) {
        json answer = call(method, std::move(params));
        if (!answer.contains("result")) throw std::runtime_error(method + " was answered with " + answer.dump());
        return answer.at("result");
    }

private:
    static std::string rpc(int id, const std::string& method, json params) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
    }
    static std::string notification(const std::string& method) {
        return json{{"jsonrpc", "2.0"}, {"method", method}}.dump();
    }
    json initialize_params() const {
        return json{{"protocolVersion", revision_},
                    {"capabilities", client_capabilities()},
                    {"clientInfo", {{"name", "conformance-test-client"}, {"version", "1.0.0"}}}};
    }
    static json client_capabilities() {
        return json{{"sampling", json::object()}, {"elicitation", json::object()}, {"roots", {{"listChanged", true}}}};
    }
    json modern_meta() const {
        return json{{"io.modelcontextprotocol/protocolVersion", revision_},
                    {"io.modelcontextprotocol/clientInfo", {{"name", "conformance-test-client"}, {"version", "1.0.0"}}},
                    {"io.modelcontextprotocol/clientCapabilities", client_capabilities()}};
    }
    httplib::Headers legacy_headers() const {
        return {{"Accept", both_types}, {"MCP-Protocol-Version", revision_}, {"Mcp-Session-Id", session_}};
    }
    // The headers that repeat the method, and the name or the URI the request is about; the media type of the body is
    // the one the client library is given with it.
    httplib::Headers modern_headers(const std::string& method, const json& params) const {
        httplib::Headers headers{{"Accept", both_types}, {"MCP-Protocol-Version", revision_}, {"Mcp-Method", method}};
        if (method == "tools/call" || method == "prompts/get") headers.emplace("Mcp-Name", params.at("name").get<std::string>());
        if (method == "resources/read") headers.emplace("Mcp-Name", params.at("uri").get<std::string>());
        return headers;
    }

    std::string revision_;
    httplib::Client http_;
    std::string session_;
    json opening_;
    int next_id_ = 1;
    int last_status_ = 0;
};

// The entry of a list whose member `key` is `value`, or null.
json entry_of(const json& list, const std::string& key, const std::string& value) {
    for (const json& item : list) {
        if (item.contains(key) && item.at(key) == value) return item;
    }
    return json();
}

class FixtureScenarios : public ::testing::TestWithParam<std::string> {
protected:
    FixtureScenarios() : client(fixture.port, GetParam()) {}

    Fixture fixture;
    Client client;
};

INSTANTIATE_TEST_SUITE_P(Revisions, FixtureScenarios, ::testing::Values(std::string(legacy_revision), std::string(modern_revision)),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string name = info.param;
                             for (char& c : name) {
                                 if (c == '-') c = '_';
                             }
                             return name;
                         });

// ---- Capabilities ----------------------------------------------------------------------------------------------------

TEST_P(FixtureScenarios, TheServerDeclaresEverythingItOffers) {
    // The handshake's answer names the capabilities in `capabilities`, and so does the discovery's.
    const json& capabilities = client.opening().at("result").at("capabilities");
    for (const char* member : {"tools", "resources", "prompts", "completions"}) {
        EXPECT_TRUE(capabilities.contains(member)) << member << " in " << capabilities.dump();
    }
}

// ---- Tools -----------------------------------------------------------------------------------------------------------

TEST_P(FixtureScenarios, EveryToolHasANameADescriptionAndASchema) {
    const json tools = client.result("tools/list").at("tools");
    ASSERT_TRUE(tools.is_array());
    EXPECT_GE(tools.size(), 9U);
    for (const json& tool : tools) {
        EXPECT_FALSE(tool.at("name").get<std::string>().empty());
        EXPECT_FALSE(tool.at("description").get<std::string>().empty()) << tool.dump();
        EXPECT_TRUE(tool.contains("inputSchema"));
    }
}

// ---- Resources -------------------------------------------------------------------------------------------------------

TEST_P(FixtureScenarios, ResourcesListHasEachResourceWithAUriANameAndADescription) {
    const json resources = client.result("resources/list").at("resources");
    ASSERT_TRUE(resources.is_array());
    std::set<std::string> uris;
    for (const json& resource : resources) {
        EXPECT_FALSE(resource.at("uri").get<std::string>().empty());
        EXPECT_FALSE(resource.at("name").get<std::string>().empty());
        EXPECT_FALSE(resource.at("description").get<std::string>().empty()) << resource.dump();
        uris.insert(resource.at("uri").get<std::string>());
    }
    EXPECT_EQ(uris, (std::set<std::string>{"test://static-binary", "test://static-text"})) << "direct resources only";
}

TEST_P(FixtureScenarios, TheStaticTextResourceIsRead) {
    const json contents = client.result("resources/read", json{{"uri", "test://static-text"}}).at("contents");
    ASSERT_TRUE(contents.is_array());
    ASSERT_GE(contents.size(), 1U);
    const json& first = contents.at(0);
    EXPECT_EQ(first.at("uri"), "test://static-text");
    EXPECT_EQ(first.at("mimeType"), "text/plain");
    EXPECT_EQ(first.at("text"), "This is the content of the static text resource.");
    EXPECT_FALSE(first.contains("blob"));
}

TEST_P(FixtureScenarios, TheStaticBinaryResourceIsRead) {
    const json contents = client.result("resources/read", json{{"uri", "test://static-binary"}}).at("contents");
    ASSERT_TRUE(contents.is_array());
    ASSERT_GE(contents.size(), 1U);
    const json& first = contents.at(0);
    EXPECT_EQ(first.at("uri"), "test://static-binary");
    EXPECT_EQ(first.at("mimeType"), "image/png");
    EXPECT_EQ(first.at("blob"), conformance::pixel_png);
    EXPECT_FALSE(first.contains("text"));
}

TEST_P(FixtureScenarios, TheTemplateIsListedAndAResourceOfItIsRead) {
    const json templates = client.result("resources/templates/list").at("resourceTemplates");
    ASSERT_TRUE(templates.is_array());
    ASSERT_EQ(templates.size(), 1U);
    EXPECT_EQ(templates.at(0).at("uriTemplate"), "test://template/{id}/data");
    EXPECT_FALSE(templates.at(0).at("name").get<std::string>().empty());
    EXPECT_FALSE(templates.at(0).at("description").get<std::string>().empty());

    const json contents = client.result("resources/read", json{{"uri", "test://template/123/data"}}).at("contents");
    ASSERT_TRUE(contents.is_array());
    ASSERT_GE(contents.size(), 1U);
    const json& first = contents.at(0);
    EXPECT_EQ(first.at("uri"), "test://template/123/data");
    EXPECT_EQ(first.at("mimeType"), "application/json");
    const std::string text = first.at("text").get<std::string>();
    EXPECT_NE(text.find("123"), std::string::npos) << "the value of the variable is in the content";
    const json data = json::parse(text);
    EXPECT_EQ(data.at("id"), "123");
    EXPECT_EQ(data.at("templateTest"), true);
    EXPECT_EQ(data.at("data"), "Data for ID: 123");
}

TEST_P(FixtureScenarios, AUriNothingServesIsAnErrorThatNamesTheUri) {
    const std::string uri = "test://nonexistent-resource-for-conformance-testing";
    const json answer = client.call("resources/read", json{{"uri", uri}});
    EXPECT_FALSE(answer.contains("result")) << "no empty contents for a resource that does not exist";
    ASSERT_TRUE(answer.contains("error")) << answer.dump();
    EXPECT_EQ(answer.at("error").at("code"), not_found_code(GetParam()));
    EXPECT_EQ(answer.at("error").at("data").at("uri"), uri);
}

// ---- Prompts ---------------------------------------------------------------------------------------------------------

TEST_P(FixtureScenarios, PromptsListHasEachPromptWithANameAndADescription) {
    const json prompts = client.result("prompts/list").at("prompts");
    ASSERT_TRUE(prompts.is_array());
    std::set<std::string> names;
    for (const json& prompt : prompts) {
        EXPECT_FALSE(prompt.at("name").get<std::string>().empty());
        EXPECT_FALSE(prompt.at("description").get<std::string>().empty()) << prompt.dump();
        names.insert(prompt.at("name").get<std::string>());
    }
    EXPECT_EQ(names, (std::set<std::string>{"test_simple_prompt", "test_prompt_with_arguments",
                                            "test_prompt_with_embedded_resource", "test_prompt_with_image"}));

    const json with_arguments = entry_of(prompts, "name", "test_prompt_with_arguments");
    ASSERT_TRUE(with_arguments.contains("arguments")) << with_arguments.dump();
    std::set<std::string> argument_names;
    for (const json& argument : with_arguments.at("arguments")) {
        argument_names.insert(argument.at("name").get<std::string>());
        EXPECT_EQ(argument.at("required"), true);
    }
    EXPECT_EQ(argument_names, (std::set<std::string>{"arg1", "arg2"}));
    const json embedding = entry_of(prompts, "name", "test_prompt_with_embedded_resource");
    ASSERT_TRUE(embedding.contains("arguments")) << embedding.dump();
    EXPECT_EQ(embedding.at("arguments").at(0).at("name"), "resourceUri");
}

TEST_P(FixtureScenarios, TheSimplePromptHasAMessageWithARoleAndContent) {
    const json messages = client.result("prompts/get", json{{"name", "test_simple_prompt"}}).at("messages");
    ASSERT_TRUE(messages.is_array());
    ASSERT_GE(messages.size(), 1U);
    for (const json& message : messages) {
        EXPECT_TRUE(message.contains("role")) << message.dump();
        EXPECT_TRUE(message.contains("content")) << message.dump();
    }
    EXPECT_EQ(messages.at(0).at("content").at("type"), "text");
}

TEST_P(FixtureScenarios, ThePromptWithArgumentsCarriesBothValues) {
    const json params{{"name", "test_prompt_with_arguments"}, {"arguments", {{"arg1", "testValue1"}, {"arg2", "testValue2"}}}};
    const json messages = client.result("prompts/get", params).at("messages");
    ASSERT_TRUE(messages.is_array());
    ASSERT_GE(messages.size(), 1U);
    const std::string text = messages.dump();
    EXPECT_NE(text.find("testValue1"), std::string::npos) << text;
    EXPECT_NE(text.find("testValue2"), std::string::npos) << text;
}

TEST_P(FixtureScenarios, ThePromptWithAnEmbeddedResourceCarriesTheResourceOfTheArgument) {
    const json params{{"name", "test_prompt_with_embedded_resource"}, {"arguments", {{"resourceUri", "test://example-resource"}}}};
    const json messages = client.result("prompts/get", params).at("messages");
    ASSERT_TRUE(messages.is_array());
    ASSERT_GE(messages.size(), 1U);
    const json& content = messages.at(0).at("content");
    EXPECT_EQ(content.at("type"), "resource");
    EXPECT_EQ(content.at("resource").at("uri"), "test://example-resource");
    EXPECT_TRUE(content.at("resource").contains("text"));
}

TEST_P(FixtureScenarios, ThePromptWithAnImageCarriesAnImage) {
    const json messages = client.result("prompts/get", json{{"name", "test_prompt_with_image"}}).at("messages");
    ASSERT_TRUE(messages.is_array());
    ASSERT_GE(messages.size(), 1U);
    const json& content = messages.at(0).at("content");
    EXPECT_EQ(content.at("type"), "image");
    EXPECT_EQ(content.at("data"), conformance::pixel_png);
    EXPECT_EQ(content.at("mimeType"), "image/png");
}

TEST_P(FixtureScenarios, APromptMissingAnArgumentOrUnknownIsInvalidParams) {
    const json missing = client.call("prompts/get", json{{"name", "test_prompt_with_arguments"}, {"arguments", {{"arg1", "x"}}}});
    ASSERT_TRUE(missing.contains("error")) << missing.dump();
    EXPECT_EQ(missing.at("error").at("code"), -32602);
    const json unknown = client.call("prompts/get", json{{"name", "no_such_prompt"}});
    ASSERT_TRUE(unknown.contains("error")) << unknown.dump();
    EXPECT_EQ(unknown.at("error").at("code"), -32602);
}

// ---- Completion ------------------------------------------------------------------------------------------------------

TEST_P(FixtureScenarios, CompletionOfAPromptArgumentIsAListOfValues) {
    const json ref{{"type", "ref/prompt"}, {"name", "test_prompt_with_arguments"}};
    const json typed = client.result("completion/complete", json{{"ref", ref}, {"argument", {{"name", "arg1"}, {"value", "test"}}}});
    ASSERT_TRUE(typed.contains("completion")) << typed.dump();
    ASSERT_TRUE(typed.at("completion").at("values").is_array());
    EXPECT_EQ(typed.at("completion").at("values").size(), 3U);

    const json narrowed = client.result("completion/complete", json{{"ref", ref}, {"argument", {{"name", "arg1"}, {"value", "testi"}}}});
    ASSERT_EQ(narrowed.at("completion").at("values").size(), 1U);
    EXPECT_EQ(narrowed.at("completion").at("values").at(0), "testing");

    const json none = client.result("completion/complete", json{{"ref", ref}, {"argument", {{"name", "arg1"}, {"value", "zzz"}}}});
    ASSERT_TRUE(none.at("completion").at("values").is_array());
    EXPECT_EQ(none.at("completion").at("values").size(), 0U);

    // An argument without a completer is answered with no values, not with an error.
    const json other = client.result("completion/complete", json{{"ref", ref}, {"argument", {{"name", "arg2"}, {"value", "t"}}}});
    EXPECT_EQ(other.at("completion").at("values").size(), 0U);
}

// ---- Caching hints ---------------------------------------------------------------------------------------------------

// The revision that has the hints carries them, as the scenario checks, on each list and on a read of the first
// resource the list names; the revisions before it carry neither member.
TEST_P(FixtureScenarios, TheListsAndTheReadCarryTheCachingHintsOfTheRevisionThatHasThem) {
    const json resources = client.result("resources/list");
    ASSERT_TRUE(resources.at("resources").is_array());
    ASSERT_GE(resources.at("resources").size(), 1U);
    const std::string first_uri = resources.at("resources").at(0).at("uri").get<std::string>();

    const std::vector<std::pair<std::string, json>> results{
        {"tools/list", client.result("tools/list")},
        {"prompts/list", client.result("prompts/list")},
        {"resources/list", resources},
        {"resources/templates/list", client.result("resources/templates/list")},
        {"resources/read", client.result("resources/read", json{{"uri", first_uri}})},
    };
    for (const auto& [method, result] : results) {
        SCOPED_TRACE(method);
        if (!client.modern()) {
            EXPECT_FALSE(result.contains("ttlMs"));
            EXPECT_FALSE(result.contains("cacheScope"));
            continue;
        }
        ASSERT_TRUE(result.contains("ttlMs")) << result.dump().substr(0, 300);
        ASSERT_TRUE(result.contains("cacheScope")) << result.dump().substr(0, 300);
        EXPECT_TRUE(result.at("ttlMs").is_number_integer());
        EXPECT_GE(result.at("ttlMs").get<std::int64_t>(), 0);
        const std::string scope = result.at("cacheScope").get<std::string>();
        EXPECT_TRUE(scope == "public" || scope == "private") << scope;
    }
}

}  // namespace
