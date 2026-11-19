// The requests that the official conformance runner's scenarios send to the conformance fixture's registrations
// (everything.h), sent over real sockets on the loopback interface in both eras, with the assertions that the
// scenarios make about what comes back. The runner needs a package manager and the network; this test needs neither
// and runs with the rest of the suite.
//
// The scenarios covered are those of resources (list, read of text, of binary and of a template, a URI nothing
// serves), prompts (list and the four gets), completion, and the caching hints of the revision that has them, and the
// ones in which the server asks its client for input while it serves a call: where the result asks and the client
// retries the call with the answers, and where the server sends its requests on the stream of the call and the client
// answers them with requests of its own. One connection per test; the HTTP library's client is used here, in the
// tests only.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    Client(std::uint16_t port, std::string revision, json capabilities = default_capabilities())
        : revision_(std::move(revision)), capabilities_(std::move(capabilities)), port_(port), http_("127.0.0.1", port) {
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
    json call(const std::string& method, json params = json::object()) { return call_with(method, std::move(params), {}); }

    // The same, with `extra` beside the headers the client sends on its own.
    json call_with(const std::string& method, json params, const httplib::Headers& extra) {
        httplib::Headers headers = modern() ? modern_headers(method, params) : legacy_headers();
        for (const auto& field : extra) headers.emplace(field.first, field.second);
        if (modern()) params["_meta"] = modern_meta(params);
        const auto answer = http_.Post(path, headers, rpc(next_id_++, method, std::move(params)), json_type);
        if (!answer) throw std::runtime_error(method + " was not answered");
        last_status_ = answer->status;
        const std::string type = answer->get_header_value("Content-Type");
        if (type.rfind(json_type, 0) != 0) throw std::runtime_error(method + " was answered with " + type);
        return json::parse(answer->body);
    }

    int last_status() const { return last_status_; }

    // What the runner's client declares.
    static json default_capabilities() {
        return json{{"sampling", json::object()}, {"elicitation", json::object()}, {"roots", {{"listChanged", true}}}};
    }

    // ---- Calls that ask the client for input -------------------------------------------------------------------

    // The request a server made of its client while it served a call: the key it was asked by (only where the result
    // asks), the method and the params.
    struct Asked {
        std::string key;
        std::string method;
        json params;
    };

    // What one call came to: every request the server made of the client, in order, the states it sent along, the
    // answer that ended the call, and the HTTP status of the reply the client gave to a request of the server.
    struct Journey {
        std::vector<Asked> asked;
        std::vector<std::string> states;
        json answer;
        int rounds = 0;
        std::vector<int> reply_statuses;
        std::vector<json> notifications;
    };

    using Responder = std::function<json(const std::string& method, const json& params)>;

    // Calls `method` and answers what the server asks with `respond`, in the way of the revision: where the result
    // asks, the call is retried with the answers and the state; where the server sends requests, the client answers
    // them with requests of its own.
    Journey travel(const std::string& method, const json& params, const Responder& respond) {
        return modern() ? retried(method, params, respond) : streamed(method, params, respond);
    }

    // The result of a request that must have one.
    json result(const std::string& method, json params = json::object()) {
        json answer = call(method, std::move(params));
        if (!answer.contains("result")) throw std::runtime_error(method + " was answered with " + answer.dump());
        return answer.at("result");
    }

private:
    // The 2026-07-28 way: an answer that asks is followed by a call with the answers and the state.
    Journey retried(const std::string& method, json params, const Responder& respond) {
        Journey trip;
        for (int round = 0; round < 8; ++round) {
            trip.answer = call(method, params);
            if (!trip.answer.contains("result")) return trip;
            const json& reply = trip.answer.at("result");
            if (!reply.contains("resultType") || reply.at("resultType") != "input_required") return trip;
            ++trip.rounds;
            json answers = json::object();
            if (reply.contains("inputRequests")) {
                for (const auto& item : reply.at("inputRequests").items()) {
                    const json& request = item.value();
                    const json asked_params = request.contains("params") ? request.at("params") : json::object();
                    trip.asked.push_back(Asked{item.key(), request.at("method").get<std::string>(), asked_params});
                    answers[item.key()] = respond(request.at("method").get<std::string>(), asked_params);
                }
            }
            params["inputResponses"] = std::move(answers);
            if (reply.contains("requestState")) {
                trip.states.push_back(reply.at("requestState").get<std::string>());
                params["requestState"] = reply.at("requestState");
            }
        }
        throw std::runtime_error(method + " kept asking");
    }

    // The 2025-11-25 way: the server's requests arrive on the stream that carries the call's answer, and the client
    // answers each with a POST of its own that carries the response.
    Journey streamed(const std::string& method, const json& params, const Responder& respond) {
        Journey trip;
        httplib::Client replies("127.0.0.1", port_);
        replies.set_keep_alive(false);
        replies.set_connection_timeout(30, 0);
        replies.set_read_timeout(60, 0);
        replies.set_write_timeout(60, 0);

        std::string type;
        std::string whole;
        std::string pending;  // the bytes of an event that is not complete yet
        std::string data;     // the data lines of the event being read
        bool has_data = false;
        const auto handle = [&](const json& message) {
            if (message.contains("method")) {
                if (!message.contains("id")) {
                    trip.notifications.push_back(message);
                    return;
                }
                const std::string asked_method = message.at("method").get<std::string>();
                const json asked_params = message.contains("params") ? message.at("params") : json::object();
                trip.asked.push_back(Asked{std::string(), asked_method, asked_params});
                const json response{{"jsonrpc", "2.0"}, {"id", message.at("id")}, {"result", respond(asked_method, asked_params)}};
                const auto posted = replies.Post(path, legacy_headers(), response.dump(), json_type);
                trip.reply_statuses.push_back(posted ? posted->status : 0);
            } else if (message.contains("result") || message.contains("error")) {
                trip.answer = message;
            }
        };
        const auto take_lines = [&] {
            for (;;) {
                const std::size_t end = pending.find('\n');
                if (end == std::string::npos) return;
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) {
                    if (has_data) handle(json::parse(data));
                    data.clear();
                    has_data = false;
                } else if (line.rfind("data:", 0) == 0) {
                    std::string value = line.substr(5);
                    if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    if (has_data) data += '\n';
                    data += value;
                    has_data = true;
                }
            }
        };

        httplib::Request request;
        request.method = "POST";
        request.path = path;
        request.headers = legacy_headers();
        request.headers.emplace("Content-Type", json_type);
        request.body = rpc(next_id_++, method, params);
        request.response_handler = [&](const httplib::Response& response) {
            type = response.get_header_value("Content-Type");
            last_status_ = response.status;
            return true;
        };
        request.content_receiver = [&](const char* bytes, std::size_t length, std::size_t, std::size_t) {
            if (type.rfind("text/event-stream", 0) == 0) {
                pending.append(bytes, length);
                take_lines();
            } else {
                whole.append(bytes, length);
            }
            return true;
        };
        const auto sent = http_.send(request);
        if (!sent) throw std::runtime_error(method + " was not answered");
        if (type.rfind("text/event-stream", 0) == 0) {
            pending += "\n\n";
            take_lines();
        } else {
            trip.answer = json::parse(whole);
        }
        return trip;
    }

    static std::string rpc(int id, const std::string& method, json params) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump();
    }
    static std::string notification(const std::string& method) {
        return json{{"jsonrpc", "2.0"}, {"method", method}}.dump();
    }
    json initialize_params() const {
        return json{{"protocolVersion", revision_},
                    {"capabilities", capabilities_},
                    {"clientInfo", {{"name", "conformance-test-client"}, {"version", "1.0.0"}}}};
    }
    // The members of the request's own `_meta` win over the ones the client writes, as they do in the runner.
    json modern_meta(const json& params) const {
        json meta{{"io.modelcontextprotocol/protocolVersion", revision_},
                  {"io.modelcontextprotocol/clientInfo", {{"name", "conformance-test-client"}, {"version", "1.0.0"}}},
                  {"io.modelcontextprotocol/clientCapabilities", capabilities_}};
        if (params.contains("_meta") && params.at("_meta").is_object()) {
            for (const auto& item : params.at("_meta").items()) meta[item.key()] = item.value();
        }
        return meta;
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
    json capabilities_;
    std::uint16_t port_ = 0;
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
    EXPECT_EQ(uris, (std::set<std::string>{"test://static-binary", "test://static-text", "test://watched-resource"}))
        << "direct resources only";
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
                                            "test_prompt_with_embedded_resource", "test_prompt_with_image",
                                            "test_input_required_result_prompt"}));

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

