// Client registration and client authentication (registration.h).
#include "client_auth/registration.h"

#include <algorithm>

#include "client_auth/document.h"
#include "client_auth/url.h"
#include "neomcp/json.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view member_client_id = "client_id";
constexpr std::string_view member_client_secret = "client_secret";
constexpr std::string_view member_client_name = "client_name";
constexpr std::string_view member_redirect_uris = "redirect_uris";
constexpr std::string_view member_grant_types = "grant_types";
constexpr std::string_view member_response_types = "response_types";
constexpr std::string_view member_token_endpoint_auth_method = "token_endpoint_auth_method";
constexpr std::string_view member_application_type = "application_type";
constexpr std::string_view grant_authorization_code = "authorization_code";
constexpr std::string_view grant_refresh_token = "refresh_token";
constexpr std::string_view response_type_code = "code";
constexpr int status_ok = 200;
constexpr int status_created = 201;

bool listed(const std::optional<std::vector<std::string>>& methods, TokenEndpointAuth method) {
    if (!methods) return false;
    return std::find(methods->begin(), methods->end(), to_string(method)) != methods->end();
}

bool has_dot_segment(std::string_view path) {
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = path.find('/', at);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view segment = path.substr(at, end - at);
        if (segment == "." || segment == "..") return true;
        if (slash == std::string_view::npos) break;
        at = slash + 1;
    }
    return false;
}

FlowFailure rejected(AuthorizationProblem problem, std::string_view issuer, std::string_view url) {
    FlowFailure out;
    out.problem = problem;
    out.issuer = std::string(issuer);
    out.url = without_query(url);
    return out;
}

}  // namespace

TokenEndpointAuth choose_token_endpoint_auth(std::optional<TokenEndpointAuth> preferred, bool has_secret,
                                             const std::optional<std::vector<std::string>>& methods) {
    if (preferred && (!methods || listed(methods, *preferred))) {
        if (*preferred == TokenEndpointAuth::none || has_secret) return *preferred;
    }
    if (has_secret) {
        if (!methods || listed(methods, TokenEndpointAuth::client_secret_basic)) {
            return TokenEndpointAuth::client_secret_basic;
        }
        if (listed(methods, TokenEndpointAuth::client_secret_post)) return TokenEndpointAuth::client_secret_post;
    }
    return TokenEndpointAuth::none;
}

ClientCredentials credentials_of(const StoredClient& client, const AuthorizationServer& server,
                                 RegistrationSource source) {
    ClientCredentials out;
    out.client_id = client.client_id;
    out.client_secret = client.client_secret;
    out.method = choose_token_endpoint_auth(client.method, client.client_secret.has_value(),
                                            server.token_endpoint_auth_methods_supported);
    out.source = source;
    return out;
}

std::variant<ClientCredentials, NeedsRegistration, FlowFailure> choose_registration(
    const AuthorizationServer& server, const RegistrationConfig& config, const std::optional<StoredClient>& stored) {
    for (const PreRegisteredClient& entry : config.pre_registered) {
        if (entry.issuer != server.issuer) continue;
        ClientCredentials out;
        out.client_id = entry.client_id;
        out.client_secret = entry.client_secret;
        out.method = entry.method ? *entry.method
                                  : choose_token_endpoint_auth(std::nullopt, entry.client_secret.has_value(),
                                                               server.token_endpoint_auth_methods_supported);
        if (out.method != TokenEndpointAuth::none && !out.client_secret) out.method = TokenEndpointAuth::none;
        out.source = RegistrationSource::pre_registered;
        return out;
    }
    if (config.client_id_metadata_url && server.client_id_metadata_document_supported) {
        ClientCredentials out;
        out.client_id = *config.client_id_metadata_url;
        out.method = TokenEndpointAuth::none;  // a metadata document cannot carry a shared secret
        out.source = RegistrationSource::client_id_metadata_document;
        return out;
    }
    if (stored) return credentials_of(*stored, server, RegistrationSource::dynamic_stored);
    if (config.dynamic_registration && server.registration_endpoint) return NeedsRegistration{};
    FlowFailure failure;
    failure.problem = AuthorizationProblem::no_client_registration;
    failure.issuer = server.issuer;
    failure.credentials_for_other_issuer = !config.pre_registered.empty();
    return failure;
}

ApplicationType application_type_for(const RegistrationConfig& config) {
    if (config.application_type) return *config.application_type;
    const auto uri = read_url(config.redirect_uri);
    return uri && scheme_is(*uri, scheme_http) && is_loopback(*uri) ? ApplicationType::native : ApplicationType::web;
}

