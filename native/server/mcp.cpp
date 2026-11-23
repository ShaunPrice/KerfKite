// The Model Context Protocol server. See mcp.hpp.
#include "mcp.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <stdexcept>
#include <thread>

#include "kerfkite/decimal.hpp"
#include "kerfkite/errors.hpp"
#include "python_text.hpp"

namespace kerfkite {
namespace server {
namespace mcp {
namespace {

const char *const kLatest = "2025-11-25";
const std::set<std::string> kVersions = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};

std::string text(const std::string &value) { return Json(value).dump(); }

std::string reply(const Json &id, const std::string &result) { return "{\"jsonrpc\":\"2.0\",\"id\":" + id.dump() + ",\"result\":" + result + "}"; }

std::string failure(const Json &id, int code, const std::string &message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id.dump() + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + text(message) + "}}";
}

const Json *member(const Json &object, const char *name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

std::string base64(const std::string &bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
    }
    return out;
}

// What a value of a schema is called in a refusal.
std::string kind(const Json &schema) {
    const Json *type = member(schema, "type");
    if (const Json *alternatives = member(schema, "anyOf")) {
        std::string out;
        for (const Json &alternative : *alternatives) out += (out.empty() ? "" : " or ") + kind(alternative);
        return out;
    }
    if (const Json *values = member(schema, "enum")) {
        std::string out;
        for (const Json &v : *values) out += (out.empty() ? "one of " : ", ") + v.dump();
        return out;
    }
    if (!type || !type->is_string()) return "a value";
    const std::string &t = type->get_ref<const std::string &>();
    if (t == "string") return "a string";
    if (t == "integer") return "an integer";
    if (t == "number") return "a number";
    if (t == "boolean") return "true or false";
    if (t == "array") return "a list";
    if (t == "object") return "an object";
    if (t == "null") return "null";
    return "a value";
}

bool ascii_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

std::string stripped(const std::string &text) {
    size_t a = 0, b = text.size();
    while (a < b && ascii_space(text[a])) ++a;
    while (b > a && ascii_space(text[b - 1])) --b;
    return text.substr(a, b - a);
}

// Underscores between digits removed (Python's 1_000), or false where one stands elsewhere.
bool without_underscores(const std::string &text, std::string &out) {
    out.clear();
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '_') {
            out += text[i];
            continue;
        }
        if (i == 0 || i + 1 == text.size() || !std::isdigit(static_cast<unsigned char>(text[i - 1])) || !std::isdigit(static_cast<unsigned char>(text[i + 1])))
            return false;
    }
    return true;
}

// A number written as text, as pydantic's lax float reads it: surrounding spaces, a sign, digits with
// underscores between them, a point and an exponent, or inf, infinity and nan in any case.
bool lax_float(const std::string &given, double &out) {
    std::string text;
    if (!without_underscores(stripped(given), text) || text.empty()) return false;
    std::string word = text;
    bool negative = false;
    if (word[0] == '+' || word[0] == '-') {
        negative = word[0] == '-';
        word = word.substr(1);
    }
    for (char &c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (word == "inf" || word == "infinity") {
        out = negative ? -INFINITY : INFINITY;
        return true;
    }
    if (word == "nan") {
        out = NAN;
        return true;
    }
    return text::parse_double(text.data(), text.data() + text.size(), out);
}

// A whole number written as text, as pydantic's lax int reads it: spaces, a sign, digits with underscores between
// them, and a point followed by zeros only.
bool lax_integer(const std::string &given, long long &out) {
    std::string text;
    if (!without_underscores(stripped(given), text) || text.empty()) return false;
    size_t at = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        at = 1;
    }
    const size_t point = text.find('.');
    const std::string digits = text.substr(at, point == std::string::npos ? std::string::npos : point - at);
    if (digits.empty() || digits.size() > 18 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    if (point != std::string::npos) {
        const std::string fraction = text.substr(point + 1);
        if (fraction.empty() || fraction.find_first_not_of('0') != std::string::npos) return false;
    }
    out = std::stoll(digits) * (negative ? -1 : 1);
    return true;
}

// The value checked against the schema, or false with the reason in why. Numbers are taken as pydantic's lax
// mode took them for FastMCP: true and false as 1 and 0, and numbers written as text.
bool conform(const Json &schema, const Json &value, const std::string &where, Json &out, std::string &why) {
    if (const Json *alternatives = member(schema, "anyOf")) {
        for (const Json &alternative : *alternatives) {
            std::string ignored;
            if (conform(alternative, value, where, out, ignored)) return true;
        }
        why = where + ": must be " + kind(schema);
        return false;
    }
    if (const Json *values = member(schema, "enum")) {
        for (const Json &v : *values)
            if (v == value) {
                out = value;
                return true;
            }
        why = where + ": must be " + kind(schema);
        return false;
    }
    const Json *type = member(schema, "type");
    const std::string t = type && type->is_string() ? type->get<std::string>() : std::string();
    const auto refuse = [&] {
        why = where + ": must be " + kind(schema);
        return false;
    };
    if (t == "string") {
        if (!value.is_string()) return refuse();
        out = value;
    } else if (t == "integer") {
        long long whole = 0;
        if (value.is_number_integer()) {
            out = value;
        } else if (value.is_number_float() && std::fabs(value.get<double>()) < 9.2e18 && value.get<double>() == std::floor(value.get<double>())) {
            out = Json(static_cast<long long>(value.get<double>()));  // a whole number written with a point
        } else if (value.is_boolean()) {
            out = Json(value.get<bool>() ? 1 : 0);
        } else if (value.is_string() && lax_integer(value.get<std::string>(), whole)) {
            out = Json(whole);
        } else {
            return refuse();
        }
    } else if (t == "number") {
        double number = 0;
        if (value.is_number()) out = Json(value.get<double>());
        else if (value.is_boolean()) out = Json(value.get<bool>() ? 1.0 : 0.0);
        else if (value.is_string() && lax_float(value.get<std::string>(), number)) out = Json(number);
        else return refuse();
    } else if (t == "boolean") {
        if (!value.is_boolean()) return refuse();
        out = value;
    } else if (t == "null") {
        if (!value.is_null()) return refuse();
        out = value;
    } else if (t == "array") {
        if (!value.is_array()) return refuse();
        out = Json::array();
        const Json *items = member(schema, "items");
        for (size_t i = 0; i < value.size(); ++i) {
            Json item;
            if (items) {
                if (!conform(*items, value[i], where + "[" + std::to_string(i) + "]", item, why)) return false;
            } else {
                item = value[i];
            }
            out.push_back(std::move(item));
        }
    } else if (t == "object") {
        if (!value.is_object()) return refuse();
        const Json *properties = member(schema, "properties");
        const Json *extra = member(schema, "additionalProperties");
        out = Json::object();
        for (const auto &item : value.items()) {
            const std::string name(item.key());
            const Json *declared = properties ? member(*properties, name.c_str()) : nullptr;
            const std::string inner = where.empty() ? name : where + "." + name;
            Json checked;
            if (declared) {
                if (!conform(*declared, item.value(), inner, checked, why)) return false;
            } else if (extra && extra->is_boolean() && !extra->get<bool>()) {
                why = inner + ": is not an argument of this tool";
                return false;
            } else if (extra && extra->is_object()) {
                if (!conform(*extra, item.value(), inner, checked, why)) return false;
            } else {
                checked = item.value();
            }
            out[name] = std::move(checked);
        }
        if (properties)
            for (const auto &item : properties->items()) {
                const std::string name(item.key());
                if (out.contains(name)) continue;
                if (const Json *fallback = member(item.value(), "default")) out[name] = *fallback;
            }
        if (const Json *required = member(schema, "required"))
            for (const Json &name : *required)
                if (!out.contains(name.get<std::string>())) {
                    why = (where.empty() ? std::string() : where + ".") + name.get<std::string>() + ": a required argument is missing";
                    return false;
                }
    } else {
        out = value;
    }
    return true;
}

// ---- the SDK's reading of messages (mcp.types with pydantic in lax mode) ------------------------------

// Rust's char::is_whitespace, Unicode's White_Space, which pydantic trims from a number written as text: what
// str.isspace() takes, less U+001C to U+001F.
bool white_space(uint32_t c) { return detail::python_space(c) && !(c >= 0x1c && c <= 0x1f); }

// pydantic's lax int: a whole number, true or false, a float of a whole value within 64 bits, or text: white
// space about it, a sign, digits with single underscores between them, and a point with zeros only after it.
bool lax_int(const Json &v) {
    if (v.is_boolean() || v.is_number_integer()) return true;
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9223372036854775808.0;
    }
    if (!v.is_string()) return false;
    const std::vector<detail::CodePoint> points = detail::code_points(v.get<std::string>());
    size_t a = 0, b = points.size();
    while (a < b && white_space(points[a].code)) ++a;
    while (b > a && white_space(points[b - 1].code)) --b;
    if (a < b && (points[a].code == '+' || points[a].code == '-')) ++a;
    bool digit = false, point = false, zeros = false;
    for (size_t i = a; i < b; ++i) {
        const uint32_t c = points[i].code;
        if (point) {
            if (c != '0') return false;
            zeros = true;
        } else if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c == '_') {
            if (i == a || i + 1 >= b || !(points[i - 1].code >= '0' && points[i - 1].code <= '9') || !(points[i + 1].code >= '0' && points[i + 1].code <= '9'))
                return false;
        } else if (c == '.' && digit) {
            point = true;
        } else {
            return false;
        }
    }
    return digit && (!point || zeros);
}

