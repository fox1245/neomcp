#include "neomcp/wire_read.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/wire_names.h"
#include "neomcp/wire.h"

namespace neomcp {
namespace {

namespace names = detail::wire;

// A step of the path from the value that was given to the place being read: a member, or an element of an array. The
// steps live in the frames of the readers that are running, so the path costs nothing until a description is made.
struct Step {
    const Step* parent = nullptr;
    std::string_view member;
    std::size_t index = 0;
    bool indexed = false;
};

Step member_step(const Step* parent, std::string_view member) { return Step{parent, member, 0, false}; }
Step index_step(const Step* parent, std::size_t index) { return Step{parent, {}, index, true}; }

constexpr std::array<ContentKind, 5> content_kinds{ContentKind::text, ContentKind::image, ContentKind::audio,
                                                    ContentKind::resource_link, ContentKind::embedded_resource};

// The state of one read: the profile, where the value sits in the larger one, and the first problem found.
class Reader {
public:
    Reader(const profile::Profile& profile, std::string_view root) : profile_(profile), root_(root) {}

    const profile::Profile& current() const noexcept { return profile_; }
    const profile::Type* type(std::string_view name) const { return profile_.type(name); }
    bool failed() const noexcept { return !error_.empty(); }
    std::string take_error() { return std::move(error_); }

    // Records the problem (the first one only) and returns false, so that a reader can `return reader.fail(...)`.
    bool fail(const Step* at, std::string_view message) {
        if (error_.empty()) {
            error_ = render(at);
            error_ += ": ";
            error_ += message;
        }
        return false;
    }

private:
    std::string render(const Step* at) const {
        std::vector<const Step*> steps;
        for (const Step* step = at; step != nullptr; step = step->parent) steps.push_back(step);
        std::string out(root_);
        for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
            if ((*step)->indexed) {
                out += "[" + std::to_string((*step)->index) + "]";
            } else {
                out += ".";
                out += (*step)->member;
            }
        }
        return out;
    }

    const profile::Profile& profile_;
    std::string_view root_;
    std::string error_;
};

bool unsigned_number(const json& wire, std::uint64_t& out) {
    if (const auto* unsigned_value = wire.get_ptr<const json::number_unsigned_t*>()) {
        out = static_cast<std::uint64_t>(*unsigned_value);
        return true;
    }
    if (const auto* signed_value = wire.get_ptr<const json::number_integer_t*>()) {
        if (*signed_value < 0) return false;
        out = static_cast<std::uint64_t>(*signed_value);
        return true;
    }
    return false;
}

bool any_number(const json& wire, double& out) {
    if (const auto* real = wire.get_ptr<const json::number_float_t*>()) {
        out = static_cast<double>(*real);
        return true;
    }
    if (const auto* unsigned_value = wire.get_ptr<const json::number_unsigned_t*>()) {
        out = static_cast<double>(*unsigned_value);
        return true;
    }
    if (const auto* signed_value = wire.get_ptr<const json::number_integer_t*>()) {
        out = static_cast<double>(*signed_value);
        return true;
    }
    return false;
}

// The members of one JSON object, read against the profile's type of it. `type` is null when the profile has no such
// type (a revision that spells the object inline): then no member is held back and the profile requires none, so the
// model's own needs decide. Every member asked for is remembered, so that what was not asked for can be told.
class Fields {
public:
    Fields(Reader& reader, const json& wire, const Step* place, std::string_view type_label)
        : reader_(reader), place_(place), type_(reader.type(type_label)) {
        members_ = wire.get_ptr<const json::object_t*>();
        if (members_ == nullptr) reader_.fail(place_, "must be an object");
    }

    bool valid() const noexcept { return members_ != nullptr; }
    const profile::Type* type() const noexcept { return type_; }
    const Step* position() const noexcept { return place_; }

    // Whether the profile's type has the member. A revision without the type holds nothing back.
    bool has(std::string_view name) const { return type_ == nullptr || type_->has_property(name); }

