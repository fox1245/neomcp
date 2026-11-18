// Tests of SHA-256 and HMAC-SHA-256 against the published examples, of the streaming interfaces against the one-shot
// ones, and of the comparison in constant time.
//
// SHA-256: the examples of NIST's "Cryptographic Standards and Guidelines: Examples with Intermediate Values" for
// SHA-256 (one block "abc", two blocks of 448 bits), the empty message, the 896-bit message and one million times
// "a" from the same collection of test vectors. HMAC-SHA-256: every test case of RFC 4231, section 4.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "crypto/hmac.h"
#include "crypto/sha256.h"
#include "neomcp/random.h"

namespace {

using neomcp::detail::constant_time_equal;
using neomcp::detail::hmac_sha256;
using neomcp::detail::HmacSha256;
using neomcp::detail::Sha256;
using neomcp::detail::sha256;
using neomcp::detail::Sha256Digest;

std::string hex(const Sha256Digest& digest) {
    return neomcp::hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string repeated(char c, std::size_t count) { return std::string(count, c); }

std::string bytes_from(std::initializer_list<int> values) {
    std::string out;
    for (const int value : values) out.push_back(static_cast<char>(value));
    return out;
}

TEST(Sha256, NistExamples) {
    EXPECT_EQ(hex(sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hex(sha256("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnop"
                         "qrstnopqrstu")),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, OneMillionTimesTheLetterA) {
    EXPECT_EQ(hex(sha256(repeated('a', 1000000))), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // The same fed in pieces of 1000.
    Sha256 hash;
    const std::string piece = repeated('a', 1000);
    for (int i = 0; i < 1000; ++i) hash.update(piece);
    EXPECT_EQ(hex(hash.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, LengthsAroundTheBlockBoundaryMatchOneShotAndPieces) {
    // 55, 56 and 64 bytes are the lengths where the padding needs one more block.
    for (std::size_t length = 0; length <= 3 * neomcp::detail::sha256_block_size; ++length) {
        const std::string message = repeated('x', length);
        Sha256 bytewise;
        for (const char c : message) bytewise.update(std::string_view(&c, 1));
        EXPECT_EQ(hex(bytewise.finish()), hex(sha256(message))) << length;
    }
}

TEST(Sha256, AnObjectStartsOverAfterFinish) {
    Sha256 hash;
    hash.update("something else");
    static_cast<void>(hash.finish());
    hash.update("abc");
    EXPECT_EQ(hex(hash.finish()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

struct HmacCase {
    const char* name;
    std::string key;
    std::string data;
    const char* expected;  // hexadecimal; RFC 4231 case 5 gives only the first 128 bits
};

std::vector<HmacCase> rfc4231_cases() {
    return {
        {"case 1", repeated('\x0b', 20), "Hi There", "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {"case 2", "Jefe", "what do ya want for nothing?",
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {"case 3", repeated('\xaa', 20), repeated('\xdd', 50),
         "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
        {"case 4",
         bytes_from({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
                     0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19}),
         repeated('\xcd', 50), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
        {"case 5", repeated('\x0c', 20), "Test With Truncation", "a3b6167473100ee06e0c796c2955552b"},
        {"case 6", repeated('\xaa', 131), "Test Using Larger Than Block-Size Key - Hash Key First",
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        {"case 7", repeated('\xaa', 131),
         "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be "
         "hashed before being used by the HMAC algorithm.",
         "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
    };
}

TEST(HmacSha256, EveryTestCaseOfRfc4231) {
    for (const auto& c : rfc4231_cases()) {
        const std::string full = hex(hmac_sha256(c.key, c.data));
        const std::string expected = c.expected;
        EXPECT_EQ(full.substr(0, expected.size()), expected) << c.name;
        if (expected.size() < full.size()) {
            // The truncated case: the full code is still 32 bytes.
            EXPECT_EQ(full.size(), 2 * neomcp::detail::sha256_size) << c.name;
        }
    }
}

TEST(HmacSha256, StreamingMatchesOneShotOverRandomSplits) {
    std::mt19937 generator(20261119u);  // a fixed seed: the same splits on every run
    for (const auto& c : rfc4231_cases()) {
        const Sha256Digest one_shot = hmac_sha256(c.key, c.data);
        for (int round = 0; round < 50; ++round) {
            HmacSha256 mac(c.key);
            std::size_t offset = 0;
            while (offset < c.data.size()) {
                std::uniform_int_distribution<std::size_t> step(0, c.data.size() - offset);
                const std::size_t count = step(generator);
                mac.update(std::string_view(c.data).substr(offset, count));
                offset += count;
            }
            EXPECT_EQ(hex(mac.finish()), hex(one_shot)) << c.name << " round " << round;
        }
    }
    // Long messages over several blocks, split anywhere.
    const std::string key = repeated('k', 32);
    std::string message;
    for (int i = 0; i < 1000; ++i) message.push_back(static_cast<char>(generator() & 0xFFu));
    const std::string expected = hex(hmac_sha256(key, message));
    for (int round = 0; round < 50; ++round) {
        HmacSha256 mac(key);
        std::size_t offset = 0;
        while (offset < message.size()) {
            std::uniform_int_distribution<std::size_t> step(1, 200);
            const std::size_t count = std::min(step(generator), message.size() - offset);
            mac.update(std::string_view(message).substr(offset, count));
            offset += count;
        }
        EXPECT_EQ(hex(mac.finish()), expected) << round;
    }
}

TEST(HmacSha256, AnObjectStartsOverWithTheSameKey) {
    HmacSha256 mac("Jefe");
    mac.update("something else");
    static_cast<void>(mac.finish());
    mac.update("what do ya want for nothing?");
    EXPECT_EQ(hex(mac.finish()), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(ConstantTimeEqual, ComparesEveryByte) {
    const std::string a = repeated('\x5a', 32);
    EXPECT_TRUE(constant_time_equal(a, a));
    EXPECT_TRUE(constant_time_equal("", ""));
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (const int bit : {0x01, 0x80}) {
            std::string b = a;
            b[i] = static_cast<char>(b[i] ^ bit);
            EXPECT_FALSE(constant_time_equal(a, b)) << i << " " << bit;
        }
    }
    EXPECT_FALSE(constant_time_equal(a, a.substr(1)));
    EXPECT_FALSE(constant_time_equal(a + "x", a));
}

TEST(Random, BytesAreAvailableAndDiffer) {
    const auto first = neomcp::random_bytes(32);
    const auto second = neomcp::random_bytes(32);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->size(), 32u);
    EXPECT_NE(*first, *second);
}

}  // namespace
