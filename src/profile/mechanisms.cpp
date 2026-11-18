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

}  // namespace neomcp::profile
