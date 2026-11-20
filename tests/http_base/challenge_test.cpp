#include "neomcp/http/challenge.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

using namespace neomcp::http;

std::vector<Challenge> parsed(std::string_view value, const ChallengeLimits& limits = {}) {
    auto result = parse_challenges(value, limits);
    auto* challenges = std::get_if<std::vector<Challenge>>(&result);
    EXPECT_NE(challenges, nullptr) << "rejected: " << value;
    return challenges != nullptr ? *challenges : std::vector<Challenge>{};
}

std::vector<Challenge> parsed_lines(const std::vector<std::string>& values, const ChallengeLimits& limits = {}) {
    auto result = parse_challenges(values, limits);
    auto* challenges = std::get_if<std::vector<Challenge>>(&result);
    EXPECT_NE(challenges, nullptr) << "rejected";
    return challenges != nullptr ? *challenges : std::vector<Challenge>{};
}

ChallengeError rejected(std::string_view value, const ChallengeLimits& limits = {}) {
    auto result = parse_challenges(value, limits);
    auto* error = std::get_if<ChallengeError>(&result);
    EXPECT_NE(error, nullptr) << "accepted: " << value;
    return error != nullptr ? *error : ChallengeError{};
}

BearerChallenge bearer_of(std::string_view value) {
    const std::vector<Challenge> challenges = parsed(value);
    auto result = find_bearer_challenge(challenges);
    auto* bearer = std::get_if<BearerChallenge>(&result);
    EXPECT_NE(bearer, nullptr) << "no usable Bearer challenge in: " << value;
    return bearer != nullptr ? *bearer : BearerChallenge{};
}

BearerProblem bearer_problem(std::string_view value) {
    const std::vector<Challenge> challenges = parsed(value);
    auto result = find_bearer_challenge(challenges);
    auto* problem = std::get_if<BearerProblem>(&result);
    EXPECT_NE(problem, nullptr) << "a Bearer challenge was found in: " << value;
    return problem != nullptr ? *problem : BearerProblem::absent;
}

// ---- The grammar of RFC 9110

TEST(ChallengeGrammar, TheExampleOfRfc9110) {
    const auto challenges =
        parsed(R"(Basic realm="simple", Newauth realm="apps", type=1, title="Login to \"apps\"")");
    ASSERT_EQ(challenges.size(), 2u);
    EXPECT_EQ(challenges[0].scheme, "basic");
    ASSERT_EQ(challenges[0].params.size(), 1u);
    EXPECT_EQ(challenges[0].params[0].name, "realm");
    EXPECT_EQ(challenges[0].params[0].value, "simple");
    EXPECT_EQ(challenges[1].scheme, "newauth");
    ASSERT_EQ(challenges[1].params.size(), 3u);
    EXPECT_EQ(challenges[1].params[0].value, "apps");
    EXPECT_EQ(challenges[1].params[1].name, "type");
    EXPECT_EQ(challenges[1].params[1].value, "1");
    EXPECT_EQ(challenges[1].params[2].name, "title");
    EXPECT_EQ(challenges[1].params[2].value, R"(Login to "apps")");
}

TEST(ChallengeGrammar, QuotedStringEscapesAreResolved) {
    const auto challenges = parsed(R"(X a="back\\slash \"quote\" \x plain", b="", c="tab)" "\t" R"(inside")");
    ASSERT_EQ(challenges.size(), 1u);
    ASSERT_EQ(challenges[0].params.size(), 3u);
    EXPECT_EQ(challenges[0].params[0].value, R"(back\slash "quote" x plain)");
    EXPECT_EQ(challenges[0].params[1].value, "");
    EXPECT_EQ(challenges[0].params[2].value, "tab\tinside");
}

TEST(ChallengeGrammar, QuotedStringHoldsListAndParameterSyntax) {
    const auto challenges = parsed(R"(Bearer scope="urn:example:channel=HBO&urn:example:rating=G,PG-13", realm=x)");
    ASSERT_EQ(challenges.size(), 1u);
    ASSERT_EQ(challenges[0].params.size(), 2u);
    EXPECT_EQ(challenges[0].params[0].value, "urn:example:channel=HBO&urn:example:rating=G,PG-13");
    EXPECT_EQ(challenges[0].params[1].value, "x");
}

