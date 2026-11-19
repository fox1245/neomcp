// Checking a schema document and compiling it into nodes.
//
// The compiler walks the document once through the keywords that hold subschemas and makes one node for every
// schema it meets, collecting `$id` resources and `$anchor` names on the way. References are resolved afterwards,
// because a reference may point forward or at an ancestor. A reference that points somewhere the walk did not go (a
// JSON Pointer into an unknown keyword, say) compiles the schema it finds there when it is first needed; ids and
// anchors found in such a place do not count as identifiers.
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/uri.h"
#include "schema/internal.h"
#include "schema/pattern.h"
#include "schema/value.h"

namespace neomcp::json_schema::detail {

std::string escape_token(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (const char c : token) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

namespace {

constexpr std::string_view dialect_uri = "https://json-schema.org/draft/2020-12/schema";

// Thrown inside the compiler at the first problem and turned into the result of `compile_schema`; it never leaves
// this file.
struct Failure {
    Issue issue;
};

[[noreturn]] void fail(std::string schema_pointer, std::string message) {
    throw Failure{Issue{{}, std::move(schema_pointer), std::move(message)}};
}

std::string child_pointer(const std::string& parent, std::string_view token) {
    return parent + "/" + escape_token(token);
}

const json* find_member(const json::object_t& object, std::string_view name) {
    const auto it = object.find(name);
    return it == object.end() ? nullptr : &it->second;
}

bool supported_dialect(const std::string& uri) {
    return uri == dialect_uri || (uri.size() == dialect_uri.size() + 1 && uri.starts_with(dialect_uri) && uri.back() == '#');
}

struct TypeName {
    std::string_view name;
    std::uint8_t bit;
};

constexpr std::array<TypeName, 7> type_names = {{{"null", type_bit::null},
                                                  {"boolean", type_bit::boolean},
                                                  {"object", type_bit::object},
                                                  {"array", type_bit::array},
                                                  {"number", type_bit::number},
                                                  {"integer", type_bit::integer},
                                                  {"string", type_bit::string}}};

// The pointer token an RFC 6901 reference token stands for; nothing when the escaping is malformed.
std::optional<std::string> unescape_token(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '~') {
            out.push_back(token[i]);
            continue;
        }
        if (i + 1 >= token.size()) return std::nullopt;
        if (token[i + 1] == '0') {
            out.push_back('~');
        } else if (token[i + 1] == '1') {
            out.push_back('/');
        } else {
            return std::nullopt;
        }
        ++i;
    }
    return out;
}

// An array index as RFC 6901 writes it: digits without a leading zero, and not larger than the array can hold.
std::optional<std::size_t> parse_index(std::string_view token, std::size_t size) {
    if (token.empty() || (token.size() > 1 && token.front() == '0')) return std::nullopt;
    constexpr auto largest = (std::numeric_limits<std::size_t>::max)();
    std::size_t index = 0;
    for (const char c : token) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto digit = static_cast<std::size_t>(c - '0');
        if (index > (largest - digit) / 10) return std::nullopt;
        index = index * 10 + digit;
    }
    if (index >= size) return std::nullopt;
    return index;
}

class Compiler {
public:
    Compiler(const Limits& limits, Compiled& out) : limits_(limits), out_(out) {}

    void run(const json& root) {
        // The root has the empty base unless it names itself with `$id`.
        const auto* object = root.get_ptr<const json::object_t*>();
        const json* id = object ? find_member(*object, "$id") : nullptr;
        if (!id || !id->is_string()) register_resource("", root, "", "");
        out_.root = node_for(root, Location{"", "", 0, true});
        resolve_references();
        reject_cycles();
    }

private:
    struct Location {
        std::string pointer;  // of the schema, in the document
        std::string base;     // the URI its references resolve against
        std::size_t depth;    // levels of subschemas below the schema the compiler started from
        bool register_ids;    // whether `$id` and `$anchor` here identify anything
    };

    struct Resource {
        const json* schema;
        std::string pointer;
    };

