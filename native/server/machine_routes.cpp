// The machine routes of studio/server.py: status and readback, the phone pendant's pairing, probe levelling,
// the phone's armed preview, connection, the job, the head mode and the controller commands. Each machine route
// that changes the controller holds its lock for the whole request (see Studio::handle).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "kerfkite/leveling.hpp"
#include "kerfkite/rotary.hpp"
#include "machine_setup.hpp"
#include "phone_monitor.hpp"
#include "protocols.hpp"
#include "studio.hpp"

namespace kerfkite {
namespace server {

namespace {

using json = Json;
using http::Request;
using http::Response;

const double kProbeSeconds = 10;  // a probe line is acknowledged only when its motion ends

const char *kProfiles = R"([
  {"id": "generic-grbl", "name": "Generic GRBL · 300 × 180 mm", "controller": "GRBL 1.1 compatible",
   "firmware": "Verify on controller", "width": 300, "height": 180, "z_travel": null, "heads": [], "commissioned": false,
   "head_mounting": "interchangeable", "unverified": ["Machine travel, origin, installed head and clearance"]},
  {"id": "generic-xpro-v5", "name": "xPRO V5 · example workspace", "controller": "xPRO V5", "firmware": "GRBL-ESP32 profile",
   "width": 600, "height": 400, "z_travel": null, "heads": [], "commissioned": false, "head_mounting": "interchangeable",
   "unverified": ["Configure actual machine envelope before preparing a job"]}
])";

Response ok(json extra = json::object()) {
    json out = json::object();
    out["ok"] = true;
    for (const auto &item : extra.items()) out[std::string(item.key())] = item.value();
    return json_response(out);
}

// float(value) of a project field, as validate_machine takes it.
double as_float(const json *value) {
    if (!value) throw ValueError("A project field is missing");
    if (value->is_boolean()) return value->get<bool>() ? 1 : 0;
    if (value->is_number()) return value->get<double>();
    if (value->is_string()) {
        char *end = nullptr;
        const std::string text = value->get<std::string>();
        const double v = std::strtod(text.c_str(), &end);
        if (end != text.c_str()) return v;
    }
    throw ValueError("could not convert a project field to float");
}

bool flag(const json &state, const char *name) {
    const json *value = member(state, name);
    return value && value->is_boolean() && value->get<bool>();
}

std::string text_of(const json *value, const char *otherwise) {
    return value && value->is_string() ? value->get<std::string>() : std::string(otherwise);
}

// What prepared() makes of a request: the program to run, the planar job's segments and bounds, and the rotary
// setup of a rotary job.
struct Prepared {
    Job planar;
    std::vector<std::string> lines;
    json rotary;  // null for a planar job
};

Prepared prepared(const json &data) {
    const json *project = member(data, "project");
    if (!project || !project->is_object()) throw ValueError("Supply a design project");
    if (truthy(member(*project, "rotary")) && !data.contains("rotary"))
        throw ValueError("Preview rotary CAM and explicitly confirm the rotary setup before running");
    Prepared out;
    if (!data.contains("rotary")) {
        out.planar = generate(*project);
        out.lines = out.planar.lines;
        return out;
    }
    const json *confirmed = member(data, "rotary_confirmed");
    if (!confirmed || !confirmed->is_boolean() || !confirmed->get<bool>())
        throw ValueError("Confirm independent rotary drive, calibration in degrees, tool axes, seam zero and fixture clearance.");
    RotaryJob job = rotary_job(*project, data.at("rotary"));
    out.planar = std::move(job.planar);
    out.lines = std::move(job.lines);
    out.rotary = std::move(job.rotary);
    return out;
}

double lowest_of(const Job &job) {
    double lowest = 0.0;
    bool any = false;
    for (const Segment &s : job.segments) {
        const double here = std::min(s.end_z, s.start_z);
        lowest = any ? std::min(lowest, here) : here;
        any = true;
    }
    return lowest;
}

}  // namespace

