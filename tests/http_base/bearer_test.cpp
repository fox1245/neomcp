// The Bearer scheme as a server meets it: the credentials of an Authorization field, read by a parser of RFC 6750's
// two productions, and the Bearer challenge a server writes, which must read back through the client's reader of
// challenges to the same values.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "neomcp/http/bearer.h"
#include "neomcp/http/challenge.h"
#include "neomcp/http/headers.h"

namespace {

using namespace neomcp::http;

using Read = std::variant<BearerCredentials, CredentialsProblem>;

Read read_one(std::string_view field, const CredentialsLimits& limits = {}) {
    const std::vector<std::string_view> fields{field};
    return read_bearer_credentials(std::span<const std::string_view>(fields), limits);
}

std::string token_of(std::string_view field, const CredentialsLimits& limits = {}) {
    const Read result = read_one(field, limits);
    const auto* credentials = std::get_if<BearerCredentials>(&result);
    EXPECT_NE(credentials, nullptr) << "refused: " << field;
    return credentials != nullptr ? credentials->token : std::string();
}

CredentialsProblem problem_of(std::string_view field, const CredentialsLimits& limits = {}) {
    const Read result = read_one(field, limits);
    const auto* problem = std::get_if<CredentialsProblem>(&result);
    EXPECT_NE(problem, nullptr) << "accepted: " << field;
    return problem != nullptr ? *problem : CredentialsProblem::absent;
}

bool accepted(std::string_view field, const CredentialsLimits& limits = {}) {
    return std::holds_alternative<BearerCredentials>(read_one(field, limits));
}

constexpr std::string_view b64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/";

bool in_b64_alphabet(char c) { return b64_alphabet.find(c) != std::string_view::npos; }

// ---- The credentials

TEST(BearerCredentials, ReadsTheTokenOfTheExampleOfRfc6750) {
    EXPECT_EQ(token_of("Bearer mF_9.B5f-4.1JqM"), "mF_9.B5f-4.1JqM");
}

TEST(BearerCredentials, TheSchemeIsComparedWithoutCase) {
    for (const char* scheme : {"Bearer", "bearer", "BEARER", "bEaReR", "beARer"}) {
        EXPECT_EQ(token_of(std::string(scheme) + " abc"), "abc") << scheme;
    }
}

TEST(BearerCredentials, TheTokenKeepsItsCase) { EXPECT_EQ(token_of("bearer AbC-dEf"), "AbC-dEf"); }

TEST(BearerCredentials, OneOrMoreSpacesSeparateTheSchemeFromTheToken) {
    EXPECT_EQ(token_of("Bearer abc"), "abc");
    EXPECT_EQ(token_of("Bearer  abc"), "abc");
    EXPECT_EQ(token_of("Bearer          abc"), "abc");
}

TEST(BearerCredentials, ATabOrNothingBetweenSchemeAndTokenIsMalformed) {
    EXPECT_EQ(problem_of("Bearer\tabc"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer \tabc"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer "), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer:abc"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer=abc"), CredentialsProblem::malformed);
}

TEST(BearerCredentials, WhitespaceAroundTheFieldValueIsNotPartOfIt) {
    EXPECT_EQ(token_of("Bearer abc "), "abc");
    EXPECT_EQ(token_of("Bearer abc\t"), "abc");
    EXPECT_EQ(token_of("Bearer abc \t "), "abc");
    EXPECT_EQ(token_of(" Bearer abc"), "abc");
    EXPECT_EQ(token_of("\tBearer abc"), "abc");
    EXPECT_EQ(token_of("\t Bearer  abc \t"), "abc");
}

TEST(BearerCredentials, WhitespaceInsideTheTokenIsMalformed) {
    EXPECT_EQ(problem_of("Bearer ab cd"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer ab\tcd"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer abc d"), CredentialsProblem::malformed);
}

TEST(BearerCredentials, PaddingIsAllowedOnlyAtTheEnd) {
    EXPECT_EQ(token_of("Bearer abc="), "abc=");
    EXPECT_EQ(token_of("Bearer abc=="), "abc==");
    EXPECT_EQ(token_of("Bearer abc==="), "abc===");
    EXPECT_EQ(problem_of("Bearer ab=c"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer ab==c="), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer =abc"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer ="), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer =="), CredentialsProblem::malformed);
}

