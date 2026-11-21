// A check of the polygon kernel that needs no Python and no other geometry library: exact answers for
// small cases, and for generated cases the properties any correct answer has. A digest of every generated
// result is compared with a value recorded here, so a compiler, processor or integer type that computed
// anything differently would be caught. validation/polygon_kernel_check.py compares with GEOS.
//
//   kerfkite_polygon_selftest           the checks
//   kerfkite_polygon_selftest --bench   timings of the two cases named in docs/POLYGON-KERNEL.md as well
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../core/src/polygon/kernel.hpp"
#include "kerfkite/cam.hpp"
#include "kerfkite/polygon.hpp"

using namespace kerfkite::polygon;
using kerfkite::polygon::detail::Int128;

static int failures = 0;

static void expect(bool condition, const std::string &what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

template <class F> static bool rejects(F &&call) {
    try {
        call();
    } catch (const kerfkite::ValueError &) {
        return true;
    }
    return false;
}

// Rings from millimetres, for brevity in the cases below.
static Ring ring(std::initializer_list<PointMm> points) { return ring_from_mm(PathMm(points)); }
static Ring square(double x, double y, double side) { return ring({{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}}); }
static PolygonSet region(const Ring &r) { return normalise({r}, FillRule::NonZero); }
static bool same(const Ring &a, const Ring &b) { return a == b; }
static bool close_to(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// A small generator with the same sequence everywhere.
struct Random {
    std::uint64_t state;
    explicit Random(std::uint64_t seed) : state(seed) {}
    std::uint32_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    }
    // A whole number from low to high.
    Coord between(Coord low, Coord high) { return low + static_cast<Coord>(next() % static_cast<std::uint64_t>(high - low + 1)); }
};

// A simple ring round a centre: vertices at increasing angles, by rational points of the unit circle so
// that no library trigonometry is involved, at varying distances.
static Ring blob(Random &random, Coord cx, Coord cy, Coord radius, int count) {
    Ring out;
    for (int i = 0; i < count; ++i) {
        // The point of the unit circle at parameter t = tan(angle / 2), t rising through the quadrant.
        const int quadrant = i * 4 / count;
        const double t = (static_cast<double>(i * 4 % count) + 0.3 + 0.4 * (random.next() % 100) / 100.0) / count;
        double x = (1 - t * t) / (1 + t * t), y = 2 * t / (1 + t * t);
        for (int turn = 0; turn < quadrant; ++turn) {
            const double old = x;
            x = -y, y = old;
        }
        const double r = static_cast<double>(radius) * (0.45 + 0.55 * (random.next() % 1000) / 1000.0);
        const Point p{cx + static_cast<Coord>(std::floor(x * r + 0.5)), cy + static_cast<Coord>(std::floor(y * r + 0.5))};
        if (out.empty() || out.back() != p) out.push_back(p);
    }
    return out;
}

// FNV-1a over every coordinate of a result.
static std::uint64_t digest = 1469598103934665603ULL;
static void absorb(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        digest ^= (value >> (8 * i)) & 0xff;
        digest *= 1099511628211ULL;
    }
}
static void absorb(const PolygonSet &set) {
    absorb(set.size());
    for (const Polygon &polygon : set) {
        absorb(polygon.holes.size());
        detail::each_ring(PolygonSet{polygon}, [](const Ring &r) {
            absorb(r.size());
            for (const Point &p : r) absorb(static_cast<std::uint64_t>(p.x)), absorb(static_cast<std::uint64_t>(p.y));
        });
    }
}

// The distance in grid units from a point to the boundary of a region.
static double distance_to_boundary(const PolygonSet &set, const Point &p) {
    double best = 1e300;
    detail::each_ring(set, [&](const Ring &r) {
        for (std::size_t i = 0; i < r.size(); ++i) {
            const Point &a = r[i], &b = r[i + 1 == r.size() ? 0 : i + 1];
            const double ax = static_cast<double>(a.x - p.x), ay = static_cast<double>(a.y - p.y);
            const double dx = static_cast<double>(b.x - a.x), dy = static_cast<double>(b.y - a.y);
            double t = -(ax * dx + ay * dy) / (dx * dx + dy * dy);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const double qx = ax + t * dx, qy = ay + t * dy;
            best = std::min(best, std::sqrt(qx * qx + qy * qy));
        }
    });
    return best;
}

static void wide_arithmetic() {
    using namespace kerfkite::polygon::detail;
    const Int128 big = multiply(Coord(1) << 40, (Coord(1) << 40) - 3);
    expect(big / Int128(Coord(1) << 40) == Int128((Coord(1) << 40) - 3), "a product divides back");
    expect(narrow(multiply(-123456789012LL, 987654321LL) / Int128(-987654321LL)) == 123456789012LL, "signs in division");
    expect(narrow(Int128(-7) / Int128(2)) == -3 && narrow(floor_divide(Int128(-7), Int128(2))) == -4, "division rounds towards zero, floor division down");
    expect(narrow(round_divide(Int128(7), Int128(2))) == 4 && narrow(round_divide(Int128(-7), Int128(2))) == -3 && narrow(round_divide(Int128(5), Int128(-2))) == -2,
           "halves round upwards");
    expect(sign(multiply(-5, 7) + Int128(35)) == 0 && sign(multiply(-5, 7)) == -1, "sign of a sum");
    const Int128 huge = big * Int128(Coord(1) << 40);  // 2^120, about
    expect(huge / big == Int128(Coord(1) << 40) && huge > big && Int128(0) - huge < big, "values beyond 64 bits compare and divide");
    expect(compare_products(huge, big, big, huge) == 0, "equal products of 240 bits");
    expect(compare_products(huge + Int128(1), big, big, huge) == 1 && compare_products(huge, big, big + Int128(1), huge) == -1, "products of 240 bits one apart");
    expect(compare_products(Int128(0) - huge, big, big, huge) == -1 && compare_products(Int128(-3), Int128(-5), Int128(7), Int128(2)) == 1, "signs of products");
    expect(to_double(huge) == 1329227995784915872903807060280344576.0 - 3.0 * 1208925819614629174706176.0, "a wide value as a double");
    expect(to_double(Int128(0) - multiply(3, 5)) == -15.0, "a negative value as a double");
    double c, s;
    series_cos_sin(0.5, c, s);
    expect(close_to(c, std::cos(0.5), 4e-16) && close_to(s, std::sin(0.5), 4e-16), "the series for cosine and sine");
    series_cos_sin(1.5707963267948966, c, s);
    expect(close_to(c, 0, 4e-16) && close_to(s, 1, 4e-16), "the series at a quarter turn");
}

