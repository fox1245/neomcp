// Revision profiles: an immutable, validated view of one profile document.
//
// A profile says how one revision of the Model Context Protocol behaves by naming mechanisms (see Slot) and lists
// what the revision puts on the wire. `load` checks a document strictly: an unknown member, a missing member, a
// value of the wrong JSON type or a value that contradicts another part of the document is an Error that names the
// place. The profiles under profiles/ are built into the library (see builtin).
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neomcp::profile {

// Why a document was rejected.
struct Error {
    // RFC 6901 JSON Pointer to the offending place in the document ("" is the whole document). Characters '~' and
    // '/' in member names are escaped as "~0" and "~1". For a missing member the pointer names the member that
    // should be there, so it points at a place that does not exist in the document.
    std::string pointer;
    // What was expected at that place.
    std::string expected;
    // What was found instead.
    std::string message;
};

// The specification's own terms for the two generations of revisions.
enum class Era { legacy, modern };

// The places where revisions differ in how they work. A profile selects one named mechanism for each slot.
enum class Slot { open, state, server_input, change_feed, batch };
inline constexpr std::size_t slot_count = 5;

// The member of `mechanisms` that holds the slot, and the document's spelling of the era.
std::string_view to_string(Slot slot) noexcept;
std::string_view to_string(Era era) noexcept;

// The transports a profile can describe.
enum class Transport { stdio, http_sse, streamable_http };

// The member of `transports` that describes the transport.
std::string_view to_string(Transport transport) noexcept;

// The names the library declares. A profile may select only these, and code that implements a mechanism attaches
// to one of them: the mechanisms of a slot, the cancellation mechanisms of a transport, the ways a revision
// reports tool arguments that fail the tool's input schema (`tools.invalid_arguments`), and the reasons a
// Streamable HTTP status map gives a status for (`transports.streamable_http.status`).
std::span<const std::string_view> declared_mechanisms(Slot slot);
std::span<const std::string_view> declared_cancel(Transport transport);
std::span<const std::string_view> declared_invalid_arguments();
std::span<const std::string_view> declared_status_reasons();
// The ways a revision says a client finds the authorization server of a server that refuses it
// (`authorization.discovery`).
std::span<const std::string_view> declared_authorization_discovery();

struct Evidence {
    std::string verified_at;
    std::string repository;
    std::string commit;
    std::vector<std::string> pages;
    std::string schema;
};

struct StdioTransport {
    std::string cancel;  // the declared name of the cancellation mechanism
};

// The older transport of events over HTTP: the client holds an event stream open (a GET), whose first event names
// where its messages go (each one a POST there), and the server's messages arrive as events of the stream.
struct HttpSseTransport {
    std::string cancel;
    // The type of the event that names where the client's messages go: the stream's first event, its data a URI
    // reference.
    std::string endpoint_event;
    // The type of the events whose data is one message of the server.
    std::string message_event;
};

// The version a request without a version header is taken to speak.
struct VersionHeader {
    std::string name;
    std::string absent_means;
};

enum class HeaderValue {
    protocol_version,  // the revision being spoken
    method,            // the JSON-RPC method of the message
    params_member      // a member of the message's params, named per method
};

enum class HeaderScopeKind {
    every_message,
    every_request,
    per_method  // only the methods listed in RequestHeader::methods
};

struct MethodMember {
    std::string method;
    std::string member;  // the params member whose value the header carries
};

struct RequestHeader {
    std::string name;
    HeaderValue value = HeaderValue::protocol_version;
    HeaderScopeKind scope = HeaderScopeKind::every_message;
    std::vector<MethodMember> methods;  // filled when scope is per_method, in document order

    // The params member the header carries for `method`, or empty when the header does not apply to it.
    std::optional<std::string_view> params_member(std::string_view method) const;
};

// Headers that carry tool arguments: a schema keyword marks a parameter, the header name is prefix + parameter.
struct ArgumentHeaders {
    std::string schema_keyword;
    std::string header_prefix;
};

// The framing of a header value that is not plain ASCII.
struct EncodedHeaderValue {
    std::string prefix;
    std::string suffix;
};

// The HTTP status a revision prescribes for a reason, one of declared_status_reasons().
struct NamedStatus {
    std::string reason;
    int status = 0;  // 100 to 599
};

struct StreamableHttpTransport {
    std::string cancel;
    std::optional<std::string> session_header;
    std::optional<VersionHeader> version_header;
    bool get_stream = false;
    bool resumable = false;
    bool delete_ends_session = false;
    std::vector<RequestHeader> request_headers;
    std::optional<ArgumentHeaders> argument_headers;
    std::optional<EncodedHeaderValue> encoded_header_value;
    // The statuses the revision prescribes, sorted by reason; a reason it prescribes none for is not listed, and the
    // transport answers it with a status of its own choosing.
    std::vector<NamedStatus> status;

    // The status the revision prescribes for the reason; empty when it prescribes none.
    std::optional<int> status_for(std::string_view reason) const;
};

// The endpoints of an authorization server that publishes no metadata, as paths on the origin of the server's URL.
struct DefaultEndpoints {
    std::string authorization;
    std::string token;
    std::string registration;
};

// Discovery on the server's origin: the authorization server's metadata at a path of the origin of the server's URL
// (the URL's path is discarded), and the default endpoints when no metadata is found there.
struct OriginMetadataDiscovery {
    std::string metadata_path;
    DefaultEndpoints default_endpoints;
};

// What the revision decides for authorization.
struct Authorization {
    // How a client finds the authorization server: one of declared_authorization_discovery().
    std::string_view discovery;
    // Present exactly when `discovery` is the discovery on the server's origin.
    std::optional<OriginMetadataDiscovery> origin_metadata;
};

struct NamedCode {
    std::string name;
    int code = 0;
};

struct WireSchema {
    std::string dialect;
    std::string sha256;
};

// A request or notification of the revision.
struct Message {
    std::string method;
    bool from_client = false;
    bool from_server = false;
    std::string type;                    // the message's type name
    std::optional<std::string> params;   // the params type name, when the message has one
    std::vector<std::string> params_required;  // sorted
};

// A request a server may place in a result for the client to answer.
struct InputRequest {
    std::string method;
    std::string type;
    std::optional<std::string> params;
    std::vector<std::string> params_required;  // sorted
};

// An object type: its properties and the ones that must be present.
struct Type {
    std::string name;
    std::vector<std::string> properties;  // sorted
    std::vector<std::string> required;    // sorted, each one also a property
    // The values the type's `type` property may have, where the schema lists them (a constant or an enumeration of
    // strings); empty when it does not. When not empty, `type` is one of the properties.
    std::vector<std::string> type_values;  // sorted

    bool has_property(std::string_view property) const;
    bool is_required(std::string_view property) const;
    bool has_type_value(std::string_view value) const;
};

// A type that is a union of other types: each alternative names an entry of the types or of the unions.
struct Union {
    std::string name;
    std::vector<std::string> alternatives;  // distinct, in document order
};

class Profile {
public:
    const std::string& revision() const noexcept { return revision_; }
    Era era() const noexcept { return era_; }
    const Evidence& evidence() const noexcept { return evidence_; }

    // The name of the mechanism the profile selects for the slot.
    std::string_view mechanism(Slot slot) const noexcept;

    // How the revision reports tool arguments that fail the tool's input schema (member `tools.invalid_arguments`):
    // one of declared_invalid_arguments().
    std::string_view invalid_arguments() const noexcept { return invalid_arguments_; }

    // What the revision decides for authorization (member `authorization`).
    const Authorization& authorization() const noexcept { return authorization_; }

    // Present when the revision defines the transport.
    const std::optional<StdioTransport>& stdio() const noexcept { return stdio_; }
    const std::optional<HttpSseTransport>& http_sse() const noexcept { return http_sse_; }
    const std::optional<StreamableHttpTransport>& streamable_http() const noexcept { return streamable_http_; }

    // The error codes the revision defines, sorted by name, and the one for a name.
    std::span<const NamedCode> error_codes() const noexcept { return error_codes_; }
    std::optional<int> error_code(std::string_view name) const;
    // Codes the revision forbids a sender to use (empty when the revision lists none).
    std::span<const int> not_emitted() const noexcept { return not_emitted_; }

    const WireSchema& wire_schema() const noexcept { return wire_schema_; }

    // The wire tables, sorted by method (types by name).
    std::span<const Message> requests() const noexcept { return requests_; }
    std::span<const Message> notifications() const noexcept { return notifications_; }
    std::span<const InputRequest> input_requests() const noexcept { return input_requests_; }
    std::span<const Type> types() const noexcept { return types_; }
    std::span<const Union> unions() const noexcept { return unions_; }  // sorted by name
    std::span<const std::string> unlisted_message_types() const noexcept { return unlisted_message_types_; }

    // Lookups; null when the table has no such entry. The pointers stay valid as long as the profile does.
    const Message* request(std::string_view method) const;
    const Message* notification(std::string_view method) const;
    const InputRequest* input_request(std::string_view method) const;
    const Type* type(std::string_view name) const;
    const Union* union_type(std::string_view name) const;

    // Whether an error response of the revision may leave out `id`, as an answer to a message whose id could not be
    // read must when the revision's types allow it: the types list at least one type that has the JSON-RPC members
    // `error` and `id`, and none of them requires `id`. False when a revision requires `id` in its error responses
    // (the answer then carries `id` as null, the one form JSON-RPC itself prescribes), and false when the types list no
    // such type.
    bool error_response_may_omit_id() const;

private:
    friend class ProfileLoader;
    Profile() = default;

    std::string revision_;
    Era era_ = Era::legacy;
    Evidence evidence_;
    std::vector<std::string_view> mechanisms_;  // indexed by Slot; views of the declared names
    std::string_view invalid_arguments_;        // a view of a declared name
    Authorization authorization_;
    std::optional<StdioTransport> stdio_;
    std::optional<HttpSseTransport> http_sse_;
    std::optional<StreamableHttpTransport> streamable_http_;
    std::vector<NamedCode> error_codes_;
    std::vector<int> not_emitted_;
    WireSchema wire_schema_;
    std::vector<Message> requests_;
    std::vector<Message> notifications_;
    std::vector<InputRequest> input_requests_;
    std::vector<std::string> unlisted_message_types_;
    std::vector<Type> types_;
    std::vector<Union> unions_;
};

using ProfilePtr = std::shared_ptr<const Profile>;

// Checks a profile document and returns the profile, or the first place that is wrong. Bad input never throws.
std::variant<ProfilePtr, Error> load(std::string_view json_text);

// The profiles built into the library: every profiles/*.json of the source tree, by revision.
// Revisions are sorted ascending.
std::span<const std::string_view> builtin_revisions();
// The document text of a built-in revision; empty when there is no such revision.
std::string_view builtin_text(std::string_view revision);
// The loaded profile of a built-in revision, loaded once and cached; null when there is no such revision.
// Thread-safe. A built-in document that fails to load is a defect of the library and throws std::logic_error
// naming the revision and the pointer.
ProfilePtr builtin(std::string_view revision);

}  // namespace neomcp::profile
