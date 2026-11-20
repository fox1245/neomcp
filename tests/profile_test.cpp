#include "neomcp/profile.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <neojson/json.hpp>

namespace {

using neojson::json;
using namespace neomcp::profile;

// Fixture revisions the values below were read from.
constexpr std::string_view modern_revision = "2026-07-28";
constexpr std::string_view batch_revision = "2025-03-26";
constexpr std::string_view sse_revision = "2024-11-05";
constexpr std::string_view last_legacy_revision = "2025-11-25";

ProfilePtr require_builtin(std::string_view revision) {
    ProfilePtr profile = builtin(revision);
    EXPECT_NE(profile, nullptr) << "no built-in profile " << revision;
    return profile;
}

json builtin_document(std::string_view revision) {
    return json::parse(builtin_text(revision));
}

// Loads a document that is expected to be rejected and returns the error.
Error rejected(const json& document) {
    auto result = load(document.dump());
    auto* error = std::get_if<Error>(&result);
    EXPECT_NE(error, nullptr) << "the document was accepted";
    return error != nullptr ? *error : Error{};
}

json& request_headers(json& document) {
    return document["transports"]["streamable_http"]["request_headers"];
}

std::size_t header_index(json& document, const std::string& name) {
    json& headers = request_headers(document);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (headers[i]["name"].get<std::string>() == name) return i;
    }
    ADD_FAILURE() << "no request header " << name;
    return 0;
}

// An RFC 6901 reference token for a member name.
std::string pointer_token(const std::string& name) {
    std::string token;
    for (const char c : name) {
        if (c == '~') {
            token += "~0";
        } else if (c == '/') {
            token += "~1";
        } else {
            token += c;
        }
    }
    return token;
}

std::string header_pointer(std::size_t index) {
    return "/transports/streamable_http/request_headers/" + std::to_string(index);
}

TEST(Builtin, EveryRevisionLoads) {
    const auto revisions = builtin_revisions();
    ASSERT_FALSE(revisions.empty());
    EXPECT_TRUE(std::is_sorted(revisions.begin(), revisions.end()));
    for (const auto revision : revisions) {
        SCOPED_TRACE(std::string(revision));
        ASSERT_FALSE(builtin_text(revision).empty());
        const ProfilePtr profile = require_builtin(revision);
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile->revision(), revision);
        EXPECT_EQ(builtin(revision), profile) << "a built-in profile is loaded once";

        auto result = load(builtin_text(revision));
        EXPECT_TRUE(std::holds_alternative<ProfilePtr>(result));
    }
}

TEST(Builtin, ConcurrentFirstUseSharesOneProfile) {
    const auto revisions = builtin_revisions();
    ASSERT_FALSE(revisions.empty());
    const std::string_view revision = revisions.front();
    std::vector<ProfilePtr> seen(8);
    std::vector<std::thread> threads;
    for (auto& slot : seen) threads.emplace_back([&slot, revision] { slot = builtin(revision); });
    for (auto& thread : threads) thread.join();
    for (const auto& profile : seen) {
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile, seen.front());
    }
}

TEST(Builtin, UnknownRevision) {
    EXPECT_EQ(builtin("no-such-revision"), nullptr);
    EXPECT_TRUE(builtin_text("no-such-revision").empty());
}