// ---- Input: what the scenarios that ask the client for input do ------------------------------------------------------

// The value a user would enter into a field of a form: the field's default or first choice, else a text.
json value_for(const std::string& field, const json& schema) {
    if (field == "name") return "Alice";
    if (field == "color") return "blue";
    if (schema.contains("default")) return schema.at("default");
    const std::string kind = schema.contains("type") ? schema.at("type").get<std::string>() : std::string("string");
    if (kind == "boolean") return true;
    if (kind == "integer") return 1;
    if (kind == "number") return 1.5;
    if (kind == "array") {
        const json& items = schema.at("items");
        json chosen = json::array();
        if (items.contains("enum")) chosen.push_back(items.at("enum").at(0));
        if (items.contains("anyOf")) chosen.push_back(items.at("anyOf").at(0).at("const"));
        return chosen;
    }
    if (schema.contains("enum")) return schema.at("enum").at(0);
    if (schema.contains("oneOf")) return schema.at("oneOf").at(0).at("const");
    return "test";
}

// What the client's user, model and roots answer.
json user_answer(const std::string& method, const json& params) {
    if (method == "elicitation/create") {
        json content = json::object();
        for (const auto& item : params.at("requestedSchema").at("properties").items()) {
            content[item.key()] = value_for(item.key(), item.value());
        }
        return json{{"action", "accept"}, {"content", content}};
    }
    if (method == "sampling/createMessage") {
        return json{{"role", "assistant"},
                    {"content", {{"type", "text"}, {"text", "The capital of France is Paris."}}},
                    {"model", "test-model"},
                    {"stopReason", "endTurn"}};
    }
    if (method == "roots/list") {
        return json{{"roots", json::array({json{{"uri", "file:///test/root"}, {"name", "Test Root"}}})}};
    }
    throw std::runtime_error("a request nobody answers: " + method);
}

std::string first_text(const json& result) { return result.at("content").at(0).at("text").get<std::string>(); }

json tool_params(const std::string& name) { return json{{"name", name}, {"arguments", json::object()}}; }

std::vector<std::string> methods_of(const std::vector<Client::Asked>& asked) {
    std::vector<std::string> methods;
    for (const Client::Asked& request : asked) methods.push_back(request.method);
    std::sort(methods.begin(), methods.end());
    return methods;
}

bool failed(const json& result) { return result.contains("isError") && result.at("isError") == true; }

bool asks(const json& result) { return result.contains("resultType") && result.at("resultType") == "input_required"; }

