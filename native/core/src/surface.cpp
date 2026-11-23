// Editable surfaces: studio/cad_exchange.py's surface_mesh in C++. Rational Bézier patches and clamped
// B-spline patches with open uniform knots are checked with the reference's messages, joined sheets have their
// common edges resolved in homogeneous coordinates as the reference resolves them (to the bit), and the surface
// is evaluated on the reference's 25 by 25 grid of parameters (to rounding: NumPy's einsum sums in its own order).
//
// A drawn outline (UV loops) is cut as the reference cuts it, in other arithmetic. Shapely clips each of the grid's
// cells to the outline and triangulates it by constrained Delaunay triangulation; the core cuts the outline with
// the grid lines in its exact planar arrangement (kerfkite/solid.hpp's region_triangles), on a grid of 1/4096 mm
// to the UV unit rounded to the nanometre, then flips each cell's diagonals until they are Delaunay, splitting a
// cell whose corners share a circle from its lowest corner, and merges points within 64 steps of that grid, where
// the reference meets a grid line at a corner of the outline that the rounded grid passes beside.
// validation/surface_mesh_check.py compares the two: the same cells cut at the same points, and the same
// triangles but for the diagonals of whole cells, which Shapely takes either way as rounding decides. Outlines
// whose loops touch one another, where GEOS's validity rules need more than the cases the core decides, are
// declined.
#include "surface.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "kerfkite/line_check.hpp"
#include "kerfkite/solid.hpp"
#include "project.hpp"

