// A small check that needs no Python: number text and one complete program. The full comparison with the
// reference is validation/native_core_parity.py.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <array>
#include <string>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/leveling.hpp"
#include "kerfkite/line_check.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/operation_command.hpp"
#include "kerfkite/pcb.hpp"

using json = kerfkite::Json;
using namespace kerfkite;

static int failures = 0;

static void expect(bool condition, const std::string &what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static std::string fixed(double value, int decimals) {
    std::string out;
    text::fixed(out, value, decimals);
    return out;
}

template <typename Thrown> static std::string message(const char *project) {
    try {
        generate(json::parse(project));
    } catch (const Thrown &error) {
        return error.what();
    } catch (const std::exception &) {
        return "(another exception)";
    }
    return "(nothing thrown)";
}

// Whether a line crosses itself (Shapely's is_simple), which a ramp entry and holding tabs ask of every
// contour: known answers, the tree against the test of every pair on generated lines, and the work taken,
// which must not grow with the square of the length of the line. validation/native_core_parity.py compares
// the answers with Shapely's.
static void self_crossing() {
    using Line = std::vector<std::array<double, 2>>;
    const LineMethod methods[3] = {LineMethod::Usual, LineMethod::Tree, LineMethod::EveryPair};
    const auto says = [&](const Line &line, bool simple) {
        for (const LineMethod method : methods) {
            const LineCheck check = check_line(line, method);
            if (check.declined || check.simple != simple) return false;
        }
        return true;
    };
    expect(says({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, true), "a closed square is simple: its ends may meet");
    expect(says({{0, 0}, {10, 10}, {10, 0}, {0, 10}}, false), "a bow-tie crosses itself");
    expect(says({{0, 0}, {10, 0}, {10, 10}, {5, 0}, {0, 10}}, false), "a vertex on another segment is a touch");
    expect(says({{0, 0}, {10, 0}, {5, 0}, {5, 5}}, false), "a step back along a segment is an overlap");
    expect(says({{0, 0}, {10, 0}, {10, 10}, {0, 0}, {-5, 5}}, false), "a vertex passed twice, away from the ends");
    expect(says({{0, 0}, {5, 0}, {5, 0}, {10, 0}, {10, 10}}, true), "a repeated point and a vertex on a straight run are allowed");
    expect(says({{3, 3}}, true) && says({{3, 3}, {3, 3}}, true) && says({}, true), "a line of one point is simple");

    std::uint64_t state = 77;
    const auto draw = [&](std::uint64_t below) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((state >> 33) % below);
    };
    // Regular lines of n points, each simple: a zigzag, a comb of long parallel diagonals, a fan of rays, a
    // square spiral and a line of short steps. Every coordinate is a ratio of whole numbers.
    const auto regular = [&](int kind, int n) {
        Line line;
        double x = 0, y = 0;
        for (int i = 0; i < n; ++i) {
            if (kind == 0) {
                line.push_back({i % 2 ? 100.0 : 0.0, i * 0.5});
            } else if (kind == 1) {
                line.push_back({(i % 2 ? 100.0 : 0.0) + i * 0.001, i % 2 ? 100.0 : 0.0});
            } else if (kind == 2) {
                // Ray k runs from (k, n - k) / n to a hundred times that; out along one, back along the next.
                const int k = i / 2;
                const double rx = static_cast<double>(k) / n, ry = static_cast<double>(n - k) / n;
                const bool near = (i % 2 == 0) == (k % 2 == 0);
                line.push_back({near ? rx : 100 * rx, near ? ry : 100 * ry});
            } else if (kind == 3) {
                line.push_back({x, y});
                const double step = 100.0 - (i / 2) * (100.0 / n);
                x += i % 4 == 0 ? step : i % 4 == 2 ? -step : 0;
                y += i % 4 == 1 ? step : i % 4 == 3 ? -step : 0;
            } else {
                line.push_back({x, y});
                x += 0.25 + draw(8) / 8;
                y += draw(9) / 4 - 1;
            }
        }
        return line;
    };
    // The same answer from the tree as from every pair: small lines on a coarse grid, where shared
    // vertices, touches and overlaps are common, and regular lines with one fault somewhere.
    std::size_t compared = 0, simple = 0, mismatched = 0, declined = 0, through_tree = 0;
    const auto compare = [&](const Line &line) {
        const LineCheck every = check_line(line, LineMethod::EveryPair), tree = check_line(line, LineMethod::Tree),
                        usual = check_line(line, LineMethod::Usual);
        if (every.declined || tree.declined || usual.declined) {
            ++declined;
            return;
        }
        ++compared;
        simple += every.simple;
        if (tree.simple != every.simple || usual.simple != every.simple) {
            if (++mismatched <= 3) {
                std::printf("a line answered %d by every pair, %d by the tree, %d as usual:", every.simple, tree.simple, usual.simple);
                for (const std::array<double, 2> &point : line) std::printf(" (%.17g, %.17g)", point[0], point[1]);
                std::printf("\n");
            }
        }
        through_tree += tree.tree;
    };
    for (int round = 0; round < 4000; ++round) {
        const double scale = draw(4) == 0 ? 0.1 : 0.5;
        const int points = 2 + static_cast<int>(draw(11));
        Line line;
        for (int i = 0; i < points; ++i) {
            const double x = scale * draw(5);
            const double y = scale * draw(5);
            line.push_back({x, y});
        }
        compare(line);
    }
    for (int round = 0; round < 1500; ++round) {
        const int kind = static_cast<int>(draw(5));
        const int n = 50 + static_cast<int>(draw(250));
        Line line = regular(kind, n);
        const std::size_t i = static_cast<std::size_t>(draw(line.size() - 1));
        const std::size_t j = static_cast<std::size_t>(draw(line.size() - 1));
        const std::array<double, 2> a = line[j], b = line[j + 1], middle = {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
        const std::uint64_t fault = static_cast<std::uint64_t>(draw(7));
        if (fault == 0) {  // a vertex moved onto the middle of another segment
            line[i] = middle;
        } else if (fault == 1) {  // a vertex moved somewhere else
            const double fx = draw(1200) / 10 - 10;
            const double fy = draw(1200) / 10 - 10;
            line[i] = {fx, fy};
        } else if (fault == 2) {  // a vertex visited again later
            line.insert(line.begin() + static_cast<std::ptrdiff_t>(i), a);
        } else if (fault == 3) {  // a step back along a segment
            line.insert(line.begin() + static_cast<std::ptrdiff_t>(j + 1), middle);
            line.insert(line.begin() + static_cast<std::ptrdiff_t>(j + 1), b);
        } else if (fault == 4) {  // a vertex one unit in the last place off the middle of another segment
            line[i] = {middle[0], std::nextafter(middle[1], draw(2) == 0 ? -1e9 : 1e9)};
        } else if (fault == 5) {  // closed
            line.push_back(line[0]);
        }
        compare(line);
    }
    expect(mismatched == 0, "the tree and the test of every pair agree on whether a line crosses itself");
    // A line of fewer than three distinct points is simple without a tree.
    expect(compared > 5000 && simple > 500 && simple < compared - 2000 && declined < 20 && through_tree > compared - 400,
           "enough lines of each kind were compared, nearly all through the tree");

    // The work for long simple lines, where nothing ends the search early. Testing every pair of a comb of
    // n long segments looks at n * n / 2 pairs; the tree's work is within a small multiple of n log n.
    bool bounded = true;
    for (int kind = 0; kind < 5; ++kind) {
        const int n = 20000;
        const LineCheck check = check_line(regular(kind, n));
        const double allowed = 100.0 * n * std::log2(static_cast<double>(n));
        if (check.declined || !check.simple || !check.tree || static_cast<double>(check.work) > allowed) {
            std::printf("a line of kind %d: simple %d, declined %d, work %llu, allowed %.0f\n", kind, check.simple, check.declined,
                        static_cast<unsigned long long>(check.work), allowed);
            bounded = false;
        }
    }
    expect(bounded, "the work for a long simple line grows no faster than n log n");
    const Line comb = regular(1, 3000);
    const LineCheck by_tree = check_line(comb), by_pairs = check_line(comb, LineMethod::EveryPair);
    expect(by_tree.simple && by_pairs.simple && by_pairs.work > 3000ull * 3000 / 2 && by_tree.work * 10 < by_pairs.work,
           "a comb of long segments, which the test of every pair takes the square of its length over");
    // Past the budget a contour is declined, and the work stops there: that is the bound on the worst case.
    const LineCheck stopped = check_line(comb, LineMethod::Usual, 20000);
    expect(stopped.declined && stopped.reason == "a contour whose self-crossing test needs more comparisons than the core allows" &&
               stopped.work > 20000 && stopped.work < 20000 + 40000,
           "a contour whose test would take too long is declined");
    expect(line_check_budget() >= 100000000 && line_check_budget() <= 1000000000, "the budget is about a second of work");
    // A crossing ends the search: many long segments that all cross take almost no work.
    Line scribble;
    for (int i = 0; i < 20000; ++i) {
        const double x = draw(100000) / 1000;
        const double y = draw(100000) / 1000;
        scribble.push_back({x, y});
    }
    const LineCheck crossed = check_line(scribble);
    expect(!crossed.declined && !crossed.simple && crossed.work < 100.0 * 20000 * std::log2(20000.0), "a line with many crossings is answered at once");
    std::printf("self-crossing: %zu lines compared by tree and by every pair (%zu simple, %zu declined); comb of 3000: %llu units by tree, %llu by every pair\n",
                compared, simple, declined, static_cast<unsigned long long>(by_tree.work), static_cast<unsigned long long>(by_pairs.work));
}

// FNV-1a of the centres of the helices placed in the generated pockets of helical_entries(), recorded from
// x86-64 GCC. A centre is worked out with + - * / alone, so every platform must give this value.
static const std::uint64_t HELIX_DIGEST = 13467638686905150612ULL;

// The distance from a point to a segment, in plain floating point: the check of helical_entries(), which
// shares no code with the core.
static double away(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
    double t = length > 0 ? ((px - ax) * dx + (py - ay) * dy) / length : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

// The distance from a point to the nearest wall of a pocket given as closed contours.
static double to_walls(const json &contours, double px, double py) {
    double nearest = 1e300;
    for (const json &contour : contours)
        for (std::size_t i = 0; i + 1 < contour.size(); ++i)
            nearest = std::min(nearest, away(px, py, contour[i][0].get<double>(), contour[i][1].get<double>(),
                                             contour[i + 1][0].get<double>(), contour[i + 1][1].get<double>()));
    return nearest;
}

// Whether a point is in the pocket: inside an odd number of its contours.
static bool in_pocket(const json &contours, double px, double py) {
    bool inside = false;
    for (const json &contour : contours)
        for (std::size_t i = 0; i + 1 < contour.size(); ++i) {
            const double ax = contour[i][0].get<double>(), ay = contour[i][1].get<double>();
            const double bx = contour[i + 1][0].get<double>(), by = contour[i + 1][1].get<double>();
            if ((ay > py) != (by > py) && px < ax + (py - ay) * (bx - ax) / (by - ay)) inside = !inside;
        }
    return inside;
}

// Helical entries into pockets: where the helix goes, that the cutter stays clear of every wall and island
// there, the program built on it, and the refusals.
static void helical_entries() {
    // A 40 mm square pocket with a 12 mm square island, a 4 mm cutter and a helix of radius 1.5 mm.
    const char *settings = R"("mode":"router","protocol":"grbl","width":90,"height":90,"safe":3,"depth":2,"step":1,
        "power":9000,"feed":400,"plunge":90,"tool_diameter":4,"stepover":2,"operation":"pocket","router_entry":"helix",
        "entry_angle":3,"stock_thickness":4)";
    const std::string square = "[[10,10],[50,10],[50,50],[10,50],[10,10]]", island = "[[24,24],[36,24],[36,36],[24,36],[24,24]]";
    const auto pocket = [&](const std::string &paths, const std::string &radius) {
        return "{" + std::string(settings) + ",\"entry_radius\":" + radius + ",\"paths\":[" + paths + "]}";
    };
    const Job job = generate(json::parse(pocket(square + "," + island, "1.5")));
    const std::vector<std::string> &lines = job.lines;
    // The pass round the island starts at (22, 23.950903); its helix is centred 1.5024 mm away, inside the
    // region kept for centres, and starts on its circle at the point nearest that seam.
    expect(lines.size() == 3282 && lines[9] == "G0 Z3.000" && lines[10] == "G0 X21.998 Y23.951" &&
               lines[11] == "G1 X21.998 Y23.951 Z0.000 F90.0" && lines[12] == "G1 X21.966 Y24.190 Z-0.009 F90.0" &&
               lines[3281] == "M5",
           "a pocket entered by a helix");
    std::size_t falling = 0;
    bool steep = false;
    for (const Segment &move : job.segments) {
        if (!(move.end_z < move.start_z) || move.start_z > 0) continue;
        ++falling;
        const double run = std::hypot(move.nx - move.x, move.ny - move.y);
        if (!(run > 0) || (move.start_z - move.end_z) / run > std::tan(3 * 3.141592653589793 / 180) + 1e-7) steep = true;
    }
    expect(falling > 36 * 2 * 10 && !steep, "every fall into the stock is along a helix, no steeper than the entry angle");
    expect(job.warnings.size() == 3 && job.warnings[1] == "Concentric pocket clearing preserves islands. Narrow residual features may remain.",
           "its warnings");

    // The same places through the command Studio's Python calls.
    const json contours = json::parse("[" + square + "," + island + "]");
    const json passes = operation_call("operation", json{{"op", "pocket"}, {"paths", contours}, {"radius", 2.0}, {"step", 2.0}})["paths"];
    json seams = json::array();
    for (const json &pass : passes) seams.push_back(pass[0]);
    const json placed = operation_call("operation", json{{"op", "helix"}, {"paths", contours}, {"seams", seams},
                                                         {"bit_radius", 2.0}, {"radius", 1.5}})["entries"];
    expect(placed.size() == passes.size() && placed.size() == 10 && seams[1].dump() == "[12.0,12.0]" &&
               placed[1][0].dump() == "13.5024" && placed[1][1].dump() == "13.5024" &&
               std::fabs(placed[1][2].get<double>() + 2.356194490192345) < 1e-12 && placed[3].dump() == "[14.0,14.0,0.0]",
           "a helix is centred on the seam where that is clear, and otherwise as near it as the walls allow");

    // Refusals, with the reference's messages.
    const auto refusal = [&](const std::string &paths, const std::string &radius) {
        return message<ValueError>(pocket(paths, radius).c_str());
    };
    expect(refusal("[[10,10],[17,10],[17,17],[10,17],[10,10]]", "1.8") == "The pocket has insufficient room for the cutter and helix radius",
           "a pocket the cutter fits and the helix does not");
    expect(refusal("[[10,10],[40,10],[40,40],[10,40],[10,10]],[[50,10],[57,10],[57,17],[50,17],[50,10]]", "1.8") ==
               "No clear helix and connecting feed to this pocket contour; reduce the helix radius or use a contour ramp",
           "a small pocket beside a large one: no straight feed leads into it");
    expect(refusal(square, "2.5") == "Helix radius cannot exceed the cutter radius; a larger helix leaves a central core",
           "a helix wider than the cutter");
    expect(refusal(square, "\"wide\"") == "entry_radius must be numeric", "a helix radius that is not a number");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,"operation":"pocket",
        "router_entry":"helix","tool_diameter":3,"stepover":1.5,"entry_radius":0.05,"entry_angle":0.25,"depth":10,"step":10,
        "paths":[[[20,20],[25,20],[25,25],[20,25],[20,20]]]})") == "Helix exceeds the path segment limit",
           "a helix of more turns than a program may hold");

    // Generated pockets: a rectangle with its corners cut by different amounts, sometimes with a triangular
    // island and sometimes with a small second pocket beside it. Every coordinate is a whole number of
    // thousandths, so the pockets are the same on every platform. For each helix placed, the centre must be
    // in the pocket and at least cutter radius + helix radius from every wall, and the feed to the seam at
    // least the cutter radius from every wall, less the two half steps of the kernel's grid.
    std::uint64_t state = 2026, digest = 1469598103934665603ULL;
    const auto draw = [&](std::uint64_t below) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (state >> 33) % below;
    };
    const auto mm = [](std::uint64_t thousandths) { return static_cast<double>(thousandths) / 1000.0; };
    std::size_t helices = 0, refused = 0;
    bool clear = true;
    for (int round = 0; round < 120; ++round) {
        const double x0 = mm(20000 + draw(5000));
        const double y0 = mm(20000 + draw(5000));
        const double w = mm(9000 + draw(14000));
        const double h = mm(9000 + draw(14000));
        double cut[4];
        for (double &corner : cut) corner = mm(draw(4000));
        json outer = json::array({json::array({x0 + cut[0], y0}), json::array({x0 + w - cut[1], y0}), json::array({x0 + w, y0 + cut[1]}),
                                  json::array({x0 + w, y0 + h - cut[2]}), json::array({x0 + w - cut[2], y0 + h}),
                                  json::array({x0 + cut[3], y0 + h}), json::array({x0, y0 + h - cut[3]}), json::array({x0, y0 + cut[0]})});
        outer.push_back(outer[0]);
        json pockets = json::array({outer});
        const std::uint64_t extra = draw(3);
        if (extra == 1) {  // an island
            const double ix = x0 + w / 2 + mm(draw(3000)) - 1.5;
            const double iy = y0 + h / 2 + mm(draw(3000)) - 1.5;
            const double size = mm(500 + draw(2500));
            pockets.push_back(json::array({json::array({ix, iy}), json::array({ix + size, iy + size / 4}),
                                           json::array({ix + size / 3, iy + size}), json::array({ix, iy})}));
        } else if (extra == 2) {  // a second pocket, which the helix may not fit
            const double side = mm(1200 + draw(3500));
            pockets.push_back(json::array({json::array({x0 + w + 3, y0}), json::array({x0 + w + 3 + side, y0}),
                                           json::array({x0 + w + 3 + side, y0 + side}), json::array({x0 + w + 3, y0 + side}),
                                           json::array({x0 + w + 3, y0})}));
        }
        const double bit = mm(500 + 250 * draw(6));
        const double radius = std::min(bit, mm(50 + 150 * draw(8)));
        json entries, starts = json::array();
        try {
            const json cuts = operation_call("operation", json{{"op", "pocket"}, {"paths", pockets}, {"radius", bit}, {"step", bit}})["paths"];
            for (const json &pass : cuts) starts.push_back(pass[0]);
            entries = operation_call("operation", json{{"op", "helix"}, {"paths", pockets}, {"seams", starts},
                                                       {"bit_radius", bit}, {"radius", radius}})["entries"];
        } catch (const ValueError &) {
            ++refused;
            continue;
        }
        if (entries.size() != starts.size()) clear = false;
        for (std::size_t i = 0; i < entries.size() && i < starts.size(); ++i) {
            ++helices;
            const double cx = entries[i][0].get<double>(), cy = entries[i][1].get<double>(), angle = entries[i][2].get<double>();
            for (const double value : {cx, cy}) {
                std::uint64_t bits;
                static_assert(sizeof bits == sizeof value, "a double is 64 bits");
                std::copy(reinterpret_cast<const unsigned char *>(&value), reinterpret_cast<const unsigned char *>(&value) + 8,
                          reinterpret_cast<unsigned char *>(&bits));
                for (int byte = 0; byte < 8; ++byte) digest = (digest ^ ((bits >> (8 * byte)) & 0xff)) * 1099511628211ULL;
            }
            if (!in_pocket(pockets, cx, cy) || to_walls(pockets, cx, cy) < bit + radius) clear = false;
            // The feed, from the point of the circle where the helix ends to the seam, a tenth at a time.
            const double sx = cx + radius * std::cos(angle), sy = cy + radius * std::sin(angle);
            const double ex = starts[i][0].get<double>(), ey = starts[i][1].get<double>();
            for (int step = 0; step <= 10; ++step) {
                const double fx = sx + (ex - sx) * step / 10, fy = sy + (ey - sy) * step / 10;
                if (!in_pocket(pockets, fx, fy) || to_walls(pockets, fx, fy) < bit - 2e-6) clear = false;
            }
        }
    }
    expect(clear, "every helix and its feed keep the cutter clear of the pocket's walls and islands");
    expect(helices > 300 && refused > 5 && refused < 80, "the generated pockets place helices and refuse some");
    if (digest != HELIX_DIGEST) std::printf("helix centres: digest %llu\n", static_cast<unsigned long long>(digest));
    expect(digest == HELIX_DIGEST, "the helix centres are the recorded ones");
    std::printf("helical entries: %zu helices in generated pockets, %zu pockets refused\n", helices, refused);
}

