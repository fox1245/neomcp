// Tests of the protocol core through the in-memory peer.
//
// Expected per-revision values are written here from the specification pages and schemas of each revision, not
// read back from the profiles, so that a wrong profile makes a test fail. Every wait is on an event a test
// controls; "nothing was written" is shown by a later request whose answer arrives (the session writes in order).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "neomcp/jsonrpc.h"
#include "neomcp/mechanism.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/session.h"
#include "neomcp/testing/peer.h"
#include "neomcp/tool_host.h"
#include "neomcp/wire.h"

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

// Long enough for a sanitizer build on a loaded machine; reached only when something is broken.
constexpr std::chrono::milliseconds patience = 30s;
constexpr std::chrono::milliseconds now = 0ms;

// The revisions and their era, from the specification (versioning.mdx of 2026-07-28: 2025-11-25 and earlier are
// legacy).
const std::vector<std::string> all_revisions{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"};
const std::vector<std::string> legacy_revisions{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
const std::vector<std::string> newest_first{"2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* batch_revision = "2025-03-26";
constexpr const char* version_key = "io.modelcontextprotocol/protocolVersion";
constexpr const char* capabilities_key = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* log_level_key = "io.modelcontextprotocol/logLevel";
constexpr const char* server_info_key = "io.modelcontextprotocol/serverInfo";

bool is_legacy(const std::string& revision) {
    return std::find(legacy_revisions.begin(), legacy_revisions.end(), revision) != legacy_revisions.end();
}

profile::ProfilePtr builtin(const std::string& revision) {
    profile::ProfilePtr found = profile::builtin(revision);
    if (!found) throw std::logic_error("no built-in profile " + revision);
    return found;
}

std::set<std::string> keys(const json& value) {
    std::set<std::string> out;
    if (const auto* members = value.get_ptr<const json::object_t*>()) {
        for (const auto& entry : *members) out.insert(entry.first);
    }
    return out;
}

json strings(const std::vector<std::string>& list) {
    json out = json::array();
    for (const auto& item : list) out.push_back(item);
    return out;
}

std::string name_of(const ::testing::TestParamInfo<std::string>& info) {
    std::string name = info.param;
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

// ---------------------------------------------------------------------------------------------------------------
// A tool host with tools that answer at once, wait for the test, report progress or log.

neomcp::ContentBlock text_block(std::string text) {
    neomcp::TextContent block;
    block.text = std::move(text);
    return block;
}

neomcp::ToolResult text_result(std::string text) {
    neomcp::ToolResult result;
    result.content.push_back(text_block(std::move(text)));
    return result;
}

neomcp::Icon icon(std::string src) {
    return neomcp::Icon{std::move(src), "image/png", {"48x48", "any"}, neomcp::IconTheme::dark};
}

// A tool with every member of the model set.
neomcp::ToolDefinition everything_tool() {
    neomcp::ToolDefinition tool;
    tool.name = "everything";
    tool.title = "Everything";
    tool.description = "a tool with every member set";
    tool.input_schema = json{{"type", "object"}, {"properties", json::object()}};
    tool.output_schema = json{{"type", "object"}};
    tool.annotations = neomcp::ToolAnnotations{"Everything", true, false, true, false};
    tool.icons = {icon("https://example.com/tool.png")};
    tool.meta = json{{"k", "v"}};
    return tool;
}

// A result with structured content and a block of every kind, each with its optional members set.
neomcp::ToolResult full_result() {
    neomcp::TextContent text;
    text.text = "hello";
    text.annotations = neomcp::ContentAnnotations{{neomcp::Role::user}, 0.5, "2025-01-12T15:00:58Z"};
    text.meta = json{{"k", "v"}};
    neomcp::ImageContent image;
    image.data = "aW1hZ2U=";
    image.mime_type = "image/png";
    image.extra = json{{"_meta", json{{"from", "extra"}}}, {"futureMember", true}};
    neomcp::AudioContent audio;
    audio.data = "YXVkaW8=";
    audio.mime_type = "audio/wav";
    neomcp::ResourceLink link;
    link.uri = "file:///a.txt";
    link.name = "a.txt";
    link.title = "A";
    link.description = "a file";
    link.mime_type = "text/plain";
    link.size = 42;
    link.icons = {icon("https://example.com/link.png")};
    link.annotations = neomcp::ContentAnnotations{{neomcp::Role::assistant}, std::nullopt, std::nullopt};
    link.meta = json{{"k", "v"}};
    neomcp::EmbeddedResource embedded;
    embedded.resource = neomcp::TextResourceContents{"file:///b.txt", "text/plain", "body", json{{"k", "v"}}};
    neomcp::ToolResult result;
    result.content = {text, image, audio, link, embedded};
    result.structured_content = json{{"answer", 42}};
    result.meta = json{{"trace", "t1"}};
    return result;
}

class FakeHost : public neomcp::ToolHost {
public:
    struct Parked {
        neomcp::CallRequest request;
        neomcp::CallControl control;
        neomcp::Completion done;
    };

    FakeHost() {
        for (const char* name : {"echo", "gated", "progress", "busy", "full", "logger", "throws", "dropper"}) {
            neomcp::ToolDefinition tool;
            tool.name = name;
            tool.description = std::string("the ") + name + " tool";
            tools_.push_back(std::move(tool));
        }
        tools_.push_back(everything_tool());
    }

    std::span<const neomcp::ToolDefinition> tools() const override { return tools_; }

    void call(const neomcp::CallRequest& request, neomcp::CallControl control, neomcp::Completion done) override {
        const std::string& name = request.name;
        if (name == "echo") {
            done(text_result(request.arguments.dump()));
        } else if (name == "gated") {
            const std::lock_guard<std::mutex> lock(mutex_);
            parked_.push_back(Parked{request, control, done});
            changed_.notify_all();
        } else if (name == "progress") {
            control.progress(1, 3, "one");
            control.progress(1, 3, "one again");  // not greater: dropped
            control.progress(2, 3, "two");
            control.progress(0.5);                // smaller: dropped
            control.progress(std::nan(""));       // not finite: dropped
            control.progress(3, 3, "three");
            done(text_result("done"));
            control.progress(4, 4, "after the response");  // dropped
        } else if (name == "busy") {
            done(neomcp::Refusal::busy);
        } else if (name == "full") {
            done(full_result());
        } else if (name == "logger") {
            for (const auto level : {neomcp::LogLevel::debug, neomcp::LogLevel::info, neomcp::LogLevel::warning,
                                     neomcp::LogLevel::error}) {
                control.log(level, std::string(neomcp::to_string(level)), "test");
            }
            done(text_result("logged"));
        } else if (name == "throws") {
            throw std::runtime_error("the host failed");
        } else if (name == "dropper") {
            // The completion is dropped without being called.
        }
    }

    std::optional<Parked> wait_parked(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return !parked_.empty(); })) return std::nullopt;
        Parked parked = std::move(parked_.front());
        parked_.pop_front();
        return parked;
    }

private:
    std::vector<neomcp::ToolDefinition> tools_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Parked> parked_;
};

// Opens once, for threads that must start together.
class Gate {
public:
    void open() {
        const std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

neomcp::SessionOptions test_options() {
    neomcp::SessionOptions options;
    options.server_info = neomcp::Implementation{"test-server", "1.2.3", "Test Server", "a server for tests",
                                                 "https://example.com", {icon("https://example.com/server.png")}};
    options.instructions = "use the tools";
    return options;
}

std::vector<profile::ProfilePtr> profiles_of(const std::vector<std::string>& revisions) {
    std::vector<profile::ProfilePtr> out;
    for (const auto& revision : revisions) out.push_back(builtin(revision));
    return out;
}

// One client speaking one revision: a handshake for a legacy revision, nothing for the modern one.
struct Conversation {
    explicit Conversation(const std::string& revision, neomcp::SessionOptions options = test_options())
        : peer(host, std::move(options)), profile(builtin(revision)) {
        if (is_legacy(revision)) {
            const auto opened = peer.initialize(*profile, patience);
            EXPECT_TRUE(opened && opened->contains("result")) << (opened ? opened->dump() : "no answer");
        }
    }

    json request(std::string method, json params = json::object()) {
        return Peer::make_request(*profile, neomcp::RequestId(next_id++), std::move(method), std::move(params));
    }

    json ask(std::string method, json params = json::object()) {
        const auto answer = peer.request(request(std::move(method), std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer within the patience");
        return *answer;
    }

    // An answer to a request sent after everything the test did, so that whatever those things wrote has been
    // written before it.
    json flush() {
        const profile::Message* ping = profile->request("ping");
        return ask(ping != nullptr ? "ping" : "server/discover");
    }

    FakeHost host;
    Peer peer;
    profile::ProfilePtr profile;
    int next_id = 1;
};

const json& result_of(const json& answer) { return answer.at("result"); }

int error_code_of(const json& answer) {
    if (!answer.contains("error")) return 0;
    return answer.at("error").at("code").get<int>();
}

std::string error_message_of(const json& answer) { return answer.at("error").at("message").get<std::string>(); }

json cancelled(const json& id) {
    return json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", id}, {"reason", "test"}}}};
}

class PerRevision : public ::testing::TestWithParam<std::string> {};
class PerLegacyRevision : public ::testing::TestWithParam<std::string> {};

INSTANTIATE_TEST_SUITE_P(Revisions, PerRevision, ::testing::ValuesIn(all_revisions), name_of);
INSTANTIATE_TEST_SUITE_P(Revisions, PerLegacyRevision, ::testing::ValuesIn(legacy_revisions), name_of);

// ---------------------------------------------------------------------------------------------------------------
// JSON-RPC

TEST(JsonRpc, RequestIdsCompareAcrossIntegerTypes) {
    EXPECT_EQ(neomcp::RequestId(5), neomcp::RequestId(std::uint64_t{5}));
    EXPECT_EQ(neomcp::RequestId::from_json(json(5)), neomcp::RequestId(5));
    EXPECT_EQ(neomcp::RequestId::from_json(json::parse("5")), neomcp::RequestId(5));
    EXPECT_FALSE(neomcp::RequestId(5) == neomcp::RequestId("5"));
    EXPECT_FALSE(neomcp::RequestId::from_json(json(1.5)));
    EXPECT_FALSE(neomcp::RequestId::from_json(json(true)));
    EXPECT_FALSE(neomcp::RequestId::from_json(json(nullptr)));
    EXPECT_EQ(neomcp::RequestId(-5).to_json().dump(), "-5");
    EXPECT_EQ(neomcp::RequestId(std::uint64_t{18446744073709551615u}).to_json().dump(), "18446744073709551615");
}

TEST(JsonRpc, Classify) {
    using neomcp::MessageKind;
    EXPECT_EQ(neomcp::classify(json::parse(R"({"jsonrpc":"2.0","id":1,"method":"ping"})")).kind, MessageKind::request);
    EXPECT_EQ(neomcp::classify(json::parse(R"({"jsonrpc":"2.0","method":"x"})")).kind, MessageKind::notification);
    EXPECT_EQ(neomcp::classify(json::parse(R"({"jsonrpc":"2.0","id":1,"result":{}})")).kind, MessageKind::response);
    EXPECT_EQ(neomcp::classify(json::parse(R"({"jsonrpc":"2.0","id":null,"error":{}})")).kind, MessageKind::response);
    const auto wrong_version = neomcp::classify(json::parse(R"({"jsonrpc":"1.0","id":7,"method":"ping"})"));
    EXPECT_EQ(wrong_version.kind, MessageKind::invalid);
    EXPECT_EQ(wrong_version.id, neomcp::RequestId(7));
    EXPECT_EQ(neomcp::classify(json::parse(R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})")).kind, MessageKind::invalid);
    EXPECT_EQ(neomcp::classify(json::parse(R"({"jsonrpc":"2.0","id":1,"method":"x","params":3})")).kind,
              MessageKind::invalid);
    EXPECT_EQ(neomcp::classify(json::parse("[1]")).kind, MessageKind::invalid);
    EXPECT_EQ(neomcp::classify(json::parse(R"({"jsonrpc":"2.0","id":1})")).kind, MessageKind::invalid);
}

TEST(JsonRpc, NestingDepth) {
    EXPECT_FALSE(neomcp::nesting_exceeds("[[1]]", 2));
    EXPECT_TRUE(neomcp::nesting_exceeds("[[[1]]]", 2));
    EXPECT_FALSE(neomcp::nesting_exceeds(R"(["[[[[\"[[["])", 1)) << "brackets inside strings do not count";
    EXPECT_FALSE(neomcp::nesting_exceeds("[][][][]", 1));
}

TEST(Wire, KeepMembersLeavesValuesOfUnknownTypes) {
    const auto latest = builtin(modern_revision);
    json value = json{{"a", 1}};
    EXPECT_FALSE(neomcp::keep_members(value, "NoSuchType", *latest));
    EXPECT_EQ(keys(value), (std::set<std::string>{"a"}));
    json tool = json{{"name", "x"}, {"inputSchema", json::object()}, {"bogus", 1}};
    EXPECT_TRUE(neomcp::keep_members(tool, "Tool", *latest));
    EXPECT_EQ(keys(tool), (std::set<std::string>{"inputSchema", "name"}));
}

// ---------------------------------------------------------------------------------------------------------------
// Creation and mechanisms

TEST(Creation, EveryBuiltinProfileCanBeServedAloneAndTogether) {
    FakeHost host;
    for (const auto revision : profile::builtin_revisions()) {
        SCOPED_TRACE(std::string(revision));
        neomcp::SessionOptions options = test_options();
        options.profiles = {profile::builtin(revision)};
        auto made = neomcp::Session::create(options, host, [](std::string) {});
        const auto* error = std::get_if<neomcp::CreateError>(&made);
        EXPECT_EQ(error, nullptr) << (error != nullptr ? error->message : "");
    }
    auto made = neomcp::Session::create(test_options(), host, [](std::string) {});
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made))
        << std::get<neomcp::CreateError>(made).message;
    const auto& session = std::get<std::unique_ptr<neomcp::Session>>(made);
    std::vector<std::string> enabled;
    for (const auto& entry : session->profiles()) enabled.push_back(entry->revision());
    EXPECT_EQ(enabled, newest_first);
}

TEST(Creation, BuiltinRegistryImplementsEveryDeclaredName) {
    const neomcp::MechanismRegistry& registry = neomcp::builtin_mechanisms();
    const std::vector<std::pair<profile::Slot, neomcp::MechanismSlot>> slots{
        {profile::Slot::open, neomcp::MechanismSlot::open},
        {profile::Slot::state, neomcp::MechanismSlot::state},
        {profile::Slot::batch, neomcp::MechanismSlot::batch}};
    for (const auto& [declared, slot] : slots) {
        for (const auto name : profile::declared_mechanisms(declared)) {
            EXPECT_TRUE(registry.contains(slot, name)) << neomcp::to_string(slot) << " " << name;
        }
    }
    for (const auto transport : {profile::Transport::stdio, profile::Transport::http_sse,
                                 profile::Transport::streamable_http}) {
        for (const auto name : profile::declared_cancel(transport)) {
            EXPECT_TRUE(registry.contains(neomcp::MechanismSlot::cancel, name)) << name;
        }
    }
    EXPECT_TRUE(registry.make_open("request_meta"));
    EXPECT_FALSE(registry.make_open("no_such_mechanism"));
    EXPECT_FALSE(registry.make_state("request_meta")) << "a name of another slot";
}

TEST(Creation, RequestMetaVersionKeyIsRequiredWhereItIsUsed) {
    // The version key is a constant of the request_meta mechanism; every profile that names the mechanism must
    // require it, or the mechanism refuses the profile.
    const auto opener = neomcp::builtin_mechanisms().make_open("request_meta");
    ASSERT_TRUE(opener);
    for (const auto revision : profile::builtin_revisions()) {
        const auto entry = profile::builtin(revision);
        if (entry->mechanism(profile::Slot::open) != "request_meta") continue;
        const profile::Type* meta = entry->type("RequestMetaObject");
        ASSERT_NE(meta, nullptr) << revision;
        EXPECT_TRUE(meta->is_required(version_key)) << revision;
        EXPECT_FALSE(opener->check(*entry)) << revision;
    }
}

TEST(Creation, MissingMechanismsAreReportedByName) {
    neomcp::MechanismRegistry registry = neomcp::builtin_mechanisms();
    ASSERT_TRUE(registry.remove(neomcp::MechanismSlot::open, "request_meta"));
    ASSERT_TRUE(registry.remove(neomcp::MechanismSlot::batch, "array"));
    FakeHost host;
    neomcp::SessionOptions options = test_options();
    options.mechanisms = &registry;
    auto made = neomcp::Session::create(options, host, [](std::string) {});
    const auto* error = std::get_if<neomcp::CreateError>(&made);
    ASSERT_NE(error, nullptr);
    ASSERT_EQ(error->missing.size(), 2u) << error->message;
    bool open_missing = false;
    bool batch_missing = false;
    for (const auto& entry : error->missing) {
        if (entry.slot == neomcp::MechanismSlot::open) {
            open_missing = entry.revision == modern_revision && entry.name == "request_meta";
        }
        if (entry.slot == neomcp::MechanismSlot::batch) {
            batch_missing = entry.revision == batch_revision && entry.name == "array";
        }
    }
    EXPECT_TRUE(open_missing) << error->message;
    EXPECT_TRUE(batch_missing) << error->message;
    EXPECT_NE(error->message.find("request_meta"), std::string::npos);

    // Without the modern profile and the batch revision nothing is missing.
    options.profiles = profiles_of({"2024-11-05", "2025-06-18", "2025-11-25"});
    auto served = neomcp::Session::create(options, host, [](std::string) {});
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(served));
}

TEST(Creation, RejectsBadOptions) {
    FakeHost host;
    neomcp::SessionOptions zero_page = test_options();
    zero_page.tools_page_size = 0;
    EXPECT_TRUE(std::holds_alternative<neomcp::CreateError>(neomcp::Session::create(zero_page, host, [](std::string) {})));
    neomcp::SessionOptions twice = test_options();
    twice.profiles = profiles_of({modern_revision, modern_revision});
    EXPECT_TRUE(std::holds_alternative<neomcp::CreateError>(neomcp::Session::create(twice, host, [](std::string) {})));
    EXPECT_TRUE(std::holds_alternative<neomcp::CreateError>(neomcp::Session::create(test_options(), host, nullptr)));
    EXPECT_THROW({ Peer peer(host, zero_page); }, std::runtime_error);
}

// ---------------------------------------------------------------------------------------------------------------
// Which era a message belongs to

TEST(Era, ModernRequestIsServedWithoutAHandshake) {
    Conversation modern(modern_revision);
    const json answer = modern.ask("tools/list");
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(result_of(answer).at("resultType"), "complete");
    EXPECT_EQ(result_of(answer).at("tools").size(), 9u);
}

TEST(Era, UnsupportedModernVersion) {
    Conversation modern(modern_revision);
    for (const std::string requested : {"2099-01-01", "2025-06-18"}) {
        SCOPED_TRACE(requested);
        json request = modern.request("tools/list");
        request["params"]["_meta"][version_key] = requested;
        const auto answer = modern.peer.request(request, patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(error_code_of(*answer), -32022) << answer->dump();
        EXPECT_EQ(error_message_of(*answer), "Unsupported protocol version");
        EXPECT_EQ(answer->at("error").at("data").at("requested"), requested);
        EXPECT_EQ(answer->at("error").at("data").at("supported"), strings(newest_first));
    }
}

TEST(Era, ModernRequiredMetaKeysOneByOne) {
    Conversation modern(modern_revision);
    // basic/index.mdx of 2026-07-28: both keys are required; a request missing one is answered with -32602.
    for (const std::string key : {version_key, capabilities_key}) {
        SCOPED_TRACE(key);
        json request = modern.request("tools/list");
        request["params"]["_meta"].erase(key);
        const auto answer = modern.peer.request(request, patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(error_code_of(*answer), -32602) << answer->dump();
        EXPECT_NE(error_message_of(*answer).find(key), std::string::npos) << answer->dump();
    }
    json wrong_type = modern.request("tools/list");
    wrong_type["params"]["_meta"][version_key] = 20260728;
    EXPECT_EQ(error_code_of(*modern.peer.request(wrong_type, patience)), -32602);
    json bad_capabilities = modern.request("tools/list");
    bad_capabilities["params"]["_meta"][capabilities_key] = "all";
    EXPECT_EQ(error_code_of(*modern.peer.request(bad_capabilities, patience)), -32602);
}

TEST(Era, VersionKeyWithoutAModernProfile) {
    neomcp::SessionOptions options = test_options();
    options.profiles = profiles_of(legacy_revisions);
    FakeHost host;
    Peer peer(host, options);
    const auto answer = peer.request(Peer::make_request(*builtin(modern_revision), 1, "tools/list"), patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32602) << answer->dump();
    EXPECT_EQ(answer->at("error").at("data").at("supported"),
              strings({"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}));
}

TEST(Era, InitializeWithoutALegacyProfile) {
    neomcp::SessionOptions options = test_options();
    options.profiles = profiles_of({modern_revision});
    FakeHost host;
    Peer peer(host, options);
    const auto answer = peer.request(Peer::initialize_request(*builtin("2025-11-25"), 1), patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32601) << answer->dump();
    EXPECT_EQ(answer->at("error").at("data").at("supported"), strings({modern_revision}));
}

TEST(Era, PingInEachEra) {
    for (const auto& revision : legacy_revisions) {
        SCOPED_TRACE(revision);
        FakeHost host;
        neomcp::SessionOptions options = test_options();
        options.profiles = profiles_of({revision});
        Peer peer(host, options);
        const auto before = peer.request(Peer::make_request(*builtin(revision), "before", "ping"), patience);
        ASSERT_TRUE(before);
        EXPECT_EQ(result_of(*before), json::object()) << before->dump();
        ASSERT_TRUE(peer.initialize(*builtin(revision), patience));
        const auto after = peer.request(Peer::make_request(*builtin(revision), "after", "ping"), patience);
        ASSERT_TRUE(after);
        EXPECT_EQ(result_of(*after), json::object()) << after->dump();
    }
    // 2026-07-28 removed ping (changelog item 5).
    Conversation modern(modern_revision);
    EXPECT_EQ(error_code_of(modern.ask("ping")), -32601);
    neomcp::SessionOptions modern_only = test_options();
    modern_only.profiles = profiles_of({modern_revision});
    FakeHost host;
    Peer peer(host, modern_only);
    const auto bare = peer.request(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}}, patience);
    ASSERT_TRUE(bare);
    EXPECT_EQ(error_code_of(*bare), -32602) << bare->dump();
}

TEST(Era, RequestWithoutVersionBeforeAHandshake) {
    FakeHost host;
    Peer peer(host, test_options());
    const auto answer =
        peer.request(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::object()}}, patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32602) << answer->dump();
    EXPECT_NE(error_message_of(*answer).find(version_key), std::string::npos);
    EXPECT_NE(error_message_of(*answer).find("initialize"), std::string::npos);
    EXPECT_EQ(answer->at("error").at("data").at("supported"), strings(newest_first));
}

TEST(Era, LegacyRequestsAreServedBeforeTheInitializedNotification) {
    FakeHost host;
    Peer peer(host, test_options());
    const auto legacy = builtin("2025-06-18");
    ASSERT_TRUE(peer.request(Peer::initialize_request(*legacy, "init"), patience));
    const auto answer = peer.request(Peer::make_request(*legacy, 2, "tools/list"), patience);
    ASSERT_TRUE(answer && answer->contains("result")) << (answer ? answer->dump() : "");
}

TEST(Era, ModernAndLegacyRequestsInterleaved) {
    Conversation legacy(batch_revision);
    const auto modern = builtin(modern_revision);
    const auto modern_list = legacy.peer.request(Peer::make_request(*modern, "m1", "tools/list"), patience);
    ASSERT_TRUE(modern_list);
    EXPECT_EQ(result_of(*modern_list).at("resultType"), "complete");
    const json legacy_list = legacy.ask("tools/list");
    EXPECT_FALSE(result_of(legacy_list).contains("resultType"));
    // 2025-03-26 Tool: annotations, description, inputSchema, name.
    EXPECT_EQ(keys(result_of(legacy_list).at("tools").at(8)),
              (std::set<std::string>{"annotations", "description", "inputSchema", "name"}));
    const auto discover = legacy.peer.request(Peer::make_request(*modern, "m2", "server/discover"), patience);
    ASSERT_TRUE(discover && discover->contains("result"));
    EXPECT_EQ(result_of(legacy.ask("ping")), json::object());
    const auto modern_call = legacy.peer.request(
        Peer::make_request(*modern, "m3", "tools/call", {{"name", "echo"}, {"arguments", {{"x", 1}}}}), patience);
    ASSERT_TRUE(modern_call);
    EXPECT_EQ(result_of(*modern_call).at("resultType"), "complete");
    EXPECT_FALSE(result_of(legacy.ask("tools/call", {{"name", "echo"}})).contains("resultType"));
}

TEST(Era, ModernRequestsReadNothingFromTheConnection) {
    FakeHost host;
    Peer peer(host, test_options());
    const auto legacy = builtin(batch_revision);
    const auto modern = builtin(modern_revision);
    const json handshake_capabilities = json{{"roots", json::object()}};
    const auto opened = peer.request(Peer::initialize_request(*legacy, "init", handshake_capabilities), patience);
    ASSERT_TRUE(opened && opened->contains("result"));

    // A modern call without clientInfo, one with its own, and a call on the connection.
    json bare = Peer::make_request(*modern, "m1", "tools/call", {{"name", "gated"}});
    bare["params"]["_meta"][capabilities_key] = json{{"sampling", json::object()}};
    peer.notify(bare);
    json named = Peer::make_request(*modern, "m2", "tools/call", {{"name", "gated"}});
    named["params"]["_meta"]["io.modelcontextprotocol/clientInfo"] = json{{"name", "modern-client"}, {"version", "1"}};
    peer.notify(named);
    peer.notify(Peer::make_request(*legacy, "l1", "tools/call", {{"name", "gated"}}));
    std::vector<FakeHost::Parked> parked;
    for (int i = 0; i < 3; ++i) {
        auto next = host.wait_parked(patience);
        ASSERT_TRUE(next);
        parked.push_back(*next);
    }
    EXPECT_EQ(parked[0].control.profile().revision(), modern_revision);
    EXPECT_TRUE(parked[0].control.client_info().is_null());
    EXPECT_EQ(parked[0].control.client_capabilities(), (json{{"sampling", json::object()}}));
    EXPECT_EQ(parked[1].control.client_info().at("name"), "modern-client");
    EXPECT_EQ(parked[1].control.client_capabilities(), json::object());
    EXPECT_EQ(parked[2].control.profile().revision(), batch_revision);
    EXPECT_EQ(parked[2].control.client_info().at("name"), "neomcp-test-peer");
    EXPECT_EQ(parked[2].control.client_capabilities(), handshake_capabilities);
    for (const auto& call : parked) call.done(text_result("done"));
    for (const char* id : {"m1", "m2", "l1"}) EXPECT_TRUE(peer.await_response(id, patience)) << id;
}

TEST(Era, InitializeThatNamesTheModernVersion) {
    FakeHost host;
    Peer peer(host, test_options());
    json request = Peer::initialize_request(*builtin("2025-11-25"), 1);
    request["params"]["_meta"] = Peer::request_meta(*builtin(modern_revision));
    const auto answer = peer.request(request, patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32601) << "initialize is not a request of the revision it names";
    const auto after =
        peer.request(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", json::object()}}, patience);
    ASSERT_TRUE(after);
    EXPECT_EQ(error_code_of(*after), -32602) << "no connection was opened";
}

TEST(Era, ResponsesAndUnlistedNotificationsAreIgnored) {
    Conversation legacy("2025-06-18");
    const std::size_t before = legacy.peer.transcript().size();
    legacy.peer.send(R"({"jsonrpc":"2.0","id":77,"result":{}})");
    legacy.peer.send(R"({"jsonrpc":"2.0","id":78,"error":{"code":1,"message":"x"}})");
    legacy.peer.send(R"({"jsonrpc":"2.0","method":"notifications/roots/list_changed"})");
    legacy.peer.send(R"({"jsonrpc":"2.0","method":"notifications/no_such_thing","params":{}})");
    legacy.peer.send(R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":1}})");
    legacy.flush();
    EXPECT_EQ(legacy.peer.transcript().size(), before + 1);
}

TEST(Era, ArraysOutsideTheBatchRevisionAreRefused) {
    for (const std::string revision : {"2024-11-05", "2025-06-18", "2025-11-25", "2026-07-28"}) {
        SCOPED_TRACE(revision);
        Conversation conversation(revision);
        conversation.peer.send(neomcp::serialize(json::array({conversation.request(conversation.profile->request("ping") ? "ping" : "tools/list")})));
        const auto answer = conversation.peer.next_other(patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(error_code_of(*answer), -32600) << answer->dump();
        EXPECT_TRUE(answer->at("id").is_null());
    }
    FakeHost host;
    Peer peer(host, test_options());
    peer.send(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])");
    const auto answer = peer.next_other(patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32600) << "no connection, so no batch mechanism";
}

// ---------------------------------------------------------------------------------------------------------------
// The legacy handshake

// Implementation per revision (schema.json of each revision).
const std::map<std::string, std::set<std::string>> implementation_members{
    {"2024-11-05", {"name", "version"}},
    {"2025-03-26", {"name", "version"}},
    {"2025-06-18", {"name", "title", "version"}},
    {"2025-11-25", {"description", "icons", "name", "title", "version", "websiteUrl"}},
    {"2026-07-28", {"description", "icons", "name", "title", "version", "websiteUrl"}},
};

TEST_P(PerLegacyRevision, HandshakeAnswersTheRequestedRevision) {
    const std::string revision = GetParam();
    FakeHost host;
    Peer peer(host, test_options());
    const auto answer = peer.request(Peer::initialize_request(*builtin(revision), 1), patience);
    ASSERT_TRUE(answer && answer->contains("result")) << (answer ? answer->dump() : "");
    const json& result = result_of(*answer);
    EXPECT_EQ(result.at("protocolVersion"), revision);
    EXPECT_EQ(keys(result), (std::set<std::string>{"capabilities", "instructions", "protocolVersion", "serverInfo"}));
    EXPECT_EQ(keys(result.at("serverInfo")), implementation_members.at(revision));
    EXPECT_EQ(result.at("capabilities"), (json{{"tools", {{"listChanged", false}}}})) << "logging is off by default";
}

TEST(Handshake, RequestedRevisionNotEnabled) {
    neomcp::SessionOptions options = test_options();
    options.profiles = profiles_of({"2024-11-05", "2025-06-18", modern_revision});
    FakeHost host;
    Peer peer(host, options);
    const auto answer = peer.request(Peer::initialize_request(*builtin("2025-03-26"), 1), patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(result_of(*answer).at("protocolVersion"), "2025-06-18") << "the newest enabled legacy revision";
}

TEST(Handshake, RevisionOlderThanAll) {
    FakeHost host;
    Peer peer(host, test_options());
    json request = Peer::initialize_request(*builtin("2024-11-05"), 1);
    request["params"]["protocolVersion"] = "2024-01-01";
    const auto answer = peer.request(request, patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(result_of(*answer).at("protocolVersion"), "2025-11-25");
}

TEST(Handshake, SecondInitialize) {
    Conversation legacy("2025-11-25");
    const auto again = legacy.peer.request(Peer::initialize_request(*legacy.profile, "again"), patience);
    ASSERT_TRUE(again);
    EXPECT_EQ(error_code_of(*again), -32600) << again->dump();
    EXPECT_EQ(result_of(legacy.ask("ping")), json::object()) << "the first handshake still holds";
}

TEST(Handshake, MalformedInitialize) {
    const auto legacy = builtin("2025-11-25");
    for (const std::string member : {"protocolVersion", "clientInfo", "capabilities"}) {
        SCOPED_TRACE(member);
        FakeHost host;
        Peer peer(host, test_options());
        json request = Peer::initialize_request(*legacy, 1);
        request["params"].erase(member);
        const auto answer = peer.request(request, patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(error_code_of(*answer), -32602) << answer->dump();
    }
    FakeHost host;
    Peer peer(host, test_options());
    const auto bare = peer.request(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, patience);
    ASSERT_TRUE(bare);
    EXPECT_EQ(error_code_of(*bare), -32602);
}

// ---------------------------------------------------------------------------------------------------------------
// server/discover

TEST(Discover, Result) {
    neomcp::SessionOptions options = test_options();
    options.tools_cache = neomcp::CacheHints{5000, neomcp::CacheScope::public_scope};
    Conversation modern(modern_revision, options);
    const json answer = modern.ask("server/discover");
    const json& result = result_of(answer);
    EXPECT_EQ(keys(result), (std::set<std::string>{"_meta", "cacheScope", "capabilities", "instructions", "resultType",
                                                   "supportedVersions", "ttlMs"}));
    EXPECT_EQ(result.at("supportedVersions"), strings(newest_first));
    EXPECT_EQ(result.at("capabilities"), (json{{"tools", {{"listChanged", false}}}}));
    EXPECT_EQ(result.at("ttlMs"), 5000);
    EXPECT_EQ(result.at("cacheScope"), "public");
    EXPECT_EQ(result.at("resultType"), "complete");
    EXPECT_EQ(keys(result.at("_meta").at(server_info_key)), implementation_members.at(modern_revision));
}

TEST(Discover, DefaultsAndServerInfoSwitchedOff) {
    neomcp::SessionOptions options = test_options();
    options.include_server_info = false;
    options.instructions.reset();
    Conversation modern(modern_revision, options);
    const json answer = modern.ask("server/discover");
    const json& result = result_of(answer);
    EXPECT_EQ(keys(result), (std::set<std::string>{"cacheScope", "capabilities", "resultType", "supportedVersions",
                                                   "ttlMs"}));
    EXPECT_EQ(result.at("ttlMs"), 0);
    EXPECT_EQ(result.at("cacheScope"), "private");
}

TEST(Discover, NotARequestOfLegacyRevisions) {
    Conversation legacy("2025-11-25");
    EXPECT_EQ(error_code_of(legacy.ask("server/discover")), -32601);
    EXPECT_EQ(error_code_of(legacy.ask("resources/list")), -32601) << "listed by the revision, not served here";
}

// ---------------------------------------------------------------------------------------------------------------
// tools/list

// Tool per revision (schema.json of each revision), restricted to the members the model has (the model has no
// `execution`, which only 2025-11-25 defines).
const std::map<std::string, std::set<std::string>> tool_members{
    {"2024-11-05", {"description", "inputSchema", "name"}},
    {"2025-03-26", {"annotations", "description", "inputSchema", "name"}},
    {"2025-06-18", {"_meta", "annotations", "description", "inputSchema", "name", "outputSchema", "title"}},
    {"2025-11-25", {"_meta", "annotations", "description", "icons", "inputSchema", "name", "outputSchema", "title"}},
    {"2026-07-28", {"_meta", "annotations", "description", "icons", "inputSchema", "name", "outputSchema", "title"}},
};
const std::set<std::string> annotation_members{"destructiveHint", "idempotentHint", "openWorldHint", "readOnlyHint",
                                               "title"};
const std::set<std::string> icon_members{"mimeType", "sizes", "src", "theme"};
const std::map<std::string, std::set<std::string>> list_result_members{
    {"2024-11-05", {"tools"}},
    {"2025-03-26", {"tools"}},
    {"2025-06-18", {"tools"}},
    {"2025-11-25", {"tools"}},
    {"2026-07-28", {"_meta", "cacheScope", "resultType", "tools", "ttlMs"}},
};

TEST_P(PerRevision, ToolDefinitionHasExactlyTheRevisionsMembers) {
    const std::string revision = GetParam();
    ASSERT_EQ(tool_members.size(), profile::builtin_revisions().size()) << "a new revision needs its values here";
    Conversation conversation(revision);
    const json answer = conversation.ask("tools/list");
    const json& result = result_of(answer);
    EXPECT_EQ(keys(result), list_result_members.at(revision));
    const json& tools = result.at("tools");
    ASSERT_EQ(tools.size(), 9u);
    const json& tool = tools.at(8);
    EXPECT_EQ(tool.at("name"), "everything");
    EXPECT_EQ(keys(tool), tool_members.at(revision)) << tool.dump();
    if (tool.contains("annotations")) {
        EXPECT_EQ(keys(tool.at("annotations")), annotation_members);
        EXPECT_EQ(tool.at("annotations").at("readOnlyHint"), true);
        EXPECT_EQ(tool.at("annotations").at("openWorldHint"), false);
    }
    if (tool.contains("icons")) {
        EXPECT_EQ(keys(tool.at("icons").at(0)), icon_members);
    }
    // Registration order.
    std::vector<std::string> names;
    for (const auto& entry : *tools.get_ptr<const json::array_t*>()) names.push_back(entry.at("name").get<std::string>());
    EXPECT_EQ(names, (std::vector<std::string>{"echo", "gated", "progress", "busy", "full", "logger", "throws",
                                               "dropper", "everything"}));
}

TEST_P(PerRevision, Paging) {
    neomcp::SessionOptions options = test_options();
    options.tools_page_size = 2;
    Conversation conversation(GetParam(), options);
    std::vector<std::string> names;
    std::vector<std::string> cursors;
    std::optional<std::string> cursor;
    for (int page = 0; page < 10; ++page) {
        json params = json::object();
        if (cursor) params["cursor"] = *cursor;
        const json answer = conversation.ask("tools/list", params);
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        const json& result = result_of(answer);
        EXPECT_LE(result.at("tools").size(), 2u);
        for (const auto& tool : *result.at("tools").get_ptr<const json::array_t*>()) {
            names.push_back(tool.at("name").get<std::string>());
        }
        if (!result.contains("nextCursor")) break;
        cursor = result.at("nextCursor").get<std::string>();
        cursors.push_back(*cursor);
    }
    EXPECT_EQ(names.size(), 9u);
    EXPECT_EQ(cursors, (std::vector<std::string>{"2", "4", "6", "8"}));
    for (const json& bad : {json("abc"), json("-1"), json("02"), json(""), json("9"), json("99"), json(" 2"),
                           json("2 "), json(3), json(nullptr)}) {
        SCOPED_TRACE(bad.dump());
        EXPECT_EQ(error_code_of(conversation.ask("tools/list", {{"cursor", bad}})), -32602);
    }
    EXPECT_TRUE(conversation.ask("tools/list", {{"cursor", "0"}}).contains("result"));
}

TEST_P(PerRevision, CursorWhenPagingIsOff) {
    Conversation conversation(GetParam());
    EXPECT_EQ(error_code_of(conversation.ask("tools/list", {{"cursor", "2"}})), -32602);
    EXPECT_FALSE(result_of(conversation.ask("tools/list")).contains("nextCursor"));
}

// ---------------------------------------------------------------------------------------------------------------
// tools/call

// CallToolResult per revision, for a result with content, structured content and _meta (isError false is not
// written).
const std::map<std::string, std::set<std::string>> call_result_members{
    {"2024-11-05", {"_meta", "content"}},
    {"2025-03-26", {"_meta", "content"}},
    {"2025-06-18", {"_meta", "content", "structuredContent"}},
    {"2025-11-25", {"_meta", "content", "structuredContent"}},
    {"2026-07-28", {"_meta", "content", "resultType", "structuredContent"}},
};
// The kinds written for [text, image, audio, resource_link, resource]: audio exists from 2025-03-26, resource links
// from 2025-06-18; a kind the revision lacks becomes a text block.
const std::map<std::string, std::vector<std::string>> content_types{
    {"2024-11-05", {"text", "image", "text", "text", "resource"}},
    {"2025-03-26", {"text", "image", "audio", "text", "resource"}},
    {"2025-06-18", {"text", "image", "audio", "resource_link", "resource"}},
    {"2025-11-25", {"text", "image", "audio", "resource_link", "resource"}},
    {"2026-07-28", {"text", "image", "audio", "resource_link", "resource"}},
};
const std::map<std::string, std::set<std::string>> text_block_members{
    {"2024-11-05", {"annotations", "text", "type"}},
    {"2025-03-26", {"annotations", "text", "type"}},
    {"2025-06-18", {"_meta", "annotations", "text", "type"}},
    {"2025-11-25", {"_meta", "annotations", "text", "type"}},
    {"2026-07-28", {"_meta", "annotations", "text", "type"}},
};
// The image block carries `_meta` only as an extra member, and an extra member no revision has.
const std::map<std::string, std::set<std::string>> image_block_members{
    {"2024-11-05", {"data", "mimeType", "type"}},
    {"2025-03-26", {"data", "mimeType", "type"}},
    {"2025-06-18", {"_meta", "data", "mimeType", "type"}},
    {"2025-11-25", {"_meta", "data", "mimeType", "type"}},
    {"2026-07-28", {"_meta", "data", "mimeType", "type"}},
};
const std::map<std::string, std::set<std::string>> resource_link_members{
    {"2025-06-18", {"_meta", "annotations", "description", "mimeType", "name", "size", "title", "type", "uri"}},
    {"2025-11-25",
     {"_meta", "annotations", "description", "icons", "mimeType", "name", "size", "title", "type", "uri"}},
    {"2026-07-28",
     {"_meta", "annotations", "description", "icons", "mimeType", "name", "size", "title", "type", "uri"}},
};
const std::map<std::string, std::set<std::string>> text_resource_members{
    {"2024-11-05", {"mimeType", "text", "uri"}},
    {"2025-03-26", {"mimeType", "text", "uri"}},
    {"2025-06-18", {"_meta", "mimeType", "text", "uri"}},
    {"2025-11-25", {"_meta", "mimeType", "text", "uri"}},
    {"2026-07-28", {"_meta", "mimeType", "text", "uri"}},
};
// Content annotations: lastModified exists from 2025-06-18. 2024-11-05 defines the annotations inline, so the
// tables do not name their members and they are written as the model has them.
const std::map<std::string, std::set<std::string>> content_annotation_members{
    {"2024-11-05", {"audience", "lastModified", "priority"}},
    {"2025-03-26", {"audience", "priority"}},
    {"2025-06-18", {"audience", "lastModified", "priority"}},
    {"2025-11-25", {"audience", "lastModified", "priority"}},
    {"2026-07-28", {"audience", "lastModified", "priority"}},
};

TEST_P(PerRevision, CallResultHasExactlyTheRevisionsMembers) {
    const std::string revision = GetParam();
    ASSERT_EQ(call_result_members.size(), profile::builtin_revisions().size());
    Conversation conversation(revision);
    const json answer = conversation.ask("tools/call", {{"name", "full"}});
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    const json& result = result_of(answer);
    EXPECT_EQ(keys(result), call_result_members.at(revision)) << result.dump();
    const json& content = result.at("content");
    ASSERT_EQ(content.size(), 5u);
    std::vector<std::string> types;
    for (const auto& block : *content.get_ptr<const json::array_t*>()) types.push_back(block.at("type").get<std::string>());
    EXPECT_EQ(types, content_types.at(revision));
    EXPECT_EQ(keys(content.at(0)), text_block_members.at(revision));
    EXPECT_EQ(keys(content.at(0).at("annotations")), content_annotation_members.at(revision));
    EXPECT_EQ(keys(content.at(1)), image_block_members.at(revision));
    for (std::size_t i = 2; i < 4; ++i) {
        if (types[i] != "text") continue;
        const std::string text = content.at(i).at("text").get<std::string>();
        const std::string kind = i == 2 ? "audio" : "resource_link";
        EXPECT_NE(text.find(kind), std::string::npos) << "the replacement names the kind: " << text;
        EXPECT_EQ(keys(content.at(i)), (std::set<std::string>{"text", "type"}));
    }
    if (types[3] == "resource_link") {
        EXPECT_EQ(keys(content.at(3)), resource_link_members.at(revision));
        EXPECT_EQ(content.at(3).at("size"), 42);
    }
    EXPECT_EQ(keys(content.at(4).at("resource")), text_resource_members.at(revision));
    if (result.contains("structuredContent")) {
        EXPECT_EQ(result.at("structuredContent"), (json{{"answer", 42}}));
    }
    EXPECT_EQ(result.at("_meta").at("trace"), "t1");
    if (revision == modern_revision) {
        EXPECT_EQ(result.at("resultType"), "complete");
        EXPECT_EQ(keys(result.at("_meta").at(server_info_key)), implementation_members.at(revision));
    } else {
        EXPECT_FALSE(result.at("_meta").contains(server_info_key));
    }
}

TEST_P(PerRevision, MalformedCallsAndUnknownTools) {
    Conversation conversation(GetParam());
    EXPECT_EQ(error_code_of(conversation.ask("tools/call", {{"arguments", json::object()}})), -32602);
    EXPECT_EQ(error_code_of(conversation.ask("tools/call", {{"name", 5}})), -32602);
    EXPECT_EQ(error_code_of(conversation.ask("tools/call", {{"name", "echo"}, {"arguments", "x"}})), -32602);
    EXPECT_EQ(error_code_of(conversation.ask("tools/call", {{"name", "echo"}, {"arguments", json::array()}})), -32602);
    const json unknown = conversation.ask("tools/call", {{"name", "nope"}});
    EXPECT_EQ(error_code_of(unknown), -32602);
    EXPECT_EQ(error_message_of(unknown), "Unknown tool: nope");
    const json echo = conversation.ask("tools/call", {{"name", "echo"}});
    EXPECT_EQ(result_of(echo).at("content").at(0).at("text"), "{}") << "absent arguments are an empty object";
}

TEST_P(PerRevision, BusyAndHostFailures) {
    Conversation conversation(GetParam());
    const json busy = conversation.ask("tools/call", {{"name", "busy"}});
    EXPECT_EQ(error_code_of(busy), -32603) << busy.dump();
    EXPECT_EQ(busy.at("error").at("data"), (json{{"reason", "busy"}}));
    EXPECT_EQ(error_code_of(conversation.ask("tools/call", {{"name", "throws"}})), -32603);
    EXPECT_EQ(error_code_of(conversation.ask("tools/call", {{"name", "dropper"}})), -32603);
    EXPECT_TRUE(conversation.ask("tools/call", {{"name", "echo"}}).contains("result")) << "the connection goes on";
}

// server/tools.mdx of each revision: invalid arguments are a protocol error up to 2025-06-18 and a tool execution
// error from 2025-11-25.
const std::map<std::string, bool> invalid_arguments_are_results{
    {"2024-11-05", false}, {"2025-03-26", false}, {"2025-06-18", false}, {"2025-11-25", true}, {"2026-07-28", true},
};

TEST_P(PerRevision, ArgumentsThatFailTheInputSchema) {
    const std::string revision = GetParam();
    ASSERT_EQ(invalid_arguments_are_results.size(), profile::builtin_revisions().size());
    neomcp::SessionOptions options = test_options();
    std::vector<std::string> checked;
    options.check_arguments = [&checked](const neomcp::ToolDefinition& tool,
                                         const json& arguments) -> std::optional<std::string> {
        checked.push_back(tool.name);
        if (arguments.contains("bad")) return "bad must not be given";
        return std::nullopt;
    };
    Conversation conversation(revision, options);
    const json answer = conversation.ask("tools/call", {{"name", "echo"}, {"arguments", {{"bad", 1}}}});
    if (invalid_arguments_are_results.at(revision)) {
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_EQ(result_of(answer).at("isError"), true);
        EXPECT_EQ(result_of(answer).at("content").at(0).at("text"), "bad must not be given");
    } else {
        EXPECT_EQ(error_code_of(answer), -32602) << answer.dump();
        EXPECT_NE(error_message_of(answer).find("bad must not be given"), std::string::npos);
    }
    EXPECT_TRUE(conversation.ask("tools/call", {{"name", "echo"}, {"arguments", {{"good", 1}}}}).contains("result"));
    EXPECT_EQ(checked, (std::vector<std::string>{"echo", "echo"}));
}

// ---------------------------------------------------------------------------------------------------------------
// Cancellation and progress

bool mentions(const json& message, const json& id, const json& token) {
    if (message.contains("id") && message.at("id") == id) return true;
    if (message.contains("params") && message.at("params").contains("progressToken") &&
        message.at("params").at("progressToken") == token) {
        return true;
    }
    return false;
}

TEST_P(PerRevision, NothingIsWrittenAfterCancellation) {
    Conversation conversation(GetParam());
    const json id = "call-1";
    const json token = "token-1";
    conversation.peer.notify(Peer::make_request(*conversation.profile, "call-1", "tools/call",
                                                {{"name", "gated"}, {"_meta", {{"progressToken", token}}}}));
    auto parked = conversation.host.wait_parked(patience);
    ASSERT_TRUE(parked);
    parked->control.progress(1);
    const auto first = conversation.peer.next_notification(patience);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->at("params").at("progressToken"), token);

    const std::size_t before = conversation.peer.transcript().size();
    conversation.peer.notify(cancelled(id));
    EXPECT_TRUE(parked->control.stop_token().stop_requested());
    std::thread late([call = *parked] {
        call.control.progress(2);
        call.control.log(neomcp::LogLevel::emergency, "late");
        call.done(text_result("late"));
    });
    late.join();
    conversation.flush();
    const auto transcript = conversation.peer.transcript();
    ASSERT_EQ(transcript.size(), before + 1) << transcript.back().dump();
    for (std::size_t i = before; i < transcript.size(); ++i) EXPECT_FALSE(mentions(transcript[i], id, token));
}

// A sink that can hold the thread inside it, so that a test can act while a line is being written.
struct HoldingSink {
    void write(std::string line) {
        std::unique_lock<std::mutex> lock(mutex);
        lines.push_back(json::parse(line));
        if (!hold) return;
        hold = false;
        held = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
        held = false;
    }
    bool wait_held() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, patience, [&] { return held; });
    }
    void release() {
        const std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<json> lines;
    bool hold = false;
    bool held = false;
    bool released = false;
};

TEST(Cancellation, QueuedOutputIsDroppedWhenTheCallIsCancelled) {
    FakeHost host;
    HoldingSink sink;
    auto made = neomcp::Session::create(test_options(), host, [&sink](std::string line) { sink.write(std::move(line)); });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    neomcp::Session& session = *std::get<std::unique_ptr<neomcp::Session>>(made);
    const auto modern = builtin(modern_revision);
    for (const std::string id : {"x", "y"}) {
        session.receive(neomcp::serialize(Peer::make_request(
            *modern, id, "tools/call", {{"name", "gated"}, {"_meta", {{"progressToken", "t" + id}}}})));
    }
    auto x = host.wait_parked(patience);
    auto y = host.wait_parked(patience);
    ASSERT_TRUE(x && y);
    {
        const std::lock_guard<std::mutex> lock(sink.mutex);
        sink.hold = true;
    }
    // Another thread writes x's response and is held inside the sink...
    std::thread writer([call = *x] { call.done(text_result("x done")); });
    ASSERT_TRUE(sink.wait_held());
    // ...so y's progress waits in the queue when y is cancelled.
    y->control.progress(1);
    session.receive(neomcp::serialize(cancelled("y")));
    sink.release();
    writer.join();
    session.receive(neomcp::serialize(Peer::make_request(*modern, "sentinel", "server/discover")));
    const std::lock_guard<std::mutex> lock(sink.mutex);
    ASSERT_EQ(sink.lines.size(), 2u);
    EXPECT_EQ(sink.lines[0].at("id"), "x");
    EXPECT_EQ(sink.lines[1].at("id"), "sentinel");
    for (const auto& line : sink.lines) EXPECT_FALSE(mentions(line, "y", "ty")) << line.dump();
}

TEST_P(PerRevision, CancellationsThatMatchNothingAreIgnored) {
    Conversation conversation(GetParam());
    const json done = conversation.ask("tools/call", {{"name", "echo"}});
    ASSERT_TRUE(done.contains("result"));
    const std::size_t before = conversation.peer.transcript().size();
    conversation.peer.notify(cancelled(999));
    conversation.peer.notify(cancelled(done.at("id")));
    conversation.peer.notify(cancelled("initialize"));
    conversation.peer.send(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{}})");
    conversation.flush();
    EXPECT_EQ(conversation.peer.transcript().size(), before + 1);
}

TEST_P(PerRevision, DuplicateIdOfARequestInFlight) {
    Conversation conversation(GetParam());
    conversation.peer.notify(Peer::make_request(*conversation.profile, "dup", "tools/call", {{"name", "gated"}}));
    auto parked = conversation.host.wait_parked(patience);
    ASSERT_TRUE(parked);
    const auto second =
        conversation.peer.request(Peer::make_request(*conversation.profile, "dup", "tools/call", {{"name", "echo"}}),
                                  patience);
    ASSERT_TRUE(second);
    EXPECT_EQ(error_code_of(*second), -32600) << second->dump();
    std::thread finish([call = *parked] { call.done(text_result("first")); });
    finish.join();
    const auto first = conversation.peer.await_response("dup", patience);
    ASSERT_TRUE(first);
    EXPECT_EQ(result_of(*first).at("content").at(0).at("text"), "first");
    EXPECT_TRUE(conversation.ask("tools/call", {{"name", "echo"}}).contains("result"));
}

TEST_P(PerRevision, ProgressOnlyWhenAskedAndIncreasing) {
    Conversation conversation(GetParam());
    const json answer = conversation.ask("tools/call", {{"name", "progress"}, {"_meta", {{"progressToken", 7}}}});
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    std::vector<double> values;
    while (const auto notification = conversation.peer.next_notification(now)) {
        EXPECT_EQ(notification->at("method"), "notifications/progress");
        EXPECT_EQ(notification->at("params").at("progressToken"), 7);
        EXPECT_EQ(notification->at("params").at("total"), 3);
        values.push_back(notification->at("params").at("progress").get<double>());
    }
    EXPECT_EQ(values, (std::vector<double>{1, 2, 3}));
    // Everything the call wrote precedes its response.
    const auto transcript = conversation.peer.transcript();
    EXPECT_EQ(transcript.back().at("id"), answer.at("id"));

    const json quiet = conversation.ask("tools/call", {{"name", "progress"}});
    ASSERT_TRUE(quiet.contains("result"));
    EXPECT_FALSE(conversation.peer.next_notification(now)) << "no progressToken, no progress";
    EXPECT_EQ(error_code_of(conversation.ask("tools/call", {{"name", "progress"}, {"_meta", {{"progressToken", 1.5}}}})),
              -32602);
}

// ---------------------------------------------------------------------------------------------------------------
// Logging

std::vector<std::string> logged_levels(Peer& peer) {
    std::vector<std::string> levels;
    while (const auto notification = peer.next_notification(now)) {
        EXPECT_EQ(notification->at("method"), "notifications/message");
        levels.push_back(notification->at("params").at("level").get<std::string>());
    }
    return levels;
}

TEST_P(PerRevision, LoggingIsOffByDefault) {
    const std::string revision = GetParam();
    Conversation conversation(revision);
    json params = {{"name", "logger"}};
    if (!is_legacy(revision)) params["_meta"] = {{log_level_key, "debug"}};
    EXPECT_TRUE(conversation.ask("tools/call", params).contains("result"));
    EXPECT_TRUE(logged_levels(conversation.peer).empty());
    if (is_legacy(revision)) {
        EXPECT_EQ(error_code_of(conversation.ask("logging/setLevel", {{"level", "debug"}})), -32601);
    } else {
        json invalid = {{"name", "echo"}, {"_meta", {{log_level_key, "loud"}}}};
        EXPECT_TRUE(conversation.ask("tools/call", invalid).contains("result")) << "the key is not read when off";
        EXPECT_FALSE(result_of(conversation.ask("server/discover")).at("capabilities").contains("logging"));
    }
}

TEST_P(PerLegacyRevision, LoggingOnConnection) {
    neomcp::SessionOptions options = test_options();
    options.protocol_logging = true;
    FakeHost host;
    Peer peer(host, options);
    const auto legacy = builtin(GetParam());
    const auto opened = peer.initialize(*legacy, patience);
    ASSERT_TRUE(opened);
    EXPECT_EQ(result_of(*opened).at("capabilities").at("logging"), json::object());
    int id = 1;
    const auto call_logger = [&] {
        const auto answer = peer.request(Peer::make_request(*legacy, id++, "tools/call", {{"name", "logger"}}), patience);
        EXPECT_TRUE(answer && answer->contains("result"));
        return logged_levels(peer);
    };
    EXPECT_EQ(call_logger(), (std::vector<std::string>{"info", "warning", "error"})) << "default_log_level info";
    const auto set = peer.request(Peer::make_request(*legacy, id++, "logging/setLevel", {{"level", "error"}}), patience);
    ASSERT_TRUE(set);
    EXPECT_EQ(result_of(*set), json::object());
    EXPECT_EQ(call_logger(), (std::vector<std::string>{"error"}));
    const auto invalid = peer.request(Peer::make_request(*legacy, id++, "logging/setLevel", {{"level", "loud"}}), patience);
    EXPECT_EQ(error_code_of(*invalid), -32602);
}

TEST(Logging, OnPerModernRequest) {
    neomcp::SessionOptions options = test_options();
    options.protocol_logging = true;
    Conversation modern(modern_revision, options);
    EXPECT_EQ(result_of(modern.ask("server/discover")).at("capabilities").at("logging"), json::object());
    EXPECT_TRUE(modern.ask("tools/call", {{"name", "logger"}}).contains("result"));
    EXPECT_TRUE(logged_levels(modern.peer).empty()) << "no logLevel in _meta, no messages";
    EXPECT_TRUE(modern.ask("tools/call", {{"name", "logger"}, {"_meta", {{log_level_key, "warning"}}}}).contains("result"));
    EXPECT_EQ(logged_levels(modern.peer), (std::vector<std::string>{"warning", "error"}));
    EXPECT_EQ(error_code_of(modern.ask("tools/call", {{"name", "logger"}, {"_meta", {{log_level_key, "loud"}}}})),
              -32602);
    EXPECT_EQ(error_code_of(modern.ask("logging/setLevel", {{"level", "debug"}})), -32601);
}

// ---------------------------------------------------------------------------------------------------------------
// Input that is not a message

TEST(Input, ParseErrorsAndTheConnectionContinues) {
    Conversation legacy("2025-06-18");
    for (const std::string line : {"{", "", "not json", "{} {}"}) {
        SCOPED_TRACE(line);
        legacy.peer.send(line);
        const auto answer = legacy.peer.next_other(patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(error_code_of(*answer), -32700);
        EXPECT_TRUE(answer->at("id").is_null());
    }
    EXPECT_EQ(result_of(legacy.ask("ping")), json::object());
}

TEST(Input, InvalidMessages) {
    Conversation legacy("2025-06-18");
    for (const std::string line : {"42", R"("text")", "null", R"({"jsonrpc":"2.0","id":1.5,"method":"ping"})",
                                   R"({"jsonrpc":"2.0","id":null,"method":"ping"})"}) {
        SCOPED_TRACE(line);
        legacy.peer.send(line);
        const auto answer = legacy.peer.next_other(patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(error_code_of(*answer), -32600);
    }
    for (const std::string line : {R"({"jsonrpc":"1.0","id":"old","method":"ping"})", R"({"id":"old","method":"ping"})",
                                   R"({"jsonrpc":"2.0","id":"old","method":7})"}) {
        SCOPED_TRACE(line);
        legacy.peer.send(line);
        const auto with_id = legacy.peer.await_response("old", patience);
        ASSERT_TRUE(with_id);
        EXPECT_EQ(error_code_of(*with_id), -32600) << "answered to its id";
    }
    EXPECT_EQ(error_code_of(legacy.ask("ping", json::array())), -32602) << "params of a request are an object";
}

TEST(Input, OversizedAndTooDeepLines) {
    neomcp::SessionOptions options = test_options();
    options.max_message_bytes = 256;
    options.max_nesting_depth = 8;
    Conversation legacy("2025-06-18", options);
    legacy.peer.send(neomcp::serialize(legacy.request("tools/call", {{"name", "echo"}, {"arguments", {{"x", std::string(300, 'a')}}}})));
    auto answer = legacy.peer.next_other(patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32600);
    EXPECT_TRUE(answer->at("id").is_null());
    legacy.peer.send(std::string(20, '[') + std::string(20, ']'));
    answer = legacy.peer.next_other(patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32600);
    EXPECT_EQ(result_of(legacy.ask("tools/call", {{"name", "echo"}, {"arguments", {{"x", 1}}}})).at("content").size(), 1u);
}

TEST(Input, DeepNestingIsRefusedAtTheDefaultLimit) {
    Conversation legacy("2025-06-18");
    // Far deeper than the default limit; parsing it would recurse once per level.
    legacy.peer.send(std::string(200000, '[') + std::string(200000, ']'));
    const auto answer = legacy.peer.next_other(patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32600);
}

TEST(Input, RequestIdsAreEchoedExactly) {
    FakeHost host;
    Peer peer(host, test_options());
    for (const json& id : {json("abc"), json(-5), json(0), json(std::uint64_t{18446744073709551615u})}) {
        SCOPED_TRACE(id.dump());
        const auto answer = peer.request(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}}, patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(answer->at("id").dump(), id.dump());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Batches (2025-03-26)

TEST(Batch, MixedRequestsAndNotifications) {
    Conversation conversation(batch_revision);
    const auto& revision = *conversation.profile;
    const json batch = json::array({
        Peer::make_request(revision, "b1", "ping"),
        json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
        Peer::make_request(revision, "b2", "tools/list"),
        Peer::make_request(revision, "b3", "tools/call", {{"name", "gated"}}),
        Peer::make_request(revision, "b4", "tools/call", {{"name", "echo"}}),
        json(17),
    });
    conversation.peer.send(neomcp::serialize(batch));
    auto parked = conversation.host.wait_parked(patience);
    ASSERT_TRUE(parked);
    conversation.flush();
    for (const auto& message : conversation.peer.transcript()) {
        EXPECT_FALSE(message.is_array()) << "the batch is answered after all of its requests completed";
    }
    std::thread finish([call = *parked] { call.done(text_result("gated")); });
    finish.join();
    const auto answer = conversation.peer.next_other(patience);
    ASSERT_TRUE(answer);
    ASSERT_TRUE(answer->is_array()) << answer->dump();
    ASSERT_EQ(answer->size(), 5u);
    EXPECT_EQ(answer->at(0).at("id"), "b1");
    EXPECT_EQ(answer->at(1).at("id"), "b2");
    EXPECT_EQ(answer->at(2).at("id"), "b3");
    EXPECT_EQ(answer->at(2).at("result").at("content").at(0).at("text"), "gated");
    EXPECT_EQ(answer->at(3).at("id"), "b4");
    EXPECT_EQ(error_code_of(answer->at(4)), -32600) << "an element that is not a message";
    EXPECT_TRUE(answer->at(4).at("id").is_null());
}

TEST(Batch, OnlyNotificationsEmptyAndInitializeInside) {
    Conversation conversation(batch_revision);
    const std::size_t before = conversation.peer.transcript().size();
    conversation.peer.send(R"([{"jsonrpc":"2.0","method":"notifications/initialized"},)"
                           R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":5}}])");
    conversation.flush();
    EXPECT_EQ(conversation.peer.transcript().size(), before + 1) << "nothing answers a batch of notifications";

    conversation.peer.send("[]");
    auto answer = conversation.peer.next_other(patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32600);
    EXPECT_TRUE(answer->at("id").is_null());

    const json with_initialize = json::array({Peer::initialize_request(*conversation.profile, "i2"),
                                              Peer::make_request(*conversation.profile, "p2", "ping")});
    conversation.peer.send(neomcp::serialize(with_initialize));
    answer = conversation.peer.next_other(patience);
    ASSERT_TRUE(answer && answer->is_array());
    ASSERT_EQ(answer->size(), 2u);
    EXPECT_EQ(error_code_of(answer->at(0)), -32600);
    EXPECT_EQ(answer->at(1).at("result"), json::object());
}

TEST(Batch, CancelledMemberIsLeftOut) {
    Conversation conversation(batch_revision);
    const json batch = json::array({Peer::make_request(*conversation.profile, "c1", "tools/call", {{"name", "gated"}}),
                                    Peer::make_request(*conversation.profile, "c2", "tools/call", {{"name", "gated"}})});
    conversation.peer.send(neomcp::serialize(batch));
    auto first = conversation.host.wait_parked(patience);
    auto second = conversation.host.wait_parked(patience);
    ASSERT_TRUE(first && second);
    conversation.peer.notify(cancelled("c1"));
    std::thread finish([one = *first, two = *second] {
        one.done(text_result("cancelled one"));
        two.done(text_result("second"));
    });
    finish.join();
    const auto answer = conversation.peer.next_other(patience);
    ASSERT_TRUE(answer && answer->is_array());
    ASSERT_EQ(answer->size(), 1u);
    EXPECT_EQ(answer->at(0).at("id"), "c2");
}

// ---------------------------------------------------------------------------------------------------------------
// close()

TEST(Close, CallsInFlightAreStoppedAndNothingIsWrittenAfter) {
    FakeHost host;
    Peer peer(host, test_options());
    const auto modern = builtin(modern_revision);
    for (int i = 0; i < 3; ++i) {
        peer.notify(Peer::make_request(*modern, i, "tools/call", {{"name", "gated"}, {"_meta", {{"progressToken", i}}}}));
    }
    std::vector<FakeHost::Parked> parked;
    for (int i = 0; i < 3; ++i) {
        auto next = host.wait_parked(patience);
        ASSERT_TRUE(next);
        parked.push_back(*next);
    }
    peer.close();
    const std::size_t closed_at = peer.transcript().size();
    for (const auto& call : parked) EXPECT_TRUE(call.control.stop_token().stop_requested());
    std::thread finish([&parked] {
        for (const auto& call : parked) {
            call.control.progress(1);
            call.done(text_result("after close"));
        }
    });
    finish.join();
    peer.send(neomcp::serialize(Peer::make_request(*modern, 9, "tools/list")));
    EXPECT_EQ(peer.transcript().size(), closed_at);
}

TEST(Close, RacesWithCompletions) {
    const auto modern = builtin(modern_revision);
    for (int round = 0; round < 200; ++round) {
        SCOPED_TRACE(round);
        FakeHost host;
        Peer peer(host, test_options());
        constexpr int calls = 4;
        for (int i = 0; i < calls; ++i) {
            peer.notify(Peer::make_request(*modern, i, "tools/call", {{"name", "gated"}, {"_meta", {{"progressToken", i}}}}));
        }
        std::vector<FakeHost::Parked> parked;
        for (int i = 0; i < calls; ++i) {
            auto next = host.wait_parked(patience);
            ASSERT_TRUE(next);
            parked.push_back(*next);
        }
        Gate gate;
        std::vector<std::thread> threads;
        for (const auto& call : parked) {
            threads.emplace_back([&gate, call] {
                gate.wait();
                call.control.progress(1);
                call.control.progress(2);
                call.done(text_result("raced"));
            });
        }
        gate.open();
        peer.close();
        const std::size_t closed_at = peer.transcript().size();
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(peer.transcript().size(), closed_at);
        for (const auto& message : peer.transcript()) {
            if (message.contains("id")) {
                EXPECT_TRUE(message.contains("result")) << message.dump();
            }
        }
    }
}

TEST(Close, FromInsideTheSink) {
    FakeHost host;
    std::unique_ptr<neomcp::Session> session;
    std::vector<std::string> lines;
    auto made = neomcp::Session::create(test_options(), host, [&](std::string line) {
        lines.push_back(std::move(line));
        session->close();
    });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
    session->receive(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    session->receive(R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
    EXPECT_EQ(lines.size(), 1u);
}

// ---------------------------------------------------------------------------------------------------------------
// A sink that fails

// Throws instead of writing the line it was told to fail on.
struct FailingSink {
    void write(std::string line) {
        const std::lock_guard<std::mutex> lock(mutex);
        ++attempts;
        if (attempts == fail_at) throw std::runtime_error("the transport failed");
        lines.push_back(json::parse(line));
    }
    void fail_on_next() {
        const std::lock_guard<std::mutex> lock(mutex);
        fail_at = attempts + 1;
    }
    std::size_t tried() {
        const std::lock_guard<std::mutex> lock(mutex);
        return attempts;
    }

    std::mutex mutex;
    std::size_t attempts = 0;
    std::size_t fail_at = 0;  // 0: never
    std::vector<json> lines;
};

std::unique_ptr<neomcp::Session> session_writing_to(FakeHost& host, FailingSink& sink) {
    auto made = neomcp::Session::create(test_options(), host, [&sink](std::string line) { sink.write(std::move(line)); });
    if (!std::holds_alternative<std::unique_ptr<neomcp::Session>>(made)) throw std::logic_error("no session");
    return std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
}

std::string modern_line(const std::string& id, const std::string& method, json params = json::object()) {
    return neomcp::serialize(Peer::make_request(*builtin(modern_revision), id, method, std::move(params)));
}

TEST(SinkFailure, TheReadingThreadGetsItAndTheSessionCloses) {
    FakeHost host;
    FailingSink sink;
    auto session = session_writing_to(host, sink);
    session->receive(modern_line("1", "server/discover"));
    sink.fail_on_next();
    EXPECT_THROW(session->receive(modern_line("2", "server/discover")), std::runtime_error);
    EXPECT_NO_THROW(session->receive(modern_line("3", "server/discover")));
    EXPECT_EQ(sink.tried(), 2u) << "nothing is written after the failure";
}

TEST(SinkFailure, AToolCompletingInlineDuringReceiveLeavesItToReceive) {
    FakeHost host;
    FailingSink sink;
    auto session = session_writing_to(host, sink);
    sink.fail_on_next();
    EXPECT_THROW(session->receive(modern_line("1", "tools/call", {{"name", "echo"}})), std::runtime_error);
    EXPECT_NO_THROW(session->receive(modern_line("2", "server/discover")));
    EXPECT_EQ(sink.tried(), 1u);
}

// A tool's thread completes or reports while the sink fails: nothing reaches that thread, and the session closes.
void expect_tool_thread_is_spared(bool through_progress) {
    FakeHost host;
    FailingSink sink;
    auto session = session_writing_to(host, sink);
    session->receive(modern_line("a", "tools/call", {{"name", "gated"}, {"_meta", {{"progressToken", "ta"}}}}));
    session->receive(modern_line("b", "tools/call", {{"name", "gated"}}));
    auto a = host.wait_parked(patience);
    auto b = host.wait_parked(patience);
    ASSERT_TRUE(a && b);
    sink.fail_on_next();
    bool escaped = false;
    std::thread tool([call = *a, through_progress, &escaped] {
        try {
            if (through_progress) {
                call.control.progress(1);
            } else {
                call.done(text_result("a"));
            }
        } catch (...) {
            escaped = true;
        }
    });
    tool.join();
    EXPECT_FALSE(escaped) << "the sink's exception reached the tool's thread";
    EXPECT_EQ(sink.tried(), 1u);
    EXPECT_TRUE(b->control.stop_token().stop_requested()) << "the session closed itself";
    EXPECT_NO_THROW(session->receive(modern_line("c", "server/discover")));
    EXPECT_NO_THROW(b->done(text_result("b")));
    EXPECT_EQ(sink.tried(), 1u) << "nothing is written after the failure";
}

TEST(SinkFailure, ACompletionOnAToolThreadIsSpared) { expect_tool_thread_is_spared(false); }
TEST(SinkFailure, ProgressOnAToolThreadIsSpared) { expect_tool_thread_is_spared(true); }

TEST(Close, HandlesOutliveTheSession) {
    FakeHost host;
    std::optional<FakeHost::Parked> parked;
    {
        Peer peer(host, test_options());
        peer.notify(Peer::make_request(*builtin(modern_revision), 1, "tools/call", {{"name", "gated"}}));
        parked = host.wait_parked(patience);
        ASSERT_TRUE(parked);
    }
    EXPECT_TRUE(parked->control.stop_token().stop_requested());
    EXPECT_EQ(parked->control.profile().revision(), modern_revision);
    parked->control.progress(1);
    parked->control.log(neomcp::LogLevel::error, "gone");
    parked->done(text_result("nobody listens"));
}

}  // namespace
