// The Solid type: validation, canonical order, measures and affine maps.
#include "mesh.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "radial.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

namespace {

const Mesh &empty_mesh() {
    static const Mesh mesh;
    return mesh;
}

struct GridPredicates {
    const std::vector<GridPoint> *v;
    int orient3d(int a, int b, int c, int d) const {
        return orient3d_grid((*v)[static_cast<size_t>(a)].data(), (*v)[static_cast<size_t>(b)].data(),
                             (*v)[static_cast<size_t>(c)].data(), (*v)[static_cast<size_t>(d)].data());
    }
    int orient2d(int a, int b, int c, int au, int av) const {
        return orient2d_grid((*v)[static_cast<size_t>(a)].data(), (*v)[static_cast<size_t>(b)].data(),
                             (*v)[static_cast<size_t>(c)].data(), au, av);
    }
};

struct UnionFind {
    std::vector<uint32_t> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
    uint32_t find(uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[a < b ? b : a] = a < b ? a : b;
    }
};

bool zero_area(const GridPoint &a, const GridPoint &b, const GridPoint &c) {
    const int64_t ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const int64_t vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    return (I128::mul(uy, vz) - I128::mul(uz, vy)).zero() && (I128::mul(uz, vx) - I128::mul(ux, vz)).zero() &&
           (I128::mul(ux, vy) - I128::mul(uy, vx)).zero();
}

}  // namespace

const Mesh &Access::mesh(const Solid &solid) { return solid.mesh_ ? *solid.mesh_ : empty_mesh(); }

Solid Access::wrap(std::shared_ptr<const Mesh> mesh) {
    Solid solid;
    solid.mesh_ = std::move(mesh);
    return solid;
}

int64_t to_grid(double mm) {
    const double units = mm * kUnitsPerMm;
    if (!(std::fabs(units) <= static_cast<double>(kMaxCoord)))
        throw ValueError("Coordinates must be finite and within \xC2\xB1" "100000 mm");
    return static_cast<int64_t>(std::round(units));
}

BigInt six_volume(const std::vector<GridPoint> &v, const std::vector<Triangle> &t) {
    BigInt total;
    if (t.empty()) return total;
    const GridPoint origin = v[t[0][0]];
    I128 partial;
    size_t pending = 0;
    for (const Triangle &tri : t) {
        // Each determinant is below 2^117; 256 of them stay inside 128 bits.
        partial = partial + orient3d_value(origin.data(), v[tri[0]].data(), v[tri[1]].data(), v[tri[2]].data());
        if (++pending == 256) {
            total = total + BigInt(partial);
            partial = I128();
            pending = 0;
        }
    }
    return total + BigInt(partial);
}

std::string check_mesh(const std::vector<GridPoint> &v, const std::vector<Triangle> &t) {
    if (t.empty()) return std::string();
    if (t.size() > kMaxTriangles) return "it has more than " + std::to_string(kMaxTriangles) + " triangles";
    for (const GridPoint &p : v)
        for (int k = 0; k < 3; ++k)
            if (p[static_cast<size_t>(k)] > kMaxCoord || p[static_cast<size_t>(k)] < -kMaxCoord)
                return "a coordinate is outside \xC2\xB1" "100000 mm";
    const size_t nv = v.size();
    std::vector<uint64_t> edges;
    edges.reserve(t.size() * 3);
    for (size_t i = 0; i < t.size(); ++i) {
        const Triangle &tri = t[i];
        if (tri[0] >= nv || tri[1] >= nv || tri[2] >= nv) return "triangle " + std::to_string(i) + " names a vertex that does not exist";
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2] || zero_area(v[tri[0]], v[tri[1]], v[tri[2]]))
            return "triangle " + std::to_string(i) + " has no area";
        for (int k = 0; k < 3; ++k)
            edges.push_back((static_cast<uint64_t>(tri[static_cast<size_t>(k)]) << 32) | tri[static_cast<size_t>((k + 1) % 3)]);
    }
    std::sort(edges.begin(), edges.end());
    for (size_t i = 1; i < edges.size(); ++i)
        if (edges[i] == edges[i - 1])
            return "an edge is used twice in the same direction (the surface is not consistently oriented, or more than two triangles meet there)";
    for (const uint64_t e : edges)
        if (!std::binary_search(edges.begin(), edges.end(), (e << 32) | (e >> 32)))
            return "the surface is not closed (an edge has a triangle on one side only)";
    if (six_volume(v, t).sign() <= 0) return "it has no positive volume (it is flat or inside out)";
    return std::string();
}

