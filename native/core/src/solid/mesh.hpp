// The mesh behind a Solid, and the routines every part of the kernel uses to make one.
#ifndef KERFKITE_SOLID_MESH_HPP
#define KERFKITE_SOLID_MESH_HPP

#include <string>
#include <vector>

#include "exact.hpp"
#include "kerfkite/solid.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

struct Mesh {
    std::vector<GridPoint> v;
    std::vector<Triangle> t;
    int64_t lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    // Known to be free of self-intersections. Set where that was established exactly; a Boolean
    // operation establishes it for an operand that lacks it and records the answer here.
    mutable bool simple = false;
};

struct Access {
    static const Mesh &mesh(const Solid &solid);
    static Solid wrap(std::shared_ptr<const Mesh> mesh);
};

// Millimetres to grid units, refusing values that are not finite or outside the coordinate limit.
int64_t to_grid(double mm);
inline double to_mm(int64_t units) { return static_cast<double>(units) / kUnitsPerMm; }

// Why the mesh is not a valid solid, or an empty string.
std::string check_mesh(const std::vector<GridPoint> &v, const std::vector<Triangle> &t);

// A Solid from grid data: unused vertices dropped, canonical order, and (unless trusted) the validity
// check, refused with ValueError(context + reason).
Solid make_solid(std::vector<GridPoint> v, std::vector<Triangle> t, bool simple, const char *context, bool trusted = false);

// Merge vertices at the same grid point, drop collapsed triangles and cancelling pairs, then pair the
// triangles at every edge by their order around it, giving each fan of triangles its own vertex.
// Refused with ValueError when the triangles do not close or overlap one another at an edge.
void weld_and_pair(std::vector<GridPoint> &v, std::vector<Triangle> &t, const char *context);

// Six times the signed volume, exactly, in cubic grid units.
BigInt six_volume(const std::vector<GridPoint> &v, const std::vector<Triangle> &t);

// The arrangement of a mesh with itself, keeping what is wound a positive number of times: the repair
// for a mesh that rounding has left with a defect. Implemented with the Boolean operations.
Solid resolve_self(std::vector<GridPoint> v, std::vector<Triangle> t, const char *context);

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
