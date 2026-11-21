// URLs and callbacks an authorization server or a browser hands the flow, at random: the reader of URLs never throws
// or reads past its text, the identity of a resource holds only for texts that are the same, and a callback yields a
// code only when its state and issuer are the ones the flow sent.
#include <gtest/gtest.h>

#include <cctype>
#include <random>
#include <string>
#include <variant>

#include "client_auth/callback.h"
#include "support.h"

namespace {

using namespace oauth_test;

constexpr std::string_view issuer = "https://as.example";
constexpr std::string_view redirect = "http://127.0.0.1:3000/callback";
constexpr std::string_view state = "StateValue0123";

std::string random_text(std::mt19937& random, std::string_view alphabet, std::size_t max_length) {
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::uniform_int_distribution<std::size_t> length(0, max_length);
    std::string out;
    const std::size_t n = length(random);
    for (std::size_t i = 0; i < n; ++i) out += alphabet[pick(random)];
    return out;
}

TEST(OAuthFlowRandomInput, TheUrlReaderTakesAnyTextAndKeepsItsPartsInsideIt) {
    const std::string alphabet = std::string("hHtTpPsS:/?#@[]%.-_~!$&'()*+,;=aA019 \\\"<>^`{|}") + "\x01\x7f\xc3\xa9" +
                                 std::string(1, '\0');
    std::mt19937 random(20261121u);
    std::size_t read_count = 0;
    for (int round = 0; round < 30000; ++round) {
        std::string text = random_text(random, alphabet, 80);
        if (round % 2 == 0) text = "https://" + text;
        const std::string_view view(text);
        const auto url = read_url(view);
        if (!url) continue;
        ++read_count;
        const auto inside = [&](std::string_view part) {
            return part.empty() || (part.data() >= view.data() && part.data() + part.size() <= view.data() + view.size());
        };
        EXPECT_TRUE(inside(url->scheme));
        EXPECT_TRUE(inside(url->authority));
        EXPECT_TRUE(inside(url->host));
        EXPECT_TRUE(inside(url->path));
        if (url->query) {
            EXPECT_TRUE(inside(*url->query));
        }
        if (url->fragment) {
            EXPECT_TRUE(inside(*url->fragment));
        }
        EXPECT_TRUE(same_resource_identifier(view, view));
        static_cast<void>(without_fragment(*url));
        static_cast<void>(is_secure_or_loopback(*url));
    }
    EXPECT_GT(read_count, 0u);
}

TEST(OAuthFlowRandomInput, AResourceIdentifierDiffersFromEveryOneCharacterChangeOutsideSchemeAndHostCase) {
    const std::string endpoint = "https://mcp.example:8443/servers/one?tenant=a";
    const std::string alphabet = "abcXYZ019/:?#%-._~";
    std::mt19937 random(7u);
    std::uniform_int_distribution<std::size_t> place(0, endpoint.size() - 1);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    // Where the scheme and host end: case there is not a difference.
    const std::size_t host_end = endpoint.find(':', std::string("https://").size());
    for (int round = 0; round < 5000; ++round) {
        std::string changed = endpoint;
        const std::size_t at = place(random);
        const char replacement = alphabet[pick(random)];
        if (changed[at] == replacement) continue;
        changed[at] = replacement;
        const bool case_only = at < host_end && std::tolower(static_cast<unsigned char>(changed[at])) ==
                                                    std::tolower(static_cast<unsigned char>(endpoint[at]));
        if (case_only) continue;
        EXPECT_FALSE(same_resource_identifier(endpoint, changed)) << changed;
    }
}

TEST(OAuthFlowRandomInput, ACallbackYieldsACodeOnlyWithTheStateAndIssuerTheFlowSent) {
    PendingAuthorization pending;
    pending.issuer = std::string(issuer);
    pending.iss_parameter_supported = true;
    pending.state = std::string(state);
    pending.redirect_uri = std::string(redirect);
    const OAuthBounds bounds;
    const std::string alphabet = "codestaiE=&%+0123456789ABCabc._-~:/?#";
    std::mt19937 random(11u);
    for (int round = 0; round < 20000; ++round) {
        std::string query = random_text(random, alphabet, 60);
        // Half the time, put a valid state and issuer somewhere in it, so both branches run.
        if (round % 2 == 0) {
            query += "&state=" + std::string(state) + "&iss=" + neomcp::http::form_encode_component(issuer);
        }
        const std::string url = std::string(redirect) + "?" + query;
        const auto result = check_callback(url, pending, bounds);
        if (!std::holds_alternative<std::string>(result)) continue;
        // A code came out: the callback must carry exactly one non-empty state and iss, equal to the flow's.
        auto decoded = neomcp::http::form_decode(query);
        ASSERT_TRUE(std::holds_alternative<std::vector<neomcp::http::FormField>>(decoded)) << url;
        const auto& fields = std::get<std::vector<neomcp::http::FormField>>(decoded);
        const auto found_state = neomcp::http::find_unique_field(fields, "state");
        const auto found_iss = neomcp::http::find_unique_field(fields, "iss");
        ASSERT_EQ(found_state.state, neomcp::http::FieldLookup::State::present) << url;
        EXPECT_EQ(found_state.value, state) << url;
        ASSERT_EQ(found_iss.state, neomcp::http::FieldLookup::State::present) << url;
        EXPECT_EQ(found_iss.value, issuer) << url;
        EXPECT_FALSE(std::get<std::string>(result).empty()) << url;
    }
}

}  // namespace
