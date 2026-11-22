// Exact planar arrangement and triangulation: see planar.hpp.
//
// The triangulation is the classical incremental Delaunay one (Lawson's flips; the constrained form after
// Sloan, "A fast algorithm for generating constrained Delaunay triangulations", 1993), written from those
// descriptions. The points are inserted in the order of a Hilbert curve over their bounding box, so that
// each is found by a short walk from the one before. Each is put into the triangle (or on the edge) that
// contains it, and the edges around it are flipped while the point across one of them is certainly inside
// the circle through the triangle on this side (legalize). Then each required edge is recovered by flipping
// the edges that cross it, and the edges that are not required are flipped by the same rule until none can
// be (improve).
//
// The rule, exactly: an edge that is not required is flipped when, and only when, the in-circle test of the
// interval enclosures of the four points (filtered_in_circle) proves the fourth point strictly inside the
// circle through the other three, and the two triangles form a strictly convex quadrilateral (exact turns).
// Points on one circle, or so near it that the intervals cannot tell, keep the edge they have; which one that
// is follows from the fixed order of insertion. Every flip is then a true Delaunay flip, so the flipping
// ends (each one lowers the points lifted onto the paraboloid), and the result is a fixed function of the
// points and the required edges. Orientation tests are exact, so no triangle without area is ever made.
#include "planar.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kerfkite {
namespace solid {
namespace detail {

namespace {
[[noreturn]] void defect(const char *what) { throw std::logic_error(std::string("solid kernel: planar arrangement: ") + what); }
}  // namespace

Planar::Planar(Pool &pool, const Plane &carrier, int axis_u, int axis_v, int sign)
    : pool_(pool), carrier_(carrier), au_(axis_u), av_(axis_v), sign_(sign) {
    root[0] = root[1] = root[2] = -1;
}

void Planar::reset(const Plane &carrier, int axis_u, int axis_v, int sign) {
    carrier_ = carrier;
    au_ = axis_u;
    av_ = axis_v;
    sign_ = sign;
    root[0] = root[1] = root[2] = -1;
    supports.clear();
    points.clear();
    segments.clear();
    pieces.clear();
    triangles.clear();
    neighbours.clear();
    on_line_.clear();
    line_start_.clear();
    line_ids_.clear();
}

// For p on the line through a and b: strictly between them.
bool Planar::between(int a, int b, int p) const {
    int axis = au_;
    if (pool_.compare(a, b, axis) == 0) axis = av_;
    const int c1 = pool_.compare(a, p, axis), c2 = pool_.compare(p, b, axis);
    return c1 == c2 && c1 != 0;
}

// The middle of each interval and half its width, with the rounding of both: the true coordinate is
// within r of m. An interval of one number (a whole coordinate) is exact.
Planar::Local Planar::local_of(const Interval &cu, const Interval &cv, bool grid) {
    Local at;
    at.u = (cu.lo + cu.hi) / 2;
    at.v = (cv.lo + cv.hi) / 2;
    at.ru = cu.lo == cu.hi ? 0.0 : (cu.hi - cu.lo) * 0.5 * (1 + 0x1p-50) + std::fabs(at.u) * 0x1p-52;
    at.rv = cv.lo == cv.hi ? 0.0 : (cv.hi - cv.lo) * 0.5 * (1 + 0x1p-50) + std::fabs(at.v) * 0x1p-52;
    at.grid = grid;
    return at;
}

// A turn in doubles. With A = bu - au, B = cv - av, C = bv - av, D = cu - au the turn is the sign of
// A B - C D. Each coordinate is known as a double m and a bound r with the true value within r of m.
// Every operation below is rounded to nearest, so its result is within 2^-53 of itself of the true
// result of its operands (written u for 2^-53; the code uses 2^-52):
//     the true A is within dA = |A| u + ra + rb of the computed A, and likewise B, C, D;
//     the true A B is within |A| dB + |B| dA + dA dB of the computed A times the computed B, which the
//     rounded product P is within |P| u of; likewise C D and Q;
//     the rounded P - Q is within |P - Q| u of P - Q.
// So the true value is within E = |A| dB + |B| dA + dA dB + |C| dD + |D| dC + dC dD + u (|P| + |Q| + |P - Q|)
// of the computed one. E is a sum of products of non-negative numbers, some twenty rounded operations,
// each raising or lowering it by 2^-53 of itself at most; it is taken a millionth larger, and a little
// more for results too small for that. A computed value beyond E has the sign of the true one; for any
// other the answer is 2, "cannot tell", and the caller asks the pool. Only + - * and comparisons of
// doubles are used, each correctly rounded on every platform (the build forbids contraction into fused
// operations), so the same turns are decided here everywhere; and since the pool gives the exact sign
// for the rest, the result would be the same even if they were not.
int Planar::filtered_turn(const Local &pa, const Local &pb, const Local &pc) {
    const double kU = 0x1p-52;
    const double A = pb.u - pa.u, B = pc.v - pa.v, C = pb.v - pa.v, D = pc.u - pa.u;
    const double dA = std::fabs(A) * kU + (pa.ru + pb.ru), dB = std::fabs(B) * kU + (pa.rv + pc.rv);
    const double dC = std::fabs(C) * kU + (pa.rv + pb.rv), dD = std::fabs(D) * kU + (pa.ru + pc.ru);
    const double P = A * B, Q = C * D, value = P - Q;
    const double bound = (std::fabs(A) * dB + std::fabs(B) * dA + dA * dB + std::fabs(C) * dD + std::fabs(D) * dC + dC * dD +
                          kU * (std::fabs(P) + std::fabs(Q) + std::fabs(value))) * 1.000001 + 0x1p-900;
    return value > bound ? 1 : value < -bound ? -1 : 2;
}

// The in-circle determinant of a, b, c, d (rows a - d, b - d, c - d, each with its squared length), in
// interval arithmetic: each operation is rounded to nearest and widened outwards by more than its rounding
// (exact.hpp), so the interval of the result contains the determinant of every choice of points within the
// intervals given, the true points among them. Its sign, times the turn of a b c, is positive when d is
// inside the circle through a, b, c. The turn is taken from the same intervals; when it cannot be told, or
// the determinant's interval contains zero, the answer is 2. Only + - * and comparisons of doubles are used.
int Planar::filtered_in_circle(const Interval a[2], const Interval b[2], const Interval c[2], const Interval d[2]) {
    const Interval adx = a[0] - d[0], ady = a[1] - d[1], bdx = b[0] - d[0], bdy = b[1] - d[1], cdx = c[0] - d[0], cdy = c[1] - d[1];
    const Interval al = adx * adx + ady * ady, bl = bdx * bdx + bdy * bdy, cl = cdx * cdx + cdy * cdy;
    const Interval det = adx * (bdy * cl - cdy * bl) - ady * (bdx * cl - cdx * bl) + al * (bdx * cdy - bdy * cdx);
    const int s = det.sign();
    if (s != 1 && s != -1) return 2;
    const Interval turn = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    const int t = turn.sign();
    if (t != 1 && t != -1) return 2;
    return s * t;
}

int Planar::in_circle(int a, int b, int c, int d) const {
    return filtered_in_circle(iv_[static_cast<size_t>(a)].data(), iv_[static_cast<size_t>(b)].data(), iv_[static_cast<size_t>(c)].data(), iv_[static_cast<size_t>(d)].data());
}

// The Hilbert curve's own order of the cells of a 2^16 by 2^16 grid: the cell (x, y) is reached after
// hilbert_index(x, y) others. Quadrant by quadrant from the largest, turning the frame as the curve does.
uint64_t Planar::hilbert_index(uint32_t x, uint32_t y) {
    const uint32_t n = 1u << 16;
    uint64_t d = 0;
    for (uint32_t s = n / 2; s > 0; s /= 2) {
        const uint32_t rx = (x & s) ? 1u : 0u, ry = (y & s) ? 1u : 0u;
        d += static_cast<uint64_t>(s) * s * ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) {
                x = n - 1 - x;
                y = n - 1 - y;
            }
            std::swap(x, y);
        }
    }
    return d;
}

