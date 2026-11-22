// STL height fields: the port of studio/mesh_import.py. See kerfkite/stl_heightmap.hpp.
#include "kerfkite/stl_heightmap.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "base64.hpp"
#include "kerfkite/cam.hpp"
#include "project.hpp"

namespace kerfkite {
namespace {

using json = Json;
using detail::get;
using detail::py_max;
using detail::py_min;
using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;

constexpr size_t kMaxBytes = 8 * 1024 * 1024;
constexpr uint32_t kMaxTriangles = 50000;
constexpr int64_t kMaxCellChecks = 2000000;

// Python's whitespace in ASCII text, for str.strip(), str.isspace() and the regular expression \s.
bool space(unsigned char c) { return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Whether text at `at` begins with the lowercase word, in either case (re.IGNORECASE on ASCII).
bool starts_with_word(const std::string &text, size_t at, const char *word) {
    const size_t length = std::strlen(word);
    if (at > text.size() || text.size() - at < length) return false;
    for (size_t i = 0; i < length; ++i)
        if (lower(text[at + i]) != word[i]) return false;
    return true;
}

// str.splitlines() for ASCII text: lines end at \n, \r, \r\n, \v, \f, \x1c, \x1d and \x1e, and a final line
// break makes no empty line after it.
std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    size_t begin = 0, i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == '\x1c' || c == '\x1d' || c == '\x1e') {
            lines.push_back(text.substr(begin, i - begin));
            i += c == '\r' && i + 1 < text.size() && text[i + 1] == '\n' ? 2 : 1;
            begin = i;
        } else {
            ++i;
        }
    }
    if (begin < text.size()) lines.push_back(text.substr(begin));
    return lines;
}

// re.fullmatch(word + r'(?:\s.*)?', line, re.I): the word alone, or followed by whitespace and anything.
bool heading(const std::string &line, const char *word) {
    const size_t length = std::strlen(word);
    return starts_with_word(line, 0, word) &&
           (line.size() == length || space(static_cast<unsigned char>(line[length])));
}

// Reads one facet at `at`, as _FACET.match(body, at) does, giving its twelve numbers and where it ends.
class Facet {
public:
    explicit Facet(const std::string &body) : body_(body) {}

    bool read(size_t at, std::array<double, 12> &numbers, size_t &end) {
        at_ = at;
        size_t n = 0;
        if (!word("facet") || !gap() || !word("normal") || !gap() || !triple(numbers, n) || !gap() || !word("outer") ||
            !gap() || !word("loop") || !gap())
            return false;
        for (int vertex = 0; vertex < 3; ++vertex)
            if (!word("vertex") || !gap() || !triple(numbers, n) || !gap()) return false;
        if (!word("endloop") || !gap() || !word("endfacet")) return false;
        end = at_;
        return true;
    }

private:
    bool word(const char *text) {
        if (!starts_with_word(body_, at_, text)) return false;
        at_ += std::strlen(text);
        return true;
    }

    // \s+
    bool gap() {
        const size_t from = at_;
        while (at_ < body_.size() && space(static_cast<unsigned char>(body_[at_]))) ++at_;
        return at_ > from;
    }

    bool digit(size_t k) const { return k < body_.size() && body_[k] >= '0' && body_[k] <= '9'; }

    // [+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?, as float() reads it. The \s+ that follows every number
    // in the pattern leaves no other way to match it.
    bool number(double &out) {
        const size_t from = at_;
        size_t i = at_;
        if (i < body_.size() && (body_[i] == '+' || body_[i] == '-')) ++i;
        if (digit(i)) {
            while (digit(i)) ++i;
            if (i < body_.size() && body_[i] == '.') {
                ++i;
                while (digit(i)) ++i;
            }
        } else if (i < body_.size() && body_[i] == '.' && digit(i + 1)) {
            ++i;
            while (digit(i)) ++i;
        } else {
            return false;
        }
        if (i < body_.size() && (body_[i] == 'e' || body_[i] == 'E')) {
            size_t k = i + 1;
            if (k < body_.size() && (body_[k] == '+' || body_[k] == '-')) ++k;
            if (digit(k)) {
                while (digit(k)) ++k;
                i = k;
            }
        }
        detail::float_from_text(body_.substr(from, i - from), out);
        at_ = i;
        return true;
    }

