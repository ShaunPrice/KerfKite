// Vector laser lead geometry, distance-based power ramps and beam-off intervals: studio/laser.py.
//
// Lengths use Python's math.dist and totals its sum(); see kerfkite/py_math.hpp. The arithmetic below keeps
// the reference's order of operations, because the result is compared with it to the last bit.
#include "laser.hpp"

#include <algorithm>

#include "kerfkite/py_math.hpp"

#if defined(_MSC_VER)
#define KERFKITE_NOINLINE __declspec(noinline)
#else
#define KERFKITE_NOINLINE __attribute__((noinline))
#endif

namespace kerfkite {
namespace detail {

namespace {

using Point = std::array<double, 2>;
const double PI = 3.141592653589793;  // math.pi

// One library call each, as Python makes them: kept out of line so that a compiler does not merge a sine
// and a cosine of the same angle into a single combined call.
KERFKITE_NOINLINE double sine(double x) { return std::sin(x); }
KERFKITE_NOINLINE double cosine(double x) { return std::cos(x); }
KERFKITE_NOINLINE double arc_cosine(double x) { return std::acos(x); }

double dist(const Point &a, const Point &b) { return pymath::dist(a[0], a[1], b[0], b[1]); }

// Additional points ordered in travel direction. The arc length is the dimension the editor shows.
std::vector<Point> lead(const Point &a, const Point &b, double length, LaserCut::Shape shape, bool entering) {
    std::vector<Point> result;
    if (length < 1e-9) return result;
    const double edge = dist(a, b);
    const double tx = (b[0] - a[0]) / edge, ty = (b[1] - a[1]) / edge;
    const double x = entering ? a[0] : b[0], y = entering ? a[1] : b[1];
    const double sign = entering ? -1 : 1;
    if (shape == LaserCut::Straight) {
        result.push_back({x + sign * length * tx, y + sign * length * ty});
        return result;
    }
    const double radius = 2 * length / PI;
    const double side = shape == LaserCut::ArcLeft ? 1 : -1;
    // Chord sagitta at most 0.01 mm; at least nine chords per quarter circle.
    const double reach = 1 - 0.01 / radius;
    const double by_sagitta = 2 * arc_cosine(py_max(-1, reach));
    const double max_step = py_min(PI / 18, by_sagitta);
    const double steps = std::ceil((PI / 2) / max_step);
    for (double i = 1; i <= steps; ++i) {
        const double theta = (PI / 2) * i / steps;
        const double along = sign * radius * sine(theta);
        const double across = side * radius * (1 - cosine(theta));
        result.push_back({x + along * tx - across * ty, y + along * ty + across * tx});
    }
    if (entering) std::reverse(result.begin(), result.end());
    return result;
}

std::vector<double> gap_boundaries(double length, double offset, const std::vector<Point> &points, const LaserCut &cut) {
    std::vector<double> result;
    if (cut.pattern == LaserCut::Continuous) return result;
    const double gap = cut.gap;
    const auto add = [&](double lo) {
        if (!(lo < length)) return;
        result.push_back(offset + lo);
        result.push_back(offset + py_min(length, lo + gap));
    };
    if (cut.pattern == LaserCut::Bridges) {
        if (points.size() < 4 || dist(points.front(), points.back()) > 1e-7)
            throw ValueError("Laser holding bridges require closed contours");
        const double count = static_cast<double>(cut.count);
        const double pitch = length / count;
        if (gap >= pitch - 1e-9) throw ValueError("Holding bridges cover the contour; reduce their width or count");
        for (double i = 0; i < count; ++i) add((i + 0.5) * pitch - gap / 2);
    } else {
        const double dash = cut.dash;
        const double pitch = dash + gap;
        const double count = std::ceil(length / pitch);
        if (count > 100000) throw ValueError("Perforation exceeds the segment limit; increase the cut or gap length");
        for (double i = 0; i < count; ++i) add(i * pitch + dash);
    }
    return result;
}

}  // namespace

LaserCut cut_options(const json &project) {
    LaserCut cut;
    const json *pattern = get(project, "laser_pattern"), *shape = get(project, "lead_shape");
    if (!pattern || is_text(pattern, "continuous")) cut.pattern = LaserCut::Continuous;
    else if (is_text(pattern, "perforate")) cut.pattern = LaserCut::Perforate;
    else if (is_text(pattern, "bridges")) cut.pattern = LaserCut::Bridges;
    else throw ValueError("Choose continuous cutting, perforation or holding bridges");
    if (!shape || is_text(shape, "straight")) cut.shape = LaserCut::Straight;
    else if (is_text(shape, "arc-left")) cut.shape = LaserCut::ArcLeft;
    else if (is_text(shape, "arc-right")) cut.shape = LaserCut::ArcRight;
    else throw ValueError("Choose a straight, left or right tangent lead");
    if (cut.pattern != LaserCut::Continuous) {
        const json *operation = get(project, "operation");
        const bool profile = !operation || is_text(operation, "line") || is_text(operation, "inside") ||
                             is_text(operation, "outside");
        if (!none(get(project, "path_powers")) || !profile)
            throw ValueError("Laser perforation and bridges require vector line or profile operations");
        cut.gap = setting(project, "laser_gap", 1, "Beam-off gap (mm)", 0.01, 100, "0.01", "100");
    }
    if (cut.pattern == LaserCut::Perforate)
        cut.dash = setting(project, "laser_dash", 3, "Perforation cut (mm)", 0.01, 1000, "0.01", "1000");
    if (cut.pattern == LaserCut::Bridges) {
        const double count = setting(project, "laser_bridge_count", 4, "Bridges per contour", 1, 1000, "1", "1000");
        if (count != std::floor(count)) throw ValueError("Bridges per contour must be a whole number");
        cut.count = static_cast<long>(count);
    }
    return cut;
}

std::vector<LaserVertex> prepare(const std::vector<Point> &path, double power, const double *options, double width,
                                 double height, const LaserCut &cut) {
    std::vector<Point> contour;
    for (const Point &point : path)
        if (contour.empty() || dist(contour.back(), point) > 1e-9) contour.push_back(point);
    if (contour.size() < 2) throw ValueError("Laser paths must contain nonzero travel");
    const double lead_in = options[0], lead_out = options[1], ramp_in = options[2], ramp_out = options[3],
                 start = options[4], end = options[5];
    const std::vector<Point> before = lead(contour[0], contour[1], lead_in, cut.shape, true);
    const std::vector<Point> after =
        lead(contour[contour.size() - 2], contour[contour.size() - 1], lead_out, cut.shape, false);
    std::vector<Point> points = before;
    points.insert(points.end(), contour.begin(), contour.end());
    points.insert(points.end(), after.begin(), after.end());
    for (const Point &p : points)
        if (p[0] < 0 || p[0] > width || p[1] < 0 || p[1] > height)
            throw ValueError(
                "Laser lead-in/out extends outside the work area; move the object or reduce the lead distance");
    std::vector<double> lengths;
    lengths.reserve(points.size());
    for (size_t i = 0; i + 1 < points.size(); ++i) lengths.push_back(dist(points[i], points[i + 1]));
    const double total = pymath::sum(lengths.begin(), lengths.end());
    if (ramp_in + ramp_out > total + 1e-9)
        throw ValueError("Power ramp distances overlap; reduce them to fit each path including its leads");
    const auto first = lengths.begin() + static_cast<std::ptrdiff_t>(before.size());
    const double offset = pymath::sum(lengths.begin(), first);
    const double perimeter = pymath::sum(first, first + static_cast<std::ptrdiff_t>(contour.size() - 1));
    const std::vector<double> gaps = gap_boundaries(perimeter, offset, contour, cut);
    // sorted(set(gaps + [ramp_in, total - ramp_out])): one of each value, the first met, in rising order.
    std::vector<double> stops = gaps;
    stops.push_back(ramp_in);
    stops.push_back(total - ramp_out);
    std::stable_sort(stops.begin(), stops.end());
    stops.erase(std::unique(stops.begin(), stops.end()), stops.end());

    const auto level = [&](double distance) -> LaserVertex {
        if ((std::upper_bound(gaps.begin(), gaps.end(), distance) - gaps.begin()) % 2) return {0, 0, 0, true};
        if (ramp_in != 0 && distance < ramp_in)
            return {0, 0, power * (start + (100 - start) * distance / ramp_in) / 100, false};
        if (ramp_out != 0 && distance > total - ramp_out)
            return {0, 0, power * (end + (100 - end) * (total - distance) / ramp_out) / 100, false};
        return {0, 0, power, false};
    };

    // The S value applies to the full move ending at its associated point.
    std::vector<LaserVertex> output;
    LaserVertex vertex = level(0);
    vertex.x = points[0][0];
    vertex.y = points[0][1];
    output.push_back(vertex);
    double travelled = 0;
    std::vector<double> cuts;
    for (size_t k = 0; k + 1 < points.size(); ++k) {
        const Point &a = points[k], &b = points[k + 1];
        const double length = lengths[k];
        if (length == 0) continue;
        const double stop = travelled + length;
        cuts.clear();
        cuts.push_back(travelled);
        cuts.insert(cuts.end(), std::upper_bound(stops.begin(), stops.end(), travelled),
                    std::lower_bound(stops.begin(), stops.end(), stop));
        cuts.push_back(stop);
        for (size_t c = 0; c + 1 < cuts.size(); ++c) {
            const double lo = cuts[c], hi = cuts[c + 1];
            const bool changing = (ramp_in != 0 && lo < ramp_in) || (ramp_out != 0 && hi > total - ramp_out);
            const double count = changing ? py_max(1, std::ceil((hi - lo) / 0.5)) : 1;
            if (static_cast<double>(output.size()) + count > 200000)
                throw ValueError("Laser path generates too many segments");
            for (double i = 0; i < count; ++i) {
                const double s0 = lo + (hi - lo) * i / count;
                const double s1 = lo + (hi - lo) * (i + 1) / count;
                const double t = (s1 - travelled) / length;
                vertex = level((s0 + s1) / 2);
                vertex.x = a[0] + (b[0] - a[0]) * t;
                vertex.y = a[1] + (b[1] - a[1]) * t;
                output.push_back(vertex);
            }
        }
        travelled = stop;
    }
    return output;
}

}  // namespace detail
}  // namespace kerfkite
