// Input handlers whose form the library cannot read. NEOMCP_CASE selects the handler that is registered:
//   0  handlers of every form, plain and coroutine: this must compile (it shows that the cases below fail for their
//      handler and not for the file)
//   1  a lambda whose parameter is `auto` (a template)    2  a form handler that returns a string
//   3  a roots handler that takes a request
// Cases 1 to 3 must not compile: each stops at the static_assert that says what a handler of its kind looks like.
#include <string>

#include <asio/awaitable.hpp>

#include "neomcp/client/input_handlers.h"

namespace cc {

void register_handlers(neomcp::InputHandlers& handlers) {
#if NEOMCP_CASE == 0
    handlers.form([](const neomcp::ElicitationForm&, neomcp::InputContext&) { return neomcp::ElicitationAnswer{}; })
        .url([](neomcp::ElicitationUrl) -> asio::awaitable<neomcp::ElicitationAnswer> { co_return neomcp::ElicitationAnswer{}; })
        .sampling([](const neomcp::SamplingRequest&) -> neomcp::SamplingReply { return neomcp::InputRefused{}; })
        .roots([](neomcp::InputContext&) { return neomcp::RootsAnswer{}; });
#elif NEOMCP_CASE == 1
    handlers.form([](const auto&) { return neomcp::ElicitationAnswer{}; });
#elif NEOMCP_CASE == 2
    handlers.form([](const neomcp::ElicitationForm&) { return std::string("yes"); });
#elif NEOMCP_CASE == 3
    handlers.roots([](const neomcp::RootsRequest&) { return neomcp::RootsAnswer{}; });
#else
#error "NEOMCP_CASE must be 0 to 3"
#endif
}

}  // namespace cc
