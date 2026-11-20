// The client's Streamable HTTP transport (the target neomcp::client_http, built with NEOMCP_HTTP_CLIENT): what an
// application can give it besides the HttpEndpoint of neomcp/client/transport.h.
//
// The transport runs on libcurl, which no header of neomcp includes. It sends every message of the client as one POST
// to the endpoint's URL, with `Content-Type: application/json`, `Accept: application/json, text/event-stream` and the
// header fields the client's engine writes for the message, and reads the answer as one JSON body or as an event
// stream. It follows a redirect only with status 307 or 308, only to the endpoint's origin and with the method kept,
// at most HttpEndpoint::max_redirects times. Certificates are verified as libcurl verifies them, against the system's
// authorities or HttpEndpoint::ca_file; there is no option that turns verification off. Proxies are those of the
// environment unless HttpEndpoint::proxy says otherwise.
//
// Threads. Each transport runs one thread of its own on which all of its exchanges proceed; whatever it reads reaches
// the client on the client's thread.
#pragma once

#include <span>
#include <string>
#include <string_view>

#include "neomcp/client/engine.h"
#include "neomcp/client/transport.h"

namespace neomcp {

// One request of the transport, as the authorizer sees it.
struct HttpRequestInfo {
    std::string method;  // the HTTP method: POST (later also GET and DELETE)
    std::string url;     // where it goes
    UnitId unit = connection_unit;  // the engine's unit the request carries
};

// Gives the requests of an HTTP transport the authorization a server asks for, and hears every refusal for want of
// it. The transport asks it for the value of one header field, `Authorization`, right before each request is sent,
// and puts that value there and nowhere else: never into the URL, never into another field. Both methods are called
// on the client's thread and must return promptly.
//
// Without an authorizer (HttpEndpoint::authorizer null), a request the server refuses with 401 or 403 fails with
// authorization_required, and so does opening contact.
class HttpAuthorizer {
public:
    virtual ~HttpAuthorizer() = default;

    // The value of the `Authorization` field for the request; empty: the request goes without one.
    virtual std::string authorization(const HttpRequestInfo& request) = 0;

    // The server answered the request with 401 or 403. `challenges` are the values of its WWW-Authenticate fields, as
    // sent, as many as the bound on response header fields kept. The request itself still fails with
    // authorization_required.
    virtual void challenge(const HttpRequestInfo& request, int status, std::span<const std::string> challenges) = 0;
};

}  // namespace neomcp
