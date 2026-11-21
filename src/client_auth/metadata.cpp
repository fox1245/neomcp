// Authorization server metadata (metadata.h).
#include "client_auth/metadata.h"

#include <algorithm>

#include "client_auth/document.h"
#include "client_auth/pkce.h"
#include "client_auth/scopes.h"
#include "client_auth/url.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view member_issuer = "issuer";
constexpr std::string_view member_authorization_endpoint = "authorization_endpoint";
constexpr std::string_view member_token_endpoint = "token_endpoint";
constexpr std::string_view member_registration_endpoint = "registration_endpoint";
constexpr std::string_view member_scopes_supported = "scopes_supported";
constexpr std::string_view member_code_challenge_methods = "code_challenge_methods_supported";
constexpr std::string_view member_token_endpoint_auth_methods = "token_endpoint_auth_methods_supported";
constexpr std::string_view member_client_id_metadata_document = "client_id_metadata_document_supported";
constexpr std::string_view member_iss_parameter = "authorization_response_iss_parameter_supported";

FlowFailure failure(AuthorizationProblem problem, std::string_view issuer, std::string_view url) {
    FlowFailure out;
    out.problem = problem;
    out.issuer = std::string(issuer);
    out.url = without_query(url);
    return out;
}

// An endpoint URL: absolute, without fragment, https or loopback http, admitted in its role.
bool usable_endpoint(std::string_view url, UrlRole role, const UrlAdmission& admission) {
    const auto parts = read_url(url);
    if (!parts || parts->fragment) return false;
    if (!is_secure_or_loopback(*parts)) return false;
    return !admission.admit(role, url);
}

}  // namespace

std::variant<std::vector<std::string>, FlowFailure> authorization_server_candidates(std::string_view issuer,
                                                                                    const UrlAdmission& admission) {
    const auto url = read_url(issuer);
    if (!url || url->query || url->fragment || !is_secure_or_loopback(*url) ||
        admission.admit(UrlRole::authorization_server, issuer)) {
        return failure(AuthorizationProblem::refused_url, {}, issuer);
    }
    std::vector<std::string> candidates;
    candidates.push_back(insert_well_known(*url, oauth_metadata_well_known_suffix, false));
    candidates.push_back(insert_well_known(*url, openid_configuration_well_known_suffix, false));
    const bool has_path = !url->path.empty() && url->path != "/";
    if (has_path) candidates.push_back(append_well_known(*url, openid_configuration_well_known_suffix));
    for (const std::string& candidate : candidates) {
        if (admission.admit(UrlRole::authorization_server_metadata, candidate)) {
            return failure(AuthorizationProblem::refused_url, issuer, candidate);
        }
    }
    return candidates;
}

std::variant<AuthorizationServer, FlowFailure> read_authorization_server(const json& document,
                                                                         std::string_view issuer,
                                                                         std::string_view metadata_url,
                                                                         const UrlAdmission& admission,
                                                                         const OAuthBounds& bounds,
                                                                         std::vector<FlowDiagnostic>& diagnostics) {
    const std::string* stated = string_member(document, member_issuer);
    if (stated == nullptr || *stated != issuer) {
        return failure(AuthorizationProblem::metadata_issuer_mismatch, issuer, metadata_url);
    }

    const std::string* authorization_endpoint = string_member(document, member_authorization_endpoint);
    const std::string* token_endpoint = string_member(document, member_token_endpoint);
    if (authorization_endpoint == nullptr || token_endpoint == nullptr) {
        return failure(AuthorizationProblem::discovery_failed, issuer, metadata_url);
    }
    if (!usable_endpoint(*authorization_endpoint, UrlRole::authorization_endpoint, admission)) {
        return failure(AuthorizationProblem::refused_url, issuer, *authorization_endpoint);
    }
    if (!usable_endpoint(*token_endpoint, UrlRole::token_endpoint, admission)) {
        return failure(AuthorizationProblem::refused_url, issuer, *token_endpoint);
    }

    AuthorizationServer out;
    out.issuer = *stated;
    out.authorization_endpoint = *authorization_endpoint;
    out.token_endpoint = *token_endpoint;
    if (const json* registration = member(document, member_registration_endpoint)) {
        const auto* text = registration->get_ptr<const std::string*>();
        if (text == nullptr) return failure(AuthorizationProblem::discovery_failed, issuer, metadata_url);
        if (!usable_endpoint(*text, UrlRole::registration_endpoint, admission)) {
            return failure(AuthorizationProblem::refused_url, issuer, *text);
        }
        out.registration_endpoint = *text;
    }

    const auto methods = string_array_member(document, member_code_challenge_methods);
    if (!methods || std::find(methods->begin(), methods->end(), code_challenge_method_s256) == methods->end()) {
        return failure(AuthorizationProblem::pkce_unsupported, issuer, metadata_url);
    }

    if (auto scopes = string_array_member(document, member_scopes_supported)) {
        out.scopes_supported = bounded_scopes(*scopes, bounds, diagnostics);
    }
    out.token_endpoint_auth_methods_supported = string_array_member(document, member_token_endpoint_auth_methods);
    out.client_id_metadata_document_supported = true_member(document, member_client_id_metadata_document);
    out.authorization_response_iss_parameter_supported = true_member(document, member_iss_parameter);
    return out;
}

}  // namespace neomcp::detail::oauth