    // The member's value; null when it is absent or the profile's type does not have it. `model_requires`: the model
    // cannot do without the member. A member that the profile's type requires is required whatever the model says; a
    // revision without the type has the model decide. Returns false after a failure.
    bool find(std::string_view name, bool model_requires, const json*& found) {
        found = nullptr;
        if (!valid()) return false;
        asked_.push_back(name);
        if (!has(name)) return true;
        const auto at_name = members_->find(std::string(name));
        if (at_name != members_->end()) {
            found = &at_name->second;
            return true;
        }
        const bool needed = type_ != nullptr ? type_->is_required(name) : model_requires;
        if (needed) return fail_member(name, "is required");
        return true;
    }

    bool fail_member(std::string_view name, std::string_view message) {
        const Step step = member_step(place_, name);
        return reader_.fail(&step, message);
    }

    bool text(std::string_view name, bool model_requires, std::string& out) {
        const json* found = nullptr;
        if (!find(name, model_requires, found)) return false;
        if (found == nullptr) return true;
        const auto* value = found->get_ptr<const std::string*>();
        if (value == nullptr) return fail_member(name, "must be a string");
        out = *value;
        return true;
    }

    bool optional_text(std::string_view name, std::optional<std::string>& out) {
        const json* found = nullptr;
        if (!find(name, false, found)) return false;
        if (found == nullptr) return true;
        const auto* value = found->get_ptr<const std::string*>();
        if (value == nullptr) return fail_member(name, "must be a string");
        out = *value;
        return true;
    }

    bool optional_flag(std::string_view name, std::optional<bool>& out) {
        const json* found = nullptr;
        if (!find(name, false, found)) return false;
        if (found == nullptr) return true;
        const auto* value = found->get_ptr<const json::boolean_t*>();
        if (value == nullptr) return fail_member(name, "must be a boolean");
        out = static_cast<bool>(*value);
        return true;
    }

    bool flag(std::string_view name, bool& out) {
        std::optional<bool> read;
        if (!optional_flag(name, read)) return false;
        if (read) out = *read;
        return true;
    }

    bool optional_count(std::string_view name, std::optional<std::uint64_t>& out) {
        const json* found = nullptr;
        if (!find(name, false, found)) return false;
        if (found == nullptr) return true;
        std::uint64_t count = 0;
        if (!unsigned_number(*found, count)) return fail_member(name, "must be a non-negative integer");
        out = count;
        return true;
    }

    bool optional_real(std::string_view name, std::optional<double>& out) {
        const json* found = nullptr;
        if (!find(name, false, found)) return false;
        if (found == nullptr) return true;
        double number = 0;
        if (!any_number(*found, number)) return fail_member(name, "must be a number");
        out = number;
        return true;
    }

    // A member that is a JSON object, kept as it is.
    bool object_value(std::string_view name, bool model_requires, json& out) {
        const json* found = nullptr;
        if (!find(name, model_requires, found)) return false;
        if (found == nullptr) return true;
        if (!found->is_object()) return fail_member(name, "must be an object");
        out = *found;
        return true;
    }

    bool optional_object_value(std::string_view name, std::optional<json>& out) {
        const json* found = nullptr;
        if (!find(name, false, found)) return false;
        if (found == nullptr) return true;
        if (!found->is_object()) return fail_member(name, "must be an object");
        out = *found;
        return true;
    }

    bool meta(std::optional<json>& out) { return optional_object_value(names::meta_member, out); }

    // A member that is an array of strings.
    bool text_list(std::string_view name, bool model_requires, std::vector<std::string>& out) {
        const json* found = nullptr;
        if (!find(name, model_requires, found)) return false;
        if (found == nullptr) return true;
        const auto* items = found->get_ptr<const json::array_t*>();
        if (items == nullptr) return fail_member(name, "must be an array");
        const Step list = member_step(place_, name);
        for (std::size_t i = 0; i < items->size(); ++i) {
            const auto* item = (*items)[i].get_ptr<const std::string*>();
            if (item == nullptr) {
                const Step element = index_step(&list, i);
                return reader_.fail(&element, "must be a string");
            }
            out.push_back(*item);
        }
        return true;
    }

