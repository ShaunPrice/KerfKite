// Manual tool changes: the port of studio/tool_changes.py. See kerfkite/tool_changes.hpp.
#include "kerfkite/tool_changes.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "kerfkite/cam.hpp"
#include "project.hpp"

namespace kerfkite {
namespace {

using json = Json;
using detail::get;
using detail::is_text;

// A number as Python's arithmetic sees it: a bool is the integer 0 or 1.
struct Number {
    bool real = false;     // a float
    double value = 0;      // the float
    bool negative = false; // an integer's sign and size
    uint64_t size = 0;
};

bool as_number(const json &value, Number &out) {
    if (value.is_boolean()) {
        out.size = value.get<bool>() ? 1 : 0;
    } else if (value.is_number_unsigned()) {
        out.size = value.get<uint64_t>();
    } else if (value.is_number_integer()) {
        const int64_t whole = value.get<int64_t>();
        out.negative = whole < 0;
        out.size = out.negative ? 0 - static_cast<uint64_t>(whole) : static_cast<uint64_t>(whole);
    } else if (value.is_number_float()) {
        out.real = true;
        out.value = value.get<double>();
    } else {
        return false;
    }
    return true;
}

// An integer equals a float, as Python compares them: exactly, not by converting the integer.
bool same_number(const Number &whole, double real) {
    if (!std::isfinite(real) || std::floor(real) != real || std::fabs(real) >= 18446744073709551616.0) return false;
    const uint64_t size = static_cast<uint64_t>(std::fabs(real));
    return size == whole.size && (size == 0 || whole.negative == (real < 0));
}

// Python's == between two values read from JSON: numbers by value across bool, int and float; text, lists and
// dicts by their contents.
bool python_equal(const json &a, const json &b) {
    Number x, y;
    const bool first = as_number(a, x), second = as_number(b, y);
    if (first || second) {
        if (!first || !second) return false;
        if (x.real && y.real) return x.value == y.value;
        if (x.real) return same_number(y, x.value);
        if (y.real) return same_number(x, y.value);
        return x.size == y.size && (x.size == 0 || x.negative == y.negative);
    }
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case json::Type::null:
        return true;
    case json::Type::string:
        return a.get_ref<const std::string &>() == b.get_ref<const std::string &>();
    case json::Type::array:
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!python_equal(a.at(i), b.at(i))) return false;
        return true;
    case json::Type::object:
        if (a.size() != b.size()) return false;
        for (const auto &item : a.items()) {
            const auto other = b.find(std::string_view(item.key()));
            if (other == b.end() || !python_equal(item.value(), *other)) return false;
        }
        return true;
    default:
        return false;
    }
}

// Python's truth of a value; an absent key is None.
bool truthy(const json *value) {
    if (!value || value->is_null()) return false;
    Number number;
    if (as_number(*value, number)) return number.real ? number.value != 0 : number.size != 0;
    if (value->is_string()) return !value->get_ref<const std::string &>().empty();
    return value->size() != 0;  // a list or a dict
}

// _numeric(value, name, lo, hi): a bool or anything but an int or a float is not numeric.
double numeric(const json *value, const std::string &name, double low, double high) {
    double result = 0;
    if (!value || value->is_boolean() || !detail::plain_number(*value, result))
        throw ValueError(name + " must be numeric");
    if (!std::isfinite(result) || !(low <= result && result <= high)) throw ValueError(name + " is out of range");
    return result;
}

// Whether Python's str.isspace() is true of a code point: category Zs, or bidirectional class WS, B or S.
bool python_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

// The tool name as the reference checks and keeps it: 1..80 characters (code points, before stripping), none
// below U+0020, and something left after str.strip(), which is what the plan keeps.
std::string tool_name(const std::string &text) {
    static const char *const message = "Tool name must contain 1..80 printable characters";
    struct Point {
        uint32_t code;
        size_t begin, end;  // its bytes
    };
    std::vector<Point> points;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        uint32_t code = length == 1 ? lead : length == 2 ? lead & 0x1f : length == 3 ? lead & 0x0f : lead & 0x07;
        for (size_t k = 1; k < length; ++k) code = code << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        points.push_back({code, i, i + length});  // the core's text is well-formed UTF-8
        i += length;
    }
    if (points.size() > 80) throw ValueError(message);
    for (const Point &point : points)
        if (point.code < 32) throw ValueError(message);
    size_t first = 0, last = points.size();
    while (first < last && python_space(points[first].code)) ++first;
    while (last > first && python_space(points[last - 1].code)) --last;
    if (first == last) throw ValueError(message);
    return text.substr(points[first].begin, points[last - 1].end - points[first].begin);
}