Solid make_solid(std::vector<GridPoint> v, std::vector<Triangle> t, bool simple, const char *context, bool trusted) {
    if (t.empty()) return Solid();
    const size_t nv = v.size();
    std::vector<char> used(nv, 0);
    for (const Triangle &tri : t)
        for (int k = 0; k < 3; ++k) {
            if (tri[static_cast<size_t>(k)] >= nv) throw ValueError(std::string(context) + ": a triangle names a vertex that does not exist");
            used[tri[static_cast<size_t>(k)]] = 1;
        }
    std::vector<uint32_t> order;
    order.reserve(nv);
    for (size_t i = 0; i < nv; ++i)
        if (used[i]) order.push_back(static_cast<uint32_t>(i));
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return v[a] != v[b] ? v[a] < v[b] : a < b; });
    std::vector<uint32_t> remap(nv, 0);
    auto mesh = std::make_shared<Mesh>();
    mesh->v.resize(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        remap[order[i]] = static_cast<uint32_t>(i);
        mesh->v[i] = v[order[i]];
    }
    mesh->t.resize(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        Triangle tri = {remap[t[i][0]], remap[t[i][1]], remap[t[i][2]]};
        if (tri[1] < tri[0] && tri[1] <= tri[2]) tri = {tri[1], tri[2], tri[0]};
        else if (tri[2] < tri[0] && tri[2] < tri[1]) tri = {tri[2], tri[0], tri[1]};
        mesh->t[i] = tri;
    }
    std::sort(mesh->t.begin(), mesh->t.end());
    if (!trusted) {
        const std::string why = check_mesh(mesh->v, mesh->t);
        if (!why.empty()) throw ValueError(std::string(context) + ": " + why);
    }
    for (int k = 0; k < 3; ++k) mesh->lo[k] = mesh->hi[k] = mesh->v[0][static_cast<size_t>(k)];
    for (const GridPoint &p : mesh->v)
        for (int k = 0; k < 3; ++k) {
            mesh->lo[k] = std::min(mesh->lo[k], p[static_cast<size_t>(k)]);
            mesh->hi[k] = std::max(mesh->hi[k], p[static_cast<size_t>(k)]);
        }
    mesh->simple = simple;
    return Access::wrap(mesh);
}