    // A member that is an array; the caller reads its elements.
    bool array(std::string_view name, bool model_requires, const json::array_t*& out) {
        out = nullptr;
        const json* found = nullptr;
        if (!find(name, model_requires, found)) return false;
        if (found == nullptr) return true;
        out = found->get_ptr<const json::array_t*>();
        if (out == nullptr) return fail_member(name, "must be an array");
        return true;
    }

    // The members of the value that the profile's type has and that were not asked for, other than `skipped`.
    json unasked(std::string_view skipped) const {
        json rest = json::object();
        if (members_ == nullptr || type_ == nullptr) return rest;
        for (const auto& [name, value] : *members_) {
            if (name == skipped || !type_->has_property(name)) continue;
            bool was_asked = false;
            for (const std::string_view asked : asked_) was_asked = was_asked || asked == name;
            if (!was_asked) rest[name] = value;
        }
        return rest;
    }

    void mark_asked(std::string_view name) { asked_.push_back(name); }

private:
    Reader& reader_;
    const Step* place_;
    const profile::Type* type_;
    const json::object_t* members_ = nullptr;
    std::vector<std::string_view> asked_;
};

// ---- The readers of the pieces

bool read_icon(Reader& reader, const json& wire, const Step* at, Icon& out) {
    Fields fields(reader, wire, at, names::icon_type);
    if (!fields.valid()) return false;
    if (!fields.text(names::src_member, true, out.src)) return false;
    if (!fields.optional_text(names::mime_type_member, out.mime_type)) return false;
    if (!fields.text_list(names::sizes_member, false, out.sizes)) return false;
    std::optional<std::string> theme;
    if (!fields.optional_text(names::theme_member, theme)) return false;
    if (theme) {
        out.theme = names::parse_theme(*theme);
        if (!out.theme) return fields.fail_member(names::theme_member, "must be light or dark");
    }
    return true;
}

bool read_icons(Reader& reader, Fields& fields, std::vector<Icon>& out) {
    const json::array_t* items = nullptr;
    if (!fields.array(names::icons_member, false, items)) return false;
    if (items == nullptr) return true;
    const Step list = member_step(fields.position(), names::icons_member);
    for (std::size_t i = 0; i < items->size(); ++i) {
        const Step element = index_step(&list, i);
        Icon icon;
        if (!read_icon(reader, (*items)[i], &element, icon)) return false;
        out.push_back(std::move(icon));
    }
    return true;
}

bool read_content_annotations(Reader& reader, const json& wire, const Step* at, ContentAnnotations& out) {
    Fields fields(reader, wire, at, names::annotations_type);
    if (!fields.valid()) return false;
    const json::array_t* audience = nullptr;
    if (!fields.array(names::audience_member, false, audience)) return false;
    if (audience != nullptr) {
        const Step list = member_step(at, names::audience_member);
        for (std::size_t i = 0; i < audience->size(); ++i) {
            const auto* name = (*audience)[i].get_ptr<const std::string*>();
            const std::optional<Role> role = name == nullptr ? std::nullopt : names::parse_role(*name);
            if (!role) {
                const Step element = index_step(&list, i);
                return reader.fail(&element, "must be user or assistant");
            }
            out.audience.push_back(*role);
        }
    }
    if (!fields.optional_real(names::priority_member, out.priority)) return false;
    return fields.optional_text(names::last_modified_member, out.last_modified);
}

// The member `annotations` of an object, when the profile's type has it and the object gives it.
bool read_annotations_member(Reader& reader, Fields& fields, std::optional<ContentAnnotations>& out) {
    const json* found = nullptr;
    if (!fields.find(names::annotations_member, false, found)) return false;
    if (found == nullptr) return true;
    const Step step = member_step(fields.position(), names::annotations_member);
    ContentAnnotations annotations;
    if (!read_content_annotations(reader, *found, &step, annotations)) return false;
    out = std::move(annotations);
    return true;
}

bool read_resource_contents(Reader& reader, const json& wire, const Step* at, ResourceContents& out) {
    const auto* members = wire.get_ptr<const json::object_t*>();
    if (members == nullptr) return reader.fail(at, "must be an object");
    const bool has_text = members->find(std::string(names::text_member)) != members->end();
    const bool has_blob = members->find(std::string(names::blob_member)) != members->end();
    if (has_text == has_blob) return reader.fail(at, "must have exactly one of text and blob");
    if (has_text) {
        Fields fields(reader, wire, at, names::text_resource_contents_type);
        TextResourceContents contents;
        if (!fields.text(names::uri_member, true, contents.uri) ||
            !fields.optional_text(names::mime_type_member, contents.mime_type) ||
            !fields.text(names::text_member, true, contents.text) || !fields.meta(contents.meta)) {
            return false;
        }
        out = std::move(contents);
    } else {
        Fields fields(reader, wire, at, names::blob_resource_contents_type);
        BlobResourceContents contents;
        if (!fields.text(names::uri_member, true, contents.uri) ||
            !fields.optional_text(names::mime_type_member, contents.mime_type) ||
            !fields.text(names::blob_member, true, contents.blob) || !fields.meta(contents.meta)) {
            return false;
        }
        out = std::move(contents);
    }
    return true;
}

// The members every content block has besides its own: annotations, _meta and the extra members.
bool read_block_common(Reader& reader, Fields& fields, std::optional<ContentAnnotations>& annotations,
                       std::optional<json>& meta, json& extra) {
    if (!read_annotations_member(reader, fields, annotations)) return false;
    if (!fields.meta(meta)) return false;
    extra = fields.unasked(names::type_member);
    return true;
}

bool read_block(Reader& reader, const json& wire, const Step* at, ContentBlock& out) {
    const auto* members = wire.get_ptr<const json::object_t*>();
    if (members == nullptr) return reader.fail(at, "must be an object");
    const Step type_at = member_step(at, names::type_member);
    const auto found = members->find(std::string(names::type_member));
    if (found == members->end()) return reader.fail(&type_at, "is required");
    const auto* kind_name = found->second.get_ptr<const std::string*>();
    if (kind_name == nullptr) return reader.fail(&type_at, "must be a string");
    std::optional<ContentKind> kind;
    for (const ContentKind candidate : content_kinds) {
        if (wire_name(candidate) == *kind_name) kind = candidate;
    }
    if (!kind) return reader.fail(&type_at, "names a content block kind that is not known");
    if (!has_content_kind(*kind, reader.current())) {
        return reader.fail(&type_at, "names a content block kind that the revision does not define");
    }
    Fields fields(reader, wire, at, type_name(*kind));
    if (!fields.valid()) return false;
    fields.mark_asked(names::type_member);
    switch (*kind) {
        case ContentKind::text: {
            TextContent block;
            if (!fields.text(names::text_member, true, block.text) ||
                !read_block_common(reader, fields, block.annotations, block.meta, block.extra)) {
                return false;
            }
            out = std::move(block);
            return true;
        }
        case ContentKind::image: {
            ImageContent block;
            if (!fields.text(names::data_member, true, block.data) ||
                !fields.text(names::mime_type_member, true, block.mime_type) ||
                !read_block_common(reader, fields, block.annotations, block.meta, block.extra)) {
                return false;
            }
            out = std::move(block);
            return true;
        }
        case ContentKind::audio: {
            AudioContent block;
            if (!fields.text(names::data_member, true, block.data) ||
                !fields.text(names::mime_type_member, true, block.mime_type) ||
                !read_block_common(reader, fields, block.annotations, block.meta, block.extra)) {
                return false;
            }
            out = std::move(block);
            return true;
        }
        case ContentKind::resource_link: {
            ResourceLink block;
            if (!fields.text(names::uri_member, true, block.uri) || !fields.text(names::name_member, true, block.name) ||
                !fields.optional_text(names::title_member, block.title) ||
                !fields.optional_text(names::description_member, block.description) ||
                !fields.optional_text(names::mime_type_member, block.mime_type) ||
                !fields.optional_count(names::size_member, block.size) || !read_icons(reader, fields, block.icons) ||
                !read_block_common(reader, fields, block.annotations, block.meta, block.extra)) {
                return false;
            }
            out = std::move(block);
            return true;
        }
        case ContentKind::embedded_resource: {
            EmbeddedResource block;
            const json* resource = nullptr;
            if (!fields.find(names::resource_member, true, resource)) return false;
            if (resource != nullptr) {
                const Step resource_at = member_step(at, names::resource_member);
                if (!read_resource_contents(reader, *resource, &resource_at, block.resource)) return false;
            }
            if (!read_block_common(reader, fields, block.annotations, block.meta, block.extra)) return false;
            out = std::move(block);
            return true;
        }
    }
    return reader.fail(&type_at, "names a content block kind that is not known");
}

bool read_tool_annotations(Reader& reader, const json& wire, const Step* at, ToolAnnotations& out) {
    Fields fields(reader, wire, at, names::tool_annotations_type);
    if (!fields.valid()) return false;
    return fields.optional_text(names::title_member, out.title) &&
           fields.optional_flag(names::read_only_hint_member, out.read_only_hint) &&
           fields.optional_flag(names::destructive_hint_member, out.destructive_hint) &&
           fields.optional_flag(names::idempotent_hint_member, out.idempotent_hint) &&
           fields.optional_flag(names::open_world_hint_member, out.open_world_hint);
}

bool read_tool(Reader& reader, const json& wire, const Step* at, ToolDefinition& out) {
    Fields fields(reader, wire, at, names::tool_type);
    if (!fields.valid()) return false;
    if (!fields.text(names::name_member, true, out.name) || !fields.optional_text(names::title_member, out.title) ||
        !fields.optional_text(names::description_member, out.description) ||
        !fields.object_value(names::input_schema_member, true, out.input_schema) ||
        !fields.optional_object_value(names::output_schema_member, out.output_schema)) {
        return false;
    }
    const json* annotations = nullptr;
    if (!fields.find(names::annotations_member, false, annotations)) return false;
    if (annotations != nullptr) {
        const Step step = member_step(at, names::annotations_member);
        ToolAnnotations read;
        if (!read_tool_annotations(reader, *annotations, &step, read)) return false;
        out.annotations = std::move(read);
    }
    return read_icons(reader, fields, out.icons) && fields.meta(out.meta);
}

bool read_tool_result(Reader& reader, const json& wire, const Step* at, ToolResult& out) {
    Fields fields(reader, wire, at, names::tool_result_type);
    if (!fields.valid()) return false;
    const json::array_t* content = nullptr;
    if (!fields.array(names::content_member, true, content)) return false;
    if (content != nullptr) {
        const Step list = member_step(at, names::content_member);
        for (std::size_t i = 0; i < content->size(); ++i) {
            const Step element = index_step(&list, i);
            ContentBlock block;
            if (!read_block(reader, (*content)[i], &element, block)) return false;
            out.content.push_back(std::move(block));
        }
    }
    return fields.optional_object_value(names::structured_content_member, out.structured_content) &&
           fields.flag(names::is_error_member, out.is_error) && fields.meta(out.meta);
}

bool read_cache(Reader& reader, const json& wire, std::string_view type_label, const Step* at,
                std::optional<CacheHints>& out) {
    const profile::Type* result_type = reader.type(type_label);
    const auto* members = wire.get_ptr<const json::object_t*>();
    if (members == nullptr) return reader.fail(at, "must be an object");
    if (result_type == nullptr) return true;
    const bool has_ttl = result_type->has_property(names::ttl_ms_member);
    const bool has_scope = result_type->has_property(names::cache_scope_member);
    if (!has_ttl && !has_scope) return true;
    CacheHints hints;
    bool given = false;
    if (has_ttl) {
        const Step step = member_step(at, names::ttl_ms_member);
        const auto found = members->find(std::string(names::ttl_ms_member));
        if (found == members->end()) {
            if (result_type->is_required(names::ttl_ms_member)) return reader.fail(&step, "is required");
        } else {
            std::uint64_t ttl = 0;
            if (const auto* signed_value = found->second.get_ptr<const json::number_integer_t*>()) {
                if (*signed_value >= 0) ttl = static_cast<std::uint64_t>(*signed_value);  // below 0: treated as 0
            } else if (!unsigned_number(found->second, ttl)) {
                return reader.fail(&step, "must be an integer");
            }
            hints.ttl_ms = ttl;
            given = true;
        }
    }
    if (has_scope) {
        const Step step = member_step(at, names::cache_scope_member);
        const auto found = members->find(std::string(names::cache_scope_member));
        if (found == members->end()) {
            if (result_type->is_required(names::cache_scope_member)) return reader.fail(&step, "is required");
        } else {
            const auto* text = found->second.get_ptr<const std::string*>();
            if (text == nullptr) return reader.fail(&step, "must be a string");
            const std::optional<CacheScope> scope = names::parse_cache_scope(*text);
            if (!scope) return reader.fail(&step, "must be public or private");
            hints.scope = *scope;
            given = true;
        }
    }
    if (given) out = hints;
    return true;
}

bool read_resource(Reader& reader, const json& wire, const Step* at, ResourceDefinition& out) {
    Fields fields(reader, wire, at, names::resource_type);
    if (!fields.valid()) return false;
    return fields.text(names::uri_member, true, out.uri) && fields.text(names::name_member, true, out.name) &&
           fields.optional_text(names::title_member, out.title) &&
           fields.optional_text(names::description_member, out.description) &&
           fields.optional_text(names::mime_type_member, out.mime_type) &&
           fields.optional_count(names::size_member, out.size) && read_annotations_member(reader, fields, out.annotations) &&
           read_icons(reader, fields, out.icons) && fields.meta(out.meta);
}

bool read_resource_template(Reader& reader, const json& wire, const Step* at, ResourceTemplateDefinition& out) {
    Fields fields(reader, wire, at, names::resource_template_type);
    if (!fields.valid()) return false;
    return fields.text(names::uri_template_member, true, out.uri_template) &&
           fields.text(names::name_member, true, out.name) && fields.optional_text(names::title_member, out.title) &&
           fields.optional_text(names::description_member, out.description) &&
           fields.optional_text(names::mime_type_member, out.mime_type) &&
           read_annotations_member(reader, fields, out.annotations) && read_icons(reader, fields, out.icons) &&
           fields.meta(out.meta);
}

bool read_read_result(Reader& reader, const json& wire, const Step* at, ReadResult& out) {
    Fields fields(reader, wire, at, names::read_result_type);
    if (!fields.valid()) return false;
    const json::array_t* contents = nullptr;
    if (!fields.array(names::contents_member, true, contents)) return false;
    if (contents != nullptr) {
        const Step list = member_step(at, names::contents_member);
        for (std::size_t i = 0; i < contents->size(); ++i) {
            const Step element = index_step(&list, i);
            ResourceContents item;
            if (!read_resource_contents(reader, (*contents)[i], &element, item)) return false;
            out.contents.push_back(std::move(item));
        }
    }
    if (!fields.meta(out.meta)) return false;
    // The hints are read when the result gives any of them; a result without them is the engine's to judge.
    const auto* members = wire.get_ptr<const json::object_t*>();
    const bool gives_hints = members->find(std::string(names::ttl_ms_member)) != members->end() ||
                             members->find(std::string(names::cache_scope_member)) != members->end();
    return !gives_hints || read_cache(reader, wire, names::read_result_type, at, out.cache);
}

bool read_prompt(Reader& reader, const json& wire, const Step* at, PromptDefinition& out) {
    Fields fields(reader, wire, at, names::prompt_type);
    if (!fields.valid()) return false;
    if (!fields.text(names::name_member, true, out.name) || !fields.optional_text(names::title_member, out.title) ||
        !fields.optional_text(names::description_member, out.description)) {
        return false;
    }
    const json::array_t* arguments = nullptr;
    if (!fields.array(names::arguments_member, false, arguments)) return false;
    if (arguments != nullptr) {
        const Step list = member_step(at, names::arguments_member);
        for (std::size_t i = 0; i < arguments->size(); ++i) {
            const Step element = index_step(&list, i);
            Fields argument_fields(reader, (*arguments)[i], &element, names::prompt_argument_type);
            PromptArgumentDefinition argument;
            if (!argument_fields.valid() || !argument_fields.text(names::name_member, true, argument.name) ||
                !argument_fields.optional_text(names::title_member, argument.title) ||
                !argument_fields.optional_text(names::description_member, argument.description) ||
                !argument_fields.flag(names::required_member, argument.required)) {
                return false;
            }
            out.arguments.push_back(std::move(argument));
        }
    }
    return read_icons(reader, fields, out.icons) && fields.meta(out.meta);
}

bool read_prompt_message(Reader& reader, const json& wire, const Step* at, PromptMessage& out) {
    Fields fields(reader, wire, at, names::prompt_message_type);
    if (!fields.valid()) return false;
    std::string role;
    if (!fields.text(names::role_member, true, role)) return false;
    const std::optional<Role> parsed = names::parse_role(role);
    if (!parsed) return fields.fail_member(names::role_member, "must be user or assistant");
    out.role = *parsed;
    const json* content = nullptr;
    if (!fields.find(names::content_member, true, content)) return false;
    if (content == nullptr) return true;
    const Step step = member_step(at, names::content_member);
    return read_block(reader, *content, &step, out.content);
}

bool read_prompt_result(Reader& reader, const json& wire, const Step* at, PromptResult& out) {
    Fields fields(reader, wire, at, names::prompt_result_type);
    if (!fields.valid()) return false;
    if (!fields.optional_text(names::description_member, out.description)) return false;
    const json::array_t* messages = nullptr;
    if (!fields.array(names::messages_member, true, messages)) return false;
    if (messages != nullptr) {
        const Step list = member_step(at, names::messages_member);
        for (std::size_t i = 0; i < messages->size(); ++i) {
            const Step element = index_step(&list, i);
            PromptMessage message;
            if (!read_prompt_message(reader, (*messages)[i], &element, message)) return false;
            out.messages.push_back(std::move(message));
        }
    }
    return fields.meta(out.meta);
}

bool read_completion(Reader& reader, const json& wire, const Step* at, CompletionResult& out) {
    Fields fields(reader, wire, at, names::complete_result_type);
    if (!fields.valid()) return false;
    const json* completion = nullptr;
    if (!fields.find(names::completion_member, true, completion)) return false;
    if (completion != nullptr) {
        const Step step = member_step(at, names::completion_member);
        // The completion object is inline in every revision's schema: there is no type of it in the profile.
        Fields inner(reader, *completion, &step, {});
        if (!inner.valid()) return false;
        if (!inner.text_list(names::values_member, true, out.values)) return false;
        if (out.values.size() > max_completion_values) {
            return inner.fail_member(names::values_member, "has more values than a completion may carry");
        }
        if (!inner.optional_count(names::total_member, out.total) ||
            !inner.optional_flag(names::has_more_member, out.has_more)) {
            return false;
        }
    }
    return fields.meta(out.meta);
}

bool read_implementation(Reader& reader, const json& wire, const Step* at, Implementation& out) {
    Fields fields(reader, wire, at, names::implementation_type);
    if (!fields.valid()) return false;
    return fields.text(names::name_member, true, out.name) && fields.optional_text(names::title_member, out.title) &&
           fields.text(names::version_member, true, out.version) &&
           fields.optional_text(names::description_member, out.description) &&
           fields.optional_text(names::website_url_member, out.website_url) && read_icons(reader, fields, out.icons);
}

// Runs a reader over `wire` and turns its outcome into the value or the description.
template <typename T, typename Read>
WireRead<T> run(const json& wire, const profile::Profile& profile, std::string_view at, Read read) {
    Reader reader(profile, at);
    T out{};
    if (!read(reader, wire, static_cast<const Step*>(nullptr), out)) {
        return reader.failed() ? reader.take_error() : std::string(at) + ": cannot be read";
    }
    return out;
}

}  // namespace

