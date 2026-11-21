// What the files of the polygon kernel share: exact predicates, the bucket grid that finds edges near
// each other, and the arrangement that turns any set of edges into a region. Internal.
#ifndef KERFKITE_POLYGON_KERNEL_HPP
#define KERFKITE_POLYGON_KERNEL_HPP

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

#include "kerfkite/polygon.hpp"
#include "wide.hpp"

// The few floating-point steps that place vertices must not be fused into multiply-add instructions,
// which round once where the source rounds twice. The build passes -ffp-contract=off or /fp:precise; these
// say so again where a compiler listens.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace kerfkite {
namespace polygon {
namespace detail {

// Coordinates may leave the input range by an offset distance and its corner allowance, no further.
constexpr Coord kHardLimit = Coord(1) << 39;

[[noreturn]] void reject(const char *message);
// Throws unless the point is inside the range the arithmetic is exact for.
void check_range(const Point &point);

// (a - o) x (b - o): positive when b is to the left of the line from o through a.
inline Int128 cross(const Point &o, const Point &a, const Point &b) {
    return multiply(a.x - o.x, b.y - o.y) - multiply(a.y - o.y, b.x - o.x);
}
inline Int128 cross(Coord ax, Coord ay, Coord bx, Coord by) { return multiply(ax, by) - multiply(ay, bx); }
inline Int128 dot(Coord ax, Coord ay, Coord bx, Coord by) { return multiply(ax, bx) + multiply(ay, by); }
// 1 when c is to the left of the line from a to b, -1 to the right, 0 on it.
inline int orient(const Point &a, const Point &b, const Point &c) { return sign(cross(a, b, c)); }

// 1 when d is inside the circle through a, b and c, which run counter-clockwise, -1 when it is outside and
// 0 when the four lie on one circle: the sign of a determinant of the fourth degree, taken exactly in 256
// bits. For a, b and c running clockwise the sign is the other way, and for three on a line it tells
// which side of the line d is on.
int in_circle(const Point &a, const Point &b, const Point &c, const Point &d);

// Twice the signed area of a ring.
Int128 twice_area(const Ring &ring);

// Whether the closed segments ab and cd have a point in common.
bool segments_meet(const Point &a, const Point &b, const Point &c, const Point &d);

// An edge of an arrangement: a < b, and w is how much the winding number of each of the two operands
// rises on crossing the edge from its right side (below it) to its left (above it).
struct Edge {
    Point a, b;
    std::int32_t w[2];
    std::uint32_t fresh;  // made since pairs of edges were last tested
};

// Buckets on a square grid, each holding the edges that may pass through it, and a finer grid of the same
// kind inside any bucket that is crowded, so that edges packed into one corner of the plane are still found
// a few at a time. Two edges with a point in common, or with a vertex of one within a unit of the other,
// always share a bucket. The grid only limits which pairs are looked at: its sizes, and the floating-point
// rounding in it, have no effect on any result.
template <class E> class Grid {
  public:
    void build(const std::vector<E> &edges) {
        std::vector<std::uint32_t> all(edges.size());
        Coord x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const E &e = edges[i];
            all[i] = static_cast<std::uint32_t>(i);
            const Coord lx = std::min(e.a.x, e.b.x), hx = std::max(e.a.x, e.b.x), ly = std::min(e.a.y, e.b.y), hy = std::max(e.a.y, e.b.y);
            if (i == 0) x0 = lx, y0 = ly, x1 = hx, y1 = hy;
            x0 = std::min(x0, lx), y0 = std::min(y0, ly), x1 = std::max(x1, hx), y1 = std::max(y1, hy);
        }
        std::size_t budget = 64 * edges.size() + 4000000;
        build_in(edges, all, x0, y0, x1, y1, 0, budget);
    }

    // Calls visit(index) for every edge in a bucket the segment from a to b passes through. An edge can
    // be visited more than once.
    template <class F> void near_segment(const Point &a, const Point &b, F &&visit) const {
        cells(a, b, [&](std::size_t cell) { in_cell(cell, visit, [&](const Grid &inner) { inner.near_segment(a, b, visit); }); });
    }

    // The same for the unbounded line through a and b.
    template <class F> void near_line(const Point &a, const Point &b, F &&visit) const {
        line_cells(a, b, [&](std::size_t cell) { in_cell(cell, visit, [&](const Grid &inner) { inner.near_line(a, b, visit); }); });
    }

