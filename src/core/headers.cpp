#include "core/headers.h"

#include <utility>

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

std::string quoted(std::string_view text) { return "'" + std::string(text) + "'"; }

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
            return prefix + header.name + " header value " + quoted(found.value) + " does not match body value " +
                   quoted(expected);
        }
    }
    return std::nullopt;
}

}  // namespace neomcp::detail
