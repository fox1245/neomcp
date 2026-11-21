#include "http/resource_server.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "core/uri.h"
#include "neomcp/http/authority.h"
#include "neomcp/http/bearer.h"
#include "neomcp/http/challenge.h"
#include "neomcp/http/policy.h"
#include "neomcp/jsonrpc.h"

namespace neomcp::http {

namespace {

class FunctionValidator final : public TokenValidator {
public:
    explicit FunctionValidator(std::function<TokenVerdict(const TokenCheck&)> check) : check_(std::move(check)) {}

    void validate(const TokenCheck& check, TokenReply reply) override {
        TokenVerdict verdict = TokenRefusal::unavailable;
        try {
            verdict = check_(check);
        } catch (...) {
            verdict = TokenRefusal::unavailable;
        }
        reply(std::move(verdict));
    }

private:
    const std::function<TokenVerdict(const TokenCheck&)> check_;
};

}  // namespace

std::shared_ptr<TokenValidator> make_validator(std::function<TokenVerdict(const TokenCheck&)> check) {
    return std::make_shared<FunctionValidator>(std::move(check));
}

}  // namespace neomcp::http

namespace neomcp::http::detail {

namespace {

using neomcp::detail::parse_uri;
using neomcp::detail::percent_decode;
using neomcp::detail::Uri;

// RFC 9728, section 3: the well-known path, inserted between the host and the path of the resource identifier.
constexpr std::string_view well_known_path = "/.well-known/oauth-protected-resource";
// The members of the document (RFC 9728, section 2).
constexpr std::string_view member_resource = "resource";
constexpr std::string_view member_authorization_servers = "authorization_servers";
constexpr std::string_view member_scopes_supported = "scopes_supported";
constexpr std::string_view member_bearer_methods_supported = "bearer_methods_supported";
constexpr std::string_view member_resource_name = "resource_name";
constexpr std::string_view member_resource_documentation = "resource_documentation";
// The one way a token is read: the Authorization field (RFC 6750, section 2.1).
constexpr std::string_view bearer_method_header = "header";
// A scope that servers should not ask for (it asks for a refresh token, which is the client's business).
constexpr std::string_view offline_access_scope = "offline_access";
constexpr std::string_view https_scheme = "https";
constexpr std::string_view http_scheme = "http";
constexpr std::string_view root_path = "/";

// What is wrong with `url` as an https URL (or http on a loopback host) without user information, query or fragment;
// empty when nothing.
std::string check_url(std::string_view url, const std::string& what) {
    const Uri parts = parse_uri(url);
    if (!parts.scheme || !parts.authority) return what + " must be an absolute URL";
    if (*parts.scheme != https_scheme && *parts.scheme != http_scheme) {
        return what + " must be an https URL, or an http URL with a loopback host";
    }
    if (parts.query) return what + " must have no query";
    if (parts.fragment) return what + " must have no fragment";
    const std::optional<Authority> authority = parse_authority(*parts.authority);
    if (!authority) return what + " must have a host (and a port, when it names one) and no user information";
    if (*parts.scheme == http_scheme && !is_loopback_address(authority->host)) {
        return what + " must be an https URL, or an http URL with a loopback host";
    }
    return {};
}

std::string check_scopes(const std::vector<std::string>& scopes, const std::string& what) {
    for (const std::string& scope : scopes) {
        if (!http::is_scope_token(scope)) return what + " holds a value that is not a scope token";
        if (scope == offline_access_scope) return what + " must not hold " + std::string(offline_access_scope);
    }
    return {};
}

std::size_t joined_bytes(std::span<const std::string> scopes) {
    std::size_t bytes = 0;
    for (const std::string& scope : scopes) bytes += scope.size() + (bytes == 0 ? 0 : 1);
    return bytes;
}

bool fits_a_challenge(std::span<const std::string> scopes) {
    return scopes.size() <= ResourceServerDefaults::max_challenge_scopes &&
           joined_bytes(scopes) <= ResourceServerDefaults::max_challenge_scope_bytes;
}

bool contains(std::span<const std::string> scopes, std::string_view scope) {
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

// Adds `scope` to `scopes` unless it is there already or is offline_access, which a server does not ask for.
void add_scope(std::vector<std::string>& scopes, const std::string& scope) {
    if (scope == offline_access_scope || contains(scopes, scope)) return;
    scopes.push_back(scope);
}

std::string check_bounds(const ResourceServerBounds& bounds) {
    if (bounds.max_token_bytes < 1) return "max_token_bytes must be at least 1";
    if (bounds.max_pending_validations < 1) return "max_pending_validations must be at least 1";
    if (bounds.validation_timeout <= std::chrono::milliseconds::zero()) return "validation_timeout must be positive";
    if (bounds.max_cache_age < std::chrono::seconds::zero()) return "max_cache_age must not be negative";
    if (bounds.negative_cache_age < std::chrono::seconds::zero()) return "negative_cache_age must not be negative";
    if (bounds.leeway < std::chrono::seconds::zero() || bounds.leeway > ResourceServerDefaults::max_leeway) {
        return "leeway must be from 0 to " + std::to_string(ResourceServerDefaults::max_leeway.count()) + " seconds";
    }
    if (bounds.max_scopes < 1) return "max_scopes must be at least 1";
    if (bounds.metadata_max_age < std::chrono::seconds::zero()) return "metadata_max_age must not be negative";
    return {};
}

std::optional<std::string> challenge(BearerChallengeParts parts) {
    auto written = write_bearer_challenge(parts);
    if (auto* value = std::get_if<std::string>(&written)) return std::move(*value);
    return std::nullopt;
}

}  // namespace

std::chrono::system_clock::time_point ResourceServerConfig::now() const {
    return options.clock ? options.clock() : std::chrono::system_clock::now();
}

std::optional<std::string> ResourceServerConfig::resource_metadata() const {
    if (fixed_tokens) return std::nullopt;
    return metadata_url;
}

std::string check_authorization_servers(const std::vector<std::string>& issuers) {
    if (issuers.empty()) return "authorization_servers must name one authorization server at least";
    if (issuers.size() > ResourceServerDefaults::max_authorization_servers) {
        return "authorization_servers names more than " +
               std::to_string(ResourceServerDefaults::max_authorization_servers);
    }
    for (const std::string& issuer : issuers) {
        if (std::string problem = check_url(issuer, "an issuer of authorization_servers"); !problem.empty()) {
            return problem;
        }
    }
    return {};
}

Outcome<ResourceServerConfig> configure(ResourceServerOptions options) {
    using Result = Outcome<ResourceServerConfig>;
    if (!options.validator) return Result::failure("authorization needs a validator");
    if (std::string problem = check_bounds(options.bounds); !problem.empty()) return Result::failure(problem);
    if (std::string problem = check_url(options.resource, "resource"); !problem.empty()) return Result::failure(problem);
    if (std::string problem = check_authorization_servers(options.authorization_servers); !problem.empty()) {
        return Result::failure(problem);
    }
    if (std::string problem = check_scopes(options.required_scopes, "required_scopes"); !problem.empty()) {
        return Result::failure(problem);
    }
    if (!fits_a_challenge(options.required_scopes)) {
        return Result::failure("required_scopes are more than a client reads in one challenge");
    }
    if (options.scopes_supported) {
        if (std::string problem = check_scopes(*options.scopes_supported, "scopes_supported"); !problem.empty()) {
            return Result::failure(problem);
        }
    }
    if (options.resource_name && options.resource_name->empty()) return Result::failure("resource_name is empty");
    if (options.resource_documentation && options.resource_documentation->empty()) {
        return Result::failure("resource_documentation is empty");
    }
    for (const std::string& audience : options.accepted_audiences) {
        if (audience.empty()) return Result::failure("accepted_audiences holds an empty value");
    }
    if (options.metadata_path && options.metadata_path->empty()) return Result::failure("metadata_path is empty");

    ResourceServerConfig config;
    // The document's URL is made of the configured text: the part before the path, the well-known path, the path.
    // Nothing is normalized, so the URL a client builds from the same identifier is this one.
    const Uri parts = parse_uri(options.resource);
    const std::string_view origin =
        std::string_view(options.resource).substr(0, options.resource.size() - parts.path.size());
    if (std::string(origin) + parts.path != options.resource) {
        return Result::failure("resource cannot be split into its origin and its path");
    }
    const std::string_view path = parts.path == root_path ? std::string_view() : std::string_view(parts.path);
    config.metadata_url = std::string(origin) + std::string(well_known_path) + std::string(path);
    // The listener hands its handlers the path percent-decoded.
    config.metadata_path = options.metadata_path ? *options.metadata_path
                                                 : percent_decode(std::string(well_known_path) + std::string(path));
    config.scopes_supported =
        options.scopes_supported && !options.scopes_supported->empty() ? *options.scopes_supported
                                                                       : options.required_scopes;

    const auto no_credentials = challenge({config.metadata_url, options.required_scopes, std::nullopt, std::nullopt});
    const auto invalid_request =
        challenge({config.metadata_url, {}, std::string(bearer_error_invalid_request), std::nullopt});
    const auto invalid_token =
        challenge({config.metadata_url, options.required_scopes, std::string(bearer_error_invalid_token), std::nullopt});
    if (!no_credentials || !invalid_request || !invalid_token) {
        return Result::failure("the metadata URL made from resource cannot be written in a challenge");
    }
    config.no_credentials_challenge = *no_credentials;
    config.invalid_request_challenge = *invalid_request;
    config.invalid_token_challenge = *invalid_token;
    config.options = std::move(options);

    if (auto document = metadata_document(config, config.options.authorization_servers); !document) {
        return Result::failure(document.error);
    }
    return Result::success(std::move(config));
}

Outcome<ResourceServerConfig> configure_fixed(const FixedTokenOptions& options) {
    using Result = Outcome<ResourceServerConfig>;
    if (std::string problem = check_scopes(options.required_scopes, "required_scopes"); !problem.empty()) {
        return Result::failure(problem);
    }
    if (!fits_a_challenge(options.required_scopes)) {
        return Result::failure("required_scopes are more than a client reads in one challenge");
    }
    ResourceServerConfig config;
    config.fixed_tokens = true;
    config.options.required_scopes = options.required_scopes;
    config.options.clock = options.clock;
    config.options.bounds.leeway = std::chrono::seconds::zero();
    // What a 403 may echo of the granted scopes, besides what the items declare: what every request needs.
    config.scopes_supported = options.required_scopes;
    const auto no_credentials = challenge({std::nullopt, options.required_scopes, std::nullopt, std::nullopt});
    const auto invalid_request = challenge({std::nullopt, {}, std::string(bearer_error_invalid_request), std::nullopt});
    const auto invalid_token =
        challenge({std::nullopt, options.required_scopes, std::string(bearer_error_invalid_token), std::nullopt});
    if (!no_credentials || !invalid_request || !invalid_token) {
        return Result::failure("the challenges cannot be written");
    }
    config.no_credentials_challenge = *no_credentials;
    config.invalid_request_challenge = *invalid_request;
    config.invalid_token_challenge = *invalid_token;
    return Result::success(std::move(config));
}

Outcome<std::string> metadata_document(const ResourceServerConfig& config, const std::vector<std::string>& issuers) {
    json document = json::object();
    document[std::string(member_resource)] = config.options.resource;
    json servers = json::array();
    for (const std::string& issuer : issuers) servers.push_back(issuer);
    document[std::string(member_authorization_servers)] = std::move(servers);
    if (!config.scopes_supported.empty()) {
        json scopes = json::array();
        for (const std::string& scope : config.scopes_supported) scopes.push_back(scope);
        document[std::string(member_scopes_supported)] = std::move(scopes);
    }
    json methods = json::array();
    methods.push_back(std::string(bearer_method_header));
    document[std::string(member_bearer_methods_supported)] = std::move(methods);
    if (config.options.resource_name) document[std::string(member_resource_name)] = *config.options.resource_name;
    if (config.options.resource_documentation) {
        document[std::string(member_resource_documentation)] = *config.options.resource_documentation;
    }
    std::string text = serialize(document);
    if (text.size() > ResourceServerDefaults::max_metadata_bytes) {
        return Outcome<std::string>::failure("the metadata document would have more than " +
                                             std::to_string(ResourceServerDefaults::max_metadata_bytes) +
                                             " bytes, more than a client reads");
    }
    return Outcome<std::string>::success(std::move(text));
}

std::optional<std::string> url_authority(std::string_view url) { return parse_uri(url).authority; }

TokenProblem check_token(const Authorization& granted, const ResourceServerConfig& config,
                         std::span<const std::string> issuers, std::chrono::system_clock::time_point now) {
    const ResourceServerOptions& options = config.options;
    if (granted.issuer.empty() || !granted.expires || granted.scopes.size() > options.bounds.max_scopes) {
        return TokenProblem::shape;
    }
    if (std::find(issuers.begin(), issuers.end(), granted.issuer) == issuers.end()) return TokenProblem::issuer;
    const bool for_this_resource =
        std::any_of(granted.audiences.begin(), granted.audiences.end(), [&](const std::string& audience) {
            return audience == options.resource ||
                   std::find(options.accepted_audiences.begin(), options.accepted_audiences.end(), audience) !=
                       options.accepted_audiences.end();
        });
    if (!for_this_resource) return TokenProblem::audience;
    // Written so that no sum can overflow, whatever expiry a validator returns: the leeway is at most
    // ResourceServerDefaults::max_leeway, and `now` is a time the clock gave.
    if (now - options.bounds.leeway >= *granted.expires) return TokenProblem::expired;
    if (granted.not_before && now + options.bounds.leeway < *granted.not_before) return TokenProblem::not_yet_valid;
    return TokenProblem::none;
}

std::vector<std::string> needed_scopes(std::span<const std::string> missing, const ResourceServerConfig& config) {
    std::vector<std::string> scopes;
    for (const std::string& scope : missing) add_scope(scopes, scope);
    for (const std::string& scope : config.options.required_scopes) add_scope(scopes, scope);
    return scopes;
}

Outcome<bool> adopt_declarations(ResourceServerConfig& config, const std::vector<std::vector<std::string>>& declarations) {
    std::vector<std::string> declared;
    for (const std::vector<std::string>& declaration : declarations) {
        if (!fits_a_challenge(needed_scopes(declaration, config))) {
            return Outcome<bool>::failure(
                "an item declares scopes that, with required_scopes, are more than a client reads in one challenge (" +
                std::to_string(ResourceServerDefaults::max_challenge_scopes) + " scopes, " +
                std::to_string(ResourceServerDefaults::max_challenge_scope_bytes) + " bytes)");
        }
        for (const std::string& scope : declaration) {
            if (!contains(declared, scope)) declared.push_back(scope);
        }
    }
    config.declared_scopes = std::move(declared);
    return Outcome<bool>::success(true);
}

std::vector<std::string> challenge_scopes(std::span<const std::string> missing, const ResourceServerConfig& config,
                                          std::span<const std::string> granted) {
    std::vector<std::string> scopes = needed_scopes(missing, config);
    // The server names a scope in required_scopes, in scopes_supported (what the metadata document lists) or in the
    // declaration of one of its items.
    const auto named = [&](const std::string& scope) {
        return contains(config.options.required_scopes, scope) || contains(config.scopes_supported, scope) ||
               contains(config.declared_scopes, scope);
    };
    const std::size_t needed = scopes.size();
    for (const std::string& scope : granted) {
        if (named(scope)) add_scope(scopes, scope);
    }
    if (!fits_a_challenge(scopes)) scopes.resize(needed);
    // What one item needs always fits (adopt_declarations); what several need together (a batch, a subscription that
    // names several resources) may not: what the request lacks is then cut from its end to what fits, so that the
    // answer is never a challenge a client refuses to read. required_scopes stay: they fit on their own (configure),
    // and a token without them is refused whatever else it holds.
    while (!fits_a_challenge(scopes)) {
        const auto last = std::find_if(scopes.rbegin(), scopes.rend(), [&](const std::string& scope) {
            return !contains(config.options.required_scopes, scope);
        });
        if (last == scopes.rend()) break;
        scopes.erase(std::next(last).base());
    }
    return scopes;
}

}  // namespace neomcp::http::detail
