// The reading of an introspection answer: which statuses and bodies give which verdict, member by member, and what an
// active answer becomes. No socket is opened.
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "server_auth_introspection/answer.h"
#include "support.h"

namespace {

using namespace introspection_test;
using neomcp::http::detail::IntrospectionContext;
using neomcp::http::detail::IntrospectionReading;
using neomcp::http::detail::read_introspection_answer;
using SystemClock = std::chrono::system_clock;

const SystemClock::time_point now = SystemClock::time_point(std::chrono::seconds(1'700'000'000));
constexpr std::chrono::seconds lifetime{45};
constexpr std::size_t bound = 4096;

IntrospectionContext context(std::size_t max_bytes = bound) {
    IntrospectionContext value;
    value.issuer = issuer;
    value.now = now;
    value.default_lifetime = lifetime;
    value.max_bytes = max_bytes;
    return value;
}

IntrospectionReading read_text(const std::string& body, int status = 200) {
    return read_introspection_answer(status, body, context());
}

IntrospectionReading read(const json& body) { return read_text(body.dump()); }

// An active answer with one member changed (or added).
json active_with(const std::string& name, json value) {
    json answer = active_answer();
    answer[name] = std::move(value);
    return answer;
}

json active_without(const std::string& name) {
    json answer = active_answer();
    answer.erase(name);
    return answer;
}

void expect_refusal(const IntrospectionReading& reading, TokenRefusal refusal, IntrospectionEvent event,
                    const std::string& what) {
    EXPECT_TRUE(is_refusal(reading.verdict, refusal)) << what;
    EXPECT_EQ(reading.event, event) << what << ": " << neomcp::http::to_string(reading.event);
}

TEST(IntrospectionAnswer, AnActiveAnswerBecomesAnAuthorization) {
    const IntrospectionReading reading = read(active_answer());
    ASSERT_EQ(reading.event, IntrospectionEvent::active);
    const neomcp::Authorization* granted = authorization_of(reading.verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(granted->issuer, issuer);
    EXPECT_EQ(granted->subject, std::optional<std::string>("alice"));
    EXPECT_EQ(granted->client_id, std::optional<std::string>("client-1"));
    EXPECT_EQ(granted->scopes, (std::vector<std::string>{"mcp:use", "files:read"}));
    EXPECT_EQ(granted->audiences, (std::vector<std::string>{resource}));
    ASSERT_TRUE(granted->expires.has_value());
    EXPECT_EQ(*granted->expires, SystemClock::time_point(std::chrono::seconds(4102444800LL)));
    EXPECT_FALSE(granted->not_before.has_value());
    EXPECT_TRUE(granted->claims.is_object());
    EXPECT_TRUE(granted->claims.empty());
}

TEST(IntrospectionAnswer, TheExpiryIsPassedThroughAsItIs) {
    for (const long long seconds : {1893456000LL, 1700000001LL, 5LL, 0LL, -7LL}) {
        const IntrospectionReading reading = read(active_with("exp", seconds));
        const neomcp::Authorization* granted = authorization_of(reading.verdict);
        ASSERT_NE(granted, nullptr) << seconds;
        ASSERT_TRUE(granted->expires.has_value());
        EXPECT_EQ(*granted->expires, SystemClock::time_point(std::chrono::seconds(seconds))) << seconds;
    }
    // An expiry in the past is reported as it is: the endpoint decides what it means.
    const IntrospectionReading past = read(active_with("exp", 1000));
    ASSERT_NE(authorization_of(past.verdict), nullptr);
    EXPECT_LT(*authorization_of(past.verdict)->expires, now);
}

TEST(IntrospectionAnswer, AFractionOfASecondIsKept) {
    const IntrospectionReading reading = read(active_with("exp", 1700000100.5));
    const neomcp::Authorization* granted = authorization_of(reading.verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(*granted->expires, SystemClock::time_point(std::chrono::seconds(1700000100) + std::chrono::milliseconds(500)));
}

TEST(IntrospectionAnswer, WithoutAnExpiryTheTokenLivesForTheDefaultLifetime) {
    const IntrospectionReading reading = read(active_without("exp"));
    const neomcp::Authorization* granted = authorization_of(reading.verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(granted->expires, std::optional<SystemClock::time_point>(now + lifetime));
}

TEST(IntrospectionAnswer, NotBeforeIsPassedThrough) {
    const IntrospectionReading reading = read(active_with("nbf", 1700000050));
    const neomcp::Authorization* granted = authorization_of(reading.verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(granted->not_before, std::optional<SystemClock::time_point>(SystemClock::time_point(std::chrono::seconds(1700000050))));
}

TEST(IntrospectionAnswer, ADateTooFarFromTheEpochIsHeldInsideTheClock) {
    for (const double seconds : {1e12, 1e18, 1e300, -1e12, -1e300, 9.3e18, 1.8e19}) {
        for (const char* name : {"exp", "nbf"}) {
            const IntrospectionReading reading = read(active_with(name, seconds));
            const neomcp::Authorization* granted = authorization_of(reading.verdict);
            ASSERT_NE(granted, nullptr) << name << " " << seconds;
            const auto date = std::string(name) == "exp" ? granted->expires : granted->not_before;
            ASSERT_TRUE(date.has_value());
            // Room is left for the leeway and the cache age the endpoint adds.
            EXPECT_LE(*date + std::chrono::hours(1), SystemClock::time_point::max()) << name << " " << seconds;
            EXPECT_GE(*date - std::chrono::hours(1), SystemClock::time_point::min()) << name << " " << seconds;
            EXPECT_EQ(*date > now, seconds > 0) << name << " " << seconds;
        }
    }
}

TEST(IntrospectionAnswer, AnInactiveAnswerIsInvalidAndNothingElseIsRead) {
    expect_refusal(read_text(R"({"active":false})"), TokenRefusal::invalid, IntrospectionEvent::inactive, "false");
    // Members of an inactive answer, wrong or not, change nothing.
    expect_refusal(read_text(R"({"active":false,"scope":5,"exp":"x","aud":3,"sub":[]})"), TokenRefusal::invalid,
                   IntrospectionEvent::inactive, "false with other members");
    json full = active_answer();
    full["active"] = false;
    expect_refusal(read(full), TokenRefusal::invalid, IntrospectionEvent::inactive, "false with an active token's members");
}

TEST(IntrospectionAnswer, ActiveMustBeTheBooleanTrue) {
    const std::vector<std::string> bodies = {
        R"({})",
        R"({"scope":"a"})",
        R"({"active":"true"})",
        R"({"active":"false"})",
        R"({"active":1})",
        R"({"active":0})",
        R"({"active":null})",
        R"({"active":[true]})",
        R"({"active":{}})",
    };
    for (const std::string& body : bodies) {
        expect_refusal(read_text(body), TokenRefusal::invalid, IntrospectionEvent::malformed, body);
    }
}

TEST(IntrospectionAnswer, AMemberOfTheWrongTypeMakesTheAnswerMalformed) {
    const std::vector<std::pair<std::string, json>> wrong = {
        {"scope", 5},          {"scope", json::array({"a"})}, {"scope", nullptr},     {"scope", true},
        {"scope", "a\tb"},     {"scope", "a\"b"},             {"scope", "a\\b"},      {"scope", "caf\xC3\xA9"},
        {"aud", 5},            {"aud", json::array({1})},     {"aud", json::array({"a", nullptr})},
        {"aud", nullptr},      {"aud", json::object()},       {"aud", true},
        {"exp", "1700000000"}, {"exp", true},                 {"exp", nullptr},       {"exp", json::array()},
        {"exp", json::object()},
        {"nbf", "1700000000"}, {"nbf", false},                {"nbf", nullptr},       {"nbf", json::array()},
        {"sub", 5},            {"sub", nullptr},              {"sub", json::array()},
        {"client_id", 5},      {"client_id", true},           {"client_id", nullptr},
        {"iss", 5},            {"iss", nullptr},              {"iss", json::array({issuer})},
    };
    for (const auto& [name, value] : wrong) {
        expect_refusal(read(active_with(name, value)), TokenRefusal::invalid, IntrospectionEvent::malformed,
                       name + " = " + value.dump());
    }
}

TEST(IntrospectionAnswer, AnIssuerThatIsNotTheConfiguredOneIsRefused) {
    for (const char* other : {"https://other.example", "https://auth.example/", "HTTPS://auth.example", ""}) {
        expect_refusal(read(active_with("iss", other)), TokenRefusal::invalid, IntrospectionEvent::issuer_mismatch,
                       other);
    }
}

TEST(IntrospectionAnswer, EveryMemberMayBeAbsentButActive) {
    for (const char* name : {"scope", "client_id", "sub", "aud", "iss", "exp", "nbf"}) {
        const IntrospectionReading reading = read(active_without(name));
        EXPECT_EQ(reading.event, IntrospectionEvent::active) << name;
        ASSERT_NE(authorization_of(reading.verdict), nullptr) << name;
    }
    const IntrospectionReading bare = read_text(R"({"active":true})");
    const neomcp::Authorization* granted = authorization_of(bare.verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_EQ(granted->issuer, issuer);
    EXPECT_FALSE(granted->subject.has_value());
    EXPECT_FALSE(granted->client_id.has_value());
    EXPECT_TRUE(granted->scopes.empty());
    EXPECT_TRUE(granted->audiences.empty());
}

TEST(IntrospectionAnswer, ScopesAreSplitAtSpaces) {
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"a", {"a"}},
        {"a b", {"a", "b"}},
        {"  a   b ", {"a", "b"}},
        {"", {}},
        {" ", {}},
        {"read:all write:all", {"read:all", "write:all"}},
        {"A a", {"A", "a"}},
    };
    for (const auto& [text, expected] : cases) {
        const IntrospectionReading reading = read(active_with("scope", text));
        const neomcp::Authorization* granted = authorization_of(reading.verdict);
        ASSERT_NE(granted, nullptr) << text;
        EXPECT_EQ(granted->scopes, expected) << text;
    }
}

TEST(IntrospectionAnswer, TheAudienceIsAStringOrAnArrayOfStrings) {
    const IntrospectionReading one = read(active_with("aud", "https://a.example"));
    ASSERT_NE(authorization_of(one.verdict), nullptr);
    EXPECT_EQ(authorization_of(one.verdict)->audiences, (std::vector<std::string>{"https://a.example"}));
    const IntrospectionReading many = read(active_with("aud", json::array({"x", "y"})));
    ASSERT_NE(authorization_of(many.verdict), nullptr);
    EXPECT_EQ(authorization_of(many.verdict)->audiences, (std::vector<std::string>{"x", "y"}));
    const IntrospectionReading none = read(active_with("aud", json::array()));
    ASSERT_NE(authorization_of(none.verdict), nullptr);
    EXPECT_TRUE(authorization_of(none.verdict)->audiences.empty());
}

TEST(IntrospectionAnswer, AMemberNamedTwiceMakesTheAnswerMalformed) {
    const std::vector<std::string> bodies = {
        R"({"active":false,"active":true})",
        R"({"active":true,"active":false})",
        R"({"active":true,"sub":"a","sub":"b"})",
        R"({"active":true,"exp":1,"exp":2})",
        R"({"active":true,"scope":"a","scope":"b"})",
    };
    for (const std::string& body : bodies) {
        expect_refusal(read_text(body), TokenRefusal::invalid, IntrospectionEvent::malformed, body);
    }
    // A name that repeats inside a member the reader does not look at is not the reader's concern.
    const IntrospectionReading nested = read_text(R"({"active":true,"extra":{"x":1,"x":2},"list":[{"y":1,"y":2}]})");
    EXPECT_EQ(nested.event, IntrospectionEvent::active);
}

TEST(IntrospectionAnswer, OtherMembersAreNotCopied) {
    json answer = active_with("token", "the-token-echoed");
    answer["username"] = "alice";
    const IntrospectionReading reading = read(answer);
    const neomcp::Authorization* granted = authorization_of(reading.verdict);
    ASSERT_NE(granted, nullptr);
    EXPECT_TRUE(granted->claims.empty());
}

TEST(IntrospectionAnswer, AnythingButAJsonObjectIsUnavailable) {
    const std::vector<std::string> bodies = {"",        "[]",         "[{\"active\":true}]", "true",
                                             "null",    "5",          "\"active\"",          "{",
                                             "{\"active\":true", "not json", "{\"active\":true}x"};
    for (const std::string& body : bodies) {
        expect_refusal(read_text(body), TokenRefusal::unavailable, IntrospectionEvent::not_an_object, body);
    }
    // Nesting beyond what an answer needs is not parsed.
    std::string deep = R"({"active":true,"x":)";
    for (int i = 0; i < 200; ++i) deep += "[";
    for (int i = 0; i < 200; ++i) deep += "]";
    deep += "}";
    expect_refusal(read_text(deep), TokenRefusal::unavailable, IntrospectionEvent::not_an_object, "deeply nested");
}

TEST(IntrospectionAnswer, AnyStatusButOkIsUnavailable) {
    for (const int status : {100, 201, 202, 204, 301, 302, 307, 400, 401, 403, 404, 429, 500, 503}) {
        expect_refusal(read_text(active_answer().dump(), status), TokenRefusal::unavailable,
                       IntrospectionEvent::unexpected_status, std::to_string(status));
    }
}

TEST(IntrospectionAnswer, TheBodyBoundIsExactlyTheBound) {
    const std::size_t limit = 200;
    std::string at_bound = R"({"active":true,"pad":")";
    at_bound.append(limit - at_bound.size() - 2, 'p');
    at_bound += "\"}";
    ASSERT_EQ(at_bound.size(), limit);
    EXPECT_EQ(read_introspection_answer(200, at_bound, context(limit)).event, IntrospectionEvent::active);
    const std::string past = at_bound.substr(0, at_bound.size() - 2) + "p\"}";
    ASSERT_EQ(past.size(), limit + 1);
    expect_refusal(read_introspection_answer(200, past, context(limit)), TokenRefusal::unavailable,
                   IntrospectionEvent::response_too_large, "one past the bound");
    // The bound applies to a body of any content, an inactive answer included.
    std::string inactive = R"({"active":false,"pad":")";
    inactive.append(limit, 'p');
    inactive += "\"}";
    expect_refusal(read_introspection_answer(200, inactive, context(limit)), TokenRefusal::unavailable,
                   IntrospectionEvent::response_too_large, "inactive past the bound");
}

}  // namespace
