// Single-operator OAuth for the assistant server's HTTP transport. See oauth.hpp.
#include "oauth.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "ip_address.hpp"
#include "os_crypto.hpp"
#include "python_text.hpp"
#include "web_transfer.hpp"

namespace kerfkite {
namespace server {

namespace oauth_detail {

bool lenient_base64url(const std::string &text, std::string &out) {
    // _bytes_from_decode_data: a str must be ASCII.
    for (const char c : text)
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    out.clear();
    int quad = 0, pads = 0;
    unsigned left = 0;
    for (const char given : text) {
        const char c = given == '-' ? '+' : given == '_' ? '/' : given;
        // A pad is counted and passed over: what follows it is still read (CPython 3.13).
        if (c == '=') {
            ++pads;
            continue;
        }
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        if (value < 0) continue;  // not base64: passed over
        pads = 0;
        switch (quad) {
            case 0:
                quad = 1;
                left = static_cast<unsigned>(value);
                break;
            case 1:
                quad = 2;
                out += static_cast<char>((left << 2) | (static_cast<unsigned>(value) >> 4));
                left = static_cast<unsigned>(value) & 0x0f;
                break;
            case 2:
                quad = 3;
                out += static_cast<char>((left << 4) | (static_cast<unsigned>(value) >> 2));
                left = static_cast<unsigned>(value) & 0x03;
                break;
            default:
                quad = 0;
                out += static_cast<char>((left << 6) | static_cast<unsigned>(value));
                left = 0;
        }
    }
    // A last quad of two or three characters needs the pads that complete it after its last character; one of a
    // single character is "1 more than a multiple of 4".
    return quad == 0 || (quad >= 2 && quad + pads >= 4);
}

bool strict_base64url(const std::string &text, std::string &out) {
    const size_t pad = (4 - text.size() % 4) % 4;
    if (pad == 3) return false;
    // The last character carries no bits past the data: a multiple of 4 (one pad) or of 16 (two).
    if (pad == 1 && std::strchr("AEIMQUYcgkosw048", text.back()) == nullptr) return false;
    if (pad == 2 && std::strchr("AQgw", text.back()) == nullptr) return false;
    out.clear();
    unsigned bits = 0;
    int count = 0;
    for (const char c : text) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '-') value = 62;
        else if (c == '_') value = 63;
        if (value < 0) return false;
        bits = (bits << 6) | static_cast<unsigned>(value);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xff);
        }
    }
    return true;
}

namespace {

// The code points of UTF-16 or UTF-32 text, big- or little-endian, as Python's codecs with 'surrogatepass' read
// them, except that (own rule) a surrogate without its pair is refused.
bool wide(const std::string &bytes, size_t start, int width, bool big, std::vector<uint32_t> &out, bool surrogates = false) {
    const auto unit = [&](size_t at) {
        uint32_t v = 0;
        for (int k = 0; k < width; ++k) {
            const uint32_t b = static_cast<unsigned char>(bytes[at + static_cast<size_t>(k)]);
            v |= big ? b << (8 * (width - 1 - k)) : b << (8 * k);
        }
        return v;
    };
    if ((bytes.size() - start) % static_cast<size_t>(width)) return false;
    for (size_t at = start; at < bytes.size(); at += static_cast<size_t>(width)) {
        uint32_t c = unit(at);
        if (width == 2 && c >= 0xd800 && c <= 0xdbff && at + 2 < bytes.size() && unit(at + 2) >= 0xdc00 && unit(at + 2) <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (unit(at + 2) - 0xdc00);
            at += 2;
        } else if (c >= 0xd800 && c <= 0xdfff && !surrogates) {
            return false;
        }
        if (c > 0x10ffff) return false;
        out.push_back(c);
    }
    return true;
}

}  // namespace

