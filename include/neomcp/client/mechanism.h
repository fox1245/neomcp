// The client halves of the mechanisms: the code behind the names a profile selects, as a client speaks them.
//
// A profile names one mechanism per slot (profile.h). The server's session asks the server half of each slot how to
// behave (neomcp/mechanism.h); a client engine asks the client half declared here the same questions from the other
// side, and never looks at a revision string or an era. Each half writes what the client sends under its mechanism and
// reads what a server sends under it; the engine owns the request ids, the pending requests, the clocks, the units a
// message travels on and the decisions that span several revisions (which revision to speak, which kind of server
// answers).
//
// The registry maps (slot, name) to a factory, exactly as the server's does; the built-in registry has a client half
// for every name profile::declared_mechanisms declares for open, state, server_input, change_feed and batch, and for
// every name profile::declared_cancel declares for a transport. A profile that names a mechanism the registry has no
// client half for cannot be spoken: select_client_mechanisms reports it with the revision, the slot and the name.
//
// Every half is stateless and every method is const, so that one instance can serve every request of a profile from
// any thread. What a server sends is not trusted: a reader never throws for a malformed message. It returns the value
// it read, or a description of the first thing that is wrong (as the readers of neomcp/wire_read.h do), and the
// description never repeats a text the server sent.
//
// What the client sends is described here without the JSON-RPC frame: a method and its params (ClientMessage). The
// engine adds the id of a request, and the open mechanism adds what a request carries about its revision and its
// client (ClientOpenMechanism::annotate).
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

#include "neomcp/change.h"
#include "neomcp/input.h"
#include "neomcp/json.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/mechanism.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/wire_read.h"

namespace neomcp {

// A request or a notification the client writes, without its JSON-RPC frame. `params` is null when the message has
// none; a request the open mechanism annotates gets an object.
struct ClientMessage {
    std::string method;
    json params = nullptr;
};

// What the client says about itself under one profile.
struct ClientSelf {
    Implementation info;                   // written for the profile wherever the profile has a place for it
    json capabilities = json::object();    // a ClientCapabilities object, already written for the profile
    std::optional<LogLevel> log_level;     // the level the application asked log messages at; empty: none asked
};

// What a server says about itself in the result that opens contact (a handshake's result or a discovery result),
// wherever the mechanism finds it in that result.
struct ServerAnnouncement {
    json capabilities = json::object();       // its ServerCapabilities object
    json info = nullptr;                      // its Implementation object as sent; null when it sent none
    std::optional<std::string> instructions;
    json result;                              // the whole result, for what the mechanism does not read
};

// A handshake the server accepted: the negotiated profile, one of the candidates, and what the server said.
struct HandshakeAccepted {
    profile::ProfilePtr profile;
    ServerAnnouncement server;
};

// A handshake result that names a revision none of the candidates is: the client cannot speak it. The revision is
// kept as data for the engine's report, never put into a description.
struct UnservedRevision {
    std::string revision;
};

// What a discovery result says: the revisions the server serves, in the order it gave them (the pages state none),
// and what it said about itself. Which of the revisions to speak is the engine's decision.
struct Discovery {
    std::vector<std::string> versions;
    ServerAnnouncement server;
};

// An error answer that refuses the revision a request was sent under and names the revisions the server serves.
struct VersionRefusal {
    std::vector<std::string> supported;     // in the order the server gave them
    std::optional<std::string> requested;   // the revision the server says was asked, when it says one
};

// Slot `open`: how the client starts contact, and what every request carries about its revision and its client.
class ClientOpenMechanism {
public:
    virtual ~ClientOpenMechanism() = default;

    // Why the half cannot speak the profile; empty when it can. A profile whose requests must carry a member this
    // half cannot fill fails here, at creation, rather than at the first request.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;

    // Whether contact starts with a handshake that fixes the revision for the connection (and the requests after it
    // carry nothing of their own about the revision). Exactly one of handshakes() and names_revision_per_request() is
    // true for each built-in half.
    virtual bool handshakes() const = 0;
    // Whether every request names its revision and its client itself, so that each request stands alone. The engine
    // takes the error codes of the profiles whose open half says so as the errors that identify a server of that kind.
    virtual bool names_revision_per_request() const = 0;