TEST(Values, ModernRevision) {
    const ProfilePtr profile = require_builtin(modern_revision);
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->era(), Era::modern);
    EXPECT_EQ(profile->mechanism(Slot::open), "request_meta");
    EXPECT_EQ(profile->mechanism(Slot::state), "none");
    EXPECT_EQ(profile->mechanism(Slot::server_input), "input_required_result");
    EXPECT_EQ(profile->mechanism(Slot::change_feed), "subscriptions_listen");
    EXPECT_EQ(profile->mechanism(Slot::batch), "none");

    ASSERT_TRUE(profile->stdio());
    EXPECT_EQ(profile->stdio()->cancel, "cancelled_notification");
    EXPECT_FALSE(profile->http_sse());

    ASSERT_TRUE(profile->streamable_http());
    const StreamableHttpTransport& http = *profile->streamable_http();
    EXPECT_EQ(http.cancel, "close_response_stream");
    EXPECT_FALSE(http.session_header);
    EXPECT_FALSE(http.version_header);
    EXPECT_FALSE(http.get_stream);
    EXPECT_FALSE(http.resumable);
    EXPECT_FALSE(http.delete_ends_session);
    ASSERT_TRUE(http.argument_headers);
    EXPECT_EQ(http.argument_headers->schema_keyword, "x-mcp-header");
    EXPECT_EQ(http.argument_headers->header_prefix, "Mcp-Param-");
    ASSERT_TRUE(http.encoded_header_value);
    EXPECT_EQ(http.encoded_header_value->prefix, "=?base64?");
    EXPECT_EQ(http.encoded_header_value->suffix, "?=");

    ASSERT_EQ(http.request_headers.size(), 3u);
    const RequestHeader& version = http.request_headers[0];
    EXPECT_EQ(version.name, "MCP-Protocol-Version");
    EXPECT_EQ(version.value, HeaderValue::protocol_version);
    EXPECT_EQ(version.scope, HeaderScopeKind::every_message);
    const RequestHeader& method = http.request_headers[1];
    EXPECT_EQ(method.name, "Mcp-Method");
    EXPECT_EQ(method.value, HeaderValue::method);
    EXPECT_EQ(method.scope, HeaderScopeKind::every_request);
    const RequestHeader& name = http.request_headers[2];
    EXPECT_EQ(name.name, "Mcp-Name");
    EXPECT_EQ(name.value, HeaderValue::params_member);
    EXPECT_EQ(name.scope, HeaderScopeKind::per_method);
    ASSERT_EQ(name.methods.size(), 3u);
    EXPECT_EQ(name.params_member("tools/call"), "name");
    EXPECT_EQ(name.params_member("prompts/get"), "name");
    EXPECT_EQ(name.params_member("resources/read"), "uri");
    EXPECT_FALSE(name.params_member("ping"));
    EXPECT_FALSE(method.params_member("tools/call"));

    EXPECT_EQ(profile->error_code("header_mismatch"), -32020);
    EXPECT_EQ(profile->error_code("missing_required_client_capability"), -32021);
    EXPECT_EQ(profile->error_code("unsupported_protocol_version"), -32022);
    EXPECT_FALSE(profile->error_code("not_emitted"));
    EXPECT_FALSE(profile->error_code("no_such_error"));
    ASSERT_EQ(profile->not_emitted().size(), 2u);
    EXPECT_EQ(profile->not_emitted()[0], -32002);
    EXPECT_EQ(profile->not_emitted()[1], -32042);
}

TEST(Values, RevisionWithBatchAndSession) {
    const ProfilePtr profile = require_builtin(batch_revision);
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->era(), Era::legacy);
    EXPECT_EQ(profile->mechanism(Slot::batch), "array");
    EXPECT_EQ(profile->mechanism(Slot::open), "initialize");
    ASSERT_TRUE(profile->streamable_http());
    const StreamableHttpTransport& http = *profile->streamable_http();
    EXPECT_EQ(http.session_header, "Mcp-Session-Id");
    EXPECT_FALSE(http.version_header);
    EXPECT_TRUE(http.get_stream);
    EXPECT_TRUE(http.resumable);
    EXPECT_TRUE(http.delete_ends_session);
    EXPECT_TRUE(http.request_headers.empty());
    EXPECT_TRUE(profile->not_emitted().empty());
    EXPECT_EQ(profile->error_code("resource_not_found"), -32002);
}

TEST(Values, RevisionWithServerSentEvents) {
    const ProfilePtr profile = require_builtin(sse_revision);
    ASSERT_NE(profile, nullptr);
    ASSERT_TRUE(profile->http_sse());
    EXPECT_EQ(profile->http_sse()->cancel, "cancelled_notification");
    // The event types of basic/transports.mdx of that revision.
    EXPECT_EQ(profile->http_sse()->endpoint_event, "endpoint");
    EXPECT_EQ(profile->http_sse()->message_event, "message");
    EXPECT_FALSE(profile->streamable_http());
    ASSERT_TRUE(profile->stdio());
}

TEST(Values, VersionHeaderOfALaterLegacyRevision) {
    const ProfilePtr profile = require_builtin(last_legacy_revision);
    ASSERT_NE(profile, nullptr);
    ASSERT_TRUE(profile->streamable_http());
    ASSERT_TRUE(profile->streamable_http()->version_header);
    EXPECT_EQ(profile->streamable_http()->version_header->name, "MCP-Protocol-Version");
    EXPECT_EQ(profile->streamable_http()->version_header->absent_means, batch_revision);
}

TEST(Wire, PingExistsInTheLastLegacyRevisionFromBothSides) {
    const ProfilePtr legacy = require_builtin(last_legacy_revision);
    const ProfilePtr modern = require_builtin(modern_revision);
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(modern, nullptr);
    const Message* ping = legacy->request("ping");
    ASSERT_NE(ping, nullptr);
    EXPECT_EQ(ping->method, "ping");
    EXPECT_TRUE(ping->from_client);
    EXPECT_TRUE(ping->from_server);
    EXPECT_EQ(modern->request("ping"), nullptr);
    EXPECT_EQ(modern->notification("ping"), nullptr);
}

