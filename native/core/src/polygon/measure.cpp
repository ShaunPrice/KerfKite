// Conversions to and from millimetres, measures and exact predicates of the polygon kernel.
#include <cmath>

#include "kerfkite/cam.hpp"
#include "kernel.hpp"

namespace kerfkite {
namespace polygon {

namespace detail {

void reject(const char *message) { throw ValueError(message); }

void check_range(const Point &point) {
    if (point.x > kHardLimit || point.x < -kHardLimit || point.y > kHardLimit || point.y < -kHardLimit)
        reject("A coordinate is beyond the range of the polygon kernel");
}

Int128 twice_area(const Ring &ring) {
    Int128 sum = 0;
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Point &p = ring[i], &q = ring[i + 1 == n ? 0 : i + 1];
        sum = sum + cross(p.x, p.y, q.x, q.y);
    }
    return sum;
}

bool segments_meet(const Point &a, const Point &b, const Point &c, const Point &d) {
    if (std::max(a.x, b.x) < std::min(c.x, d.x) || std::max(c.x, d.x) < std::min(a.x, b.x)) return false;
    if (std::max(a.y, b.y) < std::min(c.y, d.y) || std::max(c.y, d.y) < std::min(a.y, b.y)) return false;
    const int c_side = orient(a, b, c), d_side = orient(a, b, d);
    if (c_side == d_side && c_side != 0) return false;
    const int a_side = orient(c, d, a), b_side = orient(c, d, b);
    if (a_side == b_side && a_side != 0) return false;
    // Opposite sides both ways, or a point on the other's line within the boxes, which overlap.
    return true;
}

namespace {

// A ring edge for is_simple and is_valid.
struct Side {
    Point a, b;
    std::uint32_t ring, index, count;  // which ring, which edge of it, and how many edges it has
};

// The ring without consecutive equal vertices.
Ring distinct(const Ring &ring) {
    Ring out;
    out.reserve(ring.size());
    for (const Point &p : ring)
        if (out.empty() || out.back() != p) out.push_back(p);
    while (out.size() > 1 && out.back() == out.front()) out.pop_back();
    return out;
}

void sides_of(const Ring &ring, std::uint32_t id, std::vector<Side> &sides) {
    const std::uint32_t n = static_cast<std::uint32_t>(ring.size());
    for (std::uint32_t i = 0; i < n; ++i) sides.push_back({ring[i], ring[i + 1 == n ? 0 : i + 1], id, i, n});
}

// Calls visit(i, j) for each pair of sides that share a bucket, once, with i < j, until it returns false.
template <class F> bool each_near_pair(const std::vector<Side> &sides, F &&visit) {
    Grid<Side> grid;
    grid.build(sides);
    std::vector<std::uint32_t> stamp(sides.size(), kNone);
    bool go = true;
    for (std::uint32_t i = 0; go && i < sides.size(); ++i)
        grid.near_segment(sides[i].a, sides[i].b, [&](std::uint32_t j) {
            if (!go || j <= i || stamp[j] == i) return;
            stamp[j] = i;
            go = visit(i, j);
        });
    return go;
}

// Whether two sides of one simple ring are allowed to be where they are.
bool sides_agree(const Side &s, const Side &t) {
    const bool follows = t.index == s.index + 1, wraps = s.index == 0 && t.index == s.count - 1;
    if (!follows && !wraps) return !segments_meet(s.a, s.b, t.a, t.b);
    if (s.count == 2) return false;
    // Consecutive sides share a vertex and must not run back along each other.
    const Point &joint = follows ? s.b : s.a, &p = follows ? s.a : s.b, &q = follows ? t.b : t.a;
    if (orient(p, joint, q) != 0) return true;
    return dot(p.x - joint.x, p.y - joint.y, q.x - joint.x, q.y - joint.y) < Int128(0);
}

}  // namespace
}  // namespace detail

using namespace detail;

Coord to_grid(double millimetres) {
    const double units = std::floor(millimetres * kUnitsPerMm + 0.5);
    if (!(std::fabs(units) <= static_cast<double>(kMaxCoordinate))) reject("A coordinate is not finite or is beyond the range of the polygon kernel");
    return static_cast<Coord>(units);
}

Point to_grid(const PointMm &point) { return {to_grid(point[0]), to_grid(point[1])}; }

Ring ring_from_mm(const PathMm &path) {
    Ring ring;
    ring.reserve(path.size());
    for (const PointMm &point : path) {
        const Point p = to_grid(point);
        if (ring.empty() || ring.back() != p) ring.push_back(p);
    }
    while (ring.size() > 1 && ring.back() == ring.front()) ring.pop_back();
    return ring;
}

PathMm ring_to_mm(const Ring &ring, bool closed) {
    PathMm path;
    path.reserve(ring.size() + 1);
    for (const Point &p : ring) path.push_back(to_mm(p));
    if (closed && !ring.empty()) path.push_back(to_mm(ring.front()));
    return path;
}

double signed_area(const Ring &ring) { return to_double(twice_area(ring)) / (2 * kUnitsPerMm * kUnitsPerMm); }

double area(const PolygonSet &region) {
    Int128 sum = 0;
    each_ring(region, [&](const Ring &ring) { sum = sum + twice_area(ring); });
    return to_double(sum) / (2 * kUnitsPerMm * kUnitsPerMm);
}

int orientation(const Ring &ring) { return sign(twice_area(ring)); }

bool bounds(const PolygonSet &region, Bounds &box) {
    bool any = false;
    Coord x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const Polygon &polygon : region)
        for (const Point &p : polygon.shell) {
            if (!any) x0 = x1 = p.x, y0 = y1 = p.y, any = true;
            x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
        }
    if (any) box = {to_mm(x0), to_mm(y0), to_mm(x1), to_mm(y1)};
    return any;
}

