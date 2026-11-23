// Addresses as Python 3.13's ipaddress module reads and classifies them, for the checks the assistant server's
// Python reference made of configured addresses and of the addresses a name resolves to.
#ifndef KERFKITE_SERVER_IP_ADDRESS_HPP
#define KERFKITE_SERVER_IP_ADDRESS_HPP

#include <string>

namespace kerfkite {
namespace server {

struct IpAddress {
    bool v6 = false;
    unsigned char bytes[16] = {};  // an IPv4 address in the first four
};

// ipaddress.ip_address(text): four decimal octets of up to three digits without leading zeros, or an IPv6
// address (_ip_int_from_string, an IPv4 address in place of its last two groups if wanted, a scope after '%');
// false where it raises ValueError.
bool parse_ip_address(const std::string &text, IpAddress &out);

// The properties of the module's classes. An IPv4-mapped IPv6 address has its IPv4 address's, as in Python 3.13.
bool ip_is_loopback(const IpAddress &ip);
bool ip_is_unspecified(const IpAddress &ip);
bool ip_is_multicast(const IpAddress &ip);
bool ip_is_private(const IpAddress &ip);
bool ip_is_global(const IpAddress &ip);

// fastmcp.server.auth.ssrf.is_ip_allowed: a globally reachable address that is not multicast, and, for IPv6, no
// IPv4 address embedded in a transition form (mapped, 6to4, Teredo, translated, NAT64, compatible, ISATAP) that is
// not allowed itself.
bool ip_allowed(const std::string &text);

}  // namespace server
}  // namespace kerfkite

#endif