TEST(Wire, CallToolResultDiffersBetweenRevisions) {
    const ProfilePtr legacy = require_builtin(last_legacy_revision);
    const ProfilePtr modern = require_builtin(modern_revision);
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(modern, nullptr);
    const Type* now = modern->type("CallToolResult");
    ASSERT_NE(now, nullptr);
    EXPECT_TRUE(now->is_required("resultType"));
    EXPECT_TRUE(now->has_property("resultType"));
    EXPECT_TRUE(now->is_required("content"));
    EXPECT_FALSE(now->is_required("isError"));
    EXPECT_TRUE(now->has_property("isError"));
    const Type* before = legacy->type("CallToolResult");
    ASSERT_NE(before, nullptr);
    EXPECT_FALSE(before->has_property("resultType"));
    EXPECT_FALSE(before->is_required("resultType"));
    EXPECT_EQ(modern->type("NoSuchType"), nullptr);
}

TEST(Wire, InputRequestsExistOnlyInTheModernRevision) {
    for (const auto revision : builtin_revisions()) {
        SCOPED_TRACE(std::string(revision));
        const ProfilePtr profile = require_builtin(revision);
        ASSERT_NE(profile, nullptr);
        const bool modern = revision == modern_revision;
        EXPECT_EQ(profile->input_request("elicitation/create") != nullptr, modern);
        EXPECT_EQ(profile->input_requests().empty(), !modern);
    }
}

TEST(Wire, UnionParamsAndComposedTypes) {
    const ProfilePtr modern = require_builtin(modern_revision);
    const ProfilePtr legacy = require_builtin(last_legacy_revision);
    ASSERT_NE(modern, nullptr);
    ASSERT_NE(legacy, nullptr);

    const InputRequest* elicit = modern->input_request("elicitation/create");
    ASSERT_NE(elicit, nullptr);
    ASSERT_TRUE(elicit->params);
    EXPECT_EQ(*elicit->params, "ElicitRequestParams");
    EXPECT_EQ(elicit->params_required, std::vector<std::string>{"message"});
    EXPECT_EQ(modern->type("ElicitRequestParams"), nullptr);
    const Union* params = modern->union_type("ElicitRequestParams");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->name, "ElicitRequestParams");
    ASSERT_EQ(params->alternatives.size(), 2u);
    for (const auto& alternative : params->alternatives) {
        const Type* type = modern->type(alternative);
        ASSERT_NE(type, nullptr) << alternative;
        EXPECT_TRUE(type->is_required("message"));
    }
    EXPECT_EQ(modern->union_type("CallToolResult"), nullptr);
    EXPECT_EQ(modern->union_type("NoSuchUnion"), nullptr);

    const Message* status = legacy->notification("notifications/tasks/status");
    ASSERT_NE(status, nullptr);
    ASSERT_TRUE(status->params);
    EXPECT_EQ(*status->params, "TaskStatusNotificationParams");
    const Type* type = legacy->type("TaskStatusNotificationParams");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(legacy->union_type("TaskStatusNotificationParams"), nullptr);
    EXPECT_EQ(status->params_required, type->required);
    EXPECT_TRUE(type->is_required("taskId"));
    EXPECT_TRUE(type->is_required("status"));

    for (const auto revision : builtin_revisions()) {
        const ProfilePtr profile = require_builtin(revision);
        ASSERT_NE(profile, nullptr);
        for (const auto& entry : profile->unions()) {
            EXPECT_EQ(profile->union_type(entry.name), &entry);
            EXPECT_EQ(profile->type(entry.name), nullptr) << entry.name;
            EXPECT_FALSE(entry.alternatives.empty());
        }
        EXPECT_TRUE(std::is_sorted(profile->unions().begin(), profile->unions().end(),
                                   [](const Union& a, const Union& b) { return a.name < b.name; }));
    }
}

TEST(Wire, TablesAreSortedAndLookupsAgreeWithThem) {
    for (const auto revision : builtin_revisions()) {
        SCOPED_TRACE(std::string(revision));
        const ProfilePtr profile = require_builtin(revision);
        ASSERT_NE(profile, nullptr);
        for (const auto& message : profile->requests()) {
            EXPECT_EQ(profile->request(message.method), &message);
            EXPECT_TRUE(message.from_client || message.from_server);
        }
        for (const auto& message : profile->notifications()) {
            EXPECT_EQ(profile->notification(message.method), &message);
        }
        for (const auto& type : profile->types()) {
            EXPECT_EQ(profile->type(type.name), &type);
            EXPECT_TRUE(std::is_sorted(type.properties.begin(), type.properties.end()));
            for (const auto& member : type.required) EXPECT_TRUE(type.has_property(member));
        }
        const auto sorted_by_method = [](const auto& table) {
            return std::is_sorted(table.begin(), table.end(),
                                  [](const auto& a, const auto& b) { return a.method < b.method; });
        };
        EXPECT_TRUE(sorted_by_method(profile->requests()));
        EXPECT_TRUE(sorted_by_method(profile->notifications()));
        EXPECT_TRUE(sorted_by_method(profile->input_requests()));
        EXPECT_TRUE(std::is_sorted(profile->types().begin(), profile->types().end(),
                                   [](const Type& a, const Type& b) { return a.name < b.name; }));
        EXPECT_FALSE(profile->wire_schema().sha256.empty());
        EXPECT_FALSE(profile->wire_schema().dialect.empty());
    }
}

