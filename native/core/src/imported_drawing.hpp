// The artwork an importer reads from a foreign drawing, shared by the readers of program_import.cpp and
// dxf.cpp: studio/program_import.py's Drawing, with its limits, its batching of long runs of contours and the
// report Studio's import review shows. Internal to the core.
#ifndef KERFKITE_IMPORTED_DRAWING_HPP
#define KERFKITE_IMPORTED_DRAWING_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/decimal.hpp"
#include "kerfkite/json.hpp"

namespace kerfkite {
namespace imported {

using json = Json;

const double kTolerance = 0.025;  // millimetres
const size_t kMaxPoints = 100000;
const size_t kMaxPaths = 3000;

// format(value, 'g'): six significant digits, rounded half to even on the exact binary value, in fixed
// notation for decimal exponents -4 to 5 and exponent notation otherwise, trailing zeros removed.
inline std::string general(double value) {
    if (std::isnan(value)) return "nan";
    std::string out = std::signbit(value) ? "-" : "";
    value = std::fabs(value);
    if (std::isinf(value)) return out + "inf";
    if (value == 0) return out + "0";
    int exponent2 = 0;
    const double fraction = std::frexp(value, &exponent2);
    const uint64_t mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
    const int shift = exponent2 - 53;  // value = mantissa * 2^shift
    // The decimal exponent of the leading digit: an estimate, then checked exactly.
    int e10 = static_cast<int>(std::floor(std::log10(value)));
    const auto ratio = [&](int scale, text::Big &numerator, text::Big &denominator) {
        // value * 10^scale as numerator / denominator.
        numerator = text::Big(mantissa);
        denominator = text::Big(1);
        if (shift > 0) numerator.shift_left(shift);
        else denominator.shift_left(-shift);
        if (scale > 0) numerator.multiply_pow10(scale);
        else denominator.multiply_pow10(-scale);
    };
    const auto below = [&](int power) {  // value < 10^power
        text::Big numerator, denominator;
        ratio(-power, numerator, denominator);
        return numerator.compare(denominator) < 0;
    };
    while (below(e10)) --e10;
    while (!below(e10 + 1)) ++e10;
    // round(value * 10^(5 - e10)), half to even.
    text::Big numerator, denominator;
    ratio(5 - e10, numerator, denominator);
    uint64_t low = 0, high = 10000000;  // the quotient lies in [10^5, 10^6)
    while (high - low > 1) {
        const uint64_t middle = (low + high) / 2;
        text::Big product = denominator;
        product.multiply_add(static_cast<uint32_t>(middle), 0);
        if (product.compare(numerator) <= 0) low = middle;
        else high = middle;
    }
    text::Big remainder = numerator, taken = denominator;
    taken.multiply_add(static_cast<uint32_t>(low), 0);
    remainder.subtract(taken);
    text::Big twice = remainder;
    twice.add(remainder);
    const int against_half = twice.compare(denominator);
    uint64_t digits = low;
    if (against_half > 0 || (against_half == 0 && (digits & 1))) ++digits;
    if (digits == 1000000) {
        digits = 100000;
        ++e10;
    }
    std::string text = std::to_string(digits);  // six digits
    if (-4 <= e10 && e10 < 6) {
        std::string fixed;
        if (e10 < 0) fixed = "0." + std::string(static_cast<size_t>(-e10 - 1), '0') + text;
        else fixed = text.substr(0, static_cast<size_t>(e10) + 1) + "." + text.substr(static_cast<size_t>(e10) + 1);
        while (!fixed.empty() && fixed.back() == '0') fixed.pop_back();
        if (!fixed.empty() && fixed.back() == '.') fixed.pop_back();
        return out + fixed;
    }
    std::string mantissa_text = text.substr(0, 1) + "." + text.substr(1);
    while (mantissa_text.back() == '0') mantissa_text.pop_back();
    if (mantissa_text.back() == '.') mantissa_text.pop_back();
    const int size = e10 < 0 ? -e10 : e10;
    std::string exponent = std::to_string(size);
    if (exponent.size() < 2) exponent = "0" + exponent;
    return out + mantissa_text + "e" + (e10 < 0 ? "-" : "+") + exponent;
}


using Point = std::array<double, 2>;

struct Object {
    std::string name, layer;
    std::vector<std::vector<Point>> paths;
    std::vector<std::vector<double>> depths, source_z;
    bool has_depths = false, has_source_z = false;
};

// text[:count] for text in UTF-8: the first count code points.
inline std::string prefix(const std::string &text, size_t count) {
    size_t at = 0, seen = 0;
    while (at < text.size()) {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80) {
            if (seen == count) break;
            ++seen;
        }
        ++at;
    }
    return text.substr(0, at);
}

inline json points_json(const std::vector<Point> &points) {
    json list = json::array();
    for (const Point &p : points) list.push_back(json::array({p[0], p[1]}));
    return list;
}

// program_import.Drawing.
class Drawing {
public:
    std::vector<Object> objects;
    std::vector<std::string> layers;
    std::map<std::string, json> layer_extras;  // members a reader adds to a layer beside its key and name, in order
    json presets = json::array();
    std::vector<std::string> warnings;
    std::string mode;  // empty for None
    size_t points = 0;

