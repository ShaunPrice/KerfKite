// Probe levelling: see kerfkite/leveling.hpp.
#include "kerfkite/leveling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "kerfkite/number_text.hpp"
#include "kerfkite/py_math.hpp"
#include "project.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;

namespace {

const double EDGE = 1e-6;  // how far outside the probed area a point may fall through rounding

double need(const json &object, const char *key, const std::string &name, double low, double high) {
    return number(get(object, key), name, low, high, text::repr(low), text::repr(high));
}

double option(const json &object, const char *key, double fallback, const std::string &name, double low, double high) {
    return setting(object, key, fallback, name, low, high, text::repr(low), text::repr(high));
}

size_t count(const json &object, const char *key, const std::string &name, double low, double high) {
    const double value = need(object, key, name, low, high);
    if (value != std::floor(value)) throw ValueError(name + " must be a whole number");
    return static_cast<size_t>(value);
}

std::string fixed3(double value) {
    std::string out;
    if (!text::fixed(out, value, 3)) throw ValueError("A coordinate is too large to write");
    return out;
}

// The grid part shared by a plan and a height map.
void read_grid(const json &value, HeightMap &map) {
    if (!value.is_object()) throw ValueError("The probe grid must be an object");
    map.x0 = need(value, "x0", "Grid origin X", -2000, 2000);
    map.y0 = need(value, "y0", "Grid origin Y", -2000, 2000);
    map.dx = need(value, "dx", "Grid spacing X", 0.01, 2000);
    map.dy = need(value, "dy", "Grid spacing Y", 0.01, 2000);
    map.columns = count(value, "columns", "Grid columns", 2, 200);
    map.rows = count(value, "rows", "Grid rows", 2, 200);
    if (map.columns * map.rows > 400) throw ValueError("A probe grid holds at most 400 points");
}

json grid_json(const HeightMap &map) {
    return json{{"x0", map.x0}, {"y0", map.y0}, {"dx", map.dx}, {"dy", map.dy}, {"columns", map.columns}, {"rows", map.rows}};
}

// One word of a G-code line: a letter and its number, with the number's text kept for words that are
// passed through unchanged.
struct Word {
    char letter;
    double value;
    std::string text;
};

bool words(const std::string &line, std::vector<Word> &out) {
    out.clear();
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ') {
            ++i;
            continue;
        }
        const char letter = line[i];
        if (letter < 'A' || letter > 'Z') return false;
        const size_t start = ++i;
        while (i < line.size() && line[i] != ' ' && !(line[i] >= 'A' && line[i] <= 'Z')) ++i;
        const std::string number_text = line.substr(start, i - start);
        if (number_text.empty()) return false;
        // A G-code number is [+-]digits[.digits]. It is read by the core's own parser, not the C library's,
        // which follows the process locale and would misread the decimal point under some of them.
        size_t digits = 0, points = 0;
        for (size_t k = number_text[0] == '+' || number_text[0] == '-' ? 1 : 0; k < number_text.size(); ++k) {
            if (number_text[k] == '.') ++points;
            else if (number_text[k] >= '0' && number_text[k] <= '9') ++digits;
            else return false;
        }
        double value = 0;
        if (!digits || points > 1 || number_text.size() > 40 ||
            !text::parse_double(number_text.data(), number_text.data() + number_text.size(), value) || !std::isfinite(value))
            return false;
        out.push_back({letter, value, number_text});
    }
    return !out.empty();
}

// Codes that set a state this module relies on, or do not move the tool.
bool passes(const std::vector<Word> &line) {
    const Word &first = line[0];
    if (first.letter == 'G') {
        const double g = first.value;
        if (g == 21 || g == 90 || g == 17 || g == 94 || g == 54 || g == 40 || g == 49 || g == 80 || g == 61)
            return line.size() == 1;
        if (g == 4) return line.size() == 2 && line[1].letter == 'P';
        return false;
    }
    if (first.letter == 'M') {
        const double m = first.value;
        if (m == 5 || m == 30 || m == 0 || m == 2 || m == 8 || m == 9) return line.size() == 1;
        if (m == 3) return line.size() <= 2 && (line.size() == 1 || line[1].letter == 'S');
        return false;
    }
    return false;
}

}  // namespace

