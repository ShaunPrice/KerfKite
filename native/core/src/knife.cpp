// Drag-knife paths: studio.operations.value, knife and the knife branch of prepare, statement for statement
// where order affects the result.
//
// The swivel arcs call the platform's atan2, sin and cos once each, as Python does, and the remaining
// arithmetic keeps the reference's order of operations: the points are compared with it to the last bit.
#include "knife.hpp"

#include <cmath>
#include <string>

#include "kerfkite/py_math.hpp"
#include "py_trig.hpp"

namespace kerfkite {
namespace detail {

namespace {

const char *const KNIFE_WARNING =
    "Knife alignment lead-in marks material before each contour. Test blade offset and corner swivels on scrap.";

using Point = std::array<double, 2>;
using Path = std::vector<Point>;

double dist(const Point &a, const Point &b) { return pymath::dist(a[0], a[1], b[0], b[1]); }

// studio.operations.value(data, key, default, lo, hi). The default takes the range check as well.
double value(const json &data, const char *key, double fallback, double lo, double hi, const std::string &lo_text,
             const std::string &hi_text) {
    double result = fallback;
    if (const json *given = get(data, key)) {
        const bool readable = !given->is_boolean() &&
                              (plain_number(*given, result) ||
                               (given->is_string() && float_from_text(given->get_ref<const std::string &>(), result)));
        if (!readable) throw ValueError(std::string(key) + " must be numeric");
    }
    if (!std::isfinite(result) || !(lo <= result && result <= hi))
        throw ValueError(std::string(key) + " must be between " + lo_text + " and " + hi_text);
    return result;
}

// Two points of a path nearer than this (0.1 nm) are one point: studio.operations.KNIFE_REPEAT.
const double KNIFE_REPEAT = 1e-7;

// studio.operations.knife_points(path): a point is kept only when it is further than KNIFE_REPEAT from the
// last point kept, so every move that remains has a length and a direction.
Path knife_points(const Path &path) {
    Path kept{path[0]};
    for (size_t i = 1; i < path.size(); ++i)
        if (dist(kept.back(), path[i]) > KNIFE_REPEAT) kept.push_back(path[i]);
    return kept;
}

// studio.operations.knife(paths, offset, lead)
std::vector<Path> knife(const std::vector<Path> &paths, double offset, double lead) {
    const double five_degrees = 5 * (pytrig::PI / 180.0);  // math.radians(5)
    std::vector<Path> result;
    for (const Path &path : paths) {
        const Path p = knife_points(path);
        if (p.size() < 2) throw ValueError("Knife paths must contain nonzero travel");  // no direction to cut in
        Path vectors;
        vectors.reserve(p.size() - 1);
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            const double length = dist(p[i], p[i + 1]);  // more than KNIFE_REPEAT, so never zero
            vectors.push_back({(p[i + 1][0] - p[i][0]) / length, (p[i + 1][1] - p[i][1]) / length});
        }
        const double ux = vectors[0][0], uy = vectors[0][1];
        Path q = {{p[0][0] + (offset - lead) * ux, p[0][1] + (offset - lead) * uy},
                  {p[0][0] + offset * ux, p[0][1] + offset * uy}};
        const bool closed = p.back()[0] == p[0][0] && p.back()[1] == p[0][1];
        for (size_t i = 0; i < vectors.size(); ++i) {
            const double vx = vectors[i][0], vy = vectors[i][1];
            const double x = p[i + 1][0], y = p[i + 1][1];
            q.push_back({x + offset * vx, y + offset * vy});
            const Point *next = i + 1 < vectors.size() ? &vectors[i + 1] : closed ? &vectors[0] : nullptr;
            if (!next) continue;
            const double a = pytrig::atan2(vy, vx);
            const double b = pytrig::atan2((*next)[1], (*next)[0]);
            const double delta = pytrig::modulo(b - a + pytrig::PI, 2 * pytrig::PI) - pytrig::PI;
            const double n = py_max(1, std::ceil(std::fabs(delta) / five_degrees));
            for (double j = 1; j <= n; ++j)
                q.push_back({x + offset * pytrig::cosine(a + delta * j / n), y + offset * pytrig::sine(a + delta * j / n)});
        }
        result.push_back(std::move(q));
    }
    return result;
}

}  // namespace

std::vector<Path> knife_paths(const json &project, const std::vector<Path> &paths, double width, double height,
                              std::vector<std::string> &warnings) {
    const double offset = value(project, "knife_offset", 0.25, 0.01, 5, "0.01", "5");
    const double lead = value(project, "knife_lead", 1, offset, 20, text::repr(offset), "20");
    std::vector<Path> result = knife(paths, offset, lead);
    warnings.push_back(KNIFE_WARNING);
    // studio.cam: compensated geometry must still lie in the work area. Every coordinate here is finite.
    const std::string width_text = text::repr(width), height_text = text::repr(height);
    for (const Path &path : result)
        for (const Point &point : path) {
            if (!(0 <= point[0] && point[0] <= width))
                throw ValueError("Compensated X must be between 0 and " + width_text);
            if (!(0 <= point[1] && point[1] <= height))
                throw ValueError("Compensated Y must be between 0 and " + height_text);
        }
    return result;
}

}  // namespace detail
}  // namespace kerfkite
