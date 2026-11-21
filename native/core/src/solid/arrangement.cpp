// The arrangement of triangle meshes, in outline:
//
//   1. Every pair of triangles that may meet is intersected exactly. Where two triangles cross, the
//      segment they share is recorded on both; its ends are grid vertices or exact rational points.
//      Triangles that overlap in one plane are grouped, to be subdivided together.
//   2. Each triangle that has records is triangulated again so that every recorded point is a vertex
//      and every segment an edge. After this, triangles meet only in whole edges and vertices.
//   3. Identical triangles are merged into faces. Around every edge the faces are put in order, which
//      joins their sides into the regions (cells) of space they bound.
//   4. Crossing a face changes the number of times each operand winds around the point by a known
//      amount, so one exact ray count per connected piece gives the winding numbers of every cell.
//   5. The faces between a cell that is solid by the rule and one that is not are the result, paired
//      at each edge across the solid side, so that the mesh is closed and oriented by construction.
//
// The approach is that of mesh arrangements with winding numbers (Zhou, Grinspun, Zorin and Jacobson,
// "Mesh arrangements for solid geometry", 2016), written here from the published description, with
// exact integer and rational arithmetic in place of an exact-number library.
#include "arrangement.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>

#include "bucket.hpp"
#include "planar.hpp"
#include "radial.hpp"
#include "timing.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

namespace {

[[noreturn]] void defect(const char *what) { throw std::logic_error(std::string("solid kernel: arrangement: ") + what); }

// What radial_sort() asks about the faces at one edge, answered from the planes the faces are known to lie
// in where that saves computing. `parent[i]` is a loaded triangle whose plane holds face i, or the list
// is null and everything is computed from the points.
//
// Two faces at an edge u v with third corners p and q: the determinant of u, v, p, q is zero exactly when
// q is in the plane of u v p. Each face lies in the plane of its parent. If the parents' planes are one,
// it is zero. If they are two, they meet in the line of u v, which q is not on, so it is not zero, and its
// sign is computed. And a face has area seen along the largest component of its plane's normal, so that
// frame is taken at once; a turn within one plane has the same sign relative to the face's own in every
// frame that is not edge-on, so the choice of frame changes no answer.
struct PoolPredicates {
    const Pool *pool;
    const Arrangement *planes;
    const std::vector<int> *parent;
    int orient3d(int a, int b, int c, int d) const { return pool->orient3d(a, b, c, d); }
    int orient2d(int a, int b, int c, int au, int av) const { return pool->orient2d(a, b, c, au, av); }
    int frame(int a, int b, const std::vector<int> &third, int i, int &au, int &av) const {
        if (!parent) return plain_radial_frame(*this, a, b, third[static_cast<size_t>(i)], au, av);
        const int axis = planes->plane_axis((*parent)[static_cast<size_t>(i)]);
        au = (axis + 1) % 3;
        av = (axis + 2) % 3;
        return pool->orient2d(a, b, third[static_cast<size_t>(i)], au, av);
    }
    int side(int a, int b, int p, int au, int av) const { return pool->orient2d(a, b, p, au, av); }
    int turn(int a, int b, const std::vector<int> &third, int i, int j) const {
        if (parent && planes->same_plane((*parent)[static_cast<size_t>(i)], (*parent)[static_cast<size_t>(j)])) return 0;
        return pool->orient3d(a, b, third[static_cast<size_t>(i)], third[static_cast<size_t>(j)]);
    }
};

// Ray directions for the winding count: the axes first (a box query finds their candidates), then
// directions unlikely to be parallel to anything in a model.
const int64_t kDirections[][3] = {
    {1, 0, 0},      {0, 1, 0},       {0, 0, 1},       {-1, 0, 0},      {0, -1, 0},      {0, 0, -1},     {997, 31, 7},
    {-13, 1009, 29}, {41, -17, 1013}, {-983, -43, 11}, {19, -971, -53}, {-61, 23, -967}, {911, 449, 97}, {-103, 883, 421},
    {409, -107, 877}, {-859, -401, 109}, {113, -853, -397}, {-389, 127, -839}, {757, 643, 331}, {-337, 751, 641},
    {631, -347, 743}, {-739, -619, 349}, {353, -733, -617}, {-613, 359, -727}, {1019, 2, 3},   {5, 1021, 2},
    {3, 5, 1031},    {-1033, 7, 5},   {11, -1039, 7},  {7, 13, -1049},  {523, 541, 547}, {-557, 563, -569},
};

}  // namespace

bool &plain_paths() {
    static thread_local bool plain = false;
    return plain;
}

namespace {

// Shapes found inert: an operand not yet known to be free of self-intersection is examined against
// itself, every pair of its own triangles whose boxes overlap. For a primitive that is thousands of
// pairs (every two triangles of a cap share a corner), and nothing comes of them: no pair meets, and no
// pair gets as far as finding where an edge crosses a plane, so no point is made and nothing is noted.
// Such an examination changes nothing but the count of pairs examined. A model is rebuilt from the same
// primitives at every edit, and one pattern holds many copies of one, so the shapes found inert are
// remembered, with their count, and an operand of a remembered shape is taken as examined: its own pairs
// are left out and the count is added, which leaves every point, every number and every result as it
// would have been.
//
// A shape is the triangles and the vertices relative to the first vertex. Everything the examination
// decides is exact and unchanged when a solid is moved by whole grid units: which boxes overlap, on which
// side of a plane a corner lies, whether triangles of one plane overlap. The list is held for each
// thread, the last used first: at most 48 shapes and a million numbers (a few megabytes), and no shape of
// more than 300 000. A shape is recognised by comparing all of it, not by its hash alone.
struct InertShape {
    uint64_t hash;
    std::vector<int64_t> vertices;   // x, y, z of each, less those of the first
    std::vector<uint32_t> triangles;
    size_t pairs;
};
const size_t kInertShapes = 48, kInertNumbers = 300000, kInertNumbersInAll = 1000000;

std::vector<InertShape> &inert_shapes() {
    static thread_local std::vector<InertShape> shapes;
    return shapes;
}

uint64_t shape_hash(const std::vector<GridPoint> &v, const std::vector<Triangle> &t) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (static_cast<uint64_t>(v.size()) << 32) ^ t.size();
    const auto mix = [&](uint64_t x) {
        h = (h ^ x) * 0xD6E8FEB86659FD93ULL;
        h ^= h >> 29;
    };
    for (const GridPoint &p : v)
        for (size_t k = 0; k < 3; ++k) mix(static_cast<uint64_t>(p[k] - v[0][k]));
    for (const Triangle &tri : t) mix((static_cast<uint64_t>(tri[0]) << 42) ^ (static_cast<uint64_t>(tri[1]) << 21) ^ tri[2]);
    return h;
}

bool same_shape(const InertShape &shape, const std::vector<GridPoint> &v, const std::vector<Triangle> &t) {
    if (shape.vertices.size() != 3 * v.size() || shape.triangles.size() != 3 * t.size()) return false;
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t k = 0; k < 3; ++k)
            if (shape.vertices[3 * i + k] != v[i][k] - v[0][k]) return false;
    for (size_t i = 0; i < t.size(); ++i)
        for (size_t k = 0; k < 3; ++k)
            if (shape.triangles[3 * i + k] != t[i][k]) return false;
    return true;
}

// The number of own pairs of a remembered shape, or -1.
long long inert_pairs_of(const std::vector<GridPoint> &v, const std::vector<Triangle> &t) {
    if (v.empty() || t.empty()) return -1;
    std::vector<InertShape> &shapes = inert_shapes();
    if (shapes.empty()) return -1;
    const uint64_t hash = shape_hash(v, t);
    for (size_t k = 0; k < shapes.size(); ++k) {
        if (shapes[k].hash != hash || !same_shape(shapes[k], v, t)) continue;
        std::rotate(shapes.begin(), shapes.begin() + static_cast<std::ptrdiff_t>(k), shapes.begin() + static_cast<std::ptrdiff_t>(k) + 1);
        return static_cast<long long>(shapes[0].pairs);
    }
    return -1;
}

void remember_inert(const std::vector<GridPoint> &v, const std::vector<Triangle> &t, size_t pairs) {
    if (v.empty() || t.empty() || 3 * (v.size() + t.size()) > kInertNumbers) return;
    std::vector<InertShape> &shapes = inert_shapes();
    InertShape shape;
    shape.hash = shape_hash(v, t);
    shape.pairs = pairs;
    shape.vertices.resize(3 * v.size());
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t k = 0; k < 3; ++k) shape.vertices[3 * i + k] = v[i][k] - v[0][k];
    shape.triangles.resize(3 * t.size());
    for (size_t i = 0; i < t.size(); ++i)
        for (size_t k = 0; k < 3; ++k) shape.triangles[3 * i + k] = t[i][k];
    shapes.insert(shapes.begin(), std::move(shape));
    // Keep the list short and its memory bounded: drop from the end, the least recently used.
    size_t numbers = 0;
    for (size_t k = 0; k < shapes.size(); ++k) {
        numbers += shapes[k].vertices.size() + shapes[k].triangles.size();
        if (k >= kInertShapes || (k > 0 && numbers > kInertNumbersInAll)) {
            shapes.resize(k);
            break;
        }
    }
}

}  // namespace

void Arrangement::load(const std::vector<Operand> &operands) {
    KERFKITE_SOLID_PHASE(Load);
    plain_ = pool.plain = plain_paths();
    operands_ = operands;
    size_t total = 0;
    for (const Operand &op : operands) total += op.t->size();
    if (total > kMaxTriangles) throw ValueError("The solids of one operation have more than " + std::to_string(kMaxTriangles) + " triangles together");
    operand_hit_.assign(operands.size() + 1, 0);
    operand_crossed_.assign(operands.size() + 1, 0);
    own_pairs_.assign(operands.size() + 1, 0);
    inert_pairs_.assign(operands.size() + 1, -1);
    tris_.reserve(total);
    planes_.reserve(total);
    boxes_.reserve(total);
    std::vector<int> ids;
    spans_.assign(operands.size(), Span());
    for (size_t o = 0; o < operands.size(); ++o) {
        const Operand &op = operands[o];
        ids.resize(op.v->size());
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = pool.add_grid((*op.v)[i][0], (*op.v)[i][1], (*op.v)[i][2]);
        spans_[o].first = static_cast<int>(tris_.size());
        for (const Triangle &tri : *op.t) add_triangle(ids[tri[0]], ids[tri[1]], ids[tri[2]], static_cast<int>(o), op.label);
        close_span(spans_[o]);
    }
    real_triangles_ = tris_.size();
    size_lists();
}

// After the triangles of a span have been added: its extent.
void Arrangement::close_span(Span &span) {
    span.count = static_cast<int>(tris_.size()) - span.first;
    span.built = false;
    span.scans = 0;
    if (!span.count) return;
    span.box = boxes_[static_cast<size_t>(span.first)];
    for (int i = 1; i < span.count; ++i) span.box.include(boxes_[static_cast<size_t>(span.first + i)]);
}