static void conversions() {
    expect(to_grid(1.0) == 1000000 && to_grid(-0.0000005) == 0 && to_grid(0.0000005) == 1 && to_grid(-2.5e-6) == -2, "millimetres round to the nearest unit, halves upwards");
    expect(to_mm(1234567) == 1.234567 && to_mm(-1) == -1e-6, "units divide back to millimetres");
    expect(rejects([] { to_grid(std::nan("")); }) && rejects([] { to_grid(1e300); }) && rejects([] { to_grid(200000.0); }), "values that are not finite or too large are refused");
    const Ring closed = ring({{0, 0}, {1, 0}, {1, 0.0000001}, {1, 1}, {0, 1}, {0, 0}});
    expect(closed.size() == 4, "a repeated vertex and the closing vertex are dropped");
    expect(ring_to_mm(closed).size() == 5 && ring_to_mm(closed, false).size() == 4, "a ring is closed on the way out when asked");
}

static void measures() {
    const Ring box = square(0, 0, 10);
    expect(signed_area(box) == 100 && orientation(box) == 1, "a counter-clockwise square");
    Ring back(box.rbegin(), box.rend());
    expect(signed_area(back) == -100 && orientation(back) == -1, "a clockwise square");
    expect(is_simple(box) && is_simple(back), "squares are simple");
    expect(!is_simple(ring({{0, 0}, {10, 10}, {10, 0}, {0, 10}})), "a bow-tie is not simple");
    expect(!is_simple(ring({{0, 0}, {10, 0}, {5, 5}, {10, 10}, {0, 10}, {5, 5}})), "a ring that touches itself at a vertex is not simple");
    expect(!is_simple(ring({{0, 0}, {10, 0}, {10, 10}, {5, 0}, {0, 10}})), "a vertex on another edge is not simple");
    expect(!is_simple(ring({{0, 0}, {10, 0}, {5, 0}, {5, 5}})), "an edge that doubles back is not simple");
    expect(!is_simple(ring({{0, 0}, {10, 0}})) && !is_simple(ring({{0, 0}, {5, 0}, {10, 0}})), "two points, or three in a line, are not a ring");
    expect(is_simple(ring({{0, 0}, {5, 0}, {10, 0}, {10, 10}, {0, 10}})), "a vertex on a straight side is allowed");

    const PolygonSet frame = compose({box, square(3, 3, 4)});
    expect(frame.size() == 1 && frame[0].holes.size() == 1 && area(frame) == 84, "a square in a square is a frame");
    expect(locate(frame, to_grid(PointMm{1, 1})) == Location::Inside && locate(frame, to_grid(PointMm{5, 5})) == Location::Outside, "inside the frame and inside its hole");
    expect(locate(frame, to_grid(PointMm{3, 5})) == Location::Boundary && locate(frame, to_grid(PointMm{0, 0})) == Location::Boundary, "on the hole and on a corner");
    expect(locate(frame, to_grid(PointMm{-1, 5})) == Location::Outside && locate(frame, to_grid(PointMm{10.000001, 5})) == Location::Outside, "outside the frame");
    Bounds box_of{};
    expect(bounds(frame, box_of) && box_of.min_x == 0 && box_of.min_y == 0 && box_of.max_x == 10 && box_of.max_y == 10, "bounds of the frame");
    expect(!bounds(PolygonSet(), box_of) && area(PolygonSet()) == 0 && vertex_count(frame) == 8, "an empty region has no bounds");

    std::string why;
    expect(is_valid(frame, &why) && why.empty(), "the frame is valid");
    PolygonSet wrong = frame;
    std::reverse(wrong[0].shell.begin(), wrong[0].shell.end());
    expect(!is_valid(wrong, &why) && why == "an outer ring is not counter-clockwise", "a clockwise outer ring is found");
    wrong = {Polygon{box, {}}, Polygon{square(5, 5, 10), {}}};
    expect(!is_valid(wrong, &why) && why == "two rings cross", "crossing rings are found");
    wrong = {Polygon{box, {}}, Polygon{square(2, 2, 3), {}}};
    expect(!is_valid(wrong, &why) && why == "rings overlap or a hole lies outside the region", "a polygon inside another is found");
    wrong = {Polygon{box, {}}, Polygon{square(10, 2, 3), {}}};
    expect(!is_valid(wrong, &why) && why == "two rings share a length of edge", "a shared edge is found");
    Ring hole = square(20, 20, 2);
    std::reverse(hole.begin(), hole.end());
    wrong = {Polygon{box, {hole}}};
    expect(!is_valid(wrong, &why), "a hole outside its outline is found");
}

