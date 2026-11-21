// Shared by the tests of OAuthAuthorizer: a scripted authorization server and a scripted MCP resource, each a scripted
// HTTP peer on a loopback port, and a person who signs in at once (the test's prompt handler).
//
// The authorization server serves its metadata, registers clients, and answers token requests for the codes it issued
// and the refresh tokens it holds; it records every request. The resource takes requests that carry an access token
// the authorization servers issued and that holds the scopes it requires, and refuses the rest: 401 with a challenge
// that names its protected resource metadata, or 403 for want of scope. The person reads the state and the scope from
// the authorization URL, asks the authorization server for a code, and hands back the redirect URI with both.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "client_auth/url.h"
#include "neomcp/client.h"
#include "neomcp/client/http.h"
#include "neomcp/client/oauth.h"
#include "neomcp/http/form.h"
#include "rig.h"
#include "scripted_http_server.h"

namespace oauth_rig {

using http_test::json;
using http_test::Request;
using http_test::ScriptedHttpServer;

inline constexpr std::string_view modern = "2026-07-28";
inline constexpr std::string_view redirect_uri = "http://127.0.0.1:3000/callback";
// What a client secret, a code and the text an authorization server adds to an error look like here: no error and no
// diagnostic may carry them.
inline constexpr std::string_view planted_secret = "PlantedClientSecret0123";
inline constexpr std::string_view planted_description = "PlantedDescription0123";

// The decoded fields of a form, empty when it does not decode.
inline std::vector<neomcp::http::FormField> form_of(std::string_view text) {
    auto decoded = neomcp::http::form_decode(text);
    if (auto* fields = std::get_if<std::vector<neomcp::http::FormField>>(&decoded)) return *fields;
    return {};
}

inline std::optional<std::string> field_of(std::string_view form, std::string_view name) {
    const auto fields = form_of(form);
    const auto lookup = neomcp::http::find_unique_field(fields, name);
    if (lookup.state != neomcp::http::FieldLookup::State::present) return std::nullopt;
    return std::string(lookup.value);
}

inline std::string query_of(std::string_view url) {
    const auto parts = neomcp::detail::oauth::read_url(url);
    return parts && parts->query ? std::string(*parts->query) : std::string();
}

// What the issued tokens are good for; shared by the authorization servers and the resource.
class Ledger {
public:
    void grant(const std::string& access_token, std::vector<std::string> scopes) {
        std::lock_guard lock(mutex_);
        valid_[access_token] = std::move(scopes);
    }
    // Every access token issued so far stops being good (they expired).
    void expire_all() {
        std::lock_guard lock(mutex_);
        valid_.clear();
    }
    std::optional<std::vector<std::string>> scopes_of(const std::string& access_token) const {
        std::lock_guard lock(mutex_);
        const auto found = valid_.find(access_token);
        if (found == valid_.end()) return std::nullopt;
        return found->second;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<std::string>> valid_;
};

inline std::vector<std::string> split_scopes(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ' ') {
            if (!current.empty()) out.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

// A reply of the test's own for a request, in place of a peer's script; nothing: the script answers.
using Override = std::function<std::optional<http_test::Reply>(const Request&)>;

class AuthServer {
public:
    // `name` makes its client ids and tokens its own.
    AuthServer(Ledger& ledger, std::string name)
        : ledger_(ledger), name_(std::move(name)), server_([this](const Request& request) { return serve(request); }) {}

    std::string issuer() const { return server_.url(""); }
    ScriptedHttpServer& server() { return server_; }
    const ScriptedHttpServer& server() const { return server_; }

    // The test answers the requests it chooses; called without the server's lock, so it may call back in.
    void override_with(Override chosen) {
        std::lock_guard lock(mutex_);
        override_ = std::move(chosen);
    }
    // Changes the metadata document after it is made.
    void edit_metadata(std::function<void(json&)> edit) {
        std::lock_guard lock(mutex_);
        edit_ = std::move(edit);
    }
    // The script's own answer to a request (for a peer of another origin that serves this server's endpoints).
    http_test::Reply answer(const Request& request) { return reply(request); }
    // The metadata document as served.
    json metadata_document() const {
        std::lock_guard lock(mutex_);
        return metadata();
    }
    // Every code, access token and refresh token issued so far.
    std::vector<std::string> issued() const {
        std::lock_guard lock(mutex_);
        return issued_;
    }
    // The access tokens issued so far.
    std::vector<std::string> access_tokens() const {
        std::lock_guard lock(mutex_);
        return access_issued_;
    }

    // What a token the server issued stands for, as a resource server would learn it from the server.
    struct Grant {
        std::vector<std::string> scopes;
        std::string audience;  // the `resource` the request named, or the audience the test chose
        std::string client_id;
        std::string subject;
        std::chrono::system_clock::time_point expires;
    };
    // The grant behind an access token this server issued.
    std::optional<Grant> grant_of(const std::string& access_token) const {
        std::lock_guard lock(mutex_);
        const auto found = grants_.find(access_token);
        if (found == grants_.end()) return std::nullopt;
        return found->second;
    }
    // The owner every token of this server is issued for.
    static constexpr std::string_view subject_name = "alice";
    // The seconds an issued access token lives (its `expires_in`, and the end the resource server learns).
    void token_lifetime(std::chrono::seconds lifetime) {
        std::lock_guard lock(mutex_);
        lifetime_ = lifetime;
    }
    // Only these scopes are ever granted: a request for others gets a token without them (the token answer does not say).
    void grant_only(std::vector<std::string> scopes) {
        std::lock_guard lock(mutex_);
        grantable_ = std::move(scopes);
    }
    // Every token is issued for this audience, whatever resource the request named.
    void issue_for_audience(std::string audience) {
        std::lock_guard lock(mutex_);
        audience_ = std::move(audience);
    }
    // The access tokens are the text this makes from the grant (a signed token, say), in place of "<name>-access-<n>".
    // Called with the server's lock held: it must not call this server.
    void mint_with(std::function<std::string(const Grant&)> mint) {
        std::lock_guard lock(mutex_);
        mint_ = std::move(mint);
    }
    // Requests the script does not know are offered to this first (after the test's override), without the server's
    // lock held: an introspection endpoint, a key set.
    void serve_extra(Override extra) {
        std::lock_guard lock(mutex_);
        extra_ = std::move(extra);
    }

    // Refresh tokens are issued with every token, and refresh requests answered with new tokens.
    void issue_refresh_tokens(bool on) {
        std::lock_guard lock(mutex_);
        refresh_ = on;
    }
    // The next token request is refused with this OAuth error (and a description no one may repeat).
    void refuse_next_token(std::string error) {
        std::lock_guard lock(mutex_);
        refuse_token_ = std::move(error);
    }
    // Registrations answer with a client secret.
    void register_with_secret(bool on) {
        std::lock_guard lock(mutex_);
        secret_ = on;
    }

    // The person's visit: a code for these scopes.
    std::string issue_code(std::vector<std::string> scopes) {
        std::lock_guard lock(mutex_);
        std::string code = name_ + "-code-" + std::to_string(++codes_issued_);
        codes_[code] = std::move(scopes);
        issued_.push_back(code);
        return code;
    }

    // The requests that arrived, by path.
    std::vector<Request> requests_to(std::string_view path) const {
        std::vector<Request> out;
        for (const Request& one : server_.requests()) {
            if (one.target == path) out.push_back(one);
        }
        return out;
    }
    std::string client_id() const { return name_ + "-client"; }

private:
    json metadata() const {
        const std::string base = issuer();
        json document{{"issuer", base},
                      {"authorization_endpoint", base + "/authorize"},
                      {"token_endpoint", base + "/token"},
                      {"registration_endpoint", base + "/register"},
                      {"response_types_supported", json::array({"code"})},
                      {"code_challenge_methods_supported", json::array({"S256"})},
                      {"token_endpoint_auth_methods_supported", json::array({"none", "client_secret_post"})}};
        if (edit_) edit_(document);
        return document;
    }

    http_test::Reply tokens_for(std::vector<std::string> scopes, const Request& request) {
        if (grantable_) {
            std::vector<std::string> kept;
            for (const std::string& one : scopes) {
                if (std::find(grantable_->begin(), grantable_->end(), one) != grantable_->end()) kept.push_back(one);
            }
            scopes = std::move(kept);
        }
        ++tokens_issued_;
        Grant grant;
        grant.scopes = scopes;
        grant.audience = audience_ ? *audience_ : field_of(request.body, "resource").value_or(std::string());
        grant.client_id = field_of(request.body, "client_id").value_or(std::string());
        grant.subject = std::string(subject_name);
        grant.expires = std::chrono::system_clock::now() + lifetime_;
        std::string access = mint_ ? mint_(grant) : name_ + "-access-" + std::to_string(tokens_issued_);
        json body{{"access_token", access},
                  {"token_type", "Bearer"},
                  {"expires_in", static_cast<long long>(lifetime_.count())}};
        issued_.push_back(access);
        access_issued_.push_back(access);
        grants_[access] = std::move(grant);
        if (refresh_) {
            std::string refresh = name_ + "-refresh-" + std::to_string(tokens_issued_);
            body["refresh_token"] = refresh;
            refresh_tokens_[refresh] = scopes;
            issued_.push_back(refresh);
        }
        ledger_.grant(access, std::move(scopes));
        return http_test::json_reply(200, body);
    }

    http_test::Reply serve(const Request& request) {
        Override chosen;
        {
            std::lock_guard lock(mutex_);
            chosen = override_;
        }
        if (chosen) {
            if (auto given = chosen(request)) return std::move(*given);
        }
        Override extra;
        {
            std::lock_guard lock(mutex_);
            extra = extra_;
        }
        if (extra) {
            if (auto given = extra(request)) return std::move(*given);
        }
        return reply(request);
    }

    http_test::Reply reply(const Request& request) {
        std::lock_guard lock(mutex_);
        if (request.method == "GET" && request.target == "/.well-known/oauth-authorization-server") {
            return http_test::json_reply(200, metadata());
        }
        if (request.method == "POST" && request.target == "/register") {
            json body{{"client_id", client_id()}, {"redirect_uris", json::array({std::string(redirect_uri)})}};
            if (secret_) {
                body["client_secret"] = std::string(planted_secret);
                body["token_endpoint_auth_method"] = "client_secret_post";
            }
            return http_test::json_reply(201, body);
        }
        if (request.method == "POST" && request.target == "/token") {
            if (refuse_token_) {
                json body{{"error", *refuse_token_}, {"error_description", std::string(planted_description)}};
                refuse_token_.reset();
                return http_test::json_reply(400, body);
            }
            const auto grant = field_of(request.body, "grant_type");
            if (grant == std::optional<std::string>("authorization_code")) {
                const auto code = field_of(request.body, "code");
                const auto found = code ? codes_.find(*code) : codes_.end();
                if (found == codes_.end()) return http_test::json_reply(400, json{{"error", "invalid_grant"}});
                std::vector<std::string> scopes = found->second;
                codes_.erase(found);
                return tokens_for(std::move(scopes), request);
            }
            if (grant == std::optional<std::string>("refresh_token")) {
                const auto refresh = field_of(request.body, "refresh_token");
                const auto found = refresh ? refresh_tokens_.find(*refresh) : refresh_tokens_.end();
                if (found == refresh_tokens_.end()) {
                    return http_test::json_reply(400, json{{"error", "invalid_grant"}});
                }
                std::vector<std::string> scopes = found->second;
                refresh_tokens_.erase(found);
                return tokens_for(std::move(scopes), request);
            }
            return http_test::json_reply(400, json{{"error", "unsupported_grant_type"}});
        }
        return http_test::empty_reply(404);
    }

    Ledger& ledger_;
    std::string name_;
    mutable std::mutex mutex_;
    Override override_;
    std::function<void(json&)> edit_;
    std::vector<std::string> issued_;
    std::vector<std::string> access_issued_;
    bool refresh_ = false;
    bool secret_ = false;
    std::optional<std::string> refuse_token_;
    std::size_t codes_issued_ = 0;
    std::size_t tokens_issued_ = 0;
    std::map<std::string, std::vector<std::string>> codes_;
    std::map<std::string, std::vector<std::string>> refresh_tokens_;
    std::map<std::string, Grant> grants_;
    std::chrono::seconds lifetime_{3600};
    std::optional<std::vector<std::string>> grantable_;
    std::optional<std::string> audience_;
    std::function<std::string(const Grant&)> mint_;
    Override extra_;
    ScriptedHttpServer server_;
};

inline json discover_result() {
    return json{{"supportedVersions", json::array({std::string(modern)})},
                {"capabilities", json::object()},
                {"resultType", "complete"},
                {"ttlMs", 0},
                {"cacheScope", "private"}};
}

// The answer to a handshake: the revision the client asked for.
inline json initialize_result(const Request& request) {
    const json message = request.message();
    json version = std::string(modern);
    if (message.is_object() && message.contains("params") && message["params"].is_object() &&
        message["params"].contains("protocolVersion")) {
        version = message["params"]["protocolVersion"];
    }
    return json{{"protocolVersion", version},
                {"capabilities", json::object()},
                {"serverInfo", {{"name", "oauth-resource"}, {"version", "1.0.0"}}}};
}

class Resource {
public:
    Resource(Ledger& ledger, const AuthServer& first)
        : ledger_(ledger), issuer_(first.issuer()), server_([this](const Request& request) { return serve(request); }) {}

    std::string url() const { return server_.url("/mcp"); }
    std::string origin() const { return server_.url(""); }
    std::string metadata_url() const { return server_.url("/.well-known/oauth-protected-resource/mcp"); }
    ScriptedHttpServer& server() { return server_; }
    const ScriptedHttpServer& server() const { return server_; }
    std::vector<Request> requests() const { return server_.requests(); }

    // The test answers the requests it chooses; called without the resource's lock.
    void override_with(Override chosen) {
        std::lock_guard lock(mutex_);
        override_ = std::move(chosen);
    }
    // Changes the protected resource metadata document after it is made.
    void edit_metadata(std::function<void(json&)> edit) {
        std::lock_guard lock(mutex_);
        edit_ = std::move(edit);
    }
    // The URL a challenge names as its resource_metadata; empty: the challenge names none.
    void challenge_names(std::string metadata) {
        std::lock_guard lock(mutex_);
        named_metadata_ = std::move(metadata);
    }

    // Its protected resource metadata names this authorization server from now on.
    void use(const AuthServer& named) {
        std::lock_guard lock(mutex_);
        issuer_ = named.issuer();
    }
    // A request is taken only with a token that holds every one of these scopes; otherwise 403.
    void require_scopes(std::vector<std::string> scopes) {
        std::lock_guard lock(mutex_);
        required_ = std::move(scopes);
    }
    // The scope a 401 challenge names; none when empty.
    void challenge_scope(std::string scope) {
        std::lock_guard lock(mutex_);
        challenge_scope_ = std::move(scope);
    }
    // Every request with a good token is refused for want of scope, whatever it holds.
    void refuse_for_scope_always(bool on) {
        std::lock_guard lock(mutex_);
        refuse_always_ = on;
    }

    // Every request with a good token is refused for want of a scope never asked for before.
    void escalate_scopes(bool on) {
        std::lock_guard lock(mutex_);
        escalate_ = on;
    }

    std::size_t refusals() const {
        std::lock_guard lock(mutex_);
        return refusals_;
    }
    bool wait_refusals(std::size_t count, std::chrono::milliseconds bound = http_rig::wait_bound) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return refusals_ >= count; });
    }
    // The Authorization values of the MCP requests, in order.
    std::vector<std::string> authorizations() const {
        std::vector<std::string> out;
        for (const Request& one : server_.requests()) {
            if (one.target == "/mcp") out.push_back(one.field("Authorization").value_or(std::string()));
        }
        return out;
    }

private:
    http_test::Reply refuse(int status, std::string challenge) {
        ++refusals_;
        changed_.notify_all();
        return http_test::empty_reply(status, {{"WWW-Authenticate", std::move(challenge)}});
    }