namespace {

// The code points of UTF-8 with 'surrogatepass': as strict UTF-8, but the encoded surrogates ED A0 80 to ED BF BF
// are read too.
bool utf8_with_surrogates(const unsigned char *data, size_t size, std::vector<uint32_t> &out) {
    for (size_t i = 0; i < size;) {
        if (i + 2 < size && data[i] == 0xed && data[i + 1] >= 0xa0 && data[i + 1] <= 0xbf && (data[i + 2] & 0xc0) == 0x80) {
            out.push_back(0xd000 | (static_cast<uint32_t>(data[i + 1] & 0x3f) << 6) | (data[i + 2] & 0x3f));
            i += 3;
            continue;
        }
        size_t j = i + 1;
        while (j < size && (data[j] & 0xc0) == 0x80 && j - i < 4) ++j;
        std::vector<uint32_t> piece;
        if (!detail::python_utf8(data + i, j - i, piece)) return false;
        out.insert(out.end(), piece.begin(), piece.end());
        i = j;
    }
    return true;
}

// json.detect_encoding and the decoding json.loads makes of bytes.
bool decode_json_bytes(const std::string &bytes, std::vector<uint32_t> &points, bool surrogates) {
    const auto at = [&](size_t k) { return static_cast<unsigned char>(bytes[k]); };
    const auto starts = [&](const char *prefix, size_t size) { return bytes.size() >= size && std::memcmp(bytes.data(), prefix, size) == 0; };
    const unsigned char *data = reinterpret_cast<const unsigned char *>(bytes.data());
    const auto utf8 = [&](size_t from) {
        return surrogates ? utf8_with_surrogates(data + from, bytes.size() - from, points) : detail::python_utf8(data + from, bytes.size() - from, points);
    };
    if (starts("\x00\x00\xfe\xff", 4)) return wide(bytes, 4, 4, true, points, surrogates);
    if (starts("\xff\xfe\x00\x00", 4)) return wide(bytes, 4, 4, false, points, surrogates);
    if (starts("\xfe\xff", 2)) return wide(bytes, 2, 2, true, points, surrogates);
    if (starts("\xff\xfe", 2)) return wide(bytes, 2, 2, false, points, surrogates);
    if (starts("\xef\xbb\xbf", 3)) return utf8(3);
    if (bytes.size() >= 4 && !at(0)) return wide(bytes, 0, at(1) ? 2 : 4, true, points, surrogates);
    if (bytes.size() >= 4 && !at(1)) return wide(bytes, 0, at(2) || at(3) ? 2 : 4, false, points, surrogates);
    if (bytes.size() == 2 && !at(0)) return wide(bytes, 0, 2, true, points, surrogates);
    if (bytes.size() == 2 && !at(1)) return wide(bytes, 0, 2, false, points, surrogates);
    return utf8(0);
}

}  // namespace

bool decodes(const std::string &bytes) {
    std::vector<uint32_t> points;
    return decode_json_bytes(bytes, points, true);
}

bool json_from_bytes(const std::string &bytes, Json &out) {
    std::vector<uint32_t> points;
    if (!decode_json_bytes(bytes, points, false)) return false;
    std::string text;
    for (const uint32_t c : points) {
        if (c >= 0xd800 && c <= 0xdfff) return false;  // own rule: 'surrogatepass' kept these
        detail::append_utf8(text, c);
    }
    out = Json::parse(text, nullptr, false);
    return !out.is_discarded();
}

namespace {

// ---- arithmetic for the curve check: unsigned numbers in 32-bit limbs, least significant first --------------

using Limbs = std::vector<uint32_t>;

void trim(Limbs &a) {
    while (!a.empty() && a.back() == 0) a.pop_back();
}

Limbs from_bytes(const std::string &bytes) {
    Limbs out((bytes.size() + 3) / 4, 0);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const size_t bit = 8 * (bytes.size() - 1 - i);
        out[bit / 32] |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (bit % 32);
    }
    trim(out);
    return out;
}

Limbs from_hex(const char *hex) {
    std::string digits = hex, bytes;
    if (digits.size() % 2) digits.insert(digits.begin(), '0');
    for (size_t i = 0; i < digits.size(); i += 2) bytes += static_cast<char>(std::strtoul(digits.substr(i, 2).c_str(), nullptr, 16));
    return from_bytes(bytes);
}

int compare(const Limbs &a, const Limbs &b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

void subtract(Limbs &a, const Limbs &b) {  // a >= b
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int64_t v = static_cast<int64_t>(a[i]) - borrow - (i < b.size() ? static_cast<int64_t>(b[i]) : 0);
        borrow = v < 0;
        if (v < 0) v += int64_t(1) << 32;
        a[i] = static_cast<uint32_t>(v);
    }
    trim(a);
}

Limbs add(const Limbs &a, const Limbs &b) {
    Limbs out(std::max(a.size(), b.size()) + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const uint64_t v = carry + (i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        out[i] = static_cast<uint32_t>(v);
        carry = v >> 32;
    }
    trim(out);
    return out;
}

Limbs multiply(const Limbs &a, const Limbs &b) {
    Limbs out(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            const uint64_t v = static_cast<uint64_t>(a[i]) * b[j] + out[i + j] + carry;
            out[i + j] = static_cast<uint32_t>(v);
            carry = v >> 32;
        }
        out[i + b.size()] = static_cast<uint32_t>(carry);
    }
    trim(out);
    return out;
}

