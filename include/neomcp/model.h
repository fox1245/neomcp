// The values a server describes and returns, independent of any protocol revision. How a value is written for
// one revision is the job of wire.h, which keeps only the members that revision's types have.
#pragma once

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

}  // namespace neomcp
