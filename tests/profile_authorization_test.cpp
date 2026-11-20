// The `authorization` member of the profiles: what each revision decides for finding an authorization server, and the
// documents the loader refuses.
#include "neomcp/profile.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>
#include <neojson/json.hpp>

namespace {

using neojson::json;
using namespace neomcp::profile;

constexpr std::string_view origin_revision = "2025-03-26";
constexpr std::string_view modern_revision = "2026-07-28";
constexpr std::string_view protected_resource_metadata = "protected_resource_metadata";
constexpr std::string_view origin_metadata = "origin_metadata";

json builtin_document(std::string_view revision) { return json::parse(builtin_text(revision)); }

Error rejected(const json& document) {
    auto result = load(document.dump());
    auto* error = std::get_if<Error>(&result);
    EXPECT_NE(error, nullptr) << "the document was accepted";
    return error != nullptr ? *error : Error{};
}

ProfilePtr loaded(const json& document) {
    auto result = load(document.dump());
    auto* profile = std::get_if<ProfilePtr>(&result);
    if (profile == nullptr) {
        const Error& error = std::get<Error>(result);
        ADD_FAILURE() << "rejected at " << error.pointer << ": " << error.expected << "; " << error.message;
        return nullptr;
    }
    return *profile;
}

TEST(ProfileAuthorization, EveryRevisionNamesADeclaredDiscovery) {
    const auto declared = declared_authorization_discovery();
    ASSERT_FALSE(builtin_revisions().empty());
    for (const auto revision : builtin_revisions()) {
        SCOPED_TRACE(revision);
        const ProfilePtr profile = builtin(revision);
        ASSERT_NE(profile, nullptr);
        const Authorization& authorization = profile->authorization();
        EXPECT_NE(std::find(declared.begin(), declared.end(), authorization.discovery), declared.end());
        EXPECT_EQ(authorization.origin_metadata.has_value(), authorization.discovery == origin_metadata);
    }
}

TEST(ProfileAuthorization, TheDiscoveryOfEachRevision) {
    EXPECT_EQ(builtin("2024-11-05")->authorization().discovery, protected_resource_metadata);
    EXPECT_EQ(builtin("2025-03-26")->authorization().discovery, origin_metadata);
    EXPECT_EQ(builtin("2025-06-18")->authorization().discovery, protected_resource_metadata);
    EXPECT_EQ(builtin("2025-11-25")->authorization().discovery, protected_resource_metadata);
    EXPECT_EQ(builtin("2026-07-28")->authorization().discovery, protected_resource_metadata);
}

TEST(ProfileAuthorization, TheOriginDiscoveryOf20250326) {
    const ProfilePtr profile = builtin(origin_revision);
    ASSERT_NE(profile, nullptr);
    const auto& origin = profile->authorization().origin_metadata;
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(origin->metadata_path, "/.well-known/oauth-authorization-server");
    EXPECT_EQ(origin->default_endpoints.authorization, "/authorize");
    EXPECT_EQ(origin->default_endpoints.token, "/token");
    EXPECT_EQ(origin->default_endpoints.registration, "/register");
}

TEST(ProfileAuthorization, DeclaredNamesAreDistinctAndNonEmpty) {
    const auto names = declared_authorization_discovery();
    ASSERT_EQ(names.size(), 2u);
    for (const auto name : names) EXPECT_FALSE(name.empty());
    EXPECT_NE(names[0], names[1]);
}

TEST(ProfileAuthorizationRejects, TheMemberRemoved) {
    json document = builtin_document(modern_revision);
    document.erase("authorization");
    EXPECT_EQ(rejected(document).pointer, "/authorization");
}

TEST(ProfileAuthorizationRejects, AnUndeclaredDiscovery) {
    json document = builtin_document(modern_revision);
    document["authorization"]["discovery"] = "default_paths";
    const Error error = rejected(document);
    EXPECT_EQ(error.pointer, "/authorization/discovery");
    EXPECT_NE(error.expected.find("protected_resource_metadata"), std::string::npos);
}

TEST(ProfileAuthorizationRejects, ADiscoveryThatIsNotAString) {
    json document = builtin_document(modern_revision);
    document["authorization"]["discovery"] = 1;
    EXPECT_EQ(rejected(document).pointer, "/authorization/discovery");
}

TEST(ProfileAuthorizationRejects, TheDiscoveryRemoved) {
    json document = builtin_document(modern_revision);
    document["authorization"].erase("discovery");
    EXPECT_EQ(rejected(document).pointer, "/authorization/discovery");
}

TEST(ProfileAuthorizationRejects, AnUnknownMember) {
    json document = builtin_document(modern_revision);
    document["authorization"]["well_known"] = "oauth-protected-resource";
    EXPECT_EQ(rejected(document).pointer, "/authorization/well_known");
}

TEST(ProfileAuthorizationRejects, AnAuthorizationThatIsNotAnObject) {
    json document = builtin_document(modern_revision);
    document["authorization"] = "protected_resource_metadata";
    EXPECT_EQ(rejected(document).pointer, "/authorization");
}

TEST(ProfileAuthorizationRejects, OriginDataWithAnotherDiscovery) {
    json document = builtin_document(modern_revision);
    const json origin = builtin_document(origin_revision);
    document["authorization"]["origin_metadata"] = origin["authorization"]["origin_metadata"];
    EXPECT_EQ(rejected(document).pointer, "/authorization/origin_metadata");
}

TEST(ProfileAuthorizationRejects, OriginDiscoveryWithoutItsData) {
    json document = builtin_document(origin_revision);
    document["authorization"].erase("origin_metadata");
    EXPECT_EQ(rejected(document).pointer, "/authorization/origin_metadata");
}

TEST(ProfileAuthorizationRejects, OriginDataWithAMemberRemovedInTurn) {
    const json original = builtin_document(origin_revision);
    for (const std::string member : {"metadata_path", "default_endpoints"}) {
        SCOPED_TRACE(member);
        json document = original;
        document["authorization"]["origin_metadata"].erase(member);
        EXPECT_EQ(rejected(document).pointer, "/authorization/origin_metadata/" + member);
    }
    for (const std::string member : {"authorization", "token", "registration"}) {
        SCOPED_TRACE(member);
        json document = original;
        document["authorization"]["origin_metadata"]["default_endpoints"].erase(member);
        EXPECT_EQ(rejected(document).pointer, "/authorization/origin_metadata/default_endpoints/" + member);
    }
}

TEST(ProfileAuthorizationRejects, PathsThatAreNotAbsolutePaths) {
    const json original = builtin_document(origin_revision);
    for (const std::string path : {"", "token", "//as.example/token", "/token?x=1", "/token#a", "/to ken", "/t%4",
                                   "/t%zz", "https://as.example/token", "/t\xc3\xa4"}) {
        SCOPED_TRACE(path);
        json token = original;
        token["authorization"]["origin_metadata"]["default_endpoints"]["token"] = path;
        EXPECT_EQ(rejected(token).pointer, "/authorization/origin_metadata/default_endpoints/token");
        json metadata = original;
        metadata["authorization"]["origin_metadata"]["metadata_path"] = path;
        EXPECT_EQ(rejected(metadata).pointer, "/authorization/origin_metadata/metadata_path");
    }
    json number = original;
    number["authorization"]["origin_metadata"]["default_endpoints"]["registration"] = 7;
    EXPECT_EQ(rejected(number).pointer, "/authorization/origin_metadata/default_endpoints/registration");
}

TEST(ProfileAuthorization, PathsWithEveryPathCharacterAreRead) {
    json document = builtin_document(origin_revision);
    const std::string path = "/a-b._~/!$&'()*+,;=:@/%2Fx%41/";
    document["authorization"]["origin_metadata"]["default_endpoints"]["token"] = path;
    document["authorization"]["origin_metadata"]["metadata_path"] = "/";
    const ProfilePtr profile = loaded(document);
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->authorization().origin_metadata->default_endpoints.token, path);
    EXPECT_EQ(profile->authorization().origin_metadata->metadata_path, "/");
}

}  // namespace