// The turn of three local vertices: orient() of their points, by two shorter ways where they apply.
// First in doubles (filtered_turn). Then by what the points are: three points on the line of one segment
// do not turn, and split() has noted every point it placed on a segment, each by an exact test or by
// construction (a crossing lies in the carrier plane and in the planes of both segments). What is left
// is asked of the pool, with intervals and then integers, as every turn used to be. Grid points go to
// the pool at once: their turn is two products of integers.
int Planar::o(int a, int b, int c) const {
    const Local &pa = local_[static_cast<size_t>(a)], &pb = local_[static_cast<size_t>(b)], &pc = local_[static_cast<size_t>(c)];
    if (!plain && !(pa.grid && pb.grid && pc.grid)) {
        const int turn = filtered_turn(pa, pb, pc);
        if (turn != 2) return sign_ * turn;
        if (one_line(a, b, c)) return 0;
    }
    return orient(pid_[static_cast<size_t>(a)], pid_[static_cast<size_t>(b)], pid_[static_cast<size_t>(c)]);
}

// Whether the three local vertices are known to lie on the line of one segment.
bool Planar::one_line(int a, int b, int c) const {
    if (line_start_.empty()) return false;
    // Go through the shortest list and look in the other two.
    int x = a, y = b, z = c;
    const auto length = [&](int k) { return line_start_[static_cast<size_t>(k) + 1] - line_start_[static_cast<size_t>(k)]; };
    if (length(y) < length(x)) std::swap(x, y);
    if (length(z) < length(x)) std::swap(x, z);
    const int *ids = line_ids_.data();
    const int *y0 = ids + line_start_[static_cast<size_t>(y)], *y1 = ids + line_start_[static_cast<size_t>(y) + 1];
    const int *z0 = ids + line_start_[static_cast<size_t>(z)], *z1 = ids + line_start_[static_cast<size_t>(z) + 1];
    for (int k = line_start_[static_cast<size_t>(x)]; k < line_start_[static_cast<size_t>(x) + 1]; ++k)
        if (std::binary_search(y0, y1, ids[k]) && std::binary_search(z0, z1, ids[k])) return true;
    return false;
}