static void booleans() {
    const PolygonSet a = region(square(0, 0, 10)), b = region(square(5, 5, 10));
    PolygonSet r = unite(a, b);
    expect(r.size() == 1 && same(r[0].shell, ring({{0, 0}, {10, 0}, {10, 5}, {15, 5}, {15, 15}, {5, 15}, {5, 10}, {0, 10}})), "union of two squares");
    r = intersect(a, b);
    expect(r.size() == 1 && same(r[0].shell, square(5, 5, 5)), "intersection of two squares");
    r = subtract(a, b);
    expect(r.size() == 1 && same(r[0].shell, ring({{0, 0}, {10, 0}, {10, 5}, {5, 5}, {5, 10}, {0, 10}})), "difference of two squares");
    r = exclusive_or(a, b);
    expect(r.size() == 2 && same(r[0].shell, ring({{0, 0}, {10, 0}, {10, 5}, {5, 5}, {5, 10}, {0, 10}})) &&
               same(r[1].shell, ring({{5, 10}, {10, 10}, {10, 5}, {15, 5}, {15, 15}, {5, 15}})),
           "symmetric difference of two squares: two polygons that touch at two corners");
    expect(is_valid(r), "polygons that touch at corners are valid");

    // Shared edges vanish; a shared corner keeps two polygons.
    r = unite(a, region(square(10, 0, 10)));
    expect(r.size() == 1 && same(r[0].shell, ring({{0, 0}, {20, 0}, {20, 10}, {0, 10}})), "squares sharing an edge unite into a rectangle");
    r = unite(a, region(square(10, 10, 10)));
    expect(r.size() == 2 && same(r[0].shell, square(0, 0, 10)) && same(r[1].shell, square(10, 10, 10)), "squares sharing a corner stay two");
    expect(intersect(a, region(square(10, 0, 10))).empty() && intersect(a, region(square(20, 20, 1))).empty(), "touching and distant squares have no intersection");
    expect(subtract(a, a).empty() && exclusive_or(a, a).empty() && unite(a, a) == a && intersect(a, a) == a, "a region with itself");
    expect(unite(a, PolygonSet()) == a && intersect(a, PolygonSet()).empty() && subtract(PolygonSet(), a).empty(), "the empty region");

    // A hole that touches its outline at a vertex, and holes that touch each other, are kept as holes.
    r = subtract(a, region(ring({{0, 5}, {3, 3}, {6, 5}, {3, 7}})));
    expect(r.size() == 1 && r[0].holes.size() == 1 && r[0].shell.size() == 5 && area(r) == 88 && is_valid(r), "a hole touching the outline at a vertex");
    r = subtract(a, unite(region(square(2, 2, 2)), region(square(4, 4, 2))));
    expect(r.size() == 1 && r[0].holes.size() == 2 && area(r) == 92 && is_valid(r), "two holes touching at a corner");
    // A band right across splits the square in two.
    r = subtract(a, region(ring({{4, -1}, {6, -1}, {6, 11}, {4, 11}})));
    expect(r.size() == 2 && r[0].holes.empty() && area(r) == 80, "a band across a square leaves two polygons");
    // Islands inside holes, each hole found by its own outline.
    const std::vector<Ring> nest = {square(0, 0, 20), square(2, 2, 16), square(4, 4, 12), square(6, 6, 8), square(30, 0, 5), square(31, 1, 1)};
    r = compose(nest);
    expect(r.size() == 3 && r[0].holes.size() == 1 && r[1].holes.size() == 1 && r[2].holes.size() == 1 && area(r) == 400 - 256 + 144 - 64 + 24, "nested squares alternate under even-odd");
    expect(same(r[0].shell, square(0, 0, 20)) && same(r[1].shell, square(4, 4, 12)) && same(r[2].shell, square(30, 0, 5)), "polygons in order of their first vertex");
    expect(normalise(nest, FillRule::NonZero).size() == 2 && area(normalise(nest, FillRule::NonZero)) == 425, "nested squares fill under non-zero");

    // A ring that crosses itself.
    const Ring bow = ring({{0, 0}, {10, 10}, {10, 0}, {0, 10}});
    r = normalise({bow}, FillRule::EvenOdd);
    expect(r.size() == 2 && area(r) == 50 && same(r[0].shell, ring({{0, 0}, {5, 5}, {0, 10}})), "a bow-tie is two triangles");
    expect(normalise({bow}, FillRule::Positive).size() == 1 && area(normalise({bow}, FillRule::Positive)) == 25, "only the counter-clockwise half is positive");
    // Vertices on straight sides are dropped; where another ring touches, both keep the vertex.
    r = normalise({ring({{0, 0}, {5, 0}, {10, 0}, {10, 10}, {5, 10}, {0, 10}, {0, 5}})}, FillRule::NonZero);
    expect(r.size() == 1 && same(r[0].shell, square(0, 0, 10)), "vertices on straight sides are dropped");
    r = unite(a, region(ring({{10, 5}, {14, 3}, {14, 7}})));
    expect(r.size() == 2 && r[0].shell.size() == 5 && r[0].shell[2] == to_grid(PointMm{10, 5}), "a vertex where another polygon touches a side is kept");

    // United in spatial groups, as very many shapes are, the answer is the same: frames keep their holes.
    detail::Shapes frames;
    std::vector<Ring> every;
    for (int i = 0; i < 200; ++i) {
        const double x = (i * 37) % 100, y = (i * 53) % 90;
        Ring inner = square(x + 2, y + 2, 4);
        std::reverse(inner.begin(), inner.end());
        frames.push_back({square(x, y, 8), inner});
        every.push_back(square(x, y, 8));
        every.push_back(inner);
    }
    const PolygonSet at_once = normalise(every, FillRule::Positive);
    expect(detail::unite_positive(frames, 1000000) == at_once, "shapes united at once");
    expect(detail::unite_positive(frames, 400) == at_once && detail::unite_positive(frames, 64) == at_once, "shapes united in groups, and in groups of groups");

    const std::vector<PolygonSet> many = {a, b, region(square(12, 12, 10)), region(square(40, 40, 1))};
    r = unite_all(many);
    expect(r.size() == 2 && area(r) == 100 + 100 - 25 + 100 - 9 + 1, "the union of several regions at once");
}

