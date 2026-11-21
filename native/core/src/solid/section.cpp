// Plane sections of a solid.
//
// Each vertex is on one side of the plane or the other: a vertex in the plane counts as below it, which
// makes the section the material just above the plane ("lower faces inclusive, upper faces exclusive").
// Every triangle with vertices on both sides contributes one directed segment, and because the mesh is
// closed those segments chain into closed rings through the triangles' shared edges. No geometry is
// compared to join them, so the rings are closed whatever the coordinates.
//
// A ring has a point where it passes from one triangle to the next, except where the two triangles are
// parts of one flat face: the section runs straight on there, and the point would only divide a straight
// side. That test is exact, so a section of a box is four points however its faces are triangulated.
#include <algorithm>
#include <cmath>

#include "mesh.hpp"

namespace kerfkite {
namespace solid {

namespace {

struct Cut {
    uint64_t in, out;  // the crossed edges, each as (vertex below << 32 | vertex above)
    uint32_t triangle;
};

// Whether two triangles that share the edge from p to q lie in one plane, one on each side of the edge.
bool flat_across(const detail::Mesh &m, const Triangle &s, const Triangle &t, uint32_t p, uint32_t q) {
    const auto third = [&](const Triangle &of) { return of[0] != p && of[0] != q ? of[0] : of[1] != p && of[1] != q ? of[1] : of[2]; };
    const int64_t *a = m.v[p].data(), *b = m.v[q].data(), *c = m.v[third(s)].data(), *d = m.v[third(t)].data();
    if (detail::orient3d_grid(a, b, c, d) != 0) return false;
    // Seen along an axis the plane is not edge-on to, the two other corners are on opposite sides of the edge.
    for (int k = 0; k < 3; ++k) {
        const int u = (k + 1) % 3, v = (k + 2) % 3;
        const int side = detail::orient2d_grid(a, b, c, u, v);
        if (side != 0) return side * detail::orient2d_grid(a, b, d, u, v) < 0;
    }
    return false;
}

// `above[v]` for every vertex; `point(below, above)` gives the crossing as the caller wants it.
template <class Point, class Make>
std::vector<std::vector<Point>> chain(const detail::Mesh &m, const std::vector<char> &above, Make &&point) {
    std::vector<Cut> cuts;
    for (size_t index = 0; index < m.t.size(); ++index) {
        const Triangle &t = m.t[index];
        const int count = above[t[0]] + above[t[1]] + above[t[2]];
        if (count == 0 || count == 3) continue;
        Cut cut{0, 0, static_cast<uint32_t>(index)};
        for (size_t k = 0; k < 3; ++k) {
            const uint32_t a = t[k], b = t[(k + 1) % 3];
            // Going round the triangle, the edge that climbs through the plane is where the segment ends
            // and the edge that descends is where it starts: material is then on the left, seen from above.
            if (!above[a] && above[b]) cut.out = (static_cast<uint64_t>(a) << 32) | b;
            if (above[a] && !above[b]) cut.in = (static_cast<uint64_t>(b) << 32) | a;
        }
        cuts.push_back(cut);
    }
    std::sort(cuts.begin(), cuts.end(), [](const Cut &x, const Cut &y) { return x.in < y.in; });
    std::vector<char> used(cuts.size(), 0);
    std::vector<std::vector<Point>> rings;
    for (size_t start = 0; start < cuts.size(); ++start) {
        if (used[start]) continue;
        std::vector<size_t> sequence;  // the cuts of this ring, in order
        size_t current = start;
        while (!used[current]) {
            used[current] = 1;
            sequence.push_back(current);
            const uint64_t next = cuts[current].out;
            size_t lo = 0, hi = cuts.size();
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (cuts[mid].in < next) lo = mid + 1;
                else hi = mid;
            }
            if (lo == cuts.size() || cuts[lo].in != next) break;  // cannot happen in a closed mesh
            current = lo;
        }
        // The point on the edge by which each cut is entered, unless the triangle before is the same flat face.
        std::vector<Point> ring;
        for (size_t i = 0; i < sequence.size(); ++i) {
            const Cut &cut = cuts[sequence[i]], &before = cuts[sequence[i == 0 ? sequence.size() - 1 : i - 1]];
            const uint32_t below = static_cast<uint32_t>(cut.in >> 32), over = static_cast<uint32_t>(cut.in & 0xffffffffu);
            if (before.out == cut.in && flat_across(m, m.t[before.triangle], m.t[cut.triangle], below, over)) continue;
            ring.push_back(point(below, over));
        }
        rings.push_back(ring);
    }
    return rings;
}

// Drops repeated points (a section through a vertex meets it once per triangle) and rings left without area.
template <class Point, class Same>
void tidy(std::vector<std::vector<Point>> &rings, Same &&same) {
    std::vector<std::vector<Point>> kept;
    for (std::vector<Point> &ring : rings) {
        std::vector<Point> out;
        for (const Point &p : ring)
            if (out.empty() || !same(out.back(), p)) out.push_back(p);
        while (out.size() > 1 && same(out.front(), out.back())) out.pop_back();
        if (out.size() >= 3) kept.push_back(out);
    }
    rings.swap(kept);
}

}  // namespace

std::vector<Ring> section(const Solid &solid, double z) {
    const detail::Mesh &m = detail::Access::mesh(solid);
    const double h = z * kUnitsPerMm;
    if (!(std::fabs(h) <= 4.0 * static_cast<double>(detail::kMaxCoord))) throw ValueError("section: the height must be finite");
    std::vector<char> above(m.v.size());
    for (size_t i = 0; i < m.v.size(); ++i) above[i] = static_cast<double>(m.v[i][2]) > h;
    std::vector<Ring> rings = chain<Vec2>(m, above, [&](uint32_t lo, uint32_t hi) {
        const GridPoint &a = m.v[lo], &b = m.v[hi];
        const double t = (h - static_cast<double>(a[2])) / static_cast<double>(b[2] - a[2]);
        return Vec2{(static_cast<double>(a[0]) + t * static_cast<double>(b[0] - a[0])) / kUnitsPerMm,
                    (static_cast<double>(a[1]) + t * static_cast<double>(b[1] - a[1])) / kUnitsPerMm};
    });
    tidy(rings, [](const Vec2 &p, const Vec2 &q) { return p.x == q.x && p.y == q.y; });
    return rings;
}

PlaneSection section(const Solid &solid, const Plane3 &plane) {
    const detail::Mesh &m = detail::Access::mesh(solid);
    const double length = std::sqrt(plane.normal.x * plane.normal.x + plane.normal.y * plane.normal.y + plane.normal.z * plane.normal.z);
    if (!(length > 0) || !(length < 1e150)) throw ValueError("section: the plane normal must have a direction");
    PlaneSection out;
    out.origin = plane.origin;
    out.w = Vec3{plane.normal.x / length, plane.normal.y / length, plane.normal.z / length};
    // v = w x e for the axis e the normal leans on least, then u = v x w: for a horizontal plane u is X and v is Y.
    const double ax = std::fabs(out.w.x), ay = std::fabs(out.w.y), az = std::fabs(out.w.z);
    const Vec3 e = ax <= ay && ax <= az ? Vec3{1, 0, 0} : ay <= az ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    Vec3 v{out.w.y * e.z - out.w.z * e.y, out.w.z * e.x - out.w.x * e.z, out.w.x * e.y - out.w.y * e.x};
    const double vl = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    v = Vec3{v.x / vl, v.y / vl, v.z / vl};
    out.v = v;
    out.u = Vec3{v.y * out.w.z - v.z * out.w.y, v.z * out.w.x - v.x * out.w.z, v.x * out.w.y - v.y * out.w.x};
    // The height of each vertex over the plane, in millimetres; one value per vertex decides its side.
    std::vector<double> height(m.v.size());
    std::vector<char> above(m.v.size());
    for (size_t i = 0; i < m.v.size(); ++i) {
        height[i] = (detail::to_mm(m.v[i][0]) - plane.origin.x) * out.w.x + (detail::to_mm(m.v[i][1]) - plane.origin.y) * out.w.y +
                    (detail::to_mm(m.v[i][2]) - plane.origin.z) * out.w.z;
        if (!(std::fabs(height[i]) < 1e300)) throw ValueError("section: the plane must be finite");
        above[i] = height[i] > 0;
    }
    out.rings3d = chain<Vec3>(m, above, [&](uint32_t lo, uint32_t hi) {
        const double t = height[lo] / (height[lo] - height[hi]);
        const GridPoint &a = m.v[lo], &b = m.v[hi];
        return Vec3{detail::to_mm(a[0]) + t * (detail::to_mm(b[0]) - detail::to_mm(a[0])), detail::to_mm(a[1]) + t * (detail::to_mm(b[1]) - detail::to_mm(a[1])),
                    detail::to_mm(a[2]) + t * (detail::to_mm(b[2]) - detail::to_mm(a[2]))};
    });
    tidy(out.rings3d, [](const Vec3 &p, const Vec3 &q) { return p.x == q.x && p.y == q.y && p.z == q.z; });
    for (const std::vector<Vec3> &ring3 : out.rings3d) {
        Ring ring;
        for (const Vec3 &p : ring3) {
            const double dx = p.x - plane.origin.x, dy = p.y - plane.origin.y, dz = p.z - plane.origin.z;
            ring.push_back(Vec2{dx * out.u.x + dy * out.u.y + dz * out.u.z, dx * out.v.x + dy * out.v.y + dz * out.v.z});
        }
        out.rings.push_back(ring);
    }
    return out;
}

}  // namespace solid
}  // namespace kerfkite
