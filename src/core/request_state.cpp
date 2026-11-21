#include "core/request_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#include "core/base64.h"
#include "crypto/hmac.h"
#include "crypto/sha256.h"
#include "neomcp/jsonrpc.h"
#include "neomcp/random.h"

namespace neomcp::detail {
namespace {

// The label the code covers before the payload, so that a code of this format is never valid for anything else.
constexpr std::string_view mac_label = "neomcp request state";
constexpr std::int64_t format_version = 1;
constexpr char separator = '.';

// The members of the payload.
constexpr std::string_view version_member = "v";
constexpr std::string_view expires_member = "exp";
constexpr std::string_view method_member = "m";
constexpr std::string_view subject_member = "t";
constexpr std::string_view arguments_member = "a";
constexpr std::string_view server_member = "s";
constexpr std::string_view principal_member = "p";
constexpr std::string_view round_member = "r";
constexpr std::string_view asked_member = "k";
constexpr std::string_view state_member = "h";

std::string code_of(std::string_view key, std::string_view payload) {
    HmacSha256 mac(key);
    mac.update(mac_label);
    mac.update(std::string_view("\0", 1));
    mac.update(payload);
    const Sha256Digest code = mac.finish();
    return std::string(reinterpret_cast<const char*>(code.data()), code.size());
}

const json* member(const json& object, std::string_view name) {
    const auto* members = object.get_ptr<const json::object_t*>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(std::string(name));
    return found == members->end() ? nullptr : &found->second;
}

const std::string* text_member(const json& object, std::string_view name) {
    const json* value = member(object, name);
    return value == nullptr ? nullptr : value->get_ptr<const std::string*>();
}

std::optional<std::int64_t> integer_member(const json& object, std::string_view name) {
    const json* value = member(object, name);
    if (value == nullptr) return std::nullopt;
    if (const auto* number = value->get_ptr<const std::uint64_t*>()) {
        if (*number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
        return static_cast<std::int64_t>(*number);
    }
    if (const auto* number = value->get_ptr<const std::int64_t*>()) return *number;
    return std::nullopt;
}

void write_canonical(const json& value, std::string& out) {
    if (const auto* members = value.get_ptr<const json::object_t*>()) {
        std::vector<const json::object_t::value_type*> sorted;
        sorted.reserve(members->size());
        for (const auto& entry : *members) sorted.push_back(&entry);
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
        out += '{';
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0) out += ',';
            out += serialize(json(sorted[i]->first));
            out += ':';
            write_canonical(sorted[i]->second, out);
        }
        out += '}';
    } else if (const auto* items = value.get_ptr<const json::array_t*>()) {
        out += '[';
        for (std::size_t i = 0; i < items->size(); ++i) {
            if (i > 0) out += ',';
            write_canonical((*items)[i], out);
        }
        out += ']';
    } else if (const auto* number = value.get_ptr<const double*>()) {
        // A double that holds an integer is written as one, so that 1.0 and 1 agree. 2^63 bounds what int64 holds.
        constexpr double integer_bound = 9223372036854775808.0;
        if (std::isfinite(*number) && std::trunc(*number) == *number && std::fabs(*number) < integer_bound) {
            out += std::to_string(static_cast<std::int64_t>(*number));
        } else {
            out += serialize(value);
        }
    } else {
        out += serialize(value);
    }
}

}  // namespace

const std::optional<std::string>& default_state_key() {
    static const std::optional<std::string> key = random_bytes(min_state_key_size);
    return key;
}

StateSealer::StateSealer(std::vector<std::string> keys) : keys_(std::move(keys)) {
    if (keys_.empty()) throw std::invalid_argument("neomcp: a request state sealer needs a key");
    for (const auto& key : keys_) {
        if (key.size() < min_state_key_size) throw std::invalid_argument("neomcp: a request state key is too short");
    }
}

