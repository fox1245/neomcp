// Checking a value against the nodes of a compiled schema.
//
// A check that cannot be completed because it ran into a limit is not a schema that "does not match": it ends the
// whole validation (see Run::stop). Otherwise `not`, `anyOf`, `oneOf`, `if` and `contains` would turn a failure to
// look into a pass.
#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "schema/internal.h"
#include "schema/pattern_translate.h"
#include "schema/value.h"

namespace neomcp::json_schema::detail {

namespace {

// What the keywords applied to one value have evaluated, for `unevaluatedProperties` and `unevaluatedItems`. A
// subschema that fails contributes nothing.
struct Annotations {
    std::vector<bool> properties;  // by position of the member in the object
    std::size_t prefix_items = 0;  // items before this index were evaluated by `prefixItems`
    bool all_items = false;
    std::vector<bool> contained;  // by index of the item: matched by `contains`

    static void mark(std::vector<bool>& flags, std::size_t index, std::size_t size) {
        if (flags.size() < size) flags.resize(size);
        flags[index] = true;
    }
    static bool marked(const std::vector<bool>& flags, std::size_t index) {
        return index < flags.size() && flags[index];
    }
    static void merge_flags(std::vector<bool>& into, const std::vector<bool>& from) {
        if (into.size() < from.size()) into.resize(from.size());
        for (std::size_t i = 0; i < from.size(); ++i) {
            if (from[i]) into[i] = true;
        }
    }
    void merge(const Annotations& other) {
        merge_flags(properties, other.properties);
        merge_flags(contained, other.contained);
        prefix_items = (std::max)(prefix_items, other.prefix_items);
        all_items = all_items || other.all_items;
    }
};

// One step from the root of the validated value: a member name or an array index.
struct Step {
    const std::string* member;
    std::size_t index;
};

class PathGuard {
public:
    PathGuard(std::vector<Step>& path, Step step) : path_(path) { path_.push_back(step); }
    PathGuard(const PathGuard&) = delete;
    PathGuard& operator=(const PathGuard&) = delete;
    ~PathGuard() { path_.pop_back(); }

private:
    std::vector<Step>& path_;
};

std::size_t code_points(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        // Every byte of UTF-8 text except the continuation bytes starts a code point.
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++count;
    }
    return count;
}

std::string describe_types(std::uint8_t types) {
    std::string out;
    const auto add = [&](std::uint8_t bit, const char* name) {
        if ((types & bit) == 0) return;
        if (!out.empty()) out += ", ";
        out += name;
    };
    add(type_bit::null, "null");
    add(type_bit::boolean, "boolean");
    add(type_bit::object, "object");
    add(type_bit::array, "array");
    add(type_bit::number, "number");
    add(type_bit::integer, "integer");
    add(type_bit::string, "string");
    return out;
}

bool has_type(std::uint8_t types, const json& value) {
    switch (value.type()) {
        case json::value_t::null: return (types & type_bit::null) != 0;
        case json::value_t::boolean: return (types & type_bit::boolean) != 0;
        case json::value_t::object: return (types & type_bit::object) != 0;
        case json::value_t::array: return (types & type_bit::array) != 0;
        case json::value_t::string: return (types & type_bit::string) != 0;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
            return (types & type_bit::number) != 0 || ((types & type_bit::integer) != 0 && is_integral(value));
        default: return false;
    }
}

class Run {
public:
    explicit Run(const Compiled& compiled) : compiled_(compiled), limits_(compiled.limits) {}

    std::optional<Issue> validate(const json& value) {
        Issue issue;
        const bool valid = eval(*compiled_.root, value, 0, nullptr, &issue);
        if (stopped_) return std::move(stopped_);
        if (valid) return std::nullopt;
        return issue;
    }

private:
    enum class Match { found, absent, too_long, not_utf8 };

    std::string instance_pointer() const {
        std::string out;
        for (const Step& step : path_) {
            out.push_back('/');
            out += step.member ? escape_token(*step.member) : std::to_string(step.index);
        }
        return out;
    }

    Issue make_issue(const Node& node, std::string_view keyword, std::string message) const {
        Issue issue;
        issue.instance_pointer = instance_pointer();
        issue.schema_pointer = node.pointer;
        if (!keyword.empty()) issue.schema_pointer += "/" + escape_token(keyword);
        issue.message = std::move(message);
        return issue;
    }