// a mod p, a bit at a time.
Limbs remainder(const Limbs &a, const Limbs &p) {
    Limbs r;
    for (size_t i = a.size() * 32; i-- > 0;) {
        // r = 2r + bit
        uint32_t carry = (a[i / 32] >> (i % 32)) & 1;
        for (uint32_t &limb : r) {
            const uint32_t next = limb >> 31;
            limb = (limb << 1) | carry;
            carry = next;
        }
        if (carry) r.push_back(carry);
        if (compare(r, p) >= 0) subtract(r, p);
    }
    return r;
}

struct Curve {
    const char *name, *p, *a, *b;
};

// The curves joserfc registers, by their parameters (FIPS 186-4 D.1.2; SEC 2 2.4.1 for secp256k1).
const Curve kCurves[] = {
    {"P-256", "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", "ffffffff00000001000000000000000000000000fffffffffffffffffffffffc",
     "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"},
    {"P-384", "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
     "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000fffffffc",
     "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef"},
    {"P-521",
     "1ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "1fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc",
     "51953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00"},
    {"secp256k1", "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f", "00", "07"},
};

}  // namespace

bool on_curve(const std::string &curve, const std::string &x_bytes, const std::string &y_bytes) {
    for (const Curve &c : kCurves) {
        if (curve != c.name) continue;
        const Limbs p = from_hex(c.p), a = from_hex(c.a), b = from_hex(c.b);
        // OpenSSL takes the coordinates modulo p, then asks that y^2 = x^3 + ax + b.
        const Limbs x = remainder(from_bytes(x_bytes), p), y = remainder(from_bytes(y_bytes), p);
        const Limbs left = remainder(multiply(y, y), p);
        const Limbs x3 = remainder(multiply(remainder(multiply(x, x), p), x), p);
        const Limbs right = remainder(add(add(x3, remainder(multiply(a, x), p)), b), p);
        return compare(left, right) == 0;
    }
    return false;
}

}  // namespace oauth_detail

namespace {

using oauth_detail::json_from_bytes;
using oauth_detail::lenient_base64url;
using oauth_detail::strict_base64url;

// ---- configuration -----------------------------------------------------------------------------------------

// A variable of the environment as UTF-8, empty when it is not set (os.environ.get(name, '')).
std::string variable(const std::string &name) {
#ifdef _WIN32
    const std::wstring wide(name.begin(), name.end());
    const wchar_t *value = _wgetenv(wide.c_str());
    if (!value) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return std::string();
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &out[0], size, nullptr, nullptr);
    return out;
#else
    const char *value = std::getenv(name.c_str());
    return value ? std::string(value) : std::string();
#endif
}

// _setting(suffix): nonempty, without white space about it, printable and at most 2048 characters. On POSIX a value
// that is not UTF-8 reaches Python with surrogates, which are not printable: the same refusal.
std::string setting(const std::string &suffix) {
    const std::string name = "KERFKITE_MCP_" + suffix;
    const std::string value = variable(name);
    std::vector<uint32_t> points;
    bool ok = !value.empty() && detail::python_utf8(reinterpret_cast<const unsigned char *>(value.data()), value.size(), points) &&
              points.size() <= 2048 && !detail::python_space(points.front()) && !detail::python_space(points.back());
    for (size_t i = 0; ok && i < points.size(); ++i) ok = detail::python_printable(points[i]);
    if (!ok) throw std::runtime_error("Set " + name + " to a nonempty value for remote OAuth.");
    return value;
}

// An https URL as AnyHttpUrl reads it, where the text is already in the form AnyHttpUrl writes, give or take the
// case of the scheme and host, a default or zero-led port and an empty port or credentials.
struct Url {
    std::string host;      // as AnyHttpUrl writes it: a lower-case domain, dotted IPv4 or "[IPv6]"
    std::string port;      // decimal, empty for none or 443
    std::string path;      // as written, "" kept
    std::string public_host;  // url.host.strip("[]").lower().rstrip("."), which the public-address check reads
};