    struct Pending {
        Node* node;
        std::string reference;
        std::string base;
        std::string keyword_pointer;
    };

    Node* node_for(const json& schema, const Location& loc) {
        if (const auto known = nodes_.find(&schema); known != nodes_.end()) return known->second;
        if (loc.depth > limits_.max_schema_depth) {
            fail(loc.pointer, "schemas are nested deeper than " + std::to_string(limits_.max_schema_depth) +
                                  " levels");
        }
        Node& node = out_.nodes.emplace_back();
        nodes_.emplace(&schema, &node);
        node.pointer = loc.pointer;
        fill(node, schema, loc);
        return &node;
    }

    void register_resource(const std::string& key, const json& schema, const std::string& pointer,
                           const std::string& id_pointer) {
        if (!resources_.emplace(key, Resource{&schema, pointer}).second) {
            fail(id_pointer, "$id " + quote(key) + " is already the identifier of another schema");
        }
    }

    std::shared_ptr<const Pattern> compile_pattern(const std::string& source, const std::string& pointer) {
        if (const auto known = patterns_.find(source); known != patterns_.end()) return known->second;
        if (source.size() > limits_.max_pattern_length) {
            fail(pointer, "pattern is " + std::to_string(source.size()) +
                              " bytes long, longer than the limit max_pattern_length (" +
                              std::to_string(limits_.max_pattern_length) + ")");
        }
        auto compiled = compile_pattern_text(source, limits_);
        if (const auto* reason = std::get_if<std::string>(&compiled)) {
            fail(pointer, "pattern " + quote(source) + " " + *reason);
        }
        auto pattern = std::get<std::shared_ptr<const Pattern>>(std::move(compiled));
        patterns_.emplace(source, pattern);
        return pattern;
    }

    static std::optional<std::size_t> require_count(const json::object_t& object, std::string_view keyword,
                                                    const std::string& pointer) {
        const json* value = find_member(object, keyword);
        if (!value) return std::nullopt;
        const auto count = value->is_number() ? to_count(*value) : std::nullopt;
        if (!count) fail(child_pointer(pointer, keyword), std::string(keyword) + " must be a non-negative integer");
        return count;
    }

    static std::optional<json> require_number(const json::object_t& object, std::string_view keyword,
                                              const std::string& pointer) {
        const json* value = find_member(object, keyword);
        if (!value) return std::nullopt;
        if (!value->is_number()) fail(child_pointer(pointer, keyword), std::string(keyword) + " must be a number");
        return std::optional<json>(std::in_place, *value);
    }

    static std::uint8_t type_bit_of(const json& name, const std::string& pointer) {
        if (name.is_string()) {
            const auto& text = *name.get_ptr<const json::string_t*>();
            for (const auto& entry : type_names) {
                if (entry.name == text) return entry.bit;
            }
        }
        fail(pointer, "type must name a JSON type");
    }

    static std::vector<std::string> string_array(const json& value, const std::string& pointer,
                                                 std::string_view what) {
        const auto* array = value.get_ptr<const json::array_t*>();
        if (!array) fail(pointer, std::string(what) + " must be an array");
        std::vector<std::string> out;
        out.reserve(array->size());
        for (const auto& item : *array) {
            const auto* text = item.get_ptr<const json::string_t*>();
            if (!text) fail(pointer, std::string(what) + " must contain strings");
            out.push_back(*text);
        }
        return out;
    }

    // Compiling a schema recurses through `fill` and `fill_subschemas` only; the checks of the keywords that hold no
    // subschema are in `fill_keywords`, whose large frame is gone before the recursion starts.
    void fill(Node& node, const json& schema, const Location& loc) {
        if (schema.is_boolean()) {
            node.form = *schema.get_ptr<const json::boolean_t*>() ? Node::Form::always : Node::Form::never;
            return;
        }
        const auto* object = schema.get_ptr<const json::object_t*>();
        if (!object) fail(loc.pointer, "must be an object or a boolean");
        const std::string base = fill_keywords(node, schema, *object, loc);
        fill_subschemas(node, *object, loc, base);
    }

