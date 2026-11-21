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

TEST(Wire, TheTypeValuesOfThePrimitiveSchemaTypesAreRecorded) {
    // The `type` values the schema fixes: one for a string and a boolean, two for a number.
    for (const std::string_view revision : {last_legacy_revision, modern_revision}) {
        SCOPED_TRACE(std::string(revision));
        const auto profile = builtin(revision);
        const Type* text = profile->type("StringSchema");
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->type_values, std::vector<std::string>{"string"});
        EXPECT_TRUE(text->has_type_value("string"));
        EXPECT_FALSE(text->has_type_value("number"));
        EXPECT_EQ(profile->type("BooleanSchema")->type_values, std::vector<std::string>{"boolean"});
        EXPECT_EQ(profile->type("NumberSchema")->type_values, (std::vector<std::string>{"integer", "number"}));
        // A type whose `type` the schema does not fix records none.
        const Type* implementation = profile->type("Implementation");
        ASSERT_NE(implementation, nullptr);
        EXPECT_TRUE(implementation->type_values.empty());
    }
    EXPECT_EQ(builtin("2025-11-25")->type("UntitledMultiSelectEnumSchema")->type_values, std::vector<std::string>{"array"});
}

TEST(Rejects, TypeValuesThatAreEmpty) {
    json document = builtin_document(modern_revision);
    document["wire"]["types"]["StringSchema"]["type_values"] = json::array();
    EXPECT_EQ(rejected(document).pointer, "/wire/types/StringSchema/type_values");
}

TEST(Rejects, TypeValuesThatRepeat) {
    json document = builtin_document(modern_revision);
    document["wire"]["types"]["StringSchema"]["type_values"] = json::array({"string", "string"});
    EXPECT_EQ(rejected(document).pointer, "/wire/types/StringSchema/type_values/1");
}

TEST(Rejects, TypeValuesThatAreNotStrings) {
    json document = builtin_document(modern_revision);
    document["wire"]["types"]["StringSchema"]["type_values"] = json::array({1});
    EXPECT_EQ(rejected(document).pointer, "/wire/types/StringSchema/type_values/0");
}

TEST(Rejects, TypeValuesOfATypeWithoutATypeProperty) {
    json document = builtin_document(modern_revision);
    document["wire"]["types"]["Implementation"]["type_values"] = json::array({"implementation"});
    EXPECT_EQ(rejected(document).pointer, "/wire/types/Implementation/type_values");
}

TEST(Wire, TheTypeOfAnObjectPropertyIsRecorded) {
    // An object written inline in a definition is a type of its own, named after the definition and the property;
    // a property that refers to an object type has that type.
    const auto legacy = builtin("2024-11-05");
    const Type* resource = legacy->type("Resource");
    ASSERT_NE(resource, nullptr);
    ASSERT_EQ(resource->property_types.size(), 1u);
    EXPECT_EQ(resource->property_types[0].property, "annotations");
    EXPECT_EQ(resource->property_types[0].type, "Resource.annotations");
    const Type* inline_annotations = legacy->property_type(*resource, "annotations");
    ASSERT_NE(inline_annotations, nullptr);
    EXPECT_EQ(inline_annotations->name, "Resource.annotations");
    EXPECT_EQ(inline_annotations->properties, (std::vector<std::string>{"audience", "priority"}));
    EXPECT_TRUE(inline_annotations->required.empty());
    // An object inside an inline object is recorded in the same way.
    const Type* completion = legacy->type("CompleteRequest.params");
    ASSERT_NE(completion, nullptr);
    EXPECT_EQ(legacy->property_type(*completion, "argument"), legacy->type("CompleteRequest.params.argument"));

    const std::string_view named_revisions[] = {"2025-03-26", "2025-06-18", last_legacy_revision, modern_revision};
    for (const std::string_view revision : named_revisions) {
        SCOPED_TRACE(std::string(revision));
        const auto profile = builtin(revision);
        const Type* owner = profile->type("Resource");
        ASSERT_NE(owner, nullptr);
        const Type* named = profile->type("Annotations");
        ASSERT_NE(named, nullptr);
        EXPECT_EQ(profile->property_type(*owner, "annotations"), named);
        EXPECT_EQ(profile->property_type(*owner, "uri"), nullptr);
    }
    // A type without such properties records none.
    EXPECT_TRUE(legacy->type("Annotations") == nullptr);
    EXPECT_TRUE(builtin("2025-06-18")->type("Annotations")->property_types.empty());
}