// The forms of the two scenarios that check what the server puts into a form.
void expect_form_of_defaults(const json& schema) {
    const json& properties = schema.at("properties");
    EXPECT_EQ(properties.at("name").at("type"), "string");
    EXPECT_EQ(properties.at("name").at("default"), "John Doe");
    EXPECT_EQ(properties.at("age").at("type"), "integer");
    EXPECT_TRUE(properties.at("age").at("default").is_number_integer());
    EXPECT_EQ(properties.at("age").at("default").get<std::int64_t>(), 30);
    EXPECT_EQ(properties.at("score").at("type"), "number");
    EXPECT_DOUBLE_EQ(properties.at("score").at("default").get<double>(), 95.5);
    EXPECT_EQ(properties.at("status").at("type"), "string");
    EXPECT_TRUE(properties.at("status").at("enum").is_array());
    EXPECT_EQ(properties.at("status").at("default"), "active");
    bool member = false;
    for (const json& choice : properties.at("status").at("enum")) member = member || choice == "active";
    EXPECT_TRUE(member) << "the default is one of the choices";
    EXPECT_EQ(properties.at("verified").at("type"), "boolean");
    EXPECT_EQ(properties.at("verified").at("default"), true);
}

void expect_form_of_enums(const json& schema) {
    const json& properties = schema.at("properties");
    const json& untitled = properties.at("untitledSingle");
    EXPECT_EQ(untitled.at("type"), "string");
    EXPECT_TRUE(untitled.at("enum").is_array());
    EXPECT_FALSE(untitled.contains("oneOf"));
    EXPECT_FALSE(untitled.contains("enumNames"));

    const json& titled = properties.at("titledSingle");
    EXPECT_EQ(titled.at("type"), "string");
    ASSERT_TRUE(titled.at("oneOf").is_array());
    for (const json& choice : titled.at("oneOf")) {
        EXPECT_TRUE(choice.at("const").is_string());
        EXPECT_TRUE(choice.at("title").is_string());
    }
    EXPECT_FALSE(titled.contains("enum"));

    const json& legacy = properties.at("legacyEnum");
    EXPECT_EQ(legacy.at("type"), "string");
    ASSERT_TRUE(legacy.at("enum").is_array());
    ASSERT_TRUE(legacy.at("enumNames").is_array());
    EXPECT_EQ(legacy.at("enumNames").size(), legacy.at("enum").size());

    const json& many = properties.at("untitledMulti");
    EXPECT_EQ(many.at("type"), "array");
    EXPECT_EQ(many.at("items").at("type"), "string");
    EXPECT_TRUE(many.at("items").at("enum").is_array());
    EXPECT_FALSE(many.at("items").contains("anyOf"));

    const json& titled_many = properties.at("titledMulti");
    EXPECT_EQ(titled_many.at("type"), "array");
    ASSERT_TRUE(titled_many.at("items").at("anyOf").is_array());
    for (const json& choice : titled_many.at("items").at("anyOf")) {
        EXPECT_TRUE(choice.at("const").is_string());
        EXPECT_TRUE(choice.at("title").is_string());
    }
    EXPECT_FALSE(titled_many.at("items").contains("enum"));
}

// Both eras: the tools that ask are called, what they ask is answered as the scenarios answer it, and the call ends
// with a result that has content.
struct InputTool {
    const char* name;
    std::vector<std::string> methods;  // what is asked over the whole call, sorted
    const char* text;                  // what the result says
};

TEST_P(FixtureScenarios, EveryToolThatAsksIsAnsweredAndTheCallEnds) {
    const std::vector<InputTool> tools{
        {"test_input_required_result_elicitation", {"elicitation/create"}, "Hello, Alice!"},
        {"test_input_required_result_sampling", {"sampling/createMessage"}, "The model said: The capital of France is Paris."},
        {"test_input_required_result_list_roots", {"roots/list"}, "Roots (1): file:///test/root"},
        {"test_input_required_result_request_state", {"elicitation/create"}, "state-ok"},
        {"test_input_required_result_multiple_inputs",
         {"elicitation/create", "roots/list", "sampling/createMessage"},
         "All three answers came"},
        {"test_input_required_result_multi_round", {"elicitation/create", "elicitation/create"}, "Both steps are done"},
        {"test_input_required_result_tampered_state", {"elicitation/create"}, "The state was accepted"},
        {"test_input_required_result_capabilities", {"elicitation/create"}, "Answered"},
        {"test_missing_capability", {"sampling/createMessage"}, "The model said"},
        {"test_streaming_elicitation", {"elicitation/create"}, "Elicitation completed: action=accept"},
        {"test_elicitation_sep1034_defaults", {"elicitation/create"}, "Elicitation completed: action=accept"},
        {"test_elicitation_sep1330_enums", {"elicitation/create"}, "Elicitation completed: action=accept"},
    };
    for (const InputTool& tool : tools) {
        SCOPED_TRACE(tool.name);
        const Client::Journey trip = client.travel("tools/call", tool_params(tool.name), user_answer);
        ASSERT_TRUE(trip.answer.contains("result")) << trip.answer.dump();
        const json& result = trip.answer.at("result");
        EXPECT_FALSE(asks(result));
        EXPECT_FALSE(failed(result)) << result.dump();
        ASSERT_TRUE(result.at("content").is_array());
        EXPECT_NE(first_text(result).find(tool.text), std::string::npos) << result.dump();
        EXPECT_EQ(methods_of(trip.asked), tool.methods);
        for (const int status : trip.reply_statuses) EXPECT_EQ(status, 202);
    }
}

