// A check of the solid kernel that needs no Python and no other library: exact expected results on small
// cases, and invariants that need no oracle on generated ones.
//
//   kerfkite_solid_selftest                  the fixed checks and a few hundred generated cases (CTest runs this)
//   kerfkite_solid_selftest --stress N SEED  N generated cases of every kind, each result verified exactly
//   kerfkite_solid_selftest --digest N SEED  N generated cases; prints one hash of every resulting mesh, to
//                                            compare builds by different compilers (the hashes must be equal)
//   kerfkite_solid_selftest --plain N SEED   N generated pairs of solids, each operation computed twice: with the
//                                            kernel's shorter ways and with every step in full; they must agree
//   kerfkite_solid_selftest --bench          timings against the targets in docs/SOLID-KERNEL.md
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unordered_map>

#include "../core/src/solid/arrangement.hpp"
#include "../core/src/solid/bucket.hpp"
#include "../core/src/solid/bvh.hpp"
#include "../core/src/solid/edgemap.hpp"
#include "../core/src/solid/exact.hpp"
#include "../core/src/solid/planar.hpp"
#include "kerfkite/solid.hpp"

using namespace kerfkite;
using namespace kerfkite::solid;

static int failures = 0;

static void expect(bool condition, const std::string &what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static bool close_to(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

template <class F>
static bool refuses(F &&make) {
    try {
        make();
    } catch (const ValueError &) {
        return true;
    }
    return false;
}

// A small generator (xorshift), so every platform makes the same cases.
static uint64_t state = 88172645463325252ULL;
static uint64_t rnd() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
static double uni(double a, double b) { return a + (b - a) * static_cast<double>(rnd() % 1000001) / 1e6; }
static int pick(int n) { return static_cast<int>(rnd() % static_cast<uint64_t>(n)); }

static uint64_t digest = 1469598103934665603ULL;
static void mix(uint64_t v) {
    for (int k = 0; k < 8; ++k) {
        digest ^= (v >> (8 * k)) & 0xff;
        digest *= 1099511628211ULL;
    }
}
static void mix(const Solid &s) {
    mix(s.num_vertices());
    for (const GridPoint &p : s.grid_vertices())
        for (size_t k = 0; k < 3; ++k) mix(static_cast<uint64_t>(p[k]));
    for (const Triangle &t : s.triangles())
        for (size_t k = 0; k < 3; ++k) mix(t[k]);
}

static double rings_area(const std::vector<Ring> &rings) {
    double total = 0;
    for (const Ring &r : rings) total += ring_area(r);
    return total;
}

// ---- fixed checks -------------------------------------------------------------------------------

static void arithmetic() {
    using detail::BigInt;
    using detail::I128;
    for (int i = 0; i < 20000; ++i) {
        const int64_t a = static_cast<int64_t>(rnd()) >> pick(40), b = static_cast<int64_t>(rnd()) >> pick(40), c = static_cast<int64_t>(rnd() >> 26) - (int64_t(1) << 37);
        const I128 p = I128::mul(a, b);
        // (a * b) as a big integer both ways, and through a third factor small enough to stay in range.
        expect(BigInt::cmp(BigInt(p), BigInt(a) * BigInt(b)) == 0, "128-bit product against the big integer");
        const I128 q = I128::mul(a >> 26, b >> 26).times(c);
        expect(BigInt::cmp(BigInt(q), BigInt(a >> 26) * BigInt(b >> 26) * BigInt(c)) == 0, "128-bit times 64-bit against the big integer");
        expect(BigInt::cmp(BigInt(p + q) - BigInt(q), BigInt(p)) == 0 && BigInt::cmp(BigInt(p - q) + BigInt(q), BigInt(p)) == 0, "128-bit sums");
        expect(p.sign() == BigInt(p).sign() && (p < q) == (BigInt::cmp(BigInt(p), BigInt(q)) < 0), "128-bit sign and order");
    }
    // (x + y)^2 = x^2 + 2xy + y^2 on numbers of several hundred bits.
    BigInt x(1), y(1);
    for (int i = 0; i < 9; ++i) {
        x = x * BigInt(static_cast<int64_t>(rnd() >> 2)) + BigInt(i);
        y = y * BigInt(-static_cast<int64_t>(rnd() >> 3)) - BigInt(7);
    }
    expect(BigInt::cmp((x + y) * (x + y), x * x + BigInt(2) * x * y + y * y) == 0, "big integer square identity");
    expect(BigInt::cmp(x * y - y * x, BigInt(0)) == 0 && (x - x).zero(), "big integer zero");
    bool exact = false;
    expect(detail::round_ratio(BigInt(7), BigInt(2), exact) == 4 && !exact, "7/2 rounds to 4");
    expect(detail::round_ratio(BigInt(-7), BigInt(2), exact) == -3 && !exact, "-7/2 rounds to -3");
    expect(detail::round_ratio(x * BigInt(123456789), x.sign() < 0 ? -x : x, exact) == (x.sign() < 0 ? -123456789 : 123456789) && exact, "an exact quotient");
    const detail::Interval third = detail::ratio_interval(BigInt(1), BigInt(3));
    expect(third.lo < 1.0 / 3 && third.hi > 1.0 / 3 && third.hi - third.lo < 1e-15, "an interval encloses 1/3 tightly");
    // Orientation of rational points against the same points scaled to integers.
    detail::Pool pool;
    const int a = pool.add_grid(0, 0, 0), b = pool.add_grid(10, 0, 0), c = pool.add_grid(0, 10, 0);
    const int64_t pa[3] = {3, 3, -5}, pb[3] = {3, 3, 7}, pc[3] = {1, 1, -1}, pd[3] = {2, 5, 9};
    const detail::Plane floor = detail::plane_of(pool.pts[static_cast<size_t>(a)].i, pool.pts[static_cast<size_t>(b)].i, pool.pts[static_cast<size_t>(c)].i);
    const int e = pool.add_grid(pa[0], pa[1], pa[2]), f = pool.add_grid(pb[0], pb[1], pb[2]), g = pool.add_grid(pc[0], pc[1], pc[2]), h = pool.add_grid(pd[0], pd[1], pd[2]);
    const int on = pool.line_plane(e, f, floor);
    expect(pool.grid(on) && pool.pts[static_cast<size_t>(on)].i[0] == 3 && pool.pts[static_cast<size_t>(on)].i[2] == 0, "a crossing that is a grid point becomes one");
    const int cross = pool.line_plane(g, h, floor);  // (1.1, 1.4, 0)
    expect(!pool.grid(cross) && pool.orient3d(a, b, c, cross) == 0, "a constructed point lies exactly in its plane");
    expect(pool.orient2d(a, b, cross, 0, 1) > 0 && pool.compare(cross, a, 0) > 0 && pool.compare(cross, pool.line_plane(h, g, floor), 0) == 0, "predicates on a rational point");
    expect(pool.line_plane(h, g, floor) == cross, "the same point found twice is one point");
    int64_t rounded[3];
    pool.round(cross, rounded);
    expect(rounded[0] == 1 && rounded[1] == 1 && rounded[2] == 0, "rounding to the nearest grid point");
}

// ---- the shorter ways against the full computation ------------------------------------------------
//
// The kernel has ways round work that would give nothing new: points left pending until something needs
// them, crossings remembered by edge and ratio, turns decided in doubles within a proven bound, planes
// and lines known from how a point or a triangle was made, tables laid out flat. None of them may change
// an answer. Each is checked here against the computation it stands in for, on generated inputs that
// include the degenerate ones: points on the grid, the same point reached two ways, points in one line.

static void shorter_ways_arithmetic() {
    using detail::BigInt;
    using detail::I128;
    using detail::U256;
    // The 256-bit product that compares two ratios, against the big integer.
    for (int i = 0; i < 20000; ++i) {
        const I128 a = I128::mul(static_cast<int64_t>(rnd() >> (1 + pick(60))), static_cast<int64_t>(rnd() >> (10 + pick(50))));
        const I128 b = I128::mul(static_cast<int64_t>(rnd() >> (1 + pick(60))), static_cast<int64_t>(rnd() >> (10 + pick(50))));
        const U256 product = U256::product(a, b);
        BigInt sum;
        for (int k = 3; k >= 0; --k) sum = sum * BigInt(int64_t(1) << 32) * BigInt(int64_t(1) << 32) + BigInt(I128(0, product.w[k]));
        expect(BigInt::cmp(sum, BigInt(a) * BigInt(b)) == 0, "the 256-bit product against the big integer");
        // Equal ratios are found equal, and a ratio one unit off is not.
        const int64_t k = 1 + static_cast<int64_t>(rnd() >> 40);
        const I128 a2 = I128::mul(static_cast<int64_t>(rnd() >> 30), k), b2 = I128::mul(static_cast<int64_t>(rnd() >> 30) + 1, k);
        const I128 a1 = I128::mul(static_cast<int64_t>(a2.to_double() / static_cast<double>(k) + 0.5), 1), b1 = I128::mul(static_cast<int64_t>(b2.to_double() / static_cast<double>(k) + 0.5), 1);
        expect(U256::product(a1, b2) == U256::product(a2, b1), "equal ratios have equal cross products");
        expect(!(U256::product(a1 + I128(1), b2) == U256::product(a2, b1)), "unequal ratios have unequal cross products");
    }
    // The three-word running sum against the big integer, through sums that change sign.
    for (int round = 0; round < 300; ++round) {
        detail::Sum192 quick;
        BigInt slow;
        for (int term = 0; term < 200; ++term) {
            const I128 a = I128::mul(static_cast<int64_t>(rnd()) >> pick(50), static_cast<int64_t>(rnd() >> (12 + pick(40))));
            const int64_t m = (static_cast<int64_t>(rnd()) >> (24 + pick(38))) * (pick(9) ? 1 : 0);
            quick.add(a, m);
            slow = slow + BigInt(a) * BigInt(m);
        }
        const BigInt got = quick.big();
        expect(BigInt::cmp(got, slow) == 0 && got.to_double() == slow.to_double(), "the three-word running sum against the big integer");
    }
    // The counting sort against std::sort, with many items to a key and with few.
    for (int round = 0; round < 60; ++round) {
        struct Item {
            int key, b, c;
        };
        const size_t range = static_cast<size_t>(1 + pick(round % 2 ? 40 : 5000));
        std::vector<Item> items(static_cast<size_t>(pick(3000)));
        for (size_t k = 0; k < items.size(); ++k) items[k] = Item{pick(static_cast<int>(range)), pick(50), static_cast<int>(k)};
        std::vector<Item> expected = items, scratch;
        std::sort(expected.begin(), expected.end(), [](const Item &x, const Item &y) { return x.key != y.key ? x.key < y.key : x.b != y.b ? x.b < y.b : x.c < y.c; });
        std::vector<size_t> starts;
        detail::sort_by_key(items, range, [](const Item &x) { return static_cast<size_t>(x.key); }, [](const Item &x, const Item &y) { return x.b != y.b ? x.b < y.b : x.c < y.c; }, starts, scratch);
        bool same = items.size() == expected.size();
        for (size_t k = 0; same && k < items.size(); ++k) same = items[k].key == expected[k].key && items[k].b == expected[k].b && items[k].c == expected[k].c;
        expect(same, "the counting sort gives the order of std::sort");
    }
    // The edge table against the standard one, through sets, changes and removals.
    {
        detail::EdgeMap flat;
        std::unordered_map<uint64_t, uint32_t> standard;
        bool same = true;
        for (int i = 0; i < 200000 && same; ++i) {
            const uint64_t key = detail::EdgeMap::key(static_cast<uint32_t>(pick(300)), static_cast<uint32_t>(pick(300)));
            const int what = pick(5);
            if (what < 2) {
                const uint32_t value = static_cast<uint32_t>(rnd());
                flat.set(key, value);
                standard[key] = value;
            } else if (what == 2) {
                flat.erase(key);
                standard.erase(key);
            } else {
                const uint32_t *found = flat.find(key);
                const auto it = standard.find(key);
                same = (found != nullptr) == (it != standard.end()) && (!found || *found == it->second) && flat.has(key) == (found != nullptr);
            }
        }
        expect(same, "the edge table agrees with the standard table");
    }
    // A tree over part of the boxes against looking at every box of that part.
    for (int round = 0; round < 40; ++round) {
        std::vector<detail::Box> boxes(static_cast<size_t>(1 + pick(400)));
        for (detail::Box &b : boxes)
            for (int k = 0; k < 3; ++k) {
                b.lo[k] = pick(2000) - 1000;
                b.hi[k] = b.lo[k] + pick(round % 3 ? 60 : 900);
            }
        const size_t first = static_cast<size_t>(pick(static_cast<int>(boxes.size()))), count = static_cast<size_t>(pick(static_cast<int>(boxes.size() - first) + 1));
        detail::Bvh tree;
        tree.build(boxes, first, count);
        bool same = true;
        for (int q = 0; q < 50 && same; ++q) {
            detail::Box query;
            for (int k = 0; k < 3; ++k) {
                query.lo[k] = pick(2200) - 1100;
                query.hi[k] = query.lo[k] + pick(500);
            }
            std::vector<int> found, expected;
            tree.query(boxes, query, [&](int item) { found.push_back(item); });
            for (size_t k = first; k < first + count; ++k)
                if (boxes[k].overlaps(query)) expected.push_back(static_cast<int>(k));
            std::sort(found.begin(), found.end());
            same = found == expected;
        }
        expect(same, "a tree over a range of boxes reports what a scan of the range does");
    }
}

// Two pools given the same requests, one leaving crossings pending and one computing every point at once,
// must number the points alike and answer every predicate alike.
static void shorter_ways_points() {
    using detail::I128;
    using detail::Plane;
    using detail::Pool;
    size_t pending = 0, on_grid = 0, repeated = 0, rejoined = 0, asked = 0;
    for (int round = 0; round < 400; ++round) {
        Pool lazy, eager;
        eager.plain = true;
        // Small coordinates make coincidences common (crossings on the grid, one point from two edges);
        // large ones exercise the widths of the numbers.
        const int64_t span = round % 4 == 0 ? 4 : round % 4 == 1 ? 40 : round % 4 == 2 ? 1000000 : 60000000000LL;
        const auto coordinate = [&] { return static_cast<int64_t>(rnd() % static_cast<uint64_t>(2 * span + 1)) - span; };
        std::vector<int> ids;
        for (int k = 0; k < 24; ++k) {
            const int64_t x = coordinate(), y = coordinate(), z = coordinate();
            const int a = lazy.add_grid(x, y, z), b = eager.add_grid(x, y, z);
            expect(a == b, "grid points are numbered alike");
            ids.push_back(a);
        }
        bool alike = true;
        struct Request {
            int p, q;
            I128 at_p, at_q;
        };
        std::vector<Request> made;
        for (int request = 0; request < 160 && alike; ++request) {
            int p = ids[static_cast<size_t>(pick(24))], q = ids[static_cast<size_t>(pick(24))];
            const int a = ids[static_cast<size_t>(pick(24))], b = ids[static_cast<size_t>(pick(24))], c = ids[static_cast<size_t>(pick(24))];
            Plane plane = detail::plane_of(lazy.pts[static_cast<size_t>(a)].i, lazy.pts[static_cast<size_t>(b)].i, lazy.pts[static_cast<size_t>(c)].i);
            const int kind = pick(4);
            if (kind == 0) {
                // A plane square to Z through the middle of the edge: the crossing is on the grid when the
                // sums of the other coordinates are even.
                const int64_t *u = lazy.pts[static_cast<size_t>(p)].i, *v = lazy.pts[static_cast<size_t>(q)].i;
                if ((u[2] + v[2]) % 2) continue;
                const int64_t z = (u[2] + v[2]) / 2, o[3] = {0, 0, z}, ex[3] = {1, 0, z}, ey[3] = {0, 1, z};
                plane = detail::plane_of(o, ex, ey);
            }
            if (plane.n[0].zero() && plane.n[1].zero() && plane.n[2].zero()) continue;
            I128 at_p = detail::plane_value(plane, lazy.pts[static_cast<size_t>(p)].i), at_q = detail::plane_value(plane, lazy.pts[static_cast<size_t>(q)].i);
            if (kind == 1 && !made.empty()) {
                // An earlier crossing asked for again: from the other end, with the plane's values scaled
                // and perhaps of the other sign. It is the same point.
                const Request &earlier = made[static_cast<size_t>(pick(static_cast<int>(made.size())))];
                const int64_t scale = (1 + pick(1000)) * (pick(2) ? 1 : -1);
                p = earlier.q;
                q = earlier.p;
                at_p = earlier.at_q.times(scale);
                at_q = earlier.at_p.times(scale);
            }
            if (at_p.sign() * at_q.sign() >= 0) continue;
            ++asked;
            made.push_back(Request{p, q, at_p, at_q});
            const size_t before = lazy.pts.size();
            const int x = lazy.line_plane(p, q, at_p, at_q), y = eager.line_plane(p, q, at_p, at_q);
            alike = x == y && lazy.pts.size() == eager.pts.size() && lazy.rational_points() == eager.rational_points();
            if (lazy.pts.size() == before) ++repeated;
            else if (lazy.grid(x)) ++on_grid;
            else if (lazy.pts[static_cast<size_t>(x)].pending >= 0) ++pending;
            if (pick(3) == 0) {
                // The same point again as the meeting of three planes: it must be found, pending or not.
                // (Only where the request was made from this plane: a repeated one has scaled values.)
                const Plane e1 = detail::plane_through_edge(lazy.pts[static_cast<size_t>(p)].i, lazy.pts[static_cast<size_t>(q)].i, pick(3));
                const Plane e2 = detail::plane_through_edge(lazy.pts[static_cast<size_t>(p)].i, lazy.pts[static_cast<size_t>(q)].i, pick(3));
                const int u = lazy.three_planes(plane, e1, e2), v = eager.three_planes(plane, e1, e2);
                alike = alike && u == v && (u < 0 || kind == 1 || u == x) && lazy.pts.size() == eager.pts.size();
                if (u >= 0 && kind != 1) ++rejoined;
            }
            if (pick(4) == 0) {
                // A predicate between two requests settles some points early; the numbering must not care.
                const int n = static_cast<int>(lazy.pts.size());
                const int i = pick(n), j = pick(n), k = pick(n);
                const int axis = pick(3);
                alike = alike && lazy.compare(i, j, axis) == eager.compare(i, j, axis);
                alike = alike && lazy.orient2d(i, j, k, 0, 1) == eager.orient2d(i, j, k, 0, 1);
            }
        }
        expect(alike, "pending crossings are numbered as crossings computed at once");
        if (!alike) continue;
        // Every predicate on every kind of point, then the points themselves once settled.
        const int n = static_cast<int>(lazy.pts.size());
        bool same = true;
        for (int test = 0; test < 400 && same; ++test) {
            const int i = pick(n), j = pick(n), k = pick(n), m = pick(n), axis = pick(3);
            same = lazy.compare(i, j, axis) == eager.compare(i, j, axis) && lazy.orient2d(i, j, k, axis, (axis + 1) % 3) == eager.orient2d(i, j, k, axis, (axis + 1) % 3) &&
                   lazy.orient3d(i, j, k, m) == eager.orient3d(i, j, k, m);
            const Plane plane = detail::plane_of(lazy.pts[static_cast<size_t>(ids[0])].i, lazy.pts[static_cast<size_t>(ids[1 + pick(3)])].i, lazy.pts[static_cast<size_t>(ids[4 + pick(3)])].i);
            same = same && lazy.side(plane, i) == eager.side(plane, i);
        }
        for (int i = 0; i < n && same; ++i) {
            lazy.settle(i);
            const detail::Pt &a = lazy.pts[static_cast<size_t>(i)], &b = eager.pts[static_cast<size_t>(i)];
            int64_t ra[3], rb[3];
            lazy.round(i, ra);
            eager.round(i, rb);
            same = lazy.grid(i) == eager.grid(i) && ra[0] == rb[0] && ra[1] == rb[1] && ra[2] == rb[2];
            for (int k = 0; k < 3 && same; ++k) same = a.c[k].lo == b.c[k].lo && a.c[k].hi == b.c[k].hi;
        }
        expect(same, "a pending point, once computed, is the point computed at once");
    }
    expect(asked > 20000 && pending > 5000 && on_grid > 500 && repeated > 5000 && rejoined > 1000, "the point requests covered pending points, grid points, repeats and points reached two ways");
}

// The turn decided in doubles against the exact turn: whenever the doubles claim a sign it is the exact
// one, and they must claim none for three points of one line.
static void shorter_ways_turns() {
    using detail::I128;
    using detail::Planar;
    using detail::Plane;
    using detail::Pool;
    size_t decided = 0, undecided = 0, zero = 0, wrong = 0;
    for (int round = 0; round < 300; ++round) {
        Pool pool;
        const int64_t span = round % 3 == 0 ? 30 : round % 3 == 1 ? 100000 : 60000000000LL;
        const auto coordinate = [&] { return static_cast<int64_t>(rnd() % static_cast<uint64_t>(2 * span + 1)) - span; };
        std::vector<int> grid, points;
        for (int k = 0; k < 16; ++k) grid.push_back(pool.add_grid(coordinate(), coordinate(), coordinate()));
        points = grid;
        // Points along a few edges, several to an edge, so that many triples lie in one line; and points
        // a hair off such a line (the same edge cut by a plane moved by one unit).
        for (int e = 0; e < 8; ++e) {
            const int p = grid[static_cast<size_t>(pick(16))], q = grid[static_cast<size_t>(pick(16))];
            for (int cut = 0; cut < 6; ++cut) {
                Plane plane = detail::plane_of(pool.pts[static_cast<size_t>(grid[static_cast<size_t>(pick(16))])].i, pool.pts[static_cast<size_t>(grid[static_cast<size_t>(pick(16))])].i,
                                                pool.pts[static_cast<size_t>(grid[static_cast<size_t>(pick(16))])].i);
                if (plane.n[0].zero() && plane.n[1].zero() && plane.n[2].zero()) continue;
                if (cut % 2) plane.d = plane.d + I128(pick(3) - 1);
                const I128 at_p = detail::plane_value(plane, pool.pts[static_cast<size_t>(p)].i), at_q = detail::plane_value(plane, pool.pts[static_cast<size_t>(q)].i);
                if (at_p.sign() * at_q.sign() >= 0) continue;
                points.push_back(pool.line_plane(p, q, at_p, at_q));
            }
        }
        for (const int id : points) pool.settle(id);
        for (int test = 0; test < 3000; ++test) {
            const int a = points[static_cast<size_t>(pick(static_cast<int>(points.size())))], b = points[static_cast<size_t>(pick(static_cast<int>(points.size())))],
                      c = points[static_cast<size_t>(pick(static_cast<int>(points.size())))];
            const int u = pick(3), v = (u + 1 + pick(2)) % 3;
            const detail::Pt &pa = pool.pts[static_cast<size_t>(a)], &pb = pool.pts[static_cast<size_t>(b)], &pc = pool.pts[static_cast<size_t>(c)];
            const int exact = pool.orient2d(a, b, c, u, v);
            const int quick = Planar::filtered_turn(Planar::local_of(pa.c[u], pa.c[v], pa.rat < 0), Planar::local_of(pb.c[u], pb.c[v], pb.rat < 0), Planar::local_of(pc.c[u], pc.c[v], pc.rat < 0));
            if (quick == 2) ++undecided;
            else ++decided;
            if (exact == 0) ++zero;
            if (quick != 2 && quick != exact) ++wrong;
        }
    }
    expect(wrong == 0, "a turn decided in doubles is the exact turn");
    expect(decided > 100000 && undecided > 10000 && zero > 10000, "the turns covered decided, undecided and collinear triples");
}

// The side of a plane decided in doubles against the exact value: whenever the doubles claim a side it is
// the exact one, for points far from the plane, on it, and one grid unit off it, at every size of
// coordinate the kernel allows.
static void shorter_ways_sides() {
    using detail::Arrangement;
    using detail::Plane;
    size_t decided = 0, undecided = 0, on_plane = 0, wrong = 0;
    for (int round = 0; round < 4000; ++round) {
        const int64_t span = round % 4 == 0 ? 20 : round % 4 == 1 ? 50000 : round % 4 == 2 ? 3000000000LL : 100000000000LL;
        const auto coordinate = [&] { return static_cast<int64_t>(rnd() % static_cast<uint64_t>(2 * span + 1)) - span; };
        int64_t a[3], b[3], c[3];
        for (int k = 0; k < 3; ++k) {
            a[k] = coordinate();
            b[k] = coordinate();
            c[k] = coordinate();
        }
        if (round % 5 == 0) a[2] = b[2] = c[2];  // a plane square to an axis
        const Plane plane = detail::plane_of(a, b, c);
        if (plane.n[0].zero() && plane.n[1].zero() && plane.n[2].zero()) continue;
        const std::array<double, 5> doubles = Arrangement::plane_in_doubles(plane);
        for (int test = 0; test < 60; ++test) {
            int64_t p[3];
            if (test % 3 == 0) {
                for (int k = 0; k < 3; ++k) p[k] = coordinate();
            } else {
                // A point of the plane (a + i (b - a) + j (c - a), where it stays in range), or one unit off it.
                const int64_t i = pick(5) - 2, j = pick(5) - 2;
                bool inside = true;
                for (int k = 0; k < 3; ++k) {
                    p[k] = a[k] + i * (b[k] - a[k]) + j * (c[k] - a[k]);
                    inside = inside && p[k] <= detail::kMaxCoord && p[k] >= -detail::kMaxCoord;
                }
                if (!inside) continue;
                if (test % 3 == 2) p[pick(3)] += pick(2) ? 1 : -1;
            }
            const int exact = detail::plane_value(plane, p).sign(), quick = Arrangement::side_in_doubles(doubles.data(), p);
            if (quick == 2) ++undecided;
            else ++decided;
            if (exact == 0) ++on_plane;
            if (quick != 2 && quick != exact) ++wrong;
        }
    }
    expect(wrong == 0, "a side of a plane decided in doubles is the exact side");
    expect(decided > 50000 && undecided > 20000 && on_plane > 20000, "the sides covered points off the plane, on it and beside it");
}

// Whether two triangles meet, asked of a probing arrangement (which makes its crossings for one pair and
// drops them) and of a counting one: on triangles of a small lattice, where they touch at corners, along
// edges and within one plane as often as not.
static void shorter_ways_probing() {
    size_t compared = 0, meeting = 0, differ = 0;
    for (int test = 0; test < 60000; ++test) {
        // Six points; two triangles from them, so that they share none, one or two corners.
        std::vector<GridPoint> points(6);
        const int span = test % 4 == 0 ? 1 : test % 4 == 1 ? 2 : test % 4 == 2 ? 4 : 1000;
        for (GridPoint &point : points)
            for (int k = 0; k < 3; ++k) point[static_cast<size_t>(k)] = pick(2 * span + 1) - span;
        if (test % 5 == 0)
            for (GridPoint &point : points) point[2] = 0;  // all in one plane
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        if (points.size() < 4) continue;
        const uint32_t n = static_cast<uint32_t>(points.size());
        std::vector<Triangle> triangles(2);
        for (Triangle &t : triangles) {
            // Three different points (they may still lie in one line: such a triangle is left out on loading).
            const uint32_t a = static_cast<uint32_t>(pick(static_cast<int>(n))), b = (a + 1 + static_cast<uint32_t>(pick(static_cast<int>(n) - 1))) % n;
            uint32_t c = static_cast<uint32_t>(pick(static_cast<int>(n)));
            while (c == a || c == b) c = (c + 1) % n;
            t = Triangle{{a, b, c}};
        }
        const std::vector<detail::Operand> operands(1, detail::Operand{&points, &triangles, 0, false, nullptr});
        detail::Arrangement counting, probing;
        counting.load(operands);
        probing.load(operands);
        probing.probe_only();
        const std::vector<char> every(2, 1);
        const bool counted = counting.intersect() > 0, probed = probing.intersect(&every, true) > 0;
        ++compared;
        meeting += counted;
        differ += counted != probed;
    }
    expect(differ == 0, "a probing arrangement finds the pairs that meet, as a counting one does");
    expect(compared > 40000 && meeting > 8000 && compared - meeting > 8000, "the probed pairs covered meeting and separate triangles");
}

// Whole operations with every shorter way taken and with none: the same vertices, triangles and counts.
static void shorter_ways_operations(int cases);

static void trigonometry() {
    double s, c;
    const double exact[][3] = {{0, 0, 1}, {90, 1, 0}, {180, 0, -1}, {270, -1, 0}, {360, 0, 1}, {-90, -1, 0}, {30, 0.5, std::sqrt(0.75)}, {45, std::sqrt(0.5), std::sqrt(0.5)}, {150, 0.5, -std::sqrt(0.75)}, {720 + 60, std::sqrt(0.75), 0.5}};
    for (const double *row : exact) {
        sincos_degrees(row[0], s, c);
        expect(s == row[1] && c == row[2], "exact sine and cosine at " + std::to_string(row[0]));
    }
    // Against the C library, whose own argument (degrees times pi / 180) is rounded: within 90 degrees that
    // costs under 2e-16, so the comparison there is tight.
    double worst = 0, wide = 0;
    for (int i = 0; i < 20000; ++i) {
        const double degrees = uni(-90, 90), far = uni(-720, 720);
        sincos_degrees(degrees, s, c);
        worst = std::fmax(worst, std::fmax(std::fabs(s - std::sin(degrees * 3.14159265358979323846 / 180)), std::fabs(c - std::cos(degrees * 3.14159265358979323846 / 180))));
        sincos_degrees(far, s, c);
        wide = std::fmax(wide, std::fmax(std::fabs(s - std::sin(far * 3.14159265358979323846 / 180)), std::fabs(c - std::cos(far * 3.14159265358979323846 / 180))));
        expect(std::fabs(s * s + c * c - 1) < 5e-16, "sine and cosine lie on the unit circle");
    }
    expect(worst < 4e-16 && wide < 4e-15, "sine and cosine agree with the C library (worst " + std::to_string(worst * 1e16) + "e-16 within a quarter turn)");
    for (int n : {3, 5, 12, 96, 97, 360}) {
        for (int k = 0; k < n; ++k) {
            double s2, c2;
            sincos_turn(k, n, s, c);
            sincos_turn(n - k, n, s2, c2);
            expect(s == -s2 + 0.0 && c == c2, "a circle of " + std::to_string(n) + " is symmetric");
        }
    }
}

static void primitives_and_measures() {
    const Solid cube = box({1, 1, 1});
    expect(cube.num_vertices() == 8 && cube.num_triangles() == 12 && cube.num_bodies() == 1, "a box has 8 vertices and 12 triangles");
    expect(cube.volume() == 1 && cube.surface_area() == 6, "volume and area of the unit cube");
    const Vec3 centre = box({2, 4, 6}).translated({1, 1, 1}).centroid();
    expect(centre.x == 2 && centre.y == 3 && centre.z == 4, "centroid of a box");
    const Box3 bounds = box({2, 4, 6}, true).bounds();
    expect(bounds.min.x == -1 && bounds.min.y == -2 && bounds.min.z == -3 && bounds.max.x == 1 && bounds.max.z == 3, "bounds of a centred box");
    const Solid cyl = cylinder(10, 4, 4, 96);
    expect(cyl.num_triangles() == 4 * 96 - 4 && close_to(cyl.volume(), 0.5 * 96 * 16 * std::sin(2 * 3.14159265358979323846 / 96) * 10, 1e-3), "a cylinder is a prism over the inscribed polygon");
    const Solid cone = cylinder(6, 4, 0, 64, true);
    const Box3 cb = cone.bounds();
    expect(cb.min.x == -4 && cb.max.x == 4 && cb.min.y == -4 && cb.max.y == 4 && cb.min.z == -3 && cb.max.z == 3, "bounds of a centred cone");
    expect(close_to(cone.volume(), 32 * 16 * std::sin(2 * 3.14159265358979323846 / 64) * 2, 1e-3), "volume of a cone");
    const Solid ball = sphere(3, 32);
    const Box3 bb = ball.bounds();
    expect(bb.min.x == -3 && bb.max.x == 3 && bb.min.z == -3 && bb.max.z == 3 && close_to(ball.volume(), 4 * 3.14159265358979323846 * 27 / 3, 4), "a sphere touches its bounding box");
    expect(sphere(1, 30).num_triangles() == sphere(1, 32).num_triangles(), "sphere segments round up to a multiple of four");
    expect(count_self_intersections(ball) == 0 && verify_exact(ball) && verify_exact(cyl) && verify_exact(cone), "primitives are valid solids");
    const std::vector<Vec3> normals = cube.triangle_normals();
    bool unit = true;
    for (const Vec3 &n : normals) unit = unit && std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) == 1;
    expect(unit && cube.vertex_normals().size() == 8, "normals of a cube are the axes");
    expect(refuses([] { box({0, 1, 1}); }) && refuses([] { cylinder(1, 0, 0, 12); }) && refuses([] { sphere(-1, 12); }) && refuses([] { cylinder(1, 1, 1, 2); }) &&
               refuses([] { box({1e9, 1, 1}); }) && refuses([] { sphere(1e-9, 12); }),
           "impossible primitives are refused");
}

static void meshes() {
    const std::vector<Vec3> v = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const std::vector<Triangle> good = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
    expect(close_to(from_mesh(v, good).volume(), 1.0 / 6, 1e-15), "a tetrahedron from the caller's triangles");
    expect(refuses([&] { from_mesh(v, {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}}); }), "an open mesh is refused");
    expect(refuses([&] { from_mesh(v, {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {0, 2, 3}}); }), "an inside-out mesh is refused");
    expect(refuses([&] { from_mesh(v, {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}, {0, 1, 1}}); }), "a triangle without area is refused");
    expect(refuses([&] { from_mesh(v, {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 9}}); }), "a missing vertex is refused");
    expect(refuses([&] { from_mesh(v, {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 2, 3}}); }), "an inconsistently oriented mesh is refused");
    // Two tetrahedra that share only an edge, given as loose triangles: welded into two bodies.
    std::vector<Vec3> soup;
    std::vector<Triangle> loose;
    const Vec3 tetra[2][4] = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{0, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 0, -1}}};
    const int faces[4][3] = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
    for (const auto &corners : tetra)
        for (const int *f : faces) {
            const uint32_t base = static_cast<uint32_t>(soup.size());
            for (int k = 0; k < 3; ++k) soup.push_back(corners[f[k]]);
            loose.push_back({base, base + 1, base + 2});
        }
    const Solid pair = from_mesh(soup, loose, true);
    expect(pair.num_bodies() == 2 && pair.num_vertices() == 8 && close_to(pair.volume(), 1.0 / 3, 1e-15), "triangles touching along an edge weld into two bodies");
}

static void transforms() {
    const Solid part = box({2, 3, 4}) - cylinder(6, 0.5, 0.5, 12).translated({1, 1, -1});
    const Solid turned = part.rotated(90, 0, 0).rotated(0, 0, 180).rotated(0, 270, 0);
    expect(turned.volume() == part.volume() && turned.num_triangles() == part.num_triangles(), "quarter turns are exact");
    expect(part.mirrored({1, 0, 0}).volume() == part.volume() && part.scaled({-1, 1, 1}).volume() == part.volume(), "mirrors keep the volume and the orientation");
    expect(part.mirrored({1, 0, 0}).grid_vertices() == part.scaled({-1, 1, 1}).grid_vertices(), "a mirror in X is a scale by -1");
    expect(part.scaled({2, 2, 2}).volume() == 8 * part.volume(), "a whole-number scale is exact");
    expect(part.translated({0.1, 0.2, 0.3}).translated({-0.1, -0.2, -0.3}).grid_vertices() == part.grid_vertices(), "translation is exact and reversible");
    const Solid tilted = box({4, 2, 3}).rotated_about({0, 1, 0}, 90);
    const Box3 b = tilted.bounds();
    expect(b.min.x == 0 && b.max.x == 3 && b.min.z == -4 && b.max.z == 0, "rotation about an axis");
    const Solid sheared = box({1, 1, 1}).transformed({1, 1, 0, 5, 0, 2, 0, 3, 0, 0, 1, 0});
    expect(sheared.volume() == 2 && sheared.bounds().min.x == 5 && sheared.bounds().max.x == 7, "a general matrix");
    const Solid free = box({1, 2, 3}).rotated(12.5, -40, 77);
    expect(close_to(free.volume(), 6, 1e-5) && verify_exact(free), "a free rotation keeps a valid solid");
    expect(refuses([&] { part.scaled({0, 1, 1}); }) && refuses([&] { part.translated({1e9, 0, 0}); }), "maps that flatten or leave the range are refused");
}

