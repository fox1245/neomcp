// A TokenValidator that asks the authorization server what an access token says: token introspection (RFC 7662). It is
// a component of its own (neomcp::server_auth_introspection), built with the HTTP client's libcurl dependency
// (NEOMCP_HTTP_CLIENT); the header names no libcurl type.
//
// The request. For each token the endpoint has no verdict for, the validator posts a form to the introspection
// endpoint the application configures (RFC 7662, section 2.1):
//
//   token=<the token>&token_type_hint=access_token         (form-encoded, application/x-www-form-urlencoded)
//
// and authenticates as a protected resource, as the application configures (section 2.1 requires it):
//
//   IntrospectionBasicAuth   HTTP Basic with the client identifier and secret form-encoded first (the credentials of
//                            RFC 6749 section 2.3.1), as OAuth 2.1 writes them
//   IntrospectionPostAuth    `client_id` and `client_secret` as two more fields of the form
//   IntrospectionBearerAuth  an access token of its own in `Authorization: Bearer`
//
// The endpoint must be an https URL, or an http URL with a loopback host; it has no user information and no fragment.
// The certificate of an https endpoint is always verified. No redirect is followed: a redirect is an answer the
// validator cannot use. Unless IntrospectionOptions::use_proxy_from_environment is set, no proxy is used.
//
// The answer. A response with status 200 whose body is one JSON object within IntrospectionBounds::max_response_bytes
// is read as follows (RFC 7662, section 2.2). The object is refused when it names a member twice.
//
//   active   must be present and the JSON value `true`; `false` is the inactive answer. Nothing else of an inactive
//            answer is read. Any other value is a malformed answer.
//   scope    a string of scope-tokens separated by spaces (RFC 6749, section 3.3), or absent
//   aud      a string or an array of strings, or absent (an absent audience leaves `audiences` empty, which the
//            endpoint refuses: the audience of a token is something the endpoint checks, never assumes)
//   iss      a string equal to IntrospectionOptions::issuer, or absent
//   exp, nbf JSON numbers (NumericDate, seconds since the epoch, fractions allowed), or absent. `exp` goes to
//            `Authorization::expires` and `nbf` to `not_before` as they are. An answer without `exp` gets
//            `expires = now + default_lifetime`, so that the token is checked again soon (the endpoint refuses a
//            token without an expiry). A number too far from the epoch to be a time of the system clock is held to
//            the nearest time the clock can count, a day inside its limit.
//   sub, client_id   strings, or absent
//
// The verdict.
//
//   a malformed or inactive answer      TokenRefusal::invalid (the token is not accepted: 401)
//   no answer to use                    TokenRefusal::unavailable (503; nothing is remembered): the endpoint cannot be
//                                       reached, does not answer within request_timeout, answers with another status
//                                       than 200, with a body over max_response_bytes, or with a body that is not one
//                                       JSON object
//   an active answer                    an Authorization with `issuer` set to IntrospectionOptions::issuer, which
//                                       must be one of the server's `authorization_servers`. The endpoint then applies
//                                       its own checks (audience, issuer, expiry, scopes) as it does for every
//                                       validator.
//
// Secrets. Neither the token nor the client's credentials appear in any error text, in the observer's reports or in
// anything this component writes: reports name a kind of event and nothing else.
//
// Threads. One thread of its own runs the transfers (libcurl's multi interface), and a second one hands the verdicts
// to the endpoint, so that the endpoint's work on a verdict never delays another transfer. validate() returns at once.
// The validator may be used by several requests at once. Dropping it answers every request still waiting
// `unavailable`.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "neomcp/http/outcome.h"
#include "neomcp/http/resource_server.h"

