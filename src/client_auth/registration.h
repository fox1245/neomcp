// How the client is known to an authorization server, and how it authenticates at the token endpoint.
//
// In order of preference: credentials the application registered beforehand for exactly this issuer; a client ID
// metadata document URL the application hosts, when the server says it supports them; a registration made earlier
// at this server by the flow; a new dynamic registration (RFC 7591), when it is enabled and the server names a
// registration endpoint. Credentials and registrations of one issuer are never used at another.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "client_auth/bounds.h"
#include "client_auth/metadata.h"
#include "client_auth/types.h"

namespace neomcp::detail::oauth {

struct RegistrationConfig {
    std::string redirect_uri;
    std::string client_name;
    std::optional<ApplicationType> application_type;  // not set: native for a loopback redirect URI, web otherwise
    std::vector<PreRegisteredClient> pre_registered;
    std::optional<std::string> client_id_metadata_url;
    bool dynamic_registration = true;
};

enum class RegistrationSource { pre_registered, client_id_metadata_document, dynamic_stored, dynamic_new };

// The client as it presents itself at one authorization server.
struct ClientCredentials {
    std::string client_id;
    std::optional<std::string> client_secret;
    TokenEndpointAuth method = TokenEndpointAuth::none;
    RegistrationSource source = RegistrationSource::pre_registered;
};

// A dynamic registration is to be made at the server's registration endpoint.
struct NeedsRegistration {};

std::variant<ClientCredentials, NeedsRegistration, FlowFailure> choose_registration(
    const AuthorizationServer& server, const RegistrationConfig& config, const std::optional<StoredClient>& stored);

// The method of client authentication at the token endpoint: `preferred` when the server lists it or lists none;
// else, with a secret, client_secret_basic when listed (or nothing is listed), then client_secret_post when listed;
// else none. A method that needs a secret is never chosen without one.
TokenEndpointAuth choose_token_endpoint_auth(std::optional<TokenEndpointAuth> preferred, bool has_secret,
                                             const std::optional<std::vector<std::string>>& listed);

// The credentials of a client registered dynamically, with the method chosen for this server.
ClientCredentials credentials_of(const StoredClient& client, const AuthorizationServer& server,
                                 RegistrationSource source);

// The JSON body of a dynamic registration request.
std::string registration_request_body(const RegistrationConfig& config);

ApplicationType application_type_for(const RegistrationConfig& config);

// The registration the server answered, or why it cannot be used. When the answer lists the redirect URIs it
// registered (RFC 7591, section 3.2.1: the server may change what the client asked for), `redirect_uri`, the one this
// client sends, must be among them exactly; a list that is not an array of strings, or that leaves it out, refuses
// the registration.
std::variant<StoredClient, FlowFailure> read_registration_answer(const FetchResult& result, std::string_view issuer,
                                                                 std::string_view registration_endpoint,
                                                                 std::string_view redirect_uri,
                                                                 const OAuthBounds& bounds);

// A client ID metadata document URL the application gives: https, a path other than "/", no "." or ".." segment, no
// fragment, no user information. Empty when it can be used; what is wrong, otherwise. A query is allowed (it is
// discouraged); `has_query` says whether there is one.
std::optional<std::string> check_client_id_metadata_url(std::string_view url, bool& has_query);

// A redirect URI the application gives: https, or http to a loopback host, without fragment. Empty when it can be
// used; what is wrong, otherwise.
std::optional<std::string> check_redirect_uri(std::string_view uri);

}  // namespace neomcp::detail::oauth
