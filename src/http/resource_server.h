// What the endpoint does with ResourceServerOptions that needs no state: the checks made when it starts, the metadata
// document, the checks of every validated token, and the scopes a 403 asks for. Internal to neomcp::http.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "neomcp/authorization.h"
#include "neomcp/http/outcome.h"
#include "neomcp/http/resource_server.h"

namespace neomcp::http::detail {

// The options, checked, with what is derived from them once.
struct ResourceServerConfig {
    ResourceServerOptions options;
    std::string metadata_url;                   // the public URL of the document, written in every challenge
    std::string metadata_path;                  // the path the document is served on
    std::vector<std::string> scopes_supported;  // what the document lists
    std::string no_credentials_challenge;       // the WWW-Authenticate values that do not depend on the request
    std::string invalid_request_challenge;
    std::string invalid_token_challenge;

    std::chrono::system_clock::time_point now() const;
};

// Checks the options and derives the rest; the failure names what is wrong.
Outcome<ResourceServerConfig> configure(ResourceServerOptions options);

// An empty string when the list is usable as authorization_servers, else what is wrong.
std::string check_authorization_servers(const std::vector<std::string>& issuers);

// The metadata document, serialized; or why it cannot be served (it would be too large for a client).
Outcome<std::string> metadata_document(const ResourceServerConfig& config, const std::vector<std::string>& issuers);

// The authority of an absolute URL (what a Host field would carry to reach it); empty when it has none.
std::optional<std::string> url_authority(std::string_view url);

// What the endpoint finds wrong with a validated token (see neomcp/http/resource_server.h); `none` when it passes.
enum class TokenProblem { none, shape, issuer, audience, expired, not_yet_valid };

TokenProblem check_token(const Authorization& granted, const ResourceServerConfig& config,
                         std::span<const std::string> issuers, std::chrono::system_clock::time_point now);

// The scopes a 403 asks for: `missing`, then required_scopes, then the scopes of `granted` the server names, each once
// and without offline_access; the granted part is left out when the whole would exceed what a client reads.
std::vector<std::string> challenge_scopes(std::span<const std::string> missing, const ResourceServerConfig& config,
                                          std::span<const std::string> granted);

}  // namespace neomcp::http::detail