    // ---- every request

    // Adds to the params of a request what the request carries about its revision and its client under `profile`.
    // Null params become an object. Under a half that names the revision per request this writes `_meta`: every key
    // the profile's request meta type requires, the client's Implementation where the type has a key for it, and the
    // log level only when one is set and the type has a key for it. Members of `_meta` that are not the half's own
    // (a progress token, for one) are kept; the half's own keys are written anew each time, so that the same params
    // sent again under another revision name that revision. Under a half that handshakes this changes nothing.
    // Notifications are not annotated. An error when the params are neither null nor an object.
    virtual std::optional<std::string> annotate(json& params, const profile::Profile& profile,
                                                const ClientSelf& self) const = 0;

    // ---- a handshake (handshakes() true; the other half returns empty values and errors)

    // The request that opens contact at `profile`, the newest candidate the engine chose.
    virtual std::optional<ClientMessage> handshake(const profile::Profile& profile, const ClientSelf& self) const = 0;
    // Whether the client may send a request of `method` before the handshake has been answered.
    virtual bool allowed_before_open(std::string_view method) const = 0;
    // The result of the handshake: the negotiated profile among `candidates` (the profiles the handshake could have
    // chosen, newest first) and what the server said; the revision it named when that is no candidate; or what is
    // wrong when the result is malformed.
    virtual std::variant<HandshakeAccepted, UnservedRevision, std::string> read_handshake(
        const json& result, std::span<const profile::ProfilePtr> candidates) const = 0;
    // The notification that tells the server the client is ready, sent once the handshake was accepted; empty when the
    // half sends none.
    virtual std::optional<ClientMessage> opened(const profile::Profile& profile) const = 0;

    // ---- discovery (names_revision_per_request() true; the other half returns empty values)

    // The request that asks a server which revisions it serves, sent before anything else when the engine probes.
    // The engine annotates it like every request.
    virtual std::optional<ClientMessage> probe(const profile::Profile& profile) const = 0;
    // The result of that request under the profile it was sent under. Empty when the result is not a discovery
    // result (it lacks a member the profile's discovery result type requires): the engine reads that as a server of
    // the other kind. An error when the result has the members but one of them is malformed.
    virtual std::optional<WireRead<Discovery>> read_discovery(const json& result,
                                                              const profile::Profile& profile) const = 0;

    // ---- both

