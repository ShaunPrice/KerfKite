// 128-bit signed integers for the exact predicates of the polygon kernel, and the 256-bit sums of their
// products that the in-circle test needs. Internal.
//
// Coordinates are below 2^39 in magnitude, so a difference is below 2^40, a cross or dot product of two
// differences below 2^81, and such a product times another difference below 2^121. All of that is exact
// in 128 bits. The compiler's own type is used where there is one; Int128 below is the same arithmetic
// from 64-bit pieces for compilers without it (MSVC), and KERFKITE_POLYGON_PORTABLE_INT128 selects it
// anywhere so that it can be tested. Integer arithmetic is exact, so both give the same results.
#ifndef KERFKITE_POLYGON_WIDE_HPP
#define KERFKITE_POLYGON_WIDE_HPP

#include <cstdint>

namespace kerfkite {
namespace polygon {
namespace detail {

// The full product of two unsigned 64-bit values, from 32-bit halves.
inline void multiply_64(std::uint64_t a, std::uint64_t b, std::uint64_t &high, std::uint64_t &low) {
    const std::uint64_t a0 = a & 0xffffffffULL, a1 = a >> 32, b0 = b & 0xffffffffULL, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t middle = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
    low = (p00 & 0xffffffffULL) | (middle << 32);
    high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

#if defined(__SIZEOF_INT128__) && !defined(KERFKITE_POLYGON_PORTABLE_INT128)

__extension__ typedef __int128 Int128;
__extension__ typedef unsigned __int128 NativeU128;

inline Int128 multiply(std::int64_t a, std::int64_t b) { return static_cast<Int128>(a) * b; }
inline std::uint64_t low_half(Int128 v) { return static_cast<std::uint64_t>(static_cast<NativeU128>(v)); }
inline std::uint64_t high_half(Int128 v) { return static_cast<std::uint64_t>(static_cast<NativeU128>(v) >> 64); }
// The value, which the caller knows fits 64 bits.
inline std::int64_t narrow(Int128 v) { return static_cast<std::int64_t>(v); }

#else

// Two's complement in two 64-bit halves.
struct Int128 {
    std::uint64_t low = 0, high = 0;
    Int128() = default;
    Int128(std::int64_t v) : low(static_cast<std::uint64_t>(v)), high(v < 0 ? ~std::uint64_t(0) : 0) {}
    Int128(std::uint64_t h, std::uint64_t l) : low(l), high(h) {}
};

inline bool negative(const Int128 &v) { return (v.high >> 63) != 0; }
inline Int128 operator+(const Int128 &a, const Int128 &b) {
    const std::uint64_t low = a.low + b.low;
    return Int128(a.high + b.high + (low < a.low ? 1 : 0), low);
}
inline Int128 operator-(const Int128 &a) {
    const std::uint64_t low = ~a.low + 1;
    return Int128(~a.high + (low == 0 ? 1 : 0), low);
}
inline Int128 operator-(const Int128 &a, const Int128 &b) { return a + (-b); }
inline bool operator==(const Int128 &a, const Int128 &b) { return a.low == b.low && a.high == b.high; }
inline bool operator!=(const Int128 &a, const Int128 &b) { return !(a == b); }
inline bool operator<(const Int128 &a, const Int128 &b) {
    if (a.high != b.high) return static_cast<std::int64_t>(a.high) < static_cast<std::int64_t>(b.high);
    return a.low < b.low;
}
inline bool operator>(const Int128 &a, const Int128 &b) { return b < a; }
inline bool operator<=(const Int128 &a, const Int128 &b) { return !(b < a); }
inline bool operator>=(const Int128 &a, const Int128 &b) { return !(a < b); }
// The low 128 bits of the product, which is the product when it fits.
inline Int128 operator*(const Int128 &a, const Int128 &b) {
    std::uint64_t high, low;
    multiply_64(a.low, b.low, high, low);
    return Int128(high + a.low * b.high + a.high * b.low, low);
}

inline Int128 multiply(std::int64_t a, std::int64_t b) { return Int128(a) * Int128(b); }
inline std::uint64_t low_half(const Int128 &v) { return v.low; }
inline std::uint64_t high_half(const Int128 &v) { return v.high; }
inline std::int64_t narrow(const Int128 &v) { return static_cast<std::int64_t>(v.low); }

namespace wide_detail {
inline bool less_unsigned(const Int128 &a, const Int128 &b) { return a.high != b.high ? a.high < b.high : a.low < b.low; }
inline Int128 shift_left_one(const Int128 &v) { return Int128((v.high << 1) | (v.low >> 63), v.low << 1); }
inline int bit_length(const Int128 &v) {
    std::uint64_t word = v.high ? v.high : v.low;
    int bits = v.high ? 64 : 0;
    while (word) {
        ++bits;
        word >>= 1;
    }
    return bits;
}
// Unsigned division by subtracting shifted copies of the divisor.
inline Int128 divide_unsigned(const Int128 &n, const Int128 &d) {
    if (n.high == 0 && d.high == 0) return Int128(0, n.low / d.low);
    Int128 quotient, remainder;
    for (int bit = bit_length(n) - 1; bit >= 0; --bit) {
        remainder = shift_left_one(remainder);
        remainder.low |= bit >= 64 ? (n.high >> (bit - 64)) & 1 : (n.low >> bit) & 1;
        quotient = shift_left_one(quotient);
        if (!less_unsigned(remainder, d)) {
            remainder = remainder - d;
            quotient.low |= 1;
        }
    }
    return quotient;
}
}  // namespace wide_detail

// Division rounding towards zero, as the language divides integers.
inline Int128 operator/(const Int128 &a, const Int128 &b) {
    const bool minus = negative(a) != negative(b);
    const Int128 q = wide_detail::divide_unsigned(negative(a) ? -a : a, negative(b) ? -b : b);
    return minus ? -q : q;
}

#endif

inline int sign(const Int128 &v) { return v > Int128(0) ? 1 : v < Int128(0) ? -1 : 0; }

// The nearest double, by the same two steps on every platform so that the result does not depend on how
// a compiler converts its 128-bit type.
inline double to_double(const Int128 &v) {
    const bool minus = v < Int128(0);
    const Int128 magnitude = minus ? Int128(0) - v : v;
    const double value = static_cast<double>(high_half(magnitude)) * 18446744073709551616.0 + static_cast<double>(low_half(magnitude));
    return minus ? -value : value;
}

// The integer below or equal to n / d, for d greater than zero.
inline Int128 floor_divide(const Int128 &n, const Int128 &d) {
    const Int128 q = n / d;
    return (n < Int128(0) && q * d != n) ? q - Int128(1) : q;
}

// n / d rounded to the nearest integer, halves upwards; d is not zero.
inline Int128 round_divide(Int128 n, Int128 d) {
    if (d < Int128(0)) {
        n = Int128(0) - n;
        d = Int128(0) - d;
    }
    return floor_divide(n + n + d, d + d);
}

// The product of the magnitudes of two 128-bit values in four 64-bit pieces, the least significant first.
inline void multiply_magnitudes(const Int128 &x, const Int128 &y, std::uint64_t out[4]) {
    const Int128 mx = x < Int128(0) ? Int128(0) - x : x, my = y < Int128(0) ? Int128(0) - y : y;
    const std::uint64_t x0 = low_half(mx), x1 = high_half(mx), y0 = low_half(my), y1 = high_half(my);
    std::uint64_t h00, l00, h01, l01, h10, l10, h11, l11;
    multiply_64(x0, y0, h00, l00);
    multiply_64(x0, y1, h01, l01);
    multiply_64(x1, y0, h10, l10);
    multiply_64(x1, y1, h11, l11);
    out[0] = l00;
    std::uint64_t carry = 0;
    out[1] = h00 + l01;
    carry += out[1] < h00 ? 1 : 0;
    out[1] += l10;
    carry += out[1] < l10 ? 1 : 0;
    std::uint64_t next = 0;
    out[2] = h01 + h10;
    next += out[2] < h01 ? 1 : 0;
    out[2] += l11;
    next += out[2] < l11 ? 1 : 0;
    out[2] += carry;
    next += out[2] < carry ? 1 : 0;
    out[3] = h11 + next;
}

// The sign of a * b - c * d, where the products may need 256 bits.
inline int compare_products(const Int128 &a, const Int128 &b, const Int128 &c, const Int128 &d) {
    const int left = sign(a) * sign(b), right = sign(c) * sign(d);
    if (left != right) return left > right ? 1 : -1;
    if (left == 0) return 0;
    std::uint64_t p[4], q[4];
    multiply_magnitudes(a, b, p);
    multiply_magnitudes(c, d, q);
    for (int i = 3; i >= 0; --i)
        if (p[i] != q[i]) return (p[i] > q[i]) == (left > 0) ? 1 : -1;
    return 0;
}

// A signed integer of 256 bits in two's complement, the least significant 64-bit piece first. It holds
// sums of products of two 128-bit values: the in-circle test of a Delaunay triangulation is the sign of
// three such products added, which for coordinate differences below 2^40 stays below 2^165. The same
// code runs on every compiler, with or without a 128-bit type.
struct Int256 {
    std::uint64_t piece[4] = {0, 0, 0, 0};
};

// The exact product of two 128-bit values.
inline Int256 wide_product(const Int128 &x, const Int128 &y) {
    Int256 out;
    multiply_magnitudes(x, y, out.piece);
    if ((x < Int128(0)) != (y < Int128(0))) {
        // Minus the magnitude: every bit turned over, and one added. (Minus zero is zero.)
        std::uint64_t carry = 1;
        for (std::uint64_t &piece : out.piece) {
            piece = ~piece + carry;
            carry = (carry && piece == 0) ? 1 : 0;
        }
    }
    return out;
}

// The sum, which the caller knows fits 256 bits.
inline Int256 operator+(const Int256 &a, const Int256 &b) {
    Int256 out;
    std::uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t part = a.piece[i] + carry;
        const std::uint64_t over = part < carry ? 1 : 0;
        out.piece[i] = part + b.piece[i];
        carry = over + (out.piece[i] < part ? 1 : 0);
    }
    return out;
}

inline int sign(const Int256 &v) {
    if (v.piece[3] >> 63) return -1;
    return (v.piece[0] | v.piece[1] | v.piece[2] | v.piece[3]) != 0 ? 1 : 0;
}

}  // namespace detail
}  // namespace polygon
}  // namespace kerfkite

#endif
