// The table of fixed tokens replaced while requests run, over loopback sockets. Each generation of the table gives one
// token, "shared", a subject and a scope that name the generation, and holds one token of its own that the next
// generation drops. Threads call whoami with "shared" without pause while the generations follow one another: every
// answer must name one generation in its subject and its scope alike (a request sees one table, never a part of two),
// and the generations one thread sees never go back. After each replacement has returned, the dropped token is refused
// and "shared" names the new generation: no verdict of a table that is gone is used again.
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "support/fixed_tokens_scenario.h"

namespace {

using namespace neomcp::scenario;
using neomcp::json;

constexpr int generations = 200;
constexpr int readers = 3;

std::string own_token(int generation) { return "only-" + std::to_string(generation); }

std::vector<FixedToken> generation_table(int generation) {
    return {fixed("shared", "gen-" + std::to_string(generation), {"mcp:use", "gen:" + std::to_string(generation)}),
            fixed(own_token(generation), "own-" + std::to_string(generation), {"mcp:use"})};
}

// The generation an answer of whoami names, or -1 when its subject and scopes do not name the same one.
int generation_of(const json& who) {
    const std::string subject = who.at("subject").get<std::string>();
    const std::string prefix = "gen-";
    if (subject.compare(0, prefix.size(), prefix) != 0) return -1;
    const std::string number = subject.substr(prefix.size());
    if (who.at("scopes") != json::array({"mcp:use", "gen:" + number})) return -1;
    return std::stoi(number);
}

TEST(FixedTokensReplace, RequestsDuringReplacementsSeeOneWholeTableAndNoVerdictOfAReplacedOne) {
    FixedHarness harness(generation_table(1), [](neomcp::HttpServerOptions& options) { options.worker_threads = 8; });
    std::atomic<bool> stop{false};
    std::atomic<int> served{0};
    std::atomic<bool> failed{false};  // a reader stopped on a problem
    std::mutex mutex;
    std::vector<std::string> problems;
    const auto problem = [&](std::string text) {
        std::lock_guard lock(mutex);
        problems.push_back(std::move(text));
        failed = true;
    };

    std::vector<std::thread> threads;
    for (int reader = 0; reader < readers; ++reader) {
        threads.emplace_back([&, reader] {
            int last = 0;
            while (!stop.load()) {
                const Reply reply = call_tool(harness.port, "shared", "whoami");
                if (!reply.ok || reply.status != 200) {
                    problem("reader " + std::to_string(reader) + ": status " + std::to_string(reply.status));
                    return;
                }
                const int seen = generation_of(who_of(reply));
                if (seen < 0) {
                    problem("reader " + std::to_string(reader) + ": a mix of two tables: " + reply.body);
                    return;
                }
                if (seen < last) {
                    problem("reader " + std::to_string(reader) + ": generation " + std::to_string(seen) + " after " +
                            std::to_string(last));
                    return;
                }
                last = seen;
                ++served;
            }
        });
    }

    for (int generation = 2; generation <= generations; ++generation) {
        // Let the readers run against the table before this one.
        const int before = served.load();
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (!failed.load() && served.load() < before + readers && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        if (failed.load()) break;
        // The token the next table drops is good right before it.
        EXPECT_EQ(call_tool(harness.port, own_token(generation - 1), "whoami").status, 200) << generation;
        harness.replace(generation_table(generation));
        const Reply dropped = call_tool(harness.port, own_token(generation - 1), "whoami");
        EXPECT_EQ(dropped.status, 401) << "the token of generation " << generation - 1 << " is still accepted";
        EXPECT_EQ(call_tool(harness.port, own_token(generation), "whoami").status, 200) << generation;
        // No ASSERT here: the readers must be joined whatever happens.
        const Reply shared = call_tool(harness.port, "shared", "whoami");
        if (shared.status == 200) {
            EXPECT_EQ(generation_of(who_of(shared)), generation) << "a grant of a replaced table: " << shared.body;
        } else {
            ADD_FAILURE() << "shared answered " << shared.status << " at generation " << generation;
        }
    }
    stop = true;
    for (std::thread& thread : threads) thread.join();
    EXPECT_TRUE(problems.empty()) << problems.front();
    EXPECT_GE(served.load(), readers * (generations - 1));
}

}  // namespace
