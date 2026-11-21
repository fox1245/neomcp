// Names of the values of the OAuth flow (types.h).
#include "client_auth/types.h"

#include <array>
#include <utility>

namespace neomcp::detail::oauth {

namespace {

constexpr std::array<std::pair<TokenEndpointAuth, std::string_view>, 3> token_endpoint_auth_names = {{
    {TokenEndpointAuth::none, "none"},
    {TokenEndpointAuth::client_secret_basic, "client_secret_basic"},
    {TokenEndpointAuth::client_secret_post, "client_secret_post"},
}};

}  // namespace

std::string_view to_string(AuthorizationProblem problem) noexcept {
    switch (problem) {
        case AuthorizationProblem::discovery_failed: return "discovery_failed";
        case AuthorizationProblem::resource_mismatch: return "resource_mismatch";
        case AuthorizationProblem::metadata_issuer_mismatch: return "metadata_issuer_mismatch";
        case AuthorizationProblem::pkce_unsupported: return "pkce_unsupported";
        case AuthorizationProblem::refused_url: return "refused_url";
        case AuthorizationProblem::insecure_endpoint: return "insecure_endpoint";
        case AuthorizationProblem::no_client_registration: return "no_client_registration";
        case AuthorizationProblem::registration_rejected: return "registration_rejected";
        case AuthorizationProblem::prompt_cancelled: return "prompt_cancelled";
        case AuthorizationProblem::prompt_timed_out: return "prompt_timed_out";
        case AuthorizationProblem::callback_invalid: return "callback_invalid";
        case AuthorizationProblem::state_mismatch: return "state_mismatch";
        case AuthorizationProblem::issuer_mismatch: return "issuer_mismatch";
        case AuthorizationProblem::authorization_denied: return "authorization_denied";
        case AuthorizationProblem::token_request_failed: return "token_request_failed";
        case AuthorizationProblem::unsupported_token_type: return "unsupported_token_type";
        case AuthorizationProblem::insufficient_scope: return "insufficient_scope";
        case AuthorizationProblem::forbidden: return "forbidden";
        case AuthorizationProblem::attempts_exceeded: return "attempts_exceeded";
        case AuthorizationProblem::document_too_large: return "document_too_large";
        case AuthorizationProblem::timed_out: return "timed_out";
        case AuthorizationProblem::random_source_failed: return "random_source_failed";
    }
    return "unknown";
}

std::string_view to_string(TokenEndpointAuth method) noexcept {
    for (const auto& [value, name] : token_endpoint_auth_names) {
        if (value == method) return name;
    }
    return {};
}

std::optional<TokenEndpointAuth> token_endpoint_auth_from(std::string_view name) noexcept {
    for (const auto& [value, known] : token_endpoint_auth_names) {
        if (known == name) return value;
    }
    return std::nullopt;
}

std::string_view to_string(ApplicationType type) noexcept {
    switch (type) {
        case ApplicationType::native: return "native";
        case ApplicationType::web: return "web";
    }
    return {};
}

}  // namespace neomcp::detail::oauth