struct Tool {
    int id = 0;
    std::string name;
    double diameter = 0;
    std::string shape;
    bool has_angle = false, has_point = false;
    double angle = 0, point = 0;  // the V angle, and the drill's point angle

    // The reference's signature: (tool_id, diameter, shape, v_angle, drill_point_angle).
    bool same(const Tool &other) const {
        return id == other.id && diameter == other.diameter && shape == other.shape && has_angle == other.has_angle &&
               (!has_angle || angle == other.angle) && has_point == other.has_point &&
               (!has_point || point == other.point);
    }

    json as_json() const {
        json tool = json::object();
        tool["tool_id"] = id;
        tool["tool_name"] = name;
        tool["diameter"] = diameter;
        tool["shape"] = shape;
        tool["v_angle"] = has_angle ? json(angle) : json();
        if (has_point) tool["drill_point_angle"] = point;
        return tool;
    }
};

// _tool(operation), with studio.operations.router_tool_shape for the shape.
Tool tool_of(const json &operation) {
    Tool tool;
    const double id = numeric(get(operation, "tool_id"), "Assign a tool number to every operation", 1, 999);
    if (std::floor(id) != id) throw ValueError("Tool number must be an integer in 1..999");
    tool.id = static_cast<int>(id);
    const json *name = get(operation, "tool_name");
    if (!name) {
        tool.name = "Tool " + std::to_string(tool.id);
    } else if (!name->is_string()) {
        throw ValueError("Tool name must contain 1..80 printable characters");
    } else {
        tool.name = tool_name(name->get_ref<const std::string &>());
    }
    const json *diameter = get(operation, "tool_diameter");
    tool.diameter = diameter ? numeric(diameter, "Tool diameter", 0.01, 100) : 3.175;
    const json *kind = get(operation, "operation");  // 'line' when absent
    const json *shape = get(operation, "tool_shape");
    if (is_text(kind, "vcarve"))
        tool.shape = "v";
    else if (!shape)
        tool.shape = is_text(kind, "drill") ? "drill" : "flat";
    else if (shape->is_string())
        tool.shape = shape->get_ref<const std::string &>();
    if (tool.shape != "flat" && tool.shape != "ball" && tool.shape != "v" && tool.shape != "drill")
        throw ValueError("Choose a flat, ball, V or drill bit");
    if (tool.shape == "v") {
        const json *angle = get(operation, "v_angle");
        tool.angle = angle ? numeric(angle, "V angle", 10, 170) : 60;
        tool.has_angle = true;
    }
    if (tool.shape == "drill") {
        const json *point = get(operation, "drill_point_angle");
        tool.point = point ? numeric(point, "Drill point angle", 60, 170) : 118;
        tool.has_point = true;
    }
    return tool;
}

struct Group {
    Tool tool;
    json::Array operations;
    std::vector<int> numbers;  // 1-based, in the project's order
};

}  // namespace