std::string lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool digits(const std::string &text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The WHATWG URL Standard's serialization of an IPv6 address: lower-case groups, the first longest run of two or
// more zero groups as "::".
std::string ipv6_text(const IpAddress &ip) {
    unsigned groups[8];
    for (int i = 0; i < 8; ++i) groups[i] = static_cast<unsigned>(ip.bytes[2 * i]) << 8 | ip.bytes[2 * i + 1];
    int best = -1, best_length = 1;
    for (int i = 0; i < 8;) {
        if (groups[i]) {
            ++i;
            continue;
        }
        int j = i;
        while (j < 8 && !groups[j]) ++j;
        if (j - i > best_length) best = i, best_length = j - i;
        i = j;
    }
    std::string out;
    char buffer[8];
    for (int i = 0; i < 8; ++i) {
        if (i == best) {
            out += i == 0 ? "::" : ":";
            i += best_length - 1;
            continue;
        }
        std::snprintf(buffer, sizeof buffer, "%x", groups[i]);
        out += buffer;
        if (i < 7) out += ':';
    }
    return out;
}

bool plain_https(const std::string &text, Url &url) {
    const size_t separator = text.find("://");
    if (separator == std::string::npos || lower(text.substr(0, separator)) != "https") return false;
    const std::string rest = text.substr(separator + 3);
    const size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    url.path = slash == std::string::npos ? std::string() : rest.substr(slash);
    for (const char c : text)
        if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7e) return false;  // own rule: ASCII only
    // Credentials: AnyHttpUrl takes none but empty ones.
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        const std::string credentials = authority.substr(0, at);
        if (!credentials.empty() && credentials != ":") return false;
        authority = authority.substr(at + 1);
    }
    std::string after;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        const std::string inner = authority.substr(1, close - 1);
        IpAddress ip;
        if (inner.find('%') != std::string::npos || !parse_ip_address(inner, ip) || !ip.v6) return false;
        url.public_host = ipv6_text(ip);
        url.host = "[" + url.public_host + "]";
        after = authority.substr(close + 1);
    } else {
        const size_t colon = authority.find(':');
        const std::string host = lower(authority.substr(0, colon));
        after = colon == std::string::npos ? std::string() : authority.substr(colon);
        if (host.empty()) return false;
        // Own rule: a domain of letters, digits, '-' and '_' in labels, a final '.' at most, no IDNA labels.
        std::vector<std::string> labels;
        for (size_t start = 0;;) {
            const size_t dot = host.find('.', start);
            labels.push_back(host.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        const bool final_dot = labels.size() > 1 && labels.back().empty();
        if (final_dot) labels.pop_back();
        for (const std::string &label : labels) {
            if (label.empty() || label.compare(0, 4, "xn--") == 0) return false;
            for (const char c : label)
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        }
        // A host that ends in a number is an IPv4 address to the URL parser: here only in dotted decimal.
        const std::string &last = labels.back();
        const bool number = digits(last) || (last.size() >= 2 && last[0] == '0' && last[1] == 'x' &&
                                              std::all_of(last.begin() + 2, last.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));
        if (number) {
            IpAddress ip;
            if (final_dot || !parse_ip_address(host, ip) || ip.v6) return false;
        }
        url.host = host;
        url.public_host = host;
        while (!url.public_host.empty() && url.public_host.back() == '.') url.public_host.pop_back();
    }
    if (!after.empty()) {
        if (after[0] != ':') return false;
        std::string port = after.substr(1);
        if (!port.empty()) {
            if (!digits(port)) return false;
            port.erase(0, std::min(port.find_first_not_of('0'), port.size() - 1));
            if (port.size() > 5 || std::stol(port) > 65535) return false;
            if (port != "443") url.port = port;
        }
    }
    // Own rule: a path the parser leaves as it is: no characters it would encode, no '%', no dot segments.
    if (!url.path.empty()) {
        for (const char c : url.path)
            if (std::strchr("\"<>`{}%", c)) return false;
        for (size_t start = 1;;) {
            const size_t next = url.path.find('/', start);
            const std::string segment = url.path.substr(start, next == std::string::npos ? std::string::npos : next - start);
            if (segment == "." || segment == "..") return false;
            if (next == std::string::npos) break;
            start = next + 1;
        }
    }
    return true;
}

// _https_url(suffix, origin): a public HTTPS URL (or origin) without credentials, query or fragment.
Url https_url(const std::string &suffix, bool origin, std::string &value) {
    value = setting(suffix);
    const std::string error = "KERFKITE_MCP_" + suffix + " must be a public HTTPS " + (origin ? "origin" : "URL") + ".";
    std::vector<uint32_t> points;
    detail::python_utf8(reinterpret_cast<const unsigned char *>(value.data()), value.size(), points);
    for (const uint32_t c : points)
        if (c == '\\' || c == '?' || c == '#' || detail::python_space(c)) throw std::runtime_error(error);
    Url url;
    if (!plain_https(value, url) || (origin && !(url.path.empty() || url.path == "/"))) throw std::runtime_error(error);
    IpAddress ip;
    if (parse_ip_address(url.public_host, ip)) {
        if (!ip_is_global(ip) || ip_is_multicast(ip)) throw std::runtime_error(error);
    } else {
        const std::string &h = url.public_host;
        const auto ends = [&](const char *tail) {
            const size_t n = std::strlen(tail);
            return h.size() >= n && h.compare(h.size() - n, n, tail) == 0;
        };
        if (h.find('.') == std::string::npos || ends(".local") || ends(".localhost") || ends(".internal") || ends(".lan") || ends(".home.arpa"))
            throw std::runtime_error(error);
    }
    return url;
}

}  // namespace