// pydantic's lax bool: true or false, 0 or 1, or the words it knows in any case.
bool lax_bool(const Json &v) {
    if (v.is_boolean()) return true;
    if (v.is_number()) return v.get<double>() == 0 || v.get<double>() == 1;
    if (!v.is_string()) return false;
    std::string word = v.get<std::string>();
    for (char &c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char *known : {"0", "off", "f", "false", "n", "no", "1", "on", "t", "true", "y", "yes"})
        if (word == known) return true;
    return false;
}

bool absent(const Json *v) { return !v || v->is_null(); }
bool is_text(const Json *v) { return v && v->is_string(); }

// An optional object (absent or null passes) whose members, where given, pass their checks.
bool optional_object(const Json *v, std::initializer_list<std::pair<const char *, std::function<bool(const Json *)>>> fields = {}) {
    if (absent(v)) return true;
    if (!v->is_object()) return false;
    for (const auto &field : fields)
        if (!field.second(member(*v, field.first))) return false;
    return true;
}

bool optional_text(const Json *v) { return absent(v) || v->is_string(); }

bool text_values(const Json *v) {  // dict[str, str] | None
    if (absent(v)) return true;
    if (!v->is_object()) return false;
    for (const auto &item : v->items())
        if (!item.value().is_string()) return false;
    return true;
}