TEST_P(FixtureScenarios, TheToolsThatAskTakeTheirArgumentsAndPutThemIntoWhatTheyAsk) {
    const Client::Journey sampling = client.travel(
        "tools/call", json{{"name", "test_sampling"}, {"arguments", {{"prompt", "Test prompt for sampling"}}}}, user_answer);
    ASSERT_EQ(sampling.asked.size(), 1U);
    EXPECT_EQ(sampling.asked.at(0).method, "sampling/createMessage");
    const json& message = sampling.asked.at(0).params.at("messages").at(0);
    EXPECT_EQ(message.at("role"), "user");
    EXPECT_EQ(message.at("content").at("type"), "text");
    EXPECT_EQ(message.at("content").at("text"), "Test prompt for sampling");
    EXPECT_EQ(sampling.asked.at(0).params.at("maxTokens").get<std::int64_t>(), 100);
    ASSERT_TRUE(sampling.answer.contains("result")) << sampling.answer.dump();
    EXPECT_EQ(first_text(sampling.answer.at("result")), "LLM response: The capital of France is Paris.");

    const Client::Journey form = client.travel(
        "tools/call", json{{"name", "test_elicitation"}, {"arguments", {{"message", "Please provide your information"}}}},
        user_answer);
    ASSERT_EQ(form.asked.size(), 1U);
    EXPECT_EQ(form.asked.at(0).method, "elicitation/create");
    EXPECT_EQ(form.asked.at(0).params.at("message"), "Please provide your information");
    const json& schema = form.asked.at(0).params.at("requestedSchema");
    EXPECT_EQ(schema.at("type"), "object");
    EXPECT_TRUE(schema.at("properties").contains("username"));
    EXPECT_TRUE(schema.at("properties").contains("email"));
    ASSERT_TRUE(form.answer.contains("result")) << form.answer.dump();
    EXPECT_EQ(first_text(form.answer.at("result")).rfind("User response: action=accept", 0), 0U);
}

TEST_P(FixtureScenarios, TheFormWithDefaultsHasADefaultForEveryKindOfField) {
    const Client::Journey trip = client.travel("tools/call", tool_params("test_elicitation_sep1034_defaults"), user_answer);
    ASSERT_EQ(trip.asked.size(), 1U);
    expect_form_of_defaults(trip.asked.at(0).params.at("requestedSchema"));
}

TEST_P(FixtureScenarios, TheFormWithChoicesHasEveryKindOfChoice) {
    const Client::Journey trip = client.travel("tools/call", tool_params("test_elicitation_sep1330_enums"), user_answer);
    ASSERT_EQ(trip.asked.size(), 1U);
    expect_form_of_enums(trip.asked.at(0).params.at("requestedSchema"));
}

TEST_P(FixtureScenarios, ThePromptThatAsksIsAnsweredAndEndsWithMessages) {
    const Client::Journey trip = client.travel("prompts/get", json{{"name", "test_input_required_result_prompt"}}, user_answer);
    ASSERT_EQ(trip.asked.size(), 1U);
    EXPECT_EQ(trip.asked.at(0).method, "elicitation/create");
    ASSERT_TRUE(trip.answer.contains("result")) << trip.answer.dump();
    const json& result = trip.answer.at("result");
    EXPECT_FALSE(asks(result));
    ASSERT_TRUE(result.at("messages").is_array());
    ASSERT_GE(result.at("messages").size(), 1U);
    EXPECT_EQ(result.at("messages").at(0).at("role"), "user");
    EXPECT_EQ(result.at("messages").at(0).at("content").at("type"), "text");
}

TEST_P(FixtureScenarios, TheListsAreNeverAnsweredWithARequestForInput) {
    for (const char* method : {"tools/list", "prompts/list"}) {
        SCOPED_TRACE(method);
        const json answer = client.call(method);
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_FALSE(asks(answer.at("result")));
    }
}

TEST_P(FixtureScenarios, EveryToolAndThePromptThatAskAreListed) {
    const json tools = client.result("tools/list").at("tools");
    for (const char* name :
         {"test_input_required_result_elicitation", "test_input_required_result_sampling",
          "test_input_required_result_list_roots", "test_input_required_result_request_state",
          "test_input_required_result_multiple_inputs", "test_input_required_result_multi_round",
          "test_input_required_result_tampered_state", "test_input_required_result_capabilities", "test_sampling",
          "test_elicitation", "test_elicitation_sep1034_defaults", "test_elicitation_sep1330_enums",
          "test_missing_capability", "test_streaming_elicitation"}) {
        const json found = entry_of(tools, "name", name);
        EXPECT_FALSE(found.is_null()) << name;
    }
    const json sampling = entry_of(tools, "name", "test_sampling");
    ASSERT_FALSE(sampling.is_null());
    EXPECT_TRUE(sampling.at("inputSchema").at("properties").contains("prompt"));
    const json elicitation = entry_of(tools, "name", "test_elicitation");
    ASSERT_FALSE(elicitation.is_null());
    EXPECT_TRUE(elicitation.at("inputSchema").at("properties").contains("message"));
}

// ---- Input where the result asks (2026-07-28): what each scenario sends and accepts ---------------------------------

class ModernInput : public ::testing::Test {
protected:
    ModernInput() : client(fixture.port, modern_revision) {}

    // The tool called without arguments, with `extra` beside its name and arguments (inputResponses, requestState,
    // _meta).
    json call_tool(const std::string& name, const json& extra = json::object()) {
        json params = tool_params(name);
        for (const auto& item : extra.items()) params[item.key()] = item.value();
        return client.call("tools/call", params);
    }
    // The key of the first request, its method and its params.
    static std::string first_key(const json& result) {
        for (const auto& item : result.at("inputRequests").items()) return item.key();
        return std::string();
    }
    static std::string method_of_first(const json& result) {
        for (const auto& item : result.at("inputRequests").items()) return item.value().at("method").get<std::string>();
        return std::string();
    }
    static json params_of_first(const json& result) {
        for (const auto& item : result.at("inputRequests").items()) {
            return item.value().contains("params") ? item.value().at("params") : json::object();
        }
        return json::object();
    }

