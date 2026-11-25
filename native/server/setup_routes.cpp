// See setup_routes.hpp.
#include "setup_routes.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include "kerfkite/errors.hpp"
#include "studio.hpp"

namespace kerfkite {
namespace server {

namespace {

using json = Json;
using http::Request;
using http::Response;

const double kProbeSeconds = 10;  // a probe line is acknowledged only when its motion ends

std::string fixed3(double value) {
    char text[40];
    std::snprintf(text, sizeof text, "%.3f", value);
    return text;
}

std::string fixed1(double value) {
    char text[40];
    std::snprintf(text, sizeof text, "%.1f", value);
    return text;
}

bool confirmed(const json &data) {
    const json *value = member(data, "confirmed");
    return value && value->is_boolean() && value->get<bool>();
}

Response ok(json extra = json::object()) {
    json out = json::object();
    out["ok"] = true;
    for (const auto &item : extra.items()) out[std::string(item.key())] = item.value();
    return json_response(out);
}

// How the connected controller sets work zero after a probe, or a refusal when it has no probing move.
std::string zeroing(Studio &studio, const char *what) {
    const std::string how = studio.device().zeroing();
    if (how.empty()) throw ValueError(std::string(what) + " needs a controller with a probing move");
    return how;
}

}  // namespace

TouchPlate touch_plate_program(const Json &data, const std::string &zeroing) {
    if (!data.is_object()) throw ValueError("Expected the touch plate settings");
    const json *axes = member(data, "axes");
    const std::string which = axes && axes->is_string() ? axes->get<std::string>() : std::string("z");
    if (which != "z" && which != "xyz") throw ValueError("Probe Z only, or X, Y and Z");
    TouchPlate out;
    out.corner = which == "xyz";
    out.thickness = number(member(data, "thickness"), "Plate thickness", 0.1, 50);
    const double seek = member(data, "feed") ? number(member(data, "feed"), "Probe feed", 10, 500) : 150;
    const double slow = member(data, "slow_feed") ? number(member(data, "slow_feed"), "Second probe feed", 5, 100) : 40;
    const double travel = member(data, "travel") ? number(member(data, "travel"), "Probe travel", 1, 30) : 15;
    const double retract = member(data, "retract") ? number(member(data, "retract"), "Retract", 1, 20) : 5;
    if (slow > seek) throw ValueError("The second probe feed must not be faster than the first");
    const auto timed = [&](double distance, double feed, const char *what) {
        if (distance / feed * 60 > kProbeSeconds)
            throw ValueError(std::string("Each probe must finish within 10 seconds; raise the probe feed or lower the ") + what);
    };
    timed(travel, seek, "probe travel");
    timed(2, slow, "second probe feed's distance");
    std::vector<std::string> &lines = out.lines;
    // The Grbl family sets the work offset (G10 L20 P1); Marlin renames the present position (G92). Either way
    // the axis then reads the given value where the cutter stands.
    const std::string zero = zeroing == "G92" ? "G92 " : "G10 L20 P1 ";
    lines = {"G21", "G91"};
    // Down onto the plate, back off and touch again slowly; the plate's top is its thickness above the stock.
    lines.push_back("G38.2 Z-" + fixed3(travel) + " F" + fixed1(seek));
    lines.push_back("G0 Z1.000");
    lines.push_back("G38.2 Z-2.000 F" + fixed1(slow));
    lines.push_back(zero + "Z" + fixed3(out.thickness));
    lines.push_back("G0 Z" + fixed3(retract));
    if (out.corner) {
        out.wall = number(member(data, "wall"), "Plate wall", 0, 50);
        out.radius = number(member(data, "tool_diameter"), "Cutter diameter", 0.1, 25) / 2;
        const double inset = member(data, "inset") ? number(member(data, "inset"), "Distance from the plate's edges", 5, 40) : 15;
        const double drop = member(data, "drop") ? number(member(data, "drop"), "Depth beside the plate", 1, 20) : 4;
        const double reach = inset + 10;  // the cutter is at most `inset` inside the face, and stops 5 mm outside it
        timed(reach, seek, "distance from the plate's edges");
        // The cutter's centre at contact, from the edge of the stock, as the controller is told it (to 0.001 mm).
        const double edge = std::round((out.wall + out.radius) * 1000) / 1000;
        for (const char *axis : {"X", "Y"}) {
            const std::string a = axis;
            // Out past the plate's face, down beside it, in against the face twice, then back over the stock.
            lines.push_back("G0 " + a + "-" + fixed3(inset + out.radius + 5));
            lines.push_back("G0 Z-" + fixed3(retract + drop));
            lines.push_back("G38.2 " + a + fixed3(reach) + " F" + fixed1(seek));
            lines.push_back("G0 " + a + "-1.000");
            lines.push_back("G38.2 " + a + "2.000 F" + fixed1(slow));
            lines.push_back(zero + a + "-" + fixed3(edge));
            lines.push_back("G0 " + a + "-2.000");
            lines.push_back("G0 Z" + fixed3(retract + drop));
            lines.push_back("G0 " + a + fixed3(edge + 2 + 10));  // to 10 mm inside the stock
        }
    }
    lines.push_back("G90");
    return out;
}

std::string setting_line(const Json &data) {
    if (!data.is_object()) throw ValueError("Expected a controller setting");
    const json *key = member(data, "key");
    std::string name;
    if (key && key->is_string()) name = key->get<std::string>();
    else if (key && key->is_number_integer()) name = std::to_string(key->get<long long>());
    if (name.empty() || name.size() > 3) throw ValueError("Give the setting's number, such as 100");
    for (char c : name)
        if (c < '0' || c > '9') throw ValueError("Give the setting's number, such as 100");
    const json *value = member(data, "value");
    if (!value || value->is_boolean() || !value->is_number() || !std::isfinite(value->get<double>()) ||
        std::fabs(value->get<double>()) > 1e7)
        throw ValueError("The setting's value must be a number");
    std::string text = fixed3(value->get<double>());
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text == "-0") text = "0";
    return "$" + name + "=" + text;
}

// A Marlin setting: {"code": "M92" (steps per unit), "M203" (maximum feed, units a second) or "M201" (maximum
// acceleration), "axis": "X", "Y" or "Z", "value"} as the line that sets it, such as M92 X80.2. {"store": true}
// is M500, which keeps the present settings through a power cycle.
std::string marlin_setting_line(const Json &data) {
    if (!data.is_object()) throw ValueError("Expected a controller setting");
    const json *store = member(data, "store");
    if (store && store->is_boolean() && store->get<bool>()) return "M500";
    const json *code = member(data, "code"), *axis = member(data, "axis"), *value = member(data, "value");
    const std::string command = code && code->is_string() ? code->get<std::string>() : std::string();
    if (command != "M92" && command != "M203" && command != "M201")
        throw ValueError("Choose the setting: M92 steps per mm, M203 maximum feed or M201 maximum acceleration");
    const std::string letter = axis && axis->is_string() ? axis->get<std::string>() : std::string();
    if (letter != "X" && letter != "Y" && letter != "Z") throw ValueError("Choose the axis: X, Y or Z");
    if (!value || value->is_boolean() || !value->is_number() || !std::isfinite(value->get<double>()) || !(value->get<double>() > 0) ||
        value->get<double>() > 1e6)
        throw ValueError("The setting's value must be a number greater than 0");
    std::string text = fixed3(value->get<double>());
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text.empty() || text == "0") throw ValueError("The setting's value must be at least 0.001");
    return command + " " + letter + text;
}

