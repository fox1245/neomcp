// Describing C++ types for the Model Context Protocol: which members a struct has, which names an enum has, the
// JSON Schema that follows from it, and the conversion between values and JSON.
//
//   struct AddArgs { int a; int b; std::optional<std::string> note; };
//
//   template <> struct neomcp::Described<AddArgs> {
//       static constexpr auto fields = neomcp::fields(
//           neomcp::field(&AddArgs::a, "a", "first addend"),
//           neomcp::field(&AddArgs::b, "b", "second addend", neomcp::minimum(0), neomcp::maximum(100)),
//           neomcp::field(&AddArgs::note, "note", "a note to echo back"));
//   };
//
//   neomcp::json schema = neomcp::schema_of<AddArgs>();
//   AddArgs args = neomcp::from_json<AddArgs>(document);
//   neomcp::json reply = neomcp::to_json(args);
//
// For a struct whose members need no options, the macros write the specialisation. Use them at global scope:
//
//   NEOMCP_DESCRIBE(Point, x, "horizontal position", y, "vertical position")
//   NEOMCP_DESCRIBE_ENUM(Color, red, green, blue)
//
// NEOMCP_DESCRIBE takes the type, then each member followed by its description (an empty string for none); the
// member's own name is its JSON name. It accepts up to 32 members and NEOMCP_DESCRIBE_ENUM up to 63 enumerators;
// describe a larger type by hand. A description must come before the first use of its type, which includes the
// options (one_of, default_value) of another struct's field that name an enum. The pieces are in
// neomcp/describe/: model.h (descriptions and options), schema.h (schema_of, Schema) and convert.h (from_json,
// to_json, DecodeError).
#pragma once

#include "neomcp/describe/convert.h"
#include "neomcp/describe/macros.h"
#include "neomcp/describe/model.h"
#include "neomcp/describe/schema.h"

// Describes `Type` with the given members; see the top of this file.
#define NEOMCP_DESCRIBE(Type, ...)                                                                       \
    template <>                                                                                          \
    struct neomcp::Described<Type> {                                                                     \
        static constexpr auto fields =                                                                   \
            neomcp::fields(NEOMCP_DETAIL_EXPAND(NEOMCP_DETAIL_PAIRS(Type, __VA_ARGS__)));                \
    };

// Describes the enum `Type` as a string enum named by its enumerators; see the top of this file.
#define NEOMCP_DESCRIBE_ENUM(Type, ...)                                                                   \
    template <>                                                                                          \
    struct neomcp::DescribedEnum<Type> {                                                                 \
        static constexpr neomcp::EnumEntry<Type> values[] = {                                            \
            NEOMCP_DETAIL_EXPAND(NEOMCP_DETAIL_ENUMS(Type, __VA_ARGS__))};                               \
    };
