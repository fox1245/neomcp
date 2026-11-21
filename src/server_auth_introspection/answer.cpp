// The reading of an introspection answer (answer.h).
#include "server_auth_introspection/answer.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "neomcp/authorization.h"
#include "neomcp/json.h"

namespace neomcp::http::detail {

namespace {

using Clock = std::chrono::system_clock;

constexpr int status_ok = 200;
// The members of the answer (RFC 7662, section 2.2).
constexpr std::string_view member_active = "active";
constexpr std::string_view member_scope = "scope";
constexpr std::string_view member_client_id = "client_id";
constexpr std::string_view member_sub = "sub";
constexpr std::string_view member_aud = "aud";
constexpr std::string_view member_iss = "iss";
constexpr std::string_view member_exp = "exp";
constexpr std::string_view member_nbf = "nbf";
// Scope-tokens are separated by one space (RFC 6749, section 3.3).
constexpr char scope_separator = ' ';
// An answer is a flat object; arrays of strings are the deepest member. A little more is allowed so that members the
// reader does not use may hold some structure.
constexpr std::size_t max_answer_depth = 8;
// How far inside the limits of the system clock a NumericDate is held, so that the endpoint's arithmetic with the
// leeway and the cache age cannot overflow.
constexpr std::chrono::hours numeric_date_margin{24};

IntrospectionReading unavailable(IntrospectionEvent event) {
    return IntrospectionReading{TokenVerdict{TokenRefusal::unavailable}, event};
}

IntrospectionReading invalid(IntrospectionEvent event) {
    return IntrospectionReading{TokenVerdict{TokenRefusal::invalid}, event};
}

// The answer as a JSON object, and whether a member name occurs twice in it (at the top level, where the members that
// are read live). Nothing when the text is not one JSON object within the depth limit.
struct ParsedObject {
    json value;
    bool repeated = false;
};

std::optional<ParsedObject> parse_object(std::string_view body) {
    ParsedObject parsed;
    std::set<std::string> names;
    // The kinds of the containers the parser is inside of: true for an object.
    std::vector<bool> inside;
    const json::parser_callback_t watch = [&](int, json::parse_event_t event, json& value) {
        switch (event) {
            case json::parse_event_t::object_start: inside.push_back(true); break;
            case json::parse_event_t::array_start: inside.push_back(false); break;
            case json::parse_event_t::object_end:
            case json::parse_event_t::array_end:
                if (!inside.empty()) inside.pop_back();
                break;
            case json::parse_event_t::key:
                if (inside.size() == 1) {
                    const auto* name = value.get_ptr<const json::string_t*>();
                    if (name != nullptr && !names.insert(*name).second) parsed.repeated = true;
                }
                break;
            case json::parse_event_t::value: break;
        }
        return true;
    };
    neojson::parse_options options;
    options.max_depth = max_answer_depth;
    parsed.value = json::parse(body, watch, false, false, options);
    if (parsed.value.is_discarded() || !parsed.value.is_object()) return std::nullopt;
    return parsed;
}

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

// A NumericDate (RFC 7519, section 2): a JSON number of seconds, fractions allowed, held within the clock.
std::optional<Clock::time_point> numeric_date(const json& value) {
    if (!value.is_number()) return std::nullopt;
    const double seconds = value.get<double>();
    if (!std::isfinite(seconds)) return std::nullopt;
    const double latest = std::chrono::duration<double>((Clock::duration::max)() - numeric_date_margin).count();
    const double earliest = std::chrono::duration<double>((Clock::duration::min)() + numeric_date_margin).count();
    const double held = std::clamp(seconds, earliest, latest);
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(held)));
}

// The scope-tokens of a `scope` string; nothing when a piece is not one. Empty pieces (a doubled, leading or
// trailing space) are no scopes.
std::optional<std::vector<std::string>> split_scopes(std::string_view text) {
    std::vector<std::string> scopes;
    while (!text.empty()) {
        const auto end = text.find(scope_separator);
        const std::string_view piece = text.substr(0, end);
        if (!piece.empty()) {
            if (!is_scope_token(piece)) return std::nullopt;
            scopes.emplace_back(piece);
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return scopes;
}

// `aud` is a string or an array of strings (RFC 7519, section 4.1.3).
std::optional<std::vector<std::string>> audiences_of(const json& value) {
    if (const auto* one = value.get_ptr<const json::string_t*>()) return std::vector<std::string>{*one};
    const auto* items = value.get_ptr<const json::array_t*>();
    if (items == nullptr) return std::nullopt;
    std::vector<std::string> out;
    out.reserve(items->size());
    for (const json& item : *items) {
        const auto* text = item.get_ptr<const json::string_t*>();
        if (text == nullptr) return std::nullopt;
        out.push_back(*text);
    }
    return out;
}

}  // namespace

IntrospectionReading read_introspection_answer(int status, std::string_view body, const IntrospectionContext& context) {
    if (status != status_ok) return unavailable(IntrospectionEvent::unexpected_status);
    if (body.size() > context.max_bytes) return unavailable(IntrospectionEvent::response_too_large);
    const std::optional<ParsedObject> parsed = parse_object(body);
    if (!parsed) return unavailable(IntrospectionEvent::not_an_object);
    if (parsed->repeated) return invalid(IntrospectionEvent::malformed);
    const json& answer = parsed->value;

    // `active` decides first; an inactive answer is read no further (section 2.2: nothing more should be said).
    const json* active = member(answer, member_active);
    if (active == nullptr || !active->is_boolean()) return invalid(IntrospectionEvent::malformed);
    if (!active->get<bool>()) return invalid(IntrospectionEvent::inactive);

    Authorization granted;
    granted.issuer = std::string(context.issuer);
    if (const json* iss = member(answer, member_iss)) {
        const auto* text = iss->get_ptr<const json::string_t*>();
        if (text == nullptr) return invalid(IntrospectionEvent::malformed);
        if (*text != context.issuer) return invalid(IntrospectionEvent::issuer_mismatch);
    }
    for (const auto& [name, target] :
         {std::pair{member_sub, &granted.subject}, std::pair{member_client_id, &granted.client_id}}) {
        const json* found = member(answer, name);
        if (found == nullptr) continue;
        const auto* text = found->get_ptr<const json::string_t*>();
        if (text == nullptr) return invalid(IntrospectionEvent::malformed);
        *target = *text;
    }
    if (const json* scope = member(answer, member_scope)) {
        const auto* text = scope->get_ptr<const json::string_t*>();
        if (text == nullptr) return invalid(IntrospectionEvent::malformed);
        auto scopes = split_scopes(*text);
        if (!scopes) return invalid(IntrospectionEvent::malformed);
        granted.scopes = std::move(*scopes);
    }
    if (const json* aud = member(answer, member_aud)) {
        auto audiences = audiences_of(*aud);
        if (!audiences) return invalid(IntrospectionEvent::malformed);
        granted.audiences = std::move(*audiences);
    }
    if (const json* exp = member(answer, member_exp)) {
        granted.expires = numeric_date(*exp);
        if (!granted.expires) return invalid(IntrospectionEvent::malformed);
    } else {
        granted.expires = context.now + context.default_lifetime;
    }
    if (const json* nbf = member(answer, member_nbf)) {
        granted.not_before = numeric_date(*nbf);
        if (!granted.not_before) return invalid(IntrospectionEvent::malformed);
    }
    return IntrospectionReading{TokenVerdict{std::move(granted)}, IntrospectionEvent::active};
}

}  // namespace neomcp::http::detail