std::vector<std::string> macro_lines(const Json &data) {
    if (!data.is_object()) throw ValueError("Expected a macro");
    const json *given = member(data, "lines");
    if (!given || !given->is_array() || given->empty() || given->size() > 100) throw ValueError("A macro has 1 to 100 lines");
    std::vector<std::string> out;
    for (const json &entry : *given) {
        if (!entry.is_string()) throw ValueError("Each macro line is text");
        std::string line = entry.get<std::string>();
        // A comment after a semicolon is the operator's note, not the controller's.
        const size_t note = line.find(';');
        if (note != std::string::npos) line.erase(note);
        const size_t first = line.find_first_not_of(" \t"), last = line.find_last_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first, last - first + 1);
        if (line.size() > 78) throw ValueError("A macro line has at most 78 characters");
        for (char &c : line) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            const bool word = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-' || c == '+';
            if (!word) throw ValueError("A macro holds plain G-code words only: letters, numbers, spaces and . + -");
        }
        if (line[0] < 'A' || line[0] > 'Z') throw ValueError("A macro line starts with a G-code word, such as G0 or M5");
        out.push_back(line);
    }
    if (out.empty()) throw ValueError("The macro has no G-code in it");
    return out;
}

void add_setup_routes(Studio &studio) {
    Studio *s = &studio;
    studio.add("GET", "/api/setup", [s](const Request &) {
        json out = json::object();
        const std::string how = s->device().zeroing();
        out["touch_plate"] = !how.empty();
        out["settings"] = !how.empty();
        out["macros"] = true;
        out["protocol"] = s->device().protocol();
        // Marlin's settings and, in the simulator, its position: its status carries neither.
        const json report = s->device().setup_report();
        if (!report.is_null()) out["report"] = report;
        return json_response(out);
    });
    studio.add("POST", "/api/setup/probe", [s](const Request &request) {
        const json data = payload(request);
        const std::string how = zeroing(*s, "A touch plate");
        if (!confirmed(data))
            throw ValueError("Confirm that the plate is on the stock, its lead is connected and tested, and the spindle is stopped");
        const TouchPlate plate = touch_plate_program(data, how);
        s->jobs().disarm();
        s->forget_level();  // work zero moves
        s->device().expect_touch_plate(plate.thickness, plate.wall, plate.radius);
        // A firmware built without the probing move must stop there, not carry on and set a zero it never found.
        s->device().start_checked(plate.lines);
        json lines = json::array();
        for (const std::string &line : plate.lines) lines.push_back(line);
        json extra = json::object();
        extra["lines"] = lines;
        return ok(extra);
    });
    studio.add("POST", "/api/setup/setting", [s](const Request &request) {
        const json data = payload(request);
        const std::string how = zeroing(*s, "Writing a controller setting");
        if (!confirmed(data)) throw ValueError("Confirm the change: a wrong setting can make the machine move wrongly");
        const bool marlin = how == "G92";
        const std::string line = marlin ? marlin_setting_line(data) : setting_line(data);
        s->jobs().disarm();
        // Write it, then read every setting back. Marlin keeps a setting only until its power is cycled unless
        // it is stored (M500), which is asked for on its own.
        if (marlin) s->device().start_checked(line == "M500" ? std::vector<std::string>{line} : std::vector<std::string>{line, "M503"});
        else s->device().start({line, "$$"});
        json extra = json::object();
        extra["line"] = line;
        return ok(extra);
    });
    studio.add("POST", "/api/setup/macro", [s](const Request &request) {
        const json data = payload(request);
        if (!confirmed(data)) throw ValueError("Confirm that the machine is clear to move before running a macro");
        const std::vector<std::string> lines = macro_lines(data);
        s->jobs().disarm();
        s->forget_level();  // a macro may move work zero
        s->device().start(lines);
        json extra = json::object();
        extra["lines"] = static_cast<long long>(lines.size());
        return ok(extra);
    });
}

}  // namespace server
}  // namespace kerfkite