void weld_and_pair(std::vector<GridPoint> &v, std::vector<Triangle> &t, const char *context) {
    const std::string where(context);
    // Merge by position.
    std::vector<uint32_t> order(v.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return v[a] != v[b] ? v[a] < v[b] : a < b; });
    std::vector<uint32_t> remap(v.size(), 0);
    std::vector<GridPoint> points;
    for (size_t i = 0; i < order.size(); ++i) {
        if (i == 0 || v[order[i]] != v[order[i - 1]]) points.push_back(v[order[i]]);
        remap[order[i]] = static_cast<uint32_t>(points.size() - 1);
    }
    // Collapsed triangles go; a triangle and its mirror image cancel.
    std::vector<Triangle> faces;
    faces.reserve(t.size());
    for (const Triangle &tri : t) {
        if (tri[0] >= v.size() || tri[1] >= v.size() || tri[2] >= v.size()) throw ValueError(where + ": a triangle names a vertex that does not exist");
        Triangle f = {remap[tri[0]], remap[tri[1]], remap[tri[2]]};
        if (f[0] == f[1] || f[1] == f[2] || f[0] == f[2]) continue;
        if (f[1] < f[0] && f[1] <= f[2]) f = {f[1], f[2], f[0]};
        else if (f[2] < f[0] && f[2] < f[1]) f = {f[2], f[0], f[1]};
        faces.push_back(f);
    }
    // Sort so that a triangle (a, b, c) and its mirror (a, c, b) are adjacent.
    std::sort(faces.begin(), faces.end(), [](const Triangle &x, const Triangle &y) {
        const uint32_t xl = std::min(x[1], x[2]), xh = std::max(x[1], x[2]), yl = std::min(y[1], y[2]), yh = std::max(y[1], y[2]);
        if (x[0] != y[0]) return x[0] < y[0];
        if (xl != yl) return xl < yl;
        if (xh != yh) return xh < yh;
        return x[1] < y[1];
    });
    std::vector<Triangle> kept;
    for (size_t i = 0; i < faces.size();) {
        size_t j = i;
        long net = 0;
        while (j < faces.size() && faces[j][0] == faces[i][0] && std::min(faces[j][1], faces[j][2]) == std::min(faces[i][1], faces[i][2]) &&
               std::max(faces[j][1], faces[j][2]) == std::max(faces[i][1], faces[i][2])) {
            net += faces[j][1] < faces[j][2] ? 1 : -1;
            ++j;
        }
        const uint32_t lo = std::min(faces[i][1], faces[i][2]), hi = std::max(faces[i][1], faces[i][2]);
        for (long k = 0; k < (net < 0 ? -net : net); ++k) kept.push_back(net > 0 ? Triangle{faces[i][0], lo, hi} : Triangle{faces[i][0], hi, lo});
        i = j;
    }
    faces.swap(kept);
    if (faces.size() > kMaxTriangles) throw ValueError(where + ": more than " + std::to_string(kMaxTriangles) + " triangles");
    for (size_t i = 0; i < faces.size(); ++i)
        if (zero_area(points[faces[i][0]], points[faces[i][1]], points[faces[i][2]]))
            throw ValueError(where + ": a triangle has no area");

    // Pair the triangles at each edge.
    struct Use {
        uint32_t lo, hi, face;
        uint8_t corner;   // index in the face of the corner that starts this edge
        uint8_t forward;  // the face runs lo -> hi
    };
    std::vector<Use> uses;
    uses.reserve(faces.size() * 3);
    for (size_t f = 0; f < faces.size(); ++f)
        for (uint8_t k = 0; k < 3; ++k) {
            const uint32_t a = faces[f][k], b = faces[f][(k + 1) % 3];
            uses.push_back(Use{std::min(a, b), std::max(a, b), static_cast<uint32_t>(f), k, static_cast<uint8_t>(a < b)});
        }
    std::sort(uses.begin(), uses.end(), [](const Use &x, const Use &y) {
        if (x.lo != y.lo) return x.lo < y.lo;
        if (x.hi != y.hi) return x.hi < y.hi;
        if (x.face != y.face) return x.face < y.face;
        return x.corner < y.corner;
    });
    UnionFind corners(faces.size() * 3);
    const GridPredicates predicates{&points};
    const auto join = [&](const Use &x, const Use &y) {
        // Corner indices of lo and hi in each face.
        const uint32_t x_lo = x.face * 3 + (x.forward ? x.corner : (x.corner + 1) % 3), x_hi = x.face * 3 + (x.forward ? (x.corner + 1) % 3 : x.corner);
        const uint32_t y_lo = y.face * 3 + (y.forward ? y.corner : (y.corner + 1) % 3), y_hi = y.face * 3 + (y.forward ? (y.corner + 1) % 3 : y.corner);
        corners.unite(x_lo, y_lo);
        corners.unite(x_hi, y_hi);
    };
    std::vector<int> third, ring;
    for (size_t i = 0; i < uses.size();) {
        size_t j = i;
        size_t forward = 0;
        while (j < uses.size() && uses[j].lo == uses[i].lo && uses[j].hi == uses[i].hi) forward += uses[j++].forward;
        const size_t count = j - i;
        if (forward * 2 != count) throw ValueError(where + ": the surface is not closed (an edge has unmatched triangles)");
        if (count == 2) {
            join(uses[i], uses[i + 1]);
        } else {
            third.assign(count, 0);
            ring.resize(count);
            for (size_t k = 0; k < count; ++k) {
                const Use &u = uses[i + k];
                third[k] = static_cast<int>(faces[u.face][(u.corner + 2) % 3]);
                ring[k] = static_cast<int>(k);
            }
            if (!radial_sort(predicates, static_cast<int>(uses[i].lo), static_cast<int>(uses[i].hi), third, ring))
                throw ValueError(where + ": triangles overlap one another at an edge");
            // Counter-clockwise about lo -> hi, material lies after a triangle that runs hi -> lo and
            // before one that runs lo -> hi.
            size_t start = 0;
            while (start < count && uses[i + static_cast<size_t>(ring[start])].forward) ++start;
            for (size_t k = 0; k < count; k += 2) {
                const Use &open = uses[i + static_cast<size_t>(ring[(start + k) % count])];
                const Use &close = uses[i + static_cast<size_t>(ring[(start + k + 1) % count])];
                if (open.forward || !close.forward) throw ValueError(where + ": surfaces cross one another at an edge");
                join(open, close);
            }
        }
        i = j;
    }
    // One vertex for each fan of corners.
    std::vector<uint32_t> index(faces.size() * 3, UINT32_MAX);
    v.clear();
    t.assign(faces.size(), Triangle{0, 0, 0});
    for (size_t f = 0; f < faces.size(); ++f)
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t root = corners.find(static_cast<uint32_t>(f) * 3 + k);
            if (index[root] == UINT32_MAX) {
                index[root] = static_cast<uint32_t>(v.size());
                v.push_back(points[faces[f][k]]);
            }
            t[f][k] = index[root];
        }
}

}  // namespace detail

