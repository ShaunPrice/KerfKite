// Checks of the C++ workstation server (native/server), run by ctest: the workstation's guards, the machine
// routes on the simulator and on a synthetic Grbl controller (a scripted link, never a port), probe levelling,
// the phone pendant's armed start and its confirmation at the workstation, the design routes' own code, the
// key order of a request's parameters, and the HTTP server on the loopback interface.
//
//     kerfkite_server_selftest STATIC_DIR [PENDANT_TLS_RELAY]
//
// STATIC_DIR is studio/static; PENDANT_TLS_RELAY, when given, is kerfkite-pendant-tls, for the pendant's HTTPS.
// Nothing here opens a serial port or listens beyond 127.0.0.1.
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "design_extra.hpp"
#include "http.hpp"
#include "key_order.hpp"
#include "kerfkite/json.hpp"
#include "phone_monitor.hpp"
#include "studio.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using kerfkite::Json;
using namespace kerfkite::server;
namespace http = kerfkite::http;

namespace {

int failures = 0, checks = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        ++checks;                                                                           \
        if (!(condition)) {                                                                 \
            ++failures;                                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                                   \
    } while (0)

void check_text(const char *file, int line, const std::string &whole, const std::string &part) {
    ++checks;
    if (whole.find(part) == std::string::npos) {
        ++failures;
        std::fprintf(stderr, "%s:%d: \"%s\" does not contain \"%s\"\n", file, line, whole.substr(0, 400).c_str(), part.c_str());
    }
}
#define CHECK_TEXT(text, part) check_text(__FILE__, __LINE__, (text), (part))

std::string static_dir;

http::Request request(const std::string &method, const std::string &path, const std::string &body = std::string(),
                      const std::string &type = "application/json") {
    http::Request r;
    r.method = method;
    r.target = r.path = path;
    r.headers.emplace_back("host", "127.0.0.1:8765");
    if (!type.empty() && !body.empty()) r.headers.emplace_back("content-type", type);
    r.body = body;
    r.peer = "127.0.0.1";
    return r;
}

struct Client {
    Studio &studio;
    http::Response get(const std::string &path) { return studio.handle(request("GET", path)); }
    http::Response raw(const std::string &path, const std::string &body, const std::string &type = "application/json") {
        http::Request r = request("POST", path, body, type);
        r.headers.emplace_back("x-studio-token", studio.token());
        return studio.handle(r);
    }
    http::Response post(const std::string &path, const Json &body) { return raw(path, body.dump()); }
};

Json parsed(const http::Response &response) {
    try {
        return Json::parse(response.body);
    } catch (const std::exception &) {
        return Json();
    }
}

std::string error_of(const http::Response &response) {
    const Json value = parsed(response);
    return value.is_object() && value.contains("error") && value.at("error").is_string() ? value.at("error").get<std::string>() : std::string();
}

std::string error_of_value(PhoneMonitor &monitor, bool plaintext) {
    try {
        monitor.set_plaintext(Json(plaintext));
    } catch (const std::exception &e) {
        return e.what();
    }
    return std::string();
}

Json sample() {
    return Json::parse(R"({"mode": "router", "width": 100, "height": 100, "feed": 600, "power": 1000, "safe": 5, "depth": 1.2,
                          "step": 0.5, "plunge": 100, "passes": 1, "paths": [[[10, 10], [20, 10], [20, 20]]]})");
}

Json with(Json object, const std::string &key, Json value) {
    object[key] = std::move(value);
    return object;
}