void Planar::split() {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    const size_t n = segments.size();
    std::vector<std::pair<int, int>> &on = on_;  // (segment, a point on it between its ends), in no order
    on.clear();
    std::vector<Bounds> &box = box_;
    box.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Pt &a = pool_.pts[static_cast<size_t>(segments[i].a)], &b = pool_.pts[static_cast<size_t>(segments[i].b)];
        box[i] = Bounds{std::min(a.c[au_].lo, b.c[au_].lo), std::max(a.c[au_].hi, b.c[au_].hi), std::min(a.c[av_].lo, b.c[av_].lo),
                        std::max(a.c[av_].hi, b.c[av_].hi)};
    }
    std::vector<int> &order = order_;
    order.resize(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        return box[static_cast<size_t>(x)].ulo != box[static_cast<size_t>(y)].ulo ? box[static_cast<size_t>(x)].ulo < box[static_cast<size_t>(y)].ulo : x < y;
    });
    std::vector<int> &created = created_;
    created.clear();
    for (size_t ii = 0; ii < n; ++ii) {
        const size_t i = static_cast<size_t>(order[ii]);
        const PlanarSegment &s = segments[i];
        if (s.a == s.b) continue;
        for (size_t jj = ii + 1; jj < n && box[static_cast<size_t>(order[jj])].ulo <= box[i].uhi; ++jj) {
            const size_t j = static_cast<size_t>(order[jj]);
            if (box[j].vlo > box[i].vhi || box[i].vlo > box[j].vhi) continue;
            const PlanarSegment &t = segments[j];
            if (t.a == t.b || (s.a == t.a && s.b == t.b) || (s.a == t.b && s.b == t.a)) continue;
            const int o1 = orient(s.a, s.b, t.a), o2 = orient(s.a, s.b, t.b);
            if (o1 == o2 && o1 != 0) continue;
            const int o3 = orient(t.a, t.b, s.a), o4 = orient(t.a, t.b, s.b);
            if (o3 == o4 && o3 != 0) continue;
            if (o1 * o2 < 0 && o3 * o4 < 0) {
                const int x = pool_.three_planes(carrier_, supports[static_cast<size_t>(s.support)], supports[static_cast<size_t>(t.support)]);
                if (x < 0) defect("crossing segments without a common point");
                on.emplace_back(static_cast<int>(i), x);
                on.emplace_back(static_cast<int>(j), x);
                created.push_back(x);
                continue;
            }
            if (o1 == 0 && t.a != s.a && t.a != s.b && between(s.a, s.b, t.a)) on.emplace_back(static_cast<int>(i), t.a);
            if (o2 == 0 && t.b != s.a && t.b != s.b && between(s.a, s.b, t.b)) on.emplace_back(static_cast<int>(i), t.b);
            if (o3 == 0 && s.a != t.a && s.a != t.b && between(t.a, t.b, s.a)) on.emplace_back(static_cast<int>(j), s.a);
            if (o4 == 0 && s.b != t.a && s.b != t.b && between(t.a, t.b, s.b)) on.emplace_back(static_cast<int>(j), s.b);
        }
    }
    // Points that are not the end of any segment may still lie on one.
    std::vector<int> &ends = ends_;
    ends.clear();
    ends.reserve(2 * n);
    for (const PlanarSegment &s : segments) {
        ends.push_back(s.a);
        ends.push_back(s.b);
    }
    std::sort(ends.begin(), ends.end());
    for (const int p : points) {
        if (std::binary_search(ends.begin(), ends.end(), p)) continue;
        const Pt &q = pool_.pts[static_cast<size_t>(p)];
        for (size_t i = 0; i < n; ++i) {
            if (q.c[au_].lo > box[i].uhi || q.c[au_].hi < box[i].ulo || q.c[av_].lo > box[i].vhi || q.c[av_].hi < box[i].vlo) continue;
            const PlanarSegment &s = segments[i];
            if (s.a == s.b) continue;
            if (orient(s.a, s.b, p) == 0 && between(s.a, s.b, p)) on.emplace_back(static_cast<int>(i), p);
        }
    }
    points.insert(points.end(), created.begin(), created.end());
    pieces.clear();
    on_line_.clear();
    // The points on each segment, segment by segment. Within a segment they are put in order along it
    // below, so the order they come in here does not matter.
    std::sort(on.begin(), on.end());
    size_t next = 0;
    std::vector<int> &list = list_;
    for (size_t i = 0; i < n; ++i) {
        const PlanarSegment &s = segments[i];
        list.clear();
        for (; next < on.size() && on[next].first == static_cast<int>(i); ++next) list.push_back(on[next].second);
        if (s.a == s.b) continue;
        points.push_back(s.a);
        points.push_back(s.b);
        // Its ends and the points placed on it lie on its line: for o().
        on_line_.emplace_back(s.a, static_cast<int>(i));
        on_line_.emplace_back(s.b, static_cast<int>(i));
        for (const int q : list) on_line_.emplace_back(q, static_cast<int>(i));
        if (list.empty()) {
            pieces.push_back(s);
            continue;
        }
        int axis = au_;
        if (pool_.compare(s.a, s.b, axis) == 0) axis = av_;
        const int direction = pool_.compare(s.a, s.b, axis);
        std::sort(list.begin(), list.end(), [&](int x, int y) {
            if (x == y) return false;
            return pool_.compare(x, y, axis) == direction;  // in the order from a to b
        });
        list.erase(std::unique(list.begin(), list.end()), list.end());
        int previous = s.a;
        for (const int p : list) {
            pieces.push_back(PlanarSegment{previous, p, s.support, s.source});
            previous = p;
        }
        pieces.push_back(PlanarSegment{previous, s.b, s.support, s.source});
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
}

