// Mechanisms: the code behind the names a profile selects.
//
// A profile names one mechanism per slot (profile.h); a session asks the mechanism of the slot, never the revision
// string or the era, how to behave. The registry maps (slot, name) to a factory; the built-in registry has an
// implementation for every name the library declares for the slots (open, state, server_input, change_feed, batch and
// the cancellation mechanism of a transport, and tasks).
#pragma once

#include <chrono>
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
#include "neomcp/json.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/server/task_store.h"
#include "neomcp/task_host.h"

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

// How the requests of a call that asks for input (InputRequired) reach the client.
enum class InputDelivery {
    result,   // the call's result asks; the client retries the call with the answers, and the call ends here
    requests  // each request goes to the client as a request of its own; the call waits for the responses
};

// What a request carries about the round before it, under a mechanism whose retried call answers.
struct InputRetry {
    std::optional<json> responses;  // the answers member, an object
    std::optional<json> state;      // the state member as given: a well-formed one is a string
};

// Slot `server_input`: how a call asks its client for input while it is served.
class InputMechanism {
public:
    virtual ~InputMechanism() = default;

    // Why the mechanism cannot serve the profile; empty when it can.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;
    virtual InputDelivery delivery() const = 0;

    // Whether a call of the client request `method` may ask for input under the profile.
    virtual bool asks_on(const profile::Profile& profile, std::string_view method) const = 0;
    // Whether the profile lets a server ask its client with a request of `method` (an input request's method), and
    // the name of the params type of such a request where the profile names one.
    virtual bool carries(const profile::Profile& profile, std::string_view method) const = 0;
    virtual std::optional<std::string> params_type(const profile::Profile& profile, std::string_view method) const = 0;
    // The members the params of a request of `method` require under the profile, where no params type names them.
    virtual std::span<const std::string> params_required(const profile::Profile& profile,
                                                         std::string_view method) const = 0;

    // delivery result: what a request of `method` carries about the round before it, or an error when that is
    // malformed. delivery requests: always empty (the client cannot answer this way).
    virtual std::variant<InputRetry, RequestError> retry(const Message& request, const profile::Profile& profile) const = 0;
    // delivery result: the body of the result that asks, from the requests as written ({key: {method, params}}) and
    // the sealed state; the type of that result for the profile's filters.
    virtual json asking_result(json requests, std::string state) const = 0;
    virtual std::string_view asking_result_type() const = 0;
    // delivery requests: the message that asks the client one request under the session's own id.
    virtual json asking_request(const RequestId& id, std::string method, json params) const = 0;
};

// How change notifications reach a client.
enum class FeedDelivery {
    // As notifications of the connection a handshake opened, once the client said it is ready; the client watches
    // resources with requests that add and remove a URI.
    connection,
    // On subscriptions: a request the client sends opens one, its answer is a stream of the notifications it asked
    // for, and it ends when the client or the server ends it.
    subscription
};

// What a request of a change feed asks.
struct FeedRequest {
    enum class Action {
        watch,    // connection: be told of the updates of `uri`
        unwatch,  // connection: no longer
        listen    // subscription: open one that tells of what `filter` names
    };
    Action action = Action::watch;
    std::string uri;
    ChangeFilter filter;
};

// A notification a change feed writes: its method, and its params (an object; empty when it has none).
struct FeedMessage {
    std::string method;
    json params = json::object();
};

// Slot `change_feed`: how a server tells its clients that what it offers changed.
class ChangeFeedMechanism {
public:
    virtual ~ChangeFeedMechanism() = default;

    // Why the mechanism cannot serve the profile; empty when it can.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;
    virtual FeedDelivery delivery() const = 0;

    // Whether a client request of `method` is one of the mechanism's, and what such a request asks (or why it is
    // malformed). The session asks only about requests the profile has.
    virtual bool serves(std::string_view method) const = 0;
    virtual std::variant<FeedRequest, RequestError> read(const Message& request) const = 0;

    // delivery connection: whether a client notification says the client is ready for the connection's
    // notifications. delivery subscription: always false.
    virtual bool readies(const Message& notification) const = 0;

    // The notification that tells of a change.
    virtual FeedMessage notification(const Change& change) const = 0;

    // delivery subscription. The first message of a subscription, which tells what of the request is honoured; the
    // members that mark a message of the subscription `id` (the params of a notification, or a result); the notice
    // of a subscription the server ends, empty when the mechanism sends none; the result that ends it, and the name
    // of that result's type for the profile's filters. delivery connection: empty values.
    virtual FeedMessage acknowledgement(const ChangeFilter& honoured) const = 0;
    virtual void tag(json& members, const RequestId& id) const = 0;
    virtual std::optional<FeedMessage> ending_notice(const RequestId& id) const = 0;
    virtual json ending_result() const = 0;
    virtual std::string_view ending_result_type() const = 0;
};

// What a request of the requests of tasks asks.
struct TaskRequest {
    enum class Action {
        get,     // the task's state, with what it ended with or waits for
        update,  // answers to the task's requests for input
        cancel,  // stop the task
        result,  // what the task ended with: answered once it has ended, the task's requests for input meanwhile
        list     // the requester's tasks, a page at a time
    };
    Action action = Action::get;
    std::string task_id;              // empty for list
    json responses = json::object();  // update: the answers by key, an object
    std::optional<std::string> cursor;  // list: where the page starts; empty: at the first task
};

// What a call says about being answered with a task.
struct TaskAsk {
    bool takes = false;                            // the requester takes a task for an answer
    std::optional<std::chrono::milliseconds> ttl;  // the lifetime it asks for; empty: the store's default
};

// How a call of a tool is served, from what the tool declares and whether the requester takes a task.
enum class TaskServing {
    ordinary,           // as an ordinary call
    task,               // as a task
    method_not_found,   // refused as a method that does not exist
    missing_capability  // refused for a client capability the requester lacks (TaskMechanism::required_capabilities)
};

