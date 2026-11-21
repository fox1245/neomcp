// What the client refuses in the URL the browser comes back to, over loopback against the scripted authorization
// server and resource: another state or none, an issuer that is wrong, missing where the server says it sends one, or
// encoded twice, a parameter that occurs twice, another path or port than the redirect URI, an error with a long
// description, and a URL over its bound. Each refusal ends the sign-in before a token is asked for, and no error and no
// diagnostic repeats a code, a state, a description or anything else of the URL's query.
#include <gtest/gtest.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "neomcp/http/form.h"
#include "oauth_rig.h"

namespace {

using namespace oauth_rig;
using neomcp::AuthorizationProblem;
using neomcp::ClientErrorKind;
using neomcp::http::FormField;

// What the person's browser came back with, made from the prompt: the parameters of the query, and where it came back
// to (the redirect URI when empty).
struct Answer {
    std::vector<FormField> fields;
    std::string base;
};

// The state of the request, and a code the authorization server would exchange.
struct Issued {
    std::string state;
    std::string code;
};

using MakeAnswer = std::function<Answer(const neomcp::AuthorizationPromptInfo&, const Issued&)>;

struct Case {
    Case(std::string name_in, bool advertise_iss_in, MakeAnswer make_in,
         AuthorizationProblem problem_in = AuthorizationProblem::state_mismatch,
         std::optional<std::string> error_code_in = std::nullopt)
        : name(std::move(name_in)),
          advertise_iss(advertise_iss_in),
          make(std::move(make_in)),
          problem(problem_in),
          error_code(std::move(error_code_in)) {}

    std::string name;
    bool advertise_iss = false;
    MakeAnswer make;
    AuthorizationProblem problem = AuthorizationProblem::state_mismatch;
    std::optional<std::string> error_code;
};

std::string redirect_with_port(std::string_view port) {
    return "http://127.0.0.1:" + std::string(port) + "/callback";
}

// Runs one sign-in whose browser answer `make` writes, and checks the refusal.
void run_refused(const Case& one) {
    SCOPED_TRACE(one.name);
    World world;
    if (one.advertise_iss) {
        world.as.edit_metadata([](json& metadata) { metadata["authorization_response_iss_parameter_supported"] = true; });
    }
    std::vector<std::string> extra;
    std::mutex extra_mutex;
    world.person.answer_with([&](const neomcp::AuthorizationPromptInfo& info) {
        Issued issued;
        issued.state = field_of(query_of(info.authorization_url), "state").value_or(std::string());
        issued.code = world.as.issue_code({});
        Answer answer = one.make(info, issued);
        const std::string base = answer.base.empty() ? info.redirect_uri : answer.base;
        const std::string url = base + "?" + neomcp::http::form_encode(answer.fields);
        std::lock_guard lock(extra_mutex);
        extra.push_back(neomcp::detail::oauth::read_url(url) ? query_of(url) : url);
        return url;
    });
    auto client = world.open(world.authorizer());
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().kind, ClientErrorKind::authorization_failed);
    ASSERT_TRUE(client.error().authorization);
    EXPECT_EQ(client.error().authorization->problem, one.problem);
    EXPECT_EQ(client.error().authorization->error_code, one.error_code);
    EXPECT_EQ(world.person.prompts().size(), 1u);
    EXPECT_TRUE(world.as.requests_to("/token").empty()) << "a token was asked for after the refusal";
    std::vector<std::string> secrets = world.secrets();
    {
        std::lock_guard lock(extra_mutex);
        secrets.insert(secrets.end(), extra.begin(), extra.end());
    }
    expect_quiet(world, {client.error()}, secrets);
}

std::vector<FormField> code_and_state(const Issued& issued) { return {{"code", issued.code}, {"state", issued.state}}; }

std::string long_description() {
    std::string text;
    while (text.size() < 8 * 1024) text += std::string(planted_description) + " ";
    return text;
}

// ---- State

TEST(OAuthSecurity, ACallbackWithAnotherStateOrNoneIsRefused) {
    run_refused({"another state", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{{{"code", issued.code}, {"state", issued.state + "x"}}, {}};
                 }});
    run_refused({"no state", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{{{"code", issued.code}}, {}};
                 }});
    run_refused({"an empty state", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{{{"code", issued.code}, {"state", ""}}, {}};
                 }});
}

// ---- A parameter twice

TEST(OAuthSecurity, ACallbackWithAParameterTwiceIsRefused) {
    for (const char* name : {"state", "code", "iss", "error"}) {
        run_refused({std::string("twice: ") + name, false,
                     [name](const neomcp::AuthorizationPromptInfo& info, const Issued& issued) {
                         std::vector<FormField> fields = code_and_state(issued);
                         const std::string value = std::string(name) == "state" ? issued.state
                                                   : std::string(name) == "code" ? issued.code
                                                   : std::string(name) == "iss"  ? info.issuer
                                                                                 : std::string("access_denied");
                         fields.push_back({name, value});
                         fields.push_back({name, value});
                         return Answer{fields, {}};
                     }});
    }
}

// ---- The issuer

TEST(OAuthSecurity, ACallbackWithoutTheIssuerTheServerSaysItSendsIsRefused) {
    run_refused({"advertised, missing", true,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{code_and_state(issued), {}};
                 },
                 AuthorizationProblem::issuer_mismatch});
}

TEST(OAuthSecurity, ACallbackNamingAnotherIssuerIsRefused) {
    struct Variant {
        const char* name;
        bool advertise;
        std::function<std::string(const std::string&)> issuer;
    };
    const std::vector<Variant> variants = {
        {"advertised, with a slash", true, [](const std::string& issuer) { return issuer + "/"; }},
        {"advertised, another server", true, [](const std::string&) { return std::string("https://as.example"); }},
        {"not advertised, another server", false,
         [](const std::string&) { return std::string("https://as.example"); }},
        {"advertised, scheme in capitals", true,
         [](const std::string& issuer) { return "HTTP" + issuer.substr(std::string("http").size()); }},
        {"advertised, encoded twice", true,
         [](const std::string& issuer) { return neomcp::http::form_encode_component(issuer); }},
        {"advertised, empty", true, [](const std::string&) { return std::string(); }},
    };
    for (const Variant& variant : variants) {
        run_refused({variant.name, variant.advertise,
                     [&variant](const neomcp::AuthorizationPromptInfo& info, const Issued& issued) {
                         std::vector<FormField> fields = code_and_state(issued);
                         fields.push_back({"iss", variant.issuer(info.issuer)});
                         return Answer{fields, {}};
                     },
                     AuthorizationProblem::issuer_mismatch});
    }
}

// The control of the cases above: the issuer as the server's metadata names it is taken.
TEST(OAuthSecurity, ACallbackNamingTheIssuerIsTaken) {
    World world;
    world.as.edit_metadata([](json& metadata) { metadata["authorization_response_iss_parameter_supported"] = true; });
    world.person.answer_with([&](const neomcp::AuthorizationPromptInfo& info) {
        std::vector<FormField> fields = {{"code", world.as.issue_code({})},
                                         {"state", field_of(query_of(info.authorization_url), "state").value_or("")},
                                         {"iss", info.issuer}};
        return info.redirect_uri + "?" + neomcp::http::form_encode(fields);
    });
    auto client = world.open(world.authorizer());
    ASSERT_TRUE(client) << client.error().message;
    EXPECT_EQ(world.as.requests_to("/token").size(), 1u);
    EXPECT_TRUE(client->close());
}

// ---- Where the browser came back to

TEST(OAuthSecurity, ACallbackToAnotherPathOrPortIsRefused) {
    run_refused({"another path", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{code_and_state(issued), "http://127.0.0.1:3000/elsewhere"};
                 }});
    run_refused({"another port", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{code_and_state(issued), redirect_with_port("3001")};
                 }});
    run_refused({"another scheme", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{code_and_state(issued), "https://127.0.0.1:3000/callback"};
                 }});
}

// ---- An error

// The error code is the one thing of an error answer the application is told; its description and URI go nowhere.
TEST(OAuthSecurity, ACallbackWithAnErrorTellsTheCodeOnly) {
    run_refused({"an error with a long description", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{{{"state", issued.state},
                                    {"error", "access_denied"},
                                    {"error_description", long_description()},
                                    {"error_uri", "https://as.example/why?" + std::string(planted_description)}},
                                   {}};
                 },
                 AuthorizationProblem::authorization_denied, std::string("access_denied")});
    // An error that is not a code (a quote, a line end) is not repeated either.
    run_refused({"an error that is not a code", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{{{"state", issued.state}, {"error", "denied\"\r\n" + std::string(planted_secret)}},
                                   {}};
                 },
                 AuthorizationProblem::authorization_denied});
    run_refused({"neither code nor error", false,
                 [](const neomcp::AuthorizationPromptInfo&, const Issued& issued) {
                     return Answer{{{"state", issued.state}}, {}};
                 },
                 AuthorizationProblem::authorization_denied});
}

// ---- The bound of the URL

// A callback of exactly max_callback_bytes is read; one byte more is refused before it is read.
TEST(OAuthSecurity, ACallbackAtItsBoundIsTakenAndOneByteMoreIsRefused) {
    constexpr std::size_t bound = 1024;
    for (const std::size_t extra : {std::size_t{0}, std::size_t{1}}) {
        SCOPED_TRACE(extra);
        World world;
        neomcp::OAuthOptions options = world.options();
        options.bounds.max_callback_bytes = bound;
        world.person.answer_with([&](const neomcp::AuthorizationPromptInfo& info) {
            std::vector<FormField> fields = {{"code", world.as.issue_code({})},
                                             {"state", field_of(query_of(info.authorization_url), "state").value_or("")},
                                             {"pad", ""}};
            std::string url = info.redirect_uri + "?" + neomcp::http::form_encode(fields);
            url.append(bound + extra - url.size(), 'p');
            return url;
        });
        auto client = world.open(world.authorizer(std::move(options)));
        if (extra == 0) {
            ASSERT_TRUE(client) << client.error().message;
            EXPECT_EQ(world.as.requests_to("/token").size(), 1u);
            EXPECT_TRUE(client->close());
        } else {
            ASSERT_FALSE(client);
            ASSERT_TRUE(client.error().authorization);
            EXPECT_EQ(client.error().authorization->problem, AuthorizationProblem::state_mismatch);
            EXPECT_TRUE(world.as.requests_to("/token").empty());
            expect_quiet(world, {client.error()}, world.secrets());
        }
    }
}

}  // namespace