void Planar::relink(int neighbour, int from, int to) {
    if (neighbour < 0) return;
    Tri &t = tris_[static_cast<size_t>(neighbour)];
    for (int k = 0; k < 3; ++k)
        if (t.n[k] == from) {
            t.n[k] = to;
            return;
        }
    defect("broken adjacency");
}

void Planar::split_face(int t, int p) {
    const Tri old = tris_[static_cast<size_t>(t)];
    const int t1 = static_cast<int>(tris_.size()), t2 = t1 + 1;
    tris_[static_cast<size_t>(t)] = Tri{{old.v[0], old.v[1], p}, {old.n[0], t1, t2}};
    tris_.push_back(Tri{{old.v[1], old.v[2], p}, {old.n[1], t2, t}});
    tris_.push_back(Tri{{old.v[2], old.v[0], p}, {old.n[2], t, t1}});
    relink(old.n[1], t, t1);
    relink(old.n[2], t, t2);
    vtri_[static_cast<size_t>(old.v[0])] = t;
    vtri_[static_cast<size_t>(old.v[1])] = t;
    vtri_[static_cast<size_t>(old.v[2])] = t1;
    vtri_[static_cast<size_t>(p)] = t;
}

void Planar::split_edge(int t, int i, int p) {
    const Tri old = tris_[static_cast<size_t>(t)];
    const int a = old.v[i], b = old.v[(i + 1) % 3], c = old.v[(i + 2) % 3];
    const int u = old.n[i], nb = old.n[(i + 1) % 3], nc = old.n[(i + 2) % 3];
    const int t2 = static_cast<int>(tris_.size());
    tris_.push_back(Tri{{p, b, c}, {-1, nb, t}});
    tris_[static_cast<size_t>(t)] = Tri{{a, p, c}, {-1, t2, nc}};
    relink(nb, t, t2);
    vtri_[static_cast<size_t>(a)] = t;
    vtri_[static_cast<size_t>(b)] = t2;
    vtri_[static_cast<size_t>(c)] = t;
    vtri_[static_cast<size_t>(p)] = t;
    if (u < 0) return;
    const Tri other = tris_[static_cast<size_t>(u)];
    int j = 0;
    while (j < 3 && other.n[j] != t) ++j;
    if (j == 3) defect("broken adjacency");
    const int d = other.v[(j + 2) % 3];
    const int across_ad = other.n[(j + 1) % 3], across_db = other.n[(j + 2) % 3];
    const int u2 = static_cast<int>(tris_.size());
    tris_[static_cast<size_t>(u)] = Tri{{b, p, d}, {t2, u2, across_db}};
    tris_.push_back(Tri{{p, a, d}, {t, across_ad, u}});
    relink(across_ad, u, u2);
    tris_[static_cast<size_t>(t)].n[0] = u2;
    tris_[static_cast<size_t>(t2)].n[0] = u;
    vtri_[static_cast<size_t>(d)] = u;
}

// Replaces the edge i of t, shared with its neighbour, by the other diagonal of the two triangles.
void Planar::flip(int t, int i) {
    const Tri old = tris_[static_cast<size_t>(t)];
    const int u = old.n[i];
    const Tri other = tris_[static_cast<size_t>(u)];
    int j = 0;
    while (j < 3 && other.n[j] != t) ++j;
    if (j == 3) defect("broken adjacency");
    const int a = old.v[i], b = old.v[(i + 1) % 3], c = old.v[(i + 2) % 3], d = other.v[(j + 2) % 3];
    const int nb = old.n[(i + 1) % 3], nc = old.n[(i + 2) % 3];
    const int across_ad = other.n[(j + 1) % 3], across_db = other.n[(j + 2) % 3];
    tris_[static_cast<size_t>(t)] = Tri{{a, d, c}, {across_ad, u, nc}};
    tris_[static_cast<size_t>(u)] = Tri{{d, b, c}, {across_db, nb, t}};
    relink(across_ad, u, t);
    relink(nb, t, u);
    vtri_[static_cast<size_t>(a)] = t;
    vtri_[static_cast<size_t>(d)] = t;
    vtri_[static_cast<size_t>(c)] = t;
    vtri_[static_cast<size_t>(b)] = u;
}

