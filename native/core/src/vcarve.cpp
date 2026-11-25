// V-carving: the sampled medial axis of studio/operations.py on KerfKite's own polygon kernel
// (kerfkite/polygon.hpp).
//
// This file is the one implementation. The command-line tool reaches it through generate(), and Studio's
// Python through the "operation" command of its compiled extension (op "vcarve"), so both carve the same
// paths. Studio keeps its Shapely code only for an installation without the extension.
//
// The method is the reference's:
//
// 1. Every ring of the region is sampled at equal steps of at most the sampling interval.
// 2. The Voronoi diagram of the samples is cut to the region. What remains approximates the medial axis,
//    with a spoke from the axis to the wall between each two neighbouring samples.
// 3. Each piece is divided into steps of at most the interval, and at each point the V-bit goes as deep as
//    lets it cut a circle of radius r = min(tool radius, distance to the wall - interval), no deeper than
//    the depth of cut. The interval taken off is the allowance for the sampling: between two points a step
//    apart the distance to the wall cannot fall by more than half the step, so the straight move between
//    them stays clear of the wall as well.
//
// What the kernel fixes, none of it visible in the reference's source:
//
// * The region is the even-odd region of the contours on the kernel's grid of a millionth of a millimetre
//   (contour_region), and every point of the result is a grid point, written as n / 1e6.
// * A ring is sampled from its smallest vertex in the kernel's direction. Lengths are square roots of
//   whole numbers; a sample is rounded to the grid.
// * The diagram is polygon::voronoi_edges: exact tests, its vertices rounded to the grid by one rule.
// * A piece is one Voronoi edge cut to the region by the kernel's line clipper. A piece shorter than a
//   tenth of the interval is dropped, as in the reference, and so is one that nowhere cuts deeper than
//   0.0001 mm.
// * The distance to the wall is polygon::BoundaryDistance: exact up to one square root and one division.
// * The tangent of half the bit's angle comes from the kernel's series for sine and cosine, not from the C
//   library, so the depths are the same on every platform.
// * Pieces that share an end are joined into longer paths, which the reference never did: it lifted the
//   tool between every two pieces. From an end where an odd number of pieces meet, taken in order of x and
//   then y, a path follows at each junction the unused piece that continues most nearly straight (of two
//   equally straight, the one whose far end is smaller), until none is left; what then remains is closed
//   loops, taken the same way. Every piece is cut once, in one path.
// * The paths are put in an order that keeps the travel between them short: the first starts at the
//   smallest end of any path; each next one is the path with an end nearest to where the last finished
//   (of two equally near, the smaller end point, then the earlier path), entered at that end.
// * The reference's limit on a job, 200,000 points, is applied here as the pieces are made, with the
//   generator's own message, so that a carve far beyond it is refused without being built.
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numeric>

#include "operations.hpp"
#include "polygon/kernel.hpp"

