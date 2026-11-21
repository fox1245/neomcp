#include "client_http/header_fields.h"

#include <array>
#include <exception>
#include <set>
#include <utility>

#include "client_http/transfers.h"
#include "neomcp/http/headers.h"

namespace neomcp::detail::http_client {

namespace {

// The fields of HTTP that the transport writes itself (the media types, the credentials, the resumption point) or that
// frame a message or a connection; libcurl writes the others of them. The names the protocol gives its own fields
// come from the profiles (reserved_fields), not from here.
constexpr std::array<std::string_view, 14> http_level_names{
    "Accept",       "Authorization", "Connection",         "Content-Length", "Content-Type", "Expect",
    "Host",         "Last-Event-ID", "Proxy-Authorization", "Proxy-Connection", "TE",          "Trailer",
    "Transfer-Encoding", "Upgrade"};

// Controls are 0x00 to 0x1F and 0x7F; the tab is the one a value may hold.
constexpr unsigned char tab = 0x09;
constexpr unsigned char space = 0x20;
constexpr unsigned char delete_character = 0x7F;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
    std::string folded(text);
    for (char& c : folded) c = lower(c);
    return folded;
}

bool is_field_name(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name) {
        if (!is_token_char(c)) return false;
    }
    return true;
}

// RFC 9110, section 5.5: visible characters, obs-text, space and tab.
bool is_field_value(std::string_view value) {
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == tab) continue;
        if (byte < space || byte == delete_character) return false;
    }
    return true;
}

bool has_prefix(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && http::equal_ignoring_case(name.substr(0, prefix.size()), prefix);
}

std::string known_name(std::string_view name) { return "the header field " + std::string(name); }

}  // namespace

void Lines::add(std::string_view name, std::string_view value) {
    auto line = header_line(name, value);
    if (!line) {
        if (!problem) problem = "the header field " + std::string(name) + " cannot be sent as it is";
        return;
    }
    lines.push_back(std::move(*line));
    names.emplace_back(name);
}

bool ReservedFields::covers(std::string_view name) const {
    for (const std::string& reserved : names) {
        if (http::equal_ignoring_case(reserved, name)) return true;
    }
    for (const std::string& prefix : prefixes) {
        if (has_prefix(name, prefix)) return true;
    }
    return false;
}

std::span<const std::string_view> http_level_fields() noexcept { return http_level_names; }

ReservedFields reserved_fields(const std::vector<profile::ProfilePtr>& profiles) {
    std::vector<profile::ProfilePtr> enabled = profiles;
    if (enabled.empty()) {
        for (const auto revision : profile::builtin_revisions()) enabled.push_back(profile::builtin(revision));
    }
    ReservedFields reserved;
    for (const std::string_view name : http_level_names) reserved.names.emplace_back(name);
    for (const profile::ProfilePtr& one : enabled) {
        if (!one || !one->streamable_http()) continue;
        const auto& http = *one->streamable_http();
        if (http.session_header) reserved.names.push_back(*http.session_header);
        if (http.version_header) reserved.names.push_back(http.version_header->name);
        for (const auto& header : http.request_headers) reserved.names.push_back(header.name);
        if (http.argument_headers) reserved.prefixes.push_back(http.argument_headers->header_prefix);
    }
    return reserved;
}

std::optional<std::string> application_fields_problem(const std::vector<HttpHeader>& fields,
                                                      const ReservedFields& reserved, std::size_t max_fields,
                                                      std::size_t max_bytes) {
    if (fields.size() > max_fields) {
        return "more header fields of the application's own than max_header_fields allows";
    }
    std::set<std::string> seen;
    std::size_t bytes = 0;
    for (const HttpHeader& field : fields) {
        if (!is_field_name(field.name)) {
            return std::string("a header field of the application's own has a name that is not an HTTP token");
        }
        if (!is_field_value(field.value)) {
            return known_name(field.name) + " has a value that holds a control character";
        }
        if (reserved.covers(field.name)) {
            return known_name(field.name) + " is one the transport or the protocol writes, and cannot be set";
        }
        if (!seen.insert(lowered(field.name)).second) return known_name(field.name) + " is given twice";
        bytes += field.name.size() + field.value.size();
        if (bytes > max_bytes) {
            return std::string("the header fields of the application's own exceed max_header_field_bytes");
        }
    }
    return std::nullopt;
}

std::variant<ApplicationFields, std::string> ApplicationFields::make(const HttpEndpoint& endpoint,
                                                                     const std::vector<profile::ProfilePtr>& profiles) {
    if (endpoint.max_header_fields == 0) return std::string("max_header_fields is zero");
    if (endpoint.max_header_field_bytes == 0) return std::string("max_header_field_bytes is zero");
    ApplicationFields fields;
    fields.reserved_ = reserved_fields(profiles);
    fields.max_fields_ = endpoint.max_header_fields;
    fields.max_bytes_ = endpoint.max_header_field_bytes;
    if (auto problem = application_fields_problem(endpoint.header_fields, fields.reserved_, fields.max_fields_,
                                                  fields.max_bytes_)) {
        return std::move(*problem);
    }
    fields.fixed_ = endpoint.header_fields;
    fields.callback_ = endpoint.header_fields_for;
    return fields;
}

void ApplicationFields::add_to(Lines& lines, HttpRequestKind kind) const {
    if (lines.problem) return;
    std::vector<HttpHeader> given = fixed_;
    if (callback_) {
        try {
            std::vector<HttpHeader> supplied = callback_(kind);
            for (HttpHeader& field : supplied) given.push_back(std::move(field));
        } catch (const std::exception&) {
            lines.problem = "the callback of header fields threw";
            return;
        } catch (...) {
            lines.problem = "the callback of header fields threw";
            return;
        }
    }
    if (auto problem = application_fields_problem(given, reserved_, max_fields_, max_bytes_)) {
        lines.problem = std::move(*problem);
        return;
    }
    for (const HttpHeader& field : given) {
        for (const std::string& written : lines.names) {
            if (http::equal_ignoring_case(written, field.name)) {
                lines.problem =
                    known_name(field.name) + " is one the transport writes on this request, and cannot be set";
                return;
            }
        }
    }
    for (const HttpHeader& field : given) lines.add(field.name, field.value);
}

}  // namespace neomcp::detail::http_client
