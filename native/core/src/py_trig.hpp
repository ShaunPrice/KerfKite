// Python's math.atan2, math.remainder and float % as CPython computes them, and single library calls for
// the sine and cosine. Internal to the core.
//
// fmod and the IEEE remainder are exact, so they agree everywhere. sin, cos and atan2 come from the
// platform's maths library, as they do in Python: on one machine the two agree, but the last bit may
// differ between platforms.
#ifndef KERFKITE_PY_TRIG_HPP
#define KERFKITE_PY_TRIG_HPP

#include <cmath>

#if defined(_MSC_VER)
#define KERFKITE_TRIG_NOINLINE __declspec(noinline)
#else
#define KERFKITE_TRIG_NOINLINE __attribute__((noinline))
#endif

namespace kerfkite {
namespace pytrig {

const double PI = 3.141592653589793;  // math.pi

// One library call each, as Python makes them: kept out of line so that a compiler does not merge a sine
// and a cosine of the same angle into a single combined call.
KERFKITE_TRIG_NOINLINE inline double sine(double x) { return std::sin(x); }
KERFKITE_TRIG_NOINLINE inline double cosine(double x) { return std::cos(x); }
KERFKITE_TRIG_NOINLINE inline double library_atan2(double y, double x) { return std::atan2(y, x); }
KERFKITE_TRIG_NOINLINE inline double tangent(double x) { return std::tan(x); }

// math.atan2 for finite arguments: CPython answers a zero y itself and calls the library for the rest.
inline double atan2(double y, double x) {
    if (y == 0.0) return std::copysign(std::copysign(1.0, x) == 1.0 ? 0.0 : PI, y);
    return library_atan2(y, x);
}

// math.remainder(x, y) for finite arguments and a non-zero y: the IEEE remainder, in CPython's steps.
inline double remainder(double x, double y) {
    const double absx = std::fabs(x), absy = std::fabs(y);
    const double m = std::fmod(absx, absy);
    const double c = absy - m;
    double r;
    if (m < c) r = m;
    else if (m > c) r = m - absy;
    else r = m - 2.0 * std::fmod(0.5 * (absx - m), absy);
    return std::copysign(1.0, x) * r;
}

// x % y for finite floats and a non-zero y: the result takes the sign of y.
inline double modulo(double x, double y) {
    double mod = std::fmod(x, y);
    if (mod != 0.0) {
        if ((y < 0) != (mod < 0)) mod += y;
    } else {
        mod = std::copysign(0.0, y);
    }
    return mod;
}

}  // namespace pytrig
}  // namespace kerfkite

#endif
