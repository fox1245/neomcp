// The client's Streamable HTTP transport (the target neomcp::client_http, built with NEOMCP_HTTP_CLIENT): what an
// application can give it besides the HttpEndpoint of neomcp/client/transport.h.
//
// The transport runs on libcurl, which no header of neomcp includes. It sends every message of the client as one POST
// to the endpoint's URL, with `Content-Type: application/json`, `Accept: application/json, text/event-stream` and the
// header fields the client's engine writes for the message, and reads the answer as one JSON body or as an event
// stream. Under a revision with sessions it sends the session id of the handshake's answer on every later request,
// opens a new session when the server no longer knows that one, and ends the session with a DELETE when the client
// closes. Where the revision offers it and the client needs it, it holds the stream on which the server speaks outside
// any request (a GET), and it resumes an event stream that ends before its answer (a GET that names the last event
// id), as HttpEndpoint describes. It follows a redirect only with status 307 or 308, only to the endpoint's origin and
// with the method kept,
// at most HttpEndpoint::max_redirects times. Certificates are verified as libcurl verifies them, against the system's
// authorities or HttpEndpoint::ca_file; there is no option that turns verification off. Proxies are those of the
// environment unless HttpEndpoint::proxy says otherwise.
//
// The older transport of events over HTTP (the revision 2024-11-05). Client::open with an HttpEndpoint speaks it where
// an enabled revision defines it and the endpoint refuses the handshake of Streamable HTTP with 400, 404 or 405 (or
// where the verdict given as ClientOptions::prior names it, or no enabled revision defines Streamable HTTP). The
// client then holds a GET of the endpoint's URL open as an event stream; its first event, within
// HttpEndpoint::endpoint_timeout, names where the client's messages go: a URI reference, resolved against the
// endpoint's URL, that must keep the endpoint's origin and carry no user information. Every message is a POST there,
// and the server's messages arrive as events of the stream; the names of both events are the profile's. A stream that
// ends ends the connection: the next request opens a new one and repeats the handshake. make_transport() makes only
// the Streamable HTTP transport and never hands over.
//
// Authorization. Both transports take an HttpAuthorizer (HttpEndpoint::authorizer). Right before each request goes out
// the transport asks it for the value of one header field, `Authorization`, and puts that value there and nowhere
// else: never into the URL, never into another field, never on a request to another origin than the endpoint's, and
// never on any request when the endpoint is plain HTTP on a host that is not this machine. A request the server
// refuses with 401 or 403 is not failed: the transport holds it (its call's clocks wait) and tells the authorizer,
// which answers later through a ChallengeReply. Every request refused while the authorizer works on the same
// credentials waits for that one answer, so that one sign-in serves them all. On retry each is sent again, the same
// bytes with the same JSON-RPC id, with the authorizer's new value; on fail each ends with the authorizer's error.
// Closing the client ends every held request `closed` and detaches the authorizer.
//
// Threads. Each transport runs one thread of its own on which all of its exchanges proceed; whatever it reads reaches
// the client on the client's thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include "neomcp/client/engine.h"
#include "neomcp/client/result.h"
#include "neomcp/client/transport.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail::http_client {
class ReplyChannel;
class AuthorizerClaim;
}  // namespace detail::http_client

// One request of the transport, as the authorizer sees it.
struct HttpRequestInfo {
    std::string method;  // the HTTP method: POST, GET (the stream of the connection, a resumption) or DELETE
    std::string url;     // where it goes
    UnitId unit = connection_unit;  // the engine's unit the request carries (connection_unit for the stream of the
                                    // connection; the transport's own for the DELETE that ends a session)
    profile::ProfilePtr profile;    // the profile it goes out under; null when the engine named none
};

// What an authorizer gives a request: the value of its `Authorization` field (empty: the request goes without one), and
// the generation of the credentials it came from, which the transport hands back with a refusal of that request.
// Generations are the authorizer's own count; the transport only compares them.
struct Credential {
    std::string value;
    std::uint64_t generation = 0;
};

// The answer to one refusal (HttpAuthorizer::challenge): retry (new credentials exist; the held requests are sent
// again) or fail (they end with the error). Answered once; later calls are ignored. Dropped without an answer it
// fails with authorization_failed (no_answer). Any thread may answer: the answer reaches the transport on the client's
// thread, never from inside the call that answers. A reply whose transport closed is inert.
class ChallengeReply {
public:
    ChallengeReply() = default;  // answers nothing
    ChallengeReply(ChallengeReply&& other) noexcept;
    ChallengeReply& operator=(ChallengeReply&& other) noexcept;
    ChallengeReply(const ChallengeReply&) = delete;
    ChallengeReply& operator=(const ChallengeReply&) = delete;
    ~ChallengeReply();

    void retry();
    // `error.kind` authorization_failed ends the held requests with that kind and its `authorization`; any other kind
    // ends them with authorization_required. The message is the authorizer's, in words; never a secret.
    void fail(ClientError error);

    // Whether the reply still waits for its answer.
    bool pending() const noexcept { return channel_ != nullptr; }

private:
    friend class detail::http_client::ReplyChannel;
    ChallengeReply(std::shared_ptr<detail::http_client::ReplyChannel> channel, std::uint64_t flow);
    void answer(std::optional<CallFailure> failure);

    std::shared_ptr<detail::http_client::ReplyChannel> channel_;
    std::uint64_t flow_ = 0;
};

// What an authorizer fetches through the transport (AuthorizationHost::fetch).
enum class AuthorizationFetchPurpose {
    resource_metadata,  // the document of the protected resource
    server_metadata,    // the document of an authorization server
    registration,       // a registration of the client
    token               // a request for a token
};

