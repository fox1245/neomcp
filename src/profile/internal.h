// Shared by the sources of the profile library; not part of the public interface.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "neomcp/profile.h"

namespace neomcp::profile::detail {

// A profile document built into the library; the table is generated from profiles/*.json at build time.
struct BuiltinEntry {
    std::string_view revision;  // the file name without ".json"
    const unsigned char* data;
    std::size_t size;
};

std::span<const BuiltinEntry> builtin_entries() noexcept;

// An extension profile document built into the library; the table is generated from
// profiles/extensions/<folder>/<revision>.json at build time.
struct BuiltinExtensionEntry {
    std::string_view folder;    // the folder the file is in
    std::string_view revision;  // the file name without ".json"
    const unsigned char* data;
    std::size_t size;
};

std::span<const BuiltinExtensionEntry> builtin_extension_entries() noexcept;

}  // namespace neomcp::profile::detail
