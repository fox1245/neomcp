// The values a server describes and returns, independent of any protocol revision. How a value is written for
// one revision is the job of wire.h, which keeps only the members that revision's types have.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "neomcp/json.h"

namespace neomcp {

enum class IconTheme { light, dark };

struct Icon {
    std::string src;  // an https URL or a data: URI
    std::optional<std::string> mime_type;
    std::vector<std::string> sizes;  // "WxH" or "any"; empty when not given
    std::optional<IconTheme> theme;
};

// Who a client or a server is.
struct Implementation {
    std::string name;
    std::string version;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> website_url;
    std::vector<Icon> icons;
};

// Hints about a tool's behaviour; a hint left empty is not written, and the client applies the specification's
// default for it.
struct ToolAnnotations {
    std::optional<std::string> title;
    std::optional<bool> read_only_hint;
    std::optional<bool> destructive_hint;
    std::optional<bool> idempotent_hint;
    std::optional<bool> open_world_hint;
};

struct ToolDefinition {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    json input_schema = json{{"type", "object"}};
    std::optional<json> output_schema;
    std::optional<ToolAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<json> meta;  // an object, written as `_meta`
};

enum class Role { user, assistant };

// Annotations on a content block.
struct ContentAnnotations {
    std::vector<Role> audience;
    std::optional<double> priority;
    std::optional<std::string> last_modified;  // ISO 8601
};

enum class ContentKind { text, image, audio, resource_link, embedded_resource };

// Every content block also has `annotations`, `meta` (an object, written as `_meta`) and `extra`: members this
// model does not know, by their wire names (an object). Extra members are written as given and, like every other
// member, only when the revision's type of the block has them.

struct TextContent {
    static constexpr ContentKind kind = ContentKind::text;
    std::string text;
    std::optional<ContentAnnotations> annotations;
    std::optional<json> meta;
    json extra = json::object();
};

struct ImageContent {
    static constexpr ContentKind kind = ContentKind::image;
    std::string data;  // base64
    std::string mime_type;
    std::optional<ContentAnnotations> annotations;
    std::optional<json> meta;
    json extra = json::object();
};

struct AudioContent {
    static constexpr ContentKind kind = ContentKind::audio;
    std::string data;  // base64
    std::string mime_type;
    std::optional<ContentAnnotations> annotations;
    std::optional<json> meta;
    json extra = json::object();
};

struct ResourceLink {
    static constexpr ContentKind kind = ContentKind::resource_link;
    std::string uri;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<std::uint64_t> size;
    std::vector<Icon> icons;
    std::optional<ContentAnnotations> annotations;
    std::optional<json> meta;
    json extra = json::object();
};

struct TextResourceContents {
    std::string uri;
    std::optional<std::string> mime_type;
    std::string text;
    std::optional<json> meta;
};

struct BlobResourceContents {
    std::string uri;
    std::optional<std::string> mime_type;
    std::string blob;  // base64
    std::optional<json> meta;
};

struct EmbeddedResource {
    static constexpr ContentKind kind = ContentKind::embedded_resource;
    std::variant<TextResourceContents, BlobResourceContents> resource;
    std::optional<ContentAnnotations> annotations;
    std::optional<json> meta;
    json extra = json::object();
};

using ContentBlock = std::variant<TextContent, ImageContent, AudioContent, ResourceLink, EmbeddedResource>;

// The blocks below belong to sampling only (neomcp/input.h): a message of the model may ask to use a tool, and the
// message that follows it gives the tool's result. They exist where the revision's schema defines their types
// (`ToolUseContent`, `ToolResultContent`); they are never part of a tool's result or a prompt. Like the blocks above,
// each has `meta` (written as `_meta`) and `extra`, the members the revision's type has and this model does not know.

// The model asks to use a tool (`type: "tool_use"`).
struct ToolUseContent {
    std::string id;               // unique within the conversation; the result that answers it names it
    std::string name;             // the tool's name
    json input = json::object();  // the arguments, an object that fits the tool's input schema
    std::optional<json> meta;
    json extra = json::object();
};

// The result of a tool use, given back to the model (`type: "tool_result"`).
struct ToolResultContent {
    std::string tool_use_id;                  // the id of the tool use this answers
    std::vector<ContentBlock> content;        // as a tool's result holds them
    std::optional<json> structured_content;  // kept as given: the revisions' schemas differ in what they allow
    bool is_error = false;
    std::optional<json> meta;
    json extra = json::object();
};

ContentKind kind_of(const ContentBlock& block) noexcept;
// The block's `type` on the wire ("text", "image", "audio", "resource_link", "resource").
std::string_view wire_name(ContentKind kind) noexcept;
// The name of the block's type in the revisions' schemas ("TextContent", ..., "EmbeddedResource").
std::string_view type_name(ContentKind kind) noexcept;

// The outcome of a tool call.
struct ToolResult {
    std::vector<ContentBlock> content;
    std::optional<json> structured_content;
    bool is_error = false;
    std::optional<json> meta;  // an object, written as `_meta`
};

// Log levels in increasing severity (RFC 5424 order, reversed).
enum class LogLevel { debug, info, notice, warning, error, critical, alert, emergency };

std::string_view to_string(LogLevel level) noexcept;
std::optional<LogLevel> parse_log_level(std::string_view text) noexcept;

// Who may keep a cached result (`cacheScope`).
enum class CacheScope { private_scope, public_scope };
std::string_view to_string(CacheScope scope) noexcept;

// Caching hints a revision that has them requires on list, read and discovery results (`ttlMs`, `cacheScope`).
struct CacheHints {
    std::uint64_t ttl_ms = 0;                      // 0: stale at once
    CacheScope scope = CacheScope::private_scope;  // reuse only within the same authorization context
};

// ---------------------------------------------------------------------------------------------------------------
// Resources

// A resource the server can read, for resources/list. `annotations` are the resource's own (who it is for, how
// important, when it changed).
struct ResourceDefinition {
    std::string uri;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
    std::optional<std::uint64_t> size;  // bytes of the content, before any encoding
    std::optional<ContentAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<json> meta;  // an object, written as `_meta`
};

// A family of resources whose URIs a URI template (RFC 6570) describes, for resources/templates/list.
struct ResourceTemplateDefinition {
    std::string uri_template;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;  // only when every resource of the template has it
    std::optional<ContentAnnotations> annotations;
    std::vector<Icon> icons;
    std::optional<json> meta;
};

// The bytes of a resource that is not text; written as Base64 on the wire.
using Bytes = std::vector<std::byte>;

using ResourceContents = std::variant<TextResourceContents, BlobResourceContents>;

// The outcome of resources/read.
struct ReadResult {
    std::vector<ResourceContents> contents;
    std::optional<json> meta;  // an object, written as `_meta`
    // Where the revision has caching hints, the ones for this resource; empty: the server's default for reads.
    std::optional<CacheHints> cache;
};

// ---------------------------------------------------------------------------------------------------------------
// Prompts

// An argument a prompt takes. Arguments are strings on the wire.
struct PromptArgumentDefinition {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    bool required = false;
};

// A prompt the server offers, for prompts/list.
struct PromptDefinition {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::vector<PromptArgumentDefinition> arguments;
    std::vector<Icon> icons;
    std::optional<json> meta;  // an object, written as `_meta`
};

// One message of a prompt: who says it, and what: text, an image, audio, a resource link or an embedded resource.
struct PromptMessage {
    Role role = Role::user;
    ContentBlock content;
};

// The outcome of prompts/get.
struct PromptResult {
    std::optional<std::string> description;
    std::vector<PromptMessage> messages;
    std::optional<json> meta;  // an object, written as `_meta`
};

// ---------------------------------------------------------------------------------------------------------------
// Completion

// The most values a completion/complete result carries: every revision's schema states it ("must not exceed 100 items").
inline constexpr std::size_t max_completion_values = 100;

// The outcome of completion/complete: suggestions for the argument being typed. More values than
// max_completion_values are cut to that many when written, `hasMore` is then set, and `total` is the number of values
// given unless the handler stated a total.
struct CompletionResult {
    std::vector<std::string> values;
    std::optional<std::uint64_t> total;  // how many suggestions exist; may exceed values.size()
    std::optional<bool> has_more;        // whether there are suggestions beyond those given
    std::optional<json> meta;            // an object, written as `_meta`
};

}  // namespace neomcp