    bool triple(std::array<double, 12> &numbers, size_t &n) {
        for (int k = 0; k < 3; ++k) {
            if (k && !gap()) return false;
            if (!number(numbers[n++])) return false;
        }
        return true;
    }

    const std::string &body_;
    size_t at_ = 0;
};

bool all_finite(const double *values, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i])) return false;
    return true;
}

// _triangles(raw): a binary STL when the record count matches the length exactly, otherwise ASCII.
std::vector<Triangle> triangles_of(const std::vector<unsigned char> &raw) {
    std::vector<Triangle> triangles;
    if (raw.size() >= 84) {
        const uint32_t count = uint32_t(raw[80]) | uint32_t(raw[81]) << 8 | uint32_t(raw[82]) << 16 | uint32_t(raw[83]) << 24;
        if (uint64_t(raw.size()) == 84 + 50 * uint64_t(count)) {
            if (count < 1 || count > kMaxTriangles) throw ValueError("STL must contain 1..50000 triangles");
            triangles.reserve(count);
            for (size_t offset = 84; offset < raw.size(); offset += 50) {
                double numbers[12];
                for (int k = 0; k < 12; ++k) {
                    const unsigned char *b = raw.data() + offset + 4 * k;
                    const uint32_t bits = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
                    float value;
                    std::memcpy(&value, &bits, sizeof value);
                    numbers[k] = value;
                }
                if (!all_finite(numbers, 12)) throw ValueError("STL coordinates and normals must be finite");
                triangles.push_back({Point{numbers[3], numbers[4], numbers[5]}, Point{numbers[6], numbers[7], numbers[8]},
                                     Point{numbers[9], numbers[10], numbers[11]}});
            }
            return triangles;
        }
    }
    for (const unsigned char c : raw)
        if (c >= 0x80) throw ValueError("Invalid binary STL record count or ASCII encoding");
    size_t first = 0, last = raw.size();
    while (first < last && space(raw[first])) ++first;
    while (last > first && space(raw[last - 1])) --last;
    const std::string text(raw.begin() + std::ptrdiff_t(first), raw.begin() + std::ptrdiff_t(last));
    const std::vector<std::string> lines = split_lines(text);
    if (lines.size() < 3 || !heading(lines.front(), "solid") || !heading(lines.back(), "endsolid"))
        throw ValueError("ASCII STL requires solid and endsolid lines");
    std::string body;
    for (size_t i = 1; i + 1 < lines.size(); ++i) {
        if (i > 1) body.push_back('\n');
        body += lines[i];
    }
    Facet facet(body);
    size_t position = 0;
    while (position < body.size()) {
        while (position < body.size() && space(static_cast<unsigned char>(body[position]))) ++position;
        if (position == body.size()) break;
        std::array<double, 12> numbers{};
        size_t end = 0;
        if (!facet.read(position, numbers, end)) throw ValueError("Malformed ASCII STL facet");
        if (!all_finite(numbers.data(), 12)) throw ValueError("STL coordinates and normals must be finite");
        triangles.push_back({Point{numbers[3], numbers[4], numbers[5]}, Point{numbers[6], numbers[7], numbers[8]},
                             Point{numbers[9], numbers[10], numbers[11]}});
        if (triangles.size() > kMaxTriangles) throw ValueError("STL exceeds 50000 triangles");
        position = end;
    }
    if (triangles.empty()) throw ValueError("STL contains no triangles");
    return triangles;
}

