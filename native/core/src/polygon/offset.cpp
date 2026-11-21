// Round offsets and round shapes.
//
// Growing a region by d is the union of the region with a piece for each edge (the edge swept d to its
// outer side) and a piece for each corner that turns away from the outside (a sector of radius d). Here
// every piece is a fan of triangles on the grid round a vertex of the region, each of them checked, with
// exact arithmetic, to be counter-clockwise or flat. The boundaries of the region and of all the pieces
// add up to one closed curve per ring, offset_curve below, and because no piece winds negatively the
// winding number of that curve at a point is the number of pieces over it, plus one inside the region. So
// the grown region is exactly where the winding number is positive, whatever the curve does to itself:
// no loop of the curve has to be recognised and removed, and nothing can turn inside out.
//
// Shrinking is growing the outside: the same curve for each ring reversed, and the result is where the
// region's winding less the pieces' is still positive.
//
// Round corners are drawn outside the true arc (ArcOptions in polygon.hpp): each is a run of tangents to
// the circle, turning by one step at a time, and the corners of the run are where consecutive tangents
// meet. A corner that turns by less than a step is therefore a single mitred point, so offsetting a
// result again does not add vertices to its arcs. Directions come from rotating by a fixed step, from
// halving an angle with square roots and from the series for the step's sine and cosine: no library
// trigonometry, so the vertices are the same on every platform.
#include <cmath>

#include "kerfkite/cam.hpp"
#include "kernel.hpp"

