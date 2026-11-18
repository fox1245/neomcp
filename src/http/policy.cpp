#include <asio/ip/address.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <system_error>
#include <utility>

#include "neomcp/http/policy.h"

#include "ip_text.h"

namespace neomcp::http {

namespace {

constexpr std::string_view loopback_name = "localhost";

// The names a server bound to a loopback address accepts when none were given, in the form of an allowed host entry.
constexpr std::array<std::string_view, 3> default_loopback_hosts = {"localhost", "127.0.0.1", "[::1]"};

}  // namespace

bool is_loopback_address(std::string_view address) {
    if (address.empty()) return false;
    if (equal_ignoring_case(address, loopback_name)) return true;
    if (!detail::is_ipv4_text(address) && !detail::has_ipv6_characters(address)) return false;
    std::error_code error;
    const asio::ip::address parsed = asio::ip::make_address(std::string(address), error);
    if (error) return false;
    if (parsed.is_v4()) return parsed.to_v4().is_loopback();
    const asio::ip::address_v6 v6 = parsed.to_v6();
    if (v6.is_v4_mapped()) return asio::ip::make_address_v4(asio::ip::v4_mapped, v6).is_loopback();
    return v6.is_loopback();
}

Outcome<HostPolicy> HostPolicy::create(const PolicyOptions& options) {
    HostPolicy policy;
    policy.host_check_ = options.host_check;

    for (const std::string& entry : options.allowed_hosts) {
        std::optional<Authority> authority = parse_authority(entry);
        if (!authority) {
            return Outcome<HostPolicy>::failure("allowed_hosts: \"" + entry +
                                                "\" is not a host (expected host, host:port, [v6] or [v6]:port)");
        }
        const PortRule rule = authority->port ? PortRule::exactly : PortRule::any;
        policy.hosts_.push_back({std::move(*authority), rule});
    }

    if (policy.hosts_.empty()) {
        if (is_loopback_address(options.bind_address)) {
            for (std::string_view name : default_loopback_hosts) {
                const std::optional<Authority> authority = parse_authority(name);
                // Without a port, and with the bound port.
                policy.hosts_.push_back({*authority, PortRule::exactly});
                if (options.bound_port != 0) {
                    Authority with_port = *authority;
                    with_port.port = options.bound_port;
                    policy.hosts_.push_back({std::move(with_port), PortRule::exactly});
                }
            }
        } else if (options.host_check == HostCheck::on) {
            return Outcome<HostPolicy>::failure(
                "the bind address \"" + options.bind_address +
                "\" is not a loopback address and allowed_hosts is empty: name the hosts clients use to reach this "
                "server in allowed_hosts, or switch the Host check off explicitly with HostCheck::off");
        }
    }

    for (const std::string& entry : options.allowed_origins) {
        std::optional<Origin> origin = parse_origin(entry);
        if (!origin) {
            return Outcome<HostPolicy>::failure("allowed_origins: \"" + entry +
                                                "\" is not an origin (expected scheme://host or scheme://host:port)");
        }
        policy.origins_.push_back(std::move(*origin));
    }
    return Outcome<HostPolicy>::success(std::move(policy));
}

bool HostPolicy::authority_allowed(const Authority& authority) const {
    return std::any_of(hosts_.begin(), hosts_.end(), [&](const HostEntry& entry) {
        if (entry.authority.host != authority.host || entry.authority.ipv6 != authority.ipv6) return false;
        return entry.rule == PortRule::any || entry.authority.port == authority.port;
    });
}

bool HostPolicy::host_allowed(std::string_view host_header) const {
    if (host_check_ == HostCheck::off) return true;
    const std::optional<Authority> authority = parse_authority(host_header);
    return authority && authority_allowed(*authority);
}

bool HostPolicy::origin_allowed(std::string_view origin_header) const {
    const std::optional<Origin> origin = parse_origin(origin_header);
    if (!origin) return false;
    if (std::find(origins_.begin(), origins_.end(), *origin) != origins_.end()) return true;
    return authority_allowed(origin->authority);
}

PolicyVerdict HostPolicy::check(const Headers& headers) const {
    constexpr std::string_view host_field = "Host";
    constexpr std::string_view origin_field = "Origin";

    if (host_check_ == HostCheck::on) {
        const std::size_t hosts = headers.count(host_field);
        if (hosts == 0) return {false, "the Host header is missing"};
        if (hosts > 1) return {false, "the Host header occurs more than once"};
        if (!host_allowed(*headers.get(host_field))) {
            return {false, "the Host \"" + std::string(*headers.get(host_field)) + "\" is not allowed"};
        }
    }
    const std::size_t origins = headers.count(origin_field);
    if (origins > 1) return {false, "the Origin header occurs more than once"};
    if (origins == 1 && !origin_allowed(*headers.get(origin_field))) {
        return {false, "the Origin \"" + std::string(*headers.get(origin_field)) + "\" is not allowed"};
    }
    return {true, {}};
}

}  // namespace neomcp::http
