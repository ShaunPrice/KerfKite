// Queries for machining: ray casting, height maps, surface sampling and the gap between two solids.
//
// These are measurements, computed in doubles from the exact grid coordinates. They take no part in
// deciding the topology of any solid. Ray and height-map tests use the same edge functions for the two
// triangles that share an edge, so a ray through an edge or a vertex always hits one of them.
#include <algorithm>
#include <cmath>

#include "bvh.hpp"
#include "mesh.hpp"

namespace kerfkite {
namespace solid {

namespace {

using detail::Box;
using detail::Bvh;
using detail::Mesh;

Bvh build_tree(const Mesh &m, std::vector<Box> &boxes) {
    boxes.resize(m.t.size());
    for (size_t i = 0; i < m.t.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            const size_t c = static_cast<size_t>(k);
            boxes[i].lo[k] = std::min(m.v[m.t[i][0]][c], std::min(m.v[m.t[i][1]][c], m.v[m.t[i][2]][c]));
            boxes[i].hi[k] = std::max(m.v[m.t[i][0]][c], std::max(m.v[m.t[i][1]][c], m.v[m.t[i][2]][c]));
        }
    }
    Bvh tree;
    tree.build(boxes);
    return tree;
}

Vec3 unit_normal(const Mesh &m, uint32_t tri) {
    const GridPoint &a = m.v[m.t[tri][0]], &b = m.v[m.t[tri][1]], &c = m.v[m.t[tri][2]];
    const double ux = static_cast<double>(b[0] - a[0]), uy = static_cast<double>(b[1] - a[1]), uz = static_cast<double>(b[2] - a[2]);
    const double vx = static_cast<double>(c[0] - a[0]), vy = static_cast<double>(c[1] - a[1]), vz = static_cast<double>(c[2] - a[2]);
    const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
    return Vec3{nx / length + 0.0, ny / length + 0.0, nz / length + 0.0};  // adding zero leaves no negative zero
}

// A ray prepared for the triangle test of Woop, Benthin and Wald ("Watertight ray/triangle
// intersection", 2013): the dominant axis becomes Z and the ray is sheared onto it.
struct Ray {
    double org[3], dir[3];
    int kx, ky, kz;
    double sx, sy, sz;
    void prepare() {
        kz = std::fabs(dir[0]) > std::fabs(dir[1]) ? (std::fabs(dir[0]) > std::fabs(dir[2]) ? 0 : 2) : (std::fabs(dir[1]) > std::fabs(dir[2]) ? 1 : 2);
        kx = (kz + 1) % 3;
        ky = (kx + 1) % 3;
        if (dir[kz] < 0) std::swap(kx, ky);
        sx = dir[kx] / dir[kz];
        sy = dir[ky] / dir[kz];
        sz = 1.0 / dir[kz];
    }
    // The parameter of the hit, or a negative number. `edge` is set when the ray passes exactly through an
    // edge or a vertex of the triangle.
    double hit(const Mesh &m, uint32_t tri, bool &edge) const {
        double p[3][3];
        for (size_t c = 0; c < 3; ++c)
            for (size_t k = 0; k < 3; ++k) p[c][k] = static_cast<double>(m.v[m.t[tri][c]][k]) - org[k];
        const double ax = p[0][kx] - sx * p[0][kz], ay = p[0][ky] - sy * p[0][kz];
        const double bx = p[1][kx] - sx * p[1][kz], by = p[1][ky] - sy * p[1][kz];
        const double cx = p[2][kx] - sx * p[2][kz], cy = p[2][ky] - sy * p[2][kz];
        const double u = cx * by - cy * bx, v = ax * cy - ay * cx, w = bx * ay - by * ax;
        if ((u < 0 || v < 0 || w < 0) && (u > 0 || v > 0 || w > 0)) return -1;
        const double det = u + v + w;
        if (det == 0) return -1;
        edge = u == 0 || v == 0 || w == 0;
        const double t = (u * sz * p[0][kz] + v * sz * p[1][kz] + w * sz * p[2][kz]) / det;
        return t >= 0 ? t : -1;
    }
    // Whether the ray can reach the box before `limit` (boxes are widened by one unit).
    bool reaches(const Box &b, double limit) const {
        double t0 = 0, t1 = limit;
        for (int k = 0; k < 3; ++k) {
            const double lo = static_cast<double>(b.lo[k]) - 1 - org[k], hi = static_cast<double>(b.hi[k]) + 1 - org[k];
            if (dir[k] == 0) {
                if (lo > 0 || hi < 0) return false;
                continue;
            }
            double near = lo / dir[k], far = hi / dir[k];
            if (near > far) std::swap(near, far);
            if (near > t0) t0 = near;
            if (far < t1) t1 = far;
        }
        return t0 <= t1 * 1.0000000000000004 + 1e-9;
    }
};

Vec3 normalised(const Vec3 &v, const char *what) {
    const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 0) || !(length < 1e150)) throw ValueError(std::string(what) + " must have a direction");
    return Vec3{v.x / length, v.y / length, v.z / length};
}

}  // namespace

