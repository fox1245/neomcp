// Scenarios of the Streamable HTTP endpoint as an OAuth protected resource, over loopback sockets: what the checks
// made on every validated token refuse (another resource, another issuer, a token out of its time at the edge of the
// leeway, too many scopes), what the caches never confuse, how a validator that hangs, throws or answers oddly is
// treated, how a stop ends the requests that wait for a verdict, and that the raw token appears in no answer, no
// field and no line the process writes.
//
// The tests that complement the other two files of this feature (http_resource_server_test.cpp: the table of refused
// tokens and the gate's answers; http_resource_server_flight_test.cpp: single flight and arrival order) are named
// ResourceServerScenario*.
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "crypto/sha256.h"
#include "support/resource_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::Authorization;
using neomcp::json;

constexpr int internal_error_code = -32603;

std::string hex_digest(const std::string& text, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    const auto digest = neomcp::detail::sha256(text);
    std::string out;
    for (const unsigned char byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

// What an answer says apart from fields that differ by the moment (a date): the status, the fields about the challenge
// and the body.
std::string shape_of(const Reply& reply) {
    return std::to_string(reply.status) + "|" + reply.header("WWW-Authenticate") + "|" + reply.header("Cache-Control") +
           "|" + reply.header("Content-Type") + "|" + reply.body;
}

// A request that needs a valid token and runs a tool that needs nothing else.
Reply probe(Harness& harness, const std::string& token) { return call_tool(harness.port, token, "open_tool"); }

// ---- What the checks on every validated token refuse -----------------------------------------------------------------

TEST(ResourceServerScenarioToken, ATokenForAnotherResourceOrAudienceIsRefusedWhateverTheNearMiss) {
    Harness harness;
    auto& clock = harness.clock;
    const std::string actual = "http://127.0.0.1:" + std::to_string(harness.port) + "/mcp";
    const std::vector<std::string> other_audiences{
        "HTTP://localhost/mcp",  "http://LOCALHOST/mcp", "http://localhost/mcp/", "http://localhost/mcp#frag",
        "http://localhost/mcp?x=1", "http://localhost",   "http://localhost/",     "http://localhost/mcp2",
        "http://localhost/mcp/extra", " http://localhost/mcp", "http://localhost/mcp ", "http://localhost:80/mcp",
        "",                         first_issuer,          "http://127.0.0.1/mcp",  actual};
    std::set<std::string> names;
    for (std::size_t at = 0; at < other_audiences.size(); ++at) {
        const std::string name = "aud-" + std::to_string(at);
        harness.token(name, granted(clock, "a", {"mcp:use"}, 3600s, first_issuer, {other_audiences[at]}));
        names.insert(name);
    }
    for (const std::string& name : names) {
        const Reply reply = probe(harness, name);
        EXPECT_EQ(reply.status, 401) << name;
        EXPECT_EQ(challenge_of(reply).error, "invalid_token") << name;
        EXPECT_EQ(error_of(reply), "invalid_token") << name;
    }
    EXPECT_EQ(harness.items.total(), 0) << "nothing ran for a token of another resource";
    // One audience among others is enough, wherever it stands in the list.
    harness.token("among-others", granted(clock, "a", {"mcp:use"}, 3600s, first_issuer,
                                          {"http://other.example/", configured_resource, "urn:x"}));
    harness.token("first-of-many", granted(clock, "a", {"mcp:use"}, 3600s, first_issuer, {configured_resource, "urn:y"}));
    EXPECT_EQ(probe(harness, "among-others").status, 200);
    EXPECT_EQ(probe(harness, "first-of-many").status, 200);
    EXPECT_EQ(harness.items.open_tool.load(), 2);
    // The validator is told the resource the options name, never an address the request was sent to.
    for (const std::string& asked : harness.table->resources()) EXPECT_EQ(asked, configured_resource);
}

TEST(ResourceServerScenarioToken, AnAudienceTheServerAcceptsBesidesItsResourceIsCheckedExactly) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->accepted_audiences = {"urn:mcp:legacy"}; });
    auto& clock = harness.clock;
    harness.token("mapped", granted(clock, "a", {"mcp:use"}, 3600s, first_issuer, {"urn:mcp:legacy"}));
    harness.token("mapped-case", granted(clock, "a", {"mcp:use"}, 3600s, first_issuer, {"urn:mcp:LEGACY"}));
    harness.token("mapped-prefix", granted(clock, "a", {"mcp:use"}, 3600s, first_issuer, {"urn:mcp:legacy:2"}));
    harness.token("resource", granted(clock, "a", {"mcp:use"}));
    EXPECT_EQ(probe(harness, "mapped").status, 200);
    EXPECT_EQ(probe(harness, "resource").status, 200);
    EXPECT_EQ(probe(harness, "mapped-case").status, 401);
    EXPECT_EQ(probe(harness, "mapped-prefix").status, 401);
}

TEST(ResourceServerScenarioToken, ATokenOfAnotherIssuerIsRefusedWhateverTheNearMiss) {
    Harness harness;
    auto& clock = harness.clock;
    const std::vector<std::string> other_issuers{"HTTPS://auth.example", "https://AUTH.example", "https://auth.example/",
                                                 "https://auth.example:443", " https://auth.example",
                                                 "https://auth.example ", "https://auth.exampl", "https://auth.example.test",
                                                 "https://other-auth.example", configured_resource};
    for (std::size_t at = 0; at < other_issuers.size(); ++at) {
        const std::string name = "iss-" + std::to_string(at);
        harness.token(name, granted(clock, "a", {"mcp:use"}, 3600s, other_issuers[at]));
        const Reply reply = probe(harness, name);
        EXPECT_EQ(reply.status, 401) << other_issuers[at];
        EXPECT_EQ(challenge_of(reply).error, "invalid_token") << other_issuers[at];
    }
    EXPECT_EQ(harness.items.total(), 0);
}

TEST(ResourceServerScenarioToken, EveryKindOfRefusedTokenGetsTheSameAnswerSoNoneTellsWhyItFailed) {
    Harness harness;
    auto& clock = harness.clock;
    auto early = granted(clock, "a", {"mcp:use"});
    early.not_before = clock.now() + 3600s;
    harness.token("unknown-issuer", granted(clock, "a", {"mcp:use"}, 3600s, second_issuer));
    harness.token("other-audience", granted(clock, "a", {"mcp:use"}, 3600s, first_issuer, {"http://localhost/x"}));
    harness.token("expired", granted(clock, "a", {"mcp:use"}, -3600s));
    harness.token("early", early);
    harness.token("revoked", TokenRefusal::invalid);
    // "never-issued" is not in the table at all.
    std::set<std::string> answers;
    for (const char* token : {"unknown-issuer", "other-audience", "expired", "early", "revoked", "never-issued"}) {
        const Reply reply = probe(harness, token);
        ASSERT_EQ(reply.status, 401) << token;
        answers.insert(shape_of(reply));
    }
    EXPECT_EQ(answers.size(), 1u) << "the answers differ by the reason";
}

// ---- Time: the leeway at its edges ----------------------------------------------------------------------------------

TEST(ResourceServerScenarioToken, ExpiryAndNotBeforeAreCheckedAtTheLeewayAndOneSecondEitherSide) {
    for (const std::chrono::seconds leeway : {0s, 5s, 300s}) {
        Harness harness([&](neomcp::HttpServerOptions& options) { options.authorization->bounds.leeway = leeway; });
        auto& clock = harness.clock;
        const auto expiring = [&](const char* name, std::chrono::seconds relative) {
            harness.token(name, granted(clock, "a", {"mcp:use"}, relative));
            return probe(harness, name).status;
        };
        const auto starting = [&](const char* name, std::chrono::seconds relative) {
            auto authorization = granted(clock, "a", {"mcp:use"});
            authorization.not_before = clock.now() + relative;
            harness.token(name, authorization);
            return probe(harness, name).status;
        };
        EXPECT_EQ(expiring("exp-at", -leeway), 401) << "expired exactly at the end of the leeway, leeway " << leeway.count();
        EXPECT_EQ(expiring("exp-under", -leeway + 1s), 200) << "one second inside, leeway " << leeway.count();
        EXPECT_EQ(expiring("exp-over", -leeway - 1s), 401) << "one second past, leeway " << leeway.count();
        EXPECT_EQ(starting("nbf-at", leeway), 200) << "valid exactly as far ahead as the leeway, leeway " << leeway.count();
        EXPECT_EQ(starting("nbf-under", leeway - 1s), 200) << "one second inside, leeway " << leeway.count();
        EXPECT_EQ(starting("nbf-over", leeway + 1s), 401) << "one second past, leeway " << leeway.count();
    }
}

