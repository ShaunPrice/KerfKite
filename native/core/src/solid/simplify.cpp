// Removal of vertices that the surface does not need, decided exactly.
//
// A vertex whose triangles all lie in one plane, or in two planes that meet in a straight edge running
// through it, can be taken out and the hole filled with fewer triangles without changing the surface as
// a set of points. So the result stays a valid solid, with the same volume to the last digit. Subdividing
// triangles along their intersections leaves many such vertices on faces that end up whole in the
// result; removing them before rounding keeps the mesh small and leaves fewer points to round.
#include <algorithm>

#include "arrangement.hpp"
#include "edgemap.hpp"
#include "planar.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

namespace {

const size_t kMaxRing = 64;

struct Simplifier {
    const Pool &pool;
    ExactMesh &mesh;
    std::vector<char> dead;
    std::vector<uint32_t> vtri;  // one live triangle at each vertex
    EdgeMap edge;                // directed edge -> its triangle
    // Working lists of try_remove() and ears(), kept between calls so that they are allocated once.
    std::vector<Planar::Local> ear_at;
    bool plain;  // for tests: every turn asked of the pool
    std::vector<uint32_t> ring, ring_tri, polygon, ear_ring;
    std::vector<size_t> creases;
    std::vector<Triangle> fill;
    std::vector<int> fill_plane;
    const Arrangement *source;  // where the planes of the triangles are known from, or null

    static uint64_t key(uint32_t a, uint32_t b) { return EdgeMap::key(a, b); }
    int p(uint32_t v) const { return mesh.vertex[v]; }

    // Projection axes in which the triangle a b c has area, and the sign of its turn there.
    bool frame(uint32_t a, uint32_t b, uint32_t c, int &au, int &av, int &sign) const {
        for (int k = 0; k < 3; ++k) {
            au = (k + 1) % 3;
            av = (k + 2) % 3;
            sign = pool.orient2d(p(a), p(b), p(c), au, av);
            if (sign) return true;
        }
        return false;
    }
    bool collinear_between(uint32_t a, uint32_t mid, uint32_t b) const {
        for (int k = 0; k < 3; ++k)
            if (pool.orient2d(p(a), p(mid), p(b), (k + 1) % 3, (k + 2) % 3) != 0) return false;
        return between(a, mid, b);
    }
    // For three points of one line: mid strictly between a and b.
    bool between(uint32_t a, uint32_t mid, uint32_t b) const {
        for (int axis = 0; axis < 3; ++axis) {
            const int c = pool.compare(p(a), p(b), axis);
            if (c) return pool.compare(p(a), p(mid), axis) == c && pool.compare(p(mid), p(b), axis) == c;
        }
        return false;
    }

    // Triangulates a planar polygon given counter-clockwise for the frame, by cutting ears. False when no
    // triangulation without zero-area triangles is found.
    bool ears(const std::vector<uint32_t> &polygon, int au, int av, int sign, std::vector<Triangle> &out) {
        std::vector<uint32_t> &ring = ear_ring;
        ring = polygon;
        // A turn is asked of doubles first, within the bound stated at Planar::filtered_turn, and of the
        // pool when they cannot tell; `at` holds the doubles of each corner still in the ring.
        std::vector<Planar::Local> &at = ear_at;
        at.resize(ring.size());
        if (!plain)
            for (size_t k = 0; k < ring.size(); ++k) {
                const Pt &q = pool.pts[static_cast<size_t>(p(ring[k]))];
                at[k] = Planar::local_of(q.c[au], q.c[av], q.rat < 0);
            }
        const auto turn = [&](size_t ia, size_t ib, size_t ic) {
            if (!plain && !(at[ia].grid && at[ib].grid && at[ic].grid)) {
                const int quick = Planar::filtered_turn(at[ia], at[ib], at[ic]);
                if (quick != 2) return sign * quick;
            }
            return sign * pool.orient2d(p(ring[ia]), p(ring[ib]), p(ring[ic]), au, av);
        };
        while (ring.size() > 3) {
            bool cut = false;
            for (size_t i = 0; i < ring.size() && !cut; ++i) {
                const size_t ia = (i + ring.size() - 1) % ring.size(), ib = i, ic = (i + 1) % ring.size();
                if (turn(ia, ib, ic) <= 0) continue;
                bool blocked = false;
                for (size_t j = 0; j < ring.size() && !blocked; ++j) {
                    const uint32_t d = ring[j];
                    if (d == ring[ia] || d == ring[ib] || d == ring[ic]) continue;
                    blocked = turn(ia, ib, j) >= 0 && turn(ib, ic, j) >= 0 && turn(ic, ia, j) >= 0;
                }
                if (blocked) continue;
                out.push_back(Triangle{{ring[ia], ring[ib], ring[ic]}});
                ring.erase(ring.begin() + static_cast<long>(i));
                at.erase(at.begin() + static_cast<long>(i));
                cut = true;
            }
            if (!cut) return false;
        }
        if (turn(0, 1, 2) <= 0) return false;
        out.push_back(Triangle{{ring[0], ring[1], ring[2]}});
        return true;
    }

