// Tests of the protocol core through the in-memory peer.
//
// Expected per-revision values are written here from the specification pages and schemas of each revision, not
// read back from the profiles, so that a wrong profile makes a test fail. Every wait is on an event a test
// controls; "nothing was written" is shown by a later request whose answer arrives (the session writes in order).
#include <algorithm>
#include <atomic>
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
#include <string_view>
#include <stop_token>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "core/base64.h"
#include "neomcp/base64.h"
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
    // Kept out of the macro's arguments: one compiler in the build matrix misreads a raw string with quotes there.
    const std::string_view brackets_in_a_string = R"(["[[[[\"[[["])";
    EXPECT_FALSE(neomcp::nesting_exceeds(brackets_in_a_string, 1)) << "brackets inside strings do not count";
    EXPECT_FALSE(neomcp::nesting_exceeds("[][][][]", 1));
}

// ---------------------------------------------------------------------------------------------------------------
// Base64 in the strict form

// A plain encoder, written here so that the decoder is checked against an independent implementation.
std::string encode_base64(std::string_view bytes) {
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto byte = [&](std::size_t i) { return i < bytes.size() ? static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) : 0u; };
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned value = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        const std::size_t present = std::min<std::size_t>(3, bytes.size() - i);
        out.push_back(digits[(value >> 18) & 63u]);
        out.push_back(digits[(value >> 12) & 63u]);
        out.push_back(present > 1 ? digits[(value >> 6) & 63u] : '=');
        out.push_back(present > 2 ? digits[value & 63u] : '=');
    }
    return out;
}

TEST(Base64, TestVectorsOfRfc4648) {
    const std::vector<std::pair<std::string, std::string>> vectors{
        {"", ""}, {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"}, {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"},
        {"Zm9vYmFy", "foobar"}};
    for (const auto& [text, bytes] : vectors) {
        SCOPED_TRACE(text);
        EXPECT_EQ(neomcp::detail::decode_base64(text), bytes);
    }
}

// The examples of the encoded header value form (basic/transports/streamable-http.mdx of 2026-07-28).
TEST(Base64, ExamplesOfTheEncodedHeaderValue) {
    const std::vector<std::pair<std::string, std::string>> examples{
        {"SGVsbG8sIOS4lueVjA==", "Hello, \xe4\xb8\x96\xe7\x95\x8c"},
        {"IHBhZGRlZCA=", " padded "},
        {"bGluZTEKbGluZTI=", "line1\nline2"},
        {"PT9iYXNlNjQ/bGl0ZXJhbD89", "=?base64?literal?="},
    };
    for (const auto& [text, bytes] : examples) {
        SCOPED_TRACE(text);
        EXPECT_EQ(neomcp::detail::decode_base64(text), bytes);
    }
}

TEST(Base64, EveryByteValueRoundTrips) {
    std::string bytes;
    for (int value = 0; value < 256; ++value) bytes.push_back(static_cast<char>(value));
    for (std::size_t length = 0; length <= bytes.size(); length += 85) {
        const std::string part = bytes.substr(bytes.size() - length);
        EXPECT_EQ(neomcp::detail::decode_base64(encode_base64(part)), part) << length;
    }
    EXPECT_EQ(neomcp::detail::decode_base64(encode_base64(bytes)), bytes);
}

// The encoder a server uses is the plain one above, and the strict decoder reads back what it wrote.
TEST(Base64, TheLibraryEncoderWritesTheTestVectorsOfRfc4648) {
    const std::vector<std::pair<std::string, std::string>> vectors{
        {"", ""}, {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"}, {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"},
        {"Zm9vYmFy", "foobar"}};
    for (const auto& [text, bytes] : vectors) {
        SCOPED_TRACE(text);
        EXPECT_EQ(neomcp::base64_encode(bytes), text);
        const auto raw = std::as_bytes(std::span<const char>(bytes.data(), bytes.size()));
        EXPECT_EQ(neomcp::base64_encode(raw), text);
    }
}

TEST(Base64, TheLibraryEncoderAgreesWithThePlainOneAndTheDecoder) {
    std::string bytes;
    for (int value = 0; value < 256; ++value) bytes.push_back(static_cast<char>(value));
    for (std::size_t length = 0; length <= bytes.size(); ++length) {
        const std::string part = bytes.substr(0, length);
        const std::string text = neomcp::base64_encode(part);
        EXPECT_EQ(text, encode_base64(part)) << length;
        EXPECT_EQ(neomcp::detail::decode_base64(text), part) << length;
    }
}

TEST(Base64, RejectsEverythingButTheStrictForm) {
    for (const std::string text : {
             "SGVsbG8",       // not a multiple of four characters (padding missing)
             "SGVs!!!bG8=",   // characters outside the alphabet
             "Zh==",          // the bits after the data are not zero
             "Zm9=",          // the same with one padding character
             "-_-_",          // the URL-safe alphabet
             "Zm9v Zg==",     // whitespace
             "Zm9v\nZg==",    // a line break
             "Zm9vZg==\n",    // a trailing line break
             "Zg==Zg==",      // padding before the end
             "Z===",          // three padding characters
             "====",          // padding only
             "=",
             "Zg=",
             "Zm9v====",
         }) {
        SCOPED_TRACE(text);
        EXPECT_FALSE(neomcp::detail::decode_base64(text));
    }
    EXPECT_FALSE(neomcp::detail::decode_base64(std::string("Zm9v\0Zg=", 8))) << "a NUL byte";
    EXPECT_FALSE(neomcp::detail::decode_base64("Zm9v\xc3\xa9Zg")) << "bytes above 0x7f";
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
        {profile::Slot::server_input, neomcp::MechanismSlot::server_input},
        {profile::Slot::change_feed, neomcp::MechanismSlot::change_feed},
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
    neomcp::SessionOptions no_queue = test_options();
    no_queue.max_queued_notifications = 0;
    EXPECT_TRUE(std::holds_alternative<neomcp::CreateError>(neomcp::Session::create(no_queue, host, [](std::string) {})));
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
        EXPECT_EQ(answer->at("error").at("data").at("supported"), strings({modern_revision}))
            << "only the revisions a request can name";
    }
}

// The lists a client can pick a revision from hold the revisions it can name the same way; the lists in errors that
// name no revision tell the reader everything the server serves.
TEST(Era, SupportedListsFollowTheOpeningMechanism) {
    neomcp::SessionOptions options = test_options();
    options.profiles = profiles_of(all_revisions);  // the legacy revisions and one modern revision
    FakeHost host;
    Peer peer(host, options);
    const auto modern = builtin(modern_revision);
    json unsupported = Peer::make_request(*modern, 1, "tools/list");
    unsupported["params"]["_meta"][version_key] = "2099-01-01";
    const auto refused = peer.request(unsupported, patience);
    ASSERT_TRUE(refused);
    EXPECT_EQ(error_code_of(*refused), -32022);
    EXPECT_EQ(refused->at("error").at("data").at("supported"), strings({modern_revision}));
    const auto discover = peer.request(Peer::make_request(*modern, 2, "server/discover"), patience);
    ASSERT_TRUE(discover);
    EXPECT_EQ(result_of(*discover).at("supportedVersions"), strings({modern_revision}));
    const auto unnamed =
        peer.request(json{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}, {"params", json::object()}}, patience);
    ASSERT_TRUE(unnamed);
    EXPECT_EQ(error_code_of(*unnamed), -32602);
    EXPECT_EQ(unnamed->at("error").at("data").at("supported"), strings(newest_first));

    // With a second revision of the same mechanism (a copy of the modern profile under a later name), both are
    // listed, newest first.
    json document = json::parse(profile::builtin_text(modern_revision));
    document["revision"] = "2099-01-01";
    auto loaded = profile::load(document.dump());
    ASSERT_TRUE(std::holds_alternative<profile::ProfilePtr>(loaded));
    options.profiles.push_back(std::get<profile::ProfilePtr>(loaded));
    Peer wider(host, options);
    const auto both = wider.request(Peer::make_request(*modern, 4, "server/discover"), patience);
    ASSERT_TRUE(both);
    EXPECT_EQ(result_of(*both).at("supportedVersions"), strings({"2099-01-01", modern_revision}));
    const auto later = wider.request(Peer::make_request(*std::get<profile::ProfilePtr>(loaded), 5, "tools/list"), patience);
    ASSERT_TRUE(later && later->contains("result")) << "a request naming the later revision is served";
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
        if (revision == "2025-11-25") {
            // Its error responses may leave the id out, and the connection says this client speaks it.
            EXPECT_FALSE(answer->contains("id"));
        } else {
            // The others require it, or open no connection that says which revision the client speaks.
            EXPECT_TRUE(answer->at("id").is_null());
        }
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
    EXPECT_EQ(result.at("supportedVersions"), strings({modern_revision})) << "only the revisions a request can name";
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

// A host whose one tool reports that the host itself failed.
class FailingHost : public neomcp::ToolHost {
public:
    FailingHost() {
        tool_.name = "fails";
        tool_.description = "fails inside the host";
    }
    std::span<const neomcp::ToolDefinition> tools() const override { return {&tool_, 1}; }
    void call(const neomcp::CallRequest&, neomcp::CallControl, neomcp::Completion done) override {
        done(neomcp::HostFailure{"the secret detail"});
    }

private:
    neomcp::ToolDefinition tool_;
};

TEST_P(PerRevision, HostFailureIsAnInternalErrorWithoutDetails) {
    const auto revision = builtin(GetParam());
    FailingHost host;
    Peer peer(host, test_options());
    if (is_legacy(GetParam())) {
        ASSERT_TRUE(peer.initialize(*revision, patience));
    }
    const auto answer = peer.request(Peer::make_request(*revision, 1, "tools/call", {{"name", "fails"}}), patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32603) << answer->dump();
    EXPECT_EQ(error_message_of(*answer), "Internal error: the tool call failed");
    EXPECT_FALSE(answer->at("error").contains("data"));
}

