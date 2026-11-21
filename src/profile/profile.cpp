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

namespace neomcp::profile {

std::string_view to_string(Slot slot) noexcept {
    switch (slot) {
        case Slot::open: return "open";
        case Slot::state: return "state";
        case Slot::server_input: return "server_input";
        case Slot::change_feed: return "change_feed";
        case Slot::batch: return "batch";
        case Slot::tasks: return "tasks";
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

std::string_view to_string(Transport transport) noexcept {
    switch (transport) {
        case Transport::stdio: return "stdio";
        case Transport::http_sse: return "http_sse";
        case Transport::streamable_http: return "streamable_http";
    }
    return {};
}

std::optional<int> StreamableHttpTransport::status_for(std::string_view reason) const {
    const auto it = std::lower_bound(status.begin(), status.end(), reason,
                                     [](const NamedStatus& entry, std::string_view key) { return entry.reason < key; });
    if (it == status.end() || it->reason != reason) return std::nullopt;
    return it->status;
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

bool Type::has_type_value(std::string_view value) const {
    return std::binary_search(type_values.begin(), type_values.end(), value);
}

bool Profile::error_response_may_omit_id() const {
    // The members of a JSON-RPC error response (JSON-RPC 2.0, section 5.1), not data of a revision.
    constexpr std::string_view error_member = "error";
    constexpr std::string_view id_member = "id";
    bool any = false;
    for (const Type& type : types_) {
        if (!type.has_property(error_member) || !type.has_property(id_member)) continue;
        if (type.is_required(id_member)) return false;
        any = true;
    }
    return any;
}

std::string_view Profile::mechanism(Slot slot) const noexcept {
    return mechanisms_[static_cast<std::size_t>(slot)];
}

bool ExtensionProfile::applies_to(std::string_view revision) const noexcept {
    return std::find(revisions_.begin(), revisions_.end(), revision) != revisions_.end();
}

std::optional<std::string_view> ExtensionProfile::mechanism(Slot slot) const noexcept {
    const std::string_view name = mechanisms_[static_cast<std::size_t>(slot)];
    if (name.empty()) return std::nullopt;
    return name;
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

const Type* Profile::property_type(const Type& owner, std::string_view property) const {
    const auto it = std::lower_bound(
        owner.property_types.begin(), owner.property_types.end(), property,
        [](const PropertyType& entry, std::string_view key) { return entry.property < key; });
    if (it == owner.property_types.end() || it->property != property) return nullptr;
    return type(it->type);
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

// A string that is an absolute path of a URL (RFC 3986, section 3.3, path-absolute): a '/' not followed by another,
// then segments of pchar (unreserved, percent-encoded, sub-delims, ':' and '@') separated by '/'. No query, no
// fragment, no authority.
const std::string& absolute_path_of(const Node& node) {
    static const std::string expected = "an absolute path: '/' and segments of RFC 3986 path characters";
    const auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    // unreserved without letters and digits, sub-delims, ':', '@' and the separator '/'
    static constexpr std::string_view path_specials = "-._~!$&'()*+,;=:@/";
    const auto is_path_char = [](char c) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
        return path_specials.find(c) != std::string_view::npos;
    };
    const std::string& text = string_of(node);
    if (text.empty() || text.front() != '/' || (text.size() > 1 && text[1] == '/')) {
        fail(node.pointer(), expected, "found '" + text + "'");
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%') {
            if (i + 2 >= text.size() || !is_hex(text[i + 1]) || !is_hex(text[i + 2])) {
                fail(node.pointer(), expected, "found '" + text + "'");
            }
            i += 2;
        } else if (!is_path_char(text[i])) {
            fail(node.pointer(), expected, "found '" + text + "'");
        }
    }
    return text;
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

    static ExtensionPtr run_extension(const Json& document) {
        ProfileLoader loader;
        loader.extension_root(Node(document, ""));
        return std::make_shared<const ExtensionProfile>(std::move(loader.extension_));
    }

    // Adds the extensions to a copy of the core profile. The first problem is returned.
    static std::variant<ProfilePtr, Error> compose(const Profile& core, std::span<const ExtensionPtr> extensions) {
        Profile result = core;
        for (std::size_t i = 0; i < extensions.size(); ++i) {
            if (extensions[i] == nullptr) return Error{"", "an extension", "found a null extension"};
            if (auto problem = add_extension(result, *extensions[i])) return std::move(*problem);
        }
        return std::make_shared<const Profile>(std::move(result));
    }

private:
    Profile profile_;
    ExtensionProfile extension_;

    void root(const Node& node) {
        const Object object(node, {{"profile_version", true},
                                   {"revision", true},
                                   {"era", true},
                                   {"evidence", true},
                                   {"mechanisms", true},
                                   {"transports", true},
                                   {"errors", true},
                                   {"tools", true},
                                   {"authorization", true},
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
        tools(object.at("tools"));
        authorization(object.at("authorization"));
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
            profile_.mechanisms_.push_back(declared_name(object.at(to_string(slot)), declared_mechanisms(slot)));
        }
    }

    void tools(const Node& node) {
        const Object object(node, {{"invalid_arguments", true}});
        profile_.invalid_arguments_ = declared_name(object.at("invalid_arguments"), declared_invalid_arguments());
    }

    // ----- extensions

    void extension_root(const Node& node) {
        const Object object(node, {{"profile_version", true},
                                   {"extension", true},
                                   {"revisions", true},
                                   {"evidence", true},
                                   {"mechanisms", true},
                                   {"senders", true},
                                   {"transports", false},
                                   {"errors", false},
                                   {"wire", true}});

        const Node version = object.at("profile_version");
        if (const int number = int_of(version); number != 1) {
            fail(version.pointer(), "1", "found " + std::to_string(number));
        }
        extension_.extension_ = nonempty_string_of(object.at("extension"));
        const Node revisions = object.at("revisions");
        extension_.revisions_ = strings_of(revisions, true);
        if (extension_.revisions_.empty()) fail(revisions.pointer(), "at least one revision", "an empty array");
        for (const auto& revision : extension_.revisions_) {
            if (revision.empty()) fail(revisions.pointer(), "non-empty revisions", "found an empty string");
        }

        evidence(object.at("evidence"));
        extension_.evidence_ = std::move(profile_.evidence_);

        std::vector<MemberSpec> slots;
        for (std::size_t i = 0; i < slot_count; ++i) slots.push_back({to_string(static_cast<Slot>(i)), false});
        const Node mechanisms_node = object.at("mechanisms");
        const Object mechanisms_object(mechanisms_node, slots);
        extension_.mechanisms_.assign(slot_count, std::string_view());
        for (std::size_t i = 0; i < slot_count; ++i) {
            const Slot slot = static_cast<Slot>(i);
            if (const auto chosen = mechanisms_object.find(to_string(slot))) {
                extension_.mechanisms_[i] = declared_name(*chosen, declared_mechanisms(slot));
            }
        }

        wire(object.at("wire"));
        extension_.wire_schema_ = std::move(profile_.wire_schema_);
        extension_.requests_ = std::move(profile_.requests_);
        extension_.notifications_ = std::move(profile_.notifications_);
        extension_.input_requests_ = std::move(profile_.input_requests_);
        extension_.unlisted_message_types_ = std::move(profile_.unlisted_message_types_);
        extension_.types_ = std::move(profile_.types_);
        extension_.unions_ = std::move(profile_.unions_);

        senders(object.at("senders"));

        if (const auto errors_node = object.find("errors")) {
            for_each_entry(*errors_node, [&](const std::string& name, const Node& value) {
                extension_.error_codes_.push_back(NamedCode{name, int_of(value)});
            });
            std::sort(extension_.error_codes_.begin(), extension_.error_codes_.end(),
                      [](const NamedCode& a, const NamedCode& b) { return a.name < b.name; });
        }

        if (const auto transports_node = object.find("transports")) {
            const Object transports_object(*transports_node, {{"streamable_http", true}});
            const Node streamable = transports_object.at("streamable_http");
            const Object body(streamable, {{"request_headers", true}});
            const Node headers = body.at("request_headers");
            const auto& array = array_of(headers);
            HeaderNames names;
            for (std::size_t i = 0; i < array.size(); ++i) {
                extension_.request_headers_.push_back(request_header(headers.element(array[i], i), names));
            }
        }
    }

    // The senders written by hand agree with the wire tables derived from the schema: the same messages, the same
    // senders.
    void senders(const Node& node) {
        const Object object(node, {{"requests", true}, {"notifications", true}});
        const auto check = [&](std::string_view member, const std::vector<Message>& table) {
            const Node listed = object.at(member);
            std::size_t counted = 0;
            for_each_entry(listed, [&](const std::string& type, const Node& value) {
                const auto& array = array_of(value);
                bool client = false;
                bool server = false;
                for (std::size_t i = 0; i < array.size(); ++i) {
                    const Node sender = value.element(array[i], i);
                    static constexpr std::array<std::pair<std::string_view, bool>, 2> sender_names{
                        {{"client", true}, {"server", false}}};
                    bool& flag = pick(sender, sender_names) ? client : server;
                    if (flag) fail(sender.pointer(), "distinct senders", "'" + string_of(sender) + "' repeats an earlier entry");
                    flag = true;
                }
                if (!client && !server) fail(value.pointer(), "at least one sender", "an empty array");
                const auto message = std::find_if(table.begin(), table.end(),
                                                  [&](const Message& entry) { return entry.type == type; });
                if (message == table.end()) {
                    fail(value.pointer(), "a message type of wire." + std::string(member),
                         "'" + type + "' is not one");
                }
                if (message->from_client != client || message->from_server != server) {
                    fail(value.pointer(), "the senders of wire." + std::string(member) + "'s " + type, "other senders");
                }
                ++counted;
            });
            if (counted != table.size()) {
                fail(listed.pointer(), "every message of wire." + std::string(member) + " listed",
                     "the messages of the wire tables are " + std::to_string(table.size()) + ", the senders list " +
                         std::to_string(counted));
            }
        };
        check("requests", extension_.requests_);
        check("notifications", extension_.notifications_);
    }

    // ----- composition

    static std::string by_name(const NamedCode& entry) { return entry.name; }
    static std::string by_name(const Type& entry) { return entry.name; }
    static std::string by_name(const Union& entry) { return entry.name; }
    static std::string by_name(const Message& entry) { return entry.method; }
    static std::string by_name(const InputRequest& entry) { return entry.method; }

    static bool same(const NamedCode& a, const NamedCode& b) { return a.code == b.code; }
    static bool same(const Message& a, const Message& b) {
        return a.from_client == b.from_client && a.from_server == b.from_server && a.type == b.type &&
               a.params == b.params && a.params_required == b.params_required;
    }
    static bool same(const InputRequest& a, const InputRequest& b) {
        return a.type == b.type && a.params == b.params && a.params_required == b.params_required;
    }
    static bool same(const Type& a, const Type& b) {
        const auto property_types_equal = [](const std::vector<PropertyType>& x, const std::vector<PropertyType>& y) {
            return std::equal(x.begin(), x.end(), y.begin(), y.end(), [](const PropertyType& l, const PropertyType& r) {
                return l.property == r.property && l.type == r.type;
            });
        };
        return a.properties == b.properties && a.required == b.required && a.type_values == b.type_values &&
               property_types_equal(a.property_types, b.property_types);
    }
    static bool same(const Union& a, const Union& b) { return a.alternatives == b.alternatives; }

    // Adds the entries of `source` that `target` lacks, keeping `target` sorted by name; an entry both have must be
    // the same. `table` names the place in the extension's document.
    template <typename Entry>
    static std::optional<Error> merge_table(std::vector<Entry>& target, const std::vector<Entry>& source,
                                            const std::string& place, std::string_view extension) {
        const auto less = [](const Entry& entry, const std::string& key) { return by_name(entry) < key; };
        for (const Entry& entry : source) {
            const std::string name = by_name(entry);
            auto it = std::lower_bound(target.begin(), target.end(), name, less);
            if (it != target.end() && by_name(*it) == name) {
                if (!same(*it, entry)) {
                    return Error{place + "/" + escape_token(name), "a definition equal to the one already in the profile",
                                 "the extension " + std::string(extension) + " defines '" + name + "' differently"};
                }
                continue;
            }
            target.insert(it, entry);
        }
        return std::nullopt;
    }

    static std::optional<Error> add_extension(Profile& result, const ExtensionProfile& extension) {
        const std::string& id = extension.extension();
        if (!extension.applies_to(result.revision_)) {
            return Error{"/revisions", "a list that names " + result.revision_,
                         "the extension " + id + " is not defined for this revision"};
        }
        if (std::find(result.extensions_.begin(), result.extensions_.end(), id) != result.extensions_.end()) {
            return Error{"/extension", "an extension that is composed once", "'" + id + "' is composed twice"};
        }

        for (std::size_t i = 0; i < slot_count; ++i) {
            if (const auto chosen = extension.mechanism(static_cast<Slot>(i))) result.mechanisms_[i] = *chosen;
        }

        if (auto problem = merge_table(result.error_codes_, extension.error_codes_, "/errors", id)) return problem;
        if (auto problem = merge_table(result.requests_, extension.requests_, "/wire/requests", id)) return problem;
        if (auto problem = merge_table(result.notifications_, extension.notifications_, "/wire/notifications", id)) {
            return problem;
        }
        if (auto problem = merge_table(result.input_requests_, extension.input_requests_, "/wire/input_requests", id)) {
            return problem;
        }
        if (auto problem = merge_table(result.types_, extension.types_, "/wire/types", id)) return problem;
        if (auto problem = merge_table(result.unions_, extension.unions_, "/wire/unions", id)) return problem;
        for (const std::string& name : extension.unlisted_message_types_) {
            const auto it = std::lower_bound(result.unlisted_message_types_.begin(),
                                             result.unlisted_message_types_.end(), name);
            if (it == result.unlisted_message_types_.end() || *it != name) {
                result.unlisted_message_types_.insert(it, name);
            }
        }

        if (!extension.request_headers_.empty() && result.streamable_http_) {
            if (auto problem = add_headers(result, extension)) return problem;
        }
        result.extensions_.push_back(id);
        return std::nullopt;
    }

    // The extension's headers join the transport's: a header the transport has already (compared ignoring ASCII
    // case) gets the extension's methods added; any other is appended. Every method then names a member its request
    // carries.
    static std::optional<Error> add_headers(Profile& result, const ExtensionProfile& extension) {
        auto& headers = result.streamable_http_->request_headers;
        for (std::size_t i = 0; i < extension.request_headers_.size(); ++i) {
            const RequestHeader& added = extension.request_headers_[i];
            const std::string place = "/transports/streamable_http/request_headers/" + std::to_string(i);
            const std::string lowered = ascii_lower(added.name);
            const auto existing = std::find_if(headers.begin(), headers.end(), [&](const RequestHeader& header) {
                return ascii_lower(header.name) == lowered;
            });
            std::size_t index = 0;
            if (existing == headers.end()) {
                headers.push_back(added);
                index = headers.size() - 1;
            } else {
                if (existing->value != added.value || existing->scope != HeaderScopeKind::per_method ||
                    added.scope != HeaderScopeKind::per_method) {
                    return Error{place, "a header that names members of the params of some methods, as the transport's " +
                                            existing->name + " does",
                                 "the transport's header of that name is of another kind"};
                }
                for (const MethodMember& entry : added.methods) {
                    const auto listed = std::find_if(existing->methods.begin(), existing->methods.end(),
                                                     [&](const MethodMember& old) { return old.method == entry.method; });
                    if (listed != existing->methods.end()) {
                        return Error{place + "/on/" + escape_token(entry.method), "a method the header does not list yet",
                                     "'" + entry.method + "' is listed by the revision already"};
                    }
                    existing->methods.push_back(entry);
                }
                index = static_cast<std::size_t>(existing - headers.begin());
            }
            if (headers[index].value == HeaderValue::params_member) {
                if (auto problem = check_members(result, place + "/on", added)) return problem;
            }
        }
        return std::nullopt;
    }

    // ----- authorization

    void authorization(const Node& node) {
        // The declared discovery whose data is the member of the same name.
        static constexpr std::string_view origin_metadata = "origin_metadata";
        const Object object(node, {{"discovery", true}, {origin_metadata, false}});
        Authorization& authorization = profile_.authorization_;
        authorization.discovery = declared_name(object.at("discovery"), declared_authorization_discovery());
        const std::optional<Node> origin = object.find(origin_metadata);
        if (authorization.discovery != origin_metadata) {
            if (origin) {
                fail(origin->pointer(),
                     "no such member, because discovery is " + std::string(authorization.discovery),
                     "found the member");
            }
            return;
        }
        if (!origin) {
            fail(node.member_pointer(origin_metadata),
                 "a member named '" + std::string(origin_metadata) + "', because discovery is " +
                     std::string(origin_metadata),
                 "the member is missing");
        }
        const Object body(*origin, {{"metadata_path", true}, {"default_endpoints", true}});
        OriginMetadataDiscovery discovery;
        discovery.metadata_path = absolute_path_of(body.at("metadata_path"));
        const Object endpoints(body.at("default_endpoints"),
                               {{"authorization", true}, {"token", true}, {"registration", true}});
        discovery.default_endpoints.authorization = absolute_path_of(endpoints.at("authorization"));
        discovery.default_endpoints.token = absolute_path_of(endpoints.at("token"));
        discovery.default_endpoints.registration = absolute_path_of(endpoints.at("registration"));
        authorization.origin_metadata = std::move(discovery);
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
        // The property of an object type that says which type of a union the object is, and the member that lists the
        // values the schema gives it.
        static constexpr std::string_view type_property = "type";
        static constexpr std::string_view type_values_member = "type_values";
        static constexpr std::string_view property_types_member = "property_types";
        for_each_entry(node, [&](const std::string& name, const Node& value) {
            const Object object(value, {{"properties", true},
                                        {"required", true},
                                        {type_values_member, false},
                                        {property_types_member, false}});
            Type type;
            type.name = name;
            type.properties = strings_of(object.at("properties"), true);
            std::vector<std::string> sorted_properties = type.properties;
            std::sort(sorted_properties.begin(), sorted_properties.end());
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
            if (const std::optional<Node> values = object.find(type_values_member)) {
                type.type_values = strings_of(*values, true);
                if (type.type_values.empty()) fail(values->pointer(), "at least one value", "an empty list");
                if (!std::binary_search(sorted_properties.begin(), sorted_properties.end(), type_property)) {
                    fail(values->pointer(), "no member, because " + name + " has no property '" + std::string(type_property) + "'",
                         "a list of values");
                }
            }
            if (const std::optional<Node> typed = object.find(property_types_member)) {
                if (object_of(*typed).empty()) fail(typed->pointer(), "at least one property", "an empty object");
                for_each_entry(*typed, [&](const std::string& property, const Node& target) {
                    if (!std::binary_search(sorted_properties.begin(), sorted_properties.end(), property)) {
                        fail(typed->member_pointer(property), "a property of " + name,
                             "'" + property + "' is not a property of " + name);
                    }
                    const std::string& type_name = nonempty_string_of(target);
                    if (object_of(node).find(type_name) == object_of(node).end()) {
                        fail(target.pointer(), "a name of wire.types", "'" + type_name + "' is not one");
                    }
                    type.property_types.push_back({property, type_name});
                });
            }
            std::sort(type.required.begin(), type.required.end());
            std::sort(type.type_values.begin(), type.type_values.end());
            std::sort(type.property_types.begin(), type.property_types.end(),
                      [](const PropertyType& a, const PropertyType& b) { return a.property < b.property; });
            type.properties = std::move(sorted_properties);
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
    static void object_types(const Profile& profile, const std::string& name, std::vector<const Type*>& out,
                             std::set<std::string>& visited) {
        if (!visited.insert(name).second) return;
        if (const Type* type = profile.type(name)) {
            out.push_back(type);
            return;
        }
        for (const auto& alternative : profile.union_type(name)->alternatives) {
            object_types(profile, alternative, out, visited);
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
                std::string(declared_name(body.at("cancel"), declared_cancel(Transport::stdio)))};
        }
        if (const auto http_sse = object.find("http_sse")) {
            const Object body(*http_sse, {{"cancel", true}, {"endpoint_event", true}, {"message_event", true}});
            HttpSseTransport transport;
            transport.cancel = std::string(declared_name(body.at("cancel"), declared_cancel(Transport::http_sse)));
            transport.endpoint_event = nonempty_string_of(body.at("endpoint_event"));
            const Node message = body.at("message_event");
            transport.message_event = nonempty_string_of(message);
            if (transport.message_event == transport.endpoint_event) {
                fail(message.pointer(), "an event type other than endpoint_event",
                     "'" + transport.message_event + "' is the type of the endpoint event too");
            }
            profile_.http_sse_ = std::move(transport);
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
                                   {"encoded_header_value", false},
                                   {"status", false}});
        StreamableHttpTransport transport;
        HeaderNames names;

        transport.cancel = std::string(
            declared_name(object.at("cancel"), declared_cancel(Transport::streamable_http)));

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
                const Node entry = headers->element(array[i], i);
                transport.request_headers.push_back(request_header(entry, names));
                const RequestHeader& header = transport.request_headers.back();
                if (header.value == HeaderValue::params_member) {
                    if (const auto problem = check_members(profile_, entry.member_pointer("on"), header)) {
                        throw Failure{*problem};
                    }
                }
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
        if (const auto status = object.find("status")) transport.status = statuses(*status);
        return transport;
    }

    // Reason to HTTP status: each reason a declared one, each status a three-digit HTTP status code.
    static std::vector<NamedStatus> statuses(const Node& node) {
        static constexpr int lowest_status = 100;
        static constexpr int highest_status = 599;
        std::vector<NamedStatus> table;
        const auto reasons = declared_status_reasons();
        for_each_entry(node, [&](const std::string& reason, const Node& value) {
            if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
                fail(value.pointer(), "a member named after one of the declared reasons: " + join(reasons),
                     "unknown reason '" + reason + "'");
            }
            const int status = int_of(value);
            if (status < lowest_status || status > highest_status) {
                fail(value.pointer(), "an HTTP status code from 100 to 599", "found " + std::to_string(status));
            }
            table.push_back(NamedStatus{reason, status});
        });
        std::sort(table.begin(), table.end(),
                  [](const NamedStatus& a, const NamedStatus& b) { return a.reason < b.reason; });
        return table;
    }

    static RequestHeader request_header(const Node& node, HeaderNames& names) {
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
        return header;
    }

    // The type of the params of a request: the type the profile names for them or, where the schema writes the params
    // inline in the request's definition, the type of that object (the request type's property `params`).
    static const Type* params_type_of(const Profile& profile, const Message& request) {
        static constexpr std::string_view params_member = "params";
        if (request.params) return profile.type(*request.params);
        const Type* owner = profile.type(request.type);
        return owner == nullptr ? nullptr : profile.property_type(*owner, params_member);
    }

    // Every method a header lists is a request whose params type has the named member. `pointer` is the place of the
    // header's `on` member.
    static std::optional<Error> check_members(const Profile& profile, const std::string& on_pointer,
                                              const RequestHeader& header) {
        for (const auto& entry : header.methods) {
            const std::string pointer = on_pointer + "/" + escape_token(entry.method);
            const Message* request = profile.request(entry.method);
            if (request == nullptr) {
                return Error{pointer, "a method of wire.requests", "'" + entry.method + "' is not a request of this revision"};
            }
            std::vector<const Type*> candidates;
            if (request->params) {
                if (profile.type(*request->params) == nullptr && profile.union_type(*request->params) == nullptr) {
                    return Error{pointer, "a params type that is listed in wire.types or wire.unions",
                                 "'" + *request->params + "' of '" + entry.method + "' is listed in neither"};
                }
                std::set<std::string> visited;
                object_types(profile, *request->params, candidates, visited);
            } else if (const Type* inline_params = params_type_of(profile, *request)) {
                candidates.push_back(inline_params);
            } else {
                return Error{pointer, "a request that has a params type", "'" + entry.method + "' has no params type"};
            }
            for (const Type* type : candidates) {
                if (!type->has_property(entry.member)) {
                    return Error{pointer, "a property of " + type->name + ", which '" + entry.method + "' may carry",
                                 "'" + entry.member + "' is not a property of " + type->name};
                }
            }
        }
        return std::nullopt;
    }
};

std::variant<ExtensionPtr, Error> load_extension(std::string_view json_text) {
    try {
        const Json document = Json::parse(json_text, nullptr, false);
        if (document.is_discarded()) return Error{"", "a JSON document", "the text is not valid JSON"};
        return ProfileLoader::run_extension(document);
    } catch (const Failure& failure) {
        return failure.error;
    } catch (const Json::exception& problem) {
        return Error{"", "a readable JSON document", problem.what()};
    }
}

std::variant<ProfilePtr, Error> compose(const Profile& core, std::span<const ExtensionPtr> extensions) {
    return ProfileLoader::compose(core, extensions);
}

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
