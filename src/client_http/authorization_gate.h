// The part of an HTTP transport that takes part in authorization (neomcp/client/http.h), shared by both transports.
//
// A gate stamps each request with the authorizer's `Authorization` value, and only a request to the endpoint's origin
// of an endpoint that carries tokens (https, or a loopback host). It holds the requests a server refuses with 401 or
// 403 until the authorizer answers, and it is the authorizer's AuthorizationHost.
//
// One sign-in at a time per set of credentials (single flight). Every value the authorizer gives comes with a
// generation. A refusal of a request that carried an older generation than the newest the gate has seen is sent again
// at once with the newest credentials, without asking the authorizer: newer ones exist already. Otherwise the refusal
// joins the open flow of its generation, or opens one, and only the request that opens a flow is shown to the
// authorizer (challenge()). A flow ends with its reply alone: retry sends every request it holds again, fail ends each.
// A reply names its flow, so that an answer can never reach the requests of another flow; a flow stays open until its
// reply came, even when every request it held ended meanwhile, and later refusals of its generation join it.
//
// Each request may be refused at most HttpEndpoint::max_authorizations_per_request times in a row, without an answer
// of another kind between; one more refusal ends it with authorization_failed (attempts_exceeded). An endpoint that does not carry tokens gets no `Authorization` field, and
// each refusal ends at once with authorization_failed (insecure_endpoint); the authorizer is not asked.
//
// Fetches for the authorizer run on the transport's transfer loop on units of their own (from `first_fetch_unit` up),
// which no transport otherwise uses: the engine numbers its units from 1, and the transports number theirs down from
// the largest unit.
//
// Everything but ReplyChannel::answer runs on the client's thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include <asio/any_io_executor.hpp>

#include "client_http/transfers.h"
#include "client_http/url_guard.h"
#include "neomcp/client/engine.h"
#include "neomcp/client/http.h"
#include "neomcp/client/result.h"
#include "neomcp/client/transport.h"

namespace neomcp::detail::http_client {

// The first unit of the fetches of a gate.
inline constexpr UnitId first_fetch_unit = UnitId{1} << 62;

// Carries the answers of a gate's replies to the client's thread; shared by the gate and every reply it handed out.
class ReplyChannel {
public:
    using Apply = std::function<void(std::uint64_t flow, std::optional<CallFailure> failure)>;

    ReplyChannel(asio::any_io_executor executor, Apply apply);

    // Any thread: posts the answer for `flow` (empty failure: retry) to the client's thread; nothing once closed.
    void answer(std::uint64_t flow, std::optional<CallFailure> failure);
    // The client's thread: no answer is carried any more.
    void close();

    static ChallengeReply reply(std::shared_ptr<ReplyChannel> channel, std::uint64_t flow);

private:
    std::mutex mutex_;
    bool open_ = true;
    asio::any_io_executor executor_;
    Apply apply_;
};

// Binds an authorizer to one transport at a time.
class AuthorizerClaim {
public:
    // False when another transport holds it.
    static bool take(HttpAuthorizer& authorizer) noexcept;
    static void release(HttpAuthorizer& authorizer) noexcept;
};

// What a gate asks of its transport. Each is called on the client's thread, from inside refused() or when a reply's
// answer arrives, never from inside the transport's start(), send() or close(); each tolerates a unit that ended.
struct GateHooks {
    std::function<void(UnitId unit)> hold;    // the unit's request is held: tell the client
    std::function<void(UnitId unit)> resend;  // send it again, stamped anew
    std::function<void(UnitId unit, CallFailure failure, int status)> deny;  // it ends with `failure`
    std::function<void(Diagnostic diagnostic)> diagnostic;
};

class AuthorizationGate {
public:
    // A gate for `endpoint`, which claims its authorizer (none: a gate that only lets requests through). Fails with
    // invalid_options when another transport holds the authorizer.
    static std::variant<std::unique_ptr<AuthorizationGate>, ClientError> make(const HttpEndpoint& endpoint,
                                                                              const UrlGuard& guard);
    ~AuthorizationGate();
    AuthorizationGate(const AuthorizationGate&) = delete;
    AuthorizationGate& operator=(const AuthorizationGate&) = delete;

    // An authorizer is set.
    bool configured() const noexcept;
    // The transport started: the authorizer is attached, with the gate as its host.
    void start(asio::any_io_executor executor, TransferLoop& loop, GateHooks hooks);
    // The transport closes: held requests are forgotten (the client ended their calls), replies become inert, fetches
    // end `closed`, the authorizer is detached and the claim released. Idempotent.
    void stop();
    // Whether a refusal is the gate's to hold now: an authorizer is set and attached.
    bool takes_refusals() const noexcept;

    // The `Authorization` value for a request, and its generation; an empty value when the request goes without.
    Credential credential(const HttpRequestInfo& request);

    // The request on `unit` was refused with `status` (401 or 403); `generation` is that of the credentials it carried
    // and `headers` the refusal's header fields. Only when takes_refusals(). The transport has ended the refused
    // exchange and touches nothing of the unit after this call: a hook may have ended it.
    void refused(UnitId unit, const HttpRequestInfo& request, std::uint64_t generation, int status,
                 const std::vector<HttpHeader>& headers);
    // The unit ended: nothing of it is held or counted any more.
    void forget(UnitId unit);
    // The server answered the unit's request otherwise than with a refusal for want of authorization: its count of
    // refusals in a row starts again (a unit that lives long, such as the stream of the connection, is refused anew
    // whenever its credentials expire).
    void accepted(UnitId unit);

    // Whether an item of the transfer loop on `unit` belongs to a fetch of the gate, and its delivery.
    bool owns(UnitId unit) const noexcept;
    void on_fetch(UnitId unit, TransferItem item);

private:
    struct State;
    explicit AuthorizationGate(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

}  // namespace neomcp::detail::http_client
