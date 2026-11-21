// The token requests and answers (tokens.h).
#include "client_auth/tokens.h"

#include "client_auth/document.h"
#include "client_auth/http_names.h"
#include "client_auth/scopes.h"
#include "client_auth/url.h"
#include "neomcp/http/form.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view param_grant_type = "grant_type";
constexpr std::string_view param_code = "code";
constexpr std::string_view param_redirect_uri = "redirect_uri";
constexpr std::string_view param_code_verifier = "code_verifier";
constexpr std::string_view param_refresh_token = "refresh_token";
constexpr std::string_view param_resource = "resource";
constexpr std::string_view param_client_id = "client_id";
constexpr std::string_view param_client_secret = "client_secret";
constexpr std::string_view grant_authorization_code = "authorization_code";
constexpr std::string_view grant_refresh_token = "refresh_token";

constexpr std::string_view member_access_token = "access_token";
constexpr std::string_view member_token_type = "token_type";
constexpr std::string_view member_expires_in = "expires_in";
constexpr std::string_view member_scope = "scope";
constexpr std::string_view member_refresh_token = "refresh_token";
constexpr std::string_view token_type_bearer = "Bearer";
constexpr int status_ok = 200;

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}

FlowFetch token_fetch(std::string_view token_endpoint, const ClientCredentials& client,
                      std::vector<neomcp::http::FormField> fields, const OAuthBounds& bounds) {
    FlowFetch fetch;
    fetch.purpose = FetchPurpose::token;
    fetch.method = std::string(method_post);
    fetch.url = std::string(token_endpoint);
    fetch.headers.push_back({std::string(header_content_type), std::string(media_type_form)});
    fetch.headers.push_back({std::string(header_accept), std::string(media_type_json)});
    switch (client.method) {
        case TokenEndpointAuth::client_secret_basic:
            fetch.headers.push_back(
                {std::string(header_authorization),
                 neomcp::http::oauth_basic_authorization(client.client_id, client.client_secret.value_or(""))});
            break;
        case TokenEndpointAuth::client_secret_post:
            fields.push_back({std::string(param_client_id), client.client_id});
            fields.push_back({std::string(param_client_secret), client.client_secret.value_or("")});
            break;
        case TokenEndpointAuth::none:
            fields.push_back({std::string(param_client_id), client.client_id});
            break;
    }
    fetch.body = neomcp::http::form_encode(fields);
    fetch.body_bound = bounds.max_token_response_bytes;
    fetch.follow_redirects = false;
    fetch.max_redirects = 0;
    fetch.timeout = bounds.authorization_fetch_timeout;
    return fetch;
}

FlowFailure failed(AuthorizationProblem problem, std::string_view issuer, std::string_view url) {
    FlowFailure out;
    out.problem = problem;
    out.issuer = std::string(issuer);
    out.url = without_query(url);
    return out;
}

}  // namespace

bool is_b64token(std::string_view text) noexcept {
    std::size_t at = 0;
    while (at < text.size()) {
        const char c = text[at];
        if (!(is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '+' || c == '/')) {
            break;
        }
        ++at;
    }
    if (at == 0) return false;
    while (at < text.size() && text[at] == '=') ++at;
    return at == text.size();
}

FlowFetch authorization_code_request(std::string_view token_endpoint, const ClientCredentials& client,
                                     std::string_view code, std::string_view redirect_uri,
                                     std::string_view code_verifier, const std::optional<std::string>& resource,
                                     const OAuthBounds& bounds) {
    std::vector<neomcp::http::FormField> fields;
    fields.push_back({std::string(param_grant_type), std::string(grant_authorization_code)});
    fields.push_back({std::string(param_code), std::string(code)});
    fields.push_back({std::string(param_redirect_uri), std::string(redirect_uri)});
    fields.push_back({std::string(param_code_verifier), std::string(code_verifier)});
    if (resource) fields.push_back({std::string(param_resource), *resource});
    return token_fetch(token_endpoint, client, std::move(fields), bounds);
}

FlowFetch refresh_token_request(std::string_view token_endpoint, const ClientCredentials& client,
                                std::string_view refresh_token, const std::optional<std::string>& resource,
                                const OAuthBounds& bounds) {
    std::vector<neomcp::http::FormField> fields;
    fields.push_back({std::string(param_grant_type), std::string(grant_refresh_token)});
    fields.push_back({std::string(param_refresh_token), std::string(refresh_token)});
    if (resource) fields.push_back({std::string(param_resource), *resource});
    return token_fetch(token_endpoint, client, std::move(fields), bounds);
}

std::variant<StoredTokens, FlowFailure> read_token_answer(const FetchResult& result, std::string_view issuer,
                                                          std::string_view token_endpoint,
                                                          const std::vector<std::string>& requested,
                                                          FlowClock::time_point now, const OAuthBounds& bounds,
                                                          std::vector<FlowDiagnostic>& diagnostics) {
    switch (result.outcome) {
        case FetchOutcome::response: break;
        case FetchOutcome::timed_out: return failed(AuthorizationProblem::timed_out, issuer, token_endpoint);
        case FetchOutcome::too_large: return failed(AuthorizationProblem::document_too_large, issuer, token_endpoint);
        case FetchOutcome::transport_error:
            return failed(AuthorizationProblem::token_request_failed, issuer, token_endpoint);
    }
    if (result.body.size() > bounds.max_token_response_bytes) {
        return failed(AuthorizationProblem::document_too_large, issuer, token_endpoint);
    }
    if (result.status != status_ok) {
        FlowFailure failure = failed(AuthorizationProblem::token_request_failed, issuer, token_endpoint);
        if (is_client_error_status(result.status)) {
            failure.error_code = error_code_of(result.body, bounds.max_document_depth);
        }
        return failure;
    }
    const auto document =
        is_json_media_type(result.content_type) ? json_object(result.body, bounds.max_document_depth) : std::nullopt;
    if (!document) return failed(AuthorizationProblem::token_request_failed, issuer, token_endpoint);

    const std::string* access_token = string_member(*document, member_access_token);
    if (access_token == nullptr || access_token->size() > bounds.max_token_bytes || !is_b64token(*access_token)) {
        return failed(AuthorizationProblem::token_request_failed, issuer, token_endpoint);
    }
    const std::string* token_type = string_member(*document, member_token_type);
    if (token_type == nullptr || !equal_ignoring_case(*token_type, token_type_bearer)) {
        return failed(AuthorizationProblem::unsupported_token_type, issuer, token_endpoint);
    }

    StoredTokens tokens;
    tokens.access_token = *access_token;
    tokens.requested_scopes = requested;
    tokens.granted_scopes = requested;
    if (const json* expires = member(*document, member_expires_in); expires != nullptr && expires->is_number()) {
        const double seconds = expires->get<double>();
        if (seconds > 0) {
            const auto cap = static_cast<double>(bounds.max_token_lifetime.count());
            const auto whole = static_cast<std::chrono::seconds::rep>(seconds < cap ? seconds : cap);
            const auto lifetime = std::chrono::seconds(whole);
            tokens.expires_at = now + lifetime;
        }
    }
    if (const std::string* scope = string_member(*document, member_scope)) {
        tokens.granted_scopes = bounded_scopes(split_scopes(*scope), bounds, diagnostics);
    }
    if (const std::string* refresh = string_member(*document, member_refresh_token);
        refresh != nullptr && !refresh->empty() && refresh->size() <= bounds.max_token_bytes) {
        tokens.refresh_token = *refresh;
    }
    return tokens;
}

}  // namespace neomcp::detail::oauth
