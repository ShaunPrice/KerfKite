// Rigid assemblies of parametric parts: studio/assembly.py and studio/assembly_constraints.py.
//
// The part placements, the mate tree, the additional mates fitted by Gauss-Newton on the joints' free axes,
// and the contacts and interferences between the parts, statement for statement, with the reference's checks
// and messages. Each part is the parametric history rebuilt by solid_cad.cpp, placed by the kernel, and the
// overlaps and gaps are the kernel's.
//
// The matrices are the core's arithmetic: products in order, the inverse by LU decomposition with partial
// pivoting, the fitted mates' least squares by the singular value decomposition of least_squares.hpp. The
// reference has NumPy's BLAS and LAPACK for these, whose last bits differ between platforms, so the
// placements agree with the Python's to rounding (about 10^-15 of their size), not to the bit, and the core
// is the one implementation Studio uses. validation/assembly_check.py compares the two.
#include "assembly.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "least_squares.hpp"
#include "project.hpp"
#include "py_trig.hpp"
#include "python_text.hpp"
#include "solid_cad_build.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;
using namespace solid_requests;

namespace {

const double kContact = 1e-6;
const size_t kMaxParts = 24;
const long long kMaxTriangles = 150000;
const double kTolerance = 1e-6;

using Mat4 = std::array<std::array<double, 4>, 4>;
using Vec3 = std::array<double, 3>;

Mat4 identity() {
    Mat4 m{};
    for (int i = 0; i < 4; ++i) m[i][i] = 1.0;
    return m;
}

Mat4 product(const Mat4 &a, const Mat4 &b) {
    Mat4 c{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double total = a[i][0] * b[0][j];
            for (int k = 1; k < 4; ++k) total += a[i][k] * b[k][j];
            c[i][j] = total;
        }
    return c;
}

// np.linalg.inv: LU decomposition with partial pivoting, then the columns of the identity solved for.
Mat4 inverse(const Mat4 &given) {
    Mat4 a = given;
    std::array<int, 4> order = {0, 1, 2, 3};
    for (int j = 0; j < 4; ++j) {
        int pivot = j;
        for (int i = j + 1; i < 4; ++i)
            if (std::fabs(a[i][j]) > std::fabs(a[pivot][j])) pivot = i;
        if (a[pivot][j] == 0) throw ValueError("Singular matrix");
        std::swap(a[pivot], a[j]);
        std::swap(order[pivot], order[j]);
        for (int i = j + 1; i < 4; ++i) {
            a[i][j] /= a[j][j];
            for (int k = j + 1; k < 4; ++k) a[i][k] -= a[i][j] * a[j][k];
        }
    }
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        std::array<double, 4> x{};
        for (int i = 0; i < 4; ++i) {
            double value = order[i] == column ? 1.0 : 0.0;
            for (int k = 0; k < i; ++k) value -= a[i][k] * x[k];
            x[i] = value;
        }
        for (int i = 3; i >= 0; --i) {
            double value = x[i];
            for (int k = i + 1; k < 4; ++k) value -= a[i][k] * x[k];
            x[i] = value / a[i][i];
        }
        for (int i = 0; i < 4; ++i) result[i][column] = x[i];
    }
    return result;
}

// _vector: three ints or floats within ±100000.
Vec3 coordinates(const json *value) {
    const char *const message = "Use three finite coordinates within ±100000";
    if (!value || !value->is_array() || value->size() != 3) throw ValueError(message);
    Vec3 result{};
    for (size_t i = 0; i < 3; ++i) {
        const json &item = (*value)[i];
        if (!item.is_number()) throw ValueError(message);
        const double v = item.get<double>();
        if (!(-100000 <= v && v <= 100000)) throw ValueError(message);
        result[i] = v;
    }
    return result;
}

const json &zero_vector() {
    static const json zero = json::array({0, 0, 0});
    return zero;
}

const json *member(const json &object, const char *key, const json &fallback) {
    return object.contains(key) ? get(object, key) : &fallback;
}

// _pose from an origin and Euler angles in radians (extrinsic XYZ, Rz Ry Rx).
Mat4 pose_of(const Vec3 &position, const Vec3 &angles) {
    const double cx = pytrig::cosine(angles[0]), cy = pytrig::cosine(angles[1]), cz = pytrig::cosine(angles[2]);
    const double sx = pytrig::sine(angles[0]), sy = pytrig::sine(angles[1]), sz = pytrig::sine(angles[2]);
    Mat4 m = identity();
    m[0] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, position[0]};
    m[1] = {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, position[1]};
    m[2] = {-sy, cy * sx, cy * cx, position[2]};
    return m;
}

