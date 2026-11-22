// LineString.is_simple and LineString.length as GEOS answers them for one line.
//
// GEOS drops repeated points, then tests pairs of segments with its line intersector: an envelope test,
// four orientation tests and, for collinear segments, envelope tests of the endpoints. The orientation
// test is a floating-point filter followed by double-double arithmetic, which is accurate but not exact:
// for some nearly collinear points whose coordinates differ greatly in size it gives another sign than
// exact arithmetic. Both steps are repeated here operation for operation (the copy agreed with GEOS 3.14
// on 30,000 such constructed cases), and the result is also checked against the exact sign. Where the two
// disagree the answer depends on rounding inside GEOS, and the contour is declined instead of answered.
//
// The line is simple when no pair of its segments is found to cross, so the answer does not depend on
// which pairs are looked at first, nor on leaving out a pair that GEOS's own test is certain to pass.
// Looking at every pair whose boxes overlap, as this file once did, takes time that grows with the square
// of the number of segments for a contour of many long segments side by side (a comb, a fan, a spiral),
// which a design of 100,000 vertices may be. Two things bound it:
//
// * A tree over the segments, walked two branches at a time. A pair of branches is left out whole when
//   every pair of segments in it is certain to pass GEOS's test: when their boxes are apart, or when every
//   end point in one branch is clear of, and on one side of, the line of every segment in the other. See
//   Index below for why that is certain. A crossing ends the walk at once.
// * A count of the work done, in units that cost about the same time. Past kSimpleLineBudget the contour
//   is declined (NotPorted), so no contour takes more than about a second. The count, unlike the time,
//   is the same on every machine, and the tests bound it.
#include "simple_line.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>

#include "kerfkite/cam.hpp"
#include "kerfkite/line_check.hpp"

