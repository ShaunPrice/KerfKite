// Number text exactly as the Python reference writes it. No dependency beyond the C++ standard library:
// this header is shared by the Python extension (studio/_gcode_native.cpp) and the Python-free core.
//
// Fixed-point formatting uses exact integer arithmetic, rounding half to even on the exact binary value,
// as Python formats floats. The C library is not used for it, because its rounding has differed between
// platforms. Squares go through the libm pow call that Python's ** operator makes.
#ifndef KERFKITE_NUMBER_TEXT_HPP
#define KERFKITE_NUMBER_TEXT_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kerfkite {
namespace text {

struct U128 {
    uint64_t hi, lo;
};

// m < 2^53 and k < 2^32, so the product is below 2^85.
inline U128 multiply(uint64_t m, uint32_t k) {
    const uint64_t low = (m & 0xffffffffULL) * k, high = (m >> 32) * k;
    U128 r;
    r.lo = low + (high << 32);
    r.hi = (high >> 32) + (r.lo < low ? 1 : 0);
    return r;
}

// |value| * scale rounded to the nearest integer, ties to even, computed exactly.
// False when the value is not finite or the result does not fit 64 bits.
inline bool scaled_round(double value, uint32_t scale, uint64_t &q) {
    if (!std::isfinite(value)) return false;
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        q = 0;
        return true;
    }
    int exponent;
    const double fraction = std::frexp(magnitude, &exponent);
    const uint64_t mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
    const int shift = 53 - exponent;  // magnitude == mantissa * 2^-shift
    const U128 n = multiply(mantissa, scale);
    if (shift <= 0) {
        if (n.hi || shift <= -64 || (shift < 0 && (n.lo >> (64 + shift)))) return false;
        q = n.lo << -shift;
        return true;
    }
    if (shift >= 128) {
        q = 0;
        return true;
    }
    uint64_t quotient, rem_hi, rem_lo, half_hi, half_lo;
    if (shift >= 64) {
        const int s = shift - 64;
        quotient = s ? n.hi >> s : n.hi;
        rem_hi = s ? n.hi & ((1ULL << s) - 1) : 0;
        rem_lo = n.lo;
        half_hi = s ? 1ULL << (s - 1) : 0;
        half_lo = s ? 0 : 1ULL << 63;
    } else {
        if (n.hi >> shift) return false;
        quotient = (n.lo >> shift) | (n.hi << (64 - shift));
        rem_hi = 0;
        rem_lo = n.lo & ((1ULL << shift) - 1);
        half_hi = 0;
        half_lo = 1ULL << (shift - 1);
    }
    const bool above = rem_hi > half_hi || (rem_hi == half_hi && rem_lo > half_lo);
    const bool tie = rem_hi == half_hi && rem_lo == half_lo;
    if (above || (tie && (quotient & 1))) {
        if (quotient == UINT64_MAX) return false;
        quotient++;
    }
    q = quotient;
    return true;
}

inline uint32_t scale_of(int decimals) {
    static const uint32_t SCALE[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    return SCALE[decimals];
}

// Append value as Python's f'{value:.{decimals}f}' would write it. decimals is 0..9.
inline bool fixed(std::string &out, double value, int decimals) {
    uint64_t q;
    if (!scaled_round(value, scale_of(decimals), q)) return false;
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + q % 10);
        q /= 10;
    } while (q);
    while (n <= decimals) digits[n++] = '0';
    if (std::signbit(value)) out.push_back('-');
    for (int i = n - 1; i >= 0; --i) {
        out.push_back(digits[i]);
        if (i == decimals && decimals) out.push_back('.');
    }
    return true;
}

// Python's round(value, decimals) for decimals 0..3: the double nearest the correctly rounded decimal.
inline bool rounded(double value, int decimals, double &result) {
    uint64_t q;
    if (!scaled_round(value, scale_of(decimals), q) || q > (1ULL << 53)) return false;
    result = std::copysign(static_cast<double>(q) / scale_of(decimals), value);
    return true;
}

// Python's float ** 2 calls libm pow; reading the exponent through a volatile stops the compiler
// replacing the call with a multiply.
inline double square(double value) {
    static volatile double two = 2.0;
    return std::pow(value, two);
}

// Python's repr(float): the shortest digits that read back as the same double, in fixed notation for
// exponents -4..15 and exponent notation otherwise. Used where the reference puts a float in a message.
inline std::string repr(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    char buffer[40];
    int precision = 0;
    for (; precision < 17; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*e", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    if (precision == 17) std::snprintf(buffer, sizeof buffer, "%.16e", value);
    // buffer is [-]d[.ddd]e[+-]XX
    std::string digits;
    const char *p = buffer;
    const bool negative = *p == '-';
    if (negative) ++p;
    for (; *p && *p != 'e'; ++p)
        if (*p != '.') digits.push_back(*p);
    const int exponent = std::atoi(p + 1);
    std::string out = negative ? "-" : "";
    if (exponent < -4 || exponent >= 16) {
        out.push_back(digits[0]);
        if (digits.size() > 1) out.append(".").append(digits, 1, std::string::npos);
        char tail[16];
        std::snprintf(tail, sizeof tail, "e%c%02d", exponent < 0 ? '-' : '+', std::abs(exponent));
        return out + tail;
    }
    if (exponent < 0) {
        out.append("0.").append(static_cast<size_t>(-exponent - 1), '0').append(digits);
        return out;
    }
    const size_t whole = static_cast<size_t>(exponent) + 1;
    if (digits.size() <= whole) {
        out.append(digits).append(whole - digits.size(), '0').append(".0");
        return out;
    }
    out.append(digits, 0, whole).append(".").append(digits, whole, std::string::npos);
    return out;
}

}  // namespace text
}  // namespace kerfkite

#endif