// studio.rotary.validate_rotary_machine.
void validate_rotary_machine(const Json &rotary, const Json &status) {
    const std::string post = text_of(member(rotary, "postprocessor"), ""), drive = text_of(member(rotary, "output_axis"), "");
    if ((post != "grblhal" && post != "xpro-v5") || post != text_of(member(status, "protocol"), ""))
        throw ValueError("Rotary stream requires the selected grblHAL or independent-A xPRO V5 protocol");
    const std::vector<std::string> axes = controller_rotary_axes(post, status);
    if (std::find(axes.begin(), axes.end(), drive) == axes.end())
        throw ValueError(
            "Rotary axis unavailable. Connect the matching simulator or verify independent rotary firmware, position and calibration readback.");
    if (flag(status, "simulated")) return;
    const json *settings_field = member(status, "settings");
    const json settings = settings_field && settings_field->is_object() ? *settings_field : json::object();
    std::vector<std::pair<std::string, const json *>> checked = {{text_of(member(rotary, "axial_output_axis"), ""), member(rotary, "axial_bounds")},
                                                                 {text_of(member(rotary, "radial_output_axis"), ""), member(rotary, "radial_bounds")}};
    if (truthy(member(settings, "20"))) checked.emplace_back(drive, member(rotary, "angle_bounds"));
    for (const auto &item : checked) {
        const std::string &axis = item.first;
        const size_t index = std::string("XYZABC").find(axis);
        const json *travel = index == std::string::npos || axis.size() != 1 ? nullptr : member(settings, std::to_string(130 + index).c_str());
        if (!travel || !travel->is_number() || !std::isfinite(travel->get<double>()) || travel->get<double>() <= 0)
            throw ValueError("Missing configured travel readback for " + axis);
        const double lo = item.second->at(0).get<double>(), hi = item.second->at(1).get<double>();
        if (std::max(hi, 0.0) - std::min(lo, 0.0) > travel->get<double>() + 1e-6) throw ValueError("Rotary job exceeds configured " + axis + " travel");
    }
}

