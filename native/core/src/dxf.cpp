// The core's own reader of ASCII DXF drawings, written from the DXF format itself, for Studio's import
// review (/api/program-import). It reads what studio/program_import.py's DXF reader reads, through the same
// steps and with the same report:
//
// * the drawing units ($INSUNITS), or the unit chosen for a unitless drawing;
// * model-space LINE, LWPOLYLINE, POLYLINE (2D with bulges, and 3D), CIRCLE, ARC, ELLIPSE and SPLINE entities,
//   each in its object coordinate system, flattened to a chord of 0.025 mm in millimetres;
// * block references (INSERT) with their base point, scale, rotation and extrusion, arrays of them, nested
//   blocks, and planar XCLIP boundaries, which keep only the visible parts of each contour;
// * layers in the order their first outline appears, with the layer table's colour and whether it is shown;
// * entities it does not draw, counted by type, and every limit and message of the reference.
//
// The reference reads the file with ezdxf and flattens ezdxf's Bézier paths; this reader flattens the true
// curves. The outlines therefore agree to the tolerance, not vertex for vertex: arcs and circles are divided
// into equal steps whose sagitta is at most 0.025 mm, and splines are divided until every midpoint is within
// 0.025 mm of its chord. validation/dxf_import_check.py compares the two. A spline given only by fit points is
// interpolated as CAD programs and ezdxf interpolate it (fit_point_spline below). The core's own rules, where the
// reference stops with an error that is no refusal or its answer turns on rounding (own_rule, kerfkite/cam.hpp):
// a repeated fit point is passed through once, a zero end tangent leaves the ends free, an XCLIP boundary's
// validity is decided exactly, and nested windows beyond the polygon kernel's range are refused.
#include "dxf_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "aci_colors.hpp"
#include "imported_drawing.hpp"
#include "kerfkite/cam.hpp"
#include "kerfkite/line_check.hpp"
#include "kerfkite/polygon_command.hpp"
#include "project.hpp"
#include "py_trig.hpp"

