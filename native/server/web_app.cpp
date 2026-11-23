// The hosted editor's requests. See web_app.hpp.
#include "web_app.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <set>
#include <utility>

#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "os_random.hpp"
#include "static_files.hpp"
#include "step_exchange.hpp"
#include "web_transfer.hpp"

namespace kerfkite {
namespace server {
namespace {

using http::Request;
using http::Response;

const char *const kSessionCookie = "__Host-cnc-session";
const char *const kLoginCookie = "__Host-cnc-login";
const size_t kMaxContent = 10 * 1024 * 1024;  // MAX_CONTENT_LENGTH
const char *const kPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; object-src 'none'; "
    "frame-ancestors 'none'; base-uri 'self'; form-action 'self'";
const char *const kExpired = "Login expired. Save your project locally, then log in again.";

// werkzeug's descriptions of the refusals create_app makes with abort().
const char *description(int status) {
    switch (status) {
        case 400: return "The browser (or proxy) sent a request that this server could not understand.";
        case 403: return "You don't have the permission to access the requested resource. It is either read-protected or not readable by the server.";
        case 404: return "The requested URL was not found on the server. If you entered the URL manually please check your spelling and try again.";
        case 405: return "The method is not allowed for the requested URL.";
        default: return "The request was refused.";
    }
}

Response refused(int status) { return Response::error(status, description(status)); }

std::string ascii_lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string trimmed(const std::string &text) {
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
    return text.substr(a, b - a);
}

std::string header(const Request &request, const char *name) {
    const std::string *value = request.header(name);
    return value ? *value : std::string();
}

// A request's cookie of that name, as werkzeug reads the ones Studio sets: the first of the name, a quoted value
// without its quotes.
std::string cookie(const Request &request, const std::string &name) {
    for (const auto &field : request.headers) {
        if (field.first != "cookie") continue;
        const std::string &text = field.second;
        for (size_t at = 0; at <= text.size();) {
            size_t end = text.find(';', at);
            if (end == std::string::npos) end = text.size();
            const std::string pair = trimmed(text.substr(at, end - at));
            const size_t equals = pair.find('=');
            if (equals != std::string::npos && trimmed(pair.substr(0, equals)) == name) {
                std::string value = trimmed(pair.substr(equals + 1));
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
                return value;
            }
            at = end + 1;
        }
    }
    return std::string();
}

// werkzeug.http.http_date.
std::string http_date(long long seconds) {
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t moment = static_cast<std::time_t>(seconds);
    std::tm t{};
#ifdef _WIN32
    gmtime_s(&t, &moment);
#else
    gmtime_r(&moment, &t);
#endif
    char out[40];
    std::snprintf(out, sizeof out, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[t.tm_wday], t.tm_mday, months[t.tm_mon], t.tm_year + 1900, t.tm_hour,
                  t.tm_min, t.tm_sec);
    return out;
}

// response.set_cookie(name, value, secure=True, httponly=True, samesite='Strict', max_age=...) as werkzeug writes it;
// max_age 0 with the epoch for delete_cookie.
void set_cookie(Response &response, const std::string &name, const std::string &value, long long max_age) {
    const long long expires = max_age ? static_cast<long long>(std::time(nullptr)) + max_age : 0;
    response.headers.emplace_back("Set-Cookie", name + "=" + value + "; Expires=" + http_date(expires) + "; Max-Age=" + std::to_string(max_age) +
                                                    "; Secure; HttpOnly; Path=/; SameSite=Strict");
}

// markupsafe.escape.
std::string html_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&#34;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

Response html(const std::string &text, int status = 200) {
    Response r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "text/html; charset=utf-8");
    r.body = text;
    return r;
}

// werkzeug.utils.redirect(location).
Response redirect(const std::string &location) {
    const std::string shown = html_escape(location);
    Response r = html("<!doctype html>\n<html lang=en>\n<title>Redirecting...</title>\n<h1>Redirecting...</h1>\n"
                      "<p>You should be redirected automatically to the target URL: <a href=\"" +
                          shown + "\">" + shown + "</a>. If not, click the link.\n",
                      302);
    r.headers.emplace_back("Location", location);
    return r;
}

// Bytes as UTF-8 text, each ill-formed part replaced by U+FFFD as Python's decode(errors='replace') does.
std::string utf8_replaced(const std::string &bytes) {
    std::string out;
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        size_t need = 0;
        unsigned char low = 0x80, high = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) need = 1;
        else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            if (c == 0xe0) low = 0xa0;
            if (c == 0xed) high = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            if (c == 0xf0) low = 0x90;
            if (c == 0xf4) high = 0x8f;
        }
        size_t got = 0;
        if (need) {
            // The longest start of a valid sequence is one replacement; a stray byte is one of its own.
            while (got < need && i + 1 + got < bytes.size()) {
                const unsigned char next = static_cast<unsigned char>(bytes[i + 1 + got]);
                const unsigned char lo = got == 0 ? low : 0x80, hi = got == 0 ? high : 0xbf;
                if (next < lo || next > hi) break;
                ++got;
            }
        }
        if (need && got == need) {
            out.append(bytes, i, need + 1);
            i += need + 1;
        } else {
            out += "\xef\xbf\xbd";
            i += 1 + got;
        }
    }
    return out;
}