TEST_P(PerRevision, HostFailureCarriesItsMessageWhenDetailsAreExposed) {
    const auto revision = builtin(GetParam());
    FailingHost host;
    neomcp::SessionOptions options = test_options();
    options.expose_error_details = true;
    Peer peer(host, options);
    if (is_legacy(GetParam())) {
        ASSERT_TRUE(peer.initialize(*revision, patience));
    }
    const auto answer = peer.request(Peer::make_request(*revision, 1, "tools/call", {{"name", "fails"}}), patience);
    ASSERT_TRUE(answer);
    EXPECT_EQ(error_code_of(*answer), -32603) << answer->dump();
    EXPECT_EQ(error_message_of(*answer), "Internal error: the tool call failed: the secret detail");
}

TEST(Batch, HostFailureIsAnswerOfItsMember) {
    FailingHost host;
    Peer peer(host, test_options());
    const auto revision = builtin(batch_revision);
    ASSERT_TRUE(peer.initialize(*revision, patience));
    peer.send(neomcp::serialize(json::array({Peer::make_request(*revision, "f1", "tools/call", {{"name", "fails"}}),
                                             Peer::make_request(*revision, "p1", "ping")})));
    const auto answer = peer.next_other(patience);
    ASSERT_TRUE(answer && answer->is_array());
    ASSERT_EQ(answer->size(), 2u);
    EXPECT_EQ(error_code_of(answer->at(0)), -32603);
    EXPECT_EQ(answer->at(1).at("result"), json::object());
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

// The nesting of a line is counted over arrays and objects alike, so a line of objects, or of both, that is far deeper
// than the limit is refused as the line of brackets above is, and one at the limit is not refused for its depth.
// What the session does with either must neither recurse once per level nor take time that grows faster than the
// line; the bound on the wait for the answer is what shows it.
TEST(Input, ObjectsAndMixedNestingBeyondTheLimitAreRefusedAtAnyDepth) {
    Conversation legacy("2025-06-18");
    const std::size_t limit = neomcp::SessionOptions{}.max_nesting_depth;
    const auto objects = [](std::size_t depth) {
        std::string line;
        for (std::size_t level = 0; level < depth; ++level) line += "{\"a\":";
        line += "0";
        line.append(depth, '}');
        return line;
    };
    const auto mixed = [](std::size_t depth) {
        std::string line;
        for (std::size_t level = 0; level < depth; ++level) line += level % 2 == 0 ? "{\"a\":" : "[";
        line += "0";
        for (std::size_t level = depth; level > 0; --level) line += (level - 1) % 2 == 0 ? '}' : ']';
        return line;
    };
    for (const std::size_t depth : {limit + 1, std::size_t{1000}, std::size_t{200000}}) {
        for (const std::string& line : {objects(depth), mixed(depth)}) {
            SCOPED_TRACE(depth);
            legacy.peer.send(line);
            const auto answer = legacy.peer.next_other(patience);
            ASSERT_TRUE(answer);
            EXPECT_EQ(error_code_of(*answer), -32600);
            EXPECT_TRUE(answer->at("id").is_null()) << "refused before the line was read as a message";
        }
    }
    EXPECT_EQ(result_of(legacy.ask("ping")), json::object()) << "the connection continues";

    // A request that reaches exactly the limit (the message is the first level and `params` brings the others) is
    // read, and answered to its id.
    std::string at_the_limit = R"({"jsonrpc":"2.0","id":"deep","method":"ping","params":)";
    at_the_limit += objects(limit - 1);
    at_the_limit += "}";
    legacy.peer.send(at_the_limit);
    const auto answer = legacy.peer.await_response("deep", patience);
    ASSERT_TRUE(answer) << "a line at the limit is not refused for its depth";
    EXPECT_NE(error_code_of(*answer), -32600) << answer->dump().substr(0, 200);
}

// One object of very many members, repeated keys among them, as the whole line and as the parameters of a request.
TEST(Input, AnObjectOfTwoHundredThousandMembersIsAnsweredWithoutCrashingOrWaitingLong) {
    Conversation legacy("2025-06-18");
    std::string members;
    for (int index = 0; index < 200000; ++index) {
        if (index > 0) members += ',';
        // Every other key is the same one, the rest are distinct.
        members += index % 2 == 0 ? std::string("\"same\":") : "\"key" + std::to_string(index) + "\":";
        members += std::to_string(index);
    }

    legacy.peer.send("{" + members + "}");
    const auto not_a_message = legacy.peer.next_other(patience);
    ASSERT_TRUE(not_a_message);
    EXPECT_EQ(error_code_of(*not_a_message), -32600);

    legacy.peer.send(R"({"jsonrpc":"2.0","id":"wide","method":"ping","params":{)" + members + "}}");
    const auto answer = legacy.peer.await_response("wide", patience);
    ASSERT_TRUE(answer);
    EXPECT_TRUE(answer->contains("result") || answer->contains("error")) << answer->dump().substr(0, 200);

    EXPECT_EQ(result_of(legacy.ask("ping")), json::object()) << "the connection continues";
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

// ---------------------------------------------------------------------------------------------------------------
// A client that reads slower than tools report

TEST(Backpressure, NotificationsBeyondTheQueueBoundAreDropped) {
    FakeHost host;
    HoldingSink sink;
    neomcp::SessionOptions options = test_options();
    options.max_queued_notifications = 3;
    options.protocol_logging = true;
    auto made = neomcp::Session::create(options, host, [&sink](std::string line) { sink.write(std::move(line)); });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    neomcp::Session& session = *std::get<std::unique_ptr<neomcp::Session>>(made);
    // w's response holds the writer in the sink; p reports; r completes while the queue is full.
    session.receive(modern_line("w", "tools/call", {{"name", "gated"}}));
    session.receive(modern_line("p", "tools/call",
                                {{"name", "gated"}, {"_meta", {{"progressToken", "tp"}, {log_level_key, "debug"}}}}));
    session.receive(modern_line("r", "tools/call", {{"name", "gated"}}));
    auto w = host.wait_parked(patience);
    auto p = host.wait_parked(patience);
    auto r = host.wait_parked(patience);
    ASSERT_TRUE(w && p && r);
    {
        const std::lock_guard<std::mutex> lock(sink.mutex);
        sink.hold = true;
    }
    std::thread writer([call = *w] { call.done(text_result("w")); });
    ASSERT_TRUE(sink.wait_held());
    std::thread burst([reporter = *p, finisher = *r] {
        for (const double value : {1.0, 2.0, 3.0, 10.0, 11.0}) reporter.control.progress(value);
        reporter.control.log(neomcp::LogLevel::error, "dropped");
        finisher.done(text_result("r"));
    });
    burst.join();
    sink.release();
    writer.join();
    p->control.progress(4);  // greater than the last value sent (3), though not than the dropped ones
    p->done(text_result("p"));

    std::vector<std::string> written;
    const std::lock_guard<std::mutex> lock(sink.mutex);
    for (const auto& line : sink.lines) {
        if (line.contains("id")) {
            ASSERT_TRUE(line.contains("result")) << line.dump();
            written.push_back("response " + line.at("id").get<std::string>());
        } else {
            ASSERT_EQ(line.at("method"), "notifications/progress") << line.dump();
            written.push_back("progress " + std::to_string(static_cast<int>(line.at("params").at("progress").get<double>())));
        }
    }
    EXPECT_EQ(written, (std::vector<std::string>{"response w", "progress 1", "progress 2", "progress 3", "response r",
                                                 "progress 4", "response p"}));
}

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

// ---------------------------------------------------------------------------------------------------------------
// Idle

// Counts the calls of SessionOptions::on_idle and what the session says inside the callback.
struct IdleProbe {
    void fired() {
        const neomcp::Session* current = session.load();
        const bool idle_inside = current != nullptr && current->idle();  // would deadlock if the callback held a lock
        const std::lock_guard<std::mutex> lock(mutex);
        ++count;
        if (!idle_inside) ++not_idle_inside;
        changed.notify_all();
    }
    bool wait_count(int target) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, patience, [&] { return count >= target; });
    }
    int value() {
        const std::lock_guard<std::mutex> lock(mutex);
        return count;
    }
    int disagreements() {
        const std::lock_guard<std::mutex> lock(mutex);
        return not_idle_inside;
    }

    std::atomic<const neomcp::Session*> session{nullptr};
    std::mutex mutex;
    std::condition_variable changed;
    int count = 0;
    int not_idle_inside = 0;
};

neomcp::SessionOptions idle_options(IdleProbe& probe) {
    neomcp::SessionOptions options = test_options();
    options.on_idle = [&probe] { probe.fired(); };
    return options;
}

TEST(Idle, FollowsTheCallsInFlight) {
    FakeHost host;
    IdleProbe probe;
    Peer peer(host, idle_options(probe));
    probe.session = &peer.session();
    EXPECT_TRUE(peer.session().idle());

    peer.notify(Peer::make_request(*builtin(modern_revision), 1, "tools/call", {{"name", "gated"}}));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    EXPECT_FALSE(peer.session().idle());
    EXPECT_EQ(probe.value(), 0);

    parked->done(text_result("done"));
    // The callback runs after the answer was written, so the answer is there when it has run.
    ASSERT_TRUE(probe.wait_count(1));
    ASSERT_TRUE(peer.await_response(1, patience));
    EXPECT_TRUE(peer.session().idle());
    EXPECT_EQ(probe.disagreements(), 0);
}

TEST(Idle, AnAnswerWrittenWhileNothingElseWaitsTellsToo) {
    FakeHost host;
    IdleProbe probe;
    Peer peer(host, idle_options(probe));
    probe.session = &peer.session();
    const auto answer = peer.request(Peer::make_request(*builtin(modern_revision), 1, "tools/list"), patience);
    ASSERT_TRUE(answer);
    ASSERT_TRUE(probe.wait_count(1));
    EXPECT_TRUE(peer.session().idle());
}