    http_test::Reply serve(const Request& request) {
        Override chosen;
        {
            std::lock_guard lock(mutex_);
            chosen = override_;
        }
        if (chosen) {
            if (auto given = chosen(request)) return std::move(*given);
        }
        return reply(request);
    }

    http_test::Reply reply(const Request& request) {
        std::lock_guard lock(mutex_);
        if (request.method == "GET" && request.target == "/.well-known/oauth-protected-resource/mcp") {
            json document{{"resource", url()}, {"authorization_servers", json::array({issuer_})}};
            if (edit_) edit_(document);
            return http_test::json_reply(200, document);
        }
        if (request.target != "/mcp") return http_test::empty_reply(404);
        if (request.method == "DELETE") return http_test::empty_reply(200);
        if (request.method == "GET") return http_test::empty_reply(405);
        const std::string value = request.field("Authorization").value_or(std::string());
        const std::string prefix = "Bearer ";
        const auto scopes =
            value.compare(0, prefix.size(), prefix) == 0 ? ledger_.scopes_of(value.substr(prefix.size())) : std::nullopt;
        if (!scopes) {
            const std::string named = named_metadata_ ? *named_metadata_ : metadata_url();
            std::string challenge = "Bearer";
            if (!named.empty()) challenge += " resource_metadata=\"" + named + "\"";
            if (!challenge_scope_.empty()) {
                challenge += named.empty() ? " " : ", ";
                challenge += "scope=\"" + challenge_scope_ + "\"";
            }
            return refuse(401, std::move(challenge));
        }
        std::string missing;
        for (const std::string& needed : required_) {
            if (std::find(scopes->begin(), scopes->end(), needed) == scopes->end()) {
                if (!missing.empty()) missing.push_back(' ');
                missing += needed;
            }
        }
        if (refuse_always_ && missing.empty()) {
            for (const std::string& needed : required_) {
                if (!missing.empty()) missing.push_back(' ');
                missing += needed;
            }
        }
        if (escalate_) missing = "mcp:s" + std::to_string(++escalations_);
        if (!missing.empty()) {
            return refuse(403, "Bearer error=\"insufficient_scope\", scope=\"" + missing + "\", resource_metadata=\"" +
                                   metadata_url() + "\"");
        }
        if (request.rpc_method() == "server/discover") {
            return http_test::json_reply(200, http_test::result_for(request, discover_result()));
        }
        if (request.rpc_method() == "initialize") {
            return http_test::json_reply(200, http_test::result_for(request, initialize_result(request)));
        }
        if (request.rpc_id().is_null()) return http_test::empty_reply(202);
        return http_test::json_reply(200, http_test::result_for(request, json{{"tools", json::array()}}));
    }

