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

std::string Server::receive(const Json &message, Exchange &exchange) {
    if (message.is_array()) {
        if (message.empty()) return failure(Json(), -32600, "Invalid Request");
        std::string out;
        for (const Json &one : message) {
            const std::string answer = receive(one, exchange);
            if (!answer.empty()) out += (out.empty() ? "" : ",") + answer;
        }
        return out.empty() ? std::string() : "[" + out + "]";
    }
    if (!message.is_object()) return failure(Json(), -32600, "Invalid Request");
    const Json *method = member(message, "method");
    const Json *id = member(message, "id");
    if (!method) return std::string();  // a response, or nothing: no answer
    if (!method->is_string()) return id ? failure(*id, -32600, "Invalid Request") : std::string();
    if (!id) return std::string();  // a notification (initialized, cancelled, roots changed)
    if (!(id->is_string() || id->is_number_integer())) return failure(Json(), -32600, "Invalid Request");
    try {
        return request(message, exchange);
    } catch (const std::exception &e) {
        return failure(*id, -32603, e.what());
    }
}

std::string Server::request(const Json &message, Exchange &exchange) {
    const Json &id = message.at("id");
    const std::string method = message.at("method").get<std::string>();
    const Json *given = member(message, "params");
    const Json params = given && given->is_object() ? *given : Json::object();
    if (method == "initialize") {
        const Json *asked = member(params, "protocolVersion");
        const std::string version = asked && asked->is_string() && kVersions.count(asked->get<std::string>()) ? asked->get<std::string>() : kLatest;
        const Json *capabilities = member(params, "capabilities");
        client_confirms_ = capabilities && member(*capabilities, "elicitation") != nullptr;
        return reply(id, "{\"protocolVersion\":" + text(version) +
                             ",\"capabilities\":{\"logging\":{},\"resources\":{\"subscribe\":false,\"listChanged\":false},"
                             "\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":" +
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
            out += "}";
        }
        return reply(id, "{\"tools\":[" + out + "]}");
    }
    if (method == "tools/call") return call(id, params, exchange);
    if (method == "resources/list") {
        std::string out;
        for (const Resource &r : resources_)
            out += (out.empty() ? "" : ",") + std::string("{\"name\":") + text(r.name) + ",\"uri\":" + text(r.uri) + ",\"mimeType\":" + text(r.mime_type) + "}";
        return reply(id, "{\"resources\":[" + out + "]}");
    }
    if (method == "resources/templates/list") return reply(id, "{\"resourceTemplates\":[]}");
    if (method == "resources/read") {
        const Json *uri = member(params, "uri");
        for (const Resource &r : resources_)
            if (uri && uri->is_string() && uri->get<std::string>() == r.uri)
                return reply(id, "{\"contents\":[{\"uri\":" + text(r.uri) + ",\"mimeType\":" + text(r.mime_type) + ",\"text\":" + text(r.read()) + "}]}");
        return failure(id, -32002, "Unknown resource: " + (uri && uri->is_string() ? uri->get<std::string>() : std::string()));
    }
    if (method == "prompts/list") return reply(id, "{\"prompts\":[]}");
    return failure(id, -32601, "Method not found: " + method);
}

std::string Server::call(const Json &id, const Json &params, Exchange &exchange) {
    const Json *name = member(params, "name");
    const Entry *entry = nullptr;
    for (const Entry &e : tools_)
        if (name && name->is_string() && name->get<std::string>() == e.tool.name) entry = &e;
    if (!entry) return failure(id, -32602, "Unknown tool: " + (name && name->is_string() ? name->get<std::string>() : std::string()));
    const Tool &tool = entry->tool;
    const Json *given = member(params, "arguments");
    const auto error = [&](const std::string &message) {
        return reply(id, "{\"content\":[{\"type\":\"text\",\"text\":" + text(message) + "}],\"isError\":true}");
    };
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

class NoExchange : public Exchange {
public:
    std::string confirm(const std::string &) override { throw std::runtime_error("this transport cannot ask the client"); }
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
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        Json message;
        try {
            message = Json::parse(line);
        } catch (const std::exception &) {
            exchange.write(failure(Json(), -32700, "Parse error"));
            continue;
        }
        if (message.is_object() && !message.contains("method") && (message.contains("result") || message.contains("error"))) {
            exchange.answered(message);
            continue;
        }
        const Json *method = member(message, "method");
        const bool long_running = message.is_array() || (method && method->is_string() && method->get<std::string>() == "tools/call");
        if (!long_running) {
            const std::string answer = server.receive(message, exchange);
            if (!answer.empty()) exchange.write(answer);
            continue;
        }
        calls.emplace_back([&server, &exchange, message] {
            const std::string answer = server.receive(message, exchange);
            if (!answer.empty()) exchange.write(answer);
        });
    }
    exchange.close();
    for (std::thread &t : calls) t.join();
}

http::Response serve_http(Server &server, const http::Request &request) {
    if (request.method != "POST") {
        http::Response r = http::Response::error(405, "Use POST for messages; this server has no event stream");
        r.headers.emplace_back("Allow", "POST");
        return r;
    }
    const std::string *accept = request.header("accept");
    if (accept && accept->find("application/json") == std::string::npos && accept->find("*/*") == std::string::npos)
        return http::Response::error(406, "Accept application/json: this server answers each message as JSON");
    if (request.media_type() != "application/json") return http::Response::error(415, "Send the message as application/json");
    Json message;
    try {
        message = Json::parse(request.body);
    } catch (const std::exception &) {
        return http::Response::json(failure(Json(), -32700, "Parse error"), 400);
    }
    NoExchange exchange;
    const std::string answer = server.receive(message, exchange);
    if (answer.empty()) {
        http::Response accepted;
        accepted.status = 202;
        return accepted;
    }
    return http::Response::json(answer);
}

}  // namespace mcp
}  // namespace server
}  // namespace kerfkite