namespace kerfkite {

namespace {

using json = Json;
using namespace imported;

const double kPi = pytrig::PI;
const double kTau = 2 * pytrig::PI;
const size_t kMaxEntities = 20000;

[[noreturn]] void unreadable() { throw ValueError("Cannot read this ASCII DXF drawing"); }
[[noreturn]] void malformed() { throw ValueError("Malformed or unsupported DXF entity"); }

// ---- the file's group codes and values ---------------------------------------------------------------

struct Tag {
    int code;
    std::string value;
};

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string trimmed(const std::string &text) {
    size_t begin = 0, end = text.size();
    while (begin < end && blank(text[begin])) ++begin;
    while (end > begin && blank(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// int(text) for a group code or an integer value.
bool read_integer(const std::string &raw, long long &out) {
    const std::string text = trimmed(raw);
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    if (i >= text.size()) return false;
    long long value = 0;
    bool digit_before = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '_' && digit_before && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') continue;
        if (c < '0' || c > '9') return false;
        if (value > 100000000000000000LL) return false;
        value = value * 10 + (c - '0');
        digit_before = true;
    }
    out = negative ? -value : value;
    return true;
}

double real_value(const std::string &raw) {
    for (const unsigned char c : raw)
        if (c >= 0x80) unreadable();
    double value = 0;
    if (!detail::float_from_text(raw, value)) unreadable();
    return value;
}

long long integer_value(const std::string &raw) {
    long long value = 0;
    if (!read_integer(raw, value)) unreadable();
    return value;
}

std::vector<Tag> read_tags(const std::string &text) {
    std::vector<Tag> tags;
    size_t at = 0;
    const auto line = [&](std::string &out) {
        if (at >= text.size()) return false;
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        out = text.substr(at, end - at);
        at = end + 1;
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    };
    std::string code, value;
    while (line(code)) {
        if (at >= text.size() && trimmed(code).empty()) break;  // a blank last line
        if (!line(value)) unreadable();
        long long number = 0;
        if (!read_integer(code, number) || number < 0 || number > 1071) unreadable();
        if (number == 999) continue;  // a comment
        tags.push_back({static_cast<int>(number), value});
    }
    return tags;
}

// ---- the drawing's records -----------------------------------------------------------------------------

struct Record {
    std::string type;
    std::vector<Tag> tags;  // after the type: outside application groups, before extended data
    std::string handle, xdict;
    std::vector<Record> vertices;  // a POLYLINE's VERTEX records
};

// The records of a run of tags, from one (0, type) to the next.
std::vector<Record> records(const std::vector<Tag> &tags, size_t begin, size_t end) {
    std::vector<Record> out;
    size_t i = begin;
    while (i < end) {
        if (tags[i].code != 0) unreadable();
        Record record;
        record.type = trimmed(tags[i].value);
        ++i;
        bool extended = false;
        while (i < end && tags[i].code != 0) {
            const Tag &tag = tags[i];
            if (tag.code >= 1000) extended = true;
            if (extended) {
                ++i;
                continue;
            }
            if (tag.code == 102 && !tag.value.empty() && tag.value[0] == '{') {
                const bool dictionary = trimmed(tag.value) == "{ACAD_XDICTIONARY";
                ++i;
                while (i < end && tags[i].code != 0 && !(tags[i].code == 102 && trimmed(tags[i].value) == "}")) {
                    if (dictionary && tags[i].code == 360) record.xdict = trimmed(tags[i].value);
                    ++i;
                }
                if (i < end && tags[i].code == 102) ++i;
                continue;
            }
            if (tag.code == 5 || (tag.code == 105 && record.handle.empty())) record.handle = trimmed(tag.value);
            record.tags.push_back(tag);
            ++i;
        }
        out.push_back(std::move(record));
    }
    return out;
}

// Entities in order, each POLYLINE with its VERTEX records and each INSERT without its ATTRIB records.
std::vector<Record> entities(std::vector<Record> list) {
    std::vector<Record> out;
    for (size_t i = 0; i < list.size(); ++i) {
        Record &record = list[i];
        if (record.type == "POLYLINE") {
            size_t j = i + 1;
            while (j < list.size() && list[j].type == "VERTEX") record.vertices.push_back(std::move(list[j++]));
            if (j < list.size() && list[j].type == "SEQEND") ++j;
            out.push_back(std::move(record));
            i = j - 1;
        } else if (record.type == "INSERT") {
            size_t j = i + 1;
            while (j < list.size() && list[j].type == "ATTRIB") ++j;
            if (j > i + 1 && j < list.size() && list[j].type == "SEQEND") ++j;
            out.push_back(std::move(record));
            i = j - 1;
        } else if (record.type != "SEQEND") {
            out.push_back(std::move(record));
        }
    }
    return out;
}

const std::string *first(const Record &record, int code) {
    for (const Tag &tag : record.tags)
        if (tag.code == code) return &tag.value;
    return nullptr;
}

double real(const Record &record, int code, double fallback) {
    const std::string *value = first(record, code);
    return value ? real_value(*value) : fallback;
}

long long whole(const Record &record, int code, long long fallback) {
    const std::string *value = first(record, code);
    return value ? integer_value(*value) : fallback;
}

std::vector<double> reals(const Record &record, int code) {
    std::vector<double> out;
    for (const Tag &tag : record.tags)
        if (tag.code == code) out.push_back(real_value(tag.value));
    return out;
}

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

Vec3 operator+(const Vec3 &a, const Vec3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3 &a, const Vec3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(double s, const Vec3 &a) { return {s * a.x, s * a.y, s * a.z}; }
double dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3 &a, const Vec3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
Vec3 unit(const Vec3 &a) {
    const double size = length(a);
    return size > 0 ? (1 / size) * a : a;
}

// Every point of a record given by the codes x, x + 10 and x + 20, in order.
std::vector<Vec3> points(const Record &record, int code) {
    std::vector<Vec3> out;
    for (const Tag &tag : record.tags) {
        if (tag.code == code) {
            out.push_back(Vec3{real_value(tag.value), 0, 0});
        } else if (!out.empty() && tag.code == code + 10) {
            out.back().y = real_value(tag.value);
        } else if (!out.empty() && tag.code == code + 20) {
            out.back().z = real_value(tag.value);
        }
    }
    return out;
}

Vec3 point(const Record &record, int code, const Vec3 &fallback) {
    const std::vector<Vec3> all = points(record, code);
    return all.empty() ? fallback : all.front();
}

// ---- coordinate systems ----------------------------------------------------------------------------------

// x' = m x + t, as a 3 x 4 matrix.
struct Affine {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double t[3] = {0, 0, 0};

    Vec3 apply(const Vec3 &p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + t[0], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + t[1],
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + t[2]};
    }
    Vec3 linear(const Vec3 &v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
};

// outer after inner.
Affine compose(const Affine &outer, const Affine &inner) {
    Affine out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out.m[r][c] = outer.m[r][0] * inner.m[0][c] + outer.m[r][1] * inner.m[1][c] + outer.m[r][2] * inner.m[2][c];
        out.t[r] = outer.m[r][0] * inner.t[0] + outer.m[r][1] * inner.t[1] + outer.m[r][2] * inner.t[2] + outer.t[r];
    }
    return out;
}

Affine scaling(double s) {
    Affine out;
    for (int i = 0; i < 3; ++i) out.m[i][i] = s;
    return out;
}

// The object coordinate system of an extrusion direction, by the DXF format's arbitrary axis algorithm.
Affine ocs(Vec3 normal) {
    normal = unit(normal);
    Affine out;
    if (length(normal) == 0) return out;
    const Vec3 ax = unit(std::fabs(normal.x) < 1.0 / 64 && std::fabs(normal.y) < 1.0 / 64 ? cross(Vec3{0, 1, 0}, normal) : cross(Vec3{0, 0, 1}, normal));
    const Vec3 ay = unit(cross(normal, ax));
    const Vec3 axes[3] = {ax, ay, normal};
    for (int c = 0; c < 3; ++c) {
        out.m[0][c] = axes[c].x;
        out.m[1][c] = axes[c].y;
        out.m[2][c] = axes[c].z;
    }
    return out;
}

Vec3 extrusion(const Record &record) { return point(record, 210, Vec3{0, 0, 1}); }

// ---- curves --------------------------------------------------------------------------------------------

// The angular step whose sagitta is the tolerance on a circle of this radius, as the G-code reader steps.
double step_for(double radius) {
    if (!(radius > 0)) return kPi / 8;
    const double ratio = kTolerance / (2 * radius);
    return std::min(kPi / 8, 4 * std::asin(std::sqrt(std::min(1.0, ratio))));
}

// The points of q + cos(t) a + sin(t) b for t from start through sweep, the first included.
void ellipse_points(const Vec3 &q, const Vec3 &a, const Vec3 &b, double start, double sweep, std::vector<Vec3> &out) {
    // The largest semi-axis of the ellipse is the largest singular value of [a b]: the image of a circle of
    // that radius, divided as finely, is within the tolerance of its chords.
    const double aa = dot(a, a), ab = dot(a, b), bb = dot(b, b);
    const double largest = std::sqrt((aa + bb + std::sqrt((aa - bb) * (aa - bb) + 4 * ab * ab)) / 2);
    const double count = std::ceil(std::fabs(sweep) / step_for(largest));
    if (!(count <= static_cast<double>(kMaxPoints))) throw ValueError("Simplify DXF curves to at most 100000 vertices");
    const long long n = std::max(1LL, static_cast<long long>(count));
    for (long long i = 0; i <= n; ++i) {
        const double turn = start + sweep * static_cast<double>(i) / static_cast<double>(n);
        out.push_back(q + (std::cos(turn) * a + std::sin(turn) * b));
    }
}

// The arc from p to its next vertex q of a polyline with this bulge, in the polyline's own plane, after p.
void bulge_points(const Vec3 &p, const Vec3 &q, double bulge, const Affine &frame, std::vector<Vec3> &out) {
    const double dx = q.x - p.x, dy = q.y - p.y, chord = std::sqrt(dx * dx + dy * dy);
    const double sweep = 4 * std::atan(bulge);
    const double radius = chord / (2 * std::fabs(std::sin(sweep / 2)));
    if (bulge == 0 || chord == 0 || !std::isfinite(radius)) {
        out.push_back(frame.apply(q));
        return;
    }
    const double offset = chord / 2 / std::tan(sweep / 2);  // from the chord's midpoint to the centre, leftwards
    const Vec3 centre{(p.x + q.x) / 2 - offset * dy / chord, (p.y + q.y) / 2 + offset * dx / chord, p.z};
    const double start = std::atan2(p.y - centre.y, p.x - centre.x);
    std::vector<Vec3> arc;
    ellipse_points(frame.apply(centre), frame.linear(Vec3{radius, 0, 0}), frame.linear(Vec3{0, radius, 0}), start, sweep, arc);
    for (size_t i = 1; i + 1 < arc.size(); ++i) out.push_back(arc[i]);
    out.push_back(frame.apply(q));
}

// (end - start) % full, the whole turn when it is nothing but the ends differ; nothing when they are equal.
double span(double start, double end, double full) {
    if (start == end) return 0;
    const double value = pytrig::modulo(end - start, full);
    return value == 0 ? full : value;
}

// A NURBS curve, its control points in homogeneous coordinates (x w, y w, z w, w).
using Homogeneous = std::array<double, 4>;

struct Spline {
    size_t degree = 3;
    std::vector<double> knots;
    std::vector<Homogeneous> control;

    // The blossom of the span starting at knot k (knots[k] < knots[k + 1]) at the given arguments, by de
    // Boor's recursion with one argument for each level. With every argument t it is the point at t; with
    // degree - j arguments a and j arguments b it is the span's j-th Bézier control point.
    Homogeneous blossom(size_t k, const std::vector<double> &arguments) const {
        std::vector<Homogeneous> d(control.begin() + static_cast<std::ptrdiff_t>(k - degree), control.begin() + static_cast<std::ptrdiff_t>(k + 1));
        for (size_t r = 1; r <= degree; ++r) {
            const double u = arguments[r - 1];
            for (size_t j = degree; j >= r; --j) {
                const size_t i = k - degree + j;
                const double denominator = knots[i + degree + 1 - r] - knots[i];
                const double alpha = denominator == 0 ? 0 : (u - knots[i]) / denominator;
                for (size_t c = 0; c < 4; ++c) d[j][c] = (1 - alpha) * d[j - 1][c] + alpha * d[j][c];
            }
        }
        return d[degree];
    }
};

Vec3 projected(const Homogeneous &h) { return {h[0] / h[3], h[1] / h[3], h[2] / h[3]}; }

double off_chord(const Vec3 &p, const Vec3 &a, const Vec3 &b) {
    const Vec3 ab = b - a, ap = p - a;
    const double square = dot(ab, ab);
    if (square == 0) return length(ap);
    const double s = std::max(0.0, std::min(1.0, dot(ap, ab) / square));
    return length(ap - s * ab);
}

// A rational Bézier piece flattened by halving it until its control points are within the tolerance of its
// chord: with positive weights the curve lies in their convex hull, so it is then within the tolerance too.
void bezier_points(const std::vector<Homogeneous> &piece, int depth, std::vector<Vec3> &out) {
    const Vec3 first = projected(piece.front()), last = projected(piece.back());
    double worst = 0;
    for (size_t i = 1; i + 1 < piece.size(); ++i) worst = std::max(worst, off_chord(projected(piece[i]), first, last));
    if (worst <= kTolerance || depth >= 24) {
        if (out.size() > 4 * kMaxPoints) throw ValueError("Simplify DXF curves to at most 100000 vertices");
        out.push_back(last);
        return;
    }
    // de Casteljau at one half, in homogeneous coordinates.
    std::vector<Homogeneous> left, right(piece.size()), level = piece;
    left.push_back(level.front());
    right[piece.size() - 1] = level.back();
    for (size_t r = 1; r < piece.size(); ++r) {
        for (size_t i = 0; i + r < piece.size(); ++i)
            for (size_t c = 0; c < 4; ++c) level[i][c] = (level[i][c] + level[i + 1][c]) / 2;
        left.push_back(level.front());
        right[piece.size() - 1 - r] = level[piece.size() - 1 - r];
    }
    bezier_points(left, depth + 1, out);
    bezier_points(right, depth + 1, out);
}

// The values of the degree + 1 B-spline basis functions that are not zero at u, and the span (the index of the
// first of them plus the degree): The NURBS Book's A2.1 and A2.2, with the last span at the end of the domain.
size_t basis_at(const std::vector<double> &knots, size_t degree, size_t count, double u, std::vector<double> &values) {
    size_t span = count - 1;
    if (u < knots[count]) span = static_cast<size_t>(std::upper_bound(knots.begin() + static_cast<std::ptrdiff_t>(degree),
                                                                      knots.begin() + static_cast<std::ptrdiff_t>(count), u) - knots.begin()) - 1;
    values.assign(degree + 1, 0.0);
    std::vector<double> left(degree + 1), right(degree + 1);
    values[0] = 1.0;
    for (size_t j = 1; j <= degree; ++j) {
        left[j] = u - knots[span + 1 - j];
        right[j] = knots[span + j] - u;
        double saved = 0.0;
        for (size_t r = 0; r < j; ++r) {
            const double temp = values[r] / (right[r + 1] + left[j - r]);
            values[r] = saved + right[r + 1] * temp;
            saved = left[j - r] * temp;
        }
        values[j] = saved;
    }
    return span;
}

// A x = b for a square matrix whose rows hold their few nonzero values near the diagonal: Gaussian elimination
// with partial pivoting among the rows that reach the column, three right-hand sides at once. False when the
// matrix is singular.
struct SparseRow {
    size_t first = 0;
    std::vector<double> values;
    double at(size_t column) const { return column >= first && column - first < values.size() ? values[column - first] : 0.0; }
};

bool solve_rows(std::vector<SparseRow> rows, std::vector<Vec3> rhs, std::vector<Vec3> &x) {
    const size_t n = rows.size();
    for (size_t k = 0; k < n; ++k) {
        size_t pivot = k;
        double best = std::fabs(rows[k].at(k));
        for (size_t r = k + 1; r < n && rows[r].first <= k; ++r)
            if (std::fabs(rows[r].at(k)) > best) {
                best = std::fabs(rows[r].at(k));
                pivot = r;
            }
        if (!(best > 0) || !std::isfinite(best)) return false;
        std::swap(rows[k], rows[pivot]);
        std::swap(rhs[k], rhs[pivot]);
        const SparseRow &top = rows[k];
        const double diagonal = top.at(k);
        for (size_t r = k + 1; r < n && rows[r].first <= k; ++r) {
            const double factor = rows[r].at(k) / diagonal;
            if (factor == 0) continue;
            SparseRow &row = rows[r];
            const size_t last = std::max(row.first + row.values.size(), top.first + top.values.size());
            std::vector<double> merged(last - k, 0.0);
            for (size_t c = k; c < last; ++c) merged[c - k] = row.at(c) - factor * top.at(c);
            merged[0] = 0.0;
            row.first = k;
            row.values = merged;
            rhs[r] = rhs[r] - factor * rhs[k];
        }
    }
    x.assign(n, Vec3{});
    for (size_t k = n; k-- > 0;) {
        Vec3 sum = rhs[k];
        const SparseRow &row = rows[k];
        for (size_t c = k + 1; c < row.first + row.values.size(); ++c) sum = sum - row.at(c) * x[c];
        x[k] = (1.0 / row.at(k)) * sum;
    }
    return true;
}

// The cubic B-spline through a SPLINE's fit points, as CAD programs make it and the reference (ezdxf) reads it:
// parameters by chord length, the interior parameters as the knots, and at the ends the given tangents scaled
// to the total chord length, or without them a curve that does not bend at its ends. Written from Piegl and
// Tiller, The NURBS Book, 9.2.1 and 9.2.2: the control points solve the interpolation and the two end
// conditions. In the entity's own coordinates.
void fit_point_spline(const Record &record, std::vector<double> &knots, std::vector<Vec3> &control) {
    const std::vector<Vec3> given = points(record, 11);
    std::vector<Vec3> fit;
    for (const Vec3 &p : given)
        if (fit.empty() || p.x != fit.back().x || p.y != fit.back().y || p.z != fit.back().z) fit.push_back(p);
    // The reference cannot interpolate through a repeated point (a division by zero, a singular system); the
    // core's own rule passes through it once.
    if (fit.size() != given.size()) own_rule("a DXF spline whose fit points repeat");
    if (fit.size() < 2) {
        own_rule("a DXF spline with fewer than two distinct fit points");
        malformed();
    }
    const size_t m = fit.size(), n = m - 1, count = m + 2, degree = 3;
    std::vector<double> distance(n);
    double total = 0;
    for (size_t k = 0; k < n; ++k) total += distance[k] = length(fit[k + 1] - fit[k]);
    if (!(total > 1e-12) || !std::isfinite(total)) malformed();
    std::vector<double> t = {0.0};
    double run = 0;
    for (size_t k = 0; k + 1 < n; ++k) t.push_back((run += distance[k]) / total);
    t.push_back(1.0);
    knots.assign(4, 0.0);
    for (size_t k = 1; k < n; ++k) knots.push_back(t[k]);
    knots.insert(knots.end(), 4, 1.0);
    // The tangents, when both ends have one of some length.
    const std::vector<Vec3> starts = points(record, 12), ends = points(record, 13);
    bool tangents = !starts.empty() && !ends.empty();
    if (tangents && (length(starts.front()) == 0 || length(ends.front()) == 0)) {
        own_rule("a DXF spline with an end tangent of no length");
        tangents = false;
    }
    // The rows: each fit point at its parameter, the start condition after the first and the end condition before
    // the last.
    std::vector<SparseRow> rows;
    std::vector<Vec3> rhs;
    std::vector<double> values;
    const double u1 = knots[4], u2 = knots[5], v1 = knots[knots.size() - 5], v2 = knots[knots.size() - 6];
    for (size_t k = 0; k < m; ++k) {
        const size_t span = basis_at(knots, degree, count, t[k], values);
        rows.push_back(SparseRow{span - degree, values});
        rhs.push_back(fit[k]);
        if (k == 0) {
            if (tangents) {  // C'(0) = the start tangent: P1 - P0 = C'(0) u_{p+1} / p
                rows.push_back(SparseRow{0, {-1.0, 1.0}});
                rhs.push_back((u1 / 3 / length(starts.front()) * total) * starts.front());
            } else {  // C''(0) = 0
                const double f = 6.0 / u1;
                rows.push_back(SparseRow{0, {f / u1, -f * (u1 + u2) / (u1 * u2), f / u2}});
                rhs.push_back(Vec3{});
            }
        }
        if (k + 2 == m) {
            if (tangents) {  // C'(1) = the end tangent
                rows.push_back(SparseRow{count - 2, {-1.0, 1.0}});
                rhs.push_back(((1.0 - v1) / 3 / length(ends.front()) * total) * ends.front());
            } else {  // C''(1) = 0
                const double f = 6.0 / (1.0 - v1);
                rows.push_back(SparseRow{count - 3, {f / (1.0 - v2), -f * (2.0 - v1 - v2) / (1.0 - v1) / (1.0 - v2), f / (1.0 - v1)}});
                rhs.push_back(Vec3{});
            }
        }
    }
    if (!solve_rows(rows, rhs, control)) malformed();
    // A clamped curve begins and ends at its end control points: exactly the first and last fit points, which the
    // elimination gives only to rounding.
    control.front() = fit.front();
    control.back() = fit.back();
}

void spline_points(const Record &record, const Affine &frame, std::vector<Vec3> &out) {
    long long degree = whole(record, 71, 3);
    std::vector<double> knots = reals(record, 40), weights = reals(record, 41);
    std::vector<Vec3> control;
    for (const Vec3 &p : points(record, 10)) control.push_back(frame.apply(p));
    if (control.empty()) {
        if (points(record, 11).empty()) return;
        // A curve through fit points is cubic whatever degree the entity gives.
        std::vector<Vec3> local;
        fit_point_spline(record, knots, local);
        for (const Vec3 &p : local) control.push_back(frame.apply(p));
        degree = 3;
        weights.clear();
    }
    const size_t n = control.size();
    if (degree < 1 || degree > 15 || n < static_cast<size_t>(degree) + 1) malformed();
    Spline spline;
    spline.degree = static_cast<size_t>(degree);
    const size_t order = spline.degree + 1;
    if (knots.empty()) {
        // A clamped, uniform knot vector.
        for (size_t i = 0; i < n + order; ++i) knots.push_back(i < order ? 0.0 : i >= n ? static_cast<double>(n - order + 1) : static_cast<double>(i - order + 1));
    }
    if (knots.size() != n + order) malformed();
    for (size_t i = 1; i < knots.size(); ++i)
        if (!(knots[i] >= knots[i - 1])) malformed();
    if (!weights.empty() && weights.size() != n) malformed();
    for (size_t i = 0; i < n; ++i) {
        const double w = weights.empty() ? 1.0 : weights[i];
        if (!(w > 0) || !std::isfinite(w)) malformed();
        spline.control.push_back({control[i].x * w, control[i].y * w, control[i].z * w, w});
    }
    spline.knots = knots;
    // The curve's own domain, from knot degree to knot n, span by span: each span is a Bézier piece.
    bool started = false;
    for (size_t k = spline.degree; k < n; ++k) {
        const double a = knots[k], b = knots[k + 1];
        if (!(b > a)) continue;
        std::vector<Homogeneous> piece;
        for (size_t j = 0; j <= spline.degree; ++j) {
            std::vector<double> arguments(spline.degree, a);
            for (size_t m = 0; m < j; ++m) arguments[spline.degree - 1 - m] = b;
            piece.push_back(spline.blossom(k, arguments));
        }
        if (!started) {
            out.push_back(projected(piece.front()));
            started = true;
        }
        bezier_points(piece, 0, out);
    }
}

// The points of one drawing entity in millimetres; false when it is not one the reader draws.
std::vector<Vec3> entity_points(const Record &record, const Affine &frame) {
    std::vector<Vec3> out;
    const std::string &kind = record.type;
    if (kind == "LINE") {
        out.push_back(frame.apply(point(record, 10, Vec3{})));
        out.push_back(frame.apply(point(record, 11, Vec3{})));
    } else if (kind == "CIRCLE" || kind == "ARC") {
        const Affine local = compose(frame, ocs(extrusion(record)));
        const Vec3 centre = point(record, 10, Vec3{});
        const double radius = real(record, 40, 1.0);
        // A whole turn is drawn clockwise from its start, as the reference draws circles; a part of one,
        // counter-clockwise.
        double start = 0, sweep = -kTau;
        if (kind == "ARC") {
            const double a0 = real(record, 50, 0.0), a1 = real(record, 51, 360.0);
            const double turn = span(a0, a1, 360);
            if (turn == 0) return out;
            start = a0 * (kPi / 180);
            sweep = turn == 360 ? -kTau : turn * (kPi / 180);
        }
        ellipse_points(local.apply(centre), local.linear(Vec3{radius, 0, 0}), local.linear(Vec3{0, radius, 0}), start, sweep, out);
        if (sweep == -kTau) out.back() = out.front();  // a closed contour
    } else if (kind == "ELLIPSE") {
        const Vec3 centre = point(record, 10, Vec3{}), major = point(record, 11, Vec3{1, 0, 0});
        const double ratio = real(record, 40, 1.0);
        const Vec3 minor = (length(major) * ratio) * unit(cross(unit(extrusion(record)), major));
        const double s = real(record, 41, 0.0), e = real(record, 42, kTau);
        const double turn = span(s, e, kTau);
        if (turn == 0) return out;
        ellipse_points(frame.apply(centre), frame.linear(major), frame.linear(minor), s, turn == kTau ? -kTau : turn, out);
        if (turn == kTau) out.back() = out.front();
    } else if (kind == "LWPOLYLINE") {
        const Affine local = compose(frame, ocs(extrusion(record)));
        const double elevation = real(record, 38, 0.0);
        std::vector<Vec3> vertices;
        std::vector<double> bulges;
        for (const Tag &tag : record.tags) {
            if (tag.code == 10) {
                vertices.push_back(Vec3{real_value(tag.value), 0, elevation});
                bulges.push_back(0);
            } else if (!vertices.empty() && tag.code == 20) {
                vertices.back().y = real_value(tag.value);
            } else if (!vertices.empty() && tag.code == 42) {
                bulges.back() = real_value(tag.value);
            }
        }
        if (vertices.empty()) return out;
        const bool closed = whole(record, 70, 0) & 1;
        out.push_back(local.apply(vertices[0]));
        const size_t segments = closed ? vertices.size() : vertices.size() - 1;
        for (size_t i = 0; i < segments; ++i) bulge_points(vertices[i], vertices[(i + 1) % vertices.size()], bulges[i], local, out);
    } else if (kind == "POLYLINE") {
        const long long flags = whole(record, 70, 0);
        const bool closed = flags & 1;
        std::vector<Vec3> vertices;
        std::vector<double> bulges;
        if (flags & 8) {
            for (const Record &vertex : record.vertices) vertices.push_back(frame.apply(point(vertex, 10, Vec3{})));
            if (vertices.empty()) return out;
            out = vertices;
            if (closed) out.push_back(vertices.front());
            return out;
        }
        const Affine local = compose(frame, ocs(extrusion(record)));
        const double elevation = point(record, 10, Vec3{}).z;
        for (const Record &vertex : record.vertices) {
            const Vec3 p = point(vertex, 10, Vec3{});
            vertices.push_back(Vec3{p.x, p.y, elevation});
            bulges.push_back(real(vertex, 42, 0.0));
        }
        if (vertices.empty()) return out;
        out.push_back(local.apply(vertices[0]));
        const size_t segments = closed ? vertices.size() : vertices.size() - 1;
        for (size_t i = 0; i < segments; ++i) bulge_points(vertices[i], vertices[(i + 1) % vertices.size()], bulges[i], local, out);
    } else if (kind == "SPLINE") {
        spline_points(record, frame, out);
    }
    return out;
}

// ---- XCLIP windows ---------------------------------------------------------------------------------------

using Point = std::array<double, 2>;
using Ring = std::vector<Point>;

struct Document {
    long long units = 0;
    std::vector<Record> model;
    std::map<std::string, std::vector<Record>> blocks;  // by folded name
    std::map<std::string, Vec3> bases;
    std::map<std::string, Record> objects;                // by handle
    std::vector<Record> layers;
};

std::string folded(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// A dictionary object's entry by name.
const Record *entry(const Document &doc, const std::string &handle, const std::string &name) {
    const auto found = doc.objects.find(handle);
    if (found == doc.objects.end()) return nullptr;
    const std::vector<Tag> &tags = found->second.tags;
    for (size_t i = 0; i + 1 < tags.size(); ++i) {
        if (tags[i].code == 3 && tags[i].value == name && (tags[i + 1].code == 350 || tags[i + 1].code == 360)) {
            const auto target = doc.objects.find(trimmed(tags[i + 1].value));
            return target == doc.objects.end() ? nullptr : &target->second;
        }
    }
    return nullptr;
}

const Record *spatial_filter(const Document &doc, const Record &insert) {
    if (insert.xdict.empty()) return nullptr;
    const Record *filters = entry(doc, insert.xdict, "ACAD_FILTER");
    if (!filters || filters->handle.empty()) return nullptr;
    const Record *filter = entry(doc, filters->handle, "SPATIAL");
    return filter && filter->type == "SPATIAL_FILTER" ? filter : nullptr;
}

bool inverted(const Document &doc, const Record &filter) {
    if (filter.xdict.empty()) return false;
    const Record *record = entry(doc, filter.xdict, "ACAD_XREC_ROUNDTRIP");
    if (!record || record->type != "XRECORD") return false;
    for (const Tag &tag : record->tags)
        if (tag.code == 102 && tag.value == "ACAD_INVERTEDCLIP_ROUNDTRIP") return true;
    return false;
}

// The 3 x 4 matrix of twelve group 40 values, row by row, as the DXF format stores a spatial filter's.
Affine stored_matrix(const std::vector<double> &values, size_t from) {
    Affine out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out.m[r][c] = values[from + static_cast<size_t>(4 * r + c)];
        out.t[r] = values[from + static_cast<size_t>(4 * r + 3)];
    }
    return out;
}

bool identity(const Affine &a) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            if (std::fabs(a.m[r][c] - (r == c ? 1.0 : 0.0)) > 1e-12) return false;
        if (std::fabs(a.t[r]) > 1e-12) return false;
    }
    return true;
}

double ring_area(const Ring &ring) {
    double twice = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const Point &a = ring[i], &b = ring[(i + 1) % ring.size()];
        twice += a[0] * b[1] - b[0] * a[1];
    }
    return std::fabs(twice) / 2;
}

// _dxf_clip_polygon: the clipping boundary in the drawing's millimetres, or a ValueError naming why not.
Ring clip_polygon(const Document &doc, const Record &filter, const Affine &frame) {
    const std::vector<double> values = reals(filter, 40);
    const Vec3 normal = point(filter, 210, Vec3{0, 0, 1}), origin = point(filter, 11, Vec3{});
    const Affine inverse = stored_matrix(values, values.size() - 24), transform = stored_matrix(values, values.size() - 12);
    if (inverted(doc, filter) || whole(filter, 72, 0) || whole(filter, 73, 0) || std::fabs(normal.x) > 1e-12 || std::fabs(normal.y) > 1e-12 ||
        std::fabs(normal.z - 1) > 1e-12 || origin.x != 0 || origin.y != 0 || !identity(transform))
        throw ValueError("unsupported inverted, 3D or custom-plane XCLIP");
    const Affine matrix = compose(frame, inverse);
    const Vec3 z = matrix.linear(Vec3{0, 0, 1});
    const double size = length(z);
    if (!(size > 0) || std::hypot(z.x, z.y) > 1e-10 * size) throw ValueError("unsupported tilted XCLIP");
    std::vector<Vec3> vertices;
    for (const Vec3 &p : points(filter, 10)) vertices.push_back(Vec3{p.x, p.y, 0});
    if (vertices.size() == 2) {
        const double x0 = std::min(vertices[0].x, vertices[1].x), x1 = std::max(vertices[0].x, vertices[1].x);
        const double y0 = std::min(vertices[0].y, vertices[1].y), y1 = std::max(vertices[0].y, vertices[1].y);
        vertices = {Vec3{x0, y0, 0}, Vec3{x1, y0, 0}, Vec3{x1, y1, 0}, Vec3{x0, y1, 0}};
    }
    if (vertices.size() < 3 || vertices.size() > 256) throw ValueError("XCLIP needs 3\xe2\x80\x93" "256 polygon vertices");
    Ring ring;
    for (const Vec3 &v : vertices) {
        const Vec3 p = matrix.apply(v);
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw ValueError("nonfinite XCLIP boundary");
        ring.push_back({p.x, p.y});
    }
    // Shapely's Polygon(ring).is_valid: at least three distinct corners, and the ring does not cross or
    // touch itself.
    Ring distinct;
    for (const Point &p : ring)
        if (distinct.empty() || distinct.back() != p) distinct.push_back(p);
    while (distinct.size() > 1 && distinct.back() == distinct.front()) distinct.pop_back();
    std::vector<std::array<double, 2>> closed(distinct.begin(), distinct.end());
    if (!closed.empty()) closed.push_back(closed.front());
    if (distinct.size() < 3) throw ValueError("invalid XCLIP boundary");
    const LineCheck check = check_line(closed);
    if (check.declined) throw NotPorted("an XCLIP boundary whose validity turns on rounding");
    if (!check.simple || !(ring_area(ring) > 0)) throw ValueError("invalid XCLIP boundary");
    return ring;
}

// A window: the region inside every boundary, as rings read by even-odd.
struct Window {
    std::vector<Ring> rings;
    bool empty() const { return rings.empty(); }
};

json rings_json(const std::vector<Ring> &rings) {
    json out = json::array();
    for (const Ring &ring : rings) {
        json list = json::array();
        for (const Point &p : ring) list.push_back(json::array({p[0], p[1]}));
        out.push_back(std::move(list));
    }
    return out;
}

Window intersect(const Window &mask, const Ring &local) {
    json request = json::object();
    request["op"] = "intersection";
    request["a"] = rings_json(mask.rings);
    request["b"] = rings_json({local});
    json answer;
    try {
        answer = polygon_call("polygon", request);
    } catch (const ValueError &) {
        own_rule("nested XCLIP windows beyond the polygon kernel's range");
        throw ValueError("Nested XCLIP windows lie beyond the range Studio can clip; move the drawing nearer its origin");
    }
    Window out;
    if (!(answer.at("area").get<double>() > 0)) return out;
    for (const json &polygon : answer.at("polygons")) {
        std::vector<json> parts = {polygon.at("shell")};
        for (const json &hole : polygon.at("holes")) parts.push_back(hole);
        for (const json &part : parts) {
            Ring ring;
            for (const json &p : part) ring.push_back({p[0].get<double>(), p[1].get<double>()});
            out.rings.push_back(std::move(ring));
        }
    }
    return out;
}

double cross2(const Point &o, const Point &a, const Point &b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); }

