// The OAuth authorization flow of the client, without I/O: from a server's refusal (401, or 403 for want of scope)
// to new credentials or a failure.
//
// The flow is a state machine. It is started with what the refused request met, and each step it returns says what
// it waits for: a request to send (FlowFetch, answered with fetched()), the person's visit to the authorization
// server (PromptRequest, answered with prompted()), or its end (FlowDone, FlowFailed). It sends nothing, opens
// nothing and keeps no clock of its own; the time of each input is given, the random octets come from the source it
// is built with, and the stored credentials are read through CredentialLookup and changed only by the StoreUpdate it
// ends with.
//
// The steps of a full flow:
//   1. protected resource metadata (discovery.h), fetched again by every flow, which is how a changed
//      authorization server is noticed;
//   2. when the resource or the authorization server differs from the one whose tokens are in use, those tokens, and
//      the dynamic registration at the old server, are dropped: they are never sent to or used at the new one;
//   3. authorization server metadata (metadata.h), kept per issuer for the life of the FlowMemory;
//   4. after a 401 to a request that carried a token, one refresh when a refresh token is stored for this resource and
//      server; a refusal (4xx) erases it and the flow goes on;
//   5. the client registration (registration.h), and a dynamic registration when that is the one that applies;
//   6. the scopes: on a 401 those of the challenge, else the resource's `scopes_supported`, else none; on a 403 for
//      scope the union of the last request's and the challenge's, which must be strictly larger than the last
//      request's; `offline_access` added when the option is on and the authorization server lists it;
//   7. PKCE, state and the authorization URL (callback.h), and the prompt;
//   8. the callback check, then the token request (tokens.h).
//
// Bounds: a request may be refused and held at most max_authorizations_per_request times; an operation may start at
// most max_step_ups_per_operation + 1 flows in a row without a success (FlowMemory::succeeded resets the count); the
// whole flow ends after authorization_timeout of the given time.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "client_auth/admission.h"
#include "client_auth/bounds.h"
#include "client_auth/callback.h"
#include "client_auth/discovery.h"
#include "client_auth/metadata.h"
#include "client_auth/pkce.h"
#include "client_auth/registration.h"
#include "client_auth/types.h"
#include "neomcp/profile.h"

namespace neomcp::detail::oauth {

// The stored credentials, read only. Called on the thread that drives the flow; must return promptly.
class CredentialLookup {
public:
    virtual ~CredentialLookup() = default;
    virtual std::optional<StoredTokens> tokens(const CredentialKey& key) const = 0;
    virtual std::optional<StoredClient> client(const std::string& issuer) const = 0;
};

struct FlowConfig {
    std::string endpoint_url;  // as the application wrote it
    RegistrationConfig registration;
    bool request_offline_access = true;
    AcceptResource accept_resource;  // not set: a resource that differs is refused
    // Set when an enabled revision finds the authorization server through metadata on the server's origin
    // (origin_metadata_of). Discovery on the server's origin is not supported yet: without protected resource
    // metadata the flow ends as it does without this.
    std::optional<profile::OriginMetadataDiscovery> origin_metadata;
    OAuthBounds bounds;
};

// The discovery on the server's origin that the first of `profiles` declaring one uses, if any.
std::optional<profile::OriginMetadataDiscovery> origin_metadata_of(std::span<const profile::ProfilePtr> profiles);

// What an authorizer keeps from one flow to the next.
struct FlowMemory {
    std::map<std::string, AuthorizationServer> servers;  // validated metadata, by issuer
    std::optional<CredentialKey> active;                  // whose tokens are in use
    std::map<std::string, std::size_t> attempts;          // flows started per operation since its last success

