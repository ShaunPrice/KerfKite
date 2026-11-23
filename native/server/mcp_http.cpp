// The assistant server's HTTP transport. See mcp_http.hpp.
#include "mcp_http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ip_address.hpp"
#include "python_text.hpp"

namespace kerfkite {
namespace server {
namespace mcp {

namespace oauth_bytes = oauth_detail;

namespace {

const char *const kMetadataPath = "/.well-known/oauth-protected-resource/mcp";
const char *const kSupported = "2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25";
const char *const kInvalidToken =
    "Authentication failed. The provided bearer token is invalid, expired, or no longer recognized by the server. To resolve: clear authentication "
    "tokens in your MCP client and reconnect. Your client should automatically re-register and obtain new tokens.";

// ---- Python's view of a header: each byte a character (ISO 8859-1, as Starlette decodes them) --------------

bool latin_space(unsigned char c) { return detail::python_space(c); }

std::string py_strip(const std::string &text) {
    size_t a = 0, b = text.size();
    while (a < b && latin_space(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && latin_space(static_cast<unsigned char>(text[b - 1]))) --b;
    return text.substr(a, b - a);
}

std::string py_lower(std::string text) {
    for (char &c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 0xc0 && u <= 0xde && u != 0xd7)) c = static_cast<char>(u + 0x20);
    }
    return text;
}

std::vector<std::string> py_split(const std::string &text, char by) {
    std::vector<std::string> out;
    for (size_t start = 0;;) {
        const size_t at = text.find(by, start);
        out.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

const std::string *first(const http::Request &request, const char *name) { return request.header(name); }

// fnmatch.fnmatchcase of the patterns the guard can hold: '*', '?' and bracketed classes ('!' for none of them,
// ranges with '-'), anything else itself; a '[' without its ']' is itself.
bool fnmatch_case(const std::string &name, const std::string &pattern, size_t n = 0, size_t p = 0) {
    while (p < pattern.size()) {
        const char c = pattern[p];
        if (c == '*') {
            while (p < pattern.size() && pattern[p] == '*') ++p;
            for (size_t k = n; k <= name.size(); ++k)
                if (fnmatch_case(name, pattern, k, p)) return true;
            return false;
        }
        if (n >= name.size()) return false;
        if (c == '?') {
            ++n, ++p;
            continue;
        }
        if (c == '[') {
            size_t j = p + 1;
            if (j < pattern.size() && pattern[j] == '!') ++j;
            if (j < pattern.size() && pattern[j] == ']') ++j;
            while (j < pattern.size() && pattern[j] != ']') ++j;
            if (j < pattern.size()) {
                std::string set = pattern.substr(p + 1, j - p - 1);
                bool negate = !set.empty() && set[0] == '!';
                if (negate) set.erase(0, 1);
                bool hit = false;
                for (size_t k = 0; k < set.size(); ++k) {
                    if (k + 2 < set.size() && set[k + 1] == '-') {
                        hit = hit || (name[n] >= set[k] && name[n] <= set[k + 2]);
                        k += 2;
                    } else {
                        hit = hit || name[n] == set[k];
                    }
                }
                if (set.empty() && !negate) return false;
                if (hit == negate) return false;
                ++n;
                p = j + 1;
                continue;
            }
        }
        if (name[n] != c) return false;
        ++n, ++p;
    }
    return n == name.size();
}

// ---- urllib.parse.urlsplit, for the Origin header ---------------------------------------------------------

struct Split {
    std::string scheme, netloc, path, query, fragment;
};

bool urlsplit(std::string url, Split &out) {
    out = Split();
    size_t start = 0;
    while (start < url.size() && static_cast<unsigned char>(url[start]) <= 0x20) ++start;
    std::string text;
    for (size_t i = start; i < url.size(); ++i)
        if (url[i] != '\t' && url[i] != '\r' && url[i] != '\n') text += url[i];
    const size_t colon = text.find(':');
    if (colon != std::string::npos && colon > 0 && std::isalpha(static_cast<unsigned char>(text[0])) &&
        std::all_of(text.begin(), text.begin() + static_cast<long>(colon),
                    [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.'; })) {
        out.scheme = py_lower(text.substr(0, colon));
        text = text.substr(colon + 1);
    }
    if (text.compare(0, 2, "//") == 0) {
        size_t end = 2;
        while (end < text.size() && text[end] != '/' && text[end] != '?' && text[end] != '#') ++end;
        out.netloc = text.substr(2, end - 2);
        text = text.substr(end);
        const bool open = out.netloc.find('[') != std::string::npos, close = out.netloc.find(']') != std::string::npos;
        if (open != close) return false;  // "Invalid IPv6 URL"
        if (open) {
            // _check_bracketed_netloc: nothing before the bracket, only a port after it, and an IPv6 address or
            // IPvFuture between.
            std::string hostport = out.netloc.substr(out.netloc.rfind('@') == std::string::npos ? 0 : out.netloc.rfind('@') + 1);
            const size_t bracket = hostport.find('[');
            if (bracket == std::string::npos) {
                hostport = hostport.substr(0, hostport.find(':'));
            } else {
                if (bracket != 0) return false;
                const size_t shut = hostport.find(']');
                const std::string after = shut == std::string::npos ? std::string() : hostport.substr(shut + 1);
                if (!after.empty() && after[0] != ':') return false;
                hostport = hostport.substr(1, shut == std::string::npos ? std::string::npos : shut - 1);
            }
            if (!hostport.empty() && hostport[0] == 'v') {
                size_t k = 1;
                while (k < hostport.size() && std::isxdigit(static_cast<unsigned char>(hostport[k]))) ++k;
                if (k == 1 || k + 1 >= hostport.size() || hostport[k] != '.') return false;
            } else {
                IpAddress ip;
                if (!parse_ip_address(hostport, ip) || !ip.v6) return false;
            }
        }
    }
    const size_t hash = text.find('#');
    if (hash != std::string::npos) {
        out.fragment = text.substr(hash + 1);
        text = text.substr(0, hash);
    }
    const size_t question = text.find('?');
    if (question != std::string::npos) {
        out.query = text.substr(question + 1);
        text = text.substr(0, question);
    }
    out.path = text;
    return true;
}

// SplitResult.hostname and .port (false where .port raises).
std::string split_hostname(const Split &s) {
    std::string info = s.netloc.substr(s.netloc.rfind('@') == std::string::npos ? 0 : s.netloc.rfind('@') + 1);
    std::string host;
    const size_t bracket = info.find('[');
    if (bracket != std::string::npos) host = info.substr(bracket + 1, info.find(']', bracket) - bracket - 1);
    else host = info.substr(0, info.find(':'));
    return py_lower(host);
}

bool split_port(const Split &s, long &port, bool &given) {
    std::string info = s.netloc.substr(s.netloc.rfind('@') == std::string::npos ? 0 : s.netloc.rfind('@') + 1);
    std::string text;
    const size_t bracket = info.find('[');
    if (bracket != std::string::npos) {
        const size_t shut = info.find(']', bracket);
        const std::string after = shut == std::string::npos ? std::string() : info.substr(shut + 1);
        const size_t colon = after.find(':');
        text = colon == std::string::npos ? std::string() : after.substr(colon + 1);
    } else {
        const size_t colon = info.find(':');
        text = colon == std::string::npos ? std::string() : info.substr(colon + 1);
    }
    given = !text.empty();
    if (!given) return true;
    if (!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    const size_t digits = text.find_first_not_of('0');
    if (digits != std::string::npos && text.size() - digits > 5) return false;
    port = digits == std::string::npos ? 0 : std::stol(text.substr(digits));
    return port <= 65535;
}

// ---- FastMCP's HostOriginGuardMiddleware ----------------------------------------------------------------

std::string normalize_host(const std::string &given) {
    const std::string host = py_lower(py_strip(given));
    if (host.empty()) return host;
    if (host[0] == '[') {
        const size_t end = host.find(']');
        return end == std::string::npos ? host : host.substr(1, end - 1);
    }
    if (std::count(host.begin(), host.end(), ':') == 1) return host.substr(0, host.rfind(':'));
    return host;
}

bool loopback_host(const std::string &host) {
    const std::string h = normalize_host(host);
    if (h == "localhost") return true;
    IpAddress ip;
    return parse_ip_address(h, ip) && ip_is_loopback(ip);
}

std::string normalize_origin(const std::string &given) {
    std::string origin = py_strip(given);
    while (!origin.empty() && origin.back() == '/') origin.pop_back();
    Split s;
    long port = 0;
    bool has_port = false;
    if (!urlsplit(origin, s) || !split_port(s, port, has_port)) return py_lower(origin);
    const std::string hostname = split_hostname(s);
    if (s.scheme.empty() || hostname.empty()) return py_lower(origin);
    if (!s.path.empty() || !s.query.empty() || !s.fragment.empty()) return py_lower(origin);
    std::string host = normalize_host(hostname);
    if (host.find(':') != std::string::npos && host[0] != '[') host = "[" + host + "]";
    if (!has_port) {
        if (s.scheme == "http") port = 80, has_port = true;
        else if (s.scheme == "https") port = 443, has_port = true;
    }
    return s.scheme + "://" + host + (has_port ? ":" + std::to_string(port) : std::string());
}

std::string origin_host(const std::string &origin) {
    Split s;
    return urlsplit(origin, s) ? split_hostname(s) : std::string();
}

http::Response plain(int status, const std::string &text) {
    http::Response r;
    r.status = status;
    r.headers.emplace_back("content-type", "text/plain; charset=utf-8");
    r.body = text;
    return r;
}

http::Response untyped(int status, const std::string &text) {
    http::Response r;
    r.status = status;
    r.untyped = true;
    r.body = text;
    return r;
}

// The SDK transport's error answer: {"jsonrpc": "2.0", "id": "server-error", "error": {...}}.
http::Response transport_error(int status, int code, const std::string &message) {
    return http::Response::json("{\"jsonrpc\":\"2.0\",\"id\":\"server-error\",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + Json(message).dump() + "}}",
                                status);
}

// Python's json.dumps of a str (ensure_ascii).
std::string py_json_text(const std::string &text) {
    std::string out = "\"";
    std::vector<uint32_t> points;
    detail::python_utf8(reinterpret_cast<const unsigned char *>(text.data()), text.size(), points);
    for (const uint32_t c : points) {
        char buffer[16];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
        else if (c < 0x10000) std::snprintf(buffer, sizeof buffer, "\\u%04x", c), out += buffer;
        else std::snprintf(buffer, sizeof buffer, "\\u%04x\\u%04x", 0xd800 + ((c - 0x10000) >> 10), 0xdc00 + ((c - 0x10000) & 0x3ff)), out += buffer;
    }
    return out + "\"";
}

// Starlette's CORSMiddleware with allow_origins="*", methods GET and OPTIONS and the mcp-protocol-version header.
http::Response metadata(const TokenVerifier &verifier, const http::Request &request) {
    const RemoteAuth &auth = verifier.auth();
    const std::string *origin = first(request, "origin");
    if (origin && request.method == "OPTIONS" && first(request, "access-control-request-method")) {
        std::vector<std::string> failures;
        const std::string &method = *first(request, "access-control-request-method");
        if (method != "GET" && method != "OPTIONS") failures.emplace_back("method");
        if (const std::string *asked = first(request, "access-control-request-headers"))
            for (const std::string &name : py_split(py_lower(*asked), ',')) {
                const std::string h = py_strip(name);
                if (h != "accept" && h != "accept-language" && h != "content-language" && h != "content-type" && h != "mcp-protocol-version") {
                    failures.emplace_back("headers");
                    break;
                }
            }
        if (first(request, "access-control-request-private-network")) failures.emplace_back("private-network");
        std::string text = "OK";
        if (!failures.empty()) {
            text = "Disallowed CORS ";
            for (size_t i = 0; i < failures.size(); ++i) text += (i ? ", " : "") + failures[i];
        }
        http::Response r = plain(failures.empty() ? 200 : 400, text);
        r.headers.emplace_back("vary", "Origin, Access-Control-Request-Method, Access-Control-Request-Headers, Access-Control-Request-Private-Network");
        r.headers.emplace_back("access-control-allow-origin", "*");
        r.headers.emplace_back("access-control-allow-methods", "GET, OPTIONS");
        r.headers.emplace_back("access-control-max-age", "600");
        r.headers.emplace_back("access-control-allow-headers", "Accept, Accept-Language, Content-Language, Content-Type, mcp-protocol-version");
        return r;
    }
    http::Response r = http::Response::json("{\"resource\":" + Json(auth.audience()).dump() + ",\"authorization_servers\":[" + Json(auth.issuer_url).dump() +
                                            "],\"scopes_supported\":[\"kerfkite:workshop\"],\"bearer_methods_supported\":[\"header\"],\"resource_name\":\"KerfKite Workshop\"}");
    r.headers.emplace_back("cache-control", "public, max-age=3600");
    if (origin) r.headers.emplace_back("access-control-allow-origin", "*");
    r.headers.emplace_back("vary", "Origin");
    return r;
}

// Starlette's parse_host_header: the Host as a URL's authority, or false where its pattern refuses it.
bool host_authority(const std::string &host, std::string &authority) {
    const auto in = [](char c, const char *set) {
        return std::isalnum(static_cast<unsigned char>(c)) || (c != 0 && std::strchr(set, c) != nullptr);
    };
    size_t end = 0;
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos) return false;
        const std::string inner = host.substr(1, close - 1);
        if (!inner.empty() && inner[0] == 'v') {
            size_t k = 1;
            while (k < inner.size() && std::isxdigit(static_cast<unsigned char>(inner[k]))) ++k;
            if (k == 1 || k >= inner.size() || inner[k] != '.' || k + 1 >= inner.size()) return false;
            for (size_t i = k + 1; i < inner.size(); ++i)
                if (!in(inner[i], "._~!$&'()*+,;=:-")) return false;
        } else {
            const size_t colon = inner.find(':');
            if (colon == std::string::npos || colon + 1 >= inner.size()) return false;
            for (size_t i = 0; i < inner.size(); ++i)
                if (!(std::isxdigit(static_cast<unsigned char>(inner[i])) || (i >= colon && (inner[i] == ':' || inner[i] == '.')))) return false;
            IpAddress ip;
            if (!parse_ip_address(inner, ip) || !ip.v6) return false;
        }
        end = close + 1;
    } else {
        while (end < host.size() && in(host[end], "._~%!$&'()*+,;=-")) ++end;
        if (end == 0) return false;
    }
    if (end < host.size()) {
        if (host[end] != ':' || end + 1 >= host.size()) return false;
        const std::string port = host.substr(end + 1);
        if (!std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        const size_t digits = port.find_first_not_of('0');
        if (digits != std::string::npos && (port.size() - digits > 5 || std::stol(port.substr(digits)) > 65535)) return false;
    }
    authority = host;
    return true;
}

// The SDK's StreamableHTTPServerTransport without sessions, for a POST that passed authentication.
http::Response post(Server &server, const http::Request &request) {
    // TransportSecurityMiddleware.validate_request, which the SDK always makes: a Content-Type that starts with
    // application/json.
    const std::string *media = first(request, "content-type");
    if (!media || media->empty() || py_lower(*media).compare(0, 16, "application/json") != 0) return untyped(400, "Invalid Content-Type header");
    // Accept: both media types, each a prefix of an entry.
    bool json = false, events = false;
    for (const std::string &entry : py_split(first(request, "accept") ? *first(request, "accept") : std::string(), ',')) {
        const std::string type = py_strip(entry);
        json = json || type.compare(0, 16, "application/json") == 0;
        events = events || type.compare(0, 17, "text/event-stream") == 0;
    }
    if (!(json && events)) return transport_error(406, -32600, "Not Acceptable: Client must accept both application/json and text/event-stream");
    bool typed = false;
    const std::string content = first(request, "content-type") ? *first(request, "content-type") : std::string();
    for (const std::string &part : py_split(py_split(content, ';')[0], ',')) typed = typed || py_strip(part) == "application/json";
    if (!typed) return transport_error(415, -32600, "Unsupported Media Type: Content-Type must be application/json");
    // json.loads of the body: bytes that do not decode are an error of the server (500), text that is no JSON a
    // parse error (own rule: in the server's own words).
    Json message;
    if (!oauth_bytes::json_from_bytes(request.body, message)) {
        if (oauth_detail::decodes(request.body)) return transport_error(400, -32700, "Parse error: the body is not JSON");
        return transport_error(500, -32603, "Error handling POST request");
    }
    const Kind kind = classify(message);
    if (kind == Kind::invalid) return transport_error(400, -32602, "Validation error: the body is not a JSON-RPC message");
    const bool initialize = kind == Kind::request && message.at("method").get<std::string>() == "initialize";
    if (!initialize) {
        const std::string *version = first(request, "mcp-protocol-version");
        if (version) {
            bool known = false;
            for (const char *v : {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}) known = known || *version == v;
            if (!known) return transport_error(400, -32600, "Bad Request: Unsupported protocol version: " + *version + ". Supported versions: " + kSupported);
        }
    }
    if (kind != Kind::request) {
        http::Response accepted = http::Response::json(std::string(), 202);
        return accepted;
    }
    NoExchange exchange;  // own rule: a confirmation cannot be answered without a session
    http::Response r;
    r.headers.emplace_back("cache-control", "no-cache, no-transform");
    r.headers.emplace_back("content-type", "text/event-stream");
    r.headers.emplace_back("x-accel-buffering", "no");
    r.body = "event: message\r\ndata: " + server.answer(message, exchange, false) + "\r\n\r\n";
    return r;
}

}  // namespace

http::Response serve_http(Server &server, TokenVerifier &verifier, const http::Request &request, int port) {
    const RemoteAuth &auth = verifier.auth();
    // uvicorn's ProxyHeadersMiddleware trusts the loopback client: its X-Forwarded-Proto (the last one, as
    // dict(scope["headers"]) keeps it) names the scheme when it is one uvicorn knows.
    std::string scheme = "http";
    const std::string *forwarded = nullptr;
    for (const auto &h : request.headers)
        if (h.first == "x-forwarded-proto") forwarded = &h.second;
    if (forwarded) {
        const std::string proto = py_strip(*forwarded);
        if (proto == "http" || proto == "https" || proto == "ws" || proto == "wss") scheme = proto;
    }
    // h11 under uvicorn: an HTTP/1.1 request needs exactly one Host header.
    size_t hosts = 0;
    for (const auto &h : request.headers) hosts += h.first == "host";
    if (hosts > 1 || (hosts == 0 && request.version == "HTTP/1.1")) return plain(400, "Invalid HTTP request received.");
    // The guard, strict: the Host one of the allowed ones, and an Origin the public origin, a loopback origin for a
    // loopback host, or the request's own.
    const std::string host = first(request, "host") ? *first(request, "host") : std::string();
    const std::string normalized = normalize_host(host);
    bool allowed = false;
    for (const std::string &pattern : {std::string("127.0.0.1"), std::string("localhost"), std::string("::1"), normalize_host(auth.host), std::string("127.0.0.1")})
        allowed = allowed || pattern == "*" || fnmatch_case(normalized, pattern);
    if (!allowed) return untyped(421, "Misdirected Request");
    const std::string *origin = first(request, "origin");
    if (origin && !origin->empty()) {
        const std::string request_origin = normalize_origin(scheme + "://" + host);
        const std::string pattern = normalize_origin(auth.base_url);
        bool ok = pattern == "*" || fnmatch_case(normalize_origin(*origin), pattern);
        ok = ok || (loopback_host(origin_host(*origin)) && loopback_host(host));
        ok = ok || normalize_origin(*origin) == request_origin;
        if (!ok) return untyped(403, "Forbidden Origin");
    }
    // AuthenticationMiddleware with the bearer backend, for every request that passes the guard.
    bool authenticated = false;
    if (const std::string *authorization = first(request, "authorization")) {
        if (!authorization->empty() && authorization->size() >= 7 && py_lower(authorization->substr(0, 7)) == "bearer ")
            authenticated = verifier.verify(authorization->substr(7));
    }
    // Starlette's router.
    const std::string &path = request.path;
    if (path == kMetadataPath) {
        if (request.method != "GET" && request.method != "HEAD" && request.method != "OPTIONS") {
            http::Response r = plain(405, "Method Not Allowed");
            r.headers.emplace_back("allow", "GET, HEAD, OPTIONS");
            return r;
        }
        return metadata(verifier, request);
    }
    if (path == "/mcp") {
        if (request.method != "POST" && request.method != "DELETE") {
            http::Response r = plain(405, "Method Not Allowed");
            r.headers.emplace_back("allow", "POST, DELETE");
            return r;
        }
        // FastMCP's RequireAuthMiddleware: no Authorization header at all, or one that did not authenticate.
        const std::string challenge = "resource_metadata=\"" + auth.metadata_url() + "\"";
        if (!first(request, "authorization")) {
            http::Response r;
            r.status = 401;
            r.untyped = true;
            r.headers.emplace_back("www-authenticate", "Bearer " + challenge);
            return r;
        }
        if (!authenticated) {
            http::Response r = http::Response::json(std::string("{\"error\": \"invalid_token\", \"error_description\": ") + py_json_text(kInvalidToken) + "}", 401);
            r.headers.emplace_back("www-authenticate", std::string("Bearer error=\"invalid_token\", error_description=\"") + kInvalidToken + "\", " + challenge);
            return r;
        }
        if (request.method == "DELETE") return transport_error(405, -32600, "Method Not Allowed: Session termination not supported");
        return post(server, request);
    }
    // redirect_slashes: a path with a final '/' whose route exists without it.
    if (path != "/" && !path.empty() && path.back() == '/') {
        std::string stripped = path;
        while (!stripped.empty() && stripped.back() == '/') stripped.pop_back();
        if (stripped == "/mcp" || stripped == kMetadataPath) {
            // Starlette's URL from the scope: the Host where its pattern takes it, else the server's own address.
            std::string authority;
            if (!host_authority(host, authority)) authority = "127.0.0.1" + ((scheme == "https" || scheme == "wss") ? (port == 443 ? "" : ":" + std::to_string(port))
                                                                                                                       : (port == 80 ? "" : ":" + std::to_string(port)));
            const std::string location = scheme + "://" + authority + stripped + (request.query.empty() ? "" : "?" + request.query);
            // urllib.parse.quote(url, safe=":/%#?=@[]!$&'()*+,;")
            std::string quoted;
            for (const unsigned char c : location) {
                if (std::isalnum(c) || std::strchr("_.-~:/%#?=@[]!$&'()*+,;", c)) {
                    quoted += static_cast<char>(c);
                } else {
                    char buffer[4];
                    std::snprintf(buffer, sizeof buffer, "%%%02X", c);
                    quoted += buffer;
                }
            }
            http::Response r;
            r.status = 307;
            r.untyped = true;
            r.headers.emplace_back("location", quoted);
            return r;
        }
    }
    return plain(404, "Not Found");
}

}  // namespace mcp
}  // namespace server
}  // namespace kerfkite