namespace kerfkite {
namespace detail {

// About a second of work on a desktop processor of 2020; see the costs below.
const std::uint64_t kSimpleLineBudget = 400000000;

namespace {

using Point = std::array<double, 2>;

// The work one call has done, and the most it may do.
struct Meter {
    std::uint64_t units = 0;
    std::uint64_t limit = kSimpleLineBudget;
    void spend(std::uint64_t cost) {
        units += cost;
        if (units > limit) throw NotPorted("a contour whose self-crossing test needs more comparisons than the core allows");
    }
};

// What each step is counted as: roughly its time in units of two or three nanoseconds.
const std::uint64_t kLookCost = 1;      // one candidate pair of the plain sweep, passed over or not
const std::uint64_t kPairCost = 12;     // GEOS's test of two segments, when the filter settles every sign
const std::uint64_t kExactCost = 150;   // one orientation that needs double-double and exact arithmetic
const std::uint64_t kBranchCost = 6;    // one pair of branches of the tree
const std::uint64_t kCornerCost = 1;    // one corner of a box against one line
const std::uint64_t kGrowCost = 12;     // one segment placed in one level of the tree

int sign(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

// GEOS's DD: a value held as the unevaluated sum of two doubles.
struct DD {
    double hi, lo;
};

DD dd_add(DD a, DD y) {
    double H, h, T, t, S, s, e, f;
    S = a.hi + y.hi;
    T = a.lo + y.lo;
    e = S - a.hi;
    f = T - a.lo;
    s = S - e;
    t = T - f;
    s = (y.hi - e) + (a.hi - s);
    t = (y.lo - f) + (a.lo - t);
    e = s + T;
    H = S + e;
    h = e + (S - H);
    e = t + h;
    const double zhi = H + e;
    return {zhi, e + (H - zhi)};
}

DD dd_multiply(DD a, DD y) {
    const double SPLIT = 134217729.0;  // 2^27 + 1
    double hx, tx, hy, ty, C, c;
    C = SPLIT * a.hi;
    hx = C - a.hi;
    c = SPLIT * y.hi;
    hx = C - hx;
    tx = a.hi - hx;
    hy = c - y.hi;
    C = a.hi * y.hi;
    hy = c - hy;
    ty = y.hi - hy;
    c = ((((hx * hy - C) + hx * ty) + tx * hy) + tx * ty) + (a.hi * y.lo + a.lo * y.hi);
    const double zhi = C + c;
    hx = C - zhi;
    return {zhi, c + hx};
}

// The exact sign of (p2 - p1) x (q - p2): six products, each kept with its rounding error, added without loss.
int exact_orientation(const Point &p1, const Point &p2, const Point &q) {
    double parts[12];
    int count = 0;
    const auto grow = [&](double b) {  // Shewchuk's grow-expansion, dropping zero components
        int kept = 0;
        for (int i = 0; i < count; ++i) {
            const double s = b + parts[i], bb = s - b;
            const double error = (b - (s - bb)) + (parts[i] - bb);
            b = s;
            if (error != 0) parts[kept++] = error;
        }
        if (b != 0) parts[kept++] = b;
        count = kept;
    };
    const auto add = [&](double x, double y) {
        const double z = x * y;
        grow(std::fma(x, y, -z));
        grow(z);
    };
    add(p2[0], q[1]);
    add(-p1[0], q[1]);
    add(p1[0], p2[1]);
    add(-p2[1], q[0]);
    add(p1[1], q[0]);
    add(-p1[1], p2[0]);
    return count ? sign(parts[count - 1]) : 0;
}

// geos::algorithm::Orientation::index(p1, p2, q): 1 when q is to the left of p1 -> p2, -1 to the right.
int orientation(const Point &p1, const Point &p2, const Point &q, Meter &meter) {
    const double detleft = (p1[0] - q[0]) * (p2[1] - q[1]);
    const double detright = (p1[1] - q[1]) * (p2[0] - q[0]);
    const double det = detleft - detright;
    double detsum;
    if (detleft > 0.0) {
        if (detright <= 0.0) return sign(det);
        detsum = detleft + detright;
    } else if (detleft < 0.0) {
        if (detright >= 0.0) return sign(det);
        detsum = -detleft - detright;
    } else {
        return sign(det);
    }
    const double errbound = 1e-15 * detsum;
    if (det >= errbound || -det >= errbound) return sign(det);

    meter.spend(kExactCost);
    const DD dx1 = dd_add({p2[0], 0.0}, {-p1[0], 0.0}), dy1 = dd_add({p2[1], 0.0}, {-p1[1], 0.0});
    const DD dx2 = dd_add({q[0], 0.0}, {-p2[0], 0.0}), dy2 = dd_add({q[1], 0.0}, {-p2[1], 0.0});
    const DD left = dd_multiply(dx1, dy2), right = dd_multiply(dy1, dx2);
    const DD d = dd_add(left, {-right.hi, -right.lo});
    const int index = d.hi < 0 || (d.hi == 0 && d.lo < 0) ? -1 : d.hi > 0 || (d.hi == 0 && d.lo > 0) ? 1 : 0;
    // Products of very small coordinates underflow, and then neither computation is exact.
    for (const Point *point : {&p1, &p2, &q})
        for (const double v : *point)
            if (v != 0 && std::fabs(v) < 1e-140) throw NotPorted("contours with coordinates below 1e-140");
    if (index != exact_orientation(p1, p2, q))
        throw NotPorted("contours whose crossing test depends on rounding inside GEOS");
    return index;
}

// geos::geom::Envelope::intersects(p1, p2, q): q inside the box of p1 and p2.
bool within(const Point &p1, const Point &p2, const Point &q) {
    return q[0] >= (p1[0] < p2[0] ? p1[0] : p2[0]) && q[0] <= (p1[0] > p2[0] ? p1[0] : p2[0]) &&
           q[1] >= (p1[1] < p2[1] ? p1[1] : p2[1]) && q[1] <= (p1[1] > p2[1] ? p1[1] : p2[1]);
}

// What LineIntersector::computeIntersection reports for two segments.
struct Hit {
    int points;    // 0, 1 or 2
    bool proper;   // one point, inside both segments
    Point at;      // the single point when it is an endpoint
};

Hit intersect(const Point &p1, const Point &p2, const Point &q1, const Point &q2, Meter &meter) {
    const Hit none{0, false, {}};
    if (std::min(p1[0], p2[0]) > std::max(q1[0], q2[0]) || std::max(p1[0], p2[0]) < std::min(q1[0], q2[0]) ||
        std::min(p1[1], p2[1]) > std::max(q1[1], q2[1]) || std::max(p1[1], p2[1]) < std::min(q1[1], q2[1]))
        return none;
    meter.spend(kPairCost);
    const int Pq1 = orientation(p1, p2, q1, meter), Pq2 = orientation(p1, p2, q2, meter);
    if ((Pq1 > 0 && Pq2 > 0) || (Pq1 < 0 && Pq2 < 0)) return none;
    const int Qp1 = orientation(q1, q2, p1, meter), Qp2 = orientation(q1, q2, p2, meter);
    if ((Qp1 > 0 && Qp2 > 0) || (Qp1 < 0 && Qp2 < 0)) return none;
    if (Pq1 == 0 && Pq2 == 0 && Qp1 == 0 && Qp2 == 0) {
        const bool q1inP = within(p1, p2, q1), q2inP = within(p1, p2, q2);
        const bool p1inQ = within(q1, q2, p1), p2inQ = within(q1, q2, p2);
        if (q1inP && q2inP) return {2, false, {}};
        if (p1inQ && p2inQ) return {2, false, {}};
        if (q1inP && p1inQ) return q1 == p1 && !q2inP && !p2inQ ? Hit{1, false, q1} : Hit{2, false, {}};
        if (q1inP && p2inQ) return q1 == p2 && !q2inP && !p1inQ ? Hit{1, false, q1} : Hit{2, false, {}};
        if (q2inP && p1inQ) return q2 == p1 && !q1inP && !p2inQ ? Hit{1, false, q2} : Hit{2, false, {}};
        if (q2inP && p2inQ) return q2 == p2 && !q1inP && !p1inQ ? Hit{1, false, q2} : Hit{2, false, {}};
        return none;
    }
    if (Pq1 == 0 || Pq2 == 0 || Qp1 == 0 || Qp2 == 0) {
        if (p1 == q1 || p1 == q2) return {1, false, p1};
        if (p2 == q1 || p2 == q2) return {1, false, p2};
        if (Pq1 == 0) return {1, false, q1};
        if (Pq2 == 0) return {1, false, q2};
        if (Qp1 == 0) return {1, false, p1};
        return {1, false, p2};
    }
    return {1, true, {}};
}

// One line with its repeated points dropped, and the questions asked of its segments. Segment i runs
// from point i to point i + 1.
struct Line {
    Contour points;
    std::size_t count = 0, segments = 0;
    Meter meter;

    explicit Line(const Contour &path) {
        points.reserve(path.size());
        for (const Point &point : path)
            if (points.empty() || !(points.back() == point)) points.push_back(point);
        count = points.size();
        segments = count ? count - 1 : 0;
    }

    // NonSimpleIntersectionFinder::findIntersection for segments i < j of the one line.
    bool crossing(std::size_t i, std::size_t j) {
        const Point &p1 = points[i], &p2 = points[i + 1], &q1 = points[j], &q2 = points[j + 1];
        const Hit hit = intersect(p1, p2, q1, q2, meter);
        if (hit.points == 0) return false;
        if (hit.points == 2 || hit.proper) return true;
        // One point, an endpoint of at least one segment: inside the other segment it is a crossing.
        if (!(hit.at == p1 || hit.at == p2) || !(hit.at == q1 || hit.at == q2)) return true;
        if (j - i <= 1) return false;
        // A vertex shared by two segments that are not neighbours is allowed only where the line's two
        // ends meet.
        const bool end_i = hit.at == p1 ? i == 0 : i + 2 == count;
        const bool end_j = hit.at == q1 ? j == 0 : j + 2 == count;
        return !(end_i && end_j);
    }

    // Every pair of segments whose boxes overlap, found by a sweep along one axis. The time grows with the
    // number of pairs that overlap along that axis, which can be all of them.
    bool every_pair() {
        // Sweep along the axis on which the segments overlap each other least.
        double extent[2] = {0, 0}, low[2] = {points[0][0], points[0][1]}, high[2] = {points[0][0], points[0][1]};
        for (std::size_t i = 0; i < segments; ++i)
            for (int axis = 0; axis < 2; ++axis) {
                extent[axis] += std::fabs(points[i + 1][axis] - points[i][axis]);
                low[axis] = std::min(low[axis], points[i + 1][axis]);
                high[axis] = std::max(high[axis], points[i + 1][axis]);
            }
        const double span_x = high[0] - low[0], span_y = high[1] - low[1];
        const int axis = extent[0] * span_y <= extent[1] * span_x ? 0 : 1, other = 1 - axis;
        const auto lower = [&](std::size_t i, int a) { return std::min(points[i][a], points[i + 1][a]); };
        const auto upper = [&](std::size_t i, int a) { return std::max(points[i][a], points[i + 1][a]); };
        std::vector<std::size_t> order(segments);
        std::iota(order.begin(), order.end(), std::size_t{0});
        // Segments that start level are taken in the line's own order, so the walk is the same everywhere.
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const double la = lower(a, axis), lb = lower(b, axis);
            return la < lb || (la == lb && a < b);
        });
        for (std::size_t a = 0; a < segments; ++a) {
            const std::size_t i = order[a];
            const double reach = upper(i, axis), bottom = lower(i, other), top = upper(i, other);
            std::size_t b = a + 1;
            for (; b < segments && lower(order[b], axis) <= reach; ++b) {
                const std::size_t j = order[b];
                if (lower(j, other) > top || upper(j, other) < bottom) continue;
                if (crossing(std::min(i, j), std::max(i, j))) return false;
            }
            meter.spend(kLookCost * (b - a));
        }
        return true;
    }

    bool simple_by_index();
};

// A tree over the segments of a line, and the walk that finds a crossing or shows there is none.
//
// Each branch knows three boxes: one round all its segments, one round their first end points and one
// round their second, the first end of a segment being the one with the smaller x (or, level, the smaller
// y). A pair of branches M and N is left out whole in two cases.
//
// 1. The boxes round all their segments are apart. Then so are the boxes of any segment of M and any of N,
//    and GEOS's test of two segments begins by passing such a pair.
//
// 2. Every end point in N is clear of, and on one side of, the line of every segment in M (or the same
//    with M and N exchanged). GEOS passes two segments when its orientation test puts both ends of one
//    strictly on one side of the other, whichever of the two it asks first. Its orientation test is exact
//    whenever its floating-point filter settles the sign, and the filter settles it when the determinant
//    is at least 1e-15 of the sum of its two products. So it is enough that for every first end l and
//    second end r of a segment of M, and every end q of a segment of N,
//
//        f(l, r, q) = (r - l) x (q - l)
//
//    has one sign and is at least 3.2e-15 D^2 in size, D being the diagonal of the box round M and N
//    together: the two products are at most |l - q| |r - q| <= D^2 between them (Cauchy-Schwarz), and the
//    filter's own rounding takes 3.4e-16 of that. f is of the first degree in each coordinate of l, of r
//    and of q, so over the boxes of M's first ends, M's second ends and N's ends it is least and greatest
//    at corners of the boxes. The corners are computed in floating point with an error under 8e-16 D^2,
//    so each is asked to reach 4e-15 D^2. Coordinates are kept between 1e-100 and 1e100 in size here, so
//    that nothing above underflows or overflows; a line outside that takes the plain sweep.
//
// What is left is pairs of single segments, which get GEOS's own test. A pair that case 2 leaves out may
// hold an orientation whose double-double arithmetic would have gone wrong; the plain sweep would have
// declined the contour for it, and here it is answered, rightly, because GEOS passes that pair whatever
// that orientation gives.
struct Index {
    struct Box {
        double x0, y0, x1, y1;
        void take(const Point &p) {
            x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
        }
        void take(const Box &b) {
            x0 = std::min(x0, b.x0), x1 = std::max(x1, b.x1), y0 = std::min(y0, b.y0), y1 = std::max(y1, b.y1);
        }
    };
    struct Branch {
        Box all, first, second;
        std::uint32_t left = 0, right = 0;  // the two halves, for a branch of more than one segment
        std::uint32_t size = 0, segment = 0;
    };

    Line &line;
    std::vector<Branch> branches;
    std::vector<std::uint32_t> order;

    explicit Index(Line &of) : line(of) {}

    static Box around(const Point &p) { return {p[0], p[1], p[0], p[1]}; }

    // The first end of segment i (which = 0) or its second (which = 1): see above.
    const Point &end_of(std::uint32_t i, int which) const {
        const Point &a = line.points[i], &b = line.points[i + 1];
        const bool forward = a[0] < b[0] || (a[0] == b[0] && a[1] <= b[1]);
        return (which == 0) == forward ? a : b;
    }

    // The branch of the segments order[begin, end), halved until each branch is one segment. The boxes of
    // first ends and of second ends are what case 2 works with, so a branch is halved where it does most
    // to shrink them: at the middle value of whichever of the four coordinates (x or y of the first end,
    // x or y of the second) is spread furthest. The halves depend only on the points, not on how the
    // library orders equal keys.
    std::uint32_t grow(std::size_t begin, std::size_t end) {
        const std::uint32_t at = static_cast<std::uint32_t>(branches.size());
        branches.emplace_back();
        line.meter.spend(kGrowCost * (end - begin));
        Branch branch;
        branch.first = around(end_of(order[begin], 0));
        branch.second = around(end_of(order[begin], 1));
        for (std::size_t k = begin + 1; k < end; ++k) branch.first.take(end_of(order[k], 0)), branch.second.take(end_of(order[k], 1));
        branch.all = branch.first;
        branch.all.take(branch.second);
        branch.size = static_cast<std::uint32_t>(end - begin);
        if (branch.size == 1) {
            branch.segment = order[begin];
            branches[at] = branch;
            return at;
        }
        const double spread[4] = {branch.first.x1 - branch.first.x0, branch.first.y1 - branch.first.y0,
                                  branch.second.x1 - branch.second.x0, branch.second.y1 - branch.second.y0};
        int widest = 0;
        for (int k = 1; k < 4; ++k)
            if (spread[k] > spread[widest]) widest = k;
        const auto key = [&](std::uint32_t i) { return end_of(i, widest / 2)[widest % 2]; };
        const std::size_t half = begin + (end - begin) / 2;
        std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(begin), order.begin() + static_cast<std::ptrdiff_t>(half),
                         order.begin() + static_cast<std::ptrdiff_t>(end), [&](std::uint32_t s, std::uint32_t t) {
                             const double ks = key(s), kt = key(t);
                             return ks < kt || (ks == kt && s < t);
                         });
        branch.left = grow(begin, half);
        branch.right = grow(half, end);
        branches[at] = branch;
        return at;
    }

    // Case 2 above: every end point in n clear of the line of every segment in m, all on one side.
    bool one_side(const Branch &m, const Branch &n) {
        // What a corner must reach: 4e-15 D^2, D the diagonal of the box round both branches.
        const double wide = std::max(m.all.x1, n.all.x1) - std::min(m.all.x0, n.all.x0);
        const double tall = std::max(m.all.y1, n.all.y1) - std::min(m.all.y0, n.all.y0);
        const double clear = 4e-15 * (wide * wide + tall * tall);
        const double lx[2] = {m.first.x0, m.first.x1}, ly[2] = {m.first.y0, m.first.y1};
        const double rx[2] = {m.second.x0, m.second.x1}, ry[2] = {m.second.y0, m.second.y1};
        const int lxs = lx[0] == lx[1] ? 1 : 2, lys = ly[0] == ly[1] ? 1 : 2, rxs = rx[0] == rx[1] ? 1 : 2, rys = ry[0] == ry[1] ? 1 : 2;
        const Box *ends[2] = {&n.first, &n.second};
        int side = 0;
        std::uint64_t corners = 0;
        bool held = true;
        for (int a = 0; held && a < lxs; ++a)
            for (int b = 0; held && b < lys; ++b)
                for (int c = 0; held && c < rxs; ++c)
                    for (int d = 0; held && d < rys; ++d) {
                        const double dx = rx[c] - lx[a], dy = ry[d] - ly[b];
                        for (int e = 0; held && e < 2; ++e) {
                            const double qx[2] = {ends[e]->x0, ends[e]->x1}, qy[2] = {ends[e]->y0, ends[e]->y1};
                            const int qxs = qx[0] == qx[1] ? 1 : 2, qys = qy[0] == qy[1] ? 1 : 2;
                            for (int g = 0; held && g < qxs; ++g)
                                for (int h = 0; held && h < qys; ++h) {
                                    ++corners;
                                    const double f = dx * (qy[h] - ly[b]) - dy * (qx[g] - lx[a]);
                                    const int here = f > 0 && f >= clear ? 1 : f < 0 && -f >= clear ? -1 : 0;
                                    if (here == 0 || (side != 0 && here != side)) held = false;
                                    side = here;
                                }
                        }
                    }
        line.meter.spend(kCornerCost * corners);
        return held;
    }

    // Whether a segment of m crosses a segment of n, for two branches with no segment in common.
    bool between(std::uint32_t m, std::uint32_t n) {
        line.meter.spend(kBranchCost);
        const Branch &M = branches[m], &N = branches[n];
        if (M.all.x0 > N.all.x1 || M.all.x1 < N.all.x0 || M.all.y0 > N.all.y1 || M.all.y1 < N.all.y0) return false;
        if (M.size == 1 && N.size == 1) return line.crossing(std::min(M.segment, N.segment), std::max(M.segment, N.segment));
        if (one_side(M, N) || one_side(N, M)) return false;
        if (N.size == 1 || (M.size > 1 && M.size >= N.size)) return between(M.left, n) || between(M.right, n);
        return between(m, N.left) || between(m, N.right);
    }

    // Whether two segments of one branch cross.
    bool within(std::uint32_t m) {
        if (branches[m].size == 1) return false;
        const std::uint32_t left = branches[m].left, right = branches[m].right;
        return within(left) || within(right) || between(left, right);
    }
};

bool Line::simple_by_index() {
    Index index(*this);
    index.order.resize(segments);
    std::iota(index.order.begin(), index.order.end(), std::uint32_t{0});
    index.branches.reserve(2 * segments);
    const std::uint32_t root = index.grow(0, segments);
    return !index.within(root);
}

// Whether the tree's reasoning holds for these points: every coordinate zero or between 1e-100 and 1e100 in
// size, and few enough segments to number.
bool indexable(const Line &line) {
    if (line.segments > 0x7fffffffu) return false;
    for (const Point &point : line.points)
        for (const double v : point)
            if (!(v == 0 || (std::fabs(v) >= 1e-100 && std::fabs(v) <= 1e100))) return false;
    return true;
}

// Below this many segments the plain sweep is as quick as building a tree.
const std::size_t kPlainSweep = 48;

void report(const Line &line, bool indexed, SimpleLineWork *work) {
    if (!work) return;
    work->units = line.meter.units;
    work->indexed = indexed;
}

// The answer by one method: 0 the usual choice, 1 the tree wherever its reasoning holds, 2 every pair with
// no limit on the work.
bool answer(const Contour &path, int method, std::uint64_t budget, SimpleLineWork *work) {
    Line line(path);
    if (line.count < 3) return true;
    const bool indexed = method != 2 && (method == 1 || line.segments > kPlainSweep) && indexable(line);
    line.meter.limit = method == 2 ? std::numeric_limits<std::uint64_t>::max() : budget;
    try {
        const bool simple = indexed ? line.simple_by_index() : line.every_pair();
        report(line, indexed, work);
        return simple;
    } catch (...) {
        report(line, indexed, work);
        throw;
    }
}

}  // namespace

bool line_is_simple(const Contour &path, SimpleLineWork *work) { return answer(path, 0, kSimpleLineBudget, work); }

bool line_is_simple_every_pair(const Contour &path, SimpleLineWork *work) { return answer(path, 2, 0, work); }

bool line_shorter_than(const Contour &path, double limit) {
    // geos::algorithm::Length::ofLine
    double length = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        const double dx = path[i][0] - path[i - 1][0], dy = path[i][1] - path[i - 1][1];
        length += std::sqrt(dx * dx + dy * dy);
    }
    if (std::fabs(length - limit) <= limit * 1e-12) throw NotPorted("a contour length within rounding of the reference's limit");
    return length < limit;
}

}  // namespace detail

LineCheck check_line(const std::vector<std::array<double, 2>> &path, LineMethod method, std::uint64_t budget) {
    LineCheck result;
    detail::SimpleLineWork work;
    try {
        result.simple = detail::answer(path, method == LineMethod::Tree ? 1 : method == LineMethod::EveryPair ? 2 : 0,
                                       budget ? budget : detail::kSimpleLineBudget, &work);
    } catch (const NotPorted &declined) {
        result.declined = true;
        result.reason = declined.what();
    }
    result.work = work.units;
    result.tree = work.indexed;
    return result;
}

std::uint64_t line_check_budget() { return detail::kSimpleLineBudget; }

int geos_orientation(const std::array<double, 2> &p1, const std::array<double, 2> &p2, const std::array<double, 2> &q) {
    detail::Meter meter;
    meter.limit = UINT64_MAX;
    return detail::orientation(p1, p2, q, meter);
}

}  // namespace kerfkite
