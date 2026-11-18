#include "neomcp/http/random.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif
#endif
#endif

#include <algorithm>
#include <limits>

namespace neomcp::http {

namespace {

// Fills `buffer` from the operating system's random source. False when the source failed.
bool fill_random(char* buffer, std::size_t count) {
#if defined(_WIN32)
    constexpr std::size_t per_call = std::numeric_limits<ULONG>::max();
    while (count > 0) {
        const std::size_t step = std::min(count, per_call);
        const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(step),
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) return false;  // STATUS_SUCCESS
        buffer += step;
        count -= step;
    }
    return true;
#else
    // getentropy fills at most 256 bytes per call.
    constexpr std::size_t per_call = 256;
    while (count > 0) {
        const std::size_t step = std::min(count, per_call);
        if (getentropy(buffer, step) != 0) return false;
        buffer += step;
        count -= step;
    }
    return true;
#endif
}

}  // namespace

std::optional<std::string> random_bytes(std::size_t count) {
    std::string bytes(count, '\0');
    if (!fill_random(bytes.data(), count)) return std::nullopt;
    return bytes;
}

std::string hex_encode(std::string_view bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    constexpr unsigned nibble_bits = 4;
    constexpr unsigned nibble_mask = 0x0F;
    std::string text;
    text.reserve(bytes.size() * 2);
    for (char c : bytes) {
        const unsigned byte = static_cast<unsigned char>(c);
        text += digits[byte >> nibble_bits];
        text += digits[byte & nibble_mask];
    }
    return text;
}

std::optional<std::string> random_hex(std::size_t byte_count) {
    std::optional<std::string> bytes = random_bytes(byte_count);
    if (!bytes) return std::nullopt;
    return hex_encode(*bytes);
}

}  // namespace neomcp::http