static void booleans() {
    const Solid a = box({1, 1, 1}), b = a.translated({0.5, 0.5, 0.5});
    const Solid u = a + b, i = a ^ b, d = a - b;
    expect(u.volume() == 1.875 && i.volume() == 0.125 && d.volume() == 0.875, "two unit cubes offset by half: exact volumes");
    expect(u.num_vertices() == 20 && u.num_triangles() == 36 && i.num_vertices() == 8 && i.num_triangles() == 12 && d.num_vertices() == 14 && d.num_triangles() == 24, "two unit cubes offset by half: counts");
    expect(verify_exact(u) && verify_exact(i) && verify_exact(d), "two unit cubes offset by half: valid");
    expect((a + a).grid_vertices() == a.grid_vertices() && (a ^ a).triangles() == a.triangles() && (a - a).empty(), "a solid with itself");
    const Solid side = a + a.translated({1, 0, 0});
    expect(side.volume() == 2 && side.num_vertices() == 8 && side.num_triangles() == 12, "cubes sharing a face become one box");
    expect((a ^ a.translated({1, 0, 0})).empty() && (a - a.translated({1, 0, 0})).grid_vertices() == a.grid_vertices(), "touching cubes have no intersection");
    const Solid edge = a + a.translated({1, 1, 0});
    expect(edge.volume() == 2 && edge.num_bodies() == 2 && edge.num_vertices() == 16 && verify_exact(edge), "cubes sharing an edge stay two bodies");
    const Solid corner = a + a.translated({1, 1, 1});
    expect(corner.volume() == 2 && corner.num_bodies() == 2, "cubes sharing a corner stay two bodies");
    const Solid shell = box({4, 4, 4}) - box({2, 2, 2}).translated({1, 1, 1});
    expect(shell.volume() == 56 && shell.num_bodies() == 2 && verify_exact(shell), "a cavity is a second shell");
    expect((shell + box({2, 2, 2}).translated({1, 1, 1})).volume() == 64 && (box({2, 2, 2}).translated({1, 1, 1}) - box({4, 4, 4})).empty(), "one solid inside another");
    const Solid slit = box({4, 4, 4}) - (box({1, 4, 2}).translated({0, 0, 1}) + box({1, 4, 2}).translated({3, 0, 1})) - box({2, 2, 4}).translated({1, 1, 0});
    expect(verify_exact(slit) && close_to(slit.volume(), 64 - 16 - 16, 1e-12), "cuts that meet along edges");
    const Solid plate = box({40, 30, 6}) - cylinder(8, 4, 4, 96).translated({20, 15, -1});
    expect(close_to(plate.volume(), 7200 - 0.5 * 96 * 16 * std::sin(2 * 3.14159265358979323846 / 96) * 6, 1e-3) && plate.num_triangles() == 400 && verify_exact(plate), "a plate with a bore");
    const Solid lens = sphere(4, 48) ^ sphere(4, 48).translated({3, 0, 0});
    expect(lens.num_bodies() == 1 && verify_exact(lens) && close_to(lens.volume(), 3.14159265358979323846 * 19 * 25 / 12, 2), "two spheres");
    // A cylinder and a sphere of the same radius and segments share a whole circle of vertices.
    const Solid capped = sphere(4, 96) + cylinder(10, 4, 4, 96);
    expect(close_to(capped.volume(), sphere(4, 96).volume() / 2 + cylinder(10, 4, 4, 96).volume(), 1e-9) && verify_exact(capped), "a sphere and a cylinder that share an equator");
    // Batches.
    std::vector<Solid> row;
    for (int k = 0; k < 12; ++k) row.push_back(cylinder(2, 1, 1, 24).translated({0.75 * k, 0, 0}));
    const Solid merged = batch_boolean(row, Op::Union);
    Solid folded;
    for (const Solid &s : row) folded = folded + s;
    expect(close_to(merged.volume(), folded.volume(), 1e-6) && merged.num_bodies() == 1 && verify_exact(merged), "a batch union equals the unions one by one");
    std::vector<Solid> cut = {box({12, 4, 1}).translated({-2, -2, 0.5})};
    cut.insert(cut.end(), row.begin(), row.end());
    expect(close_to(batch_boolean(cut, Op::Difference).volume(), (cut[0] - merged).volume(), 1e-6), "a batch difference equals the difference from the union");
    expect(batch_boolean({}, Op::Union).empty() && batch_boolean({a}, Op::Intersection).volume() == 1 && close_to(batch_boolean({a, b, a.translated({0.25, 0.25, 0.25})}, Op::Intersection).volume(), 0.125, 1e-12), "small batches");
    std::vector<Solid> many;
    for (int k = 0; k < 150; ++k) many.push_back(box({0.5, 0.5, 0.5}).translated({static_cast<double>(k % 10), static_cast<double>(k / 10 % 5), static_cast<double>(k / 50)}));
    const Solid crowd = batch_boolean(many, Op::Union);
    expect(crowd.num_bodies() == 150 && crowd.volume() == 150 * 0.125, "many small bodies");
    // Faces that would coincide but for rounding: the common rotation moves each vertex by up to half a unit.
    const Solid left = box({10, 10, 10}).rotated(10, 20, 30), right = box({10, 10, 10}).translated({10, 0, 0}).rotated(10, 20, 30);
    const Solid joined = left + right;
    expect(joined.num_bodies() == 1 && close_to(joined.volume(), 2000, 1e-3) && verify_exact(joined) && (left ^ right).volume() < 1e-3, "boxes that share a face after a common rotation");
    const Solid nearly = left + left.translated({1e-6, 0, 0});
    expect(verify_exact(nearly) && close_to(nearly.volume(), 1000, 1e-2) && (left - left.translated({0, 1e-6, -2e-6})).volume() < 1e-2, "a solid and a copy a nanometre away");
}

