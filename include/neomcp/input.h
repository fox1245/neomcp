// Input requests: what a call of a tool, a prompt or a resource read asks its client for while it is served (a form
// the user fills in, a completion from the client's model, the client's roots), and the answers it gets back.
//
// A host answers a call with InputRequired instead of a result when it needs something only the client can give.
// The session then asks the client, in the way the revision the call is served under prescribes, and calls the host
// again with the answers by the keys the host chose. Which way that is the session reads from the revision's profile
// (mechanism `server_input`): either the call's result asks and the client retries the call with the answers, or the
// session sends each request to the client and waits for the responses. A host writes the same code for both.
//
// These are revision-independent values, like the rest of neomcp/model.h; the session writes them for the revision.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "neomcp/json.h"
#include "neomcp/model.h"

namespace neomcp {

// ---------------------------------------------------------------------------------------------------------------
// Requests

// Asks the user to fill in a form (elicitation in form mode). The client shows `message` and a form built from
// `requested_schema`, a JSON Schema of a flat object whose properties are strings, numbers, booleans or enums of
// strings (the restricted form the specification defines). Needs the client's `elicitation` capability for forms.
struct ElicitationForm {
    std::string message;
    json requested_schema = json::object();
};

// Asks the user to visit a URL out of band, for interactions that must not pass through the client (elicitation in
// URL mode). The answer says whether the user agreed to go there, never what happened there. Needs the client's
// `elicitation` capability for URLs. `elicitation_id` is written only where the revision's request has the member;
// where the revision requires it and it is empty, the session makes one from the operating system's random source.
struct ElicitationUrl {
    std::string message;
    std::string url;
    std::optional<std::string> elicitation_id;
};

// Whether the client should include context from MCP servers in a sampling request.
enum class IncludeContext { none, this_server, all_servers };

// One message of a sampling request: who says it, and one or more text, image or audio blocks. Several blocks are
// written as an array only where the revision allows one.
struct SamplingMessage {
    Role role = Role::user;
    std::vector<ContentBlock> content;
};

// The server's preferences for the model the client picks; every member is a hint the client may ignore.
struct ModelPreferences {
    std::vector<std::string> hints;  // model names, best first
    std::optional<double> cost_priority;
    std::optional<double> speed_priority;
    std::optional<double> intelligence_priority;
};

// Asks the client's model for a completion (sampling). Needs the client's `sampling` capability; a request with
// tools needs its `sampling.tools`, and context from servers (this_server, all_servers) its `sampling.context`.
struct SamplingRequest {
    std::vector<SamplingMessage> messages;
    std::uint64_t max_tokens = 0;
    std::optional<std::string> system_prompt;
    std::optional<ModelPreferences> model_preferences;
    std::optional<double> temperature;
    std::vector<std::string> stop_sequences;
    std::optional<IncludeContext> include_context;
    std::optional<json> metadata;     // an object passed through to the model's provider
    std::vector<ToolDefinition> tools;  // tools the model may use during this sampling
    std::optional<json> tool_choice;  // how the model may use them, as the revision's ToolChoice object
};

// Asks the client for its roots. Needs the client's `roots` capability.
struct RootsRequest {};

using InputRequest = std::variant<ElicitationForm, ElicitationUrl, SamplingRequest, RootsRequest>;

// ---------------------------------------------------------------------------------------------------------------
// Answers

enum class ElicitationAction {
    accept,   // the user submitted the form or agreed to visit the URL
    decline,  // the user explicitly declined
    cancel    // the user dismissed the request without choosing
};

struct ElicitationAnswer {
    ElicitationAction action = ElicitationAction::cancel;
    // The submitted fields when a form was accepted: an object whose values are strings, numbers, booleans or arrays
    // of strings. Empty otherwise. The session checks its shape, not that it matches the requested schema.
    json content = json::object();
};

// The completion the client's model produced.
struct SamplingAnswer {
    Role role = Role::assistant;
    json content;  // the content block, or the array of blocks, as the client sent it
    std::string model;  // the model that produced it
    std::optional<std::string> stop_reason;
};

struct Root {
    std::string uri;
    std::optional<std::string> name;
    std::optional<json> meta;  // an object, written as `_meta`
};

struct RootsAnswer {
    std::vector<Root> roots;
};

using InputResponse = std::variant<ElicitationAnswer, SamplingAnswer, RootsAnswer>;

// The answers of one round, by the keys the host chose for its requests. A key the client did not answer is absent.
using InputResponses = std::map<std::string, InputResponse>;

// ---------------------------------------------------------------------------------------------------------------
// The outcome that asks

// What a host answers a call with when it needs input: the requests by keys of its own choosing (unique by
// construction), and optionally a state of its own that comes back with the answers. The host is called again with
// the answers and the state, and decides from them what is still missing; it may ask again. Without requests it asks
// to be called again (for example to shed load), with its state.
//
// The state is the host's. The library never lets it travel as it is: where it passes through the client it is
// sealed (signed, not encrypted: the client can read it, so it must hold no secret), and where the session keeps it
// nothing travels at all.
struct InputRequired {
    std::map<std::string, InputRequest> requests;
    std::optional<std::string> state;
};

}  // namespace neomcp