RemoteAuth remote_auth_from_environment() {
    RemoteAuth auth;
    std::string origin_text;
    const Url origin = https_url("PUBLIC_ORIGIN", true, origin_text);
    auth.base_url = "https://" + origin.host + (origin.port.empty() ? "" : ":" + origin.port);
    auth.host = origin.host;
    const Url issuer = https_url("ISSUER", false, auth.issuer);
    auth.issuer_url = "https://" + issuer.host + (issuer.port.empty() ? "" : ":" + issuer.port) + issuer.path;
    auth.subject = setting("SUBJECT");
    https_url("JWKS_URL", false, auth.jwks_url);
    return auth;
}

namespace {

double monotonic() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double wall() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

const Json *member(const Json &object, const std::string &name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

// Python's truth of a JSON value; absent is None.
bool truthy(const Json *v) {
    if (!v || v->is_null()) return false;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0;
    if (v->is_string()) return !v->get<std::string>().empty();
    return !v->empty();
}

bool text_is(const Json *v, const char *text) { return v && v->is_string() && v->get<std::string>() == text; }

// A number as Python compares it with a float (bool counts as an int).
double number(const Json &v) { return v.is_boolean() ? (v.get<bool>() ? 1.0 : 0.0) : v.get<double>(); }

// in_choices: a list of choices or one of them.
bool in_choices(const Json &value, std::initializer_list<const char *> choices) {
    const auto one = [&](const Json &v) {
        return v.is_string() && std::any_of(choices.begin(), choices.end(), [&](const char *c) { return v.get<std::string>() == c; });
    };
    if (value.is_array()) return std::all_of(value.begin(), value.end(), one);
    return one(value);
}

bool list_of_text(const Json &value) {
    return value.is_array() && std::all_of(value.begin(), value.end(), [](const Json &v) { return v.is_string(); });
}

bool url_text(const Json &value) {
    if (!value.is_string()) return false;
    const std::string &t = value.get<std::string>();
    return t.compare(0, 7, "http://") == 0 || t.compare(0, 8, "https://") == 0;
}

// base64_to_int: the number's bytes, without leading zeros; false for text that is not strict base64url or empty.
bool integer(const Json &value, std::string &out) {
    if (!value.is_string() || !strict_base64url(value.get<std::string>(), out) || out.empty()) return false;
    out.erase(0, std::min(out.find_first_not_of('\0'), out.size()));
    return true;
}

int compare_bytes(const std::string &a, const std::string &b) {  // numbers without leading zeros
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a.compare(b) < 0 ? -1 : a == b ? 0 : 1;
}

// Whether an object of a key set's "keys" is a key FastMCP keeps (_jwk_to_pem succeeds): joserfc's checks of its
// parameters, then cryptography's of its numbers. Own rule: a private key (with "d") is passed over.
bool usable(const Json &jwk, bool &rsa, std::string &modulus, std::string &exponent) {
    const Json *kty = member(jwk, "kty");
    if (!kty || !kty->is_string()) return false;
    rsa = kty->get<std::string>() == "RSA";
    if (!rsa && kty->get<std::string>() != "EC") return false;
    // JWK_PARAMETER_REGISTRY
    const Json *use = member(jwk, "use"), *ops = member(jwk, "key_ops");
    if (use && !in_choices(*use, {"sig", "enc"})) return false;
    if (ops && !in_choices(*ops, {"sign", "verify", "encrypt", "decrypt", "wrapKey", "unwrapKey", "deriveKey", "deriveBits"})) return false;
    for (const char *name : {"alg", "kid", "x5t", "x5t#S256"})
        if (const Json *v = member(jwk, name))
            if (!v->is_string()) return false;
    if (const Json *v = member(jwk, "x5u"))
        if (!url_text(*v)) return false;
    if (const Json *v = member(jwk, "x5c"))
        if (!list_of_text(*v)) return false;
    // The key type's registry
    const std::vector<const char *> required = rsa ? std::vector<const char *>{"n", "e"} : std::vector<const char *>{"crv", "x", "y"};
    for (const char *name : required)
        if (!member(jwk, name) || !member(jwk, name)->is_string()) return false;
    const std::vector<const char *> optional = rsa ? std::vector<const char *>{"d", "p", "q", "dp", "dq", "qi"} : std::vector<const char *>{"d"};
    for (const char *name : optional)
        if (const Json *v = member(jwk, name))
            if (!v->is_string()) return false;
    if (rsa && member(jwk, "oth")) return false;
    // "use" with "key_ops": each operation one of the use's
    if (use && ops) {
        if (!use->is_string()) return false;  // a list is no key of the registry: TypeError
        const bool sig = use->get<std::string>() == "sig";
        const auto fits = [&](const Json &op) {
            if (!op.is_string()) return false;
            const std::string &o = op.get<std::string>();
            return sig ? (o == "sign" || o == "verify") : (o != "sign" && o != "verify");
        };
        if (!ops->is_array()) return false;  // a str is iterated by its characters, none of them an operation
        if (!std::all_of(ops->begin(), ops->end(), fits)) return false;
    }
    if (member(jwk, "d")) return false;  // own rule
    if (rsa) {
        // RSAPublicNumbers: 3 <= e < n, e odd
        if (!integer(*member(jwk, "n"), modulus) || !integer(*member(jwk, "e"), exponent)) return false;
        return !exponent.empty() && compare_bytes(exponent, std::string(1, '\x03')) >= 0 && compare_bytes(exponent, modulus) < 0 &&
               (static_cast<unsigned char>(exponent.back()) & 1);
    }
    std::string x, y;
    const std::string crv = member(jwk, "crv")->get<std::string>();
    if (crv != "P-256" && crv != "P-384" && crv != "P-521" && crv != "secp256k1") return false;
    return integer(*member(jwk, "x"), x) && integer(*member(jwk, "y"), y) && oauth_detail::on_curve(crv, x, y);
}

}  // namespace

std::string oauth_detail::key_kind(const Json &jwk) {
    bool rsa = false;
    std::string n, e;
    if (!jwk.is_object() || !usable(jwk, rsa, n, e)) return std::string();
    return rsa ? "RSA" : "EC";
}

namespace {

bool supported_header(const std::string &name) {
    for (const char *known : {"alg", "jku", "jwk", "kid", "x5u", "x5c", "x5t", "x5t#S256", "typ", "cty", "crit", "b64"})
        if (name == known) return true;
    return false;
}

std::vector<std::string> split(const std::string &text, char by) {
    std::vector<std::string> out;
    for (size_t start = 0;;) {
        const size_t at = text.find(by, start);
        out.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

// The relay's body (standard base64).
bool base64_body(const std::string &text, std::string &out) {
    out.clear();
    unsigned bits = 0;
    int count = 0;
    for (const char c : text) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        else if (c == '=') break;
        else return false;
        bits = (bits << 6) | static_cast<unsigned>(value);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xff);
        }
    }
    return true;
}

}  // namespace