static void profiles() {
    const Ring square = {{0, 0}, {4, 0}, {4, 4}, {0, 4}}, hole = {{1, 1}, {1, 3}, {3, 3}, {3, 1}};
    expect(extrude({square}, 5).volume() == 80 && extrude({square, hole}, 5).volume() == 60 && extrude({square, hole}, 5).num_triangles() == 32, "prisms with and without a hole");
    ExtrudeOptions point;
    point.scale_x = point.scale_y = 0;
    expect(extrude({square}, 6, point).volume() == 32 && extrude({square}, 6, point).num_vertices() == 5, "a pyramid by scaling the top to a point");
    ExtrudeOptions twist;
    twist.slices = 12;
    twist.twist_degrees = 90;
    twist.scale_x = twist.scale_y = 0.5;
    const Solid twisted = extrude({{{4, 0}, {6, 0}, {6, 2}, {4, 2}}}, 5, twist);
    expect(verify_exact(twisted) && close_to(twisted.bounds().max.z, 5, 0) && twisted.num_triangles() == 100, "a twisted, tapered extrusion");
    const Ring bow = {{0, 0}, {4, 4}, {4, 0}, {0, 4}};
    expect(extrude({bow}, 1).volume() == 8 && extrude({bow}, 1).num_bodies() == 2, "a self-crossing ring is resolved by the fill rule");
    expect(close_to(rings_area(resolve_rings({square, hole}, FillRule::EvenOdd)), 12, 1e-12) && close_to(rings_area(resolve_rings({square, hole}, FillRule::NonZero)), 12, 1e-12) &&
               close_to(rings_area(resolve_rings({square, {{1, 1}, {3, 1}, {3, 3}, {1, 3}}}, FillRule::NonZero)), 16, 1e-12) &&
               close_to(rings_area(resolve_rings({square, {{1, 1}, {3, 1}, {3, 3}, {1, 3}}}, FillRule::EvenOdd)), 12, 1e-12),
           "fill rules");
    const Solid quarter = revolve({{{2, 0}, {4, 0}, {4, 5}, {2, 5}}}, 64, 90);
    const Box3 qb = quarter.bounds();
    expect(qb.min.x == 0 && qb.min.y == 0 && qb.max.x == 4 && qb.max.y == 4 && qb.max.z == 5 && close_to(quarter.volume(), 3.14159265358979323846 * 12 * 5 / 4, 0.01) && quarter.num_triangles() == 4 * 2 * 64 + 4 &&
               verify_exact(quarter),
           "a quarter turn of a rectangle");
    const Solid lathe = revolve({{{0, 0}, {4, 0}, {4, 10}, {0, 10}}}, 96);
    expect(close_to(lathe.volume(), cylinder(10, 4, 4, 96).volume(), 1e-9) && lathe.num_bodies() == 1, "a profile on the axis turns into a cylinder");
    const Solid pinch = revolve({{{0, 1}, {1, 0}, {2, 1}, {1, 2}}}, 24);
    expect(verify_exact(pinch) && pinch.volume() > 12, "a profile touching the axis at a point");
    expect(refuses([&] { extrude({{{0, 0}, {1, 0}}}, 1); }) && refuses([&] { extrude({square}, 0); }) && refuses([&] { revolve({{{-1, 0}, {1, 0}, {0, 1}}}, 12); }) &&
               refuses([&] { revolve({square}, 12, 0); }) && refuses([&] { extrude({{{0, 0}, {1, 0}, {2, 0}}}, 1); }),
           "impossible profiles are refused");
    const Solid hull = convex_hull(std::vector<Solid>{box({2, 3, 4}), box({1, 1, 1}).translated({0.5, 0.5, 0.5})});
    expect(hull.volume() == 24 && hull.num_vertices() == 8 && hull.num_triangles() == 12, "the hull of a box and points inside it is the box");
    const Solid tent = convex_hull(std::vector<Vec3>{{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}, {1, 1, 0}, {1, 1, 3}, {1, 0, 0}});
    expect(convex_hull(std::vector<Solid>{Solid(), Solid()}).empty(), "the hull of empty solids is empty");
    expect(tent.volume() == 4 && tent.num_vertices() == 5 && refuses([] { convex_hull(std::vector<Vec3>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}); }), "a hull drops points on its faces and edges; flat input is refused");
}

