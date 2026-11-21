// Polygon operations: studio/operations.py, statement for statement where order affects the result.
//
// The reference works through Shapely; geometry.hpp makes the same GEOS calls. What must match is listed
// here because none of it is visible in the reference's source:
//
// * Polygon(path) keeps the vertices as given and repeats the first vertex when the last is not exactly
//   equal to it. A contour that closes to within a millionth of a millimetre but not exactly therefore
//   gains a vertex.
// * The region starts as an empty collection and takes the symmetric difference with each contour in
//   turn (even-odd fill).
// * buffer() is called with 32 segments a quarter turn and Shapely's other defaults: round caps, round
//   joins, mitre limit 5, both sides.
// * outlines() lists polygons by area, smallest first, keeping GEOS's order for equal areas, and for each
//   polygon its holes before its outer ring. Rings start where GEOS starts them.
// * Coordinates are doubles from end to end: Shapely passes them through NumPy as float64 unchanged.
#include "operations.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "geometry.hpp"
#include "kerfkite/py_math.hpp"

namespace kerfkite {
namespace detail {

// The sine and cosine are taken through calls that are not inlined, so that the compiler does not work
// them out itself with a different rounding from the C library the reference uses.
#if defined(_MSC_VER)
#define KERFKITE_NOINLINE __declspec(noinline)
#else
#define KERFKITE_NOINLINE __attribute__((noinline))
#endif

namespace {

using Path = std::vector<std::array<double, 2>>;

KERFKITE_NOINLINE double sine(double x) { return std::sin(x); }
KERFKITE_NOINLINE double cosine(double x) { return std::cos(x); }

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

// studio.operations.region(paths): the even-odd region of closed contours. The paths have passed
// studio.cam's validation, so the reference's first check, that they are lists of finite XY points,
// cannot fail.
geometry::Geometry region(geometry::Context &context, const std::vector<Path> &paths) {
    geometry::Geometry result = context.empty_collection();
    for (const Path &path : paths) {
        if (path.size() < 4 || pymath::dist(path.front()[0], path.front()[1], path.back()[0], path.back()[1]) > 1e-6)
            throw ValueError("This operation needs closed contours");
        const geometry::Geometry polygon = context.polygon(path);
        if (!polygon.valid() || polygon.area() < 1e-9) throw ValueError("Repair self-crossing or zero-area contours before CAM");
        result = context.symmetric_difference(result, polygon);
    }
    if (result.empty()) throw ValueError("Contours cancel under even-odd fill");
    return result;
}

// shapely.affinity.rotate(geom, angle, origin=(0, 0)), angle in degrees. With the origin at zero both
// offsets of the matrix come to 0.0, which is still added, as the reference adds it.
geometry::Geometry rotate(geometry::Context &context, const geometry::Geometry &geom, double angle) {
    if (geom.empty()) return geom.clone();
    const double pi = 3.141592653589793;  // math.pi
    const double turn = angle * pi / 180.0;
    double cosp = cosine(turn), sinp = sine(turn);
    if (std::fabs(cosp) < 2.5e-16) cosp = 0.0;
    if (std::fabs(sinp) < 2.5e-16) sinp = 0.0;
    return context.affine(geom, cosp, -sinp, sinp, cosp, 0.0, 0.0);
}

// studio.operations.lines(g): a line gives itself; a multi-part geometry or collection gives the lines
// found in its members, in order; anything else, a point where a row only touches a corner for one, gives
// nothing.
void lines(const geometry::Geometry &geom, std::vector<geometry::Geometry> &out) {
    const geometry::Kind kind = geom.kind();
    if (kind == geometry::Kind::LineString) {
        out.push_back(geom.clone());
    } else if (kind == geometry::Kind::MultiPoint || kind == geometry::Kind::MultiLineString ||
               kind == geometry::Kind::MultiPolygon || kind == geometry::Kind::Collection) {
        const size_t count = geom.parts();
        for (size_t i = 0; i < count; ++i) lines(geom.part(i), out);
    }
}

// studio.operations.hatch(g, spacing, angle): rows across the region turned by -angle, each row cut to
// the region and turned back, alternate rows reversed so that the beam sweeps to and fro.
void hatch(geometry::Context &context, const geometry::Geometry &g, double spacing, double angle, std::vector<Path> &result) {
    const geometry::Geometry rotated = rotate(context, g, -angle);
    const std::array<double, 4> box = rotated.bounds();
    const double x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
    const double rows = std::ceil((y1 - y0) / spacing);
    if (rows > 20000) throw ValueError("Too many hatch rows; increase interval");
    for (long i = 0; i <= static_cast<long>(rows); ++i) {
        const double y = py_min(y1, y0 + static_cast<double>(i) * spacing);
        std::vector<geometry::Geometry> segments;
        lines(context.intersection(rotated, context.line({{x0 - 1, y}, {x1 + 1, y}})), segments);
        const bool back = i % 2 != 0;
        if (back) std::reverse(segments.begin(), segments.end());
        for (const geometry::Geometry &line : segments) {
            Path coords = rotate(context, line, angle).coordinates();
            if (back) std::reverse(coords.begin(), coords.end());
            if (line.length() > 1e-6) result.push_back(std::move(coords));
        }
    }
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
    geometry::Context context;
    const geometry::Geometry g = region(context, paths);
    geometry::BufferStyle style;
    style.quad_segs = 32;
    const bool outside = is_text(operation, "outside");
    if (outside || is_text(operation, "inside")) {
        const double radius = (router ? value(project, "tool_diameter", 3.175, 0.01, 100, "0.01", "100")
                                      : value(project, "kerf", 0.1, 0, 5, "0", "5")) /
                              2;
        const geometry::Geometry result = context.buffer(g, outside ? radius : -radius, style);
        if (result.empty()) throw ValueError("Tool does not fit inside these contours");
        paths = geometry::outlines(result);
        warnings.push_back("Compensation follows even-odd regions; nested contours become holes. Features smaller than "
                           "the cutter may be inaccessible.");
    } else if (is_text(operation, "pocket")) {
        if (!router) throw ValueError("Pocketing requires router mode");
        const double radius = value(project, "tool_diameter", 3.175, 0.01, 100, "0.01", "100") / 2;
        const double step = value(project, "stepover", 1, 0.01, 100, "0.01", "100");
        if (step > radius) throw ValueError("Pocket stepover cannot exceed cutter radius (50% diameter)");
        geometry::Geometry inset = context.buffer(g, -radius, style);
        if (inset.empty()) throw ValueError("Cutter cannot enter this pocket");
        paths.clear();
        size_t vertices = 0;
        while (!inset.empty()) {
            for (Path &ring : geometry::outlines(inset)) {
                vertices += ring.size();
                paths.push_back(std::move(ring));
            }
            if (vertices > 100000) throw ValueError("Pocket exceeds vertex limit");
            inset = context.buffer(inset, -step, style);
        }
        warnings.push_back("Concentric pocket clearing preserves islands. Narrow residual features may remain.");
        if (is_text_or_absent(get(project, "router_entry"), "plunge")) warnings.push_back("Plunge entry requires a centre-cutting tool.");
    } else if (is_text(operation, "fill")) {
        if (!laser) throw ValueError("Hatch fill requires laser mode");
        const double interval = value(project, "interval", 0.2, 0.02, 20, "0.02", "20");
        const double angle = value(project, "hatch_angle", 0, -360, 360, "-360", "360");
        std::vector<Path> rows;
        hatch(context, g, interval, angle, rows);
        if (truthy(get(project, "crosshatch"))) hatch(context, g, interval, angle + 90, rows);
        paths = std::move(rows);
    } else if (is_text(operation, "vcarve")) {
        if (!router) throw ValueError("V-carving requires router mode");
        throw NotPorted("V-carving");
    } else {
        throw ValueError("Unsupported operation");
    }
    if (paths.empty()) throw ValueError("Operation produced no toolpaths");
    return warnings;
}

}  // namespace

std::vector<std::string> prepare_operation(const json &project, std::vector<Path> &paths, double width, double height) {
    if (!geometry::available()) throw NotPorted("offset, pocket and fill operations");
    std::vector<std::string> warnings;
    try {
        warnings = prepare(project, paths);
    } catch (const geometry::Error &) {
        // The reference stops here with Shapely's GEOSException, which is not a rejection with a message.
        throw NotPorted("a contour that GEOS cannot process");
    }
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
}  // namespace kerfkite