namespace kerfkite {
namespace detail {

namespace {

using polygon::Coord;
using polygon::Point;
using polygon::PolygonSet;
using polygon::Ring;
using polygon::detail::Int128;

// The most samples of the outlines, as the reference has it.
const std::size_t kSampleLimit = 12000;
// The generator refuses a job of more than 200,000 points (studio.cam: count * levels > 200000). Here the
// steps of the pieces are counted as they are made, and a carve has more points than steps, so a carve
// past this many steps is one the generator would refuse: it is refused here, before it is built. A carve
// just under it may still be refused by the generator, which counts the points.
const std::size_t kPointLimit = 200000;

// The length of the segment from a to b in grid units: the square root of an exact whole number.
double span(const Point &a, const Point &b) {
    const Coord dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(polygon::detail::to_double(polygon::detail::dot(dx, dy, dx, dy)));
}

Int128 squared(const Point &a, const Point &b) {
    const Coord dx = b.x - a.x, dy = b.y - a.y;
    return polygon::detail::dot(dx, dy, dx, dy);
}

// One Voronoi edge cut to the region and divided: its points from the smaller end to the larger, and the
// depth at each.
struct Piece {
    std::vector<Point> points;
    std::vector<double> depths;
    std::uint32_t ends[2];  // the junctions at its first and last point
};

// A path: pieces end to end.
struct Trail {
    std::vector<Point> points;
    std::vector<double> depths;
};

// The points of the rings, at equal steps of at most the interval along each.
std::vector<Point> samples(const PolygonSet &g, double resolution) {
    std::vector<Point> result;
    std::vector<double> lengths;
    polygon::detail::each_ring(g, [&](const Ring &ring) {
        const std::size_t n = ring.size();
        lengths.resize(n);
        double total = 0;
        for (std::size_t i = 0; i < n; ++i) {
            lengths[i] = span(ring[i], ring[i + 1 == n ? 0 : i + 1]);
            total += lengths[i];
        }
        const double count = std::ceil(total / polygon::kUnitsPerMm / resolution);
        if (!(static_cast<double>(result.size()) + count <= static_cast<double>(kSampleLimit)))
            throw ValueError("V-carve outline too complex; increase sampling interval");
        std::size_t edge = 0;
        double before = 0;  // the length of the ring before this edge
        for (double i = 0; i < count; ++i) {
            const double target = total * i / count;
            while (edge + 1 < n && before + lengths[edge] <= target) before += lengths[edge++];
            const Point &a = ring[edge], &b = ring[edge + 1 == n ? 0 : edge + 1];
            double t = (target - before) / lengths[edge];
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            result.push_back({a.x + static_cast<Coord>(std::floor(t * static_cast<double>(b.x - a.x) + 0.5)),
                              a.y + static_cast<Coord>(std::floor(t * static_cast<double>(b.y - a.y) + 0.5))});
        }
    });
    return result;
}

// Pieces joined end to end into paths, as described at the top of this file.
std::vector<Trail> join(std::vector<Piece> &pieces) {
    // The junctions: the distinct end points, in order.
    std::vector<Point> nodes;
    nodes.reserve(2 * pieces.size());
    for (const Piece &piece : pieces) nodes.push_back(piece.points.front()), nodes.push_back(piece.points.back());
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    const auto node_of = [&](const Point &p) { return static_cast<std::uint32_t>(std::lower_bound(nodes.begin(), nodes.end(), p) - nodes.begin()); };
    struct Link {
        std::uint32_t far, piece;
    };
    std::vector<std::uint32_t> start(nodes.size() + 1, 0);
    for (Piece &piece : pieces) {
        piece.ends[0] = node_of(piece.points.front()), piece.ends[1] = node_of(piece.points.back());
        ++start[piece.ends[0] + 1], ++start[piece.ends[1] + 1];
    }
    for (std::size_t i = 1; i < start.size(); ++i) start[i] += start[i - 1];
    std::vector<Link> links(start.back());
    std::vector<std::uint32_t> next(start.begin(), start.end() - 1);
    for (std::uint32_t i = 0; i < pieces.size(); ++i) {
        links[next[pieces[i].ends[0]]++] = {pieces[i].ends[1], i};
        links[next[pieces[i].ends[1]]++] = {pieces[i].ends[0], i};
    }
    // At each junction, the pieces in order of their far ends.
    for (std::size_t node = 0; node < nodes.size(); ++node)
        std::sort(links.begin() + start[node], links.begin() + start[node + 1],
                  [](const Link &s, const Link &t) { return s.far != t.far ? s.far < t.far : s.piece < t.piece; });
    std::vector<unsigned char> used(pieces.size(), 0);
    std::vector<std::uint32_t> unused(nodes.size());
    for (std::size_t node = 0; node < nodes.size(); ++node) unused[node] = start[node + 1] - start[node];

    std::vector<Trail> trails;
    const auto follow = [&](std::uint32_t from) {
        Trail trail;
        std::uint32_t here = from, before = from;
        bool first = true;
        for (;;) {
            // The unused piece that continues most nearly straight: the greatest cosine of the turn, which
            // for one direction of arrival is the greatest (arrival . departure) / |departure|.
            const Link *chosen = nullptr;
            double straightest = 0;
            for (std::uint32_t k = start[here]; k < start[here + 1]; ++k) {
                const Link &link = links[k];
                if (used[link.piece]) continue;
                if (first) {
                    chosen = &link;
                    break;
                }
                const Point &p = nodes[before], &q = nodes[here], &r = nodes[link.far];
                const double value = polygon::detail::to_double(polygon::detail::dot(q.x - p.x, q.y - p.y, r.x - q.x, r.y - q.y)) / span(q, r);
                if (!chosen || value > straightest) chosen = &link, straightest = value;
            }
            if (!chosen) break;
            const Piece &piece = pieces[chosen->piece];
            const bool forward = piece.ends[0] == here;
            const std::size_t count = piece.points.size();
            // The junction itself is already the last point of the path.
            for (std::size_t i = first ? 0 : 1; i < count; ++i) {
                const std::size_t at = forward ? i : count - 1 - i;
                trail.points.push_back(piece.points[at]);
                trail.depths.push_back(piece.depths[at]);
            }
            used[chosen->piece] = 1;
            --unused[here], --unused[chosen->far];
            before = here, here = chosen->far;
            first = false;
        }
        trails.push_back(std::move(trail));
    };
    for (std::uint32_t node = 0; node < nodes.size(); ++node)
        if (unused[node] % 2) follow(node);
    for (std::uint32_t node = 0; node < nodes.size(); ++node)
        while (unused[node]) follow(node);
    return trails;
}

// The ends of the paths in square buckets, to find the nearest to a point among those not yet taken.
class Ends {
  public:
    explicit Ends(const std::vector<Trail> &trails) {
        for (const Trail &trail : trails) {
            points_.push_back(trail.points.front());
            points_.push_back(trail.points.back());
        }
        x0_ = x1_ = points_[0].x, y0_ = y1_ = points_[0].y;
        for (const Point &p : points_) x0_ = std::min(x0_, p.x), x1_ = std::max(x1_, p.x), y0_ = std::min(y0_, p.y), y1_ = std::max(y1_, p.y);
        const double width = static_cast<double>(x1_ - x0_) + 1, height = static_cast<double>(y1_ - y0_) + 1;
        double size = std::max(1.0, std::floor(std::sqrt(width * height / static_cast<double>(points_.size()))));
        while ((std::floor(width / size) + 1) * (std::floor(height / size) + 1) > 4.0 * static_cast<double>(points_.size()) + 64) size *= 2;
        size_ = static_cast<Coord>(size);
        nx_ = (x1_ - x0_) / size_ + 1, ny_ = (y1_ - y0_) / size_ + 1;
        cells_.resize(static_cast<std::size_t>(nx_ * ny_));
        for (std::uint32_t i = 0; i < points_.size(); ++i) cells_[cell(points_[i])].push_back(i);
        left_ = points_.size();
    }