struct Index::Impl {
    Solid solid;
    std::vector<Box> boxes;
    Bvh tree;

    // Calls visit(triangle, t, on_edge) for hits in order of the tree; visit returns the new limit.
    template <class F>
    void walk(const Ray &ray, double limit, F &&visit) const {
        if (tree.nodes.empty()) return;
        const Mesh &m = detail::Access::mesh(solid);
        int stack[96];
        int top = 0;
        stack[top++] = 0;
        while (top) {
            const Bvh::Node &node = tree.nodes[static_cast<size_t>(stack[--top])];
            if (!ray.reaches(node.box, limit)) continue;
            if (node.left < 0) {
                for (int i = 0; i < node.count; ++i) {
                    const uint32_t tri = static_cast<uint32_t>(tree.items[static_cast<size_t>(node.first + i)]);
                    bool edge = false;
                    const double t = ray.hit(m, tri, edge);
                    if (t >= 0 && t <= limit) limit = visit(tri, t, edge, limit);
                }
            } else {
                stack[top++] = node.left;
                stack[top++] = node.right;
            }
        }
    }
};

Index::Index(const Solid &solid) : impl_(new Impl) {
    impl_->solid = solid;
    impl_->tree = build_tree(detail::Access::mesh(solid), impl_->boxes);
}
Index::~Index() = default;
Index::Index(Index &&) noexcept = default;
Index &Index::operator=(Index &&) noexcept = default;
const Solid &Index::solid() const { return impl_->solid; }

Hit Index::raycast(const Vec3 &origin, const Vec3 &direction, double max_distance) const {
    const Mesh &m = detail::Access::mesh(impl_->solid);
    const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!(length > 0) || !(length < 1e150)) throw ValueError("raycast: the direction must have a length");
    if (!(std::fabs(origin.x) < 1e12) || !(std::fabs(origin.y) < 1e12) || !(std::fabs(origin.z) < 1e12)) throw ValueError("raycast: the origin must be finite");
    Ray ray;
    ray.org[0] = origin.x * kUnitsPerMm;
    ray.org[1] = origin.y * kUnitsPerMm;
    ray.org[2] = origin.z * kUnitsPerMm;
    ray.dir[0] = direction.x / length;
    ray.dir[1] = direction.y / length;
    ray.dir[2] = direction.z / length;
    ray.prepare();
    Hit best;
    double best_t = -1;
    const double limit = max_distance >= 1e300 / kUnitsPerMm ? 1e300 : max_distance * kUnitsPerMm;
    impl_->walk(ray, limit, [&](uint32_t tri, double t, bool, double current) {
        // The nearest hit; of two at the same distance, the triangle with the lower index.
        if (best_t < 0 || t < best_t || (t == best_t && tri < best.triangle)) {
            best_t = t;
            best.triangle = tri;
        }
        return std::min(current, t);
    });
    if (best_t < 0) return best;
    best.hit = true;
    best.distance = best_t / kUnitsPerMm;
    best.point = Vec3{origin.x + ray.dir[0] * best.distance, origin.y + ray.dir[1] * best.distance, origin.z + ray.dir[2] * best.distance};
    best.normal = unit_normal(m, best.triangle);
    return best;
}