    // An error answer that refuses the revision of a request sent under `profile` and names the revisions the server
    // serves: under a half that names the revision per request, an error of the code the profile gives the refusal of
    // an unsupported version; under a half that handshakes, an Invalid params answer to the handshake that carries the
    // list. Empty when `error` (the `error` member of a response) is no such refusal; an error when it is one whose list
    // is malformed.
    virtual std::optional<WireRead<VersionRefusal>> read_version_refusal(const json& error,
                                                                         const profile::Profile& profile) const = 0;
    // The request that sets the level of the server's log messages for the connection, sent once after the handshake
    // when the profile has it and the server's capabilities declare logging. Empty under a half that names the level
    // in every request instead (annotate), and when the profile or the server has no such request.
    virtual std::optional<ClientMessage> level_request(LogLevel level, const profile::Profile& profile,
                                                       const json& server_capabilities) const = 0;
};

// Slot `state`: what lasts between the requests of one connection.
class ClientStateMechanism {
public:
    virtual ~ClientStateMechanism() = default;
    // Whether the connection keeps what a handshake established (the negotiated profile and, on HTTP, a session), so
    // that a connection that is lost or restarted must be opened again before requests go on, and what was watched on
    // it must be watched again. False when every request stands alone.
    virtual bool keeps_connection() const = 0;
};

// Slot `batch`: whether a server may send an array of messages. The client never sends one: it answers each request
// of an array with a response of its own, since a body that is a single response is allowed beside an array of
// responses, and answering one by one lets the answer of a quick handler go before that of a slow one.
class ClientBatchMechanism {
public:
    virtual ~ClientBatchMechanism() = default;
    // Whether an array of messages from the server is read (its messages one by one) rather than refused.
    virtual bool reads_arrays() const = 0;
    // The messages of one parsed JSON value the server sent: the value itself when it is not an array; the elements of
    // an array when arrays are read. An error for an array when they are not, and for an empty array.
    virtual WireRead<std::vector<json>> split(json value) const = 0;
};

// The cancellation mechanism of a transport, as the client speaks it.
class ClientCancelMechanism {
public:
    virtual ~ClientCancelMechanism() = default;
    // Why the half cannot speak the profile; empty when it can.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;
    // Whether ending the stream that carries a request's answer is itself the cancellation: the engine then ends that
    // stream and sends nothing. When false, the engine sends notification() (and may drop the stream as well, which
    // alone cancels nothing).
    virtual bool stream_close_cancels() const = 0;
    // The notification that cancels the client's request `id`, with `reason` where the profile's notification has a
    // member for it; empty when the half sends none.
    virtual std::optional<ClientMessage> notification(const RequestId& id, std::string_view reason,
                                                      const profile::Profile& profile) const = 0;
    // The id of the request that a server's notification cancels; empty when the notification is not a cancellation
    // under this mechanism. Whether the id names a request the server sent the client or a subscription the client
    // opened is the engine's to look up.
    virtual std::optional<RequestId> cancelled_request(const Message& notification) const = 0;
};

// One request a result asks the client to answer, by the key the server chose for it.
struct AskedInput {
    std::string key;
    std::string method;    // the input request's method, as the server named it
    InputRequest request;
};

// A result that asks for input before the call can complete: the requests (possibly none) and the state to echo.
struct InputAsking {
    std::vector<AskedInput> requests;  // sorted by key
    std::optional<json> state;         // exactly as the server sent it; empty when it sent none
};

// A request of the server that the client answers with a response of its own.
struct ServerAsk {
    std::string method;
    InputRequest request;
};

// A server's ping, answered at once (ping_answer).
struct ServerPing {};

// Slot `server_input`: how a call asks the client for input while the server serves it, as the client answers.
class ClientInputMechanism {
public:
    virtual ~ClientInputMechanism() = default;

    // Why the half cannot speak the profile; empty when it can.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;
    // result: a call's result asks, and the client retries the call with the answers; handlers run between rounds,
    // when nothing of the call is pending. requests: the server sends requests of its own while the call is pending,
    // and the client answers each with a response; only then does the client write responses at all.
    virtual InputDelivery delivery() const = 0;

    // Whether a call of the client request `method` may ask for input under the profile.
    virtual bool asks_on(const profile::Profile& profile, std::string_view method) const = 0;

    // ---- delivery result (the other half returns empty values and errors)

    // What the result of a request of `method` is: empty when it is complete (it says so, or says nothing of its
    // kind); the requests and the state when it asks. An error when it names a kind the client does not know, when it
    // asks for a method that cannot ask (asks_on), when it asks with neither requests nor a state, and when one of its
    // requests is malformed or of a method the revision does not let a server ask with. Under delivery requests every
    // result is complete.
    virtual WireRead<std::optional<InputAsking>> read_result(const json& result, std::string_view method,
                                                             const profile::Profile& profile) const = 0;
    // The params of the retry: `params` as the application gave them (without what annotate adds, which the engine
    // adds again), with the answers under the keys they answer and the state exactly as received; any answers or state
    // that `params` already carries are dropped first, so that a state the server did not send is never sent. An
    // error when an answer cannot be written for the revision, or when the state cannot be sent back exactly as it was
    // received (a text that serializing would change).
    virtual WireRead<json> retry_params(json params, const InputResponses& answers, const std::optional<json>& state,
                                        const profile::Profile& profile) const = 0;

    // ---- delivery requests (the other half returns errors)