std::size_t vertex_count(const PolygonSet &region) {
    std::size_t count = 0;
    each_ring(region, [&](const Ring &ring) { count += ring.size(); });
    return count;
}

bool is_simple(const Ring &given) {
    const Ring ring = distinct(given);
    if (ring.size() < 3) return false;
    for (const Point &p : ring) check_range(p);
    std::vector<Side> sides;
    sides_of(ring, 0, sides);
    return each_near_pair(sides, [&](std::uint32_t i, std::uint32_t j) { return sides_agree(sides[i], sides[j]); });
}

Location locate(const Ring &ring, const Point &point) {
    // Crossings of the ray from the point towards greater x, counting an edge for the half-open range of
    // heights it spans so that a vertex on the ray is counted once.
    bool inside = false;
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Point &a = ring[i], &b = ring[i + 1 == n ? 0 : i + 1];
        if (a.y == b.y) {
            if (point.y == a.y && point.x >= std::min(a.x, b.x) && point.x <= std::max(a.x, b.x)) return Location::Boundary;
            continue;
        }
        if (point.y < std::min(a.y, b.y) || point.y > std::max(a.y, b.y)) continue;
        const Point &low = a.y < b.y ? a : b, &high = a.y < b.y ? b : a;
        const int side = orient(low, high, point);  // positive: the point is to the left of the upward edge
        if (side == 0) return Location::Boundary;
        if (point.y < high.y && side > 0) inside = !inside;
    }
    return inside ? Location::Inside : Location::Outside;
}

Location locate(const PolygonSet &region, const Point &point) {
    for (const Polygon &polygon : region) {
        const Location in_shell = locate(polygon.shell, point);
        if (in_shell == Location::Outside) continue;
        if (in_shell == Location::Boundary) return Location::Boundary;
        bool in_hole = false;
        for (const Ring &hole : polygon.holes) {
            const Location where = locate(hole, point);
            if (where == Location::Boundary) return Location::Boundary;
            if (where == Location::Inside) in_hole = true;
        }
        if (!in_hole) return Location::Inside;
    }
    return Location::Outside;
}