    // A request of the operation succeeded with the current credentials.
    void succeeded(const std::string& operation) { attempts.erase(operation); }
};

// What the refused request met.
struct Trigger {
    int status = 0;                       // 401 or 403
    std::vector<std::string> challenges;  // the values of its WWW-Authenticate fields, as sent
    bool carried_token = false;           // the request was sent with an access token
    std::string operation;                // the operation's key, for the bound on step-ups
    std::size_t refusals = 0;             // how often this request was refused and held before
    // Header lines for the metadata requests to the endpoint's origin (the protocol version the request carried);
    // never sent to another origin.
    std::vector<HeaderLine> resource_headers;
};

struct FlowDone {
    CredentialKey key;  // the credentials to send from now on
    StoreUpdate updates;
    bool refreshed = false;
};

struct FlowFailed {
    FlowFailure failure;
    StoreUpdate updates;
};

// The input did not belong to what the flow waits for (an answer with another id, an input after the end); nothing
// changed.
struct FlowIgnored {};

using FlowStep = std::variant<FlowFetch, PromptRequest, FlowDone, FlowFailed, FlowIgnored>;

class OAuthFlow {
public:
    // The references must outlive the flow.
    OAuthFlow(const FlowConfig& config, const UrlAdmission& admission, const CredentialLookup& lookup,
              RandomSource random, FlowMemory& memory);

    FlowStep start(const Trigger& trigger, FlowClock::time_point now);
    FlowStep fetched(std::uint64_t id, const FetchResult& result, FlowClock::time_point now);
    FlowStep prompted(const PromptAnswer& answer, FlowClock::time_point now);

    // What the flow noticed and went on from, since the last call.
    std::vector<FlowDiagnostic> take_diagnostics();

    bool finished() const noexcept { return stage_ == Stage::finished; }

private:
    enum class Stage { idle, resource_metadata, server_metadata, refresh, registration, prompt, token, finished };

    FlowStep fail(AuthorizationProblem problem, std::string_view url = {});
    FlowStep fail(FlowFailure failure);
    FlowStep fetch(FlowFetch request, Stage stage);
    FlowStep metadata_fetch(FetchPurpose purpose, const std::string& url, Stage stage);
    bool expired(FlowClock::time_point now) const;

    FlowStep on_resource_metadata(const FetchResult& result);
    FlowStep no_resource_metadata();
    FlowStep after_resource_metadata();
    FlowStep on_server_metadata(const FetchResult& result);
    FlowStep after_server_metadata();
    FlowStep on_refresh(const FetchResult& result);
    FlowStep register_client();
    FlowStep on_registration(const FetchResult& result);
    FlowStep authorize();
    FlowStep on_token(const FetchResult& result);

    std::optional<StoredTokens> stored_tokens(const CredentialKey& key) const;
    std::optional<StoredClient> stored_client(const std::string& issuer) const;
    void drop_client(const std::string& issuer);

    const FlowConfig& config_;
    const UrlAdmission& admission_;
    const CredentialLookup& lookup_;
    RandomSource random_;
    FlowMemory& memory_;

    Stage stage_ = Stage::idle;
    FlowClock::time_point started_{};
    FlowClock::time_point now_{};  // the time of the input being handled
    std::uint64_t next_id_ = 1;
    std::uint64_t waiting_id_ = 0;
    Trigger trigger_;
    bool step_up_ = false;
    bool challenge_named_scope_ = false;
    std::vector<std::string> challenge_scopes_;
    bool changed_ = false;  // the resource or the authorization server differs from the one in use

    std::vector<PrmCandidate> resource_candidates_;
    std::size_t resource_index_ = 0;
    ProtectedResource resource_;
    CredentialKey key_;
    std::vector<std::string> server_candidates_;
    std::size_t server_index_ = 0;
    AuthorizationServer server_;
    std::size_t refresh_attempts_ = 0;
    std::optional<StoredTokens> refreshed_from_;
    ClientCredentials client_;
    std::vector<std::string> scopes_;
    Pkce pkce_;
    PendingAuthorization pending_;

    StoreUpdate updates_;
    std::vector<FlowDiagnostic> diagnostics_;
};

}  // namespace neomcp::detail::oauth
