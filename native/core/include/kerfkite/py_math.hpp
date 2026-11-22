// Python's math.dist and built-in sum for floats, to the last bit (CPython 3.12 and later).
//
// Neither is a plain formula. math.dist scales by the largest difference, squares and adds without
// rounding error, takes the square root and then corrects it; sum() of floats carries a compensation term
// (Neumaier). The reference's lengths and totals depend on both, so the core repeats them exactly.
// Python 3.10 and 3.11 used simpler arithmetic and can differ from this in the last bit.
#ifndef KERFKITE_PY_MATH_HPP
#define KERFKITE_PY_MATH_HPP

#include <cfloat>
#include <cmath>
#include <cstddef>

namespace kerfkite {
namespace pymath {

struct DoubleLength {
    double hi, lo;
};

// Error-free sum of two doubles, |a| >= |b|.
inline DoubleLength fast_sum(double a, double b) {
    const double z = a + b;
    return {z, b - (z - a)};
}

// Error-free product of two doubles.
inline DoubleLength product(double x, double y) {
    const double z = x * y;
    return {z, std::fma(x, y, -z)};
}

// CPython's vector_norm: the Euclidean norm of non-negative values whose largest is max.
inline double vector_norm(size_t n, double *vec, double max) {
    if (std::isinf(max)) return max;
    if (max == 0.0 || n <= 1) return max;
    int max_e;
    std::frexp(max, &max_e);
    if (max_e < -1023) {
        // ldexp(1.0, -max_e) would overflow: rescale by the smallest normal number first.
        for (size_t i = 0; i < n; i++) vec[i] /= DBL_MIN;
        return DBL_MIN * vector_norm(n, vec, max / DBL_MIN);
    }
    const double scale = std::ldexp(1.0, -max_e);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double x = vec[i] * scale;              // lossless scaling
        const DoubleLength pr = product(x, x);        // lossless squaring
        const DoubleLength sm = fast_sum(csum, pr.hi);  // lossless addition
        csum = sm.hi;
        frac1 += pr.lo;
        frac2 += sm.lo;
    }
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    const DoubleLength pr = product(-h, h);
    const DoubleLength sm = fast_sum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    const double x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);  // differential correction
    return h / scale;
}

// math.hypot(a, b) for finite values.
inline double hypot(double a, double b) {
    double values[2] = {std::fabs(a), std::fabs(b)};
    return vector_norm(2, values, values[0] > values[1] ? values[0] : values[1]);
}

// math.dist((ax, ay), (bx, by)) for finite coordinates.
inline double dist(double ax, double ay, double bx, double by) {
    double diffs[2] = {std::fabs(ax - bx), std::fabs(ay - by)};
    return vector_norm(2, diffs, diffs[0] > diffs[1] ? diffs[0] : diffs[1]);
}

// sum(values) for floats: the first value as it is, then Neumaier's compensated addition.
// An empty sequence gives Python's integer 0, which every use here adds to a float.
template <typename Iterator> double sum(Iterator begin, Iterator end) {
    if (begin == end) return 0.0;
    double result = *begin, c = 0.0;
    Iterator it = begin;
    for (++it; it != end; ++it) {
        const double x = *it, t = result + x;
        if (std::fabs(result) >= std::fabs(x)) c += (result - t) + x;
        else c += (x - t) + result;
        result = t;
    }
    if (c != 0.0 && std::isfinite(c)) result += c;
    return result;
}

}  // namespace pymath
}  // namespace kerfkite

#endif