TEST(Idle, ACancelledCallIsFinishedAlthoughItsToolMayRun) {
    FakeHost host;
    IdleProbe probe;
    Peer peer(host, idle_options(probe));
    probe.session = &peer.session();
    peer.notify(Peer::make_request(*builtin(modern_revision), 1, "tools/call", {{"name", "gated"}}));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    EXPECT_FALSE(peer.session().idle());

    peer.notify(cancelled(1));
    // The callback runs on the thread that delivered the cancellation, before notify() returned.
    EXPECT_EQ(probe.value(), 1);
    EXPECT_TRUE(peer.session().idle());
    EXPECT_TRUE(parked->control.stop_token().stop_requested());

    const int before = probe.value();
    parked->done(text_result("late"));  // dropped: nothing is written, nothing becomes idle
    EXPECT_EQ(probe.value(), before);
}

TEST(Idle, ABatchIsPendingUntilItsAnswerWasHandedOver) {
    FakeHost host;
    IdleProbe probe;
    Peer peer(host, idle_options(probe));
    probe.session = &peer.session();
    ASSERT_TRUE(peer.initialize(*builtin(batch_revision), patience));
    ASSERT_TRUE(probe.wait_count(1));  // the answer to initialize
    const int before = probe.value();

    json batch = json::array();
    batch.push_back(Peer::make_request(*builtin(batch_revision), 10, "tools/call", {{"name", "gated"}}));
    batch.push_back(Peer::make_request(*builtin(batch_revision), 11, "tools/call", {{"name", "echo"}}));
    peer.send(batch.dump());
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    EXPECT_FALSE(peer.session().idle());  // call 11 was answered inside the batch, call 10 was not
    EXPECT_EQ(probe.value(), before);

    parked->done(text_result("done"));
    const auto answer = peer.next_other(patience);
    ASSERT_TRUE(answer);
    EXPECT_TRUE(answer->is_array());
    ASSERT_TRUE(probe.wait_count(before + 1));
    EXPECT_TRUE(peer.session().idle());
    EXPECT_EQ(probe.value(), before + 1);
    EXPECT_EQ(probe.disagreements(), 0);
}

TEST(Idle, ABatchOfNotificationsOnlyEndsIdle) {
    FakeHost host;
    IdleProbe probe;
    Peer peer(host, idle_options(probe));
    probe.session = &peer.session();
    ASSERT_TRUE(peer.initialize(*builtin(batch_revision), patience));
    ASSERT_TRUE(probe.wait_count(1));
    const int before = probe.value();
    peer.send(json::array({json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}}).dump());
    ASSERT_TRUE(probe.wait_count(before + 1));
    EXPECT_TRUE(peer.session().idle());
}

TEST(Idle, ACloseStopsTheCallbackAndLeavesTheSessionIdle) {
    FakeHost host;
    IdleProbe probe;
    Peer peer(host, idle_options(probe));
    probe.session = &peer.session();
    peer.notify(Peer::make_request(*builtin(modern_revision), 1, "tools/call", {{"name", "gated"}}));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    peer.close();
    EXPECT_TRUE(peer.session().idle());
    const int before = probe.value();
    parked->done(text_result("late"));
    EXPECT_EQ(probe.value(), before);
}

TEST(Idle, AThrowingCallbackIsSwallowed) {
    FakeHost host;
    neomcp::SessionOptions options = test_options();
    options.on_idle = [] { throw std::runtime_error("the callback fails"); };
    Peer peer(host, std::move(options));
    const auto first = peer.request(Peer::make_request(*builtin(modern_revision), 1, "tools/list"), patience);
    const auto second = peer.request(Peer::make_request(*builtin(modern_revision), 2, "tools/list"), patience);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_TRUE(first->contains("result"));
    EXPECT_TRUE(second->contains("result"));
}

TEST(Idle, ConcurrentCompletionsEndIdleOnce) {
    FakeHost host;
    IdleProbe probe;
    Peer peer(host, idle_options(probe));
    probe.session = &peer.session();
    constexpr int calls = 8;
    for (int i = 1; i <= calls; ++i) {
        peer.notify(Peer::make_request(*builtin(modern_revision), i, "tools/call", {{"name", "gated"}}));
    }
    std::vector<FakeHost::Parked> parked;
    for (int i = 0; i < calls; ++i) {
        auto call = host.wait_parked(patience);
        ASSERT_TRUE(call);
        parked.push_back(std::move(*call));
    }
    EXPECT_FALSE(peer.session().idle());
    Gate go;
    std::vector<std::thread> threads;
    for (auto& call : parked) {
        threads.emplace_back([&go, &call] {
            go.wait();
            call.done(text_result("done"));
        });
    }
    go.open();
    for (auto& thread : threads) thread.join();
    for (int i = 1; i <= calls; ++i) ASSERT_TRUE(peer.await_response(i, patience));
    EXPECT_TRUE(peer.session().idle());
    // Idleness was reported at least once, and never while a call was left: the last report is the true one.
    EXPECT_GE(probe.value(), 1);
    EXPECT_EQ(probe.disagreements(), 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Tagged output and input units

// Records what a session created with create_tagged writes, per unit, and counts any output of a unit that follows
// the unit's last one. It can hold the thread that writes one output of a chosen unit, so that a test can act while
// that output is being written.
struct Recorder {
    struct Unit {
        std::vector<neomcp::Output> outputs;
        int ends = 0;  // outputs with `last` set
    };

    void write(neomcp::Output output) {
        std::unique_lock<std::mutex> lock(mutex);
        const std::optional<neomcp::UnitId> unit = output.unit;
        if (unit) {
            Unit& entry = units[*unit];
            if (entry.ends > 0) ++after_end;
            if (output.last) ++entry.ends;
            entry.outputs.push_back(std::move(output));
        } else {
            connection.push_back(std::move(output));
        }
        ++count;
        changed.notify_all();
        if (unit && hold == unit) {
            hold.reset();
            held = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
            held = false;
        }
    }
    bool wait_end(neomcp::UnitId unit) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, patience, [&] { return units[unit].ends > 0; });
    }
    bool wait_held() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, patience, [&] { return held; });
    }
    void hold_on(neomcp::UnitId unit) {
        const std::lock_guard<std::mutex> lock(mutex);
        hold = unit;
    }
    void release() {
        const std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
    Unit unit(neomcp::UnitId id) {
        const std::lock_guard<std::mutex> lock(mutex);
        return units[id];
    }
    std::size_t written() {
        const std::lock_guard<std::mutex> lock(mutex);
        return count;
    }
    int outputs_after_an_end() {
        const std::lock_guard<std::mutex> lock(mutex);
        return after_end;
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::map<neomcp::UnitId, Unit> units;
    std::vector<neomcp::Output> connection;
    std::size_t count = 0;
    int after_end = 0;
    std::optional<neomcp::UnitId> hold;
    bool held = false;
    bool released = false;
};

std::unique_ptr<neomcp::Session> tagged_session(neomcp::ToolHost& host, Recorder& recorder,
                                                neomcp::SessionOptions options = test_options()) {
    auto made = neomcp::Session::create_tagged(std::move(options), host,
                                               [&recorder](neomcp::Output output) { recorder.write(std::move(output)); });
    if (auto* error = std::get_if<neomcp::CreateError>(&made)) throw std::runtime_error(error->message);
    return std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
}

std::string line_of(const json& message) { return neomcp::serialize(message); }

json gated_call(const profile::Profile& revision, const json& id, const json& token = nullptr) {
    json params = {{"name", "gated"}};
    if (!token.is_null()) params["_meta"] = json{{"progressToken", token}};
    return Peer::make_request(revision, *neomcp::RequestId::from_json(id), "tools/call", std::move(params));
}

// The output's message; null for an end marker.
json message_of(const neomcp::Output& output) {
    if (output.kind == neomcp::OutputKind::end) return nullptr;
    return json::parse(output.line);
}

TEST(Units, RefusalNamesAreStatusReasons) {
    using neomcp::RefusalReason;
    const auto declared = profile::declared_status_reasons();
    for (const RefusalReason reason :
         {RefusalReason::parse_error, RefusalReason::invalid_request, RefusalReason::too_long, RefusalReason::too_deep,
          RefusalReason::header_mismatch, RefusalReason::unsupported_version, RefusalReason::invalid_request_meta,
          RefusalReason::unsupported_version_header, RefusalReason::method_not_found,
          RefusalReason::missing_connection, RefusalReason::missing_client_capability}) {
        EXPECT_NE(std::find(declared.begin(), declared.end(), neomcp::to_string(reason)), declared.end())
            << neomcp::to_string(reason);
    }
    EXPECT_EQ(neomcp::to_string(RefusalReason::none), "none");
}

TEST(Units, AnAnswerEndsItsUnit) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const auto modern = builtin(modern_revision);
    const auto legacy = builtin("2025-11-25");
    EXPECT_TRUE(session->receive(7, line_of(Peer::make_request(*modern, 1, "tools/list"))));
    EXPECT_TRUE(session->receive(8, line_of(Peer::initialize_request(*legacy, 2))));
    EXPECT_TRUE(session->receive(9, line_of(Peer::make_request(*legacy, 3, "tools/list"))));
    const std::vector<std::tuple<neomcp::UnitId, int, std::string>> expected{
        {7, 1, modern_revision}, {8, 2, "2025-11-25"}, {9, 3, "2025-11-25"}};
    for (const auto& [unit, id, revision] : expected) {
        SCOPED_TRACE(unit);
        const auto written = recorder.unit(unit);
        ASSERT_EQ(written.outputs.size(), 1u);
        const neomcp::Output& output = written.outputs[0];
        EXPECT_EQ(output.unit, unit);
        EXPECT_TRUE(output.last);
        EXPECT_EQ(output.kind, neomcp::OutputKind::response);
        EXPECT_EQ(output.refusal, neomcp::RefusalReason::none);
        ASSERT_TRUE(output.profile);
        EXPECT_EQ(output.profile->revision(), revision);
        EXPECT_EQ(message_of(output).at("id"), id);
        EXPECT_TRUE(message_of(output).contains("result"));
    }
}

