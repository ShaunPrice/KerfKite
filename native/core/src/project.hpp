// Reading a project the way the Python reference does: .get() with and without defaults, the number()
// check and its messages, and Python's min, max and number text. Internal to the core.
#ifndef KERFKITE_PROJECT_HPP
#define KERFKITE_PROJECT_HPP

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "kerfkite/cam.hpp"
#include "kerfkite/number_text.hpp"

namespace kerfkite {
namespace detail {

using json = nlohmann::json;

// project.get(key): null when the key is absent.
inline const json *get(const json &object, const char *key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

// Python's None: an absent key read with .get(), or a JSON null.
inline bool none(const json *value) { return !value || value->is_null(); }

inline bool is_text(const json *value, const char *text) {
    return value && value->is_string() && value->get_ref<const std::string &>() == text;
}

// project.get(key, text) == text: an absent key takes the default; a null does not.
inline bool is_text_or_absent(const json *value, const char *text) { return !value || is_text(value, text); }

// Python's float(str) for ASCII text. False when Python raises ValueError.
inline bool float_from_text(const std::string &raw, double &out) {
    for (const unsigned char c : raw)
        if (c >= 0x80) throw NotPorted("numbers written in non-ASCII text");
    const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f); };
    size_t begin = 0, end = raw.size();
    while (begin < end && space(raw[begin])) ++begin;
    while (end > begin && space(raw[end - 1])) --end;
    const std::string text = raw.substr(begin, end - begin);
    size_t i = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) ++i;
    std::string word;
    for (size_t k = i; k < text.size(); ++k) word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[k]))));
    if (word == "inf" || word == "infinity") {
        out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    if (word == "nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    std::string clean = text.substr(0, i);
    // digit (["_"] digit)*
    const auto digits = [&](bool required) {
        size_t count = 0;
        while (i < text.size()) {
            if (text[i] >= '0' && text[i] <= '9') {
                clean.push_back(text[i++]);
                ++count;
            } else if (text[i] == '_' && count && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
                ++i;
            } else {
                break;
            }
        }
        return count > 0 || !required;
    };
    const size_t whole_from = clean.size();
    digits(false);
    const bool whole = clean.size() > whole_from;
    bool fraction = false;
    if (i < text.size() && text[i] == '.') {
        clean.push_back(text[i++]);
        const size_t fraction_from = clean.size();
        digits(false);
        fraction = clean.size() > fraction_from;
    }
    if (!whole && !fraction) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        clean.push_back(text[i++]);
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) clean.push_back(text[i++]);
        if (!digits(true)) return false;
    }
    if (i != text.size()) return false;
    out = std::strtod(clean.c_str(), nullptr);
    return true;
}

// A plain JSON number that Python reads as an exact float or int (never a bool). False for anything else.
inline bool plain_number(const json &value, double &out) {
    if (value.is_number_unsigned()) {
        out = static_cast<double>(value.get<uint64_t>());
    } else if (value.is_number_integer()) {
        out = static_cast<double>(value.get<int64_t>());
    } else if (value.is_number_float()) {
        out = value.get<double>();
        // A literal beyond the double range reads as infinity here, but may be a Python int there.
        if (!std::isfinite(out)) throw NotPorted("numbers outside the double range");
    } else {
        return false;
    }
    return true;
}

// studio.cam.number(value, name, low, high). low_text and high_text are the bounds as the reference's
// message writes them.
inline double number(const json *value, const std::string &name, double low, double high, const std::string &low_text,
              const std::string &high_text) {
    double result = 0;
    const bool readable = value && !value->is_boolean() &&
                          (plain_number(*value, result) ||
                           (value->is_string() && float_from_text(value->get_ref<const std::string &>(), result)));
    if (!readable) throw ValueError(name + " must be a number");
    if (!std::isfinite(result) || !(low <= result && result <= high))
        throw ValueError(name + " must be between " + low_text + " and " + high_text);
    return result;
}

// number(project.get(key, default), ...): the default is used only when the key is absent.
inline double setting(const json &project, const char *key, double fallback, const std::string &name, double low,
               double high, const std::string &low_text, const std::string &high_text) {
    const json *value = get(project, key);
    return value ? number(value, name, low, high, low_text, high_text) : fallback;
}

inline std::string fixed(double value, int decimals) {
    std::string out;
    if (!text::fixed(out, value, decimals)) throw std::logic_error("A validated number could not be written");
    return out;
}

inline double rounded(double value, int decimals) {
    double out;
    if (!text::rounded(value, decimals, out)) throw std::logic_error("A validated number could not be rounded");
    return out;
}

// Python's max(a, b) and min(a, b): the first argument unless the second is strictly greater or smaller.
inline double py_max(double a, double b) { return b > a ? b : a; }
inline double py_min(double a, double b) { return b < a ? b : a; }

}  // namespace detail
}  // namespace kerfkite

#endif
