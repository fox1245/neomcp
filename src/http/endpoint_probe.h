// A view of the endpoint for the library's own tests: what it gives a session with each input. Internal to
// neomcp::http; no public header names it beyond a friend declaration.
#pragma once

#include <functional>
#include <memory>
#include <span>

#include "neomcp/authorization.h"
#include "neomcp/http/endpoint.h"
#include "neomcp/session.h"

namespace neomcp::http::detail {

struct EndpointProbe {
    // Told, on the feeding thread, the header fields and the authorization the endpoint passes to Session::receive.
    using FeedObserver = std::function<void(std::span<const HeaderField> headers, const Authorization* authorization)>;

    // Endpoint::create, with the observer in place before the endpoint serves anything.
    static Outcome<std::unique_ptr<Endpoint>> create(Server& server, HostPolicy policy, EndpointOptions options,
                                                     FeedObserver observer);

    // With fixed tokens: `observer` is told, on the request's thread with no lock held, after each request's token was
    // looked up and before the request goes on (so a test can replace the table between the two). Set before the
    // endpoint serves anything.
    static void observe_fixed_lookups(Endpoint& endpoint, std::function<void()> observer);

    // Returns once the endpoint's timer has run every job scheduled before the call (the sweep of a replacement, say).
    // Not from a job of the timer, and not while the endpoint stops.
    static void wait_for_timer(Endpoint& endpoint);
};

}  // namespace neomcp::http::detail
