// Where a helix may descend into a pocket: studio.router_entry._pocket_entries on KerfKite's own polygon
// kernel (kerfkite/polygon.hpp).
//
// This file is the one implementation. The command-line tool reaches it through generate(), and Studio's
// Python through the "operation" command of its compiled extension (op "helix"), so both place the same
// helix. Studio keeps its Shapely code only for an installation without the extension.
//
// For each pocket contour the helix is a circle of the entry radius about a centre, and a straight feed
// from the circle to the contour's first point (its seam). The tool has the cutter's radius, so:
//
// * the centre must be at least cutter radius + helix radius from every wall and island, and
// * the feed must stay where the cutter's centre may be, the pocket shrunk by the cutter radius.
//
// What the kernel fixes, none of it visible in the reference's source:
//
// * The pocket is the even-odd region of the contours on the kernel's grid of a millionth of a millimetre,
//   as it is for the pocket's own passes (contour_region).
// * Centres are taken from the region shrunk by cutter radius + helix radius + an allowance of
//   0.001 mm + 0.04% of the two radii, which the reference adds for the chords of a round offset. The
//   kernel's offsets already err on the safe side, so here the allowance is clear margin. It is kept so
//   that the separate check below never fails for a centre on the edge of that region.
// * The centre for a contour is its seam where the seam is itself far enough from the walls, and
//   otherwise the nearest point to the seam of a piece of that shrunk region: the pieces are tried nearest
//   first, in the kernel's order where two are equally near, and within a piece the first nearest point
//   in ring order. The point is a double, a grid position divided by 1e6, computed with + - * / alone, so
//   it is the same on every platform.
// * Each centre chosen is checked again against the pocket's own walls, by distance, not by offsetting.
// * The feed is checked with the kernel's line clipper between grid points: its start is rounded to the
//   grid, half a millionth of a millimetre at most.
// * The start angle is atan2 of the platform's library, as Python computes it; whether the feed is clear is
//   decided from the direction by a square root and a division, so the choice of centre does not depend on
//   the library.
#include <algorithm>
#include <cmath>
#include <numeric>

#include "operations.hpp"
#include "polygon/kernel.hpp"
#include "py_trig.hpp"