    Fixture fixture;
    Client client;
};

TEST_F(ModernInput, TheElicitationAsksByTheKeyUserNameAndEndsWithGreetingsAfterTheRetry) {
    const json first = call_tool("test_input_required_result_elicitation");
    ASSERT_TRUE(first.contains("result")) << first.dump();
    const json& result = first.at("result");
    ASSERT_TRUE(result.contains("resultType"));
    EXPECT_EQ(result.at("resultType"), "input_required");
    ASSERT_TRUE(result.at("inputRequests").contains("user_name"));
    EXPECT_EQ(result.at("inputRequests").at("user_name").at("method"), "elicitation/create");
    EXPECT_TRUE(result.at("inputRequests").at("user_name").at("params").contains("requestedSchema"));

    json extra{{"inputResponses", {{"user_name", {{"action", "accept"}, {"content", {{"name", "Alice"}}}}}}}};
    if (result.contains("requestState")) extra["requestState"] = result.at("requestState");
    const json second = call_tool("test_input_required_result_elicitation", extra);
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_FALSE(asks(second.at("result")));
    ASSERT_TRUE(second.at("result").at("content").is_array());
    EXPECT_EQ(first_text(second.at("result")), "Hello, Alice!");
}

TEST_F(ModernInput, TheSamplingAndTheRootsAreAskedAndAnsweredAfterTheRetry) {
    for (const char* name : {"test_input_required_result_sampling", "test_input_required_result_list_roots"}) {
        SCOPED_TRACE(name);
        const json first = call_tool(name);
        ASSERT_TRUE(first.contains("result")) << first.dump();
        ASSERT_TRUE(asks(first.at("result"))) << first.dump();
        const std::string method = method_of_first(first.at("result"));
        EXPECT_EQ(method, std::string(name) == "test_input_required_result_sampling" ? "sampling/createMessage" : "roots/list");
        json extra{{"inputResponses", {{first_key(first.at("result")), user_answer(method, params_of_first(first.at("result")))}}}};
        extra["requestState"] = first.at("result").at("requestState");
        const json second = call_tool(name, extra);
        ASSERT_TRUE(second.contains("result")) << second.dump();
        EXPECT_FALSE(asks(second.at("result")));
    }
}

TEST_F(ModernInput, TheRequestStateComesWithTheRequestAndIsCheckedWhenItComesBack) {
    const json first = call_tool("test_input_required_result_request_state");
    ASSERT_TRUE(first.contains("result")) << first.dump();
    const json& result = first.at("result");
    ASSERT_TRUE(asks(result));
    ASSERT_TRUE(result.contains("inputRequests"));
    ASSERT_TRUE(result.at("requestState").is_string());
    EXPECT_FALSE(result.at("requestState").get<std::string>().empty());

    const json answers{{"confirm", {{"action", "accept"}, {"content", {{"ok", true}}}}}};
    const json second = call_tool("test_input_required_result_request_state",
                                  json{{"inputResponses", answers}, {"requestState", result.at("requestState")}});
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_FALSE(asks(second.at("result")));
    EXPECT_NE(first_text(second.at("result")).find("state-ok"), std::string::npos);
}

TEST_F(ModernInput, ThreeKindsOfRequestAreAskedAtOnceWithAState) {
    const json first = call_tool("test_input_required_result_multiple_inputs");
    ASSERT_TRUE(first.contains("result")) << first.dump();
    const json& result = first.at("result");
    ASSERT_TRUE(asks(result));
    EXPECT_FALSE(result.at("requestState").get<std::string>().empty());
    std::set<std::string> methods;
    json answers = json::object();
    for (const auto& item : result.at("inputRequests").items()) {
        const std::string method = item.value().at("method").get<std::string>();
        methods.insert(method);
        answers[item.key()] = user_answer(method, item.value().contains("params") ? item.value().at("params") : json::object());
    }
    EXPECT_GE(result.at("inputRequests").size(), 3U);
    EXPECT_EQ(methods, (std::set<std::string>{"elicitation/create", "sampling/createMessage", "roots/list"}));
    const json second = call_tool("test_input_required_result_multiple_inputs",
                                  json{{"inputResponses", answers}, {"requestState", result.at("requestState")}});
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_FALSE(asks(second.at("result")));
}

TEST_F(ModernInput, TheMultiRoundCallAsksTwiceWithAStateOfEachAndThenEnds) {
    const json first = call_tool("test_input_required_result_multi_round");
    ASSERT_TRUE(first.contains("result")) << first.dump();
    ASSERT_TRUE(asks(first.at("result")));
    ASSERT_TRUE(first.at("result").contains("requestState"));
    const std::string key_one = first_key(first.at("result"));

    const json second = call_tool(
        "test_input_required_result_multi_round",
        json{{"inputResponses", {{key_one, {{"action", "accept"}, {"content", {{"name", "Alice"}}}}}}},
             {"requestState", first.at("result").at("requestState")}});
    ASSERT_TRUE(second.contains("result")) << second.dump();
    ASSERT_TRUE(asks(second.at("result"))) << second.dump();
    ASSERT_TRUE(second.at("result").contains("requestState"));
    EXPECT_NE(second.at("result").at("requestState"), first.at("result").at("requestState"));
    const std::string key_two = first_key(second.at("result"));
    EXPECT_NE(key_two, key_one);

    const json third = call_tool(
        "test_input_required_result_multi_round",
        json{{"inputResponses", {{key_two, {{"action", "accept"}, {"content", {{"color", "blue"}}}}}}},
             {"requestState", second.at("result").at("requestState")}});
    ASSERT_TRUE(third.contains("result")) << third.dump();
    EXPECT_FALSE(asks(third.at("result")));
}