double HeightMap::at(double x, double y) const {
    const double fx = std::min(std::max((x - x0) / dx, 0.0), static_cast<double>(columns - 1));
    const double fy = std::min(std::max((y - y0) / dy, 0.0), static_cast<double>(rows - 1));
    const size_t c = std::min(static_cast<size_t>(fx), columns - 2), r = std::min(static_cast<size_t>(fy), rows - 2);
    const double tx = fx - static_cast<double>(c), ty = fy - static_cast<double>(r);
    const double a = offsets[r * columns + c], b = offsets[r * columns + c + 1];
    const double d = offsets[(r + 1) * columns + c], e = offsets[(r + 1) * columns + c + 1];
    return (a * (1 - tx) + b * tx) * (1 - ty) + (d * (1 - tx) + e * tx) * ty;
}

double HeightMap::highest() const { return *std::max_element(offsets.begin(), offsets.end()); }

HeightMap height_map_from_json(const json &value) {
    HeightMap map;
    read_grid(value, map);
    const json *offsets = get(value, "offsets");
    if (!offsets || !offsets->is_array() || offsets->size() != map.columns * map.rows)
        throw ValueError("The height map needs one offset for each grid point");
    for (const json &offset : *offsets) map.offsets.push_back(number(&offset, "Height offset", -10, 10, "-10", "10"));
    return map;
}

json to_json(const HeightMap &map) {
    json out = grid_json(map);
    out["offsets"] = map.offsets;
    return out;
}

json probe_plan(const json &request) {
    if (!request.is_object()) throw ValueError("The probe request must be an object");
    const double width = need(request, "width", "Work width", 1, 2000);
    const double height = need(request, "height", "Work height", 1, 2000);
    const json *bounds = get(request, "bounds");
    if (!bounds || !bounds->is_array() || bounds->size() != 4)
        throw ValueError("Give the cutting bounds as [min X, min Y, max X, max Y]");
    const double bx0 = number(&(*bounds)[0], "Bounds min X", 0, width, "0", text::repr(width));
    const double by0 = number(&(*bounds)[1], "Bounds min Y", 0, height, "0", text::repr(height));
    const double bx1 = number(&(*bounds)[2], "Bounds max X", bx0, width, text::repr(bx0), text::repr(width));
    const double by1 = number(&(*bounds)[3], "Bounds max Y", by0, height, text::repr(by0), text::repr(height));
    const double spacing = option(request, "spacing", 10, "Probe spacing", 1, 100);
    const double margin = option(request, "margin", 2, "Probe margin", 0, 20);
    const double clearance = option(request, "clearance", 2, "Probe clearance Z", 0.5, 25);
    const double depth = option(request, "depth", 1, "Probe depth", 0.1, 5);
    const double feed = option(request, "feed", 25, "Probe feed", 1, 200);
    // With "zero", work Z zero is set where the first point makes contact, so that the reference point of
    // the height map is Z zero itself. "plate" is the thickness of a touch plate under the tool, if any.
    const json *zero_value = get(request, "zero");
    if (zero_value && !zero_value->is_boolean()) throw ValueError("Set work zero from the first probe must be true or false");
    const bool zero = zero_value && zero_value->get<bool>();
    const double plate = option(request, "plate", 0, "Touch plate thickness", 0, 50);
    if (plate != 0 && !zero) throw ValueError("A touch plate thickness applies only when work zero is set from the first probe");

    // The probed area is the cutting bounds plus a margin, kept inside the work area and at least 1 mm
    // each way so that a single line still has a surface on both sides.
    HeightMap grid;
    const auto span = [](double low, double high, double limit, double pad, double &start, double &length) {
        start = std::max(0.0, low - pad);
        double end = std::min(limit, high + pad);
        if (end - start < 1) {
            start = std::max(0.0, std::min(start, limit - 1));
            end = start + 1;
        }
        length = end - start;
    };
    double w = 0, h = 0;
    span(bx0, bx1, width, margin, grid.x0, w);
    span(by0, by1, height, margin, grid.y0, h);
    grid.columns = std::max<size_t>(2, static_cast<size_t>(std::ceil(w / spacing - 1e-9)) + 1);
    grid.rows = std::max<size_t>(2, static_cast<size_t>(std::ceil(h / spacing - 1e-9)) + 1);
    if (grid.columns * grid.rows > 400) throw ValueError("The probe grid exceeds 400 points; increase the probe spacing");
    grid.dx = w / static_cast<double>(grid.columns - 1);
    grid.dy = h / static_cast<double>(grid.rows - 1);

    std::vector<std::string> lines = {"G21", "G90", "M5"};  // the spindle stays off while probing
    const std::string lift = "G0 Z" + fixed3(clearance);
    std::string probe = "G38.2 Z" + fixed3(-depth) + " F";
    text::fixed(probe, feed, 1);
    lines.push_back(lift);
    json points = json::array(), order = json::array();
    for (size_t r = 0; r < grid.rows; ++r) {
        for (size_t k = 0; k < grid.columns; ++k) {
            const size_t c = r % 2 ? grid.columns - 1 - k : k;  // back and forth, to shorten the travel
            const double x = c + 1 == grid.columns ? grid.x0 + w : grid.x0 + grid.dx * static_cast<double>(c);
            const double y = r + 1 == grid.rows ? grid.y0 + h : grid.y0 + grid.dy * static_cast<double>(r);
            lines.push_back("G0 X" + fixed3(x) + " Y" + fixed3(y));
            lines.push_back(probe);
            if (zero && points.empty()) lines.push_back("G10 L20 P1 Z" + fixed3(plate));
            lines.push_back(lift);
            points.push_back({x, y});
            order.push_back({c, r});
        }
    }
    return json{{"lines", lines}, {"points", points}, {"order", order}, {"grid", grid_json(grid)},
                {"clearance", clearance}, {"depth", depth}, {"feed", feed}};
}