    // A failure of a keyword. Nothing is built when the caller does not want the Issue.
    bool fail(Issue* issue, const Node& node, std::string_view keyword, std::string_view message) const {
        if (issue) *issue = make_issue(node, keyword, std::string(message));
        return false;
    }

    template <typename Message, typename = std::enable_if_t<std::is_invocable_r_v<std::string, Message>>>
    bool fail(Issue* issue, const Node& node, std::string_view keyword, Message&& message) const {
        if (issue) *issue = make_issue(node, keyword, message());
        return false;
    }

    // End the whole validation with an Issue. The first reason stands.
    bool stop(const Node& node, std::string_view keyword, std::string message) {
        if (!stopped_) stopped_ = make_issue(node, keyword, std::move(message));
        return false;
    }

    bool eval(const Node& node, const json& value, std::size_t depth, Annotations* out, Issue* issue) {
        if (stopped_) return false;
        if (depth > limits_.max_depth) {
            return stop(node, "", "schemas are nested deeper than the limit max_depth (" +
                                       std::to_string(limits_.max_depth) + ")");
        }
        if (++evaluations_ > limits_.max_evaluations) {
            return stop(node, "", "more than the limit max_evaluations (" + std::to_string(limits_.max_evaluations) +
                                       ") schemas were evaluated");
        }
        if (node.form == Node::Form::always) return true;
        if (node.form == Node::Form::never) return fail(issue, node, "", "no value is valid against a false schema");

        Annotations local;
        Annotations* here = compiled_.track_annotations ? &local : nullptr;
        if (node.ref && !eval(*node.ref, value, depth + 1, here, issue)) return false;
        if (!check_type(node, value, issue) || !check_const_and_enum(node, value, issue)) return false;
        switch (value.type()) {
            case json::value_t::number_integer:
            case json::value_t::number_unsigned:
            case json::value_t::number_float:
                if (!check_number(node, value, issue)) return false;
                break;
            case json::value_t::string:
                if (!check_string(node, value, issue)) return false;
                break;
            case json::value_t::array:
                if (!check_array(node, value, depth, here, issue)) return false;
                break;
            case json::value_t::object:
                if (!check_object(node, value, depth, here, issue)) return false;
                break;
            default:
                break;
        }
        if (!check_applicators(node, value, depth, here, issue)) return false;
        if (!check_unevaluated(node, value, depth, here, issue)) return false;
        if (out && here) out->merge(*here);
        return true;
    }

    bool check_type(const Node& node, const json& value, Issue* issue) const {
        if (node.types == 0 || has_type(node.types, value)) return true;
        return fail(issue, node, "type", [&] {
            return "is " + std::string(type_name(value)) + " but must be " + describe_types(node.types);
        });
    }

    bool check_const_and_enum(const Node& node, const json& value, Issue* issue) {
        if (node.const_value) {
            switch (equal(value, *node.const_value, limits_.max_value_depth)) {
                case Equality::equal: break;
                case Equality::different: return fail(issue, node, "const", "does not match const");
                case Equality::too_deep: return stop_for_value_depth(node, "const");
            }
        }
        if (node.enum_values) {
            for (const json& option : *node.enum_values) {
                switch (equal(value, option, limits_.max_value_depth)) {
                    case Equality::equal: return true;
                    case Equality::different: break;
                    case Equality::too_deep: return stop_for_value_depth(node, "enum");
                }
            }
            return fail(issue, node, "enum", "is not one of the values in enum");
        }
        return true;
    }

    bool stop_for_value_depth(const Node& node, std::string_view keyword) {
        return stop(node, keyword, "arrays and objects are nested deeper than the limit max_value_depth (" +
                                        std::to_string(limits_.max_value_depth) + ")");
    }

    bool check_number(const Node& node, const json& value, Issue* issue) const {
        if (node.minimum && compare_numbers(value, *node.minimum) == Order::less) {
            return fail(issue, node, "minimum", [&] { return "is less than the minimum " + node.minimum->dump(); });
        }
        if (node.maximum && compare_numbers(value, *node.maximum) == Order::greater) {
            return fail(issue, node, "maximum", [&] { return "is greater than the maximum " + node.maximum->dump(); });
        }
        if (node.exclusive_minimum) {
            const Order order = compare_numbers(value, *node.exclusive_minimum);
            if (order == Order::less || order == Order::equal) {
                return fail(issue, node, "exclusiveMinimum",
                            [&] { return "is not greater than exclusiveMinimum " + node.exclusive_minimum->dump(); });
            }
        }
        if (node.exclusive_maximum) {
            const Order order = compare_numbers(value, *node.exclusive_maximum);
            if (order == Order::greater || order == Order::equal) {
                return fail(issue, node, "exclusiveMaximum",
                            [&] { return "is not less than exclusiveMaximum " + node.exclusive_maximum->dump(); });
            }
        }
        if (node.multiple_of && !is_multiple_of(value, *node.multiple_of)) {
            return fail(issue, node, "multipleOf",
                        [&] { return "is not a multiple of " + node.multiple_of->dump(); });
        }
        return true;
    }