    // What a server's request asks: an input request, a ping, or the error answer the client gives it (an unknown
    // method, malformed params).
    virtual std::variant<ServerAsk, ServerPing, RequestError> read_request(const Message& request,
                                                                           const profile::Profile& profile) const = 0;
    // The error answer to an input request the client declared no support for, as the revision prescribes it.
    // `declared` is the ClientCapabilities object the client declared under the profile: a request whose kind it does
    // not declare at all (no elicitation, no sampling, no roots) is answered as a method the client does not have; an
    // elicitation in a mode it does not declare, while it declares elicitation in another mode, as invalid params.
    virtual RequestError refusal(const InputRequest& request, const json& declared,
                                 const profile::Profile& profile) const = 0;
    // The error answer that tells the server the user refused its sampling request, as the revision prescribes it.
    virtual RequestError user_refusal(const profile::Profile& profile) const = 0;
    // The response that answers the server's request `id`; an error when the answer cannot be written for the
    // revision.
    virtual WireRead<json> answer(const RequestId& id, const InputResponse& response,
                                  const profile::Profile& profile) const = 0;
    // The response to a server's ping.
    virtual json ping_answer(const RequestId& id) const = 0;

    // ---- both

    // What keeps the requested schema of a form from the restricted form the profile defines; empty when it fits.
    // Each property must hold the required members of one alternative of the profile's union of primitive schema
    // definitions and no member that alternative lacks; the schema itself may hold only the members the half names
    // for the root of a form (its type, its properties, which of them are required, and the dialect). The profile's
    // tables name members, not their values, so the value of a property's `type` is not compared with the
    // alternative's; the engine compiles the schema as well before a handler sees the form. The description never
    // repeats a text the server sent (a property is named by its position).
    virtual std::optional<std::string> form_problem(const json& requested_schema,
                                                    const profile::Profile& profile) const = 0;

    // The notification that tells the server the client's roots changed; empty when the profile has none.
    virtual std::optional<ClientMessage> roots_changed(const profile::Profile& profile) const = 0;
};

// A message of a change feed, as the client recognises it.
struct FeedEvent {
    enum class Kind {
        change,        // something the server offers changed (`change`)
        acknowledged,  // subscription: the server says what of the filter it honours (`honoured`)
        ended          // subscription: the server ends it with a notice; the result that ends it may follow
    };
    Kind kind = Kind::change;
    // delivery subscription: the subscription the message belongs to (the id of the request that opened it).
    // delivery connection: empty.
    std::optional<RequestId> subscription;
    Change change;
    ChangeFilter honoured;
};

// Slot `change_feed`: how the client is told that what a server offers changed.
class ClientFeedMechanism {
public:
    virtual ~ClientFeedMechanism() = default;

    // Why the half cannot speak the profile; empty when it can.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;
    // connection: the changes come as notifications of the connection; the client watches resources with requests
    // that add and remove a URI, and ends nothing else. subscription: a request opens a subscription whose answer is a
    // stream of what it asked for; the client ends it the way the transport's cancellation mechanism cancels a
    // request.
    virtual FeedDelivery delivery() const = 0;

    // What the client may expect under `asked` from a server that declared `server_capabilities`: under delivery
    // connection the lists whose changes the server declares it reports, and the resources when it declares that it
    // reports their updates; under delivery subscription `asked` itself (the acknowledgement then tells what is
    // honoured).
    virtual ChangeFilter offered(const ChangeFilter& asked, const json& server_capabilities) const = 0;
    // The request that asks what `asked` says: watch or unwatch one URI under delivery connection, listen under
    // delivery subscription; the server half reads it back as the same FeedRequest. An error for an action the
    // mechanism does not have.
    virtual WireRead<ClientMessage> write(const FeedRequest& asked, const profile::Profile& profile) const = 0;

    // A server's notification under the profile: empty when it is not one of the feed's; the event when it is; an
    // error when it is one of the feed's and malformed (under delivery subscription, also when it lacks the
    // subscription it belongs to). A notification that may also cancel a request of the server's (an ending notice)
    // is read as an ending; the engine passes it on when the id names no subscription it opened.
    virtual std::optional<WireRead<FeedEvent>> read(const Message& notification,
                                                    const profile::Profile& profile) const = 0;
    // delivery subscription: whether the result that answered the request `subscription` ends that subscription
    // gracefully: empty when it does, else what is wrong with it. delivery connection: always an error (no result
    // ends anything).
    virtual std::optional<std::string> read_end(const json& result, const RequestId& subscription,
                                                const profile::Profile& profile) const = 0;
};

class ClientMechanismRegistry {
public:
    template <typename Interface>
    using Factory = std::function<std::unique_ptr<Interface>()>;

