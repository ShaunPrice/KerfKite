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
    // Makes the object ready for another plane of the same pool. Its lists are emptied and keep their
    // memory, so that one object can serve the thousands of small triangulations of an operation.
    void reset(const Plane &carrier, int axis_u, int axis_v, int sign);

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
    // For tests: every turn asked of the pool, without the shorter ways of o().
    bool plain = false;

    // What the triangulation keeps of each point for its turns: two coordinates as doubles with the most
    // each can be wrong by (nothing for a whole coordinate), and whether the point is on the grid.
    struct Local {
        double u, v, ru, rv;
        bool grid;
    };
    static Local local_of(const Interval &u, const Interval &v, bool grid);
    // The sign of the turn a b c from those doubles when they suffice to be certain of it, otherwise 2.
    static int filtered_turn(const Local &a, const Local &b, const Local &c);
    // Whether d lies inside the circle through a, b, c, from the intervals of the two coordinates of each:
    // 1 when certainly inside, -1 when certainly outside, 2 when the intervals cannot tell (which includes
    // every point on the circle). Taken relative to the turn of a b c, so the order of a, b, c does not matter.
    static int filtered_in_circle(const Interval a[2], const Interval b[2], const Interval c[2], const Interval d[2]);
    // The cell of a point on a Hilbert curve over a square grid of 2^16 by 2^16 cells.
    static uint64_t hilbert_index(uint32_t x, uint32_t y);

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
    std::vector<Local> local_;  // by local vertex
    std::vector<std::array<Interval, 2>> iv_;  // by local vertex: the intervals of its two coordinates
    // The segments each point is known to lie on the line of: split() notes them by pool id, triangulate()
    // files them by local vertex (the segments of vertex k are line_ids_[line_start_[k]] up to
    // line_ids_[line_start_[k + 1]], ascending).
    std::vector<std::pair<int, int>> on_line_;
    std::vector<int> line_start_, line_ids_;
    // Working lists, kept between calls.
    struct Bounds {
        double ulo, uhi, vlo, vhi;
    };
    std::vector<Bounds> box_;
    std::vector<std::pair<int, int>> on_, filed_, wanted_, todo_, crossed_, flip_stack_;  // on_: (segment, point on it)
    std::vector<int> order_, created_, ends_, list_, rest_, ids_, fan_;

    bool between(int a, int b, int p) const;
    int o(int a, int b, int c) const;
    bool one_line(int a, int b, int c) const;
    void relink(int neighbour, int from, int to);
    void insert(int p);
    int in_circle(int a, int b, int c, int d) const;  // filtered_in_circle() of four local vertices
    void legalize(int p);
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
