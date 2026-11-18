// Tests of random bytes from the operating system and of the hexadecimal encoder.
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "neomcp/http/random.h"

namespace {

using neomcp::http::hex_encode;
using neomcp::http::random_bytes;
using neomcp::http::random_hex;

TEST(HttpRandom, HexEncodesEveryByteAsTwoLowerCaseDigits) {
    EXPECT_EQ(hex_encode(""), "");
    EXPECT_EQ(hex_encode(std::string_view("\x00", 1)), "00");
    EXPECT_EQ(hex_encode("\xff"), "ff");
    EXPECT_EQ(hex_encode(std::string_view("\x01\x23\x45\x67\x89\xab\xcd\xef", 8)), "0123456789abcdef");
    EXPECT_EQ(hex_encode("AZ"), "415a");
    std::string all;
    for (int i = 0; i < 256; ++i) all += static_cast<char>(i);
    const std::string encoded = hex_encode(all);
    ASSERT_EQ(encoded.size(), 512u);
    EXPECT_EQ(encoded.substr(0, 6), "000102");
    EXPECT_EQ(encoded.substr(encoded.size() - 6), "fdfeff");
}

TEST(HttpRandom, ReturnsTheRequestedNumberOfBytes) {
    // 0 and sizes around and beyond what one operating system call returns at most.
    for (std::size_t count : {0u, 1u, 16u, 32u, 255u, 256u, 257u, 512u, 1000u}) {
        const auto bytes = random_bytes(count);
        ASSERT_TRUE(bytes.has_value()) << count;
        EXPECT_EQ(bytes->size(), count);
    }
}

TEST(HttpRandom, TwoDrawsDiffer) {
    const auto first = random_bytes(32);
    const auto second = random_bytes(32);
    ASSERT_TRUE(first && second);
    EXPECT_NE(*first, *second);
}

TEST(HttpRandom, ALongDrawIsNotConstant) {
    // Bytes past the first 256 come from a later call of the source: they must be random too.
    const auto bytes = random_bytes(1024);
    ASSERT_TRUE(bytes.has_value());
    std::set<char> seen(bytes->begin(), bytes->end());
    EXPECT_GT(seen.size(), 64u);  // 1024 random bytes hold about 250 distinct values
    EXPECT_NE(bytes->substr(0, 256), bytes->substr(256, 256));
}

TEST(HttpRandom, RandomHexHasTwoDigitsPerByteAndOnlyHexDigits) {
    const auto text = random_hex(32);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->size(), 64u);
    EXPECT_TRUE(std::all_of(text->begin(), text->end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
    EXPECT_NE(*random_hex(32), *random_hex(32));
}

}  // namespace