// Calls visit(t) for every triangle t of the span whose box overlaps `box`, in no particular order.
// A tree answers that in a time that grows slowly with the size of the span, but building it costs about
// as much as looking at every box some tens of times, so the first few questions are answered by looking.
template <class F>
void Arrangement::overlapping(Span &span, const Box &box, F &&visit) {
    if (!span.count || !span.box.overlaps(box)) return;
    if (!span.built) {
        if (span.count <= 8 || span.scans < 12) {
            ++span.scans;
            for (int t = span.first; t < span.first + span.count; ++t)
                if (boxes_[static_cast<size_t>(t)].overlaps(box)) visit(t);
            return;
        }
        KERFKITE_SOLID_PHASE(Tree);
        span.tree.build(boxes_, static_cast<size_t>(span.first), static_cast<size_t>(span.count));
        span.built = true;
    }
    span.tree.query(boxes_, box, visit);
}

// The normal of a triangle's plane in doubles, and its length.
// Makes room in the lists kept for each triangle, after triangles have been added.
void Arrangement::size_lists() {
    const size_t old = plane_of_.size(), n = tris_.size();
    normal_.resize(n);
    normal_known_.resize(n, 0);
    met_of_.resize(n, -1);
    plane_of_.resize(n);
    for (size_t k = old; k < n; ++k) plane_of_[k] = static_cast<int>(k);
}

const double *Arrangement::normal(int tri) {
    const size_t t = static_cast<size_t>(tri);
    std::array<double, 5> &n = normal_[t];
    if (!normal_known_[t]) {
        n = plane_in_doubles(planes_[t]);
        normal_known_[t] = 1;
    }
    return n.data();
}

std::array<double, 5> Arrangement::plane_in_doubles(const Plane &pl) {
    std::array<double, 5> n;
    n[0] = pl.n[0].to_double();
    n[1] = pl.n[1].to_double();
    n[2] = pl.n[2].to_double();
    n[3] = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    n[4] = pl.d.to_double();
    return n;
}

// The side of a plane a grid point is on, from doubles, when they are enough.
//
// The value is n . p - d. Each of nx, ny, nz, d is within a relative 2^-51 of its integer (to_double:
// two rounded conversions and a rounded sum), and the coordinates of p are whole numbers below 2^53,
// exact as doubles. Each rounded product is then within 2^-51 + 2^-53 (and their product) of the true
// product, in proportion; each of the three rounded additions is within 2^-53 of its own result, and
// those results are at most M = |nx px| + |ny py| + |nz pz| + |d| in size, give or take the same small
// proportions. So the computed value is within M (2^-51 + 2^-53 + 3 * 2^-53) < M * 2^-50 of the true one.
// M is computed from the rounded products, which is within 2^-50 of itself of the true M; the bound used
// is M * 2^-49, twice what is needed. A value beyond it has the sign of the true value. Anything else is
// answered 2 and the caller computes the value with integers, as it always did. The components are below
// 2^78 and the coordinates below 2^38, so nothing overflows.
int Arrangement::side_in_doubles(const double plane[5], const int64_t *p) {
    const double x = plane[0] * static_cast<double>(p[0]), y = plane[1] * static_cast<double>(p[1]), z = plane[2] * static_cast<double>(p[2]);
    const double value = x + y + z - plane[4];
    const double bound = (std::fabs(x) + std::fabs(y) + std::fabs(z) + std::fabs(plane[4])) * 0x1p-49;
    return value > bound ? 1 : value < -bound ? -1 : 2;
}

void Arrangement::add_triangle(int v0, int v1, int v2, int operand, int label) {
    SoupTriangle s;
    s.v[0] = v0;
    s.v[1] = v1;
    s.v[2] = v2;
    s.operand = operand;
    s.label = label;
    if (v0 == v1 || v1 == v2 || v0 == v2) return;
    const int64_t *a = pool.pts[static_cast<size_t>(v0)].i, *b = pool.pts[static_cast<size_t>(v1)].i, *c = pool.pts[static_cast<size_t>(v2)].i;
    const Plane pl = plane_of(a, b, c);
    int axis = 0;
    if (pl.n[axis].abs() < pl.n[1].abs()) axis = 1;
    if (pl.n[axis].abs() < pl.n[2].abs()) axis = 2;
    if (pl.n[axis].zero()) return;  // no area
    Box box;
    for (int k = 0; k < 3; ++k) {
        box.lo[k] = std::min(a[k], std::min(b[k], c[k]));
        box.hi[k] = std::max(a[k], std::max(b[k], c[k]));
    }
    tris_.push_back(s);
    planes_.push_back(pl);
    boxes_.push_back(box);
    axis_.push_back(static_cast<signed char>(axis));
    sign_.push_back(static_cast<signed char>(pl.n[axis].sign()));
}

// The set of triangles found to lie in the plane of this one, by its lowest member so far.
int Arrangement::plane_set(int tri) const {
    while (plane_of_[static_cast<size_t>(tri)] != tri) {
        plane_of_[static_cast<size_t>(tri)] = plane_of_[static_cast<size_t>(plane_of_[static_cast<size_t>(tri)])];
        tri = plane_of_[static_cast<size_t>(tri)];
    }
    return tri;
}

bool Arrangement::planes_joined(int a, int b) const { return plane_set(a) == plane_set(b); }

void Arrangement::join_planes(int a, int b) const {
    const int ra = plane_set(a), rb = plane_set(b);
    if (ra != rb) plane_of_[static_cast<size_t>(std::max(ra, rb))] = std::min(ra, rb);
}

// False when the planes of two triangles are certainly not one: their unit normals, in doubles, differ in
// some component by more than a billionth both ways round. The normals of one plane are equal or opposite,
// and each computed component is within a few times 10^-15 of the true one (the normal's components are
// within 2^-51 of their integers in proportion, and the length is the rounded root of a rounded sum of
// three rounded squares), so a billionth is a margin of six decimal places. True says nothing: the caller
// then asks same_plane(), which is exact.
bool Arrangement::may_be_one_plane(int a, int b) {
    const double *na = normal(a), *nb = normal(b);
    bool same = true, opposite = true;
    for (int k = 0; k < 3; ++k) {
        const double x = na[k] / na[3], y = nb[k] / nb[3];
        same = same && std::fabs(x - y) <= 1e-9;
        opposite = opposite && std::fabs(x + y) <= 1e-9;
    }
    return same || opposite;
}

// Exactly: the three corners of one are in the plane of the other. Lying in one plane is an equivalence, so
// triangles found to share a plane are joined into a set, and two of one set need no computing again.
bool Arrangement::same_plane(int a, int b) const {
    if (a == b) return true;
    const int ra = plane_set(a), rb = plane_set(b);
    if (ra == rb) return true;
    const Plane &pl = planes_[static_cast<size_t>(ra)];
    const SoupTriangle &t = tris_[static_cast<size_t>(rb)];
    for (int k = 0; k < 3; ++k)
        if (!plane_value(pl, pool.pts[static_cast<size_t>(t.v[k])].i).zero()) return false;
    plane_of_[static_cast<size_t>(std::max(ra, rb))] = std::min(ra, rb);
    return true;
}

void Arrangement::record_simple() {
    for (size_t o = 0; o < operands_.size(); ++o) {
        if (operands_[o].simple || operand_hit_[o]) continue;
        if (operands_[o].mesh) operands_[o].mesh->simple = true;
        // Examined just now, and nothing came of it: remember the shape.
        if (remember_shapes_ && inert_pairs_[o] < 0 && !operand_crossed_[o]) remember_inert(*operands_[o].v, *operands_[o].t, own_pairs_[o]);
    }
}

std::size_t Arrangement::intersect(const std::vector<char> *flagged, bool stop_at_first) {
    KERFKITE_SOLID_PHASE(Candidates);
    if (probe_only_ && !stop_at_first) defect("a probing arrangement was asked to count");
    if (remember_shapes_ && !flagged) {
        // Operands of a shape examined before and found inert: their own pairs are counted and left out.
        for (size_t o = 0; o < operands_.size(); ++o) {
            if (operands_[o].simple) continue;
            inert_pairs_[o] = inert_pairs_of(*operands_[o].v, *operands_[o].t);
            if (inert_pairs_[o] >= 0) stats.candidate_pairs += static_cast<size_t>(inert_pairs_[o]);
        }
        if (stats.candidate_pairs > kMaxCandidatePairs) throw ValueError("The solids overlap in too many places for one operation; simplify them or combine fewer at once");
    }
    std::vector<int> found;
    size_t hits = 0;
    // Examines triangle i against the others; `fresh` marks the triangles being examined in this round, so
    // that a pair of them is taken once. Without it every triangle is examined, and a pair is taken from
    // its lower triangle.
    const auto examine = [&](int i, const std::vector<char> *fresh) {
        found.clear();
        const SoupTriangle &a = tris_[static_cast<size_t>(i)];
        const bool self = a.label == 2 || !(operands_[static_cast<size_t>(a.operand)].simple || inert_pairs_[static_cast<size_t>(a.operand)] >= 0);
        const Box &box = boxes_[static_cast<size_t>(i)];
        for (size_t s = 0; s < spans_.size(); ++s) {
            Span &span = spans_[s];
            // Triangles of one operand are not intersected with each other when it is known to be simple,
            // and without `fresh` nothing below i is wanted: spans are in the order of their triangles.
            if (static_cast<int>(s) == a.operand && !self) continue;
            if (!fresh && span.first + span.count <= i) continue;
            const size_t before = found.size();
            overlapping(span, box, [&](int j) {
                if (j == i) return;
                if (j < i && (!fresh || (*fresh)[static_cast<size_t>(j)])) return;  // seen from the other side
                found.push_back(j);
            });
            if (static_cast<int>(s) == a.operand) own_pairs_[s] += found.size() - before;
        }
        if (found.empty()) return false;
        // In order, so that points are made in an order that does not depend on how the trees are laid
        // out. A probing arrangement makes no lasting points and only wants to know whether any pair meets.
        if (!probe_only_) std::sort(found.begin(), found.end());
        stats.candidate_pairs += found.size();
        if (stats.candidate_pairs > kMaxCandidatePairs) throw ValueError("The solids overlap in too many places for one operation; simplify them or combine fewer at once");
        KERFKITE_SOLID_PHASE(Pairs);
        const Pool::Mark mark = pool.mark();
        for (const int j : found) {
            const bool meet = pair(std::min(i, j), std::max(i, j));
            if (probe_only_) pool.release(mark);
            if (!meet) continue;
            const SoupTriangle &b = tris_[static_cast<size_t>(j)];
            if (a.label == 2 || b.label == 2) continue;  // ribbons are not part of any solid
            ++hits;
            if (b.operand == a.operand) operand_hit_[static_cast<size_t>(a.operand)] = 1;
            if (stop_at_first) return true;
        }
        return false;
    };
    const int n = static_cast<int>(tris_.size());
    for (int i = 0; i < n; ++i) {
        if (flagged && !(*flagged)[static_cast<size_t>(i)]) continue;
        if (examine(i, flagged) && stop_at_first) return hits;
    }
    if (!ribbon_edges_.empty()) {
        spans_.push_back(Span());
        spans_.back().first = n;
        add_ribbons();
        close_span(spans_.back());
        size_lists();
        std::vector<char> fresh(tris_.size(), 0);
        for (size_t i = static_cast<size_t>(n); i < tris_.size(); ++i) fresh[i] = 1;
        for (int i = n; i < static_cast<int>(tris_.size()); ++i) examine(i, &fresh);
    }
    stats.intersecting_pairs += hits;
    return hits;
}