HeightMap height_map_from_probes(const json &request) {
    if (!request.is_object()) throw ValueError("The probe results must be an object");
    HeightMap map;
    const json *grid = get(request, "grid");
    if (!grid) throw ValueError("The probe results need the grid of the probing plan");
    read_grid(*grid, map);
    const size_t total = map.columns * map.rows;
    const json *order = get(request, "order");
    if (!order || !order->is_array() || order->size() != total)
        throw ValueError("The probe results need the probing order of the plan");
    std::vector<double> heights;
    const json *given = get(request, "heights"), *reports = get(request, "reports");
    if (given && reports) throw ValueError("Give probe heights or controller reports, not both");
    if (reports) {
        if (!reports->is_array() || reports->size() != total) throw ValueError("One probe report is needed for each grid point");
        for (size_t i = 0; i < total; ++i) {
            // Grbl: [PRB:x,y,z:1], with further axes after z on some controllers and 0 for no contact.
            const json &report = (*reports)[i];
            const std::string text = report.is_string() ? report.get<std::string>() : std::string();
            const size_t open = text.find("[PRB:"), close = text.find(']');
            const size_t flag = text.rfind(':');
            const std::string point = std::to_string(i + 1);
            if (open != 0 || close == std::string::npos || flag == std::string::npos || flag <= 5 || flag > close)
                throw ValueError("Probe report " + point + " is not a [PRB:...] report");
            if (text.substr(flag + 1, close - flag - 1) != "1") throw ValueError("The probe did not make contact at point " + point);
            std::vector<double> axes;
            std::string field;
            for (const char c : text.substr(5, flag - 5) + ",") {
                if (c != ',') {
                    field.push_back(c);
                    continue;
                }
                double value = 0;
                if (!float_from_text(field, value) || !std::isfinite(value))
                    throw ValueError("Probe report " + point + " is not a [PRB:...] report");
                axes.push_back(value);
                field.clear();
            }
            if (axes.size() < 3) throw ValueError("Probe report " + point + " has no Z position");
            heights.push_back(axes[2]);
        }
    } else {
        if (!given || !given->is_array() || given->size() != total) throw ValueError("One probe height is needed for each grid point");
        for (const json &value : *given) heights.push_back(number(&value, "Probe height", -1000, 1000, "-1000", "1000"));
    }
    const double limit = option(request, "limit", 5, "Height difference limit", 0.01, 10);
    const size_t reference = get(request, "reference") ? count(request, "reference", "Reference point", 0, static_cast<double>(total - 1)) : 0;
    map.offsets.assign(total, 0.0);
    std::vector<bool> filled(total, false);
    for (size_t i = 0; i < total; ++i) {
        const json &cell = (*order)[i];
        if (!cell.is_array() || cell.size() != 2 || !cell[0].is_number_unsigned() || !cell[1].is_number_unsigned() ||
            cell[0].get<size_t>() >= map.columns || cell[1].get<size_t>() >= map.rows)
            throw ValueError("The probing order names a point outside the grid");
        const size_t index = cell[1].get<size_t>() * map.columns + cell[0].get<size_t>();
        if (filled[index]) throw ValueError("The probing order names a grid point twice");
        filled[index] = true;
        const double offset = heights[i] - heights[reference];
        if (std::fabs(offset) > limit)
            throw ValueError("Point " + std::to_string(i + 1) + " differs from the reference by " + fixed3(offset) +
                             " mm, more than the " + text::repr(limit) + " mm limit; check the probe and the stock");
        map.offsets[index] = offset;
    }
    return map;
}

