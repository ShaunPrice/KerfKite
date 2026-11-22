// The phone pendant listener and its app. See phone_monitor.hpp.
#include "phone_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "kerfkite/errors.hpp"
#include "os_random.hpp"
#include "static_files.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kerfkite {
namespace server {

namespace {

using json = Json;

const json *field(const json &object, const char *name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

http::Response with_headers(http::Response response) {
    response.set("Cache-Control", "no-store");
    response.set("Referrer-Policy", "no-referrer");
    response.set("X-Content-Type-Options", "nosniff");
    response.set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'");
    return response;
}

bool flag(const json &state, const char *name) {
    const json *value = field(state, name);
    return value && value->is_boolean() && value->get<bool>();
}

// str(UUID(text)): the canonical form, or empty when the text is not a UUID.
std::string canonical_uuid(const json *value) {
    if (!value || !value->is_string()) return std::string();
    std::string hex = value->get<std::string>();
    for (const char *prefix : {"urn:", "uuid:"}) {
        size_t at;
        while ((at = hex.find(prefix)) != std::string::npos) hex.erase(at, std::strlen(prefix));
    }
    while (!hex.empty() && (hex.front() == '{' || hex.front() == '}')) hex.erase(0, 1);
    while (!hex.empty() && (hex.back() == '{' || hex.back() == '}')) hex.pop_back();
    hex.erase(std::remove(hex.begin(), hex.end(), '-'), hex.end());
    if (hex.size() != 32 || !std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) return std::string();
    for (char &c : hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

struct Address {
    std::string text;
    unsigned long value;
};

unsigned long numeric(const std::string &ip) {
    in_addr a{};
    inet_pton(AF_INET, ip.c_str(), &a);
    return ntohl(a.s_addr);
}

bool loopback(const std::string &ip) { return (numeric(ip) >> 24) == 127; }
bool link_local(const std::string &ip) { return (numeric(ip) >> 16) == 0xA9FE; }

std::string routed_address() {
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return std::string();
#else
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return std::string();
#endif
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &target.sin_addr);  // documentation address: only the route is looked up
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr *>(&target), sizeof target) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof local;
        if (getsockname(s, reinterpret_cast<sockaddr *>(&local), &length) == 0) {
            char text[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &local.sin_addr, text, sizeof text);
            out = text;
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
    if (out.empty() || loopback(out) || out == "0.0.0.0") return std::string();
    return out;
}

}  // namespace

PhoneMonitor::PhoneMonitor(Device &device, PhoneJobs &jobs, std::string static_dir, int port, bool plaintext, bool allow_physical,
                           std::string host)
    : device_(device), jobs_(jobs), static_dir_(std::move(static_dir)), host_(std::move(host)), port_(port), bound_port_(port),
      plaintext_(plaintext), allow_physical_(allow_physical) {}

PhoneMonitor::~PhoneMonitor() {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    if (listener_) listener_->stop();
    if (worker_.joinable()) worker_.join();
}

std::vector<std::string> PhoneMonitor::addresses() {
    std::set<std::string> found;
    char host[256] = {0};
    if (gethostname(host, sizeof host - 1) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo *list = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &list) == 0) {
            for (addrinfo *a = list; a; a = a->ai_next) {
                char text[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ai_addr)->sin_addr, text, sizeof text);
                if (!loopback(text)) found.insert(text);
            }
            freeaddrinfo(list);
        }
    }
    const std::string routed = routed_address();
    if (!routed.empty()) found.insert(routed);
    std::vector<std::string> out(found.begin(), found.end());
    std::sort(out.begin(), out.end(), [&](const std::string &a, const std::string &b) {
        const auto key = [&](const std::string &ip) { return std::make_tuple(ip != routed, link_local(ip), numeric(ip)); };
        return key(a) < key(b);
    });
    return out;
}

