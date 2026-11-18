#include <exception>
#include <utility>

#include "neomcp/json_schema.h"
#include "schema/internal.h"

namespace neomcp::json_schema {

Validator::Validator(std::shared_ptr<const detail::Compiled> compiled) noexcept : compiled_(std::move(compiled)) {}

std::variant<Validator, Issue> Validator::compile(const json& schema, Limits limits) {
    try {
        auto result = detail::compile_schema(schema, limits);
        if (auto* issue = std::get_if<Issue>(&result)) return std::move(*issue);
        return Validator(std::move(std::get<std::shared_ptr<const detail::Compiled>>(result)));
    } catch (const std::exception& error) {
        return Issue{{}, {}, std::string("the schema could not be compiled: ") + error.what()};
    }
}

std::optional<Issue> Validator::validate(const json& value) const {
    try {
        return detail::evaluate(*compiled_, value);
    } catch (const std::exception& error) {
        return Issue{{}, {}, std::string("the value could not be checked: ") + error.what()};
    }
}

}  // namespace neomcp::json_schema
