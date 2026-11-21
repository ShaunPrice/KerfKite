// KerfKite's own planar polygon kernel: regions with holes, Boolean operations, round offsets, line
// clipping and the union of very many shapes. It depends on nothing but the C++17 standard library.
// docs/POLYGON-KERNEL.md describes the method, the limits and how it is tested.
//
// Coordinates. Every vertex is a pair of 64-bit integers on a fixed grid of one nanometre (1e-6 mm).
// Millimetre values are rounded to the grid on the way in (to_grid) and divided by 1e6 on the way out
// (to_mm). All decisions about topology (which side a point is on, whether two edges cross, which ring
// contains which) are taken with exact integer arithmetic; a point that has to be constructed, where two
// edges cross, is rounded to the nearest grid point and the edges are bent through it, repeatedly, until
// no two edges meet except at shared end points. No tolerance is compared anywhere to decide topology.
//
// Results are the same on every compiler and processor: integers are exact, and the floating-point
// arithmetic that places offset and arc vertices uses only + - * / and sqrt, which IEEE 754 rounds
// correctly, never the trigonometric functions of the platform's library.
//
// Order of a result (a PolygonSet). Outer rings run counter-clockwise and holes clockwise. A ring does
// not repeat its first vertex, has no vertex lying on the straight line between its neighbours, and
// starts at its smallest vertex, comparing x and then y. The holes of a polygon are ordered by their
// first vertex, and the polygons of a set by the first vertex of their outer rings; rings with the same
// first vertex are ordered by their following vertices. Rings are simple; two rings may touch at single
// points but never cross or share a length of edge, and the interior of each polygon is connected.
//
// Errors. Input that cannot be processed (a coordinate that is not finite or is beyond the limit, more
// vertices than the limit) throws kerfkite::ValueError, declared in kerfkite/cam.hpp.
#ifndef KERFKITE_POLYGON_HPP
#define KERFKITE_POLYGON_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kerfkite {
namespace polygon {

using Coord = std::int64_t;

struct Point {
    Coord x = 0, y = 0;
};
inline bool operator==(const Point &a, const Point &b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(const Point &a, const Point &b) { return !(a == b); }
// Order by x and then y: the order results are written in.
inline bool operator<(const Point &a, const Point &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); }

// A closed ring. The edge from the last vertex back to the first is implied.
using Ring = std::vector<Point>;

struct Polygon {
    Ring shell;
    std::vector<Ring> holes;
};
inline bool operator==(const Polygon &a, const Polygon &b) { return a.shell == b.shell && a.holes == b.holes; }
inline bool operator!=(const Polygon &a, const Polygon &b) { return !(a == b); }

// A region: polygons that do not overlap. Every function that returns one returns it in the order
// described above; functions that take one expect that form (any result of this kernel is in it).
using PolygonSet = std::vector<Polygon>;

// A point or a path in millimetres, as the application holds them.
using PointMm = std::array<double, 2>;
using PathMm = std::vector<PointMm>;

// Grid units in a millimetre.
constexpr double kUnitsPerMm = 1e6;
// The largest coordinate accepted, in grid units: about 137 m either side of the origin.
constexpr Coord kMaxCoordinate = Coord(1) << 37;
// The largest offset distance and shape size, in grid units: about 34 m.
constexpr Coord kMaxDistance = Coord(1) << 35;
// The most edges one operation works on at once, counting those it creates where edges cross.
constexpr std::size_t kMaxEdges = 8000000;
// The most shapes an Artwork takes.
constexpr std::size_t kMaxShapes = 4000000;

// Millimetres to the grid, rounding to the nearest unit (halves upwards), and back. to_grid throws
// ValueError for a value that is not finite or is beyond kMaxCoordinate.
Coord to_grid(double millimetres);
inline double to_mm(Coord units) { return static_cast<double>(units) / kUnitsPerMm; }
Point to_grid(const PointMm &point);
inline PointMm to_mm(const Point &point) { return {to_mm(point.x), to_mm(point.y)}; }

// A path in millimetres as a ring on the grid. Vertices that round to the same grid point as the one
// before are dropped, as is a last vertex equal to the first. The result may have fewer than three
// vertices, which no operation treats as a region.
Ring ring_from_mm(const PathMm &path);
// A ring in millimetres; with closed, the first vertex is repeated at the end.
PathMm ring_to_mm(const Ring &ring, bool closed = true);

// ---- Measures and predicates -----------------------------------------------------------------------

// Twice the signed area of a ring in grid units squared is exact; these give it in square millimetres.
// Positive for a counter-clockwise ring.
double signed_area(const Ring &ring);
// The area of a region in square millimetres: outer rings less holes.
double area(const PolygonSet &region);
// 1 for a counter-clockwise ring, -1 for a clockwise one, 0 when the area is exactly zero.
int orientation(const Ring &ring);

struct Bounds {
    double min_x, min_y, max_x, max_y;
};
// The bounding box in millimetres. False, leaving the box alone, for an empty region.
bool bounds(const PolygonSet &region, Bounds &box);
// The total number of vertices.
std::size_t vertex_count(const PolygonSet &region);

// Whether a ring is simple: at least three distinct vertices, and no two edges that meet anywhere but
// at the vertex two consecutive edges share. A ring that touches itself at a vertex is not simple.
// Decided exactly on the grid; consecutive equal vertices are ignored.
bool is_simple(const Ring &ring);

enum class Location { Outside, Boundary, Inside };
// Where a grid point lies in a region, decided exactly.
Location locate(const PolygonSet &region, const Point &point);
Location locate(const Ring &ring, const Point &point);

// Whether a set is a valid region in this kernel's form: every ring simple, outer rings counter-clockwise
// and holes clockwise, no two rings crossing or sharing a length of edge, no point covered twice, and
// every hole inside its own outer ring. It does not check that interiors are connected. When it is not
// valid and reason is given, reason says what was found.
bool is_valid(const PolygonSet &region, std::string *reason = nullptr);

// ---- Regions and Boolean operations ----------------------------------------------------------------

// How rings that overlap or cross themselves are read as a region, from the winding number of a point.
enum class FillRule {
    EvenOdd,   // inside where the winding number is odd
    NonZero,   // inside where it is not zero
    Positive,  // inside where it is greater than zero
};

// The region any set of rings describes under a fill rule. The rings may cross themselves and each other,
// overlap, touch, or repeat vertices; rings with fewer than three vertices are ignored.
PolygonSet normalise(const std::vector<Ring> &rings, FillRule rule);

// The region of closed contours combined by successive symmetric difference, which for simple contours is
// the even-odd rule: normalise(rings, FillRule::EvenOdd).
PolygonSet compose(const std::vector<Ring> &rings);

enum class Operation { Union, Intersection, Difference, Xor };
PolygonSet combine(Operation operation, const PolygonSet &a, const PolygonSet &b);
inline PolygonSet unite(const PolygonSet &a, const PolygonSet &b) { return combine(Operation::Union, a, b); }
inline PolygonSet intersect(const PolygonSet &a, const PolygonSet &b) { return combine(Operation::Intersection, a, b); }
inline PolygonSet subtract(const PolygonSet &a, const PolygonSet &b) { return combine(Operation::Difference, a, b); }
inline PolygonSet exclusive_or(const PolygonSet &a, const PolygonSet &b) { return combine(Operation::Xor, a, b); }

// The union of many regions at once. All their edges are resolved together, in spatial groups when there
// are very many, which costs far less than uniting them one after another.
PolygonSet unite_all(const std::vector<PolygonSet> &regions);

// The same operations on rings that are not yet regions: each side is read with its own fill rule.
PolygonSet combine(Operation operation, const std::vector<Ring> &a, FillRule rule_a, const std::vector<Ring> &b,
                   FillRule rule_b);

// ---- Offsets and shapes ----------------------------------------------------------------------------

// How a circular arc becomes straight edges. An arc of radius r is replaced by edges that touch the true
// arc and lie outside it elsewhere (a circumscribed polygon), so an offset is never short of its distance
// and a disc is never smaller than its diameter. Consecutive edges turn by at most 1.0625 steps, a step
// being a quarter turn divided by segments_per_quarter, which puts the corners outside the true arc by at
// most r * (1 / cos(0.53125 * step) - 1): 0.00034 r at the default.
struct ArcOptions {
    // Edges in a quarter turn, from 1 to 1024.
    int segments_per_quarter = 32;
    // When positive, the greatest distance in millimetres a corner may lie outside the true arc. The
    // number of segments is then chosen for each radius to meet it and segments_per_quarter is ignored.
    double tolerance = 0;
};

// The region grown by a distance in millimetres (positive) or shrunk by it (negative), with round joins:
// the points within the distance of the region, or the points of the region further than the distance
// from its boundary. An offset smaller than one grid unit returns the region unchanged. The result is
// exactly the union (or, shrinking, the difference) of the region and polygons built on the grid around
// each edge and corner, so it is always a valid region; see docs/POLYGON-KERNEL.md for its accuracy.
PolygonSet offset(const PolygonSet &region, double distance, const ArcOptions &arcs = ArcOptions());

// The corners of an offset as a drawing asks for them, in place of the circumscribed arcs above.
enum class Join {
    Round,  // an arc about the corner whose vertices lie on the true arc (an inscribed polygon)
    Miter,  // the two offset edges carried on until they meet
    Bevel,  // the ends of the two offset edges joined by one straight edge
};
struct JoinOptions {
    Join join = Join::Round;
    // Round: edges in a whole turn, from 3 to 4096. An arc is divided into equal parts of at most 1.0625
    // of one such step.
    int segments = 48;
    // Miter: the longest a corner may reach from the vertex, as a multiple of the distance, at least 1. A
    // sharper corner is cut square instead: by the line across the corner at the distance from the vertex.
    double miter_limit = 2;
};
// The region grown or shrunk by a distance in millimetres with corners joined as given. Like the round
// offset it is exactly the union (or, shrinking, the difference) of the region and polygons built on the
// grid along each edge and round each corner, so the result is always a valid region. A round join is
// short of the distance between its vertices by at most distance * (1 - cos(0.53125 * step)).
PolygonSet offset(const PolygonSet &region, double distance, const JoinOptions &joins);

// The convex hull of points: one counter-clockwise ring in this kernel's form, exact on the grid. Empty
// when the points lie on one line.
Ring convex_hull(const std::vector<Point> &points);

// Simple counter-clockwise rings, in millimetres on the way in.
// A disc, as a circumscribed polygon of 4 * segments edges (fewer for a disc of a few grid units).
Ring disc(double centre_x, double centre_y, double diameter, const ArcOptions &arcs = ArcOptions());
// A rectangle with sides parallel to the axes.
Ring rectangle(double centre_x, double centre_y, double width, double height);
// A rectangle with sides parallel to the axes whose shorter sides are replaced by half discs.
Ring obround(double centre_x, double centre_y, double width, double height, const ArcOptions &arcs = ArcOptions());
// A polyline drawn with a round pen of the given width: round ends and round joins. One point, or
// several equal ones, gives a disc.
PolygonSet stroke(const PathMm &points, double width, const ArcOptions &arcs = ArcOptions());

// Dark and clear artwork, as a photoplotter file draws it: a dark shape adds to the image and a clear
// shape removes from what has been drawn so far, in drawing order. Shapes of one polarity that follow
// each other are united in one step, so a board of very many pads and tracks costs little more than
// sorting their edges.
class Artwork {
  public:
    explicit Artwork(const ArcOptions &arcs = ArcOptions());
    ~Artwork();
    Artwork(const Artwork &) = delete;
    Artwork &operator=(const Artwork &) = delete;