TEST(BearerCredentials, EveryCharacterOfTheAlphabetIsAccepted) {
    EXPECT_EQ(token_of("Bearer " + std::string(b64_alphabet)), std::string(b64_alphabet));
    for (const char c : b64_alphabet) {
        const std::string one(1, c);
        EXPECT_EQ(token_of("Bearer a" + one + "b"), "a" + one + "b");
    }
}

TEST(BearerCredentials, NoOtherByteIsAcceptedInTheToken) {
    for (int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        // The whitespace bytes are the field's own whitespace and the padding is allowed at the end: both have tests.
        if (in_b64_alphabet(c) || c == ' ' || c == '\t' || c == '=') continue;
        const std::string one(1, c);
        // Alone, at the start, in the middle and at the end of the token.
        for (const std::string& token : {one, one + "ab", "ab" + one + "cd", "ab" + one}) {
            EXPECT_FALSE(accepted("Bearer " + token)) << "byte " << byte << " in " << token.size() << " bytes";
        }
    }
}

TEST(BearerCredentials, ANonAsciiByteIsNotPartOfAToken) {
    EXPECT_EQ(problem_of("Bearer abc\xc3\xa9"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer \xff"), CredentialsProblem::malformed);
}

TEST(BearerCredentials, ControlBytesAndLineBreaksAreMalformed) {
    EXPECT_EQ(problem_of(std::string("Bearer ab\0cd", 12)), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer ab\r\nX: 1"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer ab\ncd"), CredentialsProblem::malformed);
}

TEST(BearerCredentials, AnEmptyTokenIsMalformed) {
    EXPECT_EQ(problem_of("Bearer "), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer   "), CredentialsProblem::malformed);
}

TEST(BearerCredentials, ACommaListInOneFieldIsMalformed) {
    EXPECT_EQ(problem_of("Bearer abc, Bearer def"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer abc,def"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer abc, Basic dXNlcjpwYXNz"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("Bearer token=\"abc\""), CredentialsProblem::malformed);
}

TEST(BearerCredentials, AnotherSchemeIsNotBearerCredentialsAndItsRestIsNotRead) {
    EXPECT_EQ(problem_of("Basic dXNlcjpwYXNz"), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("Digest username=\"a\", realm=\"b\""), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("Basic"), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("Basic \x01\x02"), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("Negotiate abc"), CredentialsProblem::other_scheme);
}

TEST(BearerCredentials, ASchemeThatOnlyStartsOrEndsLikeBearerIsAnotherScheme) {
    EXPECT_EQ(problem_of("Bearerx abc"), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("xBearer abc"), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("Bearer-2 abc"), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("Bear abc"), CredentialsProblem::other_scheme);
    EXPECT_EQ(problem_of("Bearer.v2 abc"), CredentialsProblem::other_scheme);
}

TEST(BearerCredentials, AValueWithoutASchemeIsMalformed) {
    EXPECT_EQ(problem_of(""), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("   "), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("\"Bearer\" abc"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of(",Bearer abc"), CredentialsProblem::malformed);
    EXPECT_EQ(problem_of("abc"), CredentialsProblem::other_scheme);  // a lone token is a scheme with nothing after it
}

TEST(BearerCredentials, NoFieldIsAbsent) {
    const std::vector<std::string_view> none;
    const Read result = read_bearer_credentials(std::span<const std::string_view>(none));
    ASSERT_TRUE(std::holds_alternative<CredentialsProblem>(result));
    EXPECT_EQ(std::get<CredentialsProblem>(result), CredentialsProblem::absent);
}

TEST(BearerCredentials, TwoFieldsAreRefusedWhateverTheyHold) {
    const auto refused = [](std::vector<std::string_view> fields) {
        const Read result = read_bearer_credentials(std::span<const std::string_view>(fields));
        return std::holds_alternative<CredentialsProblem>(result) &&
               std::get<CredentialsProblem>(result) == CredentialsProblem::repeated;
    };
    EXPECT_TRUE(refused({"Bearer abc", "Bearer abc"}));
    EXPECT_TRUE(refused({"Bearer abc", "Bearer def"}));
    EXPECT_TRUE(refused({"Bearer abc", "Basic dXNlcjpwYXNz"}));
    EXPECT_TRUE(refused({"Basic dXNlcjpwYXNz", "Bearer abc"}));
    EXPECT_TRUE(refused({"Basic a", "Basic b"}));
    EXPECT_TRUE(refused({"Bearer abc", ""}));
    EXPECT_TRUE(refused({"Bearer abc", "Bearer abc", "Bearer abc"}));
}

// ---- The size bound

TEST(BearerCredentialsBound, ATokenAtTheBoundIsAcceptedOneUnderToo) {
    CredentialsLimits limits;
    limits.max_token_bytes = 64;
    EXPECT_EQ(token_of("Bearer " + std::string(63, 'a'), limits).size(), 63u);
    EXPECT_EQ(token_of("Bearer " + std::string(64, 'a'), limits).size(), 64u);
}

TEST(BearerCredentialsBound, ATokenOneOverTheBoundIsTooLarge) {
    CredentialsLimits limits;
    limits.max_token_bytes = 64;
    EXPECT_EQ(problem_of("Bearer " + std::string(65, 'a'), limits), CredentialsProblem::token_too_large);
    EXPECT_EQ(problem_of("Bearer " + std::string(4096, 'a'), limits), CredentialsProblem::token_too_large);
}

TEST(BearerCredentialsBound, ThePaddingCountsAndTheWhitespaceDoesNot) {
    CredentialsLimits limits;
    limits.max_token_bytes = 8;
    EXPECT_EQ(token_of("Bearer aaaaaaa=", limits), "aaaaaaa=");
    EXPECT_EQ(problem_of("Bearer aaaaaaa==", limits), CredentialsProblem::token_too_large);
    EXPECT_EQ(token_of("Bearer     aaaaaaaa      ", limits), "aaaaaaaa");
}

TEST(BearerCredentialsBound, AMalformedTokenIsMalformedWhateverItsSize) {
    CredentialsLimits limits;
    limits.max_token_bytes = 8;
    EXPECT_EQ(problem_of("Bearer " + std::string(100, 'a') + "!", limits), CredentialsProblem::malformed);
}

TEST(BearerCredentialsBound, TheDefaultIsSixteenKibibytes) {
    EXPECT_EQ(CredentialsLimits{}.max_token_bytes, std::size_t{16} * 1024);
    const std::size_t bound = CredentialsLimits{}.max_token_bytes;
    EXPECT_EQ(token_of("Bearer " + std::string(bound - 1, 'x')).size(), bound - 1);
    EXPECT_EQ(token_of("Bearer " + std::string(bound, 'x')).size(), bound);
    EXPECT_EQ(problem_of("Bearer " + std::string(bound + 1, 'x')), CredentialsProblem::token_too_large);
}

TEST(BearerCredentialsBound, AZeroBoundRefusesEveryToken) {
    CredentialsLimits limits;
    limits.max_token_bytes = 0;
    EXPECT_EQ(problem_of("Bearer a", limits), CredentialsProblem::token_too_large);
}

// ---- The header collection

TEST(BearerHeaders, ReadsTheFieldByNameWithoutCase) {
    Headers headers;
    headers.add("Host", "example.test");
    headers.add("aUtHoRiZaTiOn", "Bearer abc");
    const Read result = read_bearer_credentials(headers);
    ASSERT_TRUE(std::holds_alternative<BearerCredentials>(result));
    EXPECT_EQ(std::get<BearerCredentials>(result).token, "abc");
}

TEST(BearerHeaders, NoAuthorizationFieldIsAbsent) {
    Headers headers;
    headers.add("Host", "example.test");
    headers.add("X-Authorization", "Bearer abc");
    const Read result = read_bearer_credentials(headers);
    ASSERT_TRUE(std::holds_alternative<CredentialsProblem>(result));
    EXPECT_EQ(std::get<CredentialsProblem>(result), CredentialsProblem::absent);
}

TEST(BearerHeaders, TwoFieldsOfTheSameNameAreRepeated) {
    Headers headers;
    headers.add("Authorization", "Bearer abc");
    headers.add("authorization", "Bearer abc");
    const Read result = read_bearer_credentials(headers);
    ASSERT_TRUE(std::holds_alternative<CredentialsProblem>(result));
    EXPECT_EQ(std::get<CredentialsProblem>(result), CredentialsProblem::repeated);
}

TEST(BearerHeaders, TheProblemsHaveNames) {
    for (const auto problem : {CredentialsProblem::absent, CredentialsProblem::other_scheme,
                               CredentialsProblem::repeated, CredentialsProblem::malformed,
                               CredentialsProblem::token_too_large}) {
        EXPECT_FALSE(to_string(problem).empty());
    }
}

// ---- Random input

// A reader of the same two productions, written apart from the one under test: from the end of the value inwards.
bool oracle_accepts(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (value.size() < 6) return false;
    const std::string_view head = value.substr(0, 6);
    std::string lower;
    for (const char c : head) lower += static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    if (lower != "bearer") return false;
    std::string_view tail = value.substr(6);
    if (tail.empty() || tail.front() != ' ') return false;
    while (!tail.empty() && tail.front() == ' ') tail.remove_prefix(1);
    while (!tail.empty() && tail.back() == '=') tail.remove_suffix(1);
    if (tail.empty()) return false;
    for (const char c : tail) {
        if (!in_b64_alphabet(c)) return false;
    }
    return true;
}

TEST(BearerRandom, ValidFieldsAreReadBackToTheirToken) {
    std::mt19937_64 rng(0xB3A8E5);
    for (int round = 0; round < 3000; ++round) {
        std::string token;
        const std::size_t length = 1 + rng() % 200;
        for (std::size_t i = 0; i < length; ++i) token += b64_alphabet[rng() % b64_alphabet.size()];
        token.append(rng() % 4, '=');
        std::string scheme = "bearer";
        for (char& c : scheme) {
            if (rng() % 2 == 0) c = static_cast<char>(c - 'a' + 'A');
        }
        const auto pad = [&](bool required) {
            std::string out;
            const std::size_t count = (required ? 1 : 0) + rng() % 4;
            for (std::size_t i = 0; i < count; ++i) out += (required || rng() % 2 == 0) ? ' ' : '\t';
            return out;
        };
        // SP between scheme and token; SP and HTAB around the whole value.
        std::string separator(1 + rng() % 4, ' ');
        const std::string field = pad(false) + scheme + separator + token + pad(false);
        ASSERT_EQ(token_of(field), token) << field;
    }
}

TEST(BearerRandom, AnyBytesAgreeWithTheOracleAndNeverMisbehave) {
    std::mt19937_64 rng(0x5EEDF00D);
    // A small alphabet makes both the accepted and the refused forms common.
    const std::string alphabet = "Bbeearr \t=abXY09-._~+/\",:\\\r\n\x01\x80\xff";
    int accepted_count = 0;
    for (int round = 0; round < 40000; ++round) {
        std::string field;
        if (rng() % 3 == 0) field = "Bearer ";
        const std::size_t longest = rng() % 2 == 0 ? 6 : 24;
        const std::size_t length = rng() % longest;
        for (std::size_t i = 0; i < length; ++i) field += alphabet[rng() % alphabet.size()];
        const Read result = read_one(field);
        const bool got = std::holds_alternative<BearerCredentials>(result);
        ASSERT_EQ(got, oracle_accepts(field)) << "round " << round;
        if (got) {
            ++accepted_count;
            const std::string& token = std::get<BearerCredentials>(result).token;
            ASSERT_FALSE(token.empty());
            ASSERT_NE(token.front(), '=');
            ASSERT_EQ(token.find_first_of(" \t\r\n,\"\\"), std::string::npos);
        }
    }
    EXPECT_GT(accepted_count, 100);  // the generator reaches the accepted forms
}

TEST(BearerRandom, RandomBytesNeverThrowOrReadPastTheField) {
    std::mt19937_64 rng(0xD00D);
    for (int round = 0; round < 5000; ++round) {
        std::string field;
        const std::size_t length = rng() % 64;
        for (std::size_t i = 0; i < length; ++i) field += static_cast<char>(rng() & 0xff);
        // Held in a buffer of exactly its size: a read past the end is seen by a sanitizer.
        const std::vector<char> exact(field.begin(), field.end());
        const std::string_view view(exact.data(), exact.size());
        EXPECT_NO_THROW(static_cast<void>(read_one(view)));
    }
}

// ---- Scope values

TEST(BearerScopeToken, AcceptsTheCharactersOfRfc6749) {
    for (const char* value : {"files", "files:read", "urn:example:channel=HBO&urn:example:rating=G,PG-13", "a", "!",
                              "~", "https://example.test/scope", "openid", "a.b-c_d~e"}) {
        EXPECT_TRUE(is_scope_token(value)) << value;
    }
}

TEST(BearerScopeToken, RefusesAnEmptyTextAndTheCharactersOutsideTheSet) {
    EXPECT_FALSE(is_scope_token(""));
    for (int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        const bool allowed = byte == 0x21 || (byte >= 0x23 && byte <= 0x5b) || (byte >= 0x5d && byte <= 0x7e);
        const std::string one(1, c);
        EXPECT_EQ(is_scope_token(one), allowed) << byte;
        EXPECT_EQ(is_scope_token("ab" + one + "cd"), allowed) << byte;
    }
}

TEST(BearerScopeToken, ReadsNoMeaningFromTheText) {
    for (const char* value : {"files:*", "*", "..", "files/../x", "offline_access"}) {
        EXPECT_TRUE(is_scope_token(value)) << value;
    }
}

// ---- Writing a challenge

struct Written {
    BearerChallenge bearer;
    std::string text;
};

Written written(const BearerChallengeParts& parts) {
    auto result = write_bearer_challenge(parts);
    auto* text = std::get_if<std::string>(&result);
    EXPECT_NE(text, nullptr) << "refused";
    if (text == nullptr) return {};
    auto parsed = parse_challenges(std::string_view(*text));
    auto* challenges = std::get_if<std::vector<Challenge>>(&parsed);
    EXPECT_NE(challenges, nullptr) << "unreadable: " << *text;
    if (challenges == nullptr) return {};
    EXPECT_EQ(challenges->size(), 1u) << *text;
    auto found = find_bearer_challenge(*challenges);
    auto* bearer = std::get_if<BearerChallenge>(&found);
    EXPECT_NE(bearer, nullptr) << "no Bearer challenge in: " << *text;
    if (bearer == nullptr) return {};
    return {*bearer, *text};
}

ChallengeWriteProblem refused(const BearerChallengeParts& parts) {
    auto result = write_bearer_challenge(parts);
    auto* problem = std::get_if<ChallengeWriteProblem>(&result);
    EXPECT_NE(problem, nullptr) << "written: " << std::get<std::string>(result);
    return problem != nullptr ? *problem : ChallengeWriteProblem::invalid_error;
}

TEST(BearerChallenge, WritesJustTheSchemeWhenThereIsNothingToSay) {
    EXPECT_EQ(written({}).text, "Bearer");
}

TEST(BearerChallenge, WritesTheNoCredentialsForm) {
    BearerChallengeParts parts;
    parts.resource_metadata = "https://example.test/.well-known/oauth-protected-resource/mcp";
    parts.scopes = {"files:read", "files:write"};
    const Written out = written(parts);
    EXPECT_EQ(out.text,
              "Bearer scope=\"files:read files:write\", "
              "resource_metadata=\"https://example.test/.well-known/oauth-protected-resource/mcp\"");
    EXPECT_EQ(out.bearer.error_kind, BearerError::none);
    EXPECT_EQ(out.bearer.scopes(), (std::vector<std::string>{"files:read", "files:write"}));
    EXPECT_EQ(out.bearer.resource_metadata, parts.resource_metadata);
}

TEST(BearerChallenge, WritesTheInsufficientScopeForm) {
    BearerChallengeParts parts;
    parts.error = std::string(bearer_error_insufficient_scope);
    parts.scopes = {"admin"};
    parts.resource_metadata = "http://127.0.0.1:8080/.well-known/oauth-protected-resource/mcp";
    const Written out = written(parts);
    EXPECT_EQ(out.text,
              "Bearer error=\"insufficient_scope\", scope=\"admin\", "
              "resource_metadata=\"http://127.0.0.1:8080/.well-known/oauth-protected-resource/mcp\"");
    EXPECT_EQ(out.bearer.error_kind, BearerError::insufficient_scope);
}

TEST(BearerChallenge, TheThreeErrorCodesReadBackAsTheirKinds) {
    for (const auto& [code, kind] : {std::pair{bearer_error_invalid_request, BearerError::invalid_request},
                                     std::pair{bearer_error_invalid_token, BearerError::invalid_token},
                                     std::pair{bearer_error_insufficient_scope, BearerError::insufficient_scope}}) {
        BearerChallengeParts parts;
        parts.error = std::string(code);
        EXPECT_EQ(written(parts).bearer.error_kind, kind) << code;
    }
}

TEST(BearerChallenge, AQuoteOrABackslashInAnyValueIsRefusedNotWritten) {
    // RFC 6750, section 3: no parameter value holds the quote or the backslash.
    for (const char* text : {"say \"hi\"", "back\\slash", "\"", "\\", "x\", error=\"invalid_token"}) {
        SCOPED_TRACE(text);
        BearerChallengeParts description;
        description.error_description = text;
        EXPECT_EQ(refused(description), ChallengeWriteProblem::invalid_error_description);
        BearerChallengeParts error;
        error.error = text;
        EXPECT_EQ(refused(error), ChallengeWriteProblem::invalid_error);
        BearerChallengeParts url;
        url.resource_metadata = text;
        EXPECT_EQ(refused(url), ChallengeWriteProblem::invalid_resource_metadata);
        BearerChallengeParts scope;
        scope.scopes = {text};
        EXPECT_EQ(refused(scope), ChallengeWriteProblem::invalid_scope);
    }
}

TEST(BearerChallenge, ValuesThatCannotBeWrittenAreRefused) {
    BearerChallengeParts parts;
    parts.resource_metadata = "";
    EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_resource_metadata);
    for (const char* value :
         {"https://x/a b", "https://x/\"", "https://x/\\", "https://x/\r\nX: 1", "https://x/\xc3\xa9", "https://x/\t"}) {
        parts.resource_metadata = value;
        EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_resource_metadata) << value;
    }
    parts.resource_metadata = std::string("https://x/\0y", 12);
    EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_resource_metadata);

    parts = {};
    parts.error = "";
    EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_error);
    for (const char* value : {"a\"b", "a\\b", "a\tb", "a\nb", "a\xc3\xa9"}) {
        parts.error = value;
        EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_error) << value;
    }

    parts = {};
    parts.error_description = "";
    EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_error_description);
    for (const char* value : {"a\tb", "a\r\nb", "a\x01", "a\x7f", "caf\xc3\xa9", "a\"b", "a\\b"}) {
        parts.error_description = value;
        EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_error_description) << value;
    }

    parts = {};
    for (const char* value : {"", "a b", "a\"b", "a\\b", "a\tb", "a\xc3\xa9", "a,b\n"}) {
        parts.scopes = {"ok", value};
        EXPECT_EQ(refused(parts), ChallengeWriteProblem::invalid_scope) << value;
    }
}

