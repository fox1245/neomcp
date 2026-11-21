// The OAuth flow without I/O (flow.h).
#include "client_auth/flow.h"

#include <algorithm>
#include <utility>

#include "client_auth/document.h"
#include "client_auth/http_names.h"
#include "client_auth/scopes.h"
#include "client_auth/tokens.h"
#include "client_auth/url.h"
#include "neomcp/http/challenge.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view error_invalid_client = "invalid_client";

}  // namespace

std::optional<profile::OriginMetadataDiscovery> origin_metadata_of(std::span<const profile::ProfilePtr> profiles) {
    for (const profile::ProfilePtr& one : profiles) {
        if (one && one->authorization().origin_metadata) return one->authorization().origin_metadata;
    }
    return std::nullopt;
}

OAuthFlow::OAuthFlow(const FlowConfig& config, const UrlAdmission& admission, const CredentialLookup& lookup,
                     RandomSource random, FlowMemory& memory)
    : config_(config), admission_(admission), lookup_(lookup), random_(std::move(random)), memory_(memory) {}

std::vector<FlowDiagnostic> OAuthFlow::take_diagnostics() { return std::exchange(diagnostics_, {}); }

// ---- Ends and requests

FlowStep OAuthFlow::fail(AuthorizationProblem problem, std::string_view url) {
    FlowFailure failure;
    failure.problem = problem;
    failure.issuer = server_.issuer;
    failure.url = without_query(url);
    return fail(std::move(failure));
}

FlowStep OAuthFlow::fail(FlowFailure failure) {
    stage_ = Stage::finished;
    waiting_id_ = 0;
    return FlowFailed{std::move(failure), std::move(updates_)};
}

FlowStep OAuthFlow::fetch(FlowFetch request, Stage stage) {
    request.id = next_id_++;
    waiting_id_ = request.id;
    stage_ = stage;
    return request;
}

FlowStep OAuthFlow::metadata_fetch(FetchPurpose purpose, const std::string& url, Stage stage) {
    FlowFetch request;
    request.purpose = purpose;
    request.method = std::string(method_get);
    request.url = url;
    request.headers.push_back({std::string(header_accept), std::string(media_type_json)});
    const auto target = read_url(url);
    const auto endpoint = read_url(config_.endpoint_url);
    if (target && endpoint && same_origin(*target, *endpoint)) {
        for (const HeaderLine& line : trigger_.resource_headers) request.headers.push_back(line);
    }
    request.body_bound = config_.bounds.max_metadata_bytes;
    request.follow_redirects = true;
    request.max_redirects = config_.bounds.max_redirects;
    request.timeout = config_.bounds.authorization_fetch_timeout;
    return fetch(std::move(request), stage);
}

bool OAuthFlow::expired(FlowClock::time_point now) const {
    return now - started_ > config_.bounds.authorization_timeout;
}

// ---- The stored credentials as this flow's own changes leave them

std::optional<StoredTokens> OAuthFlow::stored_tokens(const CredentialKey& key) const {
    if (updates_.put_tokens && updates_.put_tokens->first == key) return updates_.put_tokens->second;
    if (std::find(updates_.erase_tokens.begin(), updates_.erase_tokens.end(), key) != updates_.erase_tokens.end()) {
        return std::nullopt;
    }
    return lookup_.tokens(key);
}

std::optional<StoredClient> OAuthFlow::stored_client(const std::string& issuer) const {
    if (updates_.put_client && updates_.put_client->first == issuer) return updates_.put_client->second;
    if (std::find(updates_.erase_clients.begin(), updates_.erase_clients.end(), issuer) !=
        updates_.erase_clients.end()) {
        return std::nullopt;
    }
    return lookup_.client(issuer);
}

void OAuthFlow::drop_client(const std::string& issuer) {
    if (updates_.put_client && updates_.put_client->first == issuer) updates_.put_client.reset();
    if (std::find(updates_.erase_clients.begin(), updates_.erase_clients.end(), issuer) ==
        updates_.erase_clients.end()) {
        updates_.erase_clients.push_back(issuer);
    }
}

// ---- Inputs

