// The table of fixed bearer tokens on its own: what FixedTokenTable::create refuses (each problem with the entry at
// fault), its bounds at the edge and one past it, what a lookup finds, that the token strings given are overwritten,
// and that a lookup compares the digest of every entry whatever matches.
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crypto/sha256.h"
#include "http/fixed_tokens.h"
#include "neomcp/http/fixed_tokens.h"

namespace {

using neomcp::http::FixedToken;
using neomcp::http::FixedTokenBounds;
using neomcp::http::FixedTokenDefaults;
using neomcp::http::FixedTokenError;
using neomcp::http::FixedTokenProblem;
using neomcp::http::FixedTokenTable;

FixedToken entry(std::string token, std::string subject = "alice", std::vector<std::string> scopes = {"mcp:use"}) {
    return FixedToken{std::move(token), std::move(subject), std::move(scopes), std::nullopt};
}

FixedTokenTable made(std::vector<FixedToken> tokens, const FixedTokenBounds& bounds = {}) {
    auto result = FixedTokenTable::create(std::move(tokens), bounds);
    if (const auto* error = std::get_if<FixedTokenError>(&result)) {
        ADD_FAILURE() << "refused: " << neomcp::http::to_string(error->problem) << " at " << error->index;
        return FixedTokenTable();
    }
    return std::get<FixedTokenTable>(std::move(result));
}

std::optional<FixedTokenError> refusal(std::vector<FixedToken> tokens, const FixedTokenBounds& bounds = {}) {
    auto result = FixedTokenTable::create(std::move(tokens), bounds);
    if (const auto* error = std::get_if<FixedTokenError>(&result)) return *error;
    return std::nullopt;
}

void expect_refused(std::vector<FixedToken> tokens, FixedTokenProblem problem, std::size_t index,
                    const FixedTokenBounds& bounds = {}) {
    const auto error = refusal(std::move(tokens), bounds);
    ASSERT_TRUE(error) << "accepted, expected " << neomcp::http::to_string(problem);
    EXPECT_EQ(error->problem, problem) << neomcp::http::to_string(error->problem);
    EXPECT_EQ(error->index, index);
}

TEST(FixedTokensTable, ALookupFindsTheGrantsOfItsEntryAndNothingForAnyOtherText) {
    const auto expiry = std::chrono::system_clock::now() + std::chrono::hours(1);
    std::vector<FixedToken> tokens{entry("first-token", "alice", {"mcp:use", "files:read"}),
                                   FixedToken{"second.token~+/==", "bob", {}, expiry}};
    const FixedTokenTable table = made(std::move(tokens));
    ASSERT_EQ(table.size(), 2u);
    const auto alice = table.find("first-token");
    ASSERT_TRUE(alice);
    EXPECT_TRUE(alice->issuer.empty());
    EXPECT_EQ(alice->subject, "alice");
    EXPECT_EQ(alice->scopes, (std::vector<std::string>{"mcp:use", "files:read"}));
    EXPECT_TRUE(alice->audiences.empty());
    EXPECT_FALSE(alice->expires);
    EXPECT_FALSE(alice->client_id);
    const auto bob = table.find("second.token~+/==");
    ASSERT_TRUE(bob);
    EXPECT_EQ(bob->subject, "bob");
    EXPECT_EQ(bob->expires, expiry);
    // The same object for every lookup of one entry.
    EXPECT_EQ(table.find("first-token").get(), alice.get());
    // Texts that are near an entry's are not it.
    for (const char* other : {"first-toke", "first-tokenx", "First-token", "", "second.token~+/=", "first-token "}) {
        EXPECT_FALSE(table.find(other)) << other;
    }
    EXPECT_FALSE(FixedTokenTable().find("first-token"));
    EXPECT_EQ(FixedTokenTable().size(), 0u);
}

TEST(FixedTokensTable, EveryEntryThatCannotBeServedIsRefusedWithItsKindAndPlace) {
    expect_refused({entry("good"), entry("")}, FixedTokenProblem::empty_token, 1);
    for (const char* malformed : {"has space", " lead", "trail ", "tab\there", "quote\"", "comma,", "=start", "a=b",
                                  "semi;", "\xc3\xa9", "new\nline"}) {
        expect_refused({entry(malformed)}, FixedTokenProblem::malformed_token, 0);
    }
    expect_refused({entry("a"), entry("b"), entry("a", "carol")}, FixedTokenProblem::duplicate_token, 2);
    expect_refused({entry("a"), entry("b", "")}, FixedTokenProblem::empty_subject, 1);
    for (const char* scope : {"", "has space", "quote\"", "back\\slash", "\x7f", "\xc3\xa9"}) {
        expect_refused({entry("a", "alice", {"mcp:use", scope})}, FixedTokenProblem::invalid_scope, 0);
    }
    expect_refused({entry("a")}, FixedTokenProblem::invalid_bounds, 0, FixedTokenBounds{0, 1});
    expect_refused({entry("a")}, FixedTokenProblem::invalid_bounds, 0, FixedTokenBounds{1, 0});
    // The first problem in order: the bounds, the count, then the entries one by one.
    expect_refused({entry(""), entry("x y")}, FixedTokenProblem::empty_token, 0);
    expect_refused({entry(""), entry("")}, FixedTokenProblem::too_many_tokens, 0, FixedTokenBounds{1, 1});
    for (FixedTokenProblem problem :
         {FixedTokenProblem::invalid_bounds, FixedTokenProblem::too_many_tokens, FixedTokenProblem::empty_token,
          FixedTokenProblem::malformed_token, FixedTokenProblem::token_too_large, FixedTokenProblem::duplicate_token,
          FixedTokenProblem::empty_subject, FixedTokenProblem::too_many_scopes, FixedTokenProblem::invalid_scope}) {
        EXPECT_FALSE(neomcp::http::to_string(problem).empty());
    }
}

TEST(FixedTokensTable, TheBoundsHoldAtTheirEdgeAndRefuseOnePast) {
    // Tokens per table.
    const auto tokens_of = [](std::size_t count) {
        std::vector<FixedToken> tokens;
        for (std::size_t at = 0; at < count; ++at) tokens.push_back(entry("token-" + std::to_string(at)));
        return tokens;
    };
    EXPECT_EQ(made(tokens_of(3), FixedTokenBounds{3, 1}).size(), 3u);
    expect_refused(tokens_of(4), FixedTokenProblem::too_many_tokens, 0, FixedTokenBounds{3, 1});
    EXPECT_EQ(made(tokens_of(FixedTokenDefaults::max_tokens)).size(), FixedTokenDefaults::max_tokens);
    expect_refused(tokens_of(FixedTokenDefaults::max_tokens + 1), FixedTokenProblem::too_many_tokens, 0);
    // Scopes per entry.
    const auto scopes_of = [](std::size_t count) {
        std::vector<std::string> scopes;
        for (std::size_t at = 0; at < count; ++at) scopes.push_back("scope:" + std::to_string(at));
        return scopes;
    };
    EXPECT_EQ(made({entry("a", "alice", scopes_of(2))}, FixedTokenBounds{1, 2}).size(), 1u);
    expect_refused({entry("a", "alice", scopes_of(3))}, FixedTokenProblem::too_many_scopes, 0, FixedTokenBounds{1, 2});
    EXPECT_EQ(made({entry("a", "alice", scopes_of(FixedTokenDefaults::max_scopes))}).size(), 1u);
    expect_refused({entry("a", "alice", scopes_of(FixedTokenDefaults::max_scopes + 1))},
                   FixedTokenProblem::too_many_scopes, 0);
    // The bytes of a token: what the listener lets through.
    const std::string longest(FixedTokenDefaults::max_token_bytes, 'a');
    const FixedTokenTable edge = made({entry(longest)});
    EXPECT_TRUE(edge.find(longest));
    expect_refused({entry(longest + "a")}, FixedTokenProblem::token_too_large, 0);
}

TEST(FixedTokensTable, TheTokenStringsGivenAreOverwrittenWhetherTheTableIsMadeOrRefused) {
    const auto zeros = [](const std::string& text) {
        return !text.empty() && text.find_first_not_of('\0') == std::string::npos;
    };
    const std::string first(64, 'k');
    const std::string second = "short";
    std::vector<FixedToken> given{entry(first), entry(second, "bob")};
    auto result = FixedTokenTable::create(std::move(given));
    ASSERT_TRUE(std::holds_alternative<FixedTokenTable>(result));
    EXPECT_TRUE(std::get<FixedTokenTable>(result).find(first));
    EXPECT_TRUE(std::get<FixedTokenTable>(result).find(second));
    ASSERT_EQ(given.size(), 2u);
    EXPECT_TRUE(zeros(given[0].token));
    EXPECT_EQ(given[0].token.size(), first.size());
    EXPECT_TRUE(zeros(given[1].token));

    // Refused at the second entry: the first, already digested, and the third, never looked at, are overwritten too.
    std::vector<FixedToken> refused{entry(first), entry("no good"), entry(second)};
    ASSERT_FALSE(std::holds_alternative<FixedTokenTable>(FixedTokenTable::create(std::move(refused))));
    for (const FixedToken& one : refused) EXPECT_TRUE(zeros(one.token));
}

// A comparison that counts, in place of the library's comparison in constant time.
struct Counting {
    std::size_t* calls;
    bool operator()(const neomcp::detail::Sha256Digest& left, const neomcp::detail::Sha256Digest& right) const {
        ++*calls;
        return left == right;
    }
};

TEST(FixedTokensTable, ALookupComparesEveryEntryOnceWhereverItMatchesOrWhenNoneDoes) {
    std::vector<neomcp::detail::Sha256Digest> digests;
    for (int at = 0; at < 7; ++at) digests.push_back(neomcp::detail::sha256("token-" + std::to_string(at)));
    for (std::size_t wanted = 0; wanted < digests.size(); ++wanted) {
        std::size_t calls = 0;
        EXPECT_EQ(neomcp::http::detail::scan_digests(digests, digests[wanted], Counting{&calls}), wanted);
        EXPECT_EQ(calls, digests.size()) << "the match at " << wanted << " ended the scan";
    }
    std::size_t calls = 0;
    EXPECT_EQ(neomcp::http::detail::scan_digests(digests, neomcp::detail::sha256("absent"), Counting{&calls}),
              digests.size());
    EXPECT_EQ(calls, digests.size());
    calls = 0;
    EXPECT_EQ(neomcp::http::detail::scan_digests({}, digests.front(), Counting{&calls}), 0u);
    EXPECT_EQ(calls, 0u);
}

}  // namespace
