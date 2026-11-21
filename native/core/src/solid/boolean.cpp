// Boolean operations: the arrangement engine applied to solids, then the rounding of its exact result
// to the grid, with the checks that make sure rounding has not damaged it.
//
// Rounding. The arrangement is computed exactly, so before rounding the result is a valid solid whose
// new vertices are rational points. Each of those is moved to the nearest grid point (less than 0.87 nm
// away), and rounded vertices that an edge of at most two grid units joins are merged into one. That
// can, for features about as fine as the grid, flatten a triangle, push one triangle through another
// or turn a sliver inside out. So afterwards the kernel checks, exactly:
//   (a) no triangle with a moved vertex has lost its area,
//   (b) no triangle with a moved vertex meets any other triangle except in a common vertex or edge,
//   (c) for every connected shell, moved or not, the space in front of one of its triangles is empty (an
//       exact ray count), so that no shell has turned inside out and none lies on the wrong side of
//       another. A shell that did not move is checked too: a surface that moved may have passed it.
// Triangles without a moved vertex are faces of the exact arrangement and cannot meet wrongly. With (b),
// the shells are closed surfaces that do not cross, and (c) then fixes the winding number on both sides
// of each: the mesh is a valid solid. If a check fails, the rounded mesh is resolved against itself (the
// same engine, keeping what is wound a positive number of times) and rounded again. A result that still
// fails after kRepairPasses is refused; an invalid solid is never returned.
#include <algorithm>
#include <numeric>

#include "arrangement.hpp"
#include "edgemap.hpp"
#include "timing.hpp"