    Ledger& ledger_;
    std::string issuer_;
    mutable std::mutex mutex_;
    Override override_;
    std::function<void(json&)> edit_;
    std::optional<std::string> named_metadata_;
    mutable std::condition_variable changed_;
    std::vector<std::string> required_;
    std::string challenge_scope_;
    bool refuse_always_ = false;
    bool escalate_ = false;
    std::size_t escalations_ = 0;
    std::size_t refusals_ = 0;
    ScriptedHttpServer server_;
};

// The person at the browser: what each prompt showed, and how it is answered.
class Person {
public:
    enum class Mode {
        sign_in,           // signs in at once
        wait_until_stopped,  // never comes back; returns once stop is requested
        give_up            // comes back without signing in
    };

    explicit Person(AuthServer& server) : server_(&server) {}

    void sign_in_at(AuthServer& server) {
        std::lock_guard lock(mutex_);
        server_ = &server;
    }
    void set_mode(Mode mode) {
        std::lock_guard lock(mutex_);
        mode_ = mode;
    }
    // Waits (on the handler's thread) until this returns true before answering.
    void hold_until(std::function<bool()> ready) {
        std::lock_guard lock(mutex_);
        hold_ = std::move(ready);
    }
    // Replaces the callback URL the person hands back.
    void answer_with(std::function<std::string(const neomcp::AuthorizationPromptInfo&)> callback) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(callback);
    }

    // The plain handler's body.
    neomcp::AuthorizationPromptAnswer answer(const neomcp::AuthorizationPromptInfo& info,
                                             neomcp::AuthorizationPromptContext& context) {
        Mode mode;
        std::function<bool()> hold;
        std::function<std::string(const neomcp::AuthorizationPromptInfo&)> callback;
        AuthServer* server = nullptr;
        {
            std::lock_guard lock(mutex_);
            prompts_.push_back(info);
            threads_.push_back(std::this_thread::get_id());
            mode = mode_;
            hold = hold_;
            callback = callback_;
            server = server_;
            changed_.notify_all();
        }
        if (hold) {
            const auto until = std::chrono::steady_clock::now() + http_rig::wait_bound;
            while (!hold() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        neomcp::AuthorizationPromptAnswer given;
        if (mode == Mode::wait_until_stopped) {
            const auto until = std::chrono::steady_clock::now() + http_rig::wait_bound;
            while (!context.cancelled() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            note_stop(context.cancelled());
        } else if (mode == Mode::sign_in) {
            given.callback_url = callback ? callback(info) : callback_for(info, *server);
        }
        // Nothing of this object is touched after this point, so a test may end once it saw the count.
        note_returned();
        return given;
    }

    // Waits until `count` prompts have returned.
    bool wait_returned(std::size_t count, std::chrono::milliseconds bound = http_rig::wait_bound) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return returned_ >= count; });
    }

    // The redirect URI with a fresh code for the scopes asked for, and the state of the request.
    static std::string callback_for(const neomcp::AuthorizationPromptInfo& info, AuthServer& server) {
        const std::string query = query_of(info.authorization_url);
        const auto scope = field_of(query, "scope");
        const auto state = field_of(query, "state");
        std::vector<neomcp::http::FormField> fields;
        fields.push_back({"code", server.issue_code(scope ? split_scopes(*scope) : std::vector<std::string>{})});
        if (state) fields.push_back({"state", *state});
        return info.redirect_uri + "?" + neomcp::http::form_encode(fields);
    }

    neomcp::AuthorizationPrompt plain_prompt() {
        neomcp::AuthorizationPrompt prompt;
        prompt.handler([this](const neomcp::AuthorizationPromptInfo& info,
                              neomcp::AuthorizationPromptContext& context) { return answer(info, context); });
        return prompt;
    }

    std::vector<neomcp::AuthorizationPromptInfo> prompts() const {
        std::lock_guard lock(mutex_);
        return prompts_;
    }
    std::vector<std::thread::id> threads() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }
    bool wait_prompts(std::size_t count, std::chrono::milliseconds bound = http_rig::wait_bound) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return prompts_.size() >= count; });
    }
    // Whether a prompt that waited saw stop requested; waits for one to have ended.
    std::optional<bool> wait_stop_seen(std::chrono::milliseconds bound = http_rig::wait_bound) const {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, bound, [&] { return stop_seen_.has_value(); })) return std::nullopt;
        return stop_seen_;
    }