std::vector<std::string> level_program(const std::vector<std::string> &lines, const HeightMap &map, double max_segment) {
    if (!(max_segment >= 0.05 && max_segment <= 10)) throw ValueError("Levelling segment length must be between 0.05 and 10");
    if (lines.size() > 400000) throw ValueError("The program is too long to level");
    const double highest = map.highest();
    std::vector<std::string> out;
    out.reserve(lines.size());
    bool have_xy = false, have_z = false;
    double px = 0, py = 0, pz = 0;  // the programmed position, before any correction
    std::vector<Word> line;
    const auto refuse = [](size_t index, const std::string &text, const char *why) -> ValueError {
        return ValueError("Line " + std::to_string(index + 1) + " cannot be levelled (" + why + "): " + text);
    };
    const auto inside = [&](double x, double y, size_t index) {
        if (x < map.x0 - EDGE || x > map.x1() + EDGE || y < map.y0 - EDGE || y > map.y1() + EDGE)
            throw ValueError("Line " + std::to_string(index + 1) + " cuts at X" + fixed3(x) + " Y" + fixed3(y) +
                             ", outside the probed area; probe a larger area");
    };
    for (size_t index = 0; index < lines.size(); ++index) {
        const std::string &text = lines[index];
        if (text.empty() || text[0] == '(' || text[0] == ';' || text == "%") {
            out.push_back(text);
            continue;
        }
        if (!words(text, line)) throw refuse(index, text, "not understood");
        const bool motion = line[0].letter == 'G' && (line[0].value == 0 || line[0].value == 1);
        if (!motion) {
            if (!passes(line)) throw refuse(index, text, "only straight moves in absolute millimetres are handled");
            out.push_back(text);
            continue;
        }
        const bool cutting = line[0].value == 1;
        bool has_x = false, has_y = false, has_z = false;
        double x = px, y = py, z = pz;
        std::string feed;
        for (size_t k = 1; k < line.size(); ++k) {
            const Word &word = line[k];
            if (word.letter == 'X' && !has_x) { has_x = true; x = word.value; }
            else if (word.letter == 'Y' && !has_y) { has_y = true; y = word.value; }
            else if (word.letter == 'Z' && !has_z) { has_z = true; z = word.value; }
            else if (word.letter == 'F' && feed.empty() && cutting) feed = " F" + word.text;
            else throw refuse(index, text, word.letter == 'S' ? "levelling is for cutter programs, not laser power" : "unexpected word");
        }
        if (!has_x && !has_y && !has_z) throw refuse(index, text, "a move with no axis");
        if (!cutting) {
            // A rapid is left as written, so it has to be clear of the highest point of the surface.
            const double travel_z = has_z ? z : pz;
            if ((has_z || have_z) && travel_z < highest + 0.1)
                throw ValueError("Line " + std::to_string(index + 1) + " travels at Z" + fixed3(travel_z) +
                                 ", which is not clear of the highest probed point (" + fixed3(highest) +
                                 " mm); raise the clearance height");
            if ((has_x || has_y) && !has_z && !have_z) throw refuse(index, text, "a rapid before any height is set");
            out.push_back(text);
        } else if (!has_x && !has_y) {
            if (!have_xy) throw refuse(index, text, "a plunge before any position is set");
            inside(px, py, index);
            out.push_back("G1 Z" + fixed3(z + map.at(px, py)) + feed);
        } else {
            if (!have_xy || !have_z) throw refuse(index, text, "a cut before the start position and height are set");
            inside(px, py, index);
            inside(x, y, index);
            const double length = pymath::dist(px, py, x, y);
            const double pieces = std::max(1.0, std::ceil(length / max_segment));
            if (static_cast<double>(out.size()) + pieces > 2000000) throw ValueError("The levelled program exceeds 2,000,000 lines; use a longer segment");
            for (double i = 1; i <= pieces; ++i) {
                const bool last = i == pieces;
                const double t = i / pieces;
                const double cx = last ? x : px + (x - px) * t, cy = last ? y : py + (y - py) * t;
                const double cz = last ? z : pz + (z - pz) * t;
                out.push_back("G1 X" + fixed3(cx) + " Y" + fixed3(cy) + feed + " Z" + fixed3(cz + map.at(cx, cy)));
            }
        }
        if (has_x || has_y) have_xy = true;
        if (has_z) have_z = true;
        px = x;
        py = y;
        pz = z;
    }
    return out;
}

