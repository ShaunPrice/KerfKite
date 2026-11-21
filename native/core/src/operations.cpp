// Polygon operations: studio/operations.py on KerfKite's own polygon kernel (kerfkite/polygon.hpp).
//
// This file is the one implementation of inside and outside offsets, pockets and hatch fills. The
// command-line tool reaches it through generate(), and Studio's Python through the "operation" command
// of its compiled extension, so both cut the same paths.
//
// What the kernel fixes, none of it visible in the reference's source:
//
// * Contours are placed on the kernel's grid of a millionth of a millimetre, a vertex that repeats the
//   one before it is dropped, and the region is their even-odd combination.
// * Offsets are round, 32 segments a quarter turn, with the corners of each arc outside the true arc, so
//   that a tool path is never nearer to the contour than the tool's radius.
// * outlines() lists polygons by area, smallest first, and for each polygon its holes before its outer
//   ring. Rings are closed, start at their left-most vertex, and outer rings run clockwise.
// * A grid coordinate n becomes the double n / 1e6: one correctly rounded division, so the same double on
//   every platform. The program writes three decimals, a thousand grid steps, so the conversion never
//   decides a digit.
// * Hatch rows are turned with a sine and cosine computed here by a fixed series of + - * /, not by the C
//   library, whose last bit differs between platforms.
// * A piece of a hatch row shorter than 0.001 mm is dropped: the program's coordinates have three decimals,
//   so a shorter stroke cannot be cut. (The Shapely reference dropped pieces up to 0.000001 mm.)
#include "operations.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "kerfkite/operation_command.hpp"
#include "kerfkite/polygon.hpp"
#include "kerfkite/py_math.hpp"
#include "polygon/kernel.hpp"