private:
    void note_stop(bool seen) {
        std::lock_guard lock(mutex_);
        stop_seen_ = seen;
        changed_.notify_all();
    }
    void note_returned() {
        std::lock_guard lock(mutex_);
        ++returned_;
        changed_.notify_all();
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::size_t returned_ = 0;
    AuthServer* server_;
    Mode mode_ = Mode::sign_in;
    std::function<bool()> hold_;
    std::function<std::string(const neomcp::AuthorizationPromptInfo&)> callback_;
    std::vector<neomcp::AuthorizationPromptInfo> prompts_;
    std::vector<std::thread::id> threads_;
    std::optional<bool> stop_seen_;
};

// One resource behind one authorization server, and the person who signs in there.
struct World {
    Ledger ledger;
    AuthServer as{ledger, "one"};
    Resource resource{ledger, as};
    Person person{as};
    std::shared_ptr<neomcp::MemoryTokenStore> store = std::make_shared<neomcp::MemoryTokenStore>();
    // The revisions the client enables; empty: every built-in one.
    std::vector<std::string> revisions;

    std::mutex diagnostics_mutex;
    std::vector<std::string> diagnostics;

    neomcp::OAuthOptions options() {
        neomcp::OAuthOptions made;
        made.redirect_uri = std::string(redirect_uri);
        made.prompt = person.plain_prompt();
        made.client_name = "oauth-authorizer-test";
        made.store = store;
        return made;
    }

