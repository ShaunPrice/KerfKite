// The workstation server: the request checks of studio/server.py's create_app, its base routes, and main().
// See studio.hpp.
#include "studio.hpp"

#include <cstdlib>

#include <cmath>
#include <cstring>
#include <set>
#include <utility>

#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/solid_cad.hpp"
#include "project.hpp"
#include "os_random.hpp"
#include "phone_monitor.hpp"
#include "static_files.hpp"
#include "step_exchange.hpp"

namespace kerfkite {
namespace server {

using json = Json;

namespace {

// The machine-control routes that hold the controller for the whole request.
const std::set<std::string> kLockedRoutes = {"/api/connect", "/api/disconnect", "/api/arm",       "/api/disarm",
                                             "/api/run",     "/api/command",    "/api/head",      "/api/level/probe",
                                             "/api/level/collect", "/api/level/clear"};

const size_t kMaxContent = 8 * 1024 * 1024;
const size_t kStockBinary = 76 + 200000 * 56 + 1;  // stock_wire.MAX_BINARY_BYTES, and one byte more
const size_t kStockJson = 36 * 1024 * 1024;

std::string host_name(const http::Request &request) {
    const std::string *host = request.header("host");
    if (!host) return "127.0.0.1";
    return host->substr(0, host->find(':'));
}

}  // namespace

Json json_body(const http::Request &request) {
    const std::string type = request.media_type();
    const bool declared = type == "application/json" ||
                          (type.compare(0, 12, "application/") == 0 && type.size() > 5 && type.compare(type.size() - 5, 5, "+json") == 0);
    if (!declared) return json();
    try {
        return json::parse(request.body);
    } catch (const std::exception &) {
        return json();
    }
}

Json strict_json(const http::Request &request) {
    const std::string type = request.media_type();
    const bool declared = type == "application/json" ||
                          (type.compare(0, 12, "application/") == 0 && type.size() > 5 && type.compare(type.size() - 5, 5, "+json") == 0);
    if (!declared)
        throw HttpError(415, "Did not attempt to load JSON data because the request Content-Type was not 'application/json'.");
    try {
        return json::parse(request.body);
    } catch (const std::exception &) {
        throw HttpError(400, "The request body is not valid JSON");
    }
}

Json payload(const http::Request &request, std::initializer_list<const char *> required) {
    // Machine-control routes accept only a JSON object; anything else is a 400, never a 500.
    json data = json_body(request);
    if (!data.is_object()) throw ValueError("Send a JSON object");
    std::string missing;
    for (const char *key : required)
        if (!data.contains(key)) missing += (missing.empty() ? "" : ", ") + std::string(key);
    if (!missing.empty()) throw ValueError("Missing field: " + missing);
    return data;
}

http::Response json_response(const Json &value) { return http::Response::json(value.dump()); }

const Json *member(const Json &object, const char *name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

bool truthy(const Json *value) {
    if (!value || value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_number()) return value->get<double>() != 0;
    if (value->is_string()) return !value->get<std::string>().empty();
    if (value->is_array() || value->is_object()) return !value->empty();
    return true;
}

namespace {

std::string python_number_text(double value) {
    // {low} and {high} of cam.number's message: Python's str of an int or a float.
    if (value == std::floor(value) && std::fabs(value) < 1e15) return std::to_string(static_cast<long long>(value));
    return text::repr(value);
}

}  // namespace

double number(const Json *value, const std::string &name, double low, double high) {
    return detail::number(value, name, low, high, python_number_text(low), python_number_text(high));
}

Studio::Studio(Options options, MachineOptions machine)
    : options_(std::move(options)), token_(token_urlsafe(32)), device_(std::move(machine)), jobs_(device_, options_.enable_hardware, 1.8, options_.clock) {
    monitor_ = std::make_unique<PhoneMonitor>(device_, jobs_, options_.static_dir, options_.monitor_port, options_.monitor_plaintext,
                                              options_.enable_hardware, options_.monitor_host, options_.pendant_tls, options_.monitor_cert,
                                              options_.monitor_key);
    step_ = std::make_shared<StepExchange>(options_.step_program);
    // Solid features of kind step: cad_exchange.step_manifold's order, the content's checks before Open CASCADE.
    set_step_mesher([step = step_](const Json &content) {
        const std::string raw = StepExchange::decode(&content);
        if (!step->available()) throw ValueError(kNoStep);
        StepExchange::check(raw);
        return step->inspect(raw);
    });
    add("GET", "/api/session", [this](const http::Request &) {
        json out = json::object();
        out["token"] = token_;
        out["depth_paths"] = true;
        out["advanced_cam"] = true;
        out["holding_tabs"] = true;
        out["laser_patterns"] = true;
        out["router_entries"] = true;
        out["stock_wire_v1"] = true;
        out["phone_arm_challenge"] = true;
        out["rotary_stream"] = true;
        out["probe_leveling"] = true;
        out["pcb_import"] = true;
        out["relief_map"] = 1024;
        out["machine_readback"] = true;
        out["hardware_enabled"] = options_.enable_hardware;
        return json_response(out);
    });
    add_design_routes(*this);
    add_machine_routes(*this);
}

Studio::~Studio() {
    try {
        monitor_->disable();
        device_.disconnect();
    } catch (...) {
    }
}

void Studio::add(const std::string &method, const std::string &path, Route route) { routes_[method + " " + path] = std::move(route); }

http::Response Studio::static_file(const std::string &relative) const { return server::static_file(options_.static_dir, relative); }

void Studio::forget_level() {
    std::lock_guard<std::mutex> hold(level_mutex);
    level_plan = json();
    level_surface = json();
}

size_t Studio::body_limit(const http::Request &request) const {
    if (request.path == "/api/simulate") return request.media_type() == "application/vnd.kerfkite.stock-v1" ? kStockBinary : kStockJson;
    return kMaxContent;
}

// With KERFKITE_PARITY set in its environment, the server names in a header the core's own rules it answered a
// request by (own_rule, kerfkite/cam.hpp), for validation/server_route_parity.py, which compares it with the Python
// reference that does not share them.
http::Response Studio::handle(const http::Request &request) {
    static const bool parity = [] {
        const char *value = std::getenv("KERFKITE_PARITY");
        return value && *value && std::string(value) != "0";
    }();
    take_own_rules();
    http::Response response = dispatch(request);
    const std::vector<std::string> rules = take_own_rules();
    if (parity && !rules.empty()) {
        std::string named;
        for (const std::string &rule : rules) named += (named.empty() ? "" : "; ") + rule;
        response.set("X-KerfKite-Own-Rule", named);
    }
    return response;
}

http::Response Studio::dispatch(const http::Request &request) {
    // Local UI only. Host validation blocks DNS rebinding; the token prevents cross-origin writes.
    const std::string host = host_name(request);
    if (host != "127.0.0.1" && host != "localhost") return http::Response::error(403, "Use localhost on the Raspberry Pi");
    const bool post = request.method == "POST";
    if (post) {
        const std::string *given = request.header("x-studio-token");
        if (!same_secret(given ? *given : std::string(), token_)) return http::Response::error(403, "Reload this page before using controls");
    }
    const bool get = request.method == "GET" || request.method == "HEAD";
    const auto found = routes_.find((get ? std::string("GET") : request.method) + " " + request.path);
    try {
        if (found != routes_.end()) {
            if (post && kLockedRoutes.count(request.path)) {
                Hold machine(device_.lock);
                return found->second(request);
            }
            return found->second(request);
        }
        if (get) return static_file(request.path == "/" ? std::string("index.html") : request.path.substr(1));
        return http::Response::error(405, "The method is not allowed for the requested URL.");
    } catch (const HttpError &e) {
        return http::Response::error(e.status, e.what());
    } catch (const ValueError &e) {
        return http::Response::error(400, e.what());
    } catch (const RuntimeError &e) {
        return http::Response::error(400, e.what());
    } catch (const NotPorted &e) {
        return http::Response::error(400, e.what());
    } catch (const std::bad_alloc &) {
        return http::Response::error(500, "The request needs more memory than is available");
    } catch (const std::exception &e) {
        return http::Response::error(500, std::string("Internal error: ") + e.what());
    }
}

}  // namespace server
}  // namespace kerfkite