// The corners a drawing asks for: an arc with its vertices on the true arc, a miter, a bevel.
static void joined_offsets() {
    const PolygonSet box = region(square(0, 0, 10));
    JoinOptions join;
    join.join = Join::Miter;
    PolygonSet r = offset(box, 2, join);
    expect(r.size() == 1 && same(r[0].shell, square(-2, -2, 14)), "a square grown with miters is a square");
    r = offset(box, -2, join);
    expect(r.size() == 1 && same(r[0].shell, square(2, 2, 6)), "a square shrunk with miters is a square");
    join.join = Join::Bevel;
    r = offset(box, 2, join);
    expect(r.size() == 1 && r[0].shell.size() == 8 && area(r) == 196 - 8 && r[0].shell[0] == to_grid(PointMm{-2, 0}) && r[0].shell[1] == to_grid(PointMm{0, -2}),
           "a square grown with bevels loses a triangle at each corner");
    // A round join: vertices on the arc, so the area is less than the true one, by less than the slivers of 12 parts.
    join.join = Join::Round;
    join.segments = 48;
    r = offset(box, 2, join);
    const double exact = 100 + 4 * 20 + 3.141592653589793 * 4, step = 2 * 3.141592653589793 / 48;
    expect(r.size() == 1 && r[0].shell.size() == 4 * 13 && area(r) < exact && area(r) > exact - 4 * 12 * 2 * (step - std::sin(step)) - 1e-5, "a square grown with inscribed arcs");
    Bounds b{};
    expect(bounds(r, b) && b.min_x == -2 && b.max_x == 12 && b.min_y == -2 && b.max_y == 12, "bounds of a square grown with inscribed arcs");
    for (const Point &p : r[0].shell) {
        // Every vertex is on a straight side or at the distance from a corner, to the rounding of the grid.
        const double dx = std::max(0.0, std::max(-to_mm(p.x), to_mm(p.x) - 10)), dy = std::max(0.0, std::max(-to_mm(p.y), to_mm(p.y) - 10));
        expect(close_to(std::sqrt(dx * dx + dy * dy), 2, 1e-6), "a vertex of an inscribed arc lies on the arc");
    }
    // Seven edges a turn: a quarter turn is 1.75 steps, between one and two, so it is halved.
    join.segments = 7;
    r = offset(box, 2, join);
    expect(r.size() == 1 && r[0].shell.size() == 4 * 3, "an arc between one and two steps is halved");
    join.segments = 3;
    r = offset(box, 2, join);
    expect(r.size() == 1 && r[0].shell.size() == 8, "three edges a turn leave a quarter turn whole");
    // A sharp corner. Within the limit the miter is one point; beyond it the corner is cut square at the distance.
    const PolygonSet wedge = region(ring({{0, 0}, {10, 0}, {0, 1}}));
    join.join = Join::Miter;
    join.miter_limit = 30;
    r = offset(wedge, 1, join);
    Bounds sharp{}, cut{};
    expect(r.size() == 1 && r[0].shell.size() == 3 && bounds(r, sharp) && sharp.max_x > 29 && sharp.max_x < 31, "a miter within its limit is a point");
    join.miter_limit = 2;
    r = offset(wedge, 1, join);
    expect(r.size() == 1 && r[0].shell.size() == 4 && bounds(r, cut) && cut.max_x > 11 && cut.max_x < 11.05, "a miter beyond its limit is cut square at the distance");
    // The cut is square to the bisector: its two ends are equally far from the corner.
    std::vector<double> reach;
    for (const Point &p : r[0].shell)
        if (to_mm(p.x) > 10.5) reach.push_back(std::sqrt((to_mm(p.x) - 10) * (to_mm(p.x) - 10) + to_mm(p.y) * to_mm(p.y)));
    expect(reach.size() == 2 && close_to(reach[0], reach[1], 2e-6) && reach[0] > 1, "the square cut is symmetric about the corner");
    // Shrinking joins the corners that point into the material: those of a hole, and a notch.
    const PolygonSet frame = compose({square(0, 0, 10), square(3, 3, 4)});
    join.join = Join::Bevel;
    r = offset(frame, -1, join);
    expect(r.size() == 1 && r[0].holes.size() == 1 && r[0].holes[0].size() == 8 && same(r[0].shell, square(1, 1, 8)) && area(r) == 64 - (36 - 2), "a hole grows with bevels as its region shrinks");
    join.join = Join::Miter;
    r = offset(frame, 2, join);
    expect(r.size() == 1 && r[0].holes.empty() && same(r[0].shell, square(-2, -2, 14)), "a hole of 4 closes when grown by 2 with miters");
    expect(offset(box, -5, join).empty() && offset(box, 0, join) == box, "shrunk away, and no offset");
    // Whatever the join, the result is a valid region.
    Random random(77);
    for (int k = 0; k < 40; ++k) {
        const PolygonSet shape = normalise({blob(random, 0, 0, 20000000, 9 + k % 7), blob(random, 9000000, 4000000, 12000000, 5 + k % 4)}, FillRule::NonZero);
        for (const double distance : {1.5, -0.8}) {
            JoinOptions options;
            options.miter_limit = 1000;
            options.join = Join::Miter;
            const PolygonSet mitred = offset(shape, distance, options);
            options.join = Join::Bevel;
            const PolygonSet bevelled = offset(shape, distance, options);
            options.join = Join::Round;
            options.segments = 5 + k;
            const PolygonSet round = offset(shape, distance, options);
            std::string why;
            expect(is_valid(mitred, &why) && is_valid(bevelled, &why) && is_valid(round, &why), "offsets with joins are valid regions");
            // The join is added when growing and taken away when shrinking, so the order turns round.
            const PolygonSet &least = distance > 0 ? bevelled : mitred, &most = distance > 0 ? mitred : bevelled;
            expect(area(subtract(least, round)) < 1e-4 && area(subtract(round, most)) < 1e-4, "a bevel adds least, a miter most and an arc lies between");
            expect(distance > 0 ? area(subtract(shape, bevelled)) == 0 : area(subtract(mitred, shape)) == 0, "growing contains the region and shrinking is contained in it");
        }
    }
    JoinOptions bad;
    bad.segments = 2;
    expect(rejects([&] { offset(box, 1, bad); }), "fewer than three segments a turn are refused");
    bad.segments = 48, bad.miter_limit = 0.5;
    expect(rejects([&] { offset(box, 1, bad); }), "a miter limit below one is refused");
}

static void hulls() {
    const Ring hull = convex_hull({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {2, 2}, {2, 0}, {4, 2}, {0, 0}, {1, 3}});
    expect(same(hull, Ring{{0, 0}, {4, 0}, {4, 4}, {0, 4}}) && orientation(hull) == 1, "a hull drops points inside it and on its sides");
    expect(convex_hull({{0, 0}, {1, 1}, {2, 2}, {5, 5}}).empty() && convex_hull({{0, 0}, {1, 1}}).empty() && convex_hull({}).empty(), "points on one line have no hull");
    // Far apart, where the products need more than 64 bits.
    const Coord far = Coord(1) << 36;
    const Ring wide = convex_hull({{-far, -far}, {far, -far + 1}, {far, far}, {-far + 1, far}, {0, 0}, {far - 1, 0}});
    expect(wide.size() == 4 && wide[0] == Point{-far, -far} && wide[2] == Point{far, far}, "a hull at the limits of the grid");
    Random random(5);
    for (int k = 0; k < 50; ++k) {
        std::vector<Point> points;
        for (int i = 0; i < 40; ++i) {
            const Coord x = static_cast<Coord>(random.next() % 2000001) - 1000000;
            const Coord y = static_cast<Coord>(random.next() % 2000001) - 1000000;
            points.push_back({x, y});
        }
        const Ring ring = convex_hull(points);
        bool convex = ring.size() >= 3 && orientation(ring) == 1, holds = true;
        for (std::size_t i = 0; convex && i < ring.size(); ++i)
            convex = detail::orient(ring[i], ring[(i + 1) % ring.size()], ring[(i + 2) % ring.size()]) > 0;
        for (const Point &p : points) holds = holds && locate(ring, p) != Location::Outside;
        expect(convex && holds, "a hull is strictly convex and holds every point");
    }
}

