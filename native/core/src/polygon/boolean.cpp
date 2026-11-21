// Regions, Boolean operations and the union of many shapes, all through the arrangement.
#include "kerfkite/cam.hpp"
#include "kernel.hpp"

namespace kerfkite {
namespace polygon {

using namespace detail;

namespace {

// Above this many edges a union is made in spatial groups first, so that edges buried inside the image
// are discarded before the whole is resolved.
constexpr std::size_t kGroupEdges = 600000;

std::size_t edge_count(const std::vector<Ring> &rings) {
    std::size_t count = 0;
    for (const Ring &ring : rings) count += ring.size();
    return count;
}

void append(const PolygonSet &region, std::vector<Ring> &rings) {
    each_ring(region, [&](const Ring &ring) { rings.push_back(ring); });
}

PolygonSet positive(const std::vector<Ring> &rings) {
    Arrangement arrangement;
    for (const Ring &ring : rings) arrangement.add(ring, 0);
    arrangement.resolve();
    arrangement.wind();
    return arrangement.region([](std::int32_t w, std::int32_t) { return w > 0; });
}

// Bits of x and y interleaved, so that sorting by the key keeps neighbours in the plane together.
std::uint64_t interleave(std::uint32_t x, std::uint32_t y) {
    const auto spread = [](std::uint64_t v) {
        v = (v | (v << 16)) & 0x0000ffff0000ffffULL;
        v = (v | (v << 8)) & 0x00ff00ff00ff00ffULL;
        v = (v | (v << 4)) & 0x0f0f0f0f0f0f0f0fULL;
        v = (v | (v << 2)) & 0x3333333333333333ULL;
        v = (v | (v << 1)) & 0x5555555555555555ULL;
        return v;
    };
    return spread(x) | (spread(y) << 1);
}

// The union of rings that each wind zero or more times round every point (shapes the right way round,
// regions, stroke curves): where the winding number is positive.
PolygonSet unite_positive(const std::vector<Ring> &rings) {
    const std::size_t total = edge_count(rings);
    if (total <= kGroupEdges) return positive(rings);
    // Rings in spatial order by the corner of their boxes.
    Coord x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<Point> corner(rings.size());
    bool any = false;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (rings[i].empty()) continue;
        Point low = rings[i][0];
        for (const Point &p : rings[i]) low.x = std::min(low.x, p.x), low.y = std::min(low.y, p.y);
        corner[i] = low;
        if (!any) x0 = x1 = low.x, y0 = y1 = low.y, any = true;
        x0 = std::min(x0, low.x), x1 = std::max(x1, low.x), y0 = std::min(y0, low.y), y1 = std::max(y1, low.y);
    }
    int shift = 0;
    while (((std::max(x1 - x0, y1 - y0)) >> shift) > 0xffff) ++shift;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> order(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) {
        const Point at = rings[i].empty() ? Point{x0, y0} : corner[i];
        order[i] = {interleave(static_cast<std::uint32_t>((at.x - x0) >> shift), static_cast<std::uint32_t>((at.y - y0) >> shift)),
                    static_cast<std::uint32_t>(i)};
    }
    std::sort(order.begin(), order.end());
    std::vector<Ring> merged, group;
    std::size_t in_group = 0;
    const auto flush = [&] {
        if (group.empty()) return;
        append(positive(group), merged);
        group.clear();
        in_group = 0;
    };
    for (const auto &entry : order) {
        const Ring &ring = rings[entry.second];
        if (in_group + ring.size() > kGroupEdges / 4) flush();
        group.push_back(ring);
        in_group += ring.size();
    }
    flush();
    // Grouping again only pays while it keeps removing edges.
    if (edge_count(merged) > total / 2) return positive(merged);
    return unite_positive(merged);
}

bool decide(Operation operation, bool a, bool b) {
    switch (operation) {
    case Operation::Union:
        return a || b;
    case Operation::Intersection:
        return a && b;
    case Operation::Difference:
        return a && !b;
    default:
        return a != b;
    }
}

}  // namespace

PolygonSet normalise(const std::vector<Ring> &rings, FillRule rule) {
    Arrangement arrangement;
    for (const Ring &ring : rings)
        if (ring.size() >= 3) arrangement.add(ring, 0);
    arrangement.resolve();
    arrangement.wind();
    return arrangement.region([rule](std::int32_t w, std::int32_t) { return filled(w, rule); });
}

PolygonSet compose(const std::vector<Ring> &rings) { return normalise(rings, FillRule::EvenOdd); }

PolygonSet combine(Operation operation, const std::vector<Ring> &a, FillRule rule_a, const std::vector<Ring> &b, FillRule rule_b) {
    Arrangement arrangement;
    for (const Ring &ring : a)
        if (ring.size() >= 3) arrangement.add(ring, 0);
    for (const Ring &ring : b)
        if (ring.size() >= 3) arrangement.add(ring, 1);
    arrangement.resolve();
    arrangement.wind();
    return arrangement.region(
        [=](std::int32_t wa, std::int32_t wb) { return decide(operation, filled(wa, rule_a), filled(wb, rule_b)); });
}

PolygonSet combine(Operation operation, const PolygonSet &a, const PolygonSet &b) {
    Arrangement arrangement;
    arrangement.add(a, 0);
    arrangement.add(b, 1);
    arrangement.resolve();
    arrangement.wind();
    return arrangement.region([=](std::int32_t wa, std::int32_t wb) { return decide(operation, wa > 0, wb > 0); });
}

PolygonSet unite_all(const std::vector<PolygonSet> &regions) {
    std::vector<Ring> rings;
    for (const PolygonSet &region : regions) append(region, rings);
    return unite_positive(rings);
}

// ---- Artwork ---------------------------------------------------------------------------------------

struct Artwork::State {
    ArcOptions arcs;
    PolygonSet image;
    std::vector<Ring> pending;  // shapes of one polarity not yet in the image
    std::size_t pending_edges = 0;
    bool pending_dark = true;
    std::size_t shapes = 0;

    void settle() {
        if (pending.empty()) return;
        if (pending_dark || !image.empty()) {
            const PolygonSet batch = unite_positive(pending);
            // Only the polygons of the image whose boxes meet the box of the new shapes can change. The
            // rest are carried over as they are, which is what resolving them again would give.
            Point low{}, high{};
            bool any = false;
            each_ring(batch, [&](const Ring &ring) { extend(ring, low, high, any); });
            PolygonSet near, far;
            for (Polygon &polygon : image) {
                Point p_low{}, p_high{};
                bool p_any = false;
                extend(polygon.shell, p_low, p_high, p_any);
                const bool apart = !any || p_high.x < low.x || high.x < p_low.x || p_high.y < low.y || high.y < p_low.y;
                (apart ? far : near).push_back(std::move(polygon));
            }
            image = combine(pending_dark ? Operation::Union : Operation::Difference, near, batch);
            if (!far.empty()) {
                for (Polygon &polygon : far) image.push_back(std::move(polygon));
                std::sort(image.begin(), image.end(), [](const Polygon &s, const Polygon &t) {
                    return std::lexicographical_compare(s.shell.begin(), s.shell.end(), t.shell.begin(), t.shell.end());
                });
            }
        }
        pending.clear();
        pending_edges = 0;
    }

    static void extend(const Ring &ring, Point &low, Point &high, bool &any) {
        for (const Point &p : ring) {
            if (!any) low = high = p, any = true;
            low.x = std::min(low.x, p.x), low.y = std::min(low.y, p.y), high.x = std::max(high.x, p.x), high.y = std::max(high.y, p.y);
        }
    }

    // Rings that together wind zero or more times round every point; they stay in one step.
    void take(std::vector<Ring> rings, bool dark) {
        if (++shapes > kMaxShapes) reject("Artwork has too many shapes for the polygon kernel");
        const std::size_t count = edge_count(rings);
        // A change of polarity, or enough edges waiting, moves what is waiting into the image.
        if (dark != pending_dark || pending_edges + count > kMaxEdges / 4) settle();
        pending_dark = dark;
        pending_edges += count;
        for (Ring &ring : rings) pending.push_back(std::move(ring));
    }
    void take(Ring ring, bool dark) {
        if (ring.size() < 2) return;
        std::vector<Ring> one;
        one.push_back(std::move(ring));
        take(std::move(one), dark);
    }
};

Artwork::Artwork(const ArcOptions &arcs) : state_(new State) { state_->arcs = arcs; }
Artwork::~Artwork() = default;

void Artwork::add(const Ring &simple_ring, bool dark) {
    if (simple_ring.size() < 3) return;
    Ring ring = simple_ring;
    if (orientation(ring) < 0) std::reverse(ring.begin(), ring.end());
    state_->take(std::move(ring), dark);
}

void Artwork::add(const PolygonSet &region, bool dark) {
    if (region.empty()) return;
    std::vector<Ring> rings;
    append(region, rings);
    state_->take(std::move(rings), dark);
}

void Artwork::disc(double x, double y, double diameter, bool dark) { state_->take(polygon::disc(x, y, diameter, state_->arcs), dark); }
void Artwork::rectangle(double x, double y, double width, double height, bool dark) {
    state_->take(polygon::rectangle(x, y, width, height), dark);
}
void Artwork::obround(double x, double y, double width, double height, bool dark) {
    state_->take(polygon::obround(x, y, width, height, state_->arcs), dark);
}

void Artwork::stroke(const PathMm &points, double width, bool dark) { state_->take(stroke_curve(points, width, state_->arcs), dark); }

std::size_t Artwork::shapes() const { return state_->shapes; }

PolygonSet Artwork::image() {
    state_->settle();
    return state_->image;
}

}  // namespace polygon
}  // namespace kerfkite
