// Boolean operations: the arrangement engine applied to solids, then the rounding of its exact result
// to the grid, with the checks that make sure rounding has not damaged it.
//
// Rounding. The arrangement is computed exactly, so before rounding the result is a valid solid whose
// new vertices are rational points. Each of those is moved to the nearest grid point (less than 0.87 nm
// away). That can, for features about as fine as the grid, flatten a triangle, push one triangle through
// another or turn a sliver inside out. So after rounding the kernel checks, exactly:
//   (a) no triangle with a moved vertex has lost its area,
//   (b) no triangle with a moved vertex meets any other triangle except in a common vertex or edge,
//   (c) every connected shell with a moved vertex still encloses volume of the same sign.
// Triangles without a moved vertex are faces of the exact arrangement and cannot meet wrongly. If a check
// fails, the rounded mesh is resolved against itself (the same engine, keeping what is wound a positive
// number of times) and rounded again. A result that still fails after kRepairPasses is refused; an
// invalid solid is never returned.
#include <algorithm>
#include <numeric>
#include <cstdio>
#include <cstdlib>

#include "arrangement.hpp"

namespace kerfkite {
namespace solid {

namespace {
thread_local BooleanStats g_stats;
const int kRepairPasses = 8;

void add_stats(const BooleanStats &s) {
    g_stats.candidate_pairs += s.candidate_pairs;
    g_stats.intersecting_pairs += s.intersecting_pairs;
    g_stats.split_triangles += s.split_triangles;
    g_stats.coplanar_groups += s.coplanar_groups;
    g_stats.constructed_points += s.constructed_points;
    g_stats.exact_fallbacks += s.exact_fallbacks;
}
}  // namespace

BooleanStats last_boolean_stats() { return g_stats; }

namespace detail {

namespace {

Solid resolve_self_at(std::vector<GridPoint> v, std::vector<Triangle> t, const char *context, int depth) {
    Arrangement arrangement;
    std::vector<Operand> operands(1, Operand{&v, &t, 0, false, nullptr});
    arrangement.load(operands);
    arrangement.intersect();
    ExactMesh exact;
    arrangement.resolve(Rule::Positive, exact);
    add_stats(arrangement.stats);
    return finish(arrangement, exact, context, depth);
}

// Whether rounding left a defect: see the head of this file.
bool damaged(const Arrangement &arrangement, const ExactMesh &exact, const std::vector<GridPoint> &v, const std::vector<char> &moved) {
    const std::vector<Triangle> &t = exact.tri;
    std::vector<char> flagged(t.size(), 0);
    for (size_t i = 0; i < t.size(); ++i) {
        const Triangle &tri = t[i];
        if (!moved[tri[0]] && !moved[tri[1]] && !moved[tri[2]]) continue;
        flagged[i] = 1;
        const Plane pl = plane_of(v[tri[0]].data(), v[tri[1]].data(), v[tri[2]].data());
        if (pl.n[0].zero() && pl.n[1].zero() && pl.n[2].zero()) { if (getenv("KKDEBUG")) fprintf(stderr, "damaged: flat triangle %zu\n", i); return true; }
    }
    // Shells, and the sign of the volume of those that moved.
    std::vector<uint32_t> parent(v.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (const Triangle &tri : t)
        for (int k = 1; k < 3; ++k) {
            const uint32_t a = find(tri[0]), b = find(tri[static_cast<size_t>(k)]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
    std::vector<char> shell_moved(v.size(), 0);
    for (uint32_t i = 0; i < v.size(); ++i)
        if (moved[i]) shell_moved[find(i)] = 1;
    std::vector<Interval> before(v.size());
    std::vector<I128> partial(v.size());
    std::vector<BigInt> after;
    std::vector<int> slot(v.size(), -1);
    std::vector<uint32_t> pending(v.size(), 0);
    const Pool &pool = arrangement.pool;
    for (const Triangle &tri : t) {
        const uint32_t root = find(tri[0]);
        if (!shell_moved[root]) continue;
        if (slot[root] < 0) {
            slot[root] = static_cast<int>(after.size());
            after.push_back(BigInt());
        }
        // Tetrahedra from the shell's lowest vertex, before (intervals) and after (exact).
        const Pt &o = pool.pts[static_cast<size_t>(exact.vertex[root])];
        const Pt &a = pool.pts[static_cast<size_t>(exact.vertex[tri[0]])], &b = pool.pts[static_cast<size_t>(exact.vertex[tri[1]])],
                 &c = pool.pts[static_cast<size_t>(exact.vertex[tri[2]])];
        const Interval ux = a.c[0] - o.c[0], uy = a.c[1] - o.c[1], uz = a.c[2] - o.c[2];
        const Interval vx = b.c[0] - o.c[0], vy = b.c[1] - o.c[1], vz = b.c[2] - o.c[2];
        const Interval wx = c.c[0] - o.c[0], wy = c.c[1] - o.c[1], wz = c.c[2] - o.c[2];
        before[root] = before[root] + (ux * (vy * wz - vz * wy) - uy * (vx * wz - vz * wx) + uz * (vx * wy - vy * wx));
        partial[root] = partial[root] + orient3d_value(v[root].data(), v[tri[0]].data(), v[tri[1]].data(), v[tri[2]].data());
        if (++pending[root] == 256) {
            after[static_cast<size_t>(slot[root])] = after[static_cast<size_t>(slot[root])] + BigInt(partial[root]);
            partial[root] = I128();
            pending[root] = 0;
        }
    }
    for (uint32_t i = 0; i < v.size(); ++i) {
        if (slot[i] < 0) continue;
        const int now = (after[static_cast<size_t>(slot[i])] + BigInt(partial[i])).sign();
        const int was = before[i].sign();
        if (was == 2 || was != now || now == 0) { if (getenv("KKDEBUG")) fprintf(stderr, "damaged: shell sign %d -> %d\n", was, now); return true; }
    }
    // Pairs of triangles, one of which moved.
    Arrangement check;
    std::vector<Operand> operands(1, Operand{&v, &t, 0, false, nullptr});
    check.load(operands);
    const bool hit = check.intersect(&flagged, true) > 0;
    if (hit && getenv("KKDEBUG")) { fprintf(stderr, "moved:"); for (size_t i = 0; i < v.size(); ++i) if (moved[i]) fprintf(stderr, " %zu", i); fprintf(stderr, "\n");
      for (size_t i = 0; i < v.size(); ++i) { const Pt &q = arrangement.pool.pts[exact.vertex[i]]; fprintf(stderr, "  v%zu pool %d rat %d: %.17g %.17g %.17g\n", i, exact.vertex[i], q.rat, q.c[0].lo, q.c[1].lo, q.c[2].lo); } }
    if (getenv("KKDEBUG")) fprintf(stderr, "damaged: pairs %d (nv %zu nt %zu)\n", (int)hit, v.size(), t.size());
    return hit;
}

}  // namespace

Solid finish(Arrangement &arrangement, ExactMesh &exact, const char *context, int depth) {
    if (exact.tri.empty()) return Solid();
    const size_t nv = exact.vertex.size();
    std::vector<GridPoint> v(nv);
    std::vector<char> moved(nv, 0);
    size_t rounded = 0;
    for (size_t i = 0; i < nv; ++i) {
        int64_t out[3];
        arrangement.pool.round(exact.vertex[i], out);
        v[i] = GridPoint{{out[0], out[1], out[2]}};
        if (!arrangement.pool.grid(exact.vertex[i])) {
            moved[i] = 1;
            ++rounded;
        }
    }
    g_stats.rounded_vertices += rounded;
    if (!rounded || !damaged(arrangement, exact, v, moved)) return make_solid(std::move(v), std::move(exact.tri), true, context);
    if (depth >= kRepairPasses) throw ValueError(std::string(context) + ": the result has features too fine for the 1 nm grid");
    ++g_stats.repair_passes;
    return resolve_self_at(std::move(v), std::move(exact.tri), context, depth + 1);
}

Solid resolve_self(std::vector<GridPoint> v, std::vector<Triangle> t, const char *context) {
    return resolve_self_at(std::move(v), std::move(t), context, 1);
}

}  // namespace detail

using detail::Access;
using detail::Mesh;

namespace {

bool apart(const Mesh &a, const Mesh &b) {
    for (int k = 0; k < 3; ++k)
        if (a.hi[k] < b.lo[k] || b.hi[k] < a.lo[k]) return true;
    return false;
}

Solid run(const std::vector<detail::Operand> &operands, detail::Rule rule, const char *context) {
    detail::Arrangement arrangement;
    arrangement.load(operands);
    arrangement.intersect();
    arrangement.record_simple();
    detail::ExactMesh exact;
    arrangement.resolve(rule, exact);
    add_stats(arrangement.stats);
    return detail::finish(arrangement, exact, context, 0);
}

detail::Operand operand(const Mesh &m, int label) { return detail::Operand{&m.v, &m.t, label, m.simple, &m}; }

}  // namespace

Solid boolean(const Solid &a, const Solid &b, Op op) {
    g_stats = BooleanStats();
    const Mesh &ma = Access::mesh(a), &mb = Access::mesh(b);
    if (ma.t.empty()) return op == Op::Union ? b : Solid();
    if (mb.t.empty()) return op == Op::Intersection ? Solid() : a;
    if (apart(ma, mb)) {
        if (op == Op::Intersection) return Solid();
        if (op == Op::Difference) return a;
        std::vector<GridPoint> v = ma.v;
        v.insert(v.end(), mb.v.begin(), mb.v.end());
        std::vector<Triangle> t = ma.t;
        const uint32_t offset = static_cast<uint32_t>(ma.v.size());
        for (const Triangle &tri : mb.t) t.push_back(Triangle{{tri[0] + offset, tri[1] + offset, tri[2] + offset}});
        if (t.size() > kMaxTriangles) throw ValueError("union: the result has more than " + std::to_string(kMaxTriangles) + " triangles");
        return detail::make_solid(std::move(v), std::move(t), ma.simple && mb.simple, "union", true);
    }
    std::vector<detail::Operand> operands;
    operands.push_back(operand(ma, 0));
    operands.push_back(operand(mb, 1));
    switch (op) {
        case Op::Union: return run(operands, detail::Rule::Union, "union");
        case Op::Difference: return run(operands, detail::Rule::Difference, "difference");
        default: return run(operands, detail::Rule::Intersection, "intersection");
    }
}

Solid batch_boolean(const std::vector<Solid> &solids, Op op) {
    g_stats = BooleanStats();
    if (solids.empty()) return Solid();
    if (op == Op::Intersection) {
        Solid result = solids[0];
        for (size_t i = 1; i < solids.size() && !result.empty(); ++i) {
            const BooleanStats so_far = g_stats;
            result = boolean(result, solids[i], Op::Intersection);
            add_stats(so_far);
        }
        return result;
    }
    // One arrangement of all of them: the union counts every operand together; the difference counts the
    // first apart from the rest.
    std::vector<detail::Operand> operands;
    size_t only = 0;
    for (size_t i = 0; i < solids.size(); ++i) {
        const Mesh &m = Access::mesh(solids[i]);
        if (m.t.empty()) {
            if (i == 0 && op == Op::Difference) return Solid();
            continue;
        }
        only = i;
        operands.push_back(operand(m, op == Op::Difference && i > 0 ? 1 : 0));
    }
    if (operands.empty()) return Solid();
    if (operands.size() == 1) return solids[only];
    return op == Op::Union ? run(operands, detail::Rule::Positive, "union") : run(operands, detail::Rule::Difference, "difference");
}

std::size_t count_self_intersections(const Solid &solid) {
    const Mesh &m = Access::mesh(solid);
    if (m.t.empty()) return 0;
    detail::Arrangement arrangement;
    std::vector<detail::Operand> operands(1, detail::Operand{&m.v, &m.t, 0, false, nullptr});
    arrangement.load(operands);
    const std::size_t hits = arrangement.intersect();
    if (!hits) m.simple = true;
    return hits;
}

bool verify_exact(const Solid &solid) {
    const Mesh &m = Access::mesh(solid);
    if (m.t.empty()) return true;
    detail::Arrangement arrangement;
    std::vector<detail::Operand> operands(1, detail::Operand{&m.v, &m.t, 0, false, nullptr});
    arrangement.load(operands);
    if (arrangement.intersect()) return false;
    detail::ExactMesh exact;
    try {
        arrangement.resolve(detail::Rule::Positive, exact);
    } catch (const ValueError &) {
        return false;
    }
    return arrangement.unit_winding();
}

}  // namespace solid
}  // namespace kerfkite
