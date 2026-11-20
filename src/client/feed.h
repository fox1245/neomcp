// What a subscription holds for the application between two next(): its changes, merged (neomcp/client/subscription.h).
#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "neomcp/change.h"

namespace neomcp::detail {

// The changes not yet taken, one per list kind and one per resource, in the order of their first arrival. A change of
// a kind (or of a resource) that is already waiting is merged into the one that waits: it keeps its place.
class PendingChanges {
public:
    // Whether the change was added (false: one like it waits already).
    bool add(const Change& change);
    // Every kind and resource `filter` names, as changed: what a gap in the feed may have hidden.
    void add_all(const ChangeFilter& filter);
    // The oldest change; empty when none waits.
    std::optional<Change> take();
    std::size_t size() const noexcept { return queue_.size(); }
    void clear() noexcept;

private:
    std::deque<Change> queue_;
    std::set<std::pair<ChangeKind, std::string>> waiting_;
};

// The members both filters have: the kinds both name, and the resources of `asked` that `honoured` names too, in the
// order of `asked`.
ChangeFilter common_filter(const ChangeFilter& asked, const ChangeFilter& honoured);

// `filter` with each resource once, in the order of its first mention.
ChangeFilter distinct_resources(ChangeFilter filter);

}  // namespace neomcp::detail
