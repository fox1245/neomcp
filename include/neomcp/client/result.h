// What a client's call gives back: its value, or a typed error.
//
// Every method of neomcp::Client that can fail returns Result<T>. A failure is a value too, never an exception: the
// server's error answer, a call that timed out or was cancelled, an answer that was lost, contact that could not be
// made. A tool that reports a failure of its own (a result with `isError`) is not a failure of the call: it is a value,
// the tool's result.
//
//   auto sum = client.call_tool("add", {{"a", 1}, {"b", 2}});
//   if (!sum) {
//       std::cerr << neomcp::to_string(sum.error().kind) << ": " << sum.error().message << '\n';
//   }
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/client/engine.h"
#include "neomcp/json.h"

namespace neomcp {

// Why a call failed. The kinds of the engine's failures (CallFailureKind, ContactFailureKind) each have one here, under
// the same name; error_kind() maps them. A call that failed because contact could not be made carries the kind of the
// contact failure, so CallFailureKind::contact_failed has no kind of its own.
enum class ClientErrorKind {
    // The server answered the request with an error: `code`, `message` and `data` are the server's.
    server_error,

    // ---- The call ended without an answer (CallFailureKind)
    timed_out,               // its wait (or its total) lapsed; it was cancelled as its transport prescribes
    cancelled,               // the application cancelled it (RequestOptions::stop, or cancellation of the awaiter)
    server_exited,           // stdio: the server process ended while it was pending; its answer was lost
    stream_lost,             // HTTP: the exchange that carried it ended or broke before its answer
    send_failed,             // the transport could not deliver it
    http_status,             // HTTP: answered with a status that carries no answer of the protocol
    bad_content_type,        // HTTP: answered with content of a type the protocol does not use
    authorization_required,  // HTTP: the server asks for authorization the client cannot give (no authorizer, or one
                             // that only has a fixed token)
    authorization_failed,    // HTTP: the authorizer tried to obtain the authorization and could not (`authorization`
                             // says what it met)
    message_too_large,       // its answer was longer or nested deeper than the client's bounds
    malformed_answer,        // its answer (or the answer that opens contact) is no result of its kind
    version_refused,         // the server refused its revision a second time after the corrective retry
    no_common_revision,      // the server serves none of the revisions the client speaks (`server_revisions`)
    too_many_pending,        // max_pending_requests calls are pending already
    unsupported_method,      // the governing revision has no such client request, or the params are no object
    header_value,            // an argument cannot be mirrored into a header (an integer out of range)
    input_timed_out,         // a round of input was not answered within input_handler_timeout
    task_lost,               // a task the call followed is one the server does not know (it never had it, or no
                             // longer has it, as after a restart); the tool call is never sent again (`task`)
    task_cancelled,          // a task the call followed ended cancelled, not by this call (`task`)
    task_polls_exceeded,     // a task the call followed was read ClientTaskOptions::max_polls times without ending
    input_rounds_exceeded,   // the server asked for input more than max_input_rounds times
    input_requests_exceeded, // one round asked for more than max_input_requests_per_round inputs
    server_asked_undeclared, // a round asked for input of a kind the client declared no handler for
    form_not_restricted,     // a round asked for a form whose schema is outside the restricted form
    answer_invalid,          // an accepted form's content fails the form's schema; it was not sent
    input_refused,           // the user refused a sampling request of a round
    handler_failed,          // an input handler of a round threw
    state_not_echoable,      // the state of a round cannot be sent back exactly as it was received
    answer_not_writable,     // an answer of a round cannot be written for the revision
    closed,                  // the client was closed (or the Client was moved from)

    // ---- The answer came, and the client does not take it as the result asked for (neomcp/client/calls.h)
    unverified,              // a typed call: the structured content cannot be checked, because the tool's output
                             // schema cannot be compiled within the peer bounds or budget, or the tool is not in the
                             // server's list of tools
    output_invalid,          // a typed call: the structured content fails the tool's output schema
    output_undecodable,      // a typed call: the result has no structured content, or it does not have the shape of
                             // the result type
    no_structured_results,   // a typed call under a revision whose tool results carry no structured content; nothing
                             // was sent
    list_pages_exceeded,     // a list_all_ call: the server needed more than max_list_pages pages
    list_items_exceeded,     // a list_all_ call: the server sent more than max_list_items items
    cursor_not_echoable,     // a cursor the server gave cannot be sent back exactly as it was received

    // ---- A subscription to changes (neomcp/client/subscription.h)
    subscription_lost,       // its stream was lost, and so were max_subscription_reopens reopens in a row
    subscription_ended,      // the server ended it (with its final result, or a notice)
    too_many_subscriptions,  // max_subscriptions subscriptions are open already

    // ---- Contact could not be made (ContactFailureKind)
    unserved_revision,       // the handshake's answer names a revision none of the client's is
    handshake_refused,       // the handshake was answered with an error, after its one retry where that applies
    handshake_timed_out,     // the handshake was not answered in time; the transport was ended
    transport,               // the transport could not start, or failed while contact was being made
    too_many_restarts,       // the server process ended (stdio), or the server forgot the session (HTTP),
                             // max_restarts times in a row without an answer