// FNV-1a of the paths and depths carved in the generated shapes of v_carving(), recorded from x86-64 GCC.
// Nothing in a V-carve comes from the C library but square roots, so every platform must give this value.
static const std::uint64_t VCARVE_DIGEST = 3245276528135944660ULL;

// V-carving: the program, the refusals, and for generated shapes that the bit never cuts outside the
// outline, measured with the plain distance function above, which shares no code with the core.
static void v_carving() {
    // A 30 by 8 mm bar carved with a 90 degree bit: the middle of the bar is 4 mm from its sides, so with
    // half a millimetre taken off for the sampling the bit goes 3.5 mm deep there, less than the 6 mm asked.
    const char *settings = R"("mode":"router","protocol":"grbl","width":90,"height":90,"safe":3,"power":9000,"feed":400,
        "plunge":90,"operation":"vcarve")";
    const auto carve = [&](const std::string &paths, const std::string &more) {
        return "{" + std::string(settings) + "," + more + ",\"paths\":[" + paths + "]}";
    };
    const std::string bar = "[[10,10],[40,10],[40,18],[10,18],[10,10]]";
    const Job job = generate(json::parse(carve(bar, R"("v_angle":90,"v_resolution":0.5,"depth":6,"step":6,"tool_diameter":12)")));
    double deepest = 0, lowest_end = 0;
    for (const Segment &move : job.segments) deepest = std::min(deepest, move.end_z), lowest_end = std::min(lowest_end, std::min(move.start_z, move.end_z));
    expect(job.lines.size() > 400 && job.lines[9] == "G0 Z3.000" && job.lines.back() == "M5" && std::fabs(deepest + 3.5) < 1e-9,
           "a bar carved to the depth its width allows: " + std::to_string(job.lines.size()) + " lines, deepest " + std::to_string(deepest));
    expect(job.warnings.size() == 4 && job.warnings[1] == "Sampled medial-axis V-carve with conservative sampling allowance; fine detail and "
                                                           "wide-area flat clearing require inspection.",
           "its warning");
    expect(job.bounds[0] >= 10 && job.bounds[1] >= 10 && job.bounds[2] <= 40 && job.bounds[3] <= 18, "its bounds are inside the bar");
    // The depth of cut and the tool's radius each limit the depth.
    const auto deepest_of = [&](const std::string &more) {
        double low = 0;
        for (const Segment &move : generate(json::parse(carve(bar, more))).segments) low = std::min(low, move.end_z);
        return low;
    };
    expect(deepest_of(R"("v_angle":90,"v_resolution":0.5,"depth":2,"step":2,"tool_diameter":12)") == -2.0, "no deeper than the depth of cut");
    expect(std::fabs(deepest_of(R"("v_angle":90,"v_resolution":0.5,"depth":6,"step":6,"tool_diameter":5)") + 2.5) < 1e-12, "no deeper than the bit's radius allows");
    expect(deepest_of(R"("v_resolution":0.5,"depth":6,"step":3,"tool_diameter":12)") == -6.0, "two levels reach the depth of cut with a 60 degree bit");

    // The same paths through the command Studio's Python calls: a depth for every point, and every path
    // starting where the last ended or as near it as any path does.
    const json contours = json::parse("[" + bar + "]");
    const json carved = operation_call("operation", json{{"op", "vcarve"}, {"paths", contours}, {"resolution", 0.5}, {"angle", 90.0}, {"depth", 6.0}, {"radius", 6.0}});
    bool matched = carved["paths"].size() == carved["depths"].size() && carved["paths"].size() > 20;
    for (std::size_t i = 0; matched && i < carved["paths"].size(); ++i) matched = carved["paths"][i].size() == carved["depths"][i].size() && carved["paths"][i].size() >= 2;
    expect(matched, "the command gives a depth for every point");
    expect(carved["paths"][0][0].dump() == "[10.0,10.75]" && carved["depths"][0][0].dump() == "-0.0",
           "the first path starts at the smallest end, on the wall, at no depth: " + carved["paths"][0][0].dump() + " " + carved["depths"][0][0].dump());

    // Refusals, with the reference's messages and in its order.
    const auto refusal = [&](const std::string &paths, const std::string &more) { return message<ValueError>(carve(paths, more).c_str()); };
    expect(message<ValueError>(R"({"mode":"laser","operation":"vcarve","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[10,10],[30,10],[30,30],[10,30],[10,10]]]})") == "V-carving requires router mode",
           "V-carving with a laser");
    expect(message<ValueError>(R"({"mode":"laser","operation":"vcarve","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[10,10],[30,30],[30,10],[10,30],[10,10]]]})") == "Repair self-crossing or zero-area contours before CAM",
           "a contour that crosses itself is reported before the head");
    expect(refusal(bar, R"("v_resolution":0.05)") == "v_resolution must be between 0.1 and 5" && refusal(bar, R"("v_angle":171)") == "v_angle must be between 10 and 170" &&
               refusal(bar, R"("v_angle":"sharp")") == "v_angle must be numeric" && refusal(bar, R"("v_resolution":0.5,"tool_diameter":0.05)") == "tool_diameter must be between 0.1 and 100",
           "settings out of range");
    expect(refusal("[[2,2],[88,2],[88,88],[2,88],[2,2]],[[4,4],[86,4],[86,86],[4,86],[4,4]],[[6,6],[84,6],[84,84],[6,84],[6,6]],[[8,8],[82,8],[82,82],[8,82],[8,8]]",
                   R"("v_resolution":0.1)") == "V-carve outline too complex; increase sampling interval",
           "an outline of more than 12,000 samples");
    expect(refusal("[[10,10],[40,10],[40,10.8],[10,10.8],[10,10]]", R"("v_resolution":0.5)") == "Operation produced no toolpaths", "a bar narrower than the sampling allowance");
    expect(message<ValueError>(R"({"mode":"router","operation":"vcarve","width":2000,"height":2000,"feed":600,"power":1,"depth":50,"tool_diameter":100,
        "v_resolution":0.7,"paths":[[[10,10],[1990,10],[1990,1990],[10,1990],[10,10]]]})") == "Job exceeds 200,000 cutting segments",
           "a carve of more points than a job may hold");
    expect(refusal(bar, R"("v_resolution":0.5,"router_entry":"ramp")") == "Ramp and helix entries require flat end mills on closed profiles or pockets with one target depth",
           "a ramp into a V-carve");

    // Generated shapes: a rectangle with its corners cut by different amounts, sometimes with a triangular
    // island and sometimes with a second shape beside it, every coordinate a whole number of thousandths.
    // At every point of every path the circle the bit cuts at that depth must reach no wall: its radius is
    // at most the distance to the nearest wall less the sampling interval, so the straight move to the next
    // point, which is no longer than the interval, stays clear as well, and the middle of each move is
    // measured to show it.
    std::uint64_t state = 2027, digest = 1469598103934665603ULL;
    const auto draw = [&](std::uint64_t below) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (state >> 33) % below;
    };
    const auto mm = [](std::uint64_t thousandths) { return static_cast<double>(thousandths) / 1000.0; };
    const auto absorb = [&](double value) {
        std::uint64_t bits;
        std::copy(reinterpret_cast<const unsigned char *>(&value), reinterpret_cast<const unsigned char *>(&value) + 8, reinterpret_cast<unsigned char *>(&bits));
        for (int byte = 0; byte < 8; ++byte) digest = (digest ^ ((bits >> (8 * byte)) & 0xff)) * 1099511628211ULL;
    };
    std::size_t points = 0, paths = 0, cutting = 0, empty = 0;
    double least_margin = 1e300, longest_step = 0;
    bool safe = true, inside = true, bounded = true, once = true;
    for (int round = 0; round < 100; ++round) {
        const double x0 = mm(20000 + draw(5000));
        const double y0 = mm(20000 + draw(5000));
        const double w = mm(3000 + draw(22000));
        const bool thin = round % 12 == 5;  // a sliver: nowhere further from its sides than the interval
        const double h = thin ? mm(100 + draw(300)) : mm(3000 + draw(14000));
        double cut[4];
        for (double &corner : cut) corner = thin ? 0 : mm(draw(1400));
        json outer = json::array({json::array({x0 + cut[0], y0}), json::array({x0 + w - cut[1], y0}), json::array({x0 + w, y0 + cut[1]}),
                                  json::array({x0 + w, y0 + h - cut[2]}), json::array({x0 + w - cut[2], y0 + h}),
                                  json::array({x0 + cut[3], y0 + h}), json::array({x0, y0 + h - cut[3]}), json::array({x0, y0 + cut[0]})});
        outer.push_back(outer[0]);
        json shapes = json::array({outer});
        const std::uint64_t extra = thin ? 0 : draw(3);
        if (extra == 1 && w > 8 && h > 8) {  // an island
            const double ix = x0 + w / 2 + mm(draw(2000)) - 1.5;
            const double iy = y0 + h / 2 + mm(draw(2000)) - 1.5;
            const double size = mm(500 + draw(2000));
            shapes.push_back(json::array({json::array({ix, iy}), json::array({ix + size, iy + size / 4}),
                                          json::array({ix + size / 3, iy + size}), json::array({ix, iy})}));
        } else if (extra == 2) {  // a second shape, thin enough at times that nothing is carved in it
            const double side = mm(600 + draw(5000));
            shapes.push_back(json::array({json::array({x0 + w + 3, y0}), json::array({x0 + w + 3 + side, y0}),
                                          json::array({x0 + w + 3 + side / 2, y0 + 2 * side}), json::array({x0 + w + 3, y0})}));
        }
        const double resolution = mm(200 + 100 * draw(9));
        const double angle = static_cast<double>(30 + 15 * draw(7));
        const double depth = mm(500 + 500 * draw(8));
        const double radius = mm(500 + 500 * draw(8));
        const json result = operation_call("operation", json{{"op", "vcarve"}, {"paths", shapes}, {"resolution", resolution}, {"angle", angle},
                                                             {"depth", depth}, {"radius", radius}});
        const json &lines = result["paths"], &depths = result["depths"];
        if (lines.empty()) ++empty;
        const double slope = std::tan(angle / 2 * 3.141592653589793 / 180);
        std::vector<std::array<double, 4>> moves;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            ++paths;
            double px = 0, py = 0, pz = 0;
            for (std::size_t k = 0; k < lines[i].size(); ++k) {
                ++points;
                const double x = lines[i][k][0].get<double>(), y = lines[i][k][1].get<double>(), z = depths[i][k].get<double>();
                absorb(x), absorb(y), absorb(z);
                const double wall = to_walls(shapes, x, y), reach = -z * slope;
                if (!(z <= 0) || -z > depth || reach > radius * (1 + 1e-12)) bounded = false;
                if (z < 0) {
                    ++cutting;
                    if (!in_pocket(shapes, x, y)) inside = false;
                    // The interval less two steps of the kernel's grid, for the rounding of the point and of the walls.
                    if (reach > wall - resolution + 2e-6) safe = false;
                    least_margin = std::min(least_margin, wall - reach);
                } else if (wall > 2e-6 && !in_pocket(shapes, x, y)) {
                    inside = false;  // a point at no depth may sit on a wall, within the grid, but not beyond it
                }
                if (k) {
                    const double mx = (x + px) / 2, my = (y + py) / 2, mz = (z + pz) / 2;
                    if (mz < 0 && -mz * slope > to_walls(shapes, mx, my)) safe = false;
                    if (mz < 0) least_margin = std::min(least_margin, to_walls(shapes, mx, my) + mz * slope);
                    longest_step = std::max(longest_step, std::hypot(x - px, y - py) - resolution);
                    moves.push_back(x < px || (x == px && y < py) ? std::array<double, 4>{x, y, px, py} : std::array<double, 4>{px, py, x, y});
                }
                px = x, py = y, pz = z;
            }
        }
        std::sort(moves.begin(), moves.end());
        if (std::adjacent_find(moves.begin(), moves.end()) != moves.end()) once = false;
    }
    expect(safe, "the bit cuts outside an outline");
    expect(inside, "a point of a V-carve is outside its region");
    expect(bounded, "a V-carve goes deeper than the depth of cut or wider than the bit");
    expect(once, "a piece of a V-carve is cut twice");
    expect(longest_step < 1e-6, "a move of a V-carve is longer than the sampling interval");
    expect(points > 50000 && cutting > 20000 && empty > 0 && empty < 30, "the generated shapes are carved, and a few too thin to carve: " + std::to_string(empty));
    if (digest != VCARVE_DIGEST) std::printf("V-carving: digest %llu\n", static_cast<unsigned long long>(digest));
    expect(digest == VCARVE_DIGEST, "the V-carve paths are the recorded ones");
    std::printf("V-carving: %zu points in %zu paths of generated shapes, %zu of them cutting; the bit stays at least %.4f mm from every wall\n", points, paths,
                cutting, least_margin);
}