TEST(ChallengeGrammar, SchemesAndParameterNamesAreFoldedValuesAreNot) {
    const auto challenges = parsed(R"(BEARER Resource_Metadata="https://Example.com/A", ScOpE=Mixed)");
    ASSERT_EQ(challenges.size(), 1u);
    EXPECT_EQ(challenges[0].scheme, "bearer");
    ASSERT_EQ(challenges[0].params.size(), 2u);
    EXPECT_EQ(challenges[0].params[0].name, "resource_metadata");
    EXPECT_EQ(challenges[0].params[0].value, "https://Example.com/A");
    EXPECT_EQ(challenges[0].params[1].name, "scope");
    EXPECT_EQ(challenges[0].params[1].value, "Mixed");
    ASSERT_NE(challenges[0].param("RESOURCE_METADATA"), nullptr);
    EXPECT_EQ(*challenges[0].param("RESOURCE_METADATA"), "https://Example.com/A");
    EXPECT_EQ(challenges[0].param("realm"), nullptr);
}

TEST(ChallengeGrammar, WhitespaceAroundTheEqualsSign) {
    const auto challenges = parsed("Bearer resource_metadata= \"https://r.example/m\" , scope =\t\"a b\"");
    ASSERT_EQ(challenges.size(), 1u);
    ASSERT_EQ(challenges[0].params.size(), 2u);
    EXPECT_EQ(challenges[0].params[0].value, "https://r.example/m");
    EXPECT_EQ(challenges[0].params[1].value, "a b");
}

TEST(ChallengeGrammar, EmptyListElementsAreSkipped) {
    const auto challenges = parsed(" , ,Bearer a=b,,c=d , , Basic ,");
    ASSERT_EQ(challenges.size(), 2u);
    EXPECT_EQ(challenges[0].scheme, "bearer");
    ASSERT_EQ(challenges[0].params.size(), 2u);
    EXPECT_EQ(challenges[0].params[1].name, "c");
    EXPECT_EQ(challenges[1].scheme, "basic");
    EXPECT_TRUE(challenges[1].params.empty());
    EXPECT_TRUE(parsed("").empty());
    EXPECT_TRUE(parsed(" ,, ").empty());
}

TEST(ChallengeGrammar, ASchemeAlone) {
    const auto challenges = parsed("Bearer");
    ASSERT_EQ(challenges.size(), 1u);
    EXPECT_EQ(challenges[0].scheme, "bearer");
    EXPECT_TRUE(challenges[0].params.empty());
    EXPECT_FALSE(challenges[0].token68.has_value());
    const auto two = parsed("Basic, Bearer");
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[1].scheme, "bearer");
}

TEST(ChallengeGrammar, Token68Forms) {
    const auto padded = parsed("Basic abc==");
    ASSERT_EQ(padded.size(), 1u);
    ASSERT_TRUE(padded[0].token68.has_value());
    EXPECT_EQ(*padded[0].token68, "abc==");
    EXPECT_TRUE(padded[0].params.empty());

    // A token and "=" with nothing after it is a token68 too.
    const auto one_pad = parsed("Basic abc=");
    ASSERT_EQ(one_pad.size(), 1u);
    ASSERT_TRUE(one_pad[0].token68.has_value());
    EXPECT_EQ(*one_pad[0].token68, "abc=");

    const auto alphabet = parsed("Negotiate a+b/c-d.e_f~g=, Basic realm=x");
    ASSERT_EQ(alphabet.size(), 2u);
    ASSERT_TRUE(alphabet[0].token68.has_value());
    EXPECT_EQ(*alphabet[0].token68, "a+b/c-d.e_f~g=");
    EXPECT_EQ(alphabet[1].scheme, "basic");

    // A token, "=" and a token is a parameter, with whitespace after the "=" allowed.
    const auto parameter = parsed("x abc= def");
    ASSERT_EQ(parameter.size(), 1u);
    EXPECT_FALSE(parameter[0].token68.has_value());
    ASSERT_EQ(parameter[0].params.size(), 1u);
    EXPECT_EQ(parameter[0].params[0].name, "abc");
    EXPECT_EQ(parameter[0].params[0].value, "def");
}

