// The requested schema of a form, built from a described struct.
//
// A form asks the user for a flat object whose members are strings, numbers, integers, booleans and enums, each with
// an optional title, description and default: the restricted subset of JSON Schema that elicitation in form mode
// defines. form_schema_of<Form>() writes that subset for the described struct Form:
//
//   std::string                        {"type":"string"}, with min_length, max_length and a string one_of (a choice)
//   an integer type                    {"type":"integer"}, with minimum and maximum (and the type's own range)
//   float, double                      {"type":"number"}, with minimum and maximum
//   bool                               {"type":"boolean"}
//   a described enum                   {"type":"string","enum":[...]}, a choice of one
//   std::vector of a described enum    {"type":"array","items":{"type":"string","enum":[...]}}, a choice of several,
//                                      with min_items and max_items
//   a type with a neomcp::Schema       its schema, when that is one of the shapes above (for example a string with
//                                      a format of email, uri, date or date-time)
//
// neomcp::title, the member's description and neomcp::default_value add `title`, `description` and `default`. A
// std::optional member is not required; every other member is. The object has `type`, `properties` and `required`
// and nothing else (no `additionalProperties`, which the subset does not define).
//
// Which keywords a member may have comes from the profile of the revision: the alternatives of its union of primitive
// schema definitions are the shapes a form has, a member is of the shape whose `type` values include its own `type`,
// and it may have the keywords that shape has and must have those it requires. The revisions differ (2025-06-18 has no
// `default` for a string, no choice written as `oneOf` and no choice of several), so a form is a form of a revision.
// A keyword the shape lacks that only annotates the schema (title, description, default, deprecated, readOnly,
// writeOnly, examples: the meta-data vocabulary of JSON Schema, which does not change which instances are valid) is
// left out of the form that revision gets; any other keyword the shape lacks makes the member not fit.
// form_schema_of<Form>() writes the form with every keyword, if some built-in revision defines it (possibly without
// annotations); form_schema_of<Form>(profile) writes the form the revision of `profile` defines.
//
// A member that does not fit (a nested struct, a map, neomcp::json, a vector of anything but a described enum, a
// number with a one_of) makes form_schema_of throw std::invalid_argument, whose message names the member and why.
#pragma once

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

#include "neomcp/describe.h"
#include "neomcp/json.h"
#include "neomcp/profile.h"

namespace neomcp {

namespace detail {

// What keeps `schema`, an object schema whose properties are the members of a form, from being a form of the
// restricted subset that the revision of `profile` defines: the first member that does not fit, by name, with why;
// empty when every member fits.
std::optional<std::string> form_schema_problem(const json& schema, const profile::Profile& profile);
// The form of `schema` in the revision of `profile`: the schema with the annotations that a member's shape in that
// revision lacks left out (see above), or why it is not a form of that revision (the text form_schema_problem gives).
std::variant<json, std::string> form_schema_in(const json& schema, const profile::Profile& profile);
// The same for several revisions: empty when at least one of them that defines a form defines this one, else why not
// (the same reason once when every revision gives it, else one per revision). Empty `revisions`: every built-in one.
// When none of the revisions defines a form, the built-in ones are asked.
std::optional<std::string> form_schema_problem(const json& schema, std::span<const profile::ProfilePtr> revisions);

// The object schema of the members of Form, before form_schema_problem has looked at it.
template <class Form>
json form_schema_unchecked() {
    static_assert(described<Form>,
                  "neomcp::form_schema_of<Form>: Form is a described struct (NEOMCP_DESCRIBE or a neomcp::Described "
                  "specialisation)");
    static_assert(unique_names(Described<Form>::fields), "neomcp: two members of a described struct have the same name");
    json properties = json::object();
    json required = json::array();
    std::apply([&](const auto&... item) { (add_property<Form>(properties, required, item), ...); },
               Described<Form>::fields.items);
    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

}  // namespace detail

// The requested schema of a form whose answers decode as Form (see above), as some built-in revision defines it.
template <class Form>
json form_schema_of() {
    json schema = detail::form_schema_unchecked<Form>();
    if (const auto problem = detail::form_schema_problem(schema, std::span<const profile::ProfilePtr>())) {
        throw std::invalid_argument("neomcp: a form cannot be built from the struct: " + *problem);
    }
    return schema;
}

// The same, as the revision of `profile` defines it, without the annotations its shapes lack: std::invalid_argument
// when that revision has no such form.
template <class Form>
json form_schema_of(const profile::Profile& profile) {
    auto form = detail::form_schema_in(detail::form_schema_unchecked<Form>(), profile);
    if (auto* problem = std::get_if<std::string>(&form)) {
        throw std::invalid_argument("neomcp: a form of revision " + profile.revision() +
                                    " cannot be built from the struct: " + *problem);
    }
    return std::get<json>(std::move(form));
}

}  // namespace neomcp
