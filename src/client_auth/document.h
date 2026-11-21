// The JSON documents and answers of the OAuth flow: reading a body as one JSON object within its bounds, the members
// a flow reads, and the error code of an error answer.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "client_auth/types.h"
#include "neomcp/json.h"

namespace neomcp::detail::oauth {

// The body as a JSON object: within `max_depth` levels of nesting (checked before it is parsed), JSON, and an
// object. Nothing otherwise. The caller bounds the bytes.
std::optional<json> json_object(std::string_view body, std::size_t max_depth);

// The Content-Type is application/json (parameters such as a charset do not matter).
bool is_json_media_type(std::string_view content_type);

// What the answer to a metadata request means.
struct MetadataAnswer {
    enum class Kind {
        document,  // a JSON object; `document` holds it
        next,      // 404 or another 4xx except 429: the next candidate URL is tried
        failure    // anything else ends the flow; `problem` says how
    };
    Kind kind = Kind::failure;
    json document;
    AuthorizationProblem problem = AuthorizationProblem::discovery_failed;
};

// A 2xx answer with media type application/json whose body is a JSON object within the bounds is a document; 404
// and the other 4xx answers except 429 send the flow to the next candidate; a 5xx, a 429, any other status, a body
// that is not a JSON object, a body over `max_bytes` and a request that got no answer end it.
MetadataAnswer read_metadata_answer(const FetchResult& result, std::size_t max_bytes, std::size_t max_depth);

// The member `name` of `object` as a string; nothing when absent or not a string.
const std::string* string_member(const json& object, std::string_view name);
// The member `name` of `object`; null when absent.
const json* member(const json& object, std::string_view name);
// The member `name` is the boolean true.
bool true_member(const json& object, std::string_view name);
// The member `name` as a list of strings: nothing when absent or when it is not an array of strings only.
std::optional<std::vector<std::string>> string_array_member(const json& object, std::string_view name);

// An error code as OAuth defines it: one or more of %x20-21, %x23-5B and %x5D-7E (no '"', no '\', no control
// character), of at most `max_bytes` bytes. A longer code is not shortened: a shortened code would be another code.
bool is_error_code(std::string_view code, std::size_t max_bytes) noexcept;

// The `error` code of an error answer's JSON object, when it is one; nothing otherwise. The description and the URI
// of the error are never read.
std::optional<std::string> error_code_of(std::string_view body, std::size_t max_depth, std::size_t max_code_bytes);

inline bool is_success_status(int status) noexcept { return status >= 200 && status <= 299; }
inline bool is_client_error_status(int status) noexcept { return status >= 400 && status <= 499; }

}  // namespace neomcp::detail::oauth
