// The reading of an introspection answer (RFC 7662, section 2.2) into a verdict, without I/O. Internal to
// neomcp::server_auth_introspection; include/neomcp/http/introspection.h says what is accepted.
#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

#include "neomcp/http/introspection.h"
#include "neomcp/http/resource_server.h"

namespace neomcp::http::detail {

// What the reading needs besides the answer.
struct IntrospectionContext {
    std::string_view issuer;  // reported for an active token, and the only `iss` accepted
    std::chrono::system_clock::time_point now;
    std::chrono::seconds default_lifetime{0};  // `expires - now` of an active answer without `exp`
    std::size_t max_bytes = 0;                 // the body bound
};

struct IntrospectionReading {
    TokenVerdict verdict{TokenRefusal::unavailable};
    IntrospectionEvent event = IntrospectionEvent::transport_failed;
};

// The verdict for an answer with `status` and `body`. Any status but 200, a body over `max_bytes`, and a body that is
// not one JSON object give `unavailable`; an object that is inactive or whose members cannot be read gives `invalid`;
// an active one gives an Authorization.
IntrospectionReading read_introspection_answer(int status, std::string_view body, const IntrospectionContext& context);

}  // namespace neomcp::http::detail