bool on_edge(const Point &p, const Point &a, const Point &b) {
    const double dx = b[0] - a[0], dy = b[1] - a[1], size = std::hypot(dx, dy);
    const double scale = std::max({1.0, std::fabs(a[0]), std::fabs(a[1]), std::fabs(b[0]), std::fabs(b[1])});
    if (std::fabs(cross2(a, b, p)) > 1e-12 * scale * std::max(size, 1e-300) + 1e-300) return false;
    return std::min(a[0], b[0]) - 1e-12 * scale <= p[0] && p[0] <= std::max(a[0], b[0]) + 1e-12 * scale &&
           std::min(a[1], b[1]) - 1e-12 * scale <= p[1] && p[1] <= std::max(a[1], b[1]) + 1e-12 * scale;
}

// Inside the window or on its boundary.
bool covered(const Window &window, const Point &p) {
    bool inside = false;
    for (const Ring &ring : window.rings) {
        for (size_t i = 0; i < ring.size(); ++i) {
            const Point &a = ring[i], &b = ring[(i + 1) % ring.size()];
            if (on_edge(p, a, b)) return true;
            if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < a[0] + (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1])) inside = !inside;
        }
    }
    return inside;
}

// The visible intervals of the segment from s to e, each as its ends, in order from s. A point where the
// window cuts the segment is computed from the segment's ends in a fixed order, so that a contour that
// retraces a segment is cut at the same point both ways and its runs join as the reference's do.
std::vector<std::pair<Point, Point>> visible(const Window &window, const Point &s, const Point &e) {
    const bool reversed = e < s;
    const Point &c0 = reversed ? e : s, &c1 = reversed ? s : e;
    const double dx = c1[0] - c0[0], dy = c1[1] - c0[1];
    std::vector<double> cuts = {0.0, 1.0};  // along c0 to c1
    for (const Ring &ring : window.rings) {
        for (size_t i = 0; i < ring.size(); ++i) {
            const Point &a = ring[i], &b = ring[(i + 1) % ring.size()];
            const double ex = b[0] - a[0], ey = b[1] - a[1];
            const double denominator = dx * ey - dy * ex;
            if (denominator != 0) {
                const double t = ((a[0] - c0[0]) * ey - (a[1] - c0[1]) * ex) / denominator;
                const double u = ((a[0] - c0[0]) * dy - (a[1] - c0[1]) * dx) / denominator;
                if (t > 0 && t < 1 && u >= -1e-12 && u <= 1 + 1e-12) cuts.push_back(t);
            } else {
                // Parallel: where the edge's ends fall along the segment.
                const double square = dx * dx + dy * dy;
                if (square == 0) continue;
                for (const Point &p : {a, b}) {
                    const double t = ((p[0] - c0[0]) * dx + (p[1] - c0[1]) * dy) / square;
                    if (t > 0 && t < 1) cuts.push_back(t);
                }
            }
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    const auto at = [&](double t) -> Point { return t == 0 ? c0 : t == 1 ? c1 : Point{c0[0] + dx * t, c0[1] + dy * t}; };
    std::vector<std::pair<Point, Point>> out;
    double open = -1;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const bool in = covered(window, at((cuts[i] + cuts[i + 1]) / 2));
        if (in && open < 0) open = cuts[i];
        if (!in && open >= 0) {
            out.push_back({at(open), at(cuts[i])});
            open = -1;
        }
    }
    if (open >= 0) out.push_back({at(open), c1});
    if (reversed) {
        std::reverse(out.begin(), out.end());
        for (auto &piece : out) std::swap(piece.first, piece.second);
    }
    return out;
}

// _dxf_clipped_paths: the runs of a contour inside the window, in drawing order, retraces and gaps kept.
std::vector<std::vector<Point>> clipped(const std::vector<Point> &path, const Window *mask) {
    if (path.size() < 2) return {};
    if (!mask) return {path};
    std::vector<std::vector<Point>> parts;
    size_t output = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        for (const auto &piece : visible(*mask, path[i], path[i + 1])) {
            if (piece.first == piece.second) continue;
            if (!parts.empty() && parts.back().back() == piece.first) {
                parts.back().push_back(piece.second);
                output += 1;
            } else {
                parts.push_back({piece.first, piece.second});
                output += 2;
            }
            if (parts.size() > kMaxPaths || output > kMaxPoints) throw ValueError("Simplify the clipped DXF to 100000 vertices and 3000 paths");
        }
    }
    if (parts.size() > 1 && path.front() == path.back() && parts.back().back() == parts.front().front()) {
        std::vector<Point> joined = parts.back();
        joined.insert(joined.end(), parts.front().begin() + 1, parts.front().end());
        parts.pop_back();
        parts.front() = std::move(joined);
    }
    return parts;
}

// ---- reading the file ------------------------------------------------------------------------------------

Document read_document(const std::string &text) {
    const std::vector<Tag> tags = read_tags(text);
    Document doc;
    size_t i = 0;
    while (i < tags.size()) {
        if (tags[i].code == 0 && trimmed(tags[i].value) == "EOF") break;
        if (tags[i].code != 0 || trimmed(tags[i].value) != "SECTION" || i + 1 >= tags.size() || tags[i + 1].code != 2) unreadable();
        const std::string name = trimmed(tags[i + 1].value);
        size_t end = i + 2;
        while (end < tags.size() && !(tags[end].code == 0 && trimmed(tags[end].value) == "ENDSEC")) ++end;
        const size_t body = i + 2;
        if (name == "HEADER") {
            for (size_t k = body; k + 1 < end; ++k)
                if (tags[k].code == 9 && trimmed(tags[k].value) == "$INSUNITS" && tags[k + 1].code == 70) doc.units = integer_value(tags[k + 1].value);
        } else if (name == "TABLES") {
            for (const Record &record : records(tags, body, end))
                if (record.type == "LAYER") doc.layers.push_back(record);
        } else if (name == "BLOCKS") {
            std::vector<Record> list = records(tags, body, end);
            for (size_t k = 0; k < list.size(); ++k) {
                if (list[k].type != "BLOCK") continue;
                const std::string *named = first(list[k], 2);
                if (!named) named = first(list[k], 3);
                const std::string key = folded(named ? *named : std::string());
                const Vec3 base = point(list[k], 10, Vec3{});
                std::vector<Record> body_records;
                size_t j = k + 1;
                while (j < list.size() && list[j].type != "ENDBLK") body_records.push_back(std::move(list[j++]));
                if (!doc.blocks.count(key)) {
                    doc.blocks[key] = entities(std::move(body_records));
                    doc.bases[key] = base;
                }
                k = j;
            }
        } else if (name == "ENTITIES") {
            for (Record &record : entities(records(tags, body, end)))
                if (whole(record, 67, 0) != 1) doc.model.push_back(std::move(record));
        } else if (name == "OBJECTS") {
            for (Record &record : records(tags, body, end)) {
                if (record.type == "SPATIAL_FILTER" && reals(record, 40).size() < 24) unreadable();
                if (!record.handle.empty() && !doc.objects.count(record.handle)) {
                    const std::string handle = record.handle;
                    doc.objects[handle] = std::move(record);
                }
            }
        }
        if (end >= tags.size()) break;
        i = end + 1;
    }
    return doc;
}

