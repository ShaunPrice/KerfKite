// The arrangement of triangle meshes: the engine behind the Boolean operations, the repair of a mesh
// that rounding has damaged, and the exact self-intersection checks.
#ifndef KERFKITE_SOLID_ARRANGEMENT_HPP
#define KERFKITE_SOLID_ARRANGEMENT_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "bvh.hpp"
#include "exact.hpp"
#include "mesh.hpp"

namespace kerfkite {
namespace solid {
namespace detail {

// How coarsely features too fine for the grid are removed: 0 normally. An operation whose rounded result
// cannot be made valid is tried again at 1, 2 and 3, each doubling the sizes below.
int coarseness();
struct TooFine : ValueError {
    using ValueError::ValueError;
};

// For tests: true while the kernel is to take none of its shorter ways (points left pending, remembered
// crossings, double filters, known planes and lines, probing verification) and compute everything in
// full instead. Every result must be the same, to the last vertex and count, with it on or off; the
// self-test compares them. It is thread-local and off unless a test turns it on.
bool &plain_paths();

// Which regions of space are solid, given how many times each operand's surface winds around them.
enum class Rule { Union, Intersection, Difference, Positive };

struct Operand {
    const std::vector<GridPoint> *v;
    const std::vector<Triangle> *t;
    int label;         // 0 or 1: the winding count this operand adds to
    bool simple;       // known to be free of self-intersections
    const Mesh *mesh;  // where to record that, or null
};

// The result before rounding: vertices are points of the pool. `plane` names, for each triangle, a
// triangle of the arrangement whose plane it lies in; it may be left empty.
class Planar;

struct ExactMesh {
    std::vector<int> vertex;
    std::vector<Triangle> tri;
    std::vector<int> plane;
};

class Arrangement {
public:
    Pool pool;
    BooleanStats stats;

    // Loads the operands. Triangles without area (possible only in a mesh being repaired) are left out.
    void load(const std::vector<Operand> &operands);
    // Examines every pair of triangles that may meet. With only_flagged, a pair is examined only when one
    // of its triangles is flagged. Returns how many pairs meet other than at a common corner or edge.
    std::size_t intersect(const std::vector<char> *flagged = nullptr, bool stop_at_first = false);
    // Subdivides, labels and extracts the boundary of the solid region.
    void resolve(Rule rule, ExactMesh &out);
    // Lets intersect() add separating ribbons where faces nearly coincide (see arrangement.cpp). Off for
    // the checks, which must see the mesh as it is.
    void enable_ribbons() { ribbons_enabled_ = true; }
    // For a question that only asks whether any pair meets: intersect() must then be told to stop at the
    // first that does. The points where edges cross planes are then made for one pair and dropped after
    // it, without being looked up among the points the arrangement has (see arrangement.cpp). Nothing
    // can be resolved from an arrangement used this way.
    void probe_only() { probe_only_ = !plain_; }
    // After resolve() with Rule::Positive on one operand: every face separates winding 0 from winding 1.
    bool unit_winding() const { return unit_winding_; }
    // After resolve() on two operands that are valid solids: whether every region of space is inside both
    // or outside both, which is to say that the two are one set of points.
    bool windings_agree() const;
    void record_simple();
    // Lets load() recognise an operand whose shape was examined against itself before, in this thread, and
    // found inert (see arrangement.cpp), and lets record_simple() remember the shapes found so this time.
    void remember_shapes() { remember_shapes_ = !plain_paths(); }
    // The plane {nx, ny, nz, |n|, d} in doubles, each the nearest to its integer (I128::to_double), and
    // the side of it a grid point is on when the doubles suffice to be certain: 1, -1, or 2 for "cannot
    // tell" (see arrangement.cpp).
    static std::array<double, 5> plane_in_doubles(const Plane &pl);
    static int side_in_doubles(const double plane[5], const int64_t *p);
    // Of the loaded triangles: whether two lie in one plane, and the coordinate axis along which the
    // plane of one is seen without being edge-on (the largest component of its normal).
    bool same_plane(int a, int b) const;
    int plane_axis(int tri) const { return axis_[static_cast<size_t>(tri)]; }
    // After load(): the winding number just in front of a triangle (see arrangement.cpp).
    int winding_in_front(int tri);

private:
    struct SoupTriangle {
        int v[3];
        int operand, label;  // label 2: a separating ribbon, which bounds cells but belongs to no solid
    };
    struct RibbonEdge {
        int a, b, axis;
        bool operator<(const RibbonEdge &o) const { return a != o.a ? a < o.a : b != o.b ? b < o.b : axis < o.axis; }
        bool operator==(const RibbonEdge &o) const { return a == o.a && b == o.b && axis == o.axis; }
    };
    struct Record {
        int tri, a, b, other;  // a == b for a single point
    };
    struct Instance {
        int v[3];
        int tri;
    };
    struct Face {
        int v[3];        // lowest id first, then ascending: the canonical orientation
        int jump[2];     // winding added, per label, on passing from the front to the back
        int parent;      // a soup triangle it lies in
        int parent_sign; // +1 when the canonical orientation is the parent's
        bool separator;  // has a ribbon instance: kept even when it changes no winding number
    };
    struct EdgeUse {
        int a, b, face, third;
        unsigned char forward, corner_a, corner_b;
    };
    struct Group {
        size_t first, count;
        long sorted;  // offset into radial_ of the counter-clockwise order, or -1
    };

    std::vector<Operand> operands_;
    std::vector<SoupTriangle> tris_;
    std::vector<Plane> planes_;
    std::vector<Box> boxes_;
    std::vector<signed char> axis_, sign_;
    std::vector<char> operand_hit_;
    // By operand: whether a pair of its own triangles went as far as finding where they cross a plane; how
    // many pairs of its own triangles had overlapping boxes; and, for an operand recognised as inert, that
    // number from when it was examined (otherwise -1).
    std::vector<char> operand_crossed_;
    std::vector<size_t> own_pairs_;
    std::vector<long long> inert_pairs_;
    bool remember_shapes_ = false;
    // The triangles of one operand, or the ribbons: they are contiguous in tris_. Each has a tree over its
    // own boxes, built once enough questions have been asked of it to pay for the building; until then a
    // question is answered by looking at every box. Both ways report the same triangles.
    struct Span {
        int first = 0, count = 0;
        Box box;
        Bvh tree;
        bool built = false;
        int scans = 0;
    };
    std::vector<Span> spans_;
    // The plane of a triangle in doubles (normal, its length, offset), for the choices and measures that
    // use them and for side_in_doubles(): computed for a triangle when first asked for.
    std::vector<std::array<double, 5>> normal_;
    std::vector<char> normal_known_;
    // Where an edge between two grid points crosses a plane, remembered by the edge and by the ratio of
    // the plane's values at its two ends (see crossing()).
    struct Crossing {
        uint64_t edge;
        int64_t bucket;
        I128 low, high;  // the plane's value at the end with the lower id, and at the other
        int point;
    };
    struct CrossingSlot {
        uint64_t edge;
        int64_t bucket;
        int index;  // into crossings_, plus one; 0 in an empty slot
    };
    // Where a triangle met the planes it was last intersected with: see ends_in_plane().
    struct Met {
        int plane[2];  // a triangle of that plane, or -1
        int count[2];
        int point[2][2];
        int misses;
    };
    // Triangles found to lie in one plane are joined here, so that same_plane() computes for a pair of
    // planes once: by triangle, a triangle of its plane found earlier (itself until one is).
    mutable std::vector<int> plane_of_;
    std::vector<int> met_of_;  // by triangle: an index into met_, or -1
    std::vector<Met> met_;
    std::vector<Crossing> crossings_;
    std::vector<CrossingSlot> crossing_table_;  // open addressing
    std::vector<Record> records_;
    std::vector<int> cluster_;  // union-find parent, or empty until the first coplanar overlap
    std::vector<Instance> instances_;
    std::vector<Face> faces_;
    std::vector<EdgeUse> uses_;
    std::vector<Group> groups_;
    std::vector<int> radial_;
    std::vector<uint32_t> side_parent_;
    std::vector<int> cell_of_side_;
    std::vector<std::array<int, 2>> winding_;
    bool unit_winding_ = false;
    bool ribbons_enabled_ = false;
    bool probe_only_ = false;
    bool plain_ = false;
    std::vector<RibbonEdge> ribbon_edges_;
    size_t real_triangles_ = 0;

    bool pair(int i, int j);
    void near_coplanar(int i, int j, const I128 at_j[3], const I128 at_i[3]);
    template <class F>
    void overlapping(Span &span, const Box &box, F &&visit);
    const double *normal(int tri);
    int crossing(int p, int q, const I128 &at_p, const I128 &at_q);
    size_t crossing_slot(uint64_t edge, int64_t bucket) const;
    void add_triangle(int a, int b, int c, int operand, int label);
    void close_span(Span &span);
    void add_ribbons();
    bool corner(int tri, int p) const;
    void size_lists();
    bool may_be_one_plane(int a, int b);
    int plane_set(int tri) const;
    bool planes_joined(int a, int b) const;
    void join_planes(int a, int b) const;
    int ends(int tri, const int side[3], const I128 value[3], int out[2]);
    int ends_in_plane(int tri, int plane, const int side[3], const I128 value[3], int out[2]);
    bool inside_closed(int tri, int p) const;
    bool coplanar_overlap(int i, int j) const;
    bool coplanar_overlap_plain(int i, int j) const;
    bool coplanar_fan_overlap(int i, int j, int shared) const;
    bool report_point(int i, int j, int p);
    bool report_segment(int i, int j, int p, int q);
    int cluster_find(int i);
    void cluster_join(int i, int j);
    void subdivide();
    void subdivide_one(Planar &planar, int tri, size_t first, size_t last);
    void subdivide_cluster(Planar &planar, const std::vector<int> &members, const std::vector<std::pair<size_t, size_t>> &ranges);
    void build_faces();
    void build_cells();
    void wind();
    void reference(int face, int side_cell_out[1], int count[2]);
    int ray_side(int q, const int64_t g[3], const int64_t *a, const int64_t *b) const;
    uint32_t side_find(uint32_t s);
    void absorb_thin_cells(std::vector<char> &filled) const;
    void extract(Rule rule, ExactMesh &out);
};

// Takes out vertices the surface does not need (simplify.cpp). The surface itself does not change.
// With the arrangement the mesh was extracted from, the planes of its triangles are taken from it
// (ExactMesh::plane) instead of being worked out from the points again.
void simplify(const Pool &pool, ExactMesh &mesh, const Arrangement *source = nullptr);

// Rounds the exact result to the grid, checks it, and repairs it if rounding left a defect.
Solid finish(Arrangement &arrangement, ExactMesh &exact, const char *context, int depth);

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
