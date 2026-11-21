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

// The largest whole number not above x, for |x| below 2^62: the conversion truncates towards zero, which
// is one too many for a negative x that is not whole.
inline int64_t floor_to_int(double x) {
    const int64_t t = static_cast<int64_t>(x);
    return static_cast<double>(t) > x ? t - 1 : t;
}

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

Pool::Pool() {}

void CellTable::add(int64_t x, int64_t y, int64_t z, int id) {
    if (2 * (used_ + 1) > slots_.size()) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.empty() ? 256 : 2 * old.size(), Slot{0, 0, 0, -1});
        const size_t mask = slots_.size() - 1;
        for (const Slot &slot : old) {
            if (slot.id < 0) continue;
            size_t s = hash(slot.x, slot.y, slot.z) & mask;
            while (slots_[s].id >= 0) s = (s + 1) & mask;
            slots_[s] = slot;
        }
    }
    const size_t mask = slots_.size() - 1;
    size_t s = hash(x, y, z) & mask;
    while (slots_[s].id >= 0) s = (s + 1) & mask;
    slots_[s] = Slot{x, y, z, id};
    ++used_;
}

int Pool::find_grid(int64_t x, int64_t y, int64_t z) const {
    return grid_index_.find(x, y, z, [](int) { return true; });
}

int Pool::add_grid(int64_t x, int64_t y, int64_t z) {
    const int found = find_grid(x, y, z);
    if (found >= 0) return found;
    Pt pt;
    pt.i[0] = x;
    pt.i[1] = y;
    pt.i[2] = z;
    pt.rat = -1;
    pt.pending = -1;
    for (int k = 0; k < 3; ++k) pt.c[k] = Interval(static_cast<double>(pt.i[k]));
    const int id = static_cast<int>(pts.size());
    pts.push_back(pt);
    grid_index_.add(x, y, z, id);
    return id;
}

// Files a rational point under every unit cell its intervals touch.
void Pool::file(int id) {
    const Pt &pt = pts[static_cast<size_t>(id)];
    int64_t lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        lo[k] = floor_to_int(pt.c[k].lo);
        hi[k] = floor_to_int(pt.c[k].hi);
    }
    for (int64_t x = lo[0]; x <= hi[0]; ++x)
        for (int64_t y = lo[1]; y <= hi[1]; ++y)
            for (int64_t z = lo[2]; z <= hi[2]; ++z) rat_index_.add(x, y, z, id);
}

// The point the pool already has with the value of r, whose coordinates the intervals c enclose; -1 when
// it has none. A point of that value is filed under a cell that c touches, because its own intervals
// enclose the same coordinates. Equality is decided with the integers.
int Pool::existing(const Rat &r, const Interval c[3]) {
    int64_t lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        lo[k] = floor_to_int(c[k].lo);
        hi[k] = floor_to_int(c[k].hi);
    }
    const auto same = [&](int id) {
        {
            const Pt &other = pts[static_cast<size_t>(id)];
            for (int k = 0; k < 3; ++k)
                if (other.c[k].hi < c[k].lo || other.c[k].lo > c[k].hi) return false;
        }
        settle(id);  // may move pts and rats: nothing is held across it
        const Pt &other = pts[static_cast<size_t>(id)];
        for (int k = 0; k < 3; ++k)
            if (other.c[k].hi < c[k].lo || other.c[k].lo > c[k].hi) return false;
        ++exact_fallbacks;
        const Rat &o = rats[static_cast<size_t>(other.rat)];
        return BigInt::cmp(o.x * r.w, r.x * o.w) == 0 && BigInt::cmp(o.y * r.w, r.y * o.w) == 0 && BigInt::cmp(o.z * r.w, r.z * o.w) == 0;
    };
    for (int64_t x = lo[0]; x <= hi[0]; ++x)
        for (int64_t y = lo[1]; y <= hi[1]; ++y)
            for (int64_t z = lo[2]; z <= hi[2]; ++z) {
                const int found = rat_index_.find(x, y, z, same);
                if (found >= 0) return found;
            }
    return -1;
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
    pt.pending = -1;
    // A coordinate that is a whole number is kept exactly: points in an axis plane (a cap, the face of a
    // box) then give exact zeros in the interval predicates, with no need for the integers.
    pt.c[0] = ex ? Interval(static_cast<double>(qx)) : ratio_interval(r.x, r.w);
    pt.c[1] = ey ? Interval(static_cast<double>(qy)) : ratio_interval(r.y, r.w);
    pt.c[2] = ez ? Interval(static_cast<double>(qz)) : ratio_interval(r.z, r.w);
    const int found = existing(r, pt.c);
    if (found >= 0) return found;
    pt.rat = static_cast<int>(rats.size());
    const int id = static_cast<int>(pts.size());
    pts.push_back(pt);
    rats.push_back(r);
    ++rational_points_;
    file(id);
    return id;
}

