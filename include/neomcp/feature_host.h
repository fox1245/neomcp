// The interfaces through which a session reaches resources, prompts and completion, the way it reaches tools through a
// ToolHost (see tool_host.h for the CallControl and Completion handles every call gets).
//
// A host that offers one of these returns itself from the matching accessor of ToolHost. A session lists what the host
// holds and pages the lists itself; a call of the host runs the thing asked for and completes exactly once, inline or
// later from any thread, with a FeatureOutcome (or a Refusal or HostFailure).
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/tool_host.h"

namespace neomcp {

// What a session asks of a host about one of its lists.
struct ListSettings {
    // Items per page; empty: one page with everything. Zero is an error of the host.
    std::optional<std::size_t> page_size;
    // Caching hints on the list's results, where the revision has them.
    CacheHints cache;
};

struct ReadRequest {
    std::string uri;
    json meta = nullptr;  // the request's params._meta; null when absent
    // The answers and the state of an earlier round, as for CallRequest.
    InputResponses input_responses;
    std::optional<std::string> request_state;
};

class ResourceHost {
public:
    virtual ~ResourceHost() = default;

    // The resources and the templates, each in a stable order. The spans stay valid and their contents unchanged
    // while a session uses the host.
    virtual std::span<const ResourceDefinition> resources() const = 0;
    virtual std::span<const ResourceTemplateDefinition> resource_templates() const = 0;
    // Paging and caching of resources/list and resources/templates/list.
    virtual ListSettings resource_list_settings() const { return {}; }

    // Reads the resource `request.uri`: completes with a ReadResult, with NotFound when the host offers nothing at
    // that URI, with InputRequired, a Refusal or a HostFailure. Must not block for the duration of the read.
    virtual void read(const ReadRequest& request, CallControl control, Completion done) = 0;
};

struct PromptRequest {
    std::string name;
    // The arguments the request carried, all strings. A session has checked that every required one is there.
    std::map<std::string, std::string> arguments;
    json meta = nullptr;  // the request's params._meta; null when absent
    // The answers and the state of an earlier round, as for CallRequest.
    InputResponses input_responses;
    std::optional<std::string> request_state;
};

class PromptHost {
public:
    virtual ~PromptHost() = default;

    // The prompts, in a stable order. The span stays valid and its contents unchanged while a session uses the host.
    virtual std::span<const PromptDefinition> prompts() const = 0;
    // Paging and caching of prompts/list.
    virtual ListSettings prompt_list_settings() const { return {}; }

    // Gets the prompt `request.name`, one of prompts(): completes with a PromptResult, with InvalidRequest when its
    // arguments cannot be used, with InputRequired, a Refusal or a HostFailure. Must not block for the duration of the
    // call.
    virtual void get(const PromptRequest& request, CallControl control, Completion done) = 0;
};

// What a completion request refers to.
enum class CompletionRefKind {
    prompt,   // `ref/prompt`: `reference` is the prompt's name
    resource  // `ref/resource`: `reference` is the URI or URI template of the resource
};

struct CompletionRequest {
    CompletionRefKind kind = CompletionRefKind::prompt;
    std::string reference;
    std::string argument;  // the name of the argument being completed
    std::string value;     // what has been typed of it
    // The arguments already resolved, from the request's `context` where it carries them; all strings.
    std::map<std::string, std::string> resolved;
    json meta = nullptr;  // the request's params._meta; null when absent
};

class CompletionHost {
public:
    virtual ~CompletionHost() = default;

    // Completes with a CompletionResult (empty when the host can suggest nothing for the argument), with
    // InvalidRequest when the reference names nothing the host offers, with a Refusal or a HostFailure. Must not block
    // for the duration of the call.
    virtual void complete(const CompletionRequest& request, CallControl control, Completion done) = 0;
};

}  // namespace neomcp
