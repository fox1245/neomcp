// Writing model values for one protocol revision.
//
// Each writer builds the value with every member the model has and then keeps only the members the revision's
// type has (`wire.types` of the profile), through the one filter `keep_members`. A revision string never decides
// what is written: a member exists on the wire exactly when the revision's schema gives the type that property.
#pragma once

#include <string_view>

#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

// Keeps the members of `object` that the profile's type `type_name` has, in their order, and drops the others.
// Returns false and leaves the value unchanged when the profile has no such type or the value is not an object.
bool keep_members(json& object, std::string_view type_name, const profile::Profile& profile);
// The same for a type already found, for example the type of a property (Profile::property_type). Returns false
// and leaves the value unchanged when the value is not an object.
bool keep_members(json& object, const profile::Type& type);

// Whether the revision defines the content block kind: the profile has the kind's type (type_name(kind)).
bool has_content_kind(ContentKind kind, const profile::Profile& profile);

// Type `Tool`; its annotations as `ToolAnnotations` and its icons as `Icon`.
json to_wire(const ToolDefinition& tool, const profile::Profile& profile);
// Type `ToolAnnotations`.
json to_wire(const ToolAnnotations& annotations, const profile::Profile& profile);
// Type `Implementation`.
json to_wire(const Implementation& implementation, const profile::Profile& profile);
// Type `Icon`.
json to_wire(const Icon& icon, const profile::Profile& profile);
// The block's own type (type_name of its kind) with its annotations as the type its type has for them and an
// embedded resource as `TextResourceContents` or `BlobResourceContents`. A block of a kind the revision does not
// define is written as a text block that names the kind.
json to_wire(const ContentBlock& block, const profile::Profile& profile);
// Type `ToolUseContent`, a block of sampling only. A revision without the type writes the block with every member,
// as the model has it: the writers of sampling refuse such a block before they get here.
json to_wire(const ToolUseContent& block, const profile::Profile& profile);
// Type `ToolResultContent`, a block of sampling only, its content blocks as above. As for ToolUseContent, a revision
// without the type keeps every member.
json to_wire(const ToolResultContent& block, const profile::Profile& profile);
// Type `Resource`; its annotations as the type it has for them, and its icons as `Icon`.
json to_wire(const ResourceDefinition& resource, const profile::Profile& profile);
// Type `ResourceTemplate`.
json to_wire(const ResourceTemplateDefinition& resource_template, const profile::Profile& profile);
// Type `TextResourceContents` or `BlobResourceContents`.
json to_wire(const ResourceContents& contents, const profile::Profile& profile);
// Type `ReadResourceResult`. Members the protocol engine adds (`resultType`, the caching hints) are not written
// here.
json to_wire(const ReadResult& result, const profile::Profile& profile);
// Type `Prompt`; its arguments as `PromptArgument` and its icons as `Icon`.
json to_wire(const PromptDefinition& prompt, const profile::Profile& profile);
// Type `PromptMessage`, its content as above.
json to_wire(const PromptMessage& message, const profile::Profile& profile);
// Type `GetPromptResult`. Members the protocol engine adds (`resultType`) are not written here.
json to_wire(const PromptResult& result, const profile::Profile& profile);
// Type `CompleteResult`, with at most max_completion_values values (see CompletionResult). Members the protocol
// engine adds (`resultType`) are not written here.
json to_wire(const CompletionResult& result, const profile::Profile& profile);
// Type `CallToolResult`, its content blocks as above. Members the protocol engine adds (such as `resultType`) are
// not part of the model and are not written here.
json to_wire(const ToolResult& result, const profile::Profile& profile);

}  // namespace neomcp