static void offsets() {
    const PolygonSet box = region(square(0, 0, 10));
    ArcOptions coarse;
    coarse.segments_per_quarter = 1;
    // One segment a quarter turn: a square corner is a single mitred point.
    PolygonSet r = offset(box, 1, coarse);
    expect(r.size() == 1 && same(r[0].shell, square(-1, -1, 12)), "a square grown with one segment a quarter is a square");
    // Two: each corner is cut by one tangent at 45 degrees.
    coarse.segments_per_quarter = 2;
    r = offset(box, 1, coarse);
    const double cut = std::sqrt(2.0) - 1;
    expect(r.size() == 1 && r[0].shell.size() == 8 && r[0].shell[0] == to_grid(PointMm{-1, -cut}) && r[0].shell[1] == to_grid(PointMm{-cut, -1}), "a square grown with two segments a quarter");
    r = offset(box, 2);
    expect(r.size() == 1 && r[0].shell.size() == 4 * 32, "a square grown at 32 segments a quarter has 32 corners at each corner");
    // Corners lie outside the true arc, so the area is a little more than the square, four strips and a disc.
    const double exact = 100 + 4 * 20 + 3.141592653589793 * 4;
    expect(area(r) > exact && area(r) < exact + 0.003, "area of a grown square");
    Bounds b{};
    expect(bounds(r, b) && b.min_x == -2 && b.max_x == 12 && b.min_y == -2 && b.max_y == 12, "bounds of a grown square");
    r = offset(box, -2);
    expect(r.size() == 1 && same(r[0].shell, square(2, 2, 6)), "a square shrunk keeps sharp corners");
    expect(offset(box, -5).empty() && offset(box, -6).empty() && !offset(box, -4.999999).empty(), "a square shrunk to its centre is empty");
    expect(offset(box, 0) == box && offset(box, 0.0000004) == box, "no offset, or less than a unit");
    expect(rejects([&] { offset(box, 40000); }) && rejects([&] { offset(box, std::nan("")); }), "an offset beyond the range is refused");

    // A frame: growing closes the hole, shrinking widens it with round corners.
    const PolygonSet frame = compose({square(0, 0, 10), square(3, 3, 4)});
    expect(offset(frame, 2).size() == 1 && offset(frame, 2)[0].holes.empty(), "a hole of 4 closes when grown by 2");
    r = offset(frame, 1);
    expect(r.size() == 1 && r[0].holes.size() == 1 && same(r[0].holes[0], ring({{4, 4}, {4, 6}, {6, 6}, {6, 4}})), "a hole shrinks with sharp corners as its region grows");
    r = offset(frame, -1);
    expect(r.size() == 1 && r[0].holes.size() == 1 && same(r[0].shell, square(1, 1, 8)) && r[0].holes[0].size() == 4 * 32, "a hole grows with round corners as its region shrinks");
    // Shrunk by half its width the frame is gone, but for what the hole's round corners leave in the
    // outline's square ones: four pieces, each a square of 1.5 less a quarter disc.
    r = offset(frame, -1.5);
    expect(r.size() == 4 && close_to(area(r), 4 * (2.25 - 3.141592653589793 * 2.25 / 4), 0.002) && offset(frame, -1.9).empty(), "a frame shrunk by half its width leaves its corners");
    // Shrinking can part a region.
    const PolygonSet dumbbell = unite(unite(region(square(0, 0, 10)), region(square(20, 0, 10))), region(ring({{9, 4}, {21, 4}, {21, 6}, {9, 6}})));
    expect(dumbbell.size() == 1 && offset(dumbbell, -1.5).size() == 2 && offset(dumbbell, -0.5).size() == 1, "a neck 2 wide parts when shrunk by more than 1");

    // Shapes.
    Ring d = disc(5, 5, 4);
    expect(d.size() == 128 && orientation(d) == 1 && is_simple(d) && signed_area(d) > 3.141592653589793 * 4 && signed_area(d) < 3.141592653589793 * 4 * 1.0004, "a disc is drawn just outside its circle");
    bool symmetric = true;
    for (std::size_t i = 0; i < d.size(); ++i) {
        const Point &p = d[i], &q = d[(i + 32) % d.size()];
        symmetric = symmetric && q.x - 5000000 == -(p.y - 5000000) && q.y - 5000000 == p.x - 5000000;
    }
    expect(symmetric, "a disc is the same after a quarter turn");
    ArcOptions fine;
    fine.tolerance = 0.001;
    expect(disc(0, 0, 1, fine).size() == 4 * 14 && disc(0, 0, 100, fine).size() == 4 * 132, "a tolerance sets the segments for each radius");
    expect(disc(0, 0, 0.000004).size() == 4 && rejects([] { disc(0, 0, 0.000001); }) && rejects([] { disc(0, 0, -1); }), "a disc of a few units is a square; smaller is refused");
    expect(same(rectangle(1, 2, 4, 2), ring({{-1, 1}, {3, 1}, {3, 3}, {-1, 3}})), "a rectangle");
    Ring o = obround(0, 0, 6, 2);
    expect(o.size() == 128 && is_simple(o) && close_to(signed_area(o), 8 + 3.141592653589793, 0.002), "an obround is a rectangle with half discs");
    Bounds ob{};
    expect(bounds(region(o), ob) && ob.min_x == -3 && ob.max_x == 3 && ob.min_y == -1 && ob.max_y == 1, "bounds of an obround");
    expect(obround(0, 0, 2, 2) == disc(0, 0, 2), "an obround as wide as it is high is a disc");
    PolygonSet pen = stroke({{0, 0}, {10, 0}}, 2);
    expect(pen.size() == 1 && pen == region(obround(5, 0, 12, 2)), "a stroke of one segment is an obround");
    pen = stroke({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, 2);
    expect(pen.size() == 1 && pen[0].holes.size() == 1 && same(pen[0].holes[0], ring({{1, 1}, {1, 9}, {9, 9}, {9, 1}})), "a closed stroke has a hole");
    expect(stroke({{3, 3}}, 2) == region(disc(3, 3, 2)) && stroke({}, 2).empty(), "a stroke of one point is a disc");

    // Artwork: polarity in drawing order.
    Artwork art;
    art.rectangle(5, 5, 10, 10);
    art.disc(5, 5, 4, false);
    art.rectangle(5, 5, 1, 1);
    r = art.image();
    expect(art.shapes() == 3 && r.size() == 2 && r[0].holes.size() == 1 && area(r) > 100 - 12.5714 + 1 && area(r) < 100 - 12.5663 + 1, "a clear disc in a dark square, and a dark square in that");
    Artwork other;
    other.disc(5, 5, 4, false);
    other.rectangle(5, 5, 10, 10);
    expect(area(other.image()) == 100, "a clear shape removes only what was drawn before it");
    Artwork clockwise;
    Ring turned = square(0, 0, 10);
    std::reverse(turned.begin(), turned.end());
    clockwise.add(turned);
    clockwise.add(square(5, 5, 10));
    expect(area(clockwise.image()) == 175, "a ring given clockwise is still dark");
}

static void lines() {
    const PolygonSet frame = compose({square(0, 0, 10), square(3, 3, 4)});
    const LineClipper clipper(frame);
    const auto cut = [&](PointMm a, PointMm b) { return clipper.clip(to_grid(a), to_grid(b)); };
    const auto is = [](const Segment &s, PointMm a, PointMm b) { return s[0] == to_grid(a) && s[1] == to_grid(b); };
    std::vector<Segment> parts = cut({-1, 5}, {11, 5});
    expect(parts.size() == 2 && is(parts[0], {0, 5}, {3, 5}) && is(parts[1], {7, 5}, {10, 5}), "a line across a frame");
    parts = cut({11, 5}, {-1, 5});
    expect(parts.size() == 2 && is(parts[0], {10, 5}, {7, 5}) && is(parts[1], {3, 5}, {0, 5}), "the parts run the way the line runs");
    parts = cut({-1, 0}, {11, 0});
    expect(parts.size() == 1 && is(parts[0], {0, 0}, {10, 0}), "a line along the outline counts");
    parts = cut({-1, 3}, {11, 3});
    expect(parts.size() == 1 && is(parts[0], {0, 3}, {10, 3}), "a line along the edge of the hole runs through");
    parts = cut({-1, -1}, {11, 11});
    expect(parts.size() == 2 && is(parts[0], {0, 0}, {3, 3}) && is(parts[1], {7, 7}, {10, 10}), "a line through corners");
    expect(cut({-5, 5}, {5, -5}).empty() && cut({-1, 10.000001}, {11, 10.000001}).empty(), "a line that only touches a corner, or misses, gives nothing");
    parts = cut({1, 1}, {2, 2});
    expect(parts.size() == 1 && is(parts[0], {1, 1}, {2, 2}), "a segment wholly inside is kept whole");
    expect(cut({4, 4}, {6, 6}).empty() && cut({1, 1}, {1, 1}).empty(), "a segment inside the hole, or of no length, gives nothing");
    parts = cut({1, 5}, {9, 5});
    expect(parts.size() == 2 && is(parts[0], {1, 5}, {3, 5}) && is(parts[1], {7, 5}, {9, 5}), "a segment that starts and ends inside");
}

static void order() {
    const PolygonSet set = compose({square(0, 0, 10), square(2, 2, 2), square(6, 6, 2), square(20, 0, 3), square(30, 0, 20)});
    const std::vector<PathMm> paths = outlines(set);
    // Smallest polygon first; holes before their outline; each ring closed; outlines clockwise.
    expect(paths.size() == 5 && paths[0].size() == 5 && paths[0][0] == PointMm{20, 0} && paths[0][1] == PointMm{20, 3}, "the smallest polygon first, clockwise");
    expect(paths[1][0] == PointMm{2, 2} && paths[1][1] == PointMm{4, 2} && paths[2][0] == PointMm{6, 6} && paths[3][0] == PointMm{0, 0}, "holes, counter-clockwise, before their outline");
    expect(paths[4][0] == PointMm{30, 0} && paths[4].front() == paths[4].back(), "the largest polygon last, closed");
}

// The largest coordinates: the arithmetic is exact to the limit, and beyond it input is refused.
static void limits() {
    const Coord edge = kMaxCoordinate;
    expect(to_grid(to_mm(edge)) == edge && to_grid(to_mm(-edge)) == -edge, "the largest coordinate converts both ways");
    const Ring wide = {{-edge, -edge}, {edge, -edge + 1}, {edge - 3, edge}};
    const Ring tall = {{-edge, edge}, {-edge + 5, -edge}, {edge, edge - 7}};
    const PolygonSet a = region(wide), b = region(tall);
    const PolygonSet u = unite(a, b), n = intersect(a, b), x = exclusive_or(a, b);
    expect(is_valid(u) && is_valid(n) && is_valid(x) && !n.empty(), "triangles across the whole range");
    // Areas of the order of 1e11 square millimetres, each exact to about 1e-5 in a double.
    const double slack = 8.0 * to_mm(edge) * 6e-6 + 1e-3;
    expect(close_to(area(u) + area(n), area(a) + area(b), slack) && close_to(area(x), area(u) - area(n), slack), "areas at the limit of the range");
    absorb(u), absorb(n), absorb(x);
    const PolygonSet corner = region(Ring{{edge - 5000000, edge - 5000000}, {edge, edge - 5000000}, {edge, edge}, {edge - 5000000, edge}});
    const PolygonSet grown = offset(corner, to_mm(kMaxDistance));
    expect(is_valid(grown) && subtract(corner, grown).empty() && offset(corner, -2.5).empty() && offset(corner, -2.4).size() == 1, "offsets at the corner of the range");
    absorb(grown);
    const LineClipper clipper(u);
    const std::vector<Segment> parts = clipper.clip({-edge, -edge}, {edge, edge});
    expect(!parts.empty() && parts.front()[0] == Point{-edge, -edge}, "a line across the whole range");
    const Coord beyond = detail::kHardLimit + 1;
    expect(rejects([&] { normalise({Ring{{0, 0}, {beyond, 0}, {0, 5}}}, FillRule::NonZero); }) && rejects([&] { is_simple(Ring{{0, 0}, {5, 0}, {0, -beyond}}); }) &&
               rejects([&] { clipper.clip({0, 0}, {beyond, 1}); }),
           "coordinates beyond the limit are refused");
    expect(rejects([] { ArcOptions bad; bad.segments_per_quarter = 0; disc(0, 0, 1, bad); }) && rejects([] { ArcOptions bad; bad.tolerance = -1; disc(0, 0, 1, bad); }), "arc settings out of range are refused");
}

// Properties of generated cases. Everything a result is checked against is computed here, exactly.
static void generated() {
    using namespace kerfkite::polygon::detail;
    Random random(20261121);
    const ArcOptions arcs;
    const double step = 3.141592653589793 / 2 / 32;
    const double outer = 1 / std::cos(0.53125 * step) - 1;
    int worst_rounds = 0;
    for (int round = 0; round < 400; ++round) {
        // Sizes from a few units to tens of millimetres, so that the rounding of crossings is in play.
        const Coord scale = Coord(1) << (3 + round % 24);
        std::vector<Ring> first, second;
        // Draws are made in separate statements: the order in which a call's arguments are evaluated is
        // the compiler's choice, and the cases must be the same everywhere.
        const auto another = [&](std::vector<Ring> &rings) {
            const Coord cx = random.between(-scale, scale);
            const Coord cy = random.between(-scale, scale);
            const int count = 3 + static_cast<int>(random.next() % 20);
            rings.push_back(blob(random, cx, cy, scale, count));
        };
        for (int i = 0; i < 1 + round % 3; ++i) another(first);
        for (int i = 0; i < 1 + round % 2; ++i) another(second);
        if (round % 5 == 0) {  // the first set again, moved by a unit or two: nearly coincident edges
            second = first;
            for (Ring &r : second)
                for (Point &p : r) p.x += random.between(-2, 2), p.y += random.between(-2, 2);
        }
        const PolygonSet a = normalise(first, FillRule::EvenOdd), b = normalise(second, FillRule::NonZero);
        const PolygonSet u = unite(a, b), n = intersect(a, b), d = subtract(a, b), x = exclusive_or(a, b);
        for (const PolygonSet *set : {&a, &b, &u, &n, &d, &x}) {
            std::string why;
            if (!is_valid(*set, &why)) expect(false, "generated case " + std::to_string(round) + ": " + why);
            absorb(*set);
        }
        // The identities of the set operations hold for areas to within the rounding of crossings: a few
        // units along the boundaries involved.
        double length = 0;
        for (const PolygonSet *set : {&a, &b})
            each_ring(*set, [&](const Ring &r) {
                for (std::size_t i = 0; i < r.size(); ++i) {
                    const Point &p = r[i], &q = r[i + 1 == r.size() ? 0 : i + 1];
                    length += std::fabs(static_cast<double>(q.x - p.x)) + std::fabs(static_cast<double>(q.y - p.y));
                }
            });
        const double slack = (length * 6 + 16) / (kUnitsPerMm * kUnitsPerMm);
        expect(close_to(area(u) + area(n), area(a) + area(b), slack), "area of union and intersection, case " + std::to_string(round));
        expect(close_to(area(x), area(u) - area(n), slack), "area of symmetric difference, case " + std::to_string(round));
        expect(close_to(area(d), area(a) - area(n), slack), "area of difference, case " + std::to_string(round));
        expect(unite(a, a) == a && intersect(u, u) == u && unite(b, a) == u && intersect(b, a) == n && exclusive_or(b, a) == x, "idempotent and symmetric, case " + std::to_string(round));
        // How many rounds the resolving took.
        Arrangement arrangement;
        arrangement.add(a, 0);
        arrangement.add(b, 1);
        arrangement.resolve();
        worst_rounds = std::max(worst_rounds, arrangement.rounds());

        if (scale < (Coord(1) << 12) || u.empty()) continue;
        // Offsets: every vertex of the result is at the offset distance from the source boundary, never
        // nearer (but for rounding) and further only by the arc allowance.
        const double distance = static_cast<double>(scale) * (0.02 + 0.3 * (random.next() % 100) / 100.0);
        for (const double signed_distance : {distance, -distance}) {
            const PolygonSet moved = offset(u, signed_distance / kUnitsPerMm, arcs);
            std::string why;
            if (!is_valid(moved, &why)) expect(false, "generated offset " + std::to_string(round) + ": " + why);
            absorb(moved);
            double nearest = 1e300, furthest = 0;
            each_ring(moved, [&](const Ring &r) {
                for (const Point &p : r) {
                    const double away = distance_to_boundary(u, p);
                    nearest = std::min(nearest, away), furthest = std::max(furthest, away);
                }
            });
            if (!moved.empty()) {
                expect(nearest > distance - 3, "an offset vertex is nearer than the distance, case " + std::to_string(round));
                expect(furthest < distance * (1 + outer) + 3, "an offset vertex is beyond the arc allowance, case " + std::to_string(round));
            }
            // A grown region contains its source and a shrunk one lies inside it.
            const PolygonSet &inner = signed_distance > 0 ? u : moved, &outside = signed_distance > 0 ? moved : u;
            expect(subtract(inner, outside).empty(), "an offset and its source are nested, case " + std::to_string(round));
        }
        // Hatch: rows across the union. Each part lies in the region, and the parts of all rows times
        // the spacing approach the area as a sum of strips does.
        Bounds box{};
        bounds(u, box);
        const LineClipper clipper(u);
        const Coord x0 = to_grid(box.min_x) - 5, x1 = to_grid(box.max_x) + 5, y0 = to_grid(box.min_y), y1 = to_grid(box.max_y);
        const Coord spacing = std::max<Coord>(1, (y1 - y0) / 200);
        double covered = 0, edge_length = 0;
        bool inside = true;
        for (Coord y = y0 + spacing / 2; y <= y1; y += spacing)
            for (const Segment &part : clipper.clip({x0, y}, {x1, y})) {
                covered += static_cast<double>(part[1].x - part[0].x) * static_cast<double>(spacing);
                inside = inside && part[0].y == y && part[1].y == y && part[0].x < part[1].x;
                // The ends are cuts rounded to the grid: within a unit of the boundary.
                inside = inside && distance_to_boundary(u, part[0]) <= 1 && distance_to_boundary(u, part[1]) <= 1;
                const Point middle{(part[0].x + part[1].x) / 2, y};
                inside = inside && (locate(u, middle) != Location::Outside || distance_to_boundary(u, middle) <= 1);
                absorb(static_cast<std::uint64_t>(part[0].x)), absorb(static_cast<std::uint64_t>(part[1].x));
            }
        each_ring(u, [&](const Ring &r) {
            for (std::size_t i = 0; i < r.size(); ++i) edge_length += std::fabs(static_cast<double>(r[i + 1 == r.size() ? 0 : i + 1].x - r[i].x));
        });
        expect(inside, "hatch rows lie in the region and end at its boundary, case " + std::to_string(round));
        expect(close_to(covered / (kUnitsPerMm * kUnitsPerMm), area(u), (edge_length * static_cast<double>(spacing) + 4 * edge_length) / (kUnitsPerMm * kUnitsPerMm) + 1e-12),
               "hatch rows cover the area, case " + std::to_string(round));
    }
    expect(worst_rounds <= 8, "resolving took " + std::to_string(worst_rounds) + " rounds");

    // Edges at fine angles through nearly one point, on a grid of a few units: the hardest thing to resolve.
    Arrangement knot;
    for (int i = 0; i < 60; ++i) {
        const Coord tilt = random.between(-6, 6);
        knot.add(Ring{{-1000000 + random.between(-3, 3), random.between(-3, 3)}, {1000000, tilt}, {1000000, tilt + random.between(1, 2)}}, 0);
        knot.add(Ring{{random.between(-3, 3), -1000000}, {tilt, 1000000}, {tilt + random.between(1, 2), 1000000}}, 1);
    }
    knot.resolve();
    knot.wind();
    const PolygonSet tangle = knot.region([](std::int32_t wa, std::int32_t wb) { return (wa != 0) != (wb != 0); });
    expect(is_valid(tangle) && knot.rounds() <= 12, "a knot of fine slivers resolves, in " + std::to_string(knot.rounds()) + " rounds");
    absorb(tangle);

    // Many shapes at once, with clear shapes among them, and contours round the image.
    Artwork art;
    for (int i = 0; i < 3000; ++i) {
        const double x = static_cast<double>(random.between(0, 60000)) / 1000, y = static_cast<double>(random.between(0, 40000)) / 1000;
        const double size = static_cast<double>(random.between(200, 1500)) / 1000;
        const bool dark = i % 97 != 96;
        switch (i % 4) {
        case 0:
            art.disc(x, y, size, dark);
            break;
        case 1:
            art.rectangle(x, y, size, size / 2, dark);
            break;
        case 2:
            art.obround(x, y, size * 2, size, dark);
            break;
        default:
            art.stroke({{x, y}, {x + static_cast<double>(random.between(-3000, 3000)) / 1000, y + static_cast<double>(random.between(-3000, 3000)) / 1000}}, size / 4, dark);
        }
    }
    const PolygonSet image = art.image();
    std::string why;
    expect(is_valid(image, &why), "artwork of 3,000 shapes is valid: " + why);
    absorb(image);
    PolygonSet contour = image;
    for (int pass = 0; pass < 3; ++pass) {
        contour = offset(contour, 0.1);
        expect(is_valid(contour) && subtract(image, contour).empty(), "an isolation contour is valid and contains the image");
        absorb(contour);
    }
    // A pocket: insets until nothing is left, each inside the one before.
    Random shape(7);
    PolygonSet inset = offset(region(blob(shape, 0, 0, 40000000, 300)), -0.5);
    int passes = 0;
    for (; !inset.empty() && passes < 1000; ++passes) {
        const PolygonSet next = offset(inset, -0.4);
        expect(subtract(next, inset).empty() && area(next) < area(inset), "each pocket pass lies inside the one before");
        absorb(next);
        inset = next;
    }
    expect(passes > 10 && passes < 200, "a pocket ends, after " + std::to_string(passes) + " passes");
}

static double seconds_since(const std::chrono::steady_clock::time_point &start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static void bench() {
    // 1. A pocket of a contour of 2,000 vertices: an inset by the tool radius and then 50 by the stepover.
    //    The contour is 45 mm from its centre at the nearest, so 50 passes of 0.8 mm leave something.
    Random random(3);
    const PolygonSet contour = region(blob(random, 0, 0, 100000000, 2000));
    auto start = std::chrono::steady_clock::now();
    PolygonSet inset = offset(contour, -1.5);
    std::size_t vertices = vertex_count(inset);
    int passes = 1;
    for (; passes <= 50 && !inset.empty(); ++passes) {
        inset = offset(inset, -0.8);
        vertices += vertex_count(inset);
    }
    std::printf("pocket: contour of %zu vertices, %d insets, %zu vertices of tool path: %.2f s\n", vertex_count(contour), passes, vertices, seconds_since(start));

    // 2. The union of 50,000 small shapes that overlap their neighbours: pads and tracks on a board.
    start = std::chrono::steady_clock::now();
    ArcOptions arcs;
    arcs.tolerance = 0.002;
    Artwork art(arcs);
    for (int i = 0; i < 50000; ++i) {
        const double x = static_cast<double>(random.between(0, 200000)) / 1000, y = static_cast<double>(random.between(0, 150000)) / 1000;
        switch (i % 4) {
        case 0:
            art.disc(x, y, 1.2);
            break;
        case 1:
            art.rectangle(x, y, 1.6, 1.0);
            break;
        case 2:
            art.obround(x, y, 2.0, 1.0);
            break;
        default:
            art.stroke({{x, y}, {x + static_cast<double>(random.between(-4000, 4000)) / 1000, y + static_cast<double>(random.between(-4000, 4000)) / 1000}}, 0.3);
        }
    }
    const double building = seconds_since(start);
    const PolygonSet image = art.image();
    std::printf("union: 50,000 shapes into %zu polygons of %zu vertices: %.2f s (%.2f s of it making the shapes)\n", image.size(), vertex_count(image), seconds_since(start), building);
    start = std::chrono::steady_clock::now();
    const PolygonSet isolation = offset(image, 0.1, arcs);
    std::printf("contour: the image grown by 0.1 mm, %zu vertices: %.2f s\n", vertex_count(isolation), seconds_since(start));

    // 3. A hatch of the contour at 0.1 mm.
    start = std::chrono::steady_clock::now();
    const LineClipper clipper(contour);
    std::size_t parts = 0;
    for (Coord y = -100000000; y <= 100000000; y += 100000) parts += clipper.clip({-101000000, y}, {101000000, y}).size();
    std::printf("hatch: 2,001 rows across the contour, %zu parts: %.2f s\n", parts, seconds_since(start));
}

int main(int argc, char **argv) {
    try {
        wide_arithmetic();
        conversions();
        measures();
        booleans();
        offsets();
        joined_offsets();
        hulls();
        lines();
        order();
        limits();
        generated();
        if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) bench();
    } catch (const std::exception &error) {
        std::printf("FAIL an exception: %s\n", error.what());
        ++failures;
    }
    std::printf("resolving took at most %d rounds\n", detail::resolve_rounds());
    // The same on every compiler and processor, and with either 128-bit integer.
    const std::uint64_t recorded = 0xf3d117d67084a0ecULL;
    std::printf("digest %016llx\n", static_cast<unsigned long long>(digest));
    expect(digest == recorded, "the generated results differ from the recorded digest");
    std::printf(failures ? "polygon self-test FAILED\n" : "polygon self-test passed\n");
    return failures ? 1 : 0;
}