// RequestParams: task (TaskMetadata, ttl a strict int) and _meta (progressToken text or an int).
bool base_params(const Json &params) {
    if (!optional_object(member(params, "task"), {{"ttl", [](const Json *ttl) { return absent(ttl) || ttl->is_number_integer(); }}})) return false;
    return optional_object(member(params, "_meta"), {{"progressToken", [](const Json *t) { return absent(t) || t->is_string() || lax_int(*t); }}});
}

// The special schemes of the URL Standard.
bool special_scheme(const std::string &scheme) {
    return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" || scheme == "ftp" || scheme == "file";
}

}  // namespace

bool resource_address(const std::string &given, std::string &address) {
    // The URL Standard's parse without a base, as pydantic's AnyUrl makes it: C0 controls and spaces about the text
    // and tabs and line ends in it removed, a scheme, and a host where the scheme or "//" asks for one.
    size_t a = 0, b = given.size();
    while (a < b && static_cast<unsigned char>(given[a]) <= 0x20) ++a;
    while (b > a && static_cast<unsigned char>(given[b - 1]) <= 0x20) --b;
    std::string text;
    for (size_t i = a; i < b; ++i)
        if (given[i] != '\t' && given[i] != '\n' && given[i] != '\r') text += given[i];
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || !std::isalpha(static_cast<unsigned char>(text[0]))) return false;
    std::string scheme = text.substr(0, colon);
    for (char &c : scheme) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return false;
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string rest = text.substr(colon + 1);
    const bool special = special_scheme(scheme);
    if (special || rest.compare(0, 2, "//") == 0) {
        size_t start = 0;
        if (special)
            while (start < rest.size() && (rest[start] == '/' || rest[start] == '\\')) ++start;
        else
            start = 2;
        size_t end = start;
        while (end < rest.size() && !std::strchr(special ? "/\\?#" : "/?#", rest[end])) ++end;
        std::string authority = rest.substr(start, end - start);
        const size_t at = authority.rfind('@');
        if (at != std::string::npos) authority = authority.substr(at + 1);
        std::string host = authority, port;
        if (!host.empty() && host[0] == '[') {
            const size_t close = host.find(']');
            if (close == std::string::npos) return false;
            port = host.substr(close + 1);
            host = host.substr(0, close + 1);
            if (!port.empty() && port[0] != ':') return false;
        } else {
            const size_t c = host.find(':');
            if (c != std::string::npos) {
                port = host.substr(c);
                host = host.substr(0, c);
            }
        }
        for (const char c : host.size() > 1 && host[0] == '[' ? std::string() : host)
            if (std::strchr(" #/:<>?@[\\]^|", c) || static_cast<unsigned char>(c) < 0x20 || (special && (c == '%' || c == 0x7f))) return false;
        if (special && scheme != "file" && host.empty()) return false;
        if (port.size() > 1) {
            const std::string digits = port.substr(1);
            if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
            const size_t first = digits.find_first_not_of('0');
            if (first != std::string::npos && (digits.size() - first > 5 || std::stol(digits.substr(first)) > 65535)) return false;
        }
        if (special) rest = "//" + rest.substr(start);
    }
    address = scheme + ":" + rest;
    return true;
}

