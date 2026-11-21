// Timings of the solid kernel on the models a Studio user builds: kerfkite_solid_bench [REPEAT] [FILTER]
//
// Each model is built REPEAT times (5 by default) and the best time is printed, with the size of the
// result and a hash of every vertex and triangle of it. The hash is the point of comparing two builds:
// an optimisation of the kernel must leave every one of them as it was. FILTER keeps the models whose
// name contains it.
//
// When the kernel is compiled with KERFKITE_SOLID_PROFILE (cmake -DKERFKITE_SOLID_PROFILE=ON) the time
// of each phase of the best run is printed under each model. validation/benchmark_solid.py times the
// same models through Studio, on the kernel and on the third-party library.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../core/src/solid/timing.hpp"
#include "kerfkite/solid.hpp"

using namespace kerfkite::solid;

namespace {

double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

uint64_t hash_of(const Solid &s) {
    uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](uint64_t v) {
        for (int k = 0; k < 8; ++k) {
            h ^= (v >> (8 * k)) & 0xff;
            h *= 1099511628211ULL;
        }
    };
    for (const GridPoint &p : s.grid_vertices())
        for (const int64_t c : p) mix(static_cast<uint64_t>(c));
    for (const Triangle &t : s.triangles())
        for (const uint32_t c : t) mix(c);
    return h;
}

Solid at(const Solid &s, double x, double y, double z) { return s.translated({x, y, z}); }

// The models. Segment counts are Studio's: 96 for a feature of a part, 48 for a script without $fn.

Solid plate_one_bore() { return box({40, 30, 6}) - at(cylinder(8, 4, 4, 96), 20, 15, -1); }

// The part pinned in validation/check_installed_cam.py, feature by feature, without its scripted post.
Solid bracket_base() {
    Solid part = box({41, 30, 6.25});
    std::vector<Solid> bores;
    const Solid bore = at(cylinder(8.25, 2.4, 1.9, 96), 9.3, 15, -1);
    bores.push_back(bore);
    for (int k = 1; k < 5; ++k) bores.push_back(at(at(bore, -20.5, -15, 0).rotated(0, 0, 50.0 * k), 20.5, 15, 0));
    part = part - batch_boolean(bores, Op::Union);
    part = part + at(sphere(7.1, 96), 20.5, 15, 6.25);
    part = part + at(extrude({{{0, 0}, {11.7, 0}, {3.3, 8.9}}}, 4.2).rotated(90, 0, 33), 4.4, 3.1, 5.75);
    part = part - at(revolve({{{9, 0}, {11.5, 0}, {11.5, 2.2}, {9, 3.1}}}, 96, 200).rotated(0, 0, 77), 20.5, 15, 4.75);
    part = part ^ at(box({60, 26.5, 30}), -5, 1.75, -2);
    return part;
}

Solid bracket() {
    // The post of the pinned part: a hull, less a twisted extrusion and a cross bore, at 20 segments.
    const Solid body = convex_hull(std::vector<Solid>{cylinder(5.5, 6, 4.2, 20), at(box({4, 5, 3}), 7.3, 2.1, 0)});
    ExtrudeOptions twisted;
    twisted.slices = 5;
    twisted.twist_degrees = 40;
    twisted.scale_x = twisted.scale_y = 0.8;
    const Solid slot = at(extrude({{{-2.15, -1.7}, {2.15, -1.7}, {2.15, 1.7}, {-2.15, 1.7}}}, 7.5, twisted), 0, 0, -1);
    const Solid cross = at(cylinder(30, 1.2, 1.2, 20, true), -2.5, 0, 0).rotated(0, 90, 17);
    const Solid post = at(batch_boolean({body, slot, cross}, Op::Difference), 31.3, 9.2, 5.25);
    return bracket_base() + post;
}

// 64 bores in a row, each overlapping the next: one pattern of a part.
Solid bores_overlapping() {
    std::vector<Solid> bores;
    const Solid bore = at(cylinder(8, 2, 2, 96), 4, 15, -1);
    for (int k = 0; k < 64; ++k) bores.push_back(at(bore, 1.5 * k, 0, 0));
    return box({104, 30, 6}) - batch_boolean(bores, Op::Union);
}

// 64 separate bores in a plate, as one pattern: the common real case.
Solid bores_disjoint_pattern() {
    std::vector<Solid> bores;
    const Solid bore = at(cylinder(8, 2, 2, 96), 5, 5, -1);
    for (int k = 0; k < 64; ++k) bores.push_back(at(bore, 10.0 * (k % 8), 10.0 * (k / 8), 0));
    return box({80, 80, 6}) - batch_boolean(bores, Op::Union);
}