template <>
WireRead<Icon> from_wire<Icon>(const json& wire, const profile::Profile& profile, std::string_view at) {
    return run<Icon>(wire, profile, at, read_icon);
}

template <>
WireRead<Implementation> from_wire<Implementation>(const json& wire, const profile::Profile& profile,
                                                   std::string_view at) {
    return run<Implementation>(wire, profile, at, read_implementation);
}

template <>
WireRead<ToolAnnotations> from_wire<ToolAnnotations>(const json& wire, const profile::Profile& profile,
                                                     std::string_view at) {
    return run<ToolAnnotations>(wire, profile, at, read_tool_annotations);
}

template <>
WireRead<ToolDefinition> from_wire<ToolDefinition>(const json& wire, const profile::Profile& profile,
                                                   std::string_view at) {
    return run<ToolDefinition>(wire, profile, at, read_tool);
}

template <>
WireRead<ContentBlock> from_wire<ContentBlock>(const json& wire, const profile::Profile& profile,
                                               std::string_view at) {
    return run<ContentBlock>(wire, profile, at, read_block);
}

template <>
WireRead<ResourceDefinition> from_wire<ResourceDefinition>(const json& wire, const profile::Profile& profile,
                                                           std::string_view at) {
    return run<ResourceDefinition>(wire, profile, at, read_resource);
}