namespace {

// Whether a request's parameters are what the SDK's ClientRequest takes for its method (unknown methods fail).
bool valid_request(const std::string &method, const Json *params) {
    const bool none = absent(params);
    if (!none && !base_params(*params)) return false;
    if (method == "ping") return true;
    if (method == "tools/list" || method == "prompts/list" || method == "resources/list" || method == "resources/templates/list" || method == "tasks/list")
        return none || optional_text(member(*params, "cursor"));
    if (none) return false;  // the other methods' parameters are required
    const Json &p = *params;
    if (method == "initialize") {
        const Json *version = member(p, "protocolVersion");
        if (!version || !(version->is_string() || lax_int(*version))) return false;
        const auto nested = [](const Json *v) { return optional_object(v); };
        const Json *capabilities = member(p, "capabilities");
        if (absent(capabilities) || !capabilities->is_object()) return false;
        const Json *experimental = member(*capabilities, "experimental");
        if (!absent(experimental)) {
            if (!experimental->is_object()) return false;
            for (const auto &item : experimental->items())
                if (!item.value().is_object()) return false;
        }
        if (!optional_object(member(*capabilities, "sampling"), {{"context", nested}, {"tools", nested}}) ||
            !optional_object(member(*capabilities, "elicitation"), {{"form", nested}, {"url", nested}}) ||
            !optional_object(member(*capabilities, "roots"), {{"listChanged", [](const Json *v) { return absent(v) || lax_bool(*v); }}}) ||
            !optional_object(member(*capabilities, "tasks"),
                             {{"list", nested},
                              {"cancel", nested},
                              {"requests", [&](const Json *v) {
                                   return optional_object(v, {{"sampling", [&](const Json *w) { return optional_object(w, {{"createMessage", nested}}); }},
                                                              {"elicitation", [&](const Json *w) { return optional_object(w, {{"create", nested}}); }}});
                               }}}))
            return false;
        const Json *info = member(p, "clientInfo");
        if (absent(info) || !info->is_object() || !is_text(member(*info, "name")) || !is_text(member(*info, "version")) ||
            !optional_text(member(*info, "title")) || !optional_text(member(*info, "websiteUrl")))
            return false;
        const Json *icons = member(*info, "icons");
        if (!absent(icons)) {
            if (!icons->is_array()) return false;
            for (const Json &icon : *icons) {
                const Json *sizes = member(icon, "sizes");
                if (!icon.is_object() || !is_text(member(icon, "src")) || !optional_text(member(icon, "mimeType"))) return false;
                if (!absent(sizes) && (!sizes->is_array() || std::any_of(sizes->begin(), sizes->end(), [](const Json &v) { return !v.is_string(); })))
                    return false;
            }
        }
        return true;
    }
    if (method == "completion/complete") {
        const Json *ref = member(p, "ref"), *argument = member(p, "argument");
        if (absent(ref) || !ref->is_object()) return false;
        const Json *type = member(*ref, "type");
        const bool resource = is_text(type) && type->get<std::string>() == "ref/resource" && is_text(member(*ref, "uri"));
        const bool prompt = is_text(type) && type->get<std::string>() == "ref/prompt" && is_text(member(*ref, "name"));
        if (!resource && !prompt) return false;
        if (absent(argument) || !argument->is_object() || !is_text(member(*argument, "name")) || !is_text(member(*argument, "value"))) return false;
        return optional_object(member(p, "context"), {{"arguments", text_values}});
    }
    if (method == "logging/setLevel") {
        const Json *level = member(p, "level");
        if (!is_text(level)) return false;
        for (const char *known : {"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"})
            if (level->get<std::string>() == known) return true;
        return false;
    }
    if (method == "prompts/get") return is_text(member(p, "name")) && text_values(member(p, "arguments"));
    if (method == "resources/read" || method == "resources/subscribe" || method == "resources/unsubscribe") {
        std::string address;
        return is_text(member(p, "uri")) && resource_address(member(p, "uri")->get<std::string>(), address);
    }
    if (method == "tools/call") {
        const Json *arguments = member(p, "arguments");
        return is_text(member(p, "name")) && (absent(arguments) || arguments->is_object());
    }
    if (method == "tasks/get" || method == "tasks/result" || method == "tasks/cancel") return is_text(member(p, "taskId"));
    return false;
}

}  // namespace

