// The kinds of a client's errors (neomcp/client/result.h) and how the engine's failures map to them.
#include "neomcp/client/result.h"

namespace neomcp {

std::string_view to_string(ClientErrorKind kind) noexcept {
    switch (kind) {
        case ClientErrorKind::server_error: return "server_error";
        case ClientErrorKind::timed_out: return "timed_out";
        case ClientErrorKind::cancelled: return "cancelled";
        case ClientErrorKind::server_exited: return "server_exited";
        case ClientErrorKind::stream_lost: return "stream_lost";
        case ClientErrorKind::send_failed: return "send_failed";
        case ClientErrorKind::http_status: return "http_status";
        case ClientErrorKind::bad_content_type: return "bad_content_type";
        case ClientErrorKind::authorization_required: return "authorization_required";
        case ClientErrorKind::message_too_large: return "message_too_large";
        case ClientErrorKind::malformed_answer: return "malformed_answer";
        case ClientErrorKind::version_refused: return "version_refused";
        case ClientErrorKind::no_common_revision: return "no_common_revision";
        case ClientErrorKind::too_many_pending: return "too_many_pending";
        case ClientErrorKind::unsupported_method: return "unsupported_method";
        case ClientErrorKind::header_value: return "header_value";
        case ClientErrorKind::input_timed_out: return "input_timed_out";
        case ClientErrorKind::input_rounds_exceeded: return "input_rounds_exceeded";
        case ClientErrorKind::input_requests_exceeded: return "input_requests_exceeded";
        case ClientErrorKind::server_asked_undeclared: return "server_asked_undeclared";
        case ClientErrorKind::form_not_restricted: return "form_not_restricted";
        case ClientErrorKind::answer_invalid: return "answer_invalid";
        case ClientErrorKind::input_refused: return "input_refused";
        case ClientErrorKind::handler_failed: return "handler_failed";
        case ClientErrorKind::state_not_echoable: return "state_not_echoable";
        case ClientErrorKind::answer_not_writable: return "answer_not_writable";
        case ClientErrorKind::closed: return "closed";
        case ClientErrorKind::unverified: return "unverified";
        case ClientErrorKind::output_invalid: return "output_invalid";
        case ClientErrorKind::output_undecodable: return "output_undecodable";
        case ClientErrorKind::no_structured_results: return "no_structured_results";
        case ClientErrorKind::list_pages_exceeded: return "list_pages_exceeded";
        case ClientErrorKind::list_items_exceeded: return "list_items_exceeded";
        case ClientErrorKind::cursor_not_echoable: return "cursor_not_echoable";
        case ClientErrorKind::subscription_lost: return "subscription_lost";
        case ClientErrorKind::subscription_ended: return "subscription_ended";
        case ClientErrorKind::too_many_subscriptions: return "too_many_subscriptions";
        case ClientErrorKind::unserved_revision: return "unserved_revision";
        case ClientErrorKind::handshake_refused: return "handshake_refused";
        case ClientErrorKind::handshake_timed_out: return "handshake_timed_out";
        case ClientErrorKind::transport: return "transport";
        case ClientErrorKind::too_many_restarts: return "too_many_restarts";
        case ClientErrorKind::on_client_thread: return "on_client_thread";
        case ClientErrorKind::invalid_options: return "invalid_options";
    }
    return "unknown";
}

std::optional<ClientErrorKind> error_kind(CallFailureKind kind) noexcept {
    switch (kind) {
        case CallFailureKind::timed_out: return ClientErrorKind::timed_out;
        case CallFailureKind::cancelled: return ClientErrorKind::cancelled;
        case CallFailureKind::server_exited: return ClientErrorKind::server_exited;
        case CallFailureKind::stream_lost: return ClientErrorKind::stream_lost;
        case CallFailureKind::send_failed: return ClientErrorKind::send_failed;
        case CallFailureKind::http_status: return ClientErrorKind::http_status;
        case CallFailureKind::bad_content_type: return ClientErrorKind::bad_content_type;
        case CallFailureKind::authorization_required: return ClientErrorKind::authorization_required;
        case CallFailureKind::message_too_large: return ClientErrorKind::message_too_large;
        case CallFailureKind::malformed_answer: return ClientErrorKind::malformed_answer;
        case CallFailureKind::version_refused: return ClientErrorKind::version_refused;
        case CallFailureKind::no_common_revision: return ClientErrorKind::no_common_revision;
        case CallFailureKind::contact_failed: return std::nullopt;
        case CallFailureKind::too_many_pending: return ClientErrorKind::too_many_pending;
        case CallFailureKind::unsupported_method: return ClientErrorKind::unsupported_method;
        case CallFailureKind::header_value: return ClientErrorKind::header_value;
        case CallFailureKind::input_timed_out: return ClientErrorKind::input_timed_out;
        case CallFailureKind::input_rounds_exceeded: return ClientErrorKind::input_rounds_exceeded;
        case CallFailureKind::input_requests_exceeded: return ClientErrorKind::input_requests_exceeded;
        case CallFailureKind::server_asked_undeclared: return ClientErrorKind::server_asked_undeclared;
        case CallFailureKind::form_not_restricted: return ClientErrorKind::form_not_restricted;
        case CallFailureKind::answer_invalid: return ClientErrorKind::answer_invalid;
        case CallFailureKind::input_refused: return ClientErrorKind::input_refused;
        case CallFailureKind::handler_failed: return ClientErrorKind::handler_failed;
        case CallFailureKind::state_not_echoable: return ClientErrorKind::state_not_echoable;
        case CallFailureKind::answer_not_writable: return ClientErrorKind::answer_not_writable;
        case CallFailureKind::closed: return ClientErrorKind::closed;
    }
    return ClientErrorKind::closed;
}

ClientErrorKind error_kind(ContactFailureKind kind) noexcept {
    switch (kind) {
        case ContactFailureKind::no_common_revision: return ClientErrorKind::no_common_revision;
        case ContactFailureKind::unserved_revision: return ClientErrorKind::unserved_revision;
        case ContactFailureKind::handshake_refused: return ClientErrorKind::handshake_refused;
        case ContactFailureKind::handshake_timed_out: return ClientErrorKind::handshake_timed_out;
        case ContactFailureKind::malformed_answer: return ClientErrorKind::malformed_answer;
        case ContactFailureKind::authorization_required: return ClientErrorKind::authorization_required;
        case ContactFailureKind::http_status: return ClientErrorKind::http_status;
        case ContactFailureKind::transport: return ClientErrorKind::transport;
        case ContactFailureKind::too_many_restarts: return ClientErrorKind::too_many_restarts;
    }
    return ClientErrorKind::transport;
}

}  // namespace neomcp