bool Index::occluded(const Vec3 &from, const Vec3 &direction, double distance, double clearance) const {
    const Vec3 d = normalised(direction, "occluded: the direction");
    const Vec3 start{from.x + d.x * clearance, from.y + d.y * clearance, from.z + d.z * clearance};
    return raycast(start, d, distance >= 1e290 ? 1e300 : distance - clearance).hit;
}

bool Index::contains(const Vec3 &point) const {
    // Crossings of a ray are counted; a ray through an edge or a vertex is ambiguous, so another is tried.
    static const double directions[][3] = {{0.5773, 0.5774, 0.5775}, {-0.2011, 0.6397, 0.7418}, {0.9041, -0.3011, 0.3033}, {-0.4203, -0.8117, 0.4057},
                                           {0.1301, 0.2207, -0.9666}, {0.7001, 0.1009, -0.7069}, {-0.8807, 0.4703, -0.0557}, {0.3301, -0.9409, -0.0757}};
    for (const double *g : directions) {
        Ray ray;
        ray.org[0] = point.x * kUnitsPerMm;
        ray.org[1] = point.y * kUnitsPerMm;
        ray.org[2] = point.z * kUnitsPerMm;
        ray.dir[0] = g[0];
        ray.dir[1] = g[1];
        ray.dir[2] = g[2];
        ray.prepare();
        size_t crossings = 0;
        bool ambiguous = false;
        impl_->walk(ray, 1e300, [&](uint32_t, double, bool edge, double current) {
            ambiguous = ambiguous || edge;
            ++crossings;
            return current;
        });
        if (!ambiguous) return crossings % 2 == 1;
    }
    return false;
}

Frame frame_for_axis(const Vec3 &origin, const Vec3 &axis) {
    Frame f;
    f.origin = origin;
    f.w = normalised(axis, "The tool axis");
    const double ax = std::fabs(f.w.x), ay = std::fabs(f.w.y), az = std::fabs(f.w.z);
    const Vec3 e = ax <= ay && ax <= az ? Vec3{1, 0, 0} : ay <= az ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    f.v = normalised(Vec3{f.w.y * e.z - f.w.z * e.y, f.w.z * e.x - f.w.x * e.z, f.w.x * e.y - f.w.y * e.x}, "The tool axis");
    f.u = Vec3{f.v.y * f.w.z - f.v.z * f.w.y, f.v.z * f.w.x - f.v.x * f.w.z, f.v.x * f.w.y - f.v.y * f.w.x};
    return f;
}

