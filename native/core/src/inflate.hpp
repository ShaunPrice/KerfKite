// zlib streams (RFC 1950) and their DEFLATE data (RFC 1951), decompressed as Python's zlib.decompress does
// for a sound stream: the header checked, the blocks inflated, the Adler-32 checksum compared, and any bytes
// after the stream ignored. Written from the RFCs; internal to the core (the PDF reader's FlateDecode).
#ifndef KERFKITE_INFLATE_HPP
#define KERFKITE_INFLATE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kerfkite {
namespace detail {

enum class Inflated {
    Ok,
    Broken,   // not a sound zlib stream: a bad header, code, distance or checksum, or the data ends early
    TooLong,  // the output would exceed the limit
};

// The decompressed bytes of a zlib stream, at most limit of them.
Inflated zlib_decompress(const unsigned char *data, size_t size, size_t limit, std::vector<unsigned char> &out);

}  // namespace detail
}  // namespace kerfkite

#endif