    std::shared_ptr<neomcp::OAuthAuthorizer> authorizer(neomcp::OAuthOptions chosen) {
        auto made = neomcp::OAuthAuthorizer::create(std::move(chosen));
        EXPECT_TRUE(made) << made.error().message;
        return made ? *made : nullptr;
    }
    std::shared_ptr<neomcp::OAuthAuthorizer> authorizer() { return authorizer(options()); }

    neomcp::ClientOptions client_options() {
        neomcp::ClientOptions made;
        made.client_info.name = "oauth-authorizer-test";
        made.client_info.version = "1.0.0";
        for (const std::string& revision : revisions) made.profiles.push_back(neomcp::profile::builtin(revision));
        made.on_diagnostic = [this](const neomcp::ClientDiagnostic& reported) {
            if (const auto* plain = std::get_if<neomcp::Diagnostic>(&reported)) {
                std::lock_guard lock(diagnostics_mutex);
                diagnostics.push_back(plain->message);
            }
        };
        return made;
    }

    neomcp::HttpEndpoint endpoint(std::shared_ptr<neomcp::HttpAuthorizer> given) {
        auto described = http_rig::endpoint(resource.url());
        described.authorizer = std::move(given);
        return described;
    }

    neomcp::Result<neomcp::Client> open(std::shared_ptr<neomcp::HttpAuthorizer> given) {
        return neomcp::Client::open(client_options(), endpoint(std::move(given)));
    }

