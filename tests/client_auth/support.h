// Scripted peers for the OAuth flow without sockets: a URL policy, stored credentials in maps, a fixed random
// source, and a world of one MCP resource and one or more authorization servers that answers each request the flow
// describes, with documents shaped like those of the conformance suite's servers.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "client_auth/flow.h"
#include "client_auth/scopes.h"
#include "client_auth/tokens.h"
#include "client_auth/url.h"
#include "neomcp/http/form.h"
#include "neomcp/json.h"

namespace oauth_test {

using namespace neomcp::detail::oauth;
using neomcp::json;

// Values a failure, a diagnostic or a request to the wrong place must never carry.
inline constexpr std::string_view planted_access_token = "PlantedAccessToken0123";
inline constexpr std::string_view planted_refresh_token = "PlantedRefreshToken0123";
inline constexpr std::string_view planted_code = "PlantedCode0123";
inline constexpr std::string_view planted_client_secret = "PlantedClientSecret0123";

// The policy of the transport as the flow expects it: http or https, https or loopback http, and a loopback host
// only when the endpoint is on one; and any URL listed in `refused`.
class TestAdmission final : public UrlAdmission {
public:
    explicit TestAdmission(std::string_view endpoint) {
        const auto url = read_url(endpoint);
        endpoint_loopback_ = url && is_loopback(*url);
    }
    std::optional<std::string> admit(UrlRole role, std::string_view text) const override {
        asked.emplace_back(role, std::string(text));
        const auto url = read_url(text);
        if (!url) return std::string("unreadable");
        if (!scheme_is(*url, scheme_https) && !scheme_is(*url, scheme_http)) return std::string("scheme");
        if (!is_secure_or_loopback(*url)) return std::string("plain http");
        if (is_loopback(*url) && !endpoint_loopback_) return std::string("loopback");
        if (refused.count(std::string(text)) != 0) return std::string("listed");
        return std::nullopt;
    }
    std::set<std::string> refused;
    mutable std::vector<std::pair<UrlRole, std::string>> asked;

private:
    bool endpoint_loopback_ = false;
};

class MapLookup final : public CredentialLookup {
public:
    std::optional<StoredTokens> tokens(const CredentialKey& key) const override {
        const auto found = tokens_by_key.find(key);
        if (found == tokens_by_key.end()) return std::nullopt;
        return found->second;
    }
    std::optional<StoredClient> client(const std::string& issuer) const override {
        const auto found = clients.find(issuer);
        if (found == clients.end()) return std::nullopt;
        return found->second;
    }
    void apply(const StoreUpdate& update) {
        for (const auto& key : update.erase_tokens) tokens_by_key.erase(key);
        for (const auto& issuer : update.erase_clients) clients.erase(issuer);
        if (update.put_client) clients[update.put_client->first] = update.put_client->second;
        if (update.put_tokens) tokens_by_key[update.put_tokens->first] = update.put_tokens->second;
    }
    std::map<CredentialKey, StoredTokens> tokens_by_key;
    std::map<std::string, StoredClient> clients;
};

// Octets 0, 1, 2, ... continuing across calls; `fail` makes the source fail.
struct CountingRandom {
    unsigned char next = 0;
    bool fail = false;
    std::size_t calls = 0;
    RandomSource source() {
        return [this](std::size_t count) -> std::optional<std::string> {
            ++calls;
            if (fail) return std::nullopt;
            std::string out;
            for (std::size_t i = 0; i < count; ++i) out.push_back(static_cast<char>(next++));
            return out;
        };
    }
};

inline FetchResult answer(int status, std::string body = {}, std::string content_type = "application/json") {
    FetchResult result;
    result.outcome = FetchOutcome::response;
    result.status = status;
    result.content_type = std::move(content_type);
    result.body = std::move(body);
    return result;
}
inline FetchResult json_answer(const json& body, int status = 200) { return answer(status, body.dump()); }
inline FetchResult not_found() { return answer(404, "Not Found", "text/plain"); }
inline FetchResult no_answer(FetchOutcome outcome) {
    FetchResult result;
    result.outcome = outcome;
    return result;
}

// The fields of a form-encoded text; the test fails when it does not decode.
inline std::vector<neomcp::http::FormField> form_fields(std::string_view text) {
    auto decoded = neomcp::http::form_decode(text);
    auto* fields = std::get_if<std::vector<neomcp::http::FormField>>(&decoded);
    EXPECT_NE(fields, nullptr);
    return fields != nullptr ? *fields : std::vector<neomcp::http::FormField>{};
}
inline std::optional<std::string> form_value(std::string_view text, std::string_view name) {
    const auto fields = form_fields(text);
    const auto lookup = neomcp::http::find_unique_field(fields, name);
    if (lookup.state != neomcp::http::FieldLookup::State::present) return std::nullopt;
    return std::string(lookup.value);
}
inline std::string query_of(std::string_view url) {
    const auto parts = read_url(url);
    return parts && parts->query ? std::string(*parts->query) : std::string();
}
inline std::optional<std::string> header_value(const FlowFetch& fetch, std::string_view name) {
    for (const auto& line : fetch.headers) {
        if (line.name == name) return line.value;
    }
    return std::nullopt;
}

// A failure never carries a planted secret, the code, the state or the verifier, or a query string.
inline void expect_clean(const FlowFailure& failure, const std::vector<std::string>& also = {}) {
    std::vector<std::string> secrets = {std::string(planted_access_token), std::string(planted_refresh_token),
                                        std::string(planted_code), std::string(planted_client_secret)};
    secrets.insert(secrets.end(), also.begin(), also.end());
    for (const std::string& secret : secrets) {
        if (secret.empty()) continue;
        EXPECT_EQ(failure.issuer.find(secret), std::string::npos) << secret;
        EXPECT_EQ(failure.url.find(secret), std::string::npos) << secret;
        if (failure.error_code) {
            EXPECT_EQ(failure.error_code->find(secret), std::string::npos) << secret;
        }
    }
    EXPECT_EQ(failure.url.find('?'), std::string::npos);
    EXPECT_EQ(failure.url.find('#'), std::string::npos);
}

// One MCP resource and its authorization servers, answering like the conformance suite's servers: the resource on
// http://localhost:3001, the authorization server on http://localhost:3002, the redirect URI of the conformance
// client, an authorization endpoint that redirects at once with code, state and (by default) no iss.
struct World {
    std::string endpoint = "http://localhost:3001/mcp";
    std::string issuer = "http://localhost:3002";
    std::string redirect_uri = "http://127.0.0.1:3000/callback";