// Faces that would coincide but for the rounding of their vertices cross at a hair's angle and leave,
// between them, cracks and fins a fraction of a nanometre thick that are open at the rim: open, they
// are part of the space around them and cannot be told apart from it. A ribbon is a pair of thin
// triangles standing on an edge of such a face, 16 grid units to either side of it, across the gap. It
// belongs to no solid and changes no winding number; all it does is close the rim, so that the crack
// becomes a cell of its own, which absorb_thin_cells() can then give the state of its surroundings.
// Ribbons that turn out not to be needed lie inside one cell and leave no trace in the result.
// at_j holds the value of i's plane at the corners of j, and at_i the value of j's plane at the corners of i.
void Arrangement::near_coplanar(int i, int j, const I128 at_j[3], const I128 at_i[3]) {
    // Near when, for one of the two triangles, no corner of the other is further than kNear from its
    // plane: so not near as soon as each has a corner of the other beyond that.
    const double kNear = 8.0 * (1 << coarseness());
    for (int which = 0; which < 2; ++which) {
        const double length = normal(which ? j : i)[3];
        const I128 *value = which ? at_i : at_j;
        bool beyond = false;
        for (int k = 0; k < 3 && !beyond; ++k) beyond = std::fabs(value[k].to_double()) / length > kNear;
        if (!beyond) break;
        if (which == 1) return;
    }
    const int axis = axis_[static_cast<size_t>(i)];
    for (const int t : {i, j}) {
        const SoupTriangle &tri = tris_[static_cast<size_t>(t)];
        for (int k = 0; k < 3; ++k) ribbon_edges_.push_back(RibbonEdge{std::min(tri.v[k], tri.v[(k + 1) % 3]), std::max(tri.v[k], tri.v[(k + 1) % 3]), axis});
    }
}

void Arrangement::add_ribbons() {
    const int64_t kHalf = 16 * (int64_t(1) << coarseness());
    std::sort(ribbon_edges_.begin(), ribbon_edges_.end());
    ribbon_edges_.erase(std::unique(ribbon_edges_.begin(), ribbon_edges_.end()), ribbon_edges_.end());
    const int operand = static_cast<int>(operands_.size());
    for (const RibbonEdge &e : ribbon_edges_) {
        int64_t a[3], b[3];
        for (int k = 0; k < 3; ++k) {
            a[k] = pool.pts[static_cast<size_t>(e.a)].i[k];
            b[k] = pool.pts[static_cast<size_t>(e.b)].i[k];
        }
        if (a[(e.axis + 1) % 3] == b[(e.axis + 1) % 3] && a[(e.axis + 2) % 3] == b[(e.axis + 2) % 3]) continue;  // the edge runs along the axis
        int64_t up[3] = {0, 0, 0};
        up[e.axis] = kHalf;
        const int a_hi = pool.add_grid(a[0] + up[0], a[1] + up[1], a[2] + up[2]), a_lo = pool.add_grid(a[0] - up[0], a[1] - up[1], a[2] - up[2]);
        const int b_hi = pool.add_grid(b[0] + up[0], b[1] + up[1], b[2] + up[2]), b_lo = pool.add_grid(b[0] - up[0], b[1] - up[1], b[2] - up[2]);
        // Four triangles, so that the edge itself is an edge of the ribbon.
        add_triangle(e.a, e.b, b_hi, operand, 2);
        add_triangle(e.a, b_hi, a_hi, operand, 2);
        add_triangle(e.a, b_lo, e.b, operand, 2);
        add_triangle(e.a, a_lo, b_lo, operand, 2);
    }
    ribbon_edges_.clear();
}

bool Arrangement::corner(int tri, int p) const {
    const SoupTriangle &t = tris_[static_cast<size_t>(tri)];
    return t.v[0] == p || t.v[1] == p || t.v[2] == p;
}

// A grid point of the triangle's plane: inside the triangle or on its boundary.
bool Arrangement::inside_closed(int tri, int p) const {
    const SoupTriangle &t = tris_[static_cast<size_t>(tri)];
    const int au = (axis_[static_cast<size_t>(tri)] + 1) % 3, av = (axis_[static_cast<size_t>(tri)] + 2) % 3;
    const int64_t *a = pool.pts[static_cast<size_t>(t.v[0])].i, *b = pool.pts[static_cast<size_t>(t.v[1])].i, *c = pool.pts[static_cast<size_t>(t.v[2])].i,
                  *q = pool.pts[static_cast<size_t>(p)].i;
    const int s0 = orient2d_grid(a, b, q, au, av), s1 = orient2d_grid(b, c, q, au, av), s2 = orient2d_grid(c, a, q, au, av);
    return (s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0);
}

// Two triangles of one plane with no common vertex: whether they have any point in common. They have
// when a corner of one is in or on the other, or an edge of one crosses an edge of the other. Every one
// of those tests is the turn of an edge of one triangle towards a corner of the other, so the eighteen
// turns are taken once. Before that: when every corner of one triangle is strictly outside one edge of
// the other, the line of that edge separates them, no test can succeed, and the answer is no.
bool Arrangement::coplanar_overlap(int i, int j) const {
    if (plain_) return coplanar_overlap_plain(i, j);
    const SoupTriangle &ta = tris_[static_cast<size_t>(i)], &tb = tris_[static_cast<size_t>(j)];
    const int au = (axis_[static_cast<size_t>(i)] + 1) % 3, av = (axis_[static_cast<size_t>(i)] + 2) % 3;
    const int64_t *a[3] = {pool.pts[static_cast<size_t>(ta.v[0])].i, pool.pts[static_cast<size_t>(ta.v[1])].i, pool.pts[static_cast<size_t>(ta.v[2])].i};
    const int64_t *b[3] = {pool.pts[static_cast<size_t>(tb.v[0])].i, pool.pts[static_cast<size_t>(tb.v[1])].i, pool.pts[static_cast<size_t>(tb.v[2])].i};
    // from_a[x][y]: the turn of edge x of i (corner x to the next) towards corner y of j; from_b likewise.
    int from_a[3][3], from_b[3][3];
    const int turn_a = sign_[static_cast<size_t>(i)];  // of i itself in these axes
    for (int x = 0; x < 3; ++x) {
        for (int y = 0; y < 3; ++y) from_a[x][y] = orient2d_grid(a[x], a[(x + 1) % 3], b[y], au, av);
        if (from_a[x][0] == -turn_a && from_a[x][1] == -turn_a && from_a[x][2] == -turn_a) return false;
    }
    const int turn_b = orient2d_grid(b[0], b[1], b[2], au, av);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) from_b[y][x] = orient2d_grid(b[y], b[(y + 1) % 3], a[x], au, av);
        if (from_b[y][0] == -turn_b && from_b[y][1] == -turn_b && from_b[y][2] == -turn_b) return false;
    }
    const auto within = [](int s0, int s1, int s2) { return (s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0); };
    for (int k = 0; k < 3; ++k)
        if (within(from_a[0][k], from_a[1][k], from_a[2][k]) || within(from_b[0][k], from_b[1][k], from_b[2][k])) return true;
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            if (from_a[x][y] * from_a[x][(y + 1) % 3] < 0 && from_b[y][x] * from_b[y][(x + 1) % 3] < 0) return true;
    return false;
}

// The same answer by the tests as they are stated, one at a time: what coplanar_overlap() is checked against.
bool Arrangement::coplanar_overlap_plain(int i, int j) const {
    const SoupTriangle &ta = tris_[static_cast<size_t>(i)], &tb = tris_[static_cast<size_t>(j)];
    for (int k = 0; k < 3; ++k)
        if (inside_closed(i, tb.v[k]) || inside_closed(j, ta.v[k])) return true;
    const int au = (axis_[static_cast<size_t>(i)] + 1) % 3, av = (axis_[static_cast<size_t>(i)] + 2) % 3;
    for (int x = 0; x < 3; ++x) {
        const int64_t *a = pool.pts[static_cast<size_t>(ta.v[x])].i, *b = pool.pts[static_cast<size_t>(ta.v[(x + 1) % 3])].i;
        for (int y = 0; y < 3; ++y) {
            const int64_t *c = pool.pts[static_cast<size_t>(tb.v[y])].i, *d = pool.pts[static_cast<size_t>(tb.v[(y + 1) % 3])].i;
            if (orient2d_grid(a, b, c, au, av) * orient2d_grid(a, b, d, au, av) < 0 && orient2d_grid(c, d, a, au, av) * orient2d_grid(c, d, b, au, av) < 0)
                return true;
        }
    }
    return false;
}

// Two triangles of one plane with exactly one common vertex (index `shared` in i): whether they meet
// anywhere else. They do when the angles they span at the vertex overlap or share a side.
bool Arrangement::coplanar_fan_overlap(int i, int j, int shared) const {
    const SoupTriangle &ta = tris_[static_cast<size_t>(i)], &tb = tris_[static_cast<size_t>(j)];
    const int vertex = ta.v[shared];
    int sj = 0;
    while (tb.v[sj] != vertex) ++sj;
    const int au = (axis_[static_cast<size_t>(i)] + 1) % 3, av = (axis_[static_cast<size_t>(i)] + 2) % 3;
    const int64_t *v = pool.pts[static_cast<size_t>(vertex)].i;
    const int64_t *a = pool.pts[static_cast<size_t>(ta.v[(shared + 1) % 3])].i, *b = pool.pts[static_cast<size_t>(ta.v[(shared + 2) % 3])].i;
    const int64_t *c = pool.pts[static_cast<size_t>(tb.v[(sj + 1) % 3])].i, *d = pool.pts[static_cast<size_t>(tb.v[(sj + 2) % 3])].i;
    if (orient2d_grid(v, a, b, au, av) < 0) std::swap(a, b);
    if (orient2d_grid(v, c, d, au, av) < 0) std::swap(c, d);
    if (plain_) {
        const auto within = [&](const int64_t *from, const int64_t *to, const int64_t *x) {
            return orient2d_grid(v, from, x, au, av) >= 0 && orient2d_grid(v, x, to, au, av) >= 0;
        };
        return within(a, b, c) || within(a, b, d) || within(c, d, a) || within(c, d, b);
    }
    // "x within the angle from..to" is two turns about v, and the eight turns of the four questions are
    // four turns and their opposites: the turn of v, c, a is minus that of v, a, c, exactly.
    const int ac = orient2d_grid(v, a, c, au, av), cb = orient2d_grid(v, c, b, au, av);
    if (ac >= 0 && cb >= 0) return true;  // c within a..b
    const int ad = orient2d_grid(v, a, d, au, av), db = orient2d_grid(v, d, b, au, av);
    return (ad >= 0 && db >= 0) || (ac <= 0 && ad >= 0) || (cb >= 0 && db <= 0);  // d within a..b, a within c..d, b within c..d
}