// The same plate cut by one feature after another, as a user adds holes.
Solid bores_disjoint_chain() {
    Solid plate = box({80, 80, 6});
    const Solid bore = at(cylinder(8, 2, 2, 96), 5, 5, -1);
    for (int k = 0; k < 64; ++k) plate = plate - at(bore, 10.0 * (k % 8), 10.0 * (k / 8), 0);
    return plate;
}

Solid two_spheres(int segments) { return sphere(10, segments) + at(sphere(10, segments), 7.3, 2.1, 1.7); }

// 50 boxes, each overlapping its neighbours, turned by different angles.
Solid boxes_union() {
    std::vector<Solid> boxes;
    for (int k = 0; k < 50; ++k) boxes.push_back(at(box({6, 5, 4}, true).rotated(7.0 * k, 0, 13.0 * k), 3.5 * (k % 10), 4.0 * (k / 10), 0.3 * (k % 3)));
    return batch_boolean(boxes, Op::Union);
}

// One solid less many: slots, tapered bores and a sphere, one after another.
Solid difference_chain() {
    Solid block = box({60, 40, 12});
    for (int k = 0; k < 8; ++k) block = block - at(box({3, 30, 6}), 4.0 + 7 * k, 5, 8);
    for (int k = 0; k < 8; ++k) block = block - at(cylinder(14, 1.6, 1.1, 48), 5.5 + 7 * k, 20, -1);
    for (int k = 0; k < 4; ++k) block = block - at(cylinder(70, 1.5, 1.5, 48).rotated(0, 90, 0), -5, 8.0 + 8 * k, 4);
    return block - at(sphere(9, 48), 30, 20, 14);
}

Solid revolve_and_cut() {
    const Solid turned = revolve({{{4, 0}, {12, 0}, {12, 3}, {8, 5}, {8, 14}, {4, 14}}}, 96);
    return turned - at(box({30, 4, 6}), -15, -2, 9) - at(cylinder(40, 1.5, 1.5, 48).rotated(90, 0, 0), 0, 20, 2.5);
}

Solid extrude_and_cut() {
    ExtrudeOptions options;
    options.slices = 12;
    options.twist_degrees = 60;
    const Solid star = extrude({{{10, 0}, {3, 2}, {0, 10}, {-3, 2}, {-10, 0}, {-3, -2}, {0, -10}, {3, -2}}}, 20, options);
    return (star - at(cylinder(24, 2, 2, 48), 0, 0, -2)) ^ at(sphere(11, 48), 0, 0, 10);
}

struct Model {
    const char *name;
    std::function<Solid()> build;
};

}  // namespace

int main(int argc, char **argv) {
    const int repeat = argc > 1 ? std::atoi(argv[1]) : 5;
    const char *filter = argc > 2 ? argv[2] : "";
    const std::vector<Model> models = {
        {"plate with one bore", plate_one_bore},
        {"bracket without its post", bracket_base},
        {"bracket (the pinned part)", bracket},
        {"64 overlapping bores", bores_overlapping},
        {"64 separate bores, one pattern", bores_disjoint_pattern},
        {"64 separate bores, one by one", bores_disjoint_chain},
        {"two spheres, 48 segments", [] { return two_spheres(48); }},
        {"two spheres, 96 segments", [] { return two_spheres(96); }},
        {"union of 50 boxes", boxes_union},
        {"difference chain", difference_chain},
        {"revolve and cut", revolve_and_cut},
        {"extrude and cut", extrude_and_cut},
    };
    uint64_t all = 1469598103934665603ULL;
    for (const Model &model : models) {
        if (!std::strstr(model.name, filter)) continue;
        double best = 1e300;
        uint64_t hash = 0;
        size_t triangles = 0;
        std::vector<double> phases;
        for (int k = 0; k < (repeat > 0 ? repeat : 1); ++k) {
            kerfkite::solid::detail::timing_reset();
            const double start = seconds();
            const Solid result = model.build();
            const double took = seconds() - start;
            if (took < best) {
                best = took;
                phases = kerfkite::solid::detail::timing_totals();
            }
            hash = hash_of(result);
            triangles = result.num_triangles();
        }
        all = (all ^ hash) * 1099511628211ULL;
        std::printf("%-32s %9.2f ms  %7zu triangles  %016llx\n", model.name, best * 1e3, triangles, static_cast<unsigned long long>(hash));
        double accounted = 0;
        for (size_t p = 0; p < phases.size(); ++p) {
            accounted += phases[p];
            if (phases[p] >= 0.005 * best) std::printf("    %-28s %9.2f ms  %4.1f%%\n", kerfkite::solid::detail::timing_name(p), phases[p] * 1e3, 100 * phases[p] / best);
        }
        if (!phases.empty()) std::printf("    %-28s %9.2f ms  %4.1f%%\n", "(not timed)", (best - accounted) * 1e3, 100 * (best - accounted) / best);
    }
    std::printf("all models %016llx\n", static_cast<unsigned long long>(all));
    return 0;
}
