// Tests of the core's authorization pieces from tables: scope-tokens, the closure of declared implications, the
// missing scopes of a requirement, the principal and its digest in sealed request state; and a session of a plain
// host that declares scopes, fed units with and without an authorization.
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "core/request_state.h"
#include "neomcp/authorization.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/session.h"
#include "neomcp/testing/peer.h"

namespace {

using neomcp::json;
using Scopes = std::vector<std::string>;

TEST(ScopeToken, IsTheCharacterSetOfRfc6749) {
    for (const char* good : {"a", "files:read", "!", "~", "#$%&'()*+,-./:;<=>?@[]^_`{|}"}) {
        EXPECT_TRUE(neomcp::is_scope_token(good)) << good;
    }
    for (const char* bad : {"", " ", "a b", "\"", "\\", "\t", "\x7F", "caf\xC3\xA9"}) {
        EXPECT_FALSE(neomcp::is_scope_token(bad)) << bad;
    }
}

TEST(ScopeImplications, AreClosedThroughEveryStepAndCyclesEnd) {
    const neomcp::ScopeImplications implications({{"a", {"b"}}, {"b", {"c"}}, {"c", {"d"}}, {"x", {"y"}}, {"y", {"x"}}});
    EXPECT_FALSE(implications.empty());
    const auto from_a = implications.implied_by("a");
    EXPECT_EQ(Scopes(from_a.begin(), from_a.end()), (Scopes{"b", "c", "d"}));
    const auto from_x = implications.implied_by("x");
    EXPECT_EQ(Scopes(from_x.begin(), from_x.end()), (Scopes{"x", "y"}));
    EXPECT_TRUE(implications.implied_by("d").empty());

    const Scopes granted{"a"};
    for (const char* met : {"a", "b", "c", "d"}) EXPECT_TRUE(implications.met(granted, met)) << met;
    EXPECT_FALSE(implications.met(granted, "x"));
    EXPECT_TRUE(implications.met(Scopes{"y"}, "x"));
    EXPECT_FALSE(implications.met(Scopes{"d"}, "a"));  // implications go one way
}

TEST(ScopeImplications, ReadNothingFromTheTextOfAScope) {
    const neomcp::ScopeImplications implications({{"files:*", {"files:list"}}});
    const Scopes granted{"files:*", "files"};
    EXPECT_TRUE(implications.met(granted, "files:list"));
    for (const char* undeclared : {"files:read", "files:", "file", "FILES:*", "files:*:x"}) {
        EXPECT_FALSE(implications.met(granted, undeclared)) << undeclared;
    }
    EXPECT_FALSE(neomcp::ScopeImplications().met(granted, "files:read"));
    EXPECT_TRUE(neomcp::ScopeImplications().empty());
}

TEST(ScopeImplications, RefuseWhatIsNotAScopeToken) {
    EXPECT_THROW(neomcp::ScopeImplications({{"a b", {"c"}}}), std::invalid_argument);
    EXPECT_THROW(neomcp::ScopeImplications({{"a", {"c", ""}}}), std::invalid_argument);
    EXPECT_NO_THROW(neomcp::ScopeImplications({{"a", {}}}));
}

TEST(MissingScopes, AreTheUnmetOnesInTheOrderOfTheRequirementEachOnce) {
    const neomcp::ScopeImplications implications({{"admin", {"write"}}});
    EXPECT_EQ(neomcp::missing_scopes(Scopes{"admin"}, Scopes{"z", "write", "a", "z", "admin"}, implications),
              (Scopes{"z", "a"}));
    EXPECT_TRUE(neomcp::missing_scopes(Scopes{"admin"}, Scopes{}, implications).empty());
    EXPECT_EQ(neomcp::missing_scopes(Scopes{}, Scopes{"write"}, implications), Scopes{"write"});
}

neomcp::Authorization who(std::string issuer, std::optional<std::string> subject, std::optional<std::string> client) {
    neomcp::Authorization made;
    made.issuer = std::move(issuer);
    made.subject = std::move(subject);
    made.client_id = std::move(client);
    return made;
}

TEST(Principal, OneTextPerKeyWhateverThePartsHold) {
    const auto text = [](std::string issuer, std::string subject, std::string client) {
        return neomcp::PrincipalKey{std::move(issuer), std::move(subject), std::move(client)}.text();
    };
    // Parts that would run together joined plainly.
    EXPECT_NE(text("ab", "c", ""), text("a", "bc", ""));
    EXPECT_NE(text("a", "", "b"), text("a", "b", ""));
    EXPECT_NE(text("1:a", "", ""), text("", "a", ""));
    EXPECT_NE(text("a:0:", "", ""), text("a", "", ""));
    EXPECT_EQ(text("i", "s", "c"), text("i", "s", "c"));

    EXPECT_EQ(neomcp::principal_of(who("i", std::nullopt, std::nullopt)), (neomcp::PrincipalKey{"i", "", ""}));
    EXPECT_EQ(neomcp::principal_of(who("i", "s", "c")), (neomcp::PrincipalKey{"i", "s", "c"}));
}

TEST(Principal, ItsDigestBindsAStateWithoutCarryingWhoItIs) {
    EXPECT_TRUE(neomcp::detail::principal_digest(nullptr).empty());
    neomcp::Authorization first = who("https://as", "alice", "c1");
    neomcp::Authorization refreshed = first;
    refreshed.scopes = {"more"};
    refreshed.expires = std::chrono::system_clock::now();
    const std::string digest = neomcp::detail::principal_digest(&first);
    EXPECT_FALSE(digest.empty());
    EXPECT_EQ(digest.find("alice"), std::string::npos);
    EXPECT_EQ(neomcp::detail::principal_digest(&refreshed), digest);
    for (const neomcp::Authorization& other : {who("https://as", "bob", "c1"), who("https://as", "alice", "c2"),
                                               who("https://other", "alice", "c1"), who("https://as", std::nullopt, "c1")}) {
        EXPECT_NE(neomcp::detail::principal_digest(&other), digest);
    }

    neomcp::detail::StateClaims claims;
    claims.expires = 2'000'000'000;
    claims.method = "tools/call";
    claims.subject = "t";
    claims.principal = digest;
    const auto at = std::chrono::system_clock::time_point(std::chrono::seconds(1'000'000'000));
    const std::string other = neomcp::detail::principal_digest(&refreshed);
    EXPECT_FALSE(neomcp::detail::check_claims(claims, {"tools/call", "t", "", "", other}, at));
    EXPECT_EQ(neomcp::detail::check_claims(claims, {"tools/call", "t", "", "", ""}, at),
              neomcp::RequestStateProblem::binding);
}

// A host of tools only that declares scopes for the tool at index 1.
class ScopedTools final : public neomcp::ToolHost {
public:
    ScopedTools() {
        for (const char* name : {"free", "guarded"}) {
            neomcp::ToolDefinition tool;
            tool.name = name;
            tool.input_schema = json{{"type", "object"}};
            tools_.push_back(std::move(tool));
        }
    }
    std::span<const neomcp::ToolDefinition> tools() const override { return tools_; }
    std::span<const std::string> tool_scopes(std::size_t index) const override {
        if (index == 1) return guarded_scopes_;
        return {};
    }
    void call(const neomcp::CallRequest& request, neomcp::CallControl control, neomcp::Completion done) override {
        {
            const std::lock_guard lock(mutex);
            called.push_back(request.name);
            seen.push_back(control.authorization() == nullptr ? std::string() : control.authorization()->issuer);
        }
        done(neomcp::CallOutcome(neomcp::ToolResult{}));
    }

    std::mutex mutex;
    std::vector<std::string> called;
    std::vector<std::string> seen;

private:
    std::vector<neomcp::ToolDefinition> tools_;
    Scopes guarded_scopes_{"guard"};
};

TEST(SessionScopes, AreCheckedOnlyForUnitsThatCameWithAnAuthorization) {
    ScopedTools host;
    std::mutex mutex;
    std::map<neomcp::UnitId, neomcp::Output> last;
    neomcp::SessionOptions options;
    options.scope_implications = neomcp::ScopeImplications({{"root", {"guard"}}});
    auto made = neomcp::Session::create_tagged(std::move(options), host, [&](neomcp::Output output) {
        const std::lock_guard lock(mutex);
        if (output.unit && output.last) last.emplace(*output.unit, std::move(output));
    });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    auto& session = *std::get<std::unique_ptr<neomcp::Session>>(made);
    const auto modern = neomcp::profile::builtin("2026-07-28");
    ASSERT_NE(modern, nullptr);
    const auto call = [&](int id, const char* name) {
        return neomcp::serialize(neomcp::testing::Peer::make_request(*modern, id, "tools/call", json{{"name", name}}));
    };
    const auto granted = [](Scopes scopes) {
        auto made_grant = std::make_shared<neomcp::Authorization>(who("https://as", "s", "c"));
        made_grant->scopes = std::move(scopes);
        return std::shared_ptr<const neomcp::Authorization>(std::move(made_grant));
    };
    // The host answers inline, so every unit has ended when receive() returns.
    session.receive(1, call(1, "guarded"));
    session.receive(2, call(2, "guarded"), {}, granted({}));
    session.receive(3, call(3, "free"), {}, granted({}));
    session.receive(4, call(4, "guarded"), {}, granted({"root"}));
    const std::lock_guard lock(mutex);
    ASSERT_EQ(last.size(), 4u);
    EXPECT_EQ(last.at(1).refusal, neomcp::RefusalReason::none);
    EXPECT_EQ(last.at(2).refusal, neomcp::RefusalReason::insufficient_scope);
    EXPECT_EQ(last.at(2).required_scopes, Scopes{"guard"});
    EXPECT_EQ(json::parse(last.at(2).line).at("id"), 2);
    EXPECT_EQ(last.at(3).refusal, neomcp::RefusalReason::none);
    EXPECT_EQ(last.at(4).refusal, neomcp::RefusalReason::none);
    const std::lock_guard host_lock(host.mutex);
    EXPECT_EQ(host.called, (Scopes{"guarded", "free", "guarded"}));
    EXPECT_EQ(host.seen, (Scopes{"", "https://as", "https://as"}));
}

}  // namespace