    // The axes in which the triangles of the plane of mesh triangle t are seen with area, when the planes
    // are known: those of the largest component of the plane's normal, and the turn of a b c there.
    //
    // frame() takes the first pair of axes in which a b c has area, and this takes another. What is then
    // asked is the same either way: whether another triangle of that plane turns as a b c does. Seen along
    // an axis k, a triangle of one plane has the area n_k / |n| times its own (n the plane's normal), so
    // along any axis that is not edge-on two triangles turn the same way or do not, alike.
    bool frame_of(uint32_t t, uint32_t a, uint32_t b, uint32_t c, int &au, int &av, int &sign) const {
        if (!source) return frame(a, b, c, au, av, sign);
        const int axis = source->plane_axis(mesh.plane[t]);
        au = (axis + 1) % 3;
        av = (axis + 2) % 3;
        sign = pool.orient2d(p(a), p(b), p(c), au, av);
        return sign != 0;
    }

    bool try_remove(uint32_t v) {
        // The ring of neighbours, counter-clockwise seen from outside: triangles (v, ring[i], ring[i+1]),
        // which are ring_tri[i].
        ring.clear();
        ring_tri.clear();
        {
            const Triangle &t = mesh.tri[vtri[v]];
            const int k = t[0] == v ? 0 : t[1] == v ? 1 : 2;
            uint32_t next = t[static_cast<size_t>((k + 1) % 3)];
            const uint32_t first = next;
            do {
                ring.push_back(next);
                if (ring.size() > kMaxRing) return false;
                const uint32_t *at = edge.find(key(v, next));
                if (!at) return false;
                ring_tri.push_back(*at);
                const Triangle &u = mesh.tri[*at];
                const int m = u[0] == v ? 0 : u[1] == v ? 1 : 2;
                next = u[static_cast<size_t>((m + 2) % 3)];
            } while (next != first);
        }
        const size_t n = ring.size();
        if (n < 3) return false;
        // Which edges at v are creases: where the two triangles are not in one plane, side by side.
        //
        // Whether they are in one plane is the sign of a 4 x 4 determinant of the points. With the planes
        // known it need not be computed: the two triangles (v a b) and (v b c) lie in the planes of two
        // triangles of the arrangement, which are grid triangles and compared with small integers. If those
        // planes are one, c is in the plane of v a b. If they are two, they meet in the line of v b, which
        // c is not on, so c is in the second only.
        creases.clear();
        bool folded = false;
        if (source) {
            // More than two changes of plane round the vertex are more than two creases: it stays, and
            // nothing else need be asked.
            size_t changes = 0;
            for (size_t i = 0; i < n; ++i) {
                const int here = mesh.plane[ring_tri[i]], next = mesh.plane[ring_tri[(i + 1) % n]];
                if (here != next && !source->same_plane(here, next) && ++changes > 2) return false;
            }
        }
        for (size_t i = 0; i < n; ++i) {
            const uint32_t a = ring[i], b = ring[(i + 1) % n], c = ring[(i + 2) % n];
            bool flat, plane_differs = false;
            if (source) {
                flat = source->same_plane(mesh.plane[ring_tri[i]], mesh.plane[ring_tri[(i + 1) % n]]);
                plane_differs = !flat;
            } else {
                flat = pool.orient3d(p(v), p(a), p(b), p(c)) == 0;
            }
            if (flat) {
                int au, av, sign;
                flat = frame_of(ring_tri[i], v, a, b, au, av, sign) && pool.orient2d(p(v), p(b), p(c), au, av) == sign;
            }
            if (!flat) {
                creases.push_back((i + 1) % n);
                folded = folded || !plane_differs;
                if (creases.size() > 2) return false;
            }
        }
        fill.clear();
        fill_plane.clear();
        if (creases.empty()) {
            int au, av, sign;
            if (!frame_of(ring_tri[0], v, ring[0], ring[1], au, av, sign) || !ears(ring, au, av, sign, fill)) return false;
            if (source) fill_plane.assign(fill.size(), mesh.plane[ring_tri[0]]);
        } else if (creases.size() == 2) {
            const size_t ip = creases[0], iq = creases[1];
            // Two creases between two planes both lie in the line where the planes meet, so v and the two
            // ring vertices on them are in one line; what is left to ask is whether v is between them. A
            // crease of any other kind (a fold within one plane) is tested as before.
            if (source && !folded ? !between(ring[ip], v, ring[iq]) : !collinear_between(ring[ip], v, ring[iq])) return false;
            for (int side = 0; side < 2; ++side) {
                const size_t from = side ? iq : ip, to = side ? ip : iq;
                polygon.clear();
                for (size_t i = from;; i = (i + 1) % n) {
                    polygon.push_back(ring[i]);
                    if (i == to) break;
                }
                if (polygon.size() < 3) return false;
                int au, av, sign;
                if (!frame_of(ring_tri[from], v, polygon[0], polygon[1], au, av, sign) || !ears(polygon, au, av, sign, fill)) return false;
                if (source) fill_plane.resize(fill.size(), mesh.plane[ring_tri[from]]);
            }
        } else {
            return false;
        }
        // A new edge must not be one the mesh already has elsewhere.
        for (const Triangle &t : fill)
            for (size_t k = 0; k < 3; ++k) {
                const uint32_t a = t[k], b = t[(k + 1) % 3];
                const uint32_t *at = edge.find(key(a, b));
                if (!at) continue;
                const Triangle &owner = mesh.tri[*at];
                if (owner[0] != v && owner[1] != v && owner[2] != v) return false;
            }
        for (size_t i = 0; i < n; ++i) {
            const uint32_t t = ring_tri[i];
            dead[t] = 1;
            for (size_t k = 0; k < 3; ++k) edge.erase(key(mesh.tri[t][k], mesh.tri[t][(k + 1) % 3]));
        }
        for (size_t f = 0; f < fill.size(); ++f) {
            const Triangle t = fill[f];
            const uint32_t id = static_cast<uint32_t>(mesh.tri.size());
            mesh.tri.push_back(t);
            if (source) mesh.plane.push_back(fill_plane[f]);
            dead.push_back(0);
            for (size_t k = 0; k < 3; ++k) {
                edge.set(key(t[k], t[(k + 1) % 3]), id);
                vtri[t[k]] = id;
            }
        }
        return true;
    }
};

}  // namespace