static void sections_and_queries() {
    const Solid plate = box({40, 30, 6}) - cylinder(4, 4, 4, 96).translated({20, 15, 2});
    expect(section(plate, 0).size() == 1 && section(plate, 3).size() == 2 && section(plate, 6).empty() && section(plate, -1).empty(), "sections count the blind pocket only above its floor");
    expect(close_to(rings_area(section(plate, 0)), 1200, 1e-9) && close_to(rings_area(section(plate, 4)), 1200 - 0.5 * 96 * 16 * std::sin(2 * 3.14159265358979323846 / 96), 1e-3), "section areas");
    // A straight side has no points along it, however many triangles of one face the plane crosses.
    const std::vector<Ring> through = section(plate, 4);
    size_t outline = 0, bore = 0;
    for (const Ring &r : through) (ring_area(r) > 0 ? outline : bore) += r.size();
    expect(outline == 4 && bore == 96, "a section has a point at each corner and none inside a flat face");
    const PlaneSection across = section(box({10, 10, 10}), Plane3{{5, 5, 5}, {1, 1, 1}});
    expect(across.rings.size() == 1 && across.rings[0].size() == 6, "a hexagonal section has six points");
    const Solid shape = sphere(5, 48) - box({4, 4, 20}, true);
    double integral = 0;
    for (int k = 0; k < 1000; ++k) integral += rings_area(section(shape, -5 + (k + 0.5) * 0.01)) * 0.01;
    expect(close_to(integral, shape.volume(), 1e-3 * shape.volume()), "section areas integrate to the volume");
    const PlaneSection flat = section(plate, Plane3{{0, 0, 3}, {0, 0, 2}});
    expect(flat.rings.size() == 2 && close_to(rings_area(flat.rings), rings_area(section(plate, 3)), 1e-9) && flat.u.x == 1 && flat.v.y == 1, "a horizontal plane gives the horizontal section");
    const PlaneSection diagonal = section(box({10, 10, 10}), Plane3{{5, 5, 5}, {1, 1, 1}});
    expect(diagonal.rings.size() == 1 && close_to(ring_area(diagonal.rings[0]), 1.5 * std::sqrt(3.0) * 50, 1e-9), "a cube cut through its centre across the diagonal is a regular hexagon");
    const std::vector<Ring> shadow = projection(box({40, 30, 6}) - cylinder(8, 4, 4, 96).translated({20, 15, -1}));
    expect(shadow.size() == 2 && projection(plate).size() == 1 && close_to(rings_area(projection(plate)), 1200, 1e-9), "the projection shows a through hole and not a blind one");
    const Index index(plate);
    const Hit centre = index.raycast({20, 15, 50}, {0, 0, -1}), top = index.raycast({5, 5, 50}, {0, 0, -3}), vertex = index.raycast({0, 0, 50}, {0, 0, -1});
    expect(centre.hit && centre.distance == 48 && centre.normal.z == 1 && top.hit && top.distance == 44 && vertex.hit && vertex.distance == 44, "rays from above, also through a vertex");
    expect(!index.raycast({20, 15, 50}, {0, 0, 1}).hit && !index.raycast({20, 15, 50}, {0, 0, -1}, 10).hit, "rays that miss or stop short");
    expect(index.contains({5, 5, 3}) && !index.contains({20, 15, 4}) && index.contains({20, 15, 1}) && !index.contains({50, 5, 3}), "points inside and outside");
    expect(!index.occluded({20, 15, 2}, {0, 0, 1}, 1e300, 1e-6) && index.occluded({20, 15, 2}, {1, 0, 0.1}, 1e300, 1e-6), "the pocket floor is reachable from above only");
    const HeightMap map = height_map(plate, 0, 0, 1, 1, 41, 31, -1);
    size_t disagree = 0;
    for (size_t j = 0; j < map.rows; ++j)
        for (size_t i = 0; i < map.columns; ++i) {
            const Hit ray = index.raycast({static_cast<double>(i), static_cast<double>(j), 100}, {0, 0, -1});
            disagree += !close_to(ray.hit ? 100 - ray.distance : -1, map.at(i, j), 1e-9);
        }
    expect(map.at(20, 15) == 2 && map.at(5, 5) == 6 && map.at(40, 30) == 6 && disagree == 0, "a height map agrees with rays cast from above");
    const HeightMap side = height_map(plate, frame_for_axis({0, 0, 0}, {1, 0, 0}), -40, -10, 1, 1, 50, 20, -1);
    double highest = -1;
    for (const double z : side.z) highest = std::fmax(highest, z);
    expect(highest == 40, "a height map along another axis");
    expect(sample_surface(box({10, 10, 10}), 1.0).size() == 1200, "surface samples");
    expect(min_gap(box({1, 1, 1}), box({1, 1, 1}).translated({1.5, 0, 0}), 10) == 0.5 && min_gap(box({1, 1, 1}), box({1, 1, 1}).translated({1, 0, 0}), 10) == 0 &&
               min_gap(box({1, 1, 1}), box({1, 1, 1}).translated({3, 4, 0}), 1) == 1,
           "the gap between two solids");
    const std::string binary = to_stl_binary(plate), ascii = to_stl_ascii(plate);
    expect(binary.size() == 84 + 50 * plate.num_triangles() && binary.compare(0, 25, "KerfKite Studio solid, mm") == 0, "binary STL layout");
    const Solid again = from_stl(ascii);
    expect(again.grid_vertices() == plate.grid_vertices() && again.triangles() == plate.triangles(), "ASCII STL reads back to the same solid");
    expect(from_stl(binary).num_triangles() == plate.num_triangles() && close_to(from_stl(binary).volume(), plate.volume(), 1e-2), "binary STL reads back (single precision)");
    expect(refuses([] { from_stl("solid x\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nendloop\nendfacet\nendsolid x\n"); }) && refuses([] { from_stl(std::string(100, 'x')); }) &&
               refuses([&] { read_stl(binary, 10); }) && refuses([] { from_stl("solid x\nendsolid x\n"); }),
           "malformed or oversized STL is refused");
}