size_t Arrangement::crossing_slot(uint64_t edge, int64_t bucket) const {
    uint64_t h = (edge ^ (static_cast<uint64_t>(bucket) * 0x9E3779B97F4A7C15ULL)) * 0xD6E8FEB86659FD93ULL;
    h ^= h >> 32;
    return static_cast<size_t>(h) & (crossing_table_.size() - 1);
}

// Where the edge between grid points p and q crosses a plane that has the values at_p and at_q at them
// (of opposite signs): Pool::line_plane(), which makes the point or finds the point of that value the
// pool already has.
//
// One edge is asked about many times: by every triangle that has it, against every triangle of a face it
// passes through. The point is the same whenever the two values are in the same ratio, because it divides
// the edge in that ratio; and the pool holds each point once, so line_plane() would compute the point
// again only to find it. Here the answer is remembered by the edge and the ratio. The ratios are compared
// exactly, as products of integers; the table is searched by a rounded ratio, which only decides where
// to look. Two values of one exact ratio r are computed as doubles within r * 2^-49 of it (two rounded
// conversions of a 128-bit integer, each within 2^-51, and one rounded division), so they are within
// 2^7 units in the last place of each other and their bit patterns, shifted down by 14, differ by one at
// most: looking in a bucket and its two neighbours finds every equal ratio. Missing one would cost only
// time, since line_plane() is then asked, as it always was.
//
// When the arrangement only answers whether any pair meets (probe_only), none of this is needed: the
// answer of pair() depends on its points through their values alone. It compares them along a line, by
// value; and it asks whether a point is a corner of a triangle, by id, where a crossing is never a
// corner of the triangle whose edge it lies inside, so that a pair is taken not to meet only at a corner
// both triangles name, a grid point with one id. So each crossing is a point of its own, dropped again
// after the pair.
int Arrangement::crossing(int p, int q, const I128 &at_p, const I128 &at_q) {
    if (probe_only_) return pool.line_plane_loose(p, q, at_p, at_q);
    if (plain_) return pool.line_plane_at_once(p, q, at_p, at_q);
    const bool swap = q < p;
    const int low_id = swap ? q : p, high_id = swap ? p : q;
    const I128 low = (swap ? at_q : at_p).abs(), high = (swap ? at_p : at_q).abs();
    const uint64_t edge = (static_cast<uint64_t>(static_cast<uint32_t>(low_id)) << 32) | static_cast<uint32_t>(high_id);
    const double ratio = low.to_double() / high.to_double();
    uint64_t bits;
    std::memcpy(&bits, &ratio, sizeof bits);
    const int64_t bucket = static_cast<int64_t>(bits >> 14);
    if (!crossing_table_.empty()) {
        const size_t mask = crossing_table_.size() - 1;
        for (int step = 0; step < 3; ++step) {
            const int64_t near = step == 0 ? bucket : step == 1 ? bucket - 1 : bucket + 1;
            for (size_t slot = crossing_slot(edge, near); crossing_table_[slot].index; slot = (slot + 1) & mask) {
                const CrossingSlot &at = crossing_table_[slot];
                if (at.edge != edge || at.bucket != near) continue;
                const Crossing &c = crossings_[static_cast<size_t>(at.index) - 1];
                // low / high == c.low / c.high, all four positive.
                if ((c.low == low && c.high == high) || U256::product(low, c.high) == U256::product(c.low, high)) return c.point;
            }
        }
    }
    const int point = pool.line_plane(p, q, at_p, at_q);
    if (2 * (crossings_.size() + 1) > crossing_table_.size()) {
        crossing_table_.assign(crossing_table_.empty() ? 1024 : 2 * crossing_table_.size(), CrossingSlot{0, 0, 0});
        const size_t mask = crossing_table_.size() - 1;
        for (size_t k = 0; k < crossings_.size(); ++k) {
            size_t slot = crossing_slot(crossings_[k].edge, crossings_[k].bucket);
            while (crossing_table_[slot].index) slot = (slot + 1) & mask;
            crossing_table_[slot] = CrossingSlot{crossings_[k].edge, crossings_[k].bucket, static_cast<int>(k) + 1};
        }
    }
    crossings_.push_back(Crossing{edge, bucket, low, high, point});
    const size_t mask = crossing_table_.size() - 1;
    size_t slot = crossing_slot(edge, bucket);
    while (crossing_table_[slot].index) slot = (slot + 1) & mask;
    crossing_table_[slot] = CrossingSlot{edge, bucket, static_cast<int>(crossings_.size())};
    return point;
}

// Where a triangle meets the plane of another, given the value of that plane at each of its vertices and
// its sign: one or two points.
int Arrangement::ends(int tri, const int side[3], const I128 value[3], int out[2]) {
    const SoupTriangle &t = tris_[static_cast<size_t>(tri)];
    int n = 0;
    for (int k = 0; k < 3; ++k)
        if (side[k] == 0) {
            if (n == 2) defect("three points of a triangle on a plane it is not in");
            out[n++] = t.v[k];
        }
    for (int k = 0; k < 3; ++k) {
        const int m = (k + 1) % 3;
        if (side[k] * side[m] < 0) {
            if (n == 2) defect("a triangle crosses a plane in three places");
            out[n++] = crossing(t.v[k], t.v[m], value[k], value[m]);
        }
    }
    if (n == 0) defect("a triangle that does not reach the plane");
    return n;
}

// ends() of a triangle in the plane of triangle `plane`, remembered for the two planes it was last asked
// about. A triangle is often asked about one plane many times over, once for each triangle of a flat face
// it passes through; the points depend on the plane alone, and same_plane() says exactly whether a
// remembered plane is this one. ends() would find the same points again (each is held once, by crossing()
// and by the pool) and change nothing in doing so. A triangle that never gets the same plane twice stops
// being remembered.
int Arrangement::ends_in_plane(int tri, int plane, const int side[3], const I128 value[3], int out[2]) {
    if (probe_only_ || plain_) return ends(tri, side, value, out);
    int &slot = met_of_[static_cast<size_t>(tri)];
    if (slot < 0) {
        slot = static_cast<int>(met_.size());
        met_.push_back(Met{{-1, -1}, {0, 0}, {{0, 0}, {0, 0}}, 0});
    }
    Met &met = met_[static_cast<size_t>(slot)];
    if (met.misses >= 8) return ends(tri, side, value, out);
    for (int e = 0; e < 2; ++e)
        if (met.plane[e] >= 0 && (planes_joined(met.plane[e], plane) || (may_be_one_plane(met.plane[e], plane) && same_plane(met.plane[e], plane)))) {
            out[0] = met.point[e][0];
            out[1] = met.point[e][1];
            met.misses = -(1 << 30);  // it pays for this triangle: keep remembering
            return met.count[e];
        }
    const int count = ends(tri, side, value, out);
    if (met.plane[0] >= 0) ++met.misses;
    met.plane[1] = met.plane[0];
    met.count[1] = met.count[0];
    met.point[1][0] = met.point[0][0];
    met.point[1][1] = met.point[0][1];
    met.plane[0] = plane;
    met.count[0] = count;
    met.point[0][0] = out[0];
    met.point[0][1] = count > 1 ? out[1] : out[0];
    return count;
}

bool Arrangement::report_point(int i, int j, int p) {
    const bool ci = corner(i, p), cj = corner(j, p);
    if (!ci) records_.push_back(Record{i, p, p, j});
    if (!cj) records_.push_back(Record{j, p, p, i});
    return !(ci && cj);
}

bool Arrangement::report_segment(int i, int j, int p, int q) {
    const bool ci = corner(i, p) && corner(i, q), cj = corner(j, p) && corner(j, q);
    if (!ci) records_.push_back(Record{i, p, q, j});
    if (!cj) records_.push_back(Record{j, p, q, i});
    return !(ci && cj);
}

int Arrangement::cluster_find(int i) {
    if (cluster_[static_cast<size_t>(i)] < 0) cluster_[static_cast<size_t>(i)] = i;
    while (cluster_[static_cast<size_t>(i)] != i) {
        const int up = cluster_[static_cast<size_t>(i)];
        cluster_[static_cast<size_t>(i)] = cluster_[static_cast<size_t>(up)];
        i = up;
    }
    return i;
}

