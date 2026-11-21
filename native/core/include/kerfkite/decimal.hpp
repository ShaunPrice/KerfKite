// Decimal text to double and back, exactly, with nothing but integer arithmetic.
//
// parse_double gives the double nearest the decimal it reads, ties to even, as IEEE 754 asks and as Python's
// float() does. shortest_digits gives the fewest digits that read back as the same double, and of those
// the nearest, which is what Python's repr() writes. Neither uses the C library's conversions, so the C
// locale and the platform's strtod and printf have no part in the result.
//
// The slow paths work on whole numbers of a few thousand bits: a decimal is D x 10^E, and the nearest
// double is found from the exact quotient or product; the digits of a double come from the exact
// interval of decimals that round to it (the free-format method of Steele and White, as refined by
// Burger and Dybvig, "Printing Floating-Point Numbers Quickly and Accurately", 1996). The short cuts do
// the same exact arithmetic in 64-bit words where the numbers are small enough to fit, or use one
// correctly rounded machine operation on exact operands; they give the same answers and are checked
// against the slow paths.
//
// No dependency beyond the C++ standard library: this header is used by the Python extension and the core.
#ifndef KERFKITE_DECIMAL_HPP
#define KERFKITE_DECIMAL_HPP

#include <cfloat>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace kerfkite {
namespace text {

// A non-negative whole number of up to 4,352 bits, in 32-bit limbs with the least significant first. The
// conversions below bound their operands well inside that; going past it is a programming error.
class Big {
public:
    static constexpr int LIMBS = 136;

    Big() : size_(0) {}
    explicit Big(uint64_t value) : size_(0) {
        if (value) limb_[size_++] = static_cast<uint32_t>(value);
        if (value >> 32) limb_[size_++] = static_cast<uint32_t>(value >> 32);
    }
    Big(const Big &other) : size_(other.size_) { std::memcpy(limb_, other.limb_, sizeof(uint32_t) * static_cast<size_t>(size_)); }
    Big &operator=(const Big &other) {
        size_ = other.size_;
        std::memmove(limb_, other.limb_, sizeof(uint32_t) * static_cast<size_t>(size_));
        return *this;
    }

    bool zero() const { return size_ == 0; }

    // The number of bits up to and including the highest set one; 0 for zero.
    int bits() const {
        if (!size_) return 0;
        int n = (size_ - 1) * 32;
        for (uint32_t top = limb_[size_ - 1]; top; top >>= 1) ++n;
        return n;
    }

    // this = this * factor + carry
    void multiply_add(uint32_t factor, uint32_t carry) {
        uint64_t over = carry;
        for (int i = 0; i < size_; ++i) {
            over += static_cast<uint64_t>(limb_[i]) * factor;
            limb_[i] = static_cast<uint32_t>(over);
            over >>= 32;
        }
        if (over) push(static_cast<uint32_t>(over));
    }

    void multiply_pow10(int power) {
        for (; power >= 9; power -= 9) multiply_add(1000000000u, 0);
        static const uint32_t SMALL[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
        if (power > 0) multiply_add(SMALL[power], 0);
    }

    void shift_left(int count) {
        if (!size_ || count <= 0) return;
        const int whole = count / 32, part = count % 32;
        if (size_ + whole + 1 > LIMBS) overflow();
        if (part) {
            limb_[size_ + whole] = 0;
            for (int i = size_ - 1; i >= 0; --i) {
                limb_[i + whole + 1] |= limb_[i] >> (32 - part);
                limb_[i + whole] = limb_[i] << part;
            }
        } else {
            for (int i = size_ - 1; i >= 0; --i) limb_[i + whole] = limb_[i];
        }
        for (int i = 0; i < whole; ++i) limb_[i] = 0;
        size_ += whole + (part ? 1 : 0);
        trim();
    }

    // -1, 0 or 1 as this is below, equal to or above other.
    int compare(const Big &other) const {
        if (size_ != other.size_) return size_ < other.size_ ? -1 : 1;
        for (int i = size_ - 1; i >= 0; --i)
            if (limb_[i] != other.limb_[i]) return limb_[i] < other.limb_[i] ? -1 : 1;
        return 0;
    }

    // this -= other, which must not be the larger.
    void subtract(const Big &other) {
        uint64_t borrow = 0;
        for (int i = 0; i < size_; ++i) {
            const uint64_t take = (i < other.size_ ? other.limb_[i] : 0) + borrow;
            borrow = limb_[i] < take ? 1 : 0;
            limb_[i] = static_cast<uint32_t>(limb_[i] - take);
        }
        trim();
    }

    void add(const Big &other) {
        uint64_t carry = 0;
        const int most = size_ > other.size_ ? size_ : other.size_;
        for (int i = 0; i < most; ++i) {
            carry += (i < size_ ? limb_[i] : 0);
            carry += (i < other.size_ ? other.limb_[i] : 0);
            if (i >= size_) push(0);
            limb_[i] = static_cast<uint32_t>(carry);
            carry >>= 32;
        }
        if (carry) push(static_cast<uint32_t>(carry));
    }

    // The 64 bits from the top one down, or the whole number when it has no more than 64. lower is the
    // number of bits below those, and sticky whether any of them is set.
    uint64_t top64(int &lower, bool &sticky) const {
        const int n = bits();
        lower = n > 64 ? n - 64 : 0;
        sticky = false;
        uint64_t out = 0;
        for (int bit = 0; bit < 64 && bit + lower < n; ++bit)
            if (limb_[(bit + lower) / 32] >> ((bit + lower) % 32) & 1) out |= 1ULL << bit;
        for (int i = 0; i < lower / 32 && !sticky; ++i) sticky = limb_[i] != 0;
        if (!sticky && lower % 32) sticky = (limb_[lower / 32] & ((1u << (lower % 32)) - 1)) != 0;
        return out;
    }

private:
    void push(uint32_t limb) {
        if (size_ == LIMBS) overflow();
        limb_[size_++] = limb;
    }
    void trim() {
        while (size_ && !limb_[size_ - 1]) --size_;
    }
    [[noreturn]] static void overflow() { throw std::length_error("A number conversion went past its working size"); }

    uint32_t limb_[LIMBS];
    int size_;
};

namespace decimal_detail {

inline int bit_length(uint64_t value) {
    int n = 0;
    for (; value; value >>= 1) ++n;
    return n;
}

// The double with this sign whose value is nearest (q + f) x 2^-scale, ties to even, where f is a fraction
// in [0, 1) that is non-zero exactly when sticky is set. q is not zero. Infinity past the largest double.
inline double assemble(bool negative, uint64_t q, bool sticky, int scale) {
    const uint64_t sign = negative ? 1ULL << 63 : 0, infinity = 0x7ffULL << 52;
    uint64_t pattern = 0;
    const int top = bit_length(q) - 1 - scale;  // the value lies in [2^top, 2^(top + 1))
    if (top > 1023) {
        pattern = infinity;
    } else {
        int unit = top - 52 < -1074 ? -1074 : top - 52;  // the result is a whole number of 2^unit
        const int drop = unit + scale;                   // bits of q below that unit
        uint64_t m;
        if (drop <= 0) {
            m = q << -drop;  // exact: every bit of q is kept
        } else if (drop > 64) {
            m = 0;  // below half the smallest unit
        } else if (drop == 64) {
            m = (q > (1ULL << 63) || (q == (1ULL << 63) && sticky)) ? 1 : 0;
        } else {
            m = q >> drop;
            const uint64_t rest = q & ((1ULL << drop) - 1), half = 1ULL << (drop - 1);
            if (rest > half || (rest == half && (sticky || (m & 1)))) ++m;
        }
        if (m == (1ULL << 53)) {
            m >>= 1;
            ++unit;
        }
        if (m >= (1ULL << 52)) {
            const int biased = unit + 52 + 1023;
            pattern = biased >= 2047 ? infinity : (static_cast<uint64_t>(biased) << 52) | (m & ((1ULL << 52) - 1));
        } else {
            pattern = m;  // zero or subnormal
        }
    }
    pattern |= sign;
    double out;
    std::memcpy(&out, &pattern, sizeof out);
    return out;
}

inline double power_of_ten(int n) {  // n in 0..22: each is exact in a double
    static const double TABLE[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return TABLE[n];
}

// The whole numbers 10^0 to 10^19, which are all that fit 64 bits.
inline uint64_t ten_to(int n) {
    static const uint64_t TABLE[] = {1ULL,
                                     10ULL,
                                     100ULL,
                                     1000ULL,
                                     10000ULL,
                                     100000ULL,
                                     1000000ULL,
                                     10000000ULL,
                                     100000000ULL,
                                     1000000000ULL,
                                     10000000000ULL,
                                     100000000000ULL,
                                     1000000000000ULL,
                                     10000000000000ULL,
                                     100000000000000ULL,
                                     1000000000000000ULL,
                                     10000000000000000ULL,
                                     100000000000000000ULL,
                                     1000000000000000000ULL,
                                     10000000000000000000ULL};
    return TABLE[n];
}

// a x b in full: the high and low 64 bits of the 128-bit product.
inline void multiply_wide(uint64_t a, uint64_t b, uint64_t &high, uint64_t &low) {
    const uint64_t a0 = a & 0xffffffffULL, a1 = a >> 32, b0 = b & 0xffffffffULL, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t middle = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
    high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
    low = (middle << 32) | (p00 & 0xffffffffULL);
}

// The quotient of number x 2^64 by divisor, for number below divisor and a divisor with its top bit set;
// remainder is whether anything is left over. Long division in two 32-bit digits (Knuth, The Art of
// Computer Programming, volume 2, algorithm D): each digit is guessed from the leading parts and put
// right, which takes at most two steps.
inline uint64_t divide_wide(uint64_t number, uint64_t divisor, bool &remainder) {
    const uint64_t base = 1ULL << 32, d1 = divisor >> 32, d0 = divisor & 0xffffffffULL;
    uint64_t q1 = number / d1, r = number - q1 * d1;
    while (q1 >= base || q1 * d0 > (r << 32)) {
        --q1;
        r += d1;
        if (r >= base) break;
    }
    const uint64_t rest = (number << 32) - q1 * divisor;  // the true value fits 64 bits; the parts wrap
    uint64_t q0 = rest / d1;
    r = rest - q0 * d1;
    while (q0 >= base || q0 * d0 > (r << 32)) {
        --q0;
        r += d1;
        if (r >= base) break;
    }
    remainder = (rest << 32) - q0 * divisor != 0;
    return (q1 << 32) | q0;
}

// One multiplication or division of doubles is correctly rounded only where the platform evaluates it in
// double precision. Elsewhere (x87 arithmetic) the exact paths are used for everything, as they are when
// KERFKITE_DECIMAL_EXACT_ONLY is defined, which the tests use to check them on their own.
#if !defined(KERFKITE_DECIMAL_EXACT_ONLY) && defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
const bool DOUBLE_ARITHMETIC = true;
#else
const bool DOUBLE_ARITHMETIC = false;
#endif
// The short cuts in 64-bit whole numbers are exact everywhere; they too can be left out for testing.
#if defined(KERFKITE_DECIMAL_EXACT_ONLY)
const bool SHORT_CUTS = false;
#else
const bool SHORT_CUTS = true;
#endif

}  // namespace decimal_detail

// The most significant digits kept from a decimal. A double, and the point halfway between two doubles,
// has at most 768 significant decimal digits, so whatever follows the 800th can only matter by being
// non-zero.
const int DECIMAL_DIGITS_KEPT = 800;

// Read [+-]digits[.digits][e|E[+-]digits] between first and last: at least one digit before or after the
// point, at least one in an exponent, no spaces and nothing else. False when the text is not of that
// form. out is the nearest double, ties to even: an infinity when the decimal is past the largest
// double, and a zero of the decimal's sign when it is nearer zero than the smallest. However long the
// text, the work after reading it is bounded. With exact_only the short cuts are not taken; the answer is
// the same, which the self-test checks on each platform.
inline bool parse_double(const char *first, const char *last, double &out, bool exact_only = false) {
    using namespace decimal_detail;
    const char *p = first;
    bool negative = false;
    if (p != last && (*p == '+' || *p == '-')) negative = *p++ == '-';
    char digits[DECIMAL_DIGITS_KEPT + 1];
    int kept = 0;
    int64_t exponent = 0;  // the decimal is the kept digits, as a whole number, times 10^exponent
    bool any = false, started = false, more = false;
    for (; p != last && *p >= '0' && *p <= '9'; ++p) {
        any = true;
        if (!started && *p == '0') continue;
        started = true;
        if (kept < DECIMAL_DIGITS_KEPT) {
            digits[kept++] = *p;
        } else {
            ++exponent;
            more = more || *p != '0';
        }
    }
    if (p != last && *p == '.') {
        for (++p; p != last && *p >= '0' && *p <= '9'; ++p) {
            any = true;
            if (!started && *p == '0') {
                --exponent;
                continue;
            }
            started = true;
            if (kept < DECIMAL_DIGITS_KEPT) {
                digits[kept++] = *p;
                --exponent;
            } else {
                more = more || *p != '0';
            }
        }
    }
    if (!any) return false;
    if (p != last && (*p == 'e' || *p == 'E')) {
        ++p;
        bool below = false;
        if (p != last && (*p == '+' || *p == '-')) below = *p++ == '-';
        if (p == last || *p < '0' || *p > '9') return false;
        int64_t given = 0;
        for (; p != last && *p >= '0' && *p <= '9'; ++p)
            if (given < 1000000000) given = given * 10 + (*p - '0');  // far past any double either way
        exponent += below ? -given : given;
    }
    if (p != last) return false;

    if (more) {
        // Digits were left off the end: stand in for them with one that makes the decimal just larger.
        digits[kept++] = '1';
        --exponent;
    } else {
        while (kept && digits[kept - 1] == '0') {
            --kept;
            ++exponent;
        }
    }
    const uint64_t sign = negative ? 1ULL << 63 : 0;
    const auto special = [&](uint64_t pattern) {
        pattern |= sign;
        std::memcpy(&out, &pattern, sizeof out);
        return true;
    };
    if (!kept) return special(0);
    // The decimal lies in [10^(point - 1), 10^point).
    const int64_t point = kept + exponent;
    if (point > 310) return special(0x7ffULL << 52);
    if (point < -330) return special(0);
    const int scale = static_cast<int>(exponent);

    if (DOUBLE_ARITHMETIC && !exact_only && kept <= 15 && scale >= -22 && scale <= 22 + (15 - kept)) {
        // The digits are a whole number below 2^53 and the power of ten is exact, so one operation rounds once.
        uint64_t whole = 0;
        for (int i = 0; i < kept; ++i) whole = whole * 10 + static_cast<uint64_t>(digits[i] - '0');
        int power = scale;
        for (; power > 22; --power) whole *= 10;  // still below 10^15
        const double value = static_cast<double>(whole);
        out = power < 0 ? value / power_of_ten(-power) : value * power_of_ten(power);
        if (negative) out = -out;
        return true;
    }

    if (SHORT_CUTS && !exact_only && kept <= 18 && scale <= 0 && scale >= -19) {
        // The digits and the power of ten both fit 64 bits: the same long division as below, in machine words.
        uint64_t whole = 0;
        for (int i = 0; i < kept; ++i) whole = whole * 10 + static_cast<uint64_t>(digits[i] - '0');
        if (!scale) {
            out = assemble(negative, whole, false, 0);
            return true;
        }
        uint64_t divisor = ten_to(-scale);
        const int shift = bit_length(divisor) - bit_length(whole) + 63;  // whole x 2^shift / divisor is in (2^62, 2^64)
        if (shift >= 64) whole <<= shift - 64;
        else divisor <<= 64 - shift;
        const int spare = 64 - bit_length(divisor);  // the division wants the divisor's top bit set
        bool remainder;
        const uint64_t q = divide_wide(whole << spare, divisor << spare, remainder);
        out = assemble(negative, q, remainder, shift);
        return true;
    }

    Big number;
    for (int i = 0; i < kept;) {
        uint32_t chunk = 0, factor = 1;
        for (int n = 0; n < 9 && i < kept; ++n, ++i) {
            chunk = chunk * 10 + static_cast<uint32_t>(digits[i] - '0');
            factor *= 10;
        }
        number.multiply_add(factor, chunk);
    }
    if (scale >= 0) {
        number.multiply_pow10(scale);
        int lower;
        bool sticky;
        const uint64_t q = number.top64(lower, sticky);
        out = assemble(negative, q, sticky, -lower);
        return true;
    }
    // number / 10^-scale: the quotient to 63 or 64 bits by long division, one bit at a time.
    Big divisor(1);
    divisor.multiply_pow10(-scale);
    const int shift = divisor.bits() - number.bits() + 63;  // number x 2^shift / divisor is in (2^62, 2^64)
    if (shift >= 64) number.shift_left(shift - 64);
    else divisor.shift_left(64 - shift);
    // Now the quotient wanted is number x 2^64 / divisor, and number is below divisor.
    uint64_t q = 0;
    for (int bit = 0; bit < 64; ++bit) {
        number.shift_left(1);
        q <<= 1;
        if (number.compare(divisor) >= 0) {
            number.subtract(divisor);
            q |= 1;
        }
    }
    out = assemble(negative, q, !number.zero(), shift);
    return true;
}

// The digits of a finite double above zero, by exact arithmetic: the fewest that read back as the same
// double and, of those, the nearest to it (the even digit when two are equally near). digits receives 1 to 17 characters, with no terminator; the
// value is 0.d1d2..dn x 10^point. Returns n.
inline int shortest_digits_exact(double value, char *digits, int &point) {
    using namespace decimal_detail;
    uint64_t pattern;
    std::memcpy(&pattern, &value, sizeof pattern);
    const int biased = static_cast<int>(pattern >> 52 & 0x7ff);
    const uint64_t fraction = pattern & ((1ULL << 52) - 1);
    const uint64_t f = biased ? fraction | (1ULL << 52) : fraction;  // value = f x 2^e
    const int e = biased ? biased - 1075 : -1074;
    const bool even = !(f & 1);                  // the ends of the interval round to this double too
    const bool narrow = !fraction && biased > 1;  // the double below is half as far away as the one above
    // value = r / s; the decimals that round to it lie between (r - below) / s and (r + above) / s.
    Big r(f), s(1), above(1), below(1);
    const int extra = narrow ? 2 : 1;
    if (e >= 0) {
        r.shift_left(e + extra);
        s.shift_left(extra);
        above.shift_left(e + extra - 1);
        below.shift_left(e);
    } else {
        r.shift_left(extra);
        s.shift_left(-e + extra);
        above.shift_left(extra - 1);
    }
    // A first guess at the place of the leading digit, from the binary exponent; put right just below.
    const int binary = e + bit_length(f) - 1;
    const int64_t scaled = static_cast<int64_t>(binary) * 315653;  // log10(2) is about 315653 / 2^20
    int k = static_cast<int>(scaled >= 0 ? scaled >> 20 : -((-scaled + 1048575) >> 20)) + 1;
    if (k >= 0) {
        s.multiply_pow10(k);
    } else {
        r.multiply_pow10(-k);
        above.multiply_pow10(-k);
        below.multiply_pow10(-k);
    }
    const auto reaches = [&](const Big &high, const Big &limit) {  // high / limit is past the top of the interval
        const int order = high.compare(limit);
        return even ? order >= 0 : order > 0;
    };
    for (;;) {  // while the top of the interval is 1 or more, the leading digit is a place higher
        Big high(r);
        high.add(above);
        if (!reaches(high, s)) break;
        s.multiply_add(10, 0);
        ++k;
    }
    for (;;) {  // while it is below 1/10, the leading digit is a place lower
        Big high(r);
        high.add(above);
        high.multiply_add(10, 0);
        if (reaches(high, s)) break;
        r.multiply_add(10, 0);
        above.multiply_add(10, 0);
        below.multiply_add(10, 0);
        --k;
    }
    point = k;
    int n = 0;
    for (;;) {
        r.multiply_add(10, 0);
        above.multiply_add(10, 0);
        below.multiply_add(10, 0);
        int digit = 0;
        while (r.compare(s) >= 0) {
            r.subtract(s);
            ++digit;
        }
        // Stopping here is within the interval on the low side; rounding the digit up is within it on the high side.
        const int low_order = r.compare(below);
        const bool low = even ? low_order <= 0 : low_order < 0;
        Big high(r);
        high.add(above);
        const bool up = reaches(high, s);
        if (!low && !up && n < 16) {  // seventeen digits always suffice
            digits[n++] = static_cast<char>('0' + digit);
            continue;
        }
        if (up && low) {  // both are in the interval: take the nearer, and the even digit when they are equally near
            Big twice(r);
            twice.shift_left(1);
            const int order = twice.compare(s);
            if (order > 0 || (order == 0 && (digit & 1))) ++digit;
        } else if (up) {
            ++digit;
        }
        digits[n++] = static_cast<char>('0' + digit);
        break;
    }
    return n;
}

// The same digits as shortest_digits_exact, found in 64-bit whole numbers for the values a program mostly
// holds: those from 2^-10 up to 2^53.
//
// Such a value is f / 2^s with f a whole number below 2^53 and s from 1 to 62. With p places after the
// point, the decimal nearest it is q / 10^p, for q the whole number nearest f x 10^p / 2^s (the even one
// of two equally near), and that decimal reads back as the value when it lies within half the gap to the
// next double: twice the remainder of that division is below 10^p. (It is never equal: the point halfway
// between two doubles has more places than any p tried here.) A decimal with more places is at least as
// near, so the least p that passes is found by halving the range, and its q holds the shortest digits. (Below a power of two the
// next double is half as far away, which this test does not allow for; but a power of two in this range
// is itself a decimal of few digits, and no shorter decimal is anywhere near it.)
inline int shortest_digits(double value, char *digits, int &point) {
    using namespace decimal_detail;
    uint64_t pattern;
    std::memcpy(&pattern, &value, sizeof pattern);
    const int biased = static_cast<int>(pattern >> 52 & 0x7ff);
    const uint64_t fraction = pattern & ((1ULL << 52) - 1);
    const int s = 1075 - biased;
    if (!SHORT_CUTS || s < 1 || s > 62) return shortest_digits_exact(value, digits, point);
    const uint64_t f = fraction | (1ULL << 52), unit = 1ULL << s;
    // q for p places, and whether its decimal reads back as the value.
    const auto reads_back = [&](int p, uint64_t &q) {
        uint64_t high, low;
        multiply_wide(f, ten_to(p), high, low);  // below 2^117
        q = (low >> s) | (high << (64 - s));     // below 10 x 2^53, since 10^p is at most ten times 2^s
        const uint64_t rest = low & (unit - 1), half = unit >> 1;
        const bool up = rest > half || (rest == half && (q & 1));
        if (up) ++q;
        const uint64_t twice = 2 * (up ? unit - rest : rest);  // at most 2^s
        return twice < ten_to(p);
    };
    int most = s * 1233 >> 12;  // about s x log10(2)
    while (ten_to(most) <= unit) ++most;  // 10^most is above 2^s, so that many places always read back
    int least = 0;
    uint64_t q = 0, tried;
    reads_back(most, q);
    while (least < most) {
        const int middle = (least + most) / 2;
        if (reads_back(middle, tried)) {
            most = middle;
            q = tried;
        } else {
            least = middle + 1;
        }
    }
    char reversed[20];
    int n = 0;
    for (; q; q /= 10) reversed[n++] = static_cast<char>('0' + q % 10);
    point = n - most;
    int skip = 0;
    while (skip < n - 1 && reversed[skip] == '0') ++skip;  // only a whole number ends in zeros
    for (int i = n - 1; i >= skip; --i) *digits++ = reversed[i];
    return n - skip;
}

}  // namespace text
}  // namespace kerfkite

#endif