Json plan_tool_changes(const Json &project) {
    if (!project.is_object() || !is_text(get(project, "mode"), "router"))
        throw ValueError("Manual tool-change plans require router mode; head changes are not supported");
    if (truthy(get(project, "rotary")))
        throw ValueError("Rotary tool-change plans require separate export and setup review");
    const json *given = get(project, "operations");
    const bool single = detail::none(given);
    if (!single && (!given->is_array() || given->size() < 1 || given->size() > 100))
        throw ValueError("Use 1..100 ordered router operations");
    json base = json::object();
    for (const auto &item : project.items()) {
        const std::string &key = item.key();
        if (key != "operations" && key != "paths" && key != "path_depths" && key != "path_powers")
            base[key] = item.value();
    }
    std::vector<Group> groups;
    const size_t count = single ? 1 : given->size();
    for (size_t index = 0; index < count; ++index) {
        const json &operation = single ? project : given->at(index);
        if (!operation.is_object() || operation.contains("operations"))
            throw ValueError("Nested or invalid operations are unsupported");
        for (const char *key : {"mode", "protocol", "width", "height", "dwell_units"}) {
            // operation.get(key, project.get(key)) != project.get(key)
            const json *mine = get(operation, key);
            if (!mine) continue;
            const json *theirs = get(project, key);
            if (!(theirs ? python_equal(*mine, *theirs) : mine->is_null()))
                throw ValueError("A tool-change plan must use one head, protocol and work area");
        }
        json merged = base;
        for (const auto &item : operation.items()) merged[item.key()] = item.value();
        Tool tool = tool_of(merged);
        if (groups.empty() || !groups.back().tool.same(tool)) groups.push_back({std::move(tool), {}, {}});
        groups.back().operations.push_back(std::move(merged));
        groups.back().numbers.push_back(static_cast<int>(index + 1));
    }
    json::Array stages;
    size_t total = 0;
    for (size_t index = 0; index < groups.size(); ++index) {
        Group &group = groups[index];
        json stage_project = base;
        stage_project["operations"] = json(std::move(group.operations));
        const Job job = generate(stage_project);
        total += job.lines.size();
        if (total > 200000) throw ValueError("Tool-change plan exceeds 200000 total lines");
        // ending = lines[:-1] if lines[-1] == 'M30' else lines; then ending[-1] and ending[-2]. Where the
        // reference would index past the start it raises IndexError, which is not a refusal: declined.
        if (job.lines.empty()) throw NotPorted("a tool-change stage with no program");
        const size_t ending = job.lines.back() == "M30" ? job.lines.size() - 1 : job.lines.size();
        if (ending == 0) throw NotPorted("a tool-change stage with no program");
        const char *unsafe = "Generated stage does not end retracted with spindle off";
        if (job.lines[ending - 1] != "M5") throw ValueError(unsafe);
        if (ending < 2) throw NotPorted("a tool-change stage of one program line");
        if (job.lines[ending - 2].rfind("G0 Z", 0) != 0) throw ValueError(unsafe);
        json stage = to_json(job);
        stage["stage"] = static_cast<int64_t>(index + 1);
        stage["tool"] = group.tool.as_json();
        stage["operation_indices"] = json(group.numbers);
        char filename[48];
        std::snprintf(filename, sizeof filename, "stage-%03d-tool-%03d.nc", static_cast<int>(index + 1), group.tool.id);
        stage["filename"] = std::string(filename);
        stage["before_running"] = json::array({
            "Wait for the previous job to finish, spindle to stop and motion to cease.",
            "Install and secure the specified tool using the machine manufacturer procedure.",
            "Re-establish the current tool tip Z zero in G54; exported desktop profiles cancel tool-length offsets "
            "with G49. Verify retained XY work origin.",
            "Check stock, clearances, tool geometry, feeds and this stage preview before starting it manually.",
        });
        stages.push_back(std::move(stage));
    }
    json plan = json::object();
    plan["format"] = "cnc-tool-change-plan/1";
    plan["mode"] = "router";
    plan["manual_only"] = true;
    const int64_t changes = stages.empty() ? 0 : static_cast<int64_t>(stages.size()) - 1;
    plan["stages"] = json(std::move(stages));
    plan["tool_changes"] = changes;
    plan["warnings"] = json::array({
        "Each stage is a separate job. Do not concatenate files or run the next stage automatically.",
        "No M6, automatic tool changer, probing, automatic resume or machine dispatch is implemented.",
        "Every tool change requires manual tool-tip work zero and a new clearance check; desktop exports cancel "
        "tool-length compensation.",
        "Tool numbers identify the requested setup, not a verified machine tool table. Confirm each stage diameter "
        "and shape physically.",
    });
    return plan;
}

bool tool_changes_command(const std::string &command) { return command == "tool-changes"; }

Json tool_changes_call(const std::string &, const Json &request) { return plan_tool_changes(request); }

}  // namespace kerfkite