    std::vector<std::string> diagnostics_seen() {
        std::lock_guard lock(diagnostics_mutex);
        return diagnostics;
    }

    // Every secret of the sign-ins so far: what the authorization servers issued (codes, access and refresh tokens),
    // the client secret and the description the rig plants, the state of every prompt, and the verifier of every
    // token request.
    std::vector<std::string> secrets(std::initializer_list<const AuthServer*> others = {}) const {
        std::vector<std::string> out = {std::string(planted_secret), std::string(planted_description)};
        std::vector<const AuthServer*> servers = {&as};
        servers.insert(servers.end(), others.begin(), others.end());
        for (const AuthServer* server : servers) {
            for (const std::string& one : server->issued()) out.push_back(one);
            for (const Request& one : server->requests_to("/token")) {
                if (auto verifier = field_of(one.body, "code_verifier")) out.push_back(*verifier);
            }
        }
        for (const neomcp::AuthorizationPromptInfo& prompt : person.prompts()) {
            if (auto state = field_of(query_of(prompt.authorization_url), "state")) out.push_back(*state);
        }
        return out;
    }
};

// Fails the test when `text` holds any of `secrets`.
inline void expect_none_of(std::string_view text, const std::vector<std::string>& secrets, std::string_view where) {
    for (const std::string& secret : secrets) {
        if (secret.empty()) continue;
        EXPECT_EQ(text.find(secret), std::string_view::npos) << where << " holds " << secret;
    }
}

