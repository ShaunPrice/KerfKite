// The assistant server's pendant and hosted-editor tools. See remote_tools.hpp.
#include "remote_tools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "child_process.hpp"
#include "kerfkite/errors.hpp"
#include "os_crypto.hpp"
#include "os_random.hpp"
#include "studio.hpp"
#include "web_store.hpp"
#include "web_transfer.hpp"

namespace kerfkite {
namespace server {
namespace fs = std::filesystem;

namespace {

const size_t kFileLimit = 8 * 1024 * 1024;
const char *const kLoginCookie = "__Host-cnc-login";
const char *const kSessionCookie = "__Host-cnc-session";
const char *const kMonitorFailed = "The monitor connection failed. An issued action may have reached the machine. Read its status before retrying.";
const char *const kEditorFailed = "The HTTPS editor connection failed. Check stored files or receiver receipts before retrying an upload.";
const char *const kNoRelay = "These tools need KerfKite's TLS relay (kerfkite-pendant-tls), which this installation does not include";
const size_t kNone = std::string::npos;

// ---- text ------------------------------------------------------------------------------------------------------

// A variable of the environment as UTF-8, empty when it is not set (os.environ.get(...) or '').
std::string environment(const char *name) {
#ifdef _WIN32
    const std::wstring wide(name, name + std::strlen(name));
    const wchar_t *value = _wgetenv(wide.c_str());
    if (!value) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return std::string();
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &out[0], size, nullptr, nullptr);
    return out;
#else
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

// Why bytes.decode('utf-8') fails, in CPython's words (its decoder's ranges and reasons), or empty for UTF-8.
std::string utf8_error(const std::string &data) {
    const size_t n = data.size();
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(data[k]); };
    const auto follows = [&](size_t k) { return (byte(k) & 0xc0) == 0x80; };
    for (size_t i = 0; i < n;) {
        const unsigned char c = byte(i);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const char *reason = "invalid continuation byte";
        size_t end = i + 1;
        if (c < 0xc2 || c > 0xf4) {
            reason = "invalid start byte";
        } else if (c < 0xe0) {
            if (n - i < 2) {
                reason = "unexpected end of data";
                end = n;
            } else if (follows(i + 1)) {
                i += 2;
                continue;
            }
        } else {
            const bool four = c >= 0xf0;
            const size_t length = four ? 4 : 3;
            // The second byte's own range: E0 A0-BF, ED 80-9F, F0 90-BF, F4 80-8F.
            const auto second = [&]() {
                const unsigned char d = byte(i + 1);
                if (!follows(i + 1)) return false;
                if (four) return !(c == 0xf0 && d < 0x90) && !(c == 0xf4 && d >= 0x90);
                return !(c == 0xe0 && d < 0xa0) && !(c == 0xed && d >= 0xa0);
            };
            size_t good = 0;  // the bytes of the sequence that are right, before the end of the data or a wrong one
            if (n - i >= 2 && second()) {
                good = 2;
                while (good < length && i + good < n && follows(i + good)) ++good;
            } else if (n - i < 2) {
                good = 1;
            }
            if (good == length) {
                i += length;
                continue;
            }
            if (n - i < 2 || (good >= 2 && i + good == n)) {
                reason = "unexpected end of data";
                end = n;
            } else {
                end = i + std::max<size_t>(good, 1);
            }
        }
        char buffer[160];
        if (end == i + 1)
            std::snprintf(buffer, sizeof buffer, "'utf-8' codec can't decode byte 0x%02x in position %zu: %s", c, i, reason);
        else
            std::snprintf(buffer, sizeof buffer, "'utf-8' codec can't decode bytes in position %zu-%zu: %s", i, end - 1, reason);
        return buffer;
    }
    return std::string();
}

bool is_utf8(const std::string &text) { return utf8_error(text).empty(); }

// The code points of UTF-8 text.
std::vector<unsigned> code_points(const std::string &text) {
    std::vector<unsigned> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        unsigned code = n == 1 ? c : c & (0x3fu >> (n - 1));
        for (size_t k = 1; k < n && i + k < text.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3fu);
        out.push_back(code);
        i += n;
    }
    return out;
}

size_t characters(const std::string &text) {
    size_t n = 0;
    for (const char c : text) n += (static_cast<unsigned char>(c) & 0xc0) != 0x80;
    return n;
}

// The byte where the character after count characters starts.
size_t character_offset(const std::string &text, size_t count) {
    size_t at = 0;
    for (size_t seen = 0; at < text.size() && seen < count; ++seen) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) ++at;
    }
    return at;
}

// str.isspace() of one character, which is also what \s matches in a str pattern.
bool python_space(unsigned code) {
    return (code >= 0x09 && code <= 0x0d) || (code >= 0x1c && code <= 0x20) || code == 0x85 || code == 0xa0 || code == 0x1680 ||
           (code >= 0x2000 && code <= 0x200a) || code == 0x2028 || code == 0x2029 || code == 0x202f || code == 0x205f || code == 0x3000;
}

// str.strip() of UTF-8 text.
std::string python_strip(const std::string &text) {
    const std::vector<unsigned> points = code_points(text);
    size_t first = 0, last = points.size();
    while (first < last && python_space(points[first])) ++first;
    while (last > first && python_space(points[last - 1])) --last;
    const size_t start = character_offset(text, first);
    return text.substr(start, character_offset(text, last) - start);
}

// str.replace(old, new): every occurrence, left to right, in one pass.
std::string replace_all(const std::string &text, const std::string &old, const std::string &replacement) {
    if (old.empty()) return text;
    std::string out;
    size_t at = 0;
    for (size_t found; (found = text.find(old, at)) != kNone; at = found + old.size()) out += text.substr(at, found - at) + replacement;
    return out + text.substr(at);
}

bool ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool ascii_hex(char c) { return ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// int(text, base) for base 10 or 16: white space about it, a sign, for 16 an optional 0x, and single
// underscores between digits (or after the 0x). Its magnitude's digits without leading zeros, or false where int()
// raises ValueError. Own rule: only ASCII digits are digits (int() also reads other scripts' decimal digits).
bool python_int(const std::string &given, int base, bool &negative, std::string &digits) {
    std::string s = python_strip(given);
    negative = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        negative = s[0] == '-';
        s.erase(0, 1);
    }
    bool prefixed = false;
    if (base == 16 && s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.erase(0, 2);
        prefixed = true;
    }
    const auto digit = [&](char c) { return base == 16 ? ascii_hex(c) : ascii_digit(c); };
    digits.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            const bool after = i > 0 ? digit(s[i - 1]) : prefixed;
            if (!after || i + 1 >= s.size() || !digit(s[i + 1])) return false;
            continue;
        }
        if (!digit(s[i])) return false;
        digits += static_cast<char>(s[i] >= 'A' && s[i] <= 'F' ? s[i] - 'A' + 'a' : s[i]);
    }
    if (digits.empty()) return false;
    digits.erase(0, std::min(digits.find_first_not_of('0'), digits.size() - 1));
    if (digits == "0") negative = false;
    return true;
}