bool wait_until(const std::function<bool()> &done, double seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

bool idle(Client &client) {
    return wait_until([&] {
        const Json state = parsed(client.get("/api/status"));
        return state.is_object() && state.at("active") == Json(false);
    });
}

Options options(bool hardware = false) {
    Options o;
    o.static_dir = static_dir;
    o.enable_hardware = hardware;
    o.monitor_port = 0;
    o.monitor_host = "127.0.0.1";
    return o;
}

// ---- a synthetic Grbl controller: a scripted link that answers as a laser-mode Grbl 1.1 does -------------

class SyntheticGrbl : public Link {
public:
    explicit SyntheticGrbl(std::vector<std::string> *written) : written_(written) {}
    void write(const std::string &bytes) override {
        std::lock_guard<std::mutex> hold(m_);
        written_->push_back(bytes);
        if (bytes == "?") {
            replies_.push_back("<Idle|MPos:0.000,0.000,0.000|FS:0,0>");
        } else if (bytes == "\x18") {
            replies_.push_back("Grbl 1.1h ['$' for help]");
        } else if (bytes == "$$\n") {
            for (const char *row : {"$13=0", "$20=0", "$22=1", "$30=1000", "$32=1", "$110=3000", "$111=3000", "$112=500", "$130=420",
                                    "$131=297", "$132=60", "ok"})
                replies_.push_back(row);
        } else if (bytes == "$I\n") {
            replies_.push_back("[VER:1.1h.20190825:]");
            replies_.push_back("[OPT:V,15,128]");
            replies_.push_back("ok");
        } else if (!bytes.empty() && bytes.back() == '\n') {
            replies_.push_back("ok");
        }
    }
    std::string readline(size_t) override {
        {
            std::lock_guard<std::mutex> hold(m_);
            if (!replies_.empty()) {
                std::string line = replies_.front() + "\n";
                replies_.pop_front();
                return line;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::string();
    }
    void close() override {}

private:
    std::vector<std::string> *written_;
    std::mutex m_;
    std::deque<std::string> replies_;
};

MachineOptions synthetic(std::vector<std::string> *written) {
    MachineOptions machine;
    machine.open = [written](const std::string &) { return std::unique_ptr<Link>(new SyntheticGrbl(written)); };
    machine.ports = [] { return std::vector<PortInfo>{{"KK-SYNTHETIC", "Synthetic Grbl controller"}}; };
    // Twenty times faster than the wall clock: the controller's two-second boot pause takes a tenth of one.
    const auto start = std::chrono::steady_clock::now();
    machine.clock = [start] { return 20 * std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    return machine;
}

// ---- the checks ------------------------------------------------------------------------------------------

void guards() {
    Studio studio(options());
    Client client{studio};
    http::Request evil = request("GET", "/api/session");
    evil.headers[0].second = "evil.example";
    CHECK(studio.handle(evil).status == 403);
    http::Request no_token = request("POST", "/api/connect", R"({"port": "simulator"})");
    CHECK(studio.handle(no_token).status == 403);
    CHECK_TEXT(error_of(studio.handle(no_token)), "Reload this page");
    const Json session = parsed(client.get("/api/session"));
    CHECK(session.at("token") == Json(studio.token()));
    CHECK(session.at("hardware_enabled") == Json(false));
    CHECK(client.get("/").status == 200);
    CHECK_TEXT(client.get("/").body, "<html");
    CHECK(client.get("/../server.py").status == 404);
    CHECK(client.get("/no-such-file.js").status == 404);
    CHECK(studio.handle(request("PUT", "/api/status")).status == 405);
}

void malformed_bodies() {
    Studio studio(options());
    Client client{studio};
    for (const char *path : {"/api/connect", "/api/run", "/api/command", "/api/head", "/api/arm", "/api/generate", "/api/disarm",
                             "/api/arm/check", "/api/arm/confirm", "/api/phone-monitor"}) {
        for (const auto &bad : std::vector<std::pair<std::string, std::string>>{
                 {"[]", "application/json"}, {"not json", "application/json"}, {"x", "text/plain"}}) {
            const http::Response response = client.raw(path, bad.first, bad.second);
            CHECK(response.status == 400);
            CHECK(!error_of(response).empty());
        }
    }
    CHECK_TEXT(error_of(client.post("/api/connect", Json::object())), "port");
    CHECK(client.post("/api/connect", Json::parse(R"({"port": 7})")).status == 400);
    client.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
    CHECK_TEXT(error_of(client.post("/api/command", Json::parse(R"({"bogus": 1})"))), "action");
    CHECK_TEXT(error_of(client.post("/api/command", Json::parse(R"({"action": "hold", "bogus": 1})"))), "bogus");
    // Strict JSON routes refuse an undeclared body with 415, as Flask's request.get_json() does.
    CHECK(client.raw("/api/assembly/solve", "x", "text/plain").status == 415);
    CHECK(client.raw("/api/fixture-collision", "{", "application/json").status == 400);
}

void simulator_run() {
    Studio studio(options());
    Client client{studio};
    CHECK(client.post("/api/generate", sample()).status == 200);
    CHECK(client.post("/api/run", Json::parse(R"({"confirmed": true, "project": null})")).status == 400);
    CHECK_TEXT(error_of(client.post("/api/connect", Json::parse(R"({"port": "/dev/ttyUSB0"})"))), "commissioning");
    CHECK(client.post("/api/connect", Json::parse(R"({"port": "simulator"})")).status == 200);
    Json run = Json::object();
    run["confirmed"] = false;
    run["project"] = sample();
    CHECK_TEXT(error_of(client.post("/api/run", run)), "Confirm work zero");
    run["confirmed"] = true;
    const http::Response started = client.post("/api/run", run);
    CHECK(started.status == 200);
    CHECK(parsed(started).at("lines").get<long long>() > 10);
    CHECK(idle(client));
    const Json state = parsed(client.get("/api/status"));
    CHECK(state.at("connected") == Json(true));
    CHECK(state.at("simulated") == Json(true));
    CHECK(state.at("progress") == Json(100));
    CHECK(static_cast<long long>(state.at("log").size()) == parsed(started).at("lines").get<long long>());
    CHECK(client.post("/api/command", Json::parse(R"({"action": "jog", "axis": "X", "distance": 5, "feed": 600})")).status == 200);
    CHECK(idle(client));
    CHECK(client.post("/api/disconnect", Json::object()).status == 200);
    CHECK(parsed(client.get("/api/status")).at("connected") == Json(false));
}

void synthetic_controller() {
    std::vector<std::string> written;
    {
        // Physical ports stay closed until the operator enables hardware.
        Studio closed(options(false), synthetic(&written));
        Client client{closed};
        CHECK_TEXT(error_of(client.post("/api/connect", Json::parse(R"({"port": "KK-SYNTHETIC"})"))), "commissioning");
        CHECK(written.empty());
    }
    Studio studio(options(true), synthetic(&written));
    Client client{studio};
    CHECK(parsed(client.get("/api/ports")).at(0).at("device") == Json("KK-SYNTHETIC"));
    CHECK(client.post("/api/connect", Json::parse(R"({"port": "KK-SYNTHETIC"})")).status == 200);
    CHECK(wait_until([&] { return parsed(client.get("/api/status")).at("ready") == Json(true); }));
    const Json machine = parsed(client.get("/api/machine"));
    CHECK(machine.at("travel").at("x") == Json(420));
    CHECK(machine.at("travel").at("y") == Json(297));
    CHECK(machine.at("laser_mode") == Json(true));
    // The controller is in laser mode ($32=1): a router job is refused before anything is sent.
    Json run = Json::object();
    run["confirmed"] = true;
    run["project"] = sample();
    const size_t before = written.size();
    CHECK_TEXT(error_of(client.post("/api/run", run)), "$32 does not match");
    run["project"] = with(sample(), "width", 500);
    run["project"]["mode"] = "laser";
    run["project"]["power"] = 500;
    CHECK_TEXT(error_of(client.post("/api/run", run)), "Work area exceeds");
    for (size_t i = before; i < written.size(); ++i) CHECK(written[i] == "?");  // status polls only
    // Machine readback writes nothing to the controller.
    CHECK(client.post("/api/disconnect", Json::object()).status == 200);
}

void levelling() {
    Studio studio(options());
    Client client{studio};
    Json engraving = sample();
    engraving["depth"] = 0.1;
    engraving["step"] = 0.1;
    engraving["stock_thickness"] = 1.6;
    engraving["paths"] = Json::parse("[[[10, 10], [60, 10], [60, 40], [10, 10]]]");
    Json probe = Json::object();
    probe["project"] = engraving;
    probe["spacing"] = 20;
    CHECK_TEXT(error_of(client.post("/api/level/probe", probe)), "Confirm that the probe");
    probe["confirmed"] = true;
    CHECK_TEXT(error_of(client.post("/api/level/collect", Json::object())), "Probe the stock surface first");
    client.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
    probe["project"] = with(engraving, "mode", "laser");
    CHECK_TEXT(error_of(client.post("/api/level/probe", probe)), "router jobs");
    probe["project"] = engraving;
    CHECK(client.post("/api/level/probe", probe).status == 200);
    CHECK(idle(client));
    const http::Response collected = client.post("/api/level/collect", Json::object());
    CHECK(collected.status == 200);
    const Json level = parsed(collected);
    CHECK(level.at("surface").is_object());
    CHECK(level.at("surface").at("highest").get<double>() > level.at("surface").at("lowest").get<double>());
    Json program = Json::object();
    program["project"] = engraving;
    const Json levelled = parsed(client.post("/api/level/program", program));
    CHECK(levelled.at("lines").size() > 20);
    Json run = Json::object();
    run["confirmed"] = true;
    run["leveled"] = true;
    run["project"] = engraving;
    CHECK(parsed(client.post("/api/run", run)).at("lines") == Json(static_cast<long long>(levelled.at("lines").size())));
    CHECK(idle(client));
    // Setting work zero forgets the surface.
    CHECK(client.post("/api/command", Json::parse(R"({"action": "zero"})")).status == 200);
    CHECK(parsed(client.get("/api/level")).at("surface").is_null());
    CHECK_TEXT(error_of(client.post("/api/run", run)), "Probe the stock surface before");
}

void phone_start() {
    Studio studio(options());
    Client client{studio};
    client.post("/api/connect", Json::parse(R"({"port": "simulator"})"));
    Json arm = Json::object();
    arm["project"] = sample();
    arm["confirmed"] = true;
    CHECK_TEXT(error_of(client.post("/api/arm", arm)), "Reload the updated workstation");
    arm["phone_arm_challenge"] = true;
    const Json armed = parsed(client.post("/api/arm", arm));
    const std::string arm_id = armed.at("arm_id").get<std::string>();
    CHECK(arm_id.size() >= 40);
    Json check = Json::object();
    check["arm_id"] = arm_id;
    CHECK(parsed(client.post("/api/arm/check", check)).at("armed") == Json(true));
    check["arm_id"] = "another tab";
    CHECK(parsed(client.post("/api/arm/check", check)).at("armed") == Json(false));

    // The phone pairs with the listener's bearer token and starts the armed job; the start waits until the
    // workstation that armed it confirms the challenge.
    const Json pairing = studio.monitor().enable();
    const std::string bearer = "Bearer " + pairing.at("token").get<std::string>();
    PhoneMonitor &monitor = studio.monitor();
    const auto phone = [&](const std::string &method, const std::string &path, const std::string &body) {
        http::Request r = request(method, path, body);
        r.headers.emplace_back("authorization", bearer);
        return monitor.handle(r);
    };
    CHECK(monitor.handle(request("GET", "/api/status")).status == 401);
    const Json phone_state = parsed(phone("GET", "/api/status", ""));
    CHECK(phone_state.at("armed") == Json(true));
    CHECK(!phone_state.contains("settings"));  // the phone's status is an allowlist
    const std::string epoch = phone_state.at("control_epoch").get<std::string>();
    std::atomic<int> phone_status{0};
    std::thread start([&] {
        Json action = Json::object();
        action["action"] = "start";
        action["control_epoch"] = epoch;
        phone_status = phone("POST", "/api/action", action.dump()).status;
    });
    check["arm_id"] = arm_id;
    std::string challenge;
    CHECK(wait_until([&] {
        const Json seen = parsed(client.post("/api/arm/check", check));
        if (seen.contains("challenge") && seen.at("challenge").is_string()) challenge = seen.at("challenge").get<std::string>();
        return !challenge.empty();
    }, 5));
    Json confirm = Json::object();
    confirm["arm_id"] = arm_id;
    confirm["challenge"] = challenge;
    CHECK(client.post("/api/arm/confirm", confirm).status == 200);
    start.join();
    CHECK(phone_status == 200);
    CHECK(idle(client));
    CHECK(parsed(client.get("/api/status")).at("progress") == Json(100));
    CHECK(parsed(client.post("/api/arm/check", check)).at("armed") == Json(false));

    // Without the workstation's confirmation the phone's start expires and nothing runs.
    const std::string second = parsed(client.post("/api/arm", arm)).at("arm_id").get<std::string>();
    const size_t ran = parsed(client.get("/api/status")).at("log").size();
    Json action = Json::object();
    action["action"] = "start";
    action["control_epoch"] = parsed(phone("GET", "/api/status", "")).at("control_epoch");
    const http::Response expired = phone("POST", "/api/action", action.dump());
    CHECK(expired.status == 400);
    CHECK(parsed(client.get("/api/status")).at("log").size() == ran);
    Json disarm = Json::object();
    disarm["arm_id"] = second;
    client.post("/api/disarm", disarm);
    // Disabling the listener ends the pairing.
    studio.monitor().disable();
    CHECK(phone("GET", "/api/status", "").status == 401);
}

void design_routes() {
    Studio studio(options());
    Client client{studio};
    const Json square = Json::parse("[[0, 0], [20, 0], [20, 20], [0, 20], [0, 0]]");
    const Json overlap = Json::parse("[[10, 10], [30, 10], [30, 30], [10, 30], [10, 10]]");
    const auto area = [&](const char *action, double distance) {
        Json data = Json::object();
        data["action"] = action;
        data["distance"] = distance;
        Json first = Json::object(), second = Json::object();
        first["paths"] = Json::array({square});
        second["paths"] = Json::array({overlap});
        data["objects"] = std::string(action) == "offset" ? Json::array({first}) : Json::array({first, second});
        const Json paths = parsed(client.post("/api/geometry", data)).at("paths");
        double total = 0;  // holes run the other way: the signed areas add up to the region's
        for (const Json &ring : paths) {
            double twice = 0;
            for (size_t i = 0; i + 1 < ring.size(); ++i)
                twice += ring[i][0].get<double>() * ring[i + 1][1].get<double>() - ring[i + 1][0].get<double>() * ring[i][1].get<double>();
            total += twice / 2;
        }
        return -total;  // outer rings are clockwise, as cutting needs
    };
    CHECK(std::fabs(area("union", 0) - 700) < 1e-9);
    CHECK(std::fabs(area("difference", 0) - 300) < 1e-9);
    CHECK(std::fabs(area("intersection", 0) - 100) < 1e-9);
    CHECK(std::fabs(area("offset", 1) - (400 + 80 + 3.14159265)) < 0.01);
    CHECK_TEXT(error_of(client.post("/api/geometry", Json::parse(R"({"action": "slice", "objects": [{"paths": [[[0,0],[1,0],[1,1],[0,0]]]}]})"))),
               "Choose union");
    CHECK_TEXT(error_of(client.post("/api/geometry", Json::parse(R"({"action": "union", "objects": [{"paths": [[["a","b"],[1,2],[3,4],["a","b"]]]}]})"))),
               "XY");

    // An extrusion is a closed mesh with its holes.
    Json extrude = Json::object();
    extrude["paths"] = Json::array({square, Json::parse("[[5, 5], [5, 15], [15, 15], [15, 5], [5, 5]]")});
    extrude["height"] = 4;
    const Json prism = parsed(client.post("/api/extrude", extrude));
    CHECK(prism.at("height") == Json(4));
    CHECK_TEXT(prism.at("stl").get<std::string>(), "solid cnc_studio_text\nfacet normal 0 0 0\nouter loop\nvertex ");
    CHECK(prism.at("triangles").get<long long>() == 32);  // 8 cap triangles a side and 2 for each of the 8 walls
    CHECK_TEXT(error_of(client.post("/api/extrude", Json::parse(R"({"paths": [1]})"))), "Contours must be lists");

    // A sketch: the parameters resolve in the order the request wrote them, which decides which fault shows.
    const std::string sketch = R"({"points": [[0, 0], ["w", 0], ["w", "h"], [0, "h"]], "parameters": {"w": "h / 0", "h": "zz"}})";
    CHECK_TEXT(error_of(client.raw("/api/solid-cad/sketch-points", sketch)), "Unknown parameter: zz");
    const std::string reordered = R"({"points": [[0, 0], ["w", 0], ["w", "h"], [0, "h"]], "parameters": {"h": 5, "w": "h / 0"}})";
    CHECK_TEXT(error_of(client.raw("/api/solid-cad/sketch-points", reordered)), "Division by zero");
    const Json solved = parsed(client.raw("/api/solid-cad/sketch-points",
                                           R"({"points": [[0, 0], ["w", 0], ["w", 5], [0, 5]], "parameters": {"w": 12}, "fillets": [0, 1, 1, 0]})"));
    CHECK(solved.at("points")[1][0] == Json(12));
    CHECK(solved.at("outline").size() > 4);
    CHECK(solved.at("diagnostics").at("dof") == Json(6));  // eight coordinates, two driven by w

    // The binary stock preview.
    const auto packet = [](uint32_t count, const char *magic, uint32_t shape) {
        std::string bytes(magic, 8);
        const auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) bytes += static_cast<char>((v >> (8 * k)) & 0xff); };
        const auto f64 = [&](double v) {
            uint64_t bits = 0;
            std::memcpy(&bits, &v, 8);
            for (int k = 0; k < 8; ++k) bytes += static_cast<char>((bits >> (8 * k)) & 0xff);
        };
        u32(count), u32(16), u32(shape);
        for (double v : {0.0, 0.0, 8.0, 8.0, 1.0, 60.0, 3.0}) f64(v);
        for (uint32_t i = 0; i < count; ++i)
            for (double v : {0.0, 2.0, 8.0, 2.0, -1.0, 0.0, -2.0}) f64(v);
        return bytes;
    };
    const std::string stock = "application/vnd.kerfkite.stock-v1";
    const http::Response preview = client.raw("/api/simulate", packet(1, "KKSTOCK1", 0), stock);
    CHECK(preview.status == 200);
    CHECK(parsed(preview).at("heights").size() == 16);
    CHECK_TEXT(error_of(client.raw("/api/simulate", packet(1, "KKOTHER0", 0), stock)), "Invalid stock preview header");
    CHECK_TEXT(error_of(client.raw("/api/simulate", packet(1, "KKSTOCK1", 9), stock)), "Invalid stock preview header");
    CHECK_TEXT(error_of(client.raw("/api/simulate", packet(1, "KKSTOCK1", 0).substr(0, 70), stock)), "Truncated");
    CHECK_TEXT(error_of(client.raw("/api/simulate", packet(1, "KKSTOCK1", 0) + "x", stock)), "length differs");

    // STEP exchange is not part of this program.
    CHECK_TEXT(error_of(client.raw("/api/solid-cad/step/export", R"({"surface": {}})")), "Open CASCADE");
    // The example assembly solves with its shaft in the bore.
    Json assembly = Json::object();
    assembly["assembly"] = parsed(client.get("/api/assembly/example"));
    assembly["mesh"] = false;
    const Json parts = parsed(client.post("/api/assembly/solve", assembly)).at("parts");
    CHECK(parts.at("Shaft").at("bounds")[2] == Json(-4));
}

void key_order_reading() {
    const KeyOrder text = key_order(R"({"b": 1, "a": {"z": 0, "y": [1, {"k": 2}], "z": 3}, "é": null})");
    CHECK(text.order() == Json::parse(R"(["b", "a", "é"])"));
    CHECK(text.member("a")->order() == Json::parse(R"(["z", "y"])"));
    CHECK(order_at(text, {"a", "y"}).is_null());
    CHECK(text.member("a")->member("y")->item(1)->order() == Json::parse(R"(["k"])"));
    CHECK(!key_order("{not json").object);
}

// ---- the pendant's HTTPS through the TLS relay ---------------------------------------------------------------

std::string relay_program;

std::string round_trip(int port, const std::string &raw);

void pendant_tls() {
    {
        // Without the relay, pairing is HTTP and says it cannot encrypt.
        Studio studio(options());
        const Json info = studio.monitor().info();
        CHECK(info.at("can_encrypt") == Json(false));
        const Json pairing = studio.monitor().enable();
        CHECK(pairing.at("scheme") == Json("http"));
        CHECK(pairing.at("encryption") == Json("none"));
        CHECK(pairing.at("fingerprint").is_null());
        studio.monitor().disable();
    }
    if (relay_program.empty()) {
        std::printf("pendant TLS: no relay program given, HTTPS checks skipped\n");
        return;
    }
    Options o = options();
    o.pendant_tls = relay_program;
    Studio studio(o);
    CHECK(studio.monitor().info().at("can_encrypt") == Json(true));
    const Json pairing = studio.monitor().enable();
    CHECK(pairing.at("scheme") == Json("https"));
    CHECK(pairing.at("encryption") == Json("pinned"));
    const std::string fingerprint = pairing.at("fingerprint").get<std::string>();
    CHECK(fingerprint.size() == 64 && fingerprint.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK_TEXT(pairing.at("fragment").get<std::string>(), "&fp=" + fingerprint);
    const int port = static_cast<int>(pairing.at("port").get<long long>());
    CHECK(port > 0);
    // The relay speaks TLS only: plain HTTP on the phone's port gets no answer from the monitor app.
    const std::string plain = round_trip(port, "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(plain.find("HTTP/1.1 200") == std::string::npos && plain.find("401") == std::string::npos);
    // Encryption cannot change while a pairing is live; a second pairing has its own certificate.
    CHECK_TEXT(error_of_value(studio.monitor(), true), "Disable phone pairing");
    studio.monitor().disable();
    CHECK(round_trip(port, "GET / HTTP/1.1\r\n\r\n").empty());  // the relay ended with the pairing
    const Json again = studio.monitor().enable();
    CHECK(again.at("fingerprint").get<std::string>() != fingerprint);
    studio.monitor().disable();
    studio.monitor().set_plaintext(Json(true));
    CHECK(studio.monitor().enable().at("encryption") == Json("none"));
    studio.monitor().disable();
}

// ---- the HTTP server on the loopback interface -------------------------------------------------------------

std::string round_trip(int port, const std::string &raw) {
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string reply;
    if (::connect(s, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0) {
        ::send(s, raw.data(), static_cast<int>(raw.size()), 0);
        char buffer[4096];
        for (;;) {
            const auto n = ::recv(s, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            reply.append(buffer, static_cast<size_t>(n));
        }
    }
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
    return reply;
}

void http_server() {
    Studio studio(options());
    http::Server server("127.0.0.1", 0);
    std::thread worker([&] {
        server.serve([&](const http::Request &r) { return studio.handle(r); }, [&](const http::Request &r) { return studio.body_limit(r); });
    });
    const int port = server.port();
    const std::string host = "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    CHECK_TEXT(round_trip(port, "GET /api/session HTTP/1.1\r\n" + host + "\r\n"), "HTTP/1.1 200 OK");
    CHECK_TEXT(round_trip(port, "GET /api/session HTTP/1.1\r\n" + host + "\r\n"), "\"token\":\"" + studio.token() + "\"");
    const std::string head = round_trip(port, "HEAD /app.js HTTP/1.1\r\n" + host + "\r\n");
    CHECK_TEXT(head, "Content-Type: text/javascript");
    CHECK(head.find("\r\n\r\n") + 4 == head.size());  // no body
    const std::string project = sample().dump();
    std::string chunked = "POST /api/generate HTTP/1.1\r\n" + host + "X-Studio-Token: " + studio.token() +
                          "\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n";
    char size[32];
    std::snprintf(size, sizeof size, "%zx", project.size() / 2);
    chunked += std::string(size) + "\r\n" + project.substr(0, project.size() / 2) + "\r\n";
    std::snprintf(size, sizeof size, "%zx", project.size() - project.size() / 2);
    chunked += std::string(size) + "\r\n" + project.substr(project.size() / 2) + "\r\n0\r\n\r\n";
    CHECK_TEXT(round_trip(port, chunked), "\"lines\":[\"G21\"");
    CHECK_TEXT(round_trip(port, "POST /api/generate HTTP/1.1\r\n" + host + "Content-Length: 99999999\r\n\r\n"), "413");
    CHECK_TEXT(round_trip(port, "GET / HTTP/1.1\r\n" + host + "X-Long: " + std::string(70000, 'a') + "\r\n\r\n"), "431");
    CHECK_TEXT(round_trip(port, "NONSENSE\r\n\r\n"), "400");
    // A TLS handshake sent to the HTTP listener is refused at once: a pinned client then fails fast.
    CHECK_TEXT(round_trip(port, std::string("\x16\x03\x01\x00\x05hello", 10)), "400");
    server.stop();
    worker.join();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: kerfkite_server_selftest STATIC_DIR [PENDANT_TLS_RELAY]\n");
        return 2;
    }
    static_dir = argv[1];
    if (argc == 3) relay_program = argv[2];
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    guards();
    malformed_bodies();
    simulator_run();
    synthetic_controller();
    levelling();
    phone_start();
    design_routes();
    key_order_reading();
    pendant_tls();
    http_server();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