TEST(Units, RefusalsCarryTheirReason) {
    using neomcp::RefusalReason;
    const auto modern = builtin(modern_revision);
    const auto legacy = builtin("2025-06-18");
    json unnamed_revision = Peer::make_request(*modern, 1, "tools/list");
    unnamed_revision["params"]["_meta"][version_key] = "2099-01-01";
    json no_capabilities = Peer::make_request(*modern, 1, "tools/list");
    no_capabilities["params"]["_meta"].erase(capabilities_key);
    json version_not_a_string = Peer::make_request(*modern, 1, "tools/list");
    version_not_a_string["params"]["_meta"][version_key] = 5;
    struct Case {
        std::string line;
        RefusalReason reason;
        std::string governing;  // the revision of the governing profile; empty: none known
    };
    const std::vector<Case> cases{
        {"{", RefusalReason::parse_error, ""},
        {std::string(300, ' '), RefusalReason::too_long, ""},
        {std::string(20, '[') + std::string(20, ']'), RefusalReason::too_deep, ""},
        {"42", RefusalReason::invalid_request, ""},
        {R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])", RefusalReason::invalid_request, ""},
        {line_of(Peer::make_request(*modern, 1, "ping")), RefusalReason::method_not_found, modern_revision},
        {line_of(unnamed_revision), RefusalReason::unsupported_version, modern_revision},
        {line_of(no_capabilities), RefusalReason::invalid_request_meta, modern_revision},
        {line_of(version_not_a_string), RefusalReason::invalid_request_meta, ""},
        {line_of(Peer::make_request(*legacy, 1, "tools/list")), RefusalReason::missing_connection, ""},
    };
    neomcp::SessionOptions options = test_options();
    options.max_message_bytes = 256;
    options.max_nesting_depth = 8;
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, options);
    neomcp::UnitId unit = 0;
    for (const auto& item : cases) {
        SCOPED_TRACE(item.line.substr(0, 80));
        EXPECT_TRUE(session->receive(++unit, item.line));
        const auto written = recorder.unit(unit);
        ASSERT_EQ(written.outputs.size(), 1u);
        const neomcp::Output& output = written.outputs[0];
        EXPECT_TRUE(output.last);
        EXPECT_EQ(output.kind, neomcp::OutputKind::response);
        EXPECT_EQ(output.refusal, item.reason) << neomcp::to_string(output.refusal);
        EXPECT_TRUE(message_of(output).contains("error"));
        EXPECT_EQ(output.profile ? output.profile->revision() : "", item.governing);
    }

    // On a connection: a second handshake, a duplicate id, a request the connection does not serve, and an
    // ordinary error answer, which is no refusal.
    EXPECT_TRUE(session->receive(++unit, line_of(Peer::initialize_request(*legacy, "init"))));
    ASSERT_TRUE(recorder.wait_end(unit));
    session->receive(++unit, line_of(gated_call(*legacy, "dup")));
    const auto in_flight = host.wait_parked(patience);  // kept: a dropped completion would answer the call
    ASSERT_TRUE(in_flight);
    const std::vector<std::pair<json, RefusalReason>> on_connection{
        {Peer::initialize_request(*legacy, "again"), RefusalReason::invalid_request},
        {gated_call(*legacy, "dup"), RefusalReason::invalid_request},
        {Peer::make_request(*legacy, "level", "logging/setLevel", {{"level", "info"}}), RefusalReason::method_not_found},
        {Peer::make_request(*legacy, "unknown", "tools/call", {{"name", "no_such_tool"}}), RefusalReason::none},
    };
    for (const auto& [message, reason] : on_connection) {
        SCOPED_TRACE(message.dump());
        EXPECT_TRUE(session->receive(++unit, line_of(message)));
        const auto written = recorder.unit(unit);
        ASSERT_EQ(written.outputs.size(), 1u);
        EXPECT_EQ(written.outputs[0].refusal, reason) << neomcp::to_string(written.outputs[0].refusal);
        EXPECT_TRUE(message_of(written.outputs[0]).contains("error"));
    }

    neomcp::SessionOptions modern_only = test_options();
    modern_only.profiles = profiles_of({modern_revision});
    Recorder other;
    auto served = tagged_session(host, other, modern_only);
    EXPECT_TRUE(served->receive(1, line_of(Peer::initialize_request(*legacy, 1))));
    ASSERT_EQ(other.unit(1).outputs.size(), 1u);
    EXPECT_EQ(other.unit(1).outputs[0].refusal, RefusalReason::method_not_found);
}

TEST(Units, NotificationsAndResponsesEndWithoutAnAnswer) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const auto legacy = builtin("2025-06-18");
    // Kept out of the macros' arguments: one compiler in the build matrix misreads a raw string with quotes there.
    constexpr std::string_view initialized = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    constexpr std::string_view response = R"({"jsonrpc":"2.0","id":77,"result":{}})";
    EXPECT_FALSE(session->receive(1, initialized));
    EXPECT_FALSE(session->receive(2, response));
    EXPECT_TRUE(session->receive(3, line_of(Peer::initialize_request(*legacy, 1))));
    EXPECT_FALSE(session->receive(4, initialized));
    for (const neomcp::UnitId unit : {1, 2, 4}) {
        SCOPED_TRACE(unit);
        const auto written = recorder.unit(unit);
        ASSERT_EQ(written.outputs.size(), 1u);
        const neomcp::Output& output = written.outputs[0];
        EXPECT_EQ(output.kind, neomcp::OutputKind::end);
        EXPECT_TRUE(output.last);
        EXPECT_TRUE(output.line.empty());
        EXPECT_EQ(output.unit, unit);
        EXPECT_EQ(output.refusal, neomcp::RefusalReason::none);
    }
    EXPECT_FALSE(recorder.unit(1).outputs[0].profile) << "no connection yet";
    ASSERT_TRUE(recorder.unit(4).outputs[0].profile);
    EXPECT_EQ(recorder.unit(4).outputs[0].profile->revision(), "2025-06-18") << "the connection's";
}

TEST(Units, ALaterAnswerEndsTheUnitAfterTheCallsNotifications) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const auto modern = builtin(modern_revision);
    EXPECT_TRUE(session->receive(5, line_of(gated_call(*modern, "c", "tc"))));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    EXPECT_TRUE(recorder.unit(5).outputs.empty());
    std::thread tool([call = *parked] {
        call.control.progress(1);
        call.done(text_result("done"));
    });
    tool.join();
    ASSERT_TRUE(recorder.wait_end(5));
    const auto written = recorder.unit(5);
    ASSERT_EQ(written.outputs.size(), 2u);
    EXPECT_EQ(written.outputs[0].kind, neomcp::OutputKind::notification);
    EXPECT_FALSE(written.outputs[0].last);
    EXPECT_EQ(written.outputs[0].unit, 5u);
    EXPECT_EQ(message_of(written.outputs[0]).at("params").at("progressToken"), "tc");
    EXPECT_EQ(written.outputs[1].kind, neomcp::OutputKind::response);
    EXPECT_TRUE(written.outputs[1].last);
    EXPECT_EQ(message_of(written.outputs[1]).at("id"), "c");
}

TEST(Units, ABatchIsOneUnit) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const auto revision = builtin(batch_revision);
    ASSERT_TRUE(session->receive(1, line_of(Peer::initialize_request(*revision, "init"))));
    const json batch = json::array({gated_call(*revision, "b1", "tb"), Peer::make_request(*revision, "b2", "ping"),
                                    json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}});
    EXPECT_TRUE(session->receive(2, line_of(batch)));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    std::thread tool([call = *parked] {
        call.control.progress(1);
        call.done(text_result("b1"));
    });
    tool.join();
    ASSERT_TRUE(recorder.wait_end(2));
    const auto written = recorder.unit(2);
    ASSERT_EQ(written.outputs.size(), 2u);
    EXPECT_EQ(written.outputs[0].kind, neomcp::OutputKind::notification);
    EXPECT_EQ(written.outputs[1].kind, neomcp::OutputKind::response);
    EXPECT_TRUE(written.outputs[1].last);
    ASSERT_TRUE(message_of(written.outputs[1]).is_array());
    EXPECT_EQ(message_of(written.outputs[1]).size(), 2u);
    ASSERT_TRUE(written.outputs[1].profile);
    EXPECT_EQ(written.outputs[1].profile->revision(), batch_revision);

    // Only notifications: no answer, one end.
    constexpr std::string_view notifications_only = R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])";
    EXPECT_FALSE(session->receive(3, notifications_only));
    const auto silent = recorder.unit(3);
    ASSERT_EQ(silent.outputs.size(), 1u);
    EXPECT_EQ(silent.outputs[0].kind, neomcp::OutputKind::end);
    EXPECT_EQ(recorder.outputs_after_an_end(), 0);
}

TEST(Units, ANumberMayBeUsedAgainOnlyAfterItsUnitEnded) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const auto modern = builtin(modern_revision);
    ASSERT_TRUE(session->receive(1, line_of(gated_call(*modern, "first"))));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    EXPECT_THROW(session->receive(1, line_of(Peer::make_request(*modern, "second", "tools/list"))),
                 std::invalid_argument);
    parked->done(text_result("first"));
    ASSERT_TRUE(recorder.wait_end(1));
    EXPECT_TRUE(session->receive(1, line_of(Peer::make_request(*modern, "third", "tools/list"))));
    const auto written = recorder.unit(1);
    ASSERT_EQ(written.outputs.size(), 2u);
    EXPECT_EQ(message_of(written.outputs[1]).at("id"), "third");
}

TEST(Units, ALineBelongsToNoUnit) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    session->receive(line_of(Peer::make_request(*builtin(modern_revision), 1, "tools/list")));
    session->receive(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    EXPECT_TRUE(recorder.units.empty());
    ASSERT_EQ(recorder.connection.size(), 1u) << "a notification line gets no end marker";
    EXPECT_FALSE(recorder.connection[0].unit);
    EXPECT_FALSE(recorder.connection[0].last);
    EXPECT_EQ(recorder.connection[0].kind, neomcp::OutputKind::response);
}

TEST(Units, AfterCloseInputIsIgnored) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    session->close();
    EXPECT_FALSE(session->receive(1, line_of(Peer::make_request(*builtin(modern_revision), 1, "tools/list"))));
    EXPECT_EQ(recorder.written(), 0u);
}

