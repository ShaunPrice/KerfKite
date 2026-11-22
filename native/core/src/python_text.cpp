// Python's view of text outside the core's JSON: str.isprintable(), strict UTF-8 decoding and UTF-8 encoding.
// See python_text.hpp.
#include "python_text.hpp"

#include "unicode_tables.hpp"

namespace kerfkite {
namespace detail {

bool python_printable(uint32_t c) {
    const size_t count = sizeof(kNotPrintable) / sizeof(kNotPrintable[0]);
    size_t low = 0, high = count;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (kNotPrintable[middle][1] < c) low = middle + 1;
        else high = middle;
    }
    return !(low < count && kNotPrintable[low][0] <= c);
}

bool python_utf8(const unsigned char *data, size_t size, std::vector<uint32_t> &out) {
    out.clear();
    out.reserve(size);
    for (size_t i = 0; i < size;) {
        const unsigned lead = data[i];
        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }
        size_t length = 0;
        uint32_t code = 0;
        unsigned low = 0x80, high = 0xbf;  // the second byte's range
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
            code = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            code = lead & 0x0f;
            if (lead == 0xe0) low = 0xa0;
            if (lead == 0xed) high = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            code = lead & 0x07;
            if (lead == 0xf0) low = 0x90;
            if (lead == 0xf4) high = 0x8f;
        } else {
            return false;
        }
        if (size - i < length) return false;
        for (size_t k = 1; k < length; ++k) {
            const unsigned next = data[i + k];
            if (next < (k == 1 ? low : 0x80u) || next > (k == 1 ? high : 0xbfu)) return false;
            code = code << 6 | (next & 0x3f);
        }
        out.push_back(code);
        i += length;
    }
    return true;
}

void append_utf8(std::string &text, uint32_t c) {
    if (c < 0x80) {
        text += static_cast<char>(c);
    } else if (c < 0x800) {
        text += static_cast<char>(0xc0 | (c >> 6));
        text += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        text += static_cast<char>(0xe0 | (c >> 12));
        text += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        text += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        text += static_cast<char>(0xf0 | (c >> 18));
        text += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        text += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        text += static_cast<char>(0x80 | (c & 0x3f));
    }
}

}  // namespace detail
}  // namespace kerfkite
