// Python's base64.b64decode(text, validate=True), as the reference's readers of image and STL bytes call it.
// Internal to the core.
#ifndef KERFKITE_BASE64_HPP
#define KERFKITE_BASE64_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>


namespace kerfkite {
namespace detail {

// base64.b64decode(text, validate=True) for the text the reference reads unchanged: groups of four characters of
// the standard alphabet, with one or two = only to pad the last group. False for anything else, all of which the
// reference refuses as well; the reference's messages for those differ, but its caller gives one message for all.
// A byte of text that is not ASCII is not in the alphabet, so such text is refused here too.
//
// Whole groups followed by nothing but = ("AAAA=", "AAAA====") are refused, as Python 3.12.10 and 3.13 refuse
// them (3.11.18 and 3.12.3 decoded the groups and ignored the =).
inline bool base64(const std::string &text, std::vector<unsigned char> &out) {
    // 0 to 63 for the alphabet, 64 for =, and 255 for every other byte: a group that holds either has bit 6 or 7.
    static const std::array<unsigned char, 256> table = [] {
        std::array<unsigned char, 256> values{};
        values.fill(255);
        for (int c = 0; c < 26; ++c) {
            values[size_t('A' + c)] = static_cast<unsigned char>(c);
            values[size_t('a' + c)] = static_cast<unsigned char>(c + 26);
        }
        for (int c = 0; c < 10; ++c) values[size_t('0' + c)] = static_cast<unsigned char>(c + 52);
        values[size_t('+')] = 62;
        values[size_t('/')] = 63;
        values[size_t('=')] = 64;
        return values;
    }();
    const size_t size = text.size();
    out.clear();
    size_t data = size;
    while (data > 0 && text[data - 1] == '=') --data;
    if (data < size && data > 0 && data % 4 == 0) {
        bool alphabet = true;
        for (size_t i = 0; i < data && alphabet; ++i) alphabet = table[static_cast<unsigned char>(text[i])] < 64;
        if (alphabet) return false;
    }
    if (size % 4) return false;
    if (size == 0) return true;
    const auto *in = reinterpret_cast<const unsigned char *>(text.data());
    // The = that end the text; any other = is refused below with the bytes that are not the alphabet.
    const size_t padding = in[size - 1] != '=' ? 0 : in[size - 2] != '=' ? 1 : 2;
    out.resize(size / 4 * 3 - padding);
    unsigned char *to = out.data();
    const size_t whole = padding ? size - 4 : size;
    for (size_t i = 0; i < whole; i += 4, to += 3) {
        const unsigned a = table[in[i]], b = table[in[i + 1]], c = table[in[i + 2]], d = table[in[i + 3]];
        if ((a | b | c | d) & 0xc0) return false;
        const uint32_t group = (a << 18) | (b << 12) | (c << 6) | d;
        to[0] = static_cast<unsigned char>(group >> 16);
        to[1] = static_cast<unsigned char>(group >> 8);
        to[2] = static_cast<unsigned char>(group);
    }
    if (padding) {
        // The last group: two or three characters of data. Bits beyond them may be set, as the reference allows.
        const unsigned a = table[in[size - 4]], b = table[in[size - 3]], c = padding == 1 ? table[in[size - 2]] : 0;
        if ((a | b | c) & 0xc0) return false;
        const uint32_t group = (a << 18) | (b << 12) | (c << 6);
        to[0] = static_cast<unsigned char>(group >> 16);
        if (padding == 1) to[1] = static_cast<unsigned char>(group >> 8);
    }
    return true;
}

}  // namespace detail
}  // namespace kerfkite

#endif
