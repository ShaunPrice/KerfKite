// Straight segments cut to a region.
//
// The boundary edges that cross the line of the segment are found exactly, by the side of the line each
// end of an edge is on, and put in order along the line by comparing their positions as exact fractions.
// Between the first and second crossing the line is inside, between the second and third outside, and so
// on. A vertex lying exactly on the line has no side, so the crossings are counted twice: once with such
// vertices taken as left of the line and once as right. That is the line moved a hair's breadth either
// way, and the parts inside for either are the parts of the line in the region or along its boundary.
#include "kernel.hpp"

namespace kerfkite {
namespace polygon {

using namespace detail;

namespace {

struct Side {
    Point a, b;
};

// A position along the line as numerator / denominator of the way from its start to its end, the
// denominator positive.
struct Position {
    Int128 numerator, denominator;
};

bool before(const Position &p, const Position &q) { return compare_products(p.numerator, q.denominator, q.numerator, p.denominator) < 0; }

struct Span {
    Position from, to;
};

}  // namespace

struct LineClipper::State {
    std::vector<Side> sides;
    Grid<Side> grid;
    mutable std::vector<std::uint32_t> stamp;
    mutable std::uint32_t query = 0;
};

LineClipper::LineClipper(const PolygonSet &region) : state_(new State) {
    each_ring(region, [&](const Ring &ring) {
        for (std::size_t i = 0; i < ring.size(); ++i) {
            check_range(ring[i]);
            state_->sides.push_back({ring[i], ring[i + 1 == ring.size() ? 0 : i + 1]});
        }
    });
    state_->grid.build(state_->sides);
    state_->stamp.assign(state_->sides.size(), kNone);
}

LineClipper::~LineClipper() = default;

std::vector<Segment> LineClipper::clip(const Point &a, const Point &b) const {
    std::vector<Segment> result;
    if (a == b || state_->sides.empty()) return result;
    check_range(a);
    check_range(b);
    State &state = *state_;
    if (++state.query == kNone) {
        state.stamp.assign(state.sides.size(), kNone);
        state.query = 0;
    }
    // Crossings for vertices on the line taken as left of it (0) and as right of it (1).
    std::vector<Position> crossings[2];
    state.grid.near_line(a, b, [&](std::uint32_t item) {
        if (state.stamp[item] == state.query) return;
        state.stamp[item] = state.query;
        const Side &side = state.sides[item];
        const int sa = orient(a, b, side.a), sb = orient(a, b, side.b);
        if (sa == sb) return;  // both on one side, or the edge lies along the line
        // The line a + t (b - a) meets the line of the edge at t = (p - a) x (q - p) / (b - a) x (q - p).
        Position at{cross(side.a.x - a.x, side.a.y - a.y, side.b.x - side.a.x, side.b.y - side.a.y),
                    cross(b.x - a.x, b.y - a.y, side.b.x - side.a.x, side.b.y - side.a.y)};
        if (at.denominator < Int128(0)) at.numerator = Int128(0) - at.numerator, at.denominator = Int128(0) - at.denominator;
        if ((sa >= 0) != (sb >= 0)) crossings[0].push_back(at);
        if ((sa > 0) != (sb > 0)) crossings[1].push_back(at);
    });
    std::vector<Span> spans;
    for (std::vector<Position> &list : crossings) {
        std::sort(list.begin(), list.end(), before);
        for (std::size_t i = 0; i + 1 < list.size(); i += 2) spans.push_back({list[i], list[i + 1]});
    }
    std::sort(spans.begin(), spans.end(), [](const Span &s, const Span &t) { return before(s.from, t.from); });
    const Position zero{Int128(0), Int128(1)}, one{Int128(1), Int128(1)};
    const auto point_at = [&](const Position &t) {
        if (!before(zero, t)) return a;
        if (!before(t, one)) return b;
        return Point{a.x + narrow(round_divide(Int128(b.x - a.x) * t.numerator, t.denominator)),
                     a.y + narrow(round_divide(Int128(b.y - a.y) * t.numerator, t.denominator))};
    };
    for (std::size_t i = 0; i < spans.size();) {
        // Spans that overlap or abut are one part.
        Position from = spans[i].from, to = spans[i].to;
        std::size_t j = i + 1;
        for (; j < spans.size() && !before(to, spans[j].from); ++j)
            if (before(to, spans[j].to)) to = spans[j].to;
        i = j;
        if (!before(from, to) || !before(zero, to) || !before(from, one)) continue;  // a touch, or beyond the ends
        const Point start = point_at(from), end = point_at(to);
        if (start != end) result.push_back({start, end});
    }
    return result;
}

}  // namespace polygon
}  // namespace kerfkite