// ---- generated cases ----------------------------------------------------------------------------

// The order in which a compiler evaluates the arguments of a call is its own choice, so every random
// draw below is a statement of its own (or an element of a braced list, which is ordered): otherwise
// two compilers would generate different cases and their digests could not be compared.
static Solid turned(const Solid &s, double range) {
    const double x = uni(-range, range), y = uni(-range, range), z = uni(-range, range);
    return s.rotated(x, y, z);
}

static Solid base() {
    switch (pick(3)) {
        case 0: return box({1.0 + pick(5), 1.0 + pick(5), 1.0 + pick(5)});
        case 1: {
            const int height = 1 + pick(5), low = 1 + pick(3), high = pick(4) ? 1 + pick(3) : 0, segments = 8 + 4 * pick(3);
            return cylinder(height, low, high, segments);
        }
        default: {
            const int radius = 1 + pick(3), segments = 8 + 4 * pick(2);
            return sphere(radius, segments);
        }
    }
}

static Solid placed() {
    Solid s = base();
    switch (pick(6)) {
        case 0: return s.translated({static_cast<double>(pick(9) - 4), static_cast<double>(pick(9) - 4), static_cast<double>(pick(9) - 4)});
        case 1: return s.translated({uni(-4, 4), uni(-4, 4), uni(-4, 4)});
        case 2: {
            const double x = 90.0 * pick(4), y = 90.0 * pick(4), z = 45.0 * pick(8);
            return s.rotated(x, y, z);
        }
        case 3: {
            const Solid t = turned(s, 180);
            return t.translated({static_cast<double>(pick(5) - 2), 0, 0});
        }
        case 4: return s.scaled({pick(2) ? 2 : uni(0.3, 2), 1, pick(2) ? -1.0 : 1.0});
        default: return s;
    }
}

