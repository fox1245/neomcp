// The merged changes of a subscription, and the waits before a lost subscription is reopened.
#include "client/feed.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "neomcp/client/subscription.h"

namespace neomcp::detail {

bool PendingChanges::add(const Change& change) {
    std::string uri = change.kind == ChangeKind::resource_updated ? change.uri : std::string();
    if (!waiting_.emplace(change.kind, uri).second) return false;
    queue_.push_back(Change{change.kind, std::move(uri)});
    return true;
}

void PendingChanges::add_all(const ChangeFilter& filter) {
    if (filter.tools_list) add(Change{ChangeKind::tools_list, {}});
    if (filter.prompts_list) add(Change{ChangeKind::prompts_list, {}});
    if (filter.resources_list) add(Change{ChangeKind::resources_list, {}});
    for (const auto& uri : filter.resources) add(Change{ChangeKind::resource_updated, uri});
}

std::optional<Change> PendingChanges::take() {
    if (queue_.empty()) return std::nullopt;
    Change change = std::move(queue_.front());
    queue_.pop_front();
    waiting_.erase({change.kind, change.uri});
    return change;
}

void PendingChanges::clear() noexcept {
    queue_.clear();
    waiting_.clear();
}

ChangeFilter common_filter(const ChangeFilter& asked, const ChangeFilter& honoured) {
    ChangeFilter common;
    common.tools_list = asked.tools_list && honoured.tools_list;
    common.prompts_list = asked.prompts_list && honoured.prompts_list;
    common.resources_list = asked.resources_list && honoured.resources_list;
    for (const auto& uri : asked.resources) {
        if (std::find(honoured.resources.begin(), honoured.resources.end(), uri) != honoured.resources.end()) {
            common.resources.push_back(uri);
        }
    }
    return common;
}

ChangeFilter distinct_resources(ChangeFilter filter) {
    std::vector<std::string> kept;
    kept.reserve(filter.resources.size());
    for (auto& uri : filter.resources) {
        if (std::find(kept.begin(), kept.end(), uri) == kept.end()) kept.push_back(std::move(uri));
    }
    filter.resources = std::move(kept);
    return filter;
}

std::chrono::milliseconds subscription_reopen_bound(std::size_t attempt, std::chrono::milliseconds first,
                                                    std::chrono::milliseconds most) noexcept {
    if (first.count() <= 0 || most.count() <= 0) return std::chrono::milliseconds{0};
    std::chrono::milliseconds bound = std::min(first, most);
    // Doubled once per attempt after the first, until the most; never past it, so the doubling cannot overflow.
    for (std::size_t step = 1; step < attempt && bound < most; ++step) {
        bound = bound.count() > most.count() / 2 ? most : bound * 2;
    }
    return bound;
}

std::chrono::milliseconds subscription_reopen_delay(std::chrono::milliseconds bound, std::uint64_t random) noexcept {
    if (bound.count() <= 0) return std::chrono::milliseconds{0};
    const auto span = static_cast<std::uint64_t>(bound.count()) + 1;
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(random % span)};
}

}  // namespace neomcp::detail