TEST(Rejects, PropertyTypeThatNamesNoType) {
    json document = builtin_document(last_legacy_revision);
    document["wire"]["types"]["Resource"]["property_types"]["annotations"] = "Missing.annotations";
    EXPECT_EQ(rejected(document).pointer, "/wire/types/Resource/property_types/annotations");
}

TEST(Rejects, PropertyTypeOfAnUnknownProperty) {
    json document = builtin_document(last_legacy_revision);
    document["wire"]["types"]["Resource"]["property_types"]["bogus"] = "Resource.annotations";
    EXPECT_EQ(rejected(document).pointer, "/wire/types/Resource/property_types/bogus");
}

TEST(Rejects, PropertyTypesThatAreEmpty) {
    json document = builtin_document(last_legacy_revision);
    document["wire"]["types"]["Resource"]["property_types"] = json::object();
    EXPECT_EQ(rejected(document).pointer, "/wire/types/Resource/property_types");
}

TEST(Rejects, PropertyTypeThatIsNotAString) {
    json document = builtin_document(last_legacy_revision);
    document["wire"]["types"]["Resource"]["property_types"]["annotations"] = 1;
    EXPECT_EQ(rejected(document).pointer, "/wire/types/Resource/property_types/annotations");
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

// ---------------------------------------------------------------------------------------------------------------
// Tasks and extensions

namespace {

// The identifier of the one extension profile built into the library.
constexpr std::string_view tasks_extension = "io.modelcontextprotocol/tasks";

json extension_document() {
    const std::string_view text = builtin_extension_text(tasks_extension, modern_revision);
    EXPECT_FALSE(text.empty());
    return json::parse(text);
}

Error rejected_extension(const json& document) {
    auto result = load_extension(document.dump());
    auto* error = std::get_if<Error>(&result);
    EXPECT_NE(error, nullptr) << "the document was accepted";
    return error != nullptr ? *error : Error{};
}

ExtensionPtr accepted_extension(const json& document) {
    auto result = load_extension(document.dump());
    auto* extension = std::get_if<ExtensionPtr>(&result);
    EXPECT_NE(extension, nullptr) << "the document was rejected";
    return extension != nullptr ? *extension : nullptr;
}

Error rejected_composition(const Profile& core, const ExtensionPtr& extension) {
    auto result = compose(core, std::span<const ExtensionPtr>(&extension, 1));
    auto* error = std::get_if<Error>(&result);
    EXPECT_NE(error, nullptr) << "the composition was accepted";
    return error != nullptr ? *error : Error{};
}

ProfilePtr composed(const Profile& core, const ExtensionPtr& extension) {
    auto result = compose(core, std::span<const ExtensionPtr>(&extension, 1));
    auto* profile = std::get_if<ProfilePtr>(&result);
    EXPECT_NE(profile, nullptr) << "the composition was rejected";
    return profile != nullptr ? *profile : nullptr;
}

const RequestHeader* header_named(const Profile& profile, std::string_view name) {
    for (const RequestHeader& header : profile.streamable_http()->request_headers) {
        if (header.name == name) return &header;
    }
    return nullptr;
}

}  // namespace

TEST(Tasks, EveryCoreRevisionNamesItsTasksMechanism) {
    // Stated here from the pages of each revision, not read back from the profiles: only 2025-11-25 defines tasks in
    // the core protocol, and 2026-07-28 moved them to an extension, so its core profile selects none.
    EXPECT_EQ(require_builtin("2024-11-05")->mechanism(Slot::tasks), "none");
    EXPECT_EQ(require_builtin("2025-03-26")->mechanism(Slot::tasks), "none");
    EXPECT_EQ(require_builtin("2025-06-18")->mechanism(Slot::tasks), "none");
    EXPECT_EQ(require_builtin("2025-11-25")->mechanism(Slot::tasks), "augmented_request");
    EXPECT_EQ(require_builtin("2026-07-28")->mechanism(Slot::tasks), "none");
    EXPECT_EQ(to_string(Slot::tasks), "tasks");
}

TEST(Tasks, TheTasksSlotDeclaresItsThreeNames) {
    const auto names = declared_mechanisms(Slot::tasks);
    std::vector<std::string_view> sorted(names.begin(), names.end());
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, (std::vector<std::string_view>{"augmented_request", "none", "server_directed"}));
}

