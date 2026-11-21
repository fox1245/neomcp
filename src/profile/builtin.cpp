#include <algorithm>
#include <map>
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

namespace {

std::string place(const detail::BuiltinExtensionEntry& entry) {
    return "built-in extension profile " + std::string(entry.folder) + "/" + std::string(entry.revision);
}

// Every built-in extension profile, loaded once.
struct LoadedExtensions {
    std::vector<ExtensionPtr> profiles;      // parallel to detail::builtin_extension_entries()
    std::vector<std::string> identifiers;    // sorted, each once
};

const LoadedExtensions& loaded_extensions() {
    static const LoadedExtensions loaded = [] {
        LoadedExtensions out;
        const auto entries = detail::builtin_extension_entries();
        for (const auto& entry : entries) {
            auto result = load_extension({reinterpret_cast<const char*>(entry.data), entry.size});
            if (const auto* error = std::get_if<Error>(&result)) {
                throw std::logic_error(place(entry) + " is invalid at \"" + error->pointer + "\": expected " +
                                       error->expected + "; " + error->message);
            }
            ExtensionPtr extension = std::get<ExtensionPtr>(std::move(result));
            for (const auto& revision : extension->revisions()) {
                for (const auto& other : out.profiles) {
                    if (other->extension() == extension->extension() && other->applies_to(revision)) {
                        throw std::logic_error(place(entry) + " defines " + extension->extension() + " for " +
                                               revision + ", which another built-in file defines too");
                    }
                }
            }
            out.identifiers.push_back(extension->extension());
            out.profiles.push_back(std::move(extension));
        }
        std::sort(out.identifiers.begin(), out.identifiers.end());
        out.identifiers.erase(std::unique(out.identifiers.begin(), out.identifiers.end()), out.identifiers.end());
        return out;
    }();
    return loaded;
}

}  // namespace

std::span<const std::string> builtin_extensions() { return loaded_extensions().identifiers; }

std::string_view builtin_extension_text(std::string_view extension, std::string_view revision) {
    const auto& profiles = loaded_extensions().profiles;
    const auto entries = detail::builtin_extension_entries();
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i]->extension() == extension && profiles[i]->applies_to(revision)) {
            return {reinterpret_cast<const char*>(entries[i].data), entries[i].size};
        }
    }
    return {};
}

ExtensionPtr builtin_extension(std::string_view extension, std::string_view revision) {
    for (const auto& profile : loaded_extensions().profiles) {
        if (profile->extension() == extension && profile->applies_to(revision)) return profile;
    }
    return nullptr;
}

ProfilePtr builtin(std::string_view revision, std::span<const std::string_view> extensions) {
    ProfilePtr core = builtin(revision);
    if (core == nullptr || extensions.empty()) return core;

    std::vector<ExtensionPtr> chosen;
    std::string key(revision);
    for (const auto name : extensions) {
        ExtensionPtr extension = builtin_extension(name, revision);
        if (extension == nullptr) return nullptr;
        chosen.push_back(std::move(extension));
        key += '\n';
        key += name;
    }

    static std::mutex mutex;
    static std::map<std::string, ProfilePtr> cache;
    const std::lock_guard<std::mutex> lock(mutex);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    auto result = compose(*core, chosen);
    if (const auto* error = std::get_if<Error>(&result)) {
        throw std::logic_error("built-in profile " + std::string(revision) + " cannot be served with its built-in " +
                               "extensions: invalid at \"" + error->pointer + "\": expected " + error->expected + "; " +
                               error->message);
    }
    ProfilePtr profile = std::get<ProfilePtr>(std::move(result));
    cache.emplace(std::move(key), profile);
    return profile;
}

}  // namespace neomcp::profile