namespace {

// Rasterises every triangle onto the nodes it covers. px, py, pz give each vertex in the map's frame, in mm.
HeightMap rasterise(const Mesh &m, const std::vector<double> &px, const std::vector<double> &py, const std::vector<double> &pz, double x0, double y0, double dx,
                    double dy, std::size_t columns, std::size_t rows, double empty) {
    if (!(dx > 0) || !(dy > 0) || !(dx < 1e9) || !(dy < 1e9) || !(std::fabs(x0) < 1e9) || !(std::fabs(y0) < 1e9)) throw ValueError("height map: the grid origin and spacing must be finite, and the spacing positive");
    if (columns == 0 || rows == 0 || columns > kMaxHeightMapNodes || rows > kMaxHeightMapNodes || columns * rows > kMaxHeightMapNodes)
        throw ValueError("height map: between 1 and " + std::to_string(kMaxHeightMapNodes) + " nodes");
    HeightMap map;
    map.columns = columns;
    map.rows = rows;
    map.x0 = x0;
    map.y0 = y0;
    map.dx = dx;
    map.dy = dy;
    map.empty = empty;
    map.z.assign(columns * rows, empty);
    std::vector<char> set(columns * rows, 0);
    for (const Triangle &t : m.t) {
        const double ax = px[t[0]], ay = py[t[0]], bx = px[t[1]], by = py[t[1]], cx = px[t[2]], cy = py[t[2]];
        const double xmin = std::min(ax, std::min(bx, cx)), xmax = std::max(ax, std::max(bx, cx));
        const double ymin = std::min(ay, std::min(by, cy)), ymax = std::max(ay, std::max(by, cy));
        const double i0 = std::ceil((xmin - x0) / dx - 1e-9), i1 = std::floor((xmax - x0) / dx + 1e-9);
        const double j0 = std::ceil((ymin - y0) / dy - 1e-9), j1 = std::floor((ymax - y0) / dy + 1e-9);
        if (i1 < 0 || j1 < 0 || i0 > static_cast<double>(columns - 1) || j0 > static_cast<double>(rows - 1)) continue;
        const std::size_t ia = static_cast<std::size_t>(std::max(0.0, i0)), ib = static_cast<std::size_t>(std::min(static_cast<double>(columns - 1), i1));
        const std::size_t ja = static_cast<std::size_t>(std::max(0.0, j0)), jb = static_cast<std::size_t>(std::min(static_cast<double>(rows - 1), j1));
        for (std::size_t j = ja; j <= jb; ++j) {
            const double y = y0 + static_cast<double>(j) * dy;
            for (std::size_t i = ia; i <= ib; ++i) {
                const double x = x0 + static_cast<double>(i) * dx;
                // The same products, in the opposite order, for the triangle on the other side of each edge.
                const double u = (cx - x) * (by - y) - (cy - y) * (bx - x), v = (ax - x) * (cy - y) - (ay - y) * (cx - x), w = (bx - x) * (ay - y) - (by - y) * (ax - x);
                if ((u < 0 || v < 0 || w < 0) && (u > 0 || v > 0 || w > 0)) continue;
                const double det = u + v + w;
                if (det == 0) continue;  // seen edge-on
                const double z = (u * pz[t[0]] + v * pz[t[1]] + w * pz[t[2]]) / det;
                const std::size_t at = j * columns + i;
                if (!set[at] || z > map.z[at]) {
                    map.z[at] = z;
                    set[at] = 1;
                }
            }
        }
    }
    return map;
}

}  // namespace

HeightMap height_map(const Solid &solid, double x0, double y0, double dx, double dy, std::size_t columns, std::size_t rows, double empty) {
    const Mesh &m = detail::Access::mesh(solid);
    std::vector<double> px(m.v.size()), py(m.v.size()), pz(m.v.size());
    for (size_t i = 0; i < m.v.size(); ++i) {
        px[i] = detail::to_mm(m.v[i][0]);
        py[i] = detail::to_mm(m.v[i][1]);
        pz[i] = detail::to_mm(m.v[i][2]);
    }
    return rasterise(m, px, py, pz, x0, y0, dx, dy, columns, rows, empty);
}

HeightMap height_map(const Solid &solid, const Frame &frame, double x0, double y0, double dx, double dy, std::size_t columns, std::size_t rows, double empty) {
    const Mesh &m = detail::Access::mesh(solid);
    std::vector<double> px(m.v.size()), py(m.v.size()), pz(m.v.size());
    for (size_t i = 0; i < m.v.size(); ++i) {
        const double x = detail::to_mm(m.v[i][0]) - frame.origin.x, y = detail::to_mm(m.v[i][1]) - frame.origin.y, z = detail::to_mm(m.v[i][2]) - frame.origin.z;
        px[i] = x * frame.u.x + y * frame.u.y + z * frame.u.z;
        py[i] = x * frame.v.x + y * frame.v.y + z * frame.v.z;
        pz[i] = x * frame.w.x + y * frame.w.y + z * frame.w.z;
    }
    return rasterise(m, px, py, pz, x0, y0, dx, dy, columns, rows, empty);
}