template <>
WireRead<ResourceTemplateDefinition> from_wire<ResourceTemplateDefinition>(const json& wire,
                                                                           const profile::Profile& profile,
                                                                           std::string_view at) {
    return run<ResourceTemplateDefinition>(wire, profile, at, read_resource_template);
}

template <>
WireRead<ResourceContents> from_wire<ResourceContents>(const json& wire, const profile::Profile& profile,
                                                       std::string_view at) {
    return run<ResourceContents>(wire, profile, at, read_resource_contents);
}

template <>
WireRead<ReadResult> from_wire<ReadResult>(const json& wire, const profile::Profile& profile, std::string_view at) {
    return run<ReadResult>(wire, profile, at, read_read_result);
}

template <>
WireRead<PromptDefinition> from_wire<PromptDefinition>(const json& wire, const profile::Profile& profile,
                                                       std::string_view at) {
    return run<PromptDefinition>(wire, profile, at, read_prompt);
}

template <>
WireRead<PromptMessage> from_wire<PromptMessage>(const json& wire, const profile::Profile& profile,
                                                 std::string_view at) {
    return run<PromptMessage>(wire, profile, at, read_prompt_message);
}

template <>
WireRead<PromptResult> from_wire<PromptResult>(const json& wire, const profile::Profile& profile,
                                               std::string_view at) {
    return run<PromptResult>(wire, profile, at, read_prompt_result);
}

template <>
WireRead<CompletionResult> from_wire<CompletionResult>(const json& wire, const profile::Profile& profile,
                                                       std::string_view at) {
    return run<CompletionResult>(wire, profile, at, read_completion);
}

template <>
WireRead<ToolResult> from_wire<ToolResult>(const json& wire, const profile::Profile& profile, std::string_view at) {
    return run<ToolResult>(wire, profile, at, read_tool_result);
}

WireRead<std::optional<CacheHints>> read_cache_hints(const json& result, std::string_view result_type_name,
                                                     const profile::Profile& profile, std::string_view at) {
    Reader reader(profile, at);
    std::optional<CacheHints> hints;
    if (!read_cache(reader, result, result_type_name, nullptr, hints)) return reader.take_error();
    return hints;
}

}  // namespace neomcp