TEST(Tasks, ACoreProfileThatNamesAnUndeclaredTasksMechanism) {
    json document = builtin_document(modern_revision);
    document["mechanisms"]["tasks"] = "polling";
    EXPECT_EQ(rejected(document).pointer, "/mechanisms/tasks");
    document["mechanisms"].erase("tasks");
    EXPECT_EQ(rejected(document).pointer, "/mechanisms/tasks");
}

TEST(Tasks, OnlyTheLastLegacyRevisionHasTaskMessagesInItsWire) {
    for (const auto method : {"tasks/get", "tasks/result", "tasks/list", "tasks/cancel"}) {
        EXPECT_NE(require_builtin(last_legacy_revision)->request(method), nullptr) << method;
        EXPECT_EQ(require_builtin(modern_revision)->request(method), nullptr) << method;
        EXPECT_EQ(require_builtin("2025-06-18")->request(method), nullptr) << method;
    }
}

TEST(Extension, TheBuiltInExtensionLoads) {
    const auto names = builtin_extensions();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], tasks_extension);
    const ExtensionPtr extension = builtin_extension(tasks_extension, modern_revision);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(builtin_extension(tasks_extension, modern_revision), extension) << "loaded once";
    EXPECT_EQ(extension->extension(), tasks_extension);
    ASSERT_EQ(extension->revisions().size(), 1u);
    EXPECT_EQ(extension->revisions()[0], modern_revision);
    EXPECT_TRUE(extension->applies_to(modern_revision));
    EXPECT_FALSE(extension->applies_to(last_legacy_revision));
    EXPECT_FALSE(extension->evidence().commit.empty());
    EXPECT_FALSE(extension->wire_schema().sha256.empty());

    EXPECT_EQ(extension->mechanism(Slot::tasks), std::optional<std::string_view>("server_directed"));
    for (const Slot slot : {Slot::open, Slot::state, Slot::server_input, Slot::change_feed, Slot::batch}) {
        EXPECT_EQ(extension->mechanism(slot), std::nullopt) << to_string(slot);
    }
}

TEST(Extension, UnknownExtensionOrRevision) {
    EXPECT_EQ(builtin_extension("example/none", modern_revision), nullptr);
    EXPECT_EQ(builtin_extension(tasks_extension, last_legacy_revision), nullptr);
    EXPECT_EQ(builtin_extension(tasks_extension, "no-such-revision"), nullptr);
    EXPECT_TRUE(builtin_extension_text("example/none", modern_revision).empty());
}

TEST(Extension, ItsWireHasTheThreeRequestsAndTheirSenders) {
    const ExtensionPtr extension = builtin_extension(tasks_extension, modern_revision);
    ASSERT_NE(extension, nullptr);
    std::vector<std::pair<std::string, std::string>> requests;
    for (const Message& message : extension->requests()) {
        EXPECT_TRUE(message.from_client) << message.method;
        EXPECT_FALSE(message.from_server) << message.method;
        EXPECT_EQ(std::count(message.params_required.begin(), message.params_required.end(), "taskId"), 1)
            << message.method;
        requests.emplace_back(message.method, message.type);
    }
    EXPECT_EQ(requests, (std::vector<std::pair<std::string, std::string>>{{"tasks/cancel", "CancelTaskRequest"},
                                                                          {"tasks/get", "GetTaskRequest"},
                                                                          {"tasks/update", "UpdateTaskRequest"}}));
    EXPECT_TRUE(extension->notifications().empty());
    // The status notification names a method of its own that the schema assigns to no sender.
    ASSERT_EQ(extension->unlisted_message_types().size(), 1u);
    EXPECT_EQ(extension->unlisted_message_types()[0], "TaskStatusNotification");
    ASSERT_EQ(extension->error_codes().size(), 1u);
    EXPECT_EQ(extension->error_codes()[0].name, "missing_required_client_capability");
    EXPECT_EQ(extension->error_codes()[0].code, -32021);
    ASSERT_EQ(extension->request_headers().size(), 1u);
}