json level_request(const json &request) {
    if (!request.is_object()) throw ValueError("The levelling request must be an object");
    const json *lines = get(request, "lines"), *map = get(request, "height_map");
    if (!lines || !lines->is_array()) throw ValueError("The levelling request needs the program lines");
    if (!map) throw ValueError("The levelling request needs a height map");
    std::vector<std::string> program;
    program.reserve(lines->size());
    for (const json &line : *lines) {
        if (!line.is_string()) throw ValueError("Every program line must be text");
        program.push_back(line.get<std::string>());
    }
    const HeightMap surface = height_map_from_json(*map);
    // By default a quarter of the grid spacing, between 0.25 and 2 mm: fine enough to follow the surface
    // between probe points without multiplying the program needlessly.
    const double fallback = std::min(std::max(std::min(surface.dx, surface.dy) / 4, 0.25), 2.0);
    const double segment = option(request, "max_segment", fallback, "Levelling segment length", 0.05, 10);
    const std::vector<std::string> levelled = level_program(program, surface, segment);
    return json{{"lines", levelled}, {"max_segment", segment}, {"highest", surface.highest()}};
}

// ---- tiling -------------------------------------------------------------------------------------------------

namespace {

// The part of the move from (x0, y0) to (x1, y1) inside a rectangle, as parameters along it; false when none is.
bool clip(double x0, double y0, double x1, double y1, const double box[4], double &enter, double &leave) {
    enter = 0;
    leave = 1;
    const double p[4] = {-(x1 - x0), x1 - x0, -(y1 - y0), y1 - y0};
    const double q[4] = {x0 - box[0], box[2] - x0, y0 - box[1], box[3] - y0};
    for (int k = 0; k < 4; ++k) {
        if (p[k] == 0) {
            if (q[k] < 0) return false;
        } else {
            const double t = q[k] / p[k];
            if (p[k] < 0) enter = std::max(enter, t);
            else leave = std::min(leave, t);
        }
    }
    return enter <= leave;
}

struct Tile {
    size_t column = 0, row = 0;
    std::vector<std::string> lines;
    double cut = 0;  // millimetres of cutting moves inside the tile
};

// One tile's program: the job's own lines, with every cutting move kept only where it lies in the tile and moved
// so that the tile's corner is the origin. Where a cut leaves the tile the tool retracts to the job's clearance;
// where one enters, the tool travels there at clearance and plunges to the cut's depth.
void tile_program(const std::vector<std::string> &lines, const double box[4], double plunge, Tile &tile) {
    std::vector<std::string> &out = tile.lines;
    bool have_xy = false, have_z = false, have_clear = false, there = false;  // there: the tool is at the programmed XY
    double px = 0, py = 0, pz = 0, clear = 0;
    std::string plunge_feed = plunge > 0 ? " F" + fixed3(plunge).substr(0, fixed3(plunge).size() - 2) : std::string();
    std::string last_feed;
    std::vector<Word> line;
    const auto refuse = [](size_t index, const std::string &text, const char *why) -> ValueError {
        return ValueError("Line " + std::to_string(index + 1) + " cannot be tiled (" + why + "): " + text);
    };
    const auto inside = [&](double x, double y) { return x >= box[0] - EDGE && x <= box[2] + EDGE && y >= box[1] - EDGE && y <= box[3] + EDGE; };
    const auto at = [&](double x, double y) { return "X" + fixed3(x - box[0]) + " Y" + fixed3(y - box[1]); };
    for (size_t index = 0; index < lines.size(); ++index) {
        const std::string &text = lines[index];
        if (text.empty() || text[0] == '(' || text[0] == ';' || text == "%") {
            out.push_back(text);
            continue;
        }
        if (!words(text, line)) throw refuse(index, text, "not understood");
        const bool motion = line[0].letter == 'G' && (line[0].value == 0 || line[0].value == 1);
        if (!motion) {
            if (!passes(line)) throw refuse(index, text, "only straight moves in absolute millimetres are handled");
            out.push_back(text);
            continue;
        }
        const bool cutting = line[0].value == 1;
        bool has_x = false, has_y = false, has_z = false;
        double x = px, y = py, z = pz;
        std::string feed;
        for (size_t k = 1; k < line.size(); ++k) {
            const Word &word = line[k];
            if (word.letter == 'X' && !has_x) { has_x = true; x = word.value; }
            else if (word.letter == 'Y' && !has_y) { has_y = true; y = word.value; }
            else if (word.letter == 'Z' && !has_z) { has_z = true; z = word.value; }
            else if (word.letter == 'F' && feed.empty() && cutting) feed = " F" + word.text;
            else throw refuse(index, text, "unexpected word");
        }
        if (!has_x && !has_y && !has_z) throw refuse(index, text, "a move with no axis");
        if (!feed.empty() && (has_x || has_y)) last_feed = feed;
        if (!cutting) {
            if (has_z) {
                // A rapid in Z is the job's retract: every tile makes it, so the tool is clear before it travels.
                if (has_x || has_y) throw refuse(index, text, "a rapid that moves in Z and across at once");
                clear = z;
                have_clear = true;
                out.push_back(text);
            } else {
                if (!have_clear || pz < clear - EDGE) throw refuse(index, text, "a rapid across the work below the clearance height");
                if (inside(x, y)) {
                    out.push_back("G0 " + at(x, y));
                    there = true;
                } else {
                    there = false;
                }
            }
        } else if (!has_x && !has_y) {
            // A plunge or a change of depth where the tool stands.
            if (!have_xy) throw refuse(index, text, "a plunge before any position is set");
            if (plunge_feed.empty() && !feed.empty()) plunge_feed = feed;
            if (inside(px, py)) {
                if (!there) {
                    out.push_back("G0 " + at(px, py));
                    there = true;
                }
                out.push_back("G1 Z" + fixed3(z) + feed);
            }
        } else {
            if (!have_xy || !have_z || !have_clear) throw refuse(index, text, "a cut before the start position, height and clearance are set");
            double enter = 0, leave = 1;
            if (clip(px, py, x, y, box, enter, leave)) {
                const double ex = px + (x - px) * enter, ey = py + (y - py) * enter, ez = pz + (z - pz) * enter;
                const double lx = px + (x - px) * leave, ly = py + (y - py) * leave, lz = pz + (z - pz) * leave;
                const double length = pymath::dist(ex, ey, lx, ly);
                // A move that only touches the tile's edge at a point cuts nothing in it.
                if (length > EDGE || (enter == 0 && leave == 1)) {
                    const std::string cut_feed = feed.empty() ? last_feed : feed;
                    if (!there || enter > 0) {
                        // Come to where the cut enters the tile at the clearance height, then down to its depth.
                        out.push_back("G0 " + at(ex, ey));
                        out.push_back("G1 Z" + fixed3(ez) + (plunge_feed.empty() ? cut_feed : plunge_feed));
                    }
                    out.push_back("G1 " + at(lx, ly) + cut_feed + (has_z ? " Z" + fixed3(lz) : std::string()));
                    tile.cut += length;
                    if (leave < 1) {
                        out.push_back("G0 Z" + fixed3(clear));
                        there = false;
                    } else {
                        there = true;
                    }
                } else {
                    there = false;
                }
            } else {
                there = false;
            }
        }
        if (has_x || has_y) have_xy = true;
        if (has_z) have_z = true;
        px = x;
        py = y;
        pz = z;
        if (out.size() > 2000000) throw ValueError("A tile's program exceeds 2,000,000 lines");
    }
}

// One tile of a laser job. The job has no heights: the beam is switched by M3 or M4 with a power and by M5, and
// each cutting move carries its feed and power. In a tile the beam is put on only at the start of a cut inside the
// tile, with the head already there, and is put out wherever a cut leaves the tile or the head has to travel.
void tile_laser_program(const std::vector<std::string> &lines, const double box[4], Tile &tile) {
    std::vector<std::string> &out = tile.lines;
    bool have_xy = false, there = false, lit = false;
    double px = 0, py = 0;
    std::string beam, last_feed, last_power;  // beam: the job's line that has the beam on, while it is on
    std::vector<Word> line;
    const auto refuse = [](size_t index, const std::string &text, const char *why) -> ValueError {
        return ValueError("Line " + std::to_string(index + 1) + " cannot be tiled (" + why + "): " + text);
    };
    const auto inside = [&](double x, double y) { return x >= box[0] - EDGE && x <= box[2] + EDGE && y >= box[1] - EDGE && y <= box[3] + EDGE; };
    const auto at = [&](double x, double y) { return "X" + fixed3(x - box[0]) + " Y" + fixed3(y - box[1]); };
    const auto dark = [&] {
        if (lit) out.push_back("M5");
        lit = false;
    };
    for (size_t index = 0; index < lines.size(); ++index) {
        const std::string &text = lines[index];
        if (text.empty() || text[0] == '(' || text[0] == ';' || text == "%") {
            out.push_back(text);
            continue;
        }
        if (!words(text, line)) throw refuse(index, text, "not understood");
        const bool motion = line[0].letter == 'G' && (line[0].value == 0 || line[0].value == 1);
        if (!motion) {
            if (line[0].letter == 'M' && (line[0].value == 3 || line[0].value == 4)) {
                if (line.size() > 2 || (line.size() == 2 && line[1].letter != 'S')) throw refuse(index, text, "unexpected word");
                beam = text;  // put on in a tile only where a cut starts
            } else if (line[0].letter == 'M' && line[0].value == 5 && line.size() == 1) {
                beam.clear();
                lit = false;
                out.push_back(text);
            } else if (passes(line)) {
                out.push_back(text);
            } else {
                throw refuse(index, text, "only straight moves in absolute millimetres are handled");
            }
            continue;
        }
        const bool cutting = line[0].value == 1;
        bool has_x = false, has_y = false;
        double x = px, y = py;
        std::string feed, power;
        for (size_t k = 1; k < line.size(); ++k) {
            const Word &word = line[k];
            if (word.letter == 'X' && !has_x) { has_x = true; x = word.value; }
            else if (word.letter == 'Y' && !has_y) { has_y = true; y = word.value; }
            else if (word.letter == 'F' && feed.empty() && cutting) feed = " F" + word.text;
            else if (word.letter == 'S' && power.empty() && cutting) power = " S" + word.text;
            else throw refuse(index, text, word.letter == 'Z' ? "a laser job with a height in it" : "unexpected word");
        }
        if (!has_x && !has_y) throw refuse(index, text, "a move with no axis");
        if (!feed.empty()) last_feed = feed;
        if (!power.empty()) last_power = power;
        if (!cutting || beam.empty()) {
            // Travel, with the beam out.
            dark();
            there = inside(x, y);
            if (there) out.push_back((cutting ? "G1 " : "G0 ") + at(x, y) + (cutting ? last_feed + " S0" : std::string()));
        } else {
            if (!have_xy) throw refuse(index, text, "a cut before the start position is set");
            double enter = 0, leave = 1;
            bool cut = false;
            if (clip(px, py, x, y, box, enter, leave)) {
                const double ex = px + (x - px) * enter, ey = py + (y - py) * enter;
                const double lx = px + (x - px) * leave, ly = py + (y - py) * leave;
                const double length = pymath::dist(ex, ey, lx, ly);
                if (length > EDGE || (enter == 0 && leave == 1)) {
                    if (!there || enter > 0) {
                        dark();
                        out.push_back("G0 " + at(ex, ey));
                    }
                    if (!lit) out.push_back(beam);
                    lit = true;
                    out.push_back("G1 " + at(lx, ly) + last_feed + last_power);
                    tile.cut += length;
                    cut = true;
                    there = leave == 1;
                    if (leave < 1) dark();
                }
            }
            if (!cut) {
                dark();
                there = false;
            }
        }
        have_xy = true;
        px = x;
        py = y;
        if (out.size() > 2000000) throw ValueError("A tile's program exceeds 2,000,000 lines");
    }
}

}  // namespace

