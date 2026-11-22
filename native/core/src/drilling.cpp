// Router drilling at point markers and circular hole outlines: studio/drilling.py, statement for statement
// where order affects the result.
//
// The centre of an outline is fitted through three of its points and then checked against every point, so
// the arithmetic keeps the reference's order of operations. Distances use Python's math.dist and totals its
// sum(); see kerfkite/py_math.hpp.
#include "drilling.hpp"

#include <cmath>
#include <string>

#include "kerfkite/py_math.hpp"
#include "py_trig.hpp"

namespace kerfkite {
namespace detail {

namespace {

using Point = std::array<double, 2>;
using Path = std::vector<Point>;

double dist(const Point &a, const Point &b) { return pymath::dist(a[0], a[1], b[0], b[1]); }

// studio.drilling.hole_center(path, diameter): the marker itself, or the centre of a circular outline.
Point hole_center(const Path &path, double diameter) {
    Path points{path[0]};
    for (size_t i = 1; i < path.size(); ++i)
        if (dist(points.back(), path[i]) > 1e-8) points.push_back(path[i]);
    const Point a = points[0];
    if (points.size() == 1) return a;
    if (points.size() < 9 || dist(a, points.back()) > 1e-6)
        throw ValueError("Drill requires point markers or closed circular hole outlines");
    const Path ring(points.begin(), points.end() - 1);
    // max(ring, key=...) keeps the first point with the largest key.
    size_t far = 0;
    double best = dist(a, ring[0]);
    for (size_t i = 1; i < ring.size(); ++i) {
        const double key = dist(a, ring[i]);
        if (key > best) {
            best = key;
            far = i;
        }
    }
    const Point b = ring[far];
    const double u = b[0] - a[0], v = b[1] - a[1];
    const auto across = [&](const Point &p) { return std::fabs(u * (p[1] - a[1]) - v * (p[0] - a[0])); };
    size_t wide = 0;
    best = across(ring[0]);
    for (size_t i = 1; i < ring.size(); ++i) {
        const double key = across(ring[i]);
        if (key > best) {
            best = key;
            wide = i;
        }
    }
    const Point c = ring[wide];
    const double m = c[0] - a[0], n = c[1] - a[1];
    const double det = 2 * (u * n - v * m);
    if (std::fabs(det) < 1e-12) throw ValueError("Drill outline is degenerate");
    const double ab = u * u + v * v, ac = m * m + n * n;
    const Point center = {a[0] + (n * ab - v * ac) / det, a[1] + (u * ac - m * ab) / det};
    const double radius = diameter / 2;
    const double tolerance = py_max(0.0001, 0.002 * radius);
    for (const Point &p : ring)
        if (std::fabs(dist(p, center) - radius) > tolerance)
            throw ValueError("Drill outline must be circular and match the cutter diameter; use a profile or pocket "
                             "for a larger hole");
    // A full turn with regular coverage distinguishes a bore from a star or partial arc.
    std::vector<double> directions;
    directions.reserve(ring.size());
    for (const Point &p : ring) directions.push_back(pytrig::atan2(p[1] - center[1], p[0] - center[0]));
    std::vector<double> angles, sizes;
    double largest = 0;
    for (size_t i = 0; i < directions.size(); ++i) {
        const double turn = pytrig::remainder(directions[(i + 1) % directions.size()] - directions[i], 2 * pytrig::PI);
        angles.push_back(turn);
        sizes.push_back(std::fabs(turn));
        largest = py_max(largest, std::fabs(turn));
    }
    if (largest > pytrig::PI / 4 + 1e-5 ||
        std::fabs(std::fabs(pymath::sum(angles.begin(), angles.end())) - 2 * pytrig::PI) > 1e-5 ||
        pymath::sum(sizes.begin(), sizes.end()) > 2 * pytrig::PI + 1e-5)
        throw ValueError("Drill outline must trace one complete circle without reversing");
    return center;
}

}  // namespace

Job drill_job(const json &project, const std::vector<Path> &paths) {
    if (!is_text(get(project, "mode"), "router") || !none(get(project, "path_depths")) ||
        !none(get(project, "path_powers")))
        throw ValueError("Drilling requires a router and a single target depth");
    const double diameter = setting(project, "tool_diameter", 3.175, "Drill diameter", 0.01, 100, "0.01", "100");
    const double depth = setting(project, "depth", 1, "Drill depth", 0.01, 50, "0.01", "50");
    const double safe = setting(project, "safe", 5, "Clearance Z", 0.1, 100, "0.1", "100");
    const double plunge = setting(project, "plunge", 100, "Drill plunge feed", 1, 1000, "1", "1000");
    const double power = number(get(project, "power"), "Spindle RPM", 1, 100000, "1", "100000");
    const double peck = setting(project, "drill_peck", 0, "Peck depth", 0, 50, "0", "50");
    if (peck != 0 && peck < 0.01) throw ValueError("Use zero for one plunge or a peck depth of at least 0.01 mm");
    const double angle = setting(project, "drill_point_angle", 118, "Drill point angle", 60, 170, "60", "170");
    const json *shape = get(project, "tool_shape");
    if (shape && !is_text(shape, "drill") && !is_text(shape, "flat"))
        throw ValueError("Drilling requires a drill bit or centre-cutting flat end mill");
    // max(1, ceil(depth / peck - 1e-10)) pecks, or one plunge.
    double count = 1;
    if (peck != 0) count = py_max(1, std::ceil(depth / peck - 1e-10));
    if (static_cast<double>(paths.size()) * (2 * count + 1) + 10 > 200000)
        throw ValueError("Drilling exceeds the line limit; increase the peck depth or reduce hole count");
    Path centers;
    centers.reserve(paths.size());
    for (const Path &path : paths) centers.push_back(hole_center(path, diameter));
    const double radius = diameter / 2;
    const double width = number(get(project, "width"), "Work width", 1, 2000, "1", "2000");
    const double height = number(get(project, "height"), "Work height", 1, 2000, "1", "2000");
    for (const Point &center : centers) {
        const double x = center[0], y = center[1];
        if (x - radius < -1e-8 || y - radius < -1e-8 || x + radius > width + 1e-8 || y + radius > height + 1e-8)
            throw ValueError("Drill footprint crosses the work area boundary");
    }

    Job job;
    job.mode = "router";
    const std::string safe_line = "G0 Z" + fixed(safe, 3);
    const std::string plunge_word = " F" + fixed(plunge, 1);
    std::vector<std::string> lines = {"G21", "G90", "G17", "G94", "G54", "M5", safe_line, "M3 S" + fixed(power, 0),
                                      "G4 P2"};
    const size_t pecks = static_cast<size_t>(count);
    lines.reserve(lines.size() + centers.size() * (2 * pecks + 1) + 1);
    job.segments.reserve(centers.size() * pecks);
    double length = 0;
    for (const Point &center : centers) {
        const double x = center[0], y = center[1];
        lines.push_back("G0 X" + fixed(x, 3) + " Y" + fixed(y, 3));
        for (double i = 0; i < count; ++i) {
            const double z = i + 1 == count ? -depth : -(i + 1) * peck;
            lines.push_back("G1 Z" + fixed(z, 3) + plunge_word);
            lines.push_back(safe_line);
            job.segments.push_back({x, y, x, y, z, false, 0, safe});
            length += safe - z;
        }
    }
    lines.push_back("M5");
    const json *protocol = get(project, "protocol");
    job.lines = postprocess(std::move(lines), protocol, "router", get(project, "dwell_units"));
    job.cut_length = rounded(length, 2);
    job.cut_minutes = rounded(length / plunge, 2);
    job.bounds = {centers[0][0], centers[0][1], centers[0][0], centers[0][1]};
    for (const Point &center : centers)
        job.bounds = {py_min(job.bounds[0], center[0]), py_min(job.bounds[1], center[1]),
                      py_max(job.bounds[2], center[0]), py_max(job.bounds[3], center[1])};
    job.warnings = {get_protocol(protocol).notes,
                    "Depth is measured to the tool tip. Drilling time includes feed plunges from clearance; rapid "
                    "retracts and spindle startup are excluded."};
    job.drilled = true;
    for (const Point &center : centers) job.drill_holes.push_back({center[0], center[1], diameter});
    job.has_simulation_tool = true;
    job.simulation_tool = {diameter, shape ? shape->get<std::string>() : "drill", angle};
    job.prepared = true;
    job.prepared_paths = paths;  // the holes as drawn; a drilling job has no depths
    return job;
}

}  // namespace detail
}  // namespace kerfkite