// ---- Python's words for values in messages ---------------------------------------------------------------------

const char *python_type(const Json &v) {
    if (v.is_null()) return "NoneType";
    if (v.is_boolean()) return "bool";
    if (v.is_number_integer()) return "int";
    if (v.is_number()) return "float";
    if (v.is_string()) return "str";
    if (v.is_array()) return "list";
    return "dict";
}

// repr() of a str: in single quotes unless it holds one and no double quote, with the escapes for its control
// characters. Own rule: characters past ASCII are written as they are, as repr() writes the printable ones.
std::string python_repr_text(const std::string &s) {
    const bool single = s.find('\'') == kNone || s.find('"') != kNone;
    const char quote = single ? '\'' : '"';
    std::string out(1, quote);
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == quote || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c == 0x7f) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\x%02x", c);
            out += buffer;
        } else {
            out += ch;
        }
    }
    return out + quote;
}

std::string python_repr(const Json &v) {
    if (v.is_string()) return python_repr_text(v.get<std::string>());
    if (v.is_null()) return "None";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_number()) return v.dump();
    std::string out;
    if (v.is_array()) {
        for (const Json &x : v) out += (out.empty() ? "" : ", ") + python_repr(x);
        return "[" + out + "]";
    }
    for (auto it = v.begin(); it != v.end(); ++it) out += (out.empty() ? "" : ", ") + python_repr_text(it.key()) + ": " + python_repr(it.value());
    return "{" + out + "}";
}

// str(value), and an f-string's {value}, of a value from JSON.
std::string python_str(const Json &v) { return v.is_string() ? v.get<std::string>() : python_repr(v); }

// value == number, as Python compares a JSON value with an int (True is 1).
bool python_equals(const Json *v, double number) {
    if (!v) return false;
    if (v->is_boolean()) return (v->get<bool>() ? 1.0 : 0.0) == number;
    return v->is_number() && v->get<double>() == number;
}

// item[key]: KeyError's message where a dict lacks it, TypeError's where item is not a dict.
const Json &python_item(const Json &item, const char *key) {
    if (item.is_object()) {
        const Json *found = member(item, key);
        if (!found) throw ValueError(python_repr_text(key));
        return *found;
    }
    if (item.is_array()) throw ValueError("list indices must be integers or slices, not str");
    if (item.is_string()) throw ValueError("string indices must be integers, not 'str'");
    throw ValueError(std::string("'") + python_type(item) + "' object is not subscriptable");
}

// dict.get(key); AttributeError's message for anything that is not a dict.
const Json *python_get(const Json &v, const char *key) {
    if (!v.is_object()) throw ValueError(std::string("'") + python_type(v) + "' object has no attribute 'get'");
    return member(v, key);
}

// What a for loop takes from a value: a list's values, a dict's keys, a str's characters; TypeError's message for
// anything else.
std::vector<Json> python_iterate(const Json &v) {
    std::vector<Json> out;
    if (v.is_array()) {
        for (const Json &x : v) out.push_back(x);
    } else if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) out.push_back(Json(it.key()));
    } else if (v.is_string()) {
        const std::string &s = v.get_ref<const std::string &>();
        for (size_t i = 0; i < s.size();) {
            size_t next = i + 1;
            while (next < s.size() && (static_cast<unsigned char>(s[next]) & 0xc0) == 0x80) ++next;
            out.push_back(Json(s.substr(i, next - i)));
            i = next;
        }
    } else {
        throw ValueError(std::string("'") + python_type(v) + "' object is not iterable");
    }
    return out;
}

// text in container: a list's equal member, a dict's key, a substring of a str; TypeError's message otherwise.
bool python_contains(const Json &container, const std::string &text) {
    if (container.is_string()) return container.get<std::string>().find(text) != kNone;
    if (container.is_object()) return container.find(text) != container.end();
    if (container.is_array()) {
        for (const Json &x : container)
            if (x.is_string() && x.get<std::string>() == text) return true;
        return false;
    }
    throw ValueError(std::string("argument of type '") + python_type(container) + "' is not iterable");
}

bool fullmatch_token(const std::string &text, size_t low, size_t high) {
    return text.size() >= low && text.size() <= high && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || ascii_digit(c) || c == '_' || c == '-';
           });
}

bool lower_hex(const std::string &text, size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) { return ascii_digit(c) || (c >= 'a' && c <= 'f'); });
}

// json.loads of the bytes, as the editor's answers were read: a UTF-8 signature is passed over. Own rule: the
// text is UTF-8, where json.loads would also read UTF-16 and UTF-32 by their first bytes; and JSON's own
// grammar, where json.loads also reads NaN and Infinity.
Json json_bytes(std::string raw, const char *message) {
    if (raw.compare(0, 3, "\xef\xbb\xbf") == 0) raw.erase(0, 3);
    try {
        return Json::parse(raw);
    } catch (const std::exception &) {
        throw ValueError(message);
    }
}

// ---- identifiers and formats ------------------------------------------------------------------------------------

std::string hex_text(const std::string &bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string uuid_form(const std::string &hex32) {
    return hex32.substr(0, 8) + "-" + hex32.substr(8, 4) + "-" + hex32.substr(12, 4) + "-" + hex32.substr(16, 4) + "-" + hex32.substr(20);
}

// str(uuid4()).
std::string uuid4() {
    std::string b = random_bytes(16);
    b[6] = static_cast<char>((static_cast<unsigned char>(b[6]) & 0x0f) | 0x40);
    b[8] = static_cast<char>((static_cast<unsigned char>(b[8]) & 0x3f) | 0x80);
    return uuid_form(hex_text(b));
}

// str(UUID(text)), or false where UUID() raises: "urn:" and "uuid:" replaced away, braces stripped from the ends,
// hyphens removed, then 32 characters read by int(hex, 16) and in range.
bool parse_uuid(const std::string &text, std::string &canonical) {
    std::string h = replace_all(replace_all(text, "urn:", ""), "uuid:", "");
    size_t first = 0, last = h.size();
    while (first < last && (h[first] == '{' || h[first] == '}')) ++first;
    while (last > first && (h[last - 1] == '{' || h[last - 1] == '}')) --last;
    h = replace_all(h.substr(first, last - first), "-", "");
    if (characters(h) != 32) return false;
    bool negative;
    std::string digits;
    if (!python_int(h, 16, negative, digits) || negative || digits.size() > 32) return false;
    canonical = uuid_form(std::string(32 - digits.size(), '0') + digits);
    return true;
}

// format(value, 'g').
std::string format_g(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%g", value);
    return buffer;
}

// urllib.parse.quote_plus of UTF-8 text.
std::string quote_plus(const std::string &text) {
    std::string out;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || ascii_digit(ch) || c == '_' || c == '.' || c == '-' || c == '~') {
            out += ch;
        } else if (c == ' ') {
            out += '+';
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof buffer, "%%%02X", c);
            out += buffer;
        }
    }
    return out;
}

// ---- addresses --------------------------------------------------------------------------------------------------

