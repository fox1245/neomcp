#include "neomcp/wire.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace neomcp {

bool keep_members(json& object, std::string_view type_name, const profile::Profile& profile) {
    const profile::Type* type = profile.type(type_name);
    auto* members = object.get_ptr<json::object_t*>();
    if (type == nullptr || members == nullptr) return false;
    json kept = json::object();
    auto* out = kept.get_ptr<json::object_t*>();
    for (auto& [name, value] : *members) {
        if (type->has_property(name)) (*out)[name] = std::move(value);
    }
    object = std::move(kept);
    return true;
}

bool has_content_kind(ContentKind kind, const profile::Profile& profile) {
    return profile.type(type_name(kind)) != nullptr;
}

namespace {

std::string_view to_string(IconTheme theme) {
    switch (theme) {
        case IconTheme::light: return "light";
        case IconTheme::dark: return "dark";
    }
    return {};
}

std::string_view to_string(Role role) {
    switch (role) {
        case Role::user: return "user";
        case Role::assistant: return "assistant";
    }
    return {};
}

void put(json& object, const char* name, const std::optional<std::string>& value) {
    if (value) object[name] = *value;
}

void put(json& object, const char* name, const std::optional<bool>& value) {
    if (value) object[name] = *value;
}

void put(json& object, const char* name, const std::optional<json>& value) {
    if (value) object[name] = *value;
}

json icons(const std::vector<Icon>& list, const profile::Profile& profile) {
    json out = json::array();
    for (const auto& icon : list) out.push_back(to_wire(icon, profile));
    return out;
}

// The members a content block has besides its own: extra members first, so that a model member of the same name
// replaces one of them, then annotations and _meta.
json block_base(ContentKind kind, const json& extra) {
    json object = json::object();
    object["type"] = std::string(wire_name(kind));
    if (const auto* members = extra.get_ptr<const json::object_t*>()) {
        for (const auto& [name, value] : *members) {
            if (name != "type") object[name] = value;
        }
    }
    return object;
}

json annotations_value(const ContentAnnotations& annotations, const profile::Profile& profile) {
    json value = json::object();
    if (!annotations.audience.empty()) {
        json audience = json::array();
        for (const Role role : annotations.audience) audience.push_back(std::string(to_string(role)));
        value["audience"] = std::move(audience);
    }
    if (annotations.priority) value["priority"] = *annotations.priority;
    put(value, "lastModified", annotations.last_modified);
    keep_members(value, "Annotations", profile);
    return value;
}

void block_common(json& object, const std::optional<ContentAnnotations>& annotations, const std::optional<json>& meta,
                  const profile::Profile& profile) {
    if (annotations) object["annotations"] = annotations_value(*annotations, profile);
    put(object, "_meta", meta);
}

json resource_contents(const TextResourceContents& contents, const profile::Profile& profile) {
    json value = json::object();
    value["uri"] = contents.uri;
    put(value, "mimeType", contents.mime_type);
    value["text"] = contents.text;
    put(value, "_meta", contents.meta);
    keep_members(value, "TextResourceContents", profile);
    return value;
}

json resource_contents(const BlobResourceContents& contents, const profile::Profile& profile) {
    json value = json::object();
    value["uri"] = contents.uri;
    put(value, "mimeType", contents.mime_type);
    value["blob"] = contents.blob;
    put(value, "_meta", contents.meta);
    keep_members(value, "BlobResourceContents", profile);
    return value;
}

json block_value(const TextContent& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    object["text"] = block.text;
    block_common(object, block.annotations, block.meta, profile);
    return object;
}

json block_value(const ImageContent& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    object["data"] = block.data;
    object["mimeType"] = block.mime_type;
    block_common(object, block.annotations, block.meta, profile);
    return object;
}

json block_value(const AudioContent& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    object["data"] = block.data;
    object["mimeType"] = block.mime_type;
    block_common(object, block.annotations, block.meta, profile);
    return object;
}

json block_value(const ResourceLink& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    object["uri"] = block.uri;
    object["name"] = block.name;
    put(object, "title", block.title);
    put(object, "description", block.description);
    put(object, "mimeType", block.mime_type);
    if (block.size) object["size"] = *block.size;
    if (!block.icons.empty()) object["icons"] = icons(block.icons, profile);
    block_common(object, block.annotations, block.meta, profile);
    return object;
}

json block_value(const EmbeddedResource& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    object["resource"] = std::visit([&](const auto& contents) { return resource_contents(contents, profile); },
                                    block.resource);
    block_common(object, block.annotations, block.meta, profile);
    return object;
}

}  // namespace