int Pool::line_plane(int p, int q, const Plane &pl) {
    return line_plane(p, q, plane_value(pl, pts[static_cast<size_t>(p)].i), plane_value(pl, pts[static_cast<size_t>(q)].i));
}

namespace {

// The point where the edge from grid point a to grid point b crosses a plane with the values da and db
// at them: a + (b - a) * da / (da - db) = (da * b - db * a) / (da - db).
Rat crossing_point(const Pt &a, const Pt &b, const I128 &da, const I128 &db) {
    const BigInt ba(da), bb(db);
    Rat r;
    r.x = ba * BigInt(b.i[0]) - bb * BigInt(a.i[0]);
    r.y = ba * BigInt(b.i[1]) - bb * BigInt(a.i[1]);
    r.z = ba * BigInt(b.i[2]) - bb * BigInt(a.i[2]);
    r.w = ba - bb;
    return r;
}

}  // namespace

// Most points made here are never looked at again: an edge crosses the plane of a triangle far outside
// that triangle, and the pair is dropped after the points have been put in order along a line. So the
// point is given its place and an enclosure in doubles, and the integers are computed only when a
// predicate cannot decide from the enclosure, or the point becomes part of a triangle (settle()).
//
// The pool's rules are kept exactly: a point on the grid is a grid point, and a value is held once. The
// point is left pending only when its enclosure proves both that it is off the grid (in some coordinate
// the enclosure holds no whole number) and that it differs from every point the pool has (no point filed
// under a cell the enclosure touches has intervals meeting it in all three coordinates; a point of the
// same value would). Otherwise it is computed at once, as it always was. Either way the pool gives the
// same ids, in the same order, as if every point were computed at once.
//
// The enclosure. With s = |da| and t = |db|, which have opposite signs, the point is
// a + (b - a) * L where L = s / (s + t), 0 < L < 1. I128::to_double is within a relative 2^-51 (the high
// and low halves are each converted within 2^-53 and their sum rounded once), so the rounded sum s + t
// and the rounded quotient give L within 2^-51 + 2^-51 + 2^-53 + 2^-53 and their products: below 2^-49,
// absolutely, since L < 1. D = b_k - a_k is a whole number below 2^53 in size and exact as a double, so
// the rounded product D * L is within |D| * (2^-49 + 2^-53) of the true one, and the rounded sum
// x = a_k + D * L within that plus |x| * 2^-53 of the true coordinate. The enclosure is
// x -+ (|D| * 2^-47 + |x| * 2^-51), four times that bound, which leaves room for the rounding of the
// bound itself and of the two ends. Where D is 0 the coordinate is a_k exactly.
// Fills the intervals of pt with the enclosure described above. True when a whole number lies in the
// enclosure of every coordinate, so that the point may be on the grid.
bool Pool::enclose_crossing(int p, int q, const I128 &da, const I128 &db, Pt &pt) const {
    const Pt &a = pts[static_cast<size_t>(p)], &b = pts[static_cast<size_t>(q)];
    const double s = da.abs().to_double(), t = db.abs().to_double();
    const double part = s / (s + t);
    bool whole = true;
    for (int k = 0; k < 3; ++k) {
        const int64_t difference = b.i[k] - a.i[k];
        if (!difference) {
            pt.c[k] = Interval(static_cast<double>(a.i[k]));
            continue;
        }
        const double d = static_cast<double>(difference), x = static_cast<double>(a.i[k]) + d * part;
        const double slack = std::fabs(d) * 0x1p-47 + std::fabs(x) * 0x1p-51;
        pt.c[k] = Interval(x - slack, x + slack);
        whole = whole && static_cast<double>(floor_to_int(pt.c[k].hi)) >= pt.c[k].lo;  // a whole number lies in [lo, hi]
    }
    pt.i[0] = pt.i[1] = pt.i[2] = 0;
    pt.rat = kPendingRat;
    return whole;
}

int Pool::line_plane_at_once(int p, int q, const I128 &da, const I128 &db) {
    return add_rat(crossing_point(pts[static_cast<size_t>(p)], pts[static_cast<size_t>(q)], da, db));
}