    // Both ends of a path are taken together.
    void take(std::uint32_t trail) {
        for (const std::uint32_t end : {2 * trail, 2 * trail + 1}) {
            std::vector<std::uint32_t> &bucket = cells_[cell(points_[end])];
            bucket.erase(std::find(bucket.begin(), bucket.end(), end));
            --left_;
        }
    }

    // The end nearest to a point: of two equally near the smaller point, and then the smaller number.
    std::uint32_t nearest(const Point &p) const {
        const Coord cx = column(p.x), cy = row(p.y);
        bool found = false;
        std::uint32_t best = 0;
        Int128 best_squared = 0;
        const auto look = [&](Coord c, Coord r) {
            if (c < 0 || c >= nx_ || r < 0 || r >= ny_) return;
            for (const std::uint32_t end : cells_[static_cast<std::size_t>(r * nx_ + c)]) {
                const Int128 away = squared(p, points_[end]);
                const bool better = !found || away < best_squared ||
                                    (away == best_squared && (points_[end] < points_[best] || (points_[end] == points_[best] && end < best)));
                if (better) found = true, best = end, best_squared = away;
            }
        };
        const Coord furthest = std::max(std::max(cx, nx_ - 1 - cx), std::max(cy, ny_ - 1 - cy));
        for (Coord ring = 0; ring <= furthest; ++ring) {
            if (ring == 0) {
                look(cx, cy);
            } else {
                for (Coord c = cx - ring; c <= cx + ring; ++c) look(c, cy - ring), look(c, cy + ring);
                for (Coord r = cy - ring + 1; r <= cy + ring - 1; ++r) look(cx - ring, r), look(cx + ring, r);
            }
            // An end in a bucket not yet looked at is at least `ring` buckets away, so it is further.
            if (found && best_squared < polygon::detail::multiply(ring * size_, ring * size_)) break;
        }
        return best;
    }

    bool empty() const { return left_ == 0; }

  private:
    Coord column(Coord x) const { return x <= x0_ ? 0 : std::min((x - x0_) / size_, nx_ - 1); }
    Coord row(Coord y) const { return y <= y0_ ? 0 : std::min((y - y0_) / size_, ny_ - 1); }
    std::size_t cell(const Point &p) const { return static_cast<std::size_t>(row(p.y) * nx_ + column(p.x)); }

