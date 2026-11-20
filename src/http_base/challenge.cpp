#include "neomcp/http/challenge.h"

#include <utility>

#include "http_base/grammar.h"
#include "neomcp/http/headers.h"

namespace neomcp::http {

namespace {

using detail::Cursor;
using detail::is_alpha;
using detail::is_digit;
using detail::lowered;

// The characters of token68 before its padding (RFC 9110, section 11.2).
constexpr bool is_token68_char(char c) noexcept {
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

constexpr char list_separator = ',';
constexpr char space = ' ';
constexpr char equals = '=';
constexpr char quote = '"';
constexpr char scope_delimiter = ' ';

// The field values read as one list, with the place each value starts, so a place in the list maps back to a value.
struct JoinedValues {
    std::string text;
    std::vector<std::size_t> starts;
};

ChallengeError malformed_at(const JoinedValues& joined, std::size_t position) {
    std::size_t field = 0;
    while (field + 1 < joined.starts.size() && joined.starts[field + 1] <= position) ++field;
    const std::size_t start = joined.starts.empty() ? 0 : joined.starts[field];
    return ChallengeError{ChallengeProblem::malformed, field, position - start};
}

// A parameter value: a token or a quoted string; nothing when neither is here.
std::optional<std::string> read_value(Cursor& cursor) {
    if (!cursor.done() && cursor.peek() == quote) return cursor.quoted_string();
    std::string value = cursor.token();
    if (value.empty()) return std::nullopt;
    return value;
}

// A token68 starting here; nothing when there is none.
std::optional<std::string> read_token68(Cursor& cursor) {
    std::string value;
    while (!cursor.done() && is_token68_char(cursor.peek())) {
        value += cursor.peek();
        cursor.take(cursor.peek());
    }
    if (value.empty()) return std::nullopt;
    while (cursor.take(equals)) value += equals;
    return value;
}

// Whitespace and then the end of the list element: the end of the text or a comma, which is not consumed.
bool at_element_end(Cursor& cursor) {
    cursor.skip_whitespace();
    return cursor.done() || cursor.peek() == list_separator;
}

class ChallengeReader {
public:
    ChallengeReader(const JoinedValues& joined, const ChallengeLimits& limits)
        : joined_(joined), limits_(limits), cursor_(joined.text) {}

    std::variant<std::vector<Challenge>, ChallengeError> run() {
        for (;;) {
            cursor_.skip_whitespace();
            if (cursor_.done()) return std::move(challenges_);
            if (cursor_.take(list_separator)) continue;  // an empty list element, or the comma after an element
            if (!element()) return malformed_at(joined_, cursor_.position());
        }
    }

private:
    // One list element: a parameter of the challenge being read, or a new challenge. False when it is malformed;
    // the cursor is then where reading stopped.
    bool element() {
        std::string name = cursor_.token();
        if (name.empty()) return false;
        if (accepting_params_) {
            const std::size_t after_name = cursor_.position();
            cursor_.skip_whitespace();
            if (cursor_.take(equals)) return parameter(std::move(name));
            cursor_.reset(after_name);
        }
        return challenge(std::move(name));
    }

    // The rest of `name BWS "=" BWS value`, after the "=".
    bool parameter(std::string name) {
        cursor_.skip_whitespace();
        std::optional<std::string> value = read_value(cursor_);
        if (!value) return false;
        add_param(std::move(name), std::move(*value));
        return at_element_end(cursor_);
    }

    // A challenge whose scheme was read: nothing more, a token68, or its first parameter.
    bool challenge(std::string scheme) {
        Challenge entry;
        entry.scheme = lowered(std::move(scheme));
        challenges_.push_back(std::move(entry));
        accepting_params_ = true;
        const std::size_t after_scheme = cursor_.position();
        if (at_element_end(cursor_)) return true;
        cursor_.reset(after_scheme);
        if (!cursor_.take(space)) return false;  // the scheme is followed by 1*SP
        cursor_.skip_whitespace();

        // The parameter form first: `token BWS "=" BWS ( token / quoted-string )` and the end of the element.
        const std::size_t start = cursor_.position();
        std::string name = cursor_.token();
        if (!name.empty()) {
            cursor_.skip_whitespace();
            if (cursor_.take(equals)) {
                cursor_.skip_whitespace();
                std::optional<std::string> value = read_value(cursor_);
                if (value && at_element_end(cursor_)) {
                    add_param(std::move(name), std::move(*value));
                    return true;
                }
            }
        }

        // Otherwise a token68, which is all the challenge holds. When neither form fits, reading stops where the
        // challenge's data starts.
        cursor_.reset(start);
        std::optional<std::string> token68 = read_token68(cursor_);
        if (!token68 || !at_element_end(cursor_)) {
            cursor_.reset(start);
            return false;
        }
        challenges_.back().token68 = std::move(*token68);
        accepting_params_ = false;
        return true;
    }

    void add_param(std::string name, std::string value) {
        Challenge& current = challenges_.back();
        std::string folded = lowered(std::move(name));
        if (current.too_many_parameters) return;
        for (const AuthParam& earlier : current.params) {
            if (earlier.name == folded) current.repeated_parameter = true;
        }
        if (current.params.size() >= limits_.max_params) {
            current.too_many_parameters = true;
            return;
        }
        current.params.push_back(AuthParam{std::move(folded), std::move(value)});
    }

    const JoinedValues& joined_;
    const ChallengeLimits& limits_;
    Cursor cursor_;
    std::vector<Challenge> challenges_;
    bool accepting_params_ = false;  // a challenge in the parameter form is being read
};

template <class Values>
std::variant<std::vector<Challenge>, ChallengeError> parse_values(const Values& values, const ChallengeLimits& limits) {
    std::size_t total = 0;
    for (const auto& value : values) {
        if (value.size() > limits.max_bytes - total) return ChallengeError{ChallengeProblem::too_large, 0, 0};
        total += value.size();
    }
    JoinedValues joined;
    joined.text.reserve(total + values.size());
    for (const auto& value : values) {
        if (!joined.starts.empty()) joined.text += list_separator;
        joined.starts.push_back(joined.text.size());
        joined.text.append(value.data(), value.size());
    }
    return ChallengeReader(joined, limits).run();
}

}  // namespace

std::string_view to_string(ChallengeProblem problem) noexcept {
    switch (problem) {
        case ChallengeProblem::too_large: return "the WWW-Authenticate values exceed the bound";
        case ChallengeProblem::malformed: return "a WWW-Authenticate value is not a list of challenges";
    }
    return {};
}

std::string_view to_string(BearerProblem problem) noexcept {
    switch (problem) {
        case BearerProblem::absent: return "no Bearer challenge";
        case BearerProblem::token68_form: return "the Bearer challenge has no parameters but a token68";
        case BearerProblem::repeated_parameter: return "the Bearer challenge names a parameter twice";
        case BearerProblem::too_many_parameters: return "the Bearer challenge has more parameters than the bound";
    }
    return {};
}

const std::string* Challenge::param(std::string_view name) const {
    for (const AuthParam& entry : params) {
        if (equal_ignoring_case(entry.name, name)) return &entry.value;
    }
    return nullptr;
}

std::variant<std::vector<Challenge>, ChallengeError> parse_challenges(std::span<const std::string> field_values,
                                                                      const ChallengeLimits& limits) {
    return parse_values(field_values, limits);
}

std::variant<std::vector<Challenge>, ChallengeError> parse_challenges(std::string_view field_value,
                                                                      const ChallengeLimits& limits) {
    const std::span<const std::string_view> values(&field_value, 1);
    return parse_values(values, limits);
}

std::vector<std::string> BearerChallenge::scopes() const {
    std::vector<std::string> out;
    if (!scope) return out;
    std::string_view rest = *scope;
    while (!rest.empty()) {
        const std::size_t end = rest.find(scope_delimiter);
        const std::string_view piece = rest.substr(0, end);
        if (!piece.empty()) out.emplace_back(piece);
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end + 1);
    }
    return out;
}

std::variant<BearerChallenge, BearerProblem> find_bearer_challenge(std::span<const Challenge> challenges) {
    for (const Challenge& challenge : challenges) {
        if (!equal_ignoring_case(challenge.scheme, bearer_scheme)) continue;
        if (challenge.token68) return BearerProblem::token68_form;
        if (challenge.repeated_parameter) return BearerProblem::repeated_parameter;
        if (challenge.too_many_parameters) return BearerProblem::too_many_parameters;
        BearerChallenge bearer;
        const auto copy = [&](std::string_view name) -> std::optional<std::string> {
            if (const std::string* value = challenge.param(name)) return *value;
            return std::nullopt;
        };
        bearer.resource_metadata = copy(bearer_param_resource_metadata);
        bearer.scope = copy(bearer_param_scope);
        bearer.error = copy(bearer_param_error);
        bearer.error_description = copy(bearer_param_error_description);
        if (!bearer.error) {
            bearer.error_kind = BearerError::none;
        } else if (*bearer.error == bearer_error_invalid_request) {
            bearer.error_kind = BearerError::invalid_request;
        } else if (*bearer.error == bearer_error_invalid_token) {
            bearer.error_kind = BearerError::invalid_token;
        } else if (*bearer.error == bearer_error_insufficient_scope) {
            bearer.error_kind = BearerError::insufficient_scope;
        } else {
            bearer.error_kind = BearerError::other;
        }
        return bearer;
    }
    return BearerProblem::absent;
}

}  // namespace neomcp::http