// The triangle that has the directed edge x -> y, found by turning about x.
bool Planar::find_edge(int x, int y, int &t, int &i) const {
    const int start = vtri_[static_cast<size_t>(x)];
    int current = start;
    const size_t limit = tris_.size() + 2;
    for (size_t step = 0; step < limit; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        if (tri.v[(k + 1) % 3] == y) {
            t = current;
            i = k;
            return true;
        }
        current = tri.n[(k + 2) % 3];
        if (current < 0 || current == start) break;
    }
    if (current == start) return false;
    current = start;
    for (size_t step = 0; step < limit; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        current = tri.n[k];
        if (current < 0 || current == start) break;
        const Tri &next = tris_[static_cast<size_t>(current)];
        const int m = next.v[0] == x ? 0 : next.v[1] == x ? 1 : 2;
        if (next.v[(m + 1) % 3] == y) {
            t = current;
            i = m;
            return true;
        }
    }
    return false;
}

// Every triangle at vertex x.
void Planar::around(int x, std::vector<int> &out) const {
    out.clear();
    const int start = vtri_[static_cast<size_t>(x)];
    int current = start;
    const size_t limit = tris_.size() + 2;
    for (size_t step = 0; step < limit; ++step) {
        out.push_back(current);
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        const int next = tri.n[(k + 2) % 3];
        if (next == start) return;
        if (next < 0) break;
        current = next;
    }
    current = start;
    for (size_t step = 0; step < limit; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        const int next = tri.n[k];
        if (next < 0 || next == start) return;
        out.push_back(next);
        current = next;
    }
}

void Planar::insert(int p) {
    int t = last_;
    int s[3] = {0, 0, 0};
    bool found = false;
    const size_t limit = 2 * tris_.size() + 16;
    for (size_t step = 0; step < limit && !found; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(t)];
        found = true;
        for (int k = 0; k < 3; ++k) {
            s[k] = o(tri.v[k], tri.v[(k + 1) % 3], p);
            if (s[k] < 0) {
                if (tri.n[k] < 0) defect("a point outside the triangulated area");
                t = tri.n[k];
                found = false;
                break;
            }
        }
    }
    if (!found) {
        // The walk can circle in a triangulation that is not Delaunay: look everywhere.
        for (size_t c = 0; c < tris_.size() && !found; ++c) {
            const Tri &tri = tris_[c];
            found = true;
            for (int k = 0; k < 3 && found; ++k) {
                s[k] = o(tri.v[k], tri.v[(k + 1) % 3], p);
                found = s[k] >= 0;
            }
            if (found) t = static_cast<int>(c);
        }
        if (!found) defect("a point outside the triangulated area");
    }
    const int zeros = (s[0] == 0) + (s[1] == 0) + (s[2] == 0);
    // The triangles made, each with the index of its edge across from p, for legalize().
    std::vector<std::pair<int, int>> &stack = flip_stack_;
    stack.clear();
    const int made = static_cast<int>(tris_.size());
    if (zeros == 0) {
        split_face(t, p);
        stack.emplace_back(t, 0);
        stack.emplace_back(made, 0);
        stack.emplace_back(made + 1, 0);
    } else if (zeros == 1) {
        const int i = s[0] == 0 ? 0 : s[1] == 0 ? 1 : 2;
        const bool across = tris_[static_cast<size_t>(t)].n[i] >= 0;
        const int u = tris_[static_cast<size_t>(t)].n[i];
        split_edge(t, i, p);
        stack.emplace_back(t, 2);
        stack.emplace_back(made, 1);
        if (across) {
            stack.emplace_back(u, 2);
            stack.emplace_back(made + 1, 1);
        }
    } else {
        defect("two points at one place");
    }
    legalize(p);
    last_ = vtri_[static_cast<size_t>(p)];
}

// After p has been inserted: while the point across an edge opposite p is certainly inside the circle
// through that edge and p, the edge is flipped (the rule at the head of this file), and the two edges
// opposite p that the flip makes are looked at in turn. Every triangle on the stack has p as its third
// corner across the edge named, so the triangle across that edge does not have p.
void Planar::legalize(int p) {
    std::vector<std::pair<int, int>> &stack = flip_stack_;
    size_t guard = 0;
    const size_t limit = 8 * tris_.size() + 64;
    while (!stack.empty()) {
        const std::pair<int, int> top = stack.back();
        stack.pop_back();
        const int t = top.first, i = top.second;
        const Tri &tri = tris_[static_cast<size_t>(t)];
        if (tri.v[(i + 2) % 3] != p) continue;
        const int u = tri.n[i];
        if (u < 0) continue;
        const int a = tri.v[i], b = tri.v[(i + 1) % 3];
        const Tri &other = tris_[static_cast<size_t>(u)];
        int j = 0;
        while (j < 3 && other.n[j] != t) ++j;
        if (j == 3) defect("broken adjacency");
        const int d = other.v[(j + 2) % 3];
        if (in_circle(a, b, p, d) != 1) continue;
        if (!(o(a, d, p) > 0 && o(d, b, p) > 0)) continue;
        if (++guard > limit) break;  // cannot happen with true Delaunay flips; a bound all the same
        flip(t, i);
        stack.emplace_back(t, 0);
        stack.emplace_back(u, 0);
    }
    stack.clear();
}