// ezdxf's conversion factors where they are exact, and the defined values of the others.
bool unit_scale(long long units, double &scale) {
    static const std::map<long long, double> factors = {
        {1, 25.4},      {2, 304.8},     {3, 1609344.0}, {4, 1.0},    {5, 10.0},   {6, 1000.0},          {7, 1e6},
        {8, 2.54e-5},   {9, 0.0254},    {10, 914.4},    {11, 1e-7},  {12, 1e-6},  {13, 1e-3},           {14, 100.0},
        {15, 1e4},      {16, 1e5},      {17, 1e12},     {18, 1.495978707e14},     {19, 9.4607304725808e18},
        {20, 3.0856775814913673e19},    {21, 1200000.0 / 3937},  {22, 100000.0 / 3937}, {23, 3600000.0 / 3937},
        {24, 6336000000.0 / 3937}};
    const auto found = factors.find(units);
    if (found == factors.end()) return false;
    scale = found->second;
    return true;
}

std::string title(const std::string &kind) {
    std::string out = kind;
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    return out;
}

class Reader {
public:
    Reader(const Document &doc, Drawing &drawing) : doc_(doc), drawing_(drawing) {}

    std::map<std::string, size_t> skipped;
    size_t clipped_instances = 0;

    void read(const Record &record, const Affine &transform, const std::string &parent_layer, const std::vector<std::string> &ancestry,
              const Window *mask) {
        if (++visited_ > kMaxEntities) throw ValueError("Flattened DXF exceeds 20000 entities");
        const std::string *given = first(record, 8);
        std::string layer = given ? *given : std::string("0");
        if (layer == "0") layer = parent_layer;
        const std::string &kind = record.type;
        if (kind == "INSERT") {
            insert(record, transform, layer, ancestry, mask);
            return;
        }
        static const std::set<std::string> drawn = {"LINE", "LWPOLYLINE", "POLYLINE", "CIRCLE", "ARC", "ELLIPSE", "SPLINE"};
        if (!drawn.count(kind)) {
            ++skipped[kind];
            return;
        }
        if (kind == "POLYLINE" && (whole(record, 70, 0) & (16 | 64))) {
            ++skipped["MESH"];
            return;
        }
        std::vector<Point> path;
        bool have_z = false;
        double z0 = 0;
        for (const Vec3 &p : entity_points(record, transform)) {
            if (!have_z) {
                z0 = p.z;
                have_z = true;
            }
            if (!std::isfinite(p.z) || std::fabs(p.z - z0) > 1e-6) {
                ++skipped["non-XY contour"];
                return;
            }
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw ValueError("DXF contains a nonfinite XY coordinate");
            const Point xy = {p.x, p.y};
            if (path.empty() || path.back() != xy) path.push_back(xy);
            if (path.size() + source_points_ > kMaxPoints) throw ValueError("Simplify DXF curves to at most 100000 vertices");
        }
        source_points_ += path.size();
        for (const std::vector<Point> &run : clipped(path, mask)) drawing_.path(layer + " \xc2\xb7 " + title(kind), layer, run);
        colour(layer);
    }

private:
    const Document &doc_;
    Drawing &drawing_;
    size_t visited_ = 0, source_points_ = 0;