namespace kerfkite {
namespace surface {
namespace {

using json = Json;
using detail::get;
using Point3 = std::array<double, 3>;
using UV = std::array<double, 2>;
using solid::Ring;
using solid::RegionTriangles;
using solid::Vec2;
using solid::region_triangles;

const int kGrid = 24;            // cells a side, as _patch_mesh and _trimmed_mesh take them
const size_t kMaxPatches = 16;
const double kScale = 4096.0;    // millimetres to the UV unit in the planar arrangement: exact, and within range

// One patch: poles and weights row by row (u along the rows), its kind and degrees.
struct Spec {
    int size[2] = {0, 0};
    std::vector<Point3> poles;
    std::vector<double> weights;
    bool bezier = true;
    int degree[2] = {0, 0};
    Point3 &pole(int i, int j) { return poles[static_cast<size_t>(i * size[1] + j)]; }
    double &weight(int i, int j) { return weights[static_cast<size_t>(i * size[1] + j)]; }
    Point3 pole(int i, int j) const { return poles[static_cast<size_t>(i * size[1] + j)]; }
    double weight(int i, int j) const { return weights[static_cast<size_t>(i * size[1] + j)]; }
};

bool number(const json *value) { return value && value->is_number(); }
double value_of(const json &value) { return value.get<double>(); }

// _poles and _surface_spec.
Spec surface_spec(const json &surface, const json *order) {
    if (!surface.is_object()) throw ValueError("Supply a B\xc3\xa9zier control net");
    const json *grid = get(surface, "poles");
    if (!grid || !grid->is_array() || grid->size() < 2 || grid->size() > 8)
        throw ValueError("A B\xc3\xa9zier patch needs 2\xe2\x80\x93" "8 rows of control points");
    const json &first = grid->at(0);
    if (!first.is_array() || first.size() < 2 || first.size() > 8)
        throw ValueError("A B\xc3\xa9zier patch needs 2\xe2\x80\x93" "8 columns of control points");
    const json empty = json::object();
    const json *definitions = surface.contains("parameters") ? get(surface, "parameters") : &empty;
    std::unique_ptr<Parameters> dimensions = parameters(definitions, order);
    Spec spec;
    spec.size[0] = static_cast<int>(grid->size());
    spec.size[1] = static_cast<int>(first.size());
    for (const json &row : *grid) {
        if (!row.is_array() || row.size() != first.size()) throw ValueError("Use a rectangular control net");
        for (const json &p : row) spec.poles.push_back(dimensions->point(&p));
    }
    const json *kind = get(surface, "kind");
    if (kind && !(kind->is_string() && (*kind == json("bezier") || *kind == json("bspline"))))
        throw ValueError("Select a B\xc3\xa9zier or clamped B-spline surface");
    spec.bezier = !kind || *kind == json("bezier");
    spec.degree[0] = spec.size[0] - 1;
    spec.degree[1] = spec.size[1] - 1;
    if (!spec.bezier) {
        spec.degree[0] = std::min(3, spec.size[0] - 1);
        spec.degree[1] = std::min(3, spec.size[1] - 1);
        if (surface.contains("degrees")) {
            const json *degrees = get(surface, "degrees");
            bool good = degrees->is_array() && degrees->size() == 2;
            for (size_t a = 0; good && a < 2; ++a) {
                const json &d = degrees->at(a);
                // type(d) is int, of any size and either sign: JSON text reads a whole number of no sign as unsigned.
                good = d.is_number_integer() && d.get<double>() >= 1 && d.get<double>() < spec.size[a];
                if (good) spec.degree[a] = static_cast<int>(d.get<double>());
            }
            if (!good) throw ValueError("Each B-spline degree must be at least one and less than its pole count");
        }
    }
    const json *weights = get(surface, "weights");
    if (!weights || weights->is_null()) {
        spec.weights.assign(spec.poles.size(), 1.0);
    } else {
        bool good = weights->is_array() && weights->size() == static_cast<size_t>(spec.size[0]);
        for (size_t i = 0; good && i < weights->size(); ++i) {
            const json &row = weights->at(i);
            good = row.is_array() && row.size() == static_cast<size_t>(spec.size[1]);
            for (size_t j = 0; good && j < row.size(); ++j) {
                const json &w = row.at(j);
                good = w.is_number() && value_of(w) >= 0.001 && value_of(w) <= 1000;
            }
        }
        if (!good) throw ValueError("Use a matching rectangular net of weights from 0.001 to 1000");
        for (const json &row : *weights)
            for (const json &w : row) spec.weights.push_back(value_of(w));
    }
    return spec;
}

// _trim: u0, u1, v0, v1.
std::array<double, 4> trim_of(const json &surface) {
    std::array<double, 4> bounds = {0.0, 1.0, 0.0, 1.0};
    if (surface.contains("trim")) {
        const json *given = get(surface, "trim");
        if (!given->is_array() || given->size() != 4) throw ValueError("Enter finite U and V trim bounds");
        for (size_t k = 0; k < 4; ++k) {
            if (!number(&given->at(k))) throw ValueError("Enter finite U and V trim bounds");
            bounds[k] = value_of(given->at(k));
        }
    }
    for (size_t axis = 0; axis < 2; ++axis)
        if (!(0 <= bounds[2 * axis] && bounds[2 * axis] < bounds[2 * axis + 1] && bounds[2 * axis + 1] <= 1))
            throw ValueError("Trim bounds must increase between zero and one on each axis");
    return bounds;
}

struct Visible {
    size_t patch;                 // the patch's place in the sheet
    std::array<double, 4> window;
};

struct Sheet {
    std::vector<Spec> specs;
    std::vector<Visible> visible;
    json resolved = json::array();
    bool joined = false;
    std::array<double, 4> trim{};
};

json pole_rows(const Spec &spec) {
    json rows = json::array();
    for (int i = 0; i < spec.size[0]; ++i) {
        json row = json::array();
        for (int j = 0; j < spec.size[1]; ++j) {
            const Point3 p = spec.pole(i, j);
            row.push_back(json::array({p[0], p[1], p[2]}));
        }
        rows.push_back(row);
    }
    return rows;
}

json weight_rows(const Spec &spec) {
    json rows = json::array();
    for (int i = 0; i < spec.size[0]; ++i) {
        json row = json::array();
        for (int j = 0; j < spec.size[1]; ++j) row.push_back(spec.weight(i, j));
        rows.push_back(row);
    }
    return rows;
}

// An order given, or none (null), when the parameters were not a dictionary.
const json *present(const json *order) { return order && !order->is_null() ? order : nullptr; }

const json *order_at(const json *orders, size_t index) {
    return orders && orders->is_array() && index < orders->size() ? present(&orders->at(index)) : nullptr;
}

// _sheet: the patches with their joins resolved, and the window of each that the trim leaves visible.
Sheet sheet_of(const json &surface, const json *order, const json *orders) {
    if (!surface.is_object()) throw ValueError("Supply a surface");
    Sheet sheet;
    sheet.trim = trim_of(surface);
    const json *kind = get(surface, "kind");
    if (!(kind && *kind == json("sheet"))) {
        sheet.specs.push_back(surface_spec(surface, order));
        sheet.visible.push_back({0, sheet.trim});
        return sheet;
    }
    sheet.joined = true;
    const json *patches = get(surface, "patches");
    if (!patches || !patches->is_array() || patches->size() < 2 || patches->size() > kMaxPatches)
        throw ValueError("Use 2\xe2\x80\x93" "16 patches in a joined surface");
    const json *axis_name = get(surface, "axis");
    if (!axis_name || !(*axis_name == json("u") || *axis_name == json("v"))) throw ValueError("Select a U or V join");
    const int axis = *axis_name == json("u") ? 0 : 1, transverse = 1 - axis;
    const std::array<double, 4> &trim = sheet.trim;
    const double count = static_cast<double>(patches->size());
    for (size_t i = 0; i < patches->size(); ++i) {
        const json &patch = patches->at(i);
        if (!patch.is_object() || patch.contains("trim")) throw ValueError("Apply trim bounds to the whole joined surface");
        Spec spec = surface_spec(patch, order_at(orders, i));
        if (i > 0) {
            const json *mode = get(patch, "continuity");
            if (mode && !(*mode == json("position") || *mode == json("tangent"))) throw ValueError("Choose position or tangent continuity");
            const Spec &previous = sheet.specs.back();
            if (spec.size[transverse] != previous.size[transverse] || spec.degree[transverse] != previous.degree[transverse])
                throw ValueError("Joined edges need the same transverse pole count and degree");
            // The join direction first: p[k][t] is pole k along the join, t across it.
            const auto at = [axis](Spec &s, int k, int t) -> Point3 & { return axis == 0 ? s.pole(k, t) : s.pole(t, k); };
            const auto wt = [axis](Spec &s, int k, int t) -> double & { return axis == 0 ? s.weight(k, t) : s.weight(t, k); };
            Spec &a = sheet.specs.back();
            const int last = a.size[axis] - 1;
            for (int t = 0; t < spec.size[transverse]; ++t) {
                at(spec, 0, t) = at(a, last, t);
                wt(spec, 0, t) = wt(a, last, t);
            }
            if (mode && *mode == json("tangent")) {
                // The endpoint derivative of an open uniform rational spline: degree and first knot span.
                const double prev_factor = static_cast<double>(a.degree[axis] * (a.size[axis] - a.degree[axis]));
                const double factor = static_cast<double>(spec.degree[axis] * (spec.size[axis] - spec.degree[axis]));
                const double scale = prev_factor / factor;
                std::vector<double> w1(static_cast<size_t>(spec.size[transverse]));
                bool range = true;
                for (int t = 0; t < spec.size[transverse]; ++t) {
                    w1[static_cast<size_t>(t)] = wt(a, last, t) + scale * (wt(a, last, t) - wt(a, last - 1, t));
                    if (w1[static_cast<size_t>(t)] < 0.001 || w1[static_cast<size_t>(t)] > 1000) range = false;
                }
                for (int t = 0; t < spec.size[transverse]; ++t) wt(spec, 1, t) = w1[static_cast<size_t>(t)];
                if (!range) throw ValueError("Tangent join exceeds the weight range. Adjust parent weights or use Position.");
                bool inside = true;
                for (int t = 0; t < spec.size[transverse]; ++t) {
                    Point3 p{};
                    for (size_t k = 0; k < 3; ++k) {
                        const double h0 = at(a, last, t)[k] * wt(a, last, t), h1 = at(a, last - 1, t)[k] * wt(a, last - 1, t);
                        p[k] = (h0 + scale * (h0 - h1)) / w1[static_cast<size_t>(t)];
                        if (!(std::fabs(p[k]) <= 100000)) inside = false;
                    }
                    at(spec, 1, t) = p;
                }
                if (!inside) throw ValueError("Tangent join exceeds the modelling range");
            }
        }
        json resolved = patch;
        resolved["poles"] = pole_rows(spec);
        resolved["weights"] = weight_rows(spec);
        sheet.resolved.push_back(resolved);
        sheet.specs.push_back(std::move(spec));
        std::array<double, 4> window = trim;
        const double lo = std::max(0.0, trim[2 * axis] * count - static_cast<double>(i));
        const double hi = std::min(1.0, trim[2 * axis + 1] * count - static_cast<double>(i));
        if (lo < hi) {
            window[2 * axis] = lo;
            window[2 * axis + 1] = hi;
            sheet.visible.push_back({i, window});
        }
    }
    return sheet;
}

// np.linspace(lo, hi, n).
std::vector<double> linspace(double lo, double hi, int n) {
    std::vector<double> out(static_cast<size_t>(n));
    const double step = (hi - lo) / (n - 1);
    for (int k = 0; k < n; ++k) out[static_cast<size_t>(k)] = k * step + lo;
    out.back() = hi;
    return out;
}

// _basis: the size basis functions at each parameter, row by row.
std::vector<std::vector<double>> basis(const std::vector<double> &ts, int size, int degree, bool bezier) {
    std::vector<std::vector<double>> out(ts.size(), std::vector<double>(static_cast<size_t>(size), 0.0));
    if (bezier) {
        for (size_t u = 0; u < ts.size(); ++u) {
            const double t = ts[u];
            double comb = 1;
            for (int i = 0; i < size; ++i) {
                if (i > 0) comb = comb * (degree - i + 1) / i;
                out[u][static_cast<size_t>(i)] = comb * std::pow(1 - t, degree - i) * std::pow(t, i);
            }
        }
        return out;
    }
    std::vector<double> knots;
    const std::vector<double> distinct = linspace(0.0, 1.0, size - degree + 1);
    for (size_t k = 0; k < distinct.size(); ++k) {
        const int repeat = k == 0 || k + 1 == distinct.size() ? degree + 1 : 1;
        for (int r = 0; r < repeat; ++r) knots.push_back(distinct[k]);
    }
    for (size_t u = 0; u < ts.size(); ++u) {
        const double t = ts[u];
        std::vector<double> b(knots.size() - 1);
        for (size_t i = 0; i + 1 < knots.size(); ++i) b[i] = knots[i] <= t && t < knots[i + 1] ? 1.0 : 0.0;
        for (int order = 1; order <= degree; ++order) {
            std::vector<double> next(b.size() - 1, 0.0);
            for (size_t i = 0; i < next.size(); ++i) {
                const double left = knots[i + static_cast<size_t>(order)] - knots[i];
                const double right = knots[i + static_cast<size_t>(order) + 1] - knots[i + 1];
                if (left != 0) next[i] += (t - knots[i]) / left * b[i];
                if (right != 0) next[i] += (knots[i + static_cast<size_t>(order) + 1] - t) / right * b[i + 1];
            }
            b = next;
        }
        if (t == 1) b.back() = 1;
        for (int i = 0; i < size; ++i) out[u][static_cast<size_t>(i)] = b[static_cast<size_t>(i)];
    }
    return out;
}

// The surface at one parameter pair, from the basis values there.
Point3 evaluate(const Spec &spec, const std::vector<double> &bu, const std::vector<double> &bv) {
    double x = 0, y = 0, z = 0, w = 0;
    for (int i = 0; i < spec.size[0]; ++i)
        for (int j = 0; j < spec.size[1]; ++j) {
            const double f = bu[static_cast<size_t>(i)] * bv[static_cast<size_t>(j)], weight = spec.weight(i, j);
            const Point3 p = spec.pole(i, j);
            x += f * (p[0] * weight);
            y += f * (p[1] * weight);
            z += f * (p[2] * weight);
            w += f * weight;
        }
    return {x / w, y / w, z / w};
}

struct Mesh {
    std::vector<Point3> vertices;
    std::vector<std::array<int, 3>> triangles;
    double area = 0;
    std::array<double, 6> bounds{};
};

double triangle_area(const Point3 &a, const Point3 &b, const Point3 &c) {
    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    return std::sqrt(cx * cx + cy * cy + cz * cz);
}

void finish(Mesh &mesh) {
    double twice = 0;
    for (const std::array<int, 3> &t : mesh.triangles)
        twice += triangle_area(mesh.vertices[static_cast<size_t>(t[0])], mesh.vertices[static_cast<size_t>(t[1])], mesh.vertices[static_cast<size_t>(t[2])]);
    mesh.area = twice / 2;
    for (size_t k = 0; k < 3; ++k) {
        mesh.bounds[k] = mesh.vertices[0][k];
        mesh.bounds[k + 3] = mesh.vertices[0][k];
    }
    for (const Point3 &p : mesh.vertices)
        for (size_t k = 0; k < 3; ++k) {
            mesh.bounds[k] = std::min(mesh.bounds[k], p[k]);
            mesh.bounds[k + 3] = std::max(mesh.bounds[k + 3], p[k]);
        }
}

// _patch_mesh: the 25 by 25 grid of the window.
Mesh patch_mesh(const Spec &spec, const std::array<double, 4> &window) {
    const int n = kGrid;
    const std::vector<std::vector<double>> bu = basis(linspace(window[0], window[1], n + 1), spec.size[0], spec.degree[0], spec.bezier);
    const std::vector<std::vector<double>> bv = basis(linspace(window[2], window[3], n + 1), spec.size[1], spec.degree[1], spec.bezier);
    Mesh mesh;
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j) mesh.vertices.push_back(evaluate(spec, bu[static_cast<size_t>(i)], bv[static_cast<size_t>(j)]));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const int a = i * (n + 1) + j;
            mesh.triangles.push_back({a, a + n + 1, a + 1});
            mesh.triangles.push_back({a + 1, a + n + 1, a + n + 2});
        }
    finish(mesh);
    if (mesh.area < 1e-8) throw ValueError("The control net collapses to zero surface area");
    return mesh;
}

// ---- outlines ----------------------------------------------------------------------------------------------

using Loop = std::vector<UV>;

double twice_area(const Loop &loop) {
    double sum = 0;
    for (size_t i = 0; i < loop.size(); ++i) {
        const UV &a = loop[i], &b = loop[(i + 1) % loop.size()];
        sum += a[0] * b[1] - b[0] * a[1];
    }
    return sum;
}

bool on_segment(const UV &a, const UV &b, const UV &p) {
    return std::min(a[0], b[0]) <= p[0] && p[0] <= std::max(a[0], b[0]) && std::min(a[1], b[1]) <= p[1] && p[1] <= std::max(a[1], b[1]);
}

// How two segments meet: 0 apart, 1 touching, 2 crossing at a point inside both.
int meet(const UV &a, const UV &b, const UV &c, const UV &d) {
    const int o1 = geos_orientation(a, b, c), o2 = geos_orientation(a, b, d), o3 = geos_orientation(c, d, a), o4 = geos_orientation(c, d, b);
    if (o1 * o2 < 0 && o3 * o4 < 0) return 2;
    if ((o1 == 0 && on_segment(a, b, c)) || (o2 == 0 && on_segment(a, b, d)) || (o3 == 0 && on_segment(c, d, a)) ||
        (o4 == 0 && on_segment(c, d, b)))
        return 1;
    return 0;
}

// Whether a point that is on no edge of the loop is inside it.
bool inside(const Loop &loop, const UV &q) {
    bool in = false;
    for (size_t i = 0; i < loop.size(); ++i) {
        const UV &a = loop[i], &b = loop[(i + 1) % loop.size()];
        if ((a[1] <= q[1]) == (b[1] <= q[1])) continue;
        const int o = geos_orientation(a, b, q);
        if (b[1] > a[1] ? o > 0 : o < 0) in = !in;
    }
    return in;
}

// Polygon(loops[0], loops[1:]).is_valid for loops that do not touch: each ring simple, the holes inside the shell
// and outside one another. Where loops touch, Shapely's rules (a touch at a point allowed unless the touches cut the
// inside in two) are the reference's; the core's own rule refuses such an outline, as it refuses loops that cross.
bool valid_outline(const std::vector<Loop> &loops) {
    for (const Loop &loop : loops) {
        std::vector<std::array<double, 2>> closed(loop.begin(), loop.end());
        closed.push_back(loop.front());
        const LineCheck check = check_line(closed);
        if (check.declined) throw NotPorted(check.reason);
        if (!check.simple) return false;
    }
    for (size_t a = 0; a < loops.size(); ++a)
        for (size_t b = a + 1; b < loops.size(); ++b)
            for (size_t i = 0; i < loops[a].size(); ++i)
                for (size_t j = 0; j < loops[b].size(); ++j) {
                    const int how = meet(loops[a][i], loops[a][(i + 1) % loops[a].size()], loops[b][j], loops[b][(j + 1) % loops[b].size()]);
                    if (how == 2) return false;
                    if (how == 1) {
                        own_rule("A surface outline whose loops touch is tessellated by Studio's Python surface reader");
                        return false;
                    }
                }
    for (size_t h = 1; h < loops.size(); ++h) {
        if (!inside(loops[0], loops[h][0])) return false;
        for (size_t other = 1; other < loops.size(); ++other)
            if (other != h && inside(loops[other], loops[h][0])) return false;
    }
    return true;
}

Ring ring_mm(const Loop &loop) {
    Ring ring;
    for (const UV &p : loop) ring.push_back(Vec2{p[0] * kScale, p[1] * kScale});
    return ring;
}

Ring box_ring(double u0, double v0, double u1, double v1) {
    return {Vec2{u0 * kScale, v0 * kScale}, Vec2{u1 * kScale, v0 * kScale}, Vec2{u1 * kScale, v1 * kScale}, Vec2{u0 * kScale, v1 * kScale}};
}

// The UV area of each piece of a region.
std::vector<double> piece_areas(const RegionTriangles &region) {
    std::vector<double> areas;
    for (const std::vector<int> &piece : region.pieces) {
        double twice = 0;
        for (const int t : piece) {
            const std::array<int, 3> &tri = region.triangles[static_cast<size_t>(t)];
            const Vec2 &a = region.points[static_cast<size_t>(tri[0])], &b = region.points[static_cast<size_t>(tri[1])],
                       &c = region.points[static_cast<size_t>(tri[2])];
            twice += (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        }
        areas.push_back(twice / 2 / (kScale * kScale));
    }
    return areas;
}

struct Piece {
    size_t patch;  // which spec
    std::vector<Ring> outline;  // millimetres on the arrangement's scale
    std::vector<UV> corners;    // the outline's corners in the patch's parameters, as the reference has them
    std::array<double, 4> clip{};  // u0, v0, u1, v1 of the rectangle the piece is cut to
};

// A point of a piece's outline as the reference's polygon has it: its corners are the outline's own (the core's
// are rounded to its grid) or lie on the clipping rectangle, whose sides are exact.
UV exact_point(const Piece &piece, const Vec2 &p) {
    const double step = 2 / (kScale * 1e6);  // two steps of the arrangement's grid, in parameters
    UV at = {p.x / kScale, p.y / kScale};
    for (const UV &q : piece.corners)
        if (std::fabs(q[0] - at[0]) <= step && std::fabs(q[1] - at[1]) <= step) return q;
    for (size_t axis = 0; axis < 2; ++axis)
        for (const double side : {piece.clip[axis], piece.clip[axis + 2]})
            if (std::fabs(at[axis] - side) <= step) at[axis] = side;
    return at;
}

// The bounds of a piece as the reference's polygon has them.
std::array<double, 4> exact_bounds(const Piece &piece) {
    std::array<double, 4> bounds = {1e300, 1e300, -1e300, -1e300};
    for (const Ring &ring : piece.outline)
        for (const Vec2 &p : ring) {
            const UV at = exact_point(piece, p);
            bounds[0] = std::min(bounds[0], at[0]), bounds[2] = std::max(bounds[2], at[0]);
            bounds[1] = std::min(bounds[1], at[1]), bounds[3] = std::max(bounds[3], at[1]);
        }
    return bounds;
}

// A coordinate on the arrangement's grid as its points carry it: rounded to the nanometre.
double snapped(double mm) { return std::round(mm * 1e6) / 1e6; }

// Whether d is inside the circle through a, b and c (counter-clockwise): 1 inside, -1 outside, 0 on it to within
// rounding.
int incircle(const Vec2 &a, const Vec2 &b, const Vec2 &c, const Vec2 &d) {
    const double ax = a.x - d.x, ay = a.y - d.y, bx = b.x - d.x, by = b.y - d.y, cx = c.x - d.x, cy = c.y - d.y;
    const double det = (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
                       (cx * cx + cy * cy) * (ax * by - bx * ay);
    double size = 0;
    for (const double v : {ax, ay, bx, by, cx, cy}) size = std::max(size, std::fabs(v));
    const double tie = 1e-9 * size * size * size * size;
    return det > tie ? 1 : det < -tie ? -1 : 0;
}

double turn(const Vec2 &a, const Vec2 &b, const Vec2 &c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

// The constrained Delaunay triangulation of the cells, as the reference's triangulator gives it: every edge
// that is not on a grid line or the outline flipped until each triangle's circle holds no other corner, and a
// cell whose four corners share a circle (a whole cell) split from its lowest to its highest corner.
void delaunay(const std::vector<Vec2> &points, std::vector<std::array<int, 3>> &triangles, const std::vector<double> &xs,
              const std::vector<double> &ys) {
    const auto fixed = [&](int a, int b) {
        const Vec2 &p = points[static_cast<size_t>(a)], &q = points[static_cast<size_t>(b)];
        return (p.x == q.x && std::binary_search(xs.begin(), xs.end(), p.x)) || (p.y == q.y && std::binary_search(ys.begin(), ys.end(), p.y));
    };
    for (int pass = 0; pass < 64; ++pass) {
        std::map<std::pair<int, int>, size_t> owner;
        for (size_t t = 0; t < triangles.size(); ++t)
            for (size_t k = 0; k < 3; ++k) owner[{triangles[t][k], triangles[t][(k + 1) % 3]}] = t;
        std::vector<char> touched(triangles.size(), 0);
        bool flipped = false;
        for (size_t t = 0; t < triangles.size(); ++t)
            for (size_t k = 0; k < 3 && !touched[t]; ++k) {
                const int a = triangles[t][k], b = triangles[t][(k + 1) % 3], c = triangles[t][(k + 2) % 3];
                if (a > b || fixed(a, b)) continue;
                const auto other = owner.find({b, a});
                if (other == owner.end() || touched[other->second]) continue;
                const size_t u = other->second;
                int d = -1;
                for (size_t m = 0; m < 3; ++m)
                    if (triangles[u][m] != a && triangles[u][m] != b) d = triangles[u][m];
                const Vec2 &pa = points[static_cast<size_t>(a)], &pb = points[static_cast<size_t>(b)], &pc = points[static_cast<size_t>(c)],
                           &pd = points[static_cast<size_t>(d)];
                // The quadrilateral a, d, b, c must be convex for c-d to replace a-b.
                if (!(turn(pa, pd, pc) > 0 && turn(pd, pb, pc) > 0)) continue;
                const int inside = incircle(pa, pb, pc, pd);
                bool flip = inside > 0;
                if (inside == 0) {
                    const auto low = [&](const Vec2 &p) { return p.x + p.y; };
                    const double lowest = std::min({low(pa), low(pb), low(pc), low(pd)});
                    flip = low(pa) != lowest && low(pb) != lowest;
                }
                if (!flip) continue;
                triangles[t] = {a, d, c};
                triangles[u] = {d, b, c};
                touched[t] = touched[u] = 1;
                flipped = true;
            }
        if (!flipped) break;
    }
}

// _trimmed_mesh: the piece's bounds cut into the reference's 24 by 24 cells, and the region triangulated.
Mesh trimmed_mesh(const Spec &spec, const Piece &piece) {
    const std::array<double, 4> bounds = exact_bounds(piece);
    const std::vector<double> us = linspace(bounds[0], bounds[2], kGrid + 1), vs = linspace(bounds[1], bounds[3], kGrid + 1);
    const double u0 = bounds[0] * kScale, v0 = bounds[1] * kScale, u1 = bounds[2] * kScale, v1 = bounds[3] * kScale;
    std::vector<std::array<Vec2, 2>> lines;
    for (const double u : us) lines.push_back({Vec2{u * kScale, v0}, Vec2{u * kScale, v1}});
    for (const double v : vs) lines.push_back({Vec2{u0, v * kScale}, Vec2{u1, v * kScale}});
    RegionTriangles region = region_triangles(piece.outline, lines, 1);
    if (region.triangles.empty()) throw ValueError("The trimmed surface is too small to tessellate");
    std::vector<double> xs, ys;
    for (const double u : us) xs.push_back(snapped(u * kScale));
    for (const double v : vs) ys.push_back(snapped(v * kScale));
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    delaunay(region.points, region.triangles, xs, ys);
    // The points in the order np.unique gives them: by u, then by v.
    std::vector<UV> uv;
    for (const Vec2 &p : region.points) uv.push_back({std::min(1.0, std::max(0.0, p.x / kScale)), std::min(1.0, std::max(0.0, p.y / kScale))});
    std::vector<int> order(uv.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = static_cast<int>(k);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return uv[static_cast<size_t>(a)] < uv[static_cast<size_t>(b)]; });
    // Points within 64 steps of the arrangement's grid (1.6e-8 in parameters) of one another are one. Where the
    // reference's grid passes through a corner of the outline, or a cell's corner lies on its edge, Shapely meets
    // them there, and the core's grid, rounded to the nanometre, passes beside them: by a step, and along an edge
    // that crosses the grid at a slant by the step over the slant's sine.
    const double near = 64 / (kScale * 1e6);
    std::vector<int> place(uv.size(), -1);
    std::vector<double> ucol, vcol;
    for (size_t k = 0; k < order.size(); ++k) {
        const UV &p = uv[static_cast<size_t>(order[k])];
        int same = -1;
        for (size_t back = k; back-- > 0;) {
            const UV &q = uv[static_cast<size_t>(order[back])];
            if (p[0] - q[0] > near) break;
            if (std::fabs(p[1] - q[1]) <= near) {
                same = place[static_cast<size_t>(order[back])];
                break;
            }
        }
        if (same < 0) {
            ucol.push_back(p[0]);
            vcol.push_back(p[1]);
            same = static_cast<int>(ucol.size()) - 1;
        }
        place[static_cast<size_t>(order[k])] = same;
    }
    const std::vector<std::vector<double>> bu = basis(ucol, spec.size[0], spec.degree[0], spec.bezier);
    const std::vector<std::vector<double>> bv = basis(vcol, spec.size[1], spec.degree[1], spec.bezier);
    Mesh mesh;
    for (size_t k = 0; k < ucol.size(); ++k) mesh.vertices.push_back(evaluate(spec, bu[k], bv[k]));
    for (const std::array<int, 3> &t : region.triangles) {
        const std::array<int, 3> q = {place[static_cast<size_t>(t[0])], place[static_cast<size_t>(t[1])], place[static_cast<size_t>(t[2])]};
        if (q[0] != q[1] && q[1] != q[2] && q[2] != q[0]) mesh.triangles.push_back(q);
    }
    finish(mesh);
    if (mesh.area < 1e-8) throw ValueError("The retained surface collapses to zero area");
    return mesh;
}

bool distinct_points(const json &ring) {
    std::vector<UV> points;
    for (const json &p : ring) points.push_back({value_of(p.at(0)), value_of(p.at(1))});
    std::sort(points.begin(), points.end());
    return std::adjacent_find(points.begin(), points.end()) == points.end();
}

// _regions: the visible patches, each cut to the outline where there is one.
std::vector<Piece> regions(const json &surface, const Sheet &sheet) {
    std::vector<Piece> pieces;
    const json *loops = get(surface, "boundary");
    if (!loops || loops->is_null()) {
        for (const Visible &v : sheet.visible) pieces.push_back({v.patch, {}, {}, {}});
        return pieces;
    }
    if (!loops->is_array() || loops->size() < 1 || loops->size() > 9) throw ValueError("Draw an outline with up to eight interior holes");
    for (const json &ring : *loops) {
        bool good = ring.is_array() && ring.size() >= 3 && ring.size() <= 64;
        for (size_t k = 0; good && k < ring.size(); ++k) {
            const json &pt = ring.at(k);
            good = pt.is_array() && pt.size() == 2;
            for (size_t c = 0; good && c < 2; ++c) good = number(&pt.at(c)) && value_of(pt.at(c)) >= 0 && value_of(pt.at(c)) <= 1;
        }
        if (!good || !distinct_points(ring)) throw ValueError("Each UV loop needs 3\xe2\x80\x93" "64 distinct points inside the unit square");
    }
    std::vector<Loop> outline;
    for (const json &ring : *loops) {
        Loop loop;
        for (const json &p : ring) loop.push_back({value_of(p.at(0)), value_of(p.at(1))});
        outline.push_back(loop);
    }
    double area = std::fabs(twice_area(outline[0]));
    for (size_t h = 1; h < outline.size(); ++h) area -= std::fabs(twice_area(outline[h]));
    if (!valid_outline(outline) || area / 2 < 1e-10) throw ValueError("Use a simple outline with non-overlapping holes inside it");
    // The shell counter-clockwise and the holes clockwise, so that inside the outline winds once.
    for (size_t h = 0; h < outline.size(); ++h)
        if ((twice_area(outline[h]) > 0) != (h == 0)) std::reverse(outline[h].begin(), outline[h].end());
    const std::array<double, 4> &trim = sheet.trim;
    std::vector<Ring> rings;
    for (const Loop &loop : outline) rings.push_back(ring_mm(loop));
    {
        std::vector<Ring> kept = rings;
        kept.push_back(box_ring(trim[0], trim[2], trim[1], trim[3]));
        const RegionTriangles region = region_triangles(kept, {}, 2);
        const std::vector<double> areas = piece_areas(region);
        if (areas.size() != 1 || areas[0] < 1e-10) throw ValueError("Trim bounds and outline must retain one connected region");
    }
    const size_t count = sheet.joined ? sheet.specs.size() : 1;
    const json *axis_name = get(surface, "axis");
    const bool along_u = !axis_name || *axis_name == json("u");
    const long long first = static_cast<long long>((along_u ? trim[0] : trim[2]) * static_cast<double>(count));
    for (size_t k = 0; k < sheet.visible.size(); ++k) {
        const double i = static_cast<double>(first + static_cast<long long>(k)), n = static_cast<double>(count);
        // affine_transform(kept, [n, 0, 0, 1, -i, 0]) (or across v) and its intersection with the unit square.
        std::vector<Ring> local;
        std::vector<UV> corners;
        for (const Loop &loop : outline) {
            Loop moved;
            for (const UV &p : loop) moved.push_back(along_u ? UV{n * p[0] + 0 * p[1] + -i, p[1]} : UV{p[0], 0 * p[0] + n * p[1] + -i});
            corners.insert(corners.end(), moved.begin(), moved.end());
            local.push_back(ring_mm(moved));
        }
        double lo[2] = {trim[0], trim[2]}, hi[2] = {trim[1], trim[3]};
        const int a = along_u ? 0 : 1;
        lo[a] = n * lo[a] - i;
        hi[a] = n * hi[a] - i;
        const double u0 = std::max(0.0, lo[0]), v0 = std::max(0.0, lo[1]), u1 = std::min(1.0, hi[0]), v1 = std::min(1.0, hi[1]);
        if (!(u0 < u1 && v0 < v1)) continue;
        local.push_back(box_ring(u0, v0, u1, v1));
        const RegionTriangles region = region_triangles(local, {}, 2);
        const std::vector<double> areas = piece_areas(region);
        for (size_t p = 0; p < areas.size(); ++p)
            if (areas[p] > 1e-12) pieces.push_back({sheet.visible[k].patch, region.outlines[p], corners, {u0, v0, u1, v1}});
    }
    if (pieces.empty()) throw ValueError("No surface remains inside this trim");
    return pieces;
}

json mesh_json(const Mesh &mesh) {
    json out = json::object();
    json vertices = json::array();
    for (const Point3 &p : mesh.vertices) vertices.push_back(json::array({p[0], p[1], p[2]}));
    json triangles = json::array();
    for (const std::array<int, 3> &t : mesh.triangles) triangles.push_back(json::array({t[0], t[1], t[2]}));
    out["vertices"] = vertices;
    out["triangles"] = triangles;
    out["area"] = mesh.area;
    json bounds = json::array();
    for (const double b : mesh.bounds) bounds.push_back(b);
    out["bounds"] = bounds;
    out["faces"] = 1;
    out["solids"] = 0;
    return out;
}

}  // namespace

Json mesh(const Json &request) {
    const json *surface = request.is_object() ? get(request, "surface") : nullptr;
    const json none;
    const Sheet sheet = sheet_of(surface ? *surface : none, present(get(request, "order")), get(request, "orders"));
    const std::vector<Piece> pieces = regions(*surface, sheet);
    std::vector<Mesh> meshes;
    for (const Piece &piece : pieces) {
        const Spec &spec = sheet.specs[piece.patch];
        if (piece.outline.empty()) {
            for (const Visible &v : sheet.visible)
                if (v.patch == piece.patch) meshes.push_back(patch_mesh(spec, v.window));
        } else {
            meshes.push_back(trimmed_mesh(spec, piece));
        }
    }
    if (!sheet.joined) return mesh_json(meshes[0]);
    Mesh merged;
    for (const Mesh &m : meshes) {
        const int offset = static_cast<int>(merged.vertices.size());
        for (const std::array<int, 3> &t : m.triangles) merged.triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
        merged.vertices.insert(merged.vertices.end(), m.vertices.begin(), m.vertices.end());
    }
    json out = json::object();
    json vertices = json::array();
    for (const Point3 &p : merged.vertices) vertices.push_back(json::array({p[0], p[1], p[2]}));
    json triangles = json::array();
    for (const std::array<int, 3> &t : merged.triangles) triangles.push_back(json::array({t[0], t[1], t[2]}));
    out["vertices"] = vertices;
    out["triangles"] = triangles;
    double area = 0;
    std::array<double, 6> bounds = meshes[0].bounds;
    for (const Mesh &m : meshes) {
        area += m.area;
        for (size_t k = 0; k < 3; ++k) {
            bounds[k] = std::min(bounds[k], m.bounds[k]);
            bounds[k + 3] = std::max(bounds[k + 3], m.bounds[k + 3]);
        }
    }
    out["area"] = area;
    json box = json::array();
    for (const double b : bounds) box.push_back(b);
    out["bounds"] = box;
    out["faces"] = static_cast<long long>(meshes.size());
    out["solids"] = 0;
    out["resolvedPatches"] = sheet.resolved;
    return out;
}

namespace {

// A piece's UV outline as orient(polygon, sign=1.0) gives it to _surface_face: the shell counter-clockwise, then the
// holes clockwise, each closed by its first point. Points that the arrangement's grid merges are given once.
json uv_domain(const Piece &piece) {
    json shell = json::array(), holes = json::array();
    for (const Ring &ring : piece.outline) {
        std::vector<UV> points;
        for (const Vec2 &p : ring) {
            const UV at = exact_point(piece, p);
            if (points.empty() || points.back() != at) points.push_back(at);
        }
        while (points.size() > 1 && points.back() == points.front()) points.pop_back();
        if (points.size() < 3) continue;
        points.push_back(points.front());
        json loop = json::array();
        for (const UV &at : points) loop.push_back(json::array({at[0], at[1]}));
        if (solid::ring_area(ring) > 0) shell = loop;
        else holes.push_back(loop);
    }
    json domain = json::array({shell});
    for (const json &hole : holes) domain.push_back(hole);
    return domain;
}

}  // namespace

Json step_faces(const Json &request) {
    const json *surface = request.is_object() ? get(request, "surface") : nullptr;
    const json none;
    const Sheet sheet = sheet_of(surface ? *surface : none, present(get(request, "order")), get(request, "orders"));
    const std::vector<Piece> pieces = regions(*surface, sheet);
    json faces = json::array();
    for (const Piece &piece : pieces) {
        const Spec &spec = sheet.specs[piece.patch];
        std::array<double, 4> window = sheet.trim;
        for (const Visible &v : sheet.visible)
            if (v.patch == piece.patch) window = v.window;
        json face = json::object();
        try {
            patch_mesh(spec, window);  // the editor's degeneracy check, which _surface_face makes first
        } catch (const ValueError &error) {
            // Raised where the reference raises it: after the faces before this one are built.
            face["error"] = error.what();
            faces.push_back(face);
            break;
        }
        face["kind"] = spec.bezier ? "bezier" : "bspline";
        face["poles"] = pole_rows(spec);
        face["weights"] = weight_rows(spec);
        if (!spec.bezier) {
            // _knots: open uniform knots, full multiplicity at the ends and simple interior knots.
            json degrees = json::array(), knots = json::array(), multiplicities = json::array();
            for (int axis = 0; axis < 2; ++axis) {
                const int size = spec.size[axis], degree = spec.degree[axis];
                const std::vector<double> distinct = linspace(0.0, 1.0, size - degree + 1);
                json values = json::array(), counts = json::array();
                for (size_t k = 0; k < distinct.size(); ++k) {
                    values.push_back(distinct[k]);
                    counts.push_back(k == 0 || k + 1 == distinct.size() ? degree + 1 : 1);
                }
                degrees.push_back(degree);
                knots.push_back(values);
                multiplicities.push_back(counts);
            }
            face["degrees"] = degrees;
            face["knots"] = knots;
            face["multiplicities"] = multiplicities;
        }
        if (piece.outline.empty()) face["window"] = json::array({window[0], window[1], window[2], window[3]});
        else face["domain"] = uv_domain(piece);
        faces.push_back(face);
    }
    json out = json::object();
    out["faces"] = faces;
    return out;
}

}  // namespace surface
}  // namespace kerfkite
