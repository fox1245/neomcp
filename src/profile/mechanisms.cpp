// The single place where the library declares a name: a profile that selects any other name is rejected, and code
// that implements a mechanism attaches to a name here.
#include <array>

#include "neomcp/profile.h"

namespace neomcp::profile {
namespace {

using Names = std::span<const std::string_view>;

constexpr std::array<std::string_view, 2> open_names{"initialize", "request_meta"};
constexpr std::array<std::string_view, 2> state_names{"connection", "none"};
constexpr std::array<std::string_view, 2> server_input_names{"server_request", "input_required_result"};
constexpr std::array<std::string_view, 2> change_feed_names{"connection_notifications", "subscriptions_listen"};
constexpr std::array<std::string_view, 2> batch_names{"array", "none"};

constexpr std::array<std::string_view, 1> stdio_cancel_names{"cancelled_notification"};
constexpr std::array<std::string_view, 1> http_sse_cancel_names{"cancelled_notification"};
constexpr std::array<std::string_view, 2> streamable_http_cancel_names{"cancelled_notification",
                                                                       "close_response_stream"};

// error_result: a tool result with isError set; invalid_params_error: the JSON-RPC error Invalid params.
constexpr std::array<std::string_view, 2> invalid_arguments_names{"error_result", "invalid_params_error"};

// How a client finds the authorization server: protected_resource_metadata, the resource's metadata (RFC 9728) that
// names the authorization server, whose own metadata (RFC 8414) follows; origin_metadata, the authorization server's
// metadata on the origin of the server's URL, then default endpoints there.
constexpr std::array<std::string_view, 2> authorization_discovery_names{"origin_metadata",
                                                                        "protected_resource_metadata"};

// The reasons a Streamable HTTP status map can name. The first group are the reasons a session gives when it refuses
// an input before any method ran (neomcp::RefusalReason spells them the same way); the rest are decided by the HTTP
// transport itself.
constexpr std::array<std::string_view, 19> status_reason_names{
    "parse_error",
    "invalid_request",
    "too_long",
    "too_deep",
    "header_mismatch",
    "unsupported_version",
    "invalid_request_meta",
    "unsupported_version_header",
    "method_not_found",
    "missing_connection",
    "missing_client_capability",  // a request needs a client capability the client did not declare
    "insufficient_scope",         // the request's authorization lacks a scope its item declares
    "missing_session",            // a request without a session id where the revision requires one
    "unknown_session",            // a session id that names no live session
    "accepted",                   // a notification or a response was taken; nothing answers it
    "notification_refused",       // a notification was not taken
    "origin_rejected",            // the Origin header names an origin that is not allowed
    "get_stream_not_offered",     // a GET while no stream is offered
    "delete_not_allowed",         // a DELETE while ending a session that way is not allowed
};

}  // namespace

Names declared_mechanisms(Slot slot) {
    switch (slot) {
        case Slot::open: return open_names;
        case Slot::state: return state_names;
        case Slot::server_input: return server_input_names;
        case Slot::change_feed: return change_feed_names;
        case Slot::batch: return batch_names;
    }
    return {};
}

Names declared_cancel(Transport transport) {
    switch (transport) {
        case Transport::stdio: return stdio_cancel_names;
        case Transport::http_sse: return http_sse_cancel_names;
        case Transport::streamable_http: return streamable_http_cancel_names;
    }
    return {};
}

Names declared_invalid_arguments() { return invalid_arguments_names; }

Names declared_status_reasons() { return status_reason_names; }

Names declared_authorization_discovery() { return authorization_discovery_names; }

}  // namespace neomcp::profile