namespace {

// validate_machine(project, job): the job against the connected controller's readback; the status.
json validate_machine(Studio &studio, const json &project, const Prepared &job) {
    const json status = studio.device().status();
    const json *protocol_field = member(project, "protocol");
    const Protocol &profile = protocol_of(protocol_field);
    if (!profile.live_transport) throw ValueError("File export only; live execution is unavailable for " + profile.name);
    const json project_protocol = protocol_field ? *protocol_field : json("grbl");
    const json *status_protocol = member(status, "protocol");
    if (!(project_protocol == (status_protocol ? *status_protocol : json("grbl")))) throw ValueError("Job protocol differs from connected controller");
    const bool rotary = truthy(&job.rotary);
    if (rotary) validate_rotary_machine(job.rotary, status);
    const json *settings_field = member(status, "settings");
    const json settings = settings_field && settings_field->is_object() ? *settings_field : json::object();
    const std::string mode = text_of(member(project, "mode"), "");
    if (!flag(status, "simulated") && profile.family == "grbl") {
        for (const auto &item : settings.items())
            if (!std::isfinite(as_float(&item.value()))) throw ValueError("Invalid controller settings readback; reconnect");
        const json *laser = member(settings, "32") ? member(settings, "32") : member(settings, "$32");
        const json *maximum = member(settings, "30") ? member(settings, "30") : member(settings, "$30");
        if (!laser || laser->is_null() || !maximum || maximum->is_null()) throw ValueError("Wait for GRBL $30/$32 readback before running");
        if (rotary && profile.id == "xpro-v5") {
            const json *spindle = member(status, "spindle_type");
            if (!truthy(spindle) || (text_of(spindle, "") == "LASER") != (mode == "laser"))
                throw ValueError("Verify xPRO V5 Spindle/Type for the mounted head and reconnect for readback");
            if (mode == "laser") {
                maximum = member(settings, "Laser/FullPower");
                if (!maximum || maximum->is_null() || as_float(maximum) <= 0)
                    throw ValueError("Wait for xPRO V5 Laser/FullPower readback before running a rotary laser job");
            }
        }
        const double most = as_float(maximum);
        if (most <= 0) throw ValueError("Invalid controller maximum power readback");
        if (as_float(laser) != (mode == "laser" ? 1 : 0)) throw ValueError("Controller $32 does not match the selected head; configure GRBL separately");
        if (as_float(member(project, "power")) > most) throw ValueError("Power/RPM exceeds controller $30 or active laser full-power limit");
        for (const auto &pair : {std::make_pair("width", "130"), std::make_pair("height", "131")})
            if (!rotary && member(settings, pair.second) && as_float(member(project, pair.first)) > as_float(member(settings, pair.second)))
                throw ValueError("Work area exceeds configured controller travel");
        if (const json *operations = member(project, "operations"))
            if (operations->is_array())
                for (const json &operation : *operations) {
                    const json *power = member(operation, "power");
                    if (as_float(power ? power : member(project, "power")) > most)
                        throw ValueError("Operation power exceeds controller $30 or active laser full-power limit");
                }
    }
    if (!flag(status, "simulated") && mode != "laser") {
        double lowest = lowest_of(job.planar);
        if (rotary) lowest = std::min(lowest, job.rotary.at("radial_bounds").at(0).get<double>());
        const double depth = std::max(0.0, -lowest);
        const double stock = number(member(project, "stock_thickness"), "Stock thickness", 0.01, 200);
        if (depth > stock) throw ValueError("Cut exceeds entered stock thickness");
        const json *project_safe = member(project, "safe");
        double safe = project_safe ? as_float(project_safe) : 5;
        if (const json *operations = member(project, "operations"))
            if (operations->is_array())
                for (const json &operation : *operations) {
                    const json *op_safe = member(operation, "safe");
                    safe = std::max(safe, op_safe ? as_float(op_safe) : project_safe ? as_float(project_safe) : 5);
                }
        if (!rotary && member(settings, "132") && depth + safe > as_float(member(settings, "132")))
            throw ValueError("Depth plus clearance exceeds configured Z travel");
    }
    return status;
}

const json &levelling_project(const json &data) {
    const json *project = member(data, "project");
    if (!project || !project->is_object()) throw ValueError("Supply a design project");
    if (!(text_of(member(*project, "mode"), "") == "router")) throw ValueError("Probe levelling applies to router jobs");
    if (truthy(member(*project, "rotary")) || data.contains("rotary")) throw ValueError("Probe levelling does not apply to rotary jobs");
    if (protocol_of(member(*project, "protocol")).family != "grbl") throw ValueError("Probe levelling needs a Grbl-family controller (G38.2)");
    return *project;
}

std::vector<std::string> levelled(Studio &studio, const Job &job, const json &project, const json &status) {
    json surface;
    {
        std::lock_guard<std::mutex> hold(studio.level_mutex);
        surface = studio.level_surface;
    }
    if (surface.is_null()) throw ValueError("Probe the stock surface before running a levelled job");
    json request = json::object();
    json lines = json::array();
    for (const std::string &line : job.lines) lines.push_back(line);
    request["lines"] = lines;
    request["height_map"] = surface;
    const json result = levelling_call("level", request);
    if (!flag(status, "simulated")) {
        const double lowest = lowest_of(job);
        const double stock = number(member(project, "stock_thickness"), "Stock thickness", 0.01, 200);
        double least = 0;
        bool any = false;
        for (const json &offset : surface.at("offsets")) {
            least = any ? std::min(least, offset.get<double>()) : offset.get<double>();
            any = true;
        }
        if (-(lowest + least) > stock) throw ValueError("Levelled cut exceeds entered stock thickness");
    }
    std::vector<std::string> out;
    for (const json &line : result.at("lines")) out.push_back(line.get<std::string>());
    return out;
}

json level_state(Studio &studio) {
    json plan, surface;
    {
        std::lock_guard<std::mutex> hold(studio.level_mutex);
        plan = studio.level_plan;
        surface = studio.level_surface;
    }
    json out = json::object();
    out["available"] = true;
    if (plan.is_null()) {
        out["plan"] = json();
    } else {
        json p = json::object();
        p["grid"] = plan.at("grid");
        p["points"] = plan.at("points");
        const json status = studio.device().status();
        const json *probed = member(status, "probe_count");
        p["probed"] = probed ? *probed : json(0);
        out["plan"] = p;
    }
    if (surface.is_null()) {
        out["surface"] = json();
    } else {
        json s = surface;
        double lowest = 0, highest = 0;
        bool any = false;
        for (const json &offset : surface.at("offsets")) {
            const double v = offset.get<double>();
            lowest = any ? std::min(lowest, v) : v;
            highest = any ? std::max(highest, v) : v;
            any = true;
        }
        s["lowest"] = lowest;
        s["highest"] = highest;
        out["surface"] = s;
    }
    return out;
}

std::string owner_arm_id(const json &data) {
    const json *arm_id = member(data, "arm_id");
    if (!arm_id || !arm_id->is_string() || arm_id->get<std::string>().empty()) throw ValueError("Use the workstation tab that armed the preview.");
    return arm_id->get<std::string>();
}

bool is_true(const json *value) { return value && value->is_boolean() && value->get<bool>(); }

}  // namespace