TEST_F(ModernInput, AnswersThatCameWithoutTheStateTheServerIssuedAreNotAnswers) {
    // Wrong keys, no state: the call asks again, with a result and not with an error.
    const json wrong = call_tool("test_input_required_result_elicitation",
                                 json{{"inputResponses", {{"wrong_key", {{"action", "accept"}, {"content", {{"data", "wrong"}}}}}}}});
    ASSERT_TRUE(wrong.contains("result")) << wrong.dump();
    ASSERT_TRUE(asks(wrong.at("result")));
    EXPECT_TRUE(wrong.at("result").at("inputRequests").contains("user_name"));

    // The right key and extra ones, no state: asks again as well, because no state says that anything was asked.
    const json extra = call_tool(
        "test_input_required_result_elicitation",
        json{{"inputResponses",
              {{"user_name", {{"action", "accept"}, {"content", {{"name", "Alice"}}}}},
               {"unknown_extra_key", {{"action", "accept"}, {"content", {{"foo", "bar"}}}}},
               {"another_unexpected", {{"action", "accept"}, {"content", {{"baz", 123}}}}}}}});
    ASSERT_TRUE(extra.contains("result")) << extra.dump();
    EXPECT_TRUE(asks(extra.at("result")));

    // With the state, the same extra keys are ignored and the call ends.
    const json first = call_tool("test_input_required_result_elicitation");
    const json with_state = call_tool(
        "test_input_required_result_elicitation",
        json{{"inputResponses",
              {{"user_name", {{"action", "accept"}, {"content", {{"name", "Alice"}}}}},
               {"unknown_extra_key", {{"action", "accept"}, {"content", {{"foo", "bar"}}}}}}},
             {"requestState", first.at("result").at("requestState")}});
    ASSERT_TRUE(with_state.contains("result")) << with_state.dump();
    EXPECT_FALSE(asks(with_state.at("result")));
    EXPECT_EQ(first_text(with_state.at("result")), "Hello, Alice!");
}

TEST_F(ModernInput, AnswersOfTheWrongShapeAreNeverTakenForAComplete) {
    // A number for the answer, and a null for the whole of them.
    const json number = call_tool("test_input_required_result_elicitation", json{{"inputResponses", {{"user_name", 12345}}}});
    ASSERT_TRUE(number.contains("result") || number.contains("error")) << number.dump();
    if (number.contains("result")) {
        EXPECT_TRUE(asks(number.at("result")));
    }
    const json nothing = call_tool("test_input_required_result_elicitation", json{{"inputResponses", nullptr}});
    EXPECT_TRUE(nothing.contains("error")) << nothing.dump();

    // With a state that was issued, an answer that is not an object is a malformed answer.
    const json first = call_tool("test_input_required_result_elicitation");
    const json malformed = call_tool("test_input_required_result_elicitation",
                                     json{{"inputResponses", {{"user_name", 12345}}},
                                          {"requestState", first.at("result").at("requestState")}});
    ASSERT_TRUE(malformed.contains("error")) << malformed.dump();
    EXPECT_EQ(malformed.at("error").at("code"), -32602);
}

TEST_F(ModernInput, AStateThatWasChangedIsAnErrorAndAnUnchangedOneStillWorks) {
    const json first = call_tool("test_input_required_result_tampered_state");
    ASSERT_TRUE(first.contains("result")) << first.dump();
    ASSERT_TRUE(asks(first.at("result")));
    const std::string state = first.at("result").at("requestState").get<std::string>();
    ASSERT_FALSE(state.empty());
    const std::string key = first_key(first.at("result"));
    const json answers{{key, {{"action", "accept"}, {"content", {{"ok", true}}}}}};

    const json tampered = call_tool("test_input_required_result_tampered_state",
                                    json{{"inputResponses", answers}, {"requestState", state + "-TAMPERED"}});
    EXPECT_FALSE(tampered.contains("result")) << tampered.dump();
    ASSERT_TRUE(tampered.contains("error")) << tampered.dump();
    EXPECT_EQ(tampered.at("error").at("code"), -32602);

    const json whole = call_tool("test_input_required_result_tampered_state",
                                 json{{"inputResponses", answers}, {"requestState", state}});
    ASSERT_TRUE(whole.contains("result")) << whole.dump();
    EXPECT_FALSE(asks(whole.at("result")));

    // The state belongs to the call it was issued for.
    const json other = call_tool("test_input_required_result_request_state",
                                 json{{"inputResponses", answers}, {"requestState", state}});
    EXPECT_TRUE(other.contains("error")) << other.dump();
}

