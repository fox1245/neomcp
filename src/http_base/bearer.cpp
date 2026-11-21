#include "neomcp/http/bearer.h"

#include <utility>

#include "http_base/grammar.h"
#include "neomcp/http/challenge.h"

namespace neomcp::http {

namespace {

using detail::is_alpha;
using detail::is_digit;
using detail::is_tchar;

constexpr char space = ' ';
constexpr char tab = '\t';
constexpr char padding = '=';
constexpr char quote = '"';
constexpr char backslash = '\\';
constexpr std::string_view scope_delimiter = " ";

// The characters of b64token before its padding (RFC 6750, section 2.1).
constexpr bool is_b64token_char(char c) noexcept {
    if (is_alpha(c) || is_digit(c)) return true;
    switch (c) {
        case '-':
        case '.':
        case '_':
        case '~':
        case '+':
        case '/': return true;
        default: return false;
    }
}

// %x21 / %x23-5B / %x5D-7E: visible ASCII without the quote and the backslash.
constexpr bool is_plain_visible(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte == 0x21 || (byte >= 0x23 && byte <= 0x5b) || (byte >= 0x5d && byte <= 0x7e);
}

// %x20-21 / %x23-5B / %x5D-7E: the same with the space.
constexpr bool is_plain_visible_or_space(char c) noexcept { return c == space || is_plain_visible(c); }

template <class Allowed>
bool all_of_text(std::string_view text, Allowed allowed) noexcept {
    for (const char c : text) {
        if (!allowed(c)) return false;
    }
    return true;
}

std::string_view trim_whitespace(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == space || text.front() == tab)) text.remove_prefix(1);
    while (!text.empty() && (text.back() == space || text.back() == tab)) text.remove_suffix(1);
    return text;
}

// The credentials of one field value.
std::variant<BearerCredentials, CredentialsProblem> read_one(std::string_view field, const CredentialsLimits& limits) {
    const std::string_view value = trim_whitespace(field);

    std::size_t scheme_end = 0;
    while (scheme_end < value.size() && is_tchar(value[scheme_end])) ++scheme_end;
    if (scheme_end == 0) return CredentialsProblem::malformed;  // no scheme: not credentials of any kind
    if (!equal_ignoring_case(value.substr(0, scheme_end), bearer_scheme)) return CredentialsProblem::other_scheme;

    // 1*SP, then the token and nothing after it (the whitespace at the end is already gone).
    std::string_view rest = value.substr(scheme_end);
    std::size_t spaces = 0;
    while (spaces < rest.size() && rest[spaces] == space) ++spaces;
    if (spaces == 0) return CredentialsProblem::malformed;
    rest.remove_prefix(spaces);

    std::size_t body = 0;
    while (body < rest.size() && is_b64token_char(rest[body])) ++body;
    if (body == 0) return CredentialsProblem::malformed;
    std::size_t end = body;
    while (end < rest.size() && rest[end] == padding) ++end;
    if (end != rest.size()) return CredentialsProblem::malformed;

    if (rest.size() > limits.max_token_bytes) return CredentialsProblem::token_too_large;
    return BearerCredentials{std::string(rest)};
}

// A quoted-string holding `text`, whose quotes and backslashes are written as quoted pairs (RFC 9110, section 5.6.4).
// Every parameter is checked against a set without the quote and the backslash before it gets here, so no pair is
// written today; the escape stays so that a quote can never end the string, whatever a check allows.
void append_quoted(std::string& out, std::string_view text) {
    out += quote;
    for (const char c : text) {
        if (c == quote || c == backslash) out += backslash;
        out += c;
    }
    out += quote;
}

void append_parameter(std::string& out, bool& first, std::string_view name, std::string_view value) {
    out += first ? space : ',';
    if (!first) out += space;
    first = false;
    out += name;
    out += padding;
    append_quoted(out, value);
}

}  // namespace

std::string_view to_string(CredentialsProblem problem) noexcept {
    switch (problem) {
        case CredentialsProblem::absent: return "no Authorization field";
        case CredentialsProblem::other_scheme: return "the Authorization field is not of the Bearer scheme";
        case CredentialsProblem::repeated: return "more than one Authorization field";
        case CredentialsProblem::malformed: return "the Authorization field is not Bearer credentials";
        case CredentialsProblem::token_too_large: return "the bearer token exceeds the bound";
    }
    return {};
}

std::string_view to_string(ChallengeWriteProblem problem) noexcept {
    switch (problem) {
        case ChallengeWriteProblem::invalid_resource_metadata: return "resource_metadata cannot be written";
        case ChallengeWriteProblem::invalid_scope: return "a scope value cannot be written";
        case ChallengeWriteProblem::invalid_error: return "error cannot be written";
        case ChallengeWriteProblem::invalid_error_description: return "error_description cannot be written";
    }
    return {};
}

std::variant<BearerCredentials, CredentialsProblem> read_bearer_credentials(std::span<const std::string_view> fields,
                                                                            const CredentialsLimits& limits) {
    if (fields.empty()) return CredentialsProblem::absent;
    if (fields.size() > 1) return CredentialsProblem::repeated;
    return read_one(fields.front(), limits);
}

std::variant<BearerCredentials, CredentialsProblem> read_bearer_credentials(const Headers& headers,
                                                                            const CredentialsLimits& limits) {
    const std::vector<std::string_view> fields = headers.get_all(authorization_field_name);
    return read_bearer_credentials(std::span<const std::string_view>(fields), limits);
}

std::variant<std::string, ChallengeWriteProblem> write_bearer_challenge(const BearerChallengeParts& parts) {
    if (parts.error && (parts.error->empty() || !all_of_text(*parts.error, is_plain_visible_or_space))) {
        return ChallengeWriteProblem::invalid_error;
    }
    if (parts.error_description &&
        (parts.error_description->empty() || !all_of_text(*parts.error_description, is_plain_visible_or_space))) {
        return ChallengeWriteProblem::invalid_error_description;
    }
    std::string scope;
    for (const std::string& value : parts.scopes) {
        if (!is_scope_token(value)) return ChallengeWriteProblem::invalid_scope;
        if (!scope.empty()) scope += scope_delimiter;
        scope += value;
    }
    if (parts.resource_metadata &&
        (parts.resource_metadata->empty() || !all_of_text(*parts.resource_metadata, is_plain_visible))) {
        return ChallengeWriteProblem::invalid_resource_metadata;
    }

    std::string out(bearer_scheme);
    bool first = true;
    if (parts.error) append_parameter(out, first, bearer_param_error, *parts.error);
    if (parts.error_description) {
        append_parameter(out, first, bearer_param_error_description, *parts.error_description);
    }
    if (!scope.empty()) append_parameter(out, first, bearer_param_scope, scope);
    if (parts.resource_metadata) {
        append_parameter(out, first, bearer_param_resource_metadata, *parts.resource_metadata);
    }
    return out;
}

}  // namespace neomcp::http