TEST(ChallengeGrammar, SeveralFieldLinesAreOneList) {
    const auto separate = parsed_lines({R"(Basic realm="x")", R"(Bearer scope="a")"});
    ASSERT_EQ(separate.size(), 2u);
    EXPECT_EQ(separate[0].scheme, "basic");
    EXPECT_EQ(separate[1].scheme, "bearer");

    // A line that starts with a parameter continues the last challenge of the line before.
    const auto continued = parsed_lines({R"(Bearer realm="x")", R"(scope="a")"});
    ASSERT_EQ(continued.size(), 1u);
    ASSERT_EQ(continued[0].params.size(), 2u);
    EXPECT_EQ(continued[0].params[1].name, "scope");

    EXPECT_TRUE(parsed_lines({}).empty());
    EXPECT_EQ(parsed_lines({"", "Bearer", ""}).size(), 1u);
}

TEST(ChallengeGrammar, RepeatedParameterNamesAreMarked) {
    const auto challenges = parsed(R"(Bearer scope="a", SCOPE="b", Basic realm=x)");
    ASSERT_EQ(challenges.size(), 2u);
    EXPECT_TRUE(challenges[0].repeated_parameter);
    EXPECT_EQ(challenges[0].params.size(), 2u);
    EXPECT_FALSE(challenges[1].repeated_parameter);
    EXPECT_FALSE(parsed(R"(Bearer scope="a", realm="a")")[0].repeated_parameter);
}

// ---- What the grammar does not allow

TEST(ChallengeGrammar, MalformedValues) {
    const std::vector<std::string> values = {
        "Bearer scope=mcp:basic",       // ':' is not a token character: the value needs quotes
        R"(Bearer scope="open)",        // no closing quote
        R"(Bearer scope="a\)",          // an escape at the end
        "Bearer a=, b=c",               // no value before the comma
        "=x",                           // no scheme
        "a=b",                          // a parameter before any scheme
        R"(Bearer a="x" b="y")",        // no comma between parameters
        "Bearer a=b c",                 // text after a parameter
        "Basic abc, realm=x",           // a parameter after a token68
        "Basic abc== def",              // text after a token68
        "Bearer\trealm=x",              // the scheme is followed by spaces, not a tab
        "Bearer realm=\"a\x01" "b\"",   // a control character in a quoted string
        "Bearer realm=\"a\\\x01\"",     // an escaped control character
        "Bearer re\xc3\xa4lm=x",        // a parameter name that is not a token
        "Bearer \"x\"",                 // a quoted string where a parameter or token68 must be
        "Bearer realm=x;",              // ';' is not a token character
    };
    for (const auto& value : values) {
        SCOPED_TRACE(value);
        EXPECT_EQ(rejected(value).problem, ChallengeProblem::malformed);
    }
}

TEST(ChallengeGrammar, ObsTextIsAllowedInQuotedStrings) {
    const auto challenges = parsed("Bearer realm=\"caf\xc3\xa9\"");
    ASSERT_EQ(challenges.size(), 1u);
    EXPECT_EQ(challenges[0].params[0].value, "caf\xc3\xa9");
}

TEST(ChallengeGrammar, AnErrorNamesTheFieldAndTheOffset) {
    auto result = parse_challenges(std::vector<std::string>{"Bearer a=b", "Basic realm=x", "Bearer scope=x:y"});
    auto* error = std::get_if<ChallengeError>(&result);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->problem, ChallengeProblem::malformed);
    EXPECT_EQ(error->field, 2u);
    // Reading stops where the challenge's data starts: neither a parameter nor a token68 fits there.
    EXPECT_EQ(error->offset, std::string_view("Bearer ").size());
}

TEST(ChallengeGrammar, AnErrorCarriesNoTextOfTheServer) {
    const std::string planted = "planted-secret-0123";
    const ChallengeError error = rejected("Bearer error_description=\"" + planted);
    EXPECT_EQ(std::string(to_string(error.problem)).find(planted), std::string::npos);
    EXPECT_EQ(std::string(to_string(ChallengeProblem::too_large)).find(planted), std::string::npos);
}

// ---- Bounds