FlowStep OAuthFlow::start(const Trigger& trigger, FlowClock::time_point now) {
    if (stage_ != Stage::idle) return FlowIgnored{};
    started_ = now;
    now_ = now;
    trigger_ = trigger;
    const OAuthBounds& bounds = config_.bounds;

    const auto endpoint = read_url(config_.endpoint_url);
    if (!endpoint) return fail(AuthorizationProblem::discovery_failed, config_.endpoint_url);
    if (!is_secure_or_loopback(*endpoint)) return fail(AuthorizationProblem::insecure_endpoint, config_.endpoint_url);
    if (trigger.refusals >= bounds.max_authorizations_per_request) {
        return fail(AuthorizationProblem::attempts_exceeded, config_.endpoint_url);
    }

    // The Bearer challenge, when one can be read; a field that cannot be read, or a Bearer challenge that cannot be
    // used, counts as no challenge.
    std::optional<neomcp::http::BearerChallenge> bearer;
    neomcp::http::ChallengeLimits limits;
    limits.max_params = bounds.max_challenge_params;
    auto parsed = neomcp::http::parse_challenges(std::span<const std::string>(trigger.challenges), limits);
    if (auto* challenges = std::get_if<std::vector<neomcp::http::Challenge>>(&parsed)) {
        auto found = neomcp::http::find_bearer_challenge(*challenges);
        if (auto* challenge = std::get_if<neomcp::http::BearerChallenge>(&found)) bearer = std::move(*challenge);
    }
    if (bearer && bearer->scope) {
        challenge_named_scope_ = true;
        challenge_scopes_ = bounded_scopes(bearer->scopes(), bounds, diagnostics_);
    }

    if (trigger.status == status_forbidden) {
        if (!bearer || bearer->error_kind != neomcp::http::BearerError::insufficient_scope) {
            return fail(AuthorizationProblem::forbidden, config_.endpoint_url);
        }
        step_up_ = true;
        // A step-up asks for more than the last authorization did, or it is not made.
        if (memory_.active) {
            const auto tokens = lookup_.tokens(*memory_.active);
            const std::vector<std::string> last = tokens ? tokens->requested_scopes : std::vector<std::string>{};
            if (!is_strict_superset(scope_union(last, challenge_scopes_), last)) {
                return fail(AuthorizationProblem::insufficient_scope, config_.endpoint_url);
            }
        }
    } else if (trigger.status != status_unauthorized) {
        return fail(AuthorizationProblem::forbidden, config_.endpoint_url);
    }

    std::size_t& attempts = memory_.attempts[trigger.operation];
    if (attempts > bounds.max_step_ups_per_operation) {
        return fail(AuthorizationProblem::attempts_exceeded, config_.endpoint_url);
    }
    ++attempts;

    auto candidates = protected_resource_candidates(
        config_.endpoint_url, bearer ? bearer->resource_metadata : std::optional<std::string>{}, admission_);
    if (auto* failure = std::get_if<FlowFailure>(&candidates)) return fail(std::move(*failure));
    resource_candidates_ = std::get<std::vector<PrmCandidate>>(std::move(candidates));
    resource_index_ = 0;
    return metadata_fetch(FetchPurpose::protected_resource_metadata, resource_candidates_.front().url,
                          Stage::resource_metadata);
}

FlowStep OAuthFlow::fetched(std::uint64_t id, const FetchResult& result, FlowClock::time_point now) {
    if (stage_ == Stage::idle || stage_ == Stage::finished || stage_ == Stage::prompt || id == 0 ||
        id != waiting_id_) {
        return FlowIgnored{};
    }
    waiting_id_ = 0;
    now_ = now;
    if (expired(now)) return fail(AuthorizationProblem::timed_out, config_.endpoint_url);
    switch (stage_) {
        case Stage::resource_metadata: return on_resource_metadata(result);
        case Stage::server_metadata: return on_server_metadata(result);
        case Stage::refresh: return on_refresh(result);
        case Stage::registration: return on_registration(result);
        case Stage::token: return on_token(result);
        case Stage::idle:
        case Stage::prompt:
        case Stage::finished: break;
    }
    return FlowIgnored{};
}

FlowStep OAuthFlow::prompted(const PromptAnswer& answer, FlowClock::time_point now) {
    if (stage_ != Stage::prompt) return FlowIgnored{};
    now_ = now;
    if (expired(now)) return fail(AuthorizationProblem::timed_out, server_.authorization_endpoint);
    switch (answer.kind) {
        case PromptAnswer::Kind::cancelled:
            return fail(AuthorizationProblem::prompt_cancelled, config_.registration.redirect_uri);
        case PromptAnswer::Kind::timed_out:
            return fail(AuthorizationProblem::prompt_timed_out, config_.registration.redirect_uri);
        case PromptAnswer::Kind::callback: break;
    }
    auto checked = check_callback(answer.callback_url, pending_, config_.bounds);
    if (auto* failure = std::get_if<FlowFailure>(&checked)) return fail(std::move(*failure));
    const std::string& code = std::get<std::string>(checked);
    FlowFetch request = authorization_code_request(server_.token_endpoint, client_, code,
                                                   config_.registration.redirect_uri, pkce_.verifier,
                                                   resource_.resource, config_.bounds);
    return fetch(std::move(request), Stage::token);
}

// ---- Protected resource metadata

FlowStep OAuthFlow::on_resource_metadata(const FetchResult& result) {
    const PrmCandidate& candidate = resource_candidates_[resource_index_];
    MetadataAnswer answer = read_metadata_answer(result, config_.bounds.max_metadata_bytes,
                                                 config_.bounds.max_document_depth);
    switch (answer.kind) {
        case MetadataAnswer::Kind::failure: return fail(answer.problem, candidate.url);
        case MetadataAnswer::Kind::next:
            if (++resource_index_ < resource_candidates_.size()) {
                return metadata_fetch(FetchPurpose::protected_resource_metadata,
                                      resource_candidates_[resource_index_].url, Stage::resource_metadata);
            }
            return no_resource_metadata();
        case MetadataAnswer::Kind::document: break;
    }
    auto outcome = read_protected_resource(answer.document, candidate, config_.accept_resource, admission_,
                                        config_.bounds, diagnostics_);
    if (auto* failure = std::get_if<FlowFailure>(&outcome)) return fail(std::move(*failure));
    resource_ = std::get<ProtectedResource>(std::move(outcome));
    return after_resource_metadata();
}

FlowStep OAuthFlow::no_resource_metadata() {
    // Every candidate answered with a client error. Discovery on the server's origin (config_.origin_metadata) would
    // start here; it is not supported yet, so the flow ends the same way with or without it.
    return fail(AuthorizationProblem::discovery_failed, resource_candidates_.back().url);
}

FlowStep OAuthFlow::after_resource_metadata() {
    key_ = CredentialKey{resource_.resource, resource_.authorization_server};
    if (memory_.active && *memory_.active != key_) {
        // Tokens are never sent to a resource or used with an authorization server other than the ones that issued
        // them, and a registration at one authorization server is not used at another.
        changed_ = true;
        updates_.erase_tokens.push_back(*memory_.active);
        if (memory_.active->issuer != key_.issuer) drop_client(memory_.active->issuer);
        memory_.active.reset();
    }
    // Tokens kept for this resource and authorization server (by an earlier run of the application, through its
    // store) serve a request that went without one: they are sent, and only their refusal leads to a refresh or a new
    // authorization.
    if (!step_up_ && !trigger_.carried_token && stored_tokens(key_)) {
        memory_.active = key_;
        stage_ = Stage::finished;
        return FlowDone{key_, std::move(updates_), false};
    }
    if (const auto known = memory_.servers.find(key_.issuer); known != memory_.servers.end()) {
        server_ = known->second;
        return after_server_metadata();
    }
    auto candidates = authorization_server_candidates(key_.issuer, admission_);
    if (auto* failure = std::get_if<FlowFailure>(&candidates)) return fail(std::move(*failure));
    server_candidates_ = std::get<std::vector<std::string>>(std::move(candidates));
    server_index_ = 0;
    return metadata_fetch(FetchPurpose::authorization_server_metadata, server_candidates_.front(),
                          Stage::server_metadata);
}

// ---- Authorization server metadata