    void insert(const Record &record, const Affine &transform, const std::string &layer, const std::vector<std::string> &ancestry,
                const Window *mask) {
        const std::string *named = first(record, 2);
        const std::string block = folded(named ? *named : std::string());
        if (std::find(ancestry.begin(), ancestry.end(), block) != ancestry.end() || ancestry.size() >= 12)
            throw ValueError("Recursive or excessively nested DXF block");
        long long rows = whole(record, 71, 1), columns = whole(record, 70, 1);
        if (rows < 1 || columns < 1) rows = columns = 1;
        if (rows > 256 || columns > 256 || rows * columns > 256) throw ValueError("DXF block array exceeds 256 instances");
        const auto layout = doc_.blocks.find(block);
        if (layout == doc_.blocks.end()) {
            ++skipped["missing block definition"];
            return;
        }
        const Record *filter = spatial_filter(doc_, record);
        const bool clipping = filter && whole(*filter, 71, 1) != 0;
        const Vec3 base = doc_.bases.at(block), at = point(record, 10, Vec3{});
        const double sx = real(record, 41, 1.0), sy = real(record, 42, 1.0), sz = real(record, 43, 1.0);
        const double turn = real(record, 50, 0.0) * (kPi / 180);
        const double c = std::cos(turn), s = std::sin(turn);
        const double row_spacing = real(record, 45, 0.0), column_spacing = real(record, 44, 0.0);
        const Affine plane = ocs(extrusion(record));
        std::vector<std::string> inner = ancestry;
        inner.push_back(block);
        for (long long row = 0; row < rows; ++row) {
            for (long long column = 0; column < columns; ++column) {
                const double ox = static_cast<double>(column) * column_spacing, oy = static_cast<double>(row) * row_spacing;
                const Vec3 insert_at{at.x + c * ox - s * oy, at.y + s * ox + c * oy, at.z};
                // p -> OCS(R S (p - base) + insert)
                Affine local;
                local.m[0][0] = c * sx;
                local.m[0][1] = -s * sy;
                local.m[1][0] = s * sx;
                local.m[1][1] = c * sy;
                local.m[2][2] = sz;
                const Vec3 moved = local.linear(Vec3{} - base);
                local.t[0] = moved.x + insert_at.x;
                local.t[1] = moved.y + insert_at.y;
                local.t[2] = moved.z + insert_at.z;
                const Affine frame = compose(transform, compose(plane, local));
                Window window;
                const Window *use = mask;
                if (clipping) {
                    Ring boundary;
                    try {
                        boundary = clip_polygon(doc_, *filter, frame);
                    } catch (const ValueError &error) {
                        ++skipped[std::string("block with ") + error.what() + "; export its visible outlines"];
                        continue;
                    }
                    ++clipped_instances;
                    window = mask ? intersect(*mask, boundary) : Window{{boundary}};
                    if (window.empty()) continue;
                    use = &window;
                }
                for (const Record &child : layout->second) read(child, frame, layer, inner, use);
            }
        }
    }

    // The layer table's colour and visibility for a layer that has outlines.
    void colour(const std::string &layer) {
        if (!drawing_.has_layer(layer)) return;
        const std::string key = folded(layer);
        for (const Record &entry : doc_.layers) {
            const std::string *named = first(entry, 2);
            if (!named || folded(*named) != key) continue;
            const long long number = whole(entry, 62, 7), flags = whole(entry, 70, 0);
            const std::string *true_colour = first(entry, 420);
            long long rgb = -1;
            if (true_colour) {
                rgb = integer_value(*true_colour) & 0xFFFFFF;
            } else if (std::llabs(number) >= 1 && std::llabs(number) <= 255) {
                rgb = kAciColors[std::llabs(number)];
            }
            json &extra = drawing_.layer_extras[layer];
            if (!extra.is_object()) extra = json::object();
            if (rgb >= 0) {
                static const char *digits = "0123456789abcdef";
                std::string text = "#";
                for (int shift = 20; shift >= 0; shift -= 4) text.push_back(digits[(rgb >> shift) & 15]);
                extra["color"] = text;
            }
            extra["visible"] = !(number < 0 || (flags & 1));
            return;
        }
    }
};

}  // namespace

Json dxf_drawing(const std::string &text, double default_scale) {
    const Document doc = read_document(text);
    Drawing drawing;
    double scale = default_scale;
    if (doc.units == 0) {
        drawing.warnings.push_back("Unitless DXF interpreted at " + general(scale) + " mm per drawing unit.");
    } else if (!unit_scale(doc.units, scale)) {
        throw ValueError("Unrecognised DXF drawing units");
    }
    Reader reader(doc, drawing);
    const Affine units = scaling(scale);
    for (const Record &record : doc.model) reader.read(record, units, "0", {}, nullptr);
    if (reader.clipped_instances)
        drawing.warnings.push_back("Applied XCLIP to " + std::to_string(reader.clipped_instances) +
                                   " block instance(s). Only visible contour segments are retained.");
    if (!reader.skipped.empty()) {
        std::string line = "Skipped DXF entities: ";
        bool more = false;
        for (const auto &item : reader.skipped) {
            line += (more ? ", " : "") + item.first + " (" + std::to_string(item.second) + ")";
            more = true;
        }
        drawing.warnings.push_back(line);
    }
    drawing.warnings.push_back("Imported model-space outlines in XY. Elevation, widths, paper layouts and plot styles are omitted.");
    return drawing.finish();
}

}  // namespace kerfkite
