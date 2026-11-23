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
#include <vector>

#include "kerfkite/decimal.hpp"

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

// Append value as Python's f'{value:.{decimals}f}' would write it. False, with nothing appended, when
// decimals is outside 0..9 or the value is outside the exact range.
inline bool fixed(std::string &out, double value, int decimals) {
    uint64_t q;
    if (decimals < 0 || decimals > 9 || !scaled_round(value, scale_of(decimals), q)) return false;
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

// f'{value:.{decimals}f}' for every double and decimals 0..9: fixed() where it can, and otherwise the exact value
// in a number of 32-bit words, scaled by 10^decimals and rounded half to even, then written in decimal. 'inf',
// '-inf' and 'nan' as Python writes them.
inline std::string fixed_exact(double value, int decimals) {
    std::string out;
    if (fixed(out, value, decimals)) return out;
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    if (decimals < 0) decimals = 0;
    // |value| = mantissa * 2^shift exactly.
    int exponent;
    const double fraction = std::frexp(std::fabs(value), &exponent);
    uint64_t mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
    int shift = exponent - 53;
    std::vector<uint32_t> n = {static_cast<uint32_t>(mantissa), static_cast<uint32_t>(mantissa >> 32)};
    const auto times = [&](uint32_t k) {
        uint64_t carry = 0;
        for (uint32_t &word : n) {
            const uint64_t product = static_cast<uint64_t>(word) * k + carry;
            word = static_cast<uint32_t>(product);
            carry = product >> 32;
        }
        if (carry) n.push_back(static_cast<uint32_t>(carry));
    };
    for (int i = 0; i < decimals; ++i) times(10);
    if (shift > 0) {
        for (; shift >= 32; shift -= 32) n.insert(n.begin(), 0u);
        if (shift) times(1u << shift);
    } else if (shift < 0) {
        // Divide by 2^-shift, rounding half to even on the bits shifted out.
        const int bits = -shift;
        const auto bit = [&](int i) -> bool { return i / 32 < static_cast<int>(n.size()) && ((n[static_cast<size_t>(i / 32)] >> (i % 32)) & 1u); };
        const bool half = bit(bits - 1);
        bool below = false;
        for (int i = 0; i < bits - 1 && !below; ++i) below = bit(i);
        std::vector<uint32_t> q((n.size() * 32 > static_cast<size_t>(bits) ? n.size() * 32 - static_cast<size_t>(bits) : 0) / 32 + 1, 0u);
        for (size_t i = 0; i < q.size() * 32; ++i)
            if (bit(static_cast<int>(i) + bits)) q[i / 32] |= 1u << (i % 32);
        n = q;
        if (half && (below || (n[0] & 1u))) {
            for (uint32_t &word : n)
                if (++word != 0) break;
            if (n.back() == 0) n.push_back(1u);
        }
    }
    // The decimal digits, nine at a time from the bottom.
    std::string digits;
    for (;;) {
        while (!n.empty() && n.back() == 0) n.pop_back();
        if (n.empty()) break;
        uint64_t remainder = 0;
        for (size_t i = n.size(); i-- > 0;) {
            const uint64_t part = (remainder << 32) | n[i];
            n[i] = static_cast<uint32_t>(part / 1000000000u);
            remainder = part % 1000000000u;
        }
        for (int k = 0; k < 9; ++k) {
            digits.push_back(static_cast<char>('0' + remainder % 10));
            remainder /= 10;
        }
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    while (digits.size() <= static_cast<size_t>(decimals)) digits.push_back('0');
    if (std::signbit(value)) out.push_back('-');
    for (size_t i = digits.size(); i-- > 0;) {
        out.push_back(digits[i]);
        if (i == static_cast<size_t>(decimals) && decimals) out.push_back('.');
    }
    return out;
}

// Python's round(value, decimals) for decimals 0..9: the double nearest the correctly rounded decimal. q and the
// scale are exact doubles, so their quotient, correctly rounded, is that double. False beyond 2^53 units.
inline bool rounded(double value, int decimals, double &result) {
    uint64_t q;
    if (decimals < 0 || decimals > 9 || !scaled_round(value, scale_of(decimals), q) || q > (1ULL << 53)) return false;
    result = std::copysign(static_cast<double>(q) / scale_of(decimals), value);
    return true;
}

// round(value, decimals) for every double and decimals 0..9. Beyond 2^53 units the spacing of doubles is wider than
// 10^-decimals, so the rounded decimal is nearer value than any other double: Python's answer is value itself, as
// it is for an infinity or a NaN.
inline double rounded_exact(double value, int decimals) {
    double result = 0;
    return rounded(value, decimals, result) ? result : value;
}

// Python's float ** 2 calls libm pow; reading the exponent through a volatile stops the compiler
// replacing the call with a multiply.
inline double square(double value) {
    static volatile double two = 2.0;
    return std::pow(value, two);
}

// Python's repr(float), appended to out: the shortest digits that read back as the same double, in fixed
// notation for exponents -4..15 and exponent notation otherwise. Used where the reference puts a float in
// a message, and for every float the core writes as JSON. The digits come from decimal.hpp, not the C
// library.
inline void append_repr(std::string &out, double value) {
    if (std::isnan(value)) {
        out.append("nan");
        return;
    }
    if (std::signbit(value)) out.push_back('-');
    if (std::isinf(value)) {
        out.append("inf");
        return;
    }
    if (value == 0) {
        out.append("0.0");
        return;
    }
    char digits[17];
    int point;
    const size_t count = static_cast<size_t>(shortest_digits(std::fabs(value), digits, point));
    const int exponent = point - 1;  // of the first digit
    if (exponent < -4 || exponent >= 16) {
        out.push_back(digits[0]);
        if (count > 1) out.append(".").append(digits + 1, count - 1);
        out.push_back('e');
        out.push_back(exponent < 0 ? '-' : '+');
        const int size = exponent < 0 ? -exponent : exponent;
        if (size >= 100) out.push_back(static_cast<char>('0' + size / 100));
        out.push_back(static_cast<char>('0' + size / 10 % 10));
        out.push_back(static_cast<char>('0' + size % 10));
        return;
    }
    if (exponent < 0) {
        out.append("0.").append(static_cast<size_t>(-exponent - 1), '0').append(digits, count);
        return;
    }
    const size_t whole = static_cast<size_t>(exponent) + 1;
    if (count <= whole) {
        out.append(digits, count).append(whole - count, '0').append(".0");
        return;
    }
    out.append(digits, whole).append(".").append(digits + whole, count - whole);
}

inline std::string repr(double value) {
    std::string out;
    append_repr(out, value);
    return out;
}

}  // namespace text
}  // namespace kerfkite

#endif