void add_cam_routes(RouteTable &routes, bool strict) {
    const std::string profiles = json::parse(kProfiles).dump();
    routes.add("GET", "/api/profiles", [profiles](const Request &) { return http::Response::json(profiles); });
    routes.add("POST", "/api/generate", [strict](const Request &request) {
        const json project = strict ? strict_json(request) : payload(request);
        // The core writes the job as JSON itself: for a large job that is most of the time of the request.
        return http::Response::json(to_json(generate(project)).dump());
    });
}

void add_machine_routes(Studio &studio) {
    Studio *s = &studio;
    studio.add("GET", "/api/status", [s](const Request &) { return json_response(s->device().status()); });
    studio.add("GET", "/api/machine", [s](const Request &) {
        // What the controller reports about its machine. Reading only: nothing is written to it.
        return json_response(describe_machine(s->device().status()));
    });
    studio.add("GET", "/api/ports", [s](const Request &) { return json_response(s->device().ports()); });

    studio.add("POST", "/api/phone-monitor", [s](const Request &request) {
        const json data = payload(request);
        const std::string action = text_of(member(data, "action"), "");
        if (action != "status" && action != "enable" && action != "disable") throw ValueError("Select a phone pairing action.");
        if (action == "enable" && data.contains("plaintext")) s->monitor().set_plaintext(data.at("plaintext"));
        const json info = action == "status" ? s->monitor().info() : action == "enable" ? s->monitor().enable() : s->monitor().disable();
        Response response = json_response(info);
        response.set("Cache-Control", "no-store");
        return response;
    });

    studio.add("GET", "/api/level", [s](const Request &) { return json_response(level_state(*s)); });
    studio.add("POST", "/api/level/probe", [s](const Request &request) {
        const json data = payload(request);
        if (!is_true(member(data, "confirmed")))
            throw ValueError("Confirm that the probe is connected and tested, the spindle is stopped and work zero is set at the stock surface");
        const json &project = levelling_project(data);
        Prepared job;
        job.planar = generate(project);
        job.lines = job.planar.lines;
        validate_machine(*s, project, job);
        json request_plan = json::object();
        for (const char *key : {"spacing", "margin", "clearance", "depth", "feed", "plate"})
            if (data.contains(key)) request_plan[key] = data.at(key);
        request_plan["bounds"] = json::array({job.planar.bounds[0], job.planar.bounds[1], job.planar.bounds[2], job.planar.bounds[3]});
        request_plan["width"] = project.at("width");
        request_plan["height"] = project.at("height");
        request_plan["zero"] = true;
        const json plan = levelling_call("probe-plan", request_plan);
        if ((plan.at("clearance").get<double>() + plan.at("depth").get<double>()) / plan.at("feed").get<double>() * 60 > kProbeSeconds)
            throw ValueError("Each probe must finish within 10 seconds; raise the probe feed or lower the clearance and depth");
        s->jobs().disarm();
        s->forget_level();
        std::vector<std::string> lines;
        for (const json &line : plan.at("lines")) lines.push_back(line.get<std::string>());
        s->device().start(lines);
        {
            std::lock_guard<std::mutex> hold(s->level_mutex);
            s->level_plan = plan;
        }
        return ok(level_state(*s));
    });
    studio.add("POST", "/api/level/collect", [s](const Request &request) {
        const json data = payload(request);
        json plan;
        {
            std::lock_guard<std::mutex> hold(s->level_mutex);
            plan = s->level_plan;
        }
        if (plan.is_null()) throw ValueError("Probe the stock surface first");
        const json status = s->device().status();
        if (flag(status, "active")) throw ValueError("Probing is still running");
        if (truthy(member(status, "error"))) throw ValueError("Probing did not finish: " + text_of(member(status, "error"), ""));
        const std::vector<std::string> reports = s->device().probe_reports();
        const size_t points = plan.at("points").size();
        if (reports.size() != points)
            throw ValueError("Probing stopped after " + std::to_string(reports.size()) + " of " + std::to_string(points) + " points; probe again");
        json request_map = json::object();
        request_map["grid"] = plan.at("grid");
        request_map["order"] = plan.at("order");
        json given = json::array();
        for (const std::string &report : reports) given.push_back(report);
        request_map["reports"] = given;
        if (const json *limit = member(data, "limit"))
            if (!limit->is_null()) request_map["limit"] = *limit;
        const json surface = levelling_call("height-map", request_map);
        {
            std::lock_guard<std::mutex> hold(s->level_mutex);
            s->level_surface = surface;
        }
        return ok(level_state(*s));
    });
    studio.add("POST", "/api/level/clear", [s](const Request &) {
        s->forget_level();
        return ok(level_state(*s));
    });
    studio.add("POST", "/api/level/program", [s](const Request &request) {
        const json data = payload(request);
        const json &project = levelling_project(data);
        const Job job = generate(project);
        json simulated = json::object();
        simulated["simulated"] = true;
        json lines = json::array();
        for (const std::string &line : levelled(*s, job, project, simulated)) lines.push_back(line);
        json out = json::object();
        out["lines"] = lines;
        return json_response(out);
    });

    studio.add("POST", "/api/arm", [s](const Request &request) {
        const json data = payload(request);
        if (!is_true(member(data, "confirmed"))) throw ValueError("Confirm setup at the Pi before arming");
        if (!is_true(member(data, "phone_arm_challenge"))) throw ValueError("Reload the updated workstation before arming a phone Start.");
        auto job = std::make_shared<Prepared>(prepared(data));
        const json project = data.at("project");
        validate_machine(*s, project, *job);
        const std::string arm_id = s->jobs().arm(job->lines, [s, project, job] { validate_machine(*s, project, *job); }, true);
        json extra = json::object();
        extra["arm_id"] = arm_id;
        return ok(extra);
    });
    studio.add("POST", "/api/arm/check", [s](const Request &request) { return json_response(s->jobs().check_preview(owner_arm_id(payload(request)))); });
    studio.add("POST", "/api/arm/confirm", [s](const Request &request) {
        const json data = payload(request);
        s->jobs().confirm_preview(owner_arm_id(data), member(data, "challenge"));
        return ok();
    });
    studio.add("POST", "/api/disarm", [s](const Request &request) {
        json extra = json::object();
        extra["removed"] = s->jobs().disarm(owner_arm_id(payload(request)));
        return ok(extra);
    });

    studio.add("POST", "/api/connect", [s](const Request &request) {
        const json data = payload(request, {"port"});
        const json *port = member(data, "port");
        if (!port->is_string() || port->get<std::string>().empty()) throw ValueError("Select a controller port");
        if (port->get<std::string>() != "simulator" && !s->options().enable_hardware)
            throw ValueError(
                "Physical ports are disabled. Start with --enable-hardware only after completing machine setup and commissioning; otherwise use Simulator.");
        s->jobs().invalidate_controls();
        s->forget_level();
        const json *protocol = member(data, "protocol");
        if (protocol && !protocol->is_string()) throw ValueError("Unsupported controller protocol");
        s->device().select_protocol(protocol ? protocol->get<std::string>() : "grbl");
        s->device().connect(port->get<std::string>());
        return ok();
    });
    studio.add("POST", "/api/disconnect", [s](const Request &) {
        s->jobs().invalidate_controls();
        s->forget_level();
        s->device().disconnect();
        return ok();
    });

    studio.add("POST", "/api/run", [s](const Request &request) {
        const json data = payload(request);
        if (!is_true(member(data, "confirmed"))) throw ValueError("Confirm work zero, tool and clear work area");
        const json *leveled = member(data, "leveled");
        if (leveled && !leveled->is_boolean()) throw ValueError("Levelled must be true or false");
        const Prepared job = prepared(data);
        const json status = validate_machine(*s, data.at("project"), job);
        std::vector<std::string> lines = job.lines;
        if (is_true(leveled)) lines = levelled(*s, job.planar, levelling_project(data), status);
        s->jobs().disarm();
        s->device().start(lines);
        json extra = json::object();
        extra["lines"] = static_cast<long long>(lines.size());
        return ok(extra);
    });

    studio.add("POST", "/api/head", [s](const Request &request) {
        const json data = payload(request);
        const std::string mode = text_of(member(data, "mode"), "");
        if ((mode != "router" && mode != "laser" && mode != "knife") || !is_true(member(data, "confirmed")))
            throw ValueError("Confirm the mounted head before applying mode");
        s->jobs().disarm();
        s->forget_level();
        const json state = s->device().status();
        if (!flag(state, "connected") || flag(state, "active") || !(text_of(member(state, "state"), "") == "Idle"))
            throw ValueError("Head mode changes require an idle connected controller");
        std::vector<std::string> commands = {"M5"};
        const std::string protocol = text_of(member(state, "protocol"), "grbl");
        if (protocol == "fluidnc")
            throw ValueError(
                "Configure the mounted spindle or laser in FluidNC and reconnect for readback; Studio does not change FluidNC YAML");
        if (protocol_named(protocol).family == "grbl") {
            commands.push_back(std::string("$32=") + (mode == "laser" ? "1" : "0"));
            commands.push_back("$$");
        }
        s->device().start(commands);
        json extra = json::object();
        extra["message"] = "Outputs off and mode requested. Wait for readback; re-establish tool Z/focus and work origin.";
        return ok(extra);
    });

    studio.add("POST", "/api/command", [s](const Request &request) {
        const json data = payload(request, {"action"});
        const json *action = member(data, "action");
        if (!action->is_string()) throw ValueError("Select a controller action");
        static const std::set<std::string> allowed = {"action", "axis", "distance", "feed"};
        std::set<std::string> unknown;
        for (const auto &item : data.items())
            if (!allowed.count(std::string(item.key()))) unknown.insert(std::string(item.key()));
        if (!unknown.empty()) {
            std::string list;
            for (const std::string &name : unknown) list += (list.empty() ? "" : ", ") + name;
            throw ValueError("Unsupported command field: " + list);
        }
        const std::string name = action->get<std::string>();
        if (name == "reset") s->jobs().invalidate_controls();
        else if (name != "hold" && name != "resume") s->jobs().disarm();
        if (name == "reset" || name == "home" || name == "unlock" || name == "zero") s->forget_level();  // work zero may have moved
        s->device().command(name, data);
        return ok();
    });
}

}  // namespace server
}  // namespace kerfkite