TEST(BearerChallenge, ARefusedValueWritesNothingAtAll) {
    BearerChallengeParts parts;
    parts.error = "invalid_token";
    parts.scopes = {"ok"};
    parts.resource_metadata = "https://x/bad\"url";
    EXPECT_TRUE(std::holds_alternative<ChallengeWriteProblem>(write_bearer_challenge(parts)));
}

TEST(BearerChallenge, EveryValueMayHoldItsWholeCharacterSet) {
    std::string error;  // %x20-21 / %x23-5B / %x5D-7E
    for (int byte = 0x20; byte <= 0x7e; ++byte) {
        if (byte != '"' && byte != '\\') error += static_cast<char>(byte);
    }
    BearerChallengeParts parts;
    parts.error = error;
    parts.error_description = error;
    const Written all = written(parts);
    EXPECT_EQ(all.bearer.error, error);
    EXPECT_EQ(all.bearer.error_description, error);

    std::string scope;
    for (int byte = 0x21; byte <= 0x7e; ++byte) {
        if (byte != '"' && byte != '\\') scope += static_cast<char>(byte);
    }
    parts = {};
    parts.scopes = {scope};
    parts.resource_metadata = scope;
    const Written others = written(parts);
    EXPECT_EQ(others.bearer.scope, scope);
    EXPECT_EQ(others.bearer.resource_metadata, scope);
}