TEST(Units, ACancelledCallsLateAnswerWritesNothingAndItsUnitEndsOnce) {
    FakeHost host;
    Recorder recorder;
    neomcp::SessionOptions options = test_options();
    options.protocol_logging = true;
    auto session = tagged_session(host, recorder, options);
    const auto modern = builtin(modern_revision);
    json call = gated_call(*modern, "c1", "t1");
    call["params"]["_meta"][log_level_key] = "debug";
    ASSERT_TRUE(session->receive(1, line_of(call)));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    parked->control.progress(1);
    EXPECT_FALSE(session->receive(2, line_of(cancelled("c1"))));
    EXPECT_TRUE(parked->control.stop_token().stop_requested());
    std::thread late([call = *parked] {
        call.control.progress(2);
        call.control.log(neomcp::LogLevel::emergency, "late");
        call.done(text_result("late"));
    });
    late.join();
    ASSERT_TRUE(session->receive(3, line_of(Peer::make_request(*modern, "flush", "server/discover"))));
    const auto first = recorder.unit(1);
    ASSERT_EQ(first.outputs.size(), 2u);
    EXPECT_EQ(first.outputs[0].kind, neomcp::OutputKind::notification);
    EXPECT_EQ(first.outputs[1].kind, neomcp::OutputKind::end);
    EXPECT_TRUE(first.outputs[1].last);
    ASSERT_TRUE(first.outputs[1].profile);
    EXPECT_EQ(first.outputs[1].profile->revision(), modern_revision);
    EXPECT_EQ(first.ends, 1);
    EXPECT_EQ(recorder.unit(2).outputs.size(), 1u);
    EXPECT_EQ(recorder.unit(2).outputs[0].kind, neomcp::OutputKind::end);
    EXPECT_EQ(recorder.unit(3).ends, 1);
    EXPECT_EQ(recorder.outputs_after_an_end(), 0);
}

TEST(Units, ACancellationRacingTheAnswerEndsTheUnitOnce) {
    const auto modern = builtin(modern_revision);
    for (int round = 0; round < 200; ++round) {
        SCOPED_TRACE(round);
        FakeHost host;
        Recorder recorder;
        auto session = tagged_session(host, recorder);
        ASSERT_TRUE(session->receive(1, line_of(gated_call(*modern, "c"))));
        auto parked = host.wait_parked(patience);
        ASSERT_TRUE(parked);
        Gate gate;
        std::thread tool([&gate, call = *parked] {
            gate.wait();
            call.done(text_result("raced"));
        });
        gate.open();
        session->receive(2, line_of(cancelled("c")));
        tool.join();
        ASSERT_TRUE(session->receive(3, line_of(Peer::make_request(*modern, "flush", "server/discover"))));
        const auto written = recorder.unit(1);
        ASSERT_EQ(written.ends, 1);
        ASSERT_EQ(written.outputs.size(), 1u);
        EXPECT_EQ(recorder.outputs_after_an_end(), 0);
    }
}

TEST(Units, ManyUnitsInFlightAnswerOnManyThreadsEachTaggedWithItsOwnUnit) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const auto modern = builtin(modern_revision);
    constexpr int units = 16;
    for (int unit = 1; unit <= units; ++unit) {
        ASSERT_TRUE(session->receive(static_cast<neomcp::UnitId>(unit), line_of(gated_call(*modern, unit, unit))));
    }
    std::vector<FakeHost::Parked> parked;
    for (int i = 0; i < units; ++i) {
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
            call.done(text_result("done"));
        });
    }
    gate.open();
    for (auto& thread : threads) thread.join();
    for (int unit = 1; unit <= units; ++unit) {
        SCOPED_TRACE(unit);
        ASSERT_TRUE(recorder.wait_end(static_cast<neomcp::UnitId>(unit)));
        const auto written = recorder.unit(static_cast<neomcp::UnitId>(unit));
        ASSERT_EQ(written.outputs.size(), 3u);
        for (std::size_t i = 0; i < 2; ++i) {
            EXPECT_EQ(written.outputs[i].kind, neomcp::OutputKind::notification);
            EXPECT_FALSE(written.outputs[i].last);
            EXPECT_EQ(message_of(written.outputs[i]).at("params").at("progressToken"), unit);
            EXPECT_EQ(message_of(written.outputs[i]).at("params").at("progress"), static_cast<double>(i + 1));
        }
        EXPECT_TRUE(written.outputs[2].last);
        EXPECT_EQ(message_of(written.outputs[2]).at("id"), unit);
    }
    EXPECT_EQ(recorder.outputs_after_an_end(), 0);
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    EXPECT_TRUE(recorder.connection.empty());
}

TEST(Close, EndsAUnitWhoseAnswerWasStillQueued) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const auto modern = builtin(modern_revision);
    for (const neomcp::UnitId unit : {1, 2, 3}) {
        ASSERT_TRUE(session->receive(unit, line_of(gated_call(*modern, static_cast<int>(unit)))));
    }
    // The host is called while the input is read, so the calls park in the order of their units.
    std::map<int, FakeHost::Parked> parked;
    for (int unit = 1; unit <= 3; ++unit) {
        auto next = host.wait_parked(patience);
        ASSERT_TRUE(next);
        parked.emplace(unit, *next);
    }
    recorder.hold_on(1);
    std::thread first([call = parked.at(1)] { call.done(text_result("one")); });
    ASSERT_TRUE(recorder.wait_held());
    // Unit 2's answer waits in the queue behind the one being written...
    parked.at(2).done(text_result("two"));
    // ...when the session closes; unit 3 is still in flight. Its stop callback tells when close() has emptied the
    // queue.
    Gate stopped;
    std::stop_callback on_stop(parked.at(3).control.stop_token(), [&stopped] { stopped.open(); });
    std::thread closer([&session] { session->close(); });
    stopped.wait();
    recorder.release();
    closer.join();
    first.join();
    const std::size_t at_close = recorder.written();
    parked.at(3).done(text_result("three"));
    EXPECT_EQ(recorder.written(), at_close) << "nothing is written after close() returned";
    EXPECT_EQ(recorder.unit(1).ends, 1);
    EXPECT_EQ(recorder.unit(1).outputs.back().kind, neomcp::OutputKind::response);
    for (const neomcp::UnitId unit : {2, 3}) {
        SCOPED_TRACE(unit);
        const auto written = recorder.unit(unit);
        ASSERT_EQ(written.outputs.size(), 1u);
        EXPECT_EQ(written.outputs[0].kind, neomcp::OutputKind::end);
        EXPECT_TRUE(written.outputs[0].last);
    }
    EXPECT_EQ(recorder.outputs_after_an_end(), 0);
}

TEST(Close, RacingAnswersEveryUnitEndsExactlyOnce) {
    const auto modern = builtin(modern_revision);
    for (int round = 0; round < 200; ++round) {
        SCOPED_TRACE(round);
        FakeHost host;
        Recorder recorder;
        auto session = tagged_session(host, recorder);
        constexpr int units = 4;
        for (int unit = 1; unit <= units; ++unit) {
            ASSERT_TRUE(session->receive(static_cast<neomcp::UnitId>(unit), line_of(gated_call(*modern, unit, unit))));
        }
        std::vector<FakeHost::Parked> parked;
        for (int i = 0; i < units; ++i) {
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
                call.done(text_result("raced"));
            });
        }
        gate.open();
        session->close();
        const std::size_t at_close = recorder.written();
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(recorder.written(), at_close);
        for (int unit = 1; unit <= units; ++unit) {
            ASSERT_EQ(recorder.unit(static_cast<neomcp::UnitId>(unit)).ends, 1) << "unit " << unit;
        }
        ASSERT_EQ(recorder.outputs_after_an_end(), 0);
    }
}

TEST(Close, FromInsideTheSinkTheOpenUnitsGetNoEnd) {
    FakeHost host;
    std::unique_ptr<neomcp::Session> session;
    std::vector<neomcp::Output> outputs;
    auto made = neomcp::Session::create_tagged(test_options(), host, [&](neomcp::Output output) {
        outputs.push_back(std::move(output));
        session->close();
    });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
    const auto modern = builtin(modern_revision);
    ASSERT_TRUE(session->receive(1, line_of(gated_call(*modern, "open"))));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    session->receive(2, line_of(Peer::make_request(*modern, "answered", "tools/list")));
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].unit, 2u);
    EXPECT_TRUE(parked->control.stop_token().stop_requested());
}

// ---------------------------------------------------------------------------------------------------------------
// The transport a session serves

std::vector<std::string> revisions_of(std::span<const profile::ProfilePtr> profiles) {
    std::vector<std::string> out;
    for (const auto& entry : profiles) out.push_back(entry->revision());
    return out;
}

TEST(Transport, ProfilesThatDoNotDefineItAreNotServed) {
    FakeHost host;
    neomcp::SessionOptions options = test_options();
    options.transport = profile::Transport::streamable_http;
    Peer peer(host, options);
    const std::vector<std::string> served{"2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"};
    EXPECT_EQ(revisions_of(peer.session().profiles()), served);
    const auto unnamed =
        peer.request(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::object()}}, patience);
    ASSERT_TRUE(unnamed);
    EXPECT_EQ(unnamed->at("error").at("data").at("supported"), strings(served));
    const auto opened = peer.request(Peer::initialize_request(*builtin("2024-11-05"), 2), patience);
    ASSERT_TRUE(opened && opened->contains("result"));
    EXPECT_EQ(result_of(*opened).at("protocolVersion"), "2025-11-25") << "2024-11-05 has no Streamable HTTP";

    neomcp::SessionOptions sse = test_options();
    sse.transport = profile::Transport::http_sse;
    Peer old(host, sse);
    EXPECT_EQ(revisions_of(old.session().profiles()), std::vector<std::string>{"2024-11-05"});

    neomcp::SessionOptions nothing = test_options();
    nothing.transport = profile::Transport::streamable_http;
    nothing.profiles = profiles_of({"2024-11-05"});
    auto made = neomcp::Session::create(nothing, host, [](std::string) {});
    const auto* error = std::get_if<neomcp::CreateError>(&made);
    ASSERT_NE(error, nullptr);
    EXPECT_NE(error->message.find("streamable_http"), std::string::npos) << error->message;
}

