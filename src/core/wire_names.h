// The names of the types and members that the wire writers (wire.cpp) and the wire readers (wire_read.cpp) share, and
// the spellings of the enumerations of model.h on the wire. Internal to the core: not part of the public interface.
//
// A name is written here once; a writer and its reader both take it from here, so that they cannot disagree.
#pragma once

#include <optional>
#include <string_view>

#include "neomcp/model.h"

namespace neomcp::detail::wire {

// ---- Types of the profiles' wire tables
inline constexpr std::string_view tool_type = "Tool";
inline constexpr std::string_view tool_annotations_type = "ToolAnnotations";
inline constexpr std::string_view implementation_type = "Implementation";
inline constexpr std::string_view icon_type = "Icon";
inline constexpr std::string_view annotations_type = "Annotations";
inline constexpr std::string_view resource_type = "Resource";
inline constexpr std::string_view resource_template_type = "ResourceTemplate";
inline constexpr std::string_view text_resource_contents_type = "TextResourceContents";
inline constexpr std::string_view blob_resource_contents_type = "BlobResourceContents";
inline constexpr std::string_view read_result_type = "ReadResourceResult";
inline constexpr std::string_view prompt_type = "Prompt";
inline constexpr std::string_view prompt_argument_type = "PromptArgument";
inline constexpr std::string_view prompt_message_type = "PromptMessage";
inline constexpr std::string_view prompt_result_type = "GetPromptResult";
inline constexpr std::string_view complete_result_type = "CompleteResult";
inline constexpr std::string_view tool_result_type = "CallToolResult";

// ---- Members
inline constexpr std::string_view name_member = "name";
inline constexpr std::string_view title_member = "title";
inline constexpr std::string_view description_member = "description";
inline constexpr std::string_view version_member = "version";
inline constexpr std::string_view website_url_member = "websiteUrl";
inline constexpr std::string_view icons_member = "icons";
inline constexpr std::string_view src_member = "src";
inline constexpr std::string_view mime_type_member = "mimeType";
inline constexpr std::string_view sizes_member = "sizes";
inline constexpr std::string_view theme_member = "theme";
inline constexpr std::string_view input_schema_member = "inputSchema";
inline constexpr std::string_view output_schema_member = "outputSchema";
inline constexpr std::string_view annotations_member = "annotations";
inline constexpr std::string_view meta_member = "_meta";
inline constexpr std::string_view read_only_hint_member = "readOnlyHint";
inline constexpr std::string_view destructive_hint_member = "destructiveHint";
inline constexpr std::string_view idempotent_hint_member = "idempotentHint";
inline constexpr std::string_view open_world_hint_member = "openWorldHint";
inline constexpr std::string_view audience_member = "audience";
inline constexpr std::string_view priority_member = "priority";
inline constexpr std::string_view last_modified_member = "lastModified";
inline constexpr std::string_view type_member = "type";
inline constexpr std::string_view text_member = "text";
inline constexpr std::string_view data_member = "data";
inline constexpr std::string_view uri_member = "uri";
inline constexpr std::string_view size_member = "size";
inline constexpr std::string_view resource_member = "resource";
inline constexpr std::string_view blob_member = "blob";
inline constexpr std::string_view uri_template_member = "uriTemplate";
inline constexpr std::string_view contents_member = "contents";
inline constexpr std::string_view arguments_member = "arguments";
inline constexpr std::string_view required_member = "required";
inline constexpr std::string_view role_member = "role";
inline constexpr std::string_view content_member = "content";
inline constexpr std::string_view messages_member = "messages";
inline constexpr std::string_view completion_member = "completion";
inline constexpr std::string_view values_member = "values";
inline constexpr std::string_view total_member = "total";
inline constexpr std::string_view has_more_member = "hasMore";
inline constexpr std::string_view structured_content_member = "structuredContent";
inline constexpr std::string_view is_error_member = "isError";

// The members the protocol engine adds to a result: the writers leave them out and the readers of the model's values
// do not read them. The cache hints have a reader of their own.
inline constexpr std::string_view result_type_member = "resultType";
inline constexpr std::string_view ttl_ms_member = "ttlMs";
inline constexpr std::string_view cache_scope_member = "cacheScope";

// ---- Spellings of enumerations
inline constexpr std::string_view user_role = "user";
inline constexpr std::string_view assistant_role = "assistant";
inline constexpr std::string_view light_theme = "light";
inline constexpr std::string_view dark_theme = "dark";

inline std::string_view role_name(Role role) noexcept {
    switch (role) {
        case Role::user: return user_role;
        case Role::assistant: return assistant_role;
    }
    return {};
}

inline std::optional<Role> parse_role(std::string_view text) noexcept {
    if (text == user_role) return Role::user;
    if (text == assistant_role) return Role::assistant;
    return std::nullopt;
}

inline std::string_view theme_name(IconTheme theme) noexcept {
    switch (theme) {
        case IconTheme::light: return light_theme;
        case IconTheme::dark: return dark_theme;
    }
    return {};
}

inline std::optional<IconTheme> parse_theme(std::string_view text) noexcept {
    if (text == light_theme) return IconTheme::light;
    if (text == dark_theme) return IconTheme::dark;
    return std::nullopt;
}

inline std::optional<CacheScope> parse_cache_scope(std::string_view text) noexcept {
    if (text == to_string(CacheScope::private_scope)) return CacheScope::private_scope;
    if (text == to_string(CacheScope::public_scope)) return CacheScope::public_scope;
    return std::nullopt;
}

}  // namespace neomcp::detail::wire