// urlsplit(url).port: -1 for none, false where Python's raises ValueError.
bool port_of(const UrlParts &parts, long &port) {
    port = -1;
    if (!parts.has_port || parts.port.empty()) return true;
    if (!std::all_of(parts.port.begin(), parts.port.end(), ascii_digit)) return false;
    const size_t start = std::min(parts.port.find_first_not_of('0'), parts.port.size() - 1);
    if (parts.port.size() - start > 5) return false;
    port = std::stol(parts.port.substr(start));
    return port <= 65535;
}

// ipaddress.IPv4Address's text: four decimal octets of up to three digits, no leading zeros, each at most 255.
bool ipv4(const std::string &text, unsigned char out[4]) {
    size_t at = 0;
    for (int part = 0; part < 4; ++part) {
        if (part && (at >= text.size() || text[at++] != '.')) return false;
        const size_t start = at;
        while (at < text.size() && ascii_digit(text[at])) ++at;
        if (at == start || at - start > 3 || (at - start > 1 && text[start] == '0')) return false;
        const int value = std::stoi(text.substr(start, at - start));
        if (value > 255) return false;
        out[part] = static_cast<unsigned char>(value);
    }
    return at == text.size();
}

// ipaddress.IPv6Address's text (_ip_int_from_string, with a scope after '%') and its sixteen bytes.
bool ipv6(std::string text, unsigned char out[16]) {
    const size_t percent = text.find('%');
    if (percent != kNone) {
        if (percent + 1 >= text.size() || text.find('%', percent + 1) != kNone) return false;
        text.resize(percent);
    }
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
        const size_t colon = text.find(':', at);
        parts.push_back(text.substr(at, colon == kNone ? kNone : colon - at));
        if (colon == kNone) break;
        at = colon + 1;
    }
    if (text.empty() || parts.size() < 3) return false;
    std::vector<unsigned> tail_v4;
    if (parts.back().find('.') != kNone) {
        unsigned char v4[4];
        if (!ipv4(parts.back(), v4)) return false;
        parts.pop_back();
        char buffer[16];
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
        if (part.empty() || part.size() > 4 || !std::all_of(part.begin(), part.end(), ascii_hex)) return false;
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

// ipaddress.ip_address(host).is_loopback in Python 3.13: 127.0.0.0/8, ::1, and an IPv4-mapped 127.0.0.0/8.
bool loopback(const std::string &host) {
    unsigned char v4[4], v6[16];
    if (ipv4(host, v4)) return v4[0] == 127;
    if (!ipv6(host, v6)) return false;
    static const unsigned char one[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::memcmp(v6, one, 16) == 0 || (std::memcmp(v6, mapped, 12) == 0 && v6[12] == 127);
}

struct Location {
    std::string scheme, host;
    long port = -1;
};

// The checks the reference made of a configured base address; false where it called the address unusable.
bool base_address(const std::string &url, bool https_only, Location &out) {
    UrlParts parts;
    long port;
    try {
        parts = url_split(url);
    } catch (const std::exception &) {
        return false;
    }
    if (!port_of(parts, port)) return false;
    const bool scheme = https_only ? parts.scheme == "https" : (parts.scheme == "http" || parts.scheme == "https");
    // re.search(r'[\s\\\x00-\x1f\x7f]', url). Own rule: text that is not UTF-8 is refused here too.
    bool bad = !is_utf8(url);
    for (const unsigned c : bad ? std::vector<unsigned>() : code_points(url)) bad = bad || c < 0x20 || c == 0x7f || c == '\\' || python_space(c);
    if (!scheme || !parts.has_hostname || parts.hostname.empty() || parts.has_user || (parts.path != "" && parts.path != "/") || url.find('?') != kNone ||
        url.find('#') != kNone || bad || port == 0)
        return false;
    out.scheme = parts.scheme;
    out.host = parts.hostname;
    out.port = port;
    return true;
}

// Where http.client.HTTPConnection(host, port) connects, as the relay's URL. With no port, http.client takes
// what follows the host's last ':' for one, so an IPv6 address given without a port loses its last group to the
// port, or the connection is refused as InvalidURL; refused is then what each request fails with.
std::string connection_url(const std::string &scheme, std::string host, long port, const char *failed, std::string &refused) {
    const long standard = scheme == "https" ? 443 : 80;
    if (port < 0) {
        const size_t colon = host.rfind(':'), bracket = host.rfind(']');
        if (colon != kNone && (bracket == kNone || colon > bracket)) {
            const std::string tail = host.substr(colon + 1);
            bool negative;
            std::string digits;
            if (python_int(tail, 10, negative, digits)) {
                // A port outside 0-65535 fails to connect (or to convert) in the reference.
                if (negative || digits.size() > 5 || std::stol(digits) > 65535) refused = failed;
                else port = std::stol(digits);
            } else if (tail.empty()) {
                port = standard;
            } else {
                refused = "nonnumeric port: '" + tail + "'";
            }
            host.resize(colon);
        } else {
            port = standard;
        }
    }
    if (!host.empty() && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (host.find(':') != kNone) host = "[" + replace_all(host, "%", "%25") + "]";
    return scheme + "://" + host + (port != standard ? ":" + std::to_string(port) : std::string());
}

// ---- the relay -------------------------------------------------------------------------------------------------

struct Fetched {
    long long status = 0;
    Json headers;
    std::string body;
};

bool same_name(const std::string &a, const std::string &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The header fields of an answer's head as http.client's email parser read them (FeedParser and _parse_headers
// with the compat32 policy) from the head's lines, each byte a character: the lines split at \r\n, \r or \n; fields
// up to the first line that is not one (a name of printable characters other than ':', then ':'; a continuation;
// "From "); a field's name before its first ':', and its value without the spaces and tabs after the ':', the
// continuation lines joined to it with their line ends, and the line ends at its end removed.
Json python_fields(const std::string &head) {
    std::vector<std::string> lines;
    for (size_t at = 0; at < head.size();) {
        size_t end = head.find_first_of("\r\n", at);
        if (end == kNone) {
            lines.push_back(head.substr(at));
            break;
        }
        end += head.compare(end, 2, "\r\n") == 0 ? 2 : 1;
        lines.push_back(head.substr(at, end - at));
        at = end;
    }
    std::vector<std::string> fields;
    for (const std::string &line : lines) {
        size_t name = 0;
        while (name < line.size() && static_cast<unsigned char>(line[name]) >= 0x21 && static_cast<unsigned char>(line[name]) <= 0x7e && line[name] != ':') ++name;
        const bool header = line.compare(0, 5, "From ") == 0 || (name < line.size() && line[name] == ':') || line[0] == '\t' || line[0] == ' ';
        if (!header) break;  // the empty line that ends the head, or the body's start
        fields.push_back(line);
    }
    Json out = Json::array();
    std::string name, value;
    bool pending = false;
    const auto flush = [&]() {
        if (!pending) return;
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        out.push_back(Json::array({Json(name), Json(value)}));
        pending = false;
    };
    for (const std::string &line : fields) {
        if (line[0] == ' ' || line[0] == '\t') {
            if (pending) value += line;  // a continuation with no field before it is a defect, and passed over
            continue;
        }
        flush();
        if (line.compare(0, 5, "From ") == 0) continue;  // an envelope line, not a field
        const size_t colon = line.find(':');
        if (colon == 0) continue;  // a field without a name is a defect, and passed over
        name = line.substr(0, colon);
        size_t start = colon + 1;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        value = line.substr(start);
        pending = true;
    }
    flush();
    return out;
}

// The values of an answer's header, its name in any case: Message.get_all() of the fields above, or, from a relay that
// does not give the head's lines, the values its HTTP client read.
std::vector<std::string> header_values(const Json &headers, const std::string &name) {
    std::vector<std::string> out;
    if (headers.is_array()) {
        for (const Json &field : headers)
            if (same_name(field.at(0).get<std::string>(), name)) out.push_back(field.at(1).get<std::string>());
        return out;
    }
    if (!headers.is_object()) return out;
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (!same_name(it.key(), name) || !it.value().is_array()) continue;
        for (const Json &v : it.value())
            if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

bool decode_base64(const std::string &text, std::string &out) {
    out.clear();
    unsigned value = 0;
    int bits = 0;
    bool padding = false;
    for (const unsigned char c : text) {
        int digit;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+') digit = 62;
        else if (c == '/') digit = 63;
        else if (c == '=') {
            padding = true;
            continue;
        } else {
            return false;
        }
        if (padding) return false;
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((value >> bits) & 0xff);
        }
    }
    return true;
}

// One request through the relay; false where the reference's http.client failed: no answer, the relay's error, or
// a head past http.client's limits (more than 99 header lines, or a line longer than 65536 bytes).
bool fetch(const std::string &relay, const std::string &method, const std::string &url, const Json &headers, const std::string &body,
           const std::string &ca_file, double seconds, size_t limit, Fetched &out) {
    Json request = Json::object();
    request["method"] = method;
    request["url"] = url;
    request["headers"] = headers;
    request["body"] = body;
    request["ca_file"] = ca_file;
    request["timeout_seconds"] = seconds;
    request["limit"] = static_cast<long long>(limit);
    Json answer;
    try {
        answer = relay_fetch(relay, request, seconds + 10);
    } catch (const std::exception &) {
        return false;
    }
    const Json *status = member(answer, "status");
    if (!answer.is_object() || member(answer, "error") || !status || !status->is_number_integer()) return false;
    out.status = status->get<long long>();
    const Json *heads = member(answer, "headers"), *head = member(answer, "head");
    out.headers = head && head->is_string() ? python_fields(head->get<std::string>()) : heads && heads->is_object() ? *heads : Json::object();
    const Json *lines = member(answer, "head_lines"), *longest = member(answer, "longest_line");
    if ((lines && lines->is_number() && lines->get<double>() >= 100) || (longest && longest->is_number() && longest->get<double>() > 65536)) return false;
    const Json *data = member(answer, "body");
    out.body.clear();
    return !data || (data->is_string() && decode_base64(data->get<std::string>(), out.body));
}

// A CA file the relay reads a certificate from, where ssl.create_default_context(cafile=...) succeeded. Own rule:
// as Go reads it, PEM certificates, passing over a block that does not parse.
bool ca_file_usable(const std::string &relay, const std::string &ca_file) {
    Json request = Json::object();
    request["check_only"] = true;
    request["ca_file"] = ca_file;
    try {
        const Json answer = relay_fetch(relay, request, 15);
        return answer.is_object() && !member(answer, "error");
    } catch (const std::exception &) {
        return false;
    }
}

std::string read_limited(const fs::path &path, size_t limit, bool &ok) {
    ok = false;
    std::error_code error;
    if (fs::is_directory(path, error)) return std::string();
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::string out(limit, '\0');
    in.read(&out[0], static_cast<std::streamsize>(limit));
    if (in.bad()) return std::string();
    out.resize(static_cast<size_t>(in.gcount()));
    ok = true;
    return out;
}

// ---- http.cookies.SimpleCookie, as the reference read the editor's Set-Cookie headers ---------------------------

bool cookie_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }  // \s with re.ASCII
bool cookie_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || ascii_digit(c) || c == '_'; }
bool cookie_key_char(char c) { return cookie_word(c) || (c != 0 && std::strchr("!#%&'~`><@,:/$*+-.^|)(?}{=", c)); }  // _LegalKeyChars
bool cookie_value_char(char c) { return cookie_key_char(c) || c == '[' || c == ']'; }                               // _LegalValueChars
bool cookie_legal(const std::string &key) {                                                                          // _is_legal_key
    return !key.empty() && std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || ascii_digit(c) || (c != 0 && std::strchr("!#$%&'*+-.^_`|~:", c));
    });
}
bool cookie_control(const std::string &text) {
    return std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

struct CookieToken {
    size_t end = 0;
    std::string key;
    bool has_value = false;
    std::string value;
};

// _CookiePattern.match(text, at), its backtracking followed: the key as short as can be, then the value's
// alternatives in their order (a quoted string, the date of an "expires", a word), each followed by
// \s*(\s+|;|$).
bool cookie_token(const std::string &s, size_t at, CookieToken &out) {
    const size_t n = s.size();
    const auto spaces = [&](size_t p) {
        while (p < n && cookie_space(s[p])) ++p;
        return p;
    };
    const auto ending = [&](size_t p) -> size_t {
        const size_t q = spaces(p);
        if (q > p) return q < n && s[q] == ';' ? q + 1 : q;
        if (p < n && s[p] == ';') return p + 1;
        return p == n ? p : kNone;
    };
    const auto literal = [&](size_t p, const char *text) { return p + std::strlen(text) <= n && s.compare(p, std::strlen(text), text) == 0; };
    // (\w{3,6}day|\w{3}),\s[\w\d\s-]{9,11}\s[\d:]{8}\sGMT at v: where the value and the cookie end
    const auto expires = [&](size_t v, size_t &value_end, size_t &end) {
        std::vector<size_t> commas;
        const auto words = [&](size_t p, size_t count) {
            for (size_t i = 0; i < count; ++i)
                if (p + i >= n || !cookie_word(s[p + i])) return false;
            return true;
        };
        for (size_t k = 6; k >= 3; --k)
            if (words(v, k) && literal(v + k, "day")) commas.push_back(v + k + 3);
        if (words(v, 3)) commas.push_back(v + 3);
        for (const size_t comma : commas) {
            if (comma + 1 >= n || s[comma] != ',' || !cookie_space(s[comma + 1])) continue;
            for (size_t length = 11; length >= 9; --length) {
                size_t p = comma + 2;
                bool ok = p + length <= n;
                for (size_t i = 0; ok && i < length; ++i) ok = cookie_word(s[p + i]) || cookie_space(s[p + i]) || s[p + i] == '-';
                p += length;
                ok = ok && p < n && cookie_space(s[p]);
                ++p;
                for (size_t i = 0; ok && i < 8; ++i) ok = p + i < n && (ascii_digit(s[p + i]) || s[p + i] == ':');
                p += 8;
                ok = ok && p < n && cookie_space(s[p]) && literal(p + 1, "GMT");
                if (!ok) continue;
                const size_t e = ending(p + 4);
                if (e != kNone) {
                    value_end = p + 4;
                    end = e;
                    return true;
                }
            }
        }
        return false;
    };
    const size_t start = spaces(at);
    size_t longest = start;
    while (longest < n && cookie_key_char(s[longest])) ++longest;
    for (size_t key_end = start + 1; key_end <= longest; ++key_end) {
        const auto found = [&](size_t v, size_t value_end, size_t end) {
            out.end = end;
            out.key = s.substr(start, key_end - start);
            out.has_value = v != kNone;
            out.value = v != kNone ? s.substr(v, value_end - v) : std::string();
            return true;
        };
        const size_t equals = spaces(key_end);
        if (equals < n && s[equals] == '=') {
            const size_t first = equals + 1;
            for (size_t v = spaces(first);; --v) {
                if (v < n && s[v] == '"') {
                    size_t j = v + 1;
                    while (j < n && s[j] != '"') {
                        if (s[j] == '\\') {
                            if (j + 1 >= n || s[j + 1] == '\n') break;
                            ++j;
                        }
                        ++j;
                    }
                    if (j < n && s[j] == '"') {
                        const size_t e = ending(j + 1);
                        if (e != kNone) return found(v, j + 1, e);
                    }
                }
                size_t value_end, end;
                if (expires(v, value_end, end)) return found(v, value_end, end);
                size_t r = v;
                while (r < n && cookie_value_char(s[r])) ++r;
                for (size_t word_end = r;; --word_end) {
                    const size_t e = ending(word_end);
                    if (e != kNone) return found(v, word_end, e);
                    if (word_end == v) break;
                }
                if (v == first) break;
            }
        }
        const size_t e = ending(key_end);
        if (e != kNone) return found(kNone, 0, e);
    }
    return false;
}

// _unquote: a value in double quotes loses them, and its \ooo (octal, from \000 to \377) and \c escapes.
std::string cookie_unquote(const std::string &value) {
    if (characters(value) < 2 || value.front() != '"' || value.back() != '"') return value;
    const std::string inner = value.substr(1, value.size() - 2);
    std::string out;
    for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] != '\\' || i + 1 >= inner.size() || inner[i + 1] == '\n') {
            out += inner[i];
            continue;
        }
        if (i + 3 < inner.size() && inner[i + 1] >= '0' && inner[i + 1] <= '3' && inner[i + 2] >= '0' && inner[i + 2] <= '7' && inner[i + 3] >= '0' &&
            inner[i + 3] <= '7') {
            const unsigned code = static_cast<unsigned>((inner[i + 1] - '0') * 64 + (inner[i + 2] - '0') * 8 + (inner[i + 3] - '0'));
            if (code < 0x80) out += static_cast<char>(code);
            else {
                out += static_cast<char>(0xc0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3f));
            }
            i += 3;
        } else {
            out += inner[i + 1];
            ++i;
        }
    }
    return out;
}