TEST(Extension, TheCoreProfileAloneHasNoTaskMessage) {
    const ProfilePtr core = require_builtin(modern_revision);
    EXPECT_TRUE(core->extensions().empty());
    for (const auto method : {"tasks/get", "tasks/update", "tasks/cancel", "tasks/result", "tasks/list"}) {
        EXPECT_EQ(core->request(method), nullptr) << method;
    }
    EXPECT_EQ(core->type("CreateTaskResult"), nullptr);
}

TEST(Extension, ComposedWithTheExtensionTheRevisionHasTheTaskMessagesAndHeader) {
    const ProfilePtr core = require_builtin(modern_revision);
    const ExtensionPtr extension = builtin_extension(tasks_extension, modern_revision);
    ASSERT_NE(extension, nullptr);
    const ProfilePtr served = composed(*core, extension);
    ASSERT_NE(served, nullptr);

    ASSERT_EQ(served->extensions().size(), 1u);
    EXPECT_EQ(served->extensions()[0], tasks_extension);
    EXPECT_EQ(served->revision(), modern_revision);
    EXPECT_EQ(served->mechanism(Slot::tasks), "server_directed");
    // The other slots are the core's.
    for (const Slot slot : {Slot::open, Slot::state, Slot::server_input, Slot::change_feed, Slot::batch}) {
        EXPECT_EQ(served->mechanism(slot), core->mechanism(slot)) << to_string(slot);
    }
    for (const auto method : {"tasks/get", "tasks/update", "tasks/cancel"}) {
        const Message* message = served->request(method);
        ASSERT_NE(message, nullptr) << method;
        EXPECT_TRUE(message->from_client);
    }
    EXPECT_EQ(served->request("tasks/result"), nullptr);
    EXPECT_EQ(served->request("tasks/list"), nullptr);
    EXPECT_NE(served->request("tools/call"), nullptr) << "the core's requests stay";
    EXPECT_NE(served->type("CreateTaskResult"), nullptr);
    EXPECT_NE(served->type("Tool"), nullptr);
    EXPECT_EQ(served->error_code("missing_required_client_capability"), std::optional<int>(-32021));
    EXPECT_EQ(served->unlisted_message_types().size(), 1u);
    // The tables stay sorted, so the lookups above are valid.
    EXPECT_TRUE(std::is_sorted(served->requests().begin(), served->requests().end(),
                               [](const Message& a, const Message& b) { return a.method < b.method; }));
    EXPECT_TRUE(std::is_sorted(served->types().begin(), served->types().end(),
                               [](const Type& a, const Type& b) { return a.name < b.name; }));

    // The routing header names the task id for the three methods, beside the methods the revision already had.
    const RequestHeader* header = header_named(*served, "Mcp-Name");
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->params_member("tasks/get"), std::optional<std::string_view>("taskId"));
    EXPECT_EQ(header->params_member("tasks/update"), std::optional<std::string_view>("taskId"));
    EXPECT_EQ(header->params_member("tasks/cancel"), std::optional<std::string_view>("taskId"));
    EXPECT_EQ(header->params_member("tools/call"), std::optional<std::string_view>("name"));
    EXPECT_EQ(header->params_member("tasks/result"), std::nullopt);

    // The core is unchanged by the composition.
    EXPECT_EQ(core->request("tasks/get"), nullptr);
    EXPECT_EQ(core->mechanism(Slot::tasks), "none");
    EXPECT_EQ(header_named(*core, "Mcp-Name")->params_member("tasks/get"), std::nullopt);
}

TEST(Extension, TheBuiltInComposedProfileIsCached) {
    const std::vector<std::string_view> with_tasks{tasks_extension};
    const ProfilePtr served = builtin(modern_revision, with_tasks);
    ASSERT_NE(served, nullptr);
    EXPECT_EQ(builtin(modern_revision, with_tasks), served);
    EXPECT_NE(served, builtin(modern_revision));
    EXPECT_NE(served->request("tasks/get"), nullptr);
    EXPECT_EQ(builtin(modern_revision, {}), builtin(modern_revision)) << "no extension is the plain profile";
}

TEST(Extension, TheBuiltInComposedProfileIsNullForWhatIsNotBuiltIn) {
    const std::vector<std::string_view> with_tasks{tasks_extension};
    EXPECT_EQ(builtin(last_legacy_revision, with_tasks), nullptr) << "the extension is not defined for 2025-11-25";
    EXPECT_EQ(builtin("no-such-revision", with_tasks), nullptr);
    const std::vector<std::string_view> other{"example/none"};
    EXPECT_EQ(builtin(modern_revision, other), nullptr);
}

TEST(Extension, ConcurrentFirstUseSharesOneComposedProfile) {
    const std::vector<std::string_view> with_tasks{tasks_extension};
    std::vector<ProfilePtr> seen(8);
    std::vector<std::thread> threads;
    for (auto& slot : seen) threads.emplace_back([&slot, &with_tasks] { slot = builtin(modern_revision, with_tasks); });
    for (auto& thread : threads) thread.join();
    for (const auto& profile : seen) {
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile, seen.front());
    }
}

TEST(Extension, ComposingForARevisionTheExtensionIsNotDefinedFor) {
    const ExtensionPtr extension = builtin_extension(tasks_extension, modern_revision);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(rejected_composition(*require_builtin(last_legacy_revision), extension).pointer, "/revisions");
}

TEST(Extension, ComposingTheSameExtensionTwice) {
    const ProfilePtr core = require_builtin(modern_revision);
    const ExtensionPtr extension = builtin_extension(tasks_extension, modern_revision);
    const std::vector<ExtensionPtr> twice{extension, extension};
    auto result = compose(*core, twice);
    const auto* error = std::get_if<Error>(&result);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->pointer, "/extension");
}

TEST(Extension, ComposingANullExtension) {
    const ProfilePtr core = require_builtin(modern_revision);
    const std::vector<ExtensionPtr> none{nullptr};
    auto result = compose(*core, none);
    EXPECT_NE(std::get_if<Error>(&result), nullptr);
}

TEST(Extension, ComposingWithNoExtensionIsACopy) {
    const ProfilePtr core = require_builtin(modern_revision);
    auto result = compose(*core, {});
    const auto* copy = std::get_if<ProfilePtr>(&result);
    ASSERT_NE(copy, nullptr);
    EXPECT_NE(*copy, core);
    EXPECT_EQ((*copy)->requests().size(), core->requests().size());
    EXPECT_TRUE((*copy)->extensions().empty());
}

TEST(Extension, ATypeTheExtensionDefinesDifferentlyFromTheCoreIsRefused) {
    json document = extension_document();
    // `Tool` is a type both define.
    ASSERT_TRUE(document["wire"]["types"].contains("Tool"));
    document["wire"]["types"]["Tool"]["required"].push_back("annotations");
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(rejected_composition(*require_builtin(modern_revision), extension).pointer, "/wire/types/Tool");
}

TEST(Extension, ARequestTheExtensionDefinesDifferentlyFromTheCoreIsRefused) {
    json document = extension_document();
    // The core already has tools/list; the extension defines the same method with other required members.
    json claimed = document["wire"]["requests"]["tasks/get"];
    claimed["type"] = "ListToolsRequest";
    document["wire"]["requests"]["tools/list"] = claimed;
    document["senders"]["requests"]["ListToolsRequest"] = json::array({"client"});
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(rejected_composition(*require_builtin(modern_revision), extension).pointer,
              "/wire/requests/tools~1list");
}

TEST(Extension, AnErrorCodeTheExtensionGivesAnotherNumberIsRefused) {
    json document = extension_document();
    document["errors"]["missing_required_client_capability"] = -32099;
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(rejected_composition(*require_builtin(modern_revision), extension).pointer,
              "/errors/missing_required_client_capability");
}