Json checked_arguments(const Json &schema, const Json &arguments) {
    Json out;
    std::string why;
    if (!conform(schema, arguments, std::string(), out, why)) throw ValueError(why);
    return out;
}

Server::Server(std::string name, std::string version, std::string instructions)
    : name_(std::move(name)), version_(std::move(version)), instructions_(std::move(instructions)) {}

void Server::add(Tool tool) {
    Entry entry;
    entry.schema = Json::parse(tool.input_schema);
    if (!tool.output_schema.empty()) Json::parse(tool.output_schema);  // a mistake in a declaration fails at once
    if (!tool.annotations.empty()) Json::parse(tool.annotations);
    entry.tool = std::move(tool);
    auto at = tools_.end();
    if (entry.tool.position >= 0)
        at = std::find_if(tools_.begin(), tools_.end(), [&](const Entry &e) { return e.tool.position < 0 || e.tool.position > entry.tool.position; });
    tools_.insert(at, std::move(entry));
}

void Server::add(Resource resource) { resources_.push_back(std::move(resource)); }

Kind classify(const Json &message) {
    // JSONRPCMessage.model_validate: whichever of its four models the message passes, an error first.
    if (!message.is_object() || !is_text(member(message, "jsonrpc")) || member(message, "jsonrpc")->get<std::string>() != "2.0") return Kind::invalid;
    const Json *id = member(message, "id"), *method = member(message, "method"), *params = member(message, "params");
    const Json *result = member(message, "result"), *error = member(message, "error");
    const bool strict_id = id && (id->is_number_integer() || id->is_string());  // RequestId: a strict int or text
    if (id && (id->is_string() || lax_int(*id)) && error && error->is_object() && member(*error, "code") && lax_int(*member(*error, "code")) &&
        is_text(member(*error, "message")))
        return Kind::error;
    const bool request_like = is_text(method) && (absent(params) || params->is_object());
    if (request_like && strict_id) return Kind::request;
    if (strict_id && result && result->is_object()) return Kind::response;
    if (request_like) return Kind::notification;
    return Kind::invalid;
}