FlowStep OAuthFlow::on_server_metadata(const FetchResult& result) {
    const std::string& url = server_candidates_[server_index_];
    MetadataAnswer answer = read_metadata_answer(result, config_.bounds.max_metadata_bytes,
                                                 config_.bounds.max_document_depth);
    switch (answer.kind) {
        case MetadataAnswer::Kind::failure: {
            FlowFailure failure;
            failure.problem = answer.problem;
            failure.issuer = key_.issuer;
            failure.url = without_query(url);
            return fail(std::move(failure));
        }
        case MetadataAnswer::Kind::next:
            if (++server_index_ < server_candidates_.size()) {
                return metadata_fetch(FetchPurpose::authorization_server_metadata, server_candidates_[server_index_],
                                      Stage::server_metadata);
            }
            {
                FlowFailure failure;
                failure.problem = AuthorizationProblem::discovery_failed;
                failure.issuer = key_.issuer;
                failure.url = without_query(url);
                return fail(std::move(failure));
            }
        case MetadataAnswer::Kind::document: break;
    }
    auto outcome = read_authorization_server(answer.document, key_.issuer, url, admission_, config_.bounds,
                                          diagnostics_);
    if (auto* failure = std::get_if<FlowFailure>(&outcome)) return fail(std::move(*failure));
    server_ = std::get<AuthorizationServer>(std::move(outcome));
    memory_.servers[server_.issuer] = server_;
    return after_server_metadata();
}

FlowStep OAuthFlow::after_server_metadata() {
    if (!step_up_ && !changed_ && trigger_.carried_token &&
        refresh_attempts_ < OAuthDefaults::refresh_attempts_per_flow) {
        auto tokens = stored_tokens(key_);
        if (tokens && tokens->refresh_token) {
            auto choice = choose_registration(server_, config_.registration, stored_client(server_.issuer));
            if (auto* client = std::get_if<ClientCredentials>(&choice)) {
                ++refresh_attempts_;
                client_ = std::move(*client);
                refreshed_from_ = std::move(tokens);
                FlowFetch request = refresh_token_request(server_.token_endpoint, client_,
                                                          *refreshed_from_->refresh_token, resource_.resource,
                                                          config_.bounds);
                return fetch(std::move(request), Stage::refresh);
            }
        }
    }
    return register_client();
}

FlowStep OAuthFlow::on_refresh(const FetchResult& result) {
    auto outcome = read_token_answer(result, server_.issuer, server_.token_endpoint, refreshed_from_->requested_scopes,
                                  now_, config_.bounds, diagnostics_);
    if (auto* tokens = std::get_if<StoredTokens>(&outcome)) {
        // A new refresh token replaces the old one; without one the old one stays valid.
        if (!tokens->refresh_token) tokens->refresh_token = refreshed_from_->refresh_token;
        updates_.put_tokens = std::make_pair(key_, std::move(*tokens));
        memory_.active = key_;
        stage_ = Stage::finished;
        return FlowDone{key_, std::move(updates_), true};
    }
    const FlowFailure& failure = std::get<FlowFailure>(outcome);
    const bool refused = result.outcome == FetchOutcome::response && is_client_error_status(result.status) &&
                         failure.problem == AuthorizationProblem::token_request_failed;
    if (!refused) return fail(std::get<FlowFailure>(std::move(outcome)));
    // The refresh token is no longer good: it is erased with the tokens, and the full flow follows.
    updates_.erase_tokens.push_back(key_);
    if (failure.error_code == error_invalid_client &&
        (client_.source == RegistrationSource::dynamic_stored || client_.source == RegistrationSource::dynamic_new)) {
        drop_client(server_.issuer);
    }
    return register_client();
}

// ---- Registration

FlowStep OAuthFlow::register_client() {
    auto choice = choose_registration(server_, config_.registration, stored_client(server_.issuer));
    if (auto* failure = std::get_if<FlowFailure>(&choice)) return fail(std::move(*failure));
    if (auto* client = std::get_if<ClientCredentials>(&choice)) {
        client_ = std::move(*client);
        return authorize();
    }
    FlowFetch request;
    request.purpose = FetchPurpose::registration;
    request.method = std::string(method_post);
    request.url = *server_.registration_endpoint;
    request.headers.push_back({std::string(header_content_type), std::string(media_type_json)});
    request.headers.push_back({std::string(header_accept), std::string(media_type_json)});
    request.body = registration_request_body(config_.registration);
    request.body_bound = config_.bounds.max_token_response_bytes;
    request.follow_redirects = false;
    request.max_redirects = 0;
    request.timeout = config_.bounds.authorization_fetch_timeout;
    return fetch(std::move(request), Stage::registration);
}

FlowStep OAuthFlow::on_registration(const FetchResult& result) {
    auto outcome = read_registration_answer(result, server_.issuer, *server_.registration_endpoint,
                                            config_.registration.redirect_uri, config_.bounds);
    if (auto* failure = std::get_if<FlowFailure>(&outcome)) return fail(std::move(*failure));
    StoredClient registered = std::get<StoredClient>(std::move(outcome));
    client_ = credentials_of(registered, server_, RegistrationSource::dynamic_new);
    updates_.put_client = std::make_pair(server_.issuer, std::move(registered));
    return authorize();
}

// ---- Authorization

FlowStep OAuthFlow::authorize() {
    if (step_up_) {
        const auto tokens = changed_ ? std::optional<StoredTokens>{} : stored_tokens(key_);
        const std::vector<std::string> last = tokens ? tokens->requested_scopes : std::vector<std::string>{};
        scopes_ = scope_union(last, challenge_scopes_);
        if (!last.empty() && !is_strict_superset(scopes_, last)) {
            return fail(AuthorizationProblem::insufficient_scope, config_.endpoint_url);
        }
    } else if (challenge_named_scope_ && !challenge_scopes_.empty()) {
        scopes_ = challenge_scopes_;
    } else {
        scopes_ = resource_.scopes_supported;
    }
    if (config_.request_offline_access && contains_scope(server_.scopes_supported, offline_access_scope) &&
        !contains_scope(scopes_, offline_access_scope)) {
        scopes_.emplace_back(offline_access_scope);
    }

    auto pkce = make_pkce(random_);
    auto state = make_state(random_);
    if (!pkce || !state) return fail(AuthorizationProblem::random_source_failed, server_.authorization_endpoint);
    pkce_ = std::move(*pkce);

    pending_.issuer = server_.issuer;
    pending_.iss_parameter_supported = server_.authorization_response_iss_parameter_supported;
    pending_.state = std::move(*state);
    pending_.redirect_uri = config_.registration.redirect_uri;

    AuthorizationRequest request;
    request.authorization_endpoint = server_.authorization_endpoint;
    request.client_id = client_.client_id;
    request.redirect_uri = config_.registration.redirect_uri;
    request.code_challenge = pkce_.challenge;
    request.state = pending_.state;
    request.resource = resource_.resource;
    request.scopes = scopes_;

    PromptRequest prompt;
    prompt.authorization_url = authorization_url(request);
    prompt.redirect_uri = config_.registration.redirect_uri;
    prompt.issuer = server_.issuer;
    prompt.scopes = scopes_;
    prompt.timeout = config_.bounds.prompt_timeout;
    stage_ = Stage::prompt;
    return prompt;
}

FlowStep OAuthFlow::on_token(const FetchResult& result) {
    auto outcome = read_token_answer(result, server_.issuer, server_.token_endpoint, scopes_, now_,
                                  config_.bounds, diagnostics_);
    if (auto* failure = std::get_if<FlowFailure>(&outcome)) {
        if (failure->error_code == error_invalid_client &&
            (client_.source == RegistrationSource::dynamic_stored ||
             client_.source == RegistrationSource::dynamic_new)) {
            // The server no longer knows this registration; the next flow registers anew.
            drop_client(server_.issuer);
        }
        return fail(std::move(*failure));
    }
    updates_.put_tokens = std::make_pair(key_, std::get<StoredTokens>(std::move(outcome)));
    memory_.active = key_;
    stage_ = Stage::finished;
    return FlowDone{key_, std::move(updates_), false};
}

}  // namespace neomcp::detail::oauth