TEST(Extension, AHeaderMethodTheRevisionAlreadyListsIsRefused) {
    json document = extension_document();
    document["transports"]["streamable_http"]["request_headers"][0]["on"]["tools/call"] = "name";
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(rejected_composition(*require_builtin(modern_revision), extension).pointer,
              "/transports/streamable_http/request_headers/0/on/tools~1call");
}

TEST(Extension, AHeaderMemberTheRequestDoesNotCarryIsRefused) {
    json document = extension_document();
    document["transports"]["streamable_http"]["request_headers"][0]["on"]["tasks/get"] = "no_such_member";
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(rejected_composition(*require_builtin(modern_revision), extension).pointer,
              "/transports/streamable_http/request_headers/0/on/tasks~1get");
}

TEST(Extension, AHeaderMethodNoRequestHasIsRefused) {
    json document = extension_document();
    document["transports"]["streamable_http"]["request_headers"][0]["on"]["tasks/result"] = "taskId";
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(rejected_composition(*require_builtin(modern_revision), extension).pointer,
              "/transports/streamable_http/request_headers/0/on/tasks~1result");
}

TEST(Extension, ANewHeaderIsAppendedToTheTransport) {
    json document = extension_document();
    document["transports"]["streamable_http"]["request_headers"] = json::array(
        {json{{"name", "Mcp-Task"}, {"value", "params_member"}, {"on", json{{"tasks/get", "taskId"}}}}});
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    const ProfilePtr served = composed(*require_builtin(modern_revision), extension);
    ASSERT_NE(served, nullptr);
    const RequestHeader* header = header_named(*served, "Mcp-Task");
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->params_member("tasks/get"), std::optional<std::string_view>("taskId"));
    EXPECT_EQ(header_named(*served, "Mcp-Name")->params_member("tasks/get"), std::nullopt);
}

TEST(Extension, ARevisionWithoutTheStreamableTransportTakesNoHeader) {
    json core_document = builtin_document(modern_revision);
    core_document["transports"].erase("streamable_http");
    auto loaded = load(core_document.dump());
    const auto* core = std::get_if<ProfilePtr>(&loaded);
    ASSERT_NE(core, nullptr);
    const ProfilePtr served = composed(**core, builtin_extension(tasks_extension, modern_revision));
    ASSERT_NE(served, nullptr);
    EXPECT_FALSE(served->streamable_http().has_value());
    EXPECT_NE(served->request("tasks/get"), nullptr);
}

TEST(ExtensionRejects, EveryRequiredTopLevelMemberRemovedInTurn) {
    const json original = extension_document();
    for (const char* name : {"profile_version", "extension", "revisions", "evidence", "mechanisms", "senders", "wire"}) {
        SCOPED_TRACE(name);
        json document = original;
        document.erase(name);
        EXPECT_EQ(rejected_extension(document).pointer, std::string("/") + name);
    }
}

TEST(ExtensionRejects, TheOptionalMembersMayBeLeftOut) {
    json document = extension_document();
    document.erase("transports");
    document.erase("errors");
    const ExtensionPtr extension = accepted_extension(document);
    ASSERT_NE(extension, nullptr);
    EXPECT_TRUE(extension->request_headers().empty());
    EXPECT_TRUE(extension->error_codes().empty());
}

TEST(ExtensionRejects, AnUnknownTopLevelMember) {
    json document = extension_document();
    document["era"] = "modern";
    EXPECT_EQ(rejected_extension(document).pointer, "/era");
}

TEST(ExtensionRejects, AMechanismNameThatIsNotDeclared) {
    json document = extension_document();
    document["mechanisms"]["tasks"] = "polling";
    EXPECT_EQ(rejected_extension(document).pointer, "/mechanisms/tasks");
}

TEST(ExtensionRejects, AMechanismSlotThatDoesNotExist) {
    json document = extension_document();
    document["mechanisms"]["telepathy"] = "none";
    EXPECT_EQ(rejected_extension(document).pointer, "/mechanisms/telepathy");
}

TEST(ExtensionRejects, NoRevision) {
    json document = extension_document();
    document["revisions"] = json::array();
    EXPECT_EQ(rejected_extension(document).pointer, "/revisions");
}