enum class AuthorizationFetchMethod { get, post };

// The defaults of AuthorizationFetch, as constants.
struct AuthorizationFetchDefaults {
    static constexpr std::size_t max_body_bytes = std::size_t{64} * 1024;
    static constexpr std::chrono::milliseconds timeout{30'000};
};

struct AuthorizationFetch {
    AuthorizationFetchPurpose purpose = AuthorizationFetchPurpose::resource_metadata;
    AuthorizationFetchMethod method = AuthorizationFetchMethod::get;
    // An absolute URL; it must pass the transport's rule for documents of authorization: https, or http on a host of
    // this machine; no user information, no fragment; a host of this machine only when the endpoint is on one too.
    std::string url;
    // The request's header fields, as the authorizer writes them (the transport adds none of the MCP requests', and
    // never their `Authorization`).
    std::vector<HttpHeader> headers;
    std::string body;  // the body of a POST
    // The answer's body, whatever its type, is kept whole up to this many bytes; a longer one fails too_large.
    std::size_t max_body_bytes = AuthorizationFetchDefaults::max_body_bytes;
    // The whole exchange, connecting included.
    std::chrono::milliseconds timeout = AuthorizationFetchDefaults::timeout;
};

// Why a fetch has no answer.
enum class AuthorizationFetchError {
    refused_url,      // the URL does not pass the rule for documents of authorization
    invalid_request,  // a header field that cannot be sent as it is, or a bound that is zero
    too_large,        // the body exceeded max_body_bytes
    timed_out,        // the exchange took longer than its timeout
    transport,        // the exchange could not be made or broke
    closed            // the transport closed
};

struct AuthorizationFetchResult {
    std::optional<AuthorizationFetchError> error;  // empty: the server answered (`status` and the rest)
    std::string message;                            // the error in words; never a text the server sent
    int status = 0;
    std::optional<std::string> media_type;  // as ResponseHead::media_type
    std::vector<HttpHeader> headers;
    std::string body;
};

// The transport's services to its authorizer, valid between HttpAuthorizer::attach() and detach(). Every method is
// called on the client's thread.
class AuthorizationHost {
public:
    virtual ~AuthorizationHost() = default;

    // The client's executor, where the authorizer's own work runs.
    virtual asio::any_io_executor executor() const = 0;
    // The endpoint's URL, verbatim, as the application gave it.
    virtual const std::string& endpoint_url() const = 0;
    // Makes one request on the transport's own thread, with the endpoint's proxy and certificate settings. A GET of a
    // metadata purpose follows redirects 307 and 308 within the origin of the URL fetched (at most
    // HttpEndpoint::max_redirects); every other fetch follows none. Completes with error `closed` when the transport
    // closes first.
    virtual asio::awaitable<AuthorizationFetchResult> fetch(AuthorizationFetch request) = 0;
    // Hands a report to the application (ClientOptions::on_diagnostic, kind authorization). Never pass a secret.
    virtual void diagnostic(std::string message) = 0;
};

// Gives the requests of an HTTP transport the authorization a server asks for, and answers every refusal for want of
// it. Every method is called on the client's thread, and every one but attach() and detach() must return promptly
// (no I/O): work that takes long runs on the host's executor and ends with the reply.
//
// An authorizer serves one transport at a time: Client::open with an HttpEndpoint makes one transport after another
// with the same authorizer (Streamable HTTP, then the older transport), never two at once; making a transport for an
// authorizer that serves another fails with invalid_options.
class HttpAuthorizer {
public:
    HttpAuthorizer() = default;
    virtual ~HttpAuthorizer() = default;
    HttpAuthorizer(const HttpAuthorizer&) = delete;
    HttpAuthorizer& operator=(const HttpAuthorizer&) = delete;

    // The transport started: `host` serves until detach().
    virtual void attach(AuthorizationHost&) {}
    // The transport closes: the host serves no more, and replies still held are inert.
    virtual void detach() {}

    // The `Authorization` value for the request, and its generation.
    virtual Credential authorization(const HttpRequestInfo& request) = 0;

    // The server refused a request with 401 or 403 (`status`). `challenges` are the values of its WWW-Authenticate
    // fields, as sent, as many as the bound on response header fields kept; `generation` that of the credentials the
    // request carried. Answer through `reply`, now or later: retry when credentials newer than `generation` exist (or
    // were just obtained), fail when none can be. Requests refused while this reply waits, with credentials of the
    // same generation, wait for it too: the authorizer is told of the first only.
    virtual void challenge(const HttpRequestInfo& request, int status, std::span<const std::string> challenges,
                           std::uint64_t generation, ChallengeReply reply) = 0;

private:
    friend class detail::http_client::AuthorizerClaim;
    std::atomic<bool> claimed_{false};  // a transport holds it
};

// The simplest authorizer: a token the application already has, sent as `Bearer <token>` on every request, and every
// refusal answered with fail (authorization_required), since it has no other token to offer. An empty token sends no
// field.
class StaticBearer final : public HttpAuthorizer {
public:
    static constexpr std::string_view scheme = "Bearer";

    explicit StaticBearer(std::string token) {
        if (token.empty()) return;
        value_.append(scheme);
        value_.push_back(' ');
        value_.append(token);
    }

    Credential authorization(const HttpRequestInfo&) override { return Credential{value_, 0}; }

    void challenge(const HttpRequestInfo&, int, std::span<const std::string>, std::uint64_t,
                   ChallengeReply reply) override {
        ClientError error;
        error.kind = ClientErrorKind::authorization_required;
        error.message = "the server refused the request, and the application's token is the only one there is";
        reply.fail(std::move(error));
    }

private:
    std::string value_;
};

}  // namespace neomcp
