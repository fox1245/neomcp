// Which codes an application may answer a request with (see application_error.h).
#include "core/application_error.h"

#include <algorithm>
#include <string>

#include "neomcp/jsonrpc.h"

namespace neomcp::detail {

namespace {

// The protocol's own codes are inside the range JSON-RPC reserves, and none of them is in the part of it that is left
// to implementations: the range check alone keeps an application off them.
constexpr int protocol_codes[] = {error_code::parse_error, error_code::invalid_request, error_code::method_not_found,
                                  error_code::invalid_params, error_code::internal_error};

constexpr bool reserved(int code) noexcept {
    return code >= error_code::reserved_range_first && code <= error_code::reserved_range_last;
}

constexpr bool left_to_implementations(int code) noexcept {
    return code >= error_code::implementation_range_first && code <= error_code::implementation_range_last;
}

constexpr bool protocol_codes_are_reserved() noexcept {
    for (const int code : protocol_codes) {
        if (!reserved(code) || left_to_implementations(code)) return false;
    }
    return true;
}
static_assert(protocol_codes_are_reserved());

}  // namespace

std::string refused_application_code(const profile::Profile& profile, int code) {
    const std::string text = std::to_string(code);
    if (reserved(code) && !left_to_implementations(code)) {
        return "the error code " + text + " is in the range JSON-RPC reserves for itself";
    }
    for (const profile::NamedCode& named : profile.error_codes()) {
        if (named.code == code) {
            return "the error code " + text + " is the code '" + named.name + "' of revision " + profile.revision();
        }
    }
    const auto not_emitted = profile.not_emitted();
    if (std::find(not_emitted.begin(), not_emitted.end(), code) != not_emitted.end()) {
        return "the error code " + text + " is one revision " + profile.revision() + " forbids a sender to use";
    }
    return {};
}

std::string oversized_application_error(const ApplicationFailure& failure, std::size_t size, std::size_t limit) {
    return "the error answer with the code " + std::to_string(failure.code) + " is " + std::to_string(size) +
           " bytes long, more than the " + std::to_string(limit) + " allowed";
}

}  // namespace neomcp::detail