    // The most edges in one bucket, counting finer grids by their own heaviest bucket.
    std::size_t heaviest() const {
        std::size_t most = 0;
        for (std::size_t cell = 0; cell + 1 < start_.size(); ++cell) {
            const bool finer = !inner_of_.empty() && inner_of_[cell] >= 0;
            most = std::max(most, finer ? inner_[static_cast<std::size_t>(inner_of_[cell])].heaviest() : static_cast<std::size_t>(start_[cell + 1] - start_[cell]));
        }
        return most;
    }

  private:
    static constexpr std::uint32_t kCrowded = 40;
    static constexpr int kDepth = 8;

    template <class F, class Inner> void in_cell(std::size_t cell, F &&visit, Inner &&descend) const {
        if (!inner_of_.empty() && inner_of_[cell] >= 0) {
            descend(inner_[static_cast<std::size_t>(inner_of_[cell])]);
            return;
        }
        for (std::uint32_t k = start_[cell]; k < start_[cell + 1]; ++k) visit(items_[k]);
    }

    // A grid over the box from (x0, y0) to (x1, y1) holding the edges named in subset. budget limits the
    // entries all the grids inside one another may hold.
    void build_in(const std::vector<E> &edges, const std::vector<std::uint32_t> &subset, Coord x0, Coord y0, Coord x1, Coord y1, int depth,
                  std::size_t &budget) {
        x0_ = x0, y0_ = y0;
        const double width = static_cast<double>(x1 - x0) + 1, height = static_cast<double>(y1 - y0) + 1;
        const double count = static_cast<double>(subset.empty() ? 1 : subset.size());
        double extent = 0;
        for (const std::uint32_t i : subset) {
            const E &e = edges[i];
            extent += std::min(width, std::fabs(static_cast<double>(e.b.x - e.a.x))) + std::min(height, std::fabs(static_cast<double>(e.b.y - e.a.y)));
        }
        // About one edge to a bucket, but buckets large enough that the edges are entered about five
        // times each at most.
        double size = std::max(std::sqrt(width * height / count), extent / (5 * count));
        size = std::max(1.0, std::floor(size));
        while ((std::floor(width / size) + 1) * (std::floor(height / size) + 1) > 4 * count + 64) size *= 2;
        size_ = static_cast<Coord>(size);
        nx_ = (x1 - x0) / size_ + 1;
        ny_ = (y1 - y0) / size_ + 1;
        const std::size_t buckets = static_cast<std::size_t>(nx_ * ny_);
        start_.assign(buckets + 1, 0);
        for (const std::uint32_t i : subset) cells(edges[i].a, edges[i].b, [&](std::size_t cell) { ++start_[cell + 1]; });
        for (std::size_t i = 1; i < start_.size(); ++i) start_[i] += start_[i - 1];
        items_.resize(start_.back());
        std::vector<std::uint32_t> next(start_.begin(), start_.end() - 1);
        for (const std::uint32_t i : subset) cells(edges[i].a, edges[i].b, [&](std::size_t cell) { items_[next[cell]++] = i; });
        budget -= std::min(budget, items_.size());
        if (depth >= kDepth || size_ <= 1) return;
        std::vector<std::uint32_t> crowd;
        for (std::size_t cell = 0; cell < buckets; ++cell) {
            const std::uint32_t load = start_[cell + 1] - start_[cell];
            if (load <= kCrowded || budget < 6 * static_cast<std::size_t>(load)) continue;
            crowd.assign(items_.begin() + start_[cell], items_.begin() + start_[cell + 1]);
            const Coord cx = x0_ + static_cast<Coord>(cell % static_cast<std::size_t>(nx_)) * size_;
            const Coord cy = y0_ + static_cast<Coord>(cell / static_cast<std::size_t>(nx_)) * size_;
            Grid inner;
            inner.build_in(edges, crowd, cx, cy, cx + size_ - 1, cy + size_ - 1, depth + 1, budget);
            // Kept only if it thins the crowd: edges that all cross the whole bucket cannot be told apart.
            if (inner.heaviest() * 4 > static_cast<std::size_t>(load) * 3) continue;
            if (inner_of_.empty()) inner_of_.assign(buckets, -1);
            inner_of_[cell] = static_cast<std::int32_t>(inner_.size());
            inner_.push_back(std::move(inner));
        }
    }