// The cookie of this name after SimpleCookie().load(value) of each Set-Cookie header: whether one was set, its
// value, and whether its morsel's "secure" is true. CookieError's message where load() raises one.
struct Cookie {
    bool found = false, secure = false;
    std::string value;
};

Cookie read_cookie(const Json &headers, const std::string &name) {
    static const char *const reserved[] = {"expires", "path", "comment", "domain", "max-age", "secure", "httponly", "version", "samesite"};
    const auto is_reserved = [](const std::string &lower) { return std::find(std::begin(reserved), std::end(reserved), lower) != std::end(reserved); };
    const auto lower = [](std::string text) {
        for (char &c : text)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return text;
    };
    Cookie cookie;
    for (const std::string &header : header_values(headers, "Set-Cookie")) {
        struct Item {
            bool attribute;
            std::string key;
            int kind;  // for an attribute: 0 a text value, 1 True, 2 None
            std::string value, coded;
        };
        std::vector<Item> items;
        bool morsel = false, invalid = false;
        CookieToken token;
        for (size_t i = 0; i < header.size() && cookie_token(header, i, token);) {
            i = token.end;
            if (token.key[0] == '$') {
                if (!morsel) continue;
                items.push_back({true, token.key.substr(1), token.has_value ? 0 : 2, token.value, std::string()});
            } else if (is_reserved(lower(token.key))) {
                if (!morsel) {
                    invalid = true;
                    break;
                }
                if (!token.has_value) {
                    if (lower(token.key) != "secure" && lower(token.key) != "httponly") {
                        invalid = true;
                        break;
                    }
                    items.push_back({true, token.key, 1, std::string(), std::string()});
                } else {
                    items.push_back({true, token.key, 0, cookie_unquote(token.value), std::string()});
                }
            } else if (token.has_value) {
                items.push_back({false, token.key, 0, cookie_unquote(token.value), token.value});
                morsel = true;
            } else {
                invalid = true;
                break;
            }
        }
        if (invalid) continue;
        std::string current;
        for (const Item &item : items) {
            if (item.attribute) {
                const std::string key = lower(item.key);
                if (!is_reserved(key)) throw ValueError("Invalid attribute " + python_repr_text(key));
                if (cookie_control(key) || (item.kind == 0 && cookie_control(item.value)))
                    throw ValueError("Control characters are not allowed in cookies " + python_repr_text(key) + " " +
                                     (item.kind == 0 ? python_repr_text(item.value) : item.kind == 1 ? "True" : "None"));
                if (current == name && key == "secure") cookie.secure = item.kind == 1 || (item.kind == 0 && !item.value.empty());
            } else {
                if (!cookie_legal(item.key)) throw ValueError("Illegal key " + python_repr_text(item.key));
                if (cookie_control(item.key) || cookie_control(item.value) || cookie_control(item.coded))
                    throw ValueError("Control characters are not allowed in cookies " + python_repr_text(item.key) + " " + python_repr_text(item.value) + " " +
                                     python_repr_text(item.coded));
                current = item.key;
                if (item.key == name) {
                    cookie.found = true;
                    cookie.value = item.value;
                }
            }
        }
    }
    return cookie;
}

