// read_request and write_answer: the inverses of write_params and read_answer, for every revision and for both
// mechanisms of the slot `server_input`. The tests reach the internal header of the core.
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "core/input.h"
#include "neomcp/mechanism.h"
#include "neomcp/wire.h"
#include "support.h"

namespace {

using namespace neomcp;
using client_core_test::Fill;
using client_core_test::Generator;
using client_core_test::same;
using detail::method_of;
using detail::read_answer;
using detail::read_request;
using detail::write_answer;
using detail::write_params;

constexpr std::string_view elicit = "elicitation/create";
constexpr std::string_view sampling = "sampling/createMessage";
constexpr std::string_view roots = "roots/list";
constexpr std::array<std::string_view, 3> methods{elicit, sampling, roots};
constexpr std::array<std::string_view, 2> mechanism_names{"server_request", "input_required_result"};
constexpr std::size_t elicitation_id_digits = 32;  // 16 random bytes in hex

std::vector<profile::ProfilePtr> all_profiles() {
    std::vector<profile::ProfilePtr> out;
    for (const std::string_view revision : profile::builtin_revisions()) out.push_back(profile::builtin(revision));
    return out;
}

std::unique_ptr<InputMechanism> make_mechanism(std::string_view name) {
    auto mechanism = builtin_mechanisms().make_input(name);
    EXPECT_TRUE(mechanism) << name;
    return mechanism;
}

bool starts_with(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }

InputResponse make_answer(Generator& g, const profile::Profile& profile, std::string_view method);

// ---- Requests

const profile::Type* params_type_of(const profile::Profile& profile, const InputMechanism& mechanism,
                                    std::string_view method) {
    const auto name = mechanism.params_type(profile, method);
    return name ? profile.type(*name) : nullptr;
}

bool allows(const profile::Type* type, std::string_view member) { return type == nullptr || type->has_property(member); }

InputRequest make_request(Generator& g, const profile::Profile& profile, const InputMechanism& mechanism,
                          std::string_view method) {
    if (method == roots) return RootsRequest{};
    if (method == elicit) {
        if (profile.type("ElicitRequestURLParams") != nullptr && g.random().chance()) {
            ElicitationUrl url;
            url.message = g.text();
            url.url = g.text();
            if (g.some()) url.elicitation_id = g.text();
            return url;
        }
        ElicitationForm form;
        form.message = g.text();
        form.requested_schema = g.object();
        return form;
    }
    SamplingRequest request;
    const profile::Type* type = params_type_of(profile, mechanism, sampling);
    const bool arrays = profile.union_type("SamplingMessageContentBlock") != nullptr;
    const std::array<ContentKind, 3> kinds{ContentKind::text, ContentKind::image, ContentKind::audio};
    for (std::size_t i = 0, n = 1 + g.random().below(3); i < n; ++i) {
        SamplingMessage message;
        message.role = g.random().chance() ? Role::user : Role::assistant;
        for (std::size_t j = 0, blocks = arrays ? 1 + g.random().below(3) : 1; j < blocks; ++j) {
            ContentKind kind = kinds[g.random().below(kinds.size())];
            if (!has_content_kind(kind, profile)) kind = ContentKind::text;
            message.content.push_back(g.block(kind));
        }
        request.messages.push_back(std::move(message));
    }
    request.max_tokens = g.count_value();
    if (g.some()) request.system_prompt = g.text();
    if (g.some()) {
        ModelPreferences preferences;
        for (std::size_t i = 0, n = g.items(3); i < n; ++i) preferences.hints.push_back(g.text());
        if (g.some()) preferences.cost_priority = g.real();
        if (g.some()) preferences.speed_priority = g.real();
        if (g.some()) preferences.intelligence_priority = g.real();
        request.model_preferences = preferences;
    }
    if (g.some()) request.temperature = g.real();
    for (std::size_t i = 0, n = g.items(3); i < n; ++i) request.stop_sequences.push_back(g.text());
    if (g.some()) {
        const std::array<IncludeContext, 3> contexts{IncludeContext::none, IncludeContext::this_server,
                                                     IncludeContext::all_servers};
        request.include_context = contexts[g.random().below(contexts.size())];
    }
    if (g.some()) request.metadata = g.object();
    if (type != nullptr && type->has_property("tools")) {
        Generator tools(profile, g.random().next(), Fill::any, true);
        for (std::size_t i = 0, n = g.items(2); i < n; ++i) request.tools.push_back(tools.tool());
    }
    if (type != nullptr && type->has_property("toolChoice") && g.some()) request.tool_choice = g.object();
    return request;
}

bool same_preferences(const std::optional<ModelPreferences>& a, const std::optional<ModelPreferences>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return same(a->hints, b->hints) && same(a->cost_priority, b->cost_priority) &&
           same(a->speed_priority, b->speed_priority) && same(a->intelligence_priority, b->intelligence_priority);
}

bool same_request(const InputRequest& a, const InputRequest& b) {
    if (a.index() != b.index()) return false;
    if (const auto* form = std::get_if<ElicitationForm>(&a)) {
        const auto& other = std::get<ElicitationForm>(b);
        return same(form->message, other.message) && same(form->requested_schema, other.requested_schema);
    }
    if (const auto* url = std::get_if<ElicitationUrl>(&a)) {
        const auto& other = std::get<ElicitationUrl>(b);
        return same(url->message, other.message) && same(url->url, other.url) &&
               same(url->elicitation_id, other.elicitation_id);
    }
    if (const auto* request = std::get_if<SamplingRequest>(&a)) {
        const auto& other = std::get<SamplingRequest>(b);
        if (request->messages.size() != other.messages.size()) return false;
        for (std::size_t i = 0; i < request->messages.size(); ++i) {
            if (!same(request->messages[i].role, other.messages[i].role) ||
                !same(request->messages[i].content, other.messages[i].content)) {
                return false;
            }
        }
        return same(request->max_tokens, other.max_tokens) && same(request->system_prompt, other.system_prompt) &&
               same_preferences(request->model_preferences, other.model_preferences) &&
               same(request->temperature, other.temperature) && same(request->stop_sequences, other.stop_sequences) &&
               same(request->include_context, other.include_context) && same(request->metadata, other.metadata) &&
               same(request->tools, other.tools) && same(request->tool_choice, other.tool_choice);
    }
    return true;  // roots
}

bool is_hex_id(const std::string& text) {
    if (text.size() != elicitation_id_digits) return false;
    for (const char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

std::vector<std::pair<Fill, std::uint64_t>> request_sources() {
    std::vector<std::pair<Fill, std::uint64_t>> out{{Fill::none, 1}, {Fill::all, 2}};
    for (std::uint64_t i = 0; i < 80; ++i) out.emplace_back(Fill::any, 500 + i);
    return out;
}

TEST(ReadRequest, EveryRequestAServerWritesIsReadBack) {
    std::size_t round_trips = 0;
    for (const auto& profile : all_profiles()) {
        for (const std::string_view mechanism_name : mechanism_names) {
            const auto mechanism = make_mechanism(mechanism_name);
            ASSERT_TRUE(mechanism);
            for (const std::string_view method : methods) {
                SCOPED_TRACE(profile->revision() + " " + std::string(mechanism_name) + " " + std::string(method));
                if (!mechanism->carries(*profile, method)) continue;
                for (const auto& [fill, seed] : request_sources()) {
                    Generator generator(*profile, seed, fill, true);
                    const InputRequest original = make_request(generator, *profile, *mechanism, method);
                    EXPECT_EQ(method_of(original), method);
                    const auto written = write_params(original, *profile, *mechanism);
                    const json* params = std::get_if<json>(&written);
                    ASSERT_NE(params, nullptr) << std::get<std::string>(written);
                    for (const json& sent : {*params, json::parse(params->dump())}) {
                        auto read = read_request(method, sent, *profile, *mechanism);
                        const InputRequest* value = std::get_if<InputRequest>(&read);
                        ASSERT_NE(value, nullptr) << std::get<std::string>(read) << "\nparams: " << params->dump();
                        InputRequest expected = original;
                        if (auto* url = std::get_if<ElicitationUrl>(&expected)) {
                            const auto* type = profile->type("ElicitRequestURLParams");
                            const auto& got = std::get<ElicitationUrl>(*value);
                            if (type == nullptr || !type->has_property("elicitationId")) {
                                url->elicitation_id.reset();  // the revision's request has no id
                            } else if (!url->elicitation_id) {
                                // The session made one: 16 random bytes in hex.
                                ASSERT_TRUE(got.elicitation_id.has_value());
                                EXPECT_TRUE(is_hex_id(*got.elicitation_id)) << *got.elicitation_id;
                                url->elicitation_id = got.elicitation_id;
                            }
                        }
                        EXPECT_TRUE(same_request(*value, expected)) << "params: " << params->dump();
                        // Written again, it is the same params.
                        const auto again = write_params(*value, *profile, *mechanism);
                        ASSERT_TRUE(std::holds_alternative<json>(again));
                        const bool made_id = std::holds_alternative<ElicitationUrl>(original) &&
                                             !std::get<ElicitationUrl>(original).elicitation_id &&
                                             allows(profile->type("ElicitRequestURLParams"), "elicitationId");
                        if (!made_id) EXPECT_EQ(std::get<json>(again), *params);
                        ++round_trips;
                    }
                }
            }
        }
    }
    EXPECT_GT(round_trips, 0U);
}

TEST(ReadRequest, ARequestTheRevisionDoesNotCarryIsAnError) {
    for (const auto& profile : all_profiles()) {
        for (const std::string_view mechanism_name : mechanism_names) {
            const auto mechanism = make_mechanism(mechanism_name);
            for (const std::string_view method : methods) {
                if (mechanism->carries(*profile, method)) continue;
                SCOPED_TRACE(profile->revision() + " " + std::string(mechanism_name) + " " + std::string(method));
                auto read = read_request(method, json::object(), *profile, *mechanism);
                EXPECT_TRUE(std::holds_alternative<std::string>(read));
                Generator generator(*profile, 5, Fill::all, true);
                EXPECT_TRUE(std::holds_alternative<std::string>(
                    write_answer(make_answer(generator, *profile, method), *profile, *mechanism)));
            }
        }
    }
    const auto modern = profile::builtin(profile::builtin_revisions().back());
    const auto mechanism = make_mechanism(modern->mechanism(profile::Slot::server_input));
    auto unknown = read_request("tools/list", json::object(), *modern, *mechanism);
    EXPECT_TRUE(std::holds_alternative<std::string>(unknown));
}

// Whether the revision's elicitation has modes: its form params have the member `mode`.
bool defines_modes(const profile::Profile& profile) {
    const profile::Type* form = profile.type("ElicitRequestFormParams");
    return form != nullptr && form->has_property("mode");
}

TEST(ReadRequest, ARequestWithoutAModeIsAForm) {
    for (const auto& profile : all_profiles()) {
        const auto mechanism = make_mechanism(profile->mechanism(profile::Slot::server_input));
        if (!mechanism->carries(*profile, elicit)) continue;
        SCOPED_TRACE(profile->revision());
        auto read = read_request(elicit, json{{"message", "m"}, {"requestedSchema", json::object()}}, *profile, *mechanism);
        ASSERT_TRUE(std::holds_alternative<InputRequest>(read)) << std::get<std::string>(read);
        EXPECT_TRUE(std::holds_alternative<ElicitationForm>(std::get<InputRequest>(read)));
    }
}

TEST(ReadRequest, AModeTheRevisionDoesNotDefineIsAnError) {
    for (const auto& profile : all_profiles()) {
        const auto mechanism = make_mechanism(profile->mechanism(profile::Slot::server_input));
        if (!mechanism->carries(*profile, elicit)) continue;
        SCOPED_TRACE(profile->revision());
        const json form{{"message", "m"}, {"requestedSchema", json::object()}};
        json with_form_mode = form;
        with_form_mode["mode"] = "form";
        json with_url_mode{{"message", "m"}, {"mode", "url"}, {"url", "https://example.test/"}, {"elicitationId", "e"}};
        json with_other_mode = form;
        with_other_mode["mode"] = "video";
        json with_number_mode = form;
        with_number_mode["mode"] = 5;
        const bool modes = defines_modes(*profile);
        const bool url = profile->type("ElicitRequestURLParams") != nullptr;
        EXPECT_EQ(std::holds_alternative<InputRequest>(read_request(elicit, with_form_mode, *profile, *mechanism)), modes);
        EXPECT_EQ(std::holds_alternative<InputRequest>(read_request(elicit, with_url_mode, *profile, *mechanism)), modes && url);
        EXPECT_TRUE(std::holds_alternative<std::string>(read_request(elicit, with_other_mode, *profile, *mechanism)));
        EXPECT_TRUE(std::holds_alternative<std::string>(read_request(elicit, with_number_mode, *profile, *mechanism)));
    }
}

TEST(ReadRequest, WhatTheRevisionRequiresMustBeThere) {
    for (const auto& profile : all_profiles()) {
        for (const std::string_view mechanism_name : mechanism_names) {
            const auto mechanism = make_mechanism(mechanism_name);
            for (const std::string_view method : {elicit, sampling}) {
                if (!mechanism->carries(*profile, method)) continue;
                SCOPED_TRACE(profile->revision() + " " + std::string(mechanism_name) + " " + std::string(method));
                for (std::uint64_t seed = 9; seed < 17; ++seed) {
                Generator generator(*profile, seed, Fill::all, true);
                const InputRequest request = make_request(generator, *profile, *mechanism, method);
                const json params = std::get<json>(write_params(request, *profile, *mechanism));
                // The members the written params carry that the revision's params type requires.
                std::string type_name = method == sampling ? "CreateMessageRequestParams" : "ElicitRequestFormParams";
                if (std::holds_alternative<ElicitationUrl>(request)) type_name = "ElicitRequestURLParams";
                const profile::Type* type = profile->type(type_name);
                std::vector<std::string> required;
                if (type != nullptr) {
                    required = type->required;
                } else {
                    const auto fallback = mechanism->params_required(*profile, method);
                    required.assign(fallback.begin(), fallback.end());
                }
                ASSERT_FALSE(required.empty());
                for (const std::string& name : required) {
                    // Without its mode a request is a form: what it then lacks is a form's.
                    if (name == "mode") continue;
                    json lacking = params;
                    lacking.get_ptr<json::object_t*>()->erase(name);
                    auto read = read_request(method, lacking, *profile, *mechanism);
                    ASSERT_TRUE(std::holds_alternative<std::string>(read)) << "without " << name;
                    EXPECT_NE(std::get<std::string>(read).find(name), std::string::npos) << std::get<std::string>(read);
                }
                }
            }
        }
    }
}

TEST(ReadRequest, MembersOfTheWrongKindAreErrors) {
    const auto modern = profile::builtin(profile::builtin_revisions().back());
    const auto mechanism = make_mechanism(modern->mechanism(profile::Slot::server_input));
    const json form{{"message", "m"}, {"requestedSchema", json::object()}};
    for (const char* member : {"message", "requestedSchema"}) {
        json broken = form;
        broken[member] = 5;
        auto read = read_request(elicit, broken, *modern, *mechanism);
        ASSERT_TRUE(std::holds_alternative<std::string>(read)) << member;
        EXPECT_NE(std::get<std::string>(read).find(member), std::string::npos);
    }
    const json message{{"role", "user"}, {"content", json{{"type", "text"}, {"text", "t"}}}};
    const json sampling_params{{"messages", json::array({message})}, {"maxTokens", 5}};
    ASSERT_TRUE(std::holds_alternative<InputRequest>(read_request(sampling, sampling_params, *modern, *mechanism)));
    struct Case {
        const char* member;
        json value;
    };
    const std::vector<Case> wrong{{"messages", json("x")},        {"maxTokens", json("5")},       {"maxTokens", json(-1)},
                                  {"maxTokens", json(1.5)},       {"systemPrompt", json(1)},      {"temperature", json("hot")},
                                  {"stopSequences", json("x")},   {"stopSequences", json::array({1})},
                                  {"includeContext", json("everything")}, {"includeContext", json(3)},
                                  {"metadata", json("x")},        {"modelPreferences", json(1)},
                                  {"modelPreferences", json{{"hints", json("x")}}},
                                  {"modelPreferences", json{{"hints", json::array({1})}}},
                                  {"modelPreferences", json{{"costPriority", json("x")}}},
                                  {"tools", json("x")},           {"tools", json::array({json(5)})},
                                  {"toolChoice", json("x")}};
    for (const Case& broken : wrong) {
        json params = sampling_params;
        params[broken.member] = broken.value;
        auto read = read_request(sampling, params, *modern, *mechanism);
        ASSERT_TRUE(std::holds_alternative<std::string>(read)) << broken.member << " " << broken.value.dump();
    }
    for (const json& params : {json(5), json("x"), json::array()}) {
        EXPECT_TRUE(std::holds_alternative<std::string>(read_request(sampling, params, *modern, *mechanism)));
        EXPECT_TRUE(std::holds_alternative<std::string>(read_request(elicit, params, *modern, *mechanism)));
    }
    EXPECT_TRUE(std::holds_alternative<std::string>(read_request(roots, json(5), *modern, *mechanism)));
    EXPECT_TRUE(std::holds_alternative<InputRequest>(read_request(roots, json(nullptr), *modern, *mechanism)));
    EXPECT_TRUE(std::holds_alternative<InputRequest>(read_request(roots, json::object(), *modern, *mechanism)));
}

TEST(ReadRequest, SamplingMessagesHoldTheBlocksSamplingCarries) {
    for (const auto& profile : all_profiles()) {
        const auto mechanism = make_mechanism(profile->mechanism(profile::Slot::server_input));
        if (!mechanism->carries(*profile, sampling)) continue;
        SCOPED_TRACE(profile->revision());
        const bool arrays = profile->union_type("SamplingMessageContentBlock") != nullptr;
        const json text{{"type", "text"}, {"text", "t"}};
        const json link{{"type", "resource_link"}, {"uri", "file:///a"}, {"name", "a"}};
        const json unknown{{"type", "tool_use"}, {"id", "x"}, {"name", "n"}, {"input", json::object()}};
        auto request = [&](const json& content) {
            const json message{{"role", "user"}, {"content", content}};
            return read_request(sampling, json{{"messages", json::array({message})}, {"maxTokens", 1}}, *profile, *mechanism);
        };
        EXPECT_TRUE(std::holds_alternative<InputRequest>(request(text)));
        EXPECT_EQ(std::holds_alternative<InputRequest>(request(json::array({text, text}))), arrays);
        EXPECT_TRUE(std::holds_alternative<std::string>(request(link)));
        EXPECT_TRUE(std::holds_alternative<std::string>(request(unknown)));
        EXPECT_TRUE(std::holds_alternative<std::string>(request(json::array())));
        const auto mistake = request(link);
        EXPECT_TRUE(starts_with(std::get<std::string>(mistake), "messages[0].content")) << std::get<std::string>(mistake);
    }
}

TEST(ReadRequest, TheUrlOfAUrlRequestIsKeptAsGiven) {
    // Nothing is fetched and nothing is checked of the address: the client's application decides.
    for (const auto& profile : all_profiles()) {
        const auto mechanism = make_mechanism(profile->mechanism(profile::Slot::server_input));
        if (!mechanism->carries(*profile, elicit) || profile->type("ElicitRequestURLParams") == nullptr) continue;
        json params{{"mode", "url"}, {"message", "m"}, {"url", "javascript:alert(1)"}};
        if (allows(profile->type("ElicitRequestURLParams"), "elicitationId")) params["elicitationId"] = "e-1";
        auto read = read_request(elicit, params, *profile, *mechanism);
        ASSERT_TRUE(std::holds_alternative<InputRequest>(read)) << std::get<std::string>(read);
        EXPECT_EQ(std::get<ElicitationUrl>(std::get<InputRequest>(read)).url, "javascript:alert(1)");
    }
}

// ---- Answers

InputResponse make_answer(Generator& g, const profile::Profile& profile, std::string_view method) {
    if (method == roots) {
        RootsAnswer answer;
        for (std::size_t i = 0, n = g.items(3); i < n; ++i) {
            Root root;
            root.uri = g.text();
            if (g.some()) root.name = g.text();
            if (g.some()) root.meta = g.object();
            answer.roots.push_back(std::move(root));
        }
        return answer;
    }
    if (method == elicit) {
        ElicitationAnswer answer;
        const std::array<ElicitationAction, 3> actions{ElicitationAction::accept, ElicitationAction::decline,
                                                       ElicitationAction::cancel};
        answer.action = actions[g.random().below(actions.size())];
        if (answer.action == ElicitationAction::accept) {
            for (std::size_t i = 0, n = g.items(4); i < n; ++i) {
                const std::string key = g.text() + std::to_string(i);
                switch (g.random().below(4)) {
                    case 0: answer.content[key] = g.text(); break;
                    case 1: answer.content[key] = g.real(); break;
                    case 2: answer.content[key] = g.random().chance(); break;
                    default: {
                        json list = json::array();
                        for (std::size_t j = 0, m = g.items(3); j < m; ++j) list.push_back(g.text());
                        answer.content[key] = list;
                    }
                }
            }
        }
        return answer;
    }
    SamplingAnswer answer;
    answer.role = g.random().chance() ? Role::user : Role::assistant;
    const std::array<ContentKind, 3> kinds{ContentKind::text, ContentKind::image, ContentKind::audio};
    json blocks = json::array();
    for (std::size_t i = 0, n = 1 + g.random().below(2); i < n; ++i) {
        ContentKind kind = kinds[g.random().below(kinds.size())];
        if (!has_content_kind(kind, profile)) kind = ContentKind::text;
        blocks.push_back(to_wire(g.block(kind), profile));
    }
    answer.content = blocks.size() == 1 && g.random().chance() ? blocks[0] : blocks;
    answer.model = g.text();
    if (g.some()) answer.stop_reason = g.text();
    return answer;
}

bool same_answer(const InputResponse& a, const InputResponse& b, const profile::Profile& profile) {
    if (a.index() != b.index()) return false;
    if (const auto* elicitation = std::get_if<ElicitationAnswer>(&a)) {
        const auto& other = std::get<ElicitationAnswer>(b);
        return elicitation->action == other.action && same(elicitation->content, other.content);
    }
    if (const auto* answer = std::get_if<SamplingAnswer>(&a)) {
        const auto& other = std::get<SamplingAnswer>(b);
        return answer->role == other.role && same(answer->content, other.content) && answer->model == other.model &&
               same(answer->stop_reason, other.stop_reason);
    }
    const auto& list = std::get<RootsAnswer>(a).roots;
    const auto& other = std::get<RootsAnswer>(b).roots;
    if (list.size() != other.size()) return false;
    const bool has_meta = allows(profile.type("Root"), "_meta");
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].uri != other[i].uri || !same(list[i].name, other[i].name)) return false;
        if (!same(has_meta ? list[i].meta : std::nullopt, other[i].meta)) return false;
    }
    return true;
}