// The header fields a client of 2026-07-28 sends with a request over Streamable HTTP
// (basic/transports/streamable-http.mdx): the revision, the method and, for tools/call, the tool's name.
using Fields = std::vector<std::pair<std::string, std::string>>;

Fields modern_headers(const json& request) {
    Fields fields{{"MCP-Protocol-Version", modern_revision}, {"Mcp-Method", request.at("method").get<std::string>()}};
    if (request.at("method") == "tools/call") {
        fields.emplace_back("Mcp-Name", request.at("params").at("name").get<std::string>());
    }
    return fields;
}

bool feed(neomcp::Session& session, neomcp::UnitId unit, const json& message, const Fields& fields) {
    std::vector<neomcp::HeaderField> headers;
    for (const auto& [name, value] : fields) headers.push_back(neomcp::HeaderField{name, value});
    return session.receive(unit, line_of(message), headers);
}

bool feed_modern(neomcp::Session& session, neomcp::UnitId unit, const json& request) {
    return feed(session, unit, request, modern_headers(request));
}

TEST(Transport, ItsEntryNamesTheCancellationMechanism) {
    FakeHost host;
    Recorder recorder;
    neomcp::SessionOptions options = test_options();
    options.transport = profile::Transport::streamable_http;
    auto session = tagged_session(host, recorder, options);
    const auto modern = builtin(modern_revision);
    const auto legacy = builtin("2025-11-25");
    ASSERT_TRUE(session->receive(1, line_of(Peer::initialize_request(*legacy, "init"))));
    ASSERT_TRUE(feed_modern(*session, 2, gated_call(*modern, "m")));
    ASSERT_TRUE(session->receive(3, line_of(gated_call(*legacy, "l"))));
    auto m = host.wait_parked(patience);
    auto l = host.wait_parked(patience);
    ASSERT_TRUE(m && l);
    EXPECT_FALSE(session->receive(4, line_of(cancelled("m"))));
    EXPECT_FALSE(session->receive(5, line_of(cancelled("l"))));
    EXPECT_FALSE(m->control.stop_token().stop_requested()) << "under 2026-07-28 a closed stream cancels, no message";
    EXPECT_TRUE(l->control.stop_token().stop_requested());
    m->done(text_result("m"));
    ASSERT_TRUE(recorder.wait_end(2));
    EXPECT_EQ(recorder.unit(2).outputs.back().kind, neomcp::OutputKind::response);
    EXPECT_EQ(recorder.unit(3).outputs.back().kind, neomcp::OutputKind::end);
}

TEST(Connection, TheProfileAHandshakeOpenedIsKnown) {
    FakeHost host;
    Recorder recorder;
    neomcp::SessionOptions options = test_options();
    options.transport = profile::Transport::streamable_http;
    auto session = tagged_session(host, recorder, options);
    EXPECT_FALSE(session->connection_profile());
    ASSERT_TRUE(feed_modern(*session, 1, Peer::make_request(*builtin(modern_revision), 1, "server/discover")));
    ASSERT_TRUE(message_of(recorder.unit(1).outputs.at(0)).contains("result"));
    EXPECT_FALSE(session->connection_profile()) << "a request that names its revision opens nothing";
    ASSERT_TRUE(session->receive(2, line_of(Peer::initialize_request(*builtin("2025-06-18"), 2))));
    const profile::ProfilePtr opened = session->connection_profile();
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->revision(), "2025-06-18");
    ASSERT_TRUE(opened->streamable_http());
    EXPECT_EQ(opened->streamable_http()->session_header, "Mcp-Session-Id");
}

// ---------------------------------------------------------------------------------------------------------------
// A transport that lost the stream of a unit

neomcp::SessionOptions http_options() {
    neomcp::SessionOptions options = test_options();
    options.transport = profile::Transport::streamable_http;
    return options;
}

TEST(Abandon, UnderCloseResponseStreamTheCallsStopAndNothingMoreIsWritten) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, http_options());
    const auto modern = builtin(modern_revision);
    ASSERT_TRUE(feed_modern(*session, 1, gated_call(*modern, "c", "t")));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    parked->control.progress(1);
    ASSERT_EQ(recorder.unit(1).outputs.size(), 1u);
    session->abandon(1);
    EXPECT_TRUE(parked->control.stop_token().stop_requested());
    EXPECT_TRUE(session->idle()) << "the stopped call is finished";
    std::thread late([call = *parked] {
        call.control.progress(2);
        call.done(text_result("late"));
    });
    late.join();
    session->abandon(1);
    session->abandon(99);
    ASSERT_TRUE(feed_modern(*session, 2, Peer::make_request(*modern, "flush", "server/discover")));
    ASSERT_TRUE(recorder.wait_end(2));
    const auto written = recorder.unit(1);
    EXPECT_EQ(written.outputs.size(), 1u);
    EXPECT_EQ(written.ends, 0) << "an abandoned unit gets no end";
}

TEST(Abandon, UnderCancelledNotificationTheCallsRunOnAndWhatTheyWriteIsDropped) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, http_options());
    const auto legacy = builtin("2025-11-25");
    ASSERT_TRUE(session->receive(1, line_of(Peer::initialize_request(*legacy, "init"))));
    ASSERT_TRUE(session->receive(2, line_of(gated_call(*legacy, "l", "t"))));
    auto running = host.wait_parked(patience);
    ASSERT_TRUE(running);
    session->abandon(2);
    EXPECT_FALSE(running->control.stop_token().stop_requested()) << "disconnection is not a cancellation here";
    EXPECT_FALSE(session->idle()) << "its call still runs";
    std::thread tool([call = *running] {
        call.control.progress(1);
        call.done(text_result("l"));
    });
    tool.join();
    EXPECT_TRUE(session->idle());
    EXPECT_TRUE(recorder.unit(2).outputs.empty());

    // Its call can still be cancelled by the notification of its own mechanism.
    ASSERT_TRUE(session->receive(3, line_of(gated_call(*legacy, "k"))));
    auto cancelled_later = host.wait_parked(patience);
    ASSERT_TRUE(cancelled_later);
    session->abandon(3);
    EXPECT_FALSE(session->receive(4, line_of(cancelled("k"))));
    EXPECT_TRUE(cancelled_later->control.stop_token().stop_requested());
    cancelled_later->done(text_result("k"));
    EXPECT_TRUE(recorder.unit(3).outputs.empty());
    EXPECT_EQ(recorder.unit(4).ends, 1);
}

TEST(Abandon, ABatchRunsOnAndItsAnswerIsDropped) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, http_options());
    const auto revision = builtin(batch_revision);
    ASSERT_TRUE(session->receive(1, line_of(Peer::initialize_request(*revision, "init"))));
    ASSERT_TRUE(session->receive(
        2, line_of(json::array({gated_call(*revision, "b1"), Peer::make_request(*revision, "b2", "ping")}))));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    session->abandon(2);
    EXPECT_FALSE(parked->control.stop_token().stop_requested());
    parked->done(text_result("b1"));
    EXPECT_TRUE(session->idle()) << "the batch was assembled and dropped";
    EXPECT_TRUE(recorder.unit(2).outputs.empty());
}

TEST(Abandon, WhileTheRequestIsStillBeingRead) {
    FakeHost host;
    Recorder recorder;
    neomcp::Session* target = nullptr;
    std::optional<neomcp::UnitId> abandon_on_check;
    neomcp::SessionOptions options = http_options();
    // The argument check runs while receive() reads the request, before the call starts.
    options.check_arguments = [&](const neomcp::ToolDefinition&, const json&) -> std::optional<std::string> {
        if (abandon_on_check) target->abandon(*abandon_on_check);
        return std::nullopt;
    };
    auto session = tagged_session(host, recorder, options);
    target = session.get();
    const auto modern = builtin(modern_revision);
    const auto legacy = builtin("2025-11-25");

    abandon_on_check = 1;
    EXPECT_TRUE(feed_modern(*session, 1, gated_call(*modern, "m")));
    EXPECT_FALSE(host.wait_parked(now)) << "a call whose stream is gone and whose closing cancels it does not start";
    EXPECT_TRUE(session->idle());

    abandon_on_check.reset();
    ASSERT_TRUE(session->receive(2, line_of(Peer::initialize_request(*legacy, "init"))));
    abandon_on_check = 3;
    EXPECT_TRUE(session->receive(3, line_of(gated_call(*legacy, "l"))));
    auto running = host.wait_parked(patience);
    ASSERT_TRUE(running) << "a call whose closing stream does not cancel it starts";
    running->done(text_result("l"));
    abandon_on_check.reset();
    EXPECT_TRUE(recorder.unit(1).outputs.empty());
    EXPECT_TRUE(recorder.unit(3).outputs.empty());
}

TEST(Abandon, FromInsideTheSinkForTheUnitBeingWritten) {
    FakeHost host;
    std::unique_ptr<neomcp::Session> session;
    std::vector<neomcp::Output> outputs;
    auto made = neomcp::Session::create_tagged(http_options(), host, [&](neomcp::Output output) {
        const bool first_of_unit = output.unit == 1u && outputs.empty();
        outputs.push_back(std::move(output));
        if (first_of_unit) session->abandon(1);  // would wait for itself if it did not know it is the writer
    });
    ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
    session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
    ASSERT_TRUE(feed_modern(*session, 1, gated_call(*builtin(modern_revision), "c", "t")));
    auto parked = host.wait_parked(patience);
    ASSERT_TRUE(parked);
    parked->control.progress(1);
    EXPECT_TRUE(parked->control.stop_token().stop_requested());
    parked->control.progress(2);
    parked->done(text_result("c"));
    EXPECT_EQ(outputs.size(), 1u);
}