std::string secure_cookie(const Json &headers, const char *name) {
    const Cookie cookie = read_cookie(headers, name);
    if (!cookie.found || !cookie.secure || !fullmatch_token(cookie.value, 32, 128))
        throw ValueError("The hosted editor did not issue the expected secure session cookie.");
    return cookie.value;
}

}  // namespace

// ---- the paired monitor ---------------------------------------------------------------------------------------

std::unique_ptr<PendantClient> PendantClient::from_environment(const std::string &relay) {
    const std::string url = environment("KERFKITE_PENDANT_URL"), token = environment("KERFKITE_PENDANT_TOKEN"),
                      filename = environment("KERFKITE_PENDANT_TOKEN_FILE"), ca = environment("KERFKITE_PENDANT_CA_FILE");
    if (url.empty() && token.empty() && filename.empty() && ca.empty()) return nullptr;
    if (url.empty() || token.empty() == filename.empty())
        throw ValueError("Set the pendant URL and exactly one of KERFKITE_PENDANT_TOKEN or KERFKITE_PENDANT_TOKEN_FILE.");
    std::string given = token;
    if (!filename.empty()) {
        bool ok = false;
        const std::string data = read_limited(fs::u8path(filename), 258, ok);
        if (!ok || data.size() > 257 || !std::all_of(data.begin(), data.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }))
            throw ValueError("Could not read the configured pendant token file.");
        given = python_strip(data);
    }
    return std::unique_ptr<PendantClient>(new PendantClient(url, given, ca, relay));
}

