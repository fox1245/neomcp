// The authorization request and the callback (callback.h).
#include "client_auth/callback.h"

#include "client_auth/document.h"
#include "client_auth/pkce.h"
#include "client_auth/scopes.h"
#include "client_auth/url.h"
#include "neomcp/http/form.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view param_response_type = "response_type";
constexpr std::string_view param_client_id = "client_id";
constexpr std::string_view param_redirect_uri = "redirect_uri";
constexpr std::string_view param_code_challenge = "code_challenge";
constexpr std::string_view param_code_challenge_method = "code_challenge_method";
constexpr std::string_view param_state = "state";
constexpr std::string_view param_resource = "resource";
constexpr std::string_view param_scope = "scope";
constexpr std::string_view param_iss = "iss";
constexpr std::string_view param_error = "error";
constexpr std::string_view param_code = "code";
constexpr std::string_view response_type_code = "code";

FlowFailure refused(AuthorizationProblem problem, const PendingAuthorization& pending) {
    FlowFailure out;
    out.problem = problem;
    out.issuer = pending.issuer;
    out.url = without_query(pending.redirect_uri);
    return out;
}

}  // namespace

std::string authorization_url(const AuthorizationRequest& request) {
    std::vector<neomcp::http::FormField> fields;
    fields.push_back({std::string(param_response_type), std::string(response_type_code)});
    fields.push_back({std::string(param_client_id), request.client_id});
    fields.push_back({std::string(param_redirect_uri), request.redirect_uri});
    fields.push_back({std::string(param_code_challenge), request.code_challenge});
    fields.push_back({std::string(param_code_challenge_method), std::string(code_challenge_method_s256)});
    fields.push_back({std::string(param_state), request.state});
    if (request.resource) fields.push_back({std::string(param_resource), *request.resource});
    if (!request.scopes.empty()) fields.push_back({std::string(param_scope), join_scopes(request.scopes)});

    std::string out = request.authorization_endpoint;
    const auto parts = read_url(request.authorization_endpoint);
    if (!parts || !parts->query) {
        out.push_back('?');
    } else if (!parts->query->empty()) {
        out.push_back('&');
    }
    out.append(neomcp::http::form_encode(fields));
    return out;
}

std::variant<std::string, FlowFailure> check_callback(std::string_view callback_url,
                                                      const PendingAuthorization& pending,
                                                      const OAuthBounds& bounds) {
    if (callback_url.size() > bounds.max_callback_bytes) {
        return refused(AuthorizationProblem::callback_invalid, pending);
    }
    const auto callback = read_url(callback_url);
    const auto redirect = read_url(pending.redirect_uri);
    if (!callback || !redirect || !same_origin(*callback, *redirect) || callback->path != redirect->path) {
        return refused(AuthorizationProblem::callback_invalid, pending);
    }

    std::vector<neomcp::http::FormField> fields;
    if (callback->query) {
        auto decoded = neomcp::http::form_decode(*callback->query);
        auto* list = std::get_if<std::vector<neomcp::http::FormField>>(&decoded);
        if (list == nullptr) return refused(AuthorizationProblem::callback_invalid, pending);
        fields = std::move(*list);
    }
    for (const neomcp::http::FormField& field : fields) {
        if (neomcp::http::find_unique_field(fields, field.name).state == neomcp::http::FieldLookup::State::repeated) {
            return refused(AuthorizationProblem::callback_invalid, pending);
        }
    }

    const neomcp::http::FieldLookup iss = neomcp::http::find_unique_field(fields, param_iss);
    if (iss.state == neomcp::http::FieldLookup::State::present) {
        if (iss.value != pending.issuer) return refused(AuthorizationProblem::issuer_mismatch, pending);
    } else if (pending.iss_parameter_supported) {
        return refused(AuthorizationProblem::issuer_mismatch, pending);
    }

    const neomcp::http::FieldLookup state = neomcp::http::find_unique_field(fields, param_state);
    if (state.state != neomcp::http::FieldLookup::State::present || state.value != pending.state) {
        return refused(AuthorizationProblem::state_mismatch, pending);
    }

    const neomcp::http::FieldLookup error = neomcp::http::find_unique_field(fields, param_error);
    if (error.state == neomcp::http::FieldLookup::State::present) {
        FlowFailure failure = refused(AuthorizationProblem::authorization_denied, pending);
        if (is_error_code(error.value, bounds.max_error_code_bytes)) failure.error_code = std::string(error.value);
        return failure;
    }

    const neomcp::http::FieldLookup code = neomcp::http::find_unique_field(fields, param_code);
    if (code.state != neomcp::http::FieldLookup::State::present) {
        return refused(AuthorizationProblem::authorization_denied, pending);
    }
    return std::string(code.value);
}

}  // namespace neomcp::detail::oauth