TEST(Rejects, EveryTopLevelMemberRemovedInTurn) {
    const json original = builtin_document(modern_revision);
    std::vector<std::string> names;
    for (const auto& item : original.items()) names.push_back(item.key());
    ASSERT_FALSE(names.empty());
    for (const auto& name : names) {
        SCOPED_TRACE(name);
        json document = original;
        document.erase(name);
        EXPECT_EQ(rejected(document).pointer, "/" + name);
    }
}

TEST(Rejects, UndeclaredMechanismName) {
    json document = builtin_document(modern_revision);
    document["mechanisms"]["open"] = "telepathy";
    const Error error = rejected(document);
    EXPECT_EQ(error.pointer, "/mechanisms/open");
    EXPECT_NE(error.expected.find("initialize"), std::string::npos) << error.expected;
    EXPECT_NE(error.expected.find("request_meta"), std::string::npos) << error.expected;
}

// The expected values are stated here from the error-handling section of each revision's server/tools.mdx, not
// read back from the profiles: tool arguments that fail the input schema are a protocol error up to 2025-06-18
// and a tool execution error from 2025-11-25.
TEST(Values, InvalidArgumentsPerRevision) {
    const std::vector<std::pair<std::string_view, std::string_view>> expected{
        {"2024-11-05", "invalid_params_error"},
        {"2025-03-26", "invalid_params_error"},
        {"2025-06-18", "invalid_params_error"},
        {"2025-11-25", "error_result"},
        {"2026-07-28", "error_result"},
    };
    ASSERT_EQ(builtin_revisions().size(), expected.size()) << "a new revision needs its expected value here";
    for (const auto& [revision, name] : expected) {
        SCOPED_TRACE(std::string(revision));
        const ProfilePtr profile = require_builtin(revision);
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile->invalid_arguments(), name);
        const auto declared = declared_invalid_arguments();
        EXPECT_NE(std::find(declared.begin(), declared.end(), profile->invalid_arguments()), declared.end());
        const auto& pages = profile->evidence().pages;
        const std::string page = "docs/specification/" + std::string(revision) + "/server/tools.mdx";
        EXPECT_NE(std::find(pages.begin(), pages.end(), page), pages.end()) << "the evidence names " << page;
    }
}

TEST(Values, DeclaredNamesAreDistinctAndNonEmpty) {
    const auto check = [](std::span<const std::string_view> names) {
        EXPECT_FALSE(names.empty());
        std::vector<std::string_view> sorted(names.begin(), names.end());
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());
        for (const auto name : names) EXPECT_FALSE(name.empty());
    };
    for (std::size_t i = 0; i < slot_count; ++i) check(declared_mechanisms(static_cast<Slot>(i)));
    for (const Transport transport : {Transport::stdio, Transport::http_sse, Transport::streamable_http}) {
        check(declared_cancel(transport));
    }
    check(declared_invalid_arguments());
    check(declared_status_reasons());
}

TEST(Values, TransportNamesAreTheDocumentsMembers) {
    EXPECT_EQ(to_string(Transport::stdio), "stdio");
    EXPECT_EQ(to_string(Transport::http_sse), "http_sse");
    EXPECT_EQ(to_string(Transport::streamable_http), "streamable_http");
}