namespace neomcp::http {

// Authentication of the protected resource at the introspection endpoint.
struct IntrospectionBasicAuth {
    std::string client_id;      // not empty
    std::string client_secret;  // not empty
};
struct IntrospectionPostAuth {
    std::string client_id;      // not empty
    std::string client_secret;  // not empty
};
struct IntrospectionBearerAuth {
    std::string token;  // not empty; a header field value
};
using IntrospectionAuth = std::variant<IntrospectionBasicAuth, IntrospectionPostAuth, IntrospectionBearerAuth>;

// The defaults of IntrospectionBounds, as constants.
struct IntrospectionDefaults {
    static constexpr std::size_t max_response_bytes = std::size_t{64} * 1024;
    // Shorter than ResourceServerDefaults::validation_timeout, so that an answer that comes late still comes in time
    // for the endpoint to use it.
    static constexpr std::chrono::milliseconds request_timeout{8000};
    static constexpr std::chrono::milliseconds connect_timeout{3000};
    static constexpr std::size_t max_response_header_bytes = std::size_t{16} * 1024;
    static constexpr std::size_t max_pending = ResourceServerDefaults::max_pending_validations;
    // The life given to an active answer that has no `exp`: the longest the endpoint remembers a valid verdict.
    static constexpr std::chrono::seconds default_lifetime = ResourceServerDefaults::max_cache_age;
};

struct IntrospectionBounds {
    // The bytes of an answer's body; a longer one is not read further and the request is `unavailable`. At least 1.
    std::size_t max_response_bytes = IntrospectionDefaults::max_response_bytes;
    // The time one request may take from its start to the end of its body. Positive.
    std::chrono::milliseconds request_timeout = IntrospectionDefaults::request_timeout;
    // The time connecting may take; not more than request_timeout. Positive.
    std::chrono::milliseconds connect_timeout = IntrospectionDefaults::connect_timeout;
    // The bytes of the answer's header fields. At least 1.
    std::size_t max_response_header_bytes = IntrospectionDefaults::max_response_header_bytes;
    // Requests in progress at once; one more is `unavailable`. At least 1.
    std::size_t max_pending = IntrospectionDefaults::max_pending;
    // See above. Positive.
    std::chrono::seconds default_lifetime = IntrospectionDefaults::default_lifetime;
};

// What happened to one request, for an application that counts them. The kinds carry no text of the exchange.
enum class IntrospectionEvent {
    active,                // an active answer: the verdict is an Authorization
    inactive,              // `active` is false: invalid
    malformed,             // an object with a member that cannot be read, or a name twice: invalid
    issuer_mismatch,       // `iss` is not the configured issuer: invalid
    transport_failed,      // the endpoint could not be reached or did not answer in time: unavailable
    unexpected_status,     // another status than 200: unavailable
    response_too_large,    // the body or the header fields exceed their bound: unavailable
    not_an_object,         // the body is not one JSON object, or could not be read: unavailable
    overloaded,            // max_pending requests were in progress: unavailable
    stopped                // the validator was dropped while the request waited: unavailable
};

// The name of an event, for a person to read.
std::string_view to_string(IntrospectionEvent event) noexcept;

struct IntrospectionOptions {
    // The introspection endpoint (RFC 7662): see above for what it may be.
    std::string endpoint;
    // The issuer the validator reports for every active token (Authorization::issuer), and the only `iss` an answer
    // may name. One of ResourceServerOptions::authorization_servers. Not empty.
    std::string issuer;
    IntrospectionAuth authentication;
    IntrospectionBounds bounds{};
    // A file of certificate authorities to verify an https endpoint with, instead of the system's. Verification itself
    // cannot be switched off.
    std::optional<std::filesystem::path> ca_file;
    // Let libcurl use the proxy the environment names (http_proxy, https_proxy, no_proxy). By default none is used.
    bool use_proxy_from_environment = false;
    // The library resolving host names must resolve them asynchronously, or one slow name would hold every request
    // of the validator; set this to accept a library that blocks.
    bool allow_blocking_resolver = false;
    // Told of the end of each request, on the thread that hands verdicts over; it must return promptly. Optional.
    std::function<void(IntrospectionEvent)> observer;
    // The time of an answer without `exp`; empty: the system clock. For tests.
    std::function<std::chrono::system_clock::time_point()> clock;
};

// Makes the validator, or says (without any of the options' secrets) what is wrong with them.
Outcome<std::shared_ptr<TokenValidator>> make_introspection_validator(IntrospectionOptions options);

}  // namespace neomcp::http