// {"lines": [...], "tile_width", "tile_height", optionally "plunge" (the feed for coming back down into a cut;
// by default the feed of the job's own plunges)} -> {"columns", "rows", "bounds", "tile_width", "tile_height",
// "tiles": [{"column", "row", "origin": [x, y], "lines": [...], "cut_length"}, ...]}: one program for each tile
// that has cutting in it, row by row from the origin, each with its own corner as X0 Y0.
json tile_request(const json &request) {
    if (!request.is_object()) throw ValueError("The tiling request must be an object");
    const json *given = get(request, "lines");
    if (!given || !given->is_array()) throw ValueError("The tiling request needs the program lines");
    if (given->size() > 400000) throw ValueError("The program is too long to tile");
    std::vector<std::string> program;
    program.reserve(given->size());
    for (const json &entry : *given) {
        if (!entry.is_string()) throw ValueError("Every program line must be text");
        program.push_back(entry.get<std::string>());
    }
    const double width = need(request, "tile_width", "Tile width", 10, 10000), height = need(request, "tile_height", "Tile height", 10, 10000);
    const double plunge = get(request, "plunge") ? need(request, "plunge", "Plunge feed", 1, 20000) : 0;
    // A laser job is known by the power its cutting moves carry.
    bool laser = false;
    {
        std::vector<Word> line;
        for (const std::string &text : program) {
            if (text.empty() || text[0] == '(' || text[0] == ';' || text == "%" || !words(text, line)) continue;
            if (line[0].letter != 'G' || line[0].value != 1) continue;
            for (size_t k = 1; k < line.size() && !laser; ++k) laser = line[k].letter == 'S';
            if (laser) break;
        }
    }
    // Where the job cuts: the tiles are counted from the origin to its far corner.
    double bounds[4] = {0, 0, 0, 0};
    bool any = false;
    {
        std::vector<Word> line;
        double x = 0, y = 0;
        bool placed = false;
        for (const std::string &text : program) {
            if (text.empty() || text[0] == '(' || text[0] == ';' || text == "%" || !words(text, line)) continue;
            if (line[0].letter != 'G' || (line[0].value != 0 && line[0].value != 1)) continue;
            const double fx = x, fy = y;
            bool moved = false;
            for (size_t k = 1; k < line.size(); ++k) {
                if (line[k].letter == 'X') { x = line[k].value; moved = true; }
                if (line[k].letter == 'Y') { y = line[k].value; moved = true; }
            }
            if (line[0].value == 1 && placed && moved) {
                const double xs[2] = {fx, x}, ys[2] = {fy, y};
                for (int k = 0; k < 2; ++k) {
                    bounds[0] = any ? std::min(bounds[0], xs[k]) : xs[k];
                    bounds[1] = any ? std::min(bounds[1], ys[k]) : ys[k];
                    bounds[2] = any ? std::max(bounds[2], xs[k]) : xs[k];
                    bounds[3] = any ? std::max(bounds[3], ys[k]) : ys[k];
                    any = true;
                }
            }
            if (moved) placed = true;
        }
    }
    if (!any) throw ValueError("The program has no cutting moves to tile");
    if (bounds[0] < -EDGE || bounds[1] < -EDGE) throw ValueError("Tiling needs the whole job at X0 Y0 or beyond; move the design into the work area");
    const size_t columns = static_cast<size_t>(std::max(1.0, std::ceil((bounds[2] - EDGE) / width)));
    const size_t rows = static_cast<size_t>(std::max(1.0, std::ceil((bounds[3] - EDGE) / height)));
    if (columns * rows > 100) throw ValueError("That is more than 100 tiles; use larger tiles");
    json tiles = json::array();
    for (size_t row = 0; row < rows; ++row)
        for (size_t column = 0; column < columns; ++column) {
            Tile tile;
            tile.column = column;
            tile.row = row;
            const double x0 = width * static_cast<double>(column), y0 = height * static_cast<double>(row);
            const double box[4] = {x0, y0, x0 + width, y0 + height};
            if (laser) tile_laser_program(program, box, tile);
            else tile_program(program, box, plunge, tile);
            if (tile.cut <= EDGE) continue;
            tiles.push_back(json{{"column", column}, {"row", row}, {"origin", json::array({x0, y0})}, {"lines", tile.lines}, {"cut_length", tile.cut}});
        }
    return json{{"columns", columns},
                {"rows", rows},
                {"bounds", json::array({bounds[0], bounds[1], bounds[2], bounds[3]})},
                {"tile_width", width},
                {"tile_height", height},
                {"kind", laser ? "laser" : "cutter"},
                {"tiles", tiles}};
}

json levelling_call(const std::string &command, const json &request) {
    if (command == "tile") return tile_request(request);
    if (command == "probe-plan") return probe_plan(request);
    if (command == "height-map") return to_json(height_map_from_probes(request));
    if (command == "level") return level_request(request);
    throw ValueError("Unknown command: " + command);
}

}  // namespace kerfkite
