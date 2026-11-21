// Protected resource metadata (discovery.h).
#include "client_auth/discovery.h"

#include <algorithm>

#include "client_auth/document.h"
#include "client_auth/scopes.h"
#include "client_auth/url.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view member_resource = "resource";
constexpr std::string_view member_authorization_servers = "authorization_servers";
constexpr std::string_view member_scopes_supported = "scopes_supported";

FlowFailure failure(AuthorizationProblem problem, std::string_view url) {
    FlowFailure out;
    out.problem = problem;
    out.url = without_query(url);
    return out;
}

FlowFailure refused_origin_url(std::string_view issuer, std::string_view url) {
    FlowFailure out = failure(AuthorizationProblem::refused_url, url);
    out.issuer = std::string(issuer);
    return out;
}

// An authorization server identifier the flow can build metadata URLs from: an absolute URL without query or
// fragment, https or loopback http, that the policy admits.
bool usable_issuer(std::string_view identifier, const UrlAdmission& admission) {
    const auto url = read_url(identifier);
    if (!url || url->query || url->fragment) return false;
    if (!is_secure_or_loopback(*url)) return false;
    return !admission.admit(UrlRole::authorization_server, identifier);
}

}  // namespace

std::variant<std::vector<PrmCandidate>, FlowFailure> protected_resource_candidates(
    std::string_view endpoint_url, const std::optional<std::string>& resource_metadata, const UrlAdmission& admission) {
    const auto endpoint = read_url(endpoint_url);
    if (!endpoint) return failure(AuthorizationProblem::discovery_failed, endpoint_url);
    const std::string identifier(without_fragment(*endpoint));

    std::vector<PrmCandidate> candidates;
    if (resource_metadata) {
        const auto named = read_url(*resource_metadata);
        if (!named || !same_origin(*named, *endpoint) ||
            admission.admit(UrlRole::protected_resource_metadata, *resource_metadata)) {
            return failure(AuthorizationProblem::refused_url, *resource_metadata);
        }
        candidates.push_back(PrmCandidate{*resource_metadata, {identifier}, true});
        return candidates;
    }

    PrmCandidate path_form{insert_well_known(*endpoint, protected_resource_well_known_suffix, true), {identifier},
                           false};
    // The parts of a URL are views into its text, which must outlive them.
    const std::string origin = origin_text(*endpoint);
    const auto root = read_url(origin);
    if (!root) return failure(AuthorizationProblem::discovery_failed, endpoint_url);
    PrmCandidate root_form{insert_well_known(*root, protected_resource_well_known_suffix, false), {origin}, false};
    if (path_form.url == root_form.url) {
        path_form.identifiers.push_back(root_form.identifiers.front());
        candidates.push_back(std::move(path_form));
    } else {
        candidates.push_back(std::move(path_form));
        candidates.push_back(std::move(root_form));
    }
    for (const PrmCandidate& candidate : candidates) {
        if (admission.admit(UrlRole::protected_resource_metadata, candidate.url)) {
            return failure(AuthorizationProblem::refused_url, candidate.url);
        }
    }
    return candidates;
}

std::variant<ProtectedResource, FlowFailure> read_protected_resource(const json& document,
                                                                     const PrmCandidate& candidate,
                                                                     const AcceptResource& accept_resource,
                                                                     const UrlAdmission& admission,
                                                                     const OAuthBounds& bounds,
                                                                     std::vector<FlowDiagnostic>& diagnostics) {
    const std::string* resource = string_member(document, member_resource);
    const json* servers = member(document, member_authorization_servers);
    const auto* entries = servers == nullptr ? nullptr : servers->get_ptr<const json::array_t*>();
    if (resource == nullptr || entries == nullptr || entries->empty()) {
        return failure(AuthorizationProblem::discovery_failed, candidate.url);
    }
    for (const json& entry : *entries) {
        if (!entry.is_string()) return failure(AuthorizationProblem::discovery_failed, candidate.url);
    }

    bool identical = false;
    for (const std::string& identifier : candidate.identifiers) {
        if (same_resource_identifier(identifier, *resource)) identical = true;
    }
    if (!identical) {
        const bool accepted =
            accept_resource && accept_resource(ResourceMismatch{candidate.identifiers.front(), *resource});
        if (!accepted) return failure(AuthorizationProblem::resource_mismatch, candidate.url);
    }

    ProtectedResource out;
    out.resource = *resource;
    out.metadata_url = candidate.url;
    if (entries->size() > bounds.max_authorization_servers) {
        diagnostics.push_back({FlowDiagnostic::Kind::authorization_servers_truncated,
                               "the protected resource metadata names more authorization servers than are read"});
    }
    const std::size_t readable = std::min(entries->size(), bounds.max_authorization_servers);
    for (std::size_t i = 0; i < readable; ++i) {
        const std::string& identifier = (*entries)[i].get_ref<const std::string&>();
        if (usable_issuer(identifier, admission)) {
            out.authorization_server = identifier;
            break;
        }
        diagnostics.push_back({FlowDiagnostic::Kind::authorization_server_skipped,
                               "an authorization server of the protected resource metadata was not admitted"});
    }
    if (out.authorization_server.empty()) return failure(AuthorizationProblem::refused_url, candidate.url);

    if (auto scopes = string_array_member(document, member_scopes_supported)) {
        out.scopes_supported = bounded_scopes(*scopes, bounds, diagnostics);
    }
    return out;
}

std::variant<OriginDiscovery, FlowFailure> origin_discovery(std::string_view endpoint_url,
                                                            const profile::OriginMetadataDiscovery& declared,
                                                            const UrlAdmission& admission) {
    const auto endpoint = read_url(endpoint_url);
    if (!endpoint) return failure(AuthorizationProblem::discovery_failed, endpoint_url);

    OriginDiscovery out;
    out.issuer = origin_text(*endpoint);
    out.metadata_url = out.issuer + declared.metadata_path;
    out.defaults.issuer = out.issuer;
    out.defaults.authorization_endpoint = out.issuer + declared.default_endpoints.authorization;
    out.defaults.token_endpoint = out.issuer + declared.default_endpoints.token;
    out.defaults.registration_endpoint = out.issuer + declared.default_endpoints.registration;

    if (admission.admit(UrlRole::authorization_server, out.issuer)) return refused_origin_url(out.issuer, out.issuer);
    if (admission.admit(UrlRole::authorization_server_metadata, out.metadata_url)) {
        return refused_origin_url(out.issuer, out.metadata_url);
    }
    return out;
}

std::optional<FlowFailure> refused_default_endpoints(const OriginDiscovery& origin, const UrlAdmission& admission) {
    const AuthorizationServer& defaults = origin.defaults;
    if (admission.admit(UrlRole::authorization_endpoint, defaults.authorization_endpoint)) {
        return refused_origin_url(origin.issuer, defaults.authorization_endpoint);
    }
    if (admission.admit(UrlRole::token_endpoint, defaults.token_endpoint)) {
        return refused_origin_url(origin.issuer, defaults.token_endpoint);
    }
    if (admission.admit(UrlRole::registration_endpoint, *defaults.registration_endpoint)) {
        return refused_origin_url(origin.issuer, *defaults.registration_endpoint);
    }
    return std::nullopt;
}

}  // namespace neomcp::detail::oauth
