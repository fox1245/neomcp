// Mechanisms: the code behind the names a profile selects.
//
// A profile names one mechanism per slot (profile.h); a session asks the mechanism of the slot, never the revision
// string or the era, how to behave. The registry maps (slot, name) to a factory; the built-in registry has an
// implementation for every name the library declares for the slots a session needs (open, state, batch and the
// cancellation mechanism of a transport). The slots server_input and change_feed are declared by the profile
// library only; nothing here needs them yet.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

// An error answer a mechanism gives to a request.
struct RequestError {
    int code = error_code::invalid_params;
    std::string message;
    json data = nullptr;
};

// Who the client is, as one request or a connection's handshake tells it.
struct ClientIdentity {
    json info = nullptr;                    // the client's Implementation object; null when not given
    json capabilities = json::object();     // the client's capabilities object
};

// What a handshake established.
struct Handshake {
    profile::ProfilePtr profile;  // the negotiated revision: one of the candidates
    ClientIdentity client;
};

// What a request that names its own revision carries about itself.
struct RequestScope {
    ClientIdentity client;
    std::optional<LogLevel> log_level;  // the level the request asks log messages at; empty: none
};

// Slot `open`: how a peer starts, and where a request's revision and client come from.
class OpenMechanism {
public:
    virtual ~OpenMechanism() = default;

    // Why the mechanism cannot serve the profile; empty when it can.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;
    // Whether a profile that opens this way needs the connection to keep what the handshake established.
    virtual bool needs_connection_state() const = 0;
    // Whether every request names its revision itself (see named_revision), so that under a revision this mechanism
    // serves a request that does not is malformed; false when requests are served on a connection it opens.
    virtual bool requests_name_revision() const = 0;

    // A request that names its revision itself: that revision, or an error when the naming is malformed. Empty
    // when the request carries no such signal.
    virtual std::optional<std::variant<std::string, RequestError>> named_revision(const Message& request) const = 0;
    // For a request that named its revision: what it carries about itself under `profile`, which serves that
    // revision; or an error when the request lacks something the profile requires. `protocol_logging` says
    // whether the server sends log messages at all.
    virtual std::variant<RequestScope, RequestError> scope(const Message& request, const profile::Profile& profile,
                                                      bool protocol_logging) const = 0;

    // Whether the request opens the connection (a handshake).
    virtual bool opens_connection(const Message& request) const = 0;
    // The handshake: the negotiated profile out of `candidates` (newest revision first, never empty) and the
    // client; or an error when the request is malformed.
    virtual std::variant<Handshake, RequestError> open(const Message& request,
                                                  std::span<const profile::ProfilePtr> candidates) const = 0;
    // Whether a request of this method may be served on a connection this mechanism has not opened yet.
    virtual bool allowed_before_open(std::string_view method) const = 0;

    // What a request lacks when it neither named its revision nor arrived on an opened connection, as a clause
    // for an error message.
    virtual std::string missing_signal() const = 0;
};

// Slot `state`: what the server may keep between requests of one connection.
class StateMechanism {
public:
    virtual ~StateMechanism() = default;
    // Whether the connection keeps what a handshake established (the profile, the client, the log level).
    virtual bool keeps_connection() const = 0;
};

// Slot `batch`: whether an array of messages may arrive, and how its answer is assembled.
class BatchMechanism {
public:
    virtual ~BatchMechanism() = default;
    virtual bool accepts_arrays() const = 0;
    // The answer to an array from the responses of its requests, in the array's order; empty when nothing is to be
    // written (no request in the array was answered).
    virtual std::optional<json> assemble(std::vector<json> responses) const = 0;
};

// The cancellation mechanism of a transport: how a cancellation arrives for a request in flight.
class CancelMechanism {
public:
    virtual ~CancelMechanism() = default;
    // The id of the request a client notification asks to stop; empty when the notification is not a cancellation
    // under this mechanism.
    virtual std::optional<RequestId> cancelled_request(const Message& notification) const = 0;
    // Whether the end of the stream that carries a request's answer cancels the request (Session::abandon): the
    // session then requests stop on its call. When false the call runs on, and what it writes is dropped.
    virtual bool stops_on_stream_close() const = 0;
};

// The slots of the registry.
enum class MechanismSlot { open, state, batch, cancel };
std::string_view to_string(MechanismSlot slot) noexcept;

class MechanismRegistry {
public:
    template <typename Interface>
    using Factory = std::function<std::unique_ptr<Interface>()>;

    // Adds or replaces the factory of a name.
    void add_open(std::string name, Factory<OpenMechanism> factory);
    void add_state(std::string name, Factory<StateMechanism> factory);
    void add_batch(std::string name, Factory<BatchMechanism> factory);
    void add_cancel(std::string name, Factory<CancelMechanism> factory);
    // Removes a name; returns whether it was there.
    bool remove(MechanismSlot slot, std::string_view name);

    bool contains(MechanismSlot slot, std::string_view name) const;
    // The names of a slot, sorted.
    std::vector<std::string> names(MechanismSlot slot) const;

    // A new instance; null when the name has no factory.
    std::unique_ptr<OpenMechanism> make_open(std::string_view name) const;
    std::unique_ptr<StateMechanism> make_state(std::string_view name) const;
    std::unique_ptr<BatchMechanism> make_batch(std::string_view name) const;
    std::unique_ptr<CancelMechanism> make_cancel(std::string_view name) const;

private:
    using Key = std::pair<MechanismSlot, std::string>;
    using AnyFactory = std::variant<Factory<OpenMechanism>, Factory<StateMechanism>, Factory<BatchMechanism>,
                                    Factory<CancelMechanism>>;

    template <typename Interface>
    void put(MechanismSlot slot, std::string name, Factory<Interface> factory);
    template <typename Interface>
    std::unique_ptr<Interface> make(MechanismSlot slot, std::string_view name) const;

    std::map<Key, AnyFactory> factories_;
};

// The mechanisms of the library: every name profile::declared_mechanisms declares for open, state and batch, and
// every name profile::declared_cancel declares for a transport.
const MechanismRegistry& builtin_mechanisms();

}  // namespace neomcp
