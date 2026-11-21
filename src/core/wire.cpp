#include "neomcp/wire.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

#include "core/wire_names.h"

namespace neomcp {

namespace wire = detail::wire;

bool keep_members(json& object, const profile::Type& type) {
    auto* members = object.get_ptr<json::object_t*>();
    if (members == nullptr) return false;
    json kept = json::object();
    auto* out = kept.get_ptr<json::object_t*>();
    for (auto& [name, value] : *members) {
        if (type.has_property(name)) (*out)[name] = std::move(value);
    }
    object = std::move(kept);
    return true;
}

bool keep_members(json& object, std::string_view type_name, const profile::Profile& profile) {
    const profile::Type* type = profile.type(type_name);
    return type != nullptr && keep_members(object, *type);
}

bool has_content_kind(ContentKind kind, const profile::Profile& profile) {
    return profile.type(type_name(kind)) != nullptr;
}

namespace {

json& at(json& object, std::string_view name) { return object[std::string(name)]; }

void put(json& object, std::string_view name, const std::optional<std::string>& value) {
    if (value) at(object, name) = *value;
}

void put(json& object, std::string_view name, const std::optional<bool>& value) {
    if (value) at(object, name) = *value;
}

void put(json& object, std::string_view name, const std::optional<json>& value) {
    if (value) at(object, name) = *value;
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
    at(object, wire::type_member) = std::string(wire_name(kind));
    if (const auto* members = extra.get_ptr<const json::object_t*>()) {
        for (const auto& [name, value] : *members) {
            if (name != wire::type_member) object[name] = value;
        }
    }
    return object;
}

// The annotations of an object of the type `owner_type`, with the members the profile's type of the owner's
// `annotations` property has. A revision that gives the property no type leaves the object as the model has it.
json annotations_value(const ContentAnnotations& annotations, std::string_view owner_type,
                       const profile::Profile& profile) {
    json value = json::object();
    if (!annotations.audience.empty()) {
        json audience = json::array();
        for (const Role role : annotations.audience) audience.push_back(std::string(wire::role_name(role)));
        at(value, wire::audience_member) = std::move(audience);
    }
    if (annotations.priority) at(value, wire::priority_member) = *annotations.priority;
    put(value, wire::last_modified_member, annotations.last_modified);
    if (const profile::Type* owner = profile.type(owner_type)) {
        if (const profile::Type* type = profile.property_type(*owner, wire::annotations_member)) {
            keep_members(value, *type);
        }
    }
    return value;
}

void block_common(json& object, ContentKind kind, const std::optional<ContentAnnotations>& annotations,
                  const std::optional<json>& meta, const profile::Profile& profile) {
    if (annotations) at(object, wire::annotations_member) = annotations_value(*annotations, type_name(kind), profile);
    put(object, wire::meta_member, meta);
}

json resource_contents(const TextResourceContents& contents, const profile::Profile& profile) {
    json value = json::object();
    at(value, wire::uri_member) = contents.uri;
    put(value, wire::mime_type_member, contents.mime_type);
    at(value, wire::text_member) = contents.text;
    put(value, wire::meta_member, contents.meta);
    keep_members(value, wire::text_resource_contents_type, profile);
    return value;
}

json resource_contents(const BlobResourceContents& contents, const profile::Profile& profile) {
    json value = json::object();
    at(value, wire::uri_member) = contents.uri;
    put(value, wire::mime_type_member, contents.mime_type);
    at(value, wire::blob_member) = contents.blob;
    put(value, wire::meta_member, contents.meta);
    keep_members(value, wire::blob_resource_contents_type, profile);
    return value;
}

json block_value(const TextContent& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    at(object, wire::text_member) = block.text;
    block_common(object, block.kind, block.annotations, block.meta, profile);
    return object;
}

json block_value(const ImageContent& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    at(object, wire::data_member) = block.data;
    at(object, wire::mime_type_member) = block.mime_type;
    block_common(object, block.kind, block.annotations, block.meta, profile);
    return object;
}

json block_value(const AudioContent& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    at(object, wire::data_member) = block.data;
    at(object, wire::mime_type_member) = block.mime_type;
    block_common(object, block.kind, block.annotations, block.meta, profile);
    return object;
}

json block_value(const ResourceLink& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    at(object, wire::uri_member) = block.uri;
    at(object, wire::name_member) = block.name;
    put(object, wire::title_member, block.title);
    put(object, wire::description_member, block.description);
    put(object, wire::mime_type_member, block.mime_type);
    if (block.size) at(object, wire::size_member) = *block.size;
    if (!block.icons.empty()) at(object, wire::icons_member) = icons(block.icons, profile);
    block_common(object, block.kind, block.annotations, block.meta, profile);
    return object;
}

json block_value(const EmbeddedResource& block, const profile::Profile& profile) {
    json object = block_base(block.kind, block.extra);
    at(object, wire::resource_member) = std::visit([&](const auto& contents) { return resource_contents(contents, profile); },
                                    block.resource);
    block_common(object, block.kind, block.annotations, block.meta, profile);
    return object;
}

}  // namespace

json to_wire(const Icon& icon, const profile::Profile& profile) {
    json object = json::object();
    at(object, wire::src_member) = icon.src;
    put(object, wire::mime_type_member, icon.mime_type);
    if (!icon.sizes.empty()) {
        json sizes = json::array();
        for (const auto& size : icon.sizes) sizes.push_back(size);
        at(object, wire::sizes_member) = std::move(sizes);
    }
    if (icon.theme) at(object, wire::theme_member) = std::string(wire::theme_name(*icon.theme));
    keep_members(object, wire::icon_type, profile);
    return object;
}

json to_wire(const Implementation& implementation, const profile::Profile& profile) {
    json object = json::object();
    at(object, wire::name_member) = implementation.name;
    put(object, wire::title_member, implementation.title);
    at(object, wire::version_member) = implementation.version;
    put(object, wire::description_member, implementation.description);
    put(object, wire::website_url_member, implementation.website_url);
    if (!implementation.icons.empty()) at(object, wire::icons_member) = icons(implementation.icons, profile);
    keep_members(object, wire::implementation_type, profile);
    return object;
}

json to_wire(const ToolAnnotations& annotations, const profile::Profile& profile) {
    json object = json::object();
    put(object, wire::title_member, annotations.title);
    put(object, wire::read_only_hint_member, annotations.read_only_hint);
    put(object, wire::destructive_hint_member, annotations.destructive_hint);
    put(object, wire::idempotent_hint_member, annotations.idempotent_hint);
    put(object, wire::open_world_hint_member, annotations.open_world_hint);
    keep_members(object, wire::tool_annotations_type, profile);
    return object;
}

json to_wire(const ToolDefinition& tool, const profile::Profile& profile) {
    json object = json::object();
    at(object, wire::name_member) = tool.name;
    put(object, wire::title_member, tool.title);
    put(object, wire::description_member, tool.description);
    at(object, wire::input_schema_member) = tool.input_schema;
    put(object, wire::output_schema_member, tool.output_schema);
    if (tool.annotations) at(object, wire::annotations_member) = to_wire(*tool.annotations, profile);
    if (!tool.icons.empty()) at(object, wire::icons_member) = icons(tool.icons, profile);
    put(object, wire::meta_member, tool.meta);
    keep_members(object, wire::tool_type, profile);
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
    at(object, wire::uri_member) = resource.uri;
    at(object, wire::name_member) = resource.name;
    put(object, wire::title_member, resource.title);
    put(object, wire::description_member, resource.description);
    put(object, wire::mime_type_member, resource.mime_type);
    if (resource.size) at(object, wire::size_member) = *resource.size;
    if (resource.annotations) {
        at(object, wire::annotations_member) = annotations_value(*resource.annotations, wire::resource_type, profile);
    }
    if (!resource.icons.empty()) at(object, wire::icons_member) = icons(resource.icons, profile);
    put(object, wire::meta_member, resource.meta);
    keep_members(object, wire::resource_type, profile);
    return object;
}

json to_wire(const ResourceTemplateDefinition& resource_template, const profile::Profile& profile) {
    json object = json::object();
    at(object, wire::uri_template_member) = resource_template.uri_template;
    at(object, wire::name_member) = resource_template.name;
    put(object, wire::title_member, resource_template.title);
    put(object, wire::description_member, resource_template.description);
    put(object, wire::mime_type_member, resource_template.mime_type);
    if (resource_template.annotations) {
        at(object, wire::annotations_member) =
            annotations_value(*resource_template.annotations, wire::resource_template_type, profile);
    }
    if (!resource_template.icons.empty()) at(object, wire::icons_member) = icons(resource_template.icons, profile);
    put(object, wire::meta_member, resource_template.meta);
    keep_members(object, wire::resource_template_type, profile);
    return object;
}

json to_wire(const ResourceContents& contents, const profile::Profile& profile) {
    return std::visit([&](const auto& value) { return resource_contents(value, profile); }, contents);
}

json to_wire(const ReadResult& result, const profile::Profile& profile) {
    json object = json::object();
    json contents = json::array();
    for (const auto& item : result.contents) contents.push_back(to_wire(item, profile));
    at(object, wire::contents_member) = std::move(contents);
    put(object, wire::meta_member, result.meta);
    keep_members(object, wire::read_result_type, profile);
    return object;
}

json to_wire(const PromptDefinition& prompt, const profile::Profile& profile) {
    json object = json::object();
    at(object, wire::name_member) = prompt.name;
    put(object, wire::title_member, prompt.title);
    put(object, wire::description_member, prompt.description);
    if (!prompt.arguments.empty()) {
        json arguments = json::array();
        for (const auto& argument : prompt.arguments) {
            json entry = json::object();
            at(entry, wire::name_member) = argument.name;
            put(entry, wire::title_member, argument.title);
            put(entry, wire::description_member, argument.description);
            at(entry, wire::required_member) = argument.required;
            keep_members(entry, wire::prompt_argument_type, profile);
            arguments.push_back(std::move(entry));
        }
        at(object, wire::arguments_member) = std::move(arguments);
    }
    if (!prompt.icons.empty()) at(object, wire::icons_member) = icons(prompt.icons, profile);
    put(object, wire::meta_member, prompt.meta);
    keep_members(object, wire::prompt_type, profile);
    return object;
}

json to_wire(const PromptMessage& message, const profile::Profile& profile) {
    json object = json::object();
    at(object, wire::role_member) = std::string(wire::role_name(message.role));
    at(object, wire::content_member) = to_wire(message.content, profile);
    keep_members(object, wire::prompt_message_type, profile);
    return object;
}

json to_wire(const PromptResult& result, const profile::Profile& profile) {
    json object = json::object();
    put(object, wire::description_member, result.description);
    json messages = json::array();
    for (const auto& message : result.messages) messages.push_back(to_wire(message, profile));
    at(object, wire::messages_member) = std::move(messages);
    put(object, wire::meta_member, result.meta);
    keep_members(object, wire::prompt_result_type, profile);
    return object;
}

json to_wire(const CompletionResult& result, const profile::Profile& profile) {
    const std::size_t kept = std::min(result.values.size(), max_completion_values);
    const bool truncated = kept < result.values.size();
    json values = json::array();
    for (std::size_t i = 0; i < kept; ++i) values.push_back(result.values[i]);
    json completion = json::object();
    at(completion, wire::values_member) = std::move(values);
    if (result.total) {
        at(completion, wire::total_member) = *result.total;
    } else if (truncated) {
        at(completion, wire::total_member) = static_cast<std::uint64_t>(result.values.size());
    }
    if (truncated) {
        at(completion, wire::has_more_member) = true;
    } else if (result.has_more) {
        at(completion, wire::has_more_member) = *result.has_more;
    }
    json object = json::object();
    at(object, wire::completion_member) = std::move(completion);
    put(object, wire::meta_member, result.meta);
    keep_members(object, wire::complete_result_type, profile);
    return object;
}

json to_wire(const ToolResult& result, const profile::Profile& profile) {
    json object = json::object();
    json content = json::array();
    for (const auto& block : result.content) content.push_back(to_wire(block, profile));
    at(object, wire::content_member) = std::move(content);
    put(object, wire::structured_content_member, result.structured_content);
    if (result.is_error) at(object, wire::is_error_member) = true;
    put(object, wire::meta_member, result.meta);
    keep_members(object, wire::tool_result_type, profile);
    return object;
}

}  // namespace neomcp