TokenVerifier::TokenVerifier(RemoteAuth auth, std::string relay) : auth_(std::move(auth)), relay_(std::move(relay)) {}

TokenVerifier::Look TokenVerifier::lookup(const Json *kid, Key &key) const {
    if (truthy(kid)) {
        if (kid->is_array() || kid->is_object()) return Look::refused;  // unhashable: TypeError
        if (!kid->is_string()) return Look::missing;                    // the cache's names are all text
        const auto found = cache_.find(kid->get<std::string>());
        if (found == cache_.end()) return Look::missing;
        key = found->second;
        return Look::found;
    }
    if (cache_.size() != 1) return Look::missing;
    key = cache_.begin()->second;
    return Look::found;
}

bool TokenVerifier::key_for(const Json *kid, Key &key) {
    {
        std::lock_guard<std::mutex> hold(data_);
        if (monotonic() - cache_time_ < 3600) {
            const Look look = lookup(kid, key);
            if (look != Look::missing) return look == Look::found;
        }
    }
    std::lock_guard<std::mutex> refreshing(refreshing_);
    {
        std::lock_guard<std::mutex> hold(data_);
        const double now = monotonic();
        if (now - cache_time_ < 3600) {
            const Look look = lookup(kid, key);
            if (look != Look::missing) return look == Look::found;
        }
        if (attempted_ && now - last_attempt_ < 30) return false;  // "refresh available shortly"
        attempted_ = true;
        last_attempt_ = now;
    }
    if (!refresh()) return false;
    std::lock_guard<std::mutex> hold(data_);
    return lookup(kid, key) == Look::found;
}

