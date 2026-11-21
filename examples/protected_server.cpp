// Example 8: the server of example 2 as an OAuth protected resource.
//
// With authorization configured, every request to the endpoint needs an access token in an `Authorization: Bearer`
// header. The server answers a request without a valid one with the challenge that tells a client where to sign in, and
// it publishes the document (the protected resource metadata) that names its authorization servers. What the library
// does not do is decide what a token means: the application supplies a TokenValidator for that. In a real server it
// would check a signed token or ask the authorization server about an opaque one.
//
// FOR DEMONSTRATION ONLY. The validator below is a fixed table of two made-up tokens, written in the source of this
// program. A real server must not look like this: tokens come from an authorization server, they are secret, and
// they expire. The table is here so that the example needs no authorization server to run. With it you can try the
// server from a shell, or with example 4:
//
//   ./protected_server 8000
//   ./client_http http://127.0.0.1:8000/mcp demo-token-writer
//   curl -i http://127.0.0.1:8000/mcp                       # 401 and the challenge
//   curl http://127.0.0.1:8000/.well-known/oauth-protected-resource/mcp
//
// Usage: protected_server [port]   (8000 without one; the port is part of the resource's address, so 0 is refused)
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <neomcp/http_server.h>
#include <neomcp/server.h>

#include "demo_server.h"

namespace {

// The made-up table: each token and the scopes it grants. Scopes are the permissions a token carries; the server
// below asks for one to read the notes and another to change them.
const std::map<std::string, std::vector<std::string>, std::less<>> demonstration_tokens{
    {"demo-token-reader", {"notes:read"}},
    {"demo-token-writer", {"notes:read", "notes:write"}},
};

// The authorization server that issued the tokens, as the tokens name it. The library compares it exactly with the
// ones the server lists. Nothing contacts it in this example.
constexpr const char* demonstration_issuer = "https://auth.example.com";

}  // namespace

int main(int argc, char** argv) {
    neomcp::HttpServerOptions options;
    options.port = 8000;
    if (argc > 1) {
        const std::string_view text = argv[1];
        const char* const end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, options.port);
        if (parsed.ec != std::errc{} || parsed.ptr != end || options.port == 0) {
            std::cerr << "protected_server: \"" << text << "\" is not a port (1 to 65535)\n";
            return EXIT_FAILURE;
        }
    }

    Notes notes;
    neomcp::Server server({.name = "notes-and-calc", .version = "1.0.0"});
    const DemoHandles handles = add_demo_features(server, notes);
    // Scopes an item needs, all of them: a request whose token lacks one is refused before anything of the item runs.
    // Reading the list of notes is allowed to both tokens, storing a note only to the second. The tools of the
    // calculator declare none, so any valid token may use them.
    handles.notes.scopes({"notes:read"});
    handles.add_note.scopes({"notes:write"});

    neomcp::http::ResourceServerOptions authorization;
    // The resource's own address, exactly as clients are given it. A token must name it as its audience.
    authorization.resource = "http://" + options.bind_address + ":" + std::to_string(options.port) + options.path;
    authorization.authorization_servers = {demonstration_issuer};
    authorization.scopes_supported = std::vector<std::string>{"notes:read", "notes:write"};
    authorization.resource_name = "Notes and calculator";

    // The validator says what a token means: who it is for, its scopes, until when it holds. It does not repeat the
    // checks the library makes whatever the validator is: that the issuer is one of authorization_servers, that the
    // audience is this resource, that the token has not expired, and that the scopes the endpoint requires are there.
    authorization.validator = neomcp::http::make_validator(
        [](const neomcp::http::TokenCheck& check) -> neomcp::http::TokenVerdict {
            const auto known = demonstration_tokens.find(check.token);
            if (known == demonstration_tokens.end()) return neomcp::http::TokenRefusal::invalid;
            neomcp::Authorization granted;
            granted.issuer = demonstration_issuer;
            granted.subject = "demonstration-user";
            granted.scopes = known->second;
            granted.audiences = {std::string(check.resource)};
            granted.expires = std::chrono::system_clock::now() + std::chrono::hours(1);
            return granted;
        });
    options.authorization = std::move(authorization);

    options.on_listening = [address = options.bind_address, path = options.path](std::uint16_t port) {
        std::cout << "http://" << address << ':' << port << path << std::endl;
    };
    return neomcp::run_http(server, options);
}
