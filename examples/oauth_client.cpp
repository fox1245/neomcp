// Example 7: a client that signs in to a server that asks for OAuth authorization.
//
// A server that asks for it answers a request without a valid token with a 401 and a pointer to its authorization
// server. neomcp::OAuthAuthorizer follows that pointer, registers the client where it must, and then needs one thing
// only a person can do: sign in on the authorization server's page. The library never opens a browser. It hands the
// application the address to open, and the application hands back the address the browser ended up at (the redirect
// URI, with the result of the sign-in in its query).
//
// This program does that on the terminal. It prints the address; you open it in a browser, sign in, copy the address
// the browser went to (the page itself will not load, since nothing listens at the redirect URI) and paste it here.
// The tokens live in memory for the life of the program.
//
// Usage: oauth_client URL-of-the-server [redirect URI]
// The redirect URI is http://127.0.0.1:8765/callback without one. Where it is registered with the authorization
// server must be the same.
//
// This example is built but not run by the tests: it needs an authorization server and a person.
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <neomcp/client.h>
#include <neomcp/client/http.h>
#include <neomcp/client/oauth.h>

#include "client_support.h"

namespace {
constexpr const char* program = "oauth_client";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: oauth_client URL-of-the-server [redirect URI]\n";
        return 2;
    }

    neomcp::OAuthOptions oauth;
    oauth.redirect_uri = argc > 2 ? argv[2] : "http://127.0.0.1:8765/callback";
    // The name an authorization server may show the person when the client registers itself there.
    oauth.client_name = "neomcp example";

    // Called when a sign-in is needed. A plain handler like this one runs on a thread of its own, so it may block until
    // the person is back.
    oauth.prompt.handler([](const neomcp::AuthorizationPromptInfo& prompt,
                            neomcp::AuthorizationPromptContext& context) {
        std::cout << "To sign in to " << prompt.issuer << ", open this address in a browser:\n\n  "
                  << prompt.authorization_url << "\n\nWhen the browser has gone to " << prompt.redirect_uri
                  << " (the page will not load), paste the address from its address bar and press Enter.\n"
                  << "Press Enter alone to give up.\n> " << std::flush;
        std::string pasted;
        // The sign-in stops waiting when its time runs out: context.cancelled() says so. A read from the terminal
        // cannot be interrupted, so this handler looks at it once the person has answered.
        if (!std::getline(std::cin, pasted) || pasted.empty() || context.cancelled()) {
            return neomcp::AuthorizationPromptAnswer{};  // no address: the person gave up
        }
        return neomcp::AuthorizationPromptAnswer{pasted};
    });

    auto authorizer = neomcp::OAuthAuthorizer::create(std::move(oauth));
    if (!authorizer) return report(program, "creating the authorizer", authorizer.error());

    neomcp::HttpEndpoint endpoint;
    endpoint.url = argv[1];
    endpoint.authorizer = *authorizer;

    neomcp::ClientOptions options;
    options.client_info.name = "example-client";
    options.client_info.version = "1.0.0";

    // The first request the server refuses starts the sign-in, and the call waits for it. If it fails, the error's
    // `authorization` says what went wrong, without any secret.
    auto opened = neomcp::Client::open(options, std::move(endpoint));
    if (!opened) {
        const neomcp::ClientError& error = opened.error();
        if (error.authorization) {
            std::cerr << program << ": the sign-in failed: " << neomcp::to_string(error.authorization->problem) << '\n';
        }
        return report(program, "opening the client", error);
    }
    neomcp::Client client = std::move(opened).value();

    const auto tools = client.list_all_tools();
    if (!tools) return report(program, "listing the tools", tools.error());
    std::cout << "signed in; the server offers " << tools->items.size() << " tools:\n";
    for (const neomcp::ToolDefinition& tool : tools->items) std::cout << "  " << tool.name << '\n';
    return 0;
}