bool TokenVerifier::refresh() {
    std::string body;
    Json set;
    if (!fetch(body) || !json_from_bytes(body, set) || !set.is_object()) return false;
    std::map<std::string, Key> keys;
    const Json *listed = member(set, "keys");
    if (listed && listed->is_array()) {
        for (const Json &jwk : *listed) {
            if (!jwk.is_object()) continue;
            const Json *kid = member(jwk, "kid");
            Key key;
            if (!usable(jwk, key.rsa, key.modulus, key.exponent)) {
                if (truthy(kid) && (kid->is_array() || kid->is_object())) return false;  // skipped_kids.add: TypeError
                continue;
            }
            keys[truthy(kid) ? kid->get<std::string>() : std::string("_default")] = std::move(key);
        }
    } else if (listed && !listed->is_object() && !listed->is_string()) {
        return false;  // not iterable: TypeError
    }
    std::lock_guard<std::mutex> hold(data_);
    cache_ = std::move(keys);
    cache_time_ = monotonic();
    return true;
}

bool TokenVerifier::fetch(std::string &body) const {
    const double start = monotonic();
    // urlparse(jwks_uri): hostname, port (0 and none are 443), path.
    std::string scheme_rest = auth_.jwks_url.substr(auth_.jwks_url.find("://") + 3);
    const size_t slash = scheme_rest.find('/');
    std::string authority = scheme_rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? std::string() : scheme_rest.substr(slash);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    std::string host, port_text;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) port_text = authority.substr(close + 2);
    } else {
        const size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port_text = authority.substr(colon + 1);
    }
    for (char &c : host)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    long port = port_text.empty() ? 0 : std::stol(port_text);
    if (port == 0) port = 443;
    // resolve_hostname: every address must be public (is_ip_allowed).
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) return false;
    std::vector<std::string> addresses;
    for (addrinfo *a = found; a; a = a->ai_next) {
        char text[INET6_ADDRSTRLEN] = {0};
        if (a->ai_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ai_addr)->sin_addr, text, sizeof text);
        else if (a->ai_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(a->ai_addr)->sin6_addr, text, sizeof text);
        else
            continue;
        if (std::find(addresses.begin(), addresses.end(), text) == addresses.end()) addresses.emplace_back(text);
    }
    freeaddrinfo(found);
    if (addresses.empty()) return false;
    for (const std::string &address : addresses)
        if (!ip_allowed(address)) return false;
    // Each address in turn, the request pinned to it with the name as Host and for the certificate.
    for (const std::string &address : addresses) {
        const double elapsed = monotonic() - start;
        if (elapsed > 30) return false;
        const double seconds = std::min(10.0, std::max(1.0, 30 - elapsed));
        Json request = Json::object();
        request["method"] = "GET";
        const bool v6 = address.find(':') != std::string::npos;
        request["url"] = "https://" + (v6 ? "[" + address + "]" : address) + ":" + std::to_string(port) + (path.empty() ? "/" : path);
        request["headers"] = Json::object();
        request["host"] = host;
        request["server_name"] = host;
        request["timeout_seconds"] = seconds;
        request["limit"] = 65537;
        Json answer;
        try {
            answer = relay_fetch(relay_, request, seconds + 5);
        } catch (const std::exception &) {
            continue;
        }
        const Json *problem = member(answer, "error");
        const Json *status = member(answer, "status");
        if (problem || !status || !status->is_number_integer() || truthy(member(answer, "short"))) continue;  // the connection failed
        if (status->get<long long>() != 200) return false;
        if (const Json *headers = member(answer, "headers")) {
            const Json *length = member(*headers, "Content-Length");
            if (length && length->is_array() && !length->empty() && length->at(0).is_string()) {
                const std::string &value = length->at(0).get<std::string>();
                if (digits(value) && (value.size() > 6 || std::stol(value) > 65536)) return false;
            }
        }
        const Json *data = member(answer, "body");
        if (!data) {
            body.clear();
        } else if (!data->is_string() || !base64_body(data->get<std::string>(), body)) {
            continue;
        }
        return body.size() <= 65536;
    }
    return false;
}