// The statuses each revision prescribes, stated here from the pages and not read back from the profiles:
// basic/transports.mdx of the legacy revisions; basic/transports/streamable-http.mdx, basic/index.mdx and
// basic/versioning.mdx of 2026-07-28. A cell the pages leave open is absent.
TEST(Values, StatusesPerRevision) {
    using Table = std::vector<std::pair<std::string, int>>;
    const std::vector<std::tuple<std::string_view, Table, std::vector<std::string>>> expected{
        {"2025-03-26",
         {{"accepted", 202}, {"delete_not_allowed", 405}, {"get_stream_not_offered", 405}, {"missing_session", 400},
          {"notification_refused", 400}, {"unknown_session", 404}},
         {"basic/transports.mdx"}},
        {"2025-06-18",
         {{"accepted", 202}, {"delete_not_allowed", 405}, {"get_stream_not_offered", 405}, {"missing_session", 400},
          {"notification_refused", 400}, {"unknown_session", 404}, {"unsupported_version_header", 400}},
         {"basic/transports.mdx"}},
        {"2025-11-25",
         {{"accepted", 202}, {"delete_not_allowed", 405}, {"get_stream_not_offered", 405}, {"missing_session", 400},
          {"notification_refused", 400}, {"origin_rejected", 403}, {"unknown_session", 404},
          {"unsupported_version_header", 400}},
         {"basic/transports.mdx"}},
        {"2026-07-28",
         {{"accepted", 202}, {"delete_not_allowed", 405}, {"get_stream_not_offered", 405}, {"header_mismatch", 400},
          {"invalid_request_meta", 400}, {"method_not_found", 404}, {"missing_client_capability", 400},
          {"notification_refused", 400}, {"origin_rejected", 403}, {"unsupported_version", 400}},
         {"basic/transports/streamable-http.mdx", "basic/index.mdx", "basic/versioning.mdx"}},
    };
    std::size_t with_transport = 0;
    for (const auto revision : builtin_revisions()) {
        if (require_builtin(revision)->streamable_http()) ++with_transport;
    }
    ASSERT_EQ(with_transport, expected.size()) << "a new revision needs its expected statuses here";
    const auto reasons = declared_status_reasons();
    for (const auto& [revision, table, pages] : expected) {
        SCOPED_TRACE(std::string(revision));
        const ProfilePtr profile = require_builtin(revision);
        ASSERT_NE(profile, nullptr);
        ASSERT_TRUE(profile->streamable_http());
        const StreamableHttpTransport& http = *profile->streamable_http();
        Table found;
        for (const auto& entry : http.status) found.emplace_back(entry.reason, entry.status);
        EXPECT_EQ(found, table);
        for (const auto& [reason, status] : table) {
            EXPECT_EQ(http.status_for(reason), status) << reason;
            EXPECT_NE(std::find(reasons.begin(), reasons.end(), reason), reasons.end()) << reason;
        }
        EXPECT_FALSE(http.status_for("parse_error")) << "no revision prescribes a status for it";
        EXPECT_FALSE(http.status_for("no_such_reason"));
        const auto& listed = profile->evidence().pages;
        for (const auto& page : pages) {
            const std::string path = "docs/specification/" + std::string(revision) + "/" + page;
            EXPECT_NE(std::find(listed.begin(), listed.end(), path), listed.end()) << "the evidence names " << path;
        }
    }
}

TEST(Values, StatusMapIsOptional) {
    json document = builtin_document(modern_revision);
    document["transports"]["streamable_http"].erase("status");
    auto result = load(document.dump());
    ASSERT_TRUE(std::holds_alternative<ProfilePtr>(result));
    const ProfilePtr profile = std::get<ProfilePtr>(result);
    ASSERT_TRUE(profile->streamable_http());
    EXPECT_TRUE(profile->streamable_http()->status.empty());
    EXPECT_FALSE(profile->streamable_http()->status_for("header_mismatch"));
}

TEST(Rejects, StatusForAnUndeclaredReason) {
    json document = builtin_document(modern_revision);
    document["transports"]["streamable_http"]["status"]["teapot"] = 418;
    const Error error = rejected(document);
    EXPECT_EQ(error.pointer, "/transports/streamable_http/status/teapot");
    EXPECT_NE(error.expected.find("header_mismatch"), std::string::npos) << error.expected;
}

TEST(Rejects, StatusThatIsNotAnHttpStatus) {
    for (const json& value : {json(99), json(600), json(-400), json("400"), json(400.5), json(nullptr)}) {
        SCOPED_TRACE(value.dump());
        json document = builtin_document(modern_revision);
        document["transports"]["streamable_http"]["status"]["header_mismatch"] = value;
        EXPECT_EQ(rejected(document).pointer, "/transports/streamable_http/status/header_mismatch");
    }
    json document = builtin_document(modern_revision);
    document["transports"]["streamable_http"]["status"] = json::array();
    EXPECT_EQ(rejected(document).pointer, "/transports/streamable_http/status");
}

TEST(Rejects, UndeclaredInvalidArgumentsName) {
    json document = builtin_document(modern_revision);
    document["tools"]["invalid_arguments"] = "shrug";
    const Error error = rejected(document);
    EXPECT_EQ(error.pointer, "/tools/invalid_arguments");
    EXPECT_NE(error.expected.find("error_result"), std::string::npos) << error.expected;
    EXPECT_NE(error.expected.find("invalid_params_error"), std::string::npos) << error.expected;
}