TEST(WriteAnswer, EveryAnswerAClientWritesIsReadBack) {
    std::size_t round_trips = 0;
    for (const auto& profile : all_profiles()) {
        for (const std::string_view mechanism_name : mechanism_names) {
            const auto mechanism = make_mechanism(mechanism_name);
            for (const std::string_view method : methods) {
                SCOPED_TRACE(profile->revision() + " " + std::string(mechanism_name) + " " + std::string(method));
                if (!mechanism->carries(*profile, method)) continue;
                for (const auto& [fill, seed] : request_sources()) {
                    Generator generator(*profile, seed, fill, true);
                    const InputResponse original = make_answer(generator, *profile, method);
                    const auto written = write_answer(original, *profile, *mechanism);
                    const json* result = std::get_if<json>(&written);
                    ASSERT_NE(result, nullptr) << std::get<std::string>(written);
                    for (const json& sent : {*result, json::parse(result->dump())}) {
                        auto read = read_answer(method, sent);
                        const InputResponse* value = std::get_if<InputResponse>(&read);
                        ASSERT_NE(value, nullptr) << std::get<std::string>(read) << "\nresult: " << result->dump();
                        EXPECT_TRUE(same_answer(original, *value, *profile)) << "result: " << result->dump();
                        // Written again, it is the same result.
                        const auto again = write_answer(*value, *profile, *mechanism);
                        ASSERT_TRUE(std::holds_alternative<json>(again));
                        EXPECT_EQ(std::get<json>(again), *result);
                        // The result says whose answer it is, from its members alone.
                        EXPECT_EQ(detail::answer_method(sent, *profile, *mechanism), std::optional<std::string_view>(method))
                            << "result: " << result->dump();
                        ++round_trips;
                    }
                }
            }
        }
    }
    EXPECT_GT(round_trips, 0U);
}

