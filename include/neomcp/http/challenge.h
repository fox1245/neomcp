// The challenges of a WWW-Authenticate field (RFC 9110, sections 11.2, 11.3 and 11.6.1), and the Bearer challenge of
// OAuth (RFC 6750, section 3, with the resource_metadata parameter of RFC 9728, section 5.1).
//
// The values are read by a parser of the grammar, never by pattern:
//
//   WWW-Authenticate = #challenge
//   challenge        = auth-scheme [ 1*SP ( token68 / #auth-param ) ]
//   auth-param       = token BWS "=" BWS ( token / quoted-string )
//   token68          = 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" ) *"="
//
// A field may hold several challenges and may occur on several lines. The lines are read as one list, as if they were
// joined with commas (RFC 9110, section 5.3), so a line that starts with a parameter continues the last challenge of
// the line before. Empty list elements are skipped (section 5.6.1). After a comma, a token followed by "=" is a
// parameter of the challenge being read; any other token starts a new challenge. Schemes and parameter names are
// read without regard to ASCII case and kept in lower case; values are kept as written, a quoted string without its
// quotes and with its escapes resolved.
//
// Nothing here throws for what a server sent, and no error carries the server's text: a problem is a kind and a
// place. Nothing depends on an HTTP library.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neomcp::http {

struct AuthParam {
    std::string name;   // lower case
    std::string value;  // as written; a quoted string without its quotes and with its escapes resolved
};

struct Challenge {
    std::string scheme;                  // lower case
    std::optional<std::string> token68;  // present when the challenge has the token68 form; then `params` is empty
    std::vector<AuthParam> params;       // in order, at most ChallengeLimits::max_params of them

    // A parameter name occurred more than once, ignoring case. RFC 9110 allows each name once per challenge, so such
    // a challenge is not to be used; every occurrence is in `params`.
    bool repeated_parameter = false;
    // The challenge had more parameters than ChallengeLimits::max_params. Those beyond the bound are not kept, and the
    // challenge is not to be used; the challenges after it are read as usual.
    bool too_many_parameters = false;

    // The value of the first parameter of that name, compared without regard to ASCII case.
    const std::string* param(std::string_view name) const;
};

struct ChallengeLimits {
    // The default response header bound of the HTTP client; a transport passes the bound it was configured with.
    static constexpr std::size_t default_max_bytes = std::size_t{64} * 1024;
    // Real challenges carry a handful of parameters.
    static constexpr std::size_t default_max_params = 64;

    // The bytes of all field values together.
    std::size_t max_bytes = default_max_bytes;
    // The parameters of one challenge.
    std::size_t max_params = default_max_params;
};

enum class ChallengeProblem {
    too_large,  // the field values together exceed ChallengeLimits::max_bytes; nothing was read
    malformed   // a value the grammar does not allow
};

// The kind of problem, for a person to read; never the server's text.
std::string_view to_string(ChallengeProblem problem) noexcept;

struct ChallengeError {
    ChallengeProblem problem = ChallengeProblem::malformed;
    std::size_t field = 0;   // the index of the field value where reading stopped (0 for too_large)
    std::size_t offset = 0;  // the byte offset in that value where reading stopped (0 for too_large)
};

// The challenges in order, or the first problem. A value the grammar does not allow makes the whole input invalid:
// nothing is guessed from the part that could be read. No field value, or values with only empty list elements, give
// an empty list.
std::variant<std::vector<Challenge>, ChallengeError> parse_challenges(std::span<const std::string> field_values,
                                                                      const ChallengeLimits& limits = {});
std::variant<std::vector<Challenge>, ChallengeError> parse_challenges(std::string_view field_value,
                                                                      const ChallengeLimits& limits = {});

// ---- The Bearer challenge

// The names RFC 6750 and RFC 9728 give the scheme and the parameters a client reads.
inline constexpr std::string_view bearer_scheme = "Bearer";
inline constexpr std::string_view bearer_param_resource_metadata = "resource_metadata";
inline constexpr std::string_view bearer_param_scope = "scope";
inline constexpr std::string_view bearer_param_error = "error";
inline constexpr std::string_view bearer_param_error_description = "error_description";

// The error codes of RFC 6750, section 3.1.
inline constexpr std::string_view bearer_error_invalid_request = "invalid_request";
inline constexpr std::string_view bearer_error_invalid_token = "invalid_token";
inline constexpr std::string_view bearer_error_insufficient_scope = "insufficient_scope";

enum class BearerError {
    none,                // no `error` parameter
    invalid_request,
    invalid_token,
    insufficient_scope,
    other                // a code RFC 6750 does not define; `error` holds it
};

struct BearerChallenge {
    std::optional<std::string> resource_metadata;  // the URL of the protected resource metadata
    std::optional<std::string> scope;              // as sent: scope values delimited by spaces
    std::optional<std::string> error;              // the code as sent
    BearerError error_kind = BearerError::none;    // the code, compared exactly (codes are case-sensitive)
    // A text the server wrote for its developers. It is the server's free text: it must never be shown to a person,
    // logged, or placed in an error or a diagnostic.
    std::optional<std::string> error_description;

    // The scope values of `scope` in order, split at spaces; empty pieces are dropped. The values are not checked
    // against the scope-token syntax. Empty when there is no `scope`.
    std::vector<std::string> scopes() const;
};

enum class BearerProblem {
    absent,               // no challenge has the Bearer scheme
    token68_form,         // the first Bearer challenge has the token68 form
    repeated_parameter,   // the first Bearer challenge names a parameter twice
    too_many_parameters   // the first Bearer challenge has more parameters than the bound
};

std::string_view to_string(BearerProblem problem) noexcept;

// The first challenge whose scheme is Bearer, ignoring case (RFC 6750, section 3), read. When that challenge cannot
// be used (token68 form, a repeated parameter, too many parameters) the result says why, and no later Bearer
// challenge takes its place.
std::variant<BearerChallenge, BearerProblem> find_bearer_challenge(std::span<const Challenge> challenges);

}  // namespace neomcp::http