bool is_valid(const PolygonSet &region, std::string *reason) {
    const auto fail = [&](const char *what) {
        if (reason) *reason = what;
        return false;
    };
    std::vector<Side> sides;
    std::uint32_t id = 0;
    for (const Polygon &polygon : region) {
        if (polygon.shell.size() < 3 || orientation(polygon.shell) <= 0) return fail("an outer ring is not counter-clockwise");
        for (const Ring &hole : polygon.holes)
            if (hole.size() < 3 || orientation(hole) >= 0) return fail("a hole is not clockwise");
    }
    bool repeated = false;
    each_ring(region, [&](const Ring &ring) {
        for (std::size_t i = 0; i < ring.size(); ++i) {
            check_range(ring[i]);
            if (ring[i] == ring[i + 1 == ring.size() ? 0 : i + 1]) repeated = true;
        }
        sides_of(ring, id++, sides);
    });
    if (repeated) return fail("a ring repeats a vertex");
    const char *found = nullptr;
    each_near_pair(sides, [&](std::uint32_t i, std::uint32_t j) {
        const Side &s = sides[i], &t = sides[j];
        if (s.ring == t.ring) {
            if (!sides_agree(s, t)) found = "a ring is not simple";
        } else if (segments_meet(s.a, s.b, t.a, t.b)) {
            // Rings may touch at points. They may not cross, and they may not share a length.
            const int ta = orient(s.a, s.b, t.a), tb = orient(s.a, s.b, t.b), sa = orient(t.a, t.b, s.a), sb = orient(t.a, t.b, s.b);
            if (ta * tb < 0 && sa * sb < 0) found = "two rings cross";
            if (ta == 0 && tb == 0) {
                const Point &s0 = std::min(s.a, s.b), &s1 = std::max(s.a, s.b), &t0 = std::min(t.a, t.b), &t1 = std::max(t.a, t.b);
                if (std::max(s0, t0) < std::min(s1, t1)) found = "two rings share a length of edge";
            }
        }
        return found == nullptr;
    });
    if (found) return fail(found);
    // Rings that only touch can still pass through each other at a vertex or lie the wrong way inside one
    // another; the winding number shows both: it must be 0 or 1 everywhere.
    Arrangement arrangement;
    arrangement.add(region, 0);
    arrangement.resolve();
    arrangement.wind();
    for (std::size_t i = 0; i < arrangement.resolved().size(); ++i) {
        const std::int32_t under = arrangement.below()[i][0], over = under + arrangement.resolved()[i].w[0];
        if (under < 0 || under > 1 || over < 0 || over > 1) return fail("rings overlap or a hole lies outside the region");
    }
    // Each hole with its own outer ring: the kernel's own reading of the rings groups them the same way.
    // A polygon is known by the smallest vertex of its outer ring, its area and the number of its holes.
    struct Mark {
        Point least;
        Int128 area;
        std::size_t holes;
    };
    const auto marks = [](const PolygonSet &set) {
        std::vector<Mark> out;
        for (const Polygon &polygon : set) {
            Mark mark{*std::min_element(polygon.shell.begin(), polygon.shell.end()), twice_area(polygon.shell), polygon.holes.size()};
            for (const Ring &hole : polygon.holes) mark.area = mark.area + twice_area(hole);
            out.push_back(mark);
        }
        std::sort(out.begin(), out.end(), [](const Mark &s, const Mark &t) {
            return s.least != t.least ? s.least < t.least : s.area != t.area ? s.area < t.area : s.holes < t.holes;
        });
        return out;
    };
    const std::vector<Mark> given = marks(region), read = marks(arrangement.region([](std::int32_t w, std::int32_t) { return w > 0; }));
    if (given.size() != read.size()) return fail("a hole is outside its own outer ring");
    for (std::size_t i = 0; i < given.size(); ++i)
        if (given[i].least != read[i].least || given[i].area != read[i].area || given[i].holes != read[i].holes) return fail("a hole is outside its own outer ring");
    return true;
}

std::vector<PathMm> outlines(const PolygonSet &region) {
    std::vector<std::size_t> order(region.size());
    std::vector<Int128> areas(region.size());
    for (std::size_t i = 0; i < region.size(); ++i) {
        order[i] = i;
        areas[i] = twice_area(region[i].shell);
        for (const Ring &hole : region[i].holes) areas[i] = areas[i] + twice_area(hole);
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t s, std::size_t t) { return areas[s] < areas[t]; });
    // The same ring the other way round, from the same first vertex.
    const auto reversed = [](const Ring &ring) {
        PathMm path;
        path.reserve(ring.size() + 1);
        if (ring.empty()) return path;
        path.push_back(to_mm(ring.front()));
        for (std::size_t i = ring.size(); i-- > 1;) path.push_back(to_mm(ring[i]));
        path.push_back(to_mm(ring.front()));
        return path;
    };
    std::vector<PathMm> result;
    for (const std::size_t i : order) {
        for (const Ring &hole : region[i].holes) result.push_back(reversed(hole));
        result.push_back(reversed(region[i].shell));
    }
    return result;
}

}  // namespace polygon
}  // namespace kerfkite