    Match search(const Pattern& pattern, const std::string& text) const {
        if (text.size() > limits_.max_pattern_input) return Match::too_long;
        if (!is_valid_utf8(text)) return Match::not_utf8;
        return pattern.search(text) ? Match::found : Match::absent;
    }

    // The reason a pattern could not be tried, which ends the validation.
    bool stop_for_pattern(const Node& node, std::string_view keyword, Match match, std::string_view what) {
        if (match == Match::not_utf8) {
            return stop(node, keyword, std::string(what) + " is not valid UTF-8, which a pattern is matched against");
        }
        return stop(node, keyword, std::string(what) + " is longer than the limit max_pattern_input (" +
                                       std::to_string(limits_.max_pattern_input) + " bytes)");
    }

    bool check_string(const Node& node, const json& value, Issue* issue) {
        const std::string& text = *value.get_ptr<const json::string_t*>();
        if (node.min_length || node.max_length) {
            const std::size_t length = code_points(text);
            if (node.min_length && length < *node.min_length) {
                return fail(issue, node, "minLength", [&] {
                    return "is shorter than minLength " + std::to_string(*node.min_length);
                });
            }
            if (node.max_length && length > *node.max_length) {
                return fail(issue, node, "maxLength", [&] {
                    return "is longer than maxLength " + std::to_string(*node.max_length);
                });
            }
        }
        if (node.pattern) {
            const Match match = search(*node.pattern, text);
            if (match == Match::absent) {
                return fail(issue, node, "pattern", [&] { return "does not match pattern " + quote(node.pattern->source); });
            }
            if (match != Match::found) return stop_for_pattern(node, "pattern", match, "the string");
        }
        return true;
    }

    bool check_array(const Node& node, const json& value, std::size_t depth, Annotations* here, Issue* issue) {
        const auto& items = *value.get_ptr<const json::array_t*>();
        const std::size_t size = items.size();
        if (node.min_items && size < *node.min_items) {
            return fail(issue, node, "minItems", [&] { return "has fewer than minItems " + std::to_string(*node.min_items); });
        }
        if (node.max_items && size > *node.max_items) {
            return fail(issue, node, "maxItems", [&] { return "has more than maxItems " + std::to_string(*node.max_items); });
        }
        if (node.unique_items && size > 1 && !check_unique(node, items, issue)) return false;

        const std::size_t prefix = (std::min)(size, node.prefix_items.size());
        for (std::size_t i = 0; i < prefix; ++i) {
            const PathGuard step(path_, Step{nullptr, i});
            if (!eval(*node.prefix_items[i], items[i], depth + 1, nullptr, issue)) return false;
        }
        if (here && !node.prefix_items.empty()) here->prefix_items = (std::max)(here->prefix_items, prefix);

        if (node.items) {
            for (std::size_t i = prefix; i < size; ++i) {
                const PathGuard step(path_, Step{nullptr, i});
                if (!eval(*node.items, items[i], depth + 1, nullptr, issue)) return false;
            }
            if (here) here->all_items = true;
        }

        if (node.contains) {
            const std::size_t least = node.min_contains.value_or(1);
            std::size_t matched = 0;
            for (std::size_t i = 0; i < size; ++i) {
                const PathGuard step(path_, Step{nullptr, i});
                const bool ok = eval(*node.contains, items[i], depth + 1, nullptr, nullptr);
                if (stopped_) return false;
                if (!ok) continue;
                ++matched;
                if (here) Annotations::mark(here->contained, i, size);
                // Without annotations or an upper bound, the count past the lower bound does not matter.
                if (!here && !node.max_contains && matched >= least) break;
            }
            if (matched < least) {
                return fail(issue, node, "contains", "does not contain enough items that match contains");
            }
            if (node.max_contains && matched > *node.max_contains) {
                return fail(issue, node, "maxContains", "contains more items that match contains than maxContains allows");
            }
        }
        return true;
    }