    std::vector<Point> points_;
    std::vector<std::vector<std::uint32_t>> cells_;
    Coord x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0, size_ = 1, nx_ = 1, ny_ = 1;
    std::size_t left_ = 0;
};

// The paths in an order that keeps the travel between them short, each turned to start at the end nearest
// to where the last finished.
void arrange(std::vector<Trail> &trails) {
    if (trails.empty()) return;
    Ends ends(trails);
    std::vector<Trail> ordered;
    ordered.reserve(trails.size());
    // The smallest end of all.
    std::uint32_t at = 0;
    for (std::uint32_t i = 0; i < trails.size(); ++i)
        for (const std::uint32_t end : {2 * i, 2 * i + 1}) {
            const Point &p = end % 2 ? trails[i].points.back() : trails[i].points.front();
            const Point &q = at % 2 ? trails[at / 2].points.back() : trails[at / 2].points.front();
            if (p < q) at = end;
        }
    for (;;) {
        Trail &trail = trails[at / 2];
        if (at % 2) {
            std::reverse(trail.points.begin(), trail.points.end());
            std::reverse(trail.depths.begin(), trail.depths.end());
        }
        ends.take(at / 2);
        ordered.push_back(std::move(trail));
        if (ends.empty()) break;
        at = ends.nearest(ordered.back().points.back());
    }
    trails.swap(ordered);
}

}  // namespace

VCarve vcarve_paths(const PolygonSet &g, double resolution, double angle, double max_depth, double radius, double start) {
    // math.tan(math.radians(angle / 2)), from the kernel's series.
    double cosine, sine;
    cos_sin_degrees(angle / 2, cosine, sine);
    const double tangent = sine / cosine;
    // A carve that starts below the stock top (the plug of a V inlay): the outline is cut at that depth, and
    // every depth is that much greater. The bit is already that wide at the stock top, which leaves less of it.
    if (start > 0) {
        radius -= start * tangent;
        if (!(radius > 0)) throw ValueError("The V-bit is too narrow to carve from that start depth: use a wider bit or a smaller start depth");
    }

    const std::vector<Point> sites = samples(g, resolution);
    Point low{0, 0}, high{0, 0};
    bool any = false;
    polygon::detail::each_ring(g, [&](const Ring &ring) {
        for (const Point &p : ring) {
            if (!any) low = high = p, any = true;
            low.x = std::min(low.x, p.x), low.y = std::min(low.y, p.y), high.x = std::max(high.x, p.x), high.y = std::max(high.y, p.y);
        }
    });
    const std::vector<polygon::VoronoiEdge> edges = polygon::voronoi_edges(polygon::delaunay(sites), low, high);
    const polygon::LineClipper clipper(g);
    const polygon::BoundaryDistance wall(g);

    std::vector<Piece> pieces;
    std::size_t steps = 0;
    for (const polygon::VoronoiEdge &edge : edges)
        for (const polygon::Segment &part : clipper.clip(edge.ends[0], edge.ends[1])) {
            const double length = span(part[0], part[1]) / polygon::kUnitsPerMm;
            if (length < resolution * 0.1) continue;
            const double divided = std::ceil(length / resolution);
            const Coord count = divided > 1 ? static_cast<Coord>(divided) : 1;
            // A piece of more steps than a job may hold is not kept: it is only looked along, to learn
            // whether it cuts at all.
            const bool kept = static_cast<std::size_t>(count) <= kPointLimit;
            Piece piece;
            if (kept) {
                piece.points.reserve(static_cast<std::size_t>(count) + 1);
                piece.depths.reserve(static_cast<std::size_t>(count) + 1);
            }
            const Coord dx = part[1].x - part[0].x, dy = part[1].y - part[0].y;
            double lowest = 0;
            for (Coord i = 0; i <= count; ++i) {
                const Point p{part[0].x + polygon::detail::narrow(polygon::detail::round_divide(polygon::detail::multiply(dx, i), Int128(count))),
                              part[0].y + polygon::detail::narrow(polygon::detail::round_divide(polygon::detail::multiply(dy, i), Int128(count)))};
                // r = min(radius, max(0, distance - resolution)); z = -min(max_depth, r / tan)
                double r = wall.to(p) / polygon::kUnitsPerMm - resolution;
                if (!(r > 0)) r = 0;
                if (radius < r) r = radius;
                double deep = r / tangent;
                if (max_depth < deep) deep = max_depth;
                const double z = -deep;
                if (i == 0 || z < lowest) lowest = z;
                if (kept) {
                    piece.points.push_back(p);
                    piece.depths.push_back(start > 0 ? z - start : z);
                } else if (lowest < -1e-4) {
                    break;
                }
            }
            if (!(lowest < -1e-4)) continue;
            steps += static_cast<std::size_t>(count);
            if (steps > kPointLimit) throw ValueError("Job exceeds 200,000 cutting segments");
            pieces.push_back(std::move(piece));
        }

    std::vector<Trail> trails = join(pieces);
    arrange(trails);
    VCarve result;
    result.samples.reserve(sites.size());
    for (const Point &site : sites) result.samples.push_back(polygon::to_mm(site));
    result.paths.reserve(trails.size());
    result.depths.reserve(trails.size());
    for (Trail &trail : trails) {
        std::vector<std::array<double, 2>> path;
        path.reserve(trail.points.size());
        for (const Point &p : trail.points) path.push_back(polygon::to_mm(p));
        result.paths.push_back(std::move(path));
        result.depths.push_back(std::move(trail.depths));
    }
    return result;
}

}  // namespace detail
}  // namespace kerfkite