using detail::Access;
using detail::Mesh;

Solid::Solid() {}
bool Solid::empty() const { return Access::mesh(*this).t.empty(); }
std::size_t Solid::num_vertices() const { return Access::mesh(*this).v.size(); }
std::size_t Solid::num_triangles() const { return Access::mesh(*this).t.size(); }
const std::vector<Triangle> &Solid::triangles() const { return Access::mesh(*this).t; }
const std::vector<GridPoint> &Solid::grid_vertices() const { return Access::mesh(*this).v; }

std::size_t Solid::num_bodies() const {
    const Mesh &m = Access::mesh(*this);
    detail::UnionFind groups(m.v.size());
    for (const Triangle &tri : m.t) {
        groups.unite(tri[0], tri[1]);
        groups.unite(tri[0], tri[2]);
    }
    std::size_t count = 0;
    for (uint32_t i = 0; i < m.v.size(); ++i) count += groups.find(i) == i;
    return count;
}

double Solid::volume() const {
    const Mesh &m = Access::mesh(*this);
    return detail::six_volume(m.v, m.t).to_double() / 6.0 / (kUnitsPerMm * kUnitsPerMm * kUnitsPerMm);
}

double Solid::surface_area() const {
    const Mesh &m = Access::mesh(*this);
    double total = 0;
    for (const Triangle &tri : m.t) {
        const GridPoint &a = m.v[tri[0]], &b = m.v[tri[1]], &c = m.v[tri[2]];
        const double ux = static_cast<double>(b[0] - a[0]), uy = static_cast<double>(b[1] - a[1]), uz = static_cast<double>(b[2] - a[2]);
        const double vx = static_cast<double>(c[0] - a[0]), vy = static_cast<double>(c[1] - a[1]), vz = static_cast<double>(c[2] - a[2]);
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        total += std::sqrt(nx * nx + ny * ny + nz * nz);
    }
    return total / 2.0 / (kUnitsPerMm * kUnitsPerMm);
}

Box3 Solid::bounds() const {
    const Mesh &m = Access::mesh(*this);
    return Box3{{detail::to_mm(m.lo[0]), detail::to_mm(m.lo[1]), detail::to_mm(m.lo[2])},
                {detail::to_mm(m.hi[0]), detail::to_mm(m.hi[1]), detail::to_mm(m.hi[2])}};
}

Vec3 Solid::centroid() const {
    const Mesh &m = Access::mesh(*this);
    if (m.t.empty()) return Vec3{0, 0, 0};
    // Sum over tetrahedra from an origin: volume-weighted mean of the four corners.
    const GridPoint origin = m.v[m.t[0][0]];
    detail::BigInt sum[3], volume;
    for (const Triangle &tri : m.t) {
        const GridPoint &a = m.v[tri[0]], &b = m.v[tri[1]], &c = m.v[tri[2]];
        const detail::BigInt det(detail::orient3d_value(origin.data(), a.data(), b.data(), c.data()));
        volume = volume + det;
        for (size_t k = 0; k < 3; ++k) sum[k] = sum[k] + det * detail::BigInt(a[k] + b[k] + c[k] - 3 * origin[k]);
    }
    const double v = volume.to_double();
    Vec3 out;
    double *o[3] = {&out.x, &out.y, &out.z};
    for (size_t k = 0; k < 3; ++k) *o[k] = (static_cast<double>(origin[k]) + sum[k].to_double() / (4.0 * v)) / kUnitsPerMm;
    return out;
}

std::vector<Vec3> Solid::vertices() const {
    const Mesh &m = Access::mesh(*this);
    std::vector<Vec3> out(m.v.size());
    for (size_t i = 0; i < m.v.size(); ++i) out[i] = Vec3{detail::to_mm(m.v[i][0]), detail::to_mm(m.v[i][1]), detail::to_mm(m.v[i][2])};
    return out;
}