TEST(Rejects, InvalidArgumentsRemoved) {
    json document = builtin_document(modern_revision);
    document["tools"].erase("invalid_arguments");
    EXPECT_EQ(rejected(document).pointer, "/tools/invalid_arguments");
}

TEST(Rejects, InvalidArgumentsThatIsNotAString) {
    json document = builtin_document(sse_revision);
    document["tools"]["invalid_arguments"] = true;
    EXPECT_EQ(rejected(document).pointer, "/tools/invalid_arguments");
}

TEST(Rejects, ExtraMemberInTools) {
    json document = builtin_document(modern_revision);
    document["tools"]["extra"] = "error_result";
    EXPECT_EQ(rejected(document).pointer, "/tools/extra");
}

TEST(Rejects, UndeclaredCancelName) {
    json document = builtin_document(modern_revision);
    document["transports"]["stdio"]["cancel"] = "close_response_stream";
    EXPECT_EQ(rejected(document).pointer, "/transports/stdio/cancel");
}

TEST(Rejects, EventStreamWithoutItsEventNames) {
    for (const char* name : {"endpoint_event", "message_event"}) {
        json document = builtin_document(sse_revision);
        document["transports"]["http_sse"].erase(name);
        EXPECT_EQ(rejected(document).pointer, std::string("/transports/http_sse/") + name);
    }
}

TEST(Rejects, EventStreamWithAnEmptyEventName) {
    for (const char* name : {"endpoint_event", "message_event"}) {
        json document = builtin_document(sse_revision);
        document["transports"]["http_sse"][name] = "";
        EXPECT_EQ(rejected(document).pointer, std::string("/transports/http_sse/") + name);
    }
}

TEST(Rejects, EventStreamWhoseTwoEventsShareAName) {
    json document = builtin_document(sse_revision);
    document["transports"]["http_sse"]["message_event"] = "endpoint";
    EXPECT_EQ(rejected(document).pointer, "/transports/http_sse/message_event");
}

TEST(Rejects, ExtraMemberInMechanisms) {
    json document = builtin_document(modern_revision);
    document["mechanisms"]["extra"] = "none";
    EXPECT_EQ(rejected(document).pointer, "/mechanisms/extra");
}

TEST(Rejects, UnknownMemberInANestedObject) {
    json document = builtin_document(modern_revision);
    document["evidence"]["note"] = "x";
    EXPECT_EQ(rejected(document).pointer, "/evidence/note");
}

TEST(Rejects, WrongJsonType) {
    json document = builtin_document(modern_revision);
    document["transports"]["streamable_http"]["get_stream"] = "no";
    EXPECT_EQ(rejected(document).pointer, "/transports/streamable_http/get_stream");
}

TEST(Rejects, UnsupportedProfileVersion) {
    json document = builtin_document(modern_revision);
    document["profile_version"] = 2;
    EXPECT_EQ(rejected(document).pointer, "/profile_version");
}

TEST(Rejects, ErrorCodeOutOfRange) {
    json document = builtin_document(modern_revision);
    document["errors"]["header_mismatch"] = std::uint64_t{1} << 40;
    EXPECT_EQ(rejected(document).pointer, "/errors/header_mismatch");
    document["errors"]["header_mismatch"] = -(std::int64_t{1} << 40);
    EXPECT_EQ(rejected(document).pointer, "/errors/header_mismatch");
}

TEST(Rejects, McpNameMemberThatIsNotAProperty) {
    json document = builtin_document(modern_revision);
    const std::size_t index = header_index(document, "Mcp-Name");
    request_headers(document)[index]["on"]["tools/call"] = "no_such_member";
    EXPECT_EQ(rejected(document).pointer, header_pointer(index) + "/on/tools~1call");
}

TEST(Rejects, McpNameMethodThatIsNotARequest) {
    json document = builtin_document(modern_revision);
    const std::size_t index = header_index(document, "Mcp-Name");
    request_headers(document)[index]["on"]["no/such~method"] = "name";
    EXPECT_EQ(rejected(document).pointer, header_pointer(index) + "/on/no~1such~0method");
}

TEST(Rejects, ParamsMemberHeaderOnARequestWithoutParamsType) {
    json document = builtin_document(last_legacy_revision);
    // A request of the fixture revision whose params type the schema does not name.
    std::string method;
    for (const auto& item : document["wire"]["requests"].items()) {
        if (item.value()["params"].is_null()) {
            method = item.key();
            break;
        }
    }
    ASSERT_FALSE(method.empty());
    document["transports"]["streamable_http"]["request_headers"] = json::array(
        {json{{"name", "Mcp-Name"}, {"value", "params_member"}, {"on", json{{method, "name"}}}}});
    EXPECT_EQ(rejected(document).pointer, header_pointer(0) + "/on/" + pointer_token(method));
}