PendantClient::PendantClient(const std::string &url, const std::string &token, const std::string &ca_file, const std::string &relay)
    : token_(token), ca_file_(ca_file), relay_(relay) {
    Location where;
    if (!base_address(url, false, where))
        throw ValueError("Set KERFKITE_PENDANT_URL to the monitor base address without credentials, paths, queries or fragments.");
    const bool relayed = !relay.empty() && file_exists(relay);
    if (where.scheme == "http") {
        if (where.host == "localhost") where.host = "127.0.0.1";
        if (!loopback(where.host)) throw ValueError("A network pendant requires HTTPS. HTTP is supported on loopback only.");
        ca_file_.clear();  // a plain connection has no use for it
    } else if (!ca_file.empty()) {
        if (!relayed) throw ValueError(kNoRelay);
        if (!ca_file_usable(relay, ca_file)) throw ValueError("Could not load the operator-configured pendant CA certificate.");
    }
    if (!fullmatch_token(token, 32, 256)) throw ValueError("Supply the generated monitor pairing token, separately from the base address.");
    if (!relayed) throw ValueError(kNoRelay);
    base_ = connection_url(where.scheme, where.host, where.port, kMonitorFailed, refused_);
}

Json PendantClient::request(const std::string &path, const Json *payload) const {
    // A direct connection never follows redirects, uses environment proxies, or retries motion.
    if (!refused_.empty()) throw ValueError(refused_);
    Json headers = Json::object();
    headers["Authorization"] = "Bearer " + token_;
    headers["Content-Type"] = "application/json";
    Fetched answer;
    if (!fetch(relay_, payload ? "POST" : "GET", base_ + path, headers, payload ? payload->dump() : std::string(), ca_file_, 6, 131073, answer))
        throw ValueError(kMonitorFailed);
    if (answer.status == 401) throw ValueError("Pairing expired. Renew the monitor pairing at the workstation and restart this MCP server.");
    if (answer.status >= 300 && answer.status < 400) throw ValueError("The paired monitor returned a redirect. Check its configured base address.");
    if (answer.body.size() > 131072) throw ValueError("The paired monitor response is too large.");
    // json.loads of the decoded text, which refuses a byte order mark (and, own rule, NaN and Infinity). Prevent a
    // misconfigured endpoint from echoing the bearer into an MCP result.
    const char *const invalid = "The paired monitor returned an invalid response. Check its status before retrying.";
    if (!is_utf8(answer.body) || answer.body.compare(0, 3, "\xef\xbb\xbf") == 0) throw ValueError(invalid);
    Json result;
    try {
        result = Json::parse(replace_all(answer.body, token_, "[redacted]"));
    } catch (const std::exception &) {
        throw ValueError(invalid);
    }
    if (!result.is_object()) throw ValueError("The paired monitor returned an unexpected response.");
    if (answer.status != 200) {
        const Json *error = member(result, "error");
        const std::string message = error ? python_str(*error) : std::string("Action rejected");
        throw ValueError("Paired monitor: " + message.substr(0, character_offset(message, 300)));
    }
    return result;
}

Json PendantClient::status() const {
    const Json state = request("/api/status", nullptr);
    Json out = Json::object();
    for (const char *key : {"connected", "simulated", "state", "active", "progress", "position", "error", "protocol", "pendant", "controls", "armed", "control_epoch"}) {
        const Json *value = member(state, key);
        out[key] = value ? *value : Json();
    }
    out["request_id"] = uuid4();
    return out;
}

Json PendantClient::action(mcp::Exchange &exchange, const std::string &action, const std::string &request_id, const std::string &control_epoch,
                           const Json &values) const {
    std::string identifier;
    if (!parse_uuid(request_id, identifier)) throw ValueError("Use a request_id from pendant_status, or supply a UUID for this one action.");
    Json payload = values.is_object() ? values : Json::object();
    payload["action"] = action;
    payload["request_id"] = identifier;
    if (action != "stop") {
        if (control_epoch.empty() || characters(control_epoch) > 128) throw ValueError("Read pendant_status and supply its current control_epoch.");
        payload["control_epoch"] = control_epoch;
    }
    std::string prompt;
    if (action == "jog") {
        const std::string axis = values.at("axis").get<std::string>();
        const double distance = values.at("distance").get<double>(), feed = values.at("feed").get<double>();
        if ((axis != "X" && axis != "Y" && axis != "Z") || !std::isfinite(distance) || !std::isfinite(feed) || !(std::fabs(distance) >= 0.001) ||
            !(std::fabs(distance) <= 10) || !(feed >= 0.1) || !(feed <= 1000))
            throw ValueError("A jog uses one XYZ axis, a signed 0.001\xe2\x80\x93" "10 mm step and 0.1\xe2\x80\x93" "1000 mm/min feed.");
        prompt = "Jog " + axis + " by " + format_g(distance) + " mm at " + format_g(feed) + " mm/min on the paired machine?";
    } else if (action == "start") {
        prompt = "Start the job currently armed at the workstation?";
    } else if (action == "resume") {
        prompt = "Resume the held job on the paired machine?";
    } else if (action == "home") {
        prompt = "Home the paired machine axes?";
    } else if (action == "zero") {
        prompt = "Set the paired machine work zero at its current XYZ position?";
    }
    if (!prompt.empty()) {
        std::string answer;
        try {
            answer = exchange.confirm(prompt);
        } catch (const std::exception &) {
            throw ValueError(
                "This action requires an operator confirmation through an elicitation-capable MCP client. Use the paired pendant or workstation if unavailable.");
        }
        if (answer != "accept") {
            Json out = Json::object();
            out["accepted"] = false;
            out["action"] = action;
            return out;
        }
    }
    const std::string endpoint = action == "jog" || action == "home" || action == "zero" ? "/api/pendant" : "/api/action";
    const Json result = request(endpoint, &payload);
    const Json *ok = member(result, "ok");
    if (!ok || !ok->is_boolean() || !ok->get<bool>()) throw ValueError("The paired monitor did not confirm this action. Read its status before retrying.");
    Json out = Json::object();
    out["accepted"] = true;
    out["action"] = action;
    out["request_id"] = identifier;
    return out;
}

// ---- the hosted editor account --------------------------------------------------------------------------------

