// DEFLATE decompression from RFC 1951 and the zlib wrapper from RFC 1950. See inflate.hpp.
#include "inflate.hpp"

#include <array>

namespace kerfkite {
namespace detail {

namespace {

struct Broken {};   // data zlib reports an error for
struct Short {};    // data that ends before the stream does
struct TooLong {};

class Bits {
public:
    Bits(const unsigned char *data, size_t size) : data_(data), size_(size) {}

    // The next count bits, least significant first, as RFC 1951 packs them.
    unsigned take(int count) {
        unsigned value = 0;
        for (int i = 0; i < count; ++i) value |= static_cast<unsigned>(bit()) << i;
        return value;
    }

    int bit() {
        if (used_ == 8) {
            if (at_ >= size_) throw Short();
            current_ = data_[at_++];
            used_ = 0;
        }
        return (current_ >> used_++) & 1;
    }

    // To the next byte boundary, for a stored block.
    void align() { used_ = 8; }

    unsigned char byte() {
        if (at_ >= size_) throw Short();
        return data_[at_++];
    }

    size_t position() const { return at_; }

private:
    const unsigned char *data_;
    size_t size_, at_ = 0;
    unsigned current_ = 0;
    int used_ = 8;
};

// A canonical Huffman code (RFC 1951 3.2.2): how many codes each length has, and the symbols in code order.
struct Code {
    std::array<int, 16> counts{};
    std::vector<int> symbols;
    int longest = 0;        // the longest code, zero when there is none
    bool zeros = false;     // a code-length code without codes, which zlib reads as a length of 0 a bit

    // False where zlib rejects the lengths: an oversubscribed code, or an incomplete one other than a single
    // code of one bit for literals or distances. A code without codes is accepted, as zlib accepts one and
    // reports an error only when a symbol is read; of_lengths marks the code of the code lengths.
    bool build(const int *lengths, int n, bool of_lengths = false) {
        counts.fill(0);
        for (int i = 0; i < n; ++i) ++counts[static_cast<size_t>(lengths[i])];
        counts[0] = 0;
        longest = 0;
        for (int length = 15; length >= 1 && !longest; --length)
            if (counts[static_cast<size_t>(length)]) longest = length;
        zeros = of_lengths && longest == 0;
        int left = 1;
        for (int length = 1; length < 16; ++length) {
            left = left * 2 - counts[static_cast<size_t>(length)];
            if (left < 0) return false;
        }
        if (longest && left > 0 && (of_lengths || longest != 1)) return false;
        std::array<int, 16> offsets{};
        for (int length = 1; length < 15; ++length) offsets[static_cast<size_t>(length) + 1] = offsets[static_cast<size_t>(length)] + counts[static_cast<size_t>(length)];
        symbols.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[static_cast<size_t>(offsets[static_cast<size_t>(lengths[i])]++)] = i;
        return true;
    }

    // The next symbol: the code read a bit at a time, most significant bit first, against the first code of
    // each length.
    // A code no symbol has is an error once its bits are read, as in zlib's tables.
    int decode(Bits &bits) const {
        if (zeros) {
            bits.bit();
            return 0;
        }
        int code = 0, first = 0, index = 0;
        for (int length = 1; length < 16; ++length) {
            code |= bits.bit();
            const int count = counts[static_cast<size_t>(length)];
            if (code - first < count) return symbols[static_cast<size_t>(index + code - first)];
            if (length >= longest) break;
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw Broken();
    }
};

const int kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                               6145, 8193, 12289, 16385, 24577};
const int kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put(std::vector<unsigned char> &out, unsigned char value, size_t limit) {
    if (out.size() >= limit) throw TooLong();
    out.push_back(value);
}

void codes(Bits &bits, const Code &lengths, const Code &distances, std::vector<unsigned char> &out, size_t limit) {
    for (;;) {
        const int symbol = lengths.decode(bits);
        if (symbol < 256) {
            put(out, static_cast<unsigned char>(symbol), limit);
        } else if (symbol == 256) {
            return;
        } else {
            const int index = symbol - 257;
            if (index >= 29) throw Broken();
            const int length = kLengthBase[index] + static_cast<int>(bits.take(kLengthExtra[index]));
            const int which = distances.decode(bits);
            if (which >= 30) throw Broken();
            const size_t distance = static_cast<size_t>(kDistanceBase[which]) + bits.take(kDistanceExtra[which]);
            if (distance > out.size()) throw Broken();
            for (int i = 0; i < length; ++i) put(out, out[out.size() - distance], limit);
        }
    }
}

void fixed_block(Bits &bits, std::vector<unsigned char> &out, size_t limit) {
    static const std::pair<Code, Code> fixed = [] {
        int lengths[288];
        for (int i = 0; i < 144; ++i) lengths[i] = 8;
        for (int i = 144; i < 256; ++i) lengths[i] = 9;
        for (int i = 256; i < 280; ++i) lengths[i] = 7;
        for (int i = 280; i < 288; ++i) lengths[i] = 8;
        // Distance codes 30 and 31 complete the code and are errors when read, as in zlib.
        Code literal, distance;
        literal.build(lengths, 288);
        int distances[32];
        for (int &d : distances) d = 5;
        distance.build(distances, 32);
        return std::make_pair(literal, distance);
    }();
    codes(bits, fixed.first, fixed.second, out, limit);
}

void dynamic_block(Bits &bits, std::vector<unsigned char> &out, size_t limit) {
    const int literals = static_cast<int>(bits.take(5)) + 257, distances = static_cast<int>(bits.take(5)) + 1;
    const int code_lengths = static_cast<int>(bits.take(4)) + 4;
    if (literals > 286 || distances > 30) throw Broken();
    static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int lengths[320] = {};
    for (int i = 0; i < code_lengths; ++i) lengths[order[i]] = static_cast<int>(bits.take(3));
    Code lengths_code;
    if (!lengths_code.build(lengths, 19, true)) throw Broken();
    int total = 0;
    int all[320] = {};
    while (total < literals + distances) {
        const int symbol = lengths_code.decode(bits);
        if (symbol < 16) {
            all[total++] = symbol;
            continue;
        }
        int value = 0, repeat = 0;
        if (symbol == 16) {
            repeat = 3 + static_cast<int>(bits.take(2));
            if (total == 0) throw Broken();
            value = all[total - 1];
        } else if (symbol == 17) {
            repeat = 3 + static_cast<int>(bits.take(3));
        } else {
            repeat = 11 + static_cast<int>(bits.take(7));
        }
        if (total + repeat > literals + distances) throw Broken();
        while (repeat--) all[total++] = value;
    }
    if (all[256] == 0) throw Broken();  // no end of block
    Code literal, distance;
    if (!literal.build(all, literals) || !distance.build(all + literals, distances)) throw Broken();
    codes(bits, literal, distance, out, limit);
}

uint32_t adler32(const std::vector<unsigned char> &data) {
    uint32_t a = 1, b = 0;
    for (const unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

}  // namespace

namespace {

// The DEFLATE blocks from the start of the bits; the bytes they take, with the last one whole.
void blocks(Bits &bits, std::vector<unsigned char> &out, size_t limit) {
    for (bool last = false; !last;) {
        last = bits.bit();
        const unsigned type = bits.take(2);
        if (type == 0) {
            bits.align();
            const unsigned length = bits.byte() | (static_cast<unsigned>(bits.byte()) << 8);
            const unsigned complement = bits.byte() | (static_cast<unsigned>(bits.byte()) << 8);
            if ((length ^ 0xffff) != complement) throw Broken();
            for (unsigned i = 0; i < length; ++i) put(out, bits.byte(), limit);
        } else if (type == 1) {
            fixed_block(bits, out, limit);
        } else if (type == 2) {
            dynamic_block(bits, out, limit);
        } else {
            throw Broken();
        }
    }
    bits.align();
}

}  // namespace

Inflated zlib_decompress(const unsigned char *data, size_t size, size_t limit, std::vector<unsigned char> &out) {
    out.clear();
    if (size < 2) return Inflated::Broken;
    const unsigned cmf = data[0], flg = data[1];
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 32)) return Inflated::Broken;
    try {
        Bits bits(data + 2, size - 2);
        blocks(bits, out, limit);
        uint32_t check = 0;
        for (int i = 0; i < 4; ++i) check = (check << 8) | bits.byte();
        if (check != adler32(out)) return Inflated::Broken;
    } catch (const Broken &) {
        return Inflated::Broken;
    } catch (const Short &) {
        return Inflated::Broken;
    } catch (const TooLong &) {
        return Inflated::TooLong;
    }
    return Inflated::Ok;
}

Inflated raw_inflate(const unsigned char *data, size_t size, size_t limit, std::vector<unsigned char> &out, bool &ended) {
    out.clear();
    ended = false;
    try {
        Bits bits(data, size);
        blocks(bits, out, limit);
        ended = true;
    } catch (const Broken &) {
        return Inflated::Broken;
    } catch (const Short &) {
        return Inflated::Ok;
    } catch (const TooLong &) {
        return Inflated::TooLong;
    }
    return Inflated::Ok;
}

uint32_t crc32(const unsigned char *data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            values[n] = c;
        }
        return values;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace detail
}  // namespace kerfkite