TEST(ChallengeBounds, BytesAtTheBoundAreReadOneMoreIsRefused) {
    const std::string value = R"(Bearer resource_metadata="https://r.example/.well-known/oauth-protected-resource")";
    ChallengeLimits limits;
    limits.max_bytes = value.size();
    EXPECT_EQ(parsed(value, limits).size(), 1u);
    limits.max_bytes = value.size() - 1;
    EXPECT_EQ(rejected(value, limits).problem, ChallengeProblem::too_large);
}

TEST(ChallengeBounds, TheByteBoundCoversEveryFieldLine) {
    const std::vector<std::string> values = {"Basic realm=x", "Bearer scope=y"};
    ChallengeLimits limits;
    limits.max_bytes = values[0].size() + values[1].size();
    EXPECT_EQ(parsed_lines(values, limits).size(), 2u);
    limits.max_bytes -= 1;
    auto result = parse_challenges(values, limits);
    auto* error = std::get_if<ChallengeError>(&result);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->problem, ChallengeProblem::too_large);
}

TEST(ChallengeBounds, TheDefaultsAreTheHeaderBoundAndSixtyFourParameters) {
    EXPECT_EQ(ChallengeLimits::default_max_bytes, std::size_t{64} * 1024);
    EXPECT_EQ(ChallengeLimits::default_max_params, 64u);
    const std::string at_bound = "Bearer realm=\"" + std::string(ChallengeLimits::default_max_bytes - 15, 'a') + "\"";
    ASSERT_EQ(at_bound.size(), ChallengeLimits::default_max_bytes);
    EXPECT_EQ(parsed(at_bound).size(), 1u);
    EXPECT_EQ(rejected(at_bound + " ").problem, ChallengeProblem::too_large);
}

TEST(ChallengeBounds, TooManyParametersDropOnlyThatChallenge) {
    ChallengeLimits limits;
    limits.max_params = 2;
    const auto at_bound = parsed("Bearer a=1, b=2", limits);
    ASSERT_EQ(at_bound.size(), 1u);
    EXPECT_FALSE(at_bound[0].too_many_parameters);
    EXPECT_EQ(at_bound[0].params.size(), 2u);

    const auto over = parsed("Bearer a=1, b=2, c=3, Basic realm=x, y=z", limits);
    ASSERT_EQ(over.size(), 2u);
    EXPECT_TRUE(over[0].too_many_parameters);
    EXPECT_EQ(over[0].params.size(), 2u);
    EXPECT_EQ(over[1].scheme, "basic");
    EXPECT_FALSE(over[1].too_many_parameters);
    EXPECT_EQ(over[1].params.size(), 2u);

    auto result = find_bearer_challenge(over);
    ASSERT_TRUE(std::holds_alternative<BearerProblem>(result));
    EXPECT_EQ(std::get<BearerProblem>(result), BearerProblem::too_many_parameters);
}

// ---- The Bearer challenge