    // ---- Refused by the client itself
    on_client_thread,        // a blocking method was called on the client's own thread, where it would wait for
                             // itself; use its async_ form there
    invalid_options          // an option that cannot work (in ClientOptions, a transport's description, or a call's
                             // RequestOptions), or a profile that names a mechanism the library has no client half
                             // for (the message names each)
};

std::string_view to_string(ClientErrorKind kind) noexcept;

// The kind of an engine's call failure; empty for CallFailureKind::contact_failed, whose kind is that of the contact
// failure (ContactState::failure).
std::optional<ClientErrorKind> error_kind(CallFailureKind kind) noexcept;
// The kind of an engine's contact failure.
ClientErrorKind error_kind(ContactFailureKind kind) noexcept;

struct ClientError {
    ClientErrorKind kind = ClientErrorKind::closed;
    // server_error: the JSON-RPC error code the server answered with.
    std::optional<int> code;
    // In words. For server_error the server's message as it sent it (the application decides what to show); for every
    // other kind written by the library, never repeating a text the server sent.
    std::string message;
    // server_error: the error's `data` as the server sent it; null when it sent none, and for every other kind.
    json data = nullptr;
    // no_common_revision: the revisions the server said it serves, in its order, as data.
    std::vector<std::string> server_revisions;
    // Contact could not be made over this transport, and the server may serve another one at the same place: the
    // transport to try next (ContactFailure::next_transport). Client::open with an HttpEndpoint tries it by itself; an
    // application that gives its own transport may. Empty otherwise.
    std::optional<profile::Transport> next_transport;
    // Whether the request went out to the server and the call ended without the server's answer, so that what the
    // server did is not known: a tool may have run. Set for timed_out, cancelled, server_exited and stream_lost, and
    // for the ends of a task without its result (task_lost, task_cancelled, task_polls_exceeded), when the request had
    // been sent; clear when the call failed before anything was sent, and for every other kind.
    bool may_have_run = false;
    // authorization_failed: what the authorizer met (the problem, the authorization server, the URL without its
    // query); never a secret.
    std::optional<AuthorizationFailure> authorization;
    // The id of the task the call was about, when the server answered it with a task or it followed, read or
    // cancelled one: an application that has it can follow the task again with Client::task.
    std::optional<std::string> task;
};

// Thrown by the accessors of a Result that holds no value.
class BadResultAccess : public std::logic_error {
public:
    explicit BadResultAccess(ClientError error)
        : std::logic_error("neomcp: the result holds an error, not a value"), error_(std::move(error)) {}
    const ClientError& error() const noexcept { return error_; }

private:
    ClientError error_;
};

// A value of type T, or the ClientError that took its place. T may be move-only. Accessing the value of a Result that
// holds an error throws BadResultAccess; accessing the error of one that holds a value throws std::logic_error.
template <class T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T> && !std::is_same_v<std::remove_cv_t<T>, ClientError>,
                  "neomcp: Result holds a value by value, and never a ClientError as its value");

public:
    using value_type = T;

    // Holds a ClientError of kind `closed` without a message. It exists only for asio's exception path:
    // asio::co_spawn passes a default value along with the exception of an operation that threw, and a client's
    // operation never completes with it. Note that a default Result<void> is the opposite: it is success.
    Result() : state_(std::in_place_index<1>, ClientError{}) {}
    Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(ClientError error) : state_(std::in_place_index<1>, std::move(error)) {}

    bool has_value() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        check_value();
        return std::get<0>(state_);
    }
    const T& value() const& {
        check_value();
        return std::get<0>(state_);
    }
    T&& value() && {
        check_value();
        return std::get<0>(std::move(state_));
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    const ClientError& error() const& {
        check_error();
        return std::get<1>(state_);
    }
    ClientError&& error() && {
        check_error();
        return std::get<1>(std::move(state_));
    }

private:
    void check_value() const {
        if (state_.index() != 0) throw BadResultAccess(std::get<1>(state_));
    }
    void check_error() const {
        if (state_.index() != 1) throw std::logic_error("neomcp: the result holds a value, not an error");
    }

    std::variant<T, ClientError> state_;
};

// The result of a call that gives back nothing but whether it succeeded.
template <>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;

    Result() = default;
    Result(ClientError error) : error_(std::move(error)) {}

    bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    // Throws BadResultAccess when the result holds an error.
    void value() const {
        if (error_) throw BadResultAccess(*error_);
    }

    const ClientError& error() const& {
        check_error();
        return *error_;
    }
    ClientError&& error() && {
        check_error();
        return std::move(*error_);
    }

private:
    void check_error() const {
        if (!error_) throw std::logic_error("neomcp: the result holds a value, not an error");
    }

    std::optional<ClientError> error_;
};

}  // namespace neomcp