std::vector<SurfaceSample> sample_surface(const Solid &solid, double spacing, std::size_t max_samples) {
    if (!(spacing > 0)) throw ValueError("sample: the spacing must be positive");
    const Mesh &m = detail::Access::mesh(solid);
    std::vector<SurfaceSample> out;
    for (uint32_t i = 0; i < m.t.size(); ++i) {
        const GridPoint &ga = m.v[m.t[i][0]], &gb = m.v[m.t[i][1]], &gc = m.v[m.t[i][2]];
        const Vec3 a{detail::to_mm(ga[0]), detail::to_mm(ga[1]), detail::to_mm(ga[2])}, b{detail::to_mm(gb[0]), detail::to_mm(gb[1]), detail::to_mm(gb[2])},
            c{detail::to_mm(gc[0]), detail::to_mm(gc[1]), detail::to_mm(gc[2])};
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double twice = std::sqrt(nx * nx + ny * ny + nz * nz);
        // k * k equal pieces, each about spacing^2 / 2 in area; one sample at the centre of each.
        const double wanted = std::ceil(std::sqrt(twice) / spacing);
        const std::size_t k = wanted < 1 ? 1 : wanted > 4096 ? 4096 : static_cast<std::size_t>(wanted);
        if (out.size() + k * k > max_samples) throw ValueError("sample: more than " + std::to_string(max_samples) + " samples; use a larger spacing");
        const Vec3 normal{nx / twice, ny / twice, nz / twice};
        const double step = 1.0 / static_cast<double>(k);
        for (std::size_t p = 0; p < k; ++p)
            for (std::size_t q = 0; p + q < k; ++q) {
                for (int upper = 0; upper < 2; ++upper) {
                    if (upper && p + q + 1 >= k) break;
                    const double s = (static_cast<double>(p) + (upper ? 2.0 : 1.0) / 3.0) * step, t = (static_cast<double>(q) + (upper ? 2.0 : 1.0) / 3.0) * step;
                    out.push_back(SurfaceSample{Vec3{a.x + s * ux + t * vx, a.y + s * uy + t * vy, a.z + s * uz + t * vz}, normal, i});
                }
            }
    }
    return out;
}