std::string Server::answer(const Json &message, Exchange &exchange, bool stateful) {
    const Json &id = message.at("id");
    const std::string method = message.at("method").get<std::string>();
    const Json *given = member(message, "params");
    // The session's ClientRequest, then (with a session) initialize before all but ping.
    if (!valid_request(method, given) || (stateful && !initialized_ && method != "initialize" && method != "ping"))
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id.dump() + ",\"error\":{\"code\":-32602,\"message\":\"Invalid request parameters\",\"data\":\"\"}}";
    const Json params = absent(given) ? Json::object() : *given;
    try {
        if (method == "initialize") {
            const Json *asked = member(params, "protocolVersion");
            const std::string version = asked->is_string() && kVersions.count(asked->get<std::string>()) ? asked->get<std::string>() : kLatest;
            const Json *capabilities = member(params, "capabilities");
            if (stateful) {
                client_confirms_ = !absent(member(*capabilities, "elicitation"));
                initialized_ = true;
            }
            return reply(id, "{\"protocolVersion\":" + text(version) +
                                 ",\"capabilities\":{\"experimental\":{},\"logging\":{},\"prompts\":{\"listChanged\":true},\"resources\":{\"subscribe\":false,"
                                 "\"listChanged\":true},\"tools\":{\"listChanged\":true},\"extensions\":{\"io.modelcontextprotocol/ui\":{}}},\"serverInfo\":{\"name\":" +
                                 text(name_) + ",\"version\":" + text(version_) + "},\"instructions\":" + text(instructions_) + "}");
        }
        if (method == "ping" || method == "logging/setLevel") return reply(id, "{}");
        if (method == "tools/list") {
            std::string out;
            for (const Entry &entry : tools_) {
                const Tool &t = entry.tool;
                out += (out.empty() ? "" : ",") + std::string("{\"name\":") + text(t.name) + ",\"description\":" + text(t.description) + ",\"inputSchema\":" +
                       t.input_schema;
                if (!t.output_schema.empty()) out += ",\"outputSchema\":" + t.output_schema;
                if (!t.annotations.empty()) out += ",\"annotations\":" + t.annotations;
                out += ",\"_meta\":{\"fastmcp\":{\"tags\":[]}}}";
            }
            return reply(id, "{\"tools\":[" + out + "]}");
        }
        if (method == "tools/call") return call(id, params, exchange);
        if (method == "resources/list") {
            std::string out;
            for (const Resource &r : resources_)
                out += (out.empty() ? "" : ",") + std::string("{\"name\":") + text(r.name) + ",\"uri\":" + text(r.uri) + ",\"mimeType\":" + text(r.mime_type) +
                       ",\"_meta\":{\"fastmcp\":{\"tags\":[]}}}";
            return reply(id, "{\"resources\":[" + out + "]}");
        }
        if (method == "resources/templates/list") return reply(id, "{\"resourceTemplates\":[]}");
        if (method == "resources/read") {
            std::string address;
            resource_address(params.at("uri").get<std::string>(), address);
            for (const Resource &r : resources_)
                if (address == r.uri)
                    return reply(id, "{\"contents\":[{\"uri\":" + text(r.uri) + ",\"mimeType\":" + text(r.mime_type) + ",\"text\":" + text(r.read()) + "}]}");
            return failure(id, -32002, "Resource not found: Unknown resource: " + detail::python_repr(address));
        }
        if (method == "prompts/list") return reply(id, "{\"prompts\":[]}");
        if (method == "prompts/get") return failure(id, 0, "Unknown prompt: " + detail::python_repr(params.at("name").get<std::string>()));
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id.dump() + ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
    } catch (const std::exception &e) {
        return failure(id, 0, e.what());
    }
}

void Server::notified(const Json &message) {
    // InitializedNotification ends the session's initialization as the initialize request does.
    if (is_text(member(message, "method")) && member(message, "method")->get<std::string>() == "notifications/initialized") initialized_ = true;
}

std::string Server::call(const Json &id, const Json &params, Exchange &exchange) {
    const Json *name = member(params, "name");
    const Entry *entry = nullptr;
    for (const Entry &e : tools_)
        if (name && name->is_string() && name->get<std::string>() == e.tool.name) entry = &e;
    const auto error = [&](const std::string &message) {
        return reply(id, "{\"content\":[{\"type\":\"text\",\"text\":" + text(message) + "}],\"isError\":true}");
    };
    if (!entry) return error("Unknown tool: " + detail::python_repr(name->get<std::string>()));
    const Tool &tool = entry->tool;
    const Json *given = member(params, "arguments");
    // A task-augmented call (SEP-1686): FastMCP's tools here run only in the request.
    if (!absent(member(params, "task")))
        return error("Error calling tool '" + tool.name + "': FunctionTool 'tool:" + tool.name + "@' does not support task-augmented execution");
    Json arguments;
    try {
        arguments = checked_arguments(entry->schema, given && !given->is_null() ? *given : Json::object());
    } catch (const std::exception &e) {
        return error("Invalid arguments for tool '" + tool.name + "': " + e.what());
    }
    Result result;
    try {
        result = tool.run(arguments, exchange);
    } catch (const std::bad_alloc &) {
        return error("Error calling tool '" + tool.name + "': the request needs more memory than is available");
    } catch (const std::exception &e) {
        return error("Error calling tool '" + tool.name + "': " + e.what());
    }
    if (!result.image.empty())
        return reply(id, "{\"content\":[{\"type\":\"image\",\"data\":\"" + base64(result.image) + "\",\"mimeType\":\"image/png\"}],\"isError\":false}");
    const std::string value = result.value.dump();
    std::string structured = tool.wrapped ? "{\"result\":" + value + "}" : value;
    std::string meta = tool.wrapped ? "\"_meta\":{\"fastmcp\":{\"wrap_result\":true}}," : "";
    if (!tool.wrapped && !result.value.is_object()) structured.clear();  // structured content is an object
    // An empty list is no content, as FastMCP sent it; anything else its JSON text.
    const bool nothing = result.value.is_array() && result.value.empty();
    return reply(id, "{" + meta + "\"content\":[" + (nothing ? std::string() : "{\"type\":\"text\",\"text\":" + text(value) + "}") + "]" +
                         (structured.empty() ? std::string() : ",\"structuredContent\":" + structured) + ",\"isError\":false}");
}

// ---- standard input and output ---------------------------------------------------------------------------

namespace {

class StdioExchange : public Exchange {
public:
    StdioExchange(Server &server, std::ostream &out) : server_(server), out_(out) {}

