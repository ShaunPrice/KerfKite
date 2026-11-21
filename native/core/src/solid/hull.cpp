// Convex hull of grid points, by inserting the points one at a time (the incremental algorithm of the
// computational-geometry textbooks), with exact orientation tests.
//
// A point sees a face when it is on the outer side of the face's plane or in that plane. Counting the
// plane itself as seen means a new triangle is never built on an edge the point is in line with, so no
// triangle without area arises; the points left in the middle of flat faces are then removed by the
// exact simplification the Boolean operations use.
#include <algorithm>

#include "arrangement.hpp"
#include "timing.hpp"

namespace kerfkite {
namespace solid {

namespace {

Solid hull_of(std::vector<GridPoint> points) {
    KERFKITE_SOLID_PHASE(Construct);
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    const size_t n = points.size();
    const auto p = [&](uint32_t i) { return points[i].data(); };
    // A first tetrahedron: two points, a third off their line, a fourth off their plane.
    uint32_t c = 2, d = 0;
    bool found = false;
    for (; c < n && !found; ++c) {
        const detail::Plane pl = detail::plane_of(p(0), p(1), p(c));
        found = !pl.n[0].zero() || !pl.n[1].zero() || !pl.n[2].zero();
        if (found) break;
    }
    bool solid = false;
    if (found)
        for (d = 2; d < n && !solid; ++d) {
            solid = d != c && detail::orient3d_grid(p(0), p(1), p(c), p(d)) != 0;
            if (solid) break;
        }
    if (n < 4 || !found || !solid) throw ValueError("hull: the points lie in one plane");
    struct Face {
        uint32_t v[3];
        bool alive;
    };
    std::vector<Face> faces;
    const auto add = [&](uint32_t a, uint32_t b, uint32_t e) { faces.push_back(Face{{a, b, e}, true}); };
    // Outward faces: the fourth point must be behind each.
    if (detail::orient3d_grid(p(0), p(1), p(c), p(d)) < 0) {
        add(0, 1, c);
        add(1, 0, d);
        add(c, 1, d);
        add(0, c, d);
    } else {
        add(1, 0, c);
        add(0, 1, d);
        add(1, c, d);
        add(c, 0, d);
    }
    std::vector<uint64_t> horizon;
    std::vector<size_t> seen;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == 0 || i == 1 || i == c || i == d) continue;
        seen.clear();
        bool outside = false;
        for (size_t f = 0; f < faces.size(); ++f) {
            if (!faces[f].alive) continue;
            const int side = detail::orient3d_grid(p(faces[f].v[0]), p(faces[f].v[1]), p(faces[f].v[2]), p(i));
            if (side >= 0) seen.push_back(f);
            outside = outside || side > 0;
        }
        if (seen.empty()) continue;  // strictly inside
        // Edges of the seen faces whose other side is not seen: the new faces stand on them.
        horizon.clear();
        for (const size_t f : seen)
            for (int k = 0; k < 3; ++k) horizon.push_back((static_cast<uint64_t>(faces[f].v[k]) << 32) | faces[f].v[(k + 1) % 3]);
        std::sort(horizon.begin(), horizon.end());
        for (const size_t f : seen) faces[f].alive = false;
        for (const uint64_t e : horizon) {
            if (std::binary_search(horizon.begin(), horizon.end(), (e << 32) | (e >> 32))) continue;
            add(static_cast<uint32_t>(e >> 32), static_cast<uint32_t>(e & 0xffffffffu), i);
        }
        (void)outside;
        if (faces.size() > 4 * kMaxTriangles) {
            std::vector<Face> live;
            for (const Face &f : faces)
                if (f.alive) live.push_back(f);
            faces.swap(live);
        }
    }
    // Drop the points flat faces do not need.
    detail::Pool pool;
    detail::ExactMesh mesh;
    std::vector<uint32_t> index(n, UINT32_MAX);
    for (const Face &f : faces) {
        if (!f.alive) continue;
        Triangle t;
        for (size_t k = 0; k < 3; ++k) {
            if (index[f.v[k]] == UINT32_MAX) {
                index[f.v[k]] = static_cast<uint32_t>(mesh.vertex.size());
                mesh.vertex.push_back(pool.add_grid(points[f.v[k]][0], points[f.v[k]][1], points[f.v[k]][2]));
            }
            t[k] = index[f.v[k]];
        }
        mesh.tri.push_back(t);
    }
    detail::simplify(pool, mesh);
    std::vector<GridPoint> v(mesh.vertex.size());
    for (size_t i = 0; i < v.size(); ++i) {
        const detail::Pt &q = pool.pts[static_cast<size_t>(mesh.vertex[i])];
        v[i] = GridPoint{{q.i[0], q.i[1], q.i[2]}};
    }
    return detail::make_solid(std::move(v), std::move(mesh.tri), true, "hull");
}

}  // namespace

Solid convex_hull(const std::vector<Vec3> &points) {
    std::vector<GridPoint> grid(points.size());
    for (size_t i = 0; i < points.size(); ++i) grid[i] = GridPoint{{detail::to_grid(points[i].x), detail::to_grid(points[i].y), detail::to_grid(points[i].z)}};
    return hull_of(std::move(grid));
}

Solid convex_hull(const std::vector<Solid> &solids) {
    std::vector<GridPoint> grid;
    for (const Solid &s : solids) grid.insert(grid.end(), s.grid_vertices().begin(), s.grid_vertices().end());
    // The hull of nothing is nothing: a model whose parts all cancelled is
    // not an error.  Points without a volume, given directly, still are.
    if (grid.empty()) return Solid();
    return hull_of(std::move(grid));
}

}  // namespace solid
}  // namespace kerfkite
