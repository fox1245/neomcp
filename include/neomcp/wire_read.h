// Reading model values from the wire: the inverses of the writers of wire.h.
//
// What a peer sends is not trusted, so a reader never throws for malformed input: it returns the value, or a
// description of the first thing that is wrong. The description names the place as a path from the value that was
// given ("$.content[1].resource.uri: is required") and never repeats a text the peer sent.
//
// Like a writer, a reader takes the members a type has from the profile (`wire.types`): a member the revision's type
// does not have is not read, and a member the type requires and the value lacks is an error. Members that no member of
// the model stands for are ignored. The exceptions are the content blocks, which keep in `extra` the members the
// revision's type of the block has and the model has no member for, and `_meta`, which is kept wherever the model has
// a place for it and must be an object. Two kinds of member are not part of a value and are not read here: the ones the
// protocol engine adds to a result (`resultType`, and the caching hints, which read_cache_hints reads), and the ones
// the model has no place for.
//
// A reader accepts what the writer of the same profile writes. It is stricter than that where the schemas are: an
// `inputSchema`, `outputSchema`, `structuredContent` and `_meta` must be JSON objects, and a completion has at most
// max_completion_values values. A reader decides from the profile's types and the value's own members, fetches
// nothing and resolves no reference.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"

namespace neomcp {

// The name of the value that was given, as the first part of the path in a description.
inline constexpr std::string_view wire_root_path = "$";

// The value, or what is wrong with the wire form.
template <typename T>
using WireRead = std::variant<T, std::string>;

// Reads a T that the writer to_wire(const T&, profile) writes; `at` is the path of `wire` in a larger value (the
// description of a problem starts with it). Defined for Icon, Implementation, ToolAnnotations, ToolDefinition,
// ContentBlock, ToolUseContent, ToolResultContent, ResourceDefinition, ResourceTemplateDefinition, ResourceContents,
// ReadResult, PromptDefinition, PromptMessage, PromptResult, CompletionResult and ToolResult; any other T does not link.
//
// A ContentBlock is read by its `type`. A kind the revision does not define, and a `type` this library has no block
// for, are errors: the pages say nothing of how a client treats a block it does not know, and a block read wrongly
// would put wrong content before the user. A ToolUseContent or a ToolResultContent is read where the revision defines
// its type and is an error elsewhere; its `type` must be its own. The `input` of a tool use must be an object; the
// structured content of a tool result is kept whatever JSON value it is, as the schemas differ in what they allow. A
// ReadResult also gets the caching hints of its result when it has them.
template <typename T>
WireRead<T> from_wire(const json& wire, const profile::Profile& profile, std::string_view at = wire_root_path);

template <>
WireRead<Icon> from_wire<Icon>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<Implementation> from_wire<Implementation>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ToolAnnotations> from_wire<ToolAnnotations>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ToolDefinition> from_wire<ToolDefinition>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ContentBlock> from_wire<ContentBlock>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ToolUseContent> from_wire<ToolUseContent>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ToolResultContent> from_wire<ToolResultContent>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ResourceDefinition> from_wire<ResourceDefinition>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ResourceTemplateDefinition> from_wire<ResourceTemplateDefinition>(const json&, const profile::Profile&,
                                                                           std::string_view);
template <>
WireRead<ResourceContents> from_wire<ResourceContents>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ReadResult> from_wire<ReadResult>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<PromptDefinition> from_wire<PromptDefinition>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<PromptMessage> from_wire<PromptMessage>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<PromptResult> from_wire<PromptResult>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<CompletionResult> from_wire<CompletionResult>(const json&, const profile::Profile&, std::string_view);
template <>
WireRead<ToolResult> from_wire<ToolResult>(const json&, const profile::Profile&, std::string_view);

// The caching hints (`ttlMs`, `cacheScope`) of a result whose type in the profile is `result_type_name`. Empty when the
// profile's type has neither member, or has them without requiring them and the result has neither. A member the type
// requires and the result lacks is an error, although the pages let a client assume `0` for a missing `ttlMs`
// (server/utilities/caching: the revision's types require both members, so a result without them is malformed). A
// negative `ttlMs` is read as 0, as the pages ask; a `ttlMs` that is not an integer, and a `cacheScope` that is
// neither "public" nor "private", are errors.
WireRead<std::optional<CacheHints>> read_cache_hints(const json& result, std::string_view result_type_name,
                                                     const profile::Profile& profile,
                                                     std::string_view at = wire_root_path);

}  // namespace neomcp
