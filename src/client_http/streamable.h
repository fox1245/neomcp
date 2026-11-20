// The Streamable HTTP transport of the client (neomcp/client/http.h), as the target builds it: the factory with the
// settings it takes from the client's options, for the open forms of this target and for tests.
#pragma once

#include <cstddef>
#include <memory>
#include <variant>
#include <vector>

#include "client_http/transfers.h"
#include "client_http/url_guard.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/mechanism.h"
#include "neomcp/client/result.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"

namespace neomcp::detail::http_client {

// What the transport takes from the client's options besides the endpoint.
struct HttpTransportSettings {
    // The revisions the client speaks; empty: every built-in profile. The transport reads from them where a session
    // id comes from and the name of its header.
    std::vector<profile::ProfilePtr> profiles;
    // Where the client halves come from; null: builtin_client_mechanisms().
    const ClientMechanismRegistry* mechanisms = nullptr;
    // The bound of one message: a JSON body, or the data of one event.
    std::size_t max_message_bytes = ClientDefaults::max_message_bytes;
};

// An HTTP transport for `endpoint`. Fails with invalid_options for a URL or options that cannot work, and with
// transport for an HTTP library that cannot serve it.
Result<std::unique_ptr<Transport>> make_http_transport(HttpEndpoint endpoint, HttpTransportSettings settings);

// What every HTTP transport of this target makes of an endpoint before it starts: the guard of the URLs a server
// supplies, the bounds of the transfer thread, and libcurl's global state.
struct HttpParts {
    UrlGuard guard;
    TransferLimits limits;
    std::shared_ptr<CurlGlobal> global;
};

// The parts for `endpoint`, or why it cannot serve: as make_http_transport fails.
std::variant<HttpParts, ClientError> prepare_http(const HttpEndpoint& endpoint, const HttpTransportSettings& settings);

}  // namespace neomcp::detail::http_client
