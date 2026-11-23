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

// text.split(): the runs of code points between whitespace.
inline std::vector<std::string> python_split(const std::string &text) {
    std::vector<std::string> words;
    const std::vector<CodePoint> points = code_points(text);
    for (size_t i = 0; i < points.size();) {
        while (i < points.size() && python_space(points[i].code)) ++i;
        const size_t first = i;
        while (i < points.size() && !python_space(points[i].code)) ++i;
        if (i > first) words.push_back(text.substr(points[first].begin, points[i - 1].end - points[first].begin));
    }
    return words;
}

// str.isprintable() of one code point, from the Unicode tables of python_text.cpp.
bool python_printable(uint32_t c);

// repr() of text: in single quotes unless it holds one and no double quote, with the escapes repr() writes.
std::string python_repr(const std::string &text);

// The value of a decimal digit (general category Nd, what \d matches and float() reads), or -1.
int python_decimal(uint32_t c);

// Whether the pattern \w matches one code point: str.isalnum(), or the underscore.
bool python_word(uint32_t c);

// The one to three code points str.upper() makes of one, added to out.
void python_upper(uint32_t c, std::vector<uint32_t> &out);

// str.upper() of text.
std::string python_upper(const std::string &text);

// A line boundary of str.splitlines(); "\r\n" is one.
inline bool python_line_break(uint32_t c) {
    return (c >= 0x0a && c <= 0x0d) || (c >= 0x1c && c <= 0x1e) || c == 0x85 || c == 0x2028 || c == 0x2029;
}

// str.splitlines() of text.
std::vector<std::string> python_splitlines(const std::string &text);

// Text that is not ASCII, in ASCII for a reader of ASCII text written after Python's str methods and re module:
// whitespace becomes a space and a decimal digit its ASCII digit, as \s, \d, strip(), split() and float() take
// them; any other character becomes \x01, which none of those matches and no reader's grammar holds. With lines,
// a line boundary becomes \x0b, one boundary for splitlines() that pairs with no other; without, a space. Each
// code point becomes one byte, so a byte's place is its code point's.
std::string python_ascii(const std::string &text, bool lines);

// bytes.decode('utf-8') in strict mode: the code points, or false where Python raises UnicodeDecodeError (a
// sequence that is not well-formed UTF-8 in Unicode's table 3-7).
bool python_utf8(const unsigned char *data, size_t size, std::vector<uint32_t> &out);

// The UTF-8 bytes of a code point, added to text.
void append_utf8(std::string &text, uint32_t c);

}  // namespace detail
}  // namespace kerfkite

#endif