namespace kerfkite {
namespace detail {

namespace {

using Path = std::vector<std::array<double, 2>>;
using polygon::PolygonSet;

// The pocket's limit on vertices, as the reference has it. The kernel's pockets have far fewer vertices
// than the reference's had, so it now takes a very large pocket to reach it.
const std::size_t kPocketVertexLimit = 100000;

// studio.operations.value(data, key, default, lo, hi). lo_text and hi_text are the bounds as the
// reference's message writes them.
double value(const json &project, const char *key, double fallback, double lo, double hi, const char *lo_text,
             const char *hi_text) {
    const json *found = get(project, key);
    if (!found) return fallback;
    double result = 0;
    const bool readable = !found->is_boolean() && (plain_number(*found, result) ||
                                                   (found->is_string() && float_from_text(found->get_ref<const std::string &>(), result)));
    if (!readable) throw ValueError(std::string(key) + " must be numeric");
    if (!std::isfinite(result) || !(lo <= result && result <= hi))
        throw ValueError(std::string(key) + " must be between " + lo_text + " and " + hi_text);
    return result;
}

}  // namespace

// studio.operations.region(paths): the even-odd region of closed contours. The paths have passed
// validation as lists of finite XY points before they arrive here.
//
// A contour counts as closed when its last point is within a millionth of a millimetre of its first, and
// that last point is then taken to be the first: it is not a further vertex. (Placed on the grid on its own
// it could land one step from the first and make a spike one step long.)
PolygonSet contour_region(const std::vector<Path> &paths) {
    std::vector<polygon::Ring> contours;
    contours.reserve(paths.size());
    for (const Path &path : paths) {
        if (path.size() < 4 || pymath::dist(path.front()[0], path.front()[1], path.back()[0], path.back()[1]) > 1e-6)
            throw ValueError("This operation needs closed contours");
        polygon::Ring ring = polygon::ring_from_mm(Path(path.begin(), path.end() - 1));
        if (!polygon::is_simple(ring) || std::fabs(polygon::signed_area(ring)) < 1e-9)
            throw ValueError("Repair self-crossing or zero-area contours before CAM");
        contours.push_back(std::move(ring));
    }
    PolygonSet result = polygon::compose(contours);
    if (result.empty()) throw ValueError("Contours cancel under even-odd fill");
    return result;
}

namespace {

// The cosine and sine of an angle given in degrees, the same bits on every platform. The angle is folded
// into the first eighth of a turn exactly (fmod and these subtractions do not round), so that a whole
// number of quarter turns gives exactly 0 and 1, and the series runs on at most 45 degrees.
void turn(double degrees, double &cosine, double &sine) {
    const double a = std::fmod(std::fabs(degrees), 360.0);
    const int quarter = a >= 270 ? 3 : a >= 180 ? 2 : a >= 90 ? 1 : 0;
    const double within = a - 90.0 * quarter;
    const bool folded = within > 45;
    const double pi = 3.141592653589793;
    double c, s;
    polygon::detail::series_cos_sin((folded ? 90.0 - within : within) * pi / 180.0, c, s);
    if (folded) std::swap(c, s);
    // A quarter turn on takes (c, s) to (-s, c).
    for (int i = 0; i < quarter; ++i) {
        const double was = c;
        c = -s;
        s = was;
    }
    cosine = c == 0 ? 0.0 : c;  // not minus zero
    sine = s == 0 ? 0.0 : degrees < 0 ? -s : s;
}

// studio.operations.hatch(g, spacing, angle): rows across the region as it lies when turned by -angle,
// each row cut to the region, alternate rows reversed so that the beam sweeps to and fro.
//
// The region stays where it is: its extent is measured along the turned axes, and each row is turned back
// by +angle before it is cut, which is the same rows without moving the region off its grid.
void hatch(const PolygonSet &g, double spacing, double angle, std::vector<Path> &result) {
    double c, s;
    turn(angle, c, s);
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const auto measure = [&](const polygon::Ring &ring) {
        for (const polygon::Point &p : ring) {
            const double px = polygon::to_mm(p.x), py = polygon::to_mm(p.y);
            const double x = c * px + s * py, y = c * py - s * px;  // turned by -angle
            if (!any) x0 = x1 = x, y0 = y1 = y, any = true;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
    };
    for (const polygon::Polygon &part : g) {
        measure(part.shell);
        for (const polygon::Ring &hole : part.holes) measure(hole);
    }
    const double rows = std::ceil((y1 - y0) / spacing);
    if (!(rows <= 20000)) throw ValueError("Too many hatch rows; increase interval");
    const polygon::LineClipper clipper(g);
    const auto back = [&](double x, double y) { return polygon::to_grid(polygon::PointMm{c * x - s * y, s * x + c * y}); };  // turned by +angle
    for (long i = 0; i <= static_cast<long>(rows); ++i) {
        const double y = py_min(y1, y0 + static_cast<double>(i) * spacing);
        std::vector<polygon::Segment> parts = clipper.clip(back(x0 - 1, y), back(x1 + 1, y));
        const bool reversed = i % 2 != 0;
        if (reversed) std::reverse(parts.begin(), parts.end());
        for (const polygon::Segment &part : parts) {
            // Shorter than 0.001 mm, a thousand grid steps: nothing the program could write.
            const polygon::Coord dx = part[1].x - part[0].x, dy = part[1].y - part[0].y;
            const polygon::Coord ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            if (ax < 1000 && ay < 1000 && ax * ax + ay * ay < 1000000) continue;
            const polygon::Point &from = part[reversed ? 1 : 0], &to = part[reversed ? 0 : 1];
            result.push_back({polygon::to_mm(from), polygon::to_mm(to)});
        }
    }
}

// The three operations on a region. Each is the reference's statement for statement; the messages are the
// reference's.
std::vector<Path> offset_paths(const PolygonSet &g, double distance) {
    const PolygonSet result = polygon::offset(g, distance);
    if (result.empty()) throw ValueError("Tool does not fit inside these contours");
    return polygon::outlines(result);
}

std::vector<Path> pocket_paths(const PolygonSet &g, double radius, double step) {
    PolygonSet inset = polygon::offset(g, -radius);
    if (inset.empty()) throw ValueError("Cutter cannot enter this pocket");
    std::vector<Path> paths;
    std::size_t vertices = 0;
    while (!inset.empty()) {
        for (Path &ring : polygon::outlines(inset)) {
            vertices += ring.size();
            paths.push_back(std::move(ring));
        }
        if (vertices > kPocketVertexLimit) throw ValueError("Pocket exceeds vertex limit");
        inset = polygon::offset(inset, -step);
    }
    return paths;
}

std::vector<Path> fill_paths(const PolygonSet &g, double interval, double angle, bool crosshatch) {
    std::vector<Path> rows;
    hatch(g, interval, angle, rows);
    if (crosshatch) hatch(g, interval, angle + 90, rows);
    return rows;
}

// Python's truth of a JSON value: None, False, zero, and empty text, lists and objects are false.
bool truthy(const json *value) {
    if (!value || value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_number_float()) return value->get<double>() != 0;
    if (value->is_number_unsigned()) return value->get<uint64_t>() != 0;
    if (value->is_number_integer()) return value->get<int64_t>() != 0;
    if (value->is_string()) return !value->get_ref<const std::string &>().empty();
    return !value->empty();
}

std::vector<std::string> prepare(const json &project, std::vector<Path> &paths) {
    const json *operation = get(project, "operation");
    const bool router = is_text(get(project, "mode"), "router"), laser = is_text(get(project, "mode"), "laser");
    std::vector<std::string> warnings;
    const PolygonSet g = contour_region(paths);
    const bool outside = is_text(operation, "outside");
    if (outside || is_text(operation, "inside")) {
        const double radius = (router ? value(project, "tool_diameter", 3.175, 0.01, 100, "0.01", "100")
                                      : value(project, "kerf", 0.1, 0, 5, "0", "5")) /
                              2;
        paths = offset_paths(g, outside ? radius : -radius);
        warnings.push_back("Compensation follows even-odd regions; nested contours become holes. Features smaller than "
                           "the cutter may be inaccessible.");
    } else if (is_text(operation, "pocket")) {
        if (!router) throw ValueError("Pocketing requires router mode");
        const double radius = value(project, "tool_diameter", 3.175, 0.01, 100, "0.01", "100") / 2;
        const double step = value(project, "stepover", 1, 0.01, 100, "0.01", "100");
        if (step > radius) throw ValueError("Pocket stepover cannot exceed cutter radius (50% diameter)");
        paths = pocket_paths(g, radius, step);
        warnings.push_back("Concentric pocket clearing preserves islands. Narrow residual features may remain.");
        if (is_text_or_absent(get(project, "router_entry"), "plunge")) warnings.push_back("Plunge entry requires a centre-cutting tool.");
    } else if (is_text(operation, "fill")) {
        if (!laser) throw ValueError("Hatch fill requires laser mode");
        const double interval = value(project, "interval", 0.2, 0.02, 20, "0.02", "20");
        const double angle = value(project, "hatch_angle", 0, -360, 360, "-360", "360");
        paths = fill_paths(g, interval, angle, truthy(get(project, "crosshatch")));
    } else if (is_text(operation, "vcarve")) {
        if (!router) throw ValueError("V-carving requires router mode");
        throw NotPorted("V-carving");
    } else {
        throw ValueError("Unsupported operation");
    }
    if (paths.empty()) throw ValueError("Operation produced no toolpaths");
    return warnings;
}

// The contours of an "operation" request: a list of lists of [x, y] numbers.
std::vector<Path> contours(const json &request) {
    const char *message = "Contours must be lists of finite XY points";
    const json *given = request.is_object() ? get(request, "paths") : nullptr;
    if (!given || !given->is_array()) throw ValueError(message);
    std::vector<Path> paths;
    paths.reserve(given->size());
    for (const json &contour : *given) {
        if (!contour.is_array()) throw ValueError(message);
        Path path;
        path.reserve(contour.size());
        for (const json &point : contour) {
            if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) throw ValueError(message);
            path.push_back({point[0].get<double>(), point[1].get<double>()});
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

double request_number(const json &request, const char *key) {
    const json *found = get(request, key);
    if (!found || !found->is_number()) throw ValueError(std::string("The operation request needs a number for \"") + key + "\"");
    return found->get<double>();
}

}  // namespace

std::vector<std::string> prepare_operation(const json &project, std::vector<Path> &paths, double width, double height) {
    std::vector<std::string> warnings = prepare(project, paths);
    // studio.cam: compensated geometry is checked against the work area again, X before Y at each point.
    const std::string width_text = text::repr(width), height_text = text::repr(height);
    for (const Path &path : paths)
        for (const std::array<double, 2> &point : path)
            if (!(0 <= point[0] && point[0] <= width && 0 <= point[1] && point[1] <= height)) {
                const json x = point[0], y = point[1];
                number(&x, "Compensated X", 0, width, "0", width_text);
                number(&y, "Compensated Y", 0, height, "0", height_text);
            }
    return warnings;
}

}  // namespace detail

bool operation_command(const std::string &command) { return command == "operation"; }

Json operation_call(const std::string &command, const Json &request) {
    using namespace detail;
    if (command != "operation") throw ValueError("Unknown command: " + command);
    const json *name = request.is_object() ? get(request, "op") : nullptr;
    const std::string op = name && name->is_string() ? name->get<std::string>() : std::string();
    if (op == "helix") {
        const json *given = get(request, "seams");
        if (!given || !given->is_array()) throw ValueError("The operation request needs a list of points for \"seams\"");
        std::vector<std::array<double, 2>> seams;
        seams.reserve(given->size());
        for (const json &point : *given) {
            if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number())
                throw ValueError("The operation request needs a list of points for \"seams\"");
            seams.push_back({point[0].get<double>(), point[1].get<double>()});
        }
        const double bit_radius = request_number(request, "bit_radius"), radius = request_number(request, "radius");
        // Smaller than this the shrunk regions would be the pocket itself.
        if (!(bit_radius >= 0.001) || !(radius >= 0.001))
            throw ValueError("A helical entry needs a cutter radius and a helix radius of at least 0.001 mm");
        json entries = json::array();
        for (const HelixEntry &entry : pocket_entries(contours(request), seams, bit_radius, radius))
            entries.push_back(json::array({entry.x, entry.y, entry.angle}));
        return json{{"entries", std::move(entries)}};
    }
    if (op != "region" && op != "offset" && op != "pocket" && op != "fill") throw ValueError("Unknown operation: " + op);
    const PolygonSet g = contour_region(contours(request));
    std::vector<Path> paths;
    if (op == "region") {
        paths = polygon::outlines(g);
    } else if (op == "offset") {
        paths = offset_paths(g, request_number(request, "distance"));
    } else if (op == "pocket") {
        const double radius = request_number(request, "radius"), step = request_number(request, "step");
        // Smaller than this the passes would not move, and the loop would not end.
        if (!(radius >= 0.001) || !(step >= 0.001)) throw ValueError("A pocket needs a cutter radius and a stepover of at least 0.001 mm");
        paths = pocket_paths(g, radius, step);
    } else {
        const double interval = request_number(request, "interval");
        if (!(interval > 0)) throw ValueError("A fill needs an interval greater than zero");
        const json *cross = get(request, "crosshatch");
        paths = fill_paths(g, interval, request_number(request, "angle"), cross && cross->is_boolean() && cross->get<bool>());
    }
    json out = json::array();
    for (const Path &path : paths) {
        json points = json::array();
        for (const std::array<double, 2> &point : path) points.push_back(json::array({point[0], point[1]}));
        out.push_back(std::move(points));
    }
    return json{{"paths", std::move(out)}};
}

}  // namespace kerfkite
