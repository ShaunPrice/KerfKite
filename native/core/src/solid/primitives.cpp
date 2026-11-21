// Box, cylinder and sphere, and the trigonometry they and the rotations use.
//
// Circles are faceted by a segment count, as the application asks for them. Sines and cosines come from
// a series evaluated with additions, multiplications and divisions only, in a fixed order: those
// operations are correctly rounded by IEEE 754, so every platform computes the same doubles. The C
// library's sin and cos are not used anywhere in the kernel.
#include <cmath>

#include "mesh.hpp"

namespace kerfkite {
namespace solid {

namespace {

// sin and cos of 0 <= x <= pi/4 radians. The series are cut after x^17 and x^18; the first terms left
// out are below 9e-20 and 4e-21 there, and the nested evaluation loses at most a few units in the last
// place, so both results are within 4e-16 of the true values.
void series(double x, double &s, double &c) {
    const double z = x * x;
    double p = 1 - z / 272;
    p = 1 - z / 210 * p;
    p = 1 - z / 156 * p;
    p = 1 - z / 110 * p;
    p = 1 - z / 72 * p;
    p = 1 - z / 42 * p;
    p = 1 - z / 20 * p;
    p = 1 - z / 6 * p;
    s = x * p;
    double q = 1 - z / 306;
    q = 1 - z / 240 * q;
    q = 1 - z / 182 * q;
    q = 1 - z / 132 * q;
    q = 1 - z / 90 * q;
    q = 1 - z / 56 * q;
    q = 1 - z / 30 * q;
    q = 1 - z / 12 * q;
    c = 1 - z / 2 * q;
}

// The sine and cosine of an angle of 0..45 degrees given as numerator / denominator of a right angle
// when the caller has one (so 30 and 45 degrees are recognised exactly), else as degrees.
void eighth(double degrees, bool is30, bool is45, double &s, double &c) {
    if (degrees == 0) {
        s = 0;
        c = 1;
    } else if (is45 || degrees == 45) {
        s = c = std::sqrt(0.5);
    } else if (is30 || degrees == 30) {
        s = 0.5;
        c = std::sqrt(0.75);
    } else {
        series(degrees * 0.017453292519943295, s, c);
    }
}

void quadrant(int q, bool swap, bool negative, double s, double c, double &sine, double &cosine) {
    if (swap) std::swap(s, c);
    switch (q) {
        case 0: sine = s; cosine = c; break;
        case 1: sine = c; cosine = -s; break;
        case 2: sine = -s; cosine = -c; break;
        default: sine = -c; cosine = s; break;
    }
    if (negative) sine = -sine;
    sine += 0.0;  // no negative zeros
    cosine += 0.0;
}

}  // namespace

void sincos_degrees(double degrees, double &sine, double &cosine) {
    if (!(std::fabs(degrees) <= 1e300)) throw ValueError("An angle must be finite");
    const double a = std::fmod(std::fabs(degrees), 360.0);  // exact
    const int q = a >= 270 ? 3 : a >= 180 ? 2 : a >= 90 ? 1 : 0;
    const double b = a - 90.0 * q;  // exact
    const bool swap = b > 45;
    double s, c;
    eighth(swap ? 90.0 - b : b, false, false, s, c);
    quadrant(q, swap, degrees < 0, s, c, sine, cosine);
}

void sincos_turn(std::int64_t k, std::int64_t n, double &sine, double &cosine) {
    if (n <= 0 || n > (std::int64_t(1) << 40)) throw ValueError("A circle needs between 1 and 2^40 divisions");
    k %= n;
    if (k < 0) k += n;
    const std::int64_t q = 4 * k / n;
    std::int64_t r = 4 * k - q * n;  // the angle past the quadrant is 90 * r / n degrees
    const bool swap = 2 * r > n;
    if (swap) r = n - r;
    double s, c;
    eighth(90.0 * static_cast<double>(r) / static_cast<double>(n), 3 * r == n, 2 * r == n, s, c);
    quadrant(static_cast<int>(q), swap, false, s, c, sine, cosine);
}

namespace {

int64_t round_units(double units) { return static_cast<int64_t>(std::round(units)); }

double positive_units(double mm, const char *what) {
    const double units = mm * kUnitsPerMm;
    if (!(units >= 0.5) || !(units <= static_cast<double>(detail::kMaxCoord))) throw ValueError(std::string(what) + " must be positive and at most 100000 mm");
    return units;
}

int segment_count(int segments) {
    if (segments < 3 || segments > 65536) throw ValueError("Circles need 3 to 65536 segments");
    return segments;
}

}  // namespace

Solid box(const Vec3 &size, bool center) {
    const double s[3] = {size.x, size.y, size.z};
    int64_t lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        const int64_t length = round_units(positive_units(s[k], "Box dimensions"));
        lo[k] = center ? -(length / 2) : 0;
        hi[k] = lo[k] + length;
    }
    std::vector<GridPoint> v(8);
    for (size_t i = 0; i < 8; ++i) v[i] = GridPoint{(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
    std::vector<Triangle> t = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
                               {2, 7, 3}, {2, 6, 7}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    return detail::make_solid(std::move(v), std::move(t), true, "box");
}

Solid cylinder(double height, double radius_low, double radius_high, int segments, bool center) {
    const uint32_t n = static_cast<uint32_t>(segment_count(segments));
    const int64_t h = round_units(positive_units(height, "Cylinder height"));
    if (!(radius_low >= 0) || !(radius_high >= 0) || !(radius_low > 0 || radius_high > 0))
        throw ValueError("Cylinder radii must not be negative, and one must be positive");
    const double radius[2] = {radius_low > 0 ? positive_units(radius_low, "Cylinder radius") : 0,
                              radius_high > 0 ? positive_units(radius_high, "Cylinder radius") : 0};
    const int64_t z[2] = {center ? -(h / 2) : 0, (center ? -(h / 2) : 0) + h};
    std::vector<GridPoint> v;
    uint32_t first[2];
    for (int end = 0; end < 2; ++end) {
        first[end] = static_cast<uint32_t>(v.size());
        if (radius[end] == 0) {
            v.push_back(GridPoint{0, 0, z[end]});
            continue;
        }
        for (uint32_t i = 0; i < n; ++i) {
            double s, c;
            sincos_turn(i, n, s, c);
            v.push_back(GridPoint{round_units(radius[end] * c), round_units(radius[end] * s), z[end]});
        }
    }
    std::vector<Triangle> t;
    const bool low = radius[0] > 0, high = radius[1] > 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t j = (i + 1) % n;
        if (low && high) {
            t.push_back({first[0] + i, first[0] + j, first[1] + j});
            t.push_back({first[0] + i, first[1] + j, first[1] + i});
        } else if (low) {
            t.push_back({first[0] + i, first[0] + j, first[1]});
        } else {
            t.push_back({first[0], first[1] + j, first[1] + i});
        }
    }
    for (uint32_t i = 1; i + 1 < n; ++i) {
        if (low) t.push_back({first[0], first[0] + i + 1, first[0] + i});
        if (high) t.push_back({first[1], first[1] + i, first[1] + i + 1});
    }
    return detail::make_solid(std::move(v), std::move(t), false, "cylinder");
}

Solid sphere(double radius, int segments) {
    const uint32_t n = (static_cast<uint32_t>(segment_count(segments)) + 3) / 4 * 4;
    const uint32_t stacks = n / 2;
    const double r = positive_units(radius, "Sphere radius");
    std::vector<GridPoint> v;
    v.push_back(GridPoint{0, 0, round_units(r)});
    for (uint32_t j = 1; j < stacks; ++j) {
        double st, ct;
        sincos_turn(j, 2 * static_cast<int64_t>(stacks), st, ct);
        const double ring = r * st;
        const int64_t z = round_units(r * ct);
        for (uint32_t i = 0; i < n; ++i) {
            double s, c;
            sincos_turn(i, n, s, c);
            v.push_back(GridPoint{round_units(ring * c), round_units(ring * s), z});
        }
    }
    v.push_back(GridPoint{0, 0, -round_units(r)});
    const uint32_t bottom = static_cast<uint32_t>(v.size() - 1);
    std::vector<Triangle> t;
    const auto at = [n](uint32_t ring, uint32_t i) { return 1 + (ring - 1) * n + i % n; };
    for (uint32_t i = 0; i < n; ++i) {
        t.push_back({0, at(1, i), at(1, i + 1)});
        for (uint32_t j = 1; j + 1 < stacks; ++j) {
            t.push_back({at(j, i), at(j + 1, i), at(j + 1, i + 1)});
            t.push_back({at(j, i), at(j + 1, i + 1), at(j, i + 1)});
        }
        t.push_back({bottom, at(stacks - 1, i + 1), at(stacks - 1, i)});
    }
    return detail::make_solid(std::move(v), std::move(t), false, "sphere");
}

}  // namespace solid
}  // namespace kerfkite
