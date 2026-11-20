// The client's side of the older transport of events over HTTP (neomcp/client/http.h), as this target builds it: the
// factory with the settings it takes from the client's options, for the open forms of this target and for tests.
#pragma once

#include <memory>

#include "client_http/streamable.h"
#include "neomcp/client/result.h"
#include "neomcp/client/transport.h"

namespace neomcp::detail::http_client {

// A transport of events over HTTP for `endpoint`: its event stream is a GET of the endpoint's URL. Fails as
// make_http_transport does, and with invalid_options when no enabled profile defines the transport, or when the
// enabled profiles that define it name its events differently (the events are read before a revision is chosen).
Result<std::unique_ptr<Transport>> make_http_sse_transport(HttpEndpoint endpoint, HttpTransportSettings settings);

}  // namespace neomcp::detail::http_client