// The guarantee is "after abandon() returns": a write of the unit in progress on another thread when abandon() is
// called ends before abandon() returns. The sink logs the end of that write and the abandoning thread logs its return;
// the release of the held write is sent only after the abandoning thread is about to call abandon().
TEST(Abandon, ReturnsOnlyAfterAWriteOfTheUnitOnAnotherThreadEnded) {
    const auto modern = builtin(modern_revision);
    for (int round = 0; round < 50; ++round) {
        SCOPED_TRACE(round);
        FakeHost host;
        Recorder recorder;
        std::mutex events_mutex;
        std::vector<std::string> events;
        const auto note = [&](std::string event) {
            const std::lock_guard<std::mutex> lock(events_mutex);
            events.push_back(std::move(event));
        };
        auto made = neomcp::Session::create_tagged(http_options(), host, [&](neomcp::Output output) {
            const auto unit = output.unit;
            recorder.write(std::move(output));
            if (unit == 1u) note("write of unit 1 ended");
        });
        ASSERT_TRUE(std::holds_alternative<std::unique_ptr<neomcp::Session>>(made));
        auto session = std::move(std::get<std::unique_ptr<neomcp::Session>>(made));
        ASSERT_TRUE(feed_modern(*session, 1, gated_call(*modern, "c", "t")));
        auto parked = host.wait_parked(patience);
        ASSERT_TRUE(parked);
        recorder.hold_on(1);
        std::thread writer([call = *parked] { call.control.progress(1); });
        ASSERT_TRUE(recorder.wait_held());
        Gate calling;
        std::thread abandoner([&] {
            calling.open();
            session->abandon(1);
            note("abandon returned");
        });
        calling.wait();
        recorder.release();
        writer.join();
        abandoner.join();
        {
            const std::lock_guard<std::mutex> lock(events_mutex);
            ASSERT_EQ(events, (std::vector<std::string>{"write of unit 1 ended", "abandon returned"}));
        }
        parked->control.progress(2);
        parked->done(text_result("c"));
        ASSERT_TRUE(feed_modern(*session, 2, Peer::make_request(*modern, "flush", "server/discover")));
        ASSERT_EQ(recorder.unit(1).outputs.size(), 1u);
    }
}

TEST(Abandon, NothingOfTheUnitIsWrittenAfterAbandonReturns) {
    const auto modern = builtin(modern_revision);
    for (int round = 0; round < 200; ++round) {
        SCOPED_TRACE(round);
        FakeHost host;
        Recorder recorder;
        auto session = tagged_session(host, recorder, http_options());
        ASSERT_TRUE(feed_modern(*session, 1, gated_call(*modern, "c", "t")));
        auto parked = host.wait_parked(patience);
        ASSERT_TRUE(parked);
        Gate gate;
        std::thread tool([&gate, call = *parked] {
            gate.wait();
            for (int value = 1; value <= 50; ++value) call.control.progress(value);
            call.done(text_result("c"));
        });
        gate.open();
        session->abandon(1);
        const std::size_t at_return = recorder.unit(1).outputs.size();
        tool.join();
        const auto written = recorder.unit(1);
        // The unit either ended before abandon() (which then ignored it) or was frozen when abandon() returned.
        if (written.ends == 0) {
            ASSERT_EQ(written.outputs.size(), at_return);
        }
        ASSERT_EQ(recorder.outputs_after_an_end(), 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// What the headers claim (Streamable HTTP)

// The one output of a unit that was answered at once.
neomcp::Output only_output(Recorder& recorder, neomcp::UnitId unit) {
    const auto written = recorder.unit(unit);
    if (written.outputs.size() != 1) throw std::runtime_error("unit " + std::to_string(unit) + " has " +
                                                             std::to_string(written.outputs.size()) + " outputs");
    return written.outputs[0];
}

// The status the governing profile prescribes for the refusal; 0 when it prescribes none.
int prescribed_status(const neomcp::Output& output) {
    if (!output.profile || !output.profile->streamable_http()) return 0;
    return output.profile->streamable_http()->status_for(neomcp::to_string(output.refusal)).value_or(0);
}

json with_version(json request, const json& version) {
    if (version.is_null()) {
        request["params"]["_meta"].erase(version_key);
    } else {
        request["params"]["_meta"][version_key] = version;
    }
    return request;
}

Fields without(Fields fields, const std::string& name) {
    fields.erase(std::remove_if(fields.begin(), fields.end(), [&](const auto& field) { return field.first == name; }),
                 fields.end());
    return fields;
}

Fields replaced(Fields fields, const std::string& name, const std::string& value) {
    fields = without(std::move(fields), name);
    fields.emplace_back(name, value);
    return fields;
}

// The rows of the table of the version header H against the revision B the body names: basic/transports/
// streamable-http.mdx and basic/versioning.mdx of 2026-07-28, and the version header of the legacy revisions.
TEST(Headers, TheVersionHeaderAgainstTheBodysRevision) {
    using neomcp::RefusalReason;
    const auto modern = builtin(modern_revision);
    const json list = Peer::make_request(*modern, "r", "tools/list");
    struct Row {
        std::string name;
        json request;
        Fields fields;
        int code;  // 0: a result
        RefusalReason reason;
        int status;  // what 2026-07-28 prescribes for it; 0: none
    };
    const std::vector<Row> rows{
        {"H and B agree", list, modern_headers(list), 0, RefusalReason::none, 0},
        {"H and B differ, a mismatch before the version", with_version(list, "2099-01-01"), modern_headers(list),
         -32020, RefusalReason::header_mismatch, 400},
        {"H absent, B present", list, without(modern_headers(list), "MCP-Protocol-Version"), -32020,
         RefusalReason::header_mismatch, 400},
        {"H and B name a revision not served", with_version(list, "2099-01-01"),
         replaced(modern_headers(list), "MCP-Protocol-Version", "2099-01-01"), -32022,
         RefusalReason::unsupported_version, 400},
        {"H names the modern revision, B absent", with_version(list, nullptr), modern_headers(list), -32602,
         RefusalReason::invalid_request_meta, 400},
        {"H names the modern revision, no _meta at all",
         json{{"jsonrpc", "2.0"}, {"id", "r"}, {"method", "tools/list"}, {"params", json::object()}},
         modern_headers(list), -32602, RefusalReason::invalid_request_meta, 400},
        {"H names a legacy revision, B absent, no connection",
         json{{"jsonrpc", "2.0"}, {"id", "r"}, {"method", "tools/list"}}, {{"MCP-Protocol-Version", "2025-11-25"}},
         -32602, RefusalReason::missing_connection, 0},
        {"H names no revision served, B absent", json{{"jsonrpc", "2.0"}, {"id", "r"}, {"method", "tools/list"}},
         {{"MCP-Protocol-Version", "1999-01-01"}}, -32022, RefusalReason::unsupported_version_header, 0},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        FakeHost host;
        Recorder recorder;
        auto session = tagged_session(host, recorder, http_options());
        EXPECT_TRUE(feed(*session, 1, row.request, row.fields));
        const neomcp::Output output = only_output(recorder, 1);
        const json answer = message_of(output);
        EXPECT_EQ(answer.at("id"), "r") << "an error echoes the request's id";
        if (row.code == 0) {
            EXPECT_TRUE(answer.contains("result")) << answer.dump();
            continue;
        }
        EXPECT_EQ(error_code_of(answer), row.code) << answer.dump();
        EXPECT_EQ(output.refusal, row.reason) << neomcp::to_string(output.refusal);
        EXPECT_EQ(prescribed_status(output), row.status);
        if (row.reason == RefusalReason::unsupported_version) {
            EXPECT_EQ(answer.at("error").at("data").at("supported"), strings({modern_revision}));
            EXPECT_EQ(answer.at("error").at("data").at("requested"), "2099-01-01");
        }
        if (row.reason == RefusalReason::unsupported_version_header) {
            EXPECT_EQ(answer.at("error").at("data").at("requested"), "1999-01-01");
        }
        if (row.reason == RefusalReason::header_mismatch) {
            EXPECT_NE(error_message_of(answer).find("MCP-Protocol-Version"), std::string::npos) << answer.dump();
        }
    }
}

TEST(Headers, AHandshakeProceedsWhateverTheVersionHeaderNames) {
    for (const std::string version : {"1999-01-01", "2025-11-25", "2025-06-18"}) {
        SCOPED_TRACE(version);
        FakeHost host;
        Recorder recorder;
        auto session = tagged_session(host, recorder, http_options());
        EXPECT_TRUE(feed(*session, 1, Peer::initialize_request(*builtin("2025-11-25"), 1),
                         {{"MCP-Protocol-Version", version}, {"Mcp-Method", "initialize"}}));
        const json answer = message_of(only_output(recorder, 1));
        ASSERT_TRUE(answer.contains("result")) << answer.dump();
        EXPECT_EQ(answer.at("result").at("protocolVersion"), "2025-11-25");
        ASSERT_TRUE(session->connection_profile());
    }
    // A header that names the modern revision is a modern request, whose body must say so.
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, http_options());
    EXPECT_TRUE(feed(*session, 1, Peer::initialize_request(*builtin("2025-11-25"), 1),
                     {{"MCP-Protocol-Version", modern_revision}}));
    EXPECT_EQ(only_output(recorder, 1).refusal, neomcp::RefusalReason::invalid_request_meta);
    EXPECT_FALSE(session->connection_profile());
}

TEST(Headers, OnAnOpenedConnectionAnyServedRevisionIsAccepted) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, http_options());
    const auto legacy = builtin("2025-11-25");
    ASSERT_TRUE(session->receive(1, line_of(Peer::initialize_request(*legacy, "init"))));
    const json list = Peer::make_request(*legacy, "l", "tools/list");
    for (const std::string version : {"2025-11-25", "2025-06-18", "2025-03-26"}) {
        SCOPED_TRACE(version);
        const neomcp::UnitId unit = recorder.written() + 10;
        EXPECT_TRUE(feed(*session, unit, list, {{"MCP-Protocol-Version", version}}));
        EXPECT_TRUE(message_of(only_output(recorder, unit)).contains("result"));
    }
    EXPECT_TRUE(feed(*session, 2, list, {{"MCP-Protocol-Version", "1999-01-01"}}));
    const neomcp::Output refused = only_output(recorder, 2);
    EXPECT_EQ(refused.refusal, neomcp::RefusalReason::unsupported_version_header);
    EXPECT_EQ(error_code_of(message_of(refused)), -32022) << "the code of the newest revision that defines it";

    // A notification alone is refused the same way, with an error that has no id (the connection speaks 2025-11-25,
    // whose error responses may leave the member out); one the header allows is taken.
    const json initialized = json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    EXPECT_TRUE(feed(*session, 3, initialized, {{"MCP-Protocol-Version", "1999-01-01"}}));
    const neomcp::Output notification = only_output(recorder, 3);
    EXPECT_EQ(notification.refusal, neomcp::RefusalReason::unsupported_version_header);
    EXPECT_FALSE(message_of(notification).contains("id"));
    EXPECT_FALSE(feed(*session, 4, initialized, {{"MCP-Protocol-Version", "2025-11-25"}}));
    EXPECT_EQ(only_output(recorder, 4).kind, neomcp::OutputKind::end);
}