inline void expect_clean(const neomcp::ClientError& error, const std::vector<std::string>& secrets) {
    expect_none_of(error.message, secrets, "the message");
    if (error.authorization) {
        expect_none_of(error.authorization->issuer, secrets, "the issuer");
        expect_none_of(error.authorization->url, secrets, "the URL");
        EXPECT_EQ(error.authorization->url.find('?'), std::string::npos);
        if (error.authorization->error_code) expect_none_of(*error.authorization->error_code, secrets, "the code");
    }
}

// No error the test saw and no diagnostic the client reported holds any of `secrets`.
inline void expect_quiet(World& world, const std::vector<neomcp::ClientError>& errors,
                         const std::vector<std::string>& secrets) {
    for (const neomcp::ClientError& error : errors) expect_clean(error, secrets);
    for (const std::string& seen : world.diagnostics_seen()) expect_none_of(seen, secrets, "a diagnostic");
}

// No request that reached `server` holds any of `secrets` in its target (path and query).
inline void expect_no_secret_in_targets(const ScriptedHttpServer& server, const std::vector<std::string>& secrets) {
    for (const Request& one : server.requests()) expect_none_of(one.target, secrets, "a request target");
}

inline bool holds_any(std::string_view text, const std::vector<std::string>& parts) {
    for (const std::string& part : parts) {
        if (!part.empty() && text.find(part) != std::string_view::npos) return true;
    }
    return false;
}

