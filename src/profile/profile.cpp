#include "neomcp/profile.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include <neojson/json.hpp>

#include "profile/internal.h"

namespace neomcp::profile {

std::string_view to_string(Slot slot) noexcept {
    switch (slot) {
        case Slot::open: return "open";
        case Slot::state: return "state";
        case Slot::server_input: return "server_input";
        case Slot::change_feed: return "change_feed";
        case Slot::batch: return "batch";
    }
    return {};
}

std::string_view to_string(Era era) noexcept {
    switch (era) {
        case Era::legacy: return "legacy";
        case Era::modern: return "modern";
    }
    return {};
}

std::optional<std::string_view> RequestHeader::params_member(std::string_view method) const {
    if (scope != HeaderScopeKind::per_method) return std::nullopt;
    for (const auto& entry : methods) {
        if (entry.method == method) return std::string_view(entry.member);
    }
    return std::nullopt;
}

bool Type::has_property(std::string_view property) const {
    return std::binary_search(properties.begin(), properties.end(), property);
}

bool Type::is_required(std::string_view property) const {
    return std::binary_search(required.begin(), required.end(), property);
}

std::string_view Profile::mechanism(Slot slot) const noexcept {
    return mechanisms_[static_cast<std::size_t>(slot)];
}

std::optional<int> Profile::error_code(std::string_view name) const {
    const auto it = std::lower_bound(error_codes_.begin(), error_codes_.end(), name,
                                     [](const NamedCode& entry, std::string_view key) { return entry.name < key; });
    if (it == error_codes_.end() || it->name != name) return std::nullopt;
    return it->code;
}

namespace {

template <typename Entry>
const Entry* find_by_method(const std::vector<Entry>& table, std::string_view method) {
    const auto it = std::lower_bound(table.begin(), table.end(), method,
                                     [](const Entry& entry, std::string_view key) { return entry.method < key; });
    if (it == table.end() || it->method != method) return nullptr;
    return &*it;
}

}  // namespace

const Message* Profile::request(std::string_view method) const { return find_by_method(requests_, method); }
const Message* Profile::notification(std::string_view method) const {
    return find_by_method(notifications_, method);
}
const InputRequest* Profile::input_request(std::string_view method) const {
    return find_by_method(input_requests_, method);
}

const Type* Profile::type(std::string_view name) const {
    const auto it = std::lower_bound(types_.begin(), types_.end(), name,
                                     [](const Type& entry, std::string_view key) { return entry.name < key; });
    if (it == types_.end() || it->name != name) return nullptr;
    return &*it;
}

const Union* Profile::union_type(std::string_view name) const {
    const auto it = std::lower_bound(unions_.begin(), unions_.end(), name,
                                     [](const Union& entry, std::string_view key) { return entry.name < key; });
    if (it == unions_.end() || it->name != name) return nullptr;
    return &*it;
}

// ---------------------------------------------------------------------------------------------------------------
// Loading

namespace {

using Json = neojson::json;

// Thrown inside the loader at the first problem and turned into the result of `load`; it never leaves this file.
struct Failure {
    Error error;
};

[[noreturn]] void fail(std::string pointer, std::string expected, std::string message) {
    throw Failure{Error{std::move(pointer), std::move(expected), std::move(message)}};
}

std::string escape_token(std::string_view token) {
    std::string out;
    for (const char c : token) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string join(std::span<const std::string_view> names) {
    std::string out;
    for (const auto name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// A value of the document with the pointer of its place.
class Node {
public:
    Node(const Json& value, std::string pointer) : value_(&value), pointer_(std::move(pointer)) {}

    const Json& value() const { return *value_; }
    const std::string& pointer() const { return pointer_; }

    Node member(const Json& child, std::string_view name) const {
        return Node(child, pointer_ + "/" + escape_token(name));
    }
    Node element(const Json& child, std::size_t index) const { return Node(child, pointer_ + "/" + std::to_string(index)); }
    std::string member_pointer(std::string_view name) const { return pointer_ + "/" + escape_token(name); }

private:
    const Json* value_;
    std::string pointer_;
};

std::string found(const Node& node) { return std::string("found ") + node.value().type_name(); }

const Json::object_t& object_of(const Node& node) {
    const auto* object = node.value().get_ptr<const Json::object_t*>();
    if (object == nullptr) fail(node.pointer(), "an object", found(node));
    return *object;
}

const Json::array_t& array_of(const Node& node) {
    const auto* array = node.value().get_ptr<const Json::array_t*>();
    if (array == nullptr) fail(node.pointer(), "an array", found(node));
    return *array;
}

const std::string& string_of(const Node& node) {
    const auto* text = node.value().get_ptr<const std::string*>();
    if (text == nullptr) fail(node.pointer(), "a string", found(node));
    return *text;
}

const std::string& nonempty_string_of(const Node& node) {
    const std::string& text = string_of(node);
    if (text.empty()) fail(node.pointer(), "a non-empty string", "found an empty string");
    return text;
}

bool bool_of(const Node& node) {
    const auto* flag = node.value().get_ptr<const bool*>();
    if (flag == nullptr) fail(node.pointer(), "a boolean", found(node));
    return *flag;
}

int int_of(const Node& node) {
    const Json& value = node.value();
    // neojson reports an unsigned value as an integer too, so the unsigned case is read first.
    if (value.is_number_unsigned()) {
        const std::uint64_t number = *value.get_ptr<const std::uint64_t*>();
        if (number > static_cast<std::uint64_t>(INT_MAX)) {
            fail(node.pointer(), "an integer that fits in a 32-bit signed integer", "found " + std::to_string(number));
        }
        return static_cast<int>(number);
    }
    if (value.is_number_integer()) {
        const std::int64_t number = *value.get_ptr<const std::int64_t*>();
        if (number < INT_MIN || number > INT_MAX) {
            fail(node.pointer(), "an integer that fits in a 32-bit signed integer", "found " + std::to_string(number));
        }
        return static_cast<int>(number);
    }
    fail(node.pointer(), "an integer", found(node));
}

std::vector<std::string> strings_of(const Node& node, bool distinct) {
    const auto& array = array_of(node);
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < array.size(); ++i) {
        const Node item = node.element(array[i], i);
        const std::string& text = string_of(item);
        if (distinct && !seen.insert(text).second) {
            fail(item.pointer(), "distinct strings", "'" + text + "' repeats an earlier entry");
        }
        out.push_back(text);
    }
    return out;
}

// A string that must be one of a fixed set of spellings, each standing for a value.
template <typename Value, std::size_t N>
Value pick(const Node& node, const std::array<std::pair<std::string_view, Value>, N>& table) {
    std::string expected = "one of: ";
    for (std::size_t i = 0; i < N; ++i) {
        if (i != 0) expected += ", ";
        expected += table[i].first;
    }
    const auto* text = node.value().get_ptr<const std::string*>();
    if (text == nullptr) fail(node.pointer(), expected, found(node));
    for (const auto& entry : table) {
        if (entry.first == *text) return entry.second;
    }
    fail(node.pointer(), expected, "found '" + *text + "'");
}

// A string that must be a name the library declares.
std::string_view declared_name(const Node& node, std::span<const std::string_view> names) {
    const std::string expected = "one of the declared names: " + join(names);
    const auto* text = node.value().get_ptr<const std::string*>();
    if (text == nullptr) fail(node.pointer(), expected, found(node));
    for (const auto name : names) {
        if (name == *text) return name;
    }
    fail(node.pointer(), expected, "found '" + *text + "'");
}

struct MemberSpec {
    std::string_view name;
    bool required;
};

// An object with a fixed set of members: any other member is rejected, and so is a missing required member.
class Object {
public:
    Object(const Node& node, const std::vector<MemberSpec>& members) : node_(node), map_(&object_of(node)) {
        for (const auto& entry : *map_) {
            const auto known = std::find_if(members.begin(), members.end(),
                                            [&](const MemberSpec& spec) { return spec.name == entry.first; });
            if (known == members.end()) {
                std::string allowed;
                for (const auto& spec : members) {
                    if (!allowed.empty()) allowed += ", ";
                    allowed += spec.name;
                }
                fail(node.member_pointer(entry.first), "one of the members: " + allowed,
                     "unknown member '" + entry.first + "'");
            }
        }
        for (const auto& spec : members) {
            if (spec.required && map_->find(std::string(spec.name)) == map_->end()) {
                fail(node.member_pointer(spec.name), "a member named '" + std::string(spec.name) + "'",
                     "the member is missing");
            }
        }
    }

    bool empty() const { return map_->empty(); }

    // A member the spec declared required.
    Node at(std::string_view name) const { return *find(name); }

    std::optional<Node> find(std::string_view name) const {
        const auto it = map_->find(std::string(name));
        if (it == map_->end()) return std::nullopt;
        return node_.member(it->second, name);
    }

private:
    Node node_;
    const Json::object_t* map_;
};

// The entries of an object whose member names are data (methods, type names, error names).
template <typename Visit>
void for_each_entry(const Node& node, Visit&& visit) {
    for (const auto& entry : object_of(node)) {
        if (entry.first.empty()) fail(node.member_pointer(entry.first), "a non-empty member name", "found an empty name");
        visit(entry.first, node.member(entry.second, entry.first));
    }
}

}  // namespace

class ProfileLoader {
public:
    static ProfilePtr run(const Json& document) {
        ProfileLoader loader;
        loader.root(Node(document, ""));
        return std::make_shared<const Profile>(std::move(loader.profile_));
    }

private:
    Profile profile_;

    void root(const Node& node) {
        const Object object(node, {{"profile_version", true},
                                   {"revision", true},
                                   {"era", true},
                                   {"evidence", true},
                                   {"mechanisms", true},
                                   {"transports", true},
                                   {"errors", true},
                                   {"wire", true}});

        const Node version = object.at("profile_version");
        if (const int number = int_of(version); number != 1) {
            fail(version.pointer(), "1", "found " + std::to_string(number));
        }
        profile_.revision_ = nonempty_string_of(object.at("revision"));
        static constexpr std::array<std::pair<std::string_view, Era>, 2> eras{
            {{"legacy", Era::legacy}, {"modern", Era::modern}}};
        profile_.era_ = pick(object.at("era"), eras);

        evidence(object.at("evidence"));
        mechanisms(object.at("mechanisms"));
        errors(object.at("errors"));
        wire(object.at("wire"));
        transports(object.at("transports"));
    }

    void evidence(const Node& node) {
        const Object object(node, {{"verified_at", true},
                                   {"repository", true},
                                   {"commit", true},
                                   {"pages", true},
                                   {"schema", true}});
        Evidence& evidence = profile_.evidence_;
        evidence.verified_at = nonempty_string_of(object.at("verified_at"));
        evidence.repository = nonempty_string_of(object.at("repository"));
        evidence.commit = nonempty_string_of(object.at("commit"));
        evidence.pages = strings_of(object.at("pages"), false);
        evidence.schema = nonempty_string_of(object.at("schema"));
    }

    void mechanisms(const Node& node) {
        std::vector<MemberSpec> members;
        for (std::size_t i = 0; i < slot_count; ++i) members.push_back({to_string(static_cast<Slot>(i)), true});
        const Object object(node, members);
        profile_.mechanisms_.clear();
        for (std::size_t i = 0; i < slot_count; ++i) {
            const Slot slot = static_cast<Slot>(i);
            profile_.mechanisms_.push_back(declared_name(object.at(to_string(slot)), detail::declared_mechanisms(slot)));
        }
    }

    void errors(const Node& node) {
        for_each_entry(node, [&](const std::string& name, const Node& value) {
            if (name == "not_emitted") {
                const auto& array = array_of(value);
                for (std::size_t i = 0; i < array.size(); ++i) {
                    profile_.not_emitted_.push_back(int_of(value.element(array[i], i)));
                }
            } else {
                profile_.error_codes_.push_back(NamedCode{name, int_of(value)});
            }
        });
        std::sort(profile_.error_codes_.begin(), profile_.error_codes_.end(),
                  [](const NamedCode& a, const NamedCode& b) { return a.name < b.name; });
    }

    // ----- wire

    void wire(const Node& node) {
        const Object object(node, {{"schema", true},
                                   {"requests", true},
                                   {"notifications", true},
                                   {"input_requests", true},
                                   {"unlisted_message_types", true},
                                   {"types", true},
                                   {"unions", true}});

        const Object schema(object.at("schema"), {{"dialect", true}, {"sha256", true}});
        profile_.wire_schema_.dialect = nonempty_string_of(schema.at("dialect"));
        profile_.wire_schema_.sha256 = nonempty_string_of(schema.at("sha256"));

        types(object.at("types"));
        unions(object.at("unions"));
        messages(object.at("requests"), profile_.requests_);
        messages(object.at("notifications"), profile_.notifications_);
        input_requests(object.at("input_requests"));
        profile_.unlisted_message_types_ = strings_of(object.at("unlisted_message_types"), true);

        check_params("requests", profile_.requests_);
        check_params("notifications", profile_.notifications_);
        check_params("input_requests", profile_.input_requests_);
    }

    void types(const Node& node) {
        for_each_entry(node, [&](const std::string& name, const Node& value) {
            const Object object(value, {{"properties", true}, {"required", true}});
            Type type;
            type.name = name;
            type.properties = strings_of(object.at("properties"), true);
            const Node required = object.at("required");
            type.required = strings_of(required, true);
            const auto& items = array_of(required);
            for (std::size_t i = 0; i < type.required.size(); ++i) {
                if (std::find(type.properties.begin(), type.properties.end(), type.required[i]) ==
                    type.properties.end()) {
                    fail(required.element(items[i], i).pointer(), "a member of this type's properties",
                         "'" + type.required[i] + "' is not a property of " + name);
                }
            }
            std::sort(type.properties.begin(), type.properties.end());
            std::sort(type.required.begin(), type.required.end());
            profile_.types_.push_back(std::move(type));
        });
        std::sort(profile_.types_.begin(), profile_.types_.end(),
                  [](const Type& a, const Type& b) { return a.name < b.name; });
    }

    // What every entry of the message tables has besides its senders.
    struct Entry {
        std::string type;
        std::optional<std::string> params;
        std::vector<std::string> params_required;
    };

    static Entry entry_of(const Object& object) {
        Entry entry;
        entry.type = nonempty_string_of(object.at("type"));
        const Node params = object.at("params");
        if (!params.value().is_null()) entry.params = nonempty_string_of(params);
        entry.params_required = strings_of(object.at("params_required"), true);
        std::sort(entry.params_required.begin(), entry.params_required.end());
        return entry;
    }

    void messages(const Node& node, std::vector<Message>& table) {
        for_each_entry(node, [&](const std::string& method, const Node& value) {
            const Object object(value, {{"from", true}, {"type", true}, {"params", true}, {"params_required", true}});
            Message message;
            message.method = method;

            const Node from = object.at("from");
            const auto& senders = array_of(from);
            if (senders.empty()) fail(from.pointer(), "a non-empty array", "found an empty array");
            static constexpr std::array<std::pair<std::string_view, bool>, 2> sender_names{
                {{"client", true}, {"server", false}}};
            for (std::size_t i = 0; i < senders.size(); ++i) {
                const Node sender = from.element(senders[i], i);
                const bool is_client = pick(sender, sender_names);
                bool& flag = is_client ? message.from_client : message.from_server;
                if (flag) fail(sender.pointer(), "distinct senders", "'" + string_of(sender) + "' repeats an earlier entry");
                flag = true;
            }

            Entry entry = entry_of(object);
            message.type = std::move(entry.type);
            message.params = std::move(entry.params);
            message.params_required = std::move(entry.params_required);
            table.push_back(std::move(message));
        });
        std::sort(table.begin(), table.end(), [](const Message& a, const Message& b) { return a.method < b.method; });
    }

    void input_requests(const Node& node) {
        for_each_entry(node, [&](const std::string& method, const Node& value) {
            const Object object(value, {{"type", true}, {"params", true}, {"params_required", true}});
            Entry entry = entry_of(object);
            profile_.input_requests_.push_back(
                InputRequest{method, std::move(entry.type), std::move(entry.params), std::move(entry.params_required)});
        });
        std::sort(profile_.input_requests_.begin(), profile_.input_requests_.end(),
                  [](const InputRequest& a, const InputRequest& b) { return a.method < b.method; });
    }

    // A name is an object type or a union of other names, never both; every alternative of a union is a name of
    // the types or of the unions, and no union contains itself, directly or through other unions.
    void unions(const Node& node) {
        std::vector<Union> table;
        for_each_entry(node, [&](const std::string& name, const Node& value) {
            if (profile_.type(name) != nullptr) {
                fail(value.pointer(), "a name that is not also an entry of wire.types",
                     "'" + name + "' is in wire.types and in wire.unions");
            }
            const auto& array = array_of(value);
            if (array.empty()) fail(value.pointer(), "a non-empty array of alternatives", "found an empty array");
            Union entry;
            entry.name = name;
            entry.alternatives = strings_of(value, true);
            for (std::size_t i = 0; i < entry.alternatives.size(); ++i) {
                if (entry.alternatives[i].empty()) {
                    fail(value.element(array[i], i).pointer(), "a non-empty string", "found an empty string");
                }
            }
            table.push_back(std::move(entry));
        });

        std::map<std::string, std::size_t> position;
        for (std::size_t i = 0; i < table.size(); ++i) position[table[i].name] = i;
        for (const auto& entry : table) {
            for (std::size_t i = 0; i < entry.alternatives.size(); ++i) {
                const std::string& alternative = entry.alternatives[i];
                if (profile_.type(alternative) == nullptr && position.find(alternative) == position.end()) {
                    fail(node.member_pointer(entry.name) + "/" + std::to_string(i),
                         "a name of wire.types or wire.unions", "'" + alternative + "' names neither");
                }
            }
        }

        std::vector<int> state(table.size(), 0);  // 0 not visited, 1 on the current path, 2 done
        const std::function<void(std::size_t)> visit = [&](std::size_t index) {
            state[index] = 1;
            const Union& entry = table[index];
            for (std::size_t i = 0; i < entry.alternatives.size(); ++i) {
                const auto next = position.find(entry.alternatives[i]);
                if (next == position.end()) continue;
                if (state[next->second] == 1) {
                    fail(node.member_pointer(entry.name) + "/" + std::to_string(i),
                         "an alternative that does not lead back to '" + entry.name + "'",
                         "'" + entry.alternatives[i] + "' contains '" + entry.name + "'");
                }
                if (state[next->second] == 0) visit(next->second);
            }
            state[index] = 2;
        };
        for (std::size_t i = 0; i < table.size(); ++i) {
            if (state[i] == 0) visit(i);
        }

        std::sort(table.begin(), table.end(), [](const Union& a, const Union& b) { return a.name < b.name; });
        profile_.unions_ = std::move(table);
    }

    // What each union requires, kept so that a union several others share is worked out once: the number of paths
    // through shared unions grows exponentially with their depth.
    std::map<std::string, std::vector<std::string>, std::less<>> union_required_;

    // The members every object type behind a name requires: the type's own, or for a union what all of its
    // alternatives require. The unions are known to be acyclic.
    const std::vector<std::string>& required_of(const std::string& name) {
        if (const Type* type = profile_.type(name)) return type->required;
        if (const auto known = union_required_.find(name); known != union_required_.end()) return known->second;
        std::vector<std::string> common;
        bool first = true;
        for (const auto& alternative : profile_.union_type(name)->alternatives) {
            const std::vector<std::string>& required = required_of(alternative);
            if (first) {
                common = required;
                first = false;
            } else {
                std::vector<std::string> both;
                std::set_intersection(common.begin(), common.end(), required.begin(), required.end(),
                                      std::back_inserter(both));
                common = std::move(both);
            }
        }
        return union_required_.emplace(name, std::move(common)).first->second;
    }

    // The object types behind a name, each once: the type itself, or for a union the types of all of its
    // alternatives. `visited` holds the names already followed.
    void object_types(const std::string& name, std::vector<const Type*>& out, std::set<std::string>& visited) const {
        if (!visited.insert(name).second) return;
        if (const Type* type = profile_.type(name)) {
            out.push_back(type);
            return;
        }
        for (const auto& alternative : profile_.union_type(name)->alternatives) {
            object_types(alternative, out, visited);
        }
    }

    // A message that names a params type has the members that type requires; for a union, the members every
    // alternative requires.
    template <typename Table>
    void check_params(std::string_view table_name, const Table& table) {
        for (const auto& message : table) {
            if (!message.params) continue;
            const std::string entry_pointer = "/wire/" + std::string(table_name) + "/" + escape_token(message.method);
            if (profile_.type(*message.params) == nullptr && profile_.union_type(*message.params) == nullptr) {
                fail(entry_pointer + "/params", "a name of wire.types or wire.unions",
                     "'" + *message.params + "' names neither");
            }
            const std::vector<std::string>& required = required_of(*message.params);
            if (message.params_required != required) {
                fail(entry_pointer + "/params_required",
                     "the members every alternative of " + *message.params + " requires: " + list(required),
                     "found " + list(message.params_required));
            }
        }
    }

    static std::string list(const std::vector<std::string>& names) {
        std::string out = "[";
        for (const auto& name : names) {
            if (out.size() > 1) out += ", ";
            out += name;
        }
        return out + "]";
    }

    // ----- transports

    void transports(const Node& node) {
        const Object object(node, {{"stdio", false}, {"streamable_http", false}, {"http_sse", false}});
        if (object.empty()) {
            fail(node.pointer(), "at least one of: stdio, streamable_http, http_sse", "the object has no member");
        }
        if (const auto stdio = object.find("stdio")) {
            const Object body(*stdio, {{"cancel", true}});
            profile_.stdio_ = StdioTransport{
                std::string(declared_name(body.at("cancel"), detail::declared_cancel(detail::Transport::stdio)))};
        }
        if (const auto http_sse = object.find("http_sse")) {
            const Object body(*http_sse, {{"cancel", true}});
            profile_.http_sse_ = HttpSseTransport{
                std::string(declared_name(body.at("cancel"), detail::declared_cancel(detail::Transport::http_sse)))};
        }
        if (const auto streamable = object.find("streamable_http")) {
            profile_.streamable_http_ = streamable_http(*streamable);
        }
    }

    // The header names of one transport, compared ignoring ASCII case.
    class HeaderNames {
    public:
        void add(const Node& node, const std::string& name) {
            if (!seen_.insert(ascii_lower(name)).second) {
                fail(node.pointer(), "header names that are distinct ignoring ASCII case",
                     "'" + name + "' repeats an earlier header name");
            }
        }

    private:
        std::set<std::string> seen_;
    };

    StreamableHttpTransport streamable_http(const Node& node) {
        const Object object(node, {{"cancel", true},
                                   {"session_header", true},
                                   {"version_header", true},
                                   {"get_stream", true},
                                   {"resumable", true},
                                   {"delete_ends_session", true},
                                   {"request_headers", false},
                                   {"argument_headers", false},
                                   {"encoded_header_value", false}});
        StreamableHttpTransport transport;
        HeaderNames names;

        transport.cancel = std::string(
            declared_name(object.at("cancel"), detail::declared_cancel(detail::Transport::streamable_http)));

        const Node session = object.at("session_header");
        if (!session.value().is_null()) {
            transport.session_header = nonempty_string_of(session);
            names.add(session, *transport.session_header);
        }

        const Node version = object.at("version_header");
        if (!version.value().is_null()) {
            const Object body(version, {{"name", true}, {"absent_means", true}});
            const Node name = body.at("name");
            transport.version_header =
                VersionHeader{nonempty_string_of(name), nonempty_string_of(body.at("absent_means"))};
            names.add(name, transport.version_header->name);
        }

        transport.get_stream = bool_of(object.at("get_stream"));
        transport.resumable = bool_of(object.at("resumable"));
        transport.delete_ends_session = bool_of(object.at("delete_ends_session"));

        if (const auto headers = object.find("request_headers")) {
            const auto& array = array_of(*headers);
            for (std::size_t i = 0; i < array.size(); ++i) {
                transport.request_headers.push_back(request_header(headers->element(array[i], i), names));
            }
        }
        if (const auto arguments = object.find("argument_headers")) {
            const Object body(*arguments, {{"schema_keyword", true}, {"header_prefix", true}});
            transport.argument_headers = ArgumentHeaders{nonempty_string_of(body.at("schema_keyword")),
                                                         nonempty_string_of(body.at("header_prefix"))};
        }
        if (const auto encoded = object.find("encoded_header_value")) {
            const Object body(*encoded, {{"prefix", true}, {"suffix", true}});
            transport.encoded_header_value =
                EncodedHeaderValue{nonempty_string_of(body.at("prefix")), nonempty_string_of(body.at("suffix"))};
        }
        return transport;
    }

    RequestHeader request_header(const Node& node, HeaderNames& names) {
        const Object object(node, {{"name", true}, {"value", true}, {"on", true}});
        RequestHeader header;
        const Node name = object.at("name");
        header.name = nonempty_string_of(name);
        names.add(name, header.name);

        static constexpr std::array<std::pair<std::string_view, HeaderValue>, 3> values{
            {{"protocol_version", HeaderValue::protocol_version},
             {"method", HeaderValue::method},
             {"params_member", HeaderValue::params_member}}};
        header.value = pick(object.at("value"), values);

        const Node on = object.at("on");
        if (on.value().is_string()) {
            static constexpr std::array<std::pair<std::string_view, HeaderScopeKind>, 2> scopes{
                {{"every_message", HeaderScopeKind::every_message}, {"every_request", HeaderScopeKind::every_request}}};
            header.scope = pick(on, scopes);
        } else if (on.value().is_object()) {
            header.scope = HeaderScopeKind::per_method;
            for_each_entry(on, [&](const std::string& method, const Node& member) {
                header.methods.push_back(MethodMember{method, nonempty_string_of(member)});
            });
            if (header.methods.empty()) {
                fail(on.pointer(), "at least one method", "the object has no member");
            }
        } else {
            fail(on.pointer(), "\"every_message\", \"every_request\" or an object of method to params member",
                 found(on));
        }

        if (header.value == HeaderValue::params_member && header.scope != HeaderScopeKind::per_method) {
            fail(on.pointer(), "an object of method to params member, because the value is params_member",
                 "found a scope that names no member");
        }
        if (header.value != HeaderValue::params_member && header.scope == HeaderScopeKind::per_method) {
            fail(on.pointer(), "\"every_message\" or \"every_request\", because the value is not params_member",
                 "found an object of method to params member");
        }
        if (header.value == HeaderValue::params_member) check_members(on, header);
        return header;
    }

    // Every method a header lists is a request whose params type has the named member.
    void check_members(const Node& on, const RequestHeader& header) {
        for (const auto& entry : header.methods) {
            const std::string pointer = on.member_pointer(entry.method);
            const Message* request = profile_.request(entry.method);
            if (request == nullptr) {
                fail(pointer, "a method of wire.requests", "'" + entry.method + "' is not a request of this revision");
            }
            if (!request->params) {
                fail(pointer, "a request that has a params type", "'" + entry.method + "' has no params type");
            }
            if (profile_.type(*request->params) == nullptr && profile_.union_type(*request->params) == nullptr) {
                fail(pointer, "a params type that is listed in wire.types or wire.unions",
                     "'" + *request->params + "' of '" + entry.method + "' is listed in neither");
            }
            std::vector<const Type*> candidates;
            std::set<std::string> visited;
            object_types(*request->params, candidates, visited);
            for (const Type* type : candidates) {
                if (!type->has_property(entry.member)) {
                    fail(pointer, "a property of " + type->name + ", which '" + entry.method + "' may carry",
                         "'" + entry.member + "' is not a property of " + type->name);
                }
            }
        }
    }
};

std::variant<ProfilePtr, Error> load(std::string_view json_text) {
    try {
        const Json document = Json::parse(json_text, nullptr, false);
        if (document.is_discarded()) return Error{"", "a JSON document", "the text is not valid JSON"};
        return ProfileLoader::run(document);
    } catch (const Failure& failure) {
        return failure.error;
    } catch (const Json::exception& problem) {
        return Error{"", "a readable JSON document", problem.what()};
    }
}

}  // namespace neomcp::profile
