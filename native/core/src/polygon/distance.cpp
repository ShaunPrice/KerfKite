// The distance from a point to the boundary of a region.
//
// For each edge the place of the foot of the perpendicular (before the edge, on it, or past it) is
// decided exactly, and the distance is then
//
//     the square root of a whole number of grid units squared, to an end of the edge, or
//     |(b - a) x (p - a)| / sqrt(|b - a|^2), to the edge itself,
//
// with each whole number converted to the nearest double first. Those are the operations IEEE 754 rounds
// correctly, so every platform gets the same double for an edge, within 4 parts in 10^16 of the true
// distance, and the answer is the least of them, which does not depend on the order the edges are met in.
//
// The edges are kept in square buckets, each holding the edges whose bounding boxes reach it. A query
// looks at the bucket of the point and then at the rings of buckets round it, and stops when the nearest
// edge found is nearer than anything in a bucket not yet looked at can be. The buckets only limit which
// edges are measured: their size has no effect on the answer.
#include <cmath>
#include <limits>

#include "kernel.hpp"

namespace kerfkite {
namespace polygon {

using namespace detail;

namespace {

struct Side {
    Point a, b;
};

double to_side(const Side &side, const Point &p) {
    const Coord dx = side.b.x - side.a.x, dy = side.b.y - side.a.y, wx = p.x - side.a.x, wy = p.y - side.a.y;
    const Int128 along = dot(wx, wy, dx, dy), length = dot(dx, dy, dx, dy);
    if (along <= Int128(0)) return std::sqrt(to_double(dot(wx, wy, wx, wy)));
    if (along >= length) {
        const Coord vx = p.x - side.b.x, vy = p.y - side.b.y;
        return std::sqrt(to_double(dot(vx, vy, vx, vy)));
    }
    return std::fabs(to_double(cross(dx, dy, wx, wy))) / std::sqrt(to_double(length));
}

}  // namespace

struct BoundaryDistance::State {
    std::vector<Side> sides;
    Coord x0 = 0, y0 = 0, size = 1, nx = 1, ny = 1;
    std::vector<std::uint32_t> start, items;
    mutable std::vector<std::uint32_t> stamp;
    mutable std::uint32_t query = 0;

    Coord column(Coord x) const { return x <= x0 ? 0 : std::min((x - x0) / size, nx - 1); }
    Coord row(Coord y) const { return y <= y0 ? 0 : std::min((y - y0) / size, ny - 1); }

    template <class F> void cells(const Side &side, F &&visit) const {
        const Coord c0 = column(std::min(side.a.x, side.b.x)), c1 = column(std::max(side.a.x, side.b.x));
        const Coord r0 = row(std::min(side.a.y, side.b.y)), r1 = row(std::max(side.a.y, side.b.y));
        for (Coord r = r0; r <= r1; ++r)
            for (Coord c = c0; c <= c1; ++c) visit(static_cast<std::size_t>(r * nx + c));
    }
};

BoundaryDistance::BoundaryDistance(const PolygonSet &region) : state_(new State) {
    State &state = *state_;
    Coord x1 = 0, y1 = 0;
    each_ring(region, [&](const Ring &ring) {
        for (std::size_t i = 0; i < ring.size(); ++i) {
            check_range(ring[i]);
            const Point &p = ring[i];
            if (state.sides.empty() && i == 0) state.x0 = x1 = p.x, state.y0 = y1 = p.y;
            state.x0 = std::min(state.x0, p.x), x1 = std::max(x1, p.x), state.y0 = std::min(state.y0, p.y), y1 = std::max(y1, p.y);
            state.sides.push_back({p, ring[i + 1 == ring.size() ? 0 : i + 1]});
        }
    });
    if (state.sides.empty()) return;
    // About one edge to a bucket, and larger buckets while the edges would be entered too many times: a
    // long edge at a slant is entered in every bucket its box covers.
    const double width = static_cast<double>(x1 - state.x0) + 1, height = static_cast<double>(y1 - state.y0) + 1;
    const double count = static_cast<double>(state.sides.size());
    double size = std::max(1.0, std::floor(std::sqrt(width * height / count)));
    for (;;) {
        state.size = static_cast<Coord>(size);
        state.nx = (x1 - state.x0) / state.size + 1;
        state.ny = (y1 - state.y0) / state.size + 1;
        double entries = 0;
        if (static_cast<double>(state.nx) * static_cast<double>(state.ny) <= 4 * count + 64) {
            for (const Side &side : state.sides)
                entries += static_cast<double>(state.column(std::max(side.a.x, side.b.x)) - state.column(std::min(side.a.x, side.b.x)) + 1) *
                           static_cast<double>(state.row(std::max(side.a.y, side.b.y)) - state.row(std::min(side.a.y, side.b.y)) + 1);
            if (entries <= 16 * count + 4096) break;
        }
        size *= 2;
    }
    state.start.assign(static_cast<std::size_t>(state.nx * state.ny) + 1, 0);
    for (const Side &side : state.sides) state.cells(side, [&](std::size_t cell) { ++state.start[cell + 1]; });
    for (std::size_t i = 1; i < state.start.size(); ++i) state.start[i] += state.start[i - 1];
    state.items.resize(state.start.back());
    std::vector<std::uint32_t> next(state.start.begin(), state.start.end() - 1);
    for (std::size_t i = 0; i < state.sides.size(); ++i)
        state.cells(state.sides[i], [&](std::size_t cell) { state.items[next[cell]++] = static_cast<std::uint32_t>(i); });
    state.stamp.assign(state.sides.size(), kNone);
}

BoundaryDistance::~BoundaryDistance() = default;

double BoundaryDistance::to(const Point &point) const {
    const State &state = *state_;
    double best = std::numeric_limits<double>::infinity();
    if (state.sides.empty()) return best;
    check_range(point);
    if (++state.query == kNone) {
        state.stamp.assign(state.sides.size(), kNone);
        state.query = 0;
    }
    const Coord cx = state.column(point.x), cy = state.row(point.y);
    const auto look = [&](Coord c, Coord r) {
        if (c < 0 || c >= state.nx || r < 0 || r >= state.ny) return;
        const std::size_t cell = static_cast<std::size_t>(r * state.nx + c);
        for (std::uint32_t k = state.start[cell]; k < state.start[cell + 1]; ++k) {
            const std::uint32_t item = state.items[k];
            if (state.stamp[item] == state.query) continue;
            state.stamp[item] = state.query;
            const double away = to_side(state.sides[item], point);
            if (away < best) best = away;
        }
    };
    const Coord furthest = std::max(std::max(cx, state.nx - 1 - cx), std::max(cy, state.ny - 1 - cy));
    for (Coord ring = 0; ring <= furthest; ++ring) {
        if (ring == 0) {
            look(cx, cy);
        } else {
            for (Coord c = cx - ring; c <= cx + ring; ++c) look(c, cy - ring), look(c, cy + ring);
            for (Coord r = cy - ring + 1; r <= cy + ring - 1; ++r) look(cx - ring, r), look(cx + ring, r);
        }
        // An edge in no bucket looked at so far has no point within `ring` buckets of the point's own (or,
        // for a point outside the buckets, of the nearest one, which is no further from any edge). The
        // margin is far more than the rounding of a measured distance, so no such edge can measure less.
        if (best <= static_cast<double>(ring) * static_cast<double>(state.size) * (1 - 1e-9)) break;
    }
    return best;
}

}  // namespace polygon
}  // namespace kerfkite
