// Two builds of the solid kernel compared as solids, not as meshes: kerfkite_solid_shapes
//
//   record CASES SEED FILE    build generated models and write every Boolean operation they make: the
//                             operands as meshes, and the result or the refusal
//   replay FILE OUT           do each recorded operation again, on the recorded operands, and write the results
//   compare FILE_A FILE_B     compare the results of the two files, record by record, with this build as judge
//   models FILE               the models of kerfkite_solid_bench as records (each a result without operands)
//
// The use is to build this file against two versions of the kernel. One records, the other replays the
// same operations on the same operands, and each in turn compares the two sets of results. A change to
// the kernel that leaves every solid the same shape, however it divides the shape into triangles, shows
// here as "the same set of points" in every record.
//
// What "the same" means is stated for each count that compare prints:
//   - the same mesh: equal vertex and triangle arrays;
//   - the same set of points: same_shape(), the exact arrangement of the two meshes, with nothing rounded
//     and nothing left out, in which no region of space is inside one solid alone. The exact volumes
//     (integers, six times the volume in cubic grid units) and the bounds are then compared as well;
//   - within the grid: not the same set of points, but each difference of the two is empty to the kernel,
//     whose Boolean operations leave out what is thinner than the grid resolves. The difference of the
//     volumes over the surface area, a thickness, and the largest distance from a vertex of one to the
//     surface of the other are printed, in nanometres.
// docs/SOLID-KERNEL.md says where the second and where the third is to be expected.
//
// The files are raw integers in the byte order of the machine: they are meant to be read back on the
// machine that wrote them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../core/src/solid/mesh.hpp"
#include "kerfkite/solid.hpp"

namespace kerfkite {
namespace solid {
// In the kernel since the comparison was written; declared here so that this file also builds against a
// version before it, with the function supplied beside it.
bool same_shape(const Solid &a, const Solid &b);
}  // namespace solid
}  // namespace kerfkite

using namespace kerfkite;
using namespace kerfkite::solid;