std::unique_ptr<HostedFiles> HostedFiles::from_environment(const std::string &relay) {
    const std::string url = environment("KERFKITE_FILES_URL"), username = environment("KERFKITE_FILES_USERNAME"),
                      password = environment("KERFKITE_FILES_PASSWORD"), filename = environment("KERFKITE_FILES_PASSWORD_FILE"),
                      ca = environment("KERFKITE_FILES_CA_FILE");
    if (url.empty() && username.empty() && password.empty() && filename.empty() && ca.empty()) return nullptr;
    if (url.empty() || username.empty() || password.empty() == filename.empty())
        throw ValueError("Set the hosted editor URL, username and exactly one password or password file.");
    std::string given = password;
    if (!filename.empty()) {
        bool ok = false;
        std::string data = read_limited(fs::u8path(filename), 1027, ok);
        if (!ok || data.size() > 1026 || !is_utf8(data)) throw ValueError("Could not read the configured hosted editor password file.");
        if (!data.empty() && data.back() == '\n') data.pop_back();
        if (!data.empty() && data.back() == '\r') data.pop_back();
        given = data;
    }
    return std::unique_ptr<HostedFiles>(new HostedFiles(url, username, given, ca, relay));
}

HostedFiles::HostedFiles(const std::string &url, const std::string &username, const std::string &password, const std::string &ca_file,
                         const std::string &relay)
    : ca_file_(ca_file), relay_(relay) {
    Location where;
    if (!base_address(url, true, where))
        throw ValueError("Set KERFKITE_FILES_URL to the fixed HTTPS editor origin, without credentials, paths, query or fragment.");
    origin_ = url;
    while (!origin_.empty() && origin_.back() == '/') origin_.pop_back();
    user_ = web_username(username);
    if (password.empty() || characters(password) > 256) throw ValueError("Supply the hosted account password in the operator configuration.");
    password_ = password;
    const bool relayed = !relay.empty() && file_exists(relay);
    if (!ca_file.empty()) {
        if (!relayed) throw ValueError(kNoRelay);
        if (!ca_file_usable(relay, ca_file)) throw ValueError("Could not load the configured hosted editor CA certificate.");
    }
    if (!relayed) throw ValueError(kNoRelay);
    base_ = connection_url("https", where.host, where.port, kEditorFailed, refused_);
}

HostedFiles::Answer HostedFiles::exchange(const std::string &method, const std::string &path, const Json &headers, const std::string &body,
                                          size_t limit) const {
    // Connect only to the configured origin; login and receiver redirects are never followed.
    if (!refused_.empty()) throw ValueError(refused_);
    Fetched got;
    if (!fetch(relay_, method, base_ + path, headers, body, ca_file_, 20, limit + 1, got)) throw ValueError(kEditorFailed);
    if (got.body.size() > limit) throw ValueError("Hosted editor response exceeds the allowed size.");
    Answer out;
    out.status = got.status;
    out.headers = got.headers;
    out.body = got.body;
    return out;
}

void HostedFiles::login() const {
    Answer a = exchange("GET", "/login", Json::object(), std::string(), 262144);
    if (a.status != 200) throw ValueError("The configured HTTPS origin did not serve the KerfKite login form.");
    const std::string csrf = secure_cookie(a.headers, kLoginCookie);
    const std::string form = "username=" + quote_plus(user_) + "&password=" + quote_plus(password_) + "&csrf=" + quote_plus(csrf);
    Json headers = Json::object();
    headers["Origin"] = origin_;
    headers["Cookie"] = std::string(kLoginCookie) + "=" + csrf;
    headers["Content-Type"] = "application/x-www-form-urlencoded";
    a = exchange("POST", "/login", headers, form, 262144);
    const std::vector<std::string> location = header_values(a.headers, "Location");
    if (a.status != 302 || location.empty() || location.front() != "/") throw ValueError("Hosted editor login failed. Check the configured account or login rate limit.");
    const std::string session = secure_cookie(a.headers, kSessionCookie);
    Json cookie = Json::object();
    cookie["Cookie"] = std::string(kSessionCookie) + "=" + session;
    a = exchange("GET", "/api/session", cookie, std::string(), 262144);
    const Json payload = a.status == 200 ? json_bytes(a.body, "Invalid response from the hosted editor.") : Json::object();
    const Json *web_mode = payload.is_object() ? member(payload, "web_mode") : nullptr;
    const Json *token = payload.is_object() ? member(payload, "token") : nullptr;
    const std::string token_text = token ? python_str(*token) : std::string();
    if (!payload.is_object() || !web_mode || !web_mode->is_boolean() || !web_mode->get<bool>() || !fullmatch_token(token_text, 32, 128))
        throw ValueError("Hosted editor session could not be verified.");
    session_ = session;
    csrf_ = token_text;
}

std::string HostedFiles::call(const std::string &path, const Json *payload, bool download) const {
    std::lock_guard<std::mutex> hold(lock_);
    if (session_.empty()) login();
    for (int attempt = 0;; ++attempt) {
        Json headers = Json::object();
        headers["Cookie"] = std::string(kSessionCookie) + "=" + session_;
        std::string body;
        if (payload) {
            body = payload->dump();
            headers["Origin"] = origin_;
            headers["X-Studio-Token"] = csrf_;
            headers["Content-Type"] = "application/json";
        }
        const Answer a = exchange(payload ? "POST" : "GET", path, headers, body, download ? kFileLimit : 262144);
        if (a.status == 401 && attempt == 0) {
            // KerfKite rejects an expired session before any write.
            session_.clear();
            csrf_.clear();
            login();
            continue;
        }
        if (a.status != 200)
            throw ValueError("Hosted editor rejected the file request (HTTP " + std::to_string(a.status) +
                             "). Check the account, file and configured receiver.");
        return a.body;
    }
}

Json HostedFiles::api(const std::string &path, const Json *payload) const {
    return json_bytes(call(path, payload, false), "Invalid response from the hosted editor.");
}

std::string HostedFiles::download_raw(const std::string &path) const { return call(path, nullptr, true); }

Json HostedFiles::lookup(const std::string &identifier) const {
    if (!lower_hex(identifier, 32)) throw ValueError("Supply the hosted file_id from hosted_list_files.");
    for (const Json &item : python_iterate(api("/api/files"))) {
        const Json *id = python_get(item, "id");
        if (id && id->is_string() && id->get<std::string>() == identifier) return item;
    }
    throw ValueError("The selected file is unavailable to the configured account.");
}

std::pair<std::string, std::string> HostedFiles::download(const std::string &identifier) const {
    const Json item = lookup(identifier);
    const std::string data = download_raw("/api/files/" + identifier);
    if (!python_equals(&python_item(item, "bytes"), static_cast<double>(data.size()))) throw ValueError("Hosted file checksum mismatch.");
    const Json &sha = python_item(item, "sha256");
    if (!sha.is_string() || sha256_hex(data) != sha.get<std::string>()) throw ValueError("Hosted file checksum mismatch.");
    Json upload = Json::object();
    upload["name"] = python_item(item, "name");
    const std::string error = utf8_error(data);
    if (!error.empty()) throw ValueError(error);
    upload["content"] = data;
    const Upload checked = validate_upload(upload);
    return {checked.name, checked.content};
}