namespace kerfkite {
namespace solid {

namespace {
thread_local BooleanStats g_stats;
thread_local int g_coarseness = 0;
const int kRepairPasses = 4;
const int kCoarsest = 3;

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

int coarseness() { return g_coarseness; }

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

// Whether the rounded mesh has a defect: see the head of this file. `touched` holds, in order, the grid
// points that rounding or merging produced; only triangles with a corner at one of them can be wrong.
bool damaged(const std::vector<GridPoint> &v, const std::vector<Triangle> &t, const std::vector<GridPoint> &touched) {
    KERFKITE_SOLID_WHOLE_PHASE(Verify);
    std::vector<char> at(v.size());
    for (size_t i = 0; i < v.size(); ++i) at[i] = std::binary_search(touched.begin(), touched.end(), v[i]);
    std::vector<char> flagged(t.size(), 0);
    for (size_t i = 0; i < t.size(); ++i) {
        const Triangle &tri = t[i];
        if (!at[tri[0]] && !at[tri[1]] && !at[tri[2]]) continue;
        flagged[i] = 1;
        const Plane pl = plane_of(v[tri[0]].data(), v[tri[1]].data(), v[tri[2]].data());
        if (pl.n[0].zero() && pl.n[1].zero() && pl.n[2].zero()) return true;
    }
    Arrangement check;
    std::vector<Operand> operands(1, Operand{&v, &t, 0, false, nullptr});
    check.load(operands);
    check.probe_only();
    if (check.intersect(&flagged, true) > 0) return true;
    // One triangle of each shell: the space in front of it must be empty.
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
        for (size_t k = 1; k < 3; ++k) {
            const uint32_t a = find(tri[0]), b = find(tri[k]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
    std::vector<char> shell_done(v.size(), 0);
    for (size_t i = 0; i < t.size(); ++i) {
        const uint32_t root = find(t[i][0]);
        if (shell_done[root]) continue;
        shell_done[root] = 1;
        if (check.winding_in_front(static_cast<int>(i)) != 0) return true;
    }
    return false;
}

// After rounding, a triangle with a moved corner may be left as a cap: a corner within a grid unit or two
// of the opposite edge. Its height is below the resolution of the grid, and the next rounding or a
// neighbour could turn it over. The edge it leans on is replaced by the other diagonal of the two
// triangles that share it, which removes the cap and bends the surface by less than its height.
// The corners of every triangle changed are added to `touched`.
void flip_caps(const std::vector<GridPoint> &v, std::vector<Triangle> &t, std::vector<GridPoint> &touched) {
    const double kLow = 2.0 * static_cast<double>(1 << coarseness());
    // Directed edge -> its triangle. Most results have no cap at all, so the table is made when the first
    // one is found.
    EdgeMap edge;
    bool mapped = false;
    const auto key = [](uint32_t a, uint32_t b) { return EdgeMap::key(a, b); };
    const auto map_edges = [&] {
        if (mapped) return;
        mapped = true;
        edge.reserve(t.size() * 3);
        for (uint32_t i = 0; i < t.size(); ++i)
            for (size_t k = 0; k < 3; ++k) edge.set(key(t[i][k], t[i][(k + 1) % 3]), i);
    };
    // Whether each vertex is at a touched point.
    std::vector<char> at(v.size());
    for (size_t i = 0; i < v.size(); ++i) at[i] = std::binary_search(touched.begin(), touched.end(), v[i]);
    // The squared height over the longest edge, and which corner starts that edge.
    const auto measure = [&](uint32_t a, uint32_t b, uint32_t c, int &longest) {
        const uint32_t id[3] = {a, b, c};
        double best = -1;
        for (int k = 0; k < 3; ++k) {
            const GridPoint &p = v[id[k]], &q = v[id[(k + 1) % 3]];
            const double dx = static_cast<double>(q[0] - p[0]), dy = static_cast<double>(q[1] - p[1]), dz = static_cast<double>(q[2] - p[2]);
            const double length = dx * dx + dy * dy + dz * dz;
            if (length > best) {
                best = length;
                longest = k;
            }
        }
        const double ux = static_cast<double>(v[b][0] - v[a][0]), uy = static_cast<double>(v[b][1] - v[a][1]), uz = static_cast<double>(v[b][2] - v[a][2]);
        const double wx = static_cast<double>(v[c][0] - v[a][0]), wy = static_cast<double>(v[c][1] - v[a][1]), wz = static_cast<double>(v[c][2] - v[a][2]);
        const double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        return best > 0 ? (nx * nx + ny * ny + nz * nz) / best : 0.0;
    };
    std::vector<GridPoint> added;
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (uint32_t i = 0; i < t.size(); ++i) {
            const Triangle tri = t[i];
            if (!at[tri[0]] && !at[tri[1]] && !at[tri[2]] &&
                (added.empty() || (std::find(added.begin(), added.end(), v[tri[0]]) == added.end() && std::find(added.begin(), added.end(), v[tri[1]]) == added.end() &&
                                   std::find(added.begin(), added.end(), v[tri[2]]) == added.end())))
                continue;
            int longest = 0;
            const double height2 = measure(tri[0], tri[1], tri[2], longest);
            if (height2 >= kLow * kLow) continue;
            const uint32_t a = tri[static_cast<size_t>(longest)], b = tri[static_cast<size_t>((longest + 1) % 3)], c = tri[static_cast<size_t>((longest + 2) % 3)];
            map_edges();
            const uint32_t *across = edge.find(key(b, a));
            if (!across || *across == i) continue;
            const uint32_t j = *across;
            const Triangle other = t[j];
            const uint32_t d = other[0] != a && other[0] != b ? other[0] : other[1] != a && other[1] != b ? other[1] : other[2];
            if (d == c || edge.has(key(c, d)) || edge.has(key(d, c))) continue;
            int unused = 0;
            const double first = measure(a, d, c, unused), second = measure(d, b, c, unused);
            if (!(std::min(first, second) > height2)) continue;  // no better: leave it
            for (size_t k = 0; k < 3; ++k) {
                edge.erase(key(tri[k], tri[(k + 1) % 3]));
                edge.erase(key(other[k], other[(k + 1) % 3]));
            }
            t[i] = Triangle{{a, d, c}};
            t[j] = Triangle{{d, b, c}};
            for (size_t k = 0; k < 3; ++k) {
                edge.set(key(t[i][k], t[i][(k + 1) % 3]), i);
                edge.set(key(t[j][k], t[j][(k + 1) % 3]), j);
            }
            for (const uint32_t x : {a, b, c, d}) added.push_back(v[x]);
            changed = true;
        }
        if (!changed) break;
    }
    if (added.empty()) return;
    touched.insert(touched.end(), added.begin(), added.end());
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
}

}  // namespace

Solid finish(Arrangement &arrangement, ExactMesh &exact, const char *context, int depth) {
    if (exact.tri.empty()) return Solid();
    {
        KERFKITE_SOLID_PHASE(Simplify);
        simplify(arrangement.pool, exact, &arrangement);
    }
    KERFKITE_SOLID_PHASE(Round);
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
    if (!rounded) return make_solid(std::move(v), std::move(exact.tri), true, context);
    std::vector<Triangle> t = std::move(exact.tri);

    // Rounded vertices joined by an edge of at most kMerge grid units are one vertex: what lies between
    // them is below the resolution of the grid. A vertex the operands already had stays where it is.
    const int64_t scale = int64_t(1) << g_coarseness, kMerge = 2 * scale, kMerge2 = kMerge * kMerge, kMergeSpan = 8 * scale;
    std::vector<uint32_t> parent(nv);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    bool merging = false;
    for (const Triangle &tri : t)
        for (size_t k = 0; k < 3; ++k) {
            const uint32_t a = tri[k], b = tri[(k + 1) % 3];
            if (!moved[a] && !moved[b]) continue;
            const int64_t dx = v[a][0] - v[b][0], dy = v[a][1] - v[b][1], dz = v[a][2] - v[b][2];
            if (dx > kMerge || dx < -kMerge || dy > kMerge || dy < -kMerge || dz > kMerge || dz < -kMerge || dx * dx + dy * dy + dz * dz > kMerge2) continue;
            const uint32_t ra = find(a), rb = find(b);
            if (ra != rb) {
                parent[std::max(ra, rb)] = std::min(ra, rb);
                merging = true;
            }
        }
    std::vector<GridPoint> touched;
    for (size_t i = 0; i < nv; ++i)
        if (moved[i]) touched.push_back(v[i]);
    if (merging) {
        // For each group: its extent, and the member to keep (an original vertex if it has one, then the lowest position).
        std::vector<GridPoint> lo(v), hi(v);
        std::vector<uint32_t> keep(nv);
        std::iota(keep.begin(), keep.end(), 0u);
        for (uint32_t i = 0; i < nv; ++i) {
            const uint32_t r = find(i);
            for (size_t k = 0; k < 3; ++k) {
                lo[r][k] = std::min(lo[r][k], v[i][k]);
                hi[r][k] = std::max(hi[r][k], v[i][k]);
            }
            const uint32_t best = keep[r];
            if ((moved[best] && !moved[i]) || (moved[best] == moved[i] && v[i] < v[best])) keep[r] = i;
        }
        std::vector<GridPoint> merged(v);
        for (uint32_t i = 0; i < nv; ++i) {
            const uint32_t r = find(i);
            if (hi[r][0] - lo[r][0] > kMergeSpan || hi[r][1] - lo[r][1] > kMergeSpan || hi[r][2] - lo[r][2] > kMergeSpan) continue;
            if (keep[r] != i || r != i) touched.push_back(v[keep[r]]);
            merged[i] = v[keep[r]];
        }
        v.swap(merged);
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    bool sound = true;
    bool collapsed = false;
    for (const Triangle &tri : t) collapsed = collapsed || v[tri[0]] == v[tri[1]] || v[tri[1]] == v[tri[2]] || v[tri[0]] == v[tri[2]];
    if (collapsed) {
        // Triangles have closed up: join what is left by position.
        try {
            weld_and_pair(v, t, context);
        } catch (const ValueError &) {
            sound = false;
        }
        if (sound && t.empty()) return Solid();
    }
    if (sound) flip_caps(v, t, touched);
    if (sound && !damaged(v, t, touched)) return make_solid(std::move(v), std::move(t), true, context);
    if (depth >= kRepairPasses) throw TooFine(std::string(context) + ": the result has features too fine for the 1 nm grid");
    ++g_stats.repair_passes;
    return resolve_self_at(std::move(v), std::move(t), context, depth + 1);
}

Solid resolve_self(std::vector<GridPoint> v, std::vector<Triangle> t, const char *context) {
    struct Reset {
        ~Reset() { g_coarseness = 0; }
    } reset;
    for (g_coarseness = 0;; ++g_coarseness) {
        try {
            return resolve_self_at(v, t, context, 1);
        } catch (const TooFine &) {
            if (g_coarseness == kCoarsest) throw;
        }
    }
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
    struct Reset {
        ~Reset() { g_coarseness = 0; }
    } reset;
    for (g_coarseness = 0;; ++g_coarseness) {
        try {
            detail::Arrangement arrangement;
            arrangement.load(operands);
            arrangement.enable_ribbons();
            arrangement.remember_shapes();
            arrangement.intersect();
            arrangement.record_simple();
            detail::ExactMesh exact;
            arrangement.resolve(rule, exact);
            add_stats(arrangement.stats);
            return detail::finish(arrangement, exact, context, 0);
        } catch (const detail::TooFine &) {
            // Sub-resolution features survived: remove them more coarsely and start again.
            if (g_coarseness == kCoarsest) throw;
            ++g_stats.coarser_retries;
        }
    }
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

namespace {

// One operation of a batch, with the counts of the batch so far carried along.
Solid step(const Solid &a, const Solid &b, Op op) {
    const BooleanStats so_far = g_stats;
    const Solid result = boolean(a, b, op);
    add_stats(so_far);
    g_stats.rounded_vertices += so_far.rounded_vertices;
    g_stats.repair_passes += so_far.repair_passes;
    g_stats.coarser_retries += so_far.coarser_retries;
    return result;
}

// The union of many, by joining neighbours in the list pair by pair until one is left. Each solid then
// takes part in about log2(n) operations, and material that ends up inside is dropped early. (One
// arrangement of all of them at once is possible with this engine but much slower where many overlap:
// every pair of surfaces is then intersected, including the parts deep inside the result.)
Solid union_of(std::vector<Solid> solids) {
    while (solids.size() > 1) {
        std::vector<Solid> next;
        next.reserve(solids.size() / 2 + 1);
        for (size_t i = 0; i + 1 < solids.size(); i += 2) next.push_back(step(solids[i], solids[i + 1], Op::Union));
        if (solids.size() % 2) next.push_back(solids.back());
        solids.swap(next);
    }
    return solids.empty() ? Solid() : solids[0];
}

}  // namespace

Solid batch_boolean(const std::vector<Solid> &solids, Op op) {
    g_stats = BooleanStats();
    if (solids.empty()) return Solid();
    if (op == Op::Intersection) {
        Solid result = solids[0];
        for (size_t i = 1; i < solids.size() && !result.empty(); ++i) result = step(result, solids[i], Op::Intersection);
        return result;
    }
    if (op == Op::Union) return union_of(solids);
    if (solids[0].empty()) return Solid();
    return step(solids[0], union_of(std::vector<Solid>(solids.begin() + 1, solids.end())), Op::Difference);
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