TEST(BearerChallenge, ScopesAreJoinedByOneSpaceInOrderAndDuplicatesAreKept) {
    BearerChallengeParts parts;
    parts.scopes = {"b", "a", "b"};
    const Written out = written(parts);
    EXPECT_EQ(out.bearer.scope, "b a b");
    EXPECT_EQ(out.bearer.scopes(), (std::vector<std::string>{"b", "a", "b"}));
}

TEST(BearerChallenge, NoScopeValuesLeavesTheParameterOut) {
    BearerChallengeParts parts;
    parts.resource_metadata = "https://x/m";
    const Written out = written(parts);
    EXPECT_FALSE(out.bearer.scope.has_value());
    EXPECT_EQ(out.text.find("scope"), std::string::npos);
}

TEST(BearerChallenge, NoParameterIsWrittenTwice) {
    BearerChallengeParts parts;
    parts.error = "invalid_token";
    parts.error_description = "d";
    parts.scopes = {"s"};
    parts.resource_metadata = "https://x/m";
    const Written out = written(parts);
    auto parsed = parse_challenges(std::string_view(out.text));
    const auto& challenges = std::get<std::vector<Challenge>>(parsed);
    ASSERT_EQ(challenges.size(), 1u);
    EXPECT_FALSE(challenges[0].repeated_parameter);
    EXPECT_EQ(challenges[0].params.size(), 4u);
}

