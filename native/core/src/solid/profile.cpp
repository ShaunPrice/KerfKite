// 2D regions and the solids made from them: extrusion, revolution, and the projection of a solid.
//
// A region is what a fill rule makes of a set of directed segments on the grid. The segments are
// arranged exactly (planar.hpp), each triangle of the arrangement is given its winding number, and the
// filled triangles are the region, already triangulated for the caps of an extrusion.
#include <algorithm>
#include <cmath>

#include "mesh.hpp"
#include "planar.hpp"

namespace kerfkite {
namespace solid {

namespace detail {

namespace {

struct Segment2 {
    int64_t ax, ay, bx, by;
};

struct Region {
    Pool pool;
    std::vector<std::array<int, 3>> triangles;      // filled, counter-clockwise, points of the pool
    std::vector<std::pair<int, int>> boundary;      // directed edges with the region on their left
};

bool filled(int winding, FillRule rule) {
    switch (rule) {
        case FillRule::EvenOdd: return (winding & 1) != 0;
        case FillRule::NonZero: return winding != 0;
        default: return winding > 0;
    }
}

void resolve_region(const std::vector<Segment2> &segments, FillRule rule, Region &out) {
    Plane carrier;
    carrier.n[2] = I128(1);
    Planar planar(out.pool, carrier, 0, 1, 1);
    for (const Segment2 &s : segments) {
        if (s.ax == s.bx && s.ay == s.by) continue;
        const int a = out.pool.add_grid(s.ax, s.ay, 0), b = out.pool.add_grid(s.bx, s.by, 0);
        planar.supports.push_back(plane_through_edge(out.pool.pts[static_cast<size_t>(a)].i, out.pool.pts[static_cast<size_t>(b)].i, 2));
        planar.segments.push_back(PlanarSegment{a, b, static_cast<int>(planar.supports.size()) - 1, 1});
    }
    if (planar.segments.empty()) return;
    planar.split();
    planar.triangulate_around();
    // How often each edge is crossed: +1 for every piece running from the lower id to the higher.
    struct Crossing {
        int a, b, net;
    };
    std::vector<Crossing> net;
    net.reserve(planar.pieces.size());
    for (const PlanarSegment &p : planar.pieces) net.push_back(p.a < p.b ? Crossing{p.a, p.b, 1} : Crossing{p.b, p.a, -1});
    std::sort(net.begin(), net.end(), [](const Crossing &x, const Crossing &y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
    size_t unique = 0;
    for (size_t i = 0; i < net.size();) {
        Crossing sum = net[i];
        size_t j = i + 1;
        while (j < net.size() && net[j].a == sum.a && net[j].b == sum.b) sum.net += net[j++].net;
        net[unique++] = sum;
        i = j;
    }
    net.resize(unique);
    const auto crossing = [&](int from, int to) {  // the net count of pieces running from -> to
        const int a = std::min(from, to), b = std::max(from, to);
        size_t lo = 0, hi = net.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (net[mid].a < a || (net[mid].a == a && net[mid].b < b)) lo = mid + 1;
            else hi = mid;
        }
        if (lo == net.size() || net[lo].a != a || net[lo].b != b) return 0;
        return from < to ? net[lo].net : -net[lo].net;
    };
    // Winding numbers, from the triangles that touch the surrounding triangle (winding 0).
    const size_t count = planar.triangles.size();
    std::vector<int> winding(count, 0);
    std::vector<char> seen(count, 0);
    std::vector<int> queue;
    for (size_t t = 0; t < count; ++t) {
        const std::array<int, 3> &tri = planar.triangles[t];
        bool outer = false;
        for (int k = 0; k < 3; ++k) outer = outer || tri[static_cast<size_t>(k)] == planar.root[0] || tri[static_cast<size_t>(k)] == planar.root[1] || tri[static_cast<size_t>(k)] == planar.root[2];
        if (outer) {
            seen[t] = 1;
            queue.push_back(static_cast<int>(t));
        }
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        const size_t t = static_cast<size_t>(queue[head]);
        for (size_t k = 0; k < 3; ++k) {
            const int next = planar.neighbours[t][k];
            if (next < 0 || seen[static_cast<size_t>(next)]) continue;
            // This triangle is on the left of its edge v[k] -> v[k+1]; the neighbour is on the right.
            winding[static_cast<size_t>(next)] = winding[t] - crossing(planar.triangles[t][k], planar.triangles[t][(k + 1) % 3]);
            seen[static_cast<size_t>(next)] = 1;
            queue.push_back(next);
        }
    }
    for (size_t t = 0; t < count; ++t) {
        if (!filled(winding[t], rule)) continue;
        out.triangles.push_back(planar.triangles[t]);
        for (size_t k = 0; k < 3; ++k) {
            const int next = planar.neighbours[t][k];
            if (next < 0 || !filled(winding[static_cast<size_t>(next)], rule)) out.boundary.emplace_back(planar.triangles[t][k], planar.triangles[t][(k + 1) % 3]);
        }
    }
}

std::vector<Segment2> ring_segments(const std::vector<Ring> &rings, const char *context) {
    std::vector<Segment2> segments;
    size_t total = 0;
    for (const Ring &ring : rings) total += ring.size();
    if (total > kMaxProfileVertices) throw ValueError(std::string(context) + ": the rings have more than " + std::to_string(kMaxProfileVertices) + " points");
    for (const Ring &ring : rings) {
        std::vector<std::array<int64_t, 2>> points;
        for (const Vec2 &p : ring) {
            const std::array<int64_t, 2> q = {to_grid(p.x), to_grid(p.y)};
            if (points.empty() || points.back() != q) points.push_back(q);
        }
        while (points.size() > 1 && points.front() == points.back()) points.pop_back();
        if (points.size() < 3) throw ValueError(std::string(context) + ": each ring needs at least three distinct points");
        for (size_t i = 0; i < points.size(); ++i) {
            const std::array<int64_t, 2> &a = points[i], &b = points[(i + 1) % points.size()];
            segments.push_back(Segment2{a[0], a[1], b[0], b[1]});
        }
    }
    return segments;
}

// The closed loops of a set of directed edges in which as many edges enter each point as leave it.
std::vector<std::vector<int>> loops_of(std::vector<std::pair<int, int>> edges) {
    std::sort(edges.begin(), edges.end());
    std::vector<char> used(edges.size(), 0);
    std::vector<std::vector<int>> loops;
    for (size_t start = 0; start < edges.size(); ++start) {
        if (used[start]) continue;
        std::vector<int> loop;
        size_t current = start;
        for (;;) {
            used[current] = 1;
            loop.push_back(edges[current].first);
            const int next = edges[current].second;
            if (next == edges[start].first) break;
            // The first unused edge that leaves the point we have reached.
            size_t lo = static_cast<size_t>(std::lower_bound(edges.begin(), edges.end(), std::make_pair(next, -1)) - edges.begin());
            while (lo < edges.size() && edges[lo].first == next && used[lo]) ++lo;
            if (lo == edges.size() || edges[lo].first != next) break;  // cannot happen for a closed boundary
            current = lo;
        }
        loops.push_back(loop);
    }
    return loops;
}

Vec2 point_mm(const Pool &pool, int p) {
    const Pt &q = pool.pts[static_cast<size_t>(p)];
    return Vec2{(q.c[0].lo + q.c[0].hi) / 2 / kUnitsPerMm, (q.c[1].lo + q.c[1].hi) / 2 / kUnitsPerMm};
}

std::vector<Ring> region_rings(const Region &region) {
    std::vector<Ring> rings;
    for (const std::vector<int> &loop : loops_of(region.boundary)) {
        Ring ring;
        for (const int p : loop) ring.push_back(point_mm(region.pool, p));
        if (ring.size() >= 3) rings.push_back(ring);
    }
    return rings;
}

// A solid from triangles that may share positions: welded and paired, or, where that leaves triangles
// crossing (a profile whose rounded outline folds over itself), resolved by the arrangement.
Solid solid_from_soup(std::vector<GridPoint> v, std::vector<Triangle> t, const char *context) {
    if (t.size() > kMaxTriangles) throw ValueError(std::string(context) + ": more than " + std::to_string(kMaxTriangles) + " triangles");
    std::vector<GridPoint> welded = v;
    std::vector<Triangle> paired = t;
    try {
        weld_and_pair(welded, paired, context);
        if (paired.empty()) throw ValueError(std::string(context) + ": the solid has no volume");
        return make_solid(std::move(welded), std::move(paired), false, context);
    } catch (const ValueError &) {
        Solid repaired;
        try {
            repaired = resolve_self(std::move(v), std::move(t), context);
        } catch (const ValueError &) {
            throw ValueError(std::string(context) + ": the profile does not make a closed solid on the 1 nm grid");
        }
        if (repaired.empty()) throw ValueError(std::string(context) + ": the solid has no volume");
        return repaired;
    }
}

}  // namespace
}  // namespace detail

double ring_area(const Ring &ring) {
    double twice = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Vec2 &a = ring[i], &b = ring[(i + 1) % ring.size()];
        twice += a.x * b.y - a.y * b.x;
    }
    return twice / 2;
}

std::vector<Ring> resolve_rings(const std::vector<Ring> &rings, FillRule fill) {
    detail::Region region;
    detail::resolve_region(detail::ring_segments(rings, "rings"), fill, region);
    return detail::region_rings(region);
}

Solid extrude(const std::vector<Ring> &rings, double height, const ExtrudeOptions &options) {
    const double h = height * kUnitsPerMm;
    if (!(h >= 0.5) || !(h <= static_cast<double>(detail::kMaxCoord))) throw ValueError("extrude: the height must be positive and at most 100000 mm");
    if (options.slices < 1 || options.slices > 4096) throw ValueError("extrude: 1 to 4096 slices");
    if (!(std::fabs(options.twist_degrees) <= 36000) || !(options.scale_x >= 0) || !(options.scale_y >= 0) || !(options.scale_x <= 1e6) || !(options.scale_y <= 1e6))
        throw ValueError("extrude: the twist must be finite and the scale factors zero or more");
    detail::Region region;
    detail::resolve_region(detail::ring_segments(rings, "extrude"), options.fill, region);
    if (region.triangles.empty()) throw ValueError("extrude: the rings enclose no area");
    // The points of the region, rounded to the grid.
    std::vector<int> ids;
    for (const std::array<int, 3> &t : region.triangles) ids.insert(ids.end(), t.begin(), t.end());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    const auto index = [&](int id) { return static_cast<uint32_t>(std::lower_bound(ids.begin(), ids.end(), id) - ids.begin()); };
    const uint32_t n = static_cast<uint32_t>(ids.size());
    const uint32_t layers = static_cast<uint32_t>(options.slices) + 1;
    if ((static_cast<size_t>(layers) - 1) * region.boundary.size() * 2 + region.triangles.size() * 2 > kMaxTriangles)
        throw ValueError("extrude: more than " + std::to_string(kMaxTriangles) + " triangles");
    std::vector<GridPoint> v(static_cast<size_t>(n) * layers);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        const double along = static_cast<double>(layer) / static_cast<double>(layers - 1);
        const int64_t z = static_cast<int64_t>(std::round(h * along));
        double s = 0, c = 1;
        if (layer) sincos_degrees(options.twist_degrees * along, s, c);
        const double sx = 1 + (options.scale_x - 1) * along, sy = 1 + (options.scale_y - 1) * along;
        for (uint32_t i = 0; i < n; ++i) {
            int64_t p[3];
            region.pool.round(ids[i], p);
            if (layer == 0) {
                v[i] = GridPoint{{p[0], p[1], 0}};
                continue;
            }
            const double x = static_cast<double>(p[0]) * sx, y = static_cast<double>(p[1]) * sy;
            const double rx = c * x - s * y, ry = s * x + c * y;
            if (!(std::fabs(rx) <= static_cast<double>(detail::kMaxCoord)) || !(std::fabs(ry) <= static_cast<double>(detail::kMaxCoord)))
                throw ValueError("extrude: the result is outside \xC2\xB1" "100000 mm");
            v[static_cast<size_t>(layer) * n + i] = GridPoint{{static_cast<int64_t>(std::round(rx)), static_cast<int64_t>(std::round(ry)), z}};
        }
    }
    std::vector<Triangle> t;
    const uint32_t top = (layers - 1) * n;
    const bool point_top = options.scale_x == 0 || options.scale_y == 0;  // a top without area has no cap
    for (const std::array<int, 3> &f : region.triangles) {
        const uint32_t a = index(f[0]), b = index(f[1]), c = index(f[2]);
        t.push_back(Triangle{{a, c, b}});
        if (!point_top) t.push_back(Triangle{{top + a, top + b, top + c}});
    }
    for (const std::pair<int, int> &e : region.boundary) {
        const uint32_t a = index(e.first), b = index(e.second);
        for (uint32_t layer = 0; layer + 1 < layers; ++layer) {
            const uint32_t lo = layer * n, hi = lo + n;
            t.push_back(Triangle{{lo + a, lo + b, hi + b}});
            t.push_back(Triangle{{lo + a, hi + b, hi + a}});
        }
    }
    return detail::solid_from_soup(std::move(v), std::move(t), "extrude");
}

Solid revolve(const std::vector<Ring> &rings, int segments, double degrees, FillRule fill) {
    if (segments < 3 || segments > 65536) throw ValueError("revolve: 3 to 65536 segments");
    if (!(degrees > 0) || !(degrees <= 360)) throw ValueError("revolve: the angle must be above 0 and at most 360 degrees");
    for (const Ring &ring : rings)
        for (const Vec2 &p : ring)
            if (!(p.x >= 0)) throw ValueError("revolve: the profile must not cross the axis (negative radius)");
    detail::Region region;
    detail::resolve_region(detail::ring_segments(rings, "revolve"), fill, region);
    if (region.triangles.empty()) throw ValueError("revolve: the rings enclose no area");
    std::vector<int> ids;
    for (const std::array<int, 3> &t : region.triangles) ids.insert(ids.end(), t.begin(), t.end());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    const auto index = [&](int id) { return static_cast<uint32_t>(std::lower_bound(ids.begin(), ids.end(), id) - ids.begin()); };
    const uint32_t n = static_cast<uint32_t>(ids.size());
    const bool full = degrees == 360;
    const uint32_t steps = full ? static_cast<uint32_t>(segments) : static_cast<uint32_t>(std::max(1.0, std::ceil(segments * degrees / 360.0)));
    const uint32_t stations = full ? steps : steps + 1;
    if (static_cast<size_t>(steps) * region.boundary.size() * 2 + region.triangles.size() * 2 > kMaxTriangles)
        throw ValueError("revolve: more than " + std::to_string(kMaxTriangles) + " triangles");
    std::vector<GridPoint> v(static_cast<size_t>(n) * stations);
    for (uint32_t k = 0; k < stations; ++k) {
        double s, c;
        if (full) sincos_turn(k, steps, s, c);
        else sincos_degrees(degrees * static_cast<double>(k) / static_cast<double>(steps), s, c);
        for (uint32_t i = 0; i < n; ++i) {
            int64_t p[3];
            region.pool.round(ids[i], p);
            const double r = static_cast<double>(p[0] < 0 ? 0 : p[0]);
            v[static_cast<size_t>(k) * n + i] = GridPoint{{static_cast<int64_t>(std::round(r * c)), static_cast<int64_t>(std::round(r * s)), p[1]}};
        }
    }
    std::vector<Triangle> t;
    for (const std::pair<int, int> &e : region.boundary) {
        const uint32_t a = index(e.first), b = index(e.second);
        for (uint32_t k = 0; k < steps; ++k) {
            const uint32_t lo = k * n, hi = ((k + 1) % stations) * n;
            t.push_back(Triangle{{lo + a, hi + b, lo + b}});
            t.push_back(Triangle{{lo + a, hi + a, hi + b}});
        }
    }
    if (!full) {
        const uint32_t end = steps * n;
        for (const std::array<int, 3> &f : region.triangles) {
            const uint32_t a = index(f[0]), b = index(f[1]), c = index(f[2]);
            t.push_back(Triangle{{a, b, c}});
            t.push_back(Triangle{{end + a, end + c, end + b}});
        }
    }
    return detail::solid_from_soup(std::move(v), std::move(t), "revolve");
}

std::vector<Ring> projection(const Solid &solid) {
    const detail::Mesh &m = detail::Access::mesh(solid);
    // The edges where the upward-facing part of the surface ends. Seen from above, each upward-facing
    // patch covers a point as often as its outline winds around it, so the outline filled by the positive
    // rule is the shadow.
    std::vector<uint64_t> up;
    for (const Triangle &tri : m.t) {
        if (detail::orient2d_grid(m.v[tri[0]].data(), m.v[tri[1]].data(), m.v[tri[2]].data(), 0, 1) <= 0) continue;
        for (size_t k = 0; k < 3; ++k) up.push_back((static_cast<uint64_t>(tri[k]) << 32) | tri[(k + 1) % 3]);
    }
    std::sort(up.begin(), up.end());
    std::vector<detail::Segment2> segments;
    for (const uint64_t e : up) {
        if (std::binary_search(up.begin(), up.end(), (e << 32) | (e >> 32))) continue;
        const GridPoint &a = m.v[e >> 32], &b = m.v[e & 0xffffffffu];
        segments.push_back(detail::Segment2{a[0], a[1], b[0], b[1]});
    }
    if (segments.size() > kMaxProfileVertices) throw ValueError("projection: the outline has more than " + std::to_string(kMaxProfileVertices) + " edges");
    detail::Region region;
    detail::resolve_region(segments, FillRule::Positive, region);
    return detail::region_rings(region);
}

}  // namespace solid
}  // namespace kerfkite