const double kDegToRad = pytrig::PI / 180.0;  // np.radians
const double kRadToDeg = 180.0 / pytrig::PI;  // np.degrees

// _pose(frame).
Mat4 pose(const json *frame) {
    if (!frame || !frame->is_object()) throw ValueError("Each datum frame needs an origin and rotation");
    const Vec3 position = coordinates(member(*frame, "origin", zero_vector()));
    const Vec3 degrees = coordinates(member(*frame, "rotation", zero_vector()));
    return pose_of(position, {degrees[0] * kDegToRad, degrees[1] * kDegToRad, degrees[2] * kDegToRad});
}

// _pose({'origin': values[:3], 'rotation': np.degrees(values[3:])}): the joint's motion, its checks included.
Mat4 motion_pose(const std::array<double, 6> &values) {
    const char *const message = "Use three finite coordinates within ±100000";
    Vec3 position{}, angles{};
    for (int i = 0; i < 3; ++i) {
        const double degrees = values[3 + i] * kRadToDeg;
        if (!(-100000 <= values[i] && values[i] <= 100000) || !(-100000 <= degrees && degrees <= 100000)) throw ValueError(message);
        position[i] = values[i];
        angles[i] = degrees * kDegToRad;
    }
    return pose_of(position, angles);
}

const std::map<std::string, std::vector<int>> &freedoms() {
    static const std::map<std::string, std::vector<int>> axes = {
        {"fixed", {}}, {"planar", {0, 1, 5}}, {"concentric", {2, 5}}, {"point", {3, 4, 5}}};
    return axes;
}

// _joint(mate): the parent frame, the motion along the free axes and the inverse of the child frame.
Mat4 joint(const json &mate) {
    const json *kind = get(mate, "kind");
    const Vec3 offset = coordinates(member(mate, "offset", zero_vector()));
    const Vec3 rotation = coordinates(member(mate, "rotation", zero_vector()));
    const auto none = [](std::initializer_list<double> values) {
        for (const double v : values)
            if (v != 0) return false;
        return true;
    };
    bool valid;
    if (is_text(kind, "fixed")) valid = none({offset[0], offset[1], offset[2]}) && none({rotation[0], rotation[1], rotation[2]});
    else if (is_text(kind, "planar")) valid = offset[2] == 0 && none({rotation[0], rotation[1]});
    else if (is_text(kind, "concentric")) valid = none({offset[0], offset[1]}) && none({rotation[0], rotation[1]});
    else if (is_text(kind, "point")) valid = none({offset[0], offset[1], offset[2]});
    else throw ValueError("Choose a fixed, planar, concentric or point mate");
    if (!valid) throw ValueError(kind->get<std::string>() + " mate has a displacement or rotation on a locked axis");
    static const json empty = json::object();
    const Mat4 motion = pose_of(offset, {rotation[0] * kDegToRad, rotation[1] * kDegToRad, rotation[2] * kDegToRad});
    return product(product(pose(member(mate, "parentFrame", empty)), motion), inverse(pose(member(mate, "childFrame", empty))));
}

double norm(const std::vector<double> &values) {
    double total = 0;
    for (const double v : values) total += v * v;
    return std::sqrt(total);
}

struct Drive {
    std::string parent;
    Mat4 parent_frame, child_inverse;
    std::array<double, 6> values;
};

struct Target {
    std::string child, parent, kind;
    Mat4 child_frame, parent_frame;
};