TEST(BearerChallenge, TheProblemsHaveNames) {
    for (const auto problem :
         {ChallengeWriteProblem::invalid_resource_metadata, ChallengeWriteProblem::invalid_scope,
          ChallengeWriteProblem::invalid_error, ChallengeWriteProblem::invalid_error_description}) {
        EXPECT_FALSE(to_string(problem).empty());
    }
}

// Random text from a character set; the quote and backslash are common in the wide sets.
std::string random_text(std::mt19937_64& rng, std::string_view alphabet, std::size_t max_length) {
    std::string out;
    const std::size_t length = 1 + rng() % max_length;
    for (std::size_t i = 0; i < length; ++i) out += alphabet[rng() % alphabet.size()];
    return out;
}

TEST(BearerChallengeRandom, EveryWrittenChallengeReadsBackToTheSameValues) {
    std::mt19937_64 rng(0xC0FFEE);
    std::string plain;  // the set of a URI reference, error and scope tokens: no space, quote or backslash
    for (int byte = 0x21; byte <= 0x7e; ++byte) {
        if (byte != '"' && byte != '\\') plain += static_cast<char>(byte);
    }
    std::string plain_or_space = plain + ' ';
    for (int round = 0; round < 4000; ++round) {
        BearerChallengeParts parts;
        if (rng() % 2 == 0) parts.resource_metadata = random_text(rng, plain, 80);
        if (rng() % 2 == 0) parts.error = random_text(rng, plain_or_space, 24);
        if (rng() % 2 == 0) parts.error_description = random_text(rng, plain_or_space, 60);
        const std::size_t scope_count = rng() % 5;
        for (std::size_t i = 0; i < scope_count; ++i) parts.scopes.push_back(random_text(rng, plain, 20));

        const Written out = written(parts);
        ASSERT_EQ(out.bearer.resource_metadata, parts.resource_metadata) << out.text;
        ASSERT_EQ(out.bearer.error, parts.error) << out.text;
        ASSERT_EQ(out.bearer.error_description, parts.error_description) << out.text;
        ASSERT_EQ(out.bearer.scopes(), parts.scopes) << out.text;
    }
}

