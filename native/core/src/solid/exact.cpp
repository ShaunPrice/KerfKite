// Exact predicates and constructions: see exact.hpp.
#include "exact.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

Plane plane_of(const int64_t *a, const int64_t *b, const int64_t *c) {
    const int64_t ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const int64_t vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    Plane pl;
    pl.n[0] = I128::mul(uy, vz) - I128::mul(uz, vy);
    pl.n[1] = I128::mul(uz, vx) - I128::mul(ux, vz);
    pl.n[2] = I128::mul(ux, vy) - I128::mul(uy, vx);
    pl.d = pl.n[0].times(a[0]) + pl.n[1].times(a[1]) + pl.n[2].times(a[2]);
    return pl;
}

Plane plane_through_edge(const int64_t *p, const int64_t *q, int k) {
    const int64_t d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    int64_t e[3] = {0, 0, 0};
    e[k] = 1;
    Plane pl;
    pl.n[0] = I128(d[1] * e[2] - d[2] * e[1]);
    pl.n[1] = I128(d[2] * e[0] - d[0] * e[2]);
    pl.n[2] = I128(d[0] * e[1] - d[1] * e[0]);
    pl.d = pl.n[0].times(p[0]) + pl.n[1].times(p[1]) + pl.n[2].times(p[2]);
    return pl;
}

I128 plane_value(const Plane &pl, const int64_t *p) {
    return pl.n[0].times(p[0]) + pl.n[1].times(p[1]) + pl.n[2].times(p[2]) - pl.d;
}

I128 orient3d_value(const int64_t *a, const int64_t *b, const int64_t *c, const int64_t *d) {
    const int64_t ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const int64_t vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const int64_t wx = d[0] - a[0], wy = d[1] - a[1], wz = d[2] - a[2];
    // Differences are below 2^38, minors below 2^77, the sum below 2^117.
    return (I128::mul(vy, wz) - I128::mul(vz, wy)).times(ux) - (I128::mul(vx, wz) - I128::mul(vz, wx)).times(uy) +
           (I128::mul(vx, wy) - I128::mul(vy, wx)).times(uz);
}

int orient3d_grid(const int64_t *a, const int64_t *b, const int64_t *c, const int64_t *d) {
    return orient3d_value(a, b, c, d).sign();
}

int orient2d_grid(const int64_t *a, const int64_t *b, const int64_t *c, int u, int v) {
    return (I128::mul(b[u] - a[u], c[v] - a[v]) - I128::mul(b[v] - a[v], c[u] - a[u])).sign();
}

Interval ratio_interval(const BigInt &num, const BigInt &den) {
    if (num.zero()) return Interval();
    double m1, m2;
    int e1, e2;
    num.leading(m1, e1);
    den.leading(m2, e2);
    const double q = std::ldexp(m1 / m2, e1 - e2);
    // Two mantissas each within 2^-52 and one rounded division: widening twice covers the three.
    const Interval r(widen_down(widen_down(q)), widen_up(widen_up(q)));
    return num.neg ? Interval(-r.hi, -r.lo) : r;
}

int64_t round_ratio(const BigInt &num, const BigInt &den, bool &exact) {
    const Interval r = ratio_interval(num, den);
    int64_t q = static_cast<int64_t>(std::floor((r.lo + r.hi) / 2 + 0.5));
    // q is right when 0 <= 2*num + den - 2*den*q < 2*den; the estimate is off by one at most.
    const BigInt two_den = den + den;
    BigInt t = num + num + den - two_den * BigInt(q);
    while (t.sign() < 0) {
        --q;
        t = t + two_den;
    }
    while (BigInt::cmp(t, two_den) >= 0) {
        ++q;
        t = t - two_den;
    }
    exact = BigInt::cmp(t, den) == 0;
    return q;
}

namespace {

BigInt det3(const BigInt &a, const BigInt &b, const BigInt &c, const BigInt &d, const BigInt &e, const BigInt &f,
            const BigInt &g, const BigInt &h, const BigInt &i) {
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

Interval interval_of(const I128 &v) {
    const double x = v.to_double();
    // The conversion rounds twice (two halves and their sum).
    return Interval(widen_down(widen_down(x)), widen_up(widen_up(x)));
}

struct Homogeneous {
    BigInt c[4];
};

Homogeneous homogeneous(const Pool &pool, int p) {
    const Pt &pt = pool.pts[static_cast<size_t>(p)];
    Homogeneous h;
    if (pt.rat < 0) {
        h.c[0] = BigInt(pt.i[0]);
        h.c[1] = BigInt(pt.i[1]);
        h.c[2] = BigInt(pt.i[2]);
        h.c[3] = BigInt(1);
    } else {
        const Rat &r = pool.rats[static_cast<size_t>(pt.rat)];
        h.c[0] = r.x;
        h.c[1] = r.y;
        h.c[2] = r.z;
        h.c[3] = r.w;
    }
    return h;
}

}  // namespace

Pool::Pool() : rat_index_(RatLess{this}) {}

int Pool::find_grid(int64_t x, int64_t y, int64_t z) const {
    const auto it = grid_index_.find(Key{x, y, z});
    return it == grid_index_.end() ? -1 : it->second;
}

int Pool::add_grid(int64_t x, int64_t y, int64_t z) {
    const Key key{x, y, z};
    const auto it = grid_index_.find(key);
    if (it != grid_index_.end()) return it->second;
    Pt pt;
    pt.i[0] = x;
    pt.i[1] = y;
    pt.i[2] = z;
    pt.rat = -1;
    for (int k = 0; k < 3; ++k) pt.c[k] = Interval(static_cast<double>(pt.i[k]));
    const int id = static_cast<int>(pts.size());
    pts.push_back(pt);
    grid_index_.emplace(key, id);
    return id;
}

int Pool::add_rat(const Rat &source) {
    Rat r = source;
    if (r.w.sign() < 0) {
        r.x = -r.x;
        r.y = -r.y;
        r.z = -r.z;
        r.w = -r.w;
    }
    bool ex, ey, ez;
    const int64_t qx = round_ratio(r.x, r.w, ex), qy = round_ratio(r.y, r.w, ey), qz = round_ratio(r.z, r.w, ez);
    if (ex && ey && ez) return add_grid(qx, qy, qz);
    Pt pt;
    pt.i[0] = pt.i[1] = pt.i[2] = 0;
    pt.rat = static_cast<int>(rats.size());
    pt.c[0] = ratio_interval(r.x, r.w);
    pt.c[1] = ratio_interval(r.y, r.w);
    pt.c[2] = ratio_interval(r.z, r.w);
    const int id = static_cast<int>(pts.size());
    pts.push_back(pt);
    rats.push_back(r);
    const auto inserted = rat_index_.insert(id);
    if (!inserted.second) {
        pts.pop_back();
        rats.pop_back();
        return *inserted.first;
    }
    return id;
}

int Pool::line_plane(int p, int q, const Plane &pl) {
    const Pt &a = pts[static_cast<size_t>(p)], &b = pts[static_cast<size_t>(q)];
    const I128 da = plane_value(pl, a.i), db = plane_value(pl, b.i);
    // a + (b - a) * da / (da - db) = (da * b - db * a) / (da - db).
    const BigInt ba(da), bb(db);
    Rat r;
    r.x = ba * BigInt(b.i[0]) - bb * BigInt(a.i[0]);
    r.y = ba * BigInt(b.i[1]) - bb * BigInt(a.i[1]);
    r.z = ba * BigInt(b.i[2]) - bb * BigInt(a.i[2]);
    r.w = ba - bb;
    return add_rat(r);
}

int Pool::three_planes(const Plane &a, const Plane &b, const Plane &c) {
    const BigInt a0(a.n[0]), a1(a.n[1]), a2(a.n[2]), ad(a.d);
    const BigInt b0(b.n[0]), b1(b.n[1]), b2(b.n[2]), bd(b.d);
    const BigInt c0(c.n[0]), c1(c.n[1]), c2(c.n[2]), cd(c.d);
    Rat r;
    r.w = det3(a0, a1, a2, b0, b1, b2, c0, c1, c2);
    if (r.w.zero()) return -1;
    r.x = det3(ad, a1, a2, bd, b1, b2, cd, c1, c2);
    r.y = det3(a0, ad, a2, b0, bd, b2, c0, cd, c2);
    r.z = det3(a0, a1, ad, b0, b1, bd, c0, c1, cd);
    return add_rat(r);
}

int Pool::orient3d(int a, int b, int c, int d) const {
    const Pt &pa = pts[static_cast<size_t>(a)], &pb = pts[static_cast<size_t>(b)], &pc = pts[static_cast<size_t>(c)],
             &pd = pts[static_cast<size_t>(d)];
    if (pa.rat < 0 && pb.rat < 0 && pc.rat < 0 && pd.rat < 0) return orient3d_grid(pa.i, pb.i, pc.i, pd.i);
    const Interval ux = pb.c[0] - pa.c[0], uy = pb.c[1] - pa.c[1], uz = pb.c[2] - pa.c[2];
    const Interval vx = pc.c[0] - pa.c[0], vy = pc.c[1] - pa.c[1], vz = pc.c[2] - pa.c[2];
    const Interval wx = pd.c[0] - pa.c[0], wy = pd.c[1] - pa.c[1], wz = pd.c[2] - pa.c[2];
    const Interval det = ux * (vy * wz - vz * wy) - uy * (vx * wz - vz * wx) + uz * (vx * wy - vy * wx);
    const int s = det.sign();
    if (s != 2) return s;
    ++const_cast<Pool *>(this)->exact_fallbacks;
    // det[b-a; c-a; d-a] is minus the 4x4 determinant of the homogeneous rows, and every w is positive.
    const Homogeneous h[4] = {homogeneous(*this, a), homogeneous(*this, b), homogeneous(*this, c), homogeneous(*this, d)};
    BigInt total;
    for (int i = 0; i < 4; ++i) {
        const Homogeneous &r0 = h[i == 0 ? 1 : 0], &r1 = h[i <= 1 ? 2 : 1], &r2 = h[i <= 2 ? 3 : 2];
        const BigInt minor = det3(r0.c[0], r0.c[1], r0.c[2], r1.c[0], r1.c[1], r1.c[2], r2.c[0], r2.c[1], r2.c[2]);
        const BigInt term = h[i].c[3] * minor;
        total = (i % 2 == 1) ? total + term : total - term;  // the cofactor sign of column 4 is (-1)^(i+3)
    }
    return -total.sign();
}

int Pool::orient2d(int a, int b, int c, int u, int v) const {
    const Pt &pa = pts[static_cast<size_t>(a)], &pb = pts[static_cast<size_t>(b)], &pc = pts[static_cast<size_t>(c)];
    if (pa.rat < 0 && pb.rat < 0 && pc.rat < 0) return orient2d_grid(pa.i, pb.i, pc.i, u, v);
    const Interval det = (pb.c[u] - pa.c[u]) * (pc.c[v] - pa.c[v]) - (pb.c[v] - pa.c[v]) * (pc.c[u] - pa.c[u]);
    const int s = det.sign();
    if (s != 2) return s;
    ++const_cast<Pool *>(this)->exact_fallbacks;
    const Homogeneous ha = homogeneous(*this, a), hb = homogeneous(*this, b), hc = homogeneous(*this, c);
    return det3(ha.c[u], ha.c[v], ha.c[3], hb.c[u], hb.c[v], hb.c[3], hc.c[u], hc.c[v], hc.c[3]).sign();
}

int Pool::compare(int a, int b, int axis) const {
    if (a == b) return 0;
    const Pt &pa = pts[static_cast<size_t>(a)], &pb = pts[static_cast<size_t>(b)];
    if (pa.rat < 0 && pb.rat < 0) return pa.i[axis] < pb.i[axis] ? -1 : pa.i[axis] > pb.i[axis] ? 1 : 0;
    if (pa.c[axis].hi < pb.c[axis].lo) return -1;
    if (pa.c[axis].lo > pb.c[axis].hi) return 1;
    ++const_cast<Pool *>(this)->exact_fallbacks;
    const Homogeneous ha = homogeneous(*this, a), hb = homogeneous(*this, b);
    return BigInt::cmp(ha.c[axis] * hb.c[3], hb.c[axis] * ha.c[3]);
}

int Pool::compare_lex(int a, int b) const {
    for (int axis = 0; axis < 3; ++axis) {
        const int c = compare(a, b, axis);
        if (c) return c;
    }
    return 0;
}

int Pool::side(const Plane &pl, int p) const {
    const Pt &pt = pts[static_cast<size_t>(p)];
    if (pt.rat < 0) return plane_value(pl, pt.i).sign();
    const Interval value =
        interval_of(pl.n[0]) * pt.c[0] + interval_of(pl.n[1]) * pt.c[1] + interval_of(pl.n[2]) * pt.c[2] - interval_of(pl.d);
    const int s = value.sign();
    if (s != 2) return s;
    ++const_cast<Pool *>(this)->exact_fallbacks;
    const Rat &r = rats[static_cast<size_t>(pt.rat)];
    return (BigInt(pl.n[0]) * r.x + BigInt(pl.n[1]) * r.y + BigInt(pl.n[2]) * r.z - BigInt(pl.d) * r.w).sign();
}

void Pool::round(int p, int64_t out[3]) const {
    const Pt &pt = pts[static_cast<size_t>(p)];
    if (pt.rat < 0) {
        out[0] = pt.i[0];
        out[1] = pt.i[1];
        out[2] = pt.i[2];
        return;
    }
    const Rat &r = rats[static_cast<size_t>(pt.rat)];
    bool exact;
    out[0] = round_ratio(r.x, r.w, exact);
    out[1] = round_ratio(r.y, r.w, exact);
    out[2] = round_ratio(r.z, r.w, exact);
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite
