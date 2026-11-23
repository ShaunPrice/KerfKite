// Python's ipaddress rules. See ip_address.hpp.
#include "ip_address.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kerfkite {
namespace server {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool hex_digit(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool parse_ipv4(const std::string &text, unsigned char out[4]) {
    size_t at = 0;
    for (int part = 0; part < 4; ++part) {
        if (part && (at >= text.size() || text[at++] != '.')) return false;
        const size_t start = at;
        while (at < text.size() && digit(text[at])) ++at;
        if (at == start || at - start > 3 || (at - start > 1 && text[start] == '0')) return false;
        const int value = std::stoi(text.substr(start, at - start));
        if (value > 255) return false;
        out[part] = static_cast<unsigned char>(value);
    }
    return at == text.size();
}

bool parse_ipv6(std::string text, unsigned char out[16]) {
    const size_t percent = text.find('%');
    if (percent != std::string::npos) {
        if (percent + 1 >= text.size() || text.find('%', percent + 1) != std::string::npos) return false;
        text.resize(percent);
    }
    if (text.empty() || text.size() > 45) return false;
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
        const size_t colon = text.find(':', at);
        parts.push_back(text.substr(at, colon == std::string::npos ? std::string::npos : colon - at));
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    if (parts.size() < 3) return false;
    if (parts.back().find('.') != std::string::npos) {
        unsigned char v4[4];
        if (!parse_ipv4(parts.back(), v4)) return false;
        parts.pop_back();
        char buffer[8];
        std::snprintf(buffer, sizeof buffer, "%x", static_cast<unsigned>(v4[0] << 8 | v4[1]));
        parts.push_back(buffer);
        std::snprintf(buffer, sizeof buffer, "%x", static_cast<unsigned>(v4[2] << 8 | v4[3]));
        parts.push_back(buffer);
    }
    if (parts.size() > 9) return false;
    size_t skip = 0;
    bool skipped = false;
    for (size_t i = 1; i + 1 < parts.size(); ++i)
        if (parts[i].empty()) {
            if (skipped) return false;
            skipped = true;
            skip = i;
        }
    size_t high = parts.size(), low = 0;
    if (skipped) {
        high = skip;
        low = parts.size() - skip - 1;
        if (parts.front().empty() && --high) return false;
        if (parts.back().empty() && --low) return false;
        if (high + low > 7) return false;
    } else if (parts.size() != 8 || parts.front().empty() || parts.back().empty()) {
        return false;
    }
    std::vector<unsigned> groups;
    const auto hextet = [&](const std::string &part) {
        if (part.empty() || part.size() > 4 || !std::all_of(part.begin(), part.end(), hex_digit)) return false;
        groups.push_back(static_cast<unsigned>(std::stoul(part, nullptr, 16)));
        return true;
    };
    for (size_t i = 0; i < high; ++i)
        if (!hextet(parts[i])) return false;
    groups.resize(8 - low, 0);
    for (size_t i = parts.size() - low; i < parts.size(); ++i)
        if (!hextet(parts[i])) return false;
    for (size_t i = 0; i < 8; ++i) {
        out[2 * i] = static_cast<unsigned char>(groups[i] >> 8);
        out[2 * i + 1] = static_cast<unsigned char>(groups[i] & 0xff);
    }
    return true;
}

// Whether the address lies in the network written as text, e.g. "10.0.0.0/8" or "2001:db8::/32".
bool within(const IpAddress &ip, const char *network) {
    const char *slash = std::strchr(network, '/');
    IpAddress base;
    if (!parse_ip_address(std::string(network, slash), base) || base.v6 != ip.v6) return false;
    const int bits = std::atoi(slash + 1);
    for (int i = 0; i < bits; ++i) {
        const int mask = 0x80 >> (i % 8);
        if ((ip.bytes[i / 8] & mask) != (base.bytes[i / 8] & mask)) return false;
    }
    return true;
}

bool within_any(const IpAddress &ip, std::initializer_list<const char *> networks) {
    return std::any_of(networks.begin(), networks.end(), [&](const char *network) { return within(ip, network); });
}

// IPv6Address.ipv4_mapped.
bool mapped(const IpAddress &ip, IpAddress &v4) {
    if (!ip.v6) return false;
    for (int i = 0; i < 10; ++i)
        if (ip.bytes[i]) return false;
    if (ip.bytes[10] != 0xff || ip.bytes[11] != 0xff) return false;
    v4 = IpAddress();
    std::memcpy(v4.bytes, ip.bytes + 12, 4);
    return true;
}

IpAddress ipv4_of(const unsigned char *bytes, std::initializer_list<int> offsets) {
    IpAddress v4;
    int k = 0;
    for (const int offset : offsets) v4.bytes[k++] = bytes[offset];
    return v4;
}

bool v4_allowed(const IpAddress &v4) { return ip_is_global(v4) && !ip_is_multicast(v4); }

}  // namespace

bool parse_ip_address(const std::string &text, IpAddress &out) {
    out = IpAddress();
    if (text.find('/') != std::string::npos) return false;
    if (parse_ipv4(text, out.bytes)) return true;
    if (!parse_ipv6(text, out.bytes)) return false;
    out.v6 = true;
    return true;
}

bool ip_is_loopback(const IpAddress &ip) {
    IpAddress v4;
    if (mapped(ip, v4)) return ip_is_loopback(v4);
    if (!ip.v6) return ip.bytes[0] == 127;
    static const unsigned char one[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return std::memcmp(ip.bytes, one, 16) == 0;
}

bool ip_is_unspecified(const IpAddress &ip) {
    IpAddress v4;
    if (mapped(ip, v4)) return ip_is_unspecified(v4);
    const int size = ip.v6 ? 16 : 4;
    return std::all_of(ip.bytes, ip.bytes + size, [](unsigned char b) { return b == 0; });
}

bool ip_is_multicast(const IpAddress &ip) {
    IpAddress v4;
    if (mapped(ip, v4)) return ip_is_multicast(v4);
    return ip.v6 ? ip.bytes[0] == 0xff : within(ip, "224.0.0.0/4");
}

bool ip_is_private(const IpAddress &ip) {
    IpAddress v4;
    if (mapped(ip, v4)) return ip_is_private(v4);
    if (!ip.v6)
        return within_any(ip, {"0.0.0.0/8", "10.0.0.0/8", "127.0.0.0/8", "169.254.0.0/16", "172.16.0.0/12", "192.0.0.0/24", "192.0.0.170/31",
                               "192.0.2.0/24", "192.168.0.0/16", "198.18.0.0/15", "198.51.100.0/24", "203.0.113.0/24", "240.0.0.0/4",
                               "255.255.255.255/32"}) &&
               !within_any(ip, {"192.0.0.9/32", "192.0.0.10/32"});
    return within_any(ip, {"::1/128", "::/128", "::ffff:0.0.0.0/96", "64:ff9b:1::/48", "100::/64", "2001::/23", "2001:db8::/32", "2002::/16",
                           "3fff::/20", "fc00::/7", "fe80::/10"}) &&
           !within_any(ip, {"2001:1::1/128", "2001:1::2/128", "2001:3::/32", "2001:4:112::/48", "2001:20::/28", "2001:30::/28"});
}

bool ip_is_global(const IpAddress &ip) {
    IpAddress v4;
    if (mapped(ip, v4)) return ip_is_global(v4);
    if (!ip.v6) return !within(ip, "100.64.0.0/10") && !ip_is_private(ip);
    return !ip_is_private(ip);
}

bool ip_allowed(const std::string &text) {
    IpAddress ip;
    if (!parse_ip_address(text, ip)) return false;
    if (ip.v6) {
        const unsigned char *b = ip.bytes;
        std::vector<IpAddress> embedded;
        IpAddress v4;
        if (mapped(ip, v4)) embedded.push_back(v4);
        if (b[0] == 0x20 && b[1] == 0x02) embedded.push_back(ipv4_of(b, {2, 3, 4, 5}));  // 6to4
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0) {                      // Teredo: server, then client
            embedded.push_back(ipv4_of(b, {4, 5, 6, 7}));
            IpAddress client = ipv4_of(b, {12, 13, 14, 15});
            for (int i = 0; i < 4; ++i) client.bytes[i] = static_cast<unsigned char>(~client.bytes[i]);
            embedded.push_back(client);
        }
        if (within(ip, "::ffff:0:0:0/96")) embedded.push_back(ipv4_of(b, {12, 13, 14, 15}));  // IPv4-translated
        if (within(ip, "64:ff9b::/96")) embedded.push_back(ipv4_of(b, {12, 13, 14, 15}));
        if (within(ip, "64:ff9b:1::/48"))
            for (const auto &offsets : {std::initializer_list<int>{6, 7, 9, 10}, {7, 9, 10, 11}, {9, 10, 11, 12}, {12, 13, 14, 15}})
                embedded.push_back(ipv4_of(b, offsets));
        if (std::all_of(b, b + 12, [](unsigned char c) { return c == 0; }) && !ip_is_loopback(ip) && !ip_is_unspecified(ip))
            embedded.push_back(ipv4_of(b, {12, 13, 14, 15}));  // IPv4-compatible
        static const unsigned char isatap[2][4] = {{0, 0, 0x5e, 0xfe}, {2, 0, 0x5e, 0xfe}};
        if (std::memcmp(b + 8, isatap[0], 4) == 0 || std::memcmp(b + 8, isatap[1], 4) == 0) embedded.push_back(ipv4_of(b, {12, 13, 14, 15}));
        for (const IpAddress &e : embedded)
            if (!v4_allowed(e)) return false;
    }
    return ip_is_global(ip) && !ip_is_multicast(ip);
}

}  // namespace server
}  // namespace kerfkite