TEST(ResourceServerScenarioToken, ARememberedVerdictEndsAtTheSameInstantAFreshOneWould) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.leeway = 5s; });
    const std::string token = harness.token("t", granted(harness.clock, "a", {"mcp:use"}, 10s));
    EXPECT_EQ(probe(harness, token).status, 200);
    harness.clock.advance(14s);
    EXPECT_EQ(probe(harness, token).status, 200) << "one second before the end of expiry plus leeway";
    harness.clock.advance(1s);
    EXPECT_EQ(probe(harness, token).status, 401) << "at the end of expiry plus leeway";
    EXPECT_EQ(harness.items.open_tool.load(), 2);
}

TEST(ResourceServerScenarioToken, ATokenWhoseExpiryIsTheLastInstantTheClockHoldsNeverExpires) {
    Harness harness;
    auto forever = granted(harness.clock, "a", {"mcp:use"});
    forever.expires = SystemClock::time_point::max();
    forever.not_before = SystemClock::time_point::min();
    harness.token("forever", forever);
    EXPECT_EQ(probe(harness, "forever").status, 200);
    EXPECT_EQ(probe(harness, "forever").status, 200) << "and from the remembered verdict";
    auto long_gone = granted(harness.clock, "a", {"mcp:use"});
    long_gone.expires = SystemClock::time_point::min();
    harness.token("long-gone", long_gone);
    EXPECT_EQ(probe(harness, "long-gone").status, 401);
}

// ---- Bounds on what a token says ------------------------------------------------------------------------------------

TEST(ResourceServerScenarioToken, ATokenWithAsManyScopesAsTheBoundIsAcceptedAndOneMoreIsRefused) {
    Harness harness([](neomcp::HttpServerOptions& options) { options.authorization->bounds.max_scopes = 3; });
    auto& clock = harness.clock;
    harness.token("three", granted(clock, "a", {"mcp:use", "x:1", "x:2"}));
    harness.token("four", granted(clock, "a", {"mcp:use", "x:1", "x:2", "x:3"}));
    EXPECT_EQ(probe(harness, "three").status, 200);
    const Reply four = probe(harness, "four");
    EXPECT_EQ(four.status, 401);
    EXPECT_EQ(challenge_of(four).error, "invalid_token");

    Harness defaults;
    std::vector<std::string> many{"mcp:use"};
    while (many.size() < neomcp::http::ResourceServerDefaults::max_scopes) many.push_back("s:" + std::to_string(many.size()));
    defaults.token("at-bound", granted(defaults.clock, "a", many));
    many.push_back("one-more");
    defaults.token("past-bound", granted(defaults.clock, "a", many));
    EXPECT_EQ(probe(defaults, "at-bound").status, 200);
    EXPECT_EQ(probe(defaults, "past-bound").status, 401);
}

// ---- What the caches never confuse ----------------------------------------------------------------------------------

