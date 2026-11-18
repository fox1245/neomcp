#include "neomcp/model.h"

#include <array>
#include <type_traits>
#include <utility>
#include <variant>

namespace neomcp {

ContentKind kind_of(const ContentBlock& block) noexcept {
    return std::visit([](const auto& value) { return std::decay_t<decltype(value)>::kind; }, block);
}

std::string_view wire_name(ContentKind kind) noexcept {
    switch (kind) {
        case ContentKind::text: return "text";
        case ContentKind::image: return "image";
        case ContentKind::audio: return "audio";
        case ContentKind::resource_link: return "resource_link";
        case ContentKind::embedded_resource: return "resource";
    }
    return {};
}

std::string_view type_name(ContentKind kind) noexcept {
    switch (kind) {
        case ContentKind::text: return "TextContent";
        case ContentKind::image: return "ImageContent";
        case ContentKind::audio: return "AudioContent";
        case ContentKind::resource_link: return "ResourceLink";
        case ContentKind::embedded_resource: return "EmbeddedResource";
    }
    return {};
}

namespace {

constexpr std::array<std::pair<std::string_view, LogLevel>, 8> log_levels{{
    {"debug", LogLevel::debug},
    {"info", LogLevel::info},
    {"notice", LogLevel::notice},
    {"warning", LogLevel::warning},
    {"error", LogLevel::error},
    {"critical", LogLevel::critical},
    {"alert", LogLevel::alert},
    {"emergency", LogLevel::emergency},
}};

}  // namespace

std::string_view to_string(LogLevel level) noexcept {
    for (const auto& [name, value] : log_levels) {
        if (value == level) return name;
    }
    return {};
}

std::optional<LogLevel> parse_log_level(std::string_view text) noexcept {
    for (const auto& [name, value] : log_levels) {
        if (name == text) return value;
    }
    return std::nullopt;
}

std::string_view to_string(CacheScope scope) noexcept {
    switch (scope) {
        case CacheScope::private_scope: return "private";
        case CacheScope::public_scope: return "public";
    }
    return {};
}

}  // namespace neomcp
