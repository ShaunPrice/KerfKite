// What the Gerber and Excellon importers share: decimal text read without the C locale, text made safe to
// repeat in an answer, a bounded list of warnings, and the limits common to both. Internal to the core.
#ifndef KERFKITE_PCB_TEXT_HPP
#define KERFKITE_PCB_TEXT_HPP

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "kerfkite/number_text.hpp"
#include "project.hpp"

namespace kerfkite {
namespace pcb {

using json = Json;

const double MAX_COORDINATE = 10000;  // millimetres either side of the origin
const double INCH = 25.4;

struct Point {
    double x = 0, y = 0;
};

// 10^n for n up to 22: every one is an exact double.
inline double power_of_ten(int n) {
    static const double table[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return table[n < 0 ? 0 : n > 22 ? 22 : n];
}

inline bool digit(char c) { return c >= '0' && c <= '9'; }

// [+-]digits[.digits] with at least one digit, and nothing else. strtod is not used because it follows the
// C locale. Up to eighteen significant digits are kept, which is beyond what a double holds.
inline bool decimal(const std::string &text, size_t begin, size_t end, double &out) {
    size_t i = begin;
    bool negative = false;
    if (i < end && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    uint64_t mantissa = 0;
    int kept = 0, places = 0;
    bool any = false, fraction = false;
    for (; i < end; ++i) {
        const char c = text[i];
        if (c == '.' && !fraction) {
            fraction = true;
            continue;
        }
        if (!digit(c)) return false;
        any = true;
        if (kept == 18) {
            if (!fraction) return false;  // a whole part this long is not a dimension
            continue;
        }
        // Fractional digits past the twenty-second are below any dimension, and past the end of the table.
        if (fraction && places == 22) continue;
        if (mantissa || c != '0') ++kept;
        mantissa = mantissa * 10 + static_cast<uint64_t>(c - '0');
        if (fraction) ++places;
    }
    if (!any) return false;
    const double value = static_cast<double>(mantissa) / power_of_ten(places);
    out = negative ? -value : value;
    return true;
}

// The digits text[begin, end) as a number; the caller has checked that they are digits and how many.
inline double whole_number(const std::string &text, size_t begin, size_t end) {
    double value = 0;
    for (size_t i = begin; i < end; ++i) value = value * 10 + (text[i] - '0');
    return value;
}

inline bool decimal(const std::string &text, double &out) { return decimal(text, 0, text.size(), out); }

// Text from a file, safe to repeat in a message or an answer: printable ASCII only, and not too long.
inline std::string printable(const std::string &raw, size_t limit = 60) {
    std::string out;
    for (const char c : raw) {
        if (out.size() == limit) {
            out += "...";
            break;
        }
        out.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    return out;
}

// To the nanometre: finer than any board file, and it keeps the answer short.
inline double round6(double value) {
    uint64_t q = 0;
    if (!text::scaled_round(value, 1000000, q) || q > (1ULL << 53)) return value;
    const double out = static_cast<double>(q) / 1000000;  // exact operands, so the nearest double to the decimal
    return value < 0 && q ? -out : out;
}

inline json xy(const Point &p) { return json::array({round6(p.x), round6(p.y)}); }

// A number for a message, to three places.
inline std::string mm(double value) {
    std::string out;
    return text::fixed(out, value, 3) ? out : std::string("?");
}

// Each distinct warning once, and at most fifty of them.
struct Warnings {
    std::vector<std::string> list;
    void add(const std::string &warning) {
        for (const std::string &known : list)
            if (known == warning) return;
        if (list.size() == 50) return;
        list.push_back(list.size() == 49 ? std::string("Further warnings are not listed") : warning);
    }
};

// The file text of a request, within a size limit. A field that is not one of those named is refused: a
// misspelt option would otherwise be left out without a word.
inline const std::string &file_text(const json &request, const char *what, size_t limit,
                                    std::initializer_list<const char *> fields) {
    if (!request.is_object()) throw ValueError(std::string("The ") + what + " request must be an object");
    for (const auto &item : request.items()) {
        bool known = item.key() == "text";
        for (const char *field : fields) known = known || item.key() == field;
        if (!known)
            throw ValueError(std::string("The ") + what +
                             " request has a field that is not known: " + printable(item.key()));
    }
    const json *value = detail::get(request, "text");
    if (!value || !value->is_string())
        throw ValueError(std::string("The ") + what + " request needs the file as \"text\"");
    const std::string &content = value->get_ref<const std::string &>();
    if (content.size() > limit)
        throw ValueError(std::string("The ") + what + " file is larger than " + std::to_string(limit >> 20) + " MiB");
    return content;
}

// Where the file starts: after a UTF-8 byte order mark, which some editors put first.
inline size_t after_mark(const std::string &text) { return text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0; }

// "offset":[dx, dy], added to every coordinate of a conversion; absent means none.
inline Point offset_of(const json &request) {
    Point offset;
    const json *value = detail::get(request, "offset");
    if (!value) return offset;
    if (!value->is_array() || value->size() != 2) throw ValueError("Give the offset as [X, Y]");
    offset.x = detail::number(&(*value)[0], "Offset X", -MAX_COORDINATE, MAX_COORDINATE, "-10000", "10000");
    offset.y = detail::number(&(*value)[1], "Offset Y", -MAX_COORDINATE, MAX_COORDINATE, "-10000", "10000");
    return offset;
}

// "width" and "height" of the work area, when the caller wants the fit checked. False when neither is given.
inline bool work_area(const json &request, double &width, double &height) {
    const json *w = detail::get(request, "width"), *h = detail::get(request, "height");
    if (!w && !h) return false;
    if (!w || !h) throw ValueError("Give both the work width and the work height, or neither");
    width = detail::number(w, "Work width", 1, 2000, "1", "2000");
    height = detail::number(h, "Work height", 1, 2000, "1", "2000");
    return true;
}

}  // namespace pcb
}  // namespace kerfkite

#endif
