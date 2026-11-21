#include "neomcp/model.h"

#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/authorization.h"

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

// ---------------------------------------------------------------------------------------------------------------
// Authorization (neomcp/authorization.h)

std::string PrincipalKey::text() const {
    std::string out;
    for (const std::string* part : {&issuer, &subject, &client_id}) {
        out += std::to_string(part->size());
        out += ':';
        out += *part;
    }
    return out;
}

PrincipalKey principal_of(const Authorization& granted) {
    return PrincipalKey{granted.issuer, granted.subject.value_or(std::string()),
                        granted.client_id.value_or(std::string())};
}

bool is_scope_token(std::string_view value) noexcept {
    // RFC 6749, section 3.3: scope-token = 1*( %x21 / %x23-5B / %x5D-7E ).
    static constexpr char exclamation = '\x21';
    static constexpr char quote = '\x22';
    static constexpr char backslash = '\x5C';
    static constexpr char tilde = '\x7E';
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return c >= exclamation && c <= tilde && c != quote && c != backslash;
    });
}

ScopeImplications::ScopeImplications(const Declared& declared) {
    for (const auto& [scope, implied] : declared) {
        if (!is_scope_token(scope)) {
            throw std::invalid_argument("neomcp: the scope '" + scope + "' of scope_implications is not a scope-token");
        }
        for (const auto& each : implied) {
            if (!is_scope_token(each)) {
                throw std::invalid_argument("neomcp: the scope '" + each + "' that '" + scope +
                                            "' implies is not a scope-token");
            }
        }
    }
    // Each scope's closure: everything reachable from it along the declared edges, found by a walk that visits each
    // scope once, so that cycles end.
    auto closed = std::make_shared<std::map<std::string, std::vector<std::string>, std::less<>>>();
    for (const auto& [scope, direct] : declared) {
        std::set<std::string, std::less<>> reached;
        std::vector<std::string_view> pending(direct.begin(), direct.end());
        while (!pending.empty()) {
            const std::string_view next = pending.back();
            pending.pop_back();
            if (!reached.emplace(next).second) continue;
            if (const auto further = declared.find(std::string(next)); further != declared.end()) {
                pending.insert(pending.end(), further->second.begin(), further->second.end());
            }
        }
        if (!reached.empty()) closed->emplace(scope, std::vector<std::string>(reached.begin(), reached.end()));
    }
    if (!closed->empty()) closed_ = std::move(closed);
}

std::span<const std::string> ScopeImplications::implied_by(std::string_view scope) const {
    if (!closed_) return {};
    const auto found = closed_->find(scope);
    if (found == closed_->end()) return {};
    return found->second;
}

bool ScopeImplications::met(std::span<const std::string> granted, std::string_view scope) const {
    for (const std::string& held : granted) {
        if (held == scope) return true;
        const auto implied = implied_by(held);
        if (std::binary_search(implied.begin(), implied.end(), scope, std::less<>())) return true;
    }
    return false;
}

bool ScopeImplications::empty() const noexcept { return !closed_; }

std::vector<std::string> missing_scopes(std::span<const std::string> granted, std::span<const std::string> required,
                                        const ScopeImplications& implications) {
    std::vector<std::string> missing;
    for (const std::string& scope : required) {
        if (implications.met(granted, scope)) continue;
        if (std::find(missing.begin(), missing.end(), scope) != missing.end()) continue;
        missing.push_back(scope);
    }
    return missing;
}

}  // namespace neomcp