    // Every bucket the segment from a to b passes through, and possibly neighbours of those. A part of
    // the segment outside the grid counts for the nearest bucket on the border.
    template <class F> void cells(Point a, Point b, F &&visit) const {
        if (a.x > b.x) std::swap(a, b);
        const Coord c0 = column(a.x), c1 = column(b.x);
        if (c0 == c1) {
            const Coord r0 = row(static_cast<double>(std::min(a.y, b.y)) - 2), r1 = row(static_cast<double>(std::max(a.y, b.y)) + 2);
            for (Coord r = r0; r <= r1; ++r) visit(static_cast<std::size_t>(r * nx_ + c0));
            return;
        }
        const double slope = static_cast<double>(b.y - a.y) / static_cast<double>(b.x - a.x);
        for (Coord c = c0; c <= c1; ++c) {
            // A unit beyond the bucket on each side, so that a point within a unit of the segment is in a
            // bucket the segment is given, whichever side of a bucket's edge the two fall.
            const Coord left = std::max(a.x, x0_ + c * size_ - 1), right = std::min(b.x, x0_ + (c + 1) * size_ + 1);
            const double ya = static_cast<double>(a.y) + slope * static_cast<double>(left - a.x);
            const double yb = static_cast<double>(a.y) + slope * static_cast<double>(right - a.x);
            // Two units more than the estimate, which is within a hundredth of a unit of the true height.
            const Coord r0 = row(std::min(ya, yb) - 2), r1 = row(std::max(ya, yb) + 2);
            for (Coord r = r0; r <= r1; ++r) visit(static_cast<std::size_t>(r * nx_ + c));
        }
    }

    // Every bucket the unbounded line through a and b passes through, and possibly neighbours.
    template <class F> void line_cells(const Point &a, const Point &b, F &&visit) const {
        const double dx = static_cast<double>(b.x - a.x), dy = static_cast<double>(b.y - a.y);
        if (std::fabs(dx) >= std::fabs(dy)) {
            const double slope = dy / dx;
            for (Coord c = 0; c < nx_; ++c) {
                const double ya = static_cast<double>(a.y) + slope * static_cast<double>(x0_ + c * size_ - 1 - a.x);
                const double yb = static_cast<double>(a.y) + slope * static_cast<double>(x0_ + (c + 1) * size_ + 1 - a.x);
                if (std::max(ya, yb) + 2 < static_cast<double>(y0_) || std::min(ya, yb) - 2 > static_cast<double>(y0_ + ny_ * size_)) continue;
                const Coord r0 = row(std::min(ya, yb) - 2), r1 = row(std::max(ya, yb) + 2);
                for (Coord r = r0; r <= r1; ++r) visit(static_cast<std::size_t>(r * nx_ + c));
            }
        } else {
            const double slope = dx / dy;
            for (Coord r = 0; r < ny_; ++r) {
                const double xa = static_cast<double>(a.x) + slope * static_cast<double>(y0_ + r * size_ - 1 - a.y);
                const double xb = static_cast<double>(a.x) + slope * static_cast<double>(y0_ + (r + 1) * size_ + 1 - a.y);
                if (std::max(xa, xb) + 2 < static_cast<double>(x0_) || std::min(xa, xb) - 2 > static_cast<double>(x0_ + nx_ * size_)) continue;
                const Coord c0 = column_of(std::min(xa, xb) - 2), c1 = column_of(std::max(xa, xb) + 2);
                for (Coord c = c0; c <= c1; ++c) visit(static_cast<std::size_t>(r * nx_ + c));
            }
        }
    }

    Coord column(Coord x) const { return x <= x0_ ? 0 : std::min((x - x0_) / size_, nx_ - 1); }
    Coord column_of(double x) const {
        const double c = std::floor((x - static_cast<double>(x0_)) / static_cast<double>(size_));
        return c <= 0 ? 0 : c >= static_cast<double>(nx_ - 1) ? nx_ - 1 : static_cast<Coord>(c);
    }
    Coord row(double y) const {
        const double r = std::floor((y - static_cast<double>(y0_)) / static_cast<double>(size_));
        return r <= 0 ? 0 : r >= static_cast<double>(ny_ - 1) ? ny_ - 1 : static_cast<Coord>(r);
    }

    Coord x0_ = 0, y0_ = 0, size_ = 1, nx_ = 1, ny_ = 1;
    std::vector<std::uint32_t> start_, items_;
    std::vector<std::int32_t> inner_of_;  // of a crowded bucket, its finer grid
    std::vector<Grid> inner_;
};

// Any set of closed curves, resolved into edges that meet only at shared end points, with the winding
// number of each operand known on both sides of every edge.
class Arrangement {
  public:
    // Adds a closed ring to operand 0 or 1. Rings of fewer than two distinct vertices add nothing.
    void add(const Ring &ring, int operand);
    void add(const PolygonSet &region, int operand);
    std::size_t edges() const { return edges_.size(); }

    // Splits edges where they cross, touch or overlap, rounding each crossing to the grid, until no two
    // edges have a point in common other than a shared end; then joins equal edges and sorts them.
    void resolve();
    // After resolve(): the winding numbers of both operands just below each edge.
    void wind();
    // After wind(): the region where inside(winding of operand 0, winding of operand 1) holds.
    template <class Inside> PolygonSet region(Inside &&inside) const {
        std::vector<Segment> boundary;
        for (std::size_t i = 0; i < edges_.size(); ++i) {
            const Edge &e = edges_[i];
            const bool under = inside(below_[i][0], below_[i][1]);
            const bool over = inside(below_[i][0] + e.w[0], below_[i][1] + e.w[1]);
            if (under == over) continue;
            // Directed so that the inside is on the left.
            boundary.push_back(over ? Segment{e.a, e.b} : Segment{e.b, e.a});
        }
        return assemble(boundary);
    }

    const std::vector<Edge> &resolved() const { return edges_; }
    const std::vector<std::array<std::int32_t, 2>> &below() const { return below_; }
    // How many times resolve() went round: for tests and measurements.
    int rounds() const { return rounds_; }

  private:
    void merge();
    static PolygonSet assemble(std::vector<Segment> &boundary);

    std::vector<Edge> edges_;
    std::vector<std::array<std::int32_t, 2>> below_;
    int rounds_ = 0;
};

// The most rounds of splitting any arrangement has needed since the program started, for tests and
// measurements: a round that finds nothing to split is the last, so a simple case takes one or two.
int resolve_rounds();

inline bool filled(std::int32_t winding, FillRule rule) {
    return rule == FillRule::EvenOdd ? (winding & 1) != 0 : rule == FillRule::NonZero ? winding != 0 : winding > 0;
}

// Sweeps edges that meet only at end points from left to right. edges must be sorted by a. At each
// vertex where edges start, visit(group, under) is given those edges from the lowest to the highest and
// the edge directly below the vertex, or kNone.
constexpr std::uint32_t kNone = 0xffffffffu;

// The rings of a region one after another: outer ring, then holes, for each polygon.
template <class F> void each_ring(const PolygonSet &region, F &&visit) {
    for (const Polygon &polygon : region) {
        visit(polygon.shell);
        for (const Ring &hole : polygon.holes) visit(hole);
    }
}

// Shapes for a union: each a set of rings that together wind zero or more times round every point (a ring
// the right way round, a polygon with its holes, the curve round a stroke).
using Shapes = std::vector<std::vector<Ring>>;

// The union of shapes: where the winding number is positive. Above group_edges edges the shapes are first
// united in spatial groups, so that edges buried inside the image are discarded early.
PolygonSet unite_positive(const Shapes &shapes, std::size_t group_edges);

// The cosine and sine of an angle of at most a quarter turn, from their series: the same on every
// platform, which the library functions are not.
void series_cos_sin(double angle, double &cosine, double &sine);

// The curve that bounds the pieces added by growing a ring by `distance` grid units on its right-hand
// side: see offset.cpp. The ring has its inside on the left and no two consecutive vertices equal; it may
// have only two vertices, which is a line there and back.
void offset_curve(const Ring &ring, double distance, const ArcOptions &arcs, Ring &out);

// The curve round a polyline drawn with a round pen: a ring that winds at least once round every point of
// the stroke and nowhere negatively. A single point gives a disc and no points an empty ring.
Ring stroke_curve(const PathMm &points, double width, const ArcOptions &arcs);

}  // namespace detail
}  // namespace polygon
}  // namespace kerfkite

#endif