    // Everything but the subschemas. Returns the base that the schema's own references and subschemas resolve against.
    std::string fill_keywords(Node& node, const json& schema, const json::object_t& object, const Location& loc) {
        const auto keyword_pointer = [&](std::string_view keyword) { return child_pointer(loc.pointer, keyword); };

        // The base that this schema's own references and its subschemas resolve against.
        std::string base = loc.base;
        if (const json* id = find_member(object, "$id")) {
            const auto* text = id->get_ptr<const json::string_t*>();
            if (!text) fail(keyword_pointer("$id"), "$id must be a string");
            Uri resolved = resolve_uri(parse_uri(loc.base), parse_uri(*text));
            if (resolved.fragment && !resolved.fragment->empty()) {
                fail(keyword_pointer("$id"), "$id " + quote(*text) + " must not have a fragment");
            }
            resolved.fragment.reset();
            base = to_string(resolved);
            if (loc.register_ids) register_resource(base, schema, loc.pointer, keyword_pointer("$id"));
        }
        if (const json* dialect = find_member(object, "$schema")) {
            const auto* text = dialect->get_ptr<const json::string_t*>();
            if (!text || !supported_dialect(*text)) {
                fail(keyword_pointer("$schema"), "dialect " + dump_text(*dialect) + " is not supported (only " +
                                                     std::string(dialect_uri) + " is)");
            }
        }
        for (const std::string_view unsupported : {std::string_view("$dynamicRef"), std::string_view("$recursiveRef")}) {
            if (find_member(object, unsupported)) {
                fail(keyword_pointer(unsupported), std::string(unsupported) + " is not supported");
            }
        }
        // A `$dynamicAnchor` also names the schema for a plain `$ref`; only `$dynamicRef` is not supported.
        for (const std::string_view anchor_keyword : {std::string_view("$anchor"), std::string_view("$dynamicAnchor")}) {
            const json* anchor = find_member(object, anchor_keyword);
            if (!anchor) continue;
            const auto* name = anchor->get_ptr<const json::string_t*>();
            if (!name) fail(keyword_pointer(anchor_keyword), std::string(anchor_keyword) + " must be a string");
            if (loc.register_ids && !anchors_.emplace(base + "#" + *name, &schema).second) {
                fail(keyword_pointer(anchor_keyword),
                     std::string(anchor_keyword) + " " + quote(*name) + " is already used in this resource");
            }
        }
        if (const json* reference = find_member(object, "$ref")) {
            const auto* text = reference->get_ptr<const json::string_t*>();
            if (!text) fail(keyword_pointer("$ref"), "$ref must be a string");
            pending_.push_back(Pending{&node, *text, base, keyword_pointer("$ref")});
        }

        if (const json* type = find_member(object, "type")) {
            const auto* names = type->get_ptr<const json::array_t*>();
            if (names) {
                if (names->empty()) fail(keyword_pointer("type"), "type must not be empty");
                for (const auto& name : *names) node.types |= type_bit_of(name, keyword_pointer("type"));
            } else {
                node.types = type_bit_of(*type, keyword_pointer("type"));
            }
        }
        if (const json* value = find_member(object, "const")) node.const_value.emplace(*value);
        if (const json* options = find_member(object, "enum")) {
            const auto* array = options->get_ptr<const json::array_t*>();
            if (!array) fail(keyword_pointer("enum"), "enum must be an array");
            node.enum_values.emplace(*array);
        }
        if (const json* divisor = find_member(object, "multipleOf")) {
            const bool finite = divisor->type() != json::value_t::number_float ||
                                std::isfinite(*divisor->get_ptr<const json::number_float_t*>());
            if (!divisor->is_number() || !finite || compare_numbers(*divisor, json(0)) != Order::greater) {
                fail(keyword_pointer("multipleOf"), "multipleOf must be a number greater than 0");
            }
            node.multiple_of.emplace(*divisor);
        }
        node.maximum = require_number(object, "maximum", loc.pointer);
        node.exclusive_maximum = require_number(object, "exclusiveMaximum", loc.pointer);
        node.minimum = require_number(object, "minimum", loc.pointer);
        node.exclusive_minimum = require_number(object, "exclusiveMinimum", loc.pointer);
        node.max_length = require_count(object, "maxLength", loc.pointer);
        node.min_length = require_count(object, "minLength", loc.pointer);
        node.max_items = require_count(object, "maxItems", loc.pointer);
        node.min_items = require_count(object, "minItems", loc.pointer);
        node.max_contains = require_count(object, "maxContains", loc.pointer);
        node.min_contains = require_count(object, "minContains", loc.pointer);
        node.max_properties = require_count(object, "maxProperties", loc.pointer);
        node.min_properties = require_count(object, "minProperties", loc.pointer);
        if (const json* pattern = find_member(object, "pattern")) {
            const auto* text = pattern->get_ptr<const json::string_t*>();
            if (!text) fail(keyword_pointer("pattern"), "pattern must be a string");
            node.pattern = compile_pattern(*text, keyword_pointer("pattern"));
        }
        if (const json* unique = find_member(object, "uniqueItems")) {
            const auto* flag = unique->get_ptr<const json::boolean_t*>();
            if (!flag) fail(keyword_pointer("uniqueItems"), "uniqueItems must be a boolean");
            node.unique_items = *flag;
        }
        if (const json* required = find_member(object, "required")) {
            node.required = string_array(*required, keyword_pointer("required"), "required");
        }
        if (const json* dependent = find_member(object, "dependentRequired")) {
            const auto* map = dependent->get_ptr<const json::object_t*>();
            if (!map) fail(keyword_pointer("dependentRequired"), "dependentRequired must be an object");
            for (const auto& [name, names] : *map) {
                if (!names.is_array()) {
                    fail(child_pointer(keyword_pointer("dependentRequired"), name),
                         "dependentRequired values must be arrays");
                }
                node.dependent_required.emplace_back(
                    name, string_array(names, child_pointer(keyword_pointer("dependentRequired"), name),
                                       "dependentRequired values"));
            }
        }
        if (const json* items = find_member(object, "items"); items && items->is_array()) {
            fail(keyword_pointer("items"), "items must be a schema (2020-12 describes tuples with prefixItems)");
        }

        return base;
    }