    std::map<std::string, FetchResult> documents;  // GET answers by URL; anything else is 404
    FetchResult registration = json_answer(json{{"client_id", "test-client-id"},
                                                {"token_endpoint_auth_method", "none"},
                                                {"redirect_uris", json::array({"http://127.0.0.1:3000/callback"})}},
                                           201);
    std::map<std::string, FetchResult> registrations_by_url;  // when set for a URL, used instead of `registration`
    std::vector<FetchResult> token_answers{json_answer(
        json{{"access_token", std::string(planted_access_token)}, {"token_type", "Bearer"}, {"expires_in", 3600}})};
    std::size_t token_answer_index = 0;

    // The iss the authorization endpoint adds to its redirect; none when not set.
    std::optional<std::string> callback_iss;
    // Replaces the redirect the authorization endpoint makes, when set.
    std::function<PromptAnswer(const PromptRequest&)> prompt;
    // Extra raw text appended to the redirect's query.
    std::string callback_extra;

    std::vector<FlowFetch> requests;
    std::vector<PromptRequest> prompts;

    World() { use_default_documents(); }

    std::string resource_metadata_path_url() const {
        return "http://localhost:3001/.well-known/oauth-protected-resource/mcp";
    }
    std::string resource_metadata_root_url() const {
        return "http://localhost:3001/.well-known/oauth-protected-resource";
    }
    std::string server_metadata_url() const { return issuer + "/.well-known/oauth-authorization-server"; }

    json resource_document() const {
        return json{{"resource", endpoint}, {"authorization_servers", json::array({issuer})}};
    }
    json server_document() const {
        return json{{"issuer", issuer},
                    {"authorization_endpoint", issuer + "/authorize"},
                    {"token_endpoint", issuer + "/token"},
                    {"registration_endpoint", issuer + "/register"},
                    {"response_types_supported", json::array({"code"})},
                    {"grant_types_supported", json::array({"authorization_code", "refresh_token"})},
                    {"code_challenge_methods_supported", json::array({"S256"})},
                    {"token_endpoint_auth_methods_supported", json::array({"none"})}};
    }
    void use_default_documents() {
        documents.clear();
        documents[resource_metadata_path_url()] = json_answer(resource_document());
        documents[server_metadata_url()] = json_answer(server_document());
    }

