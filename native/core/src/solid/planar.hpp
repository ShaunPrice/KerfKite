// A planar straight-line arrangement inside one plane of space: segments are split where they cross
// or overlap, and the result is triangulated with every piece as an edge. All decisions are exact.
//
// This serves two purposes: subdividing the triangles of a Boolean operation along their intersections,
// and the 2D work the kernel needs for itself (regions for extrusion and revolution, the projection).
#ifndef KERFKITE_SOLID_PLANAR_HPP
#define KERFKITE_SOLID_PLANAR_HPP

#include <array>
#include <utility>
#include <vector>

#include "exact.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

struct PlanarSegment {
    int a, b;     // points of the pool
    int support;  // index in Planar::supports of a plane that cuts the carrier plane along this segment's line
    int source;   // the caller's own reference, copied to every piece
};

class Planar {
public:
    // The carrier plane, the two coordinate axes it is viewed in, and +1 or -1 so that orient() is
    // positive for counter-clockwise turns seen from the side the caller chose.
    Planar(Pool &pool, const Plane &carrier, int axis_u, int axis_v, int sign);

    std::vector<Plane> supports;
    std::vector<int> points;              // pool ids; duplicates are allowed
    std::vector<PlanarSegment> segments;  // may cross and overlap

    // Splits the segments so that no two pieces meet except at end points; crossings become new points.
    void split();
    std::vector<PlanarSegment> pieces;    // filled by split(), in the direction of their source

    // Triangulates all points inside the counter-clockwise triangle r0 r1 r2 (which must contain them),
    // with every piece as an edge.
    void triangulate(int r0, int r1, int r2);
    // The same inside a triangle built around the points; triangles touching root[] lie outside them.
    void triangulate_around();
    int root[3];
    std::vector<std::array<int, 3>> triangles;   // pool ids, counter-clockwise
    std::vector<std::array<int, 3>> neighbours;  // across edge (v[i], v[i+1]); -1 on the outside

    int orient(int a, int b, int c) const { return sign_ * pool_.orient2d(a, b, c, au_, av_); }

private:
    struct Tri {
        int v[3], n[3];
    };
    Pool &pool_;
    Plane carrier_;
    int au_, av_, sign_;
    std::vector<Tri> tris_;
    std::vector<int> pid_;   // local vertex -> pool id
    std::vector<int> vtri_;  // local vertex -> one triangle that has it
    int last_ = 0;

    bool between(int a, int b, int p) const;
    int o(int a, int b, int c) const { return orient(pid_[static_cast<size_t>(a)], pid_[static_cast<size_t>(b)], pid_[static_cast<size_t>(c)]); }
    void relink(int neighbour, int from, int to);
    void insert(int p);
    void split_face(int t, int p);
    void split_edge(int t, int i, int p);
    void flip(int t, int i);
    bool find_edge(int x, int y, int &t, int &i) const;
    void around(int x, std::vector<int> &out) const;
    void enforce(int a, int b);
    void improve(const std::vector<std::pair<int, int>> &required);
};

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
