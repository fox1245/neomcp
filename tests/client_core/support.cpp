#include "support.h"

#include <algorithm>
#include <array>

namespace client_core_test {

using namespace neomcp;

// ---- Generator

namespace {

// Pieces of text that cover the corners of JSON strings and of header values: quotes, escapes, control characters,
// multi-byte characters, spaces at the ends, and a string that looks like the encoded header form.
const std::array<std::string_view, 16> fragments{"a",
                                                 "Hello",
                                                 "us-west1",
                                                 "line1\nline2",
                                                 "\"quoted\"",
                                                 "back\\slash",
                                                 "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                                                 "\xF0\x9F\x98\x80",
                                                 "tab\there",
                                                 "\001"
                                                 "ctl",
                                                 " padded ",
                                                 "=?base64?x?=",
                                                 "https://example.test/a?b=c&d=e",
                                                 "0",
                                                 "{}",
                                                 "x y"};

const std::array<std::uint64_t, 9> counts{0, 1, 2, 255, 65536, 4294967296ULL, 9007199254740991ULL,
                                          9223372036854775808ULL, 18446744073709551615ULL};

// Names of the members the generator adds as extra members of a block: none is a member of any revision's types.
constexpr std::string_view extra_prefix = "x-extra-";

}  // namespace

bool Generator::some() {
    switch (fill_) {
        case Fill::none: return false;
        case Fill::all: return true;
        case Fill::any: return random_.chance();
    }
    return false;
}

std::size_t Generator::items(std::size_t most) {
    switch (fill_) {
        case Fill::none: return 0;
        case Fill::all: return std::min<std::size_t>(most, 2);
        case Fill::any: return random_.below(most + 1);
    }
    return 0;
}

bool Generator::has(std::string_view type_name, std::string_view member) const {
    const profile::Type* type = profile_.type(type_name);
    return type == nullptr || type->has_property(member);
}

std::string Generator::text() {
    if (fill_ == Fill::none) return {};
    std::string out;
    const std::size_t parts = fill_ == Fill::all ? 1 + random_.below(2) : random_.below(4);
    for (std::size_t i = 0; i < parts; ++i) out += fragments[random_.below(fragments.size())];
    return out;
}

double Generator::real() { return (static_cast<double>(random_.below(41)) - 8.0) / 8.0; }

std::uint64_t Generator::count_value() { return counts[random_.below(counts.size())]; }

json Generator::scalar() {
    switch (random_.below(5)) {
        case 0: return json(nullptr);
        case 1: return json(random_.chance());
        case 2: return json(static_cast<std::int64_t>(random_.below(2000)) - 1000);
        case 3: return json(real());
        default: return json(text());
    }
}

json Generator::value(int depth) {
    if (depth <= 0) return scalar();
    switch (random_.below(4)) {
        case 0: {
            json list = json::array();
            for (std::size_t i = 0, n = random_.below(4); i < n; ++i) list.push_back(value(depth - 1));
            return list;
        }
        case 1: return object(depth - 1);
        default: return scalar();
    }
}

json Generator::object(int depth) {
    json out = json::object();
    const std::size_t n = fill_ == Fill::none ? 0 : (fill_ == Fill::all ? 2 : random_.below(4));
    for (std::size_t i = 0; i < n; ++i) out[text() + std::to_string(i)] = value(depth);
    return out;
}

std::optional<json> Generator::meta(std::string_view type_name) {
    if (!on(type_name, "_meta")) return std::nullopt;
    return object();
}

json Generator::extra() {
    json out = json::object();
    if (project_ || !some()) return out;
    for (std::size_t i = 0, n = 1 + random_.below(2); i < n; ++i) {
        out[std::string(extra_prefix) + std::to_string(i)] = scalar();
    }
    return out;
}

Role Generator::role() { return random_.chance() ? Role::user : Role::assistant; }

std::optional<std::string> Generator::optional_text(std::string_view type_name, std::string_view member) {
    if (!on(type_name, member)) return std::nullopt;
    return text();
}

std::vector<Icon> Generator::icons(std::string_view type_name) {
    std::vector<Icon> out;
    if (!has(type_name, "icons")) return out;
    for (std::size_t i = 0, n = items(3); i < n; ++i) out.push_back(icon());
    return out;
}

std::optional<ContentAnnotations> Generator::annotations(std::string_view type_name) {
    if (!on(type_name, "annotations")) return std::nullopt;
    return content_annotations(type_name);
}

void Generator::fill_common(std::optional<ContentAnnotations>& annotation_slot, std::optional<json>& meta_slot,
                            json& extra_slot, std::string_view type_name) {
    annotation_slot = annotations(type_name);
    meta_slot = meta(type_name);
    extra_slot = extra();
}

Icon Generator::icon() {
    Icon out;
    out.src = text();
    out.mime_type = optional_text("Icon", "mimeType");
    if (has("Icon", "sizes")) {
        for (std::size_t i = 0, n = items(3); i < n; ++i) out.sizes.push_back(random_.chance() ? "48x48" : text());
    }
    if (on("Icon", "theme")) out.theme = random_.chance() ? IconTheme::light : IconTheme::dark;
    return out;
}

Implementation Generator::implementation() {
    Implementation out;
    out.name = text();
    out.version = text();
    out.title = optional_text("Implementation", "title");
    out.description = optional_text("Implementation", "description");
    out.website_url = optional_text("Implementation", "websiteUrl");
    out.icons = icons("Implementation");
    return out;
}

ToolAnnotations Generator::tool_annotations() {
    ToolAnnotations out;
    out.title = optional_text("ToolAnnotations", "title");
    if (on("ToolAnnotations", "readOnlyHint")) out.read_only_hint = random_.chance();
    if (on("ToolAnnotations", "destructiveHint")) out.destructive_hint = random_.chance();
    if (on("ToolAnnotations", "idempotentHint")) out.idempotent_hint = random_.chance();
    if (on("ToolAnnotations", "openWorldHint")) out.open_world_hint = random_.chance();
    return out;
}

ToolDefinition Generator::tool() {
    ToolDefinition out;
    out.name = text();
    out.title = optional_text("Tool", "title");
    out.description = optional_text("Tool", "description");
    out.input_schema = object();
    if (on("Tool", "outputSchema")) out.output_schema = object();
    if (on("Tool", "annotations")) out.annotations = tool_annotations();
    out.icons = icons("Tool");
    out.meta = meta("Tool");
    return out;
}

ContentAnnotations Generator::content_annotations(std::string_view owner_type) {
    // The members are those of the profile's type of the owner's `annotations` property; a revision that gives the
    // property no type holds nothing back.
    const profile::Type* owner = profile_.type(owner_type);
    const profile::Type* type = owner == nullptr ? nullptr : profile_.property_type(*owner, "annotations");
    const auto allowed = [&](std::string_view member) { return type == nullptr || type->has_property(member); };
    ContentAnnotations out;
    if (allowed("audience")) {
        for (std::size_t i = 0, n = items(3); i < n; ++i) out.audience.push_back(role());
    }
    if ((!project_ || allowed("priority")) && some()) out.priority = real();
    if ((!project_ || allowed("lastModified")) && some()) out.last_modified = text();
    return out;
}

ContentBlock Generator::block() {
    std::vector<ContentKind> kinds;
    for (const ContentKind kind : {ContentKind::text, ContentKind::image, ContentKind::audio,
                                   ContentKind::resource_link, ContentKind::embedded_resource}) {
        if (!project_ || has_content_kind(kind, profile_)) kinds.push_back(kind);
    }
    return block(kinds[random_.below(kinds.size())]);
}

ContentBlock Generator::block(ContentKind kind) {
    const std::string type(type_name(kind));
    switch (kind) {
        case ContentKind::text: {
            TextContent out;
            out.text = text();
            fill_common(out.annotations, out.meta, out.extra, type);
            return out;
        }
        case ContentKind::image: {
            ImageContent out;
            out.data = text();
            out.mime_type = text();
            fill_common(out.annotations, out.meta, out.extra, type);
            return out;
        }
        case ContentKind::audio: {
            AudioContent out;
            out.data = text();
            out.mime_type = text();
            fill_common(out.annotations, out.meta, out.extra, type);
            return out;
        }
        case ContentKind::resource_link: {
            ResourceLink out;
            out.uri = text();
            out.name = text();
            out.title = optional_text(type, "title");
            out.description = optional_text(type, "description");
            out.mime_type = optional_text(type, "mimeType");
            if (on(type, "size")) out.size = count_value();
            out.icons = icons(type);
            fill_common(out.annotations, out.meta, out.extra, type);
            return out;
        }
        case ContentKind::embedded_resource: {
            EmbeddedResource out;
            out.resource = resource_contents();
            fill_common(out.annotations, out.meta, out.extra, type);
            return out;
        }
    }
    return TextContent{};
}

ResourceContents Generator::resource_contents() {
    if (random_.chance()) {
        TextResourceContents out;
        out.uri = text();
        out.mime_type = optional_text("TextResourceContents", "mimeType");
        out.text = text();
        out.meta = meta("TextResourceContents");
        return out;
    }
    BlobResourceContents out;
    out.uri = text();
    out.mime_type = optional_text("BlobResourceContents", "mimeType");
    out.blob = text();
    out.meta = meta("BlobResourceContents");
    return out;
}

ResourceDefinition Generator::resource() {
    ResourceDefinition out;
    out.uri = text();
    out.name = text();
    out.title = optional_text("Resource", "title");
    out.description = optional_text("Resource", "description");
    out.mime_type = optional_text("Resource", "mimeType");
    if (on("Resource", "size")) out.size = count_value();
    out.annotations = annotations("Resource");
    out.icons = icons("Resource");
    out.meta = meta("Resource");
    return out;
}

ResourceTemplateDefinition Generator::resource_template() {
    ResourceTemplateDefinition out;
    out.uri_template = text();
    out.name = text();
    out.title = optional_text("ResourceTemplate", "title");
    out.description = optional_text("ResourceTemplate", "description");
    out.mime_type = optional_text("ResourceTemplate", "mimeType");
    out.annotations = annotations("ResourceTemplate");
    out.icons = icons("ResourceTemplate");
    out.meta = meta("ResourceTemplate");
    return out;
}

ReadResult Generator::read_result() {
    ReadResult out;
    for (std::size_t i = 0, n = items(3); i < n; ++i) out.contents.push_back(resource_contents());
    out.meta = meta("ReadResourceResult");
    return out;
}

PromptDefinition Generator::prompt() {
    PromptDefinition out;
    out.name = text();
    out.title = optional_text("Prompt", "title");
    out.description = optional_text("Prompt", "description");
    for (std::size_t i = 0, n = items(3); i < n; ++i) {
        PromptArgumentDefinition argument;
        argument.name = text();
        argument.title = optional_text("PromptArgument", "title");
        argument.description = optional_text("PromptArgument", "description");
        argument.required = some() && random_.chance();
        if (fill_ == Fill::all) argument.required = true;
        out.arguments.push_back(std::move(argument));
    }
    out.icons = icons("Prompt");
    out.meta = meta("Prompt");
    return out;
}

PromptMessage Generator::prompt_message() {
    PromptMessage out;
    out.role = role();
    out.content = block();
    return out;
}

PromptResult Generator::prompt_result() {
    PromptResult out;
    out.description = optional_text("GetPromptResult", "description");
    for (std::size_t i = 0, n = items(3); i < n; ++i) out.messages.push_back(prompt_message());
    out.meta = meta("GetPromptResult");
    return out;
}

CompletionResult Generator::completion() {
    CompletionResult out;
    std::size_t n = 0;
    switch (fill_) {
        case Fill::none: n = 0; break;
        case Fill::all: n = 3; break;
        case Fill::any: n = random_.below(project_ ? max_completion_values + 1 : max_completion_values + 31); break;
    }
    for (std::size_t i = 0; i < n; ++i) out.values.push_back(text());
    if (some()) out.total = count_value();
    if (some()) out.has_more = random_.chance();
    out.meta = meta("CompleteResult");
    return out;
}

ToolResult Generator::tool_result() {
    ToolResult out;
    for (std::size_t i = 0, n = items(3); i < n; ++i) out.content.push_back(block());
    if (on("CallToolResult", "structuredContent")) out.structured_content = object();
    out.is_error = on("CallToolResult", "isError") && random_.chance();
    if (fill_ == Fill::all && has("CallToolResult", "isError")) out.is_error = true;
    out.meta = meta("CallToolResult");
    return out;
}

// ---- Equality

bool same(const json& a, const json& b) { return a == b; }

bool same(const Icon& a, const Icon& b) {
    return same(a.src, b.src) && same(a.mime_type, b.mime_type) && same(a.sizes, b.sizes) && same(a.theme, b.theme);
}

bool same(const Implementation& a, const Implementation& b) {
    return same(a.name, b.name) && same(a.version, b.version) && same(a.title, b.title) &&
           same(a.description, b.description) && same(a.website_url, b.website_url) && same(a.icons, b.icons);
}

bool same(const ToolAnnotations& a, const ToolAnnotations& b) {
    return same(a.title, b.title) && same(a.read_only_hint, b.read_only_hint) &&
           same(a.destructive_hint, b.destructive_hint) && same(a.idempotent_hint, b.idempotent_hint) &&
           same(a.open_world_hint, b.open_world_hint);
}

bool same(const ToolDefinition& a, const ToolDefinition& b) {
    return same(a.name, b.name) && same(a.title, b.title) && same(a.description, b.description) &&
           same(a.input_schema, b.input_schema) && same(a.output_schema, b.output_schema) &&
           same(a.annotations, b.annotations) && same(a.icons, b.icons) && same(a.meta, b.meta);
}

bool same(const ContentAnnotations& a, const ContentAnnotations& b) {
    return same(a.audience, b.audience) && same(a.priority, b.priority) && same(a.last_modified, b.last_modified);
}

namespace {

bool same_resource_contents(const TextResourceContents& a, const TextResourceContents& b) {
    return same(a.uri, b.uri) && same(a.mime_type, b.mime_type) && same(a.text, b.text) && same(a.meta, b.meta);
}

bool same_resource_contents(const BlobResourceContents& a, const BlobResourceContents& b) {
    return same(a.uri, b.uri) && same(a.mime_type, b.mime_type) && same(a.blob, b.blob) && same(a.meta, b.meta);
}

bool same_block(const TextContent& a, const TextContent& b) {
    return same(a.text, b.text) && same(a.annotations, b.annotations) && same(a.meta, b.meta) && same(a.extra, b.extra);
}

bool same_block(const ImageContent& a, const ImageContent& b) {
    return same(a.data, b.data) && same(a.mime_type, b.mime_type) && same(a.annotations, b.annotations) &&
           same(a.meta, b.meta) && same(a.extra, b.extra);
}

bool same_block(const AudioContent& a, const AudioContent& b) {
    return same(a.data, b.data) && same(a.mime_type, b.mime_type) && same(a.annotations, b.annotations) &&
           same(a.meta, b.meta) && same(a.extra, b.extra);
}

bool same_block(const ResourceLink& a, const ResourceLink& b) {
    return same(a.uri, b.uri) && same(a.name, b.name) && same(a.title, b.title) && same(a.description, b.description) &&
           same(a.mime_type, b.mime_type) && same(a.size, b.size) && same(a.icons, b.icons) &&
           same(a.annotations, b.annotations) && same(a.meta, b.meta) && same(a.extra, b.extra);
}

bool same_block(const EmbeddedResource& a, const EmbeddedResource& b) {
    return same(a.resource, b.resource) && same(a.annotations, b.annotations) && same(a.meta, b.meta) &&
           same(a.extra, b.extra);
}

}  // namespace

bool same(const ContentBlock& a, const ContentBlock& b) {
    if (a.index() != b.index()) return false;
    return std::visit(
        [&](const auto& left) {
            using T = std::decay_t<decltype(left)>;
            return same_block(left, std::get<T>(b));
        },
        a);
}

bool same(const ResourceContents& a, const ResourceContents& b) {
    if (a.index() != b.index()) return false;
    return std::visit(
        [&](const auto& left) {
            using T = std::decay_t<decltype(left)>;
            return same_resource_contents(left, std::get<T>(b));
        },
        a);
}

bool same(const ResourceDefinition& a, const ResourceDefinition& b) {
    return same(a.uri, b.uri) && same(a.name, b.name) && same(a.title, b.title) && same(a.description, b.description) &&
           same(a.mime_type, b.mime_type) && same(a.size, b.size) && same(a.annotations, b.annotations) &&
           same(a.icons, b.icons) && same(a.meta, b.meta);
}

bool same(const ResourceTemplateDefinition& a, const ResourceTemplateDefinition& b) {
    return same(a.uri_template, b.uri_template) && same(a.name, b.name) && same(a.title, b.title) &&
           same(a.description, b.description) && same(a.mime_type, b.mime_type) &&
           same(a.annotations, b.annotations) && same(a.icons, b.icons) && same(a.meta, b.meta);
}

bool same(const ReadResult& a, const ReadResult& b) {
    const auto hints = [](const std::optional<CacheHints>& left, const std::optional<CacheHints>& right) {
        if (left.has_value() != right.has_value()) return false;
        return !left || (left->ttl_ms == right->ttl_ms && left->scope == right->scope);
    };
    return same(a.contents, b.contents) && same(a.meta, b.meta) && hints(a.cache, b.cache);
}

bool same(const PromptDefinition& a, const PromptDefinition& b) {
    if (a.arguments.size() != b.arguments.size()) return false;
    for (std::size_t i = 0; i < a.arguments.size(); ++i) {
        const PromptArgumentDefinition& left = a.arguments[i];
        const PromptArgumentDefinition& right = b.arguments[i];
        if (!(same(left.name, right.name) && same(left.title, right.title) && same(left.description, right.description) &&
              same(left.required, right.required))) {
            return false;
        }
    }
    return same(a.name, b.name) && same(a.title, b.title) && same(a.description, b.description) &&
           same(a.icons, b.icons) && same(a.meta, b.meta);
}

bool same(const PromptMessage& a, const PromptMessage& b) { return same(a.role, b.role) && same(a.content, b.content); }

bool same(const PromptResult& a, const PromptResult& b) {
    return same(a.description, b.description) && same(a.messages, b.messages) && same(a.meta, b.meta);
}

bool same(const CompletionResult& a, const CompletionResult& b) {
    return same(a.values, b.values) && same(a.total, b.total) && same(a.has_more, b.has_more) && same(a.meta, b.meta);
}

bool same(const ToolResult& a, const ToolResult& b) {
    return same(a.content, b.content) && same(a.structured_content, b.structured_content) &&
           same(a.is_error, b.is_error) && same(a.meta, b.meta);
}

}  // namespace client_core_test