namespace kerfkite {
namespace polygon {

namespace detail {

namespace {

constexpr double kPi = 3.141592653589793;

struct Steps {
    int per_quarter;
    double cosine, sine;  // of one step
    double cos_one;       // of 1.0625 steps: a wider turn is divided
    double cos_two;       // of 2.125 steps
    bool two;             // 2.125 steps is less than half a turn
};

Steps steps_for(double radius, const ArcOptions &arcs) {
    Steps steps{};
    double count = arcs.segments_per_quarter;
    if (arcs.tolerance > 0) {
        // Half the widest turn must have a secant within tolerance / radius of one; cos x >= 1 - x^2 / 2.
        const double tolerance = arcs.tolerance * kUnitsPerMm;
        count = std::ceil(1.0625 * (kPi / 4) / std::sqrt(2 * tolerance / (radius + tolerance)));
        count = count < 1 ? 1 : count > 1024 ? 1024 : count;
    }
    if (!(arcs.tolerance >= 0) || !(count >= 1 && count <= 1024)) reject("Arc segments must be 1 to 1024 a quarter turn and the tolerance not negative");
    steps.per_quarter = static_cast<int>(count);
    const double step = (kPi / 2) / steps.per_quarter;
    double unused;
    series_cos_sin(step, steps.cosine, steps.sine);
    series_cos_sin(1.0625 * step, steps.cos_one, unused);
    steps.two = 2.125 * step < kPi;
    if (steps.two) series_cos_sin(2.125 * step, steps.cos_two, unused);
    return steps;
}

Coord nearest(double value) { return static_cast<Coord>(std::floor(value + 0.5)); }

// The corners of a round join at v, from the tangent with outward normal (ax, ay) counter-clockwise to
// the tangent with normal (bx, by), at most half a turn on.
void round_join(const Point &v, double ax, double ay, double bx, double by, double distance, const Steps &steps, std::vector<Point> &out) {
    // Where the tangents with normals p and q meet: distance * (p + q) / (1 + p.q).
    const auto corner = [&](double px, double py, double qx, double qy) {
        const double k = distance / std::max(0.5, 1 + (px * qx + py * qy));
        out.push_back({v.x + nearest(k * (px + qx)), v.y + nearest(k * (py + qy))});
    };
    double tx = ax, ty = ay;
    for (int guard = 4 * steps.per_quarter + 8; guard > 0; --guard) {
        const double c = std::max(-1.0, std::min(1.0, tx * bx + ty * by));  // cosine of what remains
        if (steps.two && c < steps.cos_two) {
            const double nx = tx * steps.cosine - ty * steps.sine, ny = ty * steps.cosine + tx * steps.sine;
            corner(tx, ty, nx, ny);
            tx = nx, ty = ny;
            continue;
        }
        if (c < steps.cos_one) {  // between one and two steps: two equal parts, by the half-angle formulas
            const double ch = std::sqrt((1 + c) / 2), sh = std::sqrt((1 - c) / 2);
            const double mx = tx * ch - ty * sh, my = ty * ch + tx * sh;
            corner(tx, ty, mx, my);
            tx = mx, ty = my;
        }
        break;
    }
    corner(tx, ty, bx, by);
}

Ring without_repeats(const Ring &ring) {
    Ring out;
    out.reserve(ring.size());
    for (const Point &p : ring)
        if (out.empty() || out.back() != p) out.push_back(p);
    while (out.size() > 1 && out.back() == out.front()) out.pop_back();
    return out;
}

// The offsets of a quarter disc's corners from its centre, counter-clockwise from the x axis.
std::vector<Point> quarter(double radius, const ArcOptions &arcs) {
    Steps steps = steps_for(radius, arcs);
    for (int count = steps.per_quarter;; count /= 2) {
        // Corners at least four units apart, or rounding would put them out of order.
        if (count > 1 && radius * (kPi / 2) / count < 4) continue;
        ArcOptions fixed;
        fixed.segments_per_quarter = count < 1 ? 1 : count;
        steps = steps_for(radius, fixed);
        std::vector<Point> corners;
        double tx = 1, ty = 0;
        for (int i = 0; i < steps.per_quarter; ++i) {
            double nx = tx * steps.cosine - ty * steps.sine, ny = ty * steps.cosine + tx * steps.sine;
            if (i + 1 == steps.per_quarter) nx = 0, ny = 1;
            const double k = radius / (1 + (tx * nx + ty * ny));
            corners.push_back({nearest(k * (tx + nx)), nearest(k * (ty + ny))});
            tx = nx, ty = ny;
        }
        // Each corner strictly counter-clockwise of the one before, as seen from the centre, through the
        // quarter and on to the first corner of the next quarter.
        bool ordered = true;
        for (std::size_t i = 0; ordered && i < corners.size(); ++i) {
            const Point &p = corners[i];
            const Point q = i + 1 < corners.size() ? corners[i + 1] : Point{-corners[0].y, corners[0].x};
            ordered = cross(p.x, p.y, q.x, q.y) > Int128(0);
        }
        if (ordered || count <= 1) return corners;
    }
}

double size_in_units(double millimetres, const char *message) {
    const double units = millimetres * kUnitsPerMm;
    if (!(units >= 2 && units <= static_cast<double>(kMaxDistance))) reject(message);
    return units;
}

}  // namespace

void series_cos_sin(double angle, double &cosine, double &sine) {
    const double square = angle * angle;
    double c = 1, s = 1;
    for (int k = 14; k >= 1; --k) {
        c = 1 - square / static_cast<double>((2 * k - 1) * (2 * k)) * c;
        s = 1 - square / static_cast<double>((2 * k) * (2 * k + 1)) * s;
    }
    cosine = c;
    sine = angle * s;
}

void offset_curve(const Ring &ring, double distance, const ArcOptions &arcs, Ring &out) {
    const Steps steps = steps_for(distance, arcs);
    const std::size_t n = ring.size();
    struct Side {
        Coord ex, ey;   // the edge from this vertex to the next
        double nx, ny;  // its unit normal to the right
        Point shift;    // the normal times the distance, on the grid
        Point start, end;  // the edge as offset
    };
    std::vector<Side> sides(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Point &p = ring[i], &q = ring[i + 1 == n ? 0 : i + 1];
        Side &side = sides[i];
        side.ex = q.x - p.x, side.ey = q.y - p.y;
        const double ex = static_cast<double>(side.ex), ey = static_cast<double>(side.ey);
        const double length = std::sqrt(ex * ex + ey * ey);
        side.nx = ey / length, side.ny = -ex / length;
        side.shift = {nearest(distance * side.nx), nearest(distance * side.ny)};
        // The swept edge must lie to the right of the edge; rounding a very small distance can lose that.
        if (!(cross(side.shift.x, side.shift.y, side.ex, side.ey) > Int128(0))) side.shift = {0, 0};
    }
    // Round joins where the curve turns to the left, away from the offset side, or doubles back.
    std::vector<Point> joins;
    std::vector<std::size_t> first(n + 1);
    for (std::size_t i = 0; i < n; ++i) {
        const Side &before = sides[i == 0 ? n - 1 : i - 1], &after = sides[i];
        first[i] = joins.size();
        const Int128 turn = cross(before.ex, before.ey, after.ex, after.ey);
        if (turn > Int128(0) || (turn == Int128(0) && dot(before.ex, before.ey, after.ex, after.ey) < Int128(0)))
            round_join(ring[i], before.nx, before.ny, after.nx, after.ny, distance, steps, joins);
    }
    first[n] = joins.size();
    // Each offset edge runs from the last corner of the join before it to the first corner of the join
    // after it. If that does not make two counter-clockwise triangles with the edge, it is the edge
    // shifted bodily, which always does.
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t next = i + 1 == n ? 0 : i + 1;
        Side &side = sides[i];
        const Point &p = ring[i], &q = ring[next];
        const Point shifted_p{p.x + side.shift.x, p.y + side.shift.y}, shifted_q{q.x + side.shift.x, q.y + side.shift.y};
        side.start = first[i + 1] > first[i] ? joins[first[i + 1] - 1] : shifted_p;
        side.end = first[next + 1] > first[next] ? joins[first[next]] : shifted_q;
        if (cross(p, side.start, side.end) < Int128(0) || cross(p, side.end, q) < Int128(0)) side.start = shifted_p, side.end = shifted_q;
    }
    // Round each vertex: from the end of the edge before, through the join, to the start of the edge
    // after. Two consecutive points are joined directly when they make a counter-clockwise or flat
    // triangle with the vertex, which is then one of the pieces; otherwise the curve goes by way of the
    // vertex, which adds no piece and takes none away.
    out.clear();
    const auto put = [&](const Point &p) {
        if (out.empty() || out.back() != p) out.push_back(p);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const Point &pivot = ring[i];
        Point last = sides[i == 0 ? n - 1 : i - 1].end;
        put(last);
        const auto go = [&](const Point &to) {
            if (to == last) return;
            if (cross(pivot, last, to) < Int128(0)) put(pivot);
            put(to);
            last = to;
        };
        for (std::size_t k = first[i]; k < first[i + 1]; ++k) go(joins[k]);
        go(sides[i].start);
    }
    while (out.size() > 1 && out.back() == out.front()) out.pop_back();
}

Ring stroke_curve(const PathMm &points, double width, const ArcOptions &arcs) {
    const double radius = size_in_units(width, "A stroke width is outside the range of the polygon kernel") / 2;
    Ring path;
    for (const PointMm &point : points) {
        const Point p = to_grid(point);
        if (path.empty() || path.back() != p) path.push_back(p);
    }
    if (path.empty()) return path;
    if (path.size() == 1) return disc(to_mm(path[0].x), to_mm(path[0].y), width, arcs);
    // There and back. Each end is a turn straight back, which the join rounds into a half disc.
    Ring loop = path;
    for (std::size_t i = path.size() - 1; i-- > 1;) loop.push_back(path[i]);
    Ring curve;
    offset_curve(loop, radius, arcs, curve);
    return curve;
}

}  // namespace detail

using namespace detail;

PolygonSet offset(const PolygonSet &region, double distance, const ArcOptions &arcs) {
    const double units = distance * kUnitsPerMm;
    if (!(std::fabs(units) <= static_cast<double>(kMaxDistance))) reject("An offset distance is not finite or is beyond the range of the polygon kernel");
    if (std::fabs(units) < 1) return region;
    Arrangement arrangement;
    Ring curve;
    const auto grow = [&](const Ring &given, bool outer) {
        Ring ring = without_repeats(given);
        if (ring.size() < 3 || orientation(ring) != (outer ? 1 : -1)) reject("An offset needs a region in the polygon kernel's own form");
        for (const Point &p : ring) check_range(p);
        if (units > 0) {
            offset_curve(ring, units, arcs, curve);
        } else {
            std::reverse(ring.begin(), ring.end());
            offset_curve(ring, -units, arcs, curve);
            std::reverse(curve.begin(), curve.end());
        }
        arrangement.add(curve, 0);
    };
    for (const Polygon &polygon : region) {
        grow(polygon.shell, true);
        for (const Ring &hole : polygon.holes) grow(hole, false);
    }
    arrangement.resolve();
    arrangement.wind();
    return arrangement.region([](std::int32_t w, std::int32_t) { return w > 0; });
}

Ring disc(double centre_x, double centre_y, double diameter, const ArcOptions &arcs) {
    const Point centre = to_grid(PointMm{centre_x, centre_y});
    const double radius = size_in_units(diameter, "A disc diameter is outside the range of the polygon kernel") / 2;
    const std::vector<Point> corners = quarter(radius, arcs);
    Ring ring;
    ring.reserve(4 * corners.size());
    for (int turn = 0; turn < 4; ++turn)
        for (const Point &c : corners) {
            const Point offset = turn == 0 ? c : turn == 1 ? Point{-c.y, c.x} : turn == 2 ? Point{-c.x, -c.y} : Point{c.y, -c.x};
            ring.push_back({centre.x + offset.x, centre.y + offset.y});
        }
    return ring;
}

Ring rectangle(double centre_x, double centre_y, double width, double height) {
    const Point centre = to_grid(PointMm{centre_x, centre_y});
    const Coord half_x = nearest(size_in_units(width, "A rectangle is outside the range of the polygon kernel") / 2);
    const Coord half_y = nearest(size_in_units(height, "A rectangle is outside the range of the polygon kernel") / 2);
    return {{centre.x - half_x, centre.y - half_y}, {centre.x + half_x, centre.y - half_y}, {centre.x + half_x, centre.y + half_y}, {centre.x - half_x, centre.y + half_y}};
}

Ring obround(double centre_x, double centre_y, double width, double height, const ArcOptions &arcs) {
    const Point centre = to_grid(PointMm{centre_x, centre_y});
    const Coord half_x = nearest(size_in_units(width, "An obround is outside the range of the polygon kernel") / 2);
    const Coord half_y = nearest(size_in_units(height, "An obround is outside the range of the polygon kernel") / 2);
    const Coord radius = std::min(half_x, half_y), along_x = half_x - radius, along_y = half_y - radius;
    // A disc of the shorter side, pulled apart along the longer one.
    Ring ring = disc(0, 0, 2 * to_mm(radius), arcs);
    for (Point &p : ring) {
        p.x += centre.x + (p.x > 0 ? along_x : -along_x);
        p.y += centre.y + (p.y > 0 ? along_y : -along_y);
    }
    return ring;
}

PolygonSet stroke(const PathMm &points, double width, const ArcOptions &arcs) {
    Arrangement arrangement;
    arrangement.add(stroke_curve(points, width, arcs), 0);
    arrangement.resolve();
    arrangement.wind();
    return arrangement.region([](std::int32_t w, std::int32_t) { return w > 0; });
}

}  // namespace polygon
}  // namespace kerfkite
