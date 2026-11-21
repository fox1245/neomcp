// Receiving the browser's return to a loopback address (the target neomcp::client_oauth_loopback): a prompt handler for
// OAuthAuthorizer that listens for the one request the browser makes when the person has signed in, so that a desktop
// application need not write a listener of its own.
//
// A LoopbackRedirect gives the application the redirect URI to put in OAuthOptions::redirect_uri, and the prompt
// handler to put in OAuthOptions::prompt:
//
//   neomcp::LoopbackRedirectOptions loopback;
//   loopback.open_browser = [](const std::string& url) { open_in_browser(url); };  // the application's own
//   auto redirect = neomcp::LoopbackRedirect::create(std::move(loopback));
//   neomcp::OAuthOptions oauth;
//   oauth.redirect_uri = (*redirect)->redirect_uri();   // http://127.0.0.1:<port>/callback
//   oauth.prompt = (*redirect)->plain_prompt();
//
// What a prompt does. It binds 127.0.0.1 (and nothing else), hands the authorization URL to open_browser, waits for
// one request, and answers the browser with a short page that is the same whatever came back. That request is the
// first GET to the redirect URI's path that is not larger than the callback bound; the prompt then closes the
// listener, so that the port is open for the person's visit only, and returns the URL the browser came back to,
// whole, to the authorizer, which makes every check of it (the state, the issuer, the error and the code). Nothing
// here reads the query. Any other path is answered 404 and any other method 405 (and a request larger than the bound
// 414), none of them ends the wait. The wait ends without an answer when the sign-in no longer waits for it (the
// prompt's time ran out, or the client closed): the listener is closed then too.
//
// Which port. The redirect URI must be known before the sign-in, but the port is open only during it. create() binds
// the port once to learn it (LoopbackRedirectOptions::port 0: one the system chooses) and closes it; every prompt then
// binds that same port again. Another program may take the port in between: the prompt then ends as a person who gave
// up and last_problem() says why. An application that cannot accept that gives the port itself and keeps it free.
//
// Threads. The plain prompt runs on the thread the authorizer gives a plain handler and blocks it; the coroutine
// prompt runs on the client's thread and never blocks it (closing the listener, which can take a moment, happens on a
// thread of its own and the prompt returns after it). open_browser is called on the prompt's own thread, which for the
// coroutine prompt is the client's: it must return promptly. One sign-in waits on a LoopbackRedirect at a time; a
// prompt that starts while another waits ends at once as a person who gave up.
//
// The listener is the library's own (neomcp::http::Listener), so the request is read and bounded as that listener
// reads every request; the callback bound applies on top of its own limit on the request line.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "neomcp/client/oauth.h"
#include "neomcp/client/oauth_types.h"
#include "neomcp/client/result.h"

namespace neomcp {

// The defaults of LoopbackRedirectOptions and the fixed values of the listener, as constants.
struct LoopbackRedirectDefaults {
    // The only address a LoopbackRedirect binds, and the host of its redirect URI.
    static constexpr const char* host = "127.0.0.1";
    // The path the browser comes back to.
    static constexpr const char* path = "/callback";
    // The longest callback URL accepted; the authorizer's own default bound for the callback it is handed.
    static constexpr std::size_t max_callback_bytes = OAuthDefaults::max_callback_bytes;
    // How long the listener waits for the bytes of a request that has begun.
    static constexpr std::chrono::milliseconds read_timeout{5000};
    // Browsers open connections they do not use at once, and each waits for a thread until it sends or is dropped;
    // these keep an idle one from delaying the browser's request.
    static constexpr std::size_t worker_threads = 2;
    static constexpr std::size_t max_worker_threads = 8;
    static constexpr std::size_t max_queued_connections = 16;
};

struct LoopbackRedirectOptions {
    // The port of the redirect URI; 0: one the system chooses when the redirect is created.
    std::uint16_t port = 0;
    // The path of the redirect URI: it starts with '/' and holds only letters, digits and '-', '_', '~', '/'.
    std::string path = LoopbackRedirectDefaults::path;
    // Opens the authorization URL in the person's browser, or shows it to the person; required. The library never
    // starts a browser itself. A function that throws counts as a person who gave up.
    std::function<void(const std::string& authorization_url)> open_browser;
    // The longest callback URL (scheme, host, port, path and query) a request may make. Keep it equal to
    // OAuthBounds::max_callback_bytes of the authorizer; the listener's own limit on the request line applies too.
    std::size_t max_callback_bytes = LoopbackRedirectDefaults::max_callback_bytes;
    // How long the listener waits for the next bytes of a request.
    std::chrono::milliseconds read_timeout = LoopbackRedirectDefaults::read_timeout;
};

class LoopbackRedirect {
public:
    // A redirect for these options, or invalid_options when one cannot work (no open_browser, a path or a bound that
    // is not admitted, a port that is in use). It binds the port to find out and closes it again.
    static Result<std::shared_ptr<LoopbackRedirect>> create(LoopbackRedirectOptions options);

    ~LoopbackRedirect();
    LoopbackRedirect(const LoopbackRedirect&) = delete;
    LoopbackRedirect& operator=(const LoopbackRedirect&) = delete;

    // http://127.0.0.1:<port><path>: for OAuthOptions::redirect_uri.
    const std::string& redirect_uri() const noexcept;
    std::uint16_t port() const noexcept;

    // Prompt handlers for OAuthOptions::prompt; each holds what it needs, so a prompt works for as long as the
    // authorizer keeps it, whether or not this object is still alive.
    AuthorizationPrompt plain_prompt() const;
    AuthorizationPrompt coroutine_prompt() const;

    // Why the latest prompt did not wait for the browser, or gave up (the port could not be bound, the browser could
    // not be opened, the redirect URI of the sign-in is not this one's, another sign-in was waiting); empty when it
    // waited. Never holds a callback or an authorization URL.
    std::string last_problem() const;

    struct Impl;

private:
    explicit LoopbackRedirect(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace neomcp