static size_t refusals = 0, refused_kind[6] = {0, 0, 0, 0, 0, 0};

static bool valid(const Solid &s, bool verify) { return s.empty() || !verify || (count_self_intersections(s) == 0 && verify_exact(s)); }

// One generated case of the given kind. Returns false (after printing) when an invariant fails.
static bool generated(int kind, bool verify) {
    const uint64_t seed = state;
    bool ok = true;
    const auto fail = [&](const char *what) {
        std::printf("FAIL generated kind %d seed %llu: %s\n", kind, static_cast<unsigned long long>(seed), what);
        ok = false;
    };
    try {
        if (kind == 0 || kind == 1 || kind == 2) {
            Solid a, b;
            if (kind == 0) {  // two placed primitives, one perhaps already cut or joined
                a = placed();
                b = placed();
                if (pick(3) == 0) {
                    const Solid other = placed();
                    const Solid c = boolean(a, other, pick(2) ? Op::Union : Op::Difference);
                    if (!c.empty()) a = c;
                }
            } else if (kind == 1) {  // sharing faces, then turned together, so that they nearly coincide
                a = base();
                b = base();
                const Box3 ba = a.bounds();
                b = b.translated({pick(2) ? ba.max.x - b.bounds().min.x : static_cast<double>(pick(3)), static_cast<double>(pick(3) - 1), pick(2) ? 0.0 : ba.max.z - b.bounds().min.z});
                const double rx = uni(-180, 180), ry = uni(-180, 180), rz = uni(-180, 180);
                a = a.rotated(rx, ry, rz);
                b = b.rotated(rx, ry, rz);
            } else {  // a solid and a copy turned by a millionth of a degree or moved a few nanometres
                a = turned(base(), 180);
                if (pick(2)) {
                    const double x = uni(-1e-6, 1e-6), y = uni(-1e-6, 1e-6), z = uni(-1e-5, 1e-5);
                    b = a.rotated(x, y, z);
                } else {
                    b = a.translated({(pick(7) - 3) * 1e-6, (pick(7) - 3) * 1e-6, (pick(7) - 3) * 1e-6});
                }
            }
            const Solid u = a + b, i = a ^ b, d = a - b, u2 = b + a, i2 = b ^ a;
            mix(u);
            mix(i);
            mix(d);
            // Rounding moves each new vertex by under a nanometre, and features under a few nanometres are
            // removed: the identities hold to that thickness times the area involved.
            const double tolerance = (kind == 0 ? 3e-6 : 6e-5) * (a.surface_area() + b.surface_area()) + 1e-9;
            if (!close_to(u.volume() + i.volume(), a.volume() + b.volume(), tolerance)) fail("V(A+B) + V(A^B) = V(A) + V(B)");
            if (!close_to(d.volume(), a.volume() - i.volume(), tolerance)) fail("V(A-B) = V(A) - V(A^B)");
            if (!close_to(u.volume(), u2.volume(), tolerance) || !close_to(i.volume(), i2.volume(), tolerance)) fail("union and intersection commute");
            if (!valid(u, verify) || !valid(i, verify) || !valid(d, verify)) fail("a result is not a valid solid");
        } else if (kind == 3) {  // a chain of operations
            Solid acc = base();
            for (int k = 0; k < 5; ++k) {
                const Solid shape = base();
                const double x = pick(2) ? 90.0 * pick(4) : uni(-90, 90), z = pick(2) ? 45.0 * pick(8) : uni(-90, 90);
                const Solid t = shape.rotated(x, 0, z).translated({static_cast<double>(pick(5) - 2), static_cast<double>(pick(5) - 2), static_cast<double>(pick(5) - 2)});
                const int op = pick(3);
                Solid next = boolean(acc, t, op == 0 ? Op::Union : op == 1 ? Op::Difference : Op::Intersection);
                if (next.empty()) next = t;
                mix(next);
                if (!valid(next, verify)) fail("a result in a chain is not a valid solid");
                acc = next;
            }
        } else if (kind == 4) {  // a batch against the same operations one by one
            std::vector<Solid> parts;
            const int n = 2 + pick(8);
            Solid folded, rest;
            double area = 0;
            for (int k = 0; k < n; ++k) {
                const Solid shape = base();
                const double z = pick(2) ? 30.0 * pick(12) : uni(0, 360);
                const Solid t = shape.rotated(0, 0, z).translated({(pick(7) - 3) * 0.5, (pick(7) - 3) * 0.5, static_cast<double>(pick(3) - 1)});
                parts.push_back(t);
                folded = folded + t;
                if (k) rest = rest + t;
                area += t.surface_area();
            }
            const Solid all = batch_boolean(parts, Op::Union), cut = batch_boolean(parts, Op::Difference);
            mix(all);
            mix(cut);
            const double tolerance = 3e-6 * area * n + 1e-9;
            if (!close_to(all.volume(), folded.volume(), tolerance)) fail("a batch union equals the fold");
            if (!close_to(cut.volume(), (parts[0] - rest).volume(), tolerance)) fail("a batch difference equals the fold");
            if (!valid(all, verify) || !valid(cut, verify)) fail("a batch result is not a valid solid");
        } else {  // idempotence and absorption
            const Solid a = turned(base(), 180);
            const Solid b = base().translated({uni(-1, 1), uni(-1, 1), uni(-1, 1)});
            const Solid u = a + b, i = a ^ b, p = a + i, q = a ^ u;
            mix(p);
            mix(q);
            const double tolerance = 6e-5 * (a.surface_area() + b.surface_area()) + 1e-9;
            if ((a + a).grid_vertices() != a.grid_vertices() || (a ^ a).triangles() != a.triangles() || !(a - a).empty()) fail("A+A = A, A^A = A, A-A is empty");
            if (!close_to(p.volume(), a.volume(), tolerance) || !close_to(q.volume(), a.volume(), tolerance)) fail("A + (A^B) = A and A ^ (A+B) = A");
            if (!valid(p, verify) || !valid(q, verify)) fail("an absorption result is not a valid solid");
        }
    } catch (const ValueError &error) {
        // The one refusal allowed: a result whose features cannot be represented on the grid. It is counted.
        if (std::strstr(error.what(), "too fine")) {
            ++refusals;
            ++refused_kind[kind];
            return true;
        }
        fail(error.what());
    } catch (const std::exception &error) {
        fail(error.what());
    }
    return ok;
}


static void generate(int cases, bool verify) {
    for (int c = 0; c < cases; ++c)
        for (int kind = 0; kind < 6; ++kind) {
            if (!generated(kind, verify)) ++failures;
        }
}