Json HostedFiles::send(const std::string &name, const std::string &data, const std::string *destination) const {
    const std::string digest = sha256_hex(data);
    Json payload = Json::object();
    payload["name"] = name;
    payload["content"] = data;
    payload["sha256"] = digest;
    if (destination) payload["destination"] = *destination;
    const Json result = api(destination ? "/api/send" : "/api/files", &payload);
    const auto field = [&](const char *key) { return result.is_object() ? member(result, key) : nullptr; };
    const Json *sha = field("sha256"), *status = field("status"), *bytes = field("bytes"), *stored_name = field("name"), *id = field("id");
    if (!result.is_object() || !sha || *sha != Json(digest) || !status || *status != Json("stored_for_review") ||
        !python_equals(bytes, static_cast<double>(data.size())) || !stored_name || *stored_name != Json(name) ||
        !lower_hex(id ? python_str(*id) : std::string(), 32))
        throw ValueError("Storage receipt could not be verified. Check the destination before retrying.");
    Json receipt = Json::object();
    for (const char *key : {"id", "name", "bytes", "sha256", "status"})
        if (const Json *value = member(result, key)) receipt[key] = *value;
    return receipt;
}

// ---- the tools ------------------------------------------------------------------------------------------------

namespace {

mcp::Result result_of(Json v) {
    mcp::Result r;
    r.value = std::move(v);
    return r;
}

// A workshop artifact as a hosted upload: its checked name and UTF-8 bytes.
Upload artifact_data(const Artifacts &artifacts, const std::string &artifact, const std::string &name) {
    bool ok = false;
    const std::string data = read_limited(fs::u8path(artifacts.file(artifact)), kFileLimit + 1, ok);
    if (!ok) throw ValueError("Unknown workshop artifact.");
    if (data.size() > kFileLimit) throw ValueError("Hosted uploads support up to 8 MiB.");
    if (!is_utf8(data)) throw ValueError("Choose a supported UTF-8 project, artwork or G-code artifact.");
    Json upload = Json::object();
    upload["name"] = name;
    upload["content"] = data;
    return validate_upload(upload);
}

bool approved(mcp::Exchange &exchange, const std::string &message) {
    try {
        return exchange.confirm(message) == "accept";
    } catch (const std::exception &) {
        throw ValueError("Use an elicitation-capable MCP client to confirm this operation, or use the hosted editor.");
    }
}

std::string text_argument(const Json &arguments, const char *name) { return arguments.at(name).get<std::string>(); }

}  // namespace

void add_pendant_tools(mcp::Server &server, std::shared_ptr<const PendantClient> client) {
    server.add(declared("pendant_status", [client](const Json &, mcp::Exchange &) { return result_of(client->status()); }));
    const auto simple = [client](const Json &a, mcp::Exchange &exchange) {
        return result_of(client->action(exchange, text_argument(a, "action"), text_argument(a, "request_id"), text_argument(a, "control_epoch"), Json::object()));
    };
    server.add(declared("pendant_job", simple));
    server.add(declared("pendant_jog", [client](const Json &a, mcp::Exchange &exchange) {
        Json values = Json::object();
        values["axis"] = a.at("axis");
        values["distance"] = a.at("distance");
        values["feed"] = a.at("feed");
        return result_of(client->action(exchange, "jog", text_argument(a, "request_id"), text_argument(a, "control_epoch"), values));
    }));
    server.add(declared("pendant_reference", simple));
}

void add_hosted_tools(mcp::Server &server, std::shared_ptr<const HostedFiles> hosted, const Artifacts &artifacts) {
    const Artifacts *store = &artifacts;
    server.add(declared("hosted_list_files", [hosted](const Json &, mcp::Exchange &) {
        Json out = Json::array();
        for (const Json &item : python_iterate(hosted->api("/api/files"))) {
            Json entry = Json::object();
            for (const char *key : {"id", "name", "bytes", "sha256", "created", "source"}) entry[key] = python_item(item, key);
            out.push_back(entry);
        }
        return result_of(out);
    }));
    server.add(declared("hosted_destinations", [hosted](const Json &, mcp::Exchange &) { return result_of(hosted->api("/api/destinations")); }));
    server.add(declared("hosted_upload_file", [hosted, store](const Json &a, mcp::Exchange &) {
        const Upload upload = artifact_data(*store, text_argument(a, "artifact"), text_argument(a, "name"));
        return result_of(hosted->send(upload.name, upload.content, nullptr));
    }));
    server.add(declared("hosted_download_file", [hosted, store](const Json &a, mcp::Exchange &) {
        const auto file = hosted->download(text_argument(a, "file_id"));
        return result_of(store->create_file(file.first, file.second));
    }));
    server.add(declared("hosted_delete_file", [hosted](const Json &a, mcp::Exchange &exchange) {
        const std::string id = text_argument(a, "file_id");
        const Json item = hosted->lookup(id);
        const std::string name = python_str(python_item(item, "name"));
        const std::string bytes = python_str(python_item(item, "bytes"));
        if (!approved(exchange, "Delete hosted file " + name + " (" + bytes + " bytes)?")) {
            Json out = Json::object();
            out["deleted"] = false;
            return result_of(out);
        }
        const Json nothing = Json::object();
        const Json result = hosted->api("/api/files/" + id + "/delete", &nothing);
        const Json *ok = python_get(result, "ok");
        Json out = Json::object();
        out["deleted"] = ok && ok->is_boolean() && ok->get<bool>();
        out["file_id"] = id;
        return result_of(out);
    }));
    server.add(declared("hosted_send_file", [hosted, store](const Json &a, mcp::Exchange &exchange) {
        const std::string destination = text_argument(a, "destination"), artifact = text_argument(a, "artifact"), name = text_argument(a, "name"),
                          file_id = text_argument(a, "file_id");
        if (file_id.empty() == artifact.empty() || (!file_id.empty() && !name.empty()) || (!artifact.empty() && name.empty()))
            throw ValueError("Choose a hosted file_id, or supply both a workshop artifact and its filename.");
        if (!python_contains(hosted->api("/api/destinations"), destination)) throw ValueError("Choose a receiver name returned by hosted_destinations.");
        std::string file_name, data;
        if (!file_id.empty()) {
            std::tie(file_name, data) = hosted->download(file_id);
        } else {
            const Upload upload = artifact_data(*store, artifact, name);
            file_name = upload.name;
            data = upload.content;
        }
        if (!approved(exchange, "Send " + file_name + " (" + std::to_string(data.size()) + " bytes) to the " + destination + " receiver inbox for review?")) {
            Json out = Json::object();
            out["sent"] = false;
            return result_of(out);
        }
        const Json receipt = hosted->send(file_name, data, &destination);
        Json out = Json::object();
        out["sent"] = true;
        out["destination"] = destination;
        for (auto it = receipt.begin(); it != receipt.end(); ++it) out[it.key()] = it.value();
        return result_of(out);
    }));
}

}  // namespace server
}  // namespace kerfkite