Json PhoneMonitor::info() {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    json out = json::object();
    out["enabled"] = listener_ != nullptr;
    out["port"] = bound_port_;
    json hosts = json::array();
    if (listener_)
        for (const std::string &ip : addresses()) hosts.push_back(ip);
    out["hosts"] = hosts;
    out["scheme"] = "http";
    out["token"] = token_.empty() ? json() : json(token_);
    out["encryption"] = "none";
    out["fingerprint"] = json();
    out["fragment"] = token_.empty() ? json() : json("token=" + token_);
    // What the next enable will do, so the workstation can offer the unencrypted option.
    out["can_encrypt"] = false;
    out["plaintext"] = plaintext_;
    return out;
}

void PhoneMonitor::set_plaintext(const Json &plaintext) {
    if (!plaintext.is_boolean()) throw ValueError("Choose encrypted or unencrypted pairing.");
    std::lock_guard<std::recursive_mutex> hold(lock_);
    if (plaintext.get<bool>() != plaintext_) {
        if (listener_) throw ValueError("Disable phone pairing before changing its encryption.");
        plaintext_ = plaintext.get<bool>();
    }
}

Json PhoneMonitor::enable() {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    if (listener_) return info();
    token_ = token_urlsafe(32);
    revoked_ = false;
    {
        std::lock_guard<std::mutex> hold_recent(recent_mutex_);
        recent_.clear();
    }
    try {
        listener_ = std::make_unique<http::Server>(host_, port_);
    } catch (const std::exception &) {
        token_.clear();
        throw ValueError("Phone listener could not start. Check the monitor port and TLS certificate.");
    }
    bound_port_ = listener_->port();
    http::Server *server = listener_.get();
    worker_ = std::thread([this, server] {
        server->serve([this](const http::Request &request) { return handle(request); },
                      [](const http::Request &) { return static_cast<size_t>(2048); }, 8);
    });
    return info();
}

Json PhoneMonitor::disable() {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    if (listener_) {
        {
            Hold machine(jobs_.lock_ref());
            revoked_ = true;
            jobs_.invalidate_controls();
        }
        listener_->stop();
        if (worker_.joinable()) worker_.join();
        listener_.reset();
        token_.clear();
    }
    return info();
}

void PhoneMonitor::begin_action(const Json &data, bool required) {
    if (revoked_) throw ValueError("Phone pairing has ended.");
    if (!data.is_object()) throw ValueError("Supply a phone action object.");
    const json *given = field(data, "request_id");
    if (!required && (!given || given->is_null())) return;  // Compatibility with the browser monitor.
    const std::string identifier = canonical_uuid(given);
    if (identifier.empty()) throw ValueError("Supply a unique request ID for this action.");
    std::lock_guard<std::mutex> hold(recent_mutex_);
    if (std::find(recent_.begin(), recent_.end(), identifier) != recent_.end())
        throw ValueError("This request has already been handled. Read the controller status.");
    recent_.push_back(identifier);
    if (recent_.size() > 2048) recent_.erase(recent_.begin());
}

