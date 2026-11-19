#include "core/headers.h"

#include <cmath>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include "core/base64.h"

namespace neomcp::detail {
namespace {

char ascii_lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

// A header value in the profile's encoded form decoded, a plain value as it is, or empty when the value has the
// form's prefix and suffix but what lies between them is not strict Base64.
std::optional<std::string> decoded(std::string_view value, const profile::EncodedHeaderValue* encoded) {
    if (encoded != nullptr) {
        const std::string_view prefix = encoded->prefix;
        const std::string_view suffix = encoded->suffix;
        // The length is checked first: the prefix and the suffix of a short value could overlap.
        if (value.size() >= prefix.size() + suffix.size() && value.substr(0, prefix.size()) == prefix &&
            value.substr(value.size() - suffix.size()) == suffix) {
            return decode_base64(value.substr(prefix.size(), value.size() - prefix.size() - suffix.size()));
        }
    }
    return std::string(value);
}

std::string in_quotes(std::string_view text) { return "'" + std::string(text) + "'"; }

// ---- Argument headers

// The characters a header value may hold in the plain form (RFC 9110, section 5.5): visible ASCII, space and tab.
bool plain_header_character(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return (byte >= 0x20 && byte <= 0x7E) || byte == 0x09;
}

enum class FormProblem { plain_character, encoding };

struct HeaderForm {
    std::string value;                 // the decoded value
    std::optional<FormProblem> problem;
};

// A header value in the profile's encoded form decoded; a plain value as it is, when every character may be in a
// plain value. What has the form's prefix and suffix is taken as encoded, and what lies between must be strict Base64.
HeaderForm read_form(std::string_view value, const profile::EncodedHeaderValue* encoded) {
    if (encoded != nullptr) {
        const std::string_view prefix = encoded->prefix;
        const std::string_view suffix = encoded->suffix;
        if (value.size() >= prefix.size() + suffix.size() && value.substr(0, prefix.size()) == prefix &&
            value.substr(value.size() - suffix.size()) == suffix) {
            auto bytes = decode_base64(value.substr(prefix.size(), value.size() - prefix.size() - suffix.size()));
            if (!bytes) return {std::string(), FormProblem::encoding};
            return {std::move(*bytes), std::nullopt};
        }
    }
    for (const char c : value) {
        if (!plain_header_character(c)) return {std::string(), FormProblem::plain_character};
    }
    return {std::string(value), std::nullopt};
}

// The integer that a header value spells in decimal: an optional minus sign, digits, and optionally a fraction made
// of zeros (so 42.0 is 42); empty when it spells anything else or an integer outside +-max_safe_integer.
std::optional<std::int64_t> header_integer(std::string_view text) {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') ++digits;
    if (digits == 0) return std::nullopt;
    std::string_view whole = text.substr(0, digits);
    text.remove_prefix(digits);
    if (!text.empty()) {
        if (text.front() != '.' || text.size() < 2) return std::nullopt;
        text.remove_prefix(1);
        for (const char c : text) {
            if (c != '0') return std::nullopt;
        }
    }
    while (whole.size() > 1 && whole.front() == '0') whole.remove_prefix(1);
    constexpr std::size_t safe_digits = 16;  // 2^53 - 1 has sixteen digits
    if (whole.size() > safe_digits) return std::nullopt;
    std::int64_t value = 0;
    for (const char c : whole) value = value * 10 + (c - '0');
    if (value > max_safe_integer) return std::nullopt;
    return negative ? -value : value;
}

enum class Match { equal, different, unmirrorable };

// Whether the value of a header is what the argument says. An argument that no header can carry (an object, an array,
// a number that is not a whole number, an integer outside +-max_safe_integer) is unmirrorable.
Match compare_argument(const json& argument, std::string_view given) {
    if (const auto* text = argument.get_ptr<const std::string*>()) return given == *text ? Match::equal : Match::different;
    if (const auto* flag = argument.get_ptr<const json::boolean_t*>()) {
        return given == (*flag ? "true" : "false") ? Match::equal : Match::different;
    }
    std::optional<std::int64_t> number;
    if (argument.is_number_unsigned()) {
        const std::uint64_t value = *argument.get_ptr<const json::number_unsigned_t*>();
        if (value <= static_cast<std::uint64_t>(max_safe_integer)) number = static_cast<std::int64_t>(value);
    } else if (argument.is_number_integer()) {
        const std::int64_t value = *argument.get_ptr<const json::number_integer_t*>();
        if (value >= -max_safe_integer && value <= max_safe_integer) number = value;
    } else if (const auto* real = argument.get_ptr<const json::number_float_t*>()) {
        if (std::isfinite(*real) && std::floor(*real) == *real && std::fabs(*real) <= static_cast<double>(max_safe_integer)) {
            number = static_cast<std::int64_t>(*real);
        }
    } else {
        return Match::unmirrorable;
    }
    if (!number) return Match::unmirrorable;
    const auto spelled = header_integer(given);
    return spelled && *spelled == *number ? Match::equal : Match::different;
}

// The argument at the end of the path, or null when a step of it is missing or is not an object.
const json* argument_at(const json& arguments, const std::vector<std::string>& path) {
    const json* at = &arguments;
    for (const std::string& step : path) {
        at = member(*at, step);
        if (at == nullptr) return nullptr;
    }
    return at;
}

std::string argument_name(const std::vector<std::string>& path) {
    std::string name = "params.arguments";
    for (const std::string& step : path) name += "." + step;
    return name;
}

}  // namespace

bool same_header_name(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

FieldLookup find_field(std::span<const HeaderField> fields, std::string_view name) noexcept {
    FieldLookup found;
    for (const HeaderField& field : fields) {
        if (!same_header_name(field.name, name)) continue;
        if (found.count == 0) found.value = field.value;
        ++found.count;
    }
    return found;
}

bool applies_to_request(const profile::RequestHeader& header, std::string_view method) {
    switch (header.scope) {
        case profile::HeaderScopeKind::every_message:
        case profile::HeaderScopeKind::every_request: return true;
        case profile::HeaderScopeKind::per_method: return header.params_member(method).has_value();
    }
    return false;
}

std::optional<std::string_view> required_version_header(std::span<const profile::RequestHeader> headers,
                                                        std::string_view method) {
    for (const auto& header : headers) {
        if (header.value == profile::HeaderValue::protocol_version && applies_to_request(header, method)) {
            return std::string_view(header.name);
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_request_headers(const Message& request, std::span<const HeaderField> fields,
                                                 std::span<const profile::RequestHeader> headers,
                                                 const profile::EncodedHeaderValue* encoded,
                                                 std::string_view revision) {
    const std::string prefix(header_mismatch_prefix);
    for (const auto& header : headers) {
        if (!applies_to_request(header, request.method)) continue;
        const FieldLookup found = find_field(fields, header.name);
        if (found.count == 0) return prefix + "the request lacks the " + header.name + " header";
        if (found.count > 1) return prefix + "the " + header.name + " header appears more than once";
        std::string expected;
        std::string given;
        switch (header.value) {
            case profile::HeaderValue::protocol_version:
                expected = std::string(revision);
                given = std::string(found.value);
                break;
            case profile::HeaderValue::method:
                expected = request.method;
                given = std::string(found.value);
                break;
            case profile::HeaderValue::params_member: {
                const std::string_view key = *header.params_member(request.method);
                const json* body = member(request.params, key);
                const auto* text = body == nullptr ? nullptr : body->get_ptr<const std::string*>();
                if (text == nullptr) {
                    return prefix + "the " + header.name + " header carries params." + std::string(key) +
                           ", which the body does not give as a string";
                }
                auto value = decoded(found.value, encoded);
                if (!value) return prefix + "the " + header.name + " header value is not valid in the encoded form";
                expected = *text;
                given = std::move(*value);
                break;
            }
        }
        if (given != expected) {
            return prefix + header.name + " header value " + in_quotes(found.value) + " does not match body value " +
                   in_quotes(expected);
        }
    }
    return std::nullopt;
}

std::optional<ArgumentHeaderFailure> check_argument_headers(const json& arguments, const json& input_schema,
                                                            std::span<const HeaderField> fields,
                                                            const profile::ArgumentHeaders& headers,
                                                            const profile::EncodedHeaderValue* encoded) {
    const std::string prefix(header_mismatch_prefix);
    auto parameters = header_parameters(input_schema, headers.schema_keyword);
    if (const auto* issue = std::get_if<HeaderMarkIssue>(&parameters)) {
        return ArgumentHeaderFailure{"the input schema is invalid at '" + issue->pointer + "': " + issue->rule, true};
    }
    for (const HeaderParameter& parameter : std::get<std::vector<HeaderParameter>>(parameters)) {
        const std::string name = headers.header_prefix + parameter.name;
        const std::string argument = argument_name(parameter.path);
        const FieldLookup found = find_field(fields, name);
        if (found.count > 1) return ArgumentHeaderFailure{prefix + "the " + name + " header appears more than once", false};
        const json* value = argument_at(arguments, parameter.path);
        const bool given = value != nullptr && !value->is_null();
        if (found.count == 0) {
            if (!given) continue;  // an absent or null argument has no header
            return ArgumentHeaderFailure{prefix + "the request lacks the " + name + " header, which mirrors " + argument,
                                         false};
        }
        // The header is there: its form is checked whether or not the argument is.
        HeaderForm form = read_form(found.value, encoded);
        if (form.problem == FormProblem::plain_character) {
            return ArgumentHeaderFailure{
                prefix + "the " + name + " header value holds a character that a plain header value may not hold", false};
        }
        if (form.problem == FormProblem::encoding) {
            return ArgumentHeaderFailure{prefix + "the " + name + " header value is not valid in the encoded form", false};
        }
        if (!given) continue;
        switch (compare_argument(*value, form.value)) {
            case Match::equal: break;
            case Match::different:
                return ArgumentHeaderFailure{prefix + "the " + name + " header value does not match " + argument, false};
            case Match::unmirrorable:
                return ArgumentHeaderFailure{prefix + argument + " is not a string, a boolean or a whole number within " +
                                                 "the range of a header value, so the " + name + " header cannot mirror it",
                                             false};
        }
    }
    return std::nullopt;
}

}  // namespace neomcp::detail