    // The keywords that hold subschemas, in a fixed order so that the first problem found is the same each time.
    void fill_subschemas(Node& node, const json::object_t& object, const Location& loc, const std::string& base) {
        const auto keyword_pointer = [&](std::string_view keyword) { return child_pointer(loc.pointer, keyword); };
        const auto below = [&](std::string pointer) {
            return Location{std::move(pointer), base, loc.depth + 1, loc.register_ids};
        };
        const auto one = [&](std::string_view keyword) -> const Node* {
            const json* value = find_member(object, keyword);
            return value ? node_for(*value, below(keyword_pointer(keyword))) : nullptr;
        };
        const auto each_in_map = [&](std::string_view keyword, auto&& sink) {
            const json* value = find_member(object, keyword);
            if (!value) return;
            const auto* map = value->get_ptr<const json::object_t*>();
            if (!map) fail(keyword_pointer(keyword), std::string(keyword) + " must be an object");
            for (const auto& [name, child] : *map) {
                const std::string pointer = child_pointer(keyword_pointer(keyword), name);
                sink(name, pointer, [&] { return static_cast<const Node*>(node_for(child, below(pointer))); });
            }
        };
        const auto list = [&](std::string_view keyword) {
            std::vector<const Node*> nodes;
            const json* value = find_member(object, keyword);
            if (!value) return nodes;
            const auto* array = value->get_ptr<const json::array_t*>();
            if (!array || array->empty()) fail(keyword_pointer(keyword), std::string(keyword) + " must be a non-empty array");
            for (std::size_t i = 0; i < array->size(); ++i) {
                nodes.push_back(node_for((*array)[i], below(child_pointer(keyword_pointer(keyword), std::to_string(i)))));
            }
            return nodes;
        };

        each_in_map("$defs", [](const std::string&, const std::string&, auto&& compile) { compile(); });
        each_in_map("definitions", [](const std::string&, const std::string&, auto&& compile) { compile(); });
        each_in_map("properties", [&](const std::string& name, const std::string&, auto&& compile) {
            node.properties.emplace(name, compile());
        });
        each_in_map("patternProperties", [&](const std::string& name, const std::string& pointer, auto&& compile) {
            auto pattern = compile_pattern(name, pointer);
            node.pattern_properties.push_back(PatternProperty{std::move(pattern), compile()});
        });
        each_in_map("dependentSchemas", [&](const std::string& name, const std::string&, auto&& compile) {
            node.dependent_schemas.emplace_back(name, compile());
        });
        node.additional_properties = one("additionalProperties");
        node.unevaluated_properties = one("unevaluatedProperties");
        node.property_names = one("propertyNames");
        node.items = one("items");
        node.unevaluated_items = one("unevaluatedItems");
        node.contains = one("contains");
        node.not_schema = one("not");
        node.if_schema = one("if");
        node.then_schema = one("then");
        node.else_schema = one("else");
        node.prefix_items = list("prefixItems");
        node.all_of = list("allOf");
        node.any_of = list("anyOf");
        node.one_of = list("oneOf");

        if (node.unevaluated_items || node.unevaluated_properties) out_.track_annotations = true;
    }

