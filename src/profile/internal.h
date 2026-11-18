// Shared by the sources of the profile library; not part of the public interface.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "neomcp/profile.h"

namespace neomcp::profile::detail {

// The mechanism names the library declares for a slot. This is the single place where a name is declared: a
// profile that selects any other name is rejected, and code that implements a mechanism attaches to a name here.
std::span<const std::string_view> declared_mechanisms(Slot slot);

// The transports that have a cancellation mechanism.
enum class Transport { stdio, http_sse, streamable_http };

// The cancellation mechanism names the library declares for a transport.
std::span<const std::string_view> declared_cancel(Transport transport);

// A profile document built into the library; the table is generated from profiles/*.json at build time.
struct BuiltinEntry {
    std::string_view revision;  // the file name without ".json"
    const unsigned char* data;
    std::size_t size;
};

std::span<const BuiltinEntry> builtin_entries() noexcept;

}  // namespace neomcp::profile::detail