// FNV-1a of one isolation answer as JSON text, recorded from x86-64 GCC. See circuit_boards().
static const std::uint64_t ISOLATION_DIGEST = 14393510534754887855ULL;

// What a circuit board command answers, or the message it refuses with.
static json board(const char *command, const std::string &text, json request = json::object()) {
    request["text"] = text;
    try {
        return pcb_call(command, request);
    } catch (const ValueError &error) {
        return std::string(error.what());
    } catch (const std::exception &) {
        return "(another exception)";
    }
}

static void circuit_boards() {
    // Gerber: a flash, a line, a quarter arc and a macro with arithmetic, in millimetres with six decimals.
    const std::string head = "%FSLAX46Y46*%\n%MOMM*%\n";
    const json layer =
        board("gerber",
              head +
                  "%AMBOX*$3=$1x2*21,1,$3,$2,0,0,90*%\n%ADD10C,0.5*%\n%ADD11BOX,1.5X1*%\nD11*\nX2000000Y3000000D03*\n"
                  "D10*\nG01*\nX0Y0D02*\nX10000000Y0D01*\nG75*\nG03X0Y10000000I-10000000J0D01*\nM02*\n");
    expect(layer.is_object() && layer["counts"] == json::parse(R"({"flash":1,"line":1,"arc":1,"region":0})") &&
               layer["warnings"].empty(),
           "a Gerber layer is read");
    if (layer.is_object()) {
        const json &objects = layer["objects"];
        expect(objects[0] == json::parse(R"({"type":"flash","polarity":"dark","aperture":11,"at":[2.0,3.0]})"),
               "its flash");
        expect(objects[1]["end"] == json::parse("[10.0,0.0]") && objects[2]["centre"] == json::parse("[0.0,0.0]") &&
                   objects[2]["clockwise"] == false && objects[2]["points"].size() > 8,
               "its line and arc");
        bool on_arc = true;
        for (const json &point : objects[2]["points"])
            on_arc = on_arc && std::fabs(std::hypot(point[0].get<double>(), point[1].get<double>()) - 10) < 1e-5;
        expect(on_arc, "the flattened arc lies on the circle");
        // $3 = 1.5 x 2 wide and 1 high, turned a quarter turn: 1 wide and 3 high.
        expect(layer["apertures"][1]["parts"][0]["points"] ==
                   json::parse("[[0.5,-1.5],[0.5,1.5],[-0.5,1.5],[-0.5,-1.5],[0.5,-1.5]]"),
               "a macro with arithmetic and rotation");
        expect(layer["bounds"] == json::parse("[-0.25,-0.25,10.25,10.25]"),
               "the bounds include the apertures and the arc's reach");
    }
    expect(board("gerber", head + "%ADD10C,0.5*%\nD10*\nX0Y0D03*\n") ==
               "The Gerber file has no end-of-file command (M02); it may be truncated",
           "a truncated Gerber file is refused");
    expect(board("gerber", head + "D12*\nM02*\n") == "Line 3: aperture D12 is selected but not defined",
           "an unknown aperture is refused");
    expect(board("gerber", head + "%ADD10C,0.5*%\nD10*\n%SRX1000Y1000I1J1*%\nX0Y0D03*\nM02*\n") ==
               "Step-and-repeat would draw more than 100000 objects",
           "a huge step-and-repeat is refused before it is expanded");
    expect(
        board("gerber", head + "%AMD*1,1," + std::string(5000, '(') + "1" + std::string(5000, ')') +
                            ",0,0*%\n%ADD10D*%\nM02*\n") == "Line 4: aperture macro D nests brackets more than 32 deep",
        "deep macro nesting is refused");
    expect(board("gerber", head + "%ADD10C,0.5*%\nD10*\nX99999999999Y0D03*\nM02*\n") ==
               "Line 5: a coordinate lies outside -10000 to 10000 mm",
           "an absurd coordinate is refused");

    // A board outline: four lines out of order close into one path; with one missing it is open.
    const std::string edges = head +
                              "%ADD10C,0.1*%\nD10*\nG01*\nX0Y0D02*\nX8000000Y0D01*\nX0Y5000000D02*\nX0Y0D01*"
                              "\nX8000000Y5000000D02*\nX8000000Y0D01*\n";
    const json outline =
        board("gerber-outline", edges + "X8000000Y5000000D02*\nX0Y5000000D01*\nM02*\n", json{{"offset", {1, 2}}});
    expect(outline.is_object() &&
               outline["paths"] == json::parse("[[[1.0,2.0],[9.0,2.0],[9.0,7.0],[1.0,7.0],[1.0,2.0]]]") &&
               outline["bounds"] == json::parse("[1.0,2.0,9.0,7.0]"),
           "a board outline is joined into a closed path");
    const json open = board("gerber-outline", edges + "M02*\n");
    expect(open.is_string() && open.get<std::string>().find("The board outline is open at X") == 0,
           "an open outline is refused");

    // Excellon: inches with leading zeros omitted and a repeat; then a file that leaves its format unstated.
    const json drill = board("excellon", "M48\nINCH,TZ\nT1C0.04\n%\nT1\nX10000Y5000\nR2X2500\nM30\n");
    expect(drill.is_object() &&
               drill["tools"] ==
                   json::parse(R"([{"number":1,"diameter":1.016,"hits":[[25.4,12.7],[31.75,12.7],[38.1,12.7]]}])") &&
               drill["interpretation"]["omitted_zeros"] == "leading" && drill["warnings"].size() == 1,
           "a drill file in inches with a repeat");
    const std::string bare = "M48\nM72\nT1C0.04\n%\nT1\nX5200Y3400\nM30\n";
    const json guessed = board("excellon", bare),
               stated = board("excellon", bare, json{{"omitted_zeros", "leading"}, {"integer", 2}, {"decimal", 4}});
    expect(guessed.is_object() && guessed["warnings"].size() == 2 &&
               guessed["interpretation"]["zeros_from"] == "default" &&
               guessed["tools"][0]["hits"] == json::parse("[[1320.8,863.6]]"),
           "an unstated number format is read by the defaults and reported");
    expect(stated.is_object() && stated["warnings"].empty() &&
               stated["tools"][0]["hits"] == json::parse("[[13.208,8.636]]"),
           "the request can state the number format");
    expect(board("excellon", "M48\nT1C0.8\n%\nT1\nX1.0Y1.0\nM30\n") ==
               "The drill file does not state its units (METRIC, INCH, M71 or M72); give \"units\" in the request",
           "units are not assumed");
    const json drills = board(
        "excellon-drills", "M48\nMETRIC\nT1C0.8\nT2C0.4\nT3C0.8\n%\nT1\nX1.0Y1.0\nT2\nX2.0Y1.0\nT3\nX3.0Y1.0\nM30\n");
    expect(drills.is_object() && drills["operations"] == json::parse(R"([
               {"operation":"drill","tool_shape":"drill","tool_diameter":0.4,"paths":[[[2.0,1.0],[2.0,1.0]]]},
               {"operation":"drill","tool_shape":"drill","tool_diameter":0.8,"paths":[[[1.0,1.0],[1.0,1.0]],[[3.0,1.0],[3.0,1.0]]]}])"),
           "holes become drilling operations grouped by diameter");

    // Isolation: three round pads with gaps of 0.15 and 0.85 mm, and a tool that cuts 0.2 mm wide. The first
    // two stay joined, which is reported with the place and the gap; the contours are closed and lie half a
    // tool width outside the copper.
    const std::string pads = head + "%ADD10C,1.0*%\nD10*\nX0Y0D03*\nX1150000Y0D03*\nX3000000Y0D03*\nM02*\n";
    const json isolation = board("gerber-isolation", pads, json{{"tool_width", 0.2}});
    expect(isolation.is_object() && isolation["islands"] == 3 && isolation["path_count"] == 2 &&
               isolation["unreachable_count"] == 1 && isolation["unreachable"][0]["gap"] == 0.15 &&
               isolation["unreachable"][0]["islands"] == json::parse("[0,1]") && isolation["smallest_gap"] == 0.15 &&
               isolation["separating_tool_width"] == 0.147 && isolation["bounds"] == json::parse("[-0.6,-0.6,3.6,0.6]") &&
               isolation["warnings"].size() == 1,
           "isolation contours, and the place the tool cannot reach");
    if (isolation.is_object()) {
        bool closed = true, apart = true;
        for (const json &path : isolation["operations"][0]["paths"]) {
            closed = closed && path[0] == path[path.size() - 1];
            for (const json &point : path) {
                // No nearer than half the tool width to any pad, and no further than the round-corner allowance.
                double nearest = 1e9;
                for (const double centre : {0.0, 1.15, 3.0})
                    nearest = std::min(nearest, std::hypot(point[0].get<double>() - centre, point[1].get<double>()) - 0.5);
                apart = apart && nearest > 0.1 - 1e-9 && nearest < 0.1 + 0.005 + 0.001 + 1e-6;
            }
        }
        expect(closed && apart, "every contour point is half a tool width from the copper");
    }
    const json narrow = board("gerber-isolation", pads, json{{"tool_width", 0.147}, {"passes", 2}, {"mirror_x", 2}});
    expect(narrow.is_object() && narrow["unreachable_count"] == 0 && narrow["path_count"] == 5 && narrow["warnings"].empty() &&
               narrow["copper_bounds"] == json::parse("[0.5,-0.5,4.5,0.5]") &&
               narrow["cut"] == json::parse(R"([{"distance":0.1911,"pass":2,"paths":2},{"distance":0.0735,"pass":1,"paths":3}])"),
           "a tool narrow enough separates every pad; mirrored about X2, outer pass first");
    // The whole answer, to the last digit: the kernel works in whole nanometres, so every platform agrees.
    std::uint64_t digest = 1469598103934665603ULL;
    for (const unsigned char c : narrow.dump()) digest = (digest ^ c) * 1099511628211ULL;
    expect(digest == ISOLATION_DIGEST, "the isolation answer is the recorded one: digest " + std::to_string(digest));
    expect(board("gerber-isolation", pads) == "Give \"tool_width\": the width of cut at the engraving depth, in millimetres",
           "isolation needs the tool width");
}