// _clip: the triangle's projection clipped to the closed raster cell (x, y), degeneracies included.
std::vector<Point> clip(const Triangle &points, int x, int y) {
    std::vector<Point> polygon(points.begin(), points.end()), output;
    struct Plane {
        int axis;
        double boundary, direction;
    };
    const Plane planes[4] = {{0, double(x), 1}, {0, double(x) + 1, -1}, {1, double(y), 1}, {1, double(y) + 1, -1}};
    for (const Plane &plane : planes) {
        if (polygon.empty()) return polygon;
        output.clear();
        Point previous = polygon.back();
        bool previous_in = plane.direction * (previous[plane.axis] - plane.boundary) >= -1e-10;
        for (const Point &current : polygon) {
            const bool current_in = plane.direction * (current[plane.axis] - plane.boundary) >= -1e-10;
            if (current_in != previous_in) {
                const double difference = current[plane.axis] - previous[plane.axis];
                if (difference != 0) {
                    const double t = py_max(0.0, py_min(1.0, (plane.boundary - previous[plane.axis]) / difference));
                    output.push_back({previous[0] + t * (current[0] - previous[0]), previous[1] + t * (current[1] - previous[1]),
                                      previous[2] + t * (current[2] - previous[2])});
                }
            }
            if (current_in) output.push_back(current);
            previous = current;
            previous_in = current_in;
        }
        polygon.swap(output);
    }
    return polygon;
}

json triple_of(const double *values) { return json::array({values[0], values[1], values[2]}); }

}  // namespace

