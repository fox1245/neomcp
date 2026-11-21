// The restricted subset of JSON Schema that a form of elicitation may use: a flat object whose members are strings,
// numbers, integers, booleans and enums (a choice of one or of several), each with the keywords the revision's
// types give its shape. The revision's profile says which shapes a form has (the alternatives of its union of primitive
// schema definitions), the `type` values of each, and its keywords and the ones it requires; what the profile does
// not hold, the values a keyword may have, is checked here.
#include "neomcp/server/form_schema.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace neomcp::detail {
namespace {

// The union of the schemas one member of a form may have, in the profile's wire tables.
constexpr std::string_view primitive_schema_union = "PrimitiveSchemaDefinition";

// The keywords the checks of values below look at.
constexpr std::string_view type_keyword = "type";
constexpr std::string_view properties_keyword = "properties";
constexpr std::string_view enum_keyword = "enum";
constexpr std::string_view enum_names_keyword = "enumNames";
constexpr std::string_view one_of_keyword = "oneOf";
constexpr std::string_view any_of_keyword = "anyOf";
constexpr std::string_view format_keyword = "format";
constexpr std::string_view items_keyword = "items";
constexpr std::string_view const_keyword = "const";
constexpr std::string_view title_keyword = "title";

// The keywords of the meta-data vocabulary of JSON Schema, which annotate a schema and do not change which instances
// are valid (JSON Schema Validation, draft 2020-12, section 9, "A Vocabulary for Basic Meta-Data and Annotations").
// A shape of a form that lacks one of them is still the shape of a member that has it: the form is written without it.
constexpr std::string_view annotation_keywords[] = {"title",      "description", "default", "deprecated",
                                                    "readOnly",   "writeOnly",   "examples"};

bool is_annotation(std::string_view keyword) {
    return std::find(std::begin(annotation_keywords), std::end(annotation_keywords), keyword) !=
           std::end(annotation_keywords);
}

// The formats a string member may name.
constexpr std::string_view string_formats[] = {"email", "uri", "date", "date-time"};

const json* member_of(const json::object_t& object, std::string_view member) {
    const auto found = object.find(std::string(member));
    return found == object.end() ? nullptr : &found->second;
}

bool all_strings(const json& list) {
    const auto* items = list.get_ptr<const json::array_t*>();
    if (items == nullptr || items->empty()) return false;
    return std::all_of(items->begin(), items->end(), [](const json& item) { return item.is_string(); });
}

// A list of choices with titles: [{"const": "...", "title": "..."}, ...].
bool titled_choices(const json& list) {
    const auto* items = list.get_ptr<const json::array_t*>();
    if (items == nullptr || items->empty()) return false;
    return std::all_of(items->begin(), items->end(), [](const json& item) {
        const auto* choice = item.get_ptr<const json::object_t*>();
        if (choice == nullptr || choice->size() != 2) return false;
        const json* constant = member_of(*choice, const_keyword);
        const json* label = member_of(*choice, title_keyword);
        return constant != nullptr && constant->is_string() && label != nullptr && label->is_string();
    });
}

// What is wrong with the values of the keywords of a member that the profile does not hold: whether a list of choices
// is a list of strings, with the names of the choices one string each, whether a format is one a form defines and
// whether the items of a list are a choice. Empty when each keyword the member has takes a value that fits.
std::optional<std::string> value_problem(const json::object_t& schema) {
    if (const json* choices = member_of(schema, enum_keyword)) {
        if (!all_strings(*choices)) return std::string("its choices are not a list of strings");
        if (const json* labels = member_of(schema, enum_names_keyword)) {
            if (!all_strings(*labels) || labels->size() != choices->size()) {
                return std::string("its names of the choices are not one string per choice");
            }
        }
    }
    if (const json* choices = member_of(schema, one_of_keyword)) {
        if (!titled_choices(*choices)) return std::string("its choices are not a list of a const and a title each");
    }
    if (const json* format = member_of(schema, format_keyword)) {
        const auto* text = format->get_ptr<const std::string*>();
        if (text == nullptr ||
            std::find(std::begin(string_formats), std::end(string_formats), std::string_view(*text)) ==
                std::end(string_formats)) {
            return std::string("its format is not one a form defines (email, uri, date, date-time)");
        }
    }
    if (const json* items = member_of(schema, items_keyword)) {
        const auto* item_schema = items->get_ptr<const json::object_t*>();
        if (item_schema == nullptr) return std::string("it is a list without the choices of its items");
        if (const json* choices = member_of(*item_schema, any_of_keyword)) {
            if (item_schema->size() != 1 || !titled_choices(*choices)) {
                return std::string("its items are not a choice of a const and a title each");
            }
        } else {
            const json* item_type = member_of(*item_schema, type_keyword);
            const json* choices_of_items = member_of(*item_schema, enum_keyword);
            if (item_schema->size() != 2 || item_type == nullptr || *item_type != json("string") ||
                choices_of_items == nullptr || !all_strings(*choices_of_items)) {
                return std::string(
                    "it is a list of something else than a choice of strings (a list of a described enum)");
            }
        }
    }
    return std::nullopt;
}

// The object types a union resolves to: its alternatives, through unions that name unions.
void union_types(const profile::Profile& profile, std::string_view name, std::set<std::string_view>& seen,
                 std::vector<const profile::Type*>& out) {
    if (!seen.insert(name).second) return;
    if (const profile::Type* type = profile.type(name)) {
        out.push_back(type);
        return;
    }
    if (const profile::Union* alternatives = profile.union_type(name)) {
        for (const std::string& alternative : alternatives->alternatives) union_types(profile, alternative, seen, out);
    }
}

std::vector<const profile::Type*> shapes_of_form(const profile::Profile& profile) {
    std::set<std::string_view> seen;
    std::vector<const profile::Type*> shapes;
    union_types(profile, primitive_schema_union, seen, shapes);
    return shapes;
}

// Whether a member, taken as the shape that a profile type defines, fits it, and what it would lose to fit: the
// keywords it has that the shape lacks and are annotations (they are left out), those that are not (they, and a
// keyword the shape requires and the member lacks, block it).
struct Fit {
    std::optional<std::string> stray;    // the first keyword the shape lacks that is not an annotation
    std::optional<std::string> missing;  // the first keyword the shape requires that the member lacks
    std::size_t blocking = 0;            // how many of both
    std::vector<std::string> left_out;   // the annotations the shape lacks
};

Fit fit_to(const json::object_t& schema, const profile::Type& shape) {
    Fit fit;
    for (const auto& [keyword, unused] : schema) {
        static_cast<void>(unused);
        if (shape.has_property(keyword)) continue;
        if (is_annotation(keyword)) {
            fit.left_out.push_back(keyword);
            continue;
        }
        if (!fit.stray) fit.stray = keyword;
        ++fit.blocking;
    }
    for (const std::string& needed : shape.required) {
        if (schema.find(needed) != schema.end()) continue;
        if (!fit.missing) fit.missing = needed;
        ++fit.blocking;
    }
    return fit;
}

// The member as the revision's form has it, with the annotations its shape lacks left out; or why it is not a member
// of the form.
std::variant<json, std::string> member_in(const json& member, const std::vector<const profile::Type*>& shapes) {
    const auto* schema = member.get_ptr<const json::object_t*>();
    const json* type = schema == nullptr ? nullptr : member_of(*schema, type_keyword);
    const auto* type_name = type == nullptr ? nullptr : type->get_ptr<const std::string*>();
    if (type_name == nullptr) {
        return std::string("it has no single type (a form holds strings, numbers, booleans and enums)");
    }
    // The shapes of this type: the alternatives whose `type` takes its value.
    std::vector<const profile::Type*> of_type;
    for (const profile::Type* shape : shapes) {
        if (shape->has_type_value(*type_name)) of_type.push_back(shape);
    }
    if (of_type.empty()) {
        return "it is of type " + *type_name + ", which the form of this revision has no shape for (nothing nested)";
    }
    if (auto problem = value_problem(*schema)) return std::move(*problem);
    // It fits when one shape has every keyword of the member, but for annotations, and the member has every keyword
    // the shape requires. Of the shapes it fits, the one that leaves out the fewest annotations.
    std::optional<Fit> fitting;
    std::optional<Fit> nearest;
    for (const profile::Type* shape : of_type) {
        Fit fit = fit_to(*schema, *shape);
        if (fit.blocking == 0) {
            if (!fitting || fit.left_out.size() < fitting->left_out.size()) fitting = std::move(fit);
        } else if (!nearest || fit.blocking < nearest->blocking) {
            nearest = std::move(fit);
        }
    }
    if (fitting) {
        json adapted = member;
        for (const std::string& keyword : fitting->left_out) adapted.erase(keyword);
        return adapted;
    }
    if (nearest->stray) {
        return "it has the keyword '" + *nearest->stray + "', which a form does not define for its shape";
    }
    return "it lacks the keyword '" + *nearest->missing + "', which its shape requires";
}

}  // namespace

std::variant<json, std::string> form_schema_in(const json& schema, const profile::Profile& profile) {
    const std::vector<const profile::Type*> shapes = shapes_of_form(profile);
    if (shapes.empty()) return std::string("the revision defines no form");
    const auto* object = schema.get_ptr<const json::object_t*>();
    const json* properties = object == nullptr ? nullptr : member_of(*object, properties_keyword);
    const auto* members = properties == nullptr ? nullptr : properties->get_ptr<const json::object_t*>();
    if (members == nullptr) return std::string("the schema is not an object with properties");
    json adapted = schema;
    for (const auto& [member, member_schema] : *members) {
        auto fitted = member_in(member_schema, shapes);
        if (auto* problem = std::get_if<std::string>(&fitted)) return "the member '" + member + "': " + *problem;
        adapted[std::string(properties_keyword)][member] = std::move(std::get<json>(fitted));
    }
    return adapted;
}

std::optional<std::string> form_schema_problem(const json& schema, const profile::Profile& profile) {
    auto form = form_schema_in(schema, profile);
    if (auto* problem = std::get_if<std::string>(&form)) return std::move(*problem);
    return std::nullopt;
}

std::optional<std::string> form_schema_problem(const json& schema, std::span<const profile::ProfilePtr> revisions) {
    std::vector<profile::ProfilePtr> served(revisions.begin(), revisions.end());
    if (served.empty()) {
        for (const std::string_view revision : profile::builtin_revisions()) served.push_back(profile::builtin(revision));
    }
    // A form is for the revisions that define one; when the server serves none, it is checked against the built-in ones
    // so that a form no revision defines is still found.
    std::vector<profile::ProfilePtr> defining;
    for (const auto& profile : served) {
        if (!shapes_of_form(*profile).empty()) defining.push_back(profile);
    }
    if (defining.empty()) {
        for (const std::string_view revision : profile::builtin_revisions()) {
            const profile::ProfilePtr builtin = profile::builtin(revision);
            if (!shapes_of_form(*builtin).empty()) defining.push_back(builtin);
        }
    }
    std::vector<std::pair<std::string, std::string>> problems;  // revision, problem
    for (const auto& profile : defining) {
        auto problem = form_schema_problem(schema, *profile);
        if (!problem) return std::nullopt;  // one revision defines this form
        problems.emplace_back(profile->revision(), std::move(*problem));
    }
    if (problems.empty()) return std::string("no revision defines a form");
    const bool alike = std::all_of(problems.begin(), problems.end(),
                                   [&](const auto& found) { return found.second == problems.front().second; });
    if (alike) return problems.front().second;
    std::string text;
    for (const auto& [revision, problem] : problems) {
        if (!text.empty()) text += "; ";
        text += "in " + revision + ": " + problem;
    }
    return text;
}

}  // namespace neomcp::detail
