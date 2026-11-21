// The arrangement: any closed curves on the grid resolved into a region.
//
// 1. resolve(). Every pair of edges that have a point in common, other than an end point they share, is
//    split there: at an end point of one lying on the other, exactly; at the ends of a shared length,
//    exactly; where two edges cross, at the crossing rounded to the nearest grid point, through which
//    both edges are bent. Bending can make an edge meet another it missed before, so the pieces are
//    tested again, round after round, until a round finds nothing. The edges then meet only at shared
//    end points, which is decided by exact arithmetic, not assumed.
// 2. wind(). A sweep from left to right gives the winding number on each side of every edge.
// 3. region(). Edges with the inside on one side only are the boundary. They are followed into closed
//    walks, turning at each vertex so as to keep to one face of the inside; a walk that passes a vertex
//    twice is cut there into simple rings; counter-clockwise rings are outer rings, clockwise rings are
//    holes, and a second sweep finds the outer ring each hole belongs to.
#include "kernel.hpp"

#include <array>
#include <set>

#include "kerfkite/cam.hpp"

namespace kerfkite {
namespace polygon {
namespace detail {

namespace {

// Rounds of splitting before giving up. Tests record how many are used: see docs/POLYGON-KERNEL.md.
constexpr int kMaxRounds = 200;

struct Split {
    std::uint32_t edge;
    Point at;
};

bool edge_before(const Edge &s, const Edge &t) { return s.a < t.a || (s.a == t.a && s.b < t.b); }

// Where the lines of two crossing edges meet, rounded to the grid.
Point crossing(const Edge &s, const Edge &t) {
    const Coord rx = s.b.x - s.a.x, ry = s.b.y - s.a.y, qx = t.b.x - t.a.x, qy = t.b.y - t.a.y;
    const Int128 numerator = cross(t.a.x - s.a.x, t.a.y - s.a.y, qx, qy), denominator = cross(rx, ry, qx, qy);
    return {s.a.x + narrow(round_divide(Int128(rx) * numerator, denominator)),
            s.a.y + narrow(round_divide(Int128(ry) * numerator, denominator))};
}

// Whether an edge passes through the inside of the unit square centred on a grid point that is not one of
// its ends: the point is then less than half a unit from the edge, across and along. In coordinates
// measured from the point, the square's corners are all on one side of the edge's line, or on it, exactly
// when twice the cross product of the ends is at least the sum of the edge's extents. An edge that only
// touches a corner of the square is not counted: it touches the squares of four grid points alike, and
// bending it to any of them would send the next round back again.
bool passes(const Edge &e, const Point &p) {
    if (p.x < e.a.x || p.x > e.b.x || p.y < std::min(e.a.y, e.b.y) || p.y > std::max(e.a.y, e.b.y)) return false;
    if (p == e.a || p == e.b) return false;
    const Int128 area = cross(p, e.a, e.b);
    const Coord dy = e.b.y - e.a.y;
    const Int128 reach = Int128((e.b.x - e.a.x) + (dy < 0 ? -dy : dy));
    return (area < Int128(0) ? Int128(0) - area : area) * Int128(2) < reach;
}

// Where two edges cross at a point inside both, rounded to the grid; false when they do not.
bool crosses(const Edge &s, const Edge &t, Point &at) {
    if (s.b.x < t.a.x || t.b.x < s.a.x) return false;
    if (std::max(s.a.y, s.b.y) < std::min(t.a.y, t.b.y) || std::max(t.a.y, t.b.y) < std::min(s.a.y, s.b.y)) return false;
    if (orient(s.a, s.b, t.a) * orient(s.a, s.b, t.b) >= 0) return false;
    if (orient(t.a, t.b, s.a) * orient(t.a, t.b, s.b) >= 0) return false;
    // The same grid point whichever edge is named first.
    at = edge_before(s, t) ? crossing(s, t) : crossing(t, s);
    return true;
}

// The sweep described in kernel.hpp.
template <class E, class F> void sweep(const std::vector<E> &edges, F &&visit) {
    const std::uint32_t n = static_cast<std::uint32_t>(edges.size());
    std::vector<std::uint32_t> ends(n);
    for (std::uint32_t i = 0; i < n; ++i) ends[i] = i;
    std::sort(ends.begin(), ends.end(), [&](std::uint32_t s, std::uint32_t t) { return edges[s].b < edges[t].b; });
    // Of two edges that cross the sweep line and meet nowhere but at an end, which is the lower. The one
    // that starts later starts strictly above or below the other, or they start together and fan out.
    struct Lower {
        const std::vector<E> *edges;
        bool operator()(std::uint32_t s, std::uint32_t t) const {
            if (s == t) return false;
            const E &p = (*edges)[s], &q = (*edges)[t];
            if (p.a == q.a) return cross(p.a, p.b, q.b) > Int128(0);
            if (p.a < q.a) return cross(p.a, p.b, q.a) > Int128(0);
            return cross(q.a, q.b, p.a) < Int128(0);
        }
    };
    std::set<std::uint32_t, Lower> status(Lower{&edges});
    std::vector<typename std::set<std::uint32_t, Lower>::iterator> where(n);
    std::vector<std::uint32_t> group;
    std::uint32_t i = 0, j = 0;
    while (i < n || j < n) {
        const Point at = (j >= n || (i < n && edges[i].a < edges[ends[j]].b)) ? edges[i].a : edges[ends[j]].b;
        for (; j < n && edges[ends[j]].b == at; ++j) status.erase(where[ends[j]]);
        group.clear();
        for (; i < n && edges[i].a == at; ++i) group.push_back(i);
        if (group.empty()) continue;
        std::sort(group.begin(), group.end(), [&](std::uint32_t s, std::uint32_t t) { return cross(at, edges[s].b, edges[t].b) > Int128(0); });
        auto place = status.insert(group[0]).first;
        const std::uint32_t under = place == status.begin() ? kNone : *std::prev(place);
        where[group[0]] = place;
        for (std::size_t k = 1; k < group.size(); ++k) {
            place = status.insert(std::next(place), group[k]);
            where[group[k]] = place;
        }
        visit(group, under);
    }
}

// A ring cut from a boundary walk.
struct Cycle {
    Ring points;
    Int128 area;         // twice the signed area
    std::uint32_t walk;  // the walk it was cut from
    std::int32_t owner;  // for a hole: the index of its outer ring, once known
};

// A boundary edge for the sweep that finds which outer ring a hole lies in.
struct Rim {
    Point a, b;
    std::uint32_t cycle;
    bool lowest;  // starts at the smallest vertex of its cycle
};

// Drops vertices on the straight line between their neighbours, except where another ring touches (so that
// both rings keep the vertex they share), and starts the ring at its smallest vertex.
void tidy(Ring &ring, const std::vector<Point> &shared) {
    if (ring.size() > 3) {
        Ring kept;
        kept.reserve(ring.size());
        const std::size_t n = ring.size();
        for (std::size_t i = 0; i < n; ++i) {
            // Judged against the last vertex kept. A dropped vertex lies between its neighbours, so
            // judging the first vertex against the unreduced end of the ring gives the same answer.
            const Point &before = kept.empty() ? ring[n - 1] : kept.back();
            if (orient(before, ring[i], ring[(i + 1) % n]) != 0 || std::binary_search(shared.begin(), shared.end(), ring[i])) kept.push_back(ring[i]);
        }
        if (kept.size() >= 3) ring.swap(kept);
    }
    std::rotate(ring.begin(), std::min_element(ring.begin(), ring.end()), ring.end());
}

bool ring_before(const Ring &s, const Ring &t) { return std::lexicographical_compare(s.begin(), s.end(), t.begin(), t.end()); }

}  // namespace

void Arrangement::add(const Ring &ring, int operand) {
    const std::size_t n = ring.size();
    if (n < 2) return;
    if (edges_.size() + n > kMaxEdges) reject("Geometry has too many edges for the polygon kernel");
    for (std::size_t i = 0; i < n; ++i) {
        const Point &p = ring[i], &q = ring[i + 1 == n ? 0 : i + 1];
        check_range(p);
        if (p == q) continue;
        Edge e{};
        const bool forward = p < q;
        e.a = forward ? p : q;
        e.b = forward ? q : p;
        e.w[operand] = forward ? 1 : -1;
        e.fresh = 1;
        edges_.push_back(e);
    }
}

void Arrangement::add(const PolygonSet &region, int operand) {
    each_ring(region, [&](const Ring &ring) { add(ring, operand); });
}

// Sorts the edges, joins equal ones by adding their windings, and drops those that cancel.
void Arrangement::merge() {
    std::sort(edges_.begin(), edges_.end(), edge_before);
    std::size_t out = 0;
    for (std::size_t i = 0; i < edges_.size();) {
        Edge e = edges_[i];
        std::size_t j = i + 1;
        for (; j < edges_.size() && edges_[j].a == e.a && edges_[j].b == e.b; ++j) {
            e.w[0] += edges_[j].w[0];
            e.w[1] += edges_[j].w[1];
        }
        e.fresh = 0;
        if (e.w[0] != 0 || e.w[1] != 0) edges_[out++] = e;
        i = j;
    }
    edges_.resize(out);
}

void Arrangement::resolve() {
    std::vector<Split> splits;
    std::vector<std::uint32_t> stamp;
    std::vector<Point> points;
    for (rounds_ = 0;; ++rounds_) {
        if (rounds_ == kMaxRounds) reject("The polygon kernel could not resolve this geometry");
        const std::uint32_t n = static_cast<std::uint32_t>(edges_.size());
        Grid<Edge> grid;
        grid.build(edges_);
        stamp.assign(n, kNone);
        splits.clear();
        // Each pair with at least one new edge is looked at once; two old edges were looked at in an earlier
        // round and neither has changed.
        // 1. Where the two cross, the crossing is rounded to a grid point and both are bent through it.
        // 2. An edge that passes within half a unit of a vertex of the other is bent through it: that
        //    covers a vertex lying on an edge and the ends of a shared length.
        points.clear();
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!edges_[i].fresh) continue;
            grid.near_segment(edges_[i].a, edges_[i].b, [&](std::uint32_t j) {
                if (j == i || stamp[j] == i) return;
                stamp[j] = i;
                if (edges_[j].fresh && j < i) return;
                const Edge &s = edges_[i], &t = edges_[j];
                Point at;
                if (crosses(s, t, at)) {
                    points.push_back(at);
                    if (at != s.a && at != s.b) splits.push_back({i, at});
                    if (at != t.a && at != t.b) splits.push_back({j, at});
                }
                if (passes(s, t.a)) splits.push_back({i, t.a});
                if (passes(s, t.b)) splits.push_back({i, t.b});
                if (passes(t, s.a)) splits.push_back({j, s.a});
                if (passes(t, s.b)) splits.push_back({j, s.b});
            });
        }
        // 3. Every edge that passes within half a unit of a rounded crossing is bent through it, not only
        //    the two that cross there. Without this, two edges at a fine angle chase each other along
        //    their length, a unit a round.
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        for (const Point &p : points)
            grid.near_segment({p.x - 1, p.y - 1}, {p.x + 1, p.y + 1}, [&](std::uint32_t j) {
                if (passes(edges_[j], p)) splits.push_back({j, p});
            });
        for (Edge &e : edges_) e.fresh = 0;
        if (splits.empty()) break;
        std::sort(splits.begin(), splits.end(), [](const Split &s, const Split &t) { return s.edge != t.edge ? s.edge < t.edge : s.at < t.at; });
        for (std::size_t first = 0; first < splits.size();) {
            std::size_t last = first;
            const std::uint32_t index = splits[first].edge;
            const Edge parent = edges_[index];
            points.clear();
            for (; last < splits.size() && splits[last].edge == index; ++last)
                if (points.empty() || points.back() != splits[last].at) points.push_back(splits[last].at);
            first = last;
            // In order along the edge. A point is beside the edge, not always on it, so the order is that
            // of the feet of the points on the edge; equal feet fall back on the sorting order.
            const Coord dx = parent.b.x - parent.a.x, dy = parent.b.y - parent.a.y;
            std::sort(points.begin(), points.end(), [&](const Point &p, const Point &q) {
                const Int128 fp = dot(p.x - parent.a.x, p.y - parent.a.y, dx, dy), fq = dot(q.x - parent.a.x, q.y - parent.a.y, dx, dy);
                return fp != fq ? fp < fq : p < q;
            });
            points.push_back(parent.b);
            if (edges_.size() + points.size() > kMaxEdges) reject("Geometry has too many edges for the polygon kernel");
            Point from = parent.a;
            bool replaced = false;
            for (const Point &to : points) {
                if (to == from) continue;
                Edge piece = parent;
                piece.fresh = 1;
                if (from < to) {
                    piece.a = from, piece.b = to;
                } else {
                    piece.a = to, piece.b = from;
                    piece.w[0] = -parent.w[0], piece.w[1] = -parent.w[1];
                }
                if (replaced) {
                    edges_.push_back(piece);
                } else {
                    edges_[index] = piece;
                    replaced = true;
                }
                from = to;
            }
        }
        // Pieces of different edges bent through the same points are often the same piece. Joining them
        // now keeps a knot of many edges through one place from being tested against itself in full.
        const auto old_end = std::partition(edges_.begin(), edges_.end(), [](const Edge &e) { return e.fresh == 0; });
        std::sort(old_end, edges_.end(), edge_before);
        auto out = old_end;
        for (auto it = old_end; it != edges_.end();) {
            Edge e = *it;
            for (++it; it != edges_.end() && it->a == e.a && it->b == e.b; ++it) e.w[0] += it->w[0], e.w[1] += it->w[1];
            if (e.w[0] != 0 || e.w[1] != 0) *out++ = e;
        }
        edges_.erase(out, edges_.end());
    }
    merge();
}

void Arrangement::wind() {
    below_.assign(edges_.size(), std::array<std::int32_t, 2>{0, 0});
    sweep(edges_, [&](const std::vector<std::uint32_t> &group, std::uint32_t under) {
        std::array<std::int32_t, 2> winding{0, 0};
        if (under != kNone) winding = {below_[under][0] + edges_[under].w[0], below_[under][1] + edges_[under].w[1]};
        for (const std::uint32_t e : group) {
            below_[e] = winding;
            winding[0] += edges_[e].w[0];
            winding[1] += edges_[e].w[1];
        }
    });
}

PolygonSet Arrangement::assemble(std::vector<Segment> &boundary) {
    PolygonSet result;
    const std::uint32_t n = static_cast<std::uint32_t>(boundary.size());
    if (n == 0) return result;
    // Edges grouped by the vertex they leave.
    std::sort(boundary.begin(), boundary.end(), [](const Segment &s, const Segment &t) { return s[0] < t[0] || (s[0] == t[0] && s[1] < t[1]); });
    std::vector<std::uint32_t> first;   // of each vertex, its first edge
    std::vector<std::uint32_t> vertex(n);  // of each edge, the vertex it leaves
    for (std::uint32_t e = 0; e < n; ++e) {
        if (e == 0 || boundary[e][0] != boundary[e - 1][0]) first.push_back(e);
        vertex[e] = static_cast<std::uint32_t>(first.size() - 1);
    }
    const std::uint32_t vertices = static_cast<std::uint32_t>(first.size());
    first.push_back(n);
    std::vector<Point> shared;  // vertices where the boundary passes more than once, in sorted order
    for (std::uint32_t v = 0; v < vertices; ++v)
        if (first[v + 1] - first[v] > 1) shared.push_back(boundary[first[v]][0]);
    const auto vertex_at = [&](const Point &p) {
        std::uint32_t low = 0, high = vertices;
        while (low < high) {
            const std::uint32_t middle = low + (high - low) / 2;
            if (boundary[first[middle]][0] < p)
                low = middle + 1;
            else
                high = middle;
        }
        return low;
    };
    // The edge that follows each edge: the inside is on the left, so on reaching a vertex the walk takes
    // the first edge leaving it clockwise from the way it came, which keeps it on one face of the inside.
    std::vector<std::uint32_t> next(n);
    for (std::uint32_t e = 0; e < n; ++e) {
        const Point &from = boundary[e][0], &at = boundary[e][1];
        const std::uint32_t v = vertex_at(at);
        if (v >= vertices || boundary[first[v]][0] != at) reject("The polygon kernel met an open boundary");
        std::uint32_t best = first[v];
        if (first[v + 1] - first[v] > 1) {
            const Coord rx = from.x - at.x, ry = from.y - at.y;
            // The angle of a leaving edge, counter-clockwise from the way back, as a half turn and a slope.
            const auto half = [&](const Point &to) {
                const Int128 c = cross(rx, ry, to.x - at.x, to.y - at.y);
                return c > Int128(0) || (c == Int128(0) && dot(rx, ry, to.x - at.x, to.y - at.y) > Int128(0)) ? 0 : 1;
            };
            for (std::uint32_t k = first[v] + 1; k < first[v + 1]; ++k) {
                const Point &p = boundary[best][1], &q = boundary[k][1];
                const int hp = half(p), hq = half(q);
                const bool further = hp != hq ? hq > hp : cross(at, p, q) > Int128(0);
                if (further) best = k;
            }
        }
        next[e] = best;
    }
    // Walks, cut into simple rings wherever one passes a vertex a second time.
    std::vector<Cycle> cycles;
    std::vector<char> used(n, 0);
    std::vector<std::int32_t> position(vertices, -1);  // of a vertex on the stack
    std::vector<std::uint32_t> stack;
    std::uint32_t walks = 0;
    const auto cut = [&](std::size_t from) {
        Cycle cycle;
        cycle.walk = walks;
        cycle.owner = -1;
        for (std::size_t k = from; k < stack.size(); ++k) {
            cycle.points.push_back(boundary[stack[k]][0]);
            position[vertex[stack[k]]] = -1;
        }
        stack.resize(from);
        cycle.area = twice_area(cycle.points);
        cycles.push_back(std::move(cycle));
    };
    std::vector<std::uint32_t> walk_begin;  // the first ring of each walk; its rings follow one another
    for (std::uint32_t start = 0; start < n; ++start) {
        if (used[start]) continue;
        walk_begin.push_back(static_cast<std::uint32_t>(cycles.size()));
        std::uint32_t e = start, steps = 0;
        do {
            if (used[e] || ++steps > n) reject("The polygon kernel met a boundary that does not close");
            used[e] = 1;
            const std::uint32_t v = vertex[e];
            if (position[v] >= 0) cut(static_cast<std::size_t>(position[v]));
            position[v] = static_cast<std::int32_t>(stack.size());
            stack.push_back(e);
            e = next[e];
        } while (e != start);
        cut(0);
        ++walks;
    }
    // Outer rings and holes. A walk round the outside of a face gives one counter-clockwise ring and,
    // where the face pinches, holes that belong to it. A walk round a hole gives clockwise rings only.
    const std::uint32_t count = static_cast<std::uint32_t>(cycles.size());
    walk_begin.push_back(count);
    std::vector<std::int32_t> outer_of_walk(walks, -1);
    std::vector<char> ambiguous(walks, 0);
    for (std::uint32_t c = 0; c < count; ++c) {
        if (cycles[c].area > Int128(0)) {
            if (outer_of_walk[cycles[c].walk] >= 0) ambiguous[cycles[c].walk] = 1;
            outer_of_walk[cycles[c].walk] = static_cast<std::int32_t>(c);
        }
    }
    std::vector<std::uint32_t> orphans;  // walks whose rings are all holes
    for (std::uint32_t w = 0; w < walks; ++w)
        if (outer_of_walk[w] < 0 || ambiguous[w]) orphans.push_back(w);
    for (std::uint32_t c = 0; c < count; ++c)
        if (!(cycles[c].area > Int128(0)) && outer_of_walk[cycles[c].walk] >= 0 && !ambiguous[cycles[c].walk])
            cycles[c].owner = outer_of_walk[cycles[c].walk];
    if (!orphans.empty()) {
        // Such a walk goes round a hole, or round several that touch, in some face. Nothing of the
        // boundary reaches the walk's smallest vertex from the left, so the boundary edge directly below
        // that vertex borders the same face. Its ring is the face's outer ring or another of its holes,
        // which, starting further left, already knows its outer ring when walks are taken from left to
        // right. (The smallest vertex of one ring of the walk would not do: where two of its rings
        // touch, the other can come up to that vertex from the left and hide what is below.)
        std::vector<std::uint32_t> lowest_cycle(walks, kNone);
        std::vector<Point> lowest(walks);
        for (std::uint32_t c = 0; c < count; ++c) {
            const std::uint32_t w = cycles[c].walk;
            const Point least = *std::min_element(cycles[c].points.begin(), cycles[c].points.end());
            if (lowest_cycle[w] == kNone || least < lowest[w]) lowest_cycle[w] = c, lowest[w] = least;
        }
        std::vector<Rim> rims;
        rims.reserve(n);
        for (std::uint32_t c = 0; c < count; ++c) {
            const Ring &points = cycles[c].points;
            const bool has_lowest = lowest_cycle[cycles[c].walk] == c;
            for (std::size_t k = 0; k < points.size(); ++k) {
                const Point &p = points[k], &q = points[k + 1 == points.size() ? 0 : k + 1];
                rims.push_back(p < q ? Rim{p, q, c, has_lowest && p == lowest[cycles[c].walk]} : Rim{q, p, c, false});
            }
        }
        std::sort(rims.begin(), rims.end(), [](const Rim &s, const Rim &t) { return s.a < t.a; });
        std::vector<std::uint32_t> under_walk(walks, kNone);
        sweep(rims, [&](const std::vector<std::uint32_t> &group, std::uint32_t under) {
            for (const std::uint32_t e : group)
                if (rims[e].lowest && under != kNone) under_walk[cycles[rims[e].cycle].walk] = rims[under].cycle;
        });
        std::sort(orphans.begin(), orphans.end(), [&](std::uint32_t s, std::uint32_t t) { return lowest[s] < lowest[t]; });
        std::vector<std::int32_t> owner_of_walk(walks, -1);
        for (const std::uint32_t w : orphans) {
            const std::uint32_t under = under_walk[w];
            if (under == kNone) reject("The polygon kernel met a hole outside every outline");
            owner_of_walk[w] = cycles[under].area > Int128(0) ? static_cast<std::int32_t>(under) : cycles[under].owner;
            if (owner_of_walk[w] < 0) reject("The polygon kernel met a hole outside every outline");
            for (std::uint32_t c = walk_begin[w]; c < walk_begin[w + 1]; ++c) cycles[c].owner = owner_of_walk[w];
        }
    }
    std::vector<std::int32_t> slot(count, -1);
    for (std::uint32_t c = 0; c < count; ++c) {
        if (!(cycles[c].area > Int128(0))) continue;
        slot[c] = static_cast<std::int32_t>(result.size());
        result.emplace_back();
        tidy(cycles[c].points, shared);
        result.back().shell = std::move(cycles[c].points);
    }
    for (std::uint32_t c = 0; c < count; ++c) {
        if (cycles[c].area > Int128(0)) continue;
        tidy(cycles[c].points, shared);
        result[static_cast<std::size_t>(slot[static_cast<std::size_t>(cycles[c].owner)])].holes.push_back(std::move(cycles[c].points));
    }
    for (Polygon &polygon : result) std::sort(polygon.holes.begin(), polygon.holes.end(), ring_before);
    std::sort(result.begin(), result.end(), [](const Polygon &s, const Polygon &t) { return ring_before(s.shell, t.shell); });
    return result;
}

}  // namespace detail
}  // namespace polygon
}  // namespace kerfkite
