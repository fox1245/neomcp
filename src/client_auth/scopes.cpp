// Scope values (scopes.h).
#include "client_auth/scopes.h"

#include <algorithm>

namespace neomcp::detail::oauth {

bool contains_scope(std::span<const std::string> values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::string> bounded_scopes(std::span<const std::string> values, const OAuthBounds& bounds,
                                        std::vector<FlowDiagnostic>& diagnostics) {
    std::vector<std::string> kept;
    std::size_t bytes = 0;
    bool dropped = false;
    bool truncated = false;
    for (const std::string& value : values) {
        if (!is_scope_token(value)) {
            dropped = true;
            continue;
        }
        if (contains_scope(kept, value)) continue;
        const std::size_t added = value.size() + (kept.empty() ? 0 : 1);
        if (kept.size() >= bounds.max_scopes || bytes + added > bounds.max_scope_bytes) {
            truncated = true;
            break;
        }
        bytes += added;
        kept.push_back(value);
    }
    if (dropped) {
        diagnostics.push_back({FlowDiagnostic::Kind::scope_dropped,
                               "a scope value outside the scope-token syntax was left out"});
    }
    if (truncated) {
        diagnostics.push_back({FlowDiagnostic::Kind::scopes_truncated,
                               "a list of scopes exceeded its bounds; the values after the bound were left out"});
    }
    return kept;
}

std::vector<std::string> scope_union(std::span<const std::string> first, std::span<const std::string> second) {
    std::vector<std::string> out;
    for (const std::string& value : first) {
        if (!contains_scope(out, value)) out.push_back(value);
    }
    for (const std::string& value : second) {
        if (!contains_scope(out, value)) out.push_back(value);
    }
    return out;
}

bool is_strict_superset(std::span<const std::string> larger, std::span<const std::string> smaller) {
    for (const std::string& value : smaller) {
        if (!contains_scope(larger, value)) return false;
    }
    for (const std::string& value : larger) {
        if (!contains_scope(smaller, value)) return true;
    }
    return false;
}

std::string join_scopes(std::span<const std::string> values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out.push_back(' ');
        out.append(value);
    }
    return out;
}

std::vector<std::string> split_scopes(std::string_view text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t space = text.find(' ', at);
        const std::size_t end = space == std::string_view::npos ? text.size() : space;
        if (end > at) out.emplace_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

}  // namespace neomcp::detail::oauth
