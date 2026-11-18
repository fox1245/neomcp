// Checks that the cost of comparing large values grows linearly with their size. The test binary runs under a short
// CTest timeout (see CMakeLists.txt): comparing two equal objects of n members by looking each member up in the other
// object, which is a linear scan, takes about n*n/2 key comparisons and does not finish in time.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

#include "neomcp/json.h"
#include "neomcp/json_schema.h"

namespace {

using neomcp::json;
using neomcp::json_schema::Validator;

constexpr std::size_t member_count = 20000;

// An object of `member_count` members "key0", "key1", ... built without looking up each key, in ascending or descending
// order. The value of the member "key<i>" is i, except that `last_value` replaces the value of the last key.
json make_object(bool descending, std::size_t last_value) {
    json object = json::object();
    auto* members = object.get_ptr<json::object_t*>();
    members->reserve(member_count);
    for (std::size_t step = 0; step < member_count; ++step) {
        const std::size_t i = descending ? member_count - 1 - step : step;
        const std::size_t value = i == member_count - 1 ? last_value : i;
        members->push_back(std::pair<const std::string, json>("key" + std::to_string(i), json(value)));
    }
    return object;
}

Validator unique_items() {
    auto compiled = Validator::compile(json::parse(R"({"uniqueItems":true})"));
    return std::get<Validator>(compiled);
}

TEST(JsonSchemaScale, EqualObjectsInADifferentOrderAreDuplicatesInLinearTime) {
    const Validator validator = unique_items();
    const json items = json::array({make_object(false, member_count - 1), make_object(true, member_count - 1)});
    const auto issue = validator.validate(items);
    ASSERT_TRUE(issue);
    EXPECT_NE(issue->message.find("duplicate items at indices 0 and 1"), std::string::npos) << issue->message;
}

TEST(JsonSchemaScale, EqualObjectsInTheSameOrderAreDuplicatesInLinearTime) {
    const Validator validator = unique_items();
    const json items = json::array({make_object(false, member_count - 1), make_object(false, member_count - 1)});
    EXPECT_TRUE(validator.validate(items));
}

TEST(JsonSchemaScale, ObjectsThatDifferInOneValueAreNotDuplicates) {
    const Validator validator = unique_items();
    EXPECT_FALSE(validator.validate(json::array({make_object(false, member_count - 1), make_object(true, 7)})));
    EXPECT_FALSE(validator.validate(json::array({make_object(false, member_count - 1), make_object(false, 7)})));
}

}  // namespace
