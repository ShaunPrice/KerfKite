// The design routes that are more than one call of the core. See design_extra.hpp.
#include "design_extra.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/pcb.hpp"
#include "kerfkite/polygon.hpp"
#include "kerfkite/solid.hpp"
#include "kerfkite/solid_cad.hpp"
#include "operations.hpp"
#include "project.hpp"
#include "py_trig.hpp"
#include "studio.hpp"

namespace kerfkite {
namespace server {

namespace {

using json = Json;
using polygon::PolygonSet;

// Python's str of an int or a float bound in a message: 100, -100, 0.01.
std::string bound_text(double value) {
    if (value == std::floor(value) && std::fabs(value) < 1e15) return std::to_string(static_cast<long long>(value));
    return text::repr(value);
}

// operations.value(data, key, default, lo, hi): float(data.get(key, default)) within the bounds.
double value_of(const json &data, const char *key, double fallback, double lo, double hi) {
    const json *field = member(data, key);
    if (!field) return fallback;
    const std::string name(key);
    double result = 0;
    const bool readable = !field->is_boolean() &&
                          (detail::plain_number(*field, result) ||
                           (field->is_string() && detail::float_from_text(field->get_ref<const std::string &>(), result)));
    if (!readable) throw ValueError(name + " must be numeric");
    if (!std::isfinite(result) || !(lo <= result && result <= hi))
        throw ValueError(name + " must be between " + bound_text(lo) + " and " + bound_text(hi));
    return result;
}

// operations._contours: a list of lists of finite XY points.
std::vector<std::vector<std::array<double, 2>>> contours_of(const json &paths) {
    const char *const message = "Contours must be lists of finite XY points";
    if (!paths.is_array()) throw ValueError(message);
    std::vector<std::vector<std::array<double, 2>>> out;
    out.reserve(paths.size());
    for (const json &path : paths) {
        if (!path.is_array()) throw ValueError(message);
        std::vector<std::array<double, 2>> points;
        points.reserve(path.size());
        for (const json &point : path) {
            if (!point.is_array() || point.size() != 2) throw ValueError(message);
            std::array<double, 2> xy{};
            for (size_t k = 0; k < 2; ++k) {
                const json &v = point[k];
                if (v.is_boolean() || !v.is_number()) throw ValueError(message);
                xy[k] = v.get<double>();
                if (!std::isfinite(xy[k])) throw ValueError(message);
            }
            points.push_back(xy);
        }
        out.push_back(std::move(points));
    }
    return out;
}

// operations.region(paths): the even-odd region of closed contours, on KerfKite's polygon kernel.
PolygonSet region_of(const json &paths) { return detail::contour_region(contours_of(paths)); }

PolygonSet united(const std::vector<PolygonSet> &shapes, size_t from) {
    PolygonSet result;
    for (size_t i = from; i < shapes.size(); ++i) result = i == from ? shapes[i] : polygon::unite(result, shapes[i]);
    return result;
}

size_t code_points(const std::string &text) {
    size_t count = 0;
    for (const unsigned char c : text) count += (c & 0xc0) != 0x80;
    return count;
}

std::string first_code_points(const std::string &text, size_t limit) {
    size_t count = 0, at = 0;
    for (; at < text.size(); ++at) {
        if ((static_cast<unsigned char>(text[at]) & 0xc0) != 0x80) {
            if (count == limit) break;
            ++count;
        }
    }
    return text.substr(0, at);
}

std::string fixed_text(double value, int decimals) { return text::fixed_exact(value, decimals); }

double python_round(double value, int decimals) { return text::rounded_exact(value, decimals); }

// pcb_api._number: a number (never a bool) within the bounds.
double board_number(const json *value, const std::string &name, double low, double high) {
    if (!value || value->is_boolean() || !value->is_number() || !std::isfinite(value->get<double>()))
        throw ValueError(name + " must be a number");
    const double v = value->get<double>();
    if (!(low <= v && v <= high)) throw ValueError(name + " must be between " + bound_text(low) + " and " + bound_text(high));
    return v;
}

// pcb_api._pair.
json board_pair(const json &value, const std::string &name) {
    if (!value.is_array() || value.size() != 2) throw ValueError("Give the " + name + " as [X, Y]");
    std::string label = name;
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return json::array({board_number(&value[0], label + " X", -10000, 10000), board_number(&value[1], label + " Y", -10000, 10000)});
}

bool box(const json *value) { return value && value->is_array() && value->size() == 4; }

// pcb_api._cover: the box round the boxes given, null when there are none.
json cover(const std::vector<const json *> &boxes) {
    std::vector<const json *> given;
    for (const json *b : boxes)
        if (truthy(b)) given.push_back(b);
    if (given.empty()) return json();
    std::array<double, 4> out{};
    for (size_t i = 0; i < given.size(); ++i)
        for (size_t k = 0; k < 4; ++k) {
            const double v = given[i]->at(k).get<double>();
            if (i == 0) out[k] = v;
            else if (k < 2 ? v < out[k] : v > out[k]) out[k] = v;  // Python's min and max keep the first of equals
        }
    return json::array({out[0], out[1], out[2], out[3]});
}

std::string span(const json &b) {
    return "X" + fixed_text(b[0].get<double>(), 3) + " to X" + fixed_text(b[2].get<double>(), 3) + " and Y" + fixed_text(b[1].get<double>(), 3) +
           " to Y" + fixed_text(b[3].get<double>(), 3);
}

// pcb_import._call: the text and the options that are not None.
json board_call(const char *command, const std::string &text, std::initializer_list<std::pair<const char *, json>> options) {
    json request = json::object();
    request["text"] = text;
    for (const auto &option : options)
        if (!option.second.is_null()) request[option.first] = option.second;
    return pcb_call(command, request);
}

json value_or_null(const json &object, const char *key) {
    const json *found = member(object, key);
    return found ? *found : json();
}

// pcb_import.v_bit_width: the width a V-bit cuts at a depth.
double v_bit_width(const json &tip, const json &angle, const json &depth) {
    for (const json *v : {&tip, &angle, &depth})
        if (v->is_boolean() || !v->is_number() || !std::isfinite(v->get<double>()))
            throw ValueError("The tip width, angle and depth of a V-bit must be numbers");
    const double t = tip.get<double>(), a = angle.get<double>(), d = depth.get<double>();
    if (!(0 <= t && t <= 10 && 1 <= a && a <= 179 && 0 < d && d <= 10))
        throw ValueError("A V-bit needs a tip width of 0 to 10 mm, an angle of 1 to 179 degrees and a depth above 0");
    const double radians = a * (pytrig::PI / 180.0);  // math.radians
    return python_round(t + 2 * d * pytrig::tangent(radians / 2), 6);
}

uint32_t u32(const std::string &bytes, size_t at) {
    uint32_t v = 0;
    for (int k = 3; k >= 0; --k) v = (v << 8) | static_cast<unsigned char>(bytes[at + static_cast<size_t>(k)]);
    return v;
}

double f64(const std::string &bytes, size_t at) {
    uint64_t v = 0;
    for (int k = 7; k >= 0; --k) v = (v << 8) | static_cast<unsigned char>(bytes[at + static_cast<size_t>(k)]);
    double d = 0;
    std::memcpy(&d, &v, sizeof d);
    return d;
}

}  // namespace

json geometry(const json &data) {
    const char *const select = "Select 1\xe2\x80\x93" "200 objects";
    static const json none = json::array();
    const json *objects = member(data, "objects");
    if (!objects) objects = &none;
    if (!objects->is_array() || objects->size() < 1 || objects->size() > 200) throw ValueError(select);
    for (const json &object : *objects) {
        const json *paths = member(object, "paths");
        if (!paths || !paths->is_array()) throw ValueError("Each object needs a list of paths");
    }
    size_t total = 0;
    for (const json &object : *objects)
        for (const json &path : object.at("paths")) {
            if (!path.is_array()) throw ValueError("Each path is a list of points");
            total += path.size();
        }
    if (total > 100000) throw ValueError("Geometry too complex");
    std::vector<PolygonSet> shapes;
    for (const json &object : *objects) shapes.push_back(region_of(object.at("paths")));
    const json *action = member(data, "action");
    const std::string op = action && action->is_string() ? action->get<std::string>() : std::string();
    PolygonSet result;
    if (op == "union") {
        result = united(shapes, 0);
    } else if (op == "difference") {
        result = shapes.size() > 1 ? polygon::subtract(shapes[0], united(shapes, 1)) : shapes[0];
    } else if (op == "intersection") {
        result = shapes[0];
        for (size_t i = 1; i < shapes.size(); ++i) result = polygon::intersect(result, shapes[i]);
    } else if (op == "offset") {
        const double distance = value_of(data, "distance", 0, -100, 100);
        polygon::ArcOptions arcs;
        arcs.segments_per_quarter = 32;  // Shapely's quad_segs=32
        result = polygon::offset(united(shapes, 0), distance, arcs);
    } else {
        throw ValueError("Choose union, difference, intersection or offset");
    }
    if (result.empty()) throw ValueError("Operation produced empty geometry");
    json paths = json::array();
    size_t points = 0;
    for (const polygon::PathMm &ring : polygon::outlines(result)) {
        json path = json::array();
        for (const polygon::PointMm &p : ring) path.push_back(json::array({p[0], p[1]}));
        points += ring.size();
        paths.push_back(std::move(path));
    }
    if (points > 100000) throw ValueError("Result too complex");
    json out = json::object();
    out["paths"] = std::move(paths);
    return out;
}

json extrude_artwork(const json &data) {
    const char *const limit = "Extrusion needs at most 30000 contour vertices";
    static const json none = json::array();
    const json *paths = member(data, "paths");
    if (!paths) paths = &none;
    if (!paths->is_array()) throw ValueError(limit);
    size_t total = 0;
    for (const json &path : *paths) {
        // sum(map(len, paths)): Python measures text and objects too; anything else is not a contour.
        if (path.is_array() || path.is_object()) total += path.size();
        else if (path.is_string()) total += code_points(path.get_ref<const std::string &>());
        else throw ValueError("Contours must be lists of finite XY points");
    }
    if (total > 30000) throw ValueError(limit);
    const PolygonSet region = region_of(*paths);
    const double height = value_of(data, "height", 3, 0.01, 50);
    // The prism over the region on the solid kernel: top faces point +Z and bottom faces -Z, every side wall
    // joins the caps edge to edge, so the mesh is watertight with the holes preserved.
    std::vector<solid::Ring> rings;
    for (const polygon::Polygon &polygon : region) {
        for (const polygon::Ring *ring : {&polygon.shell}) {
            solid::Ring r;
            for (const polygon::Point &p : *ring) r.push_back({polygon::to_mm(p.x), polygon::to_mm(p.y)});
            rings.push_back(std::move(r));
        }
        for (const polygon::Ring &hole : polygon.holes) {
            solid::Ring r;
            for (const polygon::Point &p : hole) r.push_back({polygon::to_mm(p.x), polygon::to_mm(p.y)});
            rings.push_back(std::move(r));
        }
    }
    const solid::Solid prism = solid::extrude(rings, height);
    const std::vector<solid::Vec3> vertices = prism.vertices();
    const std::vector<solid::Triangle> &triangles = prism.triangles();
    if (triangles.size() > 150000) throw ValueError("Extrusion exceeds triangle limit");
    std::string stl = "solid cnc_studio_text\n";
    stl.reserve(triangles.size() * 140 + 64);
    for (const solid::Triangle &t : triangles) {
        stl += "facet normal 0 0 0\nouter loop\n";
        for (const std::uint32_t index : t) {
            const solid::Vec3 &v = vertices[index];
            stl += "vertex " + fixed_text(v.x, 7) + " " + fixed_text(v.y, 7) + " " + fixed_text(v.z, 7) + "\n";
        }
        stl += "endloop\nendfacet\n";
    }
    stl += "endsolid cnc_studio_text\n";
    json out = json::object();
    out["stl"] = std::move(stl);
    out["triangles"] = static_cast<long long>(triangles.size());
    out["height"] = height;
    return out;
}

json pcb_board(const json &payload) {
    if (!payload.is_object()) throw ValueError("Send the board files as a JSON object");
    static const std::set<std::string> fields = {"copper",     "outline", "drill",   "side",      "offset",        "margin",
                                                 "mirror_x",   "width",   "height",  "tool_width", "v_bit",        "passes",
                                                 "overlap",    "direction", "order", "arc_tolerance", "clip",       "drill_format"};
    // Names are held in byte order, which for UTF-8 is the order of their code points: sorted(unknown)[0].
    for (const auto &item : payload.items()) {
        const std::string key(item.key());
        if (!fields.count(key)) throw ValueError("The board import has a field that is not known: " + first_code_points(key, 60));
    }
    const std::pair<const char *, const char *> files[] = {{"copper", "copper layer"}, {"outline", "board outline"}, {"drill", "drill file"}};
    std::map<std::string, std::string> texts;
    for (const auto &file : files) {
        const json *text = member(payload, file.first);
        if (!text || text->is_null() || (text->is_string() && text->get_ref<const std::string &>().empty())) continue;
        if (!text->is_string()) throw ValueError(std::string("Send the ") + file.second + " as text");
        if (code_points(text->get_ref<const std::string &>()) > (4u << 20)) throw ValueError(std::string("The ") + file.second + " is larger than 4 MiB");
        texts[file.first] = text->get<std::string>();
    }
    if (texts.empty()) throw ValueError("Choose a copper layer, a board outline or a drill file");
    std::string side = "top";
    if (const json *given = member(payload, "side")) {
        if (!given->is_string() || (given->get_ref<const std::string &>() != "top" && given->get_ref<const std::string &>() != "bottom"))
            throw ValueError("The side must be \"top\" or \"bottom\"");
        side = given->get<std::string>();
    }
    json drill_format = json::object();
    const json *format = member(payload, "drill_format");
    if (truthy(format)) {
        const char *const message = "The drill format may state units, integer, decimal and omitted_zeros";
        if (!format->is_object()) throw ValueError(message);
        for (const auto &item : format->items()) {
            const std::string key(item.key());
            if (key != "units" && key != "integer" && key != "decimal" && key != "omitted_zeros") throw ValueError(message);
            const json &value = item.value();
            if (!value.is_null() && !(value.is_string() && value.get_ref<const std::string &>().empty())) drill_format[key] = value;
        }
    }
    const json width = value_or_null(payload, "width"), height = value_or_null(payload, "height");
    const bool work = !width.is_null() || !height.is_null();  // the core checks the pair
    const json arc_tolerance = value_or_null(payload, "arc_tolerance");

    // The tool: its width of cut at the engraving depth, stated or worked out for a V-bit.
    json tool_width = value_or_null(payload, "tool_width");
    if (texts.count("copper")) {
        const json v_bit = value_or_null(payload, "v_bit");
        if (!v_bit.is_null()) {
            if (!tool_width.is_null()) throw ValueError("Give the tool width or the V-bit, not both");
            if (!v_bit.is_object() || v_bit.size() != 3 || !v_bit.contains("tip_width") || !v_bit.contains("angle") || !v_bit.contains("depth"))
                throw ValueError("Give the V-bit as its tip_width, angle and depth");
            tool_width = v_bit_width(v_bit.at("tip_width"), v_bit.at("angle"), v_bit.at("depth"));
        }
        if (tool_width.is_null()) throw ValueError("Give the tool width, or the V-bit, for the isolation of the copper layer");
    }

    // Where each file's content lies before it is placed.
    json first_drill;
    std::vector<const json *> found;
    json outline_bounds, copper_bounds, drill_bounds;
    if (texts.count("outline")) {
        outline_bounds = value_or_null(board_call("gerber-outline", texts["outline"], {{"arc_tolerance", arc_tolerance}}), "bounds");
        found.push_back(&outline_bounds);
    }
    if (texts.count("copper")) {
        copper_bounds = value_or_null(board_call("gerber", texts["copper"], {{"arc_tolerance", arc_tolerance}}), "bounds");
        found.push_back(&copper_bounds);
    }
    if (texts.count("drill")) {
        first_drill = board_call("excellon", texts["drill"],
                                 {{"units", value_or_null(drill_format, "units")},
                                  {"integer", value_or_null(drill_format, "integer")},
                                  {"decimal", value_or_null(drill_format, "decimal")},
                                  {"omitted_zeros", value_or_null(drill_format, "omitted_zeros")}});
        drill_bounds = value_or_null(first_drill, "bounds");
        found.push_back(&drill_bounds);
    }
    // The outline places the board when there is one: it is the same for both sides.
    const json reference = texts.count("outline") && truthy(&outline_bounds) ? outline_bounds : cover(found);
    if (reference.is_null()) throw ValueError("The files draw nothing to place");

    json offset;
    if (const json *given = member(payload, "offset"); given && !given->is_null()) {
        offset = board_pair(*given, "offset");
    } else {
        // One distance for both axes, or [X, Y]: where the lowest corner of the reference goes.
        static const json five = 5;
        const json *margin = member(payload, "margin");
        if (!margin) margin = &five;
        std::array<double, 2> corner{};
        if (margin->is_array()) {
            if (margin->size() != 2) throw ValueError("Give the margin as one distance or as [X, Y]");
            corner = {board_number(&(*margin)[0], "Margin X", 0, 1000), board_number(&(*margin)[1], "Margin Y", 0, 1000)};
        } else {
            const double both = board_number(margin, "Margin", 0, 1000);
            corner = {both, both};
        }
        offset = json::array({python_round(corner[0] - reference[0].get<double>(), 6), python_round(corner[1] - reference[1].get<double>(), 6)});
    }
    json mirror_x;
    const json given_mirror = value_or_null(payload, "mirror_x");
    if (side == "bottom") {
        if (!given_mirror.is_null()) mirror_x = board_number(&given_mirror, "Mirror X", -10000, 10000);
        else if (texts.count("outline"))
            mirror_x = python_round((reference[0].get<double>() + reference[2].get<double>()) / 2 + offset[0].get<double>(), 6);
        else
            throw ValueError(
                "The underside needs the board outline, or the mirror line used for the rest of the board, so that both sides register");
    } else if (!given_mirror.is_null()) {
        throw ValueError("Only the underside is mirrored: choose the bottom side to give a mirror line");
    }
    const json placed_width = work ? width : json(), placed_height = work ? height : json();

    json answer = json::object();
    answer["side"] = side;
    answer["offset"] = offset;
    answer["mirror_x"] = mirror_x;
    answer["copper"] = json();
    answer["outline"] = json();
    answer["drill"] = json();
    json warnings = json::array();
    if (texts.count("outline"))
        answer["outline"] = board_call("gerber-outline", texts["outline"],
                                       {{"offset", offset}, {"arc_tolerance", arc_tolerance}, {"width", placed_width}, {"height", placed_height},
                                        {"mirror_x", mirror_x}});
    if (texts.count("copper")) {
        json clip;
        if (truthy(member(payload, "clip"))) {
            if (answer["outline"].is_null()) throw ValueError("Cutting the isolation to the outline needs the board outline");
            clip = value_or_null(answer["outline"], "paths");
        }
        answer["copper"] = board_call("gerber-isolation", texts["copper"],
                                      {{"tool_width", tool_width},
                                       {"passes", value_or_null(payload, "passes")},
                                       {"overlap", value_or_null(payload, "overlap")},
                                       {"direction", value_or_null(payload, "direction")},
                                       {"order", value_or_null(payload, "order")},
                                       {"offset", offset},
                                       {"mirror_x", mirror_x},
                                       {"clip", clip},
                                       {"width", placed_width},
                                       {"height", placed_height},
                                       {"arc_tolerance", arc_tolerance}});
    }
    if (texts.count("drill")) {
        json drill = board_call("excellon-drills", texts["drill"],
                                {{"offset", offset},
                                 {"width", placed_width},
                                 {"height", placed_height},
                                 {"mirror_x", mirror_x},
                                 {"units", value_or_null(drill_format, "units")},
                                 {"integer", value_or_null(drill_format, "integer")},
                                 {"decimal", value_or_null(drill_format, "decimal")},
                                 {"omitted_zeros", value_or_null(drill_format, "omitted_zeros")}});
        drill["plated"] = value_or_null(first_drill, "plated");
        drill["file_function"] = value_or_null(first_drill, "file_function");
        drill["counts"] = value_or_null(first_drill, "counts");
        answer["drill"] = std::move(drill);
    }

    // Layers that do not lie on one board are the commonest sign of a misread file.
    const json outline_box = answer["outline"].is_null() ? json() : value_or_null(answer["outline"], "bounds");
    if (truthy(&outline_box) && truthy(&answer["drill"])) {
        const json holes = value_or_null(answer["drill"], "bounds");
        if (box(&holes) && box(&outline_box) &&
            (holes[0].get<double>() < outline_box[0].get<double>() || holes[1].get<double>() < outline_box[1].get<double>() ||
             holes[2].get<double>() > outline_box[2].get<double>() || holes[3].get<double>() > outline_box[3].get<double>()))
            warnings.push_back("Holes lie outside the board outline: they span " + span(holes) + " and the board spans " + span(outline_box) +
                               ". Check the drill file's units and number format");
    }
    if (truthy(&outline_box) && truthy(&answer["copper"])) {
        const json copper = value_or_null(answer["copper"], "copper_bounds");
        const double reach = 2.0;  // a pour may run a little past the edge; a layer of another board does not stop there
        if (box(&copper) && box(&outline_box) &&
            (copper[0].get<double>() < outline_box[0].get<double>() - reach || copper[1].get<double>() < outline_box[1].get<double>() - reach ||
             copper[2].get<double>() > outline_box[2].get<double>() + reach || copper[3].get<double>() > outline_box[3].get<double>() + reach))
            warnings.push_back("The copper reaches beyond the board outline: it spans " + span(copper) + " and the board spans " +
                               span(outline_box) + ". Check that the layers belong to one board");
    }
    if (side == "bottom")
        warnings.push_back("The underside is mirrored about X" + fixed_text(mirror_x.get<double>(), 3) +
                           ": turn the board over left to right about that line, and use the same outline and placement for every layer of the board");
    const json copper_box = answer["copper"].is_null() ? json() : value_or_null(answer["copper"], "bounds");
    const json drill_box = answer["drill"].is_null() ? json() : value_or_null(answer["drill"], "bounds");
    answer["bounds"] = cover({&outline_box, &copper_box, &drill_box});
    answer["warnings"] = std::move(warnings);
    return answer;
}

json stock_wire(const std::string &body, std::vector<double> &rows) {
    static const char *const shapes[] = {"flat", "ball", "v", "drill"};
    const size_t header = 8 + 3 * 4 + 7 * 8;  // '<8sIII7d'
    const size_t row = 7 * 8;
    if (body.size() < header) throw ValueError("Truncated stock preview header");
    const uint32_t count = u32(body, 8), resolution = u32(body, 12), shape = u32(body, 16);
    if (body.compare(0, 8, "KKSTOCK1") != 0 || count > 200000 || shape >= 4) throw ValueError("Invalid stock preview header");
    if (body.size() != header + static_cast<size_t>(count) * row) throw ValueError("Stock preview length differs from segment count");
    rows.resize(static_cast<size_t>(count) * 7);
    for (size_t i = 0; i < rows.size(); ++i) rows[i] = f64(body, header + 8 * i);
    json data = json::object();
    data["resolution"] = static_cast<long long>(resolution);
    data["x"] = f64(body, 20);
    data["y"] = f64(body, 28);
    data["width"] = f64(body, 36);
    data["height"] = f64(body, 44);
    data["tool_diameter"] = f64(body, 52);
    data["v_angle"] = f64(body, 60);
    data["tool_shape"] = shapes[shape];
    data["stock_thickness"] = f64(body, 68);
    return data;
}

json sketch_points(const json &data, const json &order) {
    json request = json::object();
    request["sketch"] = data;
    const json *parameters = member(data, "parameters");
    if (parameters && parameters->is_object()) request["order"] = order;
    return solid_cad_call("sketch-points", request);
}

json example_assembly() {
    return json::parse(R"({
  "version": 1,
  "parts": [
    {"id": "Bearing plate", "model": {"version": 1, "features": [
      {"id": "plate", "kind": "box", "size": [40, 30, 8]},
      {"id": "bore", "kind": "cylinder", "op": "cut", "height": 10, "radius": 5, "origin": [20, 15, -1]}]}},
    {"id": "Shaft", "model": {"version": 1, "features": [{"id": "shaft", "kind": "cylinder", "radius": 4, "height": 24}]}}
  ],
  "mates": [
    {"child": "Shaft", "parent": "Bearing plate", "kind": "concentric", "parentFrame": {"origin": [20, 15, 0]}, "offset": [0, 0, -4]}
  ]
})");
}

}  // namespace server
}  // namespace kerfkite