void Planar::enforce(int first, int second) {
    std::vector<std::pair<int, int>> &todo = todo_;
    std::vector<int> &fan = fan_;
    todo.clear();
    todo.emplace_back(first, second);
    size_t guard = 0;
    while (!todo.empty()) {
        if (++guard > 64 * (tris_.size() + 16)) defect("an edge could not be recovered");
        const int a = todo.back().first, b = todo.back().second;
        todo.pop_back();
        int t, i;
        if (find_edge(a, b, t, i) || find_edge(b, a, t, i)) continue;
        // The triangle at a that the segment leaves through its far edge.
        int right = -1, left = -1, far = -1;  // the crossed edge runs right -> left; far is the triangle beyond it
        bool divided = false;
        around(a, fan);
        const int pa = pid_[static_cast<size_t>(a)], pb = pid_[static_cast<size_t>(b)];
        const int du = pool_.compare(pa, pb, au_), dv = pool_.compare(pa, pb, av_);
        for (const int current : fan) {
            const Tri &tri = tris_[static_cast<size_t>(current)];
            const int k = tri.v[0] == a ? 0 : tri.v[1] == a ? 1 : 2;
            const int p = tri.v[(k + 1) % 3], q = tri.v[(k + 2) % 3];
            const int sp = o(a, b, p), sq = o(a, b, q);
            // A vertex on the segment itself (same direction from a as b): a-p is already an edge.
            if (sp == 0 && pool_.compare(pa, pid_[static_cast<size_t>(p)], au_) == du && pool_.compare(pa, pid_[static_cast<size_t>(p)], av_) == dv) {
                todo.emplace_back(p, b);
                divided = true;
                break;
            }
            if (sq == 0 && pool_.compare(pa, pid_[static_cast<size_t>(q)], au_) == du && pool_.compare(pa, pid_[static_cast<size_t>(q)], av_) == dv) {
                todo.emplace_back(q, b);
                divided = true;
                break;
            }
            if (sp < 0 && sq > 0) {
                right = p;
                left = q;
                far = tri.n[(k + 1) % 3];
                break;
            }
        }
        if (divided) continue;
        if (right < 0 || far < 0) defect("an edge leaves the triangulated area");
        // The crossed edges, taken from the front and put back at the end: a queue held as a list and the
        // place of its front.
        std::vector<std::pair<int, int>> &crossed = crossed_;
        size_t front = 0;
        crossed.clear();
        crossed.emplace_back(right, left);
        bool restart = false;
        for (size_t step = 0;; ++step) {
            if (step > tris_.size() + 4) defect("an edge could not be traced");
            const Tri &tri = tris_[static_cast<size_t>(far)];
            const int k = tri.v[0] == left ? 0 : tri.v[1] == left ? 1 : 2;  // far has left -> right
            if (tri.v[(k + 1) % 3] != right) defect("broken adjacency");
            const int r = tri.v[(k + 2) % 3];
            if (r == b) break;
            const int s = o(a, b, r);
            if (s == 0) {
                // A vertex on the segment between its ends: recover the two halves instead.
                todo.emplace_back(a, r);
                todo.emplace_back(r, b);
                restart = true;
                break;
            }
            if (s < 0) {
                // r is on the right: the segment leaves through r -> left.
                far = tri.n[(k + 2) % 3];
                right = r;
            } else {
                far = tri.n[(k + 1) % 3];
                left = r;
            }
            if (far < 0) defect("an edge leaves the triangulated area");
            crossed.emplace_back(right, left);
        }
        if (restart) continue;
        size_t flips = 0;
        const size_t flip_limit = 64 * (crossed.size() + 4) * (crossed.size() + 4) + 4096;
        while (front < crossed.size()) {
            if (++flips > flip_limit) defect("edge flips did not finish");
            const std::pair<int, int> edge = crossed[front++];
            if (!find_edge(edge.first, edge.second, t, i)) defect("a crossed edge vanished");
            const Tri &tri = tris_[static_cast<size_t>(t)];
            const int x = tri.v[i], y = tri.v[(i + 1) % 3], c = tri.v[(i + 2) % 3];
            const int u = tri.n[i];
            if (u < 0) defect("a crossed edge on the outside");
            const Tri &other = tris_[static_cast<size_t>(u)];
            int j = 0;
            while (j < 3 && other.n[j] != t) ++j;
            if (j == 3) defect("broken adjacency");
            const int d = other.v[(j + 2) % 3];
            if (o(x, d, c) > 0 && o(d, y, c) > 0) {
                flip(t, i);
                if (c != a && c != b && d != a && d != b) {
                    const int sc = o(a, b, c), sd = o(a, b, d);
                    if (sc * sd < 0) crossed.emplace_back(c, d);
                }
            } else {
                crossed.push_back(edge);
            }
        }
    }
}

// Flips edges that are not required by the rule at the head of this file until none can be: with the
// required edges, the triangulation is then constrained Delaunay but for points the intervals cannot place
// against a circle. Any triangulation is correct; this one avoids the needless slivers that rounding to the
// grid would later flatten or turn over, and the long triangles that later operations would find in the way.
void Planar::improve(const std::vector<std::pair<int, int>> &required) {
    std::vector<std::pair<int, int>> &stack = flip_stack_;
    stack.clear();
    for (size_t t = 0; t < tris_.size(); ++t)
        for (int k = 0; k < 3; ++k)
            if (tris_[t].n[k] > static_cast<int>(t)) stack.emplace_back(tris_[t].v[k], tris_[t].v[(k + 1) % 3]);
    size_t flips = 0;
    const size_t limit = 32 * tris_.size() + 64;
    while (!stack.empty() && flips < limit) {
        const std::pair<int, int> edge = stack.back();
        stack.pop_back();
        int t, i;
        if (!find_edge(edge.first, edge.second, t, i)) continue;  // flipped away meanwhile
        const Tri &tri = tris_[static_cast<size_t>(t)];
        const int u = tri.n[i];
        if (u < 0) continue;
        const int x = tri.v[i], y = tri.v[(i + 1) % 3], c = tri.v[(i + 2) % 3];
        if (std::binary_search(required.begin(), required.end(), std::make_pair(std::min(x, y), std::max(x, y)))) continue;
        const Tri &other = tris_[static_cast<size_t>(u)];
        int j = 0;
        while (j < 3 && other.n[j] != t) ++j;
        if (j == 3) defect("broken adjacency");
        const int d = other.v[(j + 2) % 3];
        if (in_circle(x, y, c, d) != 1) continue;
        if (!(o(x, d, c) > 0 && o(d, y, c) > 0)) continue;
        flip(t, i);
        ++flips;
        stack.emplace_back(x, d);
        stack.emplace_back(d, y);
        stack.emplace_back(y, c);
        stack.emplace_back(c, x);
    }
}

void Planar::triangulate(int r0, int r1, int r2) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    pid_.clear();
    pid_.push_back(r0);
    pid_.push_back(r1);
    pid_.push_back(r2);
    std::vector<int> &rest = rest_;
    rest.clear();
    for (const int p : points)
        if (p != r0 && p != r1 && p != r2) rest.push_back(p);
    // Neighbouring points one after another keep the search for the containing triangle short: the points
    // go in the order of a Hilbert curve over their bounding box, by the middles of their intervals (a fixed
    // computation in doubles), then by id. Only speed depends on the order being a good one; the result
    // depends on it only through points that the in-circle test cannot place (see the head of this file).
    for (const int p : rest) pool_.settle(p);
    {
        double ulo = 0, uhi = 0, vlo = 0, vhi = 0;
        for (size_t k = 0; k < rest.size(); ++k) {
            const Pt &q = pool_.pts[static_cast<size_t>(rest[k])];
            const double u = (q.c[au_].lo + q.c[au_].hi) / 2, v = (q.c[av_].lo + q.c[av_].hi) / 2;
            if (k == 0 || u < ulo) ulo = u;
            if (k == 0 || u > uhi) uhi = u;
            if (k == 0 || v < vlo) vlo = v;
            if (k == 0 || v > vhi) vhi = v;
        }
        const double su = uhi > ulo ? 65535.0 / (uhi - ulo) : 0.0, sv = vhi > vlo ? 65535.0 / (vhi - vlo) : 0.0;
        const auto cell = [](double x) { return static_cast<uint32_t>(x < 0 ? 0.0 : x > 65535.0 ? 65535.0 : x); };
        std::vector<std::pair<uint64_t, int>> keyed(rest.size());
        for (size_t k = 0; k < rest.size(); ++k) {
            const Pt &q = pool_.pts[static_cast<size_t>(rest[k])];
            const double u = (q.c[au_].lo + q.c[au_].hi) / 2, v = (q.c[av_].lo + q.c[av_].hi) / 2;
            keyed[k] = std::make_pair(hilbert_index(cell((u - ulo) * su), cell((v - vlo) * sv)), rest[k]);
        }
        std::sort(keyed.begin(), keyed.end());
        for (size_t k = 0; k < rest.size(); ++k) rest[k] = keyed[k].second;
    }
    pid_.insert(pid_.end(), rest.begin(), rest.end());
    std::vector<int> &ids = ids_;
    ids.resize(pid_.size());
    for (size_t k = 0; k < ids.size(); ++k) ids[k] = static_cast<int>(k);
    std::sort(ids.begin(), ids.end(), [&](int x, int y) { return pid_[static_cast<size_t>(x)] < pid_[static_cast<size_t>(y)]; });
    const auto local = [&](int pool_id) {
        size_t lo = 0, hi = ids.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (pid_[static_cast<size_t>(ids[mid])] < pool_id) lo = mid + 1;
            else hi = mid;
        }
        if (lo == ids.size() || pid_[static_cast<size_t>(ids[lo])] != pool_id) defect("a segment end that is not a point");
        return ids[lo];
    };
    if (orient(r0, r1, r2) <= 0) defect("the outer triangle is not counter-clockwise");
    // The coordinates of each local vertex, for o() and in_circle().
    local_.resize(pid_.size());
    iv_.resize(pid_.size());
    for (size_t k = 0; k < pid_.size(); ++k) {
        pool_.settle(pid_[k]);
        const Pt &q = pool_.pts[static_cast<size_t>(pid_[k])];
        local_[k] = local_of(q.c[au_], q.c[av_], q.rat < 0);
        iv_[k][0] = q.c[au_];
        iv_[k][1] = q.c[av_];
    }
    // The segments each local vertex lies on the line of, from the notes of split().
    line_start_.clear();
    line_ids_.clear();
    if (!on_line_.empty()) {
        std::vector<std::pair<int, int>> &filed = filed_;
        filed.clear();
        filed.reserve(on_line_.size());
        for (const std::pair<int, int> &note : on_line_) filed.emplace_back(local(note.first), note.second);
        std::sort(filed.begin(), filed.end());
        filed.erase(std::unique(filed.begin(), filed.end()), filed.end());
        line_start_.assign(pid_.size() + 1, 0);
        for (const std::pair<int, int> &note : filed) ++line_start_[static_cast<size_t>(note.first) + 1];
        for (size_t k = 0; k < pid_.size(); ++k) line_start_[k + 1] += line_start_[k];
        line_ids_.resize(filed.size());
        for (size_t k = 0; k < filed.size(); ++k) line_ids_[k] = filed[k].second;
    }
    tris_.clear();
    tris_.reserve(2 * pid_.size() + 4);
    tris_.push_back(Tri{{0, 1, 2}, {-1, -1, -1}});
    vtri_.assign(pid_.size(), 0);
    last_ = 0;
    for (size_t p = 3; p < pid_.size(); ++p) insert(static_cast<int>(p));
    std::vector<std::pair<int, int>> &wanted = wanted_;
    wanted.clear();
    wanted.reserve(pieces.size());
    for (const PlanarSegment &s : pieces) {
        const int a = local(s.a), b = local(s.b);
        wanted.emplace_back(std::min(a, b), std::max(a, b));
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (const std::pair<int, int> &edge : wanted) enforce(edge.first, edge.second);
    improve(wanted);
    triangles.resize(tris_.size());
    neighbours.resize(tris_.size());
    for (size_t k = 0; k < tris_.size(); ++k) {
        for (size_t c = 0; c < 3; ++c) {
            triangles[k][c] = pid_[static_cast<size_t>(tris_[k].v[c])];
            neighbours[k][c] = tris_[k].n[c];
        }
    }
}

void Planar::triangulate_around() {
    double ulo = 0, uhi = 0, vlo = 0, vhi = 0;
    bool first = true;
    for (const int p : points) {
        const Pt &q = pool_.pts[static_cast<size_t>(p)];
        if (first || q.c[au_].lo < ulo) ulo = q.c[au_].lo;
        if (first || q.c[au_].hi > uhi) uhi = q.c[au_].hi;
        if (first || q.c[av_].lo < vlo) vlo = q.c[av_].lo;
        if (first || q.c[av_].hi > vhi) vhi = q.c[av_].hi;
        first = false;
    }
    if (first) defect("nothing to triangulate");
    const int64_t u0 = static_cast<int64_t>(std::floor(ulo)) - 1, u1 = static_cast<int64_t>(std::ceil(uhi)) + 1;
    const int64_t v0 = static_cast<int64_t>(std::floor(vlo)) - 1, v1 = static_cast<int64_t>(std::ceil(vhi)) + 1;
    const int64_t reach = (u1 - u0) + (v1 - v0) + 2;
    const int64_t corner[3][2] = {{u0 - reach, v0 - 1}, {u1 + reach, v0 - 1}, {(u0 + u1) / 2, v1 + reach}};
    for (int k = 0; k < 3; ++k) {
        int64_t xyz[3] = {0, 0, 0};
        xyz[au_] = corner[k][0];
        xyz[av_] = corner[k][1];
        root[k] = pool_.add_grid(xyz[0], xyz[1], xyz[2]);
    }
    if (orient(root[0], root[1], root[2]) < 0) std::swap(root[1], root[2]);
    triangulate(root[0], root[1], root[2]);
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite
