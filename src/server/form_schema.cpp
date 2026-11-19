// The restricted subset of JSON Schema that a form of elicitation may use: a flat object whose members are strings,
// numbers, integers, booleans and enums (a choice of one or of several), each with a title, a description and a
// default. The keywords of each shape are those the specification's pages list for it.
#include "neomcp/server/form_schema.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string_view>

namespace neomcp::detail {
namespace {

using Keywords = std::initializer_list<std::string_view>;

// The keywords every shape has besides its own.
constexpr std::string_view type_keyword = "type";
constexpr std::string_view title_keyword = "title";
constexpr std::string_view description_keyword = "description";
constexpr std::string_view default_keyword = "default";

// The formats a string member may name.
constexpr std::string_view string_formats[] = {"email", "uri", "date", "date-time"};

bool listed(std::string_view word, Keywords words) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

// The first member of `schema` that is not one of `allowed` or a keyword every shape has.
std::optional<std::string> stray_keyword(const json::object_t& schema, Keywords allowed) {
    for (const auto& [keyword, unused] : schema) {
        static_cast<void>(unused);
        if (keyword == type_keyword || keyword == title_keyword || keyword == description_keyword ||
            keyword == default_keyword || listed(keyword, allowed)) {
            continue;
        }
        return "it has the keyword '" + keyword + "', which a form does not define for its shape";
    }
    return std::nullopt;
}

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
        const json* constant = member_of(*choice, "const");
        const json* label = member_of(*choice, "title");
        return constant != nullptr && constant->is_string() && label != nullptr && label->is_string();
    });
}

std::optional<std::string> string_problem(const json::object_t& schema) {
    if (const json* choices = member_of(schema, "enum")) {
        if (!all_strings(*choices)) return std::string("its choices are not a list of strings");
        if (const json* labels = member_of(schema, "enumNames")) {
            if (!all_strings(*labels) || labels->size() != choices->size()) {
                return std::string("its names of the choices are not one string per choice");
            }
        }
        return stray_keyword(schema, {"enum", "enumNames"});
    }
    if (const json* choices = member_of(schema, "oneOf")) {
        if (!titled_choices(*choices)) return std::string("its choices are not a list of a const and a title each");
        return stray_keyword(schema, {"oneOf"});
    }
    if (const json* format = member_of(schema, "format")) {
        const auto* text = format->get_ptr<const std::string*>();
        if (text == nullptr ||
            std::find(std::begin(string_formats), std::end(string_formats), std::string_view(*text)) ==
                std::end(string_formats)) {
            return std::string("its format is not one a form defines (email, uri, date, date-time)");
        }
    }
    return stray_keyword(schema, {"minLength", "maxLength", "format"});
}

std::optional<std::string> list_problem(const json::object_t& schema) {
    const json* items = member_of(schema, "items");
    const auto* item_schema = items == nullptr ? nullptr : items->get_ptr<const json::object_t*>();
    if (item_schema == nullptr) return std::string("it is a list without the choices of its items");
    if (const json* choices = member_of(*item_schema, "anyOf")) {
        if (item_schema->size() != 1 || !titled_choices(*choices)) {
            return std::string("its items are not a choice of a const and a title each");
        }
    } else {
        const json* item_type = member_of(*item_schema, "type");
        const json* choices_of_items = member_of(*item_schema, "enum");
        if (item_schema->size() != 2 || item_type == nullptr || *item_type != json("string") ||
            choices_of_items == nullptr || !all_strings(*choices_of_items)) {
            return std::string("it is a list of something else than a choice of strings (a list of a described enum)");
        }
    }
    return stray_keyword(schema, {"items", "minItems", "maxItems"});
}

std::optional<std::string> member_problem(const json& member) {
    const auto* schema = member.get_ptr<const json::object_t*>();
    const json* type = schema == nullptr ? nullptr : member_of(*schema, type_keyword);
    const auto* type_name = type == nullptr ? nullptr : type->get_ptr<const std::string*>();
    if (type_name == nullptr) return std::string("it has no single type (a form holds strings, numbers, booleans and enums)");
    if (*type_name == "string") return string_problem(*schema);
    if (*type_name == "number" || *type_name == "integer") return stray_keyword(*schema, {"minimum", "maximum"});
    if (*type_name == "boolean") return stray_keyword(*schema, {});
    if (*type_name == "array") return list_problem(*schema);
    return "it is of type " + *type_name + "; a form holds strings, numbers, integers, booleans and enums, nothing nested";
}

}  // namespace

std::optional<std::string> form_schema_problem(const json& schema) {
    const auto* object = schema.get_ptr<const json::object_t*>();
    const json* properties = object == nullptr ? nullptr : member_of(*object, "properties");
    const auto* members = properties == nullptr ? nullptr : properties->get_ptr<const json::object_t*>();
    if (members == nullptr) return std::string("the schema is not an object with properties");
    for (const auto& [member, member_schema] : *members) {
        if (auto problem = member_problem(member_schema)) return "the member '" + member + "': " + *problem;
    }
    return std::nullopt;
}

}  // namespace neomcp::detail