namespace {

// The cross product of two edges of a grid triangle, as doubles (the inputs are exact).
Vec3 raw_normal(const Mesh &m, const Triangle &tri) {
    const GridPoint &a = m.v[tri[0]], &b = m.v[tri[1]], &c = m.v[tri[2]];
    const double ux = static_cast<double>(b[0] - a[0]), uy = static_cast<double>(b[1] - a[1]), uz = static_cast<double>(b[2] - a[2]);
    const double vx = static_cast<double>(c[0] - a[0]), vy = static_cast<double>(c[1] - a[1]), vz = static_cast<double>(c[2] - a[2]);
    return Vec3{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
}

Vec3 unit(const Vec3 &n) {
    const double length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return length > 0 ? Vec3{n.x / length, n.y / length, n.z / length} : Vec3{0, 0, 0};
}

}  // namespace

std::vector<Vec3> Solid::triangle_normals() const {
    const Mesh &m = Access::mesh(*this);
    std::vector<Vec3> out(m.t.size());
    for (size_t i = 0; i < m.t.size(); ++i) out[i] = unit(raw_normal(m, m.t[i]));
    return out;
}

std::vector<Vec3> Solid::vertex_normals() const {
    const Mesh &m = Access::mesh(*this);
    std::vector<Vec3> out(m.v.size(), Vec3{0, 0, 0});
    for (const Triangle &tri : m.t) {
        const Vec3 n = raw_normal(m, tri);  // length is twice the area
        for (size_t k = 0; k < 3; ++k) {
            out[tri[k]].x += n.x;
            out[tri[k]].y += n.y;
            out[tri[k]].z += n.z;
        }
    }
    for (Vec3 &n : out) n = unit(n);
    return out;
}

namespace {

// p' = M p + t in grid units. Whole-number maps are applied exactly and keep what is known about the
// mesh; others round each vertex and are checked again.
Solid apply(const Solid &solid, const double m[9], const double t_units[3], const char *context) {
    const Mesh &mesh = Access::mesh(solid);
    if (mesh.t.empty()) return solid;
    for (int i = 0; i < 9; ++i)
        if (!(std::fabs(m[i]) <= 1e9)) throw ValueError(std::string(context) + ": the matrix must be finite");
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (det == 0 || !(std::fabs(det) < 1e300)) throw ValueError(std::string(context) + ": the map must not flatten the solid");
    bool whole = true;
    for (int i = 0; i < 9; ++i) whole = whole && m[i] == std::floor(m[i]) && std::fabs(m[i]) <= 1048576.0;
    for (int i = 0; i < 3; ++i) {
        if (!(std::fabs(t_units[i]) <= 4.0 * static_cast<double>(detail::kMaxCoord))) throw ValueError(std::string(context) + ": the offset must be finite and within range");
        whole = whole && t_units[i] == std::floor(t_units[i]);
    }
    std::vector<GridPoint> v(mesh.v.size());
    const double limit = static_cast<double>(detail::kMaxCoord);
    for (size_t i = 0; i < mesh.v.size(); ++i) {
        const double x = static_cast<double>(mesh.v[i][0]), y = static_cast<double>(mesh.v[i][1]), z = static_cast<double>(mesh.v[i][2]);
        for (size_t k = 0; k < 3; ++k) {
            // Whole-number maps of grid points stay well inside 2^53, so this is exact for them.
            const double value = m[3 * k] * x + m[3 * k + 1] * y + m[3 * k + 2] * z + t_units[k];
            if (!(std::fabs(value) <= limit)) throw ValueError(std::string(context) + ": the result is outside \xC2\xB1" "100000 mm");
            v[i][k] = static_cast<int64_t>(std::round(value));
        }
    }
    if (whole) {
        // 2^20 * 2^37 * 3 < 2^53 only when the factors are small; larger whole factors are rounded like any other map.
        double largest = 0;
        for (int i = 0; i < 9; ++i) largest = std::max(largest, std::fabs(m[i]));
        whole = largest <= 8192.0;
    }
    std::vector<Triangle> t = mesh.t;
    if (det < 0)
        for (Triangle &tri : t) std::swap(tri[1], tri[2]);
    if (whole) return detail::make_solid(std::move(v), std::move(t), mesh.simple, context, true);
    if (detail::check_mesh(v, t).empty()) return detail::make_solid(std::move(v), std::move(t), false, context, true);
    // Rounding collapsed or folded something: resolve the rounded mesh against itself.
    Solid repaired = detail::resolve_self(std::move(v), std::move(t), context);
    if (repaired.empty()) throw ValueError(std::string(context) + ": nothing is left of the solid on the 1 nm grid");
    return repaired;
}

}  // namespace

Solid Solid::translated(const Vec3 &offset) const {
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double t[3] = {static_cast<double>(detail::to_grid(offset.x)), static_cast<double>(detail::to_grid(offset.y)),
                         static_cast<double>(detail::to_grid(offset.z))};
    return apply(*this, identity, t, "translate");
}

Solid Solid::rotated(double x_degrees, double y_degrees, double z_degrees) const {
    double sx, cx, sy, cy, sz, cz;
    sincos_degrees(x_degrees, sx, cx);
    sincos_degrees(y_degrees, sy, cy);
    sincos_degrees(z_degrees, sz, cz);
    // Rz * Ry * Rx.
    const double m[9] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
                         sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                         -sy,     cy * sx,                cy * cx};
    const double t[3] = {0, 0, 0};
    return apply(*this, m, t, "rotate");
}