// What the bearer middleware of the conformance runner's servers sends: every value quoted, ", " between them.
TEST(ChallengeBearer, TheRefusalOfARequestWithoutAToken) {
    const BearerChallenge bearer = bearer_of(
        R"(Bearer error="invalid_token", error_description="Missing Authorization header", )"
        R"(resource_metadata="http://localhost:3001/.well-known/oauth-protected-resource/mcp")");
    EXPECT_EQ(bearer.error_kind, BearerError::invalid_token);
    ASSERT_TRUE(bearer.error.has_value());
    EXPECT_EQ(*bearer.error, "invalid_token");
    ASSERT_TRUE(bearer.resource_metadata.has_value());
    EXPECT_EQ(*bearer.resource_metadata, "http://localhost:3001/.well-known/oauth-protected-resource/mcp");
    ASSERT_TRUE(bearer.error_description.has_value());
    EXPECT_FALSE(bearer.scope.has_value());
    EXPECT_TRUE(bearer.scopes().empty());
}

TEST(ChallengeBearer, TheStepUpRefusal) {
    const BearerChallenge bearer = bearer_of(
        R"(Bearer error="insufficient_scope", scope="mcp:basic mcp:write", )"
        R"(resource_metadata="http://localhost:3001/.well-known/oauth-protected-resource/mcp")");
    EXPECT_EQ(bearer.error_kind, BearerError::insufficient_scope);
    EXPECT_EQ(bearer.scopes(), (std::vector<std::string>{"mcp:basic", "mcp:write"}));
}

TEST(ChallengeBearer, TheChallengeWithAScopeAndNoError) {
    const BearerChallenge bearer = bearer_of(
        R"(Bearer resource_metadata="https://mcp.example.com/.well-known/oauth-protected-resource", )"
        R"(scope="files:read")");
    EXPECT_EQ(bearer.error_kind, BearerError::none);
    EXPECT_FALSE(bearer.error.has_value());
    EXPECT_EQ(bearer.scopes(), (std::vector<std::string>{"files:read"}));
}

TEST(ChallengeBearer, TheExamplesOfRfc6750) {
    EXPECT_EQ(bearer_of(R"(Bearer realm="example")").error_kind, BearerError::none);
    const BearerChallenge expired =
        bearer_of(R"(Bearer realm="example", error="invalid_token", error_description="The access token expired")");
    EXPECT_EQ(expired.error_kind, BearerError::invalid_token);
    EXPECT_EQ(bearer_of(R"(Bearer error="invalid_request")").error_kind, BearerError::invalid_request);
    EXPECT_EQ(bearer_of(R"(Bearer scope="openid profile email")").scopes(),
              (std::vector<std::string>{"openid", "profile", "email"}));
}

TEST(ChallengeBearer, ErrorCodesCompareExactly) {
    const BearerChallenge other = bearer_of(R"(Bearer error="Invalid_Token")");
    EXPECT_EQ(other.error_kind, BearerError::other);
    ASSERT_TRUE(other.error.has_value());
    EXPECT_EQ(*other.error, "Invalid_Token");
    EXPECT_EQ(bearer_of("Bearer error=use_dpop_nonce").error_kind, BearerError::other);
}

TEST(ChallengeBearer, ScopesSplitAtSpaces) {
    EXPECT_EQ(bearer_of(R"(Bearer scope="  a  b ")").scopes(), (std::vector<std::string>{"a", "b"}));
    EXPECT_TRUE(bearer_of(R"(Bearer scope="")").scopes().empty());
}

TEST(ChallengeBearer, TheSchemeIsFoundIgnoringCaseAmongOthers) {
    const BearerChallenge bearer = bearer_of(
        R"(Basic realm="x", DPoP algs="ES256", bEaReR Resource_Metadata="https://r.example/m", SCOPE=a)");
    ASSERT_TRUE(bearer.resource_metadata.has_value());
    EXPECT_EQ(*bearer.resource_metadata, "https://r.example/m");
    ASSERT_TRUE(bearer.scope.has_value());
    EXPECT_EQ(*bearer.scope, "a");
}

TEST(ChallengeBearer, AChallengeAloneHasNoParameters) {
    const BearerChallenge bearer = bearer_of("Bearer");
    EXPECT_FALSE(bearer.resource_metadata.has_value());
    EXPECT_FALSE(bearer.scope.has_value());
    EXPECT_EQ(bearer.error_kind, BearerError::none);
}

TEST(ChallengeBearer, ChallengesThatCannotBeUsed) {
    EXPECT_EQ(bearer_problem(R"(Basic realm="x", Digest realm="y")"), BearerProblem::absent);
    EXPECT_EQ(bearer_problem(""), BearerProblem::absent);
    EXPECT_EQ(bearer_problem("Bearer abc=="), BearerProblem::token68_form);
    EXPECT_EQ(bearer_problem(R"(Bearer scope="a", Scope="b")"), BearerProblem::repeated_parameter);
    EXPECT_EQ(bearer_problem(R"(Bearer resource_metadata="x", resource_metadata="y")"),
              BearerProblem::repeated_parameter);
    // The first Bearer challenge decides; a later one does not take its place.
    EXPECT_EQ(bearer_problem(R"(Bearer abc==, Bearer scope="a")"), BearerProblem::token68_form);
}

TEST(ChallengeBearer, ProblemTextsAreFixed) {
    for (const BearerProblem problem : {BearerProblem::absent, BearerProblem::token68_form,
                                        BearerProblem::repeated_parameter, BearerProblem::too_many_parameters}) {
        EXPECT_FALSE(to_string(problem).empty());
    }
}

}  // namespace