TEST(Headers, ABatchIsRefusedForItsVersionHeaderAsAWhole) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, http_options());
    const auto revision = builtin(batch_revision);
    ASSERT_TRUE(session->receive(1, line_of(Peer::initialize_request(*revision, "init"))));
    const json batch = json::array({Peer::make_request(*revision, "b1", "ping"), Peer::make_request(*revision, "b2", "ping")});
    EXPECT_TRUE(feed(*session, 2, batch, {{"MCP-Protocol-Version", "1999-01-01"}}));
    const neomcp::Output refused = only_output(recorder, 2);
    EXPECT_EQ(refused.refusal, neomcp::RefusalReason::unsupported_version_header);
    EXPECT_TRUE(message_of(refused).at("id").is_null());
    EXPECT_TRUE(feed(*session, 3, batch, {{"MCP-Protocol-Version", batch_revision}}));
    EXPECT_TRUE(message_of(only_output(recorder, 3)).is_array());
}

// The request headers of 2026-07-28 (basic/transports/streamable-http.mdx): Mcp-Method on every request, Mcp-Name on
// tools/call with params.name; names without regard to case, values exactly, a value in the encoded form decoded.
TEST(Headers, TheRequestHeadersOfTheRevision) {
    const auto modern = builtin(modern_revision);
    const json call = Peer::make_request(*modern, "c", "tools/call", {{"name", "echo"}, {"arguments", {{"x", 1}}}});
    const Fields standard = modern_headers(call);
    struct Case {
        std::string name;
        json request;
        Fields fields;
        bool accepted;
        std::string mentions;  // a part of the error message
    };
    json number_name = call;
    number_name["params"]["name"] = 5;
    const json list = Peer::make_request(*modern, "c", "tools/list");
    const std::vector<Case> cases{
        {"the standard headers", call, standard, true, ""},
        {"names in other cases",
         call,
         {{"mcp-protocol-version", modern_revision}, {"MCP-METHOD", "tools/call"}, {"mcp-name", "echo"}},
         true,
         ""},
        {"Mcp-Name in the encoded form", call, replaced(standard, "Mcp-Name", "=?base64?ZWNobw==?="), true, ""},
        {"Mcp-Name on a request that does not need it", list,
         replaced(modern_headers(list), "Mcp-Name", "anything"), true, ""},
        {"Mcp-Method differs", call, replaced(standard, "Mcp-Method", "prompts/list"), false, "Mcp-Method"},
        {"Mcp-Method in another case", call, replaced(standard, "Mcp-Method", "TOOLS/CALL"), false, "Mcp-Method"},
        {"Mcp-Method missing", call, without(standard, "Mcp-Method"), false, "Mcp-Method"},
        {"Mcp-Method repeated", call, [&] {
             Fields fields = standard;
             fields.emplace_back("mcp-method", "tools/call");
             return fields;
         }(), false, "more than once"},
        {"MCP-Protocol-Version repeated", call, [&] {
             Fields fields = standard;
             fields.emplace_back("MCP-Protocol-Version", modern_revision);
             return fields;
         }(), false, "more than once"},
        {"Mcp-Name missing", call, without(standard, "Mcp-Name"), false, "Mcp-Name"},
        {"Mcp-Name differs", call, replaced(standard, "Mcp-Name", "wrong_tool_name"), false, "Mcp-Name"},
        {"Mcp-Name in the encoded form without its padding", call,
         replaced(standard, "Mcp-Name", "=?base64?ZWNobw?="), false, "encoded form"},
        {"Mcp-Name in the encoded form with characters outside the alphabet", call,
         replaced(standard, "Mcp-Name", "=?base64?SGVs!!!bG8=?="), false, "encoded form"},
        {"a value that only looks encoded is taken as it is", call,
         replaced(standard, "Mcp-Name", "=?base64?="), false, "does not match"},
        {"Mcp-Name whose decoded value differs", call, replaced(standard, "Mcp-Name", "=?base64?" "?="), false,
         "does not match"},
        {"Mcp-Name plain but holding the encoded digits", call, replaced(standard, "Mcp-Name", "ZWNobw=="), false,
         "does not match"},
        {"a body whose params.name is not a string", number_name, standard, false, "string"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        FakeHost host;
        Recorder recorder;
        auto session = tagged_session(host, recorder, http_options());
        EXPECT_TRUE(feed(*session, 1, item.request, item.fields));
        const neomcp::Output output = only_output(recorder, 1);
        const json answer = message_of(output);
        if (item.accepted) {
            EXPECT_TRUE(answer.contains("result")) << answer.dump();
            EXPECT_EQ(output.refusal, neomcp::RefusalReason::none);
            continue;
        }
        EXPECT_EQ(error_code_of(answer), -32020) << answer.dump();
        EXPECT_EQ(output.refusal, neomcp::RefusalReason::header_mismatch);
        EXPECT_EQ(prescribed_status(output), 400);
        EXPECT_NE(error_message_of(answer).find(item.mentions), std::string::npos) << answer.dump();
    }
}

// A message about a header names the header and the member of the body and repeats no value of the request, however
// long the values are.
TEST(Headers, AMismatchMessageRepeatsNoValueOfTheRequest) {
    const auto modern = builtin(modern_revision);
    const std::string body_name(4000, 'b');
    const std::string header_name(4000, 'h');
    const json call = Peer::make_request(*modern, "c", "tools/call", {{"name", body_name}, {"arguments", json::object()}});
    struct Case {
        std::string name;
        Fields fields;
        std::string names;  // what the message must name
    };
    const Fields standard = modern_headers(call);
    const std::vector<Case> cases{
        {"Mcp-Name differs", replaced(standard, "Mcp-Name", header_name), "Mcp-Name"},
        {"Mcp-Method differs", replaced(standard, "Mcp-Method", header_name), "Mcp-Method"},
        {"the version differs", replaced(standard, "MCP-Protocol-Version", "2025-11-25"), "MCP-Protocol-Version"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        FakeHost host;
        Recorder recorder;
        auto session = tagged_session(host, recorder, http_options());
        EXPECT_TRUE(feed(*session, 1, call, item.fields));
        const std::string message = error_message_of(message_of(only_output(recorder, 1)));
        EXPECT_NE(message.find(item.names), std::string::npos) << message;
        EXPECT_LT(message.size(), 200U);
        EXPECT_EQ(message.find("bbbb"), std::string::npos) << "the body's value is not repeated";
        EXPECT_EQ(message.find("hhhh"), std::string::npos) << "the header's value is not repeated";
        EXPECT_EQ(message.find("2025-11-25"), std::string::npos);
        EXPECT_EQ(message.find(modern_revision), std::string::npos);
    }
}

TEST(Headers, AMismatchTakesItsCodeFromTheRevisionThatDefinesIt) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder, http_options());
    const json list = Peer::make_request(*builtin(modern_revision), "r", "tools/list");
    // The header names a served legacy revision, which defines no such error; the body's revision does.
    EXPECT_TRUE(feed(*session, 1, list, replaced(modern_headers(list), "MCP-Protocol-Version", "2025-11-25")));
    const neomcp::Output output = only_output(recorder, 1);
    EXPECT_EQ(error_code_of(message_of(output)), -32020);
    ASSERT_TRUE(output.profile);
    EXPECT_EQ(output.profile->revision(), modern_revision);
}

TEST(Headers, WithoutARevisionThatDefinesTheErrors) {
    FakeHost host;
    Recorder recorder;
    neomcp::SessionOptions options = http_options();
    options.profiles = profiles_of({"2025-03-26", "2025-06-18", "2025-11-25"});
    auto session = tagged_session(host, recorder, options);
    EXPECT_TRUE(feed(*session, 1, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                     {{"MCP-Protocol-Version", "1999-01-01"}}));
    const neomcp::Output refused = only_output(recorder, 1);
    EXPECT_EQ(refused.refusal, neomcp::RefusalReason::unsupported_version_header);
    EXPECT_EQ(error_code_of(message_of(refused)), -32600) << "Invalid Request, a code of JSON-RPC itself";
    const json modern = Peer::make_request(*builtin(modern_revision), 2, "tools/list");
    EXPECT_TRUE(feed(*session, 2, modern, modern_headers(modern)));
    const neomcp::Output unserved = only_output(recorder, 2);
    EXPECT_EQ(unserved.refusal, neomcp::RefusalReason::unsupported_version)
        << "the header and the body agree on a revision that is not served";
}

TEST(Headers, OnStdioHeadersChangeNothing) {
    FakeHost host;
    Recorder recorder;
    auto session = tagged_session(host, recorder);
    const json list = Peer::make_request(*builtin(modern_revision), "r", "tools/list");
    EXPECT_TRUE(feed(*session, 1, list, {{"MCP-Protocol-Version", "1999-01-01"}, {"Mcp-Method", "prompts/list"}}));
    EXPECT_TRUE(message_of(only_output(recorder, 1)).contains("result"));
    EXPECT_TRUE(feed(*session, 2, list, {}));
    EXPECT_TRUE(message_of(only_output(recorder, 2)).contains("result"));
}

}  // namespace