    // ---- references ----------------------------------------------------------------------------------------------

    void resolve_references() {
        // Resolving can compile more schemas, which adds more references; index instead of iterating.
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            const Pending pending = pending_[i];
            pending.node->ref = resolve(pending);
        }
    }

    [[noreturn]] static void unresolved(const Pending& pending) {
        fail(pending.keyword_pointer, "$ref " + quote(pending.reference) +
                                          " does not resolve inside this schema (only references to schemas of the "
                                          "same document are supported)");
    }

    const Node* resolve(const Pending& pending) {
        const Uri target = resolve_uri(parse_uri(pending.base), parse_uri(pending.reference));
        const std::string key = without_fragment(target);
        const auto resource = resources_.find(key);
        if (resource == resources_.end()) unresolved(pending);
        const std::string fragment = percent_decode(target.fragment.value_or(""));
        if (fragment.empty()) return nodes_.at(resource->second.schema);
        if (fragment.front() != '/') {
            const auto anchor = anchors_.find(key + "#" + fragment);
            if (anchor == anchors_.end()) unresolved(pending);
            return nodes_.at(anchor->second);
        }
        return walk(pending, resource->second, key, target.fragment.value_or(""));
    }

    // Follow a JSON Pointer fragment from the root of a resource.
    const Node* walk(const Pending& pending, const Resource& resource, const std::string& resource_base,
                     const std::string& raw_fragment) {
        const json* current = resource.schema;
        std::string pointer = resource.pointer;
        std::string base = resource_base;
        std::size_t start = 1;
        for (;;) {
            // An `$id` on the way changes what the schemas below it resolve against.
            if (current != resource.schema) {
                if (const auto* object = current->get_ptr<const json::object_t*>()) {
                    if (const json* id = find_member(*object, "$id"); id && id->is_string()) {
                        Uri resolved = resolve_uri(parse_uri(base), parse_uri(*id->get_ptr<const json::string_t*>()));
                        resolved.fragment.reset();
                        base = to_string(resolved);
                    }
                }
            }
            auto end = raw_fragment.find('/', start);
            const std::string_view encoded = std::string_view(raw_fragment).substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            const auto token = unescape_token(percent_decode(encoded));
            if (!token) unresolved(pending);
            if (const auto* object = current->get_ptr<const json::object_t*>()) {
                current = find_member(*object, *token);
                if (!current) unresolved(pending);
            } else if (const auto* array = current->get_ptr<const json::array_t*>()) {
                const auto index = parse_index(*token, array->size());
                if (!index) unresolved(pending);
                current = &(*array)[*index];
            } else {
                unresolved(pending);
            }
            pointer = child_pointer(pointer, *token);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (const auto known = nodes_.find(current); known != nodes_.end()) return known->second;
        return node_for(*current, Location{pointer, base, 0, false});
    }

    // ---- cycles --------------------------------------------------------------------------------------------------

    struct Edge {
        const Node* to;
        bool is_ref;
    };

    // The schemas that are applied to the very value this schema is applied to. A path along these edges that comes
    // back to where it began applies a schema to the same value forever.
    static std::vector<Edge> in_place_edges(const Node& node) {
        std::vector<Edge> edges;
        if (node.ref) edges.push_back({node.ref, true});
        for (const auto* list : {&node.all_of, &node.any_of, &node.one_of}) {
            for (const Node* child : *list) edges.push_back({child, false});
        }
        for (const Node* child : {node.not_schema, node.if_schema, node.then_schema, node.else_schema}) {
            if (child) edges.push_back({child, false});
        }
        for (const auto& entry : node.dependent_schemas) edges.push_back({entry.second, false});
        return edges;
    }

    void reject_cycles() {
        struct Frame {
            const Node* node;
            std::vector<Edge> edges;
            std::size_t next;
        };
        enum class Mark : std::uint8_t { unseen, open, done };
        std::unordered_map<const Node*, Mark> marks;
        std::unordered_map<const Node*, std::size_t> position;
        for (const Node& start : out_.nodes) {
            if (marks[&start] != Mark::unseen) continue;
            std::vector<Frame> stack;
            stack.push_back({&start, in_place_edges(start), 0});
            marks[&start] = Mark::open;
            position[&start] = 0;
            while (!stack.empty()) {
                Frame& top = stack.back();
                if (top.next == top.edges.size()) {
                    marks[top.node] = Mark::done;
                    stack.pop_back();
                    continue;
                }
                const Edge edge = top.edges[top.next++];
                const Mark mark = marks[edge.to];
                if (mark == Mark::done) continue;
                if (mark == Mark::open) {
                    // The stack from the target of this edge up to the top is the cycle. Name a reference in it.
                    for (std::size_t i = position[edge.to]; i < stack.size(); ++i) {
                        const Frame& frame = stack[i];
                        const Edge& taken = frame.edges[frame.next - 1];
                        if (taken.is_ref) {
                            fail(child_pointer(frame.node->pointer, "$ref"),
                                 "$ref is part of a cycle of references that applies a schema to the same value "
                                 "again and never makes progress");
                        }
                    }
                    fail(child_pointer(edge.to->pointer, "$ref"), "cycle of references without progress");
                }
                marks[edge.to] = Mark::open;
                position[edge.to] = stack.size();
                stack.push_back({edge.to, in_place_edges(*edge.to), 0});
            }
        }
    }

    const Limits& limits_;
    Compiled& out_;
    std::unordered_map<const json*, Node*> nodes_;
    std::map<std::string, Resource> resources_;
    std::map<std::string, const json*> anchors_;
    std::map<std::string, std::shared_ptr<const Pattern>> patterns_;
    std::vector<Pending> pending_;
};

}  // namespace

std::variant<std::shared_ptr<const Compiled>, Issue> compile_schema(const json& schema, const Limits& limits) {
    try {
        auto compiled = std::make_shared<Compiled>();
        compiled->limits = limits;
        Compiler(limits, *compiled).run(schema);
        return std::shared_ptr<const Compiled>(std::move(compiled));
    } catch (const Failure& failure) {
        return failure.issue;
    } catch (const std::exception& error) {
        return Issue{{}, {}, std::string("the schema could not be compiled: ") + error.what()};
    }
}

}  // namespace neomcp::json_schema::detail