TEST(ResourceServerScenarioCache, TheVerdictOnOneTokenNeverAnswersAnotherThatLooksLikeIt) {
    Harness harness;
    auto& clock = harness.clock;
    harness.token("tokenA", granted(clock, "alice", {"mcp:use"}));
    harness.token("tokenB", granted(clock, "bob", {"mcp:use"}));
    const auto who = [&](const std::string& token) {
        const Reply reply = call_tool(harness.port, token, "whoami");
        return reply.status == 200 ? json::parse(text_of(reply)).at("subject").get<std::string>() : std::string();
    };
    EXPECT_EQ(who("tokenA"), "alice");
    EXPECT_EQ(who("tokenB"), "bob") << "same length, one byte apart";
    EXPECT_EQ(who("tokenA"), "alice");
    EXPECT_EQ(harness.table->calls(), 2);
    const int before = harness.table->calls();
    int refused = 0;
    for (const char* near : {"token", "tokenAA", "tokenC", "TOKENA", "tokenA=", "okenA", "tokenA-"}) {
        const Reply reply = probe(harness, near);
        if (reply.status == 401) ++refused;
    }
    EXPECT_EQ(refused, 7);
    EXPECT_EQ(harness.table->calls(), before + 7) << "each was asked about by itself";
    EXPECT_EQ(who("tokenA"), "alice");
    EXPECT_EQ(harness.table->calls(), before + 7) << "and the first one is still remembered";
}

// ---- A validator that hangs, throws or answers oddly -----------------------------------------------------------------

TEST(ResourceServerScenarioValidator, AValidatorThatNeverAnswersGives503WhenTheTimeIsUpAndNoRequestRuns) {
    // It keeps the replies, so that the test can answer late.
    auto validator = std::make_shared<Programmable>(
        [](Programmable& self, const neomcp::http::TokenCheck&, neomcp::http::TokenReply reply) { self.hold(std::move(reply)); });
    Harness harness([&](neomcp::HttpServerOptions& options) {
        options.authorization->validator = validator;
        options.authorization->bounds.validation_timeout = 300ms;
    });
    const auto started = std::chrono::steady_clock::now();
    const Reply first = probe(harness, "late-token");
    const auto waited = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(first.status, 503);
    EXPECT_GE(waited, 250ms) << "not before the time was up";
    EXPECT_LT(waited, 20s);
    EXPECT_EQ(first.count("WWW-Authenticate"), 0u) << "not the token's fault";
    const json body = json::parse(first.body.empty() ? "{}" : first.body);
    ASSERT_TRUE(body.contains("error")) << first.body;
    EXPECT_EQ(body.at("error").at("code"), internal_error_code);
    EXPECT_EQ(harness.items.total(), 0);

    // The validator answers after all, and says the token is good: the request was answered long ago, nothing runs,
    // and nothing is remembered.
    validator->release(granted(harness.clock, "a", {"mcp:use"}));
    EXPECT_EQ(harness.items.total(), 0) << "a verdict that came late runs nothing";
    const Reply second = probe(harness, "late-token");
    EXPECT_EQ(second.status, 503) << "the late verdict was not remembered";
    EXPECT_EQ(validator->calls.load(), 2) << "the token was asked about again";
    EXPECT_EQ(harness.items.total(), 0);
}

