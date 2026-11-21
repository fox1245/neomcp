// The reading of a token's parts and of the metadata (token.h).
#include "server_auth_jwt/token.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

#include "core/base64.h"
#include "neomcp/authorization.h"

namespace neomcp::http::detail {

namespace {

using neomcp::detail::decode_base64url;

using Clock = std::chrono::system_clock;

// The separator of the compact form's parts (RFC 7515, section 7.1).
constexpr char part_separator = '.';
// The members of the JOSE header that the validator reads (RFC 7515, section 4.1).
constexpr std::string_view header_alg = "alg";
constexpr std::string_view header_typ = "typ";
constexpr std::string_view header_kid = "kid";
constexpr std::string_view header_crit = "crit";
// The media types of an access token (RFC 9068, section 2.1); `typ` may leave out "application/" (RFC 7515, 4.1.9).
constexpr std::string_view access_token_type = "at+jwt";
constexpr std::string_view access_token_media_type = "application/at+jwt";
// The claims mapped to the typed members of Authorization (RFC 7519, section 4.1; RFC 9068, section 2.2).
constexpr std::string_view claim_iss = "iss";
constexpr std::string_view claim_sub = "sub";
constexpr std::string_view claim_client_id = "client_id";
constexpr std::string_view claim_aud = "aud";
constexpr std::string_view claim_exp = "exp";
constexpr std::string_view claim_nbf = "nbf";
constexpr std::string_view claim_scope = "scope";
// The members of the metadata (RFC 8414, section 2).
constexpr std::string_view metadata_issuer = "issuer";
constexpr std::string_view metadata_jwks_uri = "jwks_uri";
// Scope-tokens are separated by one space (RFC 6749, section 3.3; RFC 9068, section 2.2.3).
constexpr char scope_separator = ' ';
// How far inside the limits of the system clock a NumericDate is held, so that the endpoint's arithmetic with the
// leeway and the cache age cannot overflow.
constexpr std::chrono::hours numeric_date_margin{24};

// The JOSE names of the algorithms (RFC 7518, section 3.1).
struct AlgorithmName {
    JwtAlgorithm algorithm;
    std::string_view name;
};
constexpr AlgorithmName algorithm_names[] = {
    {JwtAlgorithm::rs256, "RS256"},
    {JwtAlgorithm::ps256, "PS256"},
    {JwtAlgorithm::es256, "ES256"},
};

char ascii_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equal_ignoring_ascii_case(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
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

// The scope-tokens of a `scope` string; nothing when a piece is not one. Empty pieces are no scopes.
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

std::optional<json> parse_unique_object(std::string_view text) {
    bool repeated = false;
    // One entry per container the parser is inside of: the names met so far for an object, nothing for an array.
    std::vector<std::optional<std::set<std::string>>> inside;
    const json::parser_callback_t watch = [&](int, json::parse_event_t event, json& value) {
        switch (event) {
            case json::parse_event_t::object_start: inside.emplace_back(std::set<std::string>{}); break;
            case json::parse_event_t::array_start: inside.emplace_back(std::nullopt); break;
            case json::parse_event_t::object_end:
            case json::parse_event_t::array_end:
                if (!inside.empty()) inside.pop_back();
                break;
            case json::parse_event_t::key:
                if (!inside.empty() && inside.back()) {
                    const auto* name = value.get_ptr<const json::string_t*>();
                    if (name != nullptr && !inside.back()->insert(*name).second) repeated = true;
                }
                break;
            case json::parse_event_t::value: break;
        }
        return true;
    };
    neojson::parse_options options;
    options.max_depth = max_document_depth;
    json parsed = json::parse(text, watch, false, false, options);
    if (repeated || parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    return parsed;
}

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

const std::string* string_member(const json& object, std::string_view name) {
    const json* found = member(object, name);
    return found == nullptr ? nullptr : found->get_ptr<const json::string_t*>();
}

std::optional<CompactParts> split_compact(std::string_view token) {
    const auto first = token.find(part_separator);
    if (first == std::string_view::npos) return std::nullopt;
    const auto second = token.find(part_separator, first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    if (token.find(part_separator, second + 1) != std::string_view::npos) return std::nullopt;
    auto header = decode_base64url(token.substr(0, first));
    auto payload = decode_base64url(token.substr(first + 1, second - first - 1));
    auto signature = decode_base64url(token.substr(second + 1));
    if (!header || !payload || !signature) return std::nullopt;
    return CompactParts{token.substr(0, second), std::move(*header), std::move(*payload), std::move(*signature)};
}

std::optional<JwtAlgorithm> algorithm_named(std::string_view name, std::span<const JwtAlgorithm> allowed) {
    for (const AlgorithmName& entry : algorithm_names) {
        if (entry.name != name) continue;
        if (std::find(allowed.begin(), allowed.end(), entry.algorithm) == allowed.end()) return std::nullopt;
        return entry.algorithm;
    }
    return std::nullopt;
}

std::variant<JoseHeader, JwtEvent> read_header(std::string_view header_json, std::span<const JwtAlgorithm> allowed) {
    const std::optional<json> header = parse_unique_object(header_json);
    if (!header) return JwtEvent::malformed;
    // An extension the token says must be understood is one this validator does not understand.
    if (member(*header, header_crit) != nullptr) return JwtEvent::critical_header;
    const std::string* alg = string_member(*header, header_alg);
    if (alg == nullptr) return JwtEvent::algorithm_refused;
    const std::optional<JwtAlgorithm> algorithm = algorithm_named(*alg, allowed);
    if (!algorithm) return JwtEvent::algorithm_refused;
    const std::string* typ = string_member(*header, header_typ);
    if (typ == nullptr ||
        !(equal_ignoring_ascii_case(*typ, access_token_type) || equal_ignoring_ascii_case(*typ, access_token_media_type))) {
        return JwtEvent::type_refused;
    }
    JoseHeader read;
    read.algorithm = *algorithm;
    if (const json* kid = member(*header, header_kid)) {
        const auto* text = kid->get_ptr<const json::string_t*>();
        if (text == nullptr) return JwtEvent::malformed;
        read.kid = *text;
    }
    return read;
}

std::variant<Authorization, JwtEvent> read_claims(std::string_view payload_json, std::string_view issuer) {
    std::optional<json> claims = parse_unique_object(payload_json);
    if (!claims) return JwtEvent::malformed;
    Authorization granted;
    const std::string* iss = string_member(*claims, claim_iss);
    if (iss == nullptr) return JwtEvent::malformed;
    if (*iss != issuer) return JwtEvent::issuer_mismatch;
    granted.issuer = *iss;
    for (const auto& [name, target] :
         {std::pair{claim_sub, &granted.subject}, std::pair{claim_client_id, &granted.client_id}}) {
        const json* found = member(*claims, name);
        if (found == nullptr) continue;
        const auto* text = found->get_ptr<const json::string_t*>();
        if (text == nullptr) return JwtEvent::malformed;
        *target = *text;
    }
    const json* aud = member(*claims, claim_aud);
    if (aud == nullptr) return JwtEvent::malformed;
    auto audiences = audiences_of(*aud);
    if (!audiences) return JwtEvent::malformed;
    granted.audiences = std::move(*audiences);
    const json* exp = member(*claims, claim_exp);
    if (exp == nullptr) return JwtEvent::malformed;
    granted.expires = numeric_date(*exp);
    if (!granted.expires) return JwtEvent::malformed;
    if (const json* nbf = member(*claims, claim_nbf)) {
        granted.not_before = numeric_date(*nbf);
        if (!granted.not_before) return JwtEvent::malformed;
    }
    if (const json* scope = member(*claims, claim_scope)) {
        const auto* text = scope->get_ptr<const json::string_t*>();
        if (text == nullptr) return JwtEvent::malformed;
        auto scopes = split_scopes(*text);
        if (!scopes) return JwtEvent::malformed;
        granted.scopes = std::move(*scopes);
    }
    // What is left is for handlers.
    auto& members = claims->get_ref<json::object_t&>();
    for (const std::string_view mapped :
         {claim_iss, claim_sub, claim_client_id, claim_aud, claim_exp, claim_nbf, claim_scope}) {
        members.erase(std::string(mapped));
    }
    granted.claims = std::move(*claims);
    return granted;
}

std::optional<std::string> read_metadata_jwks_uri(std::string_view body, std::string_view issuer) {
    const std::optional<json> metadata = parse_unique_object(body);
    if (!metadata) return std::nullopt;
    const std::string* named = string_member(*metadata, metadata_issuer);
    if (named == nullptr || *named != issuer) return std::nullopt;
    const std::string* uri = string_member(*metadata, metadata_jwks_uri);
    if (uri == nullptr) return std::nullopt;
    return *uri;
}

}  // namespace neomcp::http::detail
