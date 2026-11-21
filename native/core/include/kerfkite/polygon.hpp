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
constexpr std::size_t kMaxEdges = 16000000;
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

// Cuts straight segments to a region. Build one for a region and use it for every line of a hatch.
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