std::string StateSealer::seal(const StateClaims& claims) const {
    json payload = json::object();
    payload[std::string(version_member)] = format_version;
    payload[std::string(expires_member)] = claims.expires;
    payload[std::string(method_member)] = claims.method;
    payload[std::string(subject_member)] = claims.subject;
    payload[std::string(arguments_member)] = claims.arguments;
    payload[std::string(server_member)] = claims.server;
    payload[std::string(principal_member)] = claims.principal;
    payload[std::string(round_member)] = claims.round;
    json asked = json::object();
    for (const auto& [key, method] : claims.asked) asked[key] = method;
    payload[std::string(asked_member)] = std::move(asked);
    if (claims.state) payload[std::string(state_member)] = *claims.state;
    const std::string encoded = encode_base64url(serialize(payload));
    return encoded + separator + encode_base64url(code_of(keys_.front(), encoded));
}

std::variant<StateClaims, RequestStateProblem> StateSealer::open(std::string_view text) const {
    using Result = std::variant<StateClaims, RequestStateProblem>;
    const auto dot = text.find(separator);
    if (dot == std::string_view::npos) return Result(RequestStateProblem::malformed);
    const std::string_view payload_text = text.substr(0, dot);
    const auto code = decode_base64url(text.substr(dot + 1));
    if (!code || code->size() != sha256_size) return Result(RequestStateProblem::malformed);
    // Every key is tried, whichever matched, so that the time taken does not tell which.
    bool matched = false;
    for (const auto& key : keys_) {
        const bool equal = constant_time_equal(code_of(key, payload_text), *code);
        matched = matched || equal;
    }
    if (!matched) return Result(RequestStateProblem::signature);

    // Only what this server sealed gets here.
    const auto decoded = decode_base64url(payload_text);
    if (!decoded) return Result(RequestStateProblem::malformed);
    const json payload = json::parse(*decoded, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) return Result(RequestStateProblem::malformed);
    const auto version = integer_member(payload, version_member);
    if (!version || *version != format_version) return Result(RequestStateProblem::version);

    StateClaims claims;
    const auto expires = integer_member(payload, expires_member);
    const auto round = integer_member(payload, round_member);
    const std::string* method = text_member(payload, method_member);
    const std::string* subject = text_member(payload, subject_member);
    const std::string* arguments = text_member(payload, arguments_member);
    const std::string* server = text_member(payload, server_member);
    const std::string* principal = text_member(payload, principal_member);
    const json* asked = member(payload, asked_member);
    const auto* asked_members = asked == nullptr ? nullptr : asked->get_ptr<const json::object_t*>();
    if (!expires || !round || *round < 0 || method == nullptr || subject == nullptr || arguments == nullptr ||
        server == nullptr || principal == nullptr || asked_members == nullptr) {
        return Result(RequestStateProblem::malformed);
    }
    claims.expires = *expires;
    claims.round = static_cast<std::uint64_t>(*round);
    claims.method = *method;
    claims.subject = *subject;
    claims.arguments = *arguments;
    claims.server = *server;
    claims.principal = *principal;
    for (const auto& [key, value] : *asked_members) {
        const auto* asked_method = value.get_ptr<const std::string*>();
        if (asked_method == nullptr) return Result(RequestStateProblem::malformed);
        claims.asked.emplace(key, *asked_method);
    }
    if (const json* state = member(payload, state_member)) {
        const auto* state_text = state->get_ptr<const std::string*>();
        if (state_text == nullptr) return Result(RequestStateProblem::malformed);
        claims.state = *state_text;
    }
    return Result(std::move(claims));
}

std::optional<RequestStateProblem> check_claims(const StateClaims& claims, const StateBinding& binding,
                                                std::chrono::system_clock::time_point now) {
    const std::int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    if (seconds >= claims.expires) return RequestStateProblem::expired;
    if (claims.method != binding.method || claims.subject != binding.subject || claims.arguments != binding.arguments ||
        claims.server != binding.server || claims.principal != binding.principal) {
        return RequestStateProblem::binding;
    }
    return std::nullopt;
}

std::string canonical_json(const json& value) {
    std::string out;
    write_canonical(value, out);
    return out;
}

std::string arguments_digest(const json& arguments) {
    const Sha256Digest digest = sha256(canonical_json(arguments));
    return encode_base64url(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string principal_digest(const Authorization* granted) {
    if (granted == nullptr) return {};
    const Sha256Digest digest = sha256(principal_of(*granted).text());
    return encode_base64url(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

}  // namespace neomcp::detail