static void shorter_ways_operations(int cases) {
    size_t compared = 0, refused = 0, differ = 0, with_new_points = 0, repaired = 0;
    // One operation on fresh copies of the operands (so that neither run inherits what the other
    // established about them), in the given mode.
    struct Outcome {
        bool refused = false;
        std::vector<GridPoint> vertices;
        std::vector<Triangle> triangles;
        BooleanStats stats;
    };
    const auto run = [](const Solid &a, const Solid &b, Op op, bool plain) {
        Outcome out;
        detail::plain_paths() = plain;
        try {
            const Solid fresh_a = from_grid_mesh(a.grid_vertices(), a.triangles()), fresh_b = from_grid_mesh(b.grid_vertices(), b.triangles());
            const Solid result = boolean(fresh_a, fresh_b, op);
            out.vertices = result.grid_vertices();
            out.triangles = result.triangles();
            out.stats = last_boolean_stats();
        } catch (const ValueError &) {
            out.refused = true;
        }
        detail::plain_paths() = false;
        return out;
    };
    {
        // A shape examined against itself once is recognised the next time and not examined again; the
        // solids and every count must be the same the first time, later, and with nothing remembered.
        const Solid plate = box({40, 30, 6}), bore = cylinder(8, 4, 4, 48), prism = extrude({{{0, 0}, {5, 0}, {5, 3}, {2, 5}, {0, 3}}}, 9);
        for (int moved = 0; moved < 3; ++moved) {
            const Solid tool = (moved % 2 ? prism : bore).translated({10.0 + 7 * moved, 12.0, -1.0});
            const Outcome first = run(plate, tool, Op::Difference, false), again = run(plate, tool, Op::Difference, false), full = run(plate, tool, Op::Difference, true);
            expect(first.vertices == again.vertices && first.triangles == again.triangles && first.vertices == full.vertices && first.triangles == full.triangles &&
                       first.stats.candidate_pairs == again.stats.candidate_pairs && first.stats.candidate_pairs == full.stats.candidate_pairs &&
                       first.stats.constructed_points == full.stats.constructed_points && first.stats.candidate_pairs > 100,
                   "a shape recognised from an earlier operation gives the same solid and the same counts");
        }
    }
    for (int c = 0; c < cases; ++c) {
        Solid a, b;
        switch (c % 5) {
            case 0:  // placed primitives
                a = placed();
                b = placed();
                break;
            case 1: {  // sharing faces, then turned together: faces that nearly coincide
                a = base();
                b = base();
                const Box3 ba = a.bounds();
                b = b.translated({pick(2) ? ba.max.x - b.bounds().min.x : static_cast<double>(pick(3)), static_cast<double>(pick(3) - 1), pick(2) ? 0.0 : ba.max.z - b.bounds().min.z});
                const double rx = uni(-180, 180), ry = uni(-180, 180), rz = uni(-180, 180);
                a = a.rotated(rx, ry, rz);
                b = b.rotated(rx, ry, rz);
                break;
            }
            case 2:  // a solid and a copy a few nanometres away
                a = turned(base(), 180);
                b = a.translated({(pick(7) - 3) * 1e-6, (pick(7) - 3) * 1e-6, (pick(7) - 3) * 1e-6});
                break;
            case 3: {  // a result of an earlier operation against a third solid: many flat faces in pieces
                const Solid first = placed(), second = placed();
                try {
                    a = boolean(first, second, pick(2) ? Op::Union : Op::Difference);
                } catch (const ValueError &) {
                    a = first;
                }
                if (a.empty()) a = first;
                b = placed();
                break;
            }
            default:  // grid-aligned boxes and cylinders: crossings on the grid, coincident and touching faces
                a = pick(2) ? box({2.0 + pick(3), 2.0 + pick(3), 1.0 + pick(2)}) : cylinder(1 + pick(3), 1 + pick(2), 1 + pick(2), 8 + 4 * pick(3));
                b = (pick(2) ? box({1.0 + pick(3), 1.0 + pick(3), 1.0 + pick(3)}) : cylinder(1 + pick(3), 1, 1 + pick(2), 8 + 4 * pick(3))).translated({static_cast<double>(pick(4) - 1), static_cast<double>(pick(4) - 1), static_cast<double>(pick(3) - 1)});
                break;
        }
        for (const Op op : {Op::Union, Op::Difference, Op::Intersection}) {
            const Outcome fast = run(a, b, op, false), plain = run(a, b, op, true);
            ++compared;
            refused += fast.refused;
            with_new_points += fast.stats.constructed_points > 0;
            repaired += fast.stats.repair_passes > 0;
            const BooleanStats &x = fast.stats, &y = plain.stats;
            const bool same = fast.refused == plain.refused && fast.vertices == plain.vertices && fast.triangles == plain.triangles && x.candidate_pairs == y.candidate_pairs &&
                              x.intersecting_pairs == y.intersecting_pairs && x.split_triangles == y.split_triangles && x.coplanar_groups == y.coplanar_groups &&
                              x.constructed_points == y.constructed_points && x.rounded_vertices == y.rounded_vertices && x.repair_passes == y.repair_passes && x.coarser_retries == y.coarser_retries;
            if (!same) {
                ++differ;
                std::printf("FAIL the shorter ways changed a result: case %d, operation %d\n", c, static_cast<int>(op));
            }
        }
    }
    failures += static_cast<int>(differ);
    expect(with_new_points * 4 > compared && repaired > 0, "the compared operations made new points and needed repairs");
    (void)refused;
}

static double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void bench() {
    double t = seconds();
    for (const int segments : {144, 200, 320}) {
        // The second sphere is turned by an arbitrary angle, so that its vertices are in no special position.
        t = seconds();
        const Solid a = sphere(20, segments), b = sphere(20, segments).rotated(17, 29, 41).translated({13.1, 4.3, 2.7});
        std::printf("two spheres of %zu triangles each, built in %.3f s\n", a.num_triangles(), seconds() - t);
        for (const Op op : {Op::Union, Op::Difference, Op::Intersection}) {
            t = seconds();
            const Solid r = boolean(a, b, op);
            const double took = seconds() - t;
            std::printf("  %s: %.3f s, %zu triangles, %zu intersecting pairs, %zu rounded vertices\n", op == Op::Union ? "union" : op == Op::Difference ? "difference" : "intersection", took,
                        r.num_triangles(), last_boolean_stats().intersecting_pairs, last_boolean_stats().rounded_vertices);
        }
    }
    {
        // The hard case: the same sphere turned about its centre, so that every triangle crosses several others.
        const Solid a = sphere(20, 144), b = a.rotated(17, 29, 41);
        t = seconds();
        const Solid r = a + b;
        std::printf("union of a sphere of %zu triangles with itself turned: %.3f s, %zu triangles, %zu intersecting pairs\n", a.num_triangles(), seconds() - t, r.num_triangles(),
                    last_boolean_stats().intersecting_pairs);
    }
    std::vector<Solid> parts;
    for (int k = 0; k < 200; ++k) parts.push_back(cylinder(5, 2, 2, 48).translated({(k % 20) * 1.5, (k / 20) * 1.5, 0}));
    t = seconds();
    const Solid merged = batch_boolean(parts, Op::Union);
    std::printf("batch union of 200 overlapping cylinders (%zu triangles in): %.3f s, %zu triangles out\n", parts.size() * parts[0].num_triangles(), seconds() - t, merged.num_triangles());
    const Solid big = sphere(20, 320) - box({10, 10, 60}, true);
    t = seconds();
    const HeightMap map = height_map(big, -25, -25, 0.1, 0.1, 500, 500, -100);
    std::printf("height map 500 x 500 of %zu triangles: %.3f s\n", big.num_triangles(), seconds() - t);
    t = seconds();
    size_t points = 0;
    for (int k = 0; k < 100; ++k)
        for (const Ring &r : section(big, -19 + 0.37 * k)) points += r.size();
    std::printf("100 horizontal sections of the same solid: %.4f s each (%zu points in all)\n", (seconds() - t) / 100, points);
    t = seconds();
    const std::vector<Ring> shadow = projection(big);
    std::printf("projection: %.3f s (%zu rings)\n", seconds() - t, shadow.size());
    t = seconds();
    const Index index(big);
    size_t hits = 0;
    for (int k = 0; k < 200000; ++k) hits += index.raycast({-25 + 0.00025 * k, 0.3, 100}, {0.01, 0.02, -1}).hit;
    std::printf("index and 200000 rays: %.3f s (%zu hits)\n", seconds() - t, hits);
    t = seconds();
    std::printf("exact check of a %zu-triangle solid: %zu self-intersections", big.num_triangles(), count_self_intersections(big));
    std::printf(" in %.3f s\n", seconds() - t);
    (void)map;
}

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--bench") {
        bench();
        return 0;
    }
    if (mode == "--plain") {
        // Operations with every shorter way taken and with none, at length.
        const int cases = argc > 2 ? std::atoi(argv[2]) : 2000;
        if (argc > 3) state = std::strtoull(argv[3], nullptr, 10);
        shorter_ways_operations(cases);
        std::printf("%d cases, three operations each, with and without the shorter ways: %d differ\n", cases, failures);
        return failures ? 1 : 0;
    }
    if (mode == "--stress" || mode == "--digest") {
        const int cases = argc > 2 ? std::atoi(argv[2]) : 100;
        if (argc > 3) state = std::strtoull(argv[3], nullptr, 10);
        generate(cases, mode == "--stress");
        std::printf("%d cases of each of 6 kinds, %d failed, %zu refused as too fine (by kind: %zu %zu %zu %zu %zu %zu), digest %016llx\n", cases, failures, refusals, refused_kind[0], refused_kind[1],
                    refused_kind[2], refused_kind[3], refused_kind[4], refused_kind[5], static_cast<unsigned long long>(digest));
        return failures ? 1 : 0;
    }
    arithmetic();
    shorter_ways_arithmetic();
    shorter_ways_points();
    shorter_ways_turns();
    shorter_ways_sides();
    shorter_ways_probing();
    trigonometry();
    primitives_and_measures();
    meshes();
    transforms();
    booleans();
    profiles();
    sections_and_queries();
    generate(40, true);
    shorter_ways_operations(150);
    std::printf(failures ? "solid self-test FAILED\n" : "solid self-test passed\n");
    return failures ? 1 : 0;
}