    // Adds or replaces the factory of a name.
    void add_open(std::string name, Factory<ClientOpenMechanism> factory);
    void add_state(std::string name, Factory<ClientStateMechanism> factory);
    void add_batch(std::string name, Factory<ClientBatchMechanism> factory);
    void add_cancel(std::string name, Factory<ClientCancelMechanism> factory);
    void add_input(std::string name, Factory<ClientInputMechanism> factory);
    void add_feed(std::string name, Factory<ClientFeedMechanism> factory);
    // Removes a name; returns whether it was there.
    bool remove(MechanismSlot slot, std::string_view name);

    bool contains(MechanismSlot slot, std::string_view name) const;
    // The names of a slot, sorted.
    std::vector<std::string> names(MechanismSlot slot) const;

    // A new instance; null when the name has no factory.
    std::unique_ptr<ClientOpenMechanism> make_open(std::string_view name) const;
    std::unique_ptr<ClientStateMechanism> make_state(std::string_view name) const;
    std::unique_ptr<ClientBatchMechanism> make_batch(std::string_view name) const;
    std::unique_ptr<ClientCancelMechanism> make_cancel(std::string_view name) const;
    std::unique_ptr<ClientInputMechanism> make_input(std::string_view name) const;
    std::unique_ptr<ClientFeedMechanism> make_feed(std::string_view name) const;

private:
    using Key = std::pair<MechanismSlot, std::string>;
    using AnyFactory =
        std::variant<Factory<ClientOpenMechanism>, Factory<ClientStateMechanism>, Factory<ClientBatchMechanism>,
                     Factory<ClientCancelMechanism>, Factory<ClientInputMechanism>, Factory<ClientFeedMechanism>>;

    template <typename Interface>
    void put(MechanismSlot slot, std::string name, Factory<Interface> factory);
    template <typename Interface>
    std::unique_ptr<Interface> make(MechanismSlot slot, std::string_view name) const;

    std::map<Key, AnyFactory> factories_;
};

// The client halves of the library: every name profile::declared_mechanisms declares for open, state, server_input,
// change_feed and batch, and every name profile::declared_cancel declares for a transport.
const ClientMechanismRegistry& builtin_client_mechanisms();

// The client halves one profile selects for one transport, each never null.
struct ClientMechanisms {
    profile::ProfilePtr profile;
    std::unique_ptr<ClientOpenMechanism> open;
    std::unique_ptr<ClientStateMechanism> state;
    std::unique_ptr<ClientBatchMechanism> batch;
    std::unique_ptr<ClientCancelMechanism> cancel;  // the one the profile's entry for the transport names
    std::unique_ptr<ClientInputMechanism> server_input;
    std::unique_ptr<ClientFeedMechanism> change_feed;
};

// A mechanism a profile names that the registry has no client half for.
struct MissingClientMechanism {
    std::string revision;
    MechanismSlot slot = MechanismSlot::open;
    std::string name;
};

// Why the halves of one or more profiles could not be selected.
struct ClientMechanismsError {
    std::string message;                          // every problem, one per line
    std::vector<MissingClientMechanism> missing;  // the missing halves among them
};

// The client halves of each profile that defines `transport`, in the order given; a profile that does not define it
// is left out. Every profile is looked at, and the error names every missing half (with the revision, the slot and
// the name) and every half that cannot speak its profile (check). Null profiles are an error.
std::variant<std::vector<ClientMechanisms>, ClientMechanismsError> select_client_mechanisms(
    std::span<const profile::ProfilePtr> profiles, profile::Transport transport,
    const ClientMechanismRegistry& registry = builtin_client_mechanisms());

}  // namespace neomcp