TEST(WriteAnswer, AResultAsAClientWritesItIsWrittenAsItWas) {
    // The other direction: results written by hand, read and written again.
    for (const auto& profile : all_profiles()) {
        const auto mechanism = make_mechanism(profile->mechanism(profile::Slot::server_input));
        SCOPED_TRACE(profile->revision());
        struct Case {
            std::string_view method;
            json result;
        };
        std::vector<Case> cases{
            {sampling, json{{"role", "assistant"}, {"content", json{{"type", "text"}, {"text", "hi"}}}, {"model", "m"}, {"stopReason", "endTurn"}}},
            {sampling, json{{"role", "user"}, {"content", json::array({json{{"type", "text"}, {"text", "a"}}, json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}}})}, {"model", "m"}}},
            {elicit, json{{"action", "accept"}, {"content", json{{"a", "b"}, {"n", 3}, {"ok", true}, {"l", json::array({"x", "y"})}}}}},
            {elicit, json{{"action", "decline"}}},
            {elicit, json{{"action", "cancel"}}},
            {roots, json{{"roots", json::array({json{{"uri", "file:///a"}}, json{{"uri", "file:///b"}, {"name", "b"}}})}}},
        };
        for (const Case& item : cases) {
            if (!mechanism->carries(*profile, item.method)) continue;
            auto read = read_answer(item.method, item.result);
            ASSERT_TRUE(std::holds_alternative<InputResponse>(read)) << std::get<std::string>(read);
            auto written = write_answer(std::get<InputResponse>(read), *profile, *mechanism);
            ASSERT_TRUE(std::holds_alternative<json>(written)) << std::get<std::string>(written);
            EXPECT_EQ(std::get<json>(written), item.result);
        }
    }
}