int main() {
    // Ties round to even on the exact binary value, as Python's formatting does.
    expect(fixed(0.125, 2) == "0.12", "0.125 to two places");
    expect(fixed(0.375, 2) == "0.38", "0.375 to two places");
    expect(fixed(2.5, 0) == "2", "2.5 to no places");
    expect(fixed(-0.0004, 3) == "-0.000", "a small negative keeps its sign");
    expect(fixed(1234.56789, 3) == "1234.568", "1234.56789 to three places");
    expect(fixed(0.1 + 0.2, 9) == "0.300000000", "0.1 + 0.2 to nine places");
    double r = 0;
    expect(text::rounded(2.675, 2, r) && r == 2.67, "round(2.675, 2)");
    std::string untouched = "x";
    expect(!text::fixed(untouched, 1.0, 10) && !text::fixed(untouched, 1.0, -1) && untouched == "x" && !text::rounded(1.0, 4, r),
           "places outside the supported range are refused");
    expect(text::repr(680.0) == "680.0" && text::repr(0.1) == "0.1" && text::repr(1e16) == "1e+16" &&
               text::repr(0.00001) == "1e-05" && text::repr(-2.5) == "-2.5" && text::repr(123456789.125) == "123456789.125",
           "repr");

    const char *project = R"({"mode":"router","protocol":"grbl","width":100,"height":50,"feed":600,"power":12000,
        "depth":1,"step":0.5,"paths":[[[10,10],[20.5,10],[20.5,30]]]})";
    const Job job = generate(json::parse(project));
    std::string program;
    for (const std::string &line : job.lines) program += line + "\n";
    const std::string expected =
        "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\n"
        "G0 Z5.000\nG0 X10.000 Y10.000\nG1 Z-0.500 F100.0\nG1 X20.500 Y10.000 F600.0\nG1 X20.500 Y30.000 F600.0\nG0 Z5.000\n"
        "G0 Z5.000\nG0 X10.000 Y10.000\nG1 Z-1.000 F100.0\nG1 X20.500 Y10.000 F600.0\nG1 X20.500 Y30.000 F600.0\nG0 Z5.000\n"
        "M5\n";
    expect(program == expected, "a two-level router program");
    expect(job.cut_length == 61.0 && job.segments.size() == 4 && job.warnings.size() == 3, "its summary");
    expect(job.bounds[0] == 10 && job.bounds[1] == 10 && job.bounds[2] == 20.5 && job.bounds[3] == 30, "its bounds");

    expect(message<ValueError>(R"({"mode":"plasma"})") == "Choose router, laser or knife", "an unknown head");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[0,0],[101,0]]]})") == "X must be between 0 and 100.0", "a point outside the work area");
    expect(message<NotPorted>("{\"mode\":\"router\",\"width\":100,\"height\":50,\"feed\":\"\xef\xbc\x96\xef\xbc\x90\xef\xbc\x90\",\"power\":1,"
                              "\"paths\":[[[10,10],[30,10]]]}") == "numbers written in non-ASCII text",
           "a case that is not reproduced is declined, not guessed");

    // Drilling: a peck at each marker, with the last peck at the full depth.
    const Job drilled = generate(json::parse(R"({"mode":"router","operation":"drill","protocol":"grbl","width":100,
        "height":50,"feed":600,"power":12000,"depth":1,"drill_peck":0.4,"paths":[[[10,10],[10,10]],[[30.5,20],[30.5,20]]]})"));
    program.clear();
    for (const std::string &line : drilled.lines) program += line + "\n";
    const std::string pecks = "G1 Z-0.400 F100.0\nG0 Z5.000\nG1 Z-0.800 F100.0\nG0 Z5.000\nG1 Z-1.000 F100.0\nG0 Z5.000\n";
    expect(program == "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\nG0 X10.000 Y10.000\n" + pecks +
                          "G0 X30.500 Y20.000\n" + pecks + "M5\n",
           "a peck-drilling program");
    expect(drilled.cut_length == 34.4 && drilled.cut_minutes == 0.34 && drilled.segments.size() == 6 &&
               drilled.segments[0].end_z == -0.4 && drilled.segments[0].start_z == 5 && drilled.warnings.size() == 2 &&
               drilled.bounds[0] == 10 && drilled.bounds[1] == 10 && drilled.bounds[2] == 30.5 && drilled.bounds[3] == 20,
           "its summary");
    expect(message<ValueError>(R"({"mode":"router","operation":"drill","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[10,10],[11,10]]]})") == "Drill requires point markers or closed circular hole outlines",
           "a line is not a hole");

    // A drag knife: the blade trails its pivot, so the path starts with a lead-in and swivels at the corner.
    const Job sliced = generate(json::parse(R"({"mode":"knife","protocol":"grbl","width":100,"height":50,"feed":600,
        "power":1,"depth":0.5,"paths":[[[10,10],[20,10],[20,20]]]})"));
    const std::vector<std::string> &cut = sliced.lines;
    expect(cut.size() == 33 && cut[6] == "G0 Z5.000" && cut[7] == "G0 Z5.000" && cut[8] == "G0 X9.250 Y10.000" &&
               cut[9] == "G1 Z-0.500 F100.0" && cut[10] == "G1 X10.250 Y10.000 F600.0" &&
               cut[11] == "G1 X20.250 Y10.000 F600.0" && cut[12] == "G1 X20.249 Y10.022 F600.0" &&
               cut[29] == "G1 X20.000 Y10.250 F600.0" && cut[30] == "G1 X20.000 Y20.250 F600.0" && cut[32] == "M5",
           "a drag-knife program");
    expect(sliced.cut_length == 21.39 && sliced.warnings.size() == 4 && sliced.bounds[0] == 9.25 &&
               sliced.bounds[3] == 20.25,
           "its summary");
    expect(message<ValueError>(R"({"mode":"knife","width":100,"height":50,"feed":600,"power":1,"knife_offset":2,
        "paths":[[[10,10],[20,10]]]})") == "knife_lead must be between 2.0 and 20", "a lead shorter than the offset");

    // Repeated points on a knife path. A point is kept only when it is further than 0.1 nm from the last
    // point kept, so no move has zero length. Two steps of 0.09 nm out and one step back are 0.18 nm each
    // way: the knife turns half round at the far end and half round again where the path closes. (The steps
    // out used to be dropped and the step back kept, a move of no length, on which the reference failed.)
    const auto knife_lines = [](const char *paths) {
        return generate(json::parse(std::string(R"({"mode":"knife","width":100,"height":50,"feed":600,"power":1,
            "paths":)") + paths + "}")).lines;
    };
    const std::vector<std::string> turned = knife_lines("[[[9,9],[9.00000009,9],[9.00000018,9],[9,9]]]");
    expect(turned.size() == 166 && turned[8] == "G0 X8.250 Y9.000" && turned[10] == "G1 X9.250 Y9.000 F600.0" &&
               turned[11] == "G1 X9.250 Y9.000 F600.0" && turned[29] == "G1 X9.000 Y8.750 F600.0" &&
               turned[47] == "G1 X8.750 Y9.000 F600.0" && turned[48] == "G1 X8.750 Y9.000 F600.0" &&
               turned[66] == "G1 X9.000 Y9.250 F600.0" && turned[84] == "G1 X9.250 Y9.000 F600.0" &&
               turned[85] == "G0 Z5.000",
           "a knife path that goes out and back by more than the tolerance");
    expect(turned == knife_lines("[[[9,9],[9.00000018,9],[9,9]]]"), "the step under the tolerance is not a point of it");
    // Within the tolerance of the point kept there is no move at all, and exact repeats change nothing.
    const std::vector<std::string> plain = knife_lines("[[[20,20],[30,25]]]");
    expect(plain == knife_lines("[[[20,20],[20.00000006,20],[20.00000006,20.00000006],[20,20],[30,25]]]"),
           "an excursion within the tolerance is dropped");
    expect(plain == knife_lines("[[[20,20],[20,20],[30,25],[30,25]]]"), "exact repeats are dropped");
    // Forty steps of 0.05 nm are 2 nm of travel: they are followed, where each used to be dropped.
    std::string creeping = "[[[30,30]";
    for (int i = 1; i <= 40; ++i) creeping += ",[" + text::repr(30 + 5e-8 * i) + ",30]";
    expect(knife_lines((creeping + "]]").c_str()).size() > 10, "many short steps add up to a move");
    expect(message<ValueError>(R"({"mode":"knife","width":100,"height":50,"feed":600,"power":1,
        "paths":[[[10,10],[20,10]],[[5,5],[5,5],[5.00000005,5]]]})") == "Knife paths must contain nonzero travel",
           "a knife path that is a single point is refused");

    // A ramp entry: one lap down a closed contour at no more than the entry angle, then a lap at depth.
    const Job ramp = generate(json::parse(R"({"mode":"router","width":100,"height":50,"feed":600,"power":12000,
        "depth":1,"step":1,"router_entry":"ramp","entry_angle":15,"paths":[[[10,10],[30,10],[30,20],[10,10]]]})"));
    program.clear();
    for (const std::string &line : ramp.lines) program += line + "\n";
    expect(program ==
               "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\nG0 Z5.000\nG0 X10.000 Y10.000\n"
               "G1 X10.000 Y10.000 Z0.000 F100.0\nG1 X30.000 Y10.000 Z-0.382 F100.0\nG1 X30.000 Y20.000 Z-0.573 F100.0\n"
               "G1 X10.000 Y10.000 Z-1.000 F100.0\nG1 X30.000 Y10.000 Z-1.000 F600.0\nG1 X30.000 Y20.000 Z-1.000 F600.0\n"
               "G1 X10.000 Y10.000 Z-1.000 F600.0\nG0 Z5.000\nM5\n",
           "a ramp entry program");
    expect(ramp.cut_length == 109.73 && ramp.cut_minutes == 0.66 && ramp.warnings.size() == 2, "its summary");
    expect(to_json(ramp)["segments"][0].dump() == "[10.0,10.0,10.0,10.0,0,null,5.0]",
           "the first height of a ramp is the reference's integer 0");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,"router_entry":"ramp",
        "paths":[[[10,10],[30,30],[30,10],[10,30],[10,10]]]})") ==
               "Repair crossing or degenerate machining contours before using a ramp",
           "a ramp along a contour that crosses itself");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,"router_entry":"helix",
        "paths":[[[10,10],[30,10],[30,20],[10,10]]]})") == "Helical entry requires a pocket with confirmed cutter clearance",
           "a helix on a contour");

    // Holding tabs: the contour rises to the tab height over each tab, with vertical moves at its shoulders.
    const Job tabbed = generate(json::parse(R"({"mode":"router","width":100,"height":50,"feed":600,"power":12000,
        "depth":2,"step":2,"stock_thickness":2,"tabs":{"width":2,"height":0.5,"count":1},
        "paths":[[[10,10],[30,10],[30,20],[10,20],[10,10]]]})"));
    program.clear();
    for (const std::string &line : tabbed.lines) program += line + "\n";
    expect(program ==
               "G21\nG90\nG17\nG94\nG54\nM5\nG0 Z5.000\nM3 S12000\nG4 P2\nG0 Z5.000\nG0 X10.000 Y10.000\nG1 Z-2.000 F100.0\n"
               "G1 X30.000 Y10.000 F100.0 Z-2.000\nG1 X30.000 Y17.413 F100.0 Z-2.000\nG1 X30.000 Y17.413 F100.0 Z-1.500\n"
               "G1 X30.000 Y20.000 F100.0 Z-1.500\nG1 X27.413 Y20.000 F100.0 Z-1.500\nG1 X27.413 Y20.000 F100.0 Z-2.000\n"
               "G1 X10.000 Y20.000 F100.0 Z-2.000\nG1 X10.000 Y10.000 F100.0 Z-2.000\nG0 Z5.000\nM5\n",
           "a program with one holding tab");
    expect(tabbed.cut_length == 61.0 && tabbed.cut_minutes == 0.61 && tabbed.warnings.size() == 6 && tabbed.segments.size() == 8,
           "its summary");
    expect(message<ValueError>(R"({"mode":"router","width":100,"height":50,"feed":600,"power":1,"depth":2,"stock_thickness":2,
        "tabs":{"width":40,"height":0.5,"count":2},"paths":[[[10,10],[30,10],[30,20],[10,20],[10,10]]]})") ==
               "Tabs overlap or cover the contour; reduce count/width or change positions",
           "tabs that cover the contour");

    // Probe levelling: a plan over the job, a tilted plane as the probed surface, and the job following it.
    const json plan = probe_plan(json{{"bounds", job.bounds}, {"width", 100}, {"height", 50}, {"zero", true}});
    expect(plan["grid"] == json::parse(R"({"x0":8.0,"y0":8.0,"dx":7.25,"dy":8.0,"columns":3,"rows":4})"), "the probe grid");
    expect(plan["lines"].size() == 4 + 3 * 12 + 1 && plan["lines"][2] == "M5" && plan["lines"][4] == "G0 X8.000 Y8.000" &&
               plan["lines"][5] == "G38.2 Z-1.000 F25.0" && plan["lines"][6] == "G10 L20 P1 Z0.000" &&
               plan["lines"][8] == "G0 X15.250 Y8.000" && plan["order"][3] == json::parse("[2,1]"),
           "the probing program");
    json reports = json::array();
    for (const json &point : plan["points"]) {
        std::string z;
        text::fixed(z, -37.5 + 0.01 * point[0].get<double>(), 3);
        reports.push_back("[PRB:0.000,0.000," + z + ":1]");
    }
    const HeightMap surface = height_map_from_probes(json{{"grid", plan["grid"]}, {"order", plan["order"]}, {"reports", reports}});
    expect(surface.offsets[0] == 0 && std::fabs(surface.offsets[5] - 0.145) < 1e-12 && std::fabs(surface.highest() - 0.145) < 1e-12,
           "the height map is relative to the first point");
    const std::vector<std::string> followed = level_program(job.lines, surface, 5);
    const std::vector<std::string> first_pass(followed.begin() + 9, followed.begin() + 20);
    const std::vector<std::string> wanted = {
        "G0 Z5.000", "G0 X10.000 Y10.000", "G1 Z-0.480 F100.0",
        "G1 X13.500 Y10.000 F600.0 Z-0.445", "G1 X17.000 Y10.000 F600.0 Z-0.410", "G1 X20.500 Y10.000 F600.0 Z-0.375",
        "G1 X20.500 Y15.000 F600.0 Z-0.375", "G1 X20.500 Y20.000 F600.0 Z-0.375", "G1 X20.500 Y25.000 F600.0 Z-0.375",
        "G1 X20.500 Y30.000 F600.0 Z-0.375", "G0 Z5.000"};
    expect(first_pass == wanted && followed.size() == job.lines.size() + 10, "the first pass follows the surface");
    try {
        level_program({"G0 Z5.000", "G0 X10.000 Y10.000", "G2 X20.000 Y10.000 I5 J0 F600"}, surface, 5);
        expect(false, "an arc is refused");
    } catch (const ValueError &error) {
        expect(std::string(error.what()).find("only straight moves") != std::string::npos, "an arc is refused");
    }

    self_crossing();
    helical_entries();
    v_carving();
    circuit_boards();

    std::printf(failures ? "%d checks failed\n" : "PASS native core self-test%.0d\n", failures);
    return failures ? 1 : 0;
}