// Slot `tasks`: whether a call may be answered with a task the requester then asks about, and how tasks look on the
// wire. The session asks the mechanism; the tasks themselves are the host's (neomcp/task_host.h).
class TaskMechanism {
public:
    virtual ~TaskMechanism() = default;

    // Why the mechanism cannot serve the profile; empty when it can.
    virtual std::optional<std::string> check(const profile::Profile& profile) const = 0;
    // Whether a call may be answered with a task at all. False: every call is an ordinary one, the server declares
    // nothing about tasks, and no request is the mechanism's.
    virtual bool answers_with_tasks() const = 0;

    // Whether the requester of a request takes tasks, from what the request (or the connection's handshake) says
    // about its client.
    virtual bool requester_takes_tasks(const ClientIdentity& client) const = 0;
    // Whether the mechanism's own requests are answered only for a requester that takes tasks; the others are then
    // refused for the missing client capability.
    virtual bool requests_need_declaration() const = 0;
    // What a call (its params, and what it says about its client) says about being answered with a task, or why
    // what it says is malformed.
    virtual std::variant<TaskAsk, RequestError> asked(const json& params, const ClientIdentity& client) const = 0;
    // How a call of a tool that declares `support` is served when the requester takes a task or not.
    virtual TaskServing serving(TaskSupport support, bool takes) const = 0;
    // Whether the inputs a tool declares are asked by the call before a task exists (otherwise the task's handler
    // asks for them as it asks for any input).
    virtual bool prepares_inputs() const = 0;
    // What a requester that does not take tasks lacks, as the `requiredCapabilities` of the error for a missing client
    // capability.
    virtual json required_capabilities() const = 0;
    // Adds to the server's capabilities object that it answers with tasks; `lists`: the requester's tasks are bound to
    // it (it carries an authorization), so that it may list them.
    virtual void declare(json& capabilities, bool lists) const = 0;
    // Adds to a tool as a listing writes it whether it may run as a task.
    virtual void describe_tool(json& tool, TaskSupport support) const = 0;

    // Whether a client request of `method` is one of the mechanism's, and what such a request asks (or why it is
    // malformed). The session asks only about requests the profile has.
    virtual bool serves(std::string_view method) const = 0;
    virtual std::variant<TaskRequest, RequestError> read(const Message& request) const = 0;

    // The body of the result that answers a call with the task `record`.
    virtual json created(const TaskRecord& record) const = 0;
    // The body of the answer to a read of the task: its state, with what it ended with or the requests it waits on.
    virtual json detailed(const TaskRecord& record) const = 0;
    // The body of the answer to an update that was taken.
    virtual json acknowledgement() const = 0;
    // The body of the answer to a cancellation that was taken; `record` is the task after it.
    virtual json cancelled(const TaskRecord& record) const = 0;
    // Whether a cancellation of a task that has ended is taken (and changes nothing); otherwise it is refused.
    virtual bool acknowledges_ended_cancel() const = 0;
    // The answer to a result request for the task `record`, which has ended: the body of the result, or the error.
    virtual std::variant<json, RequestError> payload(const TaskRecord& record) const = 0;
    // The body of the answer to a listing.
    virtual json page(const TaskPage& page) const = 0;
    // Marks the params of a request the server sends the requester for the task `task_id`.
    virtual void relate(json& params, std::string_view task_id) const = 0;
    // The notification that tells the requester of a task's new status; empty when the mechanism sends none.
    virtual std::optional<FeedMessage> status_notice(const TaskRecord& record) const = 0;

    // How a task started under the mechanism keeps its record (see TaskStart in neomcp/task_host.h).
    virtual bool error_result_fails() const = 0;
    virtual bool round_keys() const = 0;
    // A request for input as the record keeps it, from its method and its params as the revision writes them (null:
    // it has none); and the method and the params of such an entry, empty (null) when it is not one.
    virtual json input_entry(std::string method, json params) const = 0;
    virtual std::optional<std::string> input_method(const json& entry) const = 0;
    virtual json input_params(const json& entry) const = 0;
    // Whether the progress and the log messages of a task's handler reach the requester.
    virtual bool task_notifications() const = 0;
};

// The slots of the registry.
enum class MechanismSlot { open, state, batch, cancel, server_input, change_feed, tasks };
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
    void add_input(std::string name, Factory<InputMechanism> factory);
    void add_feed(std::string name, Factory<ChangeFeedMechanism> factory);
    void add_tasks(std::string name, Factory<TaskMechanism> factory);
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
    std::unique_ptr<InputMechanism> make_input(std::string_view name) const;
    std::unique_ptr<ChangeFeedMechanism> make_feed(std::string_view name) const;
    std::unique_ptr<TaskMechanism> make_tasks(std::string_view name) const;

private:
    using Key = std::pair<MechanismSlot, std::string>;
    using AnyFactory = std::variant<Factory<OpenMechanism>, Factory<StateMechanism>, Factory<BatchMechanism>,
                                    Factory<CancelMechanism>, Factory<InputMechanism>, Factory<ChangeFeedMechanism>,
                                    Factory<TaskMechanism>>;

    template <typename Interface>
    void put(MechanismSlot slot, std::string name, Factory<Interface> factory);
    template <typename Interface>
    std::unique_ptr<Interface> make(MechanismSlot slot, std::string_view name) const;

    std::map<Key, AnyFactory> factories_;
};

// The mechanisms of the library: every name profile::declared_mechanisms declares for open, state, server_input,
// change_feed, batch and tasks, and every name profile::declared_cancel declares for a transport.
const MechanismRegistry& builtin_mechanisms();

}  // namespace neomcp