    bool has_layer(const std::string &layer) const { return std::find(layers.begin(), layers.end(), layer) != layers.end(); }

    void path(const std::string &name, const std::string &layer, const std::vector<Point> &path, const std::vector<double> *depths = nullptr) {
        if (path.size() < 2) return;
        points += path.size();
        if (points > kMaxPoints || objects.size() >= kMaxPaths) throw ValueError("Simplify the drawing to 100000 vertices and 3000 paths");
        for (const Point &p : path)
            for (const double v : p)
                if (!std::isfinite(v) || std::fabs(v) > 2000)
                    throw ValueError("Move or scale the drawing into the portable project ±2000 mm XY range");
        Object object;
        object.name = prefix(name, 100);
        object.layer = layer;
        object.paths.push_back(path);
        if (depths) {
            bool cutting = true;
            for (const double z : *depths) cutting = cutting && -50 <= z && z <= 0;
            if (cutting && *std::min_element(depths->begin(), depths->end()) < 0) {
                object.has_depths = true;
                object.depths.push_back(*depths);
            } else {
                object.has_source_z = true;
                object.source_z.push_back(*depths);
            }
        }
        if (!has_layer(layer)) layers.push_back(layer);
        objects.push_back(std::move(object));
    }

    json finish() {
        if (objects.empty()) {
            std::string message = "File contains no supported drawing paths";
            if (!warnings.empty()) {
                message += ". ";
                for (size_t i = 0; i < warnings.size(); ++i) message += (i ? " " : "") + warnings[i];
            }
            throw ValueError(message);
        }
        // The sender accepts at most 100 ordered operations: long consecutive runs with the same layer and
        // depth semantics are batched.
        if (objects.size() > 100) {
            std::vector<Object> groups;
            for (Object &object : objects) {
                if (!groups.empty() && groups.back().layer == object.layer && groups.back().has_depths == object.has_depths &&
                    groups.back().has_source_z == object.has_source_z) {
                    Object &group = groups.back();
                    group.name = prefix(group.layer, 80) + " contours";
                    group.paths.insert(group.paths.end(), object.paths.begin(), object.paths.end());
                    group.depths.insert(group.depths.end(), object.depths.begin(), object.depths.end());
                    group.source_z.insert(group.source_z.end(), object.source_z.begin(), object.source_z.end());
                } else {
                    groups.push_back(std::move(object));
                }
            }
            objects = std::move(groups);
        }
        json list = json::array();
        for (const Object &object : objects) {
            json entry = json::object();
            entry["name"] = object.name;
            json paths = json::array();
            for (const auto &path : object.paths) paths.push_back(points_json(path));
            entry["paths"] = std::move(paths);
            entry["sourceLayer"] = object.layer;
            if (object.has_depths) entry["depths"] = json(object.depths);
            if (object.has_source_z) entry["sourceZ"] = json(object.source_z);
            list.push_back(std::move(entry));
        }
        json layer_list = json::array();
        for (const std::string &layer : layers) {
            json entry = json{{"key", layer}, {"name", layer}};
            const auto extra = layer_extras.find(layer);
            if (extra != layer_extras.end())
                for (auto member = extra->second.begin(); member != extra->second.end(); ++member) entry[member.key()] = member.value();
            layer_list.push_back(std::move(entry));
        }
        json result = json::object();
        result["objects"] = std::move(list);
        result["layers"] = std::move(layer_list);
        result["presets"] = presets;
        result["devices"] = json::array();
        result["warnings"] = json(warnings);
        result["mode"] = mode.empty() ? json() : json(mode);
        return result;
    }
};


}  // namespace imported
}  // namespace kerfkite

#endif