    bool check_unique(const Node& node, const json::array_t& items, Issue* issue) {
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> buckets;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto hash = hash_value(items[i], limits_.max_value_depth);
            if (!hash) return stop_for_value_depth(node, "uniqueItems");
            auto& bucket = buckets[*hash];
            for (const std::size_t earlier : bucket) {
                switch (equal(items[earlier], items[i], limits_.max_value_depth)) {
                    case Equality::equal:
                        return fail(issue, node, "uniqueItems", [&] {
                            return "has duplicate items at indices " + std::to_string(earlier) + " and " +
                                   std::to_string(i);
                        });
                    case Equality::different: break;
                    case Equality::too_deep: return stop_for_value_depth(node, "uniqueItems");
                }
            }
            bucket.push_back(i);
        }
        return true;
    }

    bool check_object(const Node& node, const json& value, std::size_t depth, Annotations* here, Issue* issue) {
        const auto& members = *value.get_ptr<const json::object_t*>();
        const std::size_t size = members.size();
        if (node.min_properties && size < *node.min_properties) {
            return fail(issue, node, "minProperties", [&] {
                return "has fewer than minProperties " + std::to_string(*node.min_properties);
            });
        }
        if (node.max_properties && size > *node.max_properties) {
            return fail(issue, node, "maxProperties", [&] {
                return "has more than maxProperties " + std::to_string(*node.max_properties);
            });
        }
        const auto has = [&](const std::string& name) { return members.find(std::string_view(name)) != members.end(); };
        for (const std::string& name : node.required) {
            if (!has(name)) {
                return fail(issue, node, "required", [&] { return "is missing required property " + quote(name); });
            }
        }
        for (const auto& [trigger, names] : node.dependent_required) {
            if (!has(trigger)) continue;
            for (const std::string& name : names) {
                if (!has(name)) {
                    return fail(issue, node, "dependentRequired", [&] {
                        return "is missing property " + quote(name) + " required by " + quote(trigger);
                    });
                }
            }
        }

        std::vector<const Node*> by_pattern;
        std::size_t position = 0;
        for (const auto& member : members) {
            const std::string& key = member.first;
            const std::size_t index = position++;
            by_pattern.clear();
            for (const PatternProperty& entry : node.pattern_properties) {
                const Match match = search(*entry.pattern, key);
                if (match == Match::found) by_pattern.push_back(entry.schema);
                if (match == Match::too_long || match == Match::not_utf8) {
                    return stop_for_pattern(node, "patternProperties", match, "the property name " + quote(key));
                }
            }
            bool evaluated = false;
            {
                const PathGuard step(path_, Step{&key, 0});
                if (const auto declared = node.properties.find(std::string_view(key)); declared != node.properties.end()) {
                    if (!eval(*declared->second, member.second, depth + 1, nullptr, issue)) return false;
                    evaluated = true;
                }
                for (const Node* schema : by_pattern) {
                    if (!eval(*schema, member.second, depth + 1, nullptr, issue)) return false;
                    evaluated = true;
                }
                if (!evaluated && node.additional_properties) {
                    if (node.additional_properties->form == Node::Form::never) {
                        return fail(issue, node, "additionalProperties", "is an additional property that additionalProperties does not allow");
                    }
                    if (!eval(*node.additional_properties, member.second, depth + 1, nullptr, issue)) return false;
                    evaluated = true;
                }
            }
            if (evaluated && here) Annotations::mark(here->properties, index, size);
            if (node.property_names) {
                Issue inner;
                const json name(key);
                const bool ok = eval(*node.property_names, name, depth + 1, nullptr, issue ? &inner : nullptr);
                if (stopped_) return false;
                if (!ok) {
                    return fail(issue, node, "propertyNames", [&] {
                        return "has a property name " + quote(key) + " that propertyNames rejects: " + inner.message;
                    });
                }
            }
        }
        return true;
    }