TEST_F(ModernInput, TheRequestsAreOnlyThoseTheClientDeclaredItCanAnswer) {
    // The runner's capability scenario: the client declares sampling and no elicitation.
    const json meta{{"io.modelcontextprotocol/clientCapabilities", {{"sampling", json::object()}}}};
    const json sampling_only = call_tool("test_input_required_result_capabilities", json{{"_meta", meta}});
    ASSERT_TRUE(sampling_only.contains("result")) << sampling_only.dump();
    ASSERT_TRUE(asks(sampling_only.at("result")));
    ASSERT_GE(sampling_only.at("result").at("inputRequests").size(), 1U);
    for (const auto& item : sampling_only.at("result").at("inputRequests").items()) {
        EXPECT_EQ(item.value().at("method"), "sampling/createMessage");
    }

    // With the runner's declaration, the form is asked.
    const json all = call_tool("test_input_required_result_capabilities");
    ASSERT_TRUE(all.contains("result")) << all.dump();
    ASSERT_TRUE(asks(all.at("result")));
    EXPECT_EQ(method_of_first(all.at("result")), "elicitation/create");

    // A client that declares neither is not asked at all.
    const json neither = call_tool("test_input_required_result_capabilities",
                                   json{{"_meta", {{"io.modelcontextprotocol/clientCapabilities", json::object()}}}});
    ASSERT_TRUE(neither.contains("result")) << neither.dump();
    EXPECT_FALSE(asks(neither.at("result")));
}

TEST_F(ModernInput, ThePromptThatAsksAnswersWithARequestAndThenWithMessages) {
    const json first = client.call("prompts/get", json{{"name", "test_input_required_result_prompt"}});
    ASSERT_TRUE(first.contains("result")) << first.dump();
    const json& result = first.at("result");
    ASSERT_TRUE(asks(result));
    ASSERT_TRUE(result.at("inputRequests").contains("user_context"));
    EXPECT_EQ(result.at("inputRequests").at("user_context").at("method"), "elicitation/create");
    const json second = client.call(
        "prompts/get", json{{"name", "test_input_required_result_prompt"},
                            {"inputResponses", {{"user_context", {{"action", "accept"}, {"content", {{"context", "test context"}}}}}}},
                            {"requestState", result.at("requestState")}});
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_FALSE(asks(second.at("result")));
    EXPECT_TRUE(second.at("result").at("messages").is_array());
    EXPECT_NE(second.at("result").at("messages").dump().find("test context"), std::string::npos);
}

TEST_F(ModernInput, ACallThatNeedsTheModelOfAClientWithoutOneIsRefusedWithWhatItLacks) {
    Client bare(fixture.port, modern_revision, json::object());
    const json answer = bare.call("tools/call", tool_params("test_missing_capability"));
    ASSERT_TRUE(answer.contains("error")) << answer.dump();
    EXPECT_EQ(answer.at("error").at("code"), -32021);
    EXPECT_EQ(bare.last_status(), 400);
    ASSERT_TRUE(answer.at("error").at("data").at("requiredCapabilities").at("sampling").is_object());
    ASSERT_TRUE(answer.contains("id"));
    EXPECT_FALSE(answer.at("id").is_null());
}

TEST_F(ModernInput, ACallThatAsksTheUserSendsNothingButItsResult) {
    Client asking(fixture.port, modern_revision, json{{"elicitation", json::object()}});
    const json answer = asking.call("tools/call", tool_params("test_streaming_elicitation"));
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_TRUE(asks(answer.at("result")));
    EXPECT_EQ(method_of_first(answer.at("result")), "elicitation/create");
}

// ---- Input where the server sends requests (2025-11-25) ------------------------------------------------------------

class LegacyInput : public ::testing::Test {
protected:
    Fixture fixture;
};

TEST_F(LegacyInput, TheRequestsAreOnlyThoseTheClientDeclaredItCanAnswer) {
    Client model_only(fixture.port, legacy_revision, json{{"sampling", json::object()}});
    const Client::Journey trip = model_only.travel("tools/call", tool_params("test_input_required_result_capabilities"), user_answer);
    ASSERT_EQ(trip.asked.size(), 1U);
    EXPECT_EQ(trip.asked.at(0).method, "sampling/createMessage");
    ASSERT_TRUE(trip.answer.contains("result")) << trip.answer.dump();
    EXPECT_FALSE(failed(trip.answer.at("result")));
}

TEST_F(LegacyInput, AClientThatCannotTakeTheRequestIsToldSoAndIsNotAsked) {
    Client bare(fixture.port, legacy_revision, json::object());
    const Client::Journey trip = bare.travel("tools/call", tool_params("test_missing_capability"), user_answer);
    EXPECT_TRUE(trip.asked.empty());
    ASSERT_TRUE(trip.answer.contains("result")) << trip.answer.dump();
    EXPECT_TRUE(failed(trip.answer.at("result")));
}

TEST_F(LegacyInput, TheThreeRequestsOfOneRoundAreSentTogether) {
    Client client(fixture.port, legacy_revision);
    std::vector<std::string> seen;
    const Client::Journey trip = client.travel("tools/call", tool_params("test_input_required_result_multiple_inputs"),
                                               [&](const std::string& method, const json& params) {
                                                   seen.push_back(method);
                                                   return user_answer(method, params);
                                               });
    EXPECT_EQ(seen.size(), 3U);
    ASSERT_TRUE(trip.answer.contains("result")) << trip.answer.dump();
}

// ---- Tools whose parameters are mirrored into headers, and the schema with the keywords of 2020-12 --------------------

TEST_P(FixtureScenarios, TheSchemaWithTheKeywordsOfJsonSchema2020_12IsListedAsItWasWritten) {
    // The scenario reads $schema, $defs (with the anchor in address), additionalProperties, the composition and the
    // condition from the listing; the whole schema must arrive as registered, in every revision.
    const json tools = client.result("tools/list").at("tools");
    const json found = entry_of(tools, "name", "json_schema_2020_12_tool");
    ASSERT_FALSE(found.is_null());
    const json& schema = found.at("inputSchema");
    EXPECT_EQ(schema, conformance::json_schema_2020_12_input());
    EXPECT_EQ(schema.at("$schema"), "https://json-schema.org/draft/2020-12/schema");
    EXPECT_TRUE(schema.at("$defs").at("address").contains("$anchor"));
    EXPECT_EQ(schema.at("additionalProperties"), false);
    EXPECT_TRUE(schema.at("allOf").at(0).contains("anyOf"));
    EXPECT_TRUE(schema.contains("if") && schema.contains("then") && schema.contains("else"));
}