namespace {

struct P3 {
    double x, y, z;
};
P3 sub(const P3 &a, const P3 &b) { return P3{a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(const P3 &a, const P3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
P3 cross(const P3 &a, const P3 &b) { return P3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
P3 along(const P3 &a, const P3 &d, double t) { return P3{a.x + d.x * t, a.y + d.y * t, a.z + d.z * t}; }
double clamp01(double t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

// Squared distance between segments p1-q1 and p2-q2 (Ericson, "Real-Time Collision Detection", 5.1.9).
double segment_segment2(const P3 &p1, const P3 &q1, const P3 &p2, const P3 &q2) {
    const P3 d1 = sub(q1, p1), d2 = sub(q2, p2), r = sub(p1, p2);
    const double a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    double s, t;
    if (a <= 0 && e <= 0) return dot(r, r);
    if (a <= 0) {
        s = 0;
        t = clamp01(f / e);
    } else {
        const double c = dot(d1, r);
        if (e <= 0) {
            t = 0;
            s = clamp01(-c / a);
        } else {
            const double b = dot(d1, d2), denom = a * e - b * b;
            s = denom > 0 ? clamp01((b * f - c * e) / denom) : 0;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = clamp01(-c / a);
            } else if (t > 1) {
                t = 1;
                s = clamp01((b - c) / a);
            }
        }
    }
    const P3 d = sub(along(p1, d1, s), along(p2, d2, t));
    return dot(d, d);
}

// Squared distance from p to triangle a b c (the closest point by Voronoi regions, as in Ericson 5.1.5).
double point_triangle2(const P3 &p, const P3 &a, const P3 &b, const P3 &c) {
    const P3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return dot(ap, ap);
    const P3 bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return dot(bp, bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const P3 d = sub(p, along(a, ab, d1 / (d1 - d3)));
        return dot(d, d);
    }
    const P3 cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return dot(cp, cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const P3 d = sub(p, along(a, ac, d2 / (d2 - d6)));
        return dot(d, d);
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const P3 d = sub(p, along(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
        return dot(d, d);
    }
    const P3 n = cross(ab, ac);
    const double h = dot(n, ap);
    return h * h / dot(n, n);
}

// Whether the segment p-q passes through the triangle.
bool segment_through(const P3 &p, const P3 &q, const P3 &a, const P3 &b, const P3 &c) {
    const P3 n = cross(sub(b, a), sub(c, a));
    const double hp = dot(n, sub(p, a)), hq = dot(n, sub(q, a));
    if ((hp > 0 && hq > 0) || (hp < 0 && hq < 0) || hp == hq) return false;
    const P3 x = along(p, sub(q, p), hp / (hp - hq));
    const double u = dot(n, cross(sub(b, a), sub(x, a))), v = dot(n, cross(sub(c, b), sub(x, b))), w = dot(n, cross(sub(a, c), sub(x, c)));
    return u >= 0 && v >= 0 && w >= 0;
}

double triangle_triangle2(const P3 a[3], const P3 b[3]) {
    double best = 1e300;
    for (int i = 0; i < 3; ++i) {
        if (segment_through(a[i], a[(i + 1) % 3], b[0], b[1], b[2]) || segment_through(b[i], b[(i + 1) % 3], a[0], a[1], a[2])) return 0;
        best = std::min(best, std::min(point_triangle2(a[i], b[0], b[1], b[2]), point_triangle2(b[i], a[0], a[1], a[2])));
        for (int j = 0; j < 3; ++j) best = std::min(best, segment_segment2(a[i], a[(i + 1) % 3], b[j], b[(j + 1) % 3]));
    }
    return best;
}

}  // namespace

double min_gap(const Solid &a, const Solid &b, double search_length) {
    if (!(search_length >= 0) || !(search_length < 1e9)) throw ValueError("min_gap: the search length must be zero or more");
    const Mesh &ma = detail::Access::mesh(a), &mb = detail::Access::mesh(b);
    if (ma.t.empty() || mb.t.empty()) return search_length;
    std::vector<Box> boxes;
    const Bvh tree = build_tree(mb, boxes);
    const int64_t reach = static_cast<int64_t>(std::ceil(search_length * kUnitsPerMm)) + 1;
    double best = search_length * kUnitsPerMm;
    best *= best;
    const auto corner = [](const Mesh &m, uint32_t tri, size_t k) {
        const GridPoint &g = m.v[m.t[tri][k]];
        return P3{static_cast<double>(g[0]), static_cast<double>(g[1]), static_cast<double>(g[2])};
    };
    for (uint32_t i = 0; i < ma.t.size() && best > 0; ++i) {
        Box box;
        for (int k = 0; k < 3; ++k) {
            const size_t c = static_cast<size_t>(k);
            box.lo[k] = std::min(ma.v[ma.t[i][0]][c], std::min(ma.v[ma.t[i][1]][c], ma.v[ma.t[i][2]][c])) - reach;
            box.hi[k] = std::max(ma.v[ma.t[i][0]][c], std::max(ma.v[ma.t[i][1]][c], ma.v[ma.t[i][2]][c])) + reach;
        }
        const P3 ta[3] = {corner(ma, i, 0), corner(ma, i, 1), corner(ma, i, 2)};
        tree.query(boxes, box, [&](int j) {
            const P3 tb[3] = {corner(mb, static_cast<uint32_t>(j), 0), corner(mb, static_cast<uint32_t>(j), 1), corner(mb, static_cast<uint32_t>(j), 2)};
            best = std::min(best, triangle_triangle2(ta, tb));
        });
    }
    return std::sqrt(best) / kUnitsPerMm;
}

}  // namespace solid
}  // namespace kerfkite