TEST(ResourceServerScenarioValidator, ManyRequestsOnManySocketsWithOneNewTokenCauseOneValidation) {
    auto validator = std::make_shared<Programmable>(
        [](Programmable& self, const neomcp::http::TokenCheck&, neomcp::http::TokenReply reply) { self.hold(std::move(reply)); });
    Harness harness([&](neomcp::HttpServerOptions& options) {
        options.authorization->validator = validator;
        options.authorization->bounds.validation_timeout = 60s;
        options.worker_threads = 8;
    });
    const std::string body = rpc(1, "tools/call", modern_params({{"name", "open_tool"}, {"arguments", json::object()}}));
    std::vector<std::unique_ptr<Call>> calls;
    for (int at = 0; at < 8; ++at) {
        calls.push_back(std::make_unique<Call>(harness.port, "POST", modern_headers("tools/call", "shared-token", "open_tool"), body));
    }
    const auto waiting_since = std::chrono::steady_clock::now();
    while (validator->held_count() < 1 && std::chrono::steady_clock::now() - waiting_since < patience) {
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(300ms);  // the others arrive and wait behind the first
    for (const auto& call : calls) EXPECT_FALSE(call->finished());
    EXPECT_EQ(harness.items.total(), 0) << "no request runs before its token is validated";
    validator->release(granted(harness.clock, "a", {"mcp:use"}));
    for (const auto& call : calls) EXPECT_EQ(call->finish(30s).status, 200);
    EXPECT_EQ(validator->calls.load(), 1);
    EXPECT_EQ(harness.items.open_tool.load(), 8);
}

TEST(ResourceServerScenarioValidator, AValidatorThatThrowsGives503WithoutItsTextAndNothingIsRemembered) {
    const std::string detail = "internal-detail-of-the-validator-7f3a";
    auto text = std::make_shared<Programmable>(
        [&](Programmable&, const neomcp::http::TokenCheck& check, neomcp::http::TokenReply) {
            throw std::runtime_error(detail + std::string(check.token));
        });
    auto number = std::make_shared<Programmable>(
        [](Programmable&, const neomcp::http::TokenCheck&, neomcp::http::TokenReply) { throw 42; });
    for (const auto& validator : {text, number}) {
        Harness harness([&](neomcp::HttpServerOptions& options) { options.authorization->validator = validator; });
        const Reply first = probe(harness, "tok-to-throw-about");
        EXPECT_EQ(first.status, 503);
        EXPECT_EQ(first.count("WWW-Authenticate"), 0u);
        EXPECT_EQ(first.everything().find(detail), std::string::npos);
        EXPECT_EQ(first.everything().find("tok-to-throw-about"), std::string::npos);
        EXPECT_EQ(probe(harness, "tok-to-throw-about").status, 503);
        EXPECT_EQ(harness.items.total(), 0);
    }
    EXPECT_EQ(text->calls.load(), 2);
    EXPECT_EQ(number->calls.load(), 2);
}

TEST(ResourceServerScenarioValidator, TheFirstAnswerOfAValidatorStandsWhateverItDoesNext) {
    // Answers, then throws: the exception counts for nothing once the verdict is in.
    auto answers_then_throws = std::make_shared<Programmable>(
        [](Programmable&, const neomcp::http::TokenCheck&, neomcp::http::TokenReply reply) {
            reply(granted_now("a", {"mcp:use"}));
            throw std::runtime_error("after the answer");
        });
    // Answers twice, the second time that the token is no good.
    auto answers_twice = std::make_shared<Programmable>(
        [](Programmable&, const neomcp::http::TokenCheck&, neomcp::http::TokenReply reply) {
            reply(granted_now("a", {"mcp:use"}));
            reply(TokenRefusal::invalid);
        });
    // Answers from another thread, after the call has returned.
    auto from_another_thread = std::make_shared<Programmable>(
        [](Programmable& self, const neomcp::http::TokenCheck&, neomcp::http::TokenReply reply) {
            self.later([reply] {
                std::this_thread::sleep_for(100ms);
                reply(granted_now("a", {"mcp:use"}));
            });
        });
    for (const auto& validator : {answers_then_throws, answers_twice, from_another_thread}) {
        Harness harness([&](neomcp::HttpServerOptions& options) { options.authorization->validator = validator; });
        EXPECT_EQ(probe(harness, "t").status, 200);
        EXPECT_EQ(probe(harness, "t").status, 200);
        EXPECT_EQ(validator->calls.load(), 1) << "the verdict was remembered";
        EXPECT_EQ(harness.items.open_tool.load(), 2);
    }
}

TEST(ResourceServerScenarioValidator, StopAnswersTheRequestsThatWaitForAVerdictAndALateVerdictChangesNothing) {
    auto validator = std::make_shared<Programmable>(
        [](Programmable& self, const neomcp::http::TokenCheck&, neomcp::http::TokenReply reply) { self.hold(std::move(reply)); });
    Harness harness([&](neomcp::HttpServerOptions& options) {
        options.authorization->validator = validator;
        options.authorization->bounds.validation_timeout = 60s;
        options.authorization->bounds.max_pending_validations = 2;
    });
    const std::string body = rpc(1, "tools/call", modern_params({{"name", "open_tool"}, {"arguments", json::object()}}));
    Call one(harness.port, "POST", modern_headers("tools/call", "one", "open_tool"), body);
    Call two(harness.port, "POST", modern_headers("tools/call", "two", "open_tool"), body);
    const auto waiting_since = std::chrono::steady_clock::now();
    while (validator->held_count() < 2 && std::chrono::steady_clock::now() - waiting_since < patience) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(validator->held_count(), 2u);
    // The bound on tokens in validation is reached: the next new token is turned away at once.
    const auto turned_away_since = std::chrono::steady_clock::now();
    const Reply third = probe(harness, "three");
    EXPECT_EQ(third.status, 503);
    EXPECT_LT(std::chrono::steady_clock::now() - turned_away_since, 10s);
    EXPECT_FALSE(one.finished());
    EXPECT_FALSE(two.finished());

    const auto stopping_since = std::chrono::steady_clock::now();
    harness.http->stop();
    const Reply first = one.finish(20s);
    const Reply second = two.finish(20s);
    EXPECT_LT(std::chrono::steady_clock::now() - stopping_since, 30s) << "stop did not wait for the validation time (60 s)";
    EXPECT_EQ(first.status, 503);
    EXPECT_EQ(second.status, 503);
    // Verdicts that come after the stop change nothing: no request runs.
    validator->release(granted(harness.clock, "a", {"mcp:use"}));
    EXPECT_EQ(harness.items.total(), 0);
}

// ---- Where a token is read from and where it is not --------------------------------------------------------------------

TEST(ResourceServerScenarioSecret, ATokenInTheBodyTheQueryOrAnotherFieldIsNeverRead) {
    Harness harness;
    const std::string token = harness.token("good-token", granted(harness.clock, "a", {"mcp:use"}));
    const std::string message = rpc(1, "tools/call", modern_params({{"name", "open_tool"}, {"arguments", json::object()}}));
    const auto without_field = [&] { return modern_headers("tools/call", std::nullopt, "open_tool"); };
    const auto expect_unauthorized = [&](const Reply& reply, const std::string& where) {
        EXPECT_EQ(reply.status, 401) << where;
        EXPECT_EQ(reply.count("WWW-Authenticate"), 1u) << where;
    };
    // The token travels in the form body (RFC 6750, section 2.2) or the query (section 2.3).
    httplib::Headers form{{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", both_media_types}};
    expect_unauthorized(send(harness.port, "POST", form, "access_token=" + token), "form body");
    expect_unauthorized(send(harness.port, "POST", without_field(), message, std::string(endpoint_path) + "?access_token=" + token),
                        "query access_token");
    expect_unauthorized(send(harness.port, "POST", without_field(), message, std::string(endpoint_path) + "?token=" + token),
                        "query token");
    // In the message itself, or in a field that is not the Authorization field.
    json in_body = json::parse(message);
    in_body["params"]["access_token"] = token;
    in_body["params"]["_meta"]["access_token"] = token;
    expect_unauthorized(send(harness.port, "POST", without_field(), in_body.dump()), "JSON body");
    for (const char* name : {"X-Access-Token", "X-Authorization", "Proxy-Authorization", "Cookie", "Forwarded", "Token"}) {
        httplib::Headers headers = without_field();
        headers.emplace(name, std::string(name) == "Cookie" ? "access_token=" + token : "Bearer " + token);
        expect_unauthorized(send(harness.port, "POST", std::move(headers), message), name);
    }
    // GET and DELETE are no different.
    for (const char* method : {"GET", "DELETE"}) {
        httplib::Headers headers{{"Accept", "text/event-stream"}, {"X-Access-Token", token}};
        expect_unauthorized(send(harness.port, method, std::move(headers), {}, std::string(endpoint_path) + "?access_token=" + token),
                            method);
    }
    EXPECT_EQ(harness.table->calls(), 0) << "none of these reached the validator";
    EXPECT_EQ(harness.items.total(), 0);
    // The one place it is read, with the name of the field in any case.
    httplib::Headers proper = without_field();
    proper.emplace("authorization", "Bearer " + token);
    EXPECT_EQ(send(harness.port, "POST", std::move(proper), message).status, 200);
    EXPECT_EQ(harness.items.open_tool.load(), 1);
    // A second field of the same name, of any scheme, makes the request unreadable: neither may be taken.
    httplib::Headers two = without_field();
    two.emplace("Authorization", "Basic dXNlcjpwYXNz");
    two.emplace("Authorization", "Bearer " + token);
    const Reply unreadable = send(harness.port, "POST", std::move(two), message);
    EXPECT_EQ(unreadable.status, 400);
    EXPECT_EQ(challenge_of(unreadable).error, "invalid_request");
    EXPECT_EQ(harness.items.open_tool.load(), 1);
}

// ---- The raw token appears nowhere -------------------------------------------------------------------------------------

TEST(ResourceServerScenarioSecret, TheRawTokenIsInNoAnswerNoLogLineAndNoDiagnosticWhateverBecomesOfIt) {
    const std::string planted = "pLaNtEd-AcCeSs-ToKeN-0123456789abcdef";
    const std::string stem = planted.substr(0, 14);
    const std::vector<std::string> suffixes{"",       "-expired", "-audience", "-revoked",
                                            "-unavailable", "-throws", "-unknown",  "-bob"};
    std::string seen;
    // What the server's own diagnostic hook is told.
    std::mutex diagnostics_mutex;
    std::string diagnostics;
    std::set<int> statuses;
    const auto note = [&](const Reply& reply) {
        seen += reply.everything() + "\n";
        statuses.insert(reply.status);
    };
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    {
        auto table = std::make_shared<Table>();
        // A validator that throws with the token in the text of its exception: that text is its own business, and the
        // library must not carry it anywhere.
        auto validator = std::make_shared<Programmable>(
            [&](Programmable&, const neomcp::http::TokenCheck& check, neomcp::http::TokenReply reply) {
                if (std::string(check.token).ends_with("-throws")) {
                    throw std::runtime_error("cannot validate " + std::string(check.token));
                }
                table->validate(check, std::move(reply));
            });
        Harness harness(
            [&](neomcp::HttpServerOptions& options) {
                options.authorization->validator = validator;
                options.authorization->bounds.max_token_bytes = 80;
            },
            [&](neomcp::ServerOptions& options) {
                options.on_request_state_rejected = [&](const neomcp::RequestStateRejection& rejection) {
                    std::lock_guard lock(diagnostics_mutex);
                    diagnostics += std::string(neomcp::to_string(rejection.problem)) + " " + rejection.method + "\n";
                };
            });
        auto& clock = harness.clock;
        table->set(planted + "-bob", granted(clock, "bob", {"mcp:use"}));
        table->set(planted, granted(clock, "alice", {"mcp:use", "files:read"}));
        table->set(planted + "-expired", granted(clock, "alice", {"mcp:use"}, -3600s));
        table->set(planted + "-audience", granted(clock, "alice", {"mcp:use"}, 3600s, first_issuer, {"http://localhost/x"}));
        table->set(planted + "-revoked", TokenRefusal::invalid);
        table->set(planted + "-unavailable", TokenRefusal::unavailable);
        // A valid token: every kind of request, refused for its scope or served.
        note(call_tool(harness.port, planted, "whoami"));
        note(call_tool(harness.port, planted, "guarded"));
        note(modern_request(harness.port, planted, "resources/read", json{{"uri", "test://secret"}}, "test://secret"));
        note(modern_request(harness.port, planted, "resources/read", json{{"uri", "test://open"}}, "test://open"));
        note(list_tools(harness.port, planted));
        // Every way a token can fail.
        for (const std::string& suffix : suffixes) note(call_tool(harness.port, planted + suffix, "open_tool"));
        note(call_tool(harness.port, planted + std::string(60, 'x'), "open_tool"));  // over the bound
        // Credentials that cannot be read, a repeated field, another scheme.
        for (const std::vector<std::string>& fields :
             {std::vector<std::string>{"Bearer " + planted + " trailing"}, {"Bearer " + planted + ","},
              {"Bearer " + planted, "Bearer " + planted}, {"Basic " + planted}, {planted}}) {
            httplib::Headers headers = modern_headers("tools/list", std::nullopt);
            for (const std::string& field : fields) headers.emplace("Authorization", field);
            note(send(harness.port, "POST", std::move(headers), rpc(1, "tools/list", modern_params())));
        }
        // A retried call under another principal and one with a state that is no state: the diagnostic hook is told why.
        {
            const json forms{{"elicitation", json{{"form", json::object()}}}};
            const Reply asked = modern_request(harness.port, planted, "tools/call",
                                               json{{"name", "confirm"}, {"arguments", json::object()}}, "confirm", forms);
            note(asked);
            const json state = json::parse(asked.body).at("result").at("requestState");
            const auto retry = [&](const std::string& token, const json& given) {
                json params{{"name", "confirm"},
                            {"arguments", json::object()},
                            {"inputResponses", json{{"confirm", json{{"action", "accept"}, {"content", json{{"ok", true}}}}}}},
                            {"requestState", given}};
                note(modern_request(harness.port, token, "tools/call", std::move(params), "confirm", forms));
            };
            retry(planted + "-bob", state);
            retry(planted, json("not-a-sealed-state"));
        }
        // A session, its stream, a subscription, and the document that is served without a token.
        const std::string session = open_session(harness.port, planted);
        note(ping(harness.port, session, planted));
        note(session_request(harness.port, session, planted, "tools/call",
                             json{{"name", "guarded"}, {"arguments", json::object()}}));
        {
            httplib::Headers headers = legacy_headers(session, planted);
            headers.erase("Accept");
            headers.emplace("Accept", "text/event-stream");
            Call stream(harness.port, "GET", std::move(headers));
            stream.wait_for_headers();
            stream.disconnect();
            seen += stream.now().everything();
        }
        {
            Call listening(harness.port, "POST", modern_headers("subscriptions/listen", planted),
                           rpc(5, "subscriptions/listen", modern_params({{"notifications", {{"toolsListChanged", true}}}})));
            listening.wait_until([](const std::string& body) { return body.find("acknowledged") != std::string::npos; });
            listening.disconnect();
            seen += listening.now().everything();
        }
        note(send(harness.port, "DELETE", legacy_headers(session, planted)));
        httplib::Headers with_token{{"Authorization", "Bearer " + planted}};
        note(send(harness.port, "GET", std::move(with_token), {}, document_path_of_endpoint));
    }
    seen += ::testing::internal::GetCapturedStdout() + "\n" + ::testing::internal::GetCapturedStderr();
    {
        std::lock_guard lock(diagnostics_mutex);
        EXPECT_FALSE(diagnostics.empty()) << "the hook was told nothing: the scenario drove no path that reaches it";
        seen += diagnostics;
    }

    // The scan has something to find: the answers are in it.
    for (const int status : {200, 400, 401, 403, 503}) EXPECT_TRUE(statuses.count(status)) << status;
    EXPECT_NE(seen.find("alice"), std::string::npos);
    EXPECT_NE(seen.find("insufficient_scope"), std::string::npos);
    EXPECT_NE(seen.find("invalid_token"), std::string::npos);

    EXPECT_EQ(seen.find(planted), std::string::npos) << "the token itself";
    EXPECT_EQ(seen.find(stem), std::string::npos) << "the start of the token";
    EXPECT_EQ(seen.find("cannot validate"), std::string::npos) << "the text of the validator's exception";
    for (const std::string& suffix : suffixes) {
        for (const bool upper : {false, true}) {
            EXPECT_EQ(seen.find(hex_digest(planted + suffix, upper)), std::string::npos) << "its digest, " << suffix;
        }
    }
}

}  // namespace