std::string registration_request_body(const RegistrationConfig& config) {
    json body = json::object();
    body[std::string(member_redirect_uris)] = json::array({config.redirect_uri});
    body[std::string(member_grant_types)] =
        json::array({std::string(grant_authorization_code), std::string(grant_refresh_token)});
    body[std::string(member_response_types)] = json::array({std::string(response_type_code)});
    body[std::string(member_token_endpoint_auth_method)] = std::string(to_string(TokenEndpointAuth::none));
    body[std::string(member_application_type)] = std::string(to_string(application_type_for(config)));
    if (!config.client_name.empty()) body[std::string(member_client_name)] = config.client_name;
    return body.dump();
}

std::variant<StoredClient, FlowFailure> read_registration_answer(const FetchResult& result, std::string_view issuer,
                                                                 std::string_view registration_endpoint,
                                                                 std::string_view redirect_uri,
                                                                 const OAuthBounds& bounds) {
    switch (result.outcome) {
        case FetchOutcome::response: break;
        case FetchOutcome::timed_out:
            return rejected(AuthorizationProblem::timed_out, issuer, registration_endpoint);
        case FetchOutcome::too_large:
            return rejected(AuthorizationProblem::document_too_large, issuer, registration_endpoint);
        case FetchOutcome::transport_error:
            return rejected(AuthorizationProblem::registration_rejected, issuer, registration_endpoint);
    }
    if (result.body.size() > bounds.max_token_response_bytes) {
        return rejected(AuthorizationProblem::document_too_large, issuer, registration_endpoint);
    }
    if (result.status != status_ok && result.status != status_created) {
        FlowFailure failure = rejected(AuthorizationProblem::registration_rejected, issuer, registration_endpoint);
        if (is_client_error_status(result.status)) {
            failure.error_code = error_code_of(result.body, bounds.max_document_depth);
        }
        return failure;
    }
    const auto document = is_json_media_type(result.content_type)
                              ? json_object(result.body, bounds.max_document_depth)
                              : std::nullopt;
    if (!document) return rejected(AuthorizationProblem::registration_rejected, issuer, registration_endpoint);
    const std::string* client_id = string_member(*document, member_client_id);
    if (client_id == nullptr || client_id->empty()) {
        return rejected(AuthorizationProblem::registration_rejected, issuer, registration_endpoint);
    }
    if (member(*document, member_redirect_uris) != nullptr) {
        const auto registered = string_array_member(*document, member_redirect_uris);
        if (!registered || std::find(registered->begin(), registered->end(), redirect_uri) == registered->end()) {
            return rejected(AuthorizationProblem::registration_rejected, issuer, registration_endpoint);
        }
    }
    StoredClient client;
    client.client_id = *client_id;
    if (const std::string* secret = string_member(*document, member_client_secret);
        secret != nullptr && !secret->empty()) {
        client.client_secret = *secret;
    }
    if (const std::string* method = string_member(*document, member_token_endpoint_auth_method)) {
        client.method = token_endpoint_auth_from(*method);
    }
    return client;
}

std::optional<std::string> check_client_id_metadata_url(std::string_view url, bool& has_query) {
    has_query = false;
    const auto parts = read_url(url);
    if (!parts) {
        return std::string("the client ID metadata document URL is not an absolute URL without user information");
    }
    if (!scheme_is(*parts, scheme_https)) return std::string("the client ID metadata document URL must use https");
    if (parts->path.empty() || parts->path == "/") {
        return std::string("the client ID metadata document URL must have a path");
    }
    if (has_dot_segment(parts->path)) {
        return std::string("the client ID metadata document URL must not have a \".\" or \"..\" segment");
    }
    if (parts->fragment) return std::string("the client ID metadata document URL must not have a fragment");
    has_query = parts->query.has_value();
    return std::nullopt;
}

std::optional<std::string> check_redirect_uri(std::string_view uri) {
    const auto parts = read_url(uri);
    if (!parts) return std::string("the redirect URI is not an absolute URL without user information");
    if (parts->fragment) return std::string("the redirect URI must not have a fragment");
    if (scheme_is(*parts, scheme_https)) return std::nullopt;
    if (scheme_is(*parts, scheme_http) && is_loopback(*parts)) return std::nullopt;
    return std::string("the redirect URI must use https, or http to a loopback host");
}

}  // namespace neomcp::detail::oauth