TEST(WriteAnswer, TheActionIsTheOneAnswered) {
    for (const auto& profile : all_profiles()) {
        const auto mechanism = make_mechanism(profile->mechanism(profile::Slot::server_input));
        if (!mechanism->carries(*profile, elicit)) continue;
        SCOPED_TRACE(profile->revision());
        for (const auto& [action, text] : {std::pair{ElicitationAction::accept, "accept"},
                                           std::pair{ElicitationAction::decline, "decline"},
                                           std::pair{ElicitationAction::cancel, "cancel"}}) {
            ElicitationAnswer answer;
            answer.action = action;
            auto written = write_answer(InputResponse(answer), *profile, *mechanism);
            ASSERT_TRUE(std::holds_alternative<json>(written));
            EXPECT_EQ(std::get<json>(written).at("action"), text);
        }
        // Fields belong to an acceptance.
        ElicitationAnswer declined;
        declined.action = ElicitationAction::decline;
        declined.content = json{{"a", "b"}};
        EXPECT_FALSE(std::get<json>(write_answer(InputResponse(declined), *profile, *mechanism)).contains("content"));
    }
}

TEST(WriteAnswer, WhatTheResultTypeOfTheRevisionHasIsWritten) {
    // Roots: `_meta` is a member of a root where the revision's Root type has it. ElicitResult and ListRootsResult:
    // only the members of the revision's type.
    for (const auto& profile : all_profiles()) {
        const auto mechanism = make_mechanism(profile->mechanism(profile::Slot::server_input));
        SCOPED_TRACE(profile->revision());
        if (mechanism->carries(*profile, roots)) {
            Root root;
            root.uri = "file:///a";
            root.meta = json{{"k", 1}};
            RootsAnswer answer;
            answer.roots.push_back(root);
            const json result = std::get<json>(write_answer(InputResponse(answer), *profile, *mechanism));
            EXPECT_EQ(result.at("roots")[0].contains("_meta"), allows(profile->type("Root"), "_meta"));
            EXPECT_EQ(result.contains("_meta"), false);
        }
    }
}