TEST(Rejects, ParamsMemberHeaderWithoutAMemberTable) {
    json document = builtin_document(modern_revision);
    const std::size_t index = header_index(document, "Mcp-Name");
    request_headers(document)[index]["on"] = "every_request";
    EXPECT_EQ(rejected(document).pointer, header_pointer(index) + "/on");
}

TEST(Rejects, ErrorsValueThatIsAString) {
    json document = builtin_document(modern_revision);
    document["errors"]["header_mismatch"] = "-32020";
    EXPECT_EQ(rejected(document).pointer, "/errors/header_mismatch");
}

TEST(Rejects, NotEmittedThatIsNotAnArray) {
    json document = builtin_document(modern_revision);
    document["errors"]["not_emitted"] = -32002;
    EXPECT_EQ(rejected(document).pointer, "/errors/not_emitted");
}

TEST(Rejects, HeaderNamesThatDifferOnlyInCase) {
    json document = builtin_document(modern_revision);
    json& headers = request_headers(document);
    ASSERT_GE(headers.size(), 2u);
    std::string name = headers[0]["name"].get<std::string>();
    std::transform(name.begin(), name.end(), name.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    headers[1]["name"] = name;
    EXPECT_EQ(rejected(document).pointer, header_pointer(1) + "/name");
}

TEST(Rejects, HeaderNameThatCollidesWithTheSessionHeader) {
    json document = builtin_document(batch_revision);
    document["transports"]["streamable_http"]["request_headers"] =
        json::array({json{{"name", "mcp-session-id"}, {"value", "method"}, {"on", "every_request"}}});
    EXPECT_EQ(rejected(document).pointer, header_pointer(0) + "/name");
}

TEST(Rejects, ParamsRequiredThatDiffersFromTheTypesRequired) {
    json document = builtin_document(modern_revision);
    document["wire"]["requests"]["tools/call"]["params_required"].push_back("bogus");
    EXPECT_EQ(rejected(document).pointer, "/wire/requests/tools~1call/params_required");
}

TEST(Rejects, UnionMessageWhoseParamsRequiredHasAnExtraMember) {
    json document = builtin_document(modern_revision);
    document["wire"]["input_requests"]["elicitation/create"]["params_required"].push_back("bogus");
    EXPECT_EQ(rejected(document).pointer, "/wire/input_requests/elicitation~1create/params_required");
}

TEST(Rejects, UnionMessageWhoseParamsRequiredLacksAMember) {
    json document = builtin_document(modern_revision);
    document["wire"]["input_requests"]["elicitation/create"]["params_required"] = json::array();
    EXPECT_EQ(rejected(document).pointer, "/wire/input_requests/elicitation~1create/params_required");
}

TEST(Rejects, ParamsThatNamesNothing) {
    json document = builtin_document(modern_revision);
    document["wire"]["requests"]["tools/call"]["params"] = "NoSuchParams";
    EXPECT_EQ(rejected(document).pointer, "/wire/requests/tools~1call/params");
}

TEST(Rejects, UnionsRemoved) {
    json document = builtin_document(modern_revision);
    document["wire"].erase("unions");
    EXPECT_EQ(rejected(document).pointer, "/wire/unions");
}

TEST(Rejects, UnionAlternativeThatNamesNothing) {
    json document = builtin_document(modern_revision);
    document["wire"]["unions"]["ElicitRequestParams"].push_back("NoSuchType");
    EXPECT_EQ(rejected(document).pointer,
              "/wire/unions/ElicitRequestParams/" +
                  std::to_string(document["wire"]["unions"]["ElicitRequestParams"].size() - 1));
}

TEST(Rejects, NameInBothTypesAndUnions) {
    json document = builtin_document(modern_revision);
    document["wire"]["unions"]["CallToolResult"] = json::array({"ElicitRequestFormParams"});
    EXPECT_EQ(rejected(document).pointer, "/wire/unions/CallToolResult");
}

TEST(Rejects, UnionThatContainsItself) {
    json document = builtin_document(modern_revision);
    document["wire"]["unions"]["ElicitRequestParams"].push_back("ElicitRequestParams");
    EXPECT_EQ(rejected(document).pointer,
              "/wire/unions/ElicitRequestParams/" +
                  std::to_string(document["wire"]["unions"]["ElicitRequestParams"].size() - 1));
}

TEST(Rejects, UnionThatContainsItselfThroughAnotherUnion) {
    json document = builtin_document(modern_revision);
    document["wire"]["unions"]["Loop1"] = json::array({"Loop2"});
    document["wire"]["unions"]["Loop2"] = json::array({"Loop1"});
    const Error error = rejected(document);
    EXPECT_TRUE(error.pointer == "/wire/unions/Loop1/0" || error.pointer == "/wire/unions/Loop2/0") << error.pointer;
}

// Unions that share their alternatives: every level has two unions and both list the two unions of the next
// level, so the number of paths from the top doubles per level while the number of unions grows by two.
json document_with_shared_unions(std::size_t levels) {
    json document = builtin_document(modern_revision);
    json& unions = document["wire"]["unions"];
    for (std::size_t level = 0; level < levels; ++level) {
        const std::string next = std::to_string(level + 1);
        const json alternatives = level + 1 == levels
                                      ? json::array({"ElicitRequestFormParams", "ElicitRequestURLParams"})
                                      : json::array({"Shared" + next + "a", "Shared" + next + "b"});
        unions["Shared" + std::to_string(level) + "a"] = alternatives;
        unions["Shared" + std::to_string(level) + "b"] = alternatives;
    }
    return document;
}

TEST(Wire, UnionsSharedByManyOthersAreWorkedOutOnce) {
    json document = document_with_shared_unions(48);
    document["wire"]["requests"]["tools/list"]["params"] = "Shared0a";
    document["wire"]["requests"]["tools/list"]["params_required"] = json::array({"message"});
    const auto result = load(document.dump());
    const auto* error = std::get_if<Error>(&result);
    ASSERT_EQ(error, nullptr) << error->pointer << ": " << error->message;
}

TEST(Rejects, McpNameMemberMissingBehindUnionsSharedByManyOthers) {
    json document = document_with_shared_unions(48);
    document["wire"]["requests"]["tools/call"]["params"] = "Shared0a";
    document["wire"]["requests"]["tools/call"]["params_required"] = json::array({"message"});
    const std::size_t index = header_index(document, "Mcp-Name");
    EXPECT_EQ(rejected(document).pointer, header_pointer(index) + "/on/" + pointer_token("tools/call"));
}

TEST(Rejects, UnionAlternativeThatRepeats) {
    json document = builtin_document(modern_revision);
    document["wire"]["unions"]["ElicitRequestParams"].push_back("ElicitRequestFormParams");
    EXPECT_EQ(rejected(document).pointer,
              "/wire/unions/ElicitRequestParams/" +
                  std::to_string(document["wire"]["unions"]["ElicitRequestParams"].size() - 1));
}

TEST(Rejects, McpNameMemberMissingFromOneAlternativeOfAUnion) {
    json document = builtin_document(modern_revision);
    const std::size_t index = header_index(document, "Mcp-Name");
    // elicitation/create is not a request in this revision; point a request at the union instead.
    document["wire"]["requests"]["tools/call"]["params"] = "ElicitRequestParams";
    document["wire"]["requests"]["tools/call"]["params_required"] = json::array({"message"});
    // "url" is a property of only one of the two alternatives of the union.
    request_headers(document)[index]["on"]["tools/call"] = "url";
    const Error error = rejected(document);
    EXPECT_EQ(error.pointer, header_pointer(index) + "/on/tools~1call");
}

TEST(Rejects, RequiredMemberThatIsNotAProperty) {
    json document = builtin_document(modern_revision);
    document["wire"]["types"]["CallToolResult"]["required"].push_back("bogus");
    EXPECT_EQ(rejected(document).pointer, "/wire/types/CallToolResult/required/" +
                                              std::to_string(document["wire"]["types"]["CallToolResult"]["required"].size() - 1));
}

TEST(Rejects, SenderThatRepeats) {
    json document = builtin_document(last_legacy_revision);
    document["wire"]["requests"]["ping"]["from"] = json::array({"client", "client"});
    EXPECT_EQ(rejected(document).pointer, "/wire/requests/ping/from/1");
}

TEST(Rejects, NoTransport) {
    json document = builtin_document(modern_revision);
    document["transports"] = json::object();
    EXPECT_EQ(rejected(document).pointer, "/transports");
}

TEST(Rejects, TextThatIsNotJson) {
    for (const std::string_view text : {std::string_view("{"), std::string_view(""), std::string_view("not json"), std::string_view("{} {}")}) {
        SCOPED_TRACE(std::string(text));
        auto result = load(text);
        const auto* error = std::get_if<Error>(&result);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(error->pointer, "");
    }
}

TEST(Rejects, RootThatIsNotAnObject) {
    for (const std::string_view text : {std::string_view("[]"), std::string_view("[1, 2]"), std::string_view("\"text\""), std::string_view("null")}) {
        SCOPED_TRACE(std::string(text));
        auto result = load(text);
        const auto* error = std::get_if<Error>(&result);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(error->pointer, "");
    }
}

}  // namespace
