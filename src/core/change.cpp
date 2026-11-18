#include "neomcp/change.h"

#include <algorithm>

#include "neomcp/feature_host.h"

namespace neomcp {

bool ChangeFilter::wants(const Change& change) const {
    switch (change.kind) {
        case ChangeKind::tools_list: return tools_list;
        case ChangeKind::prompts_list: return prompts_list;
        case ChangeKind::resources_list: return resources_list;
        case ChangeKind::resource_updated:
            return std::find(resources.begin(), resources.end(), change.uri) != resources.end();
    }
    return false;
}

ChangeRegistration& ChangeRegistration::operator=(ChangeRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        release_ = std::exchange(other.release_, nullptr);
    }
    return *this;
}

void ChangeRegistration::reset() noexcept {
    std::function<void()> release = std::exchange(release_, nullptr);
    if (!release) return;
    try {
        release();
    } catch (...) {
        // A release that throws still ends the registration as far as its holder is concerned.
    }
}

bool ResourceHost::offers(std::string_view uri) const {
    const std::span<const ResourceDefinition> list = resources();
    for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index].uri == uri && resource_enabled(index)) return true;
    }
    return false;
}

}  // namespace neomcp
