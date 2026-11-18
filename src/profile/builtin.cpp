#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "profile/internal.h"

namespace neomcp::profile {
namespace {

const detail::BuiltinEntry* find_entry(std::string_view revision, std::size_t* index = nullptr) {
    const auto entries = detail::builtin_entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].revision == revision) {
            if (index != nullptr) *index = i;
            return &entries[i];
        }
    }
    return nullptr;
}

}  // namespace

std::span<const std::string_view> builtin_revisions() {
    static const std::vector<std::string_view> revisions = [] {
        std::vector<std::string_view> names;
        for (const auto& entry : detail::builtin_entries()) names.push_back(entry.revision);
        std::sort(names.begin(), names.end());
        return names;
    }();
    return revisions;
}

std::string_view builtin_text(std::string_view revision) {
    const auto* entry = find_entry(revision);
    if (entry == nullptr) return {};
    return {reinterpret_cast<const char*>(entry->data), entry->size};
}

ProfilePtr builtin(std::string_view revision) {
    std::size_t index = 0;
    const auto* entry = find_entry(revision, &index);
    if (entry == nullptr) return nullptr;

    static std::mutex mutex;
    static std::vector<ProfilePtr> cache(detail::builtin_entries().size());
    const std::lock_guard<std::mutex> lock(mutex);
    if (cache[index]) return cache[index];

    auto result = load(builtin_text(revision));
    if (const auto* error = std::get_if<Error>(&result)) {
        throw std::logic_error("built-in profile " + std::string(revision) + " is invalid at \"" + error->pointer +
                               "\": expected " + error->expected + "; " + error->message);
    }
    ProfilePtr profile = std::get<ProfilePtr>(std::move(result));
    if (profile->revision() != revision) {
        throw std::logic_error("built-in profile " + std::string(revision) + " is invalid at \"/revision\": the file " +
                               "declares revision " + profile->revision());
    }
    cache[index] = profile;
    return profile;
}

}  // namespace neomcp::profile
