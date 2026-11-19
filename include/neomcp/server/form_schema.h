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
// A member that does not fit (a nested struct, a map, neomcp::json, a vector of anything but a described enum, a
// number with a one_of) makes form_schema_of throw std::invalid_argument, whose message names the member and why.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

#include "neomcp/describe.h"
#include "neomcp/json.h"

namespace neomcp {

namespace detail {

// What keeps `schema`, an object schema whose properties are the members of a form, from being a form of the
// restricted subset: the first member that does not fit, by name, with why; empty when every member fits.
std::optional<std::string> form_schema_problem(const json& schema);

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

// The requested schema of a form whose answers decode as Form (see above).
template <class Form>
json form_schema_of() {
    json schema = detail::form_schema_unchecked<Form>();
    if (const auto problem = detail::form_schema_problem(schema)) {
        throw std::invalid_argument("neomcp: a form cannot be built from the struct: " + *problem);
    }
    return schema;
}

}  // namespace neomcp