TEST(WriteAnswer, AnswersThatCannotBeWrittenAreErrors) {
    const auto modern = profile::builtin(profile::builtin_revisions().back());
    const auto mechanism = make_mechanism(modern->mechanism(profile::Slot::server_input));
    ElicitationAnswer bad_content;
    bad_content.action = ElicitationAction::accept;
    bad_content.content = json{{"a", json::object()}};
    EXPECT_TRUE(std::holds_alternative<std::string>(write_answer(InputResponse(bad_content), *modern, *mechanism)));
    bad_content.content = json::array();
    EXPECT_TRUE(std::holds_alternative<std::string>(write_answer(InputResponse(bad_content), *modern, *mechanism)));
    SamplingAnswer bad_sampling;
    bad_sampling.content = json("text");
    EXPECT_TRUE(std::holds_alternative<std::string>(write_answer(InputResponse(bad_sampling), *modern, *mechanism)));
    bad_sampling.content = json::array();
    EXPECT_TRUE(std::holds_alternative<std::string>(write_answer(InputResponse(bad_sampling), *modern, *mechanism)));
    Root bad_root;
    bad_root.uri = "file:///a";
    bad_root.meta = json(5);
    RootsAnswer roots_answer;
    roots_answer.roots.push_back(bad_root);
    EXPECT_TRUE(std::holds_alternative<std::string>(write_answer(InputResponse(roots_answer), *modern, *mechanism)));
}

}  // namespace
