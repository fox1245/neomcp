// The annotations of a content block, a resource and a resource template, written and read as each revision's type of
// them has them. Revisions 2025-03-26 and later name the type (`Annotations`); 2024-11-05 writes the object inline in
// the definition of each owner, and `lastModified` exists from 2025-06-18. The expected members are written here from
// the schemas of the revisions, not read back from the profiles.
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>

#include "neomcp/json.h"
#include "neomcp/model.h"
#include "neomcp/profile.h"
#include "neomcp/wire.h"
#include "neomcp/wire_read.h"

namespace {

using neomcp::json;

constexpr std::string_view last_modified_text = "2026-01-01T00:00:00Z";

// The members of the annotations of every owner in each revision.
const std::map<std::string, std::set<std::string>> annotation_members{
    {"2024-11-05", {"audience", "priority"}},
    {"2025-03-26", {"audience", "priority"}},
    {"2025-06-18", {"audience", "lastModified", "priority"}},
    {"2025-11-25", {"audience", "lastModified", "priority"}},
    {"2026-07-28", {"audience", "lastModified", "priority"}},
};

neomcp::ContentAnnotations full_annotations() {
    neomcp::ContentAnnotations annotations;
    annotations.audience = {neomcp::Role::user};
    annotations.priority = 0.5;
    annotations.last_modified = std::string(last_modified_text);
    return annotations;
}

std::set<std::string> members_of(const json& object) {
    std::set<std::string> out;
    for (const auto& [name, value] : *object.get_ptr<const json::object_t*>()) out.insert(name);
    return out;
}

class AnnotationsPerRevision : public ::testing::TestWithParam<std::string> {};

TEST_P(AnnotationsPerRevision, TheTableCoversEveryRevision) {
    EXPECT_EQ(annotation_members.size(), neomcp::profile::builtin_revisions().size()) << "a new revision needs its members here";
    EXPECT_TRUE(annotation_members.count(GetParam()) == 1);
}

TEST_P(AnnotationsPerRevision, ABlockIsWrittenWithTheMembersOfItsRevision) {
    const auto profile = neomcp::profile::builtin(GetParam());
    ASSERT_NE(profile, nullptr);
    neomcp::TextContent text;
    text.text = "hi";
    text.annotations = full_annotations();
    const json wire = neomcp::to_wire(neomcp::ContentBlock{text}, *profile);
    EXPECT_EQ(members_of(wire.at("annotations")), annotation_members.at(GetParam())) << wire.dump();

    neomcp::ImageContent image;
    image.data = "AAAA";
    image.mime_type = "image/png";
    image.annotations = full_annotations();
    const json image_wire = neomcp::to_wire(neomcp::ContentBlock{image}, *profile);
    EXPECT_EQ(members_of(image_wire.at("annotations")), annotation_members.at(GetParam())) << image_wire.dump();

    neomcp::EmbeddedResource embedded;
    neomcp::TextResourceContents contents;
    contents.uri = "file:///a";
    contents.text = "a";
    embedded.resource = contents;
    embedded.annotations = full_annotations();
    const json embedded_wire = neomcp::to_wire(neomcp::ContentBlock{embedded}, *profile);
    EXPECT_EQ(members_of(embedded_wire.at("annotations")), annotation_members.at(GetParam())) << embedded_wire.dump();
}

TEST_P(AnnotationsPerRevision, AResourceAndATemplateAreWrittenWithTheMembersOfTheirRevision) {
    const auto profile = neomcp::profile::builtin(GetParam());
    ASSERT_NE(profile, nullptr);
    neomcp::ResourceDefinition resource;
    resource.uri = "file:///a";
    resource.name = "a";
    resource.annotations = full_annotations();
    const json resource_wire = neomcp::to_wire(resource, *profile);
    EXPECT_EQ(members_of(resource_wire.at("annotations")), annotation_members.at(GetParam())) << resource_wire.dump();

    neomcp::ResourceTemplateDefinition entry;
    entry.uri_template = "file:///{path}";
    entry.name = "files";
    entry.annotations = full_annotations();
    const json entry_wire = neomcp::to_wire(entry, *profile);
    EXPECT_EQ(members_of(entry_wire.at("annotations")), annotation_members.at(GetParam())) << entry_wire.dump();
}

TEST_P(AnnotationsPerRevision, AnnotationsAreReadWithTheMembersOfTheirRevision) {
    const auto profile = neomcp::profile::builtin(GetParam());
    ASSERT_NE(profile, nullptr);
    const json wire = json::parse(
        R"({"uri":"file:///a","name":"a","annotations":{"audience":["user"],"priority":0.5,"lastModified":"2026-01-01T00:00:00Z"}})");
    const auto read = neomcp::from_wire<neomcp::ResourceDefinition>(wire, *profile);
    const auto* resource = std::get_if<neomcp::ResourceDefinition>(&read);
    ASSERT_NE(resource, nullptr) << std::get<std::string>(read);
    ASSERT_TRUE(resource->annotations.has_value());
    EXPECT_EQ(resource->annotations->audience, std::vector<neomcp::Role>{neomcp::Role::user});
    EXPECT_EQ(resource->annotations->priority, std::optional<double>(0.5));
    // A revision whose type lacks `lastModified` does not read it from a peer that sends it.
    const bool has_last_modified = annotation_members.at(GetParam()).count("lastModified") == 1;
    EXPECT_EQ(resource->annotations->last_modified.has_value(), has_last_modified);
}

TEST_P(AnnotationsPerRevision, ABlockRoundTripsWhatItsRevisionHas) {
    const auto profile = neomcp::profile::builtin(GetParam());
    ASSERT_NE(profile, nullptr);
    neomcp::TextContent text;
    text.text = "hi";
    text.annotations = full_annotations();
    const json wire = neomcp::to_wire(neomcp::ContentBlock{text}, *profile);
    const auto read = neomcp::from_wire<neomcp::ContentBlock>(wire, *profile);
    const auto* block = std::get_if<neomcp::ContentBlock>(&read);
    ASSERT_NE(block, nullptr) << std::get<std::string>(read);
    const auto* read_text = std::get_if<neomcp::TextContent>(block);
    ASSERT_NE(read_text, nullptr);
    ASSERT_TRUE(read_text->annotations.has_value());
    EXPECT_EQ(neomcp::to_wire(*block, *profile), wire);
}

INSTANTIATE_TEST_SUITE_P(Revisions, AnnotationsPerRevision,
                         ::testing::Values("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"));

TEST(AnnotationsProfile, TheTypeOfAPropertyIsFoundThroughTheOwner) {
    // Revision 2024-11-05 writes the object inline in each owner; the later ones name the type.
    const auto legacy = neomcp::profile::builtin("2024-11-05");
    const auto modern = neomcp::profile::builtin("2026-07-28");
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(modern, nullptr);
    EXPECT_EQ(legacy->type("Annotations"), nullptr);
    ASSERT_NE(modern->type("Annotations"), nullptr);
    for (const std::string_view owner : {"Resource", "ResourceTemplate", "TextContent", "ImageContent", "EmbeddedResource"}) {
        SCOPED_TRACE(std::string(owner));
        const neomcp::profile::Type* legacy_owner = legacy->type(owner);
        ASSERT_NE(legacy_owner, nullptr);
        const neomcp::profile::Type* legacy_type = legacy->property_type(*legacy_owner, "annotations");
        ASSERT_NE(legacy_type, nullptr);
        EXPECT_TRUE(legacy_type->has_property("audience"));
        EXPECT_TRUE(legacy_type->has_property("priority"));
        EXPECT_FALSE(legacy_type->has_property("lastModified"));
        const neomcp::profile::Type* modern_owner = modern->type(owner);
        ASSERT_NE(modern_owner, nullptr);
        EXPECT_EQ(modern->property_type(*modern_owner, "annotations"), modern->type("Annotations"));
        // A property that is not an object of a type has none.
        EXPECT_EQ(modern->property_type(*modern_owner, "uri"), nullptr);
        EXPECT_EQ(modern->property_type(*modern_owner, "no such property"), nullptr);
    }
}

}  // namespace