    FetchResult respond(const FlowFetch& fetch) {
        requests.push_back(fetch);
        if (fetch.method == "GET") {
            const auto found = documents.find(fetch.url);
            return found == documents.end() ? not_found() : found->second;
        }
        if (fetch.purpose == FetchPurpose::registration) {
            const auto found = registrations_by_url.find(fetch.url);
            return found == registrations_by_url.end() ? registration : found->second;
        }
        if (fetch.purpose == FetchPurpose::token) {
            if (token_answers.empty()) return not_found();
            const std::size_t at = std::min(token_answer_index, token_answers.size() - 1);
            ++token_answer_index;
            return token_answers[at];
        }
        return not_found();
    }

    PromptAnswer redirect(const PromptRequest& request) {
        prompts.push_back(request);
        if (prompt) return prompt(request);
        PromptAnswer out;
        out.kind = PromptAnswer::Kind::callback;
        const auto state = form_value(query_of(request.authorization_url), "state");
        std::vector<neomcp::http::FormField> fields;
        fields.push_back({"code", std::string(planted_code)});
        if (state) fields.push_back({"state", *state});
        if (callback_iss) fields.push_back({"iss", *callback_iss});
        out.callback_url = request.redirect_uri + "?" + neomcp::http::form_encode(fields) + callback_extra;
        return out;
    }

    std::vector<const FlowFetch*> requests_for(FetchPurpose purpose) const {
        std::vector<const FlowFetch*> out;
        for (const auto& one : requests) {
            if (one.purpose == purpose) out.push_back(&one);
        }
        return out;
    }
    bool requested(std::string_view url) const {
        for (const auto& one : requests) {
            if (one.url == url) return true;
        }
        return false;
    }
};

inline Trigger unauthorized(std::vector<std::string> challenges = {}, bool carried_token = false) {
    Trigger trigger;
    trigger.status = 401;
    trigger.challenges = std::move(challenges);
    trigger.carried_token = carried_token;
    trigger.operation = "POST tools/call test-tool";
    return trigger;
}

inline Trigger insufficient_scope(std::string scope) {
    Trigger trigger;
    trigger.status = 403;
    trigger.challenges = {"Bearer error=\"insufficient_scope\", scope=\"" + scope + "\""};
    trigger.carried_token = true;
    trigger.operation = "POST tools/call test-tool";
    return trigger;
}

inline FlowClock::time_point test_time() { return FlowClock::time_point(std::chrono::hours(24 * 365 * 50)); }

// A flow and everything it needs, driven to its end against a World.
struct Rig {
    World world;
    FlowConfig config;
    std::optional<TestAdmission> admission;
    MapLookup lookup;
    CountingRandom random;
    FlowMemory memory;
    std::vector<FlowDiagnostic> diagnostics;

    Rig() {
        config.endpoint_url = world.endpoint;
        config.registration.redirect_uri = world.redirect_uri;
        config.registration.client_name = "test client";
    }

    // Starts a flow and answers every step until it ends; the store update of the end is applied.
    FlowStep run(const Trigger& trigger, FlowClock::time_point now = test_time()) {
        if (!admission) admission.emplace(config.endpoint_url);
        OAuthFlow flow(config, *admission, lookup, random.source(), memory);
        FlowStep step = flow.start(trigger, now);
        for (int i = 0; i < 64; ++i) {
            if (auto* fetch = std::get_if<FlowFetch>(&step)) {
                const FetchResult result = world.respond(*fetch);
                step = flow.fetched(fetch->id, result, now);
                continue;
            }
            if (auto* prompt = std::get_if<PromptRequest>(&step)) {
                const PromptAnswer reply = world.redirect(*prompt);
                step = flow.prompted(reply, now);
                continue;
            }
            break;
        }
        auto noticed = flow.take_diagnostics();
        diagnostics.insert(diagnostics.end(), noticed.begin(), noticed.end());
        if (auto* done = std::get_if<FlowDone>(&step)) lookup.apply(done->updates);
        if (auto* failed = std::get_if<FlowFailed>(&step)) lookup.apply(failed->updates);
        return step;
    }
};

inline const FlowDone* done_of(const FlowStep& step) {
    const auto* done = std::get_if<FlowDone>(&step);
    if (done == nullptr) {
        const auto* failed = std::get_if<FlowFailed>(&step);
        ADD_FAILURE() << "the flow did not succeed"
                      << (failed != nullptr ? std::string(": ") + std::string(to_string(failed->failure.problem))
                                            : std::string());
    }
    return done;
}

inline std::optional<AuthorizationProblem> problem_of(const FlowStep& step) {
    const auto* failed = std::get_if<FlowFailed>(&step);
    if (failed == nullptr) {
        ADD_FAILURE() << "the flow did not fail";
        return std::nullopt;
    }
    expect_clean(failed->failure);
    return failed->failure.problem;
}

}  // namespace oauth_test
