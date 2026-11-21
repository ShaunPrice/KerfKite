// The Delaunay triangulation of grid points and the Voronoi diagram that is its dual.
//
// The triangulation is built by divide and conquer on the quad-edge structure, as Guibas and Stolfi
// describe it ("Primitives for the manipulation of general subdivisions and the computation of Voronoi
// diagrams", ACM Transactions on Graphics 4(2), 1985): the points are sorted by x and then y, halved until
// two or three are left, and the halves merged from their lower common tangent upwards. That takes
// O(n log n) time whatever the arrangement of the points.
//
// Both of its tests are exact. Which side of a line a point is on is the sign of a 128-bit product, as
// everywhere in this kernel. Whether a point is inside the circle through three others is the sign of a
// determinant of the fourth degree, three products of 128-bit values added, taken in 256 bits (wide.hpp).
// Nothing is perturbed and no tolerance is used, so points on one line and points on one circle are
// ordinary input: the first give a chain or a fan with no flat triangle, and the second give triangles
// whose circumscribed circles coincide, so that the Voronoi edge between them has no length and is left
// out. The diagram that results does not depend on which diagonal the triangulation chose.
//
// A Voronoi edge lies on the perpendicular bisector of the two points a and b it separates. With m the
// middle of ab and d the direction of ab turned a quarter turn to the left, the centre of the circle
// through a, b and a third point c is m + t d with
//
//     t = (c - a) . (c - b) / (2 (b - a) x (c - a)),
//
// an exact fraction. The edge runs from the t of the triangle on the right of ab to the t of the triangle
// on its left, or without end on a side where there is no triangle. That range is cut to the box by
// comparing fractions exactly, and only then are the two ends worked out and rounded to the grid, so a
// centre far outside the box (the corners of a thin triangle) never has to be represented.
#include <algorithm>
#include <numeric>

#include "kernel.hpp"

namespace kerfkite {
namespace polygon {

namespace detail {

int in_circle(const Point &a, const Point &b, const Point &c, const Point &d) {
    const Coord adx = a.x - d.x, ady = a.y - d.y, bdx = b.x - d.x, bdy = b.y - d.y, cdx = c.x - d.x, cdy = c.y - d.y;
    const Int256 sum = wide_product(dot(adx, ady, adx, ady), cross(bdx, bdy, cdx, cdy)) +
                       wide_product(dot(bdx, bdy, bdx, bdy), cross(cdx, cdy, adx, ady)) +
                       wide_product(dot(cdx, cdy, cdx, cdy), cross(adx, ady, bdx, bdy));
    return sign(sum);
}

}  // namespace detail

using namespace detail;

namespace {

// The quad-edge structure. An edge of the triangulation is four numbers in a row: e is the edge directed
// from its origin, e + 1 the dual edge turned a quarter turn (from the face on the right to the face on
// the left), e + 2 the edge the other way, e + 3 the dual the other way. next_[q] is the next edge
// counter-clockwise about the origin of q.
class Mesh {
  public:
    explicit Mesh(const std::vector<Point> &points) : points_(points) {
        next_.reserve(12 * points.size());
        origin_.reserve(12 * points.size());
    }

    using Edge = std::uint32_t;
    static Edge rot(Edge e) { return (e & ~Edge(3)) | ((e + 1) & 3); }
    static Edge sym(Edge e) { return e ^ 2; }
    static Edge inverse_rot(Edge e) { return (e & ~Edge(3)) | ((e + 3) & 3); }
    Edge onext(Edge e) const { return next_[e]; }
    Edge oprev(Edge e) const { return rot(next_[rot(e)]); }
    Edge lnext(Edge e) const { return rot(next_[inverse_rot(e)]); }
    Edge rprev(Edge e) const { return next_[sym(e)]; }
    std::uint32_t origin(Edge e) const { return origin_[e]; }
    std::uint32_t destination(Edge e) const { return origin_[sym(e)]; }
    const Point &at(std::uint32_t index) const { return points_[index]; }

    // The two edges the paper returns: the counter-clockwise hull edge out of the left-most point and the
    // clockwise hull edge out of the right-most.
    struct Hull {
        Edge left, right;
    };