// A field of an application/x-www-form-urlencoded body, as request.form.get reads it: the first of the name, '+'
// as a space, percent escapes decoded and the text read as UTF-8.
bool form_field(const Request &request, const std::string &name, std::string &value) {
    if (request.media_type() != "application/x-www-form-urlencoded") return false;
    const auto decode = [](const std::string &text) {
        std::string bytes;
        for (size_t i = 0; i < text.size(); ++i) {
            const auto hex = [](char c) {
                return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            };
            if (text[i] == '+') bytes += ' ';
            else if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
                bytes += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
                i += 2;
            } else bytes += text[i];
        }
        return utf8_replaced(bytes);
    };
    const std::string &body = request.body;
    for (size_t at = 0; at <= body.size();) {
        size_t end = body.find('&', at);
        if (end == std::string::npos) end = body.size();
        const std::string pair = body.substr(at, end - at);
        if (!pair.empty()) {
            const size_t equals = pair.find('=');
            if (decode(pair.substr(0, equals)) == name) {
                value = equals == std::string::npos ? std::string() : decode(pair.substr(equals + 1));
                return true;
            }
        }
        at = end + 1;
    }
    return false;
}

// One segment of a path, as werkzeug's default converter matches it: not empty, without '/'.
bool segment(const std::string &text) { return !text.empty() && text.find('/') == std::string::npos; }

// The identifier of /api/files/<identifier>[suffix], or false.
bool file_route(const std::string &path, const std::string &suffix, std::string &identifier) {
    const std::string prefix = "/api/files/";
    if (path.compare(0, prefix.size(), prefix) != 0 || path.size() < prefix.size() + suffix.size()) return false;
    if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    identifier = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    return segment(identifier);
}

// quote_header_value: a token as it is, anything else in double quotes.
std::string header_value(const std::string &value) {
    const bool token = !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos;
    });
    if (token) return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

Response empty(int status) {
    Response r = html(std::string(), status);
    return r;
}

const char *const kLoginPage =
    "<!doctype html><html><meta name=\"viewport\" content=\"width=device-width\"><title>CNC Studio login</title><body><h1>KerfKite "
    "Studio</h1><form method=\"post\"><input type=\"hidden\" name=\"csrf\" value=\"%CSRF%\"><label>Username <input name=\"username\" "
    "autocomplete=\"username\" required maxlength=\"64\"></label><label>Password <input name=\"password\" type=\"password\" "
    "autocomplete=\"current-password\" required maxlength=\"256\"></label><button>Log in</button></form></body></html>";

}  // namespace

std::string web_origin(const std::string &public_url, std::string &host) {
    const char *refusal = "Public URL must be an HTTPS origin";
    std::string url = public_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    const UrlParts parts = url_split(url);
    if (parts.scheme != "https" || !parts.has_hostname || !parts.path.empty() || !parts.query.empty() || !parts.fragment.empty() || parts.has_user)
        throw ValueError(refusal);
    std::string port;
    if (parts.has_port) {
        if (parts.port.size() > 5 || !std::all_of(parts.port.begin(), parts.port.end(), [](char c) { return c >= '0' && c <= '9'; })) throw ValueError(refusal);
        const int number = std::stoi(parts.port);
        if (number < 1 || number > 65535) throw ValueError(refusal);
        if (number != 443) port = ":" + std::to_string(number);
    }
    const bool bracketed = parts.hostname.find(':') != std::string::npos;
    host = (bracketed ? "[" + parts.hostname + "]" : parts.hostname) + port;
    return "https://" + host;
}

WebApp::WebApp(WebStore &store, WebOptions options, Json destinations, std::vector<std::string> names)
    : store_(store), options_(std::move(options)), destinations_(std::move(destinations)), names_(std::move(names)) {
    origin_ = web_origin(options_.public_url, host_);
    if (!store_.has_users()) throw ValueError("Create a user first; there are no default credentials");
    if (!destinations_.is_object()) destinations_ = Json::object();
    for (const auto &entry : destinations_.items()) validate_destination(entry.value());
    step_ = std::make_shared<StepExchange>(options_.step_program);
    use_step_mesher(step_);
    add_design_routes(*this);
    add_cam_routes(*this, true);
}

void WebApp::add(const std::string &method, const std::string &path, Route route) { routes_[method + " " + path] = std::move(route); }

size_t WebApp::body_limit(const Request &) const { return kMaxContent; }

std::string WebApp::allowed(const std::string &path) const {
    // Every path is also the editor's file route (GET), which Flask answers HEAD and OPTIONS for as well.
    std::set<std::string> methods = {"GET", "HEAD", "OPTIONS"};
    for (const auto &route : routes_) {
        const size_t space = route.first.find(' ');
        if (route.first.compare(space + 1, std::string::npos, path) == 0) methods.insert(route.first.substr(0, space));
    }
    std::string identifier;
    static const std::set<std::string> posts = {"/login", "/api/logout", "/api/files", "/api/receive", "/api/send"};
    if (posts.count(path) || file_route(path, "/delete", identifier)) methods.insert("POST");
    std::string out;
    for (const std::string &m : methods) out += (out.empty() ? "" : ", ") + m;
    return out;
}

Response WebApp::dispatch(const Request &request, const WebSession &session, const std::string &user) {
    const std::string &path = request.path, &method = request.method;
    const bool get = method == "GET" || method == "HEAD", post = method == "POST";
    std::string identifier;
    if (get && path == "/login") {
        std::string csrf = cookie(request, kLoginCookie);
        const bool usable = !csrf.empty() && csrf.size() <= 128 && std::all_of(csrf.begin(), csrf.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
        });
        if (!usable) csrf = token_urlsafe(32);
        std::string page = kLoginPage;
        page.replace(page.find("%CSRF%"), 6, html_escape(csrf));
        Response r = html(page);
        set_cookie(r, kLoginCookie, csrf, 600);
        return r;
    }
    if (post && path == "/login") {
        const std::string expected = cookie(request, kLoginCookie);
        std::string given, name, password;
        form_field(request, "csrf", given);
        if (expected.empty() || header(request, "origin") != origin_ || !same_secret(expected, given))
            return html("<!doctype html><html><meta name=\"viewport\" content=\"width=device-width\"><title>CNC Studio login</title><body><h1>KerfKite "
                        "Studio</h1><p>This login form has expired or did not come from this site.</p><p><a href=\"/login\">Log in</a></p></body></html>",
                        403);
        form_field(request, "username", name);
        form_field(request, "password", password);
        const LoginResult result = store_.check_login(name, password, request.peer);
        if (!result.ok) return result.limited ? html("Login failed. Try again later.", 429) : html("Invalid username or password.", 401);
        Response r = redirect("/");
        set_cookie(r, kSessionCookie, result.session.sid, 8 * 3600);
        set_cookie(r, kLoginCookie, std::string(), 0);
        return r;
    }
    if (post && path == "/api/logout") {
        store_.logout(cookie(request, kSessionCookie));
        Json ok = Json::object();
        ok["ok"] = true;
        Response r = Response::json(ok.dump());
        set_cookie(r, kSessionCookie, std::string(), 0);
        return r;
    }
    if (get && path == "/") return static_file(options_.static_dir, "index.html");
    if (get && path == "/api/session") {
        Json out = Json::object();
        out["token"] = session.csrf;
        out["username"] = user;
        out["web_mode"] = true;
        out["depth_paths"] = true;
        out["advanced_cam"] = true;
        out["holding_tabs"] = true;
        out["laser_patterns"] = true;
        out["router_entries"] = true;
        out["stock_wire_v1"] = true;
        out["pcb_import"] = true;
        out["relief_map"] = 1024;
        return Response::json(out.dump());
    }
    if (get && path == "/api/files") return Response::json(store_.files(user).dump());
    if (post && path == "/api/files") {
        const Upload upload = validate_upload(strict_json(request));
        return Response::json(store_.put_file(user, upload.name, upload.content, "web").dump());
    }
    if (get && file_route(path, "", identifier)) {
        WebFile record;
        std::string content;
        if (!store_.file(user, identifier, record, content)) return refused(404);
        Response r;
        r.headers.emplace_back("Content-Type", "application/octet-stream");
        r.headers.emplace_back("Content-Disposition", "attachment; filename=" + header_value(record.name));
        r.body = std::move(content);
        return r;
    }
    if (post && file_route(path, "/delete", identifier)) {
        store_.delete_file(user, identifier);
        Json ok = Json::object();
        ok["ok"] = true;
        return Response::json(ok.dump());
    }
    if (post && path == "/api/receive") {
        const Upload upload = validate_upload(strict_json(request));
        return Response::json(store_.put_file(user, upload.name, upload.content, "upload").dump());
    }
    if (get && path == "/api/destinations") {
        Json out = Json::array();
        for (const std::string &name : names_) out.push_back(name);
        return Response::json(out.dump());
    }
    if (post && path == "/api/send") {
        const Json data = strict_json(request);
        const Upload upload = validate_upload(data);
        const auto found = data.find("destination");
        const Json *name = found == data.end() ? nullptr : &*found;
        if (!name || !name->is_string() || !destinations_.contains(name->get<std::string>())) throw ValueError("Choose an operator-configured receiver");
        return Response::json(send_to_receiver(destinations_.at(name->get<std::string>()), upload, options_.relay).dump());
    }
    const auto route = routes_.find((get ? std::string("GET") : method) + " " + path);
    if (route != routes_.end()) return route->second(request);
    if (get) {
        // The editor's files, as the asset route and Flask's /static route send them.
        if (path.compare(0, 8, "/static/") == 0) return static_file(options_.static_dir, path.substr(8));
        return static_file(options_.static_dir, path.substr(1));
    }
    if (method == "OPTIONS") {
        Response r = empty(200);
        r.headers.emplace_back("Allow", allowed(path));
        return r;
    }
    Response r = refused(405);
    r.headers.emplace_back("Allow", allowed(path));
    return r;
}