Solid Solid::rotated_about(const Vec3 &axis, double degrees) const {
    const double length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (!(length > 0) || !(length < 1e300)) throw ValueError("rotate: the axis must have a direction");
    const double x = axis.x / length, y = axis.y / length, z = axis.z / length;
    double s, c;
    sincos_degrees(degrees, s, c);
    const double k = 1 - c;
    const double m[9] = {k * x * x + c,     k * x * y - s * z, k * x * z + s * y,
                         k * x * y + s * z, k * y * y + c,     k * y * z - s * x,
                         k * x * z - s * y, k * y * z + s * x, k * z * z + c};
    const double t[3] = {0, 0, 0};
    return apply(*this, m, t, "rotate");
}

Solid Solid::scaled(const Vec3 &f) const {
    const double m[9] = {f.x, 0, 0, 0, f.y, 0, 0, 0, f.z};
    const double t[3] = {0, 0, 0};
    return apply(*this, m, t, "scale");
}

Solid Solid::mirrored(const Vec3 &n) const {
    const double dot = n.x * n.x + n.y * n.y + n.z * n.z;
    if (!(dot > 0) || !(dot < 1e300)) throw ValueError("mirror: the normal must have a direction");
    const double m[9] = {1 - 2 * n.x * n.x / dot, -2 * n.x * n.y / dot,    -2 * n.x * n.z / dot,
                         -2 * n.y * n.x / dot,    1 - 2 * n.y * n.y / dot, -2 * n.y * n.z / dot,
                         -2 * n.z * n.x / dot,    -2 * n.z * n.y / dot,    1 - 2 * n.z * n.z / dot};
    const double t[3] = {0, 0, 0};
    return apply(*this, m, t, "mirror");
}

Solid Solid::transformed(const std::array<double, 12> &a) const {
    const double m[9] = {a[0], a[1], a[2], a[4], a[5], a[6], a[8], a[9], a[10]};
    const double t[3] = {a[3] * kUnitsPerMm, a[7] * kUnitsPerMm, a[11] * kUnitsPerMm};
    return apply(*this, m, t, "transform");
}

Solid from_grid_mesh(const std::vector<GridPoint> &vertices, const std::vector<Triangle> &triangles, bool weld) {
    if (triangles.size() > kMaxTriangles) throw ValueError("mesh: more than " + std::to_string(kMaxTriangles) + " triangles");
    for (const GridPoint &p : vertices)
        for (size_t k = 0; k < 3; ++k)
            if (p[k] > detail::kMaxCoord || p[k] < -detail::kMaxCoord) throw ValueError("mesh: a coordinate is outside \xC2\xB1" "100000 mm");
    std::vector<GridPoint> v = vertices;
    std::vector<Triangle> t = triangles;
    if (weld) detail::weld_and_pair(v, t, "mesh");
    if (t.empty()) throw ValueError("mesh: there are no triangles with an area");
    return detail::make_solid(std::move(v), std::move(t), false, "mesh");
}

Solid from_mesh(const std::vector<Vec3> &vertices, const std::vector<Triangle> &triangles, bool weld) {
    std::vector<GridPoint> v(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) v[i] = GridPoint{detail::to_grid(vertices[i].x), detail::to_grid(vertices[i].y), detail::to_grid(vertices[i].z)};
    return from_grid_mesh(v, triangles, weld);
}

}  // namespace solid
}  // namespace kerfkite