bool TokenVerifier::verify(const std::string &token) {
    // FastMCP's decode_jwt_header (padding added, base64 read leniently) for the key's ID.
    const std::vector<std::string> parts = split(token, '.');
    if (parts.size() != 3) return false;
    std::string header_bytes, padded = parts[0];
    padded.append((4 - padded.size() % 4) % 4, '=');
    Json header;
    if (!lenient_base64url(padded, header_bytes) || !json_from_bytes(header_bytes, header) || !header.is_object()) return false;
    Key key;
    if (!key_for(member(header, "kid"), key) || !key.rsa) return false;  // an EC key is no RSA key for RS256
    // _has_unsupported_critical_headers
    if (const Json *crit = member(header, "crit")) {
        if (!crit->is_null()) {
            if (!crit->is_array()) return false;
            for (const Json &name : *crit)
                if (!name.is_string() || !member(header, name.get<std::string>()) || !supported_header(name.get<std::string>())) return false;
        }
    }
    // joserfc's jwt.decode: the token as UTF-8, its sizes, its segments read strictly.
    std::string bytes;
    for (const char c : token) detail::append_utf8(bytes, static_cast<unsigned char>(c));
    if (bytes.size() > 512 + 128000 + 1024) return false;
    const std::vector<std::string> segments = split(bytes, '.');
    if (segments.size() != 3) return false;
    const std::string &h = segments[0], &p = segments[1], &s = segments[2];
    if (h.size() > 512 || s.size() > 1024 || p.size() > 128000) return false;
    std::string decoded;
    Json protected_header;
    if (!strict_base64url(h, decoded) || !json_from_bytes(decoded, protected_header) || !protected_header.is_object() ||
        !member(protected_header, "alg"))
        return false;
    // RFC 7797: "b64" other than true needs "b64" in "crit", and then the payload is the segment itself.
    bool raw = false;
    if (const Json *b64 = member(protected_header, "b64")) {
        if (!(b64->is_boolean() && b64->get<bool>())) {
            const Json *crit = member(protected_header, "crit");
            if (!crit || !crit->is_array() || std::none_of(crit->begin(), crit->end(), [](const Json &v) { return text_is(&v, "b64"); }))
                return false;
            raw = true;
        }
    }
    std::string payload;
    if (raw) payload = p;
    else if (!strict_base64url(p, payload)) return false;
    // JWS_HEADER_REGISTRY
    for (const char *name : {"alg", "kid", "x5t", "x5t#S256", "typ", "cty"})
        if (const Json *v = member(protected_header, name))
            if (!v->is_string()) return false;
    for (const char *name : {"jku", "x5u"})
        if (const Json *v = member(protected_header, name))
            if (!url_text(*v)) return false;
    for (const char *name : {"x5c", "crit"})
        if (const Json *v = member(protected_header, name))
            if (!list_of_text(*v)) return false;
    if (const Json *v = member(protected_header, "jwk"))
        if (!v->is_object()) return false;
    if (const Json *v = member(protected_header, "b64"))
        if (!v->is_boolean()) return false;
    if (!text_is(member(protected_header, "alg"), "RS256")) return false;
    std::string signature;
    if (!strict_base64url(s, signature) || !rsa_sha256_verify(key.modulus, key.exponent, h + "." + p, signature)) return false;
    Json claims;
    if (!json_from_bytes(payload, claims) || !claims.is_object()) return false;

    // JWTVerifier.load_access_token
    const Json *exp = member(claims, "exp");
    if (exp && !exp->is_null() && (!(exp->is_number() || exp->is_boolean()) || number(*exp) < wall())) return false;
    if (!text_is(member(claims, "iss"), auth_.issuer.c_str())) return false;
    const Json *aud = member(claims, "aud");
    const std::string audience = auth_.audience();
    if (aud && aud->is_array()) {
        if (std::none_of(aud->begin(), aud->end(), [&](const Json &v) { return text_is(&v, audience.c_str()); })) return false;
    } else if (!text_is(aud, audience.c_str())) {
        return false;
    }
    std::vector<Json> scopes;
    for (const char *name : {"scope", "scp"}) {
        const Json *v = member(claims, name);
        if (v && v->is_string()) {
            for (const std::string &word : detail::python_split(v->get<std::string>())) scopes.emplace_back(word);
            break;
        }
        if (v && v->is_array()) {
            scopes.assign(v->begin(), v->end());
            break;
        }
    }
    if (std::any_of(scopes.begin(), scopes.end(), [](const Json &v) { return v.is_array() || v.is_object(); })) return false;  // set()
    if (std::none_of(scopes.begin(), scopes.end(), [](const Json &v) { return text_is(&v, "kerfkite:workshop"); })) return false;
    if (std::any_of(scopes.begin(), scopes.end(), [](const Json &v) { return !v.is_string(); })) return false;  // AccessToken.scopes
    // _WorkshopVerifier: the operator, and exp and nbf whole or finite numbers with exp ahead and nbf past.
    if (!text_is(member(claims, "sub"), auth_.subject.c_str())) return false;
    const auto timed = [](const Json *v) { return v && v->is_number() && std::isfinite(v->get<double>()); };
    const Json *nbf = member(claims, "nbf");
    if (!timed(exp) || (nbf && !timed(nbf))) return false;
    const double now = wall();
    if (number(*exp) <= now || (nbf && number(*nbf) > now)) return false;
    // BearerAuthBackend: int(exp) against int(time.time())
    const double expires = std::trunc(number(*exp));
    return !(expires != 0 && expires < std::floor(wall()));
}

}  // namespace server
}  // namespace kerfkite
