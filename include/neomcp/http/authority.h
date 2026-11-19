// A host and an optional port as the authority part of a URI writes them, and the parser of that form.
//
// The parser reads the authority form (host, optional port) by hand. It accepts only what the grammar allows and
// never guesses: userinfo, a path, a query, an empty host or a port that is not a number from 1 to 65535 make a value
// invalid. It depends on nothing of the server, so a client of HTTP uses it as a server does. neomcp/http/policy.h
// includes this header, so a program that includes that one has these names as before.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace neomcp::http {

// A host and an optional port.
struct Authority {
    // Lower case, without brackets and without a trailing dot. An IPv6 address is in its canonical text form.
    std::string host;
    bool ipv6 = false;
    std::optional<std::uint16_t> port;

    // The text form: IPv6 hosts in brackets, then ":port" when there is a port.
    std::string to_string() const;

    friend bool operator==(const Authority&, const Authority&) = default;
};

// Reads "host", "host:port", "[v6]" or "[v6]:port".
//   - A host name has letters, digits, '-', '_', '~' and '.', no empty label, and may end in one '.' (a fully
//     qualified name), which is dropped. An IPv4 address is read as a name.
//   - An IPv6 address must be in brackets and must be valid; a zone identifier is not accepted.
//   - A port is one to five digits, 1 to 65535; leading zeros are read as a number ("0080" is 80).
// Returns nothing for anything else, including "", "host:", "host:0", "host:65536", "user@host", "host/path" and an
// IPv6 address without brackets.
std::optional<Authority> parse_authority(std::string_view text);

}  // namespace neomcp::http