    // The triangulation of points first to last - 1 of the sorted list, at least two of them.
    Hull build(std::uint32_t first, std::uint32_t last) {
        const std::uint32_t count = last - first;
        if (count == 2) {
            const Edge a = make(first, first + 1);
            return {a, sym(a)};
        }
        if (count == 3) {
            const Edge a = make(first, first + 1), b = make(first + 1, first + 2);
            splice(sym(a), b);
            const int turn = orient(at(first), at(first + 1), at(first + 2));
            if (turn > 0) {
                connect(b, a);
                return {a, sym(b)};
            }
            if (turn < 0) {
                const Edge c = connect(b, a);
                return {sym(c), c};
            }
            return {a, sym(b)};  // three points on a line: a chain
        }
        const std::uint32_t middle = first + count / 2;
        const Hull left = build(first, middle), right = build(middle, last);
        Edge ldo = left.left, ldi = left.right, rdi = right.left, rdo = right.right;
        // The lower common tangent of the two halves.
        for (;;) {
            if (left_of(origin(rdi), ldi)) ldi = lnext(ldi);
            else if (right_of(origin(ldi), rdi)) rdi = rprev(rdi);
            else break;
        }
        Edge base = connect(sym(rdi), ldi);
        if (origin(ldi) == origin(ldo)) ldo = sym(base);
        if (origin(rdi) == origin(rdo)) rdo = base;
        // Upwards from the tangent: each cross edge is the one whose circle through the ends of the last is
        // empty, found after removing the edges of either half that the other half's points invalidate.
        for (;;) {
            Edge lcand = onext(sym(base));
            if (valid(lcand, base))
                while (in_circle(at(destination(base)), at(origin(base)), at(destination(lcand)), at(destination(onext(lcand)))) > 0) {
                    const Edge following = onext(lcand);
                    remove(lcand);
                    lcand = following;
                }
            Edge rcand = oprev(base);
            if (valid(rcand, base))
                while (in_circle(at(destination(base)), at(origin(base)), at(destination(rcand)), at(destination(oprev(rcand)))) > 0) {
                    const Edge following = oprev(rcand);
                    remove(rcand);
                    rcand = following;
                }
            const bool left_valid = valid(lcand, base), right_valid = valid(rcand, base);
            if (!left_valid && !right_valid) break;
            if (!left_valid ||
                (right_valid && in_circle(at(destination(lcand)), at(origin(lcand)), at(origin(rcand)), at(destination(rcand))) > 0))
                base = connect(rcand, sym(base));
            else
                base = connect(sym(base), sym(lcand));
        }
        return {ldo, rdo};
    }

    std::size_t slots() const { return next_.size() / 4; }
    bool alive(std::size_t slot) const { return alive_[slot] != 0; }

  private:
    Edge make(std::uint32_t from, std::uint32_t to) {
        Edge e;
        if (!free_.empty()) {
            e = free_.back();
            free_.pop_back();
        } else {
            e = static_cast<Edge>(next_.size());
            next_.resize(next_.size() + 4);
            origin_.resize(origin_.size() + 4);
            alive_.push_back(0);
        }
        next_[e] = e, next_[e + 1] = e + 3, next_[e + 2] = e + 2, next_[e + 3] = e + 1;
        origin_[e] = from, origin_[e + 2] = to;
        alive_[e / 4] = 1;
        return e;
    }

    void splice(Edge a, Edge b) {
        const Edge alpha = rot(next_[a]), beta = rot(next_[b]);
        std::swap(next_[a], next_[b]);
        std::swap(next_[alpha], next_[beta]);
    }

    // A new edge from the destination of a to the origin of b, so that all three have the same left face.
    Edge connect(Edge a, Edge b) {
        const Edge e = make(destination(a), origin(b));
        splice(e, lnext(a));
        splice(sym(e), b);
        return e;
    }

    void remove(Edge e) {
        splice(e, oprev(e));
        splice(sym(e), oprev(sym(e)));
        alive_[e / 4] = 0;
        free_.push_back(e & ~Edge(3));
    }

    bool left_of(std::uint32_t point, Edge e) const { return orient(at(point), at(origin(e)), at(destination(e))) > 0; }
    bool right_of(std::uint32_t point, Edge e) const { return orient(at(point), at(destination(e)), at(origin(e))) > 0; }
    // Whether an edge out of an end of the base reaches a point above the base.
    bool valid(Edge e, Edge base) const { return right_of(destination(e), base); }