void simplify(const Pool &pool, ExactMesh &mesh, const Arrangement *source) {
    if (mesh.tri.empty()) return;
    if (mesh.plane.size() != mesh.tri.size() || plain_paths()) source = nullptr;
    Simplifier s{pool, mesh, std::vector<char>(mesh.tri.size(), 0), std::vector<uint32_t>(mesh.vertex.size(), 0), {}, {}, plain_paths(), {}, {}, {}, {}, {}, {}, {}, source};
    s.edge.reserve(mesh.tri.size() * 3);
    for (uint32_t t = 0; t < mesh.tri.size(); ++t)
        for (size_t k = 0; k < 3; ++k) {
            s.edge.set(Simplifier::key(mesh.tri[t][k], mesh.tri[t][(k + 1) % 3]), t);
            s.vtri[mesh.tri[t][k]] = t;
        }
    const uint32_t nv = static_cast<uint32_t>(mesh.vertex.size());
    // A point where two parts of the surface touch has a vertex in each part. Taking one of them out
    // would leave the other part's vertex in the middle of a triangle or an edge, so those stay.
    std::vector<char> gone(nv, 0);
    {
        std::vector<int> points(mesh.vertex);
        std::sort(points.begin(), points.end());
        for (uint32_t v = 0; v < nv; ++v) {
            const auto range = std::equal_range(points.begin(), points.end(), mesh.vertex[v]);
            if (range.second - range.first > 1) gone[v] = 2;
        }
    }
    bool any = false;
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (uint32_t v = 0; v < nv; ++v) {
            if (gone[v]) continue;
            if (s.try_remove(v)) {
                gone[v] = 1;
                changed = true;
            }
        }
        any = any || changed;
        if (!changed) break;
    }
    if (!any) return;
    std::vector<Triangle> kept;
    std::vector<int> kept_plane;
    kept.reserve(mesh.tri.size());
    for (size_t t = 0; t < mesh.tri.size(); ++t)
        if (!s.dead[t]) {
            kept.push_back(mesh.tri[t]);
            if (source) kept_plane.push_back(mesh.plane[t]);
        }
    mesh.plane.swap(kept_plane);
    std::vector<uint32_t> remap(nv, UINT32_MAX);
    std::vector<int> vertex;
    for (Triangle &t : kept)
        for (size_t k = 0; k < 3; ++k) {
            if (remap[t[k]] == UINT32_MAX) {
                remap[t[k]] = static_cast<uint32_t>(vertex.size());
                vertex.push_back(mesh.vertex[t[k]]);
            }
            t[k] = remap[t[k]];
        }
    mesh.tri.swap(kept);
    mesh.vertex.swap(vertex);
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite
