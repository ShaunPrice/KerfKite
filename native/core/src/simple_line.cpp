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
// GEOS finds candidate pairs with an index; every pair is covered by the sweep below, so the answer is the
// same. Only the time differs: contours made of many long, overlapping diagonal segments are slow here.
#include "simple_line.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>

#include "kerfkite/cam.hpp"

namespace kerfkite {
namespace detail {

namespace {

using Point = std::array<double, 2>;

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
int orientation(const Point &p1, const Point &p2, const Point &q) {
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

Hit intersect(const Point &p1, const Point &p2, const Point &q1, const Point &q2) {
    const Hit none{0, false, {}};
    if (std::min(p1[0], p2[0]) > std::max(q1[0], q2[0]) || std::max(p1[0], p2[0]) < std::min(q1[0], q2[0]) ||
        std::min(p1[1], p2[1]) > std::max(q1[1], q2[1]) || std::max(p1[1], p2[1]) < std::min(q1[1], q2[1]))
        return none;
    const int Pq1 = orientation(p1, p2, q1), Pq2 = orientation(p1, p2, q2);
    if ((Pq1 > 0 && Pq2 > 0) || (Pq1 < 0 && Pq2 < 0)) return none;
    const int Qp1 = orientation(q1, q2, p1), Qp2 = orientation(q1, q2, p2);
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

}  // namespace

bool line_is_simple(const Contour &path) {
    Contour points;
    points.reserve(path.size());
    for (const Point &point : path)
        if (points.empty() || !(points.back() == point)) points.push_back(point);
    if (points.size() < 3) return true;
    const size_t count = points.size(), segments = count - 1;

    // NonSimpleIntersectionFinder::findIntersection for segments i < j of the one line.
    const auto crossing = [&](size_t i, size_t j) {
        const Point &p1 = points[i], &p2 = points[i + 1], &q1 = points[j], &q2 = points[j + 1];
        const Hit hit = intersect(p1, p2, q1, q2);
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
    };

    // Sweep along the axis on which the segments overlap each other least.
    double extent[2] = {0, 0}, low[2] = {points[0][0], points[0][1]}, high[2] = {points[0][0], points[0][1]};
    for (size_t i = 0; i < segments; ++i)
        for (int axis = 0; axis < 2; ++axis) {
            extent[axis] += std::fabs(points[i + 1][axis] - points[i][axis]);
            low[axis] = std::min(low[axis], points[i + 1][axis]);
            high[axis] = std::max(high[axis], points[i + 1][axis]);
        }
    const double span_x = high[0] - low[0], span_y = high[1] - low[1];
    const int axis = extent[0] * span_y <= extent[1] * span_x ? 0 : 1, other = 1 - axis;
    const auto lower = [&](size_t i, int a) { return std::min(points[i][a], points[i + 1][a]); };
    const auto upper = [&](size_t i, int a) { return std::max(points[i][a], points[i + 1][a]); };
    std::vector<size_t> order(segments);
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lower(a, axis) < lower(b, axis); });
    for (size_t a = 0; a < segments; ++a) {
        const size_t i = order[a];
        const double reach = upper(i, axis), bottom = lower(i, other), top = upper(i, other);
        for (size_t b = a + 1; b < segments && lower(order[b], axis) <= reach; ++b) {
            const size_t j = order[b];
            if (lower(j, other) > top || upper(j, other) < bottom) continue;
            if (crossing(std::min(i, j), std::max(i, j))) return false;
        }
    }
    return true;
}

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
}  // namespace kerfkite