http::Response PhoneMonitor::handle(const http::Request &request) {
    const std::string token = [&] {
        std::lock_guard<std::recursive_mutex> hold(lock_);
        return token_;
    }();
    const std::string *authorization = request.header("authorization");
    const bool paired = !token.empty() && same_secret(authorization ? *authorization : std::string(), "Bearer " + token);
    try {
        if (request.path.compare(0, 5, "/api/") == 0 && (revoked_ || !paired))
            return with_headers(http::Response::error(401, "Pair this phone using the monitor link from the Pi"));
        const bool get = request.method == "GET" || request.method == "HEAD";
        if (get && request.path == "/") return with_headers(static_file(static_dir_, "monitor.html"));
        if (get && request.path == "/monitor.js") return with_headers(static_file(static_dir_, "monitor.js"));
        if (get && request.path == "/monitor.css") return with_headers(static_file(static_dir_, "monitor.css"));
        if (get && request.path == "/api/status") {
            Hold machine(jobs_.lock_ref());
            const json state = device_.status();
            json phone = jobs_.status();
            phone["control_epoch"] = jobs_.control_epoch();
            // Explicit allowlist: no saved machine configuration or serial-port inventory.
            json result = json::object();
            for (const char *key : {"connected", "simulated", "state", "active", "progress", "position", "error", "log", "protocol"}) {
                const json *value = field(state, key);
                result[key] = value ? *value : json();
            }
            result["pendant"] = flag(state, "simulated") || allow_physical_;
            result["controls"] = true;
            for (const auto &item : phone.items()) result[std::string(item.key())] = item.value();
            return with_headers(http::Response::json(result.dump()));
        }
        if (request.method == "POST" && (request.path == "/api/action" || request.path == "/api/pendant")) {
            if (request.media_type() != "application/json") return with_headers(http::Response::error(415, "Send a JSON object"));
            json data;
            try {
                data = json::parse(request.body);
            } catch (const std::exception &) {
                return with_headers(http::Response::error(400, "Failed to decode JSON object"));
            }
            Hold machine(jobs_.lock_ref());
            if (request.path == "/api/action") {
                begin_action(data, false);
                const json *action = field(data, "action");
                const std::string name = action && action->is_string() ? action->get<std::string>() : std::string();
                if (name != "stop") jobs_.require_current_controls(field(data, "control_epoch"));
                if (name == "start") {
                    jobs_.start();
                } else if (name == "pause" || name == "resume" || name == "stop") {
                    if (name == "stop") jobs_.invalidate_controls();
                    device_.command(name == "pause" ? "hold" : name == "resume" ? "resume" : "reset", json::object());
                } else {
                    throw ValueError("Only start, pause, resume and stop are supported");
                }
            } else {
                begin_action(data, true);
                jobs_.require_current_controls(field(data, "control_epoch"));
                const json state = device_.status();
                const json *error = field(state, "error");
                const bool faulted = error && !error->is_null() && !(error->is_string() && error->get<std::string>().empty());
                if (!flag(state, "connected") || flag(state, "active") || !(state.at("state") == json("Idle")) || faulted)
                    throw ValueError("Connect an idle, ready controller before using the pendant.");
                if (!flag(state, "simulated") && !allow_physical_) throw ValueError("Physical control is disabled on this server.");
                const json *action = field(data, "action");
                const std::string name = action && action->is_string() ? action->get<std::string>() : std::string();
                json args = json::object();
                std::set<std::string> allowed = {"action", "request_id", "control_epoch"};
                if (name == "jog") {
                    const json *axis = field(data, "axis"), *distance = field(data, "distance"), *feed = field(data, "feed");
                    const auto finite = [](const json *n) { return n && n->is_number() && std::isfinite(n->get<double>()); };
                    if (!axis || !(*axis == json("X") || *axis == json("Y") || *axis == json("Z")) || !finite(distance) || !finite(feed))
                        throw ValueError("Supply one XYZ axis and finite numeric jog settings.");
                    const double d = std::fabs(distance->get<double>()), f = feed->get<double>();
                    if (!(0.001 <= d && d <= 10) || !(0.1 <= f && f <= 1000))
                        throw ValueError("Jog distance is 0.001\xe2\x80\x93" "10 mm and feed is 0.1\xe2\x80\x93" "1000 mm/min.");
                    args["axis"] = *axis;
                    args["distance"] = *distance;
                    args["feed"] = *feed;
                    allowed.insert({"axis", "distance", "feed"});
                } else if (name != "home" && name != "zero") {
                    throw ValueError("Only a single incremental jog, home or work zero is supported.");
                }
                std::set<std::string> given;
                for (const auto &item : data.items()) given.insert(std::string(item.key()));
                if (given != allowed) throw ValueError("Unexpected pendant action fields.");
                jobs_.disarm();
                device_.command(name, args);
            }
            json ok = json::object();
            ok["ok"] = true;
            return with_headers(http::Response::json(ok.dump()));
        }
        return with_headers(http::Response::error(request.method == "POST" || get ? 404 : 405, "Not Found"));
    } catch (const ValueError &e) {
        return with_headers(http::Response::error(400, e.what()));
    } catch (const RuntimeError &e) {
        return with_headers(http::Response::error(400, e.what()));
    }
}

}  // namespace server
}  // namespace kerfkite