Response WebApp::protect(const Request &request, WebSession &session, std::string &user, bool &answered) {
    answered = true;
    std::string host = ascii_lower(header(request, "host"));
    if (host.size() > 4 && host.compare(host.size() - 4, 4, ":443") == 0) host.resize(host.size() - 4);
    if (host != host_) return Response::error(400, "Use this server at its public address, " + origin_);
    const std::string &path = request.path;
    if (path == "/api/receive") {
        if (request.method == "OPTIONS") {
            if (options_.allow_file_upload && header(request, "origin") == "null") return empty(204);
            return refused(403);
        }
        const std::string authorization = header(request, "authorization");
        user = authorization.compare(0, 7, "Bearer ") == 0 ? store_.token_user(authorization.substr(7)) : std::string();
        if (user.empty()) return Response::error(401, "Valid upload token required");
        answered = false;
        return Response();
    }
    if (path == "/favicon.ico") return empty(204);
    if (path == "/login") {
        answered = false;
        return Response();
    }
    if (!store_.session(cookie(request, kSessionCookie), session)) {
        if (path.compare(0, 5, "/api/") == 0) return Response::error(401, kExpired);
        return redirect("/login");
    }
    user = session.user;
    if (request.method != "GET" && request.method != "HEAD" && request.method != "OPTIONS") {
        if (header(request, "origin") != origin_ || !same_secret(header(request, "x-studio-token"), session.csrf)) return refused(403);
    }
    answered = false;
    return Response();
}

Response WebApp::handle(const Request &request) {
    Response response;
    try {
        WebSession session;
        std::string user;
        bool answered = false;
        response = protect(request, session, user, answered);
        if (!answered) response = dispatch(request, session, user);
    } catch (const HttpError &e) {
        response = Response::error(e.status, e.what());
    } catch (const ValueError &e) {
        response = Response::error(400, e.what());
    } catch (const RuntimeError &e) {
        response = Response::error(400, e.what());
    } catch (const NotPorted &e) {
        response = Response::error(400, e.what());
    } catch (const std::bad_alloc &) {
        response = Response::error(500, "The request needs more memory than is available");
    } catch (const std::exception &e) {
        response = Response::error(500, std::string("Internal error: ") + e.what());
    }
    // after_request: on every answer.
    response.set("Cache-Control", "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.set("Referrer-Policy", "same-origin");
    response.set("Strict-Transport-Security", "max-age=31536000");
    response.set("Content-Security-Policy", kPolicy);
    if (request.path == "/api/receive" && options_.allow_file_upload && header(request, "origin") == "null") {
        response.set("Access-Control-Allow-Origin", "null");
        response.set("Access-Control-Allow-Methods", "POST");
        response.set("Access-Control-Allow-Headers", "Authorization, Content-Type");
        response.set("Vary", "Origin");
    }
    return response;
}

}  // namespace server
}  // namespace kerfkite