namespace kerfkite {
namespace detail {

namespace {

using polygon::Location;
using polygon::Point;
using polygon::Polygon;
using polygon::PolygonSet;
using polygon::Ring;

// A position in grid units that need not be a grid point.
struct Spot {
    double x, y;
};

// The square of the distance from p to the segment from a to b, and the nearest point of the segment.
double to_edge(const Point &a, const Point &b, const Spot &p, Spot &nearest) {
    const double ax = static_cast<double>(a.x), ay = static_cast<double>(a.y);
    const double dx = static_cast<double>(b.x - a.x), dy = static_cast<double>(b.y - a.y);
    const double wx = p.x - ax, wy = p.y - ay;
    const double along = wx * dx + wy * dy, length = dx * dx + dy * dy;
    if (along <= 0) {
        nearest = {ax, ay};
    } else if (along >= length) {
        nearest = {static_cast<double>(b.x), static_cast<double>(b.y)};
    } else {
        const double t = along / length;
        nearest = {ax + t * dx, ay + t * dy};
    }
    const double ex = p.x - nearest.x, ey = p.y - nearest.y;
    return ex * ex + ey * ey;
}

// The square of the distance from p to the nearest edge of a ring, when that is less than best: best and
// nearest are then replaced. The first of several equally near points stands.
void to_ring(const Ring &ring, const Spot &p, double &best, Spot &nearest) {
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        Spot at;
        const double squared = to_edge(ring[i], ring[i + 1 == n ? 0 : i + 1], p, at);
        if (squared < best) best = squared, nearest = at;
    }
}

Location locate(const Polygon &polygon, const Point &point) {
    const Location in_shell = polygon::locate(polygon.shell, point);
    if (in_shell != Location::Inside) return in_shell;
    for (const Ring &hole : polygon.holes) {
        const Location in_hole = polygon::locate(hole, point);
        if (in_hole == Location::Boundary) return Location::Boundary;
        if (in_hole == Location::Inside) return Location::Outside;
    }
    return Location::Inside;
}

const double kFar = 1e300;

}  // namespace

std::vector<HelixEntry> pocket_entries(const std::vector<std::vector<std::array<double, 2>>> &source,
                                       const std::vector<std::array<double, 2>> &seams, double bit_radius, double radius) {
    const PolygonSet stock = contour_region(source);
    const PolygonSet clearance = polygon::offset(stock, -bit_radius);
    // Offset polygons approximate round corners. The reference keeps an extra chord allowance and verifies
    // each chosen circle against the original material and island boundaries; so does this.
    const double pad = 0.001 + (radius + bit_radius) * 0.0004;
    const PolygonSet centres = polygon::offset(stock, -(bit_radius + radius + pad));
    if (centres.empty()) throw ValueError("The pocket has insufficient room for the cutter and helix radius");
    if (centres.size() * seams.size() > 100000) throw ValueError("Too many disconnected pocket regions for helical entry");
    const polygon::LineClipper feed(clearance);
    const double reach = bit_radius + radius;

    struct Candidate {
        double squared;  // of the distance from the seam, zero where the seam is in the piece
        bool at_seam;
        Spot nearest;
    };
    std::vector<Candidate> candidates(centres.size());
    std::vector<std::size_t> order(centres.size());
    std::vector<HelixEntry> result;
    result.reserve(seams.size());
    for (const std::array<double, 2> &seam : seams) {
        const Point on_grid = polygon::to_grid(seam);
        const Spot here{static_cast<double>(on_grid.x), static_cast<double>(on_grid.y)};
        for (std::size_t i = 0; i < centres.size(); ++i) {
            Candidate &candidate = candidates[i];
            candidate.at_seam = locate(centres[i], on_grid) != Location::Outside;
            candidate.squared = 0;
            candidate.nearest = here;
            if (!candidate.at_seam) {
                candidate.squared = kFar;
                to_ring(centres[i].shell, here, candidate.squared, candidate.nearest);
                for (const Ring &hole : centres[i].holes) to_ring(hole, here, candidate.squared, candidate.nearest);
            }
        }
        // sorted(centres, key=distance to the seam): pieces equally near keep their order.
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t s, std::size_t t) { return candidates[s].squared < candidates[t].squared; });
        bool found = false;
        for (const std::size_t i : order) {
            const Candidate &candidate = candidates[i];
            // The circle against the pocket's own walls and islands.
            double wall = kFar;
            Spot unused;
            polygon::detail::each_ring(stock, [&](const Ring &ring) { to_ring(ring, candidate.nearest, wall, unused); });
            if (std::sqrt(wall) / polygon::kUnitsPerMm < reach) continue;
            const double cx = candidate.at_seam ? seam[0] : candidate.nearest.x / polygon::kUnitsPerMm;
            const double cy = candidate.at_seam ? seam[1] : candidate.nearest.y / polygon::kUnitsPerMm;
            const double dx = seam[0] - cx, dy = seam[1] - cy;
            const double angle = pytrig::atan2(dy, dx);
            // The point of the circle towards the seam, where the helix starts and ends. A seam that is its
            // own centre gives the angle zero.
            const double length = std::sqrt(dx * dx + dy * dy);
            const double ux = length > 0 ? dx / length : 1.0, uy = length > 0 ? dy / length : 0.0;
            const Point start = polygon::to_grid(polygon::PointMm{cx + radius * ux, cy + radius * uy});
            bool clear;
            if (start == on_grid) {
                clear = polygon::locate(clearance, start) != Location::Outside;
            } else {
                const std::vector<polygon::Segment> parts = feed.clip(start, on_grid);
                clear = parts.size() == 1 && parts[0][0] == start && parts[0][1] == on_grid;
            }
            if (!clear) continue;
            result.push_back({cx, cy, angle});
            found = true;
            break;
        }
        if (!found)
            throw ValueError("No clear helix and connecting feed to this pocket contour; reduce the helix radius or use a "
                             "contour ramp");
    }
    return result;
}

}  // namespace detail
}  // namespace kerfkite
