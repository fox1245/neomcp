// Which Host and Origin values a server accepts.
//
// A server that listens on the loopback interface is still reachable from a web page in the user's browser, and a
// page can make a name it controls resolve to the loopback address (DNS rebinding). Checking the Host header (the
// name the client used) and the Origin header (the page that made the request, sent by browsers) closes that way in.
// A request without an Origin header comes from a program, not from a page, and passes the Origin check.
//
// The parsers here read the authority form (host, optional port) and the Origin form (scheme, "://", authority) by
// hand. They accept only what the grammar allows and never guess: userinfo, a path, a query, an empty host or a port
// that is not a number from 1 to 65535 make a value invalid. The authority and its parser are declared in
// neomcp/http/authority.h, which this header includes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "neomcp/http/authority.h"
#include "neomcp/http/message.h"
#include "neomcp/http/outcome.h"

namespace neomcp::http {

struct Origin {
    std::string scheme;  // lower case
    Authority authority;

    friend bool operator==(const Origin&, const Origin&) = default;
};

// Reads "scheme://authority" (no path, not even "/"). The literal "null", which a browser sends for an opaque origin,
// is not an origin and gives nothing, like every other invalid text.
std::optional<Origin> parse_origin(std::string_view text);

// True when `address` is a loopback address, decided by reading it as an IP address (IPv4 127.0.0.0/8, IPv6 ::1 and
// an IPv4-mapped IPv6 address of a loopback IPv4 address), or when it is the name "localhost". Any other name is not
// loopback: it is not resolved. The empty string is not loopback.
bool is_loopback_address(std::string_view address);

enum class HostCheck {
    on,   // the Host header must be present and allowed
    off,  // the Host header is not checked (Origin still is)
};

struct PolicyOptions {
    // The address the server binds to, as given to the listener.
    std::string bind_address = "127.0.0.1";

    // The port the server is bound to (after a bind to port 0, the port that was chosen).
    std::uint16_t bound_port = 0;

    // The hosts a client may use, each "host", "host:port", "[v6]" or "[v6]:port". An entry without a port allows
    // that host with any port or none; an entry with a port allows exactly that port. When empty and the bind
    // address is loopback, the defaults apply: "localhost", "127.0.0.1" and "[::1]", each with the bound port or
    // with no port.
    std::vector<std::string> allowed_hosts;

    // Origins accepted besides those whose host is allowed, each "scheme://authority", compared as written
    // (lower-cased) including the port.
    std::vector<std::string> allowed_origins;

    HostCheck host_check = HostCheck::on;
};

// The answer of a check: whether the request passes, and, when it does not, a reason fit for a log.
struct PolicyVerdict {
    bool allowed = false;
    std::string reason;
};

// Built once from the options; immutable afterwards, so it can be shared between threads.
class HostPolicy {
public:
    // Fails, with a message that says what to do, when:
    //   - the bind address is not loopback, `allowed_hosts` is empty and the Host check is on (a server reachable from
    //     other machines does not guess its own names: name them, or choose HostCheck::off);
    //   - an entry of `allowed_hosts` or `allowed_origins` is not valid.
    static Outcome<HostPolicy> create(const PolicyOptions& options);

    // Whether `host_header` (the value of a Host field) is allowed. Always true when the Host check is off.
    bool host_allowed(std::string_view host_header) const;

    // Whether `origin_header` (the value of an Origin field) is allowed: it must be a valid origin, not "null",
    // and either listed in `allowed_origins` or have an allowed host.
    bool origin_allowed(std::string_view origin_header) const;

    // Checks the fields of a request: a Host field that is present once (unless the check is off) and allowed, and,
    // when there is an Origin field, exactly one that is allowed.
    PolicyVerdict check(const Headers& headers) const;

private:
    enum class PortRule { any, exactly };
    struct HostEntry {
        Authority authority;  // its port is used when `rule` is `exactly`; a missing port then means "no port"
        PortRule rule = PortRule::any;
    };

    bool authority_allowed(const Authority& authority) const;

    std::vector<HostEntry> hosts_;
    std::vector<Origin> origins_;
    HostCheck host_check_ = HostCheck::on;
};

}  // namespace neomcp::http