    // A simple ring in either direction, or a region.
    void add(const Ring &simple_ring, bool dark = true);
    void add(const PolygonSet &region, bool dark = true);
    void disc(double centre_x, double centre_y, double diameter, bool dark = true);
    void rectangle(double centre_x, double centre_y, double width, double height, bool dark = true);
    void obround(double centre_x, double centre_y, double width, double height, bool dark = true);
    void stroke(const PathMm &points, double width, bool dark = true);

    std::size_t shapes() const;
    // The image so far. The artwork may be added to afterwards.
    PolygonSet image();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// ---- Lines ----------------------------------------------------------------------------------------

// A straight segment between two grid points.
using Segment = std::array<Point, 2>;

// Cuts straight segments to a region. Build one for a region and use it for every line of a hatch. One
// LineClipper is for one thread at a time; separate ones, like every other function here, are independent.
class LineClipper {
  public:
    explicit LineClipper(const PolygonSet &region);
    ~LineClipper();
    LineClipper(const LineClipper &) = delete;
    LineClipper &operator=(const LineClipper &) = delete;

    // The parts of the segment from a to b that lie in the region or along its boundary, in order from a,
    // each running in the direction of a to b. A place where the segment only touches the boundary at a
    // point gives nothing. The cuts are found exactly and their end points rounded to the grid; a part
    // whose ends round to the same grid point is dropped.
    std::vector<Segment> clip(const Point &a, const Point &b) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// ---- Voronoi diagrams of points ---------------------------------------------------------------------

// The most points a triangulation takes.
constexpr std::size_t kMaxSites = 2000000;
// Where a triangle would be named and there is none: the side of an edge that faces outwards.
constexpr std::uint32_t kNoTriangle = 0xffffffffu;

// The Delaunay triangulation of grid points: no point lies strictly inside the circle through the corners
// of any triangle. Every decision is exact, so the result is the same on every platform. Where four or more
// points lie on one circle the diagonals chosen follow from the order of the points alone.
struct Triangulation {
    // The distinct points in order of x and then y. Everything below names points by their place here.
    std::vector<Point> points;
    // Each triangle counter-clockwise, its smallest point first; the triangles in order. Empty when the
    // points all lie on one line.
    std::vector<std::array<std::uint32_t, 3>> triangles;
    // Each edge once, in order, from its smaller point a to b, with the triangles on its left and on its
    // right going from a to b, kNoTriangle where the edge is on the convex hull. Points on one line are
    // joined in a chain, with no triangle on either side.
    struct Edge {
        std::uint32_t a, b, left, right;
    };
    std::vector<Edge> edges;
};
// Throws ValueError for a coordinate beyond kMaxCoordinate or more than kMaxSites points. Repeated points
// count once. O(n log n) time for n points, whatever their arrangement.
Triangulation delaunay(const std::vector<Point> &points);

// An edge of the Voronoi diagram: the part of the perpendicular bisector of two points that is nearer to
// those two than to any other.
struct VoronoiEdge {
    Segment ends;                        // on the grid, the smaller end first
    std::array<std::uint32_t, 2> sites;  // the two points it lies between, the smaller first
};
// The edges of the Voronoi diagram of a triangulation's points, cut to the box from low to high, in order
// of their ends. An edge that has no end, or that ends outside the box, is cut where it leaves the box. The
// ends are the centres of the triangles' circumscribed circles, and the cuts by the box, worked out
// exactly as fractions and then rounded to the nearest grid point, halves upwards. An edge whose ends
// round to one point is left out, as is an edge of no length between triangles whose corners share a circle.
std::vector<VoronoiEdge> voronoi_edges(const Triangulation &triangulation, const Point &low, const Point &high);

// The distance from a point to the boundary of a region. Build one for a region and ask it for many
// points. One BoundaryDistance is for one thread at a time.
class BoundaryDistance {
  public:
    explicit BoundaryDistance(const PolygonSet &region);
    ~BoundaryDistance();
    BoundaryDistance(const BoundaryDistance &) = delete;
    BoundaryDistance &operator=(const BoundaryDistance &) = delete;

    // The distance in grid units from a grid point to the nearest point of any ring, whether the point is
    // inside the region or outside it; infinity for an empty region. For each edge the foot of the
    // perpendicular is placed exactly, and the distance is then the nearest double to an exact whole
    // number, a square root and at most one division: the same double on every platform, within
    // 4 parts in 10^16 of the true distance. The least over all the edges is returned.
    double to(const Point &point) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// ---- The application's order ------------------------------------------------------------------------

// The rings of a region as studio/operations.py lists tool paths (its outlines function): polygons by
// area, smallest first, polygons of equal area keeping this kernel's order; for each polygon its holes and
// then its outer ring. Each ring is closed by repeating its first vertex. Outer rings are given clockwise
// and holes counter-clockwise, the directions the application has cut in until now; each ring still
// starts at its smallest vertex.
std::vector<PathMm> outlines(const PolygonSet &region);

}  // namespace polygon
}  // namespace kerfkite

#endif
