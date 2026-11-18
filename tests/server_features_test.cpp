// Tests of resources, resource templates, prompts and completion through the in-memory peer.
//
// Expected per-revision values are written here from the schemas of each revision, not read back from the profiles;
// the one thing read from a profile is the list of members a named type has and requires, to show that an answer
// has every member its type requires and none its type lacks. Every wait is on an event a test controls; the bound
// on a wait only limits how long a broken build takes to fail.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <gtest/gtest.h>

#include "neomcp/base64.h"
#include "neomcp/server.h"
#include "neomcp/testing/peer.h"

namespace ft {

struct FileParams {
    std::string path;
};

struct ItemParams {
    std::int32_t id;
    bool verbose;
};

struct SearchParams {
    std::string q;
    std::optional<std::uint16_t> limit;
};

struct KindParams {
    std::string kind;
    std::string name;
};

struct ReviewArgs {
    std::string title;
    std::optional<std::string> focus;
};

struct TwoArgs {
    std::string arg1;
    std::string arg2;
};

}  // namespace ft

NEOMCP_DESCRIBE(ft::FileParams, path, "the path")
NEOMCP_DESCRIBE(ft::ItemParams, id, "", verbose, "")
NEOMCP_DESCRIBE(ft::SearchParams, q, "", limit, "")
NEOMCP_DESCRIBE(ft::KindParams, kind, "", name, "")
NEOMCP_DESCRIBE(ft::ReviewArgs, title, "what to review", focus, "what to look at")
NEOMCP_DESCRIBE(ft::TwoArgs, arg1, "", arg2, "")

namespace {

using namespace std::chrono_literals;
using neomcp::json;
using neomcp::testing::Peer;
namespace profile = neomcp::profile;

constexpr std::chrono::milliseconds patience = 30s;

const std::vector<std::string> all_revisions{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"};
constexpr const char* modern_revision = "2026-07-28";
constexpr const char* resources_capability = "resources";

bool is_legacy(const std::string& revision) { return revision != modern_revision; }

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

std::string name_of(const ::testing::TestParamInfo<std::string>& info) {
    std::string name = info.param;
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

const json& result_of(const json& answer) { return answer.at("result"); }

int error_code_of(const json& answer) {
    if (!answer.contains("error")) return 0;
    return answer.at("error").at("code").get<int>();
}

json cancelled(const json& id) {
    return json{{"jsonrpc", "2.0"},
                {"method", "notifications/cancelled"},
                {"params", {{"requestId", id}, {"reason", "test"}}}};
}

// A flag the test raises once. A wait ends when it is raised, when a stop token is stopped, or after a bound that
// only a broken build reaches.
class Event {
public:
    void set() {
        const std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, bound, [&] { return set_; });
    }
    bool wait(std::stop_token token) {
        std::stop_callback wake(token, [this] {
            const std::lock_guard<std::mutex> lock(mutex_);
            changed_.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return set_ || token.stop_requested(); });
        return set_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

// Lets `parties` threads through together: each arrives, and all wait until the last has.
class Meeting {
public:
    explicit Meeting(int parties) : parties_(parties) {}
    // False when the others did not all arrive within the bound.
    bool arrive(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_;
        changed_.notify_all();
        return changed_.wait_for(lock, bound, [&] { return arrived_ >= parties_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int parties_;
    int arrived_ = 0;
};

// A coroutine that ends when stop is requested on the token.
asio::awaitable<void> until_stopped(std::stop_token token) {
    const auto executor = co_await asio::this_coro::executor;
    auto timer = std::make_shared<asio::steady_timer>(executor, asio::steady_timer::time_point::max());
    std::stop_callback wake(token, [timer] { asio::post(timer->get_executor(), [timer] { timer->cancel(); }); });
    auto outcome = co_await timer->async_wait(asio::as_tuple(asio::use_awaitable));
    static_cast<void>(outcome);
}

// One client speaking one revision to a server: a handshake for a legacy revision, nothing for the modern one.
struct Client {
    Client(neomcp::Server& server, const std::string& revision_name)
        : peer(server), revision(builtin(revision_name)) {
        if (is_legacy(revision_name)) {
            opened = peer.initialize(*revision, patience);
            EXPECT_TRUE(opened && opened->contains("result")) << (opened ? opened->dump() : "no answer");
        }
    }

    json message(const neomcp::RequestId& id, std::string method, json params = json::object()) const {
        return Peer::make_request(*revision, id, std::move(method), std::move(params));
    }

    json ask(std::string method, json params = json::object()) {
        const auto answer = peer.request(message(next_id++, std::move(method), std::move(params)), patience);
        if (!answer) throw std::runtime_error("no answer within the patience");
        return *answer;
    }

    json read(const std::string& uri, json meta = nullptr) {
        json params = json{{"uri", uri}};
        if (!meta.is_null()) params["_meta"] = std::move(meta);
        return ask("resources/read", std::move(params));
    }

    // Sends a read without waiting for its answer.
    void start_read(const neomcp::RequestId& id, const std::string& uri, json meta = nullptr) {
        json params = json{{"uri", uri}};
        if (!meta.is_null()) params["_meta"] = std::move(meta);
        peer.notify(message(id, "resources/read", std::move(params)));
    }

    // The capabilities the server declared: from the handshake, else from server/discover.
    json capabilities() {
        if (opened) return result_of(*opened).at("capabilities");
        return result_of(ask("server/discover")).at("capabilities");
    }

    Peer peer;
    profile::ProfilePtr revision;
    std::optional<json> opened;
    int next_id = 1;
};

neomcp::ServerOptions named(std::string name) {
    neomcp::ServerOptions options;
    options.name = std::move(name);
    options.version = "1.0.0";
    return options;
}

// The members of `value` are members its type has, and the type's required members are all there.
void expect_conforms(const json& value, const std::string& type_name, const profile::Profile& profile) {
    const profile::Type* type = profile.type(type_name);
    ASSERT_NE(type, nullptr) << type_name << " in " << profile.revision();
    for (const auto& name : keys(value)) {
        EXPECT_TRUE(type->has_property(name)) << type_name << " lacks '" << name << "' in " << profile.revision();
    }
    for (const auto& name : type->required) {
        EXPECT_TRUE(value.contains(name)) << type_name << " requires '" << name << "' in " << profile.revision();
    }
}

class PerRevision : public ::testing::TestWithParam<std::string> {};
INSTANTIATE_TEST_SUITE_P(Revisions, PerRevision, ::testing::ValuesIn(all_revisions), name_of);

// ---------------------------------------------------------------------------------------------------------------
// Resources: listing

// Resource per revision (schema.json of each revision): title and _meta from 2025-06-18, icons from 2025-11-25.
const std::map<std::string, std::set<std::string>> resource_members{
    {"2024-11-05", {"annotations", "description", "mimeType", "name", "size", "uri"}},
    {"2025-03-26", {"annotations", "description", "mimeType", "name", "size", "uri"}},
    {"2025-06-18", {"_meta", "annotations", "description", "mimeType", "name", "size", "title", "uri"}},
    {"2025-11-25", {"_meta", "annotations", "description", "icons", "mimeType", "name", "size", "title", "uri"}},
    {"2026-07-28", {"_meta", "annotations", "description", "icons", "mimeType", "name", "size", "title", "uri"}},
};
// ResourceTemplate has no size.
const std::map<std::string, std::set<std::string>> template_members{
    {"2024-11-05", {"annotations", "description", "mimeType", "name", "uriTemplate"}},
    {"2025-03-26", {"annotations", "description", "mimeType", "name", "uriTemplate"}},
    {"2025-06-18", {"_meta", "annotations", "description", "mimeType", "name", "title", "uriTemplate"}},
    {"2025-11-25", {"_meta", "annotations", "description", "icons", "mimeType", "name", "title", "uriTemplate"}},
    {"2026-07-28", {"_meta", "annotations", "description", "icons", "mimeType", "name", "title", "uriTemplate"}},
};

neomcp::Icon an_icon() {
    neomcp::Icon icon;
    icon.src = "https://example.com/icon.png";
    icon.mime_type = "image/png";
    return icon;
}

neomcp::ContentAnnotations an_annotation() {
    neomcp::ContentAnnotations annotations;
    annotations.audience = {neomcp::Role::user};
    annotations.priority = 0.5;
    return annotations;
}

TEST_P(PerRevision, ListedResourcesHaveExactlyTheRevisionsMembers) {
    const std::string revision = GetParam();
    ASSERT_EQ(resource_members.size(), profile::builtin_revisions().size()) << "a new revision needs its values here";
    neomcp::Server server(named("res"));
    server.resource("config://app", "App configuration", [] { return std::string("on"); })
        .title("Configuration")
        .description("The settings")
        .mime_type("text/plain")
        .size(2)
        .annotations(an_annotation())
        .icons({an_icon()})
        .meta(json{{"k", "v"}});
    server.resource_template<ft::FileParams>("file:///{path}", "A file",
                                              [](const ft::FileParams& params) { return params.path; })
        .title("File")
        .description("Any file")
        .mime_type("text/plain")
        .annotations(an_annotation())
        .icons({an_icon()})
        .meta(json{{"k", "v"}});

    Client client(server, revision);
    const json list = result_of(client.ask("resources/list"));
    ASSERT_EQ(list.at("resources").size(), 1u);
    const json& resource = list.at("resources").at(0);
    EXPECT_EQ(keys(resource), resource_members.at(revision)) << resource.dump();
    EXPECT_EQ(resource.at("uri"), "config://app");
    EXPECT_EQ(resource.at("name"), "App configuration");
    EXPECT_EQ(resource.at("mimeType"), "text/plain");
    EXPECT_EQ(resource.at("size"), 2);
    expect_conforms(list, "ListResourcesResult", *client.revision);
    expect_conforms(resource, "Resource", *client.revision);

    const json templates = result_of(client.ask("resources/templates/list"));
    ASSERT_EQ(templates.at("resourceTemplates").size(), 1u);
    const json& entry = templates.at("resourceTemplates").at(0);
    EXPECT_EQ(keys(entry), template_members.at(revision)) << entry.dump();
    EXPECT_EQ(entry.at("uriTemplate"), "file:///{path}");
    expect_conforms(templates, "ListResourceTemplatesResult", *client.revision);
    expect_conforms(entry, "ResourceTemplate", *client.revision);
}

TEST_P(PerRevision, AResourceWithNothingSetListsOnlyItsRequiredMembers) {
    neomcp::Server server(named("res"));
    server.resource("config://app", "App configuration", [] { return std::string("on"); });
    Client client(server, GetParam());
    const json list = result_of(client.ask("resources/list"));
    EXPECT_EQ(keys(list.at("resources").at(0)), (std::set<std::string>{"name", "uri"}));
}

// ---------------------------------------------------------------------------------------------------------------
// Resources: reading and what handlers return

struct Notes {
    std::string note;
    int count;
};

}  // namespace

NEOMCP_DESCRIBE(Notes, note, "", count, "")

namespace {

TEST_P(PerRevision, ReadReturnsTheContentsOfEachResultType) {
    const std::string revision = GetParam();
    neomcp::Server server(named("res"));
    server.resource("test://text", "Text", [] { return std::string("hello"); }).mime_type("text/plain");
    server.resource("test://bytes", "Bytes", [] { return neomcp::Bytes{std::byte{0}, std::byte{1}, std::byte{0xFF}}; })
        .mime_type("image/png");
    server.resource("test://json", "Json", [] { return json{{"a", 1}}; });
    server.resource("test://struct", "Struct", [] { return Notes{"n", 3}; });
    server.resource("test://owned", "Owned", [] {
        return Notes{"n", 3};
    }).mime_type("text/x-notes");
    server.resource("test://built", "Built", [] {
        neomcp::TextResourceContents contents;
        contents.uri = "test://built/part";
        contents.text = "part";
        return neomcp::ResourceContents(contents);
    });
    server.resource("test://many", "Many", [] {
        neomcp::TextResourceContents a;
        a.uri = "test://many/a";
        a.text = "a";
        neomcp::BlobResourceContents b;
        b.uri = "test://many/b";
        b.blob = "AAA=";
        return std::vector<neomcp::ResourceContents>{a, b};
    });
    server.resource("test://result", "Result", [] {
        neomcp::TextResourceContents contents;
        contents.uri = "test://result";
        contents.text = "r";
        neomcp::ReadResult result;
        result.contents.emplace_back(contents);
        return result;
    });

    Client client(server, revision);
    const auto contents_of = [&](const std::string& uri) {
        const json answer = client.read(uri);
        EXPECT_TRUE(answer.contains("result")) << answer.dump();
        expect_conforms(result_of(answer), "ReadResourceResult", *client.revision);
        return result_of(answer).at("contents");
    };

    json text = contents_of("test://text");
    ASSERT_EQ(text.size(), 1u);
    EXPECT_EQ(text.at(0), (json{{"uri", "test://text"}, {"mimeType", "text/plain"}, {"text", "hello"}}));

    json bytes = contents_of("test://bytes");
    EXPECT_EQ(bytes.at(0), (json{{"uri", "test://bytes"}, {"mimeType", "image/png"}, {"blob", "AAH/"}}));

    json as_json = contents_of("test://json");
    EXPECT_EQ(as_json.at(0), (json{{"uri", "test://json"}, {"mimeType", "application/json"}, {"text", R"({"a":1})"}}));

    json as_struct = contents_of("test://struct");
    EXPECT_EQ(as_struct.at(0).at("mimeType"), "application/json");
    EXPECT_EQ(json::parse(as_struct.at(0).at("text").get<std::string>()), (json{{"note", "n"}, {"count", 3}}));

    // The resource's own MIME type wins over the default for JSON.
    EXPECT_EQ(contents_of("test://owned").at(0).at("mimeType"), "text/x-notes");

    EXPECT_EQ(contents_of("test://built").at(0), (json{{"uri", "test://built/part"}, {"text", "part"}}));
    json many = contents_of("test://many");
    ASSERT_EQ(many.size(), 2u);
    EXPECT_EQ(many.at(1), (json{{"uri", "test://many/b"}, {"blob", "AAA="}}));
    EXPECT_EQ(contents_of("test://result").at(0).at("text"), "r");
}

TEST(Resources, ABlobIsBase64OfEveryByteValue) {
    neomcp::Bytes all;
    for (int value = 0; value < 256; ++value) all.push_back(static_cast<std::byte>(value));
    neomcp::Server server(named("res"));
    server.resource("test://all", "All", [all] { return all; });
    Client client(server, modern_revision);
    const json answer = client.read("test://all");
    EXPECT_EQ(result_of(answer).at("contents").at(0).at("blob"), neomcp::base64_encode(std::span<const std::byte>(all)));
}

TEST(Resources, HandlerFormsAreCalled) {
    neomcp::Server server(named("res"));
    server.resource("f://plain", "n", [] { return std::string("plain"); });
    server.resource("f://context", "n", [](neomcp::Context& context) {
        return std::string(context.revision());
    });
    server.resource("f://coroutine", "n", []() -> asio::awaitable<std::string> { co_return std::string("co"); });
    server.resource("f://coroutine-context", "n", [](neomcp::Context& context) -> asio::awaitable<std::string> {
        co_return std::string(context.revision());
    });
    server.resource_template<ft::FileParams>("t://plain/{path}", "n",
                                              [](const ft::FileParams& params) { return params.path; });
    server.resource_template<ft::FileParams>("t://by-value/{path}", "n",
                                              [](ft::FileParams params) { return params.path + "!"; });
    server.resource_template<ft::FileParams>("t://context/{path}", "n",
                                              [](const ft::FileParams& params, neomcp::Context& context) {
                                                  return params.path + context.revision();
                                              });
    server.resource_template<ft::FileParams>("t://co/{path}", "n",
                                              [](const ft::FileParams& params) -> asio::awaitable<std::string> {
                                                  co_return params.path + "?";
                                              });

    Client client(server, modern_revision);
    const auto text = [&](const std::string& uri) {
        const json answer = client.read(uri);
        EXPECT_TRUE(answer.contains("result")) << answer.dump();
        return answer.contains("result") ? result_of(answer).at("contents").at(0).at("text").get<std::string>()
                                         : std::string();
    };
    EXPECT_EQ(text("f://plain"), "plain");
    EXPECT_EQ(text("f://context"), modern_revision);
    EXPECT_EQ(text("f://coroutine"), "co");
    EXPECT_EQ(text("f://coroutine-context"), modern_revision);
    EXPECT_EQ(text("t://plain/a"), "a");
    EXPECT_EQ(text("t://by-value/a"), "a!");
    EXPECT_EQ(text("t://context/a"), std::string("a") + modern_revision);
    EXPECT_EQ(text("t://co/a"), "a?");
}

// ---------------------------------------------------------------------------------------------------------------
// Resources: a resource that does not exist

// What each revision prescribes for a read of a URI nobody serves: the code in profile `errors.resource_not_found`
// where the revision defines it (2024-11-05 to 2025-11-25: -32002) and Invalid params (-32602) where it does not
// (2026-07-28, whose changelog moves the code to Invalid params and forbids -32002), with `data.uri` in both.
const std::map<std::string, int> not_found_code{
    {"2024-11-05", -32002}, {"2025-03-26", -32002}, {"2025-06-18", -32002}, {"2025-11-25", -32002},
    {"2026-07-28", -32602},
};

TEST_P(PerRevision, AResourceNobodyServesIsAnsweredWithTheRevisionsCode) {
    const std::string revision = GetParam();
    ASSERT_EQ(not_found_code.size(), profile::builtin_revisions().size());
    neomcp::Server server(named("res"));
    server.resource("test://there", "There", [] { return std::string("x"); });
    server.resource_template<ft::FileParams>("file:///{path}", "n", [](const ft::FileParams&) { return std::string("x"); });
    Client client(server, revision);

    const json answer = client.read("test://nonexistent-resource");
    ASSERT_TRUE(answer.contains("error")) << answer.dump();
    EXPECT_EQ(error_code_of(answer), not_found_code.at(revision));
    EXPECT_EQ(answer.at("error").at("data").at("uri"), "test://nonexistent-resource");
    EXPECT_FALSE(answer.contains("result"));
    // Whatever a revision forbids a server to emit, it never gets.
    for (const int forbidden : client.revision->not_emitted()) EXPECT_NE(error_code_of(answer), forbidden);
}

TEST_P(PerRevision, AHandlerThatThrowsResourceNotFoundIsAnsweredTheSameWay) {
    const std::string revision = GetParam();
    neomcp::Server server(named("res"));
    server.resource("test://gone", "n", []() -> std::string { throw neomcp::ResourceNotFound(); });
    server.resource_template<ft::FileParams>("file:///{path}", "n", [](const ft::FileParams& params) -> std::string {
        throw neomcp::ResourceNotFound("no file " + params.path);
    });
    server.resource_template<ft::FileParams>("co:///{path}", "n",
                                              [](const ft::FileParams& params) -> asio::awaitable<std::string> {
                                                  if (!params.path.empty()) throw neomcp::ResourceNotFound();
                                                  co_return std::string();
                                              });
    Client client(server, revision);
    for (const std::string uri : {"test://gone", "file:///a", "co:///a"}) {
        SCOPED_TRACE(uri);
        const json answer = client.read(uri);
        ASSERT_TRUE(answer.contains("error")) << answer.dump();
        EXPECT_EQ(error_code_of(answer), not_found_code.at(revision));
        EXPECT_EQ(answer.at("error").at("data").at("uri"), uri);
    }
}

TEST(Resources, AnyOtherExceptionIsAnInternalErrorWhoseTextStaysHome) {
    for (const bool expose : {false, true}) {
        SCOPED_TRACE(expose);
        neomcp::ServerOptions options = named("res");
        options.expose_error_details = expose;
        neomcp::Server server(std::move(options));
        server.resource("test://bad", "n", []() -> std::string { throw std::runtime_error("the secret detail"); });
        server.resource("test://tool-error", "n", []() -> std::string { throw neomcp::ToolError("tool error text"); });
        Client client(server, modern_revision);
        for (const char* uri : {"test://bad", "test://tool-error"}) {
            const json answer = client.read(uri);
            ASSERT_TRUE(answer.contains("error")) << answer.dump();
            EXPECT_EQ(error_code_of(answer), -32603);
            const std::string message = answer.at("error").at("message").get<std::string>();
            EXPECT_EQ(message.find("secret") != std::string::npos || message.find("tool error text") != std::string::npos,
                      expose)
                << message;
        }
    }
}

TEST(Resources, ReadWithoutAStringUriIsInvalidParams) {
    neomcp::Server server(named("res"));
    server.resource("test://x", "n", [] { return std::string("x"); });
    Client client(server, modern_revision);
    const json answer = client.ask("resources/read", json{{"uri", 5}});
    EXPECT_EQ(error_code_of(answer), -32602);
}

// ---------------------------------------------------------------------------------------------------------------
// Templates

TEST(Templates, DecodeVariablesAsTheMembersOfTheParameters) {
    neomcp::Server server(named("res"));
    server.resource_template<ft::ItemParams>("items://{id}/{verbose}", "n", [](const ft::ItemParams& params) {
        return std::to_string(params.id) + (params.verbose ? "+" : "-");
    });
    server.resource_template<ft::SearchParams>("search://items{?q,limit}", "n", [](const ft::SearchParams& params) {
        return params.q + ":" + (params.limit ? std::to_string(*params.limit) : std::string("none"));
    });
    server.resource_template<ft::FileParams>("file:///{path}", "n", [](const ft::FileParams& params) {
        return params.path;
    });
    Client client(server, modern_revision);
    const auto text = [&](const std::string& uri) -> std::string {
        const json answer = client.read(uri);
        if (!answer.contains("result")) return "error " + std::to_string(error_code_of(answer));
        return result_of(answer).at("contents").at(0).at("text").get<std::string>();
    };
    EXPECT_EQ(text("items://42/true"), "42+");
    EXPECT_EQ(text("items://-7/false"), "-7-");
    EXPECT_EQ(text("items://x/true"), "error -32602") << "not a number: no match";
    EXPECT_EQ(text("items://2147483648/true"), "error -32602") << "out of the range of int: no match";
    EXPECT_EQ(text("items://1/yes"), "error -32602") << "not a boolean: no match";
    EXPECT_EQ(text("search://items?q=cat&limit=5"), "cat:5");
    EXPECT_EQ(text("search://items?q=cat"), "cat:none");
    EXPECT_EQ(text("search://items?limit=5"), "error -32602") << "a required variable the URI lacks: no match";
    EXPECT_EQ(text("search://items?q=cat&limit=70000"), "error -32602") << "out of the range of uint16: no match";
    EXPECT_EQ(text("file:///a%20b"), "a b");
    EXPECT_EQ(text("file:///a%2"), "error -32602") << "a malformed escape: no match";
    EXPECT_EQ(text("file:///a/b"), "error -32602") << "{path} holds no '/'";
}

TEST(Templates, ADecodedValueMayHoldPathCharactersButNotNul) {
    neomcp::Server server(named("res"));
    server.resource_template<ft::FileParams>("file:///{path}", "n", [](const ft::FileParams& params) {
        return params.path;
    });
    server.resource_template<ft::KindParams>("kind:///{kind}/{+name}", "n", [](const ft::KindParams& params) {
        return params.kind + "|" + params.name;
    });
    Client client(server, modern_revision);
    const auto text = [&](const std::string& uri) -> std::string {
        const json answer = client.read(uri);
        if (!answer.contains("result")) return "error " + std::to_string(error_code_of(answer));
        return result_of(answer).at("contents").at(0).at("text").get<std::string>();
    };
    EXPECT_EQ(text("file:///a%2Fb"), "a/b");
    EXPECT_EQ(text("file:///%2E%2E%2Fetc"), "../etc");
    EXPECT_EQ(text("file:///a%00b"), "error -32602") << "NUL is refused: no template matches";
    EXPECT_EQ(text("kind:///k/dir/%00"), "error -32602");
    EXPECT_EQ(text("kind:///k/dir/ok"), "k|dir/ok");
}

TEST(Templates, TheTestSuitesCase) {
    neomcp::Server server(named("res"));
    server.resource_template<ft::FileParams>("test://template/{path}/data", "n", [](const ft::FileParams& params) {
        return json{{"id", params.path}, {"templateTest", true}};
    });
    Client client(server, modern_revision);
    const json answer = client.read("test://template/123/data");
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    const json& contents = result_of(answer).at("contents").at(0);
    EXPECT_NE(contents.at("text").get<std::string>().find("123"), std::string::npos);
    EXPECT_EQ(contents.at("uri"), "test://template/123/data");
}

TEST(Templates, AFixedResourceWinsAndTheFirstRegisteredTemplateWins) {
    neomcp::Server server(named("res"));
    server.resource_template<ft::FileParams>("m://{path}", "first", [](const ft::FileParams&) { return std::string("first"); });
    server.resource("m://special", "special", [] { return std::string("fixed"); });
    // Matches every URI of the first one, and more.
    server.resource_template<ft::FileParams>("m:{+path}", "second", [](const ft::FileParams&) { return std::string("second"); });
    Client client(server, modern_revision);
    const auto text = [&](const std::string& uri) {
        return result_of(client.read(uri)).at("contents").at(0).at("text").get<std::string>();
    };
    EXPECT_EQ(text("m://special"), "fixed");
    EXPECT_EQ(text("m://other"), "first");
    EXPECT_EQ(text("m:a/b"), "second");
}

TEST(Templates, AMatchWhoseVariablesDoNotDecodeFallsThroughToTheNextTemplate) {
    neomcp::Server server(named("res"));
    server.resource_template<ft::ItemParams>("g://{id}/{verbose}", "numbers",
                                              [](const ft::ItemParams&) { return std::string("number"); });
    server.resource_template<ft::KindParams>("g://{kind}/{name}", "words",
                                              [](const ft::KindParams&) { return std::string("word"); });
    Client client(server, modern_revision);
    EXPECT_EQ(result_of(client.read("g://3/true")).at("contents").at(0).at("text"), "number");
    EXPECT_EQ(result_of(client.read("g://a/true")).at("contents").at(0).at("text"), "word");
}

// ---------------------------------------------------------------------------------------------------------------
// Registration

TEST(Registration, RefusedResourcesNameThemselves) {
    neomcp::Server server(named("res"));
    server.resource("a://one", "One", [] { return std::string("x"); });
    const auto refused = [&](auto registration, const std::string& part) {
        try {
            registration();
            ADD_FAILURE() << "expected a refusal naming " << part;
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(part), std::string::npos) << error.what();
        }
    };
    refused([&] { server.resource("", "n", [] { return std::string(); }); }, "URI");
    refused([&] { server.resource("a://x", "", [] { return std::string(); }); }, "a://x");
    refused([&] { server.resource("a://one", "again", [] { return std::string(); }); }, "a://one");

    const auto file = [](const ft::FileParams&) { return std::string(); };
    refused([&] { server.resource_template<ft::FileParams>("", "n", file); }, "URI template");
    refused([&] { server.resource_template<ft::FileParams>("t://{path", "n", file); }, "t://{path");
    refused([&] { server.resource_template<ft::FileParams>("t://{=path}", "n", file); }, "'='");
    refused([&] { server.resource_template<ft::FileParams>("t://{#path}", "n", file); }, "'#'");
    refused([&] { server.resource_template<ft::FileParams>("t://{path*}", "n", file); }, "'*'");
    refused([&] { server.resource_template<ft::FileParams>("t://{path:3}", "n", file); }, "':3'");
    refused([&] { server.resource_template<ft::FileParams>("t://{path}{path}", "n", file); }, "path");
    refused([&] { server.resource_template<ft::FileParams>("t://{other}", "n", file); }, "'other'");
    refused([&] { server.resource_template<ft::FileParams>("t://fixed", "n", file); }, "'path'");
    refused([&] {
        server.resource_template<ft::KindParams>("t://{kind}", "n", [](const ft::KindParams&) { return std::string(); });
    }, "'name'");
    server.resource_template<ft::FileParams>("t://{path}", "n", file);
    refused([&] { server.resource_template<ft::FileParams>("t://{path}", "again", file); }, "t://{path}");

    const auto handle = server.resource("a://two", "Two", [] { return std::string("x"); });
    EXPECT_THROW(handle.meta(json::array()), std::invalid_argument);
    EXPECT_EQ(handle.uri(), "a://two");
}

TEST(Registration, ChangesAfterTheServerServesAreLogicErrors) {
    neomcp::Server server(named("res"));
    const auto handle = server.resource("a://one", "One", [] { return std::string("x"); });
    const auto template_handle = server.resource_template<ft::FileParams>(
        "t://{path}", "T", [](const ft::FileParams&) { return std::string(); });
    Client client(server, modern_revision);
    static_cast<void>(client.ask("resources/list"));
    EXPECT_THROW(handle.title("late"), std::logic_error);
    EXPECT_THROW(handle.cache(neomcp::CacheHints{}), std::logic_error);
    EXPECT_THROW(template_handle.title("late"), std::logic_error);
    EXPECT_THROW(server.resource("a://late", "Late", [] { return std::string(); }), std::logic_error);
    EXPECT_THROW(server.resource_template<ft::FileParams>("late://{path}", "Late",
                                                           [](const ft::FileParams&) { return std::string(); }),
                 std::logic_error);
}

TEST(Registration, ZeroResourcePageSizeIsRefused) {
    neomcp::ServerOptions options = named("res");
    options.resources_page_size = 0;
    EXPECT_THROW(neomcp::Server{std::move(options)}, std::invalid_argument);
}

// ---------------------------------------------------------------------------------------------------------------
// Paging

TEST_P(PerRevision, ResourceListsArePagedWithOpaqueCursors) {
    const std::string revision = GetParam();
    neomcp::ServerOptions options = named("res");
    options.resources_page_size = 2;
    neomcp::Server server(std::move(options));
    for (int i = 0; i < 5; ++i) {
        server.resource("r://" + std::to_string(i), "R" + std::to_string(i), [] { return std::string(); });
    }
    for (int i = 0; i < 3; ++i) {
        server.resource_template<ft::FileParams>("t" + std::to_string(i) + "://{path}", "T" + std::to_string(i),
                                                  [](const ft::FileParams&) { return std::string(); });
    }
    Client client(server, revision);

    std::vector<std::string> seen;
    json params = json::object();
    int pages = 0;
    for (;;) {
        const json page = result_of(client.ask("resources/list", params));
        ++pages;
        EXPECT_LE(page.at("resources").size(), 2u);
        for (const auto& resource : *page.at("resources").get_ptr<const json::array_t*>()) {
            seen.push_back(resource.at("uri").get<std::string>());
        }
        if (!page.contains("nextCursor")) break;
        params = json{{"cursor", page.at("nextCursor")}};
        ASSERT_LT(pages, 10);
    }
    EXPECT_EQ(pages, 3);
    EXPECT_EQ(seen, (std::vector<std::string>{"r://0", "r://1", "r://2", "r://3", "r://4"}));

    const json templates = result_of(client.ask("resources/templates/list"));
    EXPECT_EQ(templates.at("resourceTemplates").size(), 2u);
    ASSERT_TRUE(templates.contains("nextCursor"));
    const json rest = result_of(client.ask("resources/templates/list", json{{"cursor", templates.at("nextCursor")}}));
    EXPECT_EQ(rest.at("resourceTemplates").size(), 1u);
    EXPECT_FALSE(rest.contains("nextCursor"));

    // A cursor this server did not issue is Invalid params, whatever it looks like.
    for (const json& cursor : {json("nonsense"), json("99"), json("-1"), json("01"), json(""), json(3), json(nullptr)}) {
        SCOPED_TRACE(cursor.dump());
        EXPECT_EQ(error_code_of(client.ask("resources/list", json{{"cursor", cursor}})), -32602);
        EXPECT_EQ(error_code_of(client.ask("resources/templates/list", json{{"cursor", cursor}})), -32602);
    }
}

TEST(Paging, WithoutAPageSizeNoCursorIsValid) {
    neomcp::Server server(named("res"));
    server.resource("r://0", "R", [] { return std::string(); });
    Client client(server, modern_revision);
    const json page = result_of(client.ask("resources/list"));
    EXPECT_FALSE(page.contains("nextCursor"));
    EXPECT_EQ(error_code_of(client.ask("resources/list", json{{"cursor", "0"}})), -32602);
}

// ---------------------------------------------------------------------------------------------------------------
// Capabilities

TEST_P(PerRevision, TheResourcesCapabilityIsDeclaredExactlyWhenSomethingIsRegistered) {
    const std::string revision = GetParam();
    {
        neomcp::Server server(named("none"));
        server.tool("t", "", [] { return std::string(); });
        Client client(server, revision);
        EXPECT_FALSE(client.capabilities().contains(resources_capability)) << client.capabilities().dump();
        for (const char* method : {"resources/list", "resources/templates/list", "resources/read"}) {
            SCOPED_TRACE(method);
            const json answer = client.ask(method, json{{"uri", "x://y"}});
            EXPECT_EQ(error_code_of(answer), -32601) << answer.dump();
        }
    }
    {
        neomcp::Server server(named("resource"));
        server.resource("a://x", "X", [] { return std::string(); });
        Client client(server, revision);
        const json capabilities = client.capabilities();
        ASSERT_TRUE(capabilities.contains(resources_capability)) << capabilities.dump();
        // Nothing is promised that a later feature will deliver.
        EXPECT_FALSE(capabilities.at(resources_capability).contains("listChanged"));
        EXPECT_FALSE(capabilities.at(resources_capability).contains("subscribe"));
        EXPECT_TRUE(client.ask("resources/templates/list").contains("result"));
    }
    {
        neomcp::Server server(named("template"));
        server.resource_template<ft::FileParams>("a://{path}", "T", [](const ft::FileParams&) { return std::string(); });
        Client client(server, revision);
        EXPECT_TRUE(client.capabilities().contains(resources_capability));
        const json list = client.ask("resources/list");
        ASSERT_TRUE(list.contains("result")) << list.dump();
        EXPECT_EQ(result_of(list).at("resources").size(), 0u);
    }
}

// A session over a host that offers nothing but tools answers the methods of the other features as unknown.
class ToolsOnlyHost final : public neomcp::ToolHost {
public:
    std::span<const neomcp::ToolDefinition> tools() const override { return {}; }
    void call(const neomcp::CallRequest&, neomcp::CallControl, neomcp::Completion done) override {
        done(neomcp::HostFailure{"no tools"});
    }
};

TEST(Capabilities, ABareToolHostAnswersTheOtherMethodsAsUnknown) {
    ToolsOnlyHost host;
    Peer peer(host);
    const auto revision = builtin("2025-06-18");
    ASSERT_TRUE(peer.initialize(*revision, patience));
    for (const char* method : {"resources/list", "resources/templates/list", "resources/read"}) {
        const auto answer = peer.request(Peer::make_request(*revision, 7, method, json{{"uri", "x://y"}}), patience);
        ASSERT_TRUE(answer);
        EXPECT_EQ(error_code_of(*answer), -32601) << method;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Results of 2026-07-28 and caching hints

TEST(Caching, EveryResourceResultOf20260728CarriesWhatItsSchemaRequires) {
    neomcp::Server server(named("res"));
    server.resource("a://x", "X", [] { return std::string("x"); });
    server.resource_template<ft::FileParams>("t://{path}", "T", [](const ft::FileParams&) { return std::string("t"); });
    Client client(server, modern_revision);
    for (const auto& [method, type] : std::vector<std::pair<std::string, std::string>>{
             {"resources/list", "ListResourcesResult"}, {"resources/templates/list", "ListResourceTemplatesResult"}}) {
        SCOPED_TRACE(method);
        const json result = result_of(client.ask(method));
        expect_conforms(result, type, *client.revision);
        EXPECT_EQ(result.at("resultType"), "complete");
        EXPECT_EQ(result.at("ttlMs"), 0);
        EXPECT_EQ(result.at("cacheScope"), "private");
    }
    for (const std::string uri : {"a://x", "t://abc"}) {
        const json result = result_of(client.read(uri));
        expect_conforms(result, "ReadResourceResult", *client.revision);
        EXPECT_EQ(result.at("resultType"), "complete");
        EXPECT_EQ(result.at("ttlMs"), 0);
        EXPECT_EQ(result.at("cacheScope"), "private");
    }
}

TEST(Caching, DefaultsComeFromTheOptionsAndAResourceCanOverrideThem) {
    neomcp::ServerOptions options = named("res");
    options.resources_cache = neomcp::CacheHints{5000, neomcp::CacheScope::public_scope};
    options.resource_read_cache = neomcp::CacheHints{7000, neomcp::CacheScope::private_scope};
    options.tools_cache = neomcp::CacheHints{1, neomcp::CacheScope::private_scope};
    neomcp::Server server(std::move(options));
    server.resource("a://default", "D", [] { return std::string("x"); });
    server.resource("a://own", "O", [] { return std::string("x"); }).cache({9000, neomcp::CacheScope::public_scope});
    server.resource("a://built", "B", [] {
        neomcp::ReadResult result;
        neomcp::TextResourceContents contents;
        contents.uri = "a://built";
        contents.text = "x";
        result.contents.emplace_back(contents);
        result.cache = neomcp::CacheHints{11000, neomcp::CacheScope::public_scope};
        return result;
    }).cache({9000, neomcp::CacheScope::public_scope});
    server.resource_template<ft::FileParams>("t://{path}", "T", [](const ft::FileParams&) { return std::string("t"); })
        .cache({13000, neomcp::CacheScope::public_scope});

    Client client(server, modern_revision);
    EXPECT_EQ(result_of(client.ask("resources/list")).at("ttlMs"), 5000);
    EXPECT_EQ(result_of(client.ask("resources/list")).at("cacheScope"), "public");
    EXPECT_EQ(result_of(client.ask("resources/templates/list")).at("ttlMs"), 5000);
    const auto hints = [&](const std::string& uri) {
        const json result = result_of(client.read(uri));
        return std::make_pair(result.at("ttlMs").get<int>(), result.at("cacheScope").get<std::string>());
    };
    EXPECT_EQ(hints("a://default"), std::make_pair(7000, std::string("private")));
    EXPECT_EQ(hints("a://own"), std::make_pair(9000, std::string("public")));
    EXPECT_EQ(hints("a://built"), std::make_pair(11000, std::string("public")));
    EXPECT_EQ(hints("t://x"), std::make_pair(13000, std::string("public")));
}

TEST_P(PerRevision, LegacyResultsCarryNeitherResultTypeNorCachingHints) {
    const std::string revision = GetParam();
    if (!is_legacy(revision)) GTEST_SKIP() << "only the revisions that lack them";
    neomcp::Server server(named("res"));
    server.resource("a://x", "X", [] { return std::string("x"); });
    Client client(server, revision);
    for (const json& result : {result_of(client.ask("resources/list")), result_of(client.read("a://x"))}) {
        for (const char* member : {"resultType", "ttlMs", "cacheScope"}) EXPECT_FALSE(result.contains(member)) << member;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Context

TEST(Resources, AReadReportsProgress) {
    neomcp::Server server(named("res"));
    server.resource("a://x", "X", [](neomcp::Context& context) {
        context.progress(1, 2, "half");
        context.progress(2, 2);
        return std::string("done");
    });
    Client client(server, modern_revision);
    const json answer = client.read("a://x", json{{"progressToken", "p"}});
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    const auto first = client.peer.next_notification(patience);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->at("params").at("progressToken"), "p");
    EXPECT_EQ(first->at("params").at("progress"), 1);
    const auto second = client.peer.next_notification(patience);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->at("params").at("progress"), 2);
}

// ---------------------------------------------------------------------------------------------------------------
// Concurrency

TEST(Concurrency, ACancelledPlainReadSeesStopAndNothingIsWrittenAfter) {
    Event entered;
    Event observed;
    bool saw_stop = false;
    neomcp::ServerOptions options = named("res");
    options.protocol_logging = true;
    options.max_concurrent_calls = 1;
    neomcp::Server server(std::move(options));
    server.resource("a://wait", "W", [&](neomcp::Context& context) {
        entered.set();
        Event never;
        static_cast<void>(never.wait(context.stop_token()));
        saw_stop = context.cancelled();
        observed.set();
        context.progress(5);
        return std::string("late");
    });
    server.resource("a://echo", "E", [] { return std::string("echo"); });

    Client client(server, modern_revision);
    client.start_read("c1", "a://wait", json{{"progressToken", "t1"}});
    ASSERT_TRUE(entered.wait(patience));
    const std::size_t before = client.peer.transcript().size();
    client.peer.notify(cancelled("c1"));
    ASSERT_TRUE(observed.wait(patience));
    EXPECT_TRUE(saw_stop);
    // The only worker was held by the cancelled read; the next read runs once it ended.
    EXPECT_EQ(result_of(client.read("a://echo")).at("contents").at(0).at("text"), "echo");
    const auto transcript = client.peer.transcript();
    ASSERT_EQ(transcript.size(), before + 1) << transcript.back().dump();
    for (std::size_t i = before; i < transcript.size(); ++i) {
        EXPECT_FALSE(transcript[i].contains("id") && transcript[i].at("id") == "c1") << transcript[i].dump();
        EXPECT_FALSE(transcript[i].contains("params") && transcript[i].at("params").contains("progressToken"));
    }
}

TEST(Concurrency, ACancelledCoroutineReadSeesStop) {
    Event entered;
    Event observed;
    neomcp::Server server(named("res"));
    server.resource_template<ft::FileParams>("a://wait/{path}", "W",
                                              [&](const ft::FileParams&, neomcp::Context& context)
                                                  -> asio::awaitable<std::string> {
                                                  entered.set();
                                                  co_await until_stopped(context.stop_token());
                                                  if (context.cancelled()) observed.set();
                                                  co_return std::string("late");
                                              });
    Client client(server, modern_revision);
    client.start_read("c1", "a://wait/x");
    ASSERT_TRUE(entered.wait(patience));
    client.peer.notify(cancelled("c1"));
    EXPECT_TRUE(observed.wait(patience));
}

TEST(Concurrency, TwoReadsRunAtOnce) {
    Meeting meeting(2);
    std::atomic<int> failures{0};
    neomcp::Server server(named("res"));  // four workers by default
    server.resource("a://one", "1", [&] {
        if (!meeting.arrive(patience)) ++failures;
        return std::string("one");
    });
    server.resource_template<ft::FileParams>("a://two/{path}", "2", [&](const ft::FileParams&) {
        if (!meeting.arrive(patience)) ++failures;
        return std::string("two");
    });
    Client client(server, modern_revision);
    client.start_read("one", "a://one");
    client.start_read("two", "a://two/x");
    const auto first = client.peer.await_response("one", patience);
    const auto second = client.peer.await_response("two", patience);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(result_of(*first).at("contents").at(0).at("text"), "one");
    EXPECT_EQ(result_of(*second).at("contents").at(0).at("text"), "two");
    EXPECT_EQ(failures.load(), 0) << "the two reads were not in their handlers together";
}

TEST(Concurrency, ReadsAreBoundedLikeToolCalls) {
    Event entered;
    Event release;
    neomcp::ServerOptions options = named("res");
    options.max_concurrent_calls = 1;
    options.max_pending_calls = 0;
    neomcp::Server server(std::move(options));
    server.resource("a://hold", "H", [&] {
        entered.set();
        static_cast<void>(release.wait(patience));
        return std::string("held");
    });
    server.resource("a://other", "O", [] { return std::string("other"); });
    Client client(server, modern_revision);
    client.start_read("hold", "a://hold");
    ASSERT_TRUE(entered.wait(patience));
    const json refused = client.read("a://other");
    ASSERT_TRUE(refused.contains("error")) << refused.dump();
    EXPECT_EQ(error_code_of(refused), -32603);
    EXPECT_EQ(refused.at("error").at("data").at("reason"), "busy");
    release.set();
    ASSERT_TRUE(client.peer.await_response("hold", patience));
}

// ---------------------------------------------------------------------------------------------------------------
// Prompts

// Prompt per revision (schema.json of each revision): title and _meta from 2025-06-18, icons from 2025-11-25.
const std::map<std::string, std::set<std::string>> prompt_members{
    {"2024-11-05", {"arguments", "description", "name"}},
    {"2025-03-26", {"arguments", "description", "name"}},
    {"2025-06-18", {"_meta", "arguments", "description", "name", "title"}},
    {"2025-11-25", {"_meta", "arguments", "description", "icons", "name", "title"}},
    {"2026-07-28", {"_meta", "arguments", "description", "icons", "name", "title"}},
};

TEST_P(PerRevision, ListedPromptsHaveExactlyTheRevisionsMembersAndTheArgumentsOfTheirType) {
    const std::string revision = GetParam();
    ASSERT_EQ(prompt_members.size(), profile::builtin_revisions().size());
    neomcp::Server server(named("prompts"));
    server.prompt<ft::ReviewArgs>("review", "Review a change",
                                   [](const ft::ReviewArgs& args) { return "Review " + args.title; })
        .title("Review")
        .icons({an_icon()})
        .meta(json{{"k", "v"}});
    server.prompt("hello", "Say hello", [] { return std::string("Hello"); });

    Client client(server, revision);
    const json list = result_of(client.ask("prompts/list"));
    expect_conforms(list, "ListPromptsResult", *client.revision);
    ASSERT_EQ(list.at("prompts").size(), 2u);
    const json& review = list.at("prompts").at(0);
    EXPECT_EQ(keys(review), prompt_members.at(revision)) << review.dump();
    expect_conforms(review, "Prompt", *client.revision);
    EXPECT_EQ(review.at("name"), "review");
    EXPECT_EQ(review.at("description"), "Review a change");
    EXPECT_EQ(review.at("arguments"),
              (json::array({json{{"name", "title"}, {"description", "what to review"}, {"required", true}},
                            json{{"name", "focus"}, {"description", "what to look at"}, {"required", false}}})));
    for (const auto& argument : *review.at("arguments").get_ptr<const json::array_t*>()) {
        expect_conforms(argument, "PromptArgument", *client.revision);
    }
    const json& hello = list.at("prompts").at(1);
    EXPECT_FALSE(hello.contains("arguments")) << "a prompt without arguments lists none";
}

TEST_P(PerRevision, GetReturnsTheMessagesOfEachResultType) {
    const std::string revision = GetParam();
    neomcp::Server server(named("prompts"));
    server.prompt("text", "", [] { return std::string("Hello"); });
    server.prompt("message", "", [] {
        neomcp::PromptMessage message;
        message.role = neomcp::Role::assistant;
        neomcp::TextContent text;
        text.text = "from the assistant";
        message.content = text;
        return message;
    });
    server.prompt("image", "", [] {
        neomcp::ImageContent image;
        image.data = "AAAA";
        image.mime_type = "image/png";
        neomcp::PromptMessage message;
        message.content = image;
        return std::vector<neomcp::PromptMessage>{message, message};
    });
    server.prompt("resource", "", [] {
        neomcp::TextResourceContents contents;
        contents.uri = "test://example-resource";
        contents.text = "inside";
        neomcp::EmbeddedResource embedded;
        embedded.resource = contents;
        neomcp::PromptMessage message;
        message.content = embedded;
        neomcp::PromptResult result;
        result.description = "with a resource";
        result.messages.push_back(message);
        result.meta = json{{"k", "v"}};
        return result;
    });
    server.prompt("audio", "", [] {
        neomcp::AudioContent audio;
        audio.data = "AAAA";
        audio.mime_type = "audio/wav";
        neomcp::PromptMessage message;
        message.content = audio;
        return message;
    });

    Client client(server, revision);
    const auto get = [&](const std::string& name) {
        const json answer = client.ask("prompts/get", json{{"name", name}});
        EXPECT_TRUE(answer.contains("result")) << answer.dump();
        expect_conforms(result_of(answer), "GetPromptResult", *client.revision);
        return result_of(answer);
    };

    const json text = get("text");
    ASSERT_EQ(text.at("messages").size(), 1u);
    EXPECT_EQ(text.at("messages").at(0), (json{{"role", "user"}, {"content", {{"type", "text"}, {"text", "Hello"}}}}));
    EXPECT_FALSE(text.contains("description"));

    EXPECT_EQ(get("message").at("messages").at(0).at("role"), "assistant");

    const json image = get("image");
    ASSERT_EQ(image.at("messages").size(), 2u);
    EXPECT_EQ(image.at("messages").at(1).at("content"),
              (json{{"type", "image"}, {"data", "AAAA"}, {"mimeType", "image/png"}}));

    const json resource = get("resource");
    EXPECT_EQ(resource.at("description"), "with a resource");
    EXPECT_EQ(resource.at("messages").at(0).at("content").at("type"), "resource");
    EXPECT_EQ(resource.at("messages").at(0).at("content").at("resource").at("uri"), "test://example-resource");
    // Every revision's result type has `_meta`.
    EXPECT_EQ(resource.at("_meta").at("k"), "v");

    // Audio exists from 2025-03-26; before it the block is written as text that says so.
    const json audio = get("audio").at("messages").at(0).at("content");
    EXPECT_EQ(audio.at("type"), revision == "2024-11-05" ? "text" : "audio");
}

TEST(Prompts, HandlerFormsAreCalled) {
    neomcp::Server server(named("prompts"));
    server.prompt("plain", "", [] { return std::string("plain"); });
    server.prompt("context", "", [](neomcp::Context& context) { return std::string(context.revision()); });
    server.prompt("coroutine", "", []() -> asio::awaitable<std::string> { co_return std::string("co"); });
    server.prompt<ft::TwoArgs>("by-value", "", [](ft::TwoArgs args) { return args.arg1 + args.arg2; });
    server.prompt<ft::TwoArgs>("with-context", "", [](const ft::TwoArgs& args, neomcp::Context& context) {
        return args.arg1 + std::string(context.revision());
    });
    server.prompt<ft::TwoArgs>("co-args", "", [](const ft::TwoArgs& args) -> asio::awaitable<std::string> {
        co_return args.arg2 + args.arg1;
    });
    server.prompt<ft::ReviewArgs>("optional", "", [](const ft::ReviewArgs& args) {
        return args.title + (args.focus ? "/" + *args.focus : std::string("/none"));
    });

    Client client(server, modern_revision);
    const auto text = [&](const std::string& name, json arguments = json::object()) {
        const json answer = client.ask("prompts/get", json{{"name", name}, {"arguments", std::move(arguments)}});
        EXPECT_TRUE(answer.contains("result")) << answer.dump();
        return answer.contains("result")
                   ? result_of(answer).at("messages").at(0).at("content").at("text").get<std::string>()
                   : std::string();
    };
    EXPECT_EQ(text("plain"), "plain");
    EXPECT_EQ(text("context"), modern_revision);
    EXPECT_EQ(text("coroutine"), "co");
    EXPECT_EQ(text("by-value", {{"arg1", "a"}, {"arg2", "b"}}), "ab");
    EXPECT_EQ(text("with-context", {{"arg1", "a"}, {"arg2", "b"}}), std::string("a") + modern_revision);
    EXPECT_EQ(text("co-args", {{"arg1", "a"}, {"arg2", "b"}}), "ba");
    EXPECT_EQ(text("optional", {{"title", "t"}}), "t/none");
    EXPECT_EQ(text("optional", {{"title", "t"}, {"focus", "f"}}), "t/f");
    EXPECT_EQ(text("optional", {{"title", "t"}, {"unknown", "ignored"}}), "t/none");
}

TEST_P(PerRevision, UnknownPromptsAndBadArgumentsAreInvalidParams) {
    neomcp::Server server(named("prompts"));
    server.prompt<ft::ReviewArgs>("review", "", [](const ft::ReviewArgs&) { return std::string("x"); });
    Client client(server, GetParam());

    const auto code = [&](json params) { return error_code_of(client.ask("prompts/get", std::move(params))); };
    EXPECT_EQ(code(json{{"name", "nonexistent"}}), -32602);
    EXPECT_EQ(code(json{{"name", 5}}), -32602);
    EXPECT_EQ(code(json{{"name", "review"}}), -32602) << "the required argument is missing";
    EXPECT_EQ(code(json{{"name", "review"}, {"arguments", json::object()}}), -32602);
    EXPECT_EQ(code(json{{"name", "review"}, {"arguments", {{"focus", "f"}}}}), -32602);
    EXPECT_EQ(code(json{{"name", "review"}, {"arguments", json::array()}}), -32602);
    EXPECT_EQ(code(json{{"name", "review"}, {"arguments", {{"title", 5}}}}), -32602) << "arguments are strings";
    EXPECT_EQ(code(json{{"name", "review"}, {"arguments", {{"title", "t"}}}}), 0);

    const json missing = client.ask("prompts/get", json{{"name", "review"}});
    EXPECT_NE(missing.at("error").at("message").get<std::string>().find("title"), std::string::npos)
        << "the message names the missing argument";
    const json unknown = client.ask("prompts/get", json{{"name", "nonexistent"}});
    EXPECT_NE(unknown.at("error").at("message").get<std::string>().find("nonexistent"), std::string::npos);
}

TEST_P(PerRevision, ThePromptsCapabilityIsDeclaredExactlyWhenAPromptIsRegistered) {
    const std::string revision = GetParam();
    {
        neomcp::Server server(named("none"));
        server.resource("a://x", "X", [] { return std::string(); });
        Client client(server, revision);
        EXPECT_FALSE(client.capabilities().contains("prompts")) << client.capabilities().dump();
        EXPECT_EQ(error_code_of(client.ask("prompts/list")), -32601);
        EXPECT_EQ(error_code_of(client.ask("prompts/get", json{{"name", "x"}})), -32601);
    }
    {
        neomcp::Server server(named("prompt"));
        server.prompt("p", "", [] { return std::string(); });
        Client client(server, revision);
        const json capabilities = client.capabilities();
        ASSERT_TRUE(capabilities.contains("prompts")) << capabilities.dump();
        EXPECT_FALSE(capabilities.at("prompts").contains("listChanged"));
        EXPECT_FALSE(capabilities.contains(resources_capability));
        EXPECT_EQ(error_code_of(client.ask("resources/list")), -32601);
    }
}

TEST_P(PerRevision, PromptListsArePaged) {
    neomcp::ServerOptions options = named("prompts");
    options.prompts_page_size = 2;
    neomcp::Server server(std::move(options));
    for (int i = 0; i < 5; ++i) server.prompt("p" + std::to_string(i), "d", [] { return std::string(); });
    Client client(server, GetParam());
    std::vector<std::string> seen;
    json params = json::object();
    for (int pages = 0; pages < 10; ++pages) {
        const json page = result_of(client.ask("prompts/list", params));
        for (const auto& prompt : *page.at("prompts").get_ptr<const json::array_t*>()) {
            seen.push_back(prompt.at("name").get<std::string>());
        }
        if (!page.contains("nextCursor")) break;
        params = json{{"cursor", page.at("nextCursor")}};
    }
    EXPECT_EQ(seen, (std::vector<std::string>{"p0", "p1", "p2", "p3", "p4"}));
    EXPECT_EQ(error_code_of(client.ask("prompts/list", json{{"cursor", "7"}})), -32602);
    EXPECT_EQ(error_code_of(client.ask("prompts/list", json{{"cursor", "x"}})), -32602);
}

TEST(Prompts, ResultsOf20260728CarryWhatTheirSchemasRequire) {
    neomcp::ServerOptions options = named("prompts");
    options.prompts_cache = neomcp::CacheHints{4000, neomcp::CacheScope::public_scope};
    neomcp::Server server(std::move(options));
    server.prompt("p", "d", [] { return std::string("x"); });
    Client client(server, modern_revision);
    const json list = result_of(client.ask("prompts/list"));
    expect_conforms(list, "ListPromptsResult", *client.revision);
    EXPECT_EQ(list.at("resultType"), "complete");
    EXPECT_EQ(list.at("ttlMs"), 4000);
    EXPECT_EQ(list.at("cacheScope"), "public");
    const json got = result_of(client.ask("prompts/get", json{{"name", "p"}}));
    expect_conforms(got, "GetPromptResult", *client.revision);
    EXPECT_EQ(got.at("resultType"), "complete");
}

TEST(Prompts, RegistrationRefusalsNameThePrompt) {
    neomcp::Server server(named("prompts"));
    const auto handle = server.prompt("p", "d", [] { return std::string(); });
    EXPECT_EQ(handle.name(), "p");
    EXPECT_THROW(handle.meta(json::array()), std::invalid_argument);
    try {
        server.prompt("p", "again", [] { return std::string(); });
        ADD_FAILURE() << "a duplicate name is refused";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("'p'"), std::string::npos) << error.what();
    }
    EXPECT_THROW(server.prompt("", "d", [] { return std::string(); }), std::invalid_argument);
    Client client(server, modern_revision);
    static_cast<void>(client.ask("prompts/list"));
    EXPECT_THROW(handle.title("late"), std::logic_error);
    EXPECT_THROW(server.prompt("late", "d", [] { return std::string(); }), std::logic_error);

    neomcp::ServerOptions options = named("zero");
    options.prompts_page_size = 0;
    EXPECT_THROW(neomcp::Server{std::move(options)}, std::invalid_argument);
}

TEST(Prompts, ACancelledGetSeesStopAndTwoRunAtOnce) {
    Event entered;
    Event observed;
    Meeting meeting(2);
    std::atomic<int> failures{0};
    neomcp::Server server(named("prompts"));
    server.prompt("wait", "d", [&](neomcp::Context& context) {
        entered.set();
        Event never;
        static_cast<void>(never.wait(context.stop_token()));
        if (context.cancelled()) observed.set();
        return std::string("late");
    });
    server.prompt("meet-a", "d", [&] {
        if (!meeting.arrive(patience)) ++failures;
        return std::string("a");
    });
    server.prompt<ft::TwoArgs>("meet-b", "d", [&](const ft::TwoArgs&) -> asio::awaitable<std::string> {
        if (!meeting.arrive(patience)) ++failures;
        co_return std::string("b");
    });
    Client client(server, modern_revision);
    client.peer.notify(client.message("c1", "prompts/get", json{{"name", "wait"}}));
    ASSERT_TRUE(entered.wait(patience));
    client.peer.notify(cancelled("c1"));
    EXPECT_TRUE(observed.wait(patience));

    client.peer.notify(client.message("a", "prompts/get", json{{"name", "meet-a"}}));
    client.peer.notify(client.message("b", "prompts/get",
                                      json{{"name", "meet-b"}, {"arguments", {{"arg1", "1"}, {"arg2", "2"}}}}));
    ASSERT_TRUE(client.peer.await_response("a", patience));
    ASSERT_TRUE(client.peer.await_response("b", patience));
    EXPECT_EQ(failures.load(), 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Completion

// Whether a revision's ServerCapabilities has `completions`: the schemas of 2025-03-26 and later define it.
const std::map<std::string, bool> has_completions_capability{
    {"2024-11-05", false}, {"2025-03-26", true}, {"2025-06-18", true}, {"2025-11-25", true}, {"2026-07-28", true},
};

json prompt_ref(const std::string& name) { return json{{"type", "ref/prompt"}, {"name", name}}; }
json resource_ref(const std::string& uri) { return json{{"type", "ref/resource"}, {"uri", uri}}; }

json complete_params(json ref, const std::string& argument, const std::string& value, json context = nullptr) {
    json params = json{{"ref", std::move(ref)}, {"argument", {{"name", argument}, {"value", value}}}};
    if (!context.is_null()) params["context"] = std::move(context);
    return params;
}

std::vector<std::string> values_of(const json& answer) {
    std::vector<std::string> out;
    for (const auto& value : *result_of(answer).at("completion").at("values").get_ptr<const json::array_t*>()) {
        out.push_back(value.get<std::string>());
    }
    return out;
}

TEST(Completion, HandlerFormsAreCalledWithWhatWasTyped) {
    neomcp::Server server(named("completion"));
    server.prompt<ft::TwoArgs>("two", "d", [](const ft::TwoArgs&) { return std::string(); })
        .complete("arg1", [](std::string_view typed) { return std::vector<std::string>{std::string(typed) + "1"}; })
        .complete("arg2", [](const std::string& typed, const std::map<std::string, std::string>& resolved) {
            return std::vector<std::string>{typed, resolved.count("arg1") != 0 ? resolved.at("arg1") : "none"};
        });
    server.prompt<ft::ReviewArgs>("review", "d", [](const ft::ReviewArgs&) { return std::string(); })
        .complete("title", [](std::string typed, neomcp::Context& context) {
            return std::vector<std::string>{typed, std::string(context.revision())};
        })
        .complete("focus", [](std::string_view typed, const std::map<std::string, std::string>&,
                              neomcp::Context&) -> asio::awaitable<neomcp::CompletionResult> {
            neomcp::CompletionResult result;
            result.values = {std::string(typed) + "!"};
            result.total = 10;
            result.has_more = true;
            co_return result;
        });

    Client client(server, modern_revision);
    const auto complete = [&](json params) { return client.ask("completion/complete", std::move(params)); };

    EXPECT_EQ(values_of(complete(complete_params(prompt_ref("two"), "arg1", "ty"))), std::vector<std::string>{"ty1"});
    EXPECT_EQ(values_of(complete(complete_params(prompt_ref("two"), "arg2", "v", json{{"arguments", {{"arg1", "x"}}}}))),
              (std::vector<std::string>{"v", "x"}));
    EXPECT_EQ(values_of(complete(complete_params(prompt_ref("two"), "arg2", "v"))), (std::vector<std::string>{"v", "none"}));
    EXPECT_EQ(values_of(complete(complete_params(prompt_ref("review"), "title", "t"))),
              (std::vector<std::string>{"t", modern_revision}));
    const json built = complete(complete_params(prompt_ref("review"), "focus", "f"));
    EXPECT_EQ(values_of(built), std::vector<std::string>{"f!"});
    EXPECT_EQ(result_of(built).at("completion").at("total"), 10);
    EXPECT_EQ(result_of(built).at("completion").at("hasMore"), true);
}

TEST_P(PerRevision, ATemplateVariableIsCompletedThroughItsUriTemplate) {
    neomcp::Server server(named("completion"));
    server.resource_template<ft::KindParams>("db://{kind}/{name}", "T", [](const ft::KindParams&) { return std::string(); })
        .complete("name", [](std::string_view typed, const std::map<std::string, std::string>& resolved) {
            return std::vector<std::string>{std::string(typed) + "-" + (resolved.count("kind") ? resolved.at("kind") : "?")};
        });
    server.resource("db://fixed", "F", [] { return std::string(); });
    Client client(server, GetParam());
    const auto complete = [&](json params) { return client.ask("completion/complete", std::move(params)); };

    EXPECT_EQ(values_of(complete(complete_params(resource_ref("db://{kind}/{name}"), "name", "ab", json{{"arguments", {{"kind", "k"}}}}))),
              std::vector<std::string>{"ab-k"});
    EXPECT_EQ(values_of(complete(complete_params(resource_ref("db://{kind}/{name}"), "kind", "x"))),
              std::vector<std::string>{}) << "a variable without a completer";
    EXPECT_EQ(values_of(complete(complete_params(resource_ref("db://fixed"), "any", "x"))), std::vector<std::string>{})
        << "a resource at a fixed URI has nothing to complete";
    EXPECT_EQ(error_code_of(complete(complete_params(resource_ref("db://other/{x}"), "x", "y"))), -32602);
}

TEST_P(PerRevision, CompleteResultsHaveExactlyTheRevisionsMembers) {
    neomcp::Server server(named("completion"));
    server.prompt<ft::TwoArgs>("two", "d", [](const ft::TwoArgs&) { return std::string(); })
        .complete("arg1", [](std::string_view) { return std::vector<std::string>{"a", "b"}; });
    Client client(server, GetParam());
    const json answer = client.ask("completion/complete", complete_params(prompt_ref("two"), "arg1", "x"));
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    expect_conforms(result_of(answer), "CompleteResult", *client.revision);
    EXPECT_EQ(result_of(answer).at("completion"), (json{{"values", {"a", "b"}}}));
    EXPECT_EQ(result_of(answer).contains("resultType"), GetParam() == modern_revision);
}

TEST_P(PerRevision, AtMostAHundredValuesAreSentAndTheRestIsSaid) {
    neomcp::Server server(named("completion"));
    const auto many = [](std::size_t count) {
        std::vector<std::string> values;
        for (std::size_t i = 0; i < count; ++i) values.push_back("v" + std::to_string(i));
        return values;
    };
    server.prompt<ft::TwoArgs>("limit", "d", [](const ft::TwoArgs&) { return std::string(); })
        .complete("arg1", [&](std::string_view) { return many(150); })
        .complete("arg2", [&](std::string_view) { return many(100); });
    server.prompt<ft::ReviewArgs>("limit2", "d", [](const ft::ReviewArgs&) { return std::string(); })
        .complete("title", [&](std::string_view) {
            neomcp::CompletionResult result;
            result.values = many(120);
            result.total = 500;
            result.has_more = false;  // the cut says otherwise
            return result;
        })
        .complete("focus", [&](std::string_view) {
            neomcp::CompletionResult result;
            result.values = many(3);
            result.has_more = false;
            return result;
        });
    Client client(server, GetParam());
    const auto completion = [&](const std::string& prompt, const std::string& argument) {
        const json answer = client.ask("completion/complete", complete_params(prompt_ref(prompt), argument, ""));
        EXPECT_TRUE(answer.contains("result")) << answer.dump();
        return result_of(answer).at("completion");
    };

    const json cut = completion("limit", "arg1");
    EXPECT_EQ(cut.at("values").size(), neomcp::max_completion_values);
    EXPECT_EQ(cut.at("values").at(99), "v99");
    EXPECT_EQ(cut.at("total"), 150);
    EXPECT_EQ(cut.at("hasMore"), true);

    const json exact = completion("limit", "arg2");
    EXPECT_EQ(exact.at("values").size(), 100u);
    EXPECT_FALSE(exact.contains("total"));
    EXPECT_FALSE(exact.contains("hasMore"));

    const json stated = completion("limit2", "title");
    EXPECT_EQ(stated.at("values").size(), 100u);
    EXPECT_EQ(stated.at("total"), 500) << "a total the handler stated is kept";
    EXPECT_EQ(stated.at("hasMore"), true);

    const json small = completion("limit2", "focus");
    EXPECT_EQ(small.at("values").size(), 3u);
    EXPECT_EQ(small.at("hasMore"), false);
}

TEST_P(PerRevision, ARequestWithoutACompleterOrWithABadReferenceIsAnsweredAsThePagesSay) {
    neomcp::Server server(named("completion"));
    server.prompt<ft::TwoArgs>("two", "d", [](const ft::TwoArgs&) { return std::string(); })
        .complete("arg1", [](std::string_view) { return std::vector<std::string>{"a"}; });
    Client client(server, GetParam());
    const auto complete = [&](json params) { return client.ask("completion/complete", std::move(params)); };

    // A reference with no completer for the argument: an empty list.
    EXPECT_EQ(values_of(complete(complete_params(prompt_ref("two"), "arg2", "x"))), std::vector<std::string>{});
    EXPECT_EQ(values_of(complete(complete_params(prompt_ref("two"), "nonexistent", "x"))), std::vector<std::string>{});
    // A reference to something the server lacks: Invalid params.
    EXPECT_EQ(error_code_of(complete(complete_params(prompt_ref("nonexistent"), "arg1", "x"))), -32602);
    EXPECT_EQ(error_code_of(complete(complete_params(json{{"type", "ref/other"}, {"name", "two"}}, "arg1", "x"))), -32602);
    EXPECT_EQ(error_code_of(complete(complete_params(json{{"type", "ref/prompt"}}, "arg1", "x"))), -32602);
    EXPECT_EQ(error_code_of(complete(complete_params(json{{"type", "ref/resource"}, {"name", "x"}}, "arg1", "x"))), -32602);
    EXPECT_EQ(error_code_of(complete(json{{"ref", prompt_ref("two")}})), -32602);
    EXPECT_EQ(error_code_of(complete(json{{"ref", prompt_ref("two")}, {"argument", {{"name", "arg1"}}}})), -32602);
    EXPECT_EQ(error_code_of(complete(json{{"ref", prompt_ref("two")}, {"argument", {{"name", "arg1"}, {"value", 3}}}})), -32602);
    EXPECT_EQ(error_code_of(complete(complete_params(prompt_ref("two"), "arg1", "x", json{{"arguments", {{"k", 1}}}}))), -32602)
        << "resolved arguments are strings";
    EXPECT_EQ(error_code_of(complete(complete_params(prompt_ref("two"), "arg1", "x", json{{"arguments", json::array()}}))), -32602);
    EXPECT_EQ(error_code_of(complete(complete_params(prompt_ref("two"), "arg1", "x", json::array()))), -32602);
    EXPECT_EQ(values_of(complete(complete_params(prompt_ref("two"), "arg1", "x", json{{"arguments", {{"k", "v"}}}}))),
              std::vector<std::string>{"a"});
}

TEST_P(PerRevision, TheCompletionsCapabilityIsDeclaredExactlyWhenACompleterIsRegistered) {
    const std::string revision = GetParam();
    ASSERT_EQ(has_completions_capability.size(), profile::builtin_revisions().size());
    {
        neomcp::Server server(named("none"));
        server.prompt<ft::TwoArgs>("two", "d", [](const ft::TwoArgs&) { return std::string(); });
        Client client(server, revision);
        EXPECT_FALSE(client.capabilities().contains("completions")) << "a prompt alone declares none";
        EXPECT_EQ(error_code_of(client.ask("completion/complete", complete_params(prompt_ref("two"), "arg1", "x"))), -32601);
    }
    {
        neomcp::Server server(named("some"));
        server.prompt<ft::TwoArgs>("two", "d", [](const ft::TwoArgs&) { return std::string(); })
            .complete("arg1", [](std::string_view) { return std::vector<std::string>{}; });
        Client client(server, revision);
        // The schema of 2024-11-05 has no `completions` member, so it is not written there; the method is served.
        EXPECT_EQ(client.capabilities().contains("completions"), has_completions_capability.at(revision))
            << client.capabilities().dump();
        EXPECT_TRUE(client.ask("completion/complete", complete_params(prompt_ref("two"), "arg1", "x")).contains("result"));
    }
}

TEST(Completion, RegistrationRefusalsNameTheArgument) {
    neomcp::Server server(named("completion"));
    const auto prompt = server.prompt<ft::TwoArgs>("two", "d", [](const ft::TwoArgs&) { return std::string(); });
    const auto tpl = server.resource_template<ft::FileParams>("f://{path}", "T", [](const ft::FileParams&) { return std::string(); });
    const auto nothing = [](std::string_view) { return std::vector<std::string>{}; };
    const auto refused = [&](auto registration, const std::string& part) {
        try {
            registration();
            ADD_FAILURE() << "expected a refusal naming " << part;
        } catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find(part), std::string::npos) << error.what();
        }
    };
    refused([&] { prompt.complete("nonexistent", nothing); }, "'nonexistent'");
    refused([&] { tpl.complete("nonexistent", nothing); }, "'nonexistent'");
    prompt.complete("arg1", nothing);
    tpl.complete("path", nothing);
    refused([&] { prompt.complete("arg1", nothing); }, "'arg1'");
    refused([&] { tpl.complete("path", nothing); }, "'path'");

    Client client(server, modern_revision);
    static_cast<void>(client.ask("prompts/list"));
    EXPECT_THROW(prompt.complete("arg2", nothing), std::logic_error);
    EXPECT_THROW(tpl.complete("path", nothing), std::logic_error);
}

TEST(Completion, ACancelledCompleterSeesStopAndTwoRunAtOnce) {
    Event entered;
    Event observed;
    Meeting meeting(2);
    std::atomic<int> failures{0};
    neomcp::Server server(named("completion"));
    server.prompt<ft::TwoArgs>("two", "d", [](const ft::TwoArgs&) { return std::string(); })
        .complete("arg1",
                  [&](std::string_view, const std::map<std::string, std::string>&, neomcp::Context& context) {
                      entered.set();
                      Event never;
                      static_cast<void>(never.wait(context.stop_token()));
                      if (context.cancelled()) observed.set();
                      return std::vector<std::string>{};
                  })
        .complete("arg2", [&](std::string_view) {
            if (!meeting.arrive(patience)) ++failures;
            return std::vector<std::string>{"x"};
        });
    server.resource_template<ft::FileParams>("f://{path}", "T", [](const ft::FileParams&) { return std::string(); })
        .complete("path", [&](std::string_view) -> asio::awaitable<std::vector<std::string>> {
            if (!meeting.arrive(patience)) ++failures;
            co_return std::vector<std::string>{"y"};
        });
    Client client(server, modern_revision);
    client.peer.notify(client.message("c1", "completion/complete", complete_params(prompt_ref("two"), "arg1", "")));
    ASSERT_TRUE(entered.wait(patience));
    client.peer.notify(cancelled("c1"));
    EXPECT_TRUE(observed.wait(patience));

    client.peer.notify(client.message("a", "completion/complete", complete_params(prompt_ref("two"), "arg2", "")));
    client.peer.notify(client.message("b", "completion/complete", complete_params(resource_ref("f://{path}"), "path", "")));
    ASSERT_TRUE(client.peer.await_response("a", patience));
    ASSERT_TRUE(client.peer.await_response("b", patience));
    EXPECT_EQ(failures.load(), 0);
}

}  // namespace
