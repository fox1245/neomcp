// JSON documents and answers of the OAuth flow (document.h).
#include "client_auth/document.h"

#include <utility>

#include "neomcp/http/media_type.h"
#include "neomcp/jsonrpc.h"

namespace neomcp::detail::oauth {

namespace {

constexpr std::string_view media_type_application = "application";
constexpr std::string_view media_subtype_json = "json";
constexpr std::string_view error_member = "error";
// Too many requests: a server that asks the client to slow down is not one whose next candidate URL is tried.
constexpr int status_too_many_requests = 429;

}  // namespace

std::optional<json> json_object(std::string_view body, std::size_t max_depth) {
    if (nesting_exceeds(body, max_depth)) return std::nullopt;
    json value = json::parse(body, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return std::nullopt;
    return value;
}

bool is_json_media_type(std::string_view content_type) {
    return neomcp::http::is_media_type(content_type, media_type_application, media_subtype_json);
}

MetadataAnswer read_metadata_answer(const FetchResult& result, std::size_t max_bytes, std::size_t max_depth) {
    MetadataAnswer answer;
    switch (result.outcome) {
        case FetchOutcome::response: break;
        case FetchOutcome::timed_out: answer.problem = AuthorizationProblem::timed_out; return answer;
        case FetchOutcome::too_large: answer.problem = AuthorizationProblem::document_too_large; return answer;
        case FetchOutcome::transport_error: answer.problem = AuthorizationProblem::discovery_failed; return answer;
    }
    if (is_client_error_status(result.status) && result.status != status_too_many_requests) {
        answer.kind = MetadataAnswer::Kind::next;
        return answer;
    }
    if (!is_success_status(result.status)) return answer;
    if (result.body.size() > max_bytes) {
        answer.problem = AuthorizationProblem::document_too_large;
        return answer;
    }
    if (!is_json_media_type(result.content_type)) return answer;
    auto document = json_object(result.body, max_depth);
    if (!document) return answer;
    answer.kind = MetadataAnswer::Kind::document;
    answer.document = std::move(*document);
    return answer;
}

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

const std::string* string_member(const json& object, std::string_view name) {
    const json* found = member(object, name);
    return found == nullptr ? nullptr : found->get_ptr<const std::string*>();
}

bool true_member(const json& object, std::string_view name) {
    const json* found = member(object, name);
    return found != nullptr && found->is_boolean() && found->get<bool>();
}

std::optional<std::vector<std::string>> string_array_member(const json& object, std::string_view name) {
    const json* found = member(object, name);
    if (found == nullptr) return std::nullopt;
    const auto* items = found->get_ptr<const json::array_t*>();
    if (items == nullptr) return std::nullopt;
    std::vector<std::string> out;
    out.reserve(items->size());
    for (const json& item : *items) {
        const auto* text = item.get_ptr<const std::string*>();
        if (text == nullptr) return std::nullopt;
        out.push_back(*text);
    }
    return out;
}

bool is_error_code(std::string_view code, std::size_t max_bytes) noexcept {
    if (code.empty() || code.size() > max_bytes) return false;
    for (const char c : code) {
        const auto value = static_cast<unsigned char>(c);
        const bool allowed = value == 0x20 || value == 0x21 || (value >= 0x23 && value <= 0x5B) ||
                             (value >= 0x5D && value <= 0x7E);
        if (!allowed) return false;
    }
    return true;
}

std::optional<std::string> error_code_of(std::string_view body, std::size_t max_depth, std::size_t max_code_bytes) {
    const auto document = json_object(body, max_depth);
    if (!document) return std::nullopt;
    const std::string* code = string_member(*document, error_member);
    if (code == nullptr || !is_error_code(*code, max_code_bytes)) return std::nullopt;
    return *code;
}

}  // namespace neomcp::detail::oauth
