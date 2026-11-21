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
};

}  // namespace neomcp::http::detail