Json stl_heightmap(const Json &data) {
    if (!data.is_object()) throw ValueError("STL input must be an object");
    const json *given = get(data, "resolution");
    int64_t resolution = 64;
    if (given) {
        // An int that is not a bool, in 2..128.
        bool whole = false;
        if (given->is_number_unsigned()) {
            const uint64_t value = given->get<uint64_t>();
            whole = value >= 2 && value <= 128;
            resolution = static_cast<int64_t>(value);
        } else if (given->is_number_integer()) {
            resolution = given->get<int64_t>();
            whole = resolution >= 2 && resolution <= 128;
        }
        if (!whole) throw ValueError("resolution must be an integer in 2..128");
    }
    const json *content = get(data, "content_base64");
    if (!content || !content->is_string()) throw ValueError("STL content must be base64, at most 8 MiB decoded");
    const std::string &encoded = content->get_ref<const std::string &>();
    size_t characters = 0;  // len(content): code points, not bytes
    for (const unsigned char c : encoded) characters += (c & 0xc0) != 0x80;
    if (characters > 4 * ((kMaxBytes + 2) / 3)) throw ValueError("STL content must be base64, at most 8 MiB decoded");
    std::vector<unsigned char> raw;
    if (!detail::base64(encoded, raw)) throw ValueError("Invalid STL base64 data");
    if (raw.empty() || raw.size() > kMaxBytes) throw ValueError("STL must contain 1 byte..8 MiB");
    const std::vector<Triangle> triangles = triangles_of(raw);

    // min() and max() keep the first of equal values, as Python's do.
    double minimum[3], maximum[3], spans[3];
    for (int i = 0; i < 3; ++i) minimum[i] = maximum[i] = triangles[0][0][i];
    for (const Triangle &triangle : triangles)
        for (const Point &p : triangle)
            for (int i = 0; i < 3; ++i) {
                minimum[i] = py_min(minimum[i], p[i]);
                maximum[i] = py_max(maximum[i], p[i]);
            }
    for (int i = 0; i < 3; ++i) spans[i] = maximum[i] - minimum[i];
    for (int i = 0; i < 3; ++i)
        for (const double v : {minimum[i], maximum[i], spans[i]})
            if (!std::isfinite(v) || std::fabs(v) > 1e12) throw ValueError("STL coordinates exceed supported numeric range");
    // The check above is over minimum + maximum + spans in the reference; every value is checked either way.
    if (spans[0] < 1e-9 || spans[1] < 1e-9) throw ValueError("STL must have nonzero X and Y extent");

    const int cells = static_cast<int>(resolution) - 1;
    struct Prepared {
        Triangle projected;
        int x0, x1, y0, y1;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(triangles.size());
    int64_t checks = 0;
    for (const Triangle &triangle : triangles) {
        Prepared item;
        for (int k = 0; k < 3; ++k) {
            const Point &p = triangle[k];
            item.projected[k] = {(p[0] - minimum[0]) / spans[0] * cells, (p[1] - minimum[1]) / spans[1] * cells,
                                 spans[2] == 0 ? 1.0 : (p[2] - minimum[2]) / spans[2]};
        }
        const Triangle &q = item.projected;
        const double low_x = py_min(py_min(q[0][0], q[1][0]), q[2][0]), high_x = py_max(py_max(q[0][0], q[1][0]), q[2][0]);
        const double low_y = py_min(py_min(q[0][1], q[1][1]), q[2][1]), high_y = py_max(py_max(q[0][1], q[1][1]), q[2][1]);
        // One neighbouring cell is included at lower boundaries for robustness, as in the reference.
        item.x0 = static_cast<int>(std::max<int64_t>(0, static_cast<int64_t>(std::floor(low_x)) - 1));
        item.x1 = static_cast<int>(std::min<int64_t>(cells - 1, static_cast<int64_t>(std::floor(high_x))));
        item.y0 = static_cast<int>(std::max<int64_t>(0, static_cast<int64_t>(std::floor(low_y)) - 1));
        item.y1 = static_cast<int>(std::min<int64_t>(cells - 1, static_cast<int64_t>(std::floor(high_y))));
        checks += int64_t(item.x1 - item.x0 + 1) * int64_t(item.y1 - item.y0 + 1);
        if (checks > kMaxCellChecks)
            throw ValueError("STL raster workload too large; reduce resolution or simplify the mesh");
        prepared.push_back(item);
    }
    std::vector<double> levels(size_t(cells) * cells, 0.0);
    std::vector<char> set(size_t(cells) * cells, 0);
    for (const Prepared &item : prepared) {
        for (int y = item.y0; y <= item.y1; ++y) {
            for (int x = item.x0; x <= item.x1; ++x) {
                const std::vector<Point> clipped = clip(item.projected, x, y);
                if (clipped.empty()) continue;
                double top = clipped[0][2];
                for (size_t k = 1; k < clipped.size(); ++k) top = py_max(top, clipped[k][2]);
                const double level = py_max(0.0, py_min(1.0, top));
                const size_t cell = size_t(y) * cells + x;
                levels[cell] = set[cell] ? py_max(levels[cell], level) : level;
                set[cell] = 1;
            }
        }
    }
    json heightmap = json::array();
    for (int y = 0; y < resolution; ++y) {
        json row = json::array();
        for (int x = 0; x < resolution; ++x) {
            bool empty = false, any = false;
            double highest = 0;
            for (const int j : {y - 1, y})
                for (const int i : {x - 1, x}) {
                    if (i < 0 || i >= cells || j < 0 || j >= cells) continue;
                    const size_t cell = size_t(j) * cells + i;
                    if (!set[cell]) {
                        empty = true;
                        continue;
                    }
                    highest = any ? py_max(highest, levels[cell]) : levels[cell];
                    any = true;
                }
            row.push_back(empty ? 1.0 : highest);
        }
        heightmap.push_back(std::move(row));
    }
    json bounds = json::object();
    bounds["min"] = triple_of(minimum);
    bounds["max"] = triple_of(maximum);
    bounds["size"] = triple_of(spans);
    bounds["triangles"] = static_cast<int64_t>(triangles.size());
    bounds["units"] = "unspecified";
    json result = json::object();
    result["heightmap"] = std::move(heightmap);
    result["source_bounds"] = std::move(bounds);
    result["warnings"] = json::array({
        "STL units are unspecified. Set output width, height and relief depth explicitly; normalization can change "
        "proportions.",
        "Conservative top-access envelope: clipped triangle maxima raise intersected cells, leaving extra stock at "
        "finite raster resolution. Thin peaks are retained, not sampled away.",
        "Empty exterior cells remain at stock top. Cells partly intersecting the mesh use its conservative envelope; "
        "exterior portions within those boundary cells are not protected.",
        "No solid, watertightness, undercut, fixture or holder analysis. Review the height field and toolpaths before "
        "machining.",
    });
    return result;
}

bool stl_command(const std::string &command) { return command == "stl-heightmap"; }

Json stl_call(const std::string &, const Json &request) { return stl_heightmap(request); }

}  // namespace kerfkite
