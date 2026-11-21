#include "http/fixed_tokens.h"

#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "crypto/hmac.h"
#include "neomcp/http/bearer.h"
#include "neomcp/http/challenge.h"

namespace neomcp::http {

namespace {

using neomcp::detail::Sha256Digest;

// Whether `token` is a b64token that a client can send: it is read back by the reader of the Authorization field, from
// the field a client would write, and must come out whole. What the reader refuses or trims (a space, a character
// outside the b64token set, padding in the middle) is not a token.
std::optional<FixedTokenProblem> check_token_text(const std::string& token) {
    if (token.empty()) return FixedTokenProblem::empty_token;
    std::string field(bearer_scheme);
    field.push_back(' ');
    field.append(token);
    const std::string_view fields[] = {field};
    auto read = read_bearer_credentials(std::span<const std::string_view>(fields),
                                        CredentialsLimits{FixedTokenDefaults::max_token_bytes});
    std::optional<FixedTokenProblem> problem;
    if (auto* credentials = std::get_if<BearerCredentials>(&read)) {
        if (credentials->token != token) problem = FixedTokenProblem::malformed_token;
        neomcp::detail::wipe(credentials->token.data(), credentials->token.size());
    } else if (std::get<CredentialsProblem>(read) == CredentialsProblem::token_too_large) {
        problem = FixedTokenProblem::token_too_large;
    } else {
        problem = FixedTokenProblem::malformed_token;
    }
    neomcp::detail::wipe(field.data(), field.size());
    return problem;
}

std::optional<FixedTokenProblem> check_entry(const FixedToken& entry, const FixedTokenBounds& bounds) {
    if (auto problem = check_token_text(entry.token)) return problem;
    if (entry.subject.empty()) return FixedTokenProblem::empty_subject;
    if (entry.scopes.size() > bounds.max_scopes) return FixedTokenProblem::too_many_scopes;
    for (const std::string& scope : entry.scopes) {
        if (!is_scope_token(scope)) return FixedTokenProblem::invalid_scope;
    }
    return std::nullopt;
}

void wipe_tokens(std::vector<FixedToken>& tokens) {
    for (FixedToken& entry : tokens) neomcp::detail::wipe(entry.token.data(), entry.token.size());
}

}  // namespace

std::string_view to_string(FixedTokenProblem problem) noexcept {
    switch (problem) {
        case FixedTokenProblem::invalid_bounds: return "a bound of the table is 0";
        case FixedTokenProblem::too_many_tokens: return "the table has more tokens than its bound";
        case FixedTokenProblem::empty_token: return "a token is empty";
        case FixedTokenProblem::malformed_token: return "a token is not a bearer token (b64token) a client can send";
        case FixedTokenProblem::token_too_large: return "a token is longer than the listener lets through";
        case FixedTokenProblem::duplicate_token: return "two entries have the same token";
        case FixedTokenProblem::empty_subject: return "a subject is empty";
        case FixedTokenProblem::too_many_scopes: return "an entry has more scopes than its bound";
        case FixedTokenProblem::invalid_scope: return "a scope is not a scope token";
    }
    return {};
}

FixedTokenTable::FixedTokenTable() : entries_(std::make_shared<const Entries>()) {}

FixedTokenTable::FixedTokenTable(std::shared_ptr<const Entries> entries) : entries_(std::move(entries)) {}

std::variant<FixedTokenTable, FixedTokenError> FixedTokenTable::create(std::vector<FixedToken>&& tokens,
                                                                       const FixedTokenBounds& bounds) {
    const auto refuse = [&tokens](FixedTokenProblem problem, std::size_t index) {
        wipe_tokens(tokens);
        return std::variant<FixedTokenTable, FixedTokenError>(FixedTokenError{problem, index});
    };
    if (bounds.max_tokens < 1 || bounds.max_scopes < 1) return refuse(FixedTokenProblem::invalid_bounds, 0);
    if (tokens.size() > bounds.max_tokens) return refuse(FixedTokenProblem::too_many_tokens, 0);

    auto entries = std::make_shared<Entries>();
    entries->digests.reserve(tokens.size());
    entries->grants.reserve(tokens.size());
    std::set<Sha256Digest> seen;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (auto problem = check_entry(tokens[index], bounds)) return refuse(*problem, index);
        const Sha256Digest digest = neomcp::detail::sha256(tokens[index].token);
        // Two tokens with one digest are one token (SHA-256 has no known collision): the later entry is refused.
        if (!seen.insert(digest).second) return refuse(FixedTokenProblem::duplicate_token, index);
        Authorization granted;
        granted.subject = std::move(tokens[index].subject);
        granted.scopes = std::move(tokens[index].scopes);
        granted.expires = tokens[index].expires;
        entries->digests.push_back(digest);
        entries->grants.push_back(std::make_shared<const Authorization>(std::move(granted)));
    }
    wipe_tokens(tokens);
    return FixedTokenTable(std::move(entries));
}

std::shared_ptr<const Authorization> FixedTokenTable::find(std::string_view token) const {
    const Sha256Digest digest = neomcp::detail::sha256(token);
    const std::size_t found = detail::scan_digests(
        entries_->digests, digest, [](const Sha256Digest& left, const Sha256Digest& right) {
            return neomcp::detail::constant_time_equal(
                std::string_view(reinterpret_cast<const char*>(left.data()), left.size()),
                std::string_view(reinterpret_cast<const char*>(right.data()), right.size()));
        });
    if (found == entries_->grants.size()) return nullptr;
    return entries_->grants[found];
}

std::size_t FixedTokenTable::size() const noexcept { return entries_->digests.size(); }

}  // namespace neomcp::http