// assembly_constraints.fit: the joints' free axes moved until the additional mates hold.
std::map<std::string, Mat4> fit(const std::map<std::string, Mat4> &world, const std::vector<std::string> &names,
                                const std::vector<std::pair<std::string, const json *>> &tree, const std::map<std::string, Mat4> &roots,
                                const json *constraints) {
    static const json none = json::array();
    if (!constraints) constraints = &none;
    if (!constraints->is_array() || constraints->size() > 48) throw ValueError("Use a list of up to 48 additional assembly mates");
    if (constraints->empty()) return world;
    std::map<std::string, const json *> mates;
    for (const auto &item : tree) mates[item.first] = item.second;
    std::vector<Target> targets;
    std::set<std::string> involved;
    static const json empty = json::object();
    for (const json &item : *constraints) {
        const json *kind = item.is_object() ? get(item, "kind") : nullptr;
        if (!kind || !kind->is_string() || !freedoms().count(kind->get<std::string>()))
            throw ValueError("Choose a fixed, planar, concentric or point additional mate");
        const json *child = get(item, "child"), *parent = get(item, "parent");
        for (const json *name : {child, parent})
            if (!name || !name->is_string() || !world.count(name->get<std::string>()))
                throw ValueError("An additional mate must connect two different existing parts");
        if (child->get<std::string>() == parent->get<std::string>()) throw ValueError("An additional mate must connect two different existing parts");
        Target target{child->get<std::string>(), parent->get<std::string>(), kind->get<std::string>(), {}, {}};
        target.child_frame = pose(member(item, "childFrame", empty));
        target.parent_frame = pose(member(item, "parentFrame", empty));
        for (std::string name : {target.child, target.parent})
            while (mates.count(name)) {
                involved.insert(name);
                name = get(*mates.at(name), "parent")->get<std::string>();
            }
        targets.push_back(std::move(target));
    }
    std::map<std::string, Drive> drives;
    std::vector<std::pair<std::string, int>> variables;
    std::vector<double> start;
    for (const auto &item : tree) {
        const json &mate = *item.second;
        const Vec3 offset = coordinates(member(mate, "offset", zero_vector()));
        const Vec3 rotation = coordinates(member(mate, "rotation", zero_vector()));
        Drive drive;
        drive.parent = get(mate, "parent")->get<std::string>();
        drive.parent_frame = pose(member(mate, "parentFrame", empty));
        drive.child_inverse = inverse(pose(member(mate, "childFrame", empty)));
        drive.values = {offset[0], offset[1], offset[2], rotation[0] * kDegToRad, rotation[1] * kDegToRad, rotation[2] * kDegToRad};
        if (involved.count(item.first))
            for (const int axis : freedoms().at(get(mate, "kind")->get<std::string>())) {
                variables.push_back({item.first, axis});
                start.push_back(drive.values[static_cast<size_t>(axis)]);
            }
        drives[item.first] = drive;
    }
    // The driving tree already satisfies its own mates. This parameterisation preserves those equations,
    // including locked axes, throughout the solve.
    const auto evaluate = [&](const std::vector<double> &parameters, std::map<std::string, Mat4> &solved) {
        solved = roots;
        std::map<std::string, std::array<double, 6>> motion;
        for (const auto &entry : drives) motion[entry.first] = entry.second.values;
        for (size_t i = 0; i < variables.size(); ++i) motion[variables[i].first][static_cast<size_t>(variables[i].second)] = parameters[i];
        std::function<Mat4(const std::string &)> place = [&](const std::string &name) -> Mat4 {
            const auto known = solved.find(name);
            if (known != solved.end()) return known->second;
            const Drive &drive = drives.at(name);
            const Mat4 transform = motion_pose(motion.at(name));
            const Mat4 placed = product(product(product(place(drive.parent), drive.parent_frame), transform), drive.child_inverse);
            solved[name] = placed;
            return placed;
        };
        for (const std::string &name : names) place(name);
        std::vector<double> rows;
        for (const Target &target : targets) {
            const Mat4 first = product(solved.at(target.child), target.child_frame);
            const Mat4 second = product(solved.at(target.parent), target.parent_frame);
            const Vec3 axis = {second[0][2], second[1][2], second[2][2]};
            const Vec3 delta = {first[0][3] - second[0][3], first[1][3] - second[1][3], first[2][3] - second[2][3]};
            const double along = delta[0] * axis[0] + delta[1] * axis[1] + delta[2] * axis[2];
            if (target.kind == "fixed" || target.kind == "point") rows.insert(rows.end(), delta.begin(), delta.end());
            else if (target.kind == "planar") rows.push_back(along);
            else
                for (int i = 0; i < 3; ++i) rows.push_back(delta[i] - along * axis[i]);
            if (target.kind == "fixed") {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) rows.push_back(first[i][j] - second[i][j]);
            } else if (target.kind != "point") {
                for (int i = 0; i < 3; ++i) rows.push_back(first[i][2] - axis[i]);
            }
        }
        return rows;
    };
    std::vector<double> x = start;
    std::map<std::string, Mat4> solved;
    std::vector<double> residual = evaluate(x, solved);
    for (int iteration = 0; iteration < 40; ++iteration) {
        double largest = 0;
        for (const double r : residual) largest = std::max(largest, std::fabs(r));
        if (largest <= kTolerance) return solved;
        if (variables.empty()) break;
        Matrix jacobian(residual.size(), std::vector<double>(x.size()));
        for (size_t i = 0; i < x.size(); ++i) {
            std::vector<double> trial = x;
            trial[i] += 1e-6;
            std::map<std::string, Mat4> unused;
            const std::vector<double> moved = evaluate(trial, unused);
            for (size_t r = 0; r < residual.size(); ++r) jacobian[r][i] = (moved[r] - residual[r]) / 1e-6;
        }
        std::vector<double> negated;
        for (const double r : residual) negated.push_back(-r);
        const std::vector<double> delta = least_squares(jacobian, residual.size(), x.size(), negated, 1e-9);
        bool finite = true;
        for (const double d : delta) finite = finite && std::isfinite(d);
        if (!finite || norm(delta) < 1e-12) break;
        const double old_norm = norm(residual);
        double scale = 1.0;
        for (size_t i = 0; i < delta.size(); ++i)
            if (variables[i].second >= 3 && std::fabs(delta[i]) > pytrig::PI) scale = std::min(scale, pytrig::PI / std::fabs(delta[i]));
        bool accepted = false;
        for (int halving = 0; halving < 16; ++halving) {
            std::vector<double> trial(x.size());
            bool inside = true;
            for (size_t i = 0; i < x.size(); ++i) {
                trial[i] = x[i] + delta[i] * scale;
                inside = inside && std::fabs(trial[i]) <= (variables[i].second < 3 ? 1e5 : 1e5 * kDegToRad);
            }
            if (inside) {
                std::map<std::string, Mat4> positions;
                const std::vector<double> candidate = evaluate(trial, positions);
                if (norm(candidate) < old_norm) {
                    x = trial;
                    residual = candidate;
                    solved = positions;
                    accepted = true;
                    break;
                }
            }
            scale *= 0.5;
        }
        if (!accepted) break;
    }
    throw ValueError(
        "Additional mates could not be satisfied from these joint positions. Check the datum frames, independent placements and "
        "conflicting constraints.");
}

