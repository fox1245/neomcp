#include "profile/internal.h"

#include <array>

namespace neomcp::profile::detail {
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

}  // namespace neomcp::profile::detail