    const std::vector<Point> &points_;
    std::vector<Edge> next_, free_;
    std::vector<std::uint32_t> origin_;
    std::vector<unsigned char> alive_;
};

void check_site(const Point &point) {
    if (point.x > kMaxCoordinate || point.x < -kMaxCoordinate || point.y > kMaxCoordinate || point.y < -kMaxCoordinate)
        reject("A coordinate is beyond the range of the polygon kernel");
}

// A fraction with a positive denominator, or a place without end in either direction.
struct Fraction {
    Int128 numerator = 0, denominator = 1;
    int endless = 0;  // -1 before everything, 1 after everything
};

Fraction fraction(Int128 numerator, Int128 denominator) {
    if (denominator < Int128(0)) numerator = Int128(0) - numerator, denominator = Int128(0) - denominator;
    Fraction out;
    out.numerator = numerator, out.denominator = denominator;
    return out;
}

// -1, 0 or 1 as p is before, at or after q.
int compare(const Fraction &p, const Fraction &q) {
    if (p.endless || q.endless) return p.endless < q.endless ? -1 : p.endless > q.endless ? 1 : 0;
    return compare_products(p.numerator, q.denominator, q.numerator, p.denominator);
}

// Where along the bisector of a and b, as described at the top of this file, the centre of the circle
// through a, b and c lies.
Fraction centre(const Point &a, const Point &b, const Point &c) {
    const Coord bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y;
    const Int128 twice = cross(bx, by, cx, cy);
    return fraction(dot(cx, cy, cx - bx, cy - by), twice + twice);
}

}  // namespace

Triangulation delaunay(const std::vector<Point> &given) {
    if (given.size() > kMaxSites) reject("Too many points for a triangulation");
    Triangulation result;
    result.points = given;
    for (const Point &point : result.points) check_site(point);
    std::sort(result.points.begin(), result.points.end());
    result.points.erase(std::unique(result.points.begin(), result.points.end()), result.points.end());
    const std::uint32_t count = static_cast<std::uint32_t>(result.points.size());
    if (count < 2) return result;

    Mesh mesh(result.points);
    mesh.build(0, count);

    // The triangle on the left of each directed edge that has one. A face is a triangle when three steps
    // round it return to the start and its corners turn to the left; the face outside the hull turns to
    // the right, and round points on one line there is no turn at all.
    const std::size_t slots = mesh.slots();
    std::vector<std::uint32_t> face(2 * slots, kNoTriangle);  // of slot s: 2 s for the edge, 2 s + 1 for its reverse
    const auto face_of = [&](Mesh::Edge e) -> std::uint32_t & { return face[e / 4 * 2 + ((e & 2) ? 1 : 0)]; };
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (!mesh.alive(slot)) continue;
        for (const Mesh::Edge e : {static_cast<Mesh::Edge>(4 * slot), static_cast<Mesh::Edge>(4 * slot + 2)}) {
            if (face_of(e) != kNoTriangle) continue;
            const Mesh::Edge second = mesh.lnext(e), third = mesh.lnext(second);
            if (mesh.lnext(third) != e) continue;
            const std::uint32_t a = mesh.origin(e), b = mesh.origin(second), c = mesh.origin(third);
            if (orient(mesh.at(a), mesh.at(b), mesh.at(c)) <= 0) continue;
            const std::uint32_t id = static_cast<std::uint32_t>(result.triangles.size());
            face_of(e) = face_of(second) = face_of(third) = id;
            result.triangles.push_back(a < b && a < c ? std::array<std::uint32_t, 3>{a, b, c}
                                       : b < c        ? std::array<std::uint32_t, 3>{b, c, a}
                                                      : std::array<std::uint32_t, 3>{c, a, b});
        }
    }
    // Triangles in order, and the edges naming them by their new places.
    std::vector<std::uint32_t> order(result.triangles.size()), place(result.triangles.size());
    std::iota(order.begin(), order.end(), std::uint32_t{0});
    std::sort(order.begin(), order.end(), [&](std::uint32_t s, std::uint32_t t) { return result.triangles[s] < result.triangles[t]; });
    std::vector<std::array<std::uint32_t, 3>> sorted(result.triangles.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        sorted[i] = result.triangles[order[i]];
        place[order[i]] = static_cast<std::uint32_t>(i);
    }
    result.triangles.swap(sorted);
    const auto placed = [&](std::uint32_t id) { return id == kNoTriangle ? kNoTriangle : place[id]; };
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (!mesh.alive(slot)) continue;
        Mesh::Edge e = static_cast<Mesh::Edge>(4 * slot);
        if (mesh.origin(e) > mesh.destination(e)) e = Mesh::sym(e);
        result.edges.push_back({mesh.origin(e), mesh.destination(e), placed(face_of(e)), placed(face_of(Mesh::sym(e)))});
    }
    std::sort(result.edges.begin(), result.edges.end(),
              [](const Triangulation::Edge &s, const Triangulation::Edge &t) { return s.a != t.a ? s.a < t.a : s.b < t.b; });
    return result;
}

std::vector<VoronoiEdge> voronoi_edges(const Triangulation &triangulation, const Point &low, const Point &high) {
    check_site(low);
    check_site(high);
    std::vector<VoronoiEdge> result;
    if (low.x > high.x || low.y > high.y) return result;
    const std::vector<Point> &points = triangulation.points;
    const auto third = [&](std::uint32_t triangle, std::uint32_t a, std::uint32_t b) {
        for (const std::uint32_t corner : triangulation.triangles[triangle])
            if (corner != a && corner != b) return corner;
        return a;
    };
    for (const Triangulation::Edge &edge : triangulation.edges) {
        const Point &a = points[edge.a], &b = points[edge.b];
        // The bisector is m + t d, with 2 m = a + b and d the direction of ab turned to the left.
        const Coord sx = a.x + b.x, sy = a.y + b.y, dx = -(b.y - a.y), dy = b.x - a.x;
        Fraction from, to;
        from.endless = -1, to.endless = 1;
        if (edge.right != kNoTriangle) from = centre(a, b, points[third(edge.right, edge.a, edge.b)]);
        if (edge.left != kNoTriangle) to = centre(a, b, points[third(edge.left, edge.a, edge.b)]);
        // Cut to the box: low <= m + t d <= high in each coordinate, that is 2 low - s <= 2 t d <= 2 high - s.
        bool any = true;
        const auto cut = [&](Coord s, Coord d, Coord least, Coord most) {
            const Int128 lower = Int128(2 * least - s), upper = Int128(2 * most - s);
            if (d == 0) {
                if (lower > Int128(0) || upper < Int128(0)) any = false;
                return;
            }
            const Fraction first = fraction(d > 0 ? lower : upper, Int128(2 * d)), second = fraction(d > 0 ? upper : lower, Int128(2 * d));
            if (compare(first, from) > 0) from = first;
            if (compare(second, to) < 0) to = second;
        };
        cut(sx, dx, low.x, high.x);
        cut(sy, dy, low.y, high.y);
        if (!any || compare(from, to) >= 0) continue;
        // m + t d = (s denominator + 2 numerator d) / (2 denominator), rounded to the nearest whole number.
        const auto place = [&](const Fraction &t) {
            const Int128 twice = t.denominator + t.denominator;
            return Point{narrow(round_divide(Int128(sx) * t.denominator + (t.numerator + t.numerator) * Int128(dx), twice)),
                         narrow(round_divide(Int128(sy) * t.denominator + (t.numerator + t.numerator) * Int128(dy), twice))};
        };
        VoronoiEdge out;
        out.ends = {place(from), place(to)};
        if (out.ends[0] == out.ends[1]) continue;
        if (out.ends[1] < out.ends[0]) std::swap(out.ends[0], out.ends[1]);
        out.sites = {edge.a, edge.b};
        result.push_back(out);
    }
    std::sort(result.begin(), result.end(), [](const VoronoiEdge &s, const VoronoiEdge &t) {
        if (s.ends[0] != t.ends[0]) return s.ends[0] < t.ends[0];
        if (s.ends[1] != t.ends[1]) return s.ends[1] < t.ends[1];
        return s.sites < t.sites;
    });
    return result;
}

}  // namespace polygon
}  // namespace kerfkite
