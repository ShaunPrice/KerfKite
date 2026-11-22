// zlib streams (RFC 1950) and their DEFLATE data (RFC 1951), decompressed as Python's zlib.decompress does for a
// sound stream: the header checked, the blocks inflated, the Adler-32 checksum compared, and any bytes after the
// stream ignored; and raw DEFLATE data as a ZIP member holds it, with the errors zlib reports. Written from the
// RFCs; internal to the core (the PDF reader's FlateDecode and the bundle reader's members).
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

// Raw DEFLATE data, as a ZIP member holds it and zlib.decompressobj(-15) reads it in one call: Broken where
// zlib reports an error, and Ok with ended false where the data stops before the stream ends, with the bytes
// decoded so far. Bytes after the end are ignored.
Inflated raw_inflate(const unsigned char *data, size_t size, size_t limit, std::vector<unsigned char> &out, bool &ended);

// The CRC-32 of ZIP and PNG (ISO 3309), as zlib.crc32 gives it.
uint32_t crc32(const unsigned char *data, size_t size);

}  // namespace detail
}  // namespace kerfkite

#endif