json to_wire(const Icon& icon, const profile::Profile& profile) {
    json object = json::object();
    object["src"] = icon.src;
    put(object, "mimeType", icon.mime_type);
    if (!icon.sizes.empty()) {
        json sizes = json::array();
        for (const auto& size : icon.sizes) sizes.push_back(size);
        object["sizes"] = std::move(sizes);
    }
    if (icon.theme) object["theme"] = std::string(to_string(*icon.theme));
    keep_members(object, "Icon", profile);
    return object;
}

json to_wire(const Implementation& implementation, const profile::Profile& profile) {
    json object = json::object();
    object["name"] = implementation.name;
    put(object, "title", implementation.title);
    object["version"] = implementation.version;
    put(object, "description", implementation.description);
    put(object, "websiteUrl", implementation.website_url);
    if (!implementation.icons.empty()) object["icons"] = icons(implementation.icons, profile);
    keep_members(object, "Implementation", profile);
    return object;
}

json to_wire(const ToolAnnotations& annotations, const profile::Profile& profile) {
    json object = json::object();
    put(object, "title", annotations.title);
    put(object, "readOnlyHint", annotations.read_only_hint);
    put(object, "destructiveHint", annotations.destructive_hint);
    put(object, "idempotentHint", annotations.idempotent_hint);
    put(object, "openWorldHint", annotations.open_world_hint);
    keep_members(object, "ToolAnnotations", profile);
    return object;
}

json to_wire(const ToolDefinition& tool, const profile::Profile& profile) {
    json object = json::object();
    object["name"] = tool.name;
    put(object, "title", tool.title);
    put(object, "description", tool.description);
    object["inputSchema"] = tool.input_schema;
    put(object, "outputSchema", tool.output_schema);
    if (tool.annotations) object["annotations"] = to_wire(*tool.annotations, profile);
    if (!tool.icons.empty()) object["icons"] = icons(tool.icons, profile);
    put(object, "_meta", tool.meta);
    keep_members(object, "Tool", profile);
    return object;
}

json to_wire(const ContentBlock& block, const profile::Profile& profile) {
    const ContentKind kind = kind_of(block);
    if (!has_content_kind(kind, profile)) {
        TextContent text;
        text.text = "[" + std::string(wire_name(kind)) +
                    " content omitted: the protocol revision in use does not define this kind]";
        json object = block_value(text, profile);
        keep_members(object, type_name(ContentKind::text), profile);
        return object;
    }
    json object = std::visit([&](const auto& value) { return block_value(value, profile); }, block);
    keep_members(object, type_name(kind), profile);
    return object;
}

json to_wire(const ResourceDefinition& resource, const profile::Profile& profile) {
    json object = json::object();
    object["uri"] = resource.uri;
    object["name"] = resource.name;
    put(object, "title", resource.title);
    put(object, "description", resource.description);
    put(object, "mimeType", resource.mime_type);
    if (resource.size) object["size"] = *resource.size;
    if (resource.annotations) object["annotations"] = annotations_value(*resource.annotations, profile);
    if (!resource.icons.empty()) object["icons"] = icons(resource.icons, profile);
    put(object, "_meta", resource.meta);
    keep_members(object, "Resource", profile);
    return object;
}

json to_wire(const ResourceTemplateDefinition& resource_template, const profile::Profile& profile) {
    json object = json::object();
    object["uriTemplate"] = resource_template.uri_template;
    object["name"] = resource_template.name;
    put(object, "title", resource_template.title);
    put(object, "description", resource_template.description);
    put(object, "mimeType", resource_template.mime_type);
    if (resource_template.annotations) {
        object["annotations"] = annotations_value(*resource_template.annotations, profile);
    }
    if (!resource_template.icons.empty()) object["icons"] = icons(resource_template.icons, profile);
    put(object, "_meta", resource_template.meta);
    keep_members(object, "ResourceTemplate", profile);
    return object;
}

json to_wire(const ResourceContents& contents, const profile::Profile& profile) {
    return std::visit([&](const auto& value) { return resource_contents(value, profile); }, contents);
}

json to_wire(const ReadResult& result, const profile::Profile& profile) {
    json object = json::object();
    json contents = json::array();
    for (const auto& item : result.contents) contents.push_back(to_wire(item, profile));
    object["contents"] = std::move(contents);
    put(object, "_meta", result.meta);
    keep_members(object, "ReadResourceResult", profile);
    return object;
}

json to_wire(const PromptDefinition& prompt, const profile::Profile& profile) {
    json object = json::object();
    object["name"] = prompt.name;
    put(object, "title", prompt.title);
    put(object, "description", prompt.description);
    if (!prompt.arguments.empty()) {
        json arguments = json::array();
        for (const auto& argument : prompt.arguments) {
            json entry = json::object();
            entry["name"] = argument.name;
            put(entry, "title", argument.title);
            put(entry, "description", argument.description);
            entry["required"] = argument.required;
            keep_members(entry, "PromptArgument", profile);
            arguments.push_back(std::move(entry));
        }
        object["arguments"] = std::move(arguments);
    }
    if (!prompt.icons.empty()) object["icons"] = icons(prompt.icons, profile);
    put(object, "_meta", prompt.meta);
    keep_members(object, "Prompt", profile);
    return object;
}

json to_wire(const PromptMessage& message, const profile::Profile& profile) {
    json object = json::object();
    object["role"] = std::string(to_string(message.role));
    object["content"] = to_wire(message.content, profile);
    keep_members(object, "PromptMessage", profile);
    return object;
}

json to_wire(const PromptResult& result, const profile::Profile& profile) {
    json object = json::object();
    put(object, "description", result.description);
    json messages = json::array();
    for (const auto& message : result.messages) messages.push_back(to_wire(message, profile));
    object["messages"] = std::move(messages);
    put(object, "_meta", result.meta);
    keep_members(object, "GetPromptResult", profile);
    return object;
}

json to_wire(const CompletionResult& result, const profile::Profile& profile) {
    const std::size_t kept = std::min(result.values.size(), max_completion_values);
    const bool truncated = kept < result.values.size();
    json values = json::array();
    for (std::size_t i = 0; i < kept; ++i) values.push_back(result.values[i]);
    json completion = json::object();
    completion["values"] = std::move(values);
    if (result.total) {
        completion["total"] = *result.total;
    } else if (truncated) {
        completion["total"] = static_cast<std::uint64_t>(result.values.size());
    }
    if (truncated) {
        completion["hasMore"] = true;
    } else if (result.has_more) {
        completion["hasMore"] = *result.has_more;
    }
    json object = json::object();
    object["completion"] = std::move(completion);
    put(object, "_meta", result.meta);
    keep_members(object, "CompleteResult", profile);
    return object;
}

json to_wire(const ToolResult& result, const profile::Profile& profile) {
    json object = json::object();
    json content = json::array();
    for (const auto& block : result.content) content.push_back(to_wire(block, profile));
    object["content"] = std::move(content);
    put(object, "structuredContent", result.structured_content);
    if (result.is_error) object["isError"] = true;
    put(object, "_meta", result.meta);
    keep_members(object, "CallToolResult", profile);
    return object;
}

}  // namespace neomcp