    void write(const std::string &line) {
        std::lock_guard<std::mutex> hold(write_);
        out_ << line << '\n';
        out_.flush();
    }

    std::string confirm(const std::string &message) override {
        if (!server_.client_confirms()) throw std::runtime_error("the client cannot ask its user to confirm");
        std::shared_ptr<Pending> pending = std::make_shared<Pending>();
        const std::string id = "kerfkite-" + std::to_string(++counter_);
        {
            std::lock_guard<std::mutex> hold(lock_);
            if (closed_) throw std::runtime_error("the client has gone");
            waiting_[id] = pending;
        }
        write("{\"jsonrpc\":\"2.0\",\"id\":" + text(id) + ",\"method\":\"elicitation/create\",\"params\":{\"message\":" + text(message) +
              ",\"requestedSchema\":{\"type\":\"object\",\"properties\":{}}}}");
        std::unique_lock<std::mutex> hold(lock_);
        changed_.wait(hold, [&] { return pending->done; });
        waiting_.erase(id);
        if (!pending->ok) throw std::runtime_error("the client did not confirm");
        return pending->action;
    }

    // A response from the client to one of the server's requests.
    void answered(const Json &message) {
        const Json *id = member(message, "id");
        if (!id || !id->is_string()) return;
        std::lock_guard<std::mutex> hold(lock_);
        const auto found = waiting_.find(id->get<std::string>());
        if (found == waiting_.end()) return;
        const Json *result = member(message, "result");
        const Json *action = result ? member(*result, "action") : nullptr;
        found->second->ok = action && action->is_string();
        if (found->second->ok) found->second->action = action->get<std::string>();
        found->second->done = true;
        changed_.notify_all();
    }

    // Input has ended: whoever waits for an answer gets none.
    void close() {
        std::lock_guard<std::mutex> hold(lock_);
        closed_ = true;
        for (auto &w : waiting_) w.second->done = true;
        changed_.notify_all();
    }

private:
    struct Pending {
        bool done = false, ok = false;
        std::string action;
    };
    Server &server_;
    std::ostream &out_;
    std::mutex write_, lock_;
    std::condition_variable changed_;
    std::map<std::string, std::shared_ptr<Pending>> waiting_;
    std::atomic<long> counter_{0};
    bool closed_ = false;
};

}  // namespace

void serve_stdio(Server &server, std::istream &in, std::ostream &out) {
    // Reading must not flush the output (std::cin is tied to std::cout): the tools' threads write it under a lock,
    // and a flush from this thread at the same moment could send a line twice.
    in.tie(nullptr);
    StdioExchange exchange(server, out);
    std::vector<std::thread> calls;
    std::string line;
    while (std::getline(in, line)) {
        // JSONRPCMessage.model_validate_json of each line: one that is no message is reported to the client as the
        // SDK's server reports an exception from its stream.
        Json message = Json::parse(line, nullptr, false);
        const Kind kind = message.is_discarded() ? Kind::invalid : classify(message);
        if (kind == Kind::invalid) {
            exchange.write("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\",\"params\":{\"level\":\"error\",\"logger\":\"mcp.server.exception_handler\","
                           "\"data\":\"Internal Server Error\"}}");
            continue;
        }
        if (kind == Kind::response || kind == Kind::error) {
            exchange.answered(message);
            continue;
        }
        if (kind == Kind::notification) {
            server.notified(message);
            continue;
        }
        const bool long_running = message.at("method").get<std::string>() == "tools/call";
        if (!long_running) {
            exchange.write(server.answer(message, exchange, true));
            continue;
        }
        calls.emplace_back([&server, &exchange, message] { exchange.write(server.answer(message, exchange, true)); });
    }
    exchange.close();
    for (std::thread &t : calls) t.join();
}

}  // namespace mcp
}  // namespace server
}  // namespace kerfkite
