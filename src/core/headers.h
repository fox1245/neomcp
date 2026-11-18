// What a transport's header fields claim about an input, checked against a profile's data. Shared by the sources of
// the core; not part of the public interface.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "neomcp/jsonrpc.h"
#include "neomcp/profile.h"
#include "neomcp/session.h"

namespace neomcp::detail {

// Whether two header names are the same, ignoring ASCII case.
bool same_header_name(std::string_view a, std::string_view b) noexcept;

// The fields of one name: how many there are, and the value of the first.
struct FieldLookup {
    std::size_t count = 0;
    std::string_view value;
};
FieldLookup find_field(std::span<const HeaderField> fields, std::string_view name) noexcept;

// Whether a request header of the profile applies to a request of this method.
bool applies_to_request(const profile::RequestHeader& header, std::string_view method);

// The name of a header that carries the revision and that every request of this method must have; empty when the
// headers require none.
std::optional<std::string_view> required_version_header(std::span<const profile::RequestHeader> headers,
                                                        std::string_view method);

// Checks the fields against every request header that applies to the request under a profile of `revision`: each must
// be present once and equal what the body says (the revision, the method, or a params member). A params member's
// value in the encoded form is decoded first. Returns the message of the first failure; empty when all hold.
std::optional<std::string> check_request_headers(const Message& request, std::span<const HeaderField> fields,
                                                 std::span<const profile::RequestHeader> headers,
                                                 const profile::EncodedHeaderValue* encoded,
                                                 std::string_view revision);

// The prefix of every message about a header that fails its rule.
inline constexpr std::string_view header_mismatch_prefix = "Header mismatch: ";

}  // namespace neomcp::detail