// Where the secrets of a sign-in went, for every server the test ran: no request to an authorization server carries
// an access token or the protocol version of the resource; no request target anywhere carries a secret; the MCP
// endpoint never receives a code, a verifier, a refresh token or a client secret.
inline void expect_secrets_kept(World& world, std::initializer_list<const AuthServer*> others = {},
                         const std::vector<std::string>& more = {}) {
    std::vector<std::string> secrets = world.secrets(others);
    secrets.insert(secrets.end(), more.begin(), more.end());
    std::vector<const AuthServer*> servers = {&world.as};
    servers.insert(servers.end(), others.begin(), others.end());
    std::vector<std::string> access;
    for (const AuthServer* server : servers) {
        for (const std::string& one : server->access_tokens()) access.push_back(one);
    }
    for (const AuthServer* server : servers) {
        for (const Request& one : server->server().requests()) {
            EXPECT_FALSE(holds_any(one.head, access)) << "an access token reached " << one.target;
            EXPECT_FALSE(holds_any(one.body, access)) << "an access token reached " << one.target;
            EXPECT_FALSE(one.field("MCP-Protocol-Version")) << one.target;
            const auto value = one.field("Authorization");
            EXPECT_TRUE(!value || value->rfind("Bearer", 0) != 0) << one.target;
        }
        expect_no_secret_in_targets(server->server(), secrets);
    }
    std::vector<std::string> not_for_the_resource;
    for (const std::string& one : secrets) {
        if (std::find(access.begin(), access.end(), one) == access.end()) not_for_the_resource.push_back(one);
    }
    for (const Request& one : world.resource.requests()) {
        if (one.target != "/mcp") continue;  // a document or an endpoint of the authorization server hosted there
        EXPECT_FALSE(holds_any(one.head, not_for_the_resource)) << one.target;
        EXPECT_FALSE(holds_any(one.body, not_for_the_resource)) << one.target;
    }
    expect_no_secret_in_targets(world.resource.server(), secrets);
    // The URL the person opens carries its own state and nothing else secret: no verifier, token or client secret.
    for (const neomcp::AuthorizationPromptInfo& prompt : world.person.prompts()) {
        const auto own_state = field_of(query_of(prompt.authorization_url), "state");
        for (const std::string& one : secrets) {
            if (one.empty() || (own_state && one == *own_state)) continue;
            EXPECT_EQ(prompt.authorization_url.find(one), std::string::npos) << "the authorization URL holds " << one;
        }
    }
    expect_quiet(world, {}, secrets);
}

inline std::size_t grants_of(const AuthServer& server, std::string_view grant) {
    std::size_t count = 0;
    for (const Request& one : server.requests_to("/token")) {
        if (field_of(one.body, "grant_type") == std::optional<std::string>(std::string(grant))) ++count;
    }
    return count;
}

}  // namespace oauth_rig