namespace {

// ---- records --------------------------------------------------------------------------------------

struct Shape {
    std::vector<GridPoint> v;
    std::vector<Triangle> t;
    bool operator==(const Shape &o) const { return v == o.v && t == o.t; }
};

struct Record {
    int kind = 0;       // which generator made it
    int op = 0;         // 0 union, 1 difference, 2 intersection; 8 more for a batch; 99 for a model
    int status = 0;     // 0 a result, 1 refused as too fine, 2 refused otherwise
    std::vector<Shape> operands;
    Shape result;
    std::string message;
};

Shape shape_of(const Solid &s) { return Shape{s.grid_vertices(), s.triangles()}; }

Solid solid_of(const Shape &shape) {
    if (shape.t.empty()) return Solid();
    return from_grid_mesh(shape.v, shape.t);
}

void put(FILE *f, uint64_t x) {
    if (std::fwrite(&x, sizeof x, 1, f) != 1) {
        std::perror("write");
        std::exit(2);
    }
}

bool get(FILE *f, uint64_t &x) { return std::fread(&x, sizeof x, 1, f) == 1; }

void put(FILE *f, const Shape &s) {
    put(f, s.v.size());
    put(f, s.t.size());
    for (const GridPoint &p : s.v)
        for (const int64_t c : p) put(f, static_cast<uint64_t>(c));
    for (const Triangle &t : s.t)
        for (const uint32_t c : t) put(f, c);
}

bool get(FILE *f, Shape &s) {
    uint64_t nv = 0, nt = 0;
    if (!get(f, nv) || !get(f, nt) || nv > 100000000 || nt > 100000000) return false;
    s.v.resize(static_cast<size_t>(nv));
    s.t.resize(static_cast<size_t>(nt));
    for (GridPoint &p : s.v)
        for (int64_t &c : p) {
            uint64_t x = 0;
            if (!get(f, x)) return false;
            c = static_cast<int64_t>(x);
        }
    for (Triangle &t : s.t)
        for (uint32_t &c : t) {
            uint64_t x = 0;
            if (!get(f, x)) return false;
            c = static_cast<uint32_t>(x);
        }
    return true;
}

void put(FILE *f, const Record &r) {
    put(f, 0x4b4b5348u);
    put(f, static_cast<uint64_t>(r.kind));
    put(f, static_cast<uint64_t>(r.op));
    put(f, static_cast<uint64_t>(r.status));
    put(f, r.operands.size());
    for (const Shape &s : r.operands) put(f, s);
    put(f, r.result);
    put(f, r.message.size());
    for (const char c : r.message) put(f, static_cast<unsigned char>(c));
}

bool get(FILE *f, Record &r) {
    uint64_t magic = 0, kind = 0, op = 0, status = 0, count = 0, length = 0;
    if (!get(f, magic)) return false;
    if (magic != 0x4b4b5348u || !get(f, kind) || !get(f, op) || !get(f, status) || !get(f, count) || count > 100000) {
        std::fprintf(stderr, "not a file of records\n");
        std::exit(2);
    }
    r.kind = static_cast<int>(kind);
    r.op = static_cast<int>(op);
    r.status = static_cast<int>(status);
    r.operands.assign(static_cast<size_t>(count), Shape());
    for (Shape &s : r.operands)
        if (!get(f, s)) return false;
    if (!get(f, r.result) || !get(f, length) || length > 100000) return false;
    r.message.clear();
    for (uint64_t k = 0; k < length; ++k) {
        uint64_t c = 0;
        if (!get(f, c)) return false;
        r.message.push_back(static_cast<char>(c));
    }
    return true;
}

FILE *open_file(const char *path, const char *mode) {
    FILE *f = std::fopen(path, mode);
    if (!f) {
        std::perror(path);
        std::exit(2);
    }
    return f;
}

// ---- one operation, recorded --------------------------------------------------------------------------

FILE *g_out = nullptr;
int g_kind = 0;
size_t g_records = 0, g_refused = 0;

struct Refused {};

Op op_of(int code) { return (code & 7) == 0 ? Op::Union : (code & 7) == 1 ? Op::Difference : Op::Intersection; }
int code_of(Op op) { return op == Op::Union ? 0 : op == Op::Difference ? 1 : 2; }

// Runs the operation of a record on its operands and fills in the outcome; returns the solid.
Solid run(Record &r, const std::vector<Solid> &operands) {
    r.status = 0;
    r.message.clear();
    r.result = Shape();
    try {
        const Solid result = (r.op & 8) ? batch_boolean(operands, op_of(r.op)) : boolean(operands[0], operands[1], op_of(r.op));
        r.result = shape_of(result);
        return result;
    } catch (const ValueError &error) {
        r.status = std::strstr(error.what(), "too fine") ? 1 : 2;
        r.message = error.what();
    }
    return Solid();
}

Solid recorded(const std::vector<Solid> &operands, int code) {
    Record r;
    r.kind = g_kind;
    r.op = code;
    for (const Solid &s : operands) r.operands.push_back(shape_of(s));
    const Solid result = run(r, operands);
    if (g_out) put(g_out, r);
    ++g_records;
    if (r.status) {
        ++g_refused;
        throw Refused();
    }
    return result;
}

Solid apply(const Solid &a, const Solid &b, Op op) { return recorded({a, b}, code_of(op)); }
Solid batch(const std::vector<Solid> &list, Op op) { return recorded(list, code_of(op) | 8); }

// ---- generated models -----------------------------------------------------------------------------
//
// The order in which a compiler evaluates the arguments of a call is its own choice, so every random
// draw is a statement of its own (or an element of a braced list, which is ordered).

uint64_t state = 88172645463325252ULL;
uint64_t rnd() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
double uni(double a, double b) { return a + (b - a) * static_cast<double>(rnd() % 1000001) / 1e6; }
int pick(int n) { return static_cast<int>(rnd() % static_cast<uint64_t>(n)); }

Solid at(const Solid &s, double x, double y, double z) { return s.translated({x, y, z}); }

Solid turned(const Solid &s, double range) {
    const double x = uni(-range, range), y = uni(-range, range), z = uni(-range, range);
    return s.rotated(x, y, z);
}

Solid base() {
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

Solid placed() {
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

// A tool for a part: a bore, a tapered bore, a slot, a cross bore, a dish or a tilted bore, at a place on
// or off a lattice of half millimetres.
Solid tool(double width, double depth, double thickness) {
    const int segments = 12 + 12 * pick(4);
    const double x = pick(2) ? 0.5 * pick(static_cast<int>(2 * width) + 1) : uni(0, width), y = pick(2) ? 0.5 * pick(static_cast<int>(2 * depth) + 1) : uni(0, depth);
    switch (pick(7)) {
        case 0: return at(cylinder(thickness + 2, 0.5 + 0.25 * pick(6), 0.5 + 0.25 * pick(6), segments), x, y, -1);  // through, perhaps tapered
        case 1: {
            const double radius = 0.5 + 0.25 * pick(6);
            return at(cylinder(thickness, radius, radius, segments), x, y, thickness * 0.5);  // blind
        }
        case 2: {
            const double sx = 0.5 + 0.5 * pick(6), sy = 0.5 + 0.5 * pick(8);
            return at(box({sx, sy, thickness}), x, y, thickness * 0.5);  // a slot
        }
        case 3: {
            const double radius = 0.25 + 0.25 * pick(4);
            return at(cylinder(width + 4, radius, radius, segments).rotated(0, 90, 0), -2, y, thickness * 0.5);  // a cross bore
        }
        case 4: return at(sphere(0.75 + 0.25 * pick(8), segments), x, y, thickness);  // a dish
        case 5: {
            const double tilt = uni(-35, 35), turn = uni(0, 360), radius = 0.4 + 0.2 * pick(5);
            return at(cylinder(3 * thickness + 4, radius, radius, segments, true).rotated(tilt, 0, turn), x, y, thickness * 0.5);  // a tilted bore
        }
        default: {
            const double size = 1 + 0.5 * pick(5), angle = uni(0, 90);
            return at(box({size, size, thickness + 2}, true).rotated(0, 0, angle), x, y, thickness * 0.5);  // a turned square hole
        }
    }
}

// One generated model. Every Boolean operation it makes is recorded; a refusal ends it.
void generate(int kind) {
    g_kind = kind;
    try {
        if (kind == 0 || kind == 1 || kind == 2) {
            Solid a, b;
            if (kind == 0) {  // two placed primitives, one perhaps already cut or joined
                a = placed();
                b = placed();
                if (pick(3) == 0) {
                    const Solid other = placed();
                    const Solid c = apply(a, other, pick(2) ? Op::Union : Op::Difference);
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
            apply(a, b, Op::Union);
            apply(a, b, Op::Intersection);
            apply(a, b, Op::Difference);
            apply(b, a, Op::Union);
            apply(b, a, Op::Intersection);
        } else if (kind == 3) {  // a chain of operations
            Solid acc = base();
            for (int k = 0; k < 5; ++k) {
                const Solid shape = base();
                const double x = pick(2) ? 90.0 * pick(4) : uni(-90, 90), z = pick(2) ? 45.0 * pick(8) : uni(-90, 90);
                const Solid t = shape.rotated(x, 0, z).translated({static_cast<double>(pick(5) - 2), static_cast<double>(pick(5) - 2), static_cast<double>(pick(5) - 2)});
                const int op = pick(3);
                Solid next = apply(acc, t, op == 0 ? Op::Union : op == 1 ? Op::Difference : Op::Intersection);
                if (next.empty()) next = t;
                acc = next;
            }
        } else if (kind == 4) {  // a batch, and the same operations one by one
            std::vector<Solid> parts;
            const int n = 2 + pick(8);
            Solid folded, rest;
            for (int k = 0; k < n; ++k) {
                const Solid shape = base();
                const double z = pick(2) ? 30.0 * pick(12) : uni(0, 360);
                const Solid t = shape.rotated(0, 0, z).translated({(pick(7) - 3) * 0.5, (pick(7) - 3) * 0.5, static_cast<double>(pick(3) - 1)});
                parts.push_back(t);
                folded = k ? apply(folded, t, Op::Union) : t;
                if (k) rest = k > 1 ? apply(rest, t, Op::Union) : t;
            }
            batch(parts, Op::Union);
            batch(parts, Op::Difference);
            batch(parts, Op::Intersection);
        } else if (kind == 5) {  // idempotence and absorption
            const Solid a = turned(base(), 180);
            const Solid b = base().translated({uni(-1, 1), uni(-1, 1), uni(-1, 1)});
            const Solid u = apply(a, b, Op::Union), i = apply(a, b, Op::Intersection);
            if (!i.empty()) apply(a, i, Op::Union);
            apply(a, u, Op::Intersection);
            apply(a, a, Op::Union);
            apply(a, a, Op::Intersection);
            apply(a, a, Op::Difference);
        } else if (kind == 6) {  // a plate and features cut from it or added to it one after another, as a part is built
            const double width = 8 + 2 * pick(5), depth = 6 + 2 * pick(4), thickness = 1 + 0.5 * pick(5);
            Solid part = box({width, depth, thickness});
            const int features = 3 + pick(5);
            for (int k = 0; k < features; ++k) {
                const Solid t = tool(width, depth, thickness);
                const int op = pick(8);
                const Solid next = apply(part, t, op == 0 ? Op::Union : op == 1 ? Op::Intersection : Op::Difference);
                if (!next.empty()) part = next;
            }
        } else if (kind == 7) {  // a plate less a pattern: bores in a row, apart or overlapping, as one batch
            const double thickness = 1 + 0.5 * pick(4);
            const int count = 2 + pick(9), segments = 12 + 12 * pick(4);
            const double radius = 0.5 + 0.25 * pick(4), top = pick(3) ? radius : 0.25 + 0.25 * pick(4), step = pick(2) ? 2 * radius + 0.5 : radius * (0.5 + 0.25 * pick(4));
            const double turn = pick(3) ? 0.0 : uni(0, 90);
            std::vector<Solid> list;
            list.push_back(box({count * step + 4, 6, thickness}));
            const Solid bore = cylinder(thickness + 2, radius, top, segments);
            for (int k = 0; k < count; ++k) list.push_back(at(turn ? bore.rotated(0, 0, turn * k) : bore, 2 + step * k, 3, -1));
            const Solid cut = batch(list, Op::Difference);
            std::vector<Solid> bores(list.begin() + 1, list.end());
            const Solid joined = batch(bores, Op::Union);
            if (!cut.empty() && !joined.empty()) apply(cut, at(joined, 0.25 * pick(4), 0.25 * pick(4), 0), pick(2) ? Op::Union : Op::Difference);
        } else if (kind == 8) {  // turned and extruded bodies with cuts
            const int segments = 12 + 12 * pick(3);
            Solid body;
            if (pick(2)) {
                const double inner = 1 + 0.5 * pick(4), outer = inner + 1 + 0.5 * pick(6), height = 2 + pick(6), angle = pick(2) ? 360.0 : 30.0 + 30 * pick(10);
                body = revolve({{{inner, 0}, {outer, 0}, {outer, height * 0.4}, {inner + 0.5, height}, {inner, height}}}, segments, angle);
            } else {
                ExtrudeOptions options;
                options.slices = 1 + pick(6);
                options.twist_degrees = pick(2) ? 0.0 : 10.0 * pick(9);
                options.scale_x = options.scale_y = pick(2) ? 1.0 : 0.5 + 0.1 * pick(8);
                const double reach = 2 + pick(4), waist = 0.5 + 0.25 * pick(4);
                body = extrude({{{reach, 0}, {waist, waist}, {0, reach}, {-waist, waist}, {-reach, 0}, {-waist, -waist}, {0, -reach}, {waist, -waist}}}, 3 + pick(5), options);
            }
            const int cuts = 1 + pick(3);
            for (int k = 0; k < cuts; ++k) {
                Solid t;
                switch (pick(4)) {
                    case 0: t = at(box({12, 1.0 + pick(3), 1.0 + pick(3)}), -6, -0.5 * pick(4), 0.5 * pick(8)); break;
                    case 1: {
                        const double radius = 0.5 + 0.25 * pick(5);
                        t = at(cylinder(16, radius, radius, segments).rotated(90, 0, 0), 0.5 * pick(5), 8, 0.5 + 0.5 * pick(6));
                        break;
                    }
                    case 2: t = at(sphere(2 + 0.5 * pick(5), segments), 0.5 * pick(6), 0.5 * pick(6), 0.5 * pick(8)); break;
                    default: {
                        const double radius = 0.5 + 0.25 * pick(4);
                        t = at(cylinder(12, radius, radius, segments), 0.5 * pick(4), 0.5 * pick(4), -1);
                        break;
                    }
                }
                const Solid next = apply(body, t, pick(4) ? Op::Difference : pick(2) ? Op::Union : Op::Intersection);
                if (!next.empty()) body = next;
            }
        } else {  // boxes turned by different angles, joined as one batch and trimmed
            std::vector<Solid> boxes;
            const int count = 3 + pick(8);
            for (int k = 0; k < count; ++k) {
                const double rx = pick(3) ? 7.0 * k : 90.0 * pick(4), rz = pick(3) ? 13.0 * k : 45.0 * pick(8);
                boxes.push_back(at(box({3.0 + pick(3), 2.0 + pick(3), 1.0 + pick(3)}, true).rotated(rx, 0, rz), 1.5 * (k % 4), 2.0 * (k / 4), 0.25 * (k % 3)));
            }
            const Solid all = batch(boxes, Op::Union);
            if (!all.empty()) apply(all, at(box({20, 4.5, 20}), -5, -1.25, -10), Op::Intersection);
        }
    } catch (const Refused &) {
        // Counted where it was recorded.
    }
}

const int kKinds = 10;

// ---- the models of the benchmark ----------------------------------------------------------------------

Solid plate_one_bore() { return box({40, 30, 6}) - at(cylinder(8, 4, 4, 96), 20, 15, -1); }

Solid bracket() {
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
    const Solid body = convex_hull(std::vector<Solid>{cylinder(5.5, 6, 4.2, 20), at(box({4, 5, 3}), 7.3, 2.1, 0)});
    ExtrudeOptions twisted;
    twisted.slices = 5;
    twisted.twist_degrees = 40;
    twisted.scale_x = twisted.scale_y = 0.8;
    const Solid slot = at(extrude({{{-2.15, -1.7}, {2.15, -1.7}, {2.15, 1.7}, {-2.15, 1.7}}}, 7.5, twisted), 0, 0, -1);
    const Solid cross = at(cylinder(30, 1.2, 1.2, 20, true), -2.5, 0, 0).rotated(0, 90, 17);
    const Solid post = at(batch_boolean({body, slot, cross}, Op::Difference), 31.3, 9.2, 5.25);
    return part + post;
}

Solid bores_overlapping(int count) {
    std::vector<Solid> bores;
    const Solid bore = at(cylinder(8, 2, 2, 96), 4, 15, -1);
    for (int k = 0; k < count; ++k) bores.push_back(at(bore, 1.5 * k, 0, 0));
    return box({8 + 1.5 * count, 30, 6}) - batch_boolean(bores, Op::Union);
}

Solid bores_pattern(int across) {
    std::vector<Solid> bores;
    const Solid bore = at(cylinder(8, 2, 2, 96), 5, 5, -1);
    for (int k = 0; k < across * across; ++k) bores.push_back(at(bore, 10.0 * (k % across), 10.0 * (k / across), 0));
    return box({10.0 * across, 10.0 * across, 6}) - batch_boolean(bores, Op::Union);
}

Solid bores_chain(int across) {
    Solid plate = box({10.0 * across, 10.0 * across, 6});
    const Solid bore = at(cylinder(8, 2, 2, 96), 5, 5, -1);
    for (int k = 0; k < across * across; ++k) plate = plate - at(bore, 10.0 * (k % across), 10.0 * (k / across), 0);
    return plate;
}

Solid two_spheres(int segments) { return sphere(10, segments) + at(sphere(10, segments), 7.3, 2.1, 1.7); }

Solid boxes_union() {
    std::vector<Solid> boxes;
    for (int k = 0; k < 50; ++k) boxes.push_back(at(box({6, 5, 4}, true).rotated(7.0 * k, 0, 13.0 * k), 3.5 * (k % 10), 4.0 * (k / 10), 0.3 * (k % 3)));
    return batch_boolean(boxes, Op::Union);
}

Solid difference_chain(int segments) {
    Solid block = box({60, 40, 12});
    for (int k = 0; k < 8; ++k) block = block - at(box({3, 30, 6}), 4.0 + 7 * k, 5, 8);
    for (int k = 0; k < 8; ++k) block = block - at(cylinder(14, 1.6, 1.1, segments), 5.5 + 7 * k, 20, -1);
    for (int k = 0; k < 4; ++k) block = block - at(cylinder(70, 1.5, 1.5, segments).rotated(0, 90, 0), -5, 8.0 + 8 * k, 4);
    return block - at(sphere(9, segments), 30, 20, 14);
}

Solid revolve_and_cut() {
    const Solid turned_body = revolve({{{4, 0}, {12, 0}, {12, 3}, {8, 5}, {8, 14}, {4, 14}}}, 96);
    return turned_body - at(box({30, 4, 6}), -15, -2, 9) - at(cylinder(40, 1.5, 1.5, 48).rotated(90, 0, 0), 0, 20, 2.5);
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

const std::vector<Model> &models() {
    static const std::vector<Model> list = {
        {"plate with one bore", plate_one_bore},
        {"bracket (the pinned part)", bracket},
        {"8 overlapping bores", [] { return bores_overlapping(8); }},
        {"64 overlapping bores", [] { return bores_overlapping(64); }},
        {"9 separate bores, one pattern", [] { return bores_pattern(3); }},
        {"64 separate bores, one pattern", [] { return bores_pattern(8); }},
        {"9 separate bores, one by one", [] { return bores_chain(3); }},
        {"64 separate bores, one by one", [] { return bores_chain(8); }},
        {"two spheres, 48 segments", [] { return two_spheres(48); }},
        {"two spheres, 96 segments", [] { return two_spheres(96); }},
        {"union of 50 boxes", boxes_union},
        {"difference chain, 48 segments", [] { return difference_chain(48); }},
        {"difference chain, 96 segments", [] { return difference_chain(96); }},
        {"revolve and cut", revolve_and_cut},
        {"extrude and cut", extrude_and_cut},
    };
    return list;
}

// ---- comparing two results ---------------------------------------------------------------------------

struct Measure {
    double thickness_nm = 0, reach_nm = 0;
};

// The distance from p to the triangle a b c, in grid units (doubles on exact coordinates: a measure).
double distance_to_triangle(const GridPoint &p, const GridPoint &a, const GridPoint &b, const GridPoint &c) {
    double ab[3], ac[3], ap[3];
    for (size_t k = 0; k < 3; ++k) {
        ab[k] = static_cast<double>(b[k] - a[k]);
        ac[k] = static_cast<double>(c[k] - a[k]);
        ap[k] = static_cast<double>(p[k] - a[k]);
    }
    const auto dot = [](const double *x, const double *y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    const double d1 = dot(ab, ap), d2 = dot(ac, ap), e1 = dot(ab, ab), e2 = dot(ab, ac), e3 = dot(ac, ac);
    // The nearest point of the triangle, by the region of its plane the foot of p falls in.
    double s = 0, t = 0;
    const double det = e1 * e3 - e2 * e2;
    s = (e3 * d1 - e2 * d2);
    t = (e1 * d2 - e2 * d1);
    if (det > 0 && s >= 0 && t >= 0 && s + t <= det) {
        s /= det;
        t /= det;
    } else {
        // On an edge: the nearest of the three edge distances.
        double best = 1e300;
        const GridPoint *corner[3] = {&a, &b, &c};
        for (int k = 0; k < 3; ++k) {
            const GridPoint &u = *corner[k], &w = *corner[(k + 1) % 3];
            double uw[3], up[3];
            for (size_t m = 0; m < 3; ++m) {
                uw[m] = static_cast<double>(w[m] - u[m]);
                up[m] = static_cast<double>(p[m] - u[m]);
            }
            const double length = dot(uw, uw);
            double along = length > 0 ? dot(uw, up) / length : 0;
            along = along < 0 ? 0 : along > 1 ? 1 : along;
            double q[3];
            for (size_t m = 0; m < 3; ++m) q[m] = up[m] - along * uw[m];
            best = std::fmin(best, dot(q, q));
        }
        return std::sqrt(best);
    }
    double q[3];
    for (size_t k = 0; k < 3; ++k) q[k] = ap[k] - s * ab[k] - t * ac[k];
    return std::sqrt(dot(q, q));
}

// The largest distance from a vertex of `from` that is not a vertex of `to` to the surface of `to`.
double reach(const Shape &from, const Shape &to) {
    std::vector<GridPoint> known = to.v;
    std::sort(known.begin(), known.end());
    double worst = 0;
    for (const GridPoint &p : from.v) {
        if (std::binary_search(known.begin(), known.end(), p)) continue;
        double best = 1e300;
        for (const Triangle &t : to.t) {
            const GridPoint &a = to.v[t[0]], &b = to.v[t[1]], &c = to.v[t[2]];
            // A triangle whose box is further away than the best so far cannot be nearer.
            bool far = false;
            for (size_t k = 0; k < 3 && !far; ++k) {
                const int64_t lo = std::min(a[k], std::min(b[k], c[k])), hi = std::max(a[k], std::max(b[k], c[k]));
                far = static_cast<double>(p[k] - hi) > best || static_cast<double>(lo - p[k]) > best;
            }
            if (!far) best = std::fmin(best, distance_to_triangle(p, a, b, c));
        }
        worst = std::fmax(worst, best);
    }
    return worst;
}

std::vector<GridPoint> sorted_vertices(const Shape &s) {
    std::vector<GridPoint> v = s.v;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

const char *op_name(int op) {
    static const char *const names[] = {"union", "difference", "intersection"};
    return op == 99 ? "model" : names[op & 3];
}

int compare(const char *path_a, const char *path_b, size_t exact_limit, bool validate) {
    FILE *fa = open_file(path_a, "rb"), *fb = open_file(path_b, "rb");
    Record a, b;
    size_t records = 0, both_refused = 0, only_a = 0, only_b = 0, both_empty = 0, same_mesh = 0, same_points = 0, within_grid = 0, different = 0, skipped = 0, judge_refused = 0;
    size_t same_vertices = 0, same_volume = 0, same_bounds = 0, compared = 0, invalid = 0, fewer = 0, more = 0, operands_differ = 0;
    size_t triangles_a = 0, triangles_b = 0;
    double worst_thickness = 0, worst_reach = 0, worst_area = 0;
    std::vector<size_t> by_kind_same(64, 0), by_kind_grid(64, 0), by_kind_all(64, 0);
    while (get(fa, a)) {
        if (!get(fb, b)) {
            std::printf("the second file ends after %zu records\n", records);
            return 1;
        }
        const size_t index = records++;
        if (a.operands != b.operands && !a.operands.empty() && !b.operands.empty()) ++operands_differ;
        const auto describe = [&]() { std::printf("record %zu (kind %d, %s of %zu):", index, a.kind, op_name(a.op), a.operands.size()); };
        if (a.status && b.status) {
            ++both_refused;
            continue;
        }
        if (a.status || b.status) {
            ++(a.status ? only_a : only_b);
            describe();
            std::printf(" refused by the %s only: %s\n", a.status ? "first" : "second", (a.status ? a.message : b.message).c_str());
            continue;
        }
        ++compared;
        ++by_kind_all[static_cast<size_t>(a.kind) & 63];
        triangles_a += a.result.t.size();
        triangles_b += b.result.t.size();
        if (b.result.t.size() < a.result.t.size()) ++fewer;
        if (b.result.t.size() > a.result.t.size()) ++more;
        if (a.result.t.empty() && b.result.t.empty()) {
            ++both_empty;
            ++same_points;
            ++same_vertices;
            ++same_volume;
            ++same_bounds;
            ++by_kind_same[static_cast<size_t>(a.kind) & 63];
            continue;
        }
        if (a.result.t.empty() || b.result.t.empty()) {
            ++different;
            describe();
            std::printf(" one result is empty and the other has %zu triangles\n", a.result.t.size() + b.result.t.size());
            continue;
        }
        const bool mesh = a.result == b.result;
        same_mesh += mesh;
        const bool vertices = mesh || sorted_vertices(a.result) == sorted_vertices(b.result);
        same_vertices += vertices;
        const detail::BigInt va = detail::six_volume(a.result.v, a.result.t), vb = detail::six_volume(b.result.v, b.result.t);
        const bool volume = detail::BigInt::cmp(va, vb) == 0;
        same_volume += volume;
        const Solid sa = solid_of(a.result), sb = solid_of(b.result);
        const Box3 ba = sa.bounds(), bb = sb.bounds();
        const bool bounds = ba.min.x == bb.min.x && ba.min.y == bb.min.y && ba.min.z == bb.min.z && ba.max.x == bb.max.x && ba.max.y == bb.max.y && ba.max.z == bb.max.z;
        same_bounds += bounds;
        if (validate && !(count_self_intersections(sb) == 0 && verify_exact(sb))) {
            ++invalid;
            describe();
            std::printf(" the second result is not a valid solid to this kernel\n");
        }
        if (mesh) {
            ++same_points;
            ++by_kind_same[static_cast<size_t>(a.kind) & 63];
            continue;
        }
        if (a.result.t.size() + b.result.t.size() > exact_limit) {
            ++skipped;
            describe();
            std::printf(" not compared exactly: %zu triangles in the two (volume %s, vertices %s, bounds %s)\n", a.result.t.size() + b.result.t.size(), volume ? "equal" : "differs",
                        vertices ? "equal" : "differ", bounds ? "equal" : "differ");
            continue;
        }
        bool same = false;
        try {
            same = volume && bounds && same_shape(sa, sb);
        } catch (const std::exception &error) {
            describe();
            std::printf(" the exact comparison failed: %s\n", error.what());
        }
        if (same) {
            ++same_points;
            ++by_kind_same[static_cast<size_t>(a.kind) & 63];
            continue;
        }
        // Not one set of points. Is either difference anything the kernel keeps?
        const double area = sa.surface_area() + sb.surface_area();
        const double thickness = std::fabs(sa.volume() - sb.volume()) / (area / 2) * 1e6;
        const double far = std::fmax(reach(a.result, b.result), reach(b.result, a.result));
        const double area_change = std::fabs(sa.surface_area() - sb.surface_area()) / (area / 2);
        bool empty = false, refused = false;
        double left = 0;
        try {
            const Solid ab = boolean(sa, sb, Op::Difference), ba2 = boolean(sb, sa, Op::Difference);
            empty = ab.empty() && ba2.empty();
            left = ab.volume() + ba2.volume();
        } catch (const ValueError &) {
            refused = true;
        }
        if (empty) {
            ++within_grid;
            ++by_kind_grid[static_cast<size_t>(a.kind) & 63];
            worst_thickness = std::fmax(worst_thickness, thickness);
            worst_reach = std::fmax(worst_reach, far);
            worst_area = std::fmax(worst_area, area_change);
            continue;
        }
        if (refused) ++judge_refused;
        else ++different;
        describe();
        std::printf(" %s: volumes %.17g and %.17g mm3 (a layer of %.3g nm), vertices %s, furthest vertex %.3g nm from the other surface, %zu and %zu triangles%s\n",
                    refused ? "the kernel refuses the difference of the two as too fine" : "the two differ", sa.volume(), sb.volume(), thickness, vertices ? "equal" : "differ", far,
                    a.result.t.size(), b.result.t.size(), refused ? "" : (", difference " + std::to_string(left) + " mm3").c_str());
    }
    if (get(fb, b)) std::printf("the second file has more records than the first\n");
    std::fclose(fa);
    std::fclose(fb);
    std::printf("%zu records: %zu refused in both, %zu in the first only, %zu in the second only\n", records, both_refused, only_a, only_b);
    std::printf("%zu results compared (%zu empty in both); the operands differ in %zu records\n", compared, both_empty, operands_differ);
    std::printf("  the same mesh                      %zu\n", same_mesh);
    std::printf("  the same set of points, exactly    %zu\n", same_points);
    std::printf("  within the grid (each difference empty to the kernel)  %zu: thickest layer %.3g nm, furthest vertex %.3g nm, largest change of area %.3g\n", within_grid, worst_thickness,
                worst_reach, worst_area);
    std::printf("  the kernel refuses their difference as too fine        %zu\n", judge_refused);
    std::printf("  different                          %zu\n", different);
    std::printf("  too large to compare exactly       %zu\n", skipped);
    std::printf("  equal exact volume %zu, equal bounds %zu, equal set of vertices %zu\n", same_volume, same_bounds, same_vertices);
    std::printf("  triangles: %zu in the first, %zu in the second; fewer in the second in %zu results, more in %zu\n", triangles_a, triangles_b, fewer, more);
    if (validate) std::printf("  results of the second file that are not valid solids to this kernel: %zu\n", invalid);
    std::printf("  by kind (the same set of points / within the grid / compared):");
    for (size_t k = 0; k < 64; ++k)
        if (by_kind_all[k]) std::printf(" %zu:%zu/%zu/%zu", k, by_kind_same[k], by_kind_grid[k], by_kind_all[k]);
    std::printf("\n");
    return different || only_a || only_b || invalid ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "record" && argc == 5) {
        const int cases = std::atoi(argv[2]);
        state = std::strtoull(argv[3], nullptr, 10);
        g_out = open_file(argv[4], "wb");
        for (int c = 0; c < cases; ++c)
            for (int kind = 0; kind < kKinds; ++kind) generate(kind);
        std::fclose(g_out);
        std::printf("%d models of each of %d kinds: %zu operations recorded, %zu refused\n", cases, kKinds, g_records, g_refused);
        return 0;
    }
    if (mode == "replay" && argc == 4) {
        FILE *in = open_file(argv[2], "rb");
        g_out = open_file(argv[3], "wb");
        Record r;
        size_t records = 0, refused = 0, same = 0;
        while (get(in, r)) {
            const Shape before = r.result;
            const int status = r.status;
            if (r.op == 99) {
                std::fprintf(stderr, "a file of models has no operands to replay\n");
                return 2;
            }
            std::vector<Solid> operands;
            for (const Shape &s : r.operands) operands.push_back(solid_of(s));
            run(r, operands);
            put(g_out, r);
            ++records;
            refused += r.status != 0;
            same += r.status == status && r.result == before;
        }
        std::fclose(in);
        std::fclose(g_out);
        std::printf("%zu operations replayed, %zu refused, %zu with the mesh of the recording\n", records, refused, same);
        return 0;
    }
    if (mode == "models" && argc >= 3) {
        g_out = open_file(argv[2], "wb");
        const char *filter = argc > 3 ? argv[3] : "";
        for (const Model &model : models()) {
            if (!std::strstr(model.name, filter)) continue;
            Record r;
            r.kind = 40;
            r.op = 99;
            try {
                r.result = shape_of(model.build());
            } catch (const ValueError &error) {
                r.status = std::strstr(error.what(), "too fine") ? 1 : 2;
                r.message = error.what();
            }
            put(g_out, r);
            std::printf("%-34s %7zu vertices %7zu triangles\n", model.name, r.result.v.size(), r.result.t.size());
        }
        std::fclose(g_out);
        return 0;
    }
    if (mode == "compare" && argc >= 4) {
        size_t limit = 400000;
        bool validate = false;
        for (int k = 4; k < argc; ++k) {
            if (!std::strcmp(argv[k], "--validate")) validate = true;
            else if (!std::strcmp(argv[k], "--limit") && k + 1 < argc) limit = static_cast<size_t>(std::strtoull(argv[++k], nullptr, 10));
        }
        return compare(argv[2], argv[3], limit, validate);
    }
    std::fprintf(stderr,
                 "usage: kerfkite_solid_shapes record CASES SEED FILE | replay FILE OUT | models FILE [FILTER] |\n"
                 "       compare FILE_A FILE_B [--validate] [--limit TRIANGLES]\n");
    return 2;
}
