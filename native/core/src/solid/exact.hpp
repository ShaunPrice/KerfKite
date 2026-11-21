// Exact arithmetic for the solid kernel: a 128-bit integer, a fixed-capacity big integer, an interval
// of doubles, and points that are either on the integer grid or exact rationals.
//
// Every decision about topology is made with these. Intervals only ever answer "certainly positive",
// "certainly negative" or "cannot tell"; a predicate that cannot tell is evaluated again with integers,
// so the answer is the exact sign on every platform. Nothing here depends on the C library's
// mathematical functions.
#ifndef KERFKITE_SOLID_EXACT_HPP
#define KERFKITE_SOLID_EXACT_HPP

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace kerfkite {
namespace solid {
namespace detail {

// The largest coordinate magnitude, in grid units: 100 m at 1 nm. It is below 2^37, which the bit
// budgets of the predicates below rely on.
constexpr int64_t kMaxCoord = 100000000000LL;

// 64 x 64 -> 128 bits, unsigned. The compiler's 128-bit type is used where there is one; the portable
// branch gives the same integers, and KERFKITE_SOLID_PORTABLE_ARITHMETIC selects it for testing.
inline void umul64(uint64_t a, uint64_t b, uint64_t &hi, uint64_t &lo) {
#if defined(__SIZEOF_INT128__) && !defined(KERFKITE_SOLID_PORTABLE_ARITHMETIC)
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    hi = static_cast<uint64_t>(p >> 64);
    lo = static_cast<uint64_t>(p);
#else
    const uint64_t a0 = a & 0xffffffffULL, a1 = a >> 32, b0 = b & 0xffffffffULL, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
    lo = (p00 & 0xffffffffULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}

// A signed 128-bit integer in two's complement. Callers keep values inside the range; the bounds are
// stated where each use is.
struct I128 {
    uint64_t lo;
    int64_t hi;
    I128() : lo(0), hi(0) {}
    I128(int64_t v) : lo(static_cast<uint64_t>(v)), hi(v < 0 ? -1 : 0) {}  // NOLINT: implicit by design
    I128(int64_t h, uint64_t l) : lo(l), hi(h) {}

    int sign() const { return hi < 0 ? -1 : (hi == 0 && lo == 0) ? 0 : 1; }
    bool zero() const { return hi == 0 && lo == 0; }
    I128 operator-() const {
        const uint64_t l = ~lo + 1;
        const uint64_t h = ~static_cast<uint64_t>(hi) + (l == 0 ? 1 : 0);
        return I128(static_cast<int64_t>(h), l);
    }
    I128 operator+(const I128 &o) const {
        const uint64_t l = lo + o.lo;
        const uint64_t h = static_cast<uint64_t>(hi) + static_cast<uint64_t>(o.hi) + (l < lo ? 1 : 0);
        return I128(static_cast<int64_t>(h), l);
    }
    I128 operator-(const I128 &o) const { return *this + (-o); }
    bool operator==(const I128 &o) const { return hi == o.hi && lo == o.lo; }
    bool operator<(const I128 &o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
    I128 abs() const { return hi < 0 ? -*this : *this; }

    static I128 mul(int64_t a, int64_t b) {
        const bool negative = (a < 0) != (b < 0);
        const uint64_t ua = a < 0 ? ~static_cast<uint64_t>(a) + 1 : static_cast<uint64_t>(a);
        const uint64_t ub = b < 0 ? ~static_cast<uint64_t>(b) + 1 : static_cast<uint64_t>(b);
        uint64_t h, l;
        umul64(ua, ub, h, l);
        const I128 r(static_cast<int64_t>(h), l);
        return negative ? -r : r;
    }
    // The low 128 bits of this * m; exact when the product fits.
    I128 times(int64_t m) const {
        const bool negative = (hi < 0) != (m < 0);
        const I128 a = abs();
        const uint64_t um = m < 0 ? ~static_cast<uint64_t>(m) + 1 : static_cast<uint64_t>(m);
        uint64_t h, l;
        umul64(a.lo, um, h, l);
        h += static_cast<uint64_t>(a.hi) * um;
        const I128 r(static_cast<int64_t>(h), l);
        return negative ? -r : r;
    }
    // Nearest double, for measures (never for decisions).
    double to_double() const {
        const I128 a = abs();
        const double v = static_cast<double>(static_cast<uint64_t>(a.hi)) * 18446744073709551616.0 + static_cast<double>(a.lo);
        return hi < 0 ? -v : v;
    }
};

// Thrown when a value outgrows the fixed capacity. The capacity covers every predicate on inputs inside
// kMaxCoord (see docs/SOLID-KERNEL.md), so reaching this is a defect, not an input error.
struct ArithmeticRange : std::logic_error {
    ArithmeticRange() : std::logic_error("solid kernel: exact arithmetic exceeded its capacity") {}
};

// A signed integer of up to 1536 bits, sign and magnitude, 32-bit limbs, no allocation.
struct BigInt {
    enum { CAP = 48 };
    uint32_t d[CAP];
    int n;      // limbs in use; 0 is the value zero
    bool neg;   // never set on zero

    BigInt() : n(0), neg(false) {}
    BigInt(int64_t v) : n(0), neg(v < 0) {  // NOLINT: implicit by design
        uint64_t u = v < 0 ? ~static_cast<uint64_t>(v) + 1 : static_cast<uint64_t>(v);
        while (u) {
            d[n++] = static_cast<uint32_t>(u);
            u >>= 32;
        }
    }
    BigInt(const I128 &v) : n(0), neg(v.hi < 0) {  // NOLINT: implicit by design
        const I128 a = v.abs();
        uint64_t parts[2] = {a.lo, static_cast<uint64_t>(a.hi)};
        d[0] = static_cast<uint32_t>(parts[0]);
        d[1] = static_cast<uint32_t>(parts[0] >> 32);
        d[2] = static_cast<uint32_t>(parts[1]);
        d[3] = static_cast<uint32_t>(parts[1] >> 32);
        n = 4;
        while (n && !d[n - 1]) --n;
        if (!n) neg = false;
    }
    BigInt(const BigInt &o) : n(o.n), neg(o.neg) { std::memcpy(d, o.d, sizeof(uint32_t) * static_cast<size_t>(o.n)); }
    BigInt &operator=(const BigInt &o) {
        if (this != &o) {
            n = o.n;
            neg = o.neg;
            std::memcpy(d, o.d, sizeof(uint32_t) * static_cast<size_t>(o.n));
        }
        return *this;
    }

    int sign() const { return n == 0 ? 0 : neg ? -1 : 1; }
    bool zero() const { return n == 0; }
    BigInt operator-() const {
        BigInt r(*this);
        if (r.n) r.neg = !r.neg;
        return r;
    }

    static int cmp_mag(const BigInt &a, const BigInt &b) {
        if (a.n != b.n) return a.n < b.n ? -1 : 1;
        for (int i = a.n - 1; i >= 0; --i)
            if (a.d[i] != b.d[i]) return a.d[i] < b.d[i] ? -1 : 1;
        return 0;
    }
    static void add_mag(const BigInt &a, const BigInt &b, BigInt &r) {
        const BigInt &big = a.n >= b.n ? a : b, &small = a.n >= b.n ? b : a;
        uint64_t carry = 0;
        int i = 0;
        for (; i < small.n; ++i) {
            carry += static_cast<uint64_t>(big.d[i]) + small.d[i];
            r.d[i] = static_cast<uint32_t>(carry);
            carry >>= 32;
        }
        for (; i < big.n; ++i) {
            carry += big.d[i];
            r.d[i] = static_cast<uint32_t>(carry);
            carry >>= 32;
        }
        r.n = big.n;
        if (carry) {
            if (r.n >= CAP) throw ArithmeticRange();
            r.d[r.n++] = static_cast<uint32_t>(carry);
        }
    }
    // r = a - b for |a| >= |b|.
    static void sub_mag(const BigInt &a, const BigInt &b, BigInt &r) {
        int64_t borrow = 0;
        int i = 0;
        for (; i < b.n; ++i) {
            int64_t v = static_cast<int64_t>(a.d[i]) - b.d[i] - borrow;
            borrow = v < 0;
            if (v < 0) v += 4294967296LL;
            r.d[i] = static_cast<uint32_t>(v);
        }
        for (; i < a.n; ++i) {
            int64_t v = static_cast<int64_t>(a.d[i]) - borrow;
            borrow = v < 0;
            if (v < 0) v += 4294967296LL;
            r.d[i] = static_cast<uint32_t>(v);
        }
        r.n = a.n;
        while (r.n && !r.d[r.n - 1]) --r.n;
    }
    BigInt operator+(const BigInt &o) const {
        BigInt r;
        if (neg == o.neg) {
            add_mag(*this, o, r);
            r.neg = neg && r.n;
        } else {
            const int c = cmp_mag(*this, o);
            if (c == 0) return r;
            if (c > 0) {
                sub_mag(*this, o, r);
                r.neg = neg;
            } else {
                sub_mag(o, *this, r);
                r.neg = o.neg;
            }
        }
        return r;
    }
    BigInt operator-(const BigInt &o) const { return *this + (-o); }
    BigInt operator*(const BigInt &o) const {
        BigInt r;
        if (!n || !o.n) return r;
        if (n + o.n > CAP) throw ArithmeticRange();
        std::memset(r.d, 0, sizeof(uint32_t) * static_cast<size_t>(n + o.n));
        for (int i = 0; i < n; ++i) {
            uint64_t carry = 0;
            const uint64_t a = d[i];
            for (int j = 0; j < o.n; ++j) {
                carry += a * o.d[j] + r.d[i + j];
                r.d[i + j] = static_cast<uint32_t>(carry);
                carry >>= 32;
            }
            r.d[i + o.n] = static_cast<uint32_t>(carry);
        }
        r.n = n + o.n;
        while (r.n && !r.d[r.n - 1]) --r.n;
        r.neg = neg != o.neg;
        return r;
    }
    static int cmp(const BigInt &a, const BigInt &b) {
        if (a.sign() != b.sign()) return a.sign() < b.sign() ? -1 : 1;
        const int c = cmp_mag(a, b);
        return a.neg ? -c : c;
    }
    // The leading 64 bits as a double in [2^63, 2^64] (the bits below are dropped, then the conversion
    // rounds) and the exponent e with |value| ~ mantissa * 2^e, to within a relative 2^-52. Zero gives 0.
    void leading(double &mantissa, int &exponent) const {
        if (!n) {
            mantissa = 0;
            exponent = 0;
            return;
        }
        const int limbs = n >= 2 ? 2 : 1;
        uint64_t top = n >= 2 ? (static_cast<uint64_t>(d[n - 1]) << 32) | d[n - 2] : d[0];
        int shift = 0;
        while (!(top >> 63)) {
            top <<= 1;
            ++shift;
        }
        if (shift && n >= 3) {
            const uint64_t below = (static_cast<uint64_t>(d[n - 3]) << 32) | (n >= 4 ? d[n - 4] : 0u);
            top |= below >> (64 - shift);
        }
        mantissa = static_cast<double>(top);
        exponent = 32 * (n - limbs) - shift;
    }
    // Nearest-ish double for measures.
    double to_double() const {
        double m;
        int e;
        leading(m, e);
        const double v = std::ldexp(m, e);
        return neg ? -v : v;
    }
};

// A closed interval of doubles that is guaranteed to contain the real result of the operations applied.
// Each operation is computed in round-to-nearest and then widened by more than the rounding error.
struct Interval {
    double lo, hi;
    Interval() : lo(0), hi(0) {}
    Interval(double v) : lo(v), hi(v) {}  // NOLINT: implicit by design
    Interval(double l, double h) : lo(l), hi(h) {}
    bool exact_zero() const { return lo == 0 && hi == 0; }
    // 1, -1, 0 (exactly zero) or 2 (cannot tell).
    int sign() const { return lo > 0 ? 1 : hi < 0 ? -1 : exact_zero() ? 0 : 2; }
};
// Round-to-nearest is within half a unit in the last place, at most |x| * 2^-53; moving by |x| * 2^-51
// therefore passes the real value. The constant covers results too small for that product.
inline double widen_down(double x) { return x - (std::fabs(x) * 0x1p-51 + 0x1p-1000); }
inline double widen_up(double x) { return x + (std::fabs(x) * 0x1p-51 + 0x1p-1000); }
inline Interval operator+(const Interval &a, const Interval &b) {
    if (a.exact_zero()) return b;
    if (b.exact_zero()) return a;
    return Interval(widen_down(a.lo + b.lo), widen_up(a.hi + b.hi));
}
inline Interval operator-(const Interval &a, const Interval &b) {
    if (b.exact_zero()) return a;
    return Interval(widen_down(a.lo - b.hi), widen_up(a.hi - b.lo));
}
inline Interval operator*(const Interval &a, const Interval &b) {
    if (a.exact_zero() || b.exact_zero()) return Interval();
    const double p1 = a.lo * b.lo, p2 = a.lo * b.hi, p3 = a.hi * b.lo, p4 = a.hi * b.hi;
    double lo = p1, hi = p1;
    if (p2 < lo) lo = p2;
    if (p3 < lo) lo = p3;
    if (p4 < lo) lo = p4;
    if (p2 > hi) hi = p2;
    if (p3 > hi) hi = p3;
    if (p4 > hi) hi = p4;
    return Interval(widen_down(lo), widen_up(hi));
}

// An exact rational point: (x, y, z) / w with w > 0.
struct Rat {
    BigInt x, y, z, w;
};

// A point of an arrangement. Grid points carry their integers; the others index a Rat. The intervals
// enclose the coordinates and are exact (lo == hi) for grid points.
struct Pt {
    int64_t i[3];
    int rat;
    Interval c[3];
};

// An integer plane a*x + b*y + c*z = d. For the plane of a grid triangle the normal components are below
// 2^78 and d below 2^117.
struct Plane {
    I128 n[3];
    I128 d;
};

Plane plane_of(const int64_t *a, const int64_t *b, const int64_t *c);
// The plane through the segment p-q that contains the direction of coordinate axis k.
Plane plane_through_edge(const int64_t *p, const int64_t *q, int k);
// n . p - d, exact (below 2^119 for a triangle's plane and a grid point).
I128 plane_value(const Plane &pl, const int64_t *p);

// Exact predicates on grid points.
// The sign of det[b-a; c-a; d-a]: positive when d is on the side the normal (b-a)x(c-a) points to.
int orient3d_grid(const int64_t *a, const int64_t *b, const int64_t *c, const int64_t *d);
I128 orient3d_value(const int64_t *a, const int64_t *b, const int64_t *c, const int64_t *d);
// The sign of the cross product (b-a)x(c-a) in the plane of coordinates u and v.
int orient2d_grid(const int64_t *a, const int64_t *b, const int64_t *c, int u, int v);

// The points of one computation. Grid points are unique by position; rational points are unique by
// value (a rational that is in fact on the grid becomes a grid point).
class Pool {
public:
    Pool();
    std::vector<Pt> pts;
    std::vector<Rat> rats;

    int add_grid(int64_t x, int64_t y, int64_t z);
    int find_grid(int64_t x, int64_t y, int64_t z) const;  // -1 when absent
    int add_rat(const Rat &r);
    // Where the line through grid points p and q meets a plane that separates them.
    int line_plane(int p, int q, const Plane &pl);
    // The common point of three planes; -1 when they have none or more than one.
    int three_planes(const Plane &a, const Plane &b, const Plane &c);

    bool grid(int p) const { return pts[static_cast<size_t>(p)].rat < 0; }
    int orient3d(int a, int b, int c, int d) const;
    int orient2d(int a, int b, int c, int u, int v) const;
    int compare(int a, int b, int axis) const;  // -1, 0, 1 by that coordinate
    int compare_lex(int a, int b) const;
    int side(const Plane &pl, int p) const;  // the sign of n . p - d
    // The nearest grid point (ties towards +infinity), coordinate by coordinate.
    void round(int p, int64_t out[3]) const;

    size_t exact_fallbacks = 0;  // predicates the intervals could not decide

private:
    struct Key {
        int64_t x, y, z;
        bool operator==(const Key &o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct Hash {
        size_t operator()(const Key &k) const {
            uint64_t h = static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<uint64_t>(k.y) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
            h ^= static_cast<uint64_t>(k.z) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
            return static_cast<size_t>(h);
        }
    };
    struct RatLess {
        const Pool *pool;
        bool operator()(int a, int b) const { return pool->compare_lex(a, b) < 0; }
    };
    std::unordered_map<Key, int, Hash> grid_index_;  // looked up, never iterated
    std::set<int, RatLess> rat_index_;
};

// The interval of num / den for den > 0, and the nearest integer to it (ties towards +infinity) with
// whether the division is exact.
Interval ratio_interval(const BigInt &num, const BigInt &den);
int64_t round_ratio(const BigInt &num, const BigInt &den, bool &exact);

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
