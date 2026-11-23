// Python's view of text: str.isprintable(), the decimal digits, \w, str.upper() and str.splitlines(), strict
// UTF-8 decoding and UTF-8 encoding. See python_text.hpp.
#include "python_text.hpp"

#include <algorithm>
#include <cstdio>

#include "unicode_tables.hpp"

namespace kerfkite {
namespace detail {

namespace {

// Whether c lies in one of the sorted, separate runs.
bool in_runs(const uint32_t (*runs)[2], size_t count, uint32_t c) {
    size_t low = 0, high = count;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (runs[middle][1] < c) low = middle + 1;
        else high = middle;
    }
    return low < count && runs[low][0] <= c;
}

}  // namespace

int python_decimal(uint32_t c) {
    const uint32_t *runs = kDecimalDigitRuns;
    const size_t count = sizeof(kDecimalDigitRuns) / sizeof(kDecimalDigitRuns[0]);
    const uint32_t *after = std::upper_bound(runs, runs + count, c);
    if (after == runs || c - *(after - 1) >= 10) return -1;
    return static_cast<int>(c - *(after - 1));
}

std::string python_repr(const std::string &text) {
    const bool single = text.find('\'') != std::string::npos, twice = text.find('"') != std::string::npos;
    const char quote = single && !twice ? '"' : '\'';
    std::string out(1, quote);
    char buffer[16];
    for (const CodePoint &point : code_points(text)) {
        const uint32_t c = point.code;
        if (c == static_cast<uint32_t>(quote) || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 0x20 || c == 0x7f) {
            std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned>(c));
            out += buffer;
        } else if (c < 0x7f || python_printable(c)) {
            append_utf8(out, c);
        } else {
            if (c <= 0xff) std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned>(c));
            else if (c <= 0xffff) std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
            else std::snprintf(buffer, sizeof buffer, "\\U%08x", static_cast<unsigned>(c));
            out += buffer;
        }
    }
    out += quote;
    return out;
}

bool python_word(uint32_t c) { return in_runs(kWordRuns, sizeof(kWordRuns) / sizeof(kWordRuns[0]), c); }

void python_upper(uint32_t c, std::vector<uint32_t> &out) {
    if (c >= 'a' && c <= 'z') {
        out.push_back(c - 'a' + 'A');
        return;
    }
    if (c < 0x80) {
        out.push_back(c);
        return;
    }
    const size_t count = sizeof(kUpper) / sizeof(kUpper[0]);
    const auto *found = std::lower_bound(kUpper, kUpper + count, c, [](const uint32_t (&row)[4], uint32_t code) {
        return row[0] < code;
    });
    if (found == kUpper + count || (*found)[0] != c) {
        out.push_back(c);
        return;
    }
    for (size_t k = 1; k < 4 && (*found)[k]; ++k) out.push_back((*found)[k]);
}

void python_casefold(uint32_t c, std::vector<uint32_t> &out) {
    if (c < 0x80) {
        out.push_back(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return;
    }
    const size_t count = sizeof(kCaseFold) / sizeof(kCaseFold[0]);
    const auto *found = std::lower_bound(kCaseFold, kCaseFold + count, c, [](const uint32_t (&row)[4], uint32_t code) {
        return row[0] < code;
    });
    if (found == kCaseFold + count || (*found)[0] != c) {
        out.push_back(c);
        return;
    }
    for (size_t k = 1; k < 4 && (*found)[k]; ++k) out.push_back((*found)[k]);
}

std::string python_casefold(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    std::vector<uint32_t> mapped;
    for (const CodePoint &point : code_points(text)) {
        mapped.clear();
        python_casefold(point.code, mapped);
        for (const uint32_t c : mapped) append_utf8(out, c);
    }
    return out;
}

std::string python_upper(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    std::vector<uint32_t> mapped;
    for (const CodePoint &point : code_points(text)) {
        mapped.clear();
        python_upper(point.code, mapped);
        for (const uint32_t c : mapped) append_utf8(out, c);
    }
    return out;
}

std::vector<std::string> python_splitlines(const std::string &text) {
    std::vector<std::string> lines;
    const std::vector<CodePoint> points = code_points(text);
    size_t start = 0;  // the line's first byte
    for (size_t i = 0; i < points.size(); ++i) {
        if (!python_line_break(points[i].code)) continue;
        lines.push_back(text.substr(start, points[i].begin - start));
        if (points[i].code == '\r' && i + 1 < points.size() && points[i + 1].code == '\n') ++i;
        start = points[i].end;
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    return lines;
}

std::string python_ascii(const std::string &text, bool lines) {
    std::string out;
    out.reserve(text.size());
    for (const CodePoint &point : code_points(text)) {
        const uint32_t c = point.code;
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (python_line_break(c)) out.push_back(lines ? '\x0b' : ' ');
        else if (python_space(c)) out.push_back(' ');
        else if (python_decimal(c) >= 0) out.push_back(static_cast<char>('0' + python_decimal(c)));
        else out.push_back('\x01');
    }
    return out;
}

bool python_printable(uint32_t c) {
    return !in_runs(kNotPrintable, sizeof(kNotPrintable) / sizeof(kNotPrintable[0]), c);
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
