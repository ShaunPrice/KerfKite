// STL files, binary and ASCII. Numbers in ASCII files are read by this file's own exact decimal
// conversion, not the C library's, so the grid points of an imported solid do not depend on the
// platform or its locale.
#include <cmath>
#include <cstring>

#include "kerfkite/number_text.hpp"
#include "mesh.hpp"

namespace kerfkite {
namespace solid {

namespace {

float read_float(const unsigned char *p) {
    const uint32_t bits = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void write_u32(std::string &out, uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<char>((v >> (8 * k)) & 0xff));
}

void write_float(std::string &out, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    write_u32(out, bits);
}

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

struct Reader {
    const std::string &text;
    size_t at;
    void skip() {
        while (at < text.size() && space(text[at])) ++at;
    }
    // The next word, lower-cased; empty at the end.
    std::string word() {
        skip();
        std::string w;
        while (at < text.size() && !space(text[at])) {
            const char c = text[at++];
            w.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
        return w;
    }
    void rest_of_line() {
        while (at < text.size() && text[at] != '\n') ++at;
    }
};

// A decimal number as a double: the digits are gathered into an integer (up to 19 significant ones) and
// scaled by powers of ten, each step one correctly rounded operation, so the result is the same double on
// every platform and within a few units in the last place of the true value. Coordinates are rounded to
// the grid afterwards, which is far coarser.
bool number(const std::string &w, double &out) {
    size_t i = 0;
    bool negative = false;
    if (i < w.size() && (w[i] == '+' || w[i] == '-')) negative = w[i++] == '-';
    uint64_t mantissa = 0;
    int digits = 0, exponent = 0;
    bool any = false, point = false;
    for (; i < w.size(); ++i) {
        const char c = w[i];
        if (c == '.') {
            if (point) return false;
            point = true;
        } else if (c >= '0' && c <= '9') {
            any = true;
            if (digits < 19) {
                mantissa = mantissa * 10 + static_cast<uint64_t>(c - '0');
                if (mantissa) ++digits;
                if (point) --exponent;
            } else if (!point) {
                ++exponent;
            }
        } else {
            break;
        }
    }
    if (!any) return false;
    if (i < w.size()) {
        if (w[i] != 'e') return false;
        ++i;
        bool minus = false;
        if (i < w.size() && (w[i] == '+' || w[i] == '-')) minus = w[i++] == '-';
        if (i == w.size()) return false;
        int e = 0;
        for (; i < w.size(); ++i) {
            if (w[i] < '0' || w[i] > '9') return false;
            if (e < 100000) e = e * 10 + (w[i] - '0');
        }
        exponent += minus ? -e : e;
    }
    double value = static_cast<double>(mantissa);
    if (mantissa) {
        if (exponent > 40 || exponent < -400) return exponent < 0 ? (out = negative ? -0.0 : 0.0, true) : false;
        static const double tens[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
        int e = exponent;
        while (e > 0) {
            const int step = e > 22 ? 22 : e;
            value *= tens[step];
            e -= step;
        }
        while (e < 0) {
            const int step = -e > 22 ? 22 : -e;
            value /= tens[step];
            e += step;
        }
    }
    out = negative ? -value : value;
    return true;
}

}  // namespace

std::vector<StlTriangle> read_stl(const std::string &bytes, std::size_t max_triangles) {
    if (bytes.size() > kMaxStlBytes) throw ValueError("STL: the file is larger than " + std::to_string(kMaxStlBytes / (1024 * 1024)) + " MiB");
    std::vector<StlTriangle> out;
    // A binary file is recognised by its length, which must match its triangle count exactly; a header
    // that happens to begin with "solid" does not make it text.
    if (bytes.size() >= 84) {
        const unsigned char *raw = reinterpret_cast<const unsigned char *>(bytes.data());
        const uint64_t count = static_cast<uint64_t>(raw[80]) | static_cast<uint64_t>(raw[81]) << 8 | static_cast<uint64_t>(raw[82]) << 16 | static_cast<uint64_t>(raw[83]) << 24;
        if (bytes.size() == 84 + 50 * count) {
            if (count == 0) throw ValueError("STL: the file has no triangles");
            if (count > max_triangles) throw ValueError("STL: more than " + std::to_string(max_triangles) + " triangles");
            out.resize(static_cast<size_t>(count));
            for (size_t i = 0; i < out.size(); ++i) {
                const unsigned char *record = raw + 84 + 50 * i;
                double c[9];
                for (int k = 0; k < 9; ++k) {
                    c[k] = static_cast<double>(read_float(record + 12 + 4 * k));
                    if (!(std::fabs(c[k]) <= 1e30)) throw ValueError("STL: a coordinate is not a finite number");
                }
                out[i] = StlTriangle{{c[0], c[1], c[2]}, {c[3], c[4], c[5]}, {c[6], c[7], c[8]}};
            }
            return out;
        }
    }
    Reader r{bytes, 0};
    if (r.word() != "solid") throw ValueError("STL: neither a binary file of the right length nor text beginning with \"solid\"");
    r.rest_of_line();
    for (;;) {
        std::string w = r.word();
        if (w == "endsolid") break;
        if (w == "solid") {  // some files hold several solids one after another
            r.rest_of_line();
            continue;
        }
        if (w.empty()) throw ValueError("STL: the text ends without \"endsolid\"");
        if (w != "facet" || r.word() != "normal") throw ValueError("STL: expected \"facet normal\" at byte " + std::to_string(r.at));
        double unused;
        for (int k = 0; k < 3; ++k)
            if (!number(r.word(), unused)) throw ValueError("STL: a normal is not a number, at byte " + std::to_string(r.at));
        if (r.word() != "outer" || r.word() != "loop") throw ValueError("STL: expected \"outer loop\" at byte " + std::to_string(r.at));
        double c[9];
        for (int v = 0; v < 3; ++v) {
            if (r.word() != "vertex") throw ValueError("STL: expected \"vertex\" at byte " + std::to_string(r.at));
            for (int k = 0; k < 3; ++k)
                if (!number(r.word(), c[3 * v + k]) || !(std::fabs(c[3 * v + k]) <= 1e30)) throw ValueError("STL: a coordinate is not a finite number, at byte " + std::to_string(r.at));
        }
        if (r.word() != "endloop" || r.word() != "endfacet") throw ValueError("STL: expected \"endloop\" and \"endfacet\" at byte " + std::to_string(r.at));
        if (out.size() >= max_triangles) throw ValueError("STL: more than " + std::to_string(max_triangles) + " triangles");
        out.push_back(StlTriangle{{c[0], c[1], c[2]}, {c[3], c[4], c[5]}, {c[6], c[7], c[8]}});
    }
    if (out.empty()) throw ValueError("STL: the file has no triangles");
    return out;
}

Solid from_stl(const std::string &bytes, std::size_t max_triangles) {
    const std::vector<StlTriangle> triangles = read_stl(bytes, max_triangles);
    std::vector<GridPoint> v;
    std::vector<Triangle> t;
    v.reserve(triangles.size() * 3);
    t.reserve(triangles.size());
    for (const StlTriangle &s : triangles) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        for (const Vec3 *p : {&s.a, &s.b, &s.c}) v.push_back(GridPoint{{detail::to_grid(p->x), detail::to_grid(p->y), detail::to_grid(p->z)}});
        t.push_back(Triangle{{base, base + 1, base + 2}});
    }
    return from_grid_mesh(v, t, true);
}

std::string to_stl_binary(const Solid &solid, const std::string &header) {
    const detail::Mesh &m = detail::Access::mesh(solid);
    std::string out(80, '\0');
    std::memcpy(&out[0], header.data(), header.size() < 80 ? header.size() : 80);
    out.reserve(84 + 50 * m.t.size());
    write_u32(out, static_cast<uint32_t>(m.t.size()));
    for (const Triangle &t : m.t) {
        // The normal as Studio's writer computes it: the cross product of two edges in millimetres,
        // divided by its length, then narrowed to single precision.
        double p[3][3];
        for (size_t c = 0; c < 3; ++c)
            for (size_t k = 0; k < 3; ++k) p[c][k] = detail::to_mm(m.v[t[c]][k]);
        const double ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
        const double vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
        double n[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; ++k) write_float(out, static_cast<float>(n[k] / (length == 0 ? 1 : length)));
        for (size_t c = 0; c < 3; ++c)
            for (size_t k = 0; k < 3; ++k) write_float(out, static_cast<float>(p[c][k]));
        out.push_back('\0');
        out.push_back('\0');
    }
    return out;
}

namespace {

// A grid coordinate as millimetres, exactly: whole millimetres, then up to six decimals.
void append_mm(std::string &out, int64_t units) {
    if (units < 0) out.push_back('-');
    const uint64_t u = units < 0 ? ~static_cast<uint64_t>(units) + 1 : static_cast<uint64_t>(units);
    out += std::to_string(u / 1000000);
    uint64_t fraction = u % 1000000;
    if (!fraction) return;
    char digits[7] = "000000";
    for (int k = 5; k >= 0; --k) {
        digits[k] = static_cast<char>('0' + fraction % 10);
        fraction /= 10;
    }
    int length = 6;
    while (digits[length - 1] == '0') --length;
    out.push_back('.');
    out.append(digits, static_cast<size_t>(length));
}

}  // namespace

std::string to_stl_ascii(const Solid &solid, const std::string &name) {
    const detail::Mesh &m = detail::Access::mesh(solid);
    std::string label;
    for (const char c : name)
        if (c > ' ' && c < 127) label.push_back(c);
    std::string out = "solid " + label + "\n";
    const std::vector<Vec3> normals = solid.triangle_normals();
    for (size_t i = 0; i < m.t.size(); ++i) {
        out += "facet normal";
        for (const double component : {normals[i].x, normals[i].y, normals[i].z}) {
            std::string text;
            if (!text::fixed(text, component, 9)) text = "0";
            out.push_back(' ');
            out += text;
        }
        out += "\nouter loop\n";
        for (size_t c = 0; c < 3; ++c) {
            out += "vertex";
            for (size_t k = 0; k < 3; ++k) {
                out.push_back(' ');
                append_mm(out, m.v[m.t[i][c]][k]);
            }
            out.push_back('\n');
        }
        out += "endloop\nendfacet\n";
    }
    out += "endsolid " + label + "\n";
    return out;
}

}  // namespace solid
}  // namespace kerfkite
