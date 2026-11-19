#include "neomcp/argument_headers.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace neomcp {
namespace {

constexpr std::array<std::string_view, 4> data_keywords{"const", "enum", "default", "examples"};
// Keywords whose value maps names to subschemas: the names are not keywords.
constexpr std::array<std::string_view, 4> schema_maps{"patternProperties", "$defs", "definitions", "dependentSchemas"};
constexpr std::string_view properties_keyword = "properties";

template <std::size_t N>
bool listed(const std::array<std::string_view, N>& words, std::string_view word) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

std::string escaped(std::string_view token) {
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

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// tchar of RFC 9110, section 5.6.2.
bool is_token_char(char c) noexcept {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    constexpr std::string_view others = "!#$%&'*+-.^_`|~";
    return others.find(c) != std::string_view::npos;
}

const json* member_of(const json::object_t& object, std::string_view name) {
    const auto found = object.find(std::string(name));
    return found == object.end() ? nullptr : &found->second;
}

class Reader {
public:
    Reader(std::string_view keyword, std::size_t max_depth) : keyword_(keyword), max_depth_(max_depth) {}

    std::optional<HeaderMarkIssue> read(const json& root) {
        walk(root, true, 0);
        if (issue_) return issue_;
        std::sort(found_.begin(), found_.end(),
                  [](const HeaderParameter& a, const HeaderParameter& b) { return a.path < b.path; });
        for (std::size_t i = 0; i < found_.size(); ++i) {
            for (std::size_t j = i + 1; j < found_.size(); ++j) {
                if (same_name(found_[i].name, found_[j].name)) {
                    return HeaderMarkIssue{found_[j].pointer + "/" + escaped(keyword_),
                                           "the header name '" + found_[j].name + "' repeats the one on " +
                                               found_[i].pointer + ", and names are unique without regard to case"};
                }
            }
        }
        return std::nullopt;
    }

    std::vector<HeaderParameter> take() { return std::move(found_); }

private:
    static bool same_name(const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
    }

    void refuse(std::string pointer, std::string rule) {
        if (!issue_) issue_ = HeaderMarkIssue{std::move(pointer), std::move(rule)};
    }

    // `reachable`: every step from the root to this schema was a member of `properties`.
    void walk(const json& schema, bool reachable, std::size_t depth) {
        if (issue_) return;
        const auto* object = schema.get_ptr<const json::object_t*>();
        if (object == nullptr) return;
        if (depth > max_depth_) {
            refuse(pointer_, "the schema nests deeper than " + std::to_string(max_depth_) + " levels");
            return;
        }
        if (const json* mark = member_of(*object, keyword_)) mark_at(*object, *mark, reachable);
        for (const auto& [key, value] : *object) {
            if (issue_) return;
            if (key == keyword_ || listed(data_keywords, key)) continue;
            const std::size_t length = pointer_.size();
            pointer_ += "/" + escaped(key);
            if (key == properties_keyword && value.is_object()) {
                for (const auto& [name, sub] : *value.get_ptr<const json::object_t*>()) {
                    const std::size_t inner = pointer_.size();
                    pointer_ += "/" + escaped(name);
                    path_.push_back(name);
                    walk(sub, reachable, depth + 1);
                    path_.pop_back();
                    pointer_.resize(inner);
                    if (issue_) break;
                }
            } else if (listed(schema_maps, key) && value.is_object()) {
                for (const auto& [name, sub] : *value.get_ptr<const json::object_t*>()) {
                    const std::size_t inner = pointer_.size();
                    pointer_ += "/" + escaped(name);
                    walk(sub, false, depth + 1);
                    pointer_.resize(inner);
                    if (issue_) break;
                }
            } else if (value.is_object()) {
                walk(value, false, depth + 1);
            } else if (const auto* items = value.get_ptr<const json::array_t*>()) {
                for (std::size_t i = 0; i < items->size() && !issue_; ++i) {
                    const std::size_t inner = pointer_.size();
                    pointer_ += "/" + std::to_string(i);
                    walk((*items)[i], false, depth + 1);
                    pointer_.resize(inner);
                }
            }
            pointer_.resize(length);
        }
    }

    void mark_at(const json::object_t& schema, const json& mark, bool reachable) {
        const std::string where = pointer_ + "/" + escaped(keyword_);
        if (!reachable) {
            refuse(where,
                   "the mark is not statically reachable: the chain from the root to it must consist of 'properties' "
                   "members only");
            return;
        }
        if (path_.empty()) {
            refuse(where, "the root of the schema cannot carry the mark, only a property can");
            return;
        }
        const auto* name = mark.get_ptr<const std::string*>();
        if (name == nullptr) {
            refuse(where, "the mark's value must be a string, the name portion of the header");
            return;
        }
        if (name->empty()) {
            refuse(where, "the mark's value must not be empty");
            return;
        }
        if (!std::all_of(name->begin(), name->end(), is_token_char)) {
            refuse(where, "the mark's value must be an HTTP token (letters, digits and !#$%&'*+-.^_`|~ only)");
            return;
        }
        const auto type = type_of(schema, where);
        if (!type) return;
        found_.push_back(HeaderParameter{*name, path_, *type, pointer_});
    }

    // The one primitive type the schema declares; "null" may be declared besides.
    std::optional<HeaderParameterType> type_of(const json::object_t& schema, const std::string& where) {
        const std::string wrong =
            "the property's 'type' must be integer, string or boolean (not number, and nothing else), alone or with "
            "'null'";
        const json* type = member_of(schema, "type");
        if (type == nullptr) {
            refuse(where, wrong);
            return std::nullopt;
        }
        std::vector<std::string> names;
        if (const auto* single = type->get_ptr<const std::string*>()) {
            names.push_back(*single);
        } else if (const auto* list = type->get_ptr<const json::array_t*>()) {
            for (const json& entry : *list) {
                const auto* text = entry.get_ptr<const std::string*>();
                if (text == nullptr) {
                    refuse(where, wrong);
                    return std::nullopt;
                }
                names.push_back(*text);
            }
        } else {
            refuse(where, wrong);
            return std::nullopt;
        }
        std::optional<HeaderParameterType> primitive;
        for (const std::string& name : names) {
            std::optional<HeaderParameterType> kind;
            if (name == "integer") {
                kind = HeaderParameterType::integer;
            } else if (name == "string") {
                kind = HeaderParameterType::string;
            } else if (name == "boolean") {
                kind = HeaderParameterType::boolean;
            } else if (name != "null") {
                refuse(where, wrong);
                return std::nullopt;
            }
            if (kind) {
                if (primitive) {
                    refuse(where, wrong);
                    return std::nullopt;
                }
                primitive = kind;
            }
        }
        if (!primitive) refuse(where, wrong);
        return primitive;
    }

    std::string_view keyword_;
    std::size_t max_depth_;
    std::string pointer_;
    std::vector<std::string> path_;
    std::vector<HeaderParameter> found_;
    std::optional<HeaderMarkIssue> issue_;
};

}  // namespace

std::variant<std::vector<HeaderParameter>, HeaderMarkIssue> header_parameters(const json& input_schema,
                                                                              std::string_view keyword,
                                                                              std::size_t max_depth) {
    Reader reader(keyword, max_depth);
    if (auto issue = reader.read(input_schema)) return std::move(*issue);
    return reader.take();
}

}  // namespace neomcp
