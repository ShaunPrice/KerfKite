// Removal of vertices that the surface does not need, decided exactly.
//
// A vertex whose triangles all lie in one plane, or in two planes that meet in a straight edge running
// through it, can be taken out and the hole filled with fewer triangles without changing the surface as
// a set of points. So the result stays a valid solid, with the same volume to the last digit. Subdividing
// triangles along their intersections leaves many such vertices on faces that end up whole in the
// result; removing them before rounding keeps the mesh small and leaves fewer points to round.
#include <algorithm>
#include <unordered_map>

#include "arrangement.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

namespace {

const size_t kMaxRing = 64;

struct Simplifier {
    const Pool &pool;
    ExactMesh &mesh;
    std::vector<char> dead;
    std::vector<uint32_t> vtri;                      // one live triangle at each vertex
    std::unordered_map<uint64_t, uint32_t> edge;     // directed edge -> its triangle; looked up, never iterated

    static uint64_t key(uint32_t a, uint32_t b) { return (static_cast<uint64_t>(a) << 32) | b; }
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
        for (int axis = 0; axis < 3; ++axis) {
            const int c = pool.compare(p(a), p(b), axis);
            if (c) return pool.compare(p(a), p(mid), axis) == c && pool.compare(p(mid), p(b), axis) == c;
        }
        return false;
    }

    // Triangulates a planar polygon given counter-clockwise for the frame, by cutting ears. False when no
    // triangulation without zero-area triangles is found.
    bool ears(const std::vector<uint32_t> &polygon, int au, int av, int sign, std::vector<Triangle> &out) const {
        std::vector<uint32_t> ring = polygon;
        const auto turn = [&](uint32_t a, uint32_t b, uint32_t c) { return sign * pool.orient2d(p(a), p(b), p(c), au, av); };
        while (ring.size() > 3) {
            bool cut = false;
            for (size_t i = 0; i < ring.size() && !cut; ++i) {
                const uint32_t a = ring[(i + ring.size() - 1) % ring.size()], b = ring[i], c = ring[(i + 1) % ring.size()];
                if (turn(a, b, c) <= 0) continue;
                bool blocked = false;
                for (size_t j = 0; j < ring.size() && !blocked; ++j) {
                    const uint32_t d = ring[j];
                    if (d == a || d == b || d == c) continue;
                    blocked = turn(a, b, d) >= 0 && turn(b, c, d) >= 0 && turn(c, a, d) >= 0;
                }
                if (blocked) continue;
                out.push_back(Triangle{{a, b, c}});
                ring.erase(ring.begin() + static_cast<long>(i));
                cut = true;
            }
            if (!cut) return false;
        }
        if (turn(ring[0], ring[1], ring[2]) <= 0) return false;
        out.push_back(Triangle{{ring[0], ring[1], ring[2]}});
        return true;
    }

    bool try_remove(uint32_t v) {
        // The ring of neighbours, counter-clockwise seen from outside: triangles (v, ring[i], ring[i+1]).
        std::vector<uint32_t> ring;
        {
            const Triangle &t = mesh.tri[vtri[v]];
            const int k = t[0] == v ? 0 : t[1] == v ? 1 : 2;
            uint32_t next = t[static_cast<size_t>((k + 1) % 3)];
            const uint32_t first = next;
            do {
                ring.push_back(next);
                if (ring.size() > kMaxRing) return false;
                const auto it = edge.find(key(v, next));
                if (it == edge.end()) return false;
                const Triangle &u = mesh.tri[it->second];
                const int m = u[0] == v ? 0 : u[1] == v ? 1 : 2;
                next = u[static_cast<size_t>((m + 2) % 3)];
            } while (next != first);
        }
        const size_t n = ring.size();
        if (n < 3) return false;
        // Which edges at v are creases: where the two triangles are not in one plane, side by side.
        std::vector<size_t> creases;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t a = ring[i], b = ring[(i + 1) % n], c = ring[(i + 2) % n];
            bool flat = pool.orient3d(p(v), p(a), p(b), p(c)) == 0;
            if (flat) {
                int au, av, sign;
                flat = frame(v, a, b, au, av, sign) && pool.orient2d(p(v), p(b), p(c), au, av) == sign;
            }
            if (!flat) {
                creases.push_back((i + 1) % n);
                if (creases.size() > 2) return false;
            }
        }
        std::vector<Triangle> fill;
        if (creases.empty()) {
            int au, av, sign;
            if (!frame(v, ring[0], ring[1], au, av, sign) || !ears(ring, au, av, sign, fill)) return false;
        } else if (creases.size() == 2) {
            const size_t ip = creases[0], iq = creases[1];
            if (!collinear_between(ring[ip], v, ring[iq])) return false;
            for (int side = 0; side < 2; ++side) {
                const size_t from = side ? iq : ip, to = side ? ip : iq;
                std::vector<uint32_t> polygon;
                for (size_t i = from;; i = (i + 1) % n) {
                    polygon.push_back(ring[i]);
                    if (i == to) break;
                }
                if (polygon.size() < 3) return false;
                int au, av, sign;
                if (!frame(v, polygon[0], polygon[1], au, av, sign) || !ears(polygon, au, av, sign, fill)) return false;
            }
        } else {
            return false;
        }
        // A new edge must not be one the mesh already has elsewhere.
        for (const Triangle &t : fill)
            for (size_t k = 0; k < 3; ++k) {
                const uint32_t a = t[k], b = t[(k + 1) % 3];
                const auto it = edge.find(key(a, b));
                if (it == edge.end()) continue;
                const Triangle &owner = mesh.tri[it->second];
                if (owner[0] != v && owner[1] != v && owner[2] != v) return false;
            }
        for (size_t i = 0; i < n; ++i) {
            const auto it = edge.find(key(v, ring[i]));
            const uint32_t t = it->second;
            dead[t] = 1;
            for (size_t k = 0; k < 3; ++k) edge.erase(key(mesh.tri[t][k], mesh.tri[t][(k + 1) % 3]));
        }
        for (const Triangle &t : fill) {
            const uint32_t id = static_cast<uint32_t>(mesh.tri.size());
            mesh.tri.push_back(t);
            dead.push_back(0);
            for (size_t k = 0; k < 3; ++k) {
                edge[key(t[k], t[(k + 1) % 3])] = id;
                vtri[t[k]] = id;
            }
        }
        return true;
    }
};

}  // namespace

void simplify(const Pool &pool, ExactMesh &mesh) {
    if (mesh.tri.empty()) return;
    Simplifier s{pool, mesh, std::vector<char>(mesh.tri.size(), 0), std::vector<uint32_t>(mesh.vertex.size(), 0), {}};
    s.edge.reserve(mesh.tri.size() * 3);
    for (uint32_t t = 0; t < mesh.tri.size(); ++t)
        for (size_t k = 0; k < 3; ++k) {
            s.edge[Simplifier::key(mesh.tri[t][k], mesh.tri[t][(k + 1) % 3])] = t;
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
    kept.reserve(mesh.tri.size());
    for (size_t t = 0; t < mesh.tri.size(); ++t)
        if (!s.dead[t]) kept.push_back(mesh.tri[t]);
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
