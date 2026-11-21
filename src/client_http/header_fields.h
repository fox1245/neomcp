// The header lines of the requests of an HTTP transport, and the header fields of the application's own among them
// (HttpEndpoint::header_fields and header_fields_for).
//
// The fields the application gives are checked as a field of HTTP is (RFC 9110, section 5: a token for a name, no
// control character but a tab in a value), bounded in count and in bytes, and refused when they name a field the
// transport or the protocol writes: the names the enabled profiles give (the header of the session id and of the
// protocol version, the fields that mirror a message's method, name or parameters), the fields of HTTP that frame a
// message, and any field the request already holds. Nothing is replaced and nothing is sent twice.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/client/engine.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"

namespace neomcp::detail::http_client {

// The header lines of a request, in libcurl's form (header_line), and why one cannot be sent.
struct Lines {
    std::vector<std::string> lines;
    std::optional<std::string> problem;
    std::vector<std::string> names;  // of the fields added by add(), as written

    // Adds one field; a field that cannot be sent as it is sets `problem` (the first one stays) and is left out.
    void add(std::string_view name, std::string_view value);
};

// The names the transport and the protocol write, which the application's own fields may not take.
struct ReservedFields {
    // Names, compared without regard to case.
    std::vector<std::string> names;
    // Prefixes: every field whose name begins with one (the fields that mirror a tool's arguments).
    std::vector<std::string> prefixes;

    bool covers(std::string_view name) const;
};

// The fields of HTTP the transport writes or that frame a message, which no application field may take whatever the
// profiles say.
std::span<const std::string_view> http_level_fields() noexcept;

// What the enabled profiles' Streamable HTTP entries write: the session header, the version header, the mirrored
// fields and the prefix of the argument fields; and http_level_fields(). `profiles` empty: every built-in profile.
ReservedFields reserved_fields(const std::vector<profile::ProfilePtr>& profiles);

// Why `fields` cannot be sent as the application's own, or nothing when they can: each name and value as HTTP field
// syntax, no name the reserved list covers, no name twice, at most `max_fields` and `max_bytes` (names and values
// together). Never echoes a value, nor a name that is not a token.
std::optional<std::string> application_fields_problem(const std::vector<HttpHeader>& fields,
                                                      const ReservedFields& reserved, std::size_t max_fields,
                                                      std::size_t max_bytes);

// The application's fields of one transport: the endpoint's fixed ones and its callback, with the rules above.
class ApplicationFields {
public:
    // No fields.
    ApplicationFields() = default;

    // Why the endpoint's fixed fields or bounds cannot be used, otherwise the fields for the endpoint.
    static std::variant<ApplicationFields, std::string> make(const HttpEndpoint& endpoint,
                                                             const std::vector<profile::ProfilePtr>& profiles);

    // Adds the fields of one request of this kind to `lines`: the fixed ones, then the callback's, checked together
    // and against what `lines` holds already. When they cannot be sent, `lines.problem` says why and none is added.
    void add_to(Lines& lines, HttpRequestKind kind) const;

private:
    std::vector<HttpHeader> fixed_;
    HttpFieldsCallback callback_;
    ReservedFields reserved_;
    std::size_t max_fields_ = HttpClientDefaults::max_header_fields;
    std::size_t max_bytes_ = HttpClientDefaults::max_header_field_bytes;
};

}  // namespace neomcp::detail::http_client
