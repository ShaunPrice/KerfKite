// KerfKite's own 3D solid modelling kernel. No dependency beyond the C++17 standard library.
//
// A Solid is a closed, outward-oriented triangle mesh whose vertices lie on a 1 nm grid. Topology is
// decided only with exact integer arithmetic; the same input gives the same vertices and triangles, in
// the same order, on every platform. docs/SOLID-KERNEL.md describes the representation, the Boolean
// algorithm, the rounding rule and the limits.
//
// Lengths are millimetres and angles degrees unless a name says otherwise. Invalid input is refused
// with kerfkite::ValueError (cam.hpp is included for that type only; the kernel does not use JSON).
#ifndef KERFKITE_SOLID_HPP
#define KERFKITE_SOLID_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kerfkite/cam.hpp"

namespace kerfkite {
namespace solid {

// Grid units in a millimetre, and the largest coordinate a solid may have.
constexpr double kUnitsPerMm = 1e6;
constexpr double kMaxCoordinateMm = 100000.0;
// Limits on size, each refused with a ValueError that names it.
constexpr std::size_t kMaxTriangles = 2000000;       // in any one solid, and in the operands of one operation
constexpr std::size_t kMaxCandidatePairs = 60000000; // triangle pairs one Boolean operation may have to examine
constexpr std::size_t kMaxProfileVertices = 200000;  // in the rings of one extrusion, revolution or projection
constexpr std::size_t kMaxHeightMapNodes = 16000000;
constexpr std::size_t kMaxStlBytes = 256u * 1024u * 1024u;

struct Vec2 {
    double x, y;
};
struct Vec3 {
    double x, y, z;
};
struct Box3 {
    Vec3 min, max;
};
using Triangle = std::array<std::uint32_t, 3>;   // vertex indices, counter-clockwise seen from outside
using GridPoint = std::array<std::int64_t, 3>;   // nanometres
using Ring = std::vector<Vec2>;                  // a closed polygon; the last point joins the first

namespace detail {
struct Mesh;
struct Access;
}  // namespace detail

// A valid solid, or the empty solid. Copies share the mesh; a Solid never changes after it is made.
// Every solid the kernel returns is closed (each edge is used by exactly two triangles, in opposite
// directions), consistently oriented outwards, free of zero-area triangles and of positive volume.
// Vertices are in ascending (x, y, z) order; triangles start at their lowest vertex and are in ascending
// order of their indices.
class Solid {
public:
    Solid();
    bool empty() const;
    std::size_t num_vertices() const;
    std::size_t num_triangles() const;
    // Connected shells. Two shells that only touch (at a point or along an edge) count separately, and a
    // cavity is a shell of its own.
    std::size_t num_bodies() const;
    double volume() const;         // mm^3, from the exact integer volume
    double surface_area() const;   // mm^2
    Box3 bounds() const;           // of the empty solid: all zero
    Vec3 centroid() const;         // of the volume
    std::vector<Vec3> vertices() const;
    const std::vector<Triangle> &triangles() const;
    const std::vector<GridPoint> &grid_vertices() const;
    std::vector<Vec3> triangle_normals() const;  // unit, outward
    std::vector<Vec3> vertex_normals() const;    // unit, the area-weighted mean of the triangles at the vertex

    // Rigid and affine maps. A translation is rounded to the grid and is then exact. A map whose matrix is
    // whole numbers (quarter turns, axis mirrors, whole-number scales) is exact too. Any other map rounds
    // each vertex to the nearest grid point.
    Solid translated(const Vec3 &offset) const;
    Solid rotated(double x_degrees, double y_degrees, double z_degrees) const;  // about X, then Y, then Z
    Solid rotated_about(const Vec3 &axis, double degrees) const;                // right-handed about the axis through the origin
    Solid scaled(const Vec3 &factors) const;                                    // each factor non-zero; a negative product turns the mesh inside out and back
    Solid mirrored(const Vec3 &normal) const;                                   // in the plane through the origin with this normal
    // p' = M p + t, rows of the 3x4 matrix [M t]; M must be invertible.
    Solid transformed(const std::array<double, 12> &matrix) const;

private:
    std::shared_ptr<const detail::Mesh> mesh_;
    friend struct detail::Access;
};

// ---- Construction ---------------------------------------------------------------------------------

// A solid from the caller's triangles. Coordinates are rounded to the grid. With weld false the indices
// must already describe a valid solid. With weld true, vertices at the same grid point are merged first,
// triangles that collapse are dropped, and where more than two triangles then meet at an edge they are
// paired by their order around it (so solids that touch along an edge or at a point are accepted).
Solid from_mesh(const std::vector<Vec3> &vertices, const std::vector<Triangle> &triangles, bool weld = false);
Solid from_grid_mesh(const std::vector<GridPoint> &vertices, const std::vector<Triangle> &triangles, bool weld = false);

// A box with one corner at the origin, or centred on it.
Solid box(const Vec3 &size, bool center = false);
// A cylinder, frustum or cone along Z from 0 to height (or centred). The cross-section is a regular
// polygon of `segments` sides inscribed in the circle, with a vertex on the +X axis. One radius may be 0.
Solid cylinder(double height, double radius_low, double radius_high, int segments, bool center = false);
// A sphere about the origin: `segments` (rounded up to a multiple of 4) around the equator and half as
// many bands from pole to pole, vertices on the sphere.
Solid sphere(double radius, int segments);

enum class FillRule { EvenOdd, NonZero, Positive };

struct ExtrudeOptions {
    int slices = 1;            // layers of side walls
    double twist_degrees = 0;  // rotation of the top relative to the bottom, counter-clockwise seen from +Z
    double scale_x = 1;        // scale of the top relative to the bottom; both 0 makes a point
    double scale_y = 1;
    FillRule fill = FillRule::EvenOdd;
};
// The prism over a 2D region, from Z = 0 to height. The region is what the fill rule makes of the rings:
// rings may nest (holes) and may cross, and crossings are resolved exactly before the grid rounding.
Solid extrude(const std::vector<Ring> &rings, double height, const ExtrudeOptions &options = ExtrudeOptions());
// The solid swept by a profile in the (radius, Z) half-plane turning about the Z axis: ring x is the
// radius (not negative), ring y is Z. The turn starts on the +X axis and runs counter-clockwise seen
// from +Z, in ceil(segments * degrees / 360) equal steps. degrees is above 0 and at most 360.
Solid revolve(const std::vector<Ring> &rings, int segments, double degrees = 360, FillRule fill = FillRule::EvenOdd);
// The convex hull of points, or of the vertices of solids.
Solid convex_hull(const std::vector<Vec3> &points);
Solid convex_hull(const std::vector<Solid> &solids);

// ---- Boolean operations ---------------------------------------------------------------------------

enum class Op { Union, Difference, Intersection };
Solid boolean(const Solid &a, const Solid &b, Op op);
// Union and Intersection of all; Difference is the first minus the union of the rest. An empty list
// gives the empty solid.
Solid batch_boolean(const std::vector<Solid> &solids, Op op);
inline Solid operator+(const Solid &a, const Solid &b) { return boolean(a, b, Op::Union); }
inline Solid operator-(const Solid &a, const Solid &b) { return boolean(a, b, Op::Difference); }
inline Solid operator^(const Solid &a, const Solid &b) { return boolean(a, b, Op::Intersection); }

// What the last Boolean operation on this thread did, for tests and tuning.
struct BooleanStats {
    std::size_t candidate_pairs = 0, intersecting_pairs = 0, split_triangles = 0, coplanar_groups = 0;
    std::size_t constructed_points = 0, exact_fallbacks = 0, rounded_vertices = 0, repair_passes = 0, coarser_retries = 0;
};
BooleanStats last_boolean_stats();

// ---- Checks ---------------------------------------------------------------------------------------

// Exact: pairs of triangles that meet anywhere other than at a common vertex or along a whole common
// edge. 0 for every solid the Boolean operations return.
std::size_t count_self_intersections(const Solid &solid);
// The complete check, by building the arrangement of the mesh with itself: true when every region of
// space is wound exactly once (inside) or not at all (outside) and no triangle crosses another.
bool verify_exact(const Solid &solid);

// ---- Sections and projection ----------------------------------------------------------------------

// The material at the horizontal plane Z = z, as closed rings: counter-clockwise around material,
// clockwise around holes. A face lying exactly in the plane counts when the material is above it
// ("lower faces are inclusive, upper faces exclusive").
std::vector<Ring> section(const Solid &solid, double z);

struct Plane3 {
    Vec3 origin, normal;  // the normal need not be a unit vector
};
struct PlaneSection {
    Vec3 origin, u, v, w;      // an orthonormal frame: u and v span the plane, w is the unit normal
    std::vector<Ring> rings;   // in (u, v) coordinates about origin, oriented as for section()
    std::vector<std::vector<Vec3>> rings3d;  // the same rings in space
};
// The material just on the normal's side of an arbitrary plane.
PlaneSection section(const Solid &solid, const Plane3 &plane);

// The outline of the solid seen along Z (its shadow on the XY plane), as rings oriented as above.
std::vector<Ring> projection(const Solid &solid);

// Rings (which may cross and nest) resolved by a fill rule into the boundary of the filled region.
// This is the 2D helper behind extrude(), revolve() and projection(); see the documentation.
std::vector<Ring> resolve_rings(const std::vector<Ring> &rings, FillRule fill);
double ring_area(const Ring &ring);  // signed, positive when counter-clockwise

// ---- Queries for machining ------------------------------------------------------------------------

struct Hit {
    bool hit = false;
    double distance = 0;        // millimetres from the origin
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 0};       // unit outward normal of the triangle hit
    std::uint32_t triangle = 0;
};
struct SurfaceSample {
    Vec3 point, normal;
    std::uint32_t triangle;
};
struct Frame {
    Vec3 origin, u, v, w;  // orthonormal; heights are measured along w
};
struct HeightMap {
    std::size_t columns = 0, rows = 0;
    double x0 = 0, y0 = 0, dx = 0, dy = 0;
    std::vector<double> z;  // rows * columns, row by row; `empty` where the solid is not above the node
    double empty = 0;
    double at(std::size_t column, std::size_t row) const { return z[row * columns + column]; }
};

// A spatial index over one solid, for many queries. It keeps the solid alive.
class Index {
public:
    explicit Index(const Solid &solid);
    ~Index();
    Index(Index &&) noexcept;
    Index &operator=(Index &&) noexcept;
    // The nearest triangle hit by the ray origin + t * direction, 0 <= t <= max_distance / |direction|.
    // Hit::distance is in millimetres. A ray that passes exactly through an edge or a vertex of the mesh
    // hits one of the triangles there: rays do not leak between triangles.
    Hit raycast(const Vec3 &origin, const Vec3 &direction, double max_distance = 1e300) const;
    // True when anything lies on the segment from `from` towards `direction` over `distance` mm, starting
    // `clearance` mm along it: the test for whether a tool axis reaches a surface point.
    bool occluded(const Vec3 &from, const Vec3 &direction, double distance = 1e300, double clearance = 0) const;
    // Whether the point is inside the solid (by counting crossings of a ray; a point on the surface may
    // give either answer).
    bool contains(const Vec3 &point) const;
    const Solid &solid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The highest point of the surface above each node (x0 + i*dx, y0 + j*dy) of a regular grid, or `empty`.
HeightMap height_map(const Solid &solid, double x0, double y0, double dx, double dy, std::size_t columns, std::size_t rows,
                     double empty);
// The same seen along another tool axis: node (i, j) is origin + (x0 + i*dx) u + (y0 + j*dy) v, and the
// height is the largest w coordinate of the surface on the line through the node along w.
HeightMap height_map(const Solid &solid, const Frame &frame, double x0, double y0, double dx, double dy,
                     std::size_t columns, std::size_t rows, double empty);
// Points spread over the surface about `spacing` mm apart, with the normal of their triangle, in a
// defined order (triangle by triangle).
std::vector<SurfaceSample> sample_surface(const Solid &solid, double spacing, std::size_t max_samples = 2000000);
// An orthonormal frame whose w is the given direction.
Frame frame_for_axis(const Vec3 &origin, const Vec3 &axis);
// The smallest distance between the surfaces of two solids if it is below search_length, otherwise
// search_length. 0 when they touch or cross.
double min_gap(const Solid &a, const Solid &b, double search_length);

// ---- STL ------------------------------------------------------------------------------------------

struct StlTriangle {
    Vec3 a, b, c;
};
// The triangles of a binary or ASCII STL file, as they stand in the file. Refused when malformed, too
// large, or holding a number that is not finite.
std::vector<StlTriangle> read_stl(const std::string &bytes, std::size_t max_triangles = kMaxTriangles);
// The solid an STL file describes: its triangles welded on the grid (see from_mesh with weld true).
Solid from_stl(const std::string &bytes, std::size_t max_triangles = kMaxTriangles);
// Binary STL in millimetres. The 80-byte header is the text, padded with zero bytes.
std::string to_stl_binary(const Solid &solid, const std::string &header = "KerfKite Studio solid, mm");
std::string to_stl_ascii(const Solid &solid, const std::string &name = "kerfkite");

// ---- Deterministic trigonometry ---------------------------------------------------------------------

// Sine and cosine of an angle in degrees, built only from correctly rounded additions and
// multiplications, so the result is the same double on every platform. Exact at multiples of 90 degrees;
// the error elsewhere is below 4e-16.
void sincos_degrees(double degrees, double &sine, double &cosine);
// The same for the angle 360 * k / n, with exactly symmetric results around the circle.
void sincos_turn(std::int64_t k, std::int64_t n, double &sine, double &cosine);

}  // namespace solid
}  // namespace kerfkite

#endif
