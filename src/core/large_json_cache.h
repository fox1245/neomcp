// Releases idle neojson thread-local caches when a transient large JSON parse or serialization ends.
#pragma once

#include <cstddef>

#include "neomcp/json.h"

namespace neomcp::detail {

inline constexpr std::size_t large_json_trim_threshold = 64 * 1024;

struct LargeJsonCacheGuard {
    bool active = false;
    explicit LargeJsonCacheGuard(bool on) noexcept : active(on) {}
    ~LargeJsonCacheGuard() noexcept {
        if (active) neojson::trim_thread_caches();
    }
    LargeJsonCacheGuard(const LargeJsonCacheGuard&) = delete;
    LargeJsonCacheGuard& operator=(const LargeJsonCacheGuard&) = delete;
};

}  // namespace neomcp::detail
