// The arrangement of triangle meshes: the engine behind the Boolean operations, the repair of a mesh
// that rounding has damaged, and the exact self-intersection checks.
#ifndef KERFKITE_SOLID_ARRANGEMENT_HPP
#define KERFKITE_SOLID_ARRANGEMENT_HPP

#include <array>
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

// Which regions of space are solid, given how many times each operand's surface winds around them.
enum class Rule { Union, Intersection, Difference, Positive };

struct Operand {
    const std::vector<GridPoint> *v;
    const std::vector<Triangle> *t;
    int label;         // 0 or 1: the winding count this operand adds to
    bool simple;       // known to be free of self-intersections
    const Mesh *mesh;  // where to record that, or null
};

// The result before rounding: vertices are points of the pool.
struct ExactMesh {
    std::vector<int> vertex;
    std::vector<Triangle> tri;
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
    // After resolve() with Rule::Positive on one operand: every face separates winding 0 from winding 1.
    bool unit_winding() const { return unit_winding_; }
    void record_simple() const;
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
    Bvh bvh_;
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
    std::vector<RibbonEdge> ribbon_edges_;
    size_t real_triangles_ = 0;

    bool pair(int i, int j);
    void near_coplanar(int i, int j);
    void add_triangle(int a, int b, int c, int operand, int label);
    void add_ribbons();
    bool corner(int tri, int p) const;
    int ends(int tri, const int side[3], const Plane &pl, int out[2]);
    bool inside_closed(int tri, int p) const;
    bool coplanar_overlap(int i, int j) const;
    bool coplanar_fan_overlap(int i, int j, int shared) const;
    bool report_point(int i, int j, int p);
    bool report_segment(int i, int j, int p, int q);
    int cluster_find(int i);
    void cluster_join(int i, int j);
    void subdivide();
    void subdivide_one(int tri, size_t first, size_t last);
    void subdivide_cluster(const std::vector<int> &members, const std::vector<std::pair<size_t, size_t>> &ranges);
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
void simplify(const Pool &pool, ExactMesh &mesh);

// Rounds the exact result to the grid, checks it, and repairs it if rounding left a defect.
Solid finish(Arrangement &arrangement, ExactMesh &exact, const char *context, int depth);

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