    bool check_applicators(const Node& node, const json& value, std::size_t depth, Annotations* here, Issue* issue) {
        // A subschema that passes adds what it evaluated to `here`; one that fails adds nothing.
        for (const Node* schema : node.all_of) {
            if (!eval(*schema, value, depth + 1, here, issue)) return false;
        }
        if (!node.any_of.empty()) {
            bool matched = false;
            Issue first;
            bool have_first = false;
            for (const Node* schema : node.any_of) {
                Issue branch;
                const bool ok = eval(*schema, value, depth + 1, here, issue && !have_first ? &branch : nullptr);
                if (stopped_) return false;
                if (ok) {
                    matched = true;
                    // Annotations of every passing branch count, so keep going when they are tracked.
                    if (!here) break;
                } else if (issue && !have_first) {
                    first = std::move(branch);
                    have_first = true;
                }
            }
            if (!matched) {
                return fail(issue, node, "anyOf", [&] {
                    return "does not match any schema in anyOf (the first fails at " + quote(first.instance_pointer) +
                           " with " + quote(first.message) + ")";
                });
            }
        }
        if (!node.one_of.empty()) {
            std::size_t matched = 0;
            Issue first;
            bool have_first = false;
            for (const Node* schema : node.one_of) {
                Issue branch;
                const bool ok = eval(*schema, value, depth + 1, here, issue && !have_first ? &branch : nullptr);
                if (stopped_) return false;
                if (ok) {
                    if (++matched > 1) break;
                } else if (issue && !have_first) {
                    first = std::move(branch);
                    have_first = true;
                }
            }
            if (matched == 0) {
                return fail(issue, node, "oneOf", [&] {
                    return "does not match any schema in oneOf (the first fails at " + quote(first.instance_pointer) +
                           " with " + quote(first.message) + ")";
                });
            }
            if (matched > 1) return fail(issue, node, "oneOf", "matches more than one schema in oneOf");
        }
        if (node.not_schema) {
            const bool ok = eval(*node.not_schema, value, depth + 1, nullptr, nullptr);
            if (stopped_) return false;
            if (ok) return fail(issue, node, "not", "matches the schema in not");
        }
        if (node.if_schema) {
            const bool condition = eval(*node.if_schema, value, depth + 1, here, nullptr);
            if (stopped_) return false;
            const Node* branch = condition ? node.then_schema : node.else_schema;
            if (branch && !eval(*branch, value, depth + 1, here, issue)) return false;
        }
        if (value.is_object()) {
            for (const auto& [trigger, schema] : node.dependent_schemas) {
                const auto& members = *value.get_ptr<const json::object_t*>();
                if (members.find(std::string_view(trigger)) == members.end()) continue;
                if (!eval(*schema, value, depth + 1, here, issue)) return false;
            }
        }
        return true;
    }

    bool check_unevaluated(const Node& node, const json& value, std::size_t depth, Annotations* here, Issue* issue) {
        if (node.unevaluated_items && value.is_array() && here && !here->all_items) {
            const auto& items = *value.get_ptr<const json::array_t*>();
            for (std::size_t i = here->prefix_items; i < items.size(); ++i) {
                if (Annotations::marked(here->contained, i)) continue;
                const PathGuard step(path_, Step{nullptr, i});
                if (node.unevaluated_items->form == Node::Form::never) {
                    return fail(issue, node, "unevaluatedItems", "is an item that no keyword evaluated and unevaluatedItems does not allow");
                }
                if (!eval(*node.unevaluated_items, items[i], depth + 1, nullptr, issue)) return false;
            }
            here->all_items = true;
        }
        if (node.unevaluated_properties && value.is_object() && here) {
            const auto& members = *value.get_ptr<const json::object_t*>();
            std::size_t position = 0;
            for (const auto& member : members) {
                const std::size_t index = position++;
                if (Annotations::marked(here->properties, index)) continue;
                const PathGuard step(path_, Step{&member.first, 0});
                if (node.unevaluated_properties->form == Node::Form::never) {
                    return fail(issue, node, "unevaluatedProperties", "is a property that no keyword evaluated and unevaluatedProperties does not allow");
                }
                if (!eval(*node.unevaluated_properties, member.second, depth + 1, nullptr, issue)) return false;
                Annotations::mark(here->properties, index, members.size());
            }
        }
        return true;
    }

    const Compiled& compiled_;
    const Limits& limits_;
    std::vector<Step> path_;
    std::size_t evaluations_ = 0;
    std::optional<Issue> stopped_;
};

}  // namespace

std::optional<Issue> evaluate(const Compiled& compiled, const json& value) {
    try {
        return Run(compiled).validate(value);
    } catch (const std::exception& error) {
        return Issue{{}, {}, std::string("the value could not be checked: ") + error.what()};
    }
}

}  // namespace neomcp::json_schema::detail