TEST_P(FixtureScenarios, TheToolWithMirroredParametersIsListedWithTheirMarks) {
    // The scenario looks for the first tool whose properties carry the mark, and in it for a string.
    const json tools = client.result("tools/list").at("tools");
    const json found = entry_of(tools, "name", "test_header_parameters");
    ASSERT_FALSE(found.is_null());
    const json& properties = found.at("inputSchema").at("properties");
    EXPECT_EQ(properties.at("region").at("type"), "string");
    EXPECT_EQ(properties.at("region").at("x-mcp-header"), "Region");
    EXPECT_EQ(properties.at("priority").at("x-mcp-header"), "Priority");
    EXPECT_EQ(properties.at("verbose").at("x-mcp-header"), "Verbose");
    EXPECT_EQ(found.at("inputSchema").at("required"), json::array({"region"}));
    int marked = 0;
    for (const json& tool : tools) {
        const json& schema = tool.at("inputSchema");
        if (!schema.contains("properties")) continue;
        for (const auto& property : schema.at("properties").items()) {
            if (property.value().contains("x-mcp-header")) ++marked;
        }
    }
    EXPECT_EQ(marked, 3) << "no other tool carries a mark";
}

class ModernHeaders : public ::testing::Test {
protected:
    ModernHeaders() : client(fixture.port, modern_revision) {}

    // The call the scenario makes: the tool, the arguments with the string it varies, and the header it sends.
    json call_region(const std::string& body_value, const std::optional<std::string>& header_value) {
        httplib::Headers extra;
        if (header_value) extra.emplace("Mcp-Param-Region", *header_value);
        return client.call_with("tools/call", json{{"name", "test_header_parameters"}, {"arguments", json{{"region", body_value}}}},
                                extra);
    }

    // Whether the answer is the refusal the scenario wants: status 400 and the code of a header mismatch.
    void expect_rejected(const json& answer) {
        EXPECT_EQ(client.last_status(), 400);
        ASSERT_TRUE(answer.contains("error")) << answer.dump();
        EXPECT_EQ(answer.at("error").at("code"), -32020);
    }
    void expect_accepted(const json& answer) {
        EXPECT_EQ(client.last_status(), 200);
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_FALSE(failed(answer.at("result")));
    }

    Fixture fixture;
    Client client;
};

TEST_F(ModernHeaders, ABase64ValueIsDecodedAndMatchedAgainstTheBody) {
    expect_accepted(call_region("Hello", "=?base64?SGVsbG8=?="));
}

TEST_F(ModernHeaders, Base64WithoutItsPaddingIsRejected) {
    expect_rejected(call_region("Hello", "=?base64?SGVsbG8?="));
}

TEST_F(ModernHeaders, Base64WithCharactersOutsideTheAlphabetIsRejected) {
    expect_rejected(call_region("Hello", "=?base64?SGVs!!!bG8=?="));
}

TEST_F(ModernHeaders, AValueWithoutThePrefixOrTheSuffixIsTakenLiterally) {
    const std::string digits = "SGVsbG8=";
    expect_accepted(call_region(digits, digits));
    expect_accepted(call_region("=?base64?" + digits, "=?base64?" + digits));
    expect_rejected(call_region("Hello", digits));
}

TEST_F(ModernHeaders, AHeaderLeftOutWhileTheBodyHasTheValueIsRejected) {
    expect_rejected(call_region("test-value", std::nullopt));
}

TEST_F(ModernHeaders, AHeaderThatDiffersFromTheBodyIsRejected) {
    expect_rejected(call_region("us-west1", "us-east1"));
}

TEST_F(ModernHeaders, ThePlainValueMatchesAndOtherParametersAreMirroredToo) {
    httplib::Headers extra{{"Mcp-Param-Region", "us-west1"}, {"Mcp-Param-Priority", "42.0"}, {"Mcp-Param-Verbose", "true"}};
    json params{{"name", "test_header_parameters"},
                {"arguments", json{{"region", "us-west1"}, {"priority", 42}, {"verbose", true}}}};
    expect_accepted(client.call_with("tools/call", params, extra));
    extra = {{"Mcp-Param-Region", "us-west1"}, {"Mcp-Param-Priority", "43"}, {"Mcp-Param-Verbose", "true"}};
    expect_rejected(client.call_with("tools/call", params, extra));
    extra = {{"Mcp-Param-Region", "us-west1"}, {"Mcp-Param-Priority", "42"}};
    expect_rejected(client.call_with("tools/call", params, extra));
}

TEST_F(ModernHeaders, ACallWithoutTheOptionalParametersNeedsOnlyTheRegionHeader) {
    expect_accepted(call_region("us-west1", "us-west1"));
}

TEST(LegacyHeaders, ARevisionBeforeTheOnesThatDefineThemLooksAtNoParameterHeader) {
    Fixture fixture;
    Client client(fixture.port, legacy_revision);
    const json params{{"name", "test_header_parameters"}, {"arguments", json{{"region", "us-west1"}}}};
    json answer = client.call("tools/call", params);
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(client.last_status(), 200);
    EXPECT_FALSE(failed(answer.at("result")));
    answer = client.call_with("tools/call", params, httplib::Headers{{"Mcp-Param-Region", "somewhere-else"}});
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(client.last_status(), 200);
    EXPECT_FALSE(failed(answer.at("result")));
}

}  // namespace