json rows_of(const Mat4 &m) {
    json rows = json::array();
    for (const auto &row : m) rows.push_back(json::array({row[0], row[1], row[2], row[3]}));
    return rows;
}

}  // namespace

// assembly.solve(document, include_mesh).
json assembly_solve(const json &payload) {
    if (!payload.is_object()) throw ValueError("Supply an assembly and a boolean mesh flag");
    const json *mesh_flag = get(payload, "mesh");
    if (mesh_flag && !mesh_flag->is_boolean()) throw ValueError("Supply an assembly and a boolean mesh flag");
    const bool include_mesh = mesh_flag && mesh_flag->get<bool>();
    const json *document = get(payload, "assembly");
    const json *orders = get(payload, "orders");
    const json *version = document && document->is_object() ? get(*document, "version") : nullptr;
    if (!version || !version->is_number_integer() || version->get<long long>() != 1) throw ValueError("Expected a version 1 assembly");
    const json *parts = get(*document, "parts");
    static const json no_mates = json::array();
    const json *mates = member(*document, "mates", no_mates);
    if (!parts || !parts->is_array() || parts->size() < 1 || parts->size() > kMaxParts) throw ValueError("An assembly needs 1–24 parts");
    if (!mates->is_array() || mates->size() >= parts->size()) throw ValueError("Provide a mate tree with at least one independent root part");
    std::vector<std::string> names;
    std::map<std::string, Mat4> free;
    for (const json &part : *parts) {
        const json *name = part.is_object() ? get(part, "id") : nullptr;
        bool good = name && name->is_string();
        if (good) {
            const std::string &text = name->get_ref<const std::string &>();
            const std::vector<CodePoint> points = code_points(text);
            good = !python_strip(text, points).empty() && points.size() <= 80 && !free.count(text);
        }
        if (!good) throw ValueError("Each part needs a unique name of 1–80 characters");
        names.push_back(name->get<std::string>());
        free[names.back()] = pose(&part);
    }
    std::map<std::string, std::string> parents;
    std::map<std::string, Mat4> local;
    std::vector<std::pair<std::string, const json *>> tree;
    for (const json &mate : *mates) {
        if (!mate.is_object()) throw ValueError("Each mate must specify a child and parent part");
        const json *child = get(mate, "child"), *parent = get(mate, "parent");
        for (const json *key : {child, parent})
            if (!key || !key->is_string() || !free.count(key->get<std::string>())) throw ValueError("Mate refers to an unknown part");
        const std::string name = child->get<std::string>();
        if (parents.count(name)) throw ValueError(name + " already has a driving mate");
        parents[name] = parent->get<std::string>();
        local[name] = joint(mate);
        tree.emplace_back(name, &mate);
    }
    std::map<std::string, Mat4> world;
    std::function<Mat4(const std::string &, std::set<std::string>)> resolve = [&](const std::string &name, std::set<std::string> visiting) -> Mat4 {
        if (visiting.count(name)) throw ValueError("Assembly mates contain a cycle");
        const auto known = world.find(name);
        if (known != world.end()) return known->second;
        Mat4 placed;
        if (parents.count(name)) {
            visiting.insert(name);
            placed = product(resolve(parents.at(name), visiting), local.at(name));
        } else {
            placed = free.at(name);
        }
        world[name] = placed;
        return placed;
    };
    for (const std::string &name : names) resolve(name, {});
    std::map<std::string, Mat4> roots;
    for (const std::string &name : names)
        if (!parents.count(name)) roots[name] = free.at(name);
    world = fit(world, names, tree, roots, get(*document, "constraints"));

    std::vector<Shape> solids;
    json result = json::object();
    long long total_triangles = 0;
    std::vector<std::vector<double>> boxes;
    for (size_t i = 0; i < names.size(); ++i) {
        const json &part = (*parts)[i];
        const json *order = orders && orders->is_array() && i < orders->size() && !(*orders)[i].is_null() ? &(*orders)[i] : nullptr;
        const Shape geometry = solid_cad_build(get(part, "model"), order);
        total_triangles += geometry.info.at("triangles").get<long long>();
        if (total_triangles > kMaxTriangles) throw ValueError("Assembly exceeds the 150000 triangle budget");
        const Mat4 &m = world.at(names[i]);
        json matrix = json::array();
        for (int r = 0; r < 3; ++r) matrix.push_back(json::array({m[r][0], m[r][1], m[r][2], m[r][3]}));
        json node = json::object();
        node["matrix"] = std::move(matrix);
        node["of"] = kept_node(0);
        Shape solid = ask(node, {&geometry});
        json entry = json::object();
        entry["matrix"] = rows_of(m);
        entry["bounds"] = solid.info.at("bounds");
        std::vector<double> box;
        for (const json &value : solid.info.at("bounds")) box.push_back(value.get<double>());
        boxes.push_back(box);
        if (include_mesh) {
            json vertices, triangles;
            mesh_of(solid, vertices, triangles);
            entry["vertices"] = std::move(vertices);
            entry["triangles"] = std::move(triangles);
        }
        result[names[i]] = std::move(entry);
        solids.push_back(std::move(solid));
    }
    json contacts = json::array(), collisions = json::array();
    for (size_t a = 0; a < solids.size(); ++a)
        for (size_t b = a + 1; b < solids.size(); ++b) {
            const std::vector<double> &ba = boxes[a], &bb = boxes[b];
            bool apart = false;
            for (int i = 0; i < 3; ++i) apart = apart || ba[i + 3] + kContact < bb[i] || bb[i + 3] + kContact < ba[i];
            if (apart) continue;
            const Shape overlap = combined("intersection", {&solids[a], &solids[b]});
            const double volume = overlap.info.at("volume").get<double>();
            const json pair = json::array({names[a], names[b]});
            if (volume > 1e-9) {
                json collision = json::object();
                collision["parts"] = pair;
                collision["volume"] = volume;
                collisions.push_back(std::move(collision));
                continue;
            }
            json request = json::object();
            request["shape"] = kept_node(0);
            json gap = json::object();
            gap["to"] = kept_node(1);
            gap["search_length"] = 2 * kContact;
            request["min_gap"] = std::move(gap);
            json answer;
            try {
                answer = solid_call(request, {solids[a].solid, solids[b].solid}, nullptr);
            } catch (const ValueError &error) {
                throw too_fine(error);
            }
            if (answer.at("min_gap").get<double>() <= kContact) {
                json contact = json::object();
                contact["parts"] = pair;
                contacts.push_back(std::move(contact));
            }
        }
    json answer = json::object();
    answer["parts"] = std::move(result);
    answer["contacts"] = std::move(contacts);
    answer["interferences"] = std::move(collisions);
    return answer;
}

}  // namespace kerfkite