TEST(BearerChallengeRandom, AValueOutsideItsSetIsRefusedAndNothingIsWritten) {
    std::mt19937_64 rng(0xBADC0DE);
    for (int round = 0; round < 3000; ++round) {
        const char bad = static_cast<char>(rng() & 0xff);
        std::string text = random_text(rng, "abcXYZ019-._~", 12);
        text.insert(rng() % (text.size() + 1), 1, bad);
        const auto b = static_cast<unsigned char>(bad);
        const bool visible = b >= 0x21 && b <= 0x7e && bad != '"' && bad != '\\';

        BearerChallengeParts url;
        url.resource_metadata = text;
        EXPECT_EQ(std::holds_alternative<std::string>(write_bearer_challenge(url)), visible) << int(b);

        BearerChallengeParts scope;
        scope.scopes = {text};
        EXPECT_EQ(std::holds_alternative<std::string>(write_bearer_challenge(scope)), visible) << int(b);

        BearerChallengeParts error;
        error.error = text;
        EXPECT_EQ(std::holds_alternative<std::string>(write_bearer_challenge(error)), visible || bad == ' ') << int(b);

        BearerChallengeParts description;
        description.error_description = text;
        EXPECT_EQ(std::holds_alternative<std::string>(write_bearer_challenge(description)), visible || bad == ' ')
            << int(b);
    }
}

}  // namespace