TEST(ExtensionRejects, ARevisionThatRepeats) {
    json document = extension_document();
    document["revisions"] = json::array({"2026-07-28", "2026-07-28"});
    EXPECT_EQ(rejected_extension(document).pointer, "/revisions/1");
}

TEST(ExtensionRejects, AnEmptyIdentifier) {
    json document = extension_document();
    document["extension"] = "";
    EXPECT_EQ(rejected_extension(document).pointer, "/extension");
}

TEST(ExtensionRejects, UnsupportedProfileVersion) {
    json document = extension_document();
    document["profile_version"] = 2;
    EXPECT_EQ(rejected_extension(document).pointer, "/profile_version");
}

TEST(ExtensionRejects, ASenderListThatLeavesAMessageOut) {
    json document = extension_document();
    document["senders"]["requests"].erase("UpdateTaskRequest");
    EXPECT_EQ(rejected_extension(document).pointer, "/senders/requests");
}

TEST(ExtensionRejects, ASenderThatDiffersFromTheWire) {
    json document = extension_document();
    document["senders"]["requests"]["GetTaskRequest"] = json::array({"server"});
    EXPECT_EQ(rejected_extension(document).pointer, "/senders/requests/GetTaskRequest");
}

TEST(ExtensionRejects, ASenderListForATypeThatIsNotAMessage) {
    json document = extension_document();
    document["senders"]["requests"]["Task"] = json::array({"client"});
    EXPECT_EQ(rejected_extension(document).pointer, "/senders/requests/Task");
}

TEST(ExtensionRejects, ASenderThatRepeatsOrIsUnknown) {
    json document = extension_document();
    document["senders"]["requests"]["GetTaskRequest"] = json::array({"client", "client"});
    EXPECT_EQ(rejected_extension(document).pointer, "/senders/requests/GetTaskRequest/1");
    document["senders"]["requests"]["GetTaskRequest"] = json::array({"proxy"});
    EXPECT_EQ(rejected_extension(document).pointer, "/senders/requests/GetTaskRequest/0");
    document["senders"]["requests"]["GetTaskRequest"] = json::array();
    EXPECT_EQ(rejected_extension(document).pointer, "/senders/requests/GetTaskRequest");
}

TEST(ExtensionRejects, ASenderListForANotificationTheWireLacks) {
    json document = extension_document();
    document["senders"]["notifications"]["TaskStatusNotification"] = json::array({"server"});
    EXPECT_EQ(rejected_extension(document).pointer, "/senders/notifications/TaskStatusNotification");
}

TEST(ExtensionRejects, ATransportOtherThanStreamableHttp) {
    json document = extension_document();
    document["transports"]["stdio"] = json{{"cancel", "cancelled_notification"}};
    EXPECT_EQ(rejected_extension(document).pointer, "/transports/stdio");
}

TEST(ExtensionRejects, TwoHeadersWhoseNamesDifferOnlyInCase) {
    json document = extension_document();
    json& headers = document["transports"]["streamable_http"]["request_headers"];
    headers.push_back(json{{"name", "mcp-name"}, {"value", "method"}, {"on", "every_request"}});
    EXPECT_EQ(rejected_extension(document).pointer, "/transports/streamable_http/request_headers/1/name");
}

TEST(ExtensionRejects, AnErrorCodeOutOfRange) {
    json document = extension_document();
    document["errors"]["missing_required_client_capability"] = -(std::int64_t{1} << 40);
    EXPECT_EQ(rejected_extension(document).pointer, "/errors/missing_required_client_capability");
}

TEST(ExtensionRejects, AWireThatIsBroken) {
    json document = extension_document();
    document["wire"]["requests"]["tasks/get"]["type"] = "";
    EXPECT_EQ(rejected_extension(document).pointer, "/wire/requests/tasks~1get/type");
}

TEST(ExtensionRejects, TextThatIsNotJsonOrNotAnObject) {
    auto text = load_extension("{");
    ASSERT_NE(std::get_if<Error>(&text), nullptr);
    EXPECT_EQ(std::get<Error>(text).pointer, "");
    auto array = load_extension("[]");
    ASSERT_NE(std::get_if<Error>(&array), nullptr);
    EXPECT_EQ(std::get<Error>(array).pointer, "");
}