void Arrangement::cluster_join(int i, int j) {
    if (cluster_.size() < tris_.size()) cluster_.resize(tris_.size(), -1);
    const int a = cluster_find(i), b = cluster_find(j);
    if (a != b) cluster_[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
}

// Intersects triangles i < j. True when they meet anywhere other than at a common corner or along a
// common edge.
bool Arrangement::pair(int i, int j) {
    const SoupTriangle &ta = tris_[static_cast<size_t>(i)], &tb = tris_[static_cast<size_t>(j)];
    int shared = 0, ia = -1, ib = -1;
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            if (ta.v[x] == tb.v[y]) {
                ++shared;
                ia = x;
                ib = y;
            }
    if (shared == 3) return false;
    const auto grid = [&](int id) { return pool.pts[static_cast<size_t>(id)].i; };
    if (shared == 2) {
        int pa = 0, pb = 0;
        while (corner(j, ta.v[pa])) ++pa;
        while (corner(i, tb.v[pb])) ++pb;
        const int64_t *u = grid(ta.v[(pa + 1) % 3]), *v = grid(ta.v[(pa + 2) % 3]), *p = grid(ta.v[pa]), *q = grid(tb.v[pb]);
        if (orient3d_grid(u, v, p, q) != 0) return false;  // a fold along the edge: they share the edge only
        const int au = (axis_[static_cast<size_t>(i)] + 1) % 3, av = (axis_[static_cast<size_t>(i)] + 2) % 3;
        if (orient2d_grid(u, v, p, au, av) != orient2d_grid(u, v, q, au, av)) return false;  // side by side
        cluster_join(i, j);
        return true;
    }
    // Most pairs end here: one triangle lies wholly on one side of the other's plane. Where doubles are
    // certain that every corner of one (but a common corner) is on the same side of the other's plane, that
    // is the answer; otherwise the values are computed with integers, and the same is asked of them.
    const auto certainly_apart = [&](int plane, const SoupTriangle &other, int common) {
        const double *pl = normal(plane);
        int side = 0;
        for (int k = 0; k < 3; ++k) {
            if (k == common) continue;
            const int s = side_in_doubles(pl, grid(other.v[k]));
            if (s == 2 || (side && s != side)) return false;
            side = s;
        }
        return true;
    };
    // The value of each plane at the corners of the other triangle. A common corner lies in both planes.
    int side_b[3] = {0, 0, 0}, side_a[3] = {0, 0, 0};
    I128 value_b[3], value_a[3];
    // Two triangles already found to lie in one plane have every one of those values zero, and so have two
    // found to here: the corners of j all in the plane of i put i in the plane of j as well.
    bool one_plane = !plain_ && planes_joined(i, j);
    if (!one_plane) {
        if (!plain_ && certainly_apart(i, tb, shared == 1 ? ib : -1)) return false;
        for (int k = 0; k < 3; ++k) {
            if (!(shared == 1 && k == ib)) value_b[k] = plane_value(planes_[static_cast<size_t>(i)], grid(tb.v[k]));
            side_b[k] = value_b[k].sign();
        }
        if (shared == 0) {
            if (side_b[0] == side_b[1] && side_b[1] == side_b[2] && side_b[0] != 0) return false;
        } else if (side_b[(ib + 1) % 3] == side_b[(ib + 2) % 3] && side_b[(ib + 1) % 3] != 0) {
            return false;
        }
        one_plane = side_b[0] == 0 && side_b[1] == 0 && side_b[2] == 0;
        if (one_plane && !plain_) join_planes(i, j);
    }
    if (!one_plane || plain_) {
        if (!plain_ && certainly_apart(j, ta, shared == 1 ? ia : -1)) return false;
        for (int k = 0; k < 3; ++k) {
            if (!(shared == 1 && k == ia)) value_a[k] = plane_value(planes_[static_cast<size_t>(j)], grid(ta.v[k]));
            side_a[k] = value_a[k].sign();
        }
        if (shared == 0) {
            if (side_a[0] == side_a[1] && side_a[1] == side_a[2] && side_a[0] != 0) return false;
        } else if (side_a[(ia + 1) % 3] == side_a[(ia + 2) % 3] && side_a[(ia + 1) % 3] != 0) {
            return false;
        }
    }
    if (one_plane) {
        if (!(shared == 1 ? coplanar_fan_overlap(i, j, ia) : coplanar_overlap(i, j))) return false;
        cluster_join(i, j);
        return true;
    }
    if (ribbons_enabled_ && ta.label != 2 && tb.label != 2 && ta.operand != tb.operand) near_coplanar(i, j, value_b, value_a);
    // Where an edge of one passes through the plane of the other a point is asked for: an operand whose own
    // triangles do that is not inert (see InertShape).
    if (ta.operand == tb.operand) {
        const auto passes = [](const int side[3]) { return side[0] * side[1] < 0 || side[1] * side[2] < 0 || side[0] * side[2] < 0; };
        if (passes(side_b) || passes(side_a)) operand_crossed_[static_cast<size_t>(ta.operand)] = 1;
    }
    if (probe_only_) {
        // A triangle with one corner in the other's plane and the other two on one side of it touches the
        // plane at that corner only: ends() would give that corner alone. The pair is then decided below
        // from corners, and the crossings of the other triangle, which ends() would make first, are not
        // looked at; when nothing is kept they need not be made.
        const auto lone = [](const int side[3]) {
            for (int k = 0; k < 3; ++k)
                if (side[k] == 0 && side[(k + 1) % 3] * side[(k + 2) % 3] > 0) return k;
            return -1;
        };
        const int kj = lone(side_b), ki = lone(side_a);
        if (kj >= 0 || ki >= 0) {
            const int p = kj >= 0 ? tb.v[kj] : ta.v[ki];
            if (kj >= 0 && ki >= 0 && tb.v[kj] != ta.v[ki]) return false;
            if (!inside_closed(kj >= 0 ? i : j, p)) return false;
            return report_point(i, j, p);
        }
    }
    int ei[2], ej[2];
    const int nj = ends_in_plane(j, i, side_b, value_b, ej);
    const int ni = ends_in_plane(i, j, side_a, value_a, ei);
    if (nj == 1 || ni == 1) {
        // One triangle only touches the other's plane, at a vertex.
        const int p = nj == 1 ? ej[0] : ei[0];
        if (nj == 1 && ni == 1 && ej[0] != ei[0]) return false;
        if (!inside_closed(nj == 1 ? i : j, p)) return false;
        return report_point(i, j, p);
    }
    // Both meet the common line in a segment: the overlap of the two, by position along the line.
    // The coordinate in which the common line of the two planes runs fastest, from the cross product of
    // the normals in doubles: any coordinate in which the line runs at all would do, and the largest of
    // three estimates accurate to a part in 10^15 is certainly not zero.
    int axis = 0;
    {
        const double *a = normal(i), *b = normal(j);
        const double line[3] = {std::fabs(a[1] * b[2] - a[2] * b[1]), std::fabs(a[2] * b[0] - a[0] * b[2]), std::fabs(a[0] * b[1] - a[1] * b[0])};
        if (line[1] > line[axis]) axis = 1;
        if (line[2] > line[axis]) axis = 2;
    }
    if (pool.compare(ej[0], ej[1], axis) > 0) std::swap(ej[0], ej[1]);
    if (pool.compare(ei[0], ei[1], axis) > 0) std::swap(ei[0], ei[1]);
    const int lo = pool.compare(ei[0], ej[0], axis) >= 0 ? ei[0] : ej[0];
    const int hi = pool.compare(ei[1], ej[1], axis) <= 0 ? ei[1] : ej[1];
    const int c = pool.compare(lo, hi, axis);
    if (c > 0) return false;
    if (c == 0) return report_point(i, j, lo);
    return report_segment(i, j, lo, hi);
}

void Arrangement::subdivide_one(Planar &planar, int tri, size_t first, size_t last) {
    const SoupTriangle &t = tris_[static_cast<size_t>(tri)];
    const int axis = axis_[static_cast<size_t>(tri)];
    planar.reset(planes_[static_cast<size_t>(tri)], (axis + 1) % 3, (axis + 2) % 3, sign_[static_cast<size_t>(tri)]);
    for (int k = 0; k < 3; ++k) {
        const int a = t.v[k], b = t.v[(k + 1) % 3];
        planar.points.push_back(a);
        planar.supports.push_back(plane_through_edge(pool.pts[static_cast<size_t>(a)].i, pool.pts[static_cast<size_t>(b)].i, axis));
        planar.segments.push_back(PlanarSegment{a, b, k, -1});
    }
    for (size_t r = first; r < last; ++r) {
        const Record &record = records_[r];
        planar.points.push_back(record.a);
        if (record.a == record.b) continue;
        planar.points.push_back(record.b);
        planar.supports.push_back(planes_[static_cast<size_t>(record.other)]);
        planar.segments.push_back(PlanarSegment{record.a, record.b, static_cast<int>(planar.supports.size()) - 1, -1});
    }
    planar.split();
    planar.triangulate(t.v[0], t.v[1], t.v[2]);
    for (const std::array<int, 3> &f : planar.triangles) instances_.push_back(Instance{{f[0], f[1], f[2]}, tri});
    ++stats.split_triangles;
}

void Arrangement::subdivide_cluster(Planar &planar, const std::vector<int> &members, const std::vector<std::pair<size_t, size_t>> &ranges) {
    const int lead = members[0];
    const int axis = axis_[static_cast<size_t>(lead)], au = (axis + 1) % 3, av = (axis + 2) % 3;
    planar.reset(planes_[static_cast<size_t>(lead)], au, av, sign_[static_cast<size_t>(lead)]);
    std::vector<std::pair<int, int>> edges;
    for (const int m : members) {
        const SoupTriangle &t = tris_[static_cast<size_t>(m)];
        for (int k = 0; k < 3; ++k) edges.emplace_back(std::min(t.v[k], t.v[(k + 1) % 3]), std::max(t.v[k], t.v[(k + 1) % 3]));
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (size_t k = 0; k < edges.size(); ++k) {
        const std::pair<int, int> &e = edges[k];
        planar.points.push_back(e.first);
        planar.points.push_back(e.second);
        planar.supports.push_back(plane_through_edge(pool.pts[static_cast<size_t>(e.first)].i, pool.pts[static_cast<size_t>(e.second)].i, axis));
        planar.segments.push_back(PlanarSegment{e.first, e.second, static_cast<int>(planar.supports.size()) - 1, static_cast<int>(k)});
    }
    for (const int m : members) {
        for (size_t r = ranges[static_cast<size_t>(m)].first; r < ranges[static_cast<size_t>(m)].second; ++r) {
            const Record &record = records_[r];
            planar.points.push_back(record.a);
            if (record.a == record.b) continue;
            planar.points.push_back(record.b);
            planar.supports.push_back(planes_[static_cast<size_t>(record.other)]);
            planar.segments.push_back(PlanarSegment{record.a, record.b, static_cast<int>(planar.supports.size()) - 1, -1});
        }
    }
    planar.split();
    planar.triangulate_around();
    // Each piece lies wholly inside or wholly outside each member, because the members' edges are edges
    // of the triangulation. Which members it is inside changes exactly when an edge of a member is crossed,
    // so the answer spreads from the outside (inside none) with no further geometry.
    struct EdgeOwner {
        int a, b, member;  // a < b; member indexes `members`
    };
    std::vector<EdgeOwner> owners;
    for (const PlanarSegment &piece : planar.pieces) {
        if (piece.source < 0) continue;
        // The members that have the edge this piece is part of.
        const std::pair<int, int> &e = edges[static_cast<size_t>(piece.source)];
        for (size_t m = 0; m < members.size(); ++m) {
            const SoupTriangle &t = tris_[static_cast<size_t>(members[m])];
            for (int k = 0; k < 3; ++k)
                if (std::min(t.v[k], t.v[(k + 1) % 3]) == e.first && std::max(t.v[k], t.v[(k + 1) % 3]) == e.second)
                    owners.push_back(EdgeOwner{std::min(piece.a, piece.b), std::max(piece.a, piece.b), static_cast<int>(m)});
        }
    }
    std::sort(owners.begin(), owners.end(), [](const EdgeOwner &x, const EdgeOwner &y) { return x.a != y.a ? x.a < y.a : x.b != y.b ? x.b < y.b : x.member < y.member; });
    owners.erase(std::unique(owners.begin(), owners.end(), [](const EdgeOwner &x, const EdgeOwner &y) { return x.a == y.a && x.b == y.b && x.member == y.member; }), owners.end());
    std::vector<int> turn(members.size());
    for (size_t m = 0; m < members.size(); ++m) {
        const SoupTriangle &t = tris_[static_cast<size_t>(members[m])];
        turn[m] = planar.orient(t.v[0], t.v[1], t.v[2]);
    }
    const size_t count = planar.triangles.size();
    std::vector<std::vector<int>> inside(count);
    std::vector<char> seen(count, 0);
    std::vector<int> queue, toggled;
    for (size_t f = 0; f < count; ++f) {
        const std::array<int, 3> &tri = planar.triangles[f];
        bool outer = false;
        for (int k = 0; k < 3; ++k) outer = outer || tri[static_cast<size_t>(k)] == planar.root[0] || tri[static_cast<size_t>(k)] == planar.root[1] || tri[static_cast<size_t>(k)] == planar.root[2];
        if (outer) {
            seen[f] = 1;
            queue.push_back(static_cast<int>(f));
        }
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        const size_t f = static_cast<size_t>(queue[head]);
        for (size_t k = 0; k < 3; ++k) {
            const int next = planar.neighbours[f][k];
            if (next < 0 || seen[static_cast<size_t>(next)]) continue;
            const int a = std::min(planar.triangles[f][k], planar.triangles[f][(k + 1) % 3]), b = std::max(planar.triangles[f][k], planar.triangles[f][(k + 1) % 3]);
            toggled.clear();
            auto it = std::lower_bound(owners.begin(), owners.end(), EdgeOwner{a, b, -1},
                                       [](const EdgeOwner &x, const EdgeOwner &y) { return x.a != y.a ? x.a < y.a : x.b != y.b ? x.b < y.b : x.member < y.member; });
            for (; it != owners.end() && it->a == a && it->b == b; ++it) toggled.push_back(it->member);
            // The symmetric difference of two ascending lists.
            std::vector<int> &out = inside[static_cast<size_t>(next)];
            const std::vector<int> &from = inside[f];
            size_t x = 0, y = 0;
            while (x < from.size() || y < toggled.size()) {
                if (y == toggled.size() || (x < from.size() && from[x] < toggled[y])) out.push_back(from[x++]);
                else if (x == from.size() || toggled[y] < from[x]) out.push_back(toggled[y++]);
                else {
                    ++x;
                    ++y;
                }
            }
            seen[static_cast<size_t>(next)] = 1;
            queue.push_back(next);
        }
    }
    for (size_t f = 0; f < count; ++f) {
        const std::array<int, 3> &tri = planar.triangles[f];
        for (const int m : inside[f]) {
            if (turn[static_cast<size_t>(m)] > 0) instances_.push_back(Instance{{tri[0], tri[1], tri[2]}, members[static_cast<size_t>(m)]});
            else instances_.push_back(Instance{{tri[0], tri[2], tri[1]}, members[static_cast<size_t>(m)]});
        }
    }
    ++stats.coplanar_groups;
    stats.split_triangles += members.size();
}

void Arrangement::subdivide() {
    KERFKITE_SOLID_PHASE(Subdivide);
    if (probe_only_) defect("a probing arrangement was asked to resolve");
    // From here on points are read directly: those that take part are computed.
    for (const Record &record : records_) {
        pool.settle(record.a);
        pool.settle(record.b);
    }
    std::stable_sort(records_.begin(), records_.end(), [](const Record &x, const Record &y) { return x.tri < y.tri; });
    const size_t n = tris_.size();
    std::vector<std::pair<size_t, size_t>> ranges(n, std::make_pair(size_t(0), size_t(0)));
    for (size_t r = 0; r < records_.size();) {
        size_t e = r;
        while (e < records_.size() && records_[e].tri == records_[r].tri) ++e;
        ranges[static_cast<size_t>(records_[r].tri)] = std::make_pair(r, e);
        r = e;
    }
    instances_.reserve(n + 4 * records_.size());
    // One triangulation object for every triangle and group: it keeps its working memory between them.
    Planar planar(pool, Plane(), 0, 1, 1);
    planar.plain = plain_;
    std::vector<std::pair<int, int>> grouped;
    for (size_t i = 0; i < n; ++i) {
        if (i < cluster_.size() && cluster_[i] >= 0) {
            grouped.emplace_back(cluster_find(static_cast<int>(i)), static_cast<int>(i));
            continue;
        }
        if (ranges[i].first == ranges[i].second) {
            const SoupTriangle &t = tris_[i];
            instances_.push_back(Instance{{t.v[0], t.v[1], t.v[2]}, static_cast<int>(i)});
        } else {
            subdivide_one(planar, static_cast<int>(i), ranges[i].first, ranges[i].second);
        }
    }
    std::sort(grouped.begin(), grouped.end());
    std::vector<int> members;
    for (size_t g = 0; g < grouped.size();) {
        members.clear();
        size_t e = g;
        while (e < grouped.size() && grouped[e].first == grouped[g].first) members.push_back(grouped[e++].second);
        subdivide_cluster(planar, members, ranges);
        g = e;
    }
    stats.constructed_points = pool.rational_points();
}

void Arrangement::build_faces() {
    KERFKITE_SOLID_PHASE(Faces);
    struct Keyed {
        int a, b, c, sign, tri;
    };
    std::vector<Keyed> keyed(instances_.size());
    for (size_t i = 0; i < instances_.size(); ++i) {
        const Instance &in = instances_[i];
        int v[3] = {in.v[0], in.v[1], in.v[2]};
        if (v[1] < v[0] && v[1] < v[2]) {
            const int t = v[0];
            v[0] = v[1];
            v[1] = v[2];
            v[2] = t;
        } else if (v[2] < v[0] && v[2] < v[1]) {
            const int t = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t;
        }
        keyed[i] = v[1] < v[2] ? Keyed{v[0], v[1], v[2], 1, in.tri} : Keyed{v[0], v[2], v[1], -1, in.tri};
    }
    // By a, b, c, then the triangle and the sign.
    {
        std::vector<size_t> starts;
        std::vector<Keyed> scratch;
        sort_by_key(
            keyed, pool.pts.size(), [](const Keyed &x) { return static_cast<size_t>(x.a); },
            [](const Keyed &x, const Keyed &y) {
                if (x.b != y.b) return x.b < y.b;
                if (x.c != y.c) return x.c < y.c;
                if (x.tri != y.tri) return x.tri < y.tri;
                return x.sign < y.sign;
            },
            starts, scratch);
    }
    faces_.clear();
    for (size_t i = 0; i < keyed.size();) {
        Face face;
        face.v[0] = keyed[i].a;
        face.v[1] = keyed[i].b;
        face.v[2] = keyed[i].c;
        face.jump[0] = face.jump[1] = 0;
        face.parent = keyed[i].tri;
        face.parent_sign = keyed[i].sign;
        face.separator = false;
        size_t e = i;
        while (e < keyed.size() && keyed[e].a == keyed[i].a && keyed[e].b == keyed[i].b && keyed[e].c == keyed[i].c) {
            const int label = tris_[static_cast<size_t>(keyed[e].tri)].label;
            if (label == 2) face.separator = true;
            else face.jump[label] += keyed[e].sign;
            ++e;
        }
        if (face.jump[0] || face.jump[1] || face.separator) faces_.push_back(face);
        i = e;
    }
}

uint32_t Arrangement::side_find(uint32_t s) {
    while (side_parent_[s] != s) {
        side_parent_[s] = side_parent_[side_parent_[s]];
        s = side_parent_[s];
    }
    return s;
}

void Arrangement::build_cells() {
    KERFKITE_SOLID_PHASE(Cells);
    const size_t nf = faces_.size();
    uses_.clear();
    uses_.reserve(3 * nf);
    for (size_t f = 0; f < nf; ++f) {
        const Face &face = faces_[f];
        const int fi = static_cast<int>(f);
        uses_.push_back(EdgeUse{face.v[0], face.v[1], fi, face.v[2], 1, 0, 1});
        uses_.push_back(EdgeUse{face.v[1], face.v[2], fi, face.v[0], 1, 1, 2});
        uses_.push_back(EdgeUse{face.v[0], face.v[2], fi, face.v[1], 0, 0, 2});
    }
    // By a, b, then the face.
    {
        std::vector<size_t> starts;
        std::vector<EdgeUse> scratch;
        sort_by_key(
            uses_, pool.pts.size(), [](const EdgeUse &x) { return static_cast<size_t>(x.a); },
            [](const EdgeUse &x, const EdgeUse &y) { return x.b != y.b ? x.b < y.b : x.face < y.face; }, starts, scratch);
    }
    side_parent_.resize(2 * nf);
    std::iota(side_parent_.begin(), side_parent_.end(), 0u);
    const auto unite = [&](uint32_t x, uint32_t y) {
        x = side_find(x);
        y = side_find(y);
        if (x != y) side_parent_[std::max(x, y)] = std::min(x, y);
    };
    // Counter-clockwise about a -> b, a face that runs a -> b shows its front; the others their back.
    const auto ahead = [&](const EdgeUse &u) { return static_cast<uint32_t>(2 * u.face + (u.forward ? 0 : 1)); };
    const auto behind = [&](const EdgeUse &u) { return static_cast<uint32_t>(2 * u.face + (u.forward ? 1 : 0)); };
    groups_.clear();
    radial_.clear();
    std::vector<int> third, order, parent;
    const PoolPredicates predicates{&pool, this, plain_ ? nullptr : &parent};
    for (size_t i = 0; i < uses_.size();) {
        size_t e = i;
        while (e < uses_.size() && uses_[e].a == uses_[i].a && uses_[e].b == uses_[i].b) ++e;
        const size_t count = e - i;
        Group group{i, count, -1};
        if (count == 1) {
            // A free edge: the space goes round it. Proper for a ribbon; for a face of a solid it means the
            // surface is open, which the winding numbers will show.
            unite(ahead(uses_[i]), behind(uses_[i]));
        } else if (count == 2) {
            unite(ahead(uses_[i]), behind(uses_[i + 1]));
            unite(ahead(uses_[i + 1]), behind(uses_[i]));
        } else {
            third.resize(count);
            order.resize(count);
            parent.resize(count);
            for (size_t k = 0; k < count; ++k) {
                third[k] = uses_[i + k].third;
                order[k] = static_cast<int>(k);
                parent[k] = faces_[static_cast<size_t>(uses_[i + k].face)].parent;
            }
            if (!radial_sort(predicates, uses_[i].a, uses_[i].b, third, order)) defect("two faces in one half-plane at an edge");
            group.sorted = static_cast<long>(radial_.size());
            for (size_t k = 0; k < count; ++k) radial_.push_back(static_cast<int>(i) + order[k]);
            for (size_t k = 0; k < count; ++k)
                unite(ahead(uses_[static_cast<size_t>(radial_[static_cast<size_t>(group.sorted) + k])]),
                      behind(uses_[static_cast<size_t>(radial_[static_cast<size_t>(group.sorted) + (k + 1) % count])]));
        }
        groups_.push_back(group);
        i = e;
    }
}

// The sign of g . ((a - q) x (b - q)): which way the line through q along g passes the edge a -> b.
int Arrangement::ray_side(int q, const int64_t g[3], const int64_t *a, const int64_t *b) const {
    const Pt &point = pool.pts[static_cast<size_t>(q)];
    const Interval ax = Interval(static_cast<double>(a[0])) - point.c[0], ay = Interval(static_cast<double>(a[1])) - point.c[1],
                   az = Interval(static_cast<double>(a[2])) - point.c[2];
    const Interval bx = Interval(static_cast<double>(b[0])) - point.c[0], by = Interval(static_cast<double>(b[1])) - point.c[1],
                   bz = Interval(static_cast<double>(b[2])) - point.c[2];
    const Interval value = Interval(static_cast<double>(g[0])) * (ay * bz - az * by) + Interval(static_cast<double>(g[1])) * (az * bx - ax * bz) +
                           Interval(static_cast<double>(g[2])) * (ax * by - ay * bx);
    const int s = value.sign();
    if (s != 2) return s;
    // Exactly: g . (a x b) + q . ((a - b) x g), which is linear in q.
    const I128 cross[3] = {I128::mul(a[1], b[2]) - I128::mul(a[2], b[1]), I128::mul(a[2], b[0]) - I128::mul(a[0], b[2]),
                           I128::mul(a[0], b[1]) - I128::mul(a[1], b[0])};
    const I128 constant = cross[0].times(g[0]) + cross[1].times(g[1]) + cross[2].times(g[2]);
    const int64_t d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    const int64_t k[3] = {d[1] * g[2] - d[2] * g[1], d[2] * g[0] - d[0] * g[2], d[0] * g[1] - d[1] * g[0]};
    if (point.rat < 0)
        return (BigInt(constant) + BigInt(I128::mul(point.i[0], k[0]) + I128::mul(point.i[1], k[1]) + I128::mul(point.i[2], k[2]))).sign();
    const Rat &r = pool.rats[static_cast<size_t>(point.rat)];
    return (r.w * BigInt(constant) + r.x * BigInt(k[0]) + r.y * BigInt(k[1]) + r.z * BigInt(k[2])).sign();
}

// The winding numbers, by an exact ray count, of the cell on one side of a face: the side the ray leaves by.
void Arrangement::reference(int f, int side_out[1], int count[2]) {
    const Face &face = faces_[static_cast<size_t>(f)];
    // The centroid of the face is strictly inside it.
    BigInt h[3][4];
    for (int k = 0; k < 3; ++k) {
        const Pt &p = pool.pts[static_cast<size_t>(face.v[k])];
        if (p.rat < 0) {
            h[k][0] = BigInt(p.i[0]);
            h[k][1] = BigInt(p.i[1]);
            h[k][2] = BigInt(p.i[2]);
            h[k][3] = BigInt(1);
        } else {
            const Rat &r = pool.rats[static_cast<size_t>(p.rat)];
            h[k][0] = r.x;
            h[k][1] = r.y;
            h[k][2] = r.z;
            h[k][3] = r.w;
        }
    }
    const BigInt w01 = h[0][3] * h[1][3], w02 = h[0][3] * h[2][3], w12 = h[1][3] * h[2][3];
    Rat centre;
    centre.x = h[0][0] * w12 + h[1][0] * w02 + h[2][0] * w01;
    centre.y = h[0][1] * w12 + h[1][1] * w02 + h[2][1] * w01;
    centre.z = h[0][2] * w12 + h[1][2] * w02 + h[2][2] * w01;
    centre.w = BigInt(3) * w01 * h[2][3];
    const int q = pool.add_rat(centre);
    const Pt point = pool.pts[static_cast<size_t>(q)];
    const Plane &own = planes_[static_cast<size_t>(face.parent)];
    const int64_t far = 4 * kMaxCoord;
    for (const int64_t *g : kDirections) {
        const I128 along = own.n[0].times(g[0]) + own.n[1].times(g[1]) + own.n[2].times(g[2]);
        if (along.zero()) continue;
        count[0] = count[1] = 0;
        bool degenerate = false;
        const auto visit = [&](int t) {
            if (degenerate || tris_[static_cast<size_t>(t)].label == 2) return;
            const Plane &pl = planes_[static_cast<size_t>(t)];
            const int at = pool.side(pl, q);
            const int heading = (pl.n[0].times(g[0]) + pl.n[1].times(g[1]) + pl.n[2].times(g[2])).sign();
            if (at == 0) {
                // The ray starts in this plane: it leaves at once, unless it runs along the plane.
                if (heading == 0) degenerate = true;
                return;
            }
            if (heading == 0 || at * heading > 0) return;  // parallel, or the plane is behind the start
            const SoupTriangle &tri = tris_[static_cast<size_t>(t)];
            const int64_t *a = pool.pts[static_cast<size_t>(tri.v[0])].i, *b = pool.pts[static_cast<size_t>(tri.v[1])].i,
                          *c = pool.pts[static_cast<size_t>(tri.v[2])].i;
            const int s0 = ray_side(q, g, a, b), s1 = ray_side(q, g, b, c), s2 = ray_side(q, g, c, a);
            const bool plus = s0 > 0 || s1 > 0 || s2 > 0, minus = s0 < 0 || s1 < 0 || s2 < 0;
            if (plus && minus) return;  // the line misses the triangle
            if (s0 == 0 || s1 == 0 || s2 == 0) {
                degenerate = true;  // through an edge or a vertex: try another direction
                return;
            }
            count[tri.label] += heading;
        };
        const int axis = g[0] ? 0 : g[1] ? 1 : 2;
        if ((g[0] != 0) + (g[1] != 0) + (g[2] != 0) == 1) {
            Box slab;
            for (int k = 0; k < 3; ++k) {
                slab.lo[k] = static_cast<int64_t>(std::floor(point.c[k].lo)) - 1;
                slab.hi[k] = static_cast<int64_t>(std::ceil(point.c[k].hi)) + 1;
            }
            if (g[axis] > 0) slab.hi[axis] = far;
            else slab.lo[axis] = -far;
            for (Span &span : spans_) overlapping(span, slab, visit);
        } else {
            for (int t = 0; t < static_cast<int>(tris_.size()); ++t) visit(t);
        }
        if (degenerate) continue;
        side_out[0] = 2 * f + ((along.sign() * face.parent_sign > 0) ? 0 : 1);
        return;
    }
    defect("no ray direction avoids every edge");
}

// For a loaded soup triangle of a closed surface: how many times the surface winds around the points
// just in front of it. 0 in front of every triangle of a valid solid.
int Arrangement::winding_in_front(int tri) {
    const SoupTriangle &t = tris_[static_cast<size_t>(tri)];
    faces_.assign(1, Face{{t.v[0], t.v[1], t.v[2]}, {1, 0}, tri, 1, false});
    int side[1], count[2];
    reference(0, side, count);
    return side[0] == 0 ? count[0] : count[0] - 1;
}

void Arrangement::wind() {
    KERFKITE_SOLID_PHASE(Winding);
    const size_t nf = faces_.size();
    cell_of_side_.assign(2 * nf, -1);
    int cells = 0;
    {
        std::vector<int> id(2 * nf, -1);
        for (uint32_t s = 0; s < 2 * nf; ++s) {
            const uint32_t root = side_find(s);
            if (id[root] < 0) id[root] = cells++;
            cell_of_side_[s] = id[root];
        }
    }
    // Faces around each cell.
    std::vector<int> start(static_cast<size_t>(cells) + 1, 0);
    for (size_t f = 0; f < nf; ++f) {
        ++start[static_cast<size_t>(cell_of_side_[2 * f]) + 1];
        ++start[static_cast<size_t>(cell_of_side_[2 * f + 1]) + 1];
    }
    for (size_t c = 0; c < static_cast<size_t>(cells); ++c) start[c + 1] += start[c];
    std::vector<int> around(2 * nf), fill(start.begin(), start.end() - 1);
    for (size_t f = 0; f < nf; ++f) {
        around[static_cast<size_t>(fill[static_cast<size_t>(cell_of_side_[2 * f])]++)] = static_cast<int>(f);
        around[static_cast<size_t>(fill[static_cast<size_t>(cell_of_side_[2 * f + 1])]++)] = static_cast<int>(f);
    }
    winding_.assign(static_cast<size_t>(cells), std::array<int, 2>{{0, 0}});
    std::vector<char> seen(static_cast<size_t>(cells), 0);
    std::vector<int> queue;
    for (size_t f = 0; f < nf; ++f) {
        const int first = cell_of_side_[2 * f];
        if (seen[static_cast<size_t>(first)]) continue;
        queue.clear();
        queue.push_back(first);
        seen[static_cast<size_t>(first)] = 1;
        for (size_t head = 0; head < queue.size(); ++head) {
            const int cell = queue[head];
            for (int k = start[static_cast<size_t>(cell)]; k < start[static_cast<size_t>(cell) + 1]; ++k) {
                const int g = around[static_cast<size_t>(k)];
                const Face &face = faces_[static_cast<size_t>(g)];
                const int front = cell_of_side_[2 * static_cast<size_t>(g)], back = cell_of_side_[2 * static_cast<size_t>(g) + 1];
                std::array<int, 2> other_winding = winding_[static_cast<size_t>(cell)];
                int other;
                if (front == back) {
                    if (face.jump[0] || face.jump[1]) throw ValueError("The surface is not closed or not consistently oriented");
                    continue;
                } else if (cell == front) {
                    other = back;
                    other_winding[0] += face.jump[0];
                    other_winding[1] += face.jump[1];
                } else {
                    other = front;
                    other_winding[0] -= face.jump[0];
                    other_winding[1] -= face.jump[1];
                }
                if (!seen[static_cast<size_t>(other)]) {
                    seen[static_cast<size_t>(other)] = 1;
                    winding_[static_cast<size_t>(other)] = other_winding;
                    queue.push_back(other);
                } else if (winding_[static_cast<size_t>(other)] != other_winding) {
                    throw ValueError("The surface is not closed or not consistently oriented");
                }
            }
        }
        // One exact ray count fixes the whole connected piece.
        int side[1], count[2];
        reference(static_cast<int>(f), side, count);
        const int cell = cell_of_side_[static_cast<size_t>(side[0])];
        const int shift0 = count[0] - winding_[static_cast<size_t>(cell)][0], shift1 = count[1] - winding_[static_cast<size_t>(cell)][1];
        for (const int c : queue) {
            winding_[static_cast<size_t>(c)][0] += shift0;
            winding_[static_cast<size_t>(c)][1] += shift1;
        }
    }
    unit_winding_ = true;
    for (size_t f = 0; f < nf && unit_winding_; ++f) {
        if (!faces_[f].jump[0] && !faces_[f].jump[1]) continue;
        const int a = winding_[static_cast<size_t>(cell_of_side_[2 * f])][0], b = winding_[static_cast<size_t>(cell_of_side_[2 * f + 1])][0];
        unit_winding_ = (a == 0 && b == 1) || (a == 1 && b == 0);
    }
}

// Cells thinner than the grid can resolve take the state (solid or empty) of what surrounds them.
//
// Two surfaces that would coincide but for the rounding of their vertices (two faces of solids turned by
// the same angle, a solid and the result of an earlier operation on it) enclose flakes and cracks a
// fraction of a nanometre thick. Rounding the result would flatten them or turn them inside out. They are
// below the resolution of the grid, so they are removed here, where it costs nothing in correctness:
// the boundary between solid and empty cells is a closed, oriented surface without self-intersections
// whatever state each cell is given. A cell is thin when three times its volume is less than kThin grid
// units times its surface area (for a slab that is a thickness of two thirds of kThin). Volumes and areas
// are measured in doubles, in a fixed order, so the same cells are chosen on every platform.
void Arrangement::absorb_thin_cells(std::vector<char> &filled) const {
    const double kThin = 3.0 * (1 << coarseness());
    const size_t cells = filled.size(), nf = faces_.size();
    std::vector<double> volume(cells, 0.0), area(cells, 0.0);
    std::vector<int> origin(cells, -1);
    std::vector<double> face_area(nf);
    const auto mid = [&](int p, int k) {
        const Pt &q = pool.pts[static_cast<size_t>(p)];
        return (q.c[k].lo + q.c[k].hi) / 2;
    };
    for (size_t f = 0; f < nf; ++f) {
        const Face &face = faces_[f];
        double a[3], b[3], c[3];
        for (int k = 0; k < 3; ++k) {
            a[k] = mid(face.v[0], k);
            b[k] = mid(face.v[1], k);
            c[k] = mid(face.v[2], k);
        }
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        face_area[f] = std::sqrt(nx * nx + ny * ny + nz * nz) / 2;
        for (int s = 0; s < 2; ++s) {
            const size_t cell = static_cast<size_t>(cell_of_side_[2 * f + static_cast<size_t>(s)]);
            if (origin[cell] < 0) origin[cell] = face.v[0];
            const double ox = mid(origin[cell], 0), oy = mid(origin[cell], 1), oz = mid(origin[cell], 2);
            // The normal points out of the cell behind the face, and into the cell in front of it.
            const double det = (a[0] - ox) * nx + (a[1] - oy) * ny + (a[2] - oz) * nz;
            volume[cell] += s == 1 ? det : -det;
            area[cell] += face_area[f];
        }
    }
    std::vector<char> thin(cells, 0);
    bool any = false;
    for (size_t c = 0; c < cells; ++c) {
        // volume[] holds six times the volume. The unbounded cell of each piece has a negative one.
        thin[c] = volume[c] > 0 && volume[c] / 2 < kThin * area[c];
        any = any || thin[c];
    }
    if (!any) return;
    for (int round = 0; round < 8; ++round) {
        std::vector<double> solid_area(cells, 0.0), empty_area(cells, 0.0);
        for (size_t f = 0; f < nf; ++f) {
            const size_t front = static_cast<size_t>(cell_of_side_[2 * f]), back = static_cast<size_t>(cell_of_side_[2 * f + 1]);
            if (thin[front]) (filled[back] ? solid_area : empty_area)[front] += face_area[f];
            if (thin[back]) (filled[front] ? solid_area : empty_area)[back] += face_area[f];
        }
        bool changed = false;
        for (size_t c = 0; c < cells; ++c) {
            if (!thin[c] || solid_area[c] == empty_area[c]) continue;
            const char state = solid_area[c] > empty_area[c];
            if (state != filled[c]) {
                filled[c] = state;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

void Arrangement::extract(Rule rule, ExactMesh &out) {
    KERFKITE_SOLID_PHASE(Extract);
    const auto solid = [rule](const std::array<int, 2> &w) {
        switch (rule) {
            case Rule::Union: return w[0] > 0 || w[1] > 0;
            case Rule::Intersection: return w[0] > 0 && w[1] > 0;
            case Rule::Difference: return w[0] > 0 && !(w[1] > 0);
            default: return w[0] > 0;
        }
    };
    const size_t nf = faces_.size();
    std::vector<char> filled(winding_.size());
    for (size_t c = 0; c < winding_.size(); ++c) filled[c] = solid(winding_[c]);
    absorb_thin_cells(filled);
    // For each face: -1 when it is not part of the result, else its index there; and whether it is turned over.
    std::vector<int> chosen(nf, -1);
    std::vector<char> turned(nf, 0), front_solid(nf, 0);
    int kept = 0;
    for (size_t f = 0; f < nf; ++f) {
        const bool front = filled[static_cast<size_t>(cell_of_side_[2 * f])] != 0, back = filled[static_cast<size_t>(cell_of_side_[2 * f + 1])] != 0;
        if (front == back) continue;
        chosen[f] = kept++;
        turned[f] = front;  // the outward normal must point away from the solid side
        front_solid[f] = front;
    }
    std::vector<uint32_t> parent(static_cast<size_t>(kept) * 3);
    const auto find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    const auto join = [&](const EdgeUse &x, const EdgeUse &y) {
        const uint32_t fx = static_cast<uint32_t>(chosen[static_cast<size_t>(x.face)]) * 3, fy = static_cast<uint32_t>(chosen[static_cast<size_t>(y.face)]) * 3;
        uint32_t p = find(fx + x.corner_a), q = find(fy + y.corner_a);
        if (p != q) parent[std::max(p, q)] = std::min(p, q);
        p = find(fx + x.corner_b);
        q = find(fy + y.corner_b);
        if (p != q) parent[std::max(p, q)] = std::min(p, q);
    };
    // Where exactly two result faces meet at an edge they are joined. Where more meet (the solid touches
    // itself along the edge) they are in counter-clockwise order, alternately entering and leaving the
    // solid, and can be joined in two ways: across each solid wedge, or across each empty wedge. One of
    // the two always gives a proper mesh: see below.
    struct Touching {
        std::vector<int> ring;  // uses, counter-clockwise, starting with a face that has solid after it
        bool across_empty, switched;
    };
    std::vector<std::pair<int, int>> plain;
    std::vector<Touching> touching;
    std::vector<int> ring;
    for (const Group &group : groups_) {
        ring.clear();
        for (size_t k = 0; k < group.count; ++k) {
            const int use = group.sorted < 0 ? static_cast<int>(group.first + k) : radial_[static_cast<size_t>(group.sorted) + k];
            if (chosen[static_cast<size_t>(uses_[static_cast<size_t>(use)].face)] >= 0) ring.push_back(use);
        }
        if (ring.empty()) continue;
        if (ring.size() % 2) defect("an odd number of result faces at an edge");
        if (ring.size() == 2) {
            plain.emplace_back(ring[0], ring[1]);
            continue;
        }
        if (group.sorted < 0) defect("unordered faces at an edge");
        const auto solid_after = [&](int use) {
            const EdgeUse &x = uses_[static_cast<size_t>(use)];
            return x.forward ? front_solid[static_cast<size_t>(x.face)] != 0 : front_solid[static_cast<size_t>(x.face)] == 0;
        };
        size_t first = 0;
        while (first < ring.size() && !solid_after(ring[first])) ++first;
        if (first == ring.size()) defect("no solid wedge at an edge");
        Touching touch;
        touch.across_empty = touch.switched = false;
        for (size_t k = 0; k < ring.size(); ++k) {
            const int use = ring[(first + k) % ring.size()];
            if (solid_after(use) != (k % 2 == 0)) defect("result faces do not alternate at an edge");
            touch.ring.push_back(use);
        }
        touching.push_back(touch);
    }
    // Joining across the solid wedges keeps bodies that only touch apart. It fails where both ends of the
    // edge are surrounded by one connected fan of faces (a slit): the edge would then be used twice by the
    // same two vertices. Such an edge is joined across the empty wedges instead, which separates the fans.
    std::vector<std::pair<uint32_t, uint32_t>> keys;
    for (int pass = 0;; ++pass) {
        std::iota(parent.begin(), parent.end(), 0u);
        for (const std::pair<int, int> &p : plain) join(uses_[static_cast<size_t>(p.first)], uses_[static_cast<size_t>(p.second)]);
        for (const Touching &touch : touching) {
            const size_t n = touch.ring.size();
            for (size_t k = touch.across_empty ? 1 : 0; k < n + (touch.across_empty ? 1 : 0); k += 2)
                join(uses_[static_cast<size_t>(touch.ring[k % n])], uses_[static_cast<size_t>(touch.ring[(k + 1) % n])]);
        }
        bool changed = false, stuck = false;
        for (Touching &touch : touching) {
            const size_t n = touch.ring.size();
            keys.clear();
            for (size_t k = touch.across_empty ? 1 : 0; k < n + (touch.across_empty ? 1 : 0); k += 2) {
                const EdgeUse &x = uses_[static_cast<size_t>(touch.ring[k % n])];
                const uint32_t fx = static_cast<uint32_t>(chosen[static_cast<size_t>(x.face)]) * 3;
                keys.emplace_back(find(fx + x.corner_a), find(fx + x.corner_b));
            }
            std::sort(keys.begin(), keys.end());
            if (std::adjacent_find(keys.begin(), keys.end()) == keys.end()) continue;
            if (touch.switched) {
                stuck = true;
            } else {
                touch.across_empty = !touch.across_empty;
                touch.switched = true;
                changed = true;
            }
        }
        if (!changed) {
            if (stuck) defect("faces touching along an edge could not be separated");
            break;
        }
        if (pass > 64) defect("pairing of touching faces did not settle");
    }
    out.vertex.clear();
    out.tri.assign(static_cast<size_t>(kept), Triangle{{0, 0, 0}});
    out.plane.assign(static_cast<size_t>(kept), 0);
    std::vector<uint32_t> index(parent.size(), UINT32_MAX);
    for (size_t f = 0; f < nf; ++f) {
        if (chosen[f] < 0) continue;
        uint32_t corner[3];
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t root = find(static_cast<uint32_t>(chosen[f]) * 3 + k);
            if (index[root] == UINT32_MAX) {
                index[root] = static_cast<uint32_t>(out.vertex.size());
                out.vertex.push_back(faces_[f].v[k]);
            }
            corner[k] = index[root];
        }
        out.tri[static_cast<size_t>(chosen[f])] = turned[f] ? Triangle{{corner[0], corner[2], corner[1]}} : Triangle{{corner[0], corner[1], corner[2]}};
        out.plane[static_cast<size_t>(chosen[f])] = faces_[f].parent;
    }
}

void Arrangement::resolve(Rule rule, ExactMesh &out) {
    subdivide();
    build_faces();
    build_cells();
    wind();
    extract(rule, out);
    stats.exact_fallbacks = pool.exact_fallbacks;
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite
