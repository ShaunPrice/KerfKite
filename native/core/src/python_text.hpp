// Python's view of text where the reference counts or strips it: characters are code points, and whitespace is
// what str.isspace() calls whitespace. Internal to the core; the core's text is well-formed UTF-8.
#ifndef KERFKITE_PYTHON_TEXT_HPP
#define KERFKITE_PYTHON_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kerfkite {
namespace detail {

// str.isspace() of one code point: general category Zs, or bidirectional class WS, B or S (Unicode 15 and 16,
// as Python 3.12 to 3.14 have them).
inline bool python_space(uint32_t c) {
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

struct CodePoint {
    uint32_t code;
    size_t begin, end;  // its bytes in the text
};

// The code points of well-formed UTF-8 text, which is what the core's JSON holds.
inline std::vector<CodePoint> code_points(const std::string &text) {
    std::vector<CodePoint> points;
    points.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        uint32_t code = length == 1 ? lead : length == 2 ? lead & 0x1fu : length == 3 ? lead & 0x0fu : lead & 0x07u;
        for (size_t k = 1; k < length && i + k < text.size(); ++k)
            code = code << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3fu);
        points.push_back({code, i, i + length});
        i += length;
    }
    return points;
}

// text.strip(): the text between the first and the last code points that are not whitespace; empty when
// there are none.
inline std::string python_strip(const std::string &text, const std::vector<CodePoint> &points) {
    size_t first = 0, last = points.size();
    while (first < last && python_space(points[first].code)) ++first;
    while (last > first && python_space(points[last - 1].code)) --last;
    if (first == last) return std::string();
    return text.substr(points[first].begin, points[last - 1].end - points[first].begin);
}

// str.isprintable() of one code point, from the Unicode tables of python_text.cpp.
bool python_printable(uint32_t c);

// bytes.decode('utf-8') in strict mode: the code points, or false where Python raises UnicodeDecodeError (a
// sequence that is not well-formed UTF-8 in Unicode's table 3-7).
bool python_utf8(const unsigned char *data, size_t size, std::vector<uint32_t> &out);

// The UTF-8 bytes of a code point, added to text.
void append_utf8(std::string &text, uint32_t c);

}  // namespace detail
}  // namespace kerfkite

#endif