int Pool::line_plane(int p, int q, const I128 &da, const I128 &db) {
    if (plain) return line_plane_at_once(p, q, da, db);
    Pt pt;
    bool alone = !enclose_crossing(p, q, da, db, pt);
    int64_t lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    if (alone) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = floor_to_int(pt.c[k].lo);
            hi[k] = floor_to_int(pt.c[k].hi);
        }
        const auto meets = [&](int id) {
            const Pt &other = pts[static_cast<size_t>(id)];
            for (int k = 0; k < 3; ++k)
                if (other.c[k].hi < pt.c[k].lo || other.c[k].lo > pt.c[k].hi) return false;
            return true;
        };
        for (int64_t x = lo[0]; x <= hi[0] && alone; ++x)
            for (int64_t y = lo[1]; y <= hi[1] && alone; ++y)
                for (int64_t z = lo[2]; z <= hi[2] && alone; ++z) alone = rat_index_.find(x, y, z, meets) < 0;
    }
    if (!alone) return line_plane_at_once(p, q, da, db);
    pt.pending = static_cast<int>(recipes_.size());
    recipes_.push_back(Recipe{p, q, da, db, false});
    const int id = static_cast<int>(pts.size());
    pts.push_back(pt);
    ++rational_points_;
    for (int64_t x = lo[0]; x <= hi[0]; ++x)
        for (int64_t y = lo[1]; y <= hi[1]; ++y)
            for (int64_t z = lo[2]; z <= hi[2]; ++z) rat_index_.add(x, y, z, id);
    return id;
}

int Pool::line_plane_loose(int p, int q, const I128 &da, const I128 &db) {
    Pt pt;
    enclose_crossing(p, q, da, db, pt);
    pt.pending = static_cast<int>(recipes_.size());
    recipes_.push_back(Recipe{p, q, da, db, true});
    pts.push_back(pt);
    return static_cast<int>(pts.size()) - 1;
}

// The Rat and the intervals of a pending point, as add_rat() computes them.
void Pool::compute_pending(int id) {
    const Recipe recipe = recipes_[static_cast<size_t>(pts[static_cast<size_t>(id)].pending)];
    Rat r = crossing_point(pts[static_cast<size_t>(recipe.p)], pts[static_cast<size_t>(recipe.q)], recipe.at_p, recipe.at_q);
    if (r.w.sign() < 0) {
        r.x = -r.x;
        r.y = -r.y;
        r.z = -r.z;
        r.w = -r.w;
    }
    bool ex, ey, ez;
    const int64_t qx = round_ratio(r.x, r.w, ex), qy = round_ratio(r.y, r.w, ey), qz = round_ratio(r.z, r.w, ez);
    if (ex && ey && ez && !recipe.loose) throw std::logic_error("solid kernel: a pending point is on the grid");
    Pt &pt = pts[static_cast<size_t>(id)];
    pt.c[0] = ex ? Interval(static_cast<double>(qx)) : ratio_interval(r.x, r.w);
    pt.c[1] = ey ? Interval(static_cast<double>(qy)) : ratio_interval(r.y, r.w);
    pt.c[2] = ez ? Interval(static_cast<double>(qz)) : ratio_interval(r.z, r.w);
    pt.rat = static_cast<int>(rats.size());
    pt.pending = -1;
    rats.push_back(r);
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
    // One point named twice: two rows of the determinant are equal or one is zero, so it is zero. (The
    // intervals of such a determinant always straddle zero, and the integers would be asked.)
    if (!plain && (a == b || a == c || a == d || b == c || b == d || c == d)) return 0;
    if (pa.pending >= 0 || pb.pending >= 0 || pc.pending >= 0 || pd.pending >= 0) {
        settle(a);
        settle(b);
        settle(c);
        settle(d);
    }
    const Interval ux = pb.c[0] - pa.c[0], uy = pb.c[1] - pa.c[1], uz = pb.c[2] - pa.c[2];
    const Interval vx = pc.c[0] - pa.c[0], vy = pc.c[1] - pa.c[1], vz = pc.c[2] - pa.c[2];
    const Interval wx = pd.c[0] - pa.c[0], wy = pd.c[1] - pa.c[1], wz = pd.c[2] - pa.c[2];
    const Interval det = ux * (vy * wz - vz * wy) - uy * (vx * wz - vz * wx) + uz * (vx * wy - vy * wx);
    const int s = det.sign();
    if (s != 2) return s;
    ++const_cast<Pool *>(this)->exact_fallbacks;
    // With a grid point among them, take it as the origin: the rows are then differences (scaled by the
    // positive w of a rational point), and the determinant is 3x3 with small entries. The determinant
    // changes sign with each exchange of two points, so moving point k to the front costs (-1)^k.
    const int id[4] = {a, b, c, d};
    for (int k = 0; k < 4; ++k) {
        const Pt &origin = pts[static_cast<size_t>(id[k])];
        if (origin.rat >= 0) continue;
        BigInt row[3][3];
        int filled = 0;
        for (int m = 0; m < 4; ++m) {
            if (m == k) continue;
            const Pt &q = pts[static_cast<size_t>(id[m])];
            for (int axis = 0; axis < 3; ++axis) {
                if (q.rat < 0) {
                    row[filled][axis] = BigInt(q.i[axis] - origin.i[axis]);
                } else {
                    const Rat &r = rats[static_cast<size_t>(q.rat)];
                    row[filled][axis] = (axis == 0 ? r.x : axis == 1 ? r.y : r.z) - BigInt(origin.i[axis]) * r.w;
                }
            }
            ++filled;
        }
        const int sign = det3(row[0][0], row[0][1], row[0][2], row[1][0], row[1][1], row[1][2], row[2][0], row[2][1], row[2][2]).sign();
        return k % 2 ? -sign : sign;
    }
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
    if (!plain && (a == b || a == c || b == c)) return 0;  // one point named twice: no area
    if (pa.pending >= 0 || pb.pending >= 0 || pc.pending >= 0) {
        settle(a);
        settle(b);
        settle(c);
    }
    const Interval det = (pb.c[u] - pa.c[u]) * (pc.c[v] - pa.c[v]) - (pb.c[v] - pa.c[v]) * (pc.c[u] - pa.c[u]);
    const int s = det.sign();
    if (s != 2) return s;
    ++const_cast<Pool *>(this)->exact_fallbacks;
    // With a grid point among them, turn it to the front (which keeps the sign) and use differences.
    const int id[3] = {a, b, c};
    for (int k = 0; k < 3; ++k) {
        const Pt &origin = pts[static_cast<size_t>(id[k])];
        if (origin.rat >= 0) continue;
        BigInt row[2][2];
        for (int m = 0; m < 2; ++m) {
            const Pt &q = pts[static_cast<size_t>(id[(k + 1 + m) % 3])];
            if (q.rat < 0) {
                row[m][0] = BigInt(q.i[u] - origin.i[u]);
                row[m][1] = BigInt(q.i[v] - origin.i[v]);
            } else {
                const Rat &r = rats[static_cast<size_t>(q.rat)];
                row[m][0] = (u == 0 ? r.x : u == 1 ? r.y : r.z) - BigInt(origin.i[u]) * r.w;
                row[m][1] = (v == 0 ? r.x : v == 1 ? r.y : r.z) - BigInt(origin.i[v]) * r.w;
            }
        }
        return (row[0][0] * row[1][1] - row[0][1] * row[1][0]).sign();
    }
    const Homogeneous ha = homogeneous(*this, a), hb = homogeneous(*this, b), hc = homogeneous(*this, c);
    return det3(ha.c[u], ha.c[v], ha.c[3], hb.c[u], hb.c[v], hb.c[3], hc.c[u], hc.c[v], hc.c[3]).sign();
}

int Pool::compare(int a, int b, int axis) const {
    if (a == b) return 0;
    const Pt &pa = pts[static_cast<size_t>(a)], &pb = pts[static_cast<size_t>(b)];
    if (pa.rat < 0 && pb.rat < 0) return pa.i[axis] < pb.i[axis] ? -1 : pa.i[axis] > pb.i[axis] ? 1 : 0;
    if (pa.c[axis].hi < pb.c[axis].lo) return -1;
    if (pa.c[axis].lo > pb.c[axis].hi) return 1;
    if (pa.pending >= 0 || pb.pending >= 0) {
        // The enclosures of pending points are wide: look again with the intervals of the computed ones.
        settle(a);
        settle(b);
        if (pa.c[axis].hi < pb.c[axis].lo) return -1;
        if (pa.c[axis].lo > pb.c[axis].hi) return 1;
    }
    ++const_cast<Pool *>(this)->exact_fallbacks;
    if (pa.rat < 0) {
        const Rat &rb = rats[static_cast<size_t>(pb.rat)];
        return BigInt::cmp(BigInt(pa.i[axis]) * rb.w, axis == 0 ? rb.x : axis == 1 ? rb.y : rb.z);
    }
    const Rat &ra = rats[static_cast<size_t>(pa.rat)];
    const BigInt &xa = axis == 0 ? ra.x : axis == 1 ? ra.y : ra.z;
    if (pb.rat < 0) return BigInt::cmp(xa, BigInt(pb.i[axis]) * ra.w);
    const Rat &rb = rats[static_cast<size_t>(pb.rat)];
    return BigInt::cmp(xa * rb.w, (axis == 0 ? rb.x : axis == 1 ? rb.y : rb.z) * ra.w);
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
    settle(p);
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
    settle(p);
    const Rat &r = rats[static_cast<size_t>(pt.rat)];
    bool exact;
    out[0] = round_ratio(r.x, r.w, exact);
    out[1] = round_ratio(r.y, r.w, exact);
    out[2] = round_ratio(r.z, r.w, exact);
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite
