// Isolation toolpaths from a Gerber copper layer: see kerfkite/isolation.hpp and docs/PCB.md.
//
// Everything here is on the polygon kernel's one-nanometre grid. The layer is read by the Gerber importer and
// taken from its answer, whose coordinates are already whole nanometres, so the copper image and the contours
// are the same on every platform: no function of the platform's mathematics library places a vertex.
#include "kerfkite/isolation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kerfkite/gerber.hpp"
#include "kerfkite/polygon.hpp"
#include "pcb_text.hpp"
#include "polygon/kernel.hpp"

namespace kerfkite {

namespace {

using json = Json;
namespace pg = polygon;
using pg::Coord;

const size_t MAX_TEXT = 16u << 20;
const double MAX_PASSES = 50;
const size_t MAX_PATHS = 100000;         // contours in the whole answer
const size_t MAX_PASS_PATHS = 20000;     // contours in one pass: they are put in order pair by pair
const size_t MAX_PATH_POINTS = 1000000;  // points of all the contours together
const size_t MAX_COPPER_POINTS = 2000000;
const size_t MAX_CLIP_PATHS = 3000, MAX_CLIP_POINTS = 100000;
const size_t MAX_PLACES = 1000;  // unreachable places listed; all of them are counted
// What the generator takes in one operation.
const size_t OPERATION_PATHS = 3000, OPERATION_POINTS = 100000;
// The corners of a contour's round joins lie outside the true offset by at most this, in millimetres.
const double OFFSET_ALLOWANCE = 0.001;
// Two copper islands can share a contour only when they are nearer than the tool width plus this: twice the
// allowance, and a margin over the kernel's rounding.
const double GAP_MARGIN = 2 * OFFSET_ALLOWANCE + 0.00001;
const double SMALLEST = 0.000002;  // two grid units: the smallest shape the kernel draws
const double PI = 3.141592653589793;

using Path = std::vector<pg::Point>;

double field(const json &object, const char *key) { return object.at(key).get<double>(); }
pg::PointMm pair_of(const json &value) { return {value.at(0).get<double>(), value.at(1).get<double>()}; }
const std::string &text_of(const json &object, const char *key) { return object.at(key).get_ref<const std::string &>(); }

// The cosine and sine of an angle in degrees, from the kernel's series: the same on every platform.
void cos_sin_degrees(double degrees, double &cosine, double &sine) {
    const double turn = degrees - 360 * std::floor(degrees / 360);  // 0 to 360
    int quarter = static_cast<int>(std::floor(turn / 90));
    const double rest = turn - 90 * quarter;
    double c = 1, s = 0;
    pg::detail::series_cos_sin(rest * (PI / 180), c, s);
    quarter &= 3;
    cosine = quarter == 0 ? c : quarter == 1 ? -s : quarter == 2 ? -c : s;
    sine = quarter == 0 ? s : quarter == 1 ? c : quarter == 2 ? -s : -c;
}

Path moved(const Path &ring, const pg::Point &by) {
    Path out;
    out.reserve(ring.size());
    for (const pg::Point &p : ring) out.push_back({p.x + by.x, p.y + by.y});
    return out;
}

pg::PolygonSet moved(const pg::PolygonSet &region, const pg::Point &by) {
    pg::PolygonSet out;
    out.reserve(region.size());
    for (const pg::Polygon &polygon : region) {
        pg::Polygon copy;
        copy.shell = moved(polygon.shell, by);
        for (const pg::Ring &hole : polygon.holes) copy.holes.push_back(moved(hole, by));
        out.push_back(std::move(copy));
    }
    return out;
}

pg::PolygonSet region_of(const pg::Ring &ring) { return pg::normalise({ring}, pg::FillRule::NonZero); }

// What an aperture draws when it is flashed at the origin, and what is needed to draw a line with it.
struct Stamp {
    bool circle = false, rectangle = false, hole = false;
    double diameter = 0, width = 0, height = 0;  // millimetres
    pg::PolygonSet shape;
    pg::Ring ring;  // the shape again, when it is one ring without a hole: cheaper to add
};

Stamp stamp_of(const json &aperture, const pg::ArcOptions &arcs) {
    Stamp stamp;
    const std::string &shape = text_of(aperture, "shape");
    pg::Ring ring;
    if (shape == "circle") {
        stamp.circle = true;
        stamp.diameter = field(aperture, "diameter");
        if (stamp.diameter >= SMALLEST) ring = pg::disc(0, 0, stamp.diameter, arcs);
    } else if (shape == "rectangle" || shape == "obround") {
        stamp.rectangle = shape == "rectangle";
        stamp.width = field(aperture, "width");
        stamp.height = field(aperture, "height");
        if (stamp.width >= SMALLEST && stamp.height >= SMALLEST)
            ring = stamp.rectangle ? pg::rectangle(0, 0, stamp.width, stamp.height) : pg::obround(0, 0, stamp.width, stamp.height, arcs);
    } else if (shape == "polygon") {
        // A regular polygon by the circle through its vertices, the first on the positive X axis before rotation.
        const double radius = field(aperture, "diameter") / 2, rotation = field(aperture, "rotation");
        const int vertices = aperture.at("vertices").get<int>();
        for (int k = 0; k < vertices && radius * 2 >= SMALLEST; ++k) {
            double c = 1, s = 0;
            cos_sin_degrees(rotation + 360.0 * k / vertices, c, s);
            const pg::Point p = pg::to_grid(pg::PointMm{radius * c, radius * s});
            if (ring.empty() || ring.back() != p) ring.push_back(p);
        }
    } else if (shape == "macro") {
        // The parts in order: exposure on adds, exposure off cuts from what the earlier parts built.
        pg::Artwork image(arcs);
        for (const json &part : aperture.at("parts")) {
            const bool exposure = part.at("exposure").get<bool>();
            if (text_of(part, "shape") == "circle") {
                if (field(part, "diameter") >= SMALLEST) image.disc(field(part, "x"), field(part, "y"), field(part, "diameter"), exposure);
            } else {
                pg::PathMm points;
                for (const json &point : part.at("points")) points.push_back(pair_of(point));
                image.add(region_of(pg::ring_from_mm(points)), exposure);
            }
        }
        stamp.shape = image.image();
        return stamp;
    } else {
        throw ValueError("The layer has an aperture of a shape that isolation does not know: " + pcb::printable(shape));
    }
    if (ring.size() < 3) return stamp;
    const double hole = aperture.contains("hole") ? field(aperture, "hole") : 0;
    if (hole >= SMALLEST) {
        stamp.hole = true;
        stamp.shape = pg::combine(pg::Operation::Difference, {ring}, pg::FillRule::NonZero, {pg::disc(0, 0, hole, arcs)}, pg::FillRule::NonZero);
    } else {
        stamp.shape = region_of(ring);
        if (stamp.shape.size() == 1 && stamp.shape[0].holes.empty()) stamp.ring = stamp.shape[0].shell;
    }
    return stamp;
}

void flash(pg::Artwork &image, const Stamp &stamp, const pg::Point &at, bool dark) {
    if (!stamp.ring.empty())
        image.add(moved(stamp.ring, at), dark);
    else
        image.add(moved(stamp.shape, at), dark);
}

// A rectangle swept from a to b without turning: the convex hull of the rectangle at both ends.
Path swept_rectangle(const pg::Point &a, const pg::Point &b, Coord half_x, Coord half_y) {
    const Coord sx = b.x > a.x ? 1 : -1, sy = b.y > a.y ? 1 : -1;
    if (a.x == b.x || a.y == b.y) {
        const Coord x0 = std::min(a.x, b.x) - half_x, x1 = std::max(a.x, b.x) + half_x;
        const Coord y0 = std::min(a.y, b.y) - half_y, y1 = std::max(a.y, b.y) + half_y;
        return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
    const Coord hx = sx * half_x, hy = sy * half_y;
    return {{a.x - hx, a.y - hy}, {a.x + hx, a.y - hy}, {b.x + hx, b.y - hy}, {b.x + hx, b.y + hy}, {b.x - hx, b.y + hy}, {a.x - hx, a.y + hy}};
}

// The copper image of a layer as the importer answers it, in the file's own coordinates.
pg::PolygonSet copper_image(const json &layer, const pg::ArcOptions &arcs, size_t &without_size) {
    std::map<int, Stamp> stamps;
    for (const json &aperture : layer.at("apertures")) stamps.emplace(aperture.at("code").get<int>(), stamp_of(aperture, arcs));
    pg::Artwork image(arcs);
    for (const json &object : layer.at("objects")) {
        const std::string &type = text_of(object, "type");
        const bool dark = text_of(object, "polarity") == "dark";
        if (type == "region") {
            // A contour may touch itself, and a cut-in runs along itself: the non-zero rule reads both.
            pg::PathMm points;
            for (const json &point : object.at("points")) points.push_back(pair_of(point));
            image.add(region_of(pg::ring_from_mm(points)), dark);
            continue;
        }
        const auto found = stamps.find(object.at("aperture").get<int>());
        if (found == stamps.end()) throw ValueError("The layer draws with an aperture it does not define");
        const Stamp &stamp = found->second;
        if (stamp.shape.empty()) {
            ++without_size;  // an aperture of no size draws nothing
            continue;
        }
        if (type == "flash") {
            flash(image, stamp, pg::to_grid(pair_of(object.at("at"))), dark);
            continue;
        }
        if (stamp.hole || !(stamp.circle || stamp.rectangle) || (type == "arc" && !stamp.circle))
            throw ValueError("The layer draws a line with an aperture that isolation cannot sweep");
        if (type == "line") {
            const pg::PointMm start = pair_of(object.at("start")), end = pair_of(object.at("end"));
            const pg::Point a = pg::to_grid(start), b = pg::to_grid(end);
            if (a == b)
                flash(image, stamp, a, dark);  // a line with no length is a dot of its aperture
            else if (stamp.circle)
                image.stroke({start, end}, stamp.diameter, dark);
            else
                image.add(swept_rectangle(a, b, pg::to_grid(stamp.width / 2), pg::to_grid(stamp.height / 2)), dark);
        } else if (type == "arc") {
            pg::PathMm points;
            for (const json &point : object.at("points")) points.push_back(pair_of(point));
            image.stroke(points, stamp.diameter, dark);
        } else {
            throw ValueError("The layer holds an object that isolation does not know: " + pcb::printable(type));
        }
    }
    return image.image();
}

// Which polygon of a region each of many points lies in. A ray is cast to the right of the point and the
// boundary edges it crosses are counted, polygon by polygon, with exact arithmetic; the edges are kept in
// horizontal bands so that a point looks only at edges near its own height.
class Locator {
  public:
    explicit Locator(const pg::PolygonSet &region) : state_(region.size(), 0) {
        bool any = false;
        double climb = 0;
        for (size_t index = 0; index < region.size(); ++index) {
            const auto ring = [&](const pg::Ring &points) {
                for (size_t k = 0; k < points.size(); ++k) {
                    pg::Point a = points[k], b = points[k + 1 == points.size() ? 0 : k + 1];
                    if (a.y == b.y) continue;  // a level edge is never crossed by a level ray
                    if (a.y > b.y) std::swap(a, b);
                    low_ = any ? std::min(low_, a.y) : a.y;
                    high_ = any ? std::max(high_, b.y) : b.y;
                    any = true;
                    climb += static_cast<double>(b.y - a.y);
                    edges_.push_back({a, b, static_cast<uint32_t>(index)});
                }
            };
            ring(region[index].shell);
            for (const pg::Ring &hole : region[index].holes) ring(hole);
        }
        // About eight edges to a band, but no more entries than a few for each edge: tall edges are in many bands.
        Coord bands = static_cast<Coord>(std::min<size_t>(4096, std::max<size_t>(1, edges_.size() / 8)));
        const auto height_for = [&](Coord count) { return (high_ - low_) / count + 1; };
        while (bands > 1 && climb / static_cast<double>(height_for(bands)) > 8.0 * static_cast<double>(edges_.size()) + 1e6) bands /= 2;
        height_ = height_for(bands);
        start_.assign(static_cast<size_t>(bands) + 1, 0);
        for (const Edge &edge : edges_)
            for (Coord band = (edge.a.y - low_) / height_; band <= (edge.b.y - 1 - low_) / height_; ++band) ++start_[static_cast<size_t>(band) + 1];
        for (size_t i = 1; i < start_.size(); ++i) start_[i] += start_[i - 1];
        items_.resize(start_.back());
        std::vector<uint32_t> next(start_.begin(), start_.end() - 1);
        for (size_t index = 0; index < edges_.size(); ++index)
            for (Coord band = (edges_[index].a.y - low_) / height_; band <= (edges_[index].b.y - 1 - low_) / height_; ++band)
                items_[next[static_cast<size_t>(band)]++] = static_cast<uint32_t>(index);
    }

    // The polygon with the point in its interior, or -1. A point on a boundary may be given either answer.
    int find(const pg::Point &p) {
        if (edges_.empty() || p.y < low_ || p.y >= high_) return -1;
        const size_t band = static_cast<size_t>((p.y - low_) / height_);
        touched_.clear();
        for (uint32_t k = start_[band]; k < start_[band + 1]; ++k) {
            const Edge &edge = edges_[items_[k]];
            // The edge runs upwards; it is crossed when the point is level with it and to its left.
            if (edge.a.y > p.y || p.y >= edge.b.y || pg::detail::orient(edge.a, edge.b, p) <= 0) continue;
            unsigned char &state = state_[edge.polygon];  // bit 0: crossings odd; bit 1: listed in touched_
            if (!(state & 2)) touched_.push_back(edge.polygon);
            state = static_cast<unsigned char>((state ^ 1) | 2);
        }
        int found = -1;
        for (const uint32_t polygon : touched_) {
            if ((state_[polygon] & 1) && found < 0) found = static_cast<int>(polygon);
            state_[polygon] = 0;
        }
        return found;
    }

  private:
    struct Edge {
        pg::Point a, b;  // a below b
        uint32_t polygon;
    };
    std::vector<Edge> edges_;
    std::vector<uint32_t> start_, items_, touched_;
    std::vector<unsigned char> state_;
    Coord low_ = 0, high_ = 0, height_ = 1;
};

// The image without its specks: islands that vanish when shrunk by the arc tolerance. Drawing round shapes
// just outside their true curves leaves such slivers wherever a clear shape was made to end exactly on a round
// one (the bars of a thermal reach exactly to its rim), and each would otherwise count as a piece of copper.
pg::PolygonSet without_specks(const pg::PolygonSet &copper, double tolerance, size_t &specks) {
    pg::ArcOptions joins;
    joins.tolerance = OFFSET_ALLOWANCE;
    const pg::PolygonSet shrunk = pg::offset(copper, -tolerance, joins);
    Locator locator(copper);
    std::vector<unsigned char> solid(copper.size(), 0);
    for (const pg::Polygon &polygon : shrunk) {
        const int home = locator.find(polygon.shell.front());
        if (home >= 0) solid[static_cast<size_t>(home)] = 1;
    }
    pg::PolygonSet out;
    for (size_t island = 0; island < copper.size(); ++island) {
        if (solid[island])
            out.push_back(copper[island]);
        else
            ++specks;
    }
    return out;
}

// A place where two copper islands are too near for the tool to pass between them.
struct Place {
    uint32_t first = 0, second = 0;  // the islands
    double gap = 0;                  // millimetres; negative when it was not measured
    double ax = 0, ay = 0, bx = 0, by = 0;  // the nearest points of the two, in grid units
    bool direct = false;             // the tool's sweep covers the point midway between them
};

// The point of the segment from a to b nearest to p.
void nearest_on(double px, double py, double ax, double ay, double bx, double by, double &qx, double &qy) {
    const double dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
    double t = length > 0 ? ((px - ax) * dx + (py - ay) * dy) / length : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    qx = ax + t * dx, qy = ay + t * dy;
}

Coord floor_div(Coord value, Coord size) { return value >= 0 ? value / size : -((-value + size - 1) / size); }

// The pairs of islands, among those named in group (-1 for an island that shares its contour with no other),
// that are in one group and no further apart than reach grid units, each with its gap and nearest points.
// The boundaries of valid polygons do not cross, so the gap between two is the least distance from a vertex
// of one to an edge of the other.
std::vector<Place> near_pairs(const pg::PolygonSet &copper, const std::vector<int> &group, double reach) {
    struct Edge {
        pg::Point a, b;
        uint32_t island;
    };
    std::vector<Edge> edges;
    for (size_t island = 0; island < copper.size(); ++island) {
        if (group[island] < 0) continue;
        const auto ring = [&](const pg::Ring &points) {
            for (size_t k = 0; k < points.size(); ++k) edges.push_back({points[k], points[k + 1 == points.size() ? 0 : k + 1], static_cast<uint32_t>(island)});
        };
        ring(copper[island].shell);
        for (const pg::Ring &hole : copper[island].holes) ring(hole);
    }
    const Coord size = static_cast<Coord>(std::ceil(reach)) + 1;
    // Every cell an edge passes through, and with spread those within one cell of them.
    const auto cells = [&](const Edge &edge, Coord spread, const auto &visit) {
        pg::Point a = edge.a, b = edge.b;
        if (a.x > b.x) std::swap(a, b);
        const Coord c0 = floor_div(a.x, size), c1 = floor_div(b.x, size);
        const double slope = b.x == a.x ? 0 : static_cast<double>(b.y - a.y) / static_cast<double>(b.x - a.x);
        for (Coord c = c0 - spread; c <= c1 + spread; ++c) {
            const Coord left = std::max(a.x, std::min(b.x, (c - spread) * size)), right = std::min(b.x, std::max(a.x, (c + 1 + spread) * size));
            double ya = static_cast<double>(a.y) + slope * static_cast<double>(left - a.x);
            double yb = static_cast<double>(a.y) + slope * static_cast<double>(right - a.x);
            if (b.x == a.x) ya = static_cast<double>(std::min(a.y, b.y)), yb = static_cast<double>(std::max(a.y, b.y));
            const Coord r0 = floor_div(static_cast<Coord>(std::floor(std::min(ya, yb))) - 2, size) - spread;
            const Coord r1 = floor_div(static_cast<Coord>(std::ceil(std::max(ya, yb))) + 2, size) + spread;
            for (Coord r = r0; r <= r1; ++r) visit(static_cast<uint64_t>(c) * 0x9E3779B97F4A7C15ULL + static_cast<uint64_t>(r));
        }
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> table;
    for (size_t index = 0; index < edges.size(); ++index) cells(edges[index], 0, [&](uint64_t key) { table[key].push_back(static_cast<uint32_t>(index)); });

    std::map<std::pair<uint32_t, uint32_t>, Place> best;
    std::vector<uint32_t> seen(edges.size(), 0xffffffffu);
    for (size_t index = 0; index < edges.size(); ++index) {
        const Edge &e = edges[index];
        cells(e, 1, [&](uint64_t key) {
            const auto found = table.find(key);
            if (found == table.end()) return;
            for (const uint32_t other : found->second) {
                const Edge &f = edges[other];
                // Each pair of islands once, from the lower island's side, and each pair of edges once.
                if (f.island <= e.island || group[f.island] != group[e.island] || seen[other] == index) continue;
                seen[other] = static_cast<uint32_t>(index);
                const double ex0 = static_cast<double>(e.a.x), ey0 = static_cast<double>(e.a.y), ex1 = static_cast<double>(e.b.x), ey1 = static_cast<double>(e.b.y);
                const double fx0 = static_cast<double>(f.a.x), fy0 = static_cast<double>(f.a.y), fx1 = static_cast<double>(f.b.x), fy1 = static_cast<double>(f.b.y);
                Place place;
                place.first = e.island, place.second = f.island;
                place.gap = -1;
                const auto consider = [&](double px, double py, double qx, double qy) {
                    const double d = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
                    if (place.gap < 0 || d < place.gap) place.gap = d, place.ax = px, place.ay = py, place.bx = qx, place.by = qy;
                };
                double qx = 0, qy = 0;
                nearest_on(ex0, ey0, fx0, fy0, fx1, fy1, qx, qy), consider(ex0, ey0, qx, qy);
                nearest_on(ex1, ey1, fx0, fy0, fx1, fy1, qx, qy), consider(ex1, ey1, qx, qy);
                nearest_on(fx0, fy0, ex0, ey0, ex1, ey1, qx, qy), consider(qx, qy, fx0, fy0);
                nearest_on(fx1, fy1, ex0, ey0, ex1, ey1, qx, qy), consider(qx, qy, fx1, fy1);
                if (place.gap > reach) continue;
                const auto key_of = std::make_pair(e.island, f.island);
                const auto known = best.find(key_of);
                if (known == best.end() || place.gap < known->second.gap) best[key_of] = place;
            }
        });
    }
    std::vector<Place> out;
    for (const auto &entry : best) out.push_back(entry.second);
    return out;
}

struct Sets {
    std::vector<uint32_t> parent;
    explicit Sets(size_t count) : parent(count) {
        for (size_t i = 0; i < count; ++i) parent[i] = static_cast<uint32_t>(i);
    }
    uint32_t find(uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    }
    bool join(uint32_t a, uint32_t b) {
        a = find(a), b = find(b);
        if (a == b) return false;
        parent[b] = a;
        return true;
    }
};

// The places the tool cannot reach: every pair of islands that the first contour encloses together and
// that are near enough to be the reason. grown is the copper grown by half the tool width.
std::vector<Place> unreachable(const pg::PolygonSet &copper, const pg::PolygonSet &grown, double tool_width) {
    Locator locator(grown);
    std::vector<int> home(copper.size(), -1), members(grown.size(), 0);
    for (size_t island = 0; island < copper.size(); ++island) {
        // Copper is well inside its own grown polygon, so any of its vertices tells which that is.
        home[island] = locator.find(copper[island].shell.front());
        if (home[island] < 0) throw std::logic_error("A copper island is not inside the first isolation contour");
        ++members[static_cast<size_t>(home[island])];
    }
    std::vector<int> group(copper.size(), -1);
    bool shared = false;
    for (size_t island = 0; island < copper.size(); ++island)
        if (members[static_cast<size_t>(home[island])] > 1) group[island] = home[island], shared = true;
    if (!shared) return {};

    std::vector<Place> pairs = near_pairs(copper, group, (tool_width + GAP_MARGIN) * pg::kUnitsPerMm);
    std::sort(pairs.begin(), pairs.end(), [](const Place &p, const Place &q) {
        if (p.gap != q.gap) return p.gap < q.gap;
        return p.first != q.first ? p.first < q.first : p.second < q.second;
    });
    // A pair is a place when the tool's sweep covers the point midway between the two: that is where they stay
    // joined. The locator answers for interior points; one it does not place is settled exactly.
    for (Place &place : pairs) {
        const pg::Point middle{static_cast<Coord>(std::floor((place.ax + place.bx) / 2 + 0.5)), static_cast<Coord>(std::floor((place.ay + place.by) / 2 + 0.5))};
        place.direct = locator.find(middle) == group[place.first] ||
                       pg::locate(pg::PolygonSet{grown[static_cast<size_t>(group[place.first])]}, middle) != pg::Location::Outside;
    }
    Sets sets(copper.size());
    std::vector<Place> places;
    for (const Place &place : pairs)
        if (place.direct) places.push_back(place), sets.join(place.first, place.second);
    // Islands of one group that the places so far do not connect are joined somewhere else along the pair's
    // length, by the allowance of a round corner: the nearest such pairs are places too.
    for (const Place &place : pairs)
        if (!place.direct && sets.join(place.first, place.second)) places.push_back(place);
    // Every group has to be accounted for. A group still in pieces gets a place without a measured gap.
    std::map<int, uint32_t> anchor;
    for (size_t island = 0; island < copper.size(); ++island) {
        if (group[island] < 0) continue;
        const auto known = anchor.find(group[island]);
        if (known == anchor.end()) {
            anchor[group[island]] = static_cast<uint32_t>(island);
        } else if (sets.join(known->second, static_cast<uint32_t>(island))) {
            Place place;
            place.first = known->second, place.second = static_cast<uint32_t>(island);
            place.gap = -1;
            const pg::Point a = copper[place.first].shell.front(), b = copper[island].shell.front();
            place.ax = static_cast<double>(a.x), place.ay = static_cast<double>(a.y), place.bx = static_cast<double>(b.x), place.by = static_cast<double>(b.y);
            places.push_back(place);
        }
    }
    std::sort(places.begin(), places.end(), [](const Place &p, const Place &q) {
        const double p_gap = p.gap < 0 ? 1e300 : p.gap, q_gap = q.gap < 0 ? 1e300 : q.gap;
        if (p_gap != q_gap) return p_gap < q_gap;
        return p.first != q.first ? p.first < q.first : p.second < q.second;
    });
    return places;
}

json point_json(const pg::Point &p) { return json::array({pg::to_mm(p.x), pg::to_mm(p.y)}); }

json ring_json(const Path &ring, bool closed) {
    json out = json::array();
    for (const pg::Point &p : ring) out.push_back(point_json(p));
    if (closed && !ring.empty()) out.push_back(point_json(ring.front()));
    return out;
}

// The contours of a region as closed paths: outer rings clockwise and holes counter-clockwise, so that the
// region, which is the copper side, is on the right of travel.
std::vector<Path> contours(const pg::PolygonSet &region) {
    std::vector<Path> out;
    for (const pg::PathMm &ring : pg::outlines(region)) {
        Path path;
        path.reserve(ring.size());
        for (const pg::PointMm &p : ring) path.push_back(pg::to_grid(p));
        out.push_back(std::move(path));
    }
    return out;
}

// One closed contour cut to the clip region: the pieces inside it, in order along the contour.
void clipped(const Path &contour, const pg::LineClipper &clipper, std::vector<Path> &out, size_t &cut, size_t &dropped) {
    std::vector<Path> pieces;
    for (size_t k = 0; k + 1 < contour.size(); ++k) {
        for (const pg::Segment &part : clipper.clip(contour[k], contour[k + 1])) {
            if (!pieces.empty() && pieces.back().back() == part[0])
                pieces.back().push_back(part[1]);
            else
                pieces.push_back({part[0], part[1]});
        }
    }
    if (pieces.empty()) {
        ++dropped;
        return;
    }
    // The contour is closed, so a piece that ends where the contour starts runs on into the first piece.
    if (pieces.size() > 1 && pieces.back().back() == pieces.front().front()) {
        Path joined = std::move(pieces.back());
        joined.insert(joined.end(), pieces.front().begin() + 1, pieces.front().end());
        pieces.front() = std::move(joined);
        pieces.pop_back();
    }
    if (!(pieces.size() == 1 && pieces[0].front() == pieces[0].back() && pieces[0].size() == contour.size())) ++cut;
    for (Path &piece : pieces) out.push_back(std::move(piece));
}

// Each path after the one that starts nearest to where the last ended, to shorten the travel between them.
void in_travel_order(std::vector<Path> &paths, pg::Point &from) {
    for (size_t done = 0; done < paths.size(); ++done) {
        size_t nearest = done;
        double least = -1;
        for (size_t k = done; k < paths.size(); ++k) {
            const double dx = static_cast<double>(paths[k].front().x - from.x), dy = static_cast<double>(paths[k].front().y - from.y);
            const double d = dx * dx + dy * dy;
            if (least < 0 || d < least) least = d, nearest = k;
        }
        // Rotating keeps the others in the order they had, so that equal distances always choose alike.
        std::rotate(paths.begin() + static_cast<std::ptrdiff_t>(done), paths.begin() + static_cast<std::ptrdiff_t>(nearest),
                    paths.begin() + static_cast<std::ptrdiff_t>(nearest) + 1);
        from = paths[done].back();
    }
}

pg::PolygonSet clip_region(const json &value) {
    if (!value.is_array() || value.empty() || value.size() > MAX_CLIP_PATHS) throw ValueError("Give the clip as 1 to 3000 closed paths of [X, Y] points");
    std::vector<pg::Ring> rings;
    size_t points = 0;
    for (const json &path : value) {
        if (!path.is_array() || path.size() < 3) throw ValueError("Each clip path needs at least three [X, Y] points");
        points += path.size();
        if (points > MAX_CLIP_POINTS) throw ValueError("The clip has more than 100000 points");
        pg::PathMm ring;
        for (const json &point : path) {
            if (!point.is_array() || point.size() != 2) throw ValueError("Each clip path needs at least three [X, Y] points");
            ring.push_back({detail::number(&point[0], "Clip X", -pcb::MAX_COORDINATE, pcb::MAX_COORDINATE, "-10000", "10000"),
                            detail::number(&point[1], "Clip Y", -pcb::MAX_COORDINATE, pcb::MAX_COORDINATE, "-10000", "10000")});
        }
        rings.push_back(pg::ring_from_mm(ring));
    }
    // Even-odd, as a board outline with cut-outs inside it reads.
    const pg::PolygonSet region = pg::compose(rings);
    if (region.empty()) throw ValueError("The clip paths enclose no area");
    return region;
}

std::string choice(const json &request, const char *key, const char *first, const char *second, const char *what) {
    const json *value = detail::get(request, key);
    if (!value) return first;
    if (detail::is_text(value, first) || detail::is_text(value, second)) return value->get_ref<const std::string &>();
    throw ValueError(std::string(what) + " must be \"" + first + "\" or \"" + second + "\"");
}

}  // namespace

json gerber_isolation(const json &request) {
    const std::string &text = pcb::file_text(request, "isolation", MAX_TEXT,
                                             {"arc_tolerance", "tool_width", "passes", "overlap", "direction", "order", "offset", "mirror_x", "clip", "width", "height"});
    if (!detail::get(request, "tool_width")) throw ValueError("Give \"tool_width\": the width of cut at the engraving depth, in millimetres");
    const double tool_width = detail::number(detail::get(request, "tool_width"), "Tool width", 0.01, 10, "0.01", "10");
    const double pass_count = detail::setting(request, "passes", 1, "Isolation passes", 1, MAX_PASSES, "1", "50");
    if (pass_count != std::floor(pass_count)) throw ValueError("Isolation passes must be a whole number");
    const int passes = static_cast<int>(pass_count);
    const double overlap = detail::setting(request, "overlap", 0.2, "Overlap", 0, 0.9, "0", "0.9");
    const double arc_tolerance = detail::setting(request, "arc_tolerance", 0.005, "Arc tolerance", 0.0005, 0.1, "0.0005", "0.1");
    const bool climb = choice(request, "direction", "climb", "conventional", "Direction") == "climb";
    const bool outer_first = choice(request, "order", "outer_first", "inner_first", "Order") == "outer_first";
    const pcb::Placement place = pcb::placement_of(request);
    double width = 0, height = 0;
    const bool fit = pcb::work_area(request, width, height);
    pg::PolygonSet clip;
    const json *clip_paths = detail::get(request, "clip");
    if (!detail::none(clip_paths)) clip = clip_region(*clip_paths);
    const double step = pcb::round6(tool_width * (1 - overlap));

    const json layer = gerber_import(json{{"text", text}, {"arc_tolerance", arc_tolerance}});
    pcb::Warnings warnings;
    for (const json &warning : layer.at("warnings")) warnings.add(warning.get<std::string>());
    const json &attributes = layer.at("attributes");
    if (attributes.contains(".FilePolarity") && text_of(attributes, ".FilePolarity") == "Negative")
        throw ValueError("The layer is a negative image (.FilePolarity): it draws where copper is absent, which isolation does not support");

    // Round shapes are drawn just outside the true curve, by no more than the arc tolerance.
    pg::ArcOptions arcs;
    arcs.tolerance = arc_tolerance;
    size_t without_size = 0;
    pg::PolygonSet copper = copper_image(layer, arcs, without_size);
    if (without_size)
        warnings.add(std::to_string(without_size) + (without_size == 1 ? " object is" : " objects are") + " drawn with an aperture of no size and add no copper");
    size_t specks = 0;
    copper = without_specks(copper, arc_tolerance, specks);
    if (specks)
        warnings.add(std::to_string(specks) + (specks == 1 ? " speck" : " specks") + " of copper thinner than twice the arc tolerance " +
                     (specks == 1 ? "was" : "were") + " left out of the image");
    if (copper.empty()) throw ValueError("The layer draws no copper");

    // Placed on the grid, so that a board moved by an offset has exactly the same contours, moved.
    const pg::Point shift = pg::to_grid(pg::PointMm{place.offset.x, place.offset.y});
    copper = moved(copper, shift);
    if (place.mirrored) {
        const Coord twice = 2 * pg::to_grid(place.axis);
        std::vector<pg::Ring> rings;
        for (const pg::Polygon &polygon : copper) {
            const auto turned = [&](const pg::Ring &ring) {
                pg::Ring out(ring.rbegin(), ring.rend());  // a mirror image runs the other way round
                for (pg::Point &p : out) p.x = twice - p.x;
                rings.push_back(std::move(out));
            };
            turned(polygon.shell);
            for (const pg::Ring &hole : polygon.holes) turned(hole);
        }
        copper = pg::normalise(rings, pg::FillRule::Positive);
    }
    if (pg::vertex_count(copper) > MAX_COPPER_POINTS) throw ValueError("The copper image has more than 2000000 points; use a coarser arc tolerance");

    // The contours, pass by pass, each from the copper itself so that no error builds up.
    pg::ArcOptions joins;
    joins.tolerance = OFFSET_ALLOWANCE;
    std::vector<std::vector<Path>> pass_paths(static_cast<size_t>(passes));
    std::vector<double> distances;
    std::vector<Place> places;
    size_t total_paths = 0, total_points = 0;
    for (int k = 0; k < passes; ++k) {
        const double distance = pcb::round6(tool_width / 2 + k * step);
        distances.push_back(distance);
        const pg::PolygonSet grown = pg::offset(copper, distance, joins);
        if (k == 0) places = unreachable(copper, grown, tool_width);
        std::vector<Path> &paths = pass_paths[static_cast<size_t>(k)];
        paths = contours(grown);
        if (paths.size() > MAX_PASS_PATHS) throw ValueError("One isolation pass has more than 20000 contours");
        total_paths += paths.size();
        for (const Path &path : paths) total_points += path.size();
        if (total_paths > MAX_PATHS) throw ValueError("The isolation has more than 100000 contours; use fewer passes");
        if (total_points > MAX_PATH_POINTS) throw ValueError("The isolation contours have more than 1000000 points; use fewer passes or a coarser arc tolerance");
    }

    // Cut to the clip outline, turned for the direction of cut, and put in cutting order.
    size_t cut = 0, dropped = 0;
    if (!clip.empty()) {
        const pg::LineClipper clipper(clip);
        for (std::vector<Path> &paths : pass_paths) {
            std::vector<Path> kept;
            for (const Path &path : paths) clipped(path, clipper, kept, cut, dropped);
            paths = std::move(kept);
        }
    }
    json operations = json::array(), cut_order = json::array();
    json chunk = json::array();
    size_t chunk_points = 0, path_count = 0, point_count = 0;
    bool any = false;
    Coord box[4] = {0, 0, 0, 0};
    pg::Point from{};
    bool started = false;
    const auto flush = [&] {
        if (chunk.empty()) return;
        operations.push_back({{"operation", "line"}, {"paths", std::move(chunk)}});
        chunk = json::array();
        chunk_points = 0;
    };
    for (int n = 0; n < passes; ++n) {
        const int k = outer_first ? passes - 1 - n : n;
        std::vector<Path> &paths = pass_paths[static_cast<size_t>(k)];
        if (!climb)
            for (Path &path : paths) std::reverse(path.begin(), path.end());
        if (!started && !paths.empty()) {
            // The first cut starts at the path nearest the lowest corner of the work.
            from = paths[0].front();
            for (const std::vector<Path> &others : pass_paths)
                for (const Path &path : others)
                    for (const pg::Point &p : path) from.x = std::min(from.x, p.x), from.y = std::min(from.y, p.y);
            started = true;
        }
        in_travel_order(paths, from);
        for (const Path &path : paths) {
            if (path.size() > OPERATION_POINTS) throw ValueError("One isolation contour has more than 100000 points; use a coarser arc tolerance");
            if (chunk.size() == OPERATION_PATHS || chunk_points + path.size() > OPERATION_POINTS) flush();
            for (const pg::Point &p : path) {
                box[0] = any ? std::min(box[0], p.x) : p.x, box[1] = any ? std::min(box[1], p.y) : p.y;
                box[2] = any ? std::max(box[2], p.x) : p.x, box[3] = any ? std::max(box[3], p.y) : p.y;
                any = true;
            }
            chunk.push_back(ring_json(path, false));
            chunk_points += path.size();
            point_count += path.size();
            ++path_count;
        }
        cut_order.push_back({{"pass", k + 1}, {"distance", distances[static_cast<size_t>(k)]}, {"paths", paths.size()}});
    }
    flush();
    if (!path_count) throw ValueError("No isolation contour lies inside the clip outline");
    const double bounds[4] = {pg::to_mm(box[0]), pg::to_mm(box[1]), pg::to_mm(box[2]), pg::to_mm(box[3])};
    if (fit && (bounds[0] < 0 || bounds[1] < 0 || bounds[2] > width || bounds[3] > height))
        throw ValueError("The isolation paths do not fit the work area with this offset: they span X" + pcb::mm(bounds[0]) + " to X" + pcb::mm(bounds[2]) +
                         " and Y" + pcb::mm(bounds[1]) + " to Y" + pcb::mm(bounds[3]));

    json islands = json::array();
    for (const pg::Polygon &polygon : copper) {
        json holes = json::array();
        for (const pg::Ring &hole : polygon.holes) holes.push_back(ring_json(hole, true));
        islands.push_back({{"shell", ring_json(polygon.shell, true)}, {"holes", holes}});
    }
    pg::Bounds extent{0, 0, 0, 0};
    pg::bounds(copper, extent);

    json listed = json::array();
    double smallest = -1;
    for (const Place &spot : places) {
        if (spot.gap >= 0 && (smallest < 0 || spot.gap < smallest)) smallest = spot.gap;
        if (listed.size() == MAX_PLACES) continue;
        const pg::PointMm a{spot.ax / pg::kUnitsPerMm, spot.ay / pg::kUnitsPerMm}, b{spot.bx / pg::kUnitsPerMm, spot.by / pg::kUnitsPerMm};
        listed.push_back({{"at", json::array({pcb::round6((a[0] + b[0]) / 2), pcb::round6((a[1] + b[1]) / 2)})},
                          {"gap", spot.gap < 0 ? json(nullptr) : json(pcb::round6(spot.gap / pg::kUnitsPerMm))},
                          {"between", json::array({json::array({pcb::round6(a[0]), pcb::round6(a[1])}), json::array({pcb::round6(b[0]), pcb::round6(b[1])})})},
                          {"islands", json::array({spot.first, spot.second})}});
    }
    json smallest_gap = nullptr, separating = nullptr;
    if (!places.empty()) {
        std::string warning = "The cutter cannot pass between separate copper features in " + std::to_string(places.size()) +
                              (places.size() == 1 ? " place" : " places") + ": the paths leave them joined. The tool cuts " + pcb::mm(tool_width) + " mm wide";
        if (smallest >= 0) {
            const double gap = smallest / pg::kUnitsPerMm;
            smallest_gap = pcb::round6(gap);
            warning += " and the smallest gap is " + pcb::mm(gap) + " mm.";
            // The widest tool, to the micrometre below, that leaves a contour in every gap.
            const double widest = std::floor((gap - GAP_MARGIN) * 1000) / 1000;
            if (widest >= 0.01) {
                separating = pcb::round6(widest);
                warning += " A tool that cuts no wider than " + pcb::mm(widest) + " mm would separate everything.";
            } else {
                warning += " No tool this program accepts is narrow enough to separate everything.";
            }
        } else {
            warning += ".";
        }
        warnings.add(warning);
    }
    if (cut)
        warnings.add(std::to_string(cut) + (cut == 1 ? " contour is" : " contours are") +
                     " cut short at the clip outline: the copper is isolated only once the board is cut out along that outline");

    return json{
        {"format", "isolation"},
        {"tool_width", tool_width},
        {"passes", passes},
        {"overlap", overlap},
        {"step", step},
        {"direction", climb ? "climb" : "conventional"},
        {"order", outer_first ? "outer_first" : "inner_first"},
        {"offset", json::array({pcb::round6(place.offset.x), pcb::round6(place.offset.y)})},
        {"mirror_x", place.mirrored ? json(pcb::round6(place.axis)) : json(nullptr)},
        {"arc_tolerance", arc_tolerance},
        {"layer",
         {{"units", layer.at("units")}, {"file_function", layer.at("file_function")}, {"counts", layer.at("counts")}, {"bounds", layer.at("bounds")}}},
        {"operations", operations},
        {"cut", cut_order},
        {"path_count", path_count},
        {"point_count", point_count},
        {"clipped", {{"cut", cut}, {"dropped", dropped}}},
        {"bounds", json::array({bounds[0], bounds[1], bounds[2], bounds[3]})},
        {"copper", islands},
        {"islands", copper.size()},
        {"specks", specks},
        {"copper_bounds", json::array({extent.min_x, extent.min_y, extent.max_x, extent.max_y})},
        {"unreachable", listed},
        {"unreachable_count", places.size()},
        {"smallest_gap", smallest_gap},
        {"separating_tool_width", separating},
        {"warnings", warnings.list},
    };
}

}  // namespace kerfkite
