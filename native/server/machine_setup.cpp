// studio/machine_setup.py's describe(): what the connected controller says about its machine, and the
// workspace settings that follow. Nothing here writes to a controller.
#include "machine_setup.hpp"

#include <cmath>
#include <regex>

#include "imported_drawing.hpp"
#include "protocols.hpp"

namespace kerfkite {
namespace server {

namespace {

using json = Json;

const double kTravelLimit = 2000;
const double kFeedLimit = 10000;

const json *member(const json &object, const char *name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

bool truthy(const json *value) {
    if (!value || value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_number()) return value->get<double>() != 0;
    if (value->is_string()) return !value->get<std::string>().empty();
    if (value->is_array() || value->is_object()) return !value->empty();
    return true;
}

// _positive: float(value) when it is finite and positive.
json positive(const json *value) {
    if (!value || !value->is_number()) {
        if (value && value->is_boolean()) return value->get<bool>() ? json(1.0) : json();
        if (value && value->is_string()) {
            char *end = nullptr;
            const std::string text = value->get<std::string>();
            const double v = std::strtod(text.c_str(), &end);
            if (end != text.c_str() && std::isfinite(v) && v > 0) return json(v);
        }
        return json();
    }
    const double v = value->get<double>();
    return std::isfinite(v) && v > 0 ? json(v) : json();
}

std::string text(double value) { return imported::general(value); }

std::string str(const json *value) {
    if (!value || value->is_null()) return std::string();
    if (value->is_string()) return value->get<std::string>();
    return value->dump();
}

std::string strip(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool starts(const std::string &s, const std::string &prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

json axes_of(const json &a, const json &b, const json &c) {
    json out = json::object();
    out["x"] = a;
    out["y"] = b;
    out["z"] = c;
    return out;
}

void grbl(const json &status, json &out) {
    const json *settings_field = member(status, "settings");
    const json settings = truthy(settings_field) && settings_field->is_object() ? *settings_field : json::object();
    const auto setting = [&](const char *key) { return positive(member(settings, key)); };
    const auto flag = [&](const char *key) -> json {
        const json *value = member(settings, key);
        if (!value) return json();
        double v = 0;
        if (value->is_number()) v = value->get<double>();
        else if (value->is_boolean()) v = value->get<bool>() ? 1 : 0;
        return json(v != 0);
    };
    std::vector<std::string> lines;
    if (const json *rows = member(status, "identification"))
        if (rows->is_array())
            for (const json &row : *rows) lines.push_back(row.is_string() ? row.get<std::string>() : row.dump());
    const std::string banner = str(member(status, "banner"));
    std::string version;
    static const std::regex ver(R"(\[VER:([^:\]]*))");
    for (const std::string &line : lines)
        if (starts(line, "[VER:")) {
            std::smatch found;
            std::regex_search(line, found, ver, std::regex_constants::match_continuous);
            version = strip(found[1].str());
            break;
        }
    json detected;
    const bool grblhal = std::find(lines.begin(), lines.end(), "[FIRMWARE:grblHAL]") != lines.end();
    if (grblhal) {
        out["firmware"] = strip("grblHAL " + version);
        detected = "grblhal";
    } else if (starts(banner, "FluidNC") || version.find("FluidNC") != std::string::npos) {
        out["firmware"] = version.find("FluidNC") != std::string::npos ? version : strip(banner.substr(0, banner.find('[')));
        detected = "fluidnc";
    } else if (std::any_of(lines.begin(), lines.end(), [](const std::string &l) { return starts(l, "[MSG:Using machine:CNC_xPRO_V5"); })) {
        out["firmware"] = strip("Grbl_Esp32 " + version) + " \xc2\xb7 xPRO V5";
        detected = "xpro-v5";
    } else if (!version.empty() || starts(banner, "Grbl")) {
        std::string word;
        if (version.empty()) {
            const size_t a = banner.find_first_of(" \t");
            const size_t b = a == std::string::npos ? std::string::npos : banner.find_first_not_of(" \t", a);
            word = b == std::string::npos ? std::string() : banner.substr(b, banner.find_first_of(" \t", b) - b);
        }
        out["firmware"] = "Grbl " + (!version.empty() ? version : word);
        detected = "grbl";
    }
    out["detected_protocol"] = detected;
    out["travel"] = axes_of(setting("130"), setting("131"), setting("132"));
    out["max_feed"] = axes_of(setting("110"), setting("111"), setting("112"));
    out["max_power"] = setting("30");
    const json *protocol = member(status, "protocol");
    const json laser = flag("32");
    if (protocol && *protocol == json("xpro-v5") && truthy(&laser) && !setting("Laser/FullPower").is_null())
        out["max_power"] = setting("Laser/FullPower");
    out["laser_mode"] = laser;
    out["soft_limits"] = flag("20");
    out["hard_limits"] = flag("21");
    out["homing"] = flag("22");
    const json *count = member(status, "position_count");
    out["axes"] = truthy(count) ? *count : json();
    const json inches = flag("13");
    if (truthy(&inches)) out["warnings"].push_back("The controller reports positions in inches ($13=1); Studio works in millimetres");
}

void marlin(const json &status, json &out) {
    const json *readback_field = member(status, "readback");
    const json readback = truthy(readback_field) && readback_field->is_object() ? *readback_field : json::object();
    const json *firmware = member(readback, "firmware");
    if (truthy(firmware)) {
        const json *type = member(readback, "machine_type");
        out["firmware"] = str(firmware) + (truthy(type) ? " \xc2\xb7 " + str(type) : std::string());
    }
    std::string name = str(firmware);
    for (char &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out["detected_protocol"] = name.find("marlin") != std::string::npos ? json("marlin") : json();
    for (const char *key : {"travel", "max_feed"}) {
        const json *values = member(readback, key);
        json row = json::object();
        const char *axes[] = {"x", "y", "z"};
        for (size_t k = 0; k < 3; ++k) row[axes[k]] = truthy(values) && values->is_array() && k < values->size() ? positive(&values->at(k)) : json();
        out[key] = row;
    }
    const json *endstops = member(readback, "soft_endstops");
    out["soft_limits"] = endstops ? *endstops : json();
    if (endstops && endstops->is_boolean() && !endstops->get<bool>() && truthy(member(readback, "travel")))
        out["warnings"].push_back("Soft endstops are off; the travel reported is the configured bed size, not an enforced limit");
}

}  // namespace

Json describe_machine(const Json &status) {
    json out = json::object();
    out["available"] = false;
    out["reason"] = json();
    out["waiting"] = false;
    out["identity"] = json();
    out["name"] = json();
    out["firmware"] = json();
    out["protocol"] = json();
    out["detected_protocol"] = json();
    out["travel"] = axes_of(json(), json(), json());
    out["max_feed"] = axes_of(json(), json(), json());
    out["max_power"] = json();
    out["laser_mode"] = json();
    out["soft_limits"] = json();
    out["hard_limits"] = json();
    out["homing"] = json();
    out["axes"] = json();
    out["environment"] = json::object();
    out["reported"] = json::array();
    out["missing"] = json::array();
    out["warnings"] = json::array();
    if (!truthy(member(status, "connected"))) {
        out["reason"] = "Connect a controller to read its machine settings";
        return out;
    }
    if (truthy(member(status, "simulated"))) {
        out["reason"] = "The simulator has no machine settings; the workspace is left as it is";
        return out;
    }
    if (truthy(member(status, "error"))) {
        out["reason"] = "The controller reported a fault; resolve it and reconnect";
        return out;
    }
    if (!truthy(member(status, "ready"))) {
        out["reason"] = "Waiting for the controller to report its settings";
        out["waiting"] = true;
        return out;
    }
    const json *protocol_field = member(status, "protocol");
    const std::string protocol = protocol_field && protocol_field->is_string() ? protocol_field->get<std::string>() : "grbl";
    const Protocol &profile = protocol_of(protocol_field ? protocol_field : nullptr);
    out["protocol"] = protocol;
    if (profile.family == "grbl") grbl(status, out);
    else marlin(status, out);
    const json detected = out["detected_protocol"];
    if (detected.is_string() && detected.get<std::string>() != protocol)
        out["warnings"].push_back("The controller identifies as " + protocol_named(detected.get<std::string>()).name + ", but the connection uses " +
                                  profile.name + "; reconnect with the matching dialect");
    json environment = json::object();
    environment["protocol"] = protocol;
    json &reported = out["reported"];
    json &missing = out["missing"];
    const json travel = out["travel"], feed = out["max_feed"];
    for (const auto &pair : {std::make_pair("x", "width"), std::make_pair("y", "height")}) {
        const json value = travel.at(pair.first);
        const std::string axis(1, static_cast<char>(std::toupper(pair.first[0])));
        if (value.is_null()) {
            missing.push_back(axis + " travel");
        } else if (value.get<double>() > kTravelLimit) {
            out["warnings"].push_back(axis + " travel of " + text(value.get<double>()) + " mm is beyond the 2000 mm a workspace allows");
        } else {
            environment[pair.second] = value;
            reported.push_back(axis + " travel " + text(value.get<double>()) + " mm");
        }
    }
    const json z = travel.at("z");
    if (z.is_null()) {
        missing.push_back("Z travel");
    } else if (z.get<double>() > kTravelLimit) {
        out["warnings"].push_back("Z travel of " + text(z.get<double>()) + " mm is beyond the 2000 mm a machine profile allows");
    } else {
        environment["z_travel"] = z;
        reported.push_back("Z travel " + text(z.get<double>()) + " mm");
    }
    std::vector<double> planar;
    for (const char *axis : {"x", "y"})
        if (!feed.at(axis).is_null()) planar.push_back(feed.at(axis).get<double>());
    if (planar.size() == 2) {
        const double limit = std::min({planar[0], planar[1], kFeedLimit});
        environment["feed_max"] = limit == kFeedLimit && planar[0] > kFeedLimit && planar[1] > kFeedLimit ? json(10000) : json(limit);
        reported.push_back("feed up to " + text(limit) + " mm/min");
    } else {
        missing.push_back("maximum feed");
    }
    if (!feed.at("z").is_null()) {
        const double limit = std::min(feed.at("z").get<double>(), kFeedLimit);
        environment["plunge_max"] = feed.at("z").get<double>() > kFeedLimit ? json(10000) : json(limit);
        reported.push_back("plunge up to " + text(limit) + " mm/min");
    }
    if (!out["max_power"].is_null()) {
        environment["power_max"] = out["max_power"];
        reported.push_back("spindle speed or laser power up to " + text(out["max_power"].get<double>()));
    } else if (profile.family == "grbl") {
        missing.push_back("maximum spindle speed or laser power");
    }
    if (!out["laser_mode"].is_null()) {
        environment["laser_mode"] = out["laser_mode"];
        reported.push_back(out["laser_mode"].get<bool>() ? "laser mode on" : "laser mode off");
    }
    if (!out["homing"].is_null()) reported.push_back(out["homing"].get<bool>() ? "homing enabled" : "homing disabled");
    if (!out["soft_limits"].is_null() && out["soft_limits"].is_boolean())
        reported.push_back(out["soft_limits"].get<bool>() ? "soft limits on" : "soft limits off");
    if (environment.contains("width") && environment.contains("height")) {
        const std::string size = text(environment["width"].get<double>()) + " \xc3\x97 " + text(environment["height"].get<double>()) + " mm";
        const std::string firmware = out["firmware"].is_string() ? out["firmware"].get<std::string>() : std::string();
        const std::string name = (firmware.empty() ? profile.name : firmware) + " \xc2\xb7 " + size;
        out["name"] = imported::prefix(name, 80);
        std::string identity = protocol + "|" + firmware;
        for (const char *axis : {"x", "y", "z"}) {
            const json value = travel.at(axis);
            identity += "|" + (value.is_null() ? std::string() : text(value.get<double>()));
        }
        out["identity"] = imported::prefix(identity, 120);
    }
    out["environment"] = environment;
    out["available"] = true;
    return out;
}

}  // namespace server
}  // namespace kerfkite
