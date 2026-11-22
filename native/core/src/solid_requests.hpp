// The solid and polygon kernels asked as studio/solid_kernel.py asks them, for the ports of the Python that
// models through that module (solid_cad.cpp, scad.cpp). Internal to the core.
//
// The Python keeps each solid the kernel answers and names it in the next request as {"kept": i}; a 2D
// drawing is kept as the rings of the polygon kernel's answer. The functions here make the same requests,
// so the solids, rings and measures are the same to the bit.
#ifndef KERFKITE_SOLID_REQUESTS_HPP
#define KERFKITE_SOLID_REQUESTS_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/polygon_command.hpp"
#include "kerfkite/solid_command.hpp"

namespace kerfkite {
namespace solid_requests {

using json = Json;

// A kept solid and the kernel's answer about it: "empty", "volume", "triangles", "bounds" and so on.
struct Shape {
    solid::Solid solid;
    json info;
};

// As JSON text reads it: a whole number of no sign is unsigned, which the solid command requires here.
inline json kept_node(size_t index) { return json{{"kept", static_cast<std::uint64_t>(index)}}; }

inline const char *const kTooFine = "too fine for the 1 nm grid";

// solid_kernel._solid's advice for a result the grid cannot hold.
inline ValueError too_fine(const ValueError &error) {
    const std::string text = error.what();
    if (text.find(kTooFine) == std::string::npos) return error;
    return ValueError("The solid kernel cannot represent this result: " + text +
                      ". Two faces almost coincide here, usually after a rotation. Move, turn or resize one of the shapes by a small amount");
}

// One request to the kernel: its answer and its solid.
inline Shape ask(const json &node, const std::vector<const Shape *> &kept) {
    json request = json::object();
    request["shape"] = node;
    std::vector<solid::Solid> solids;
    solids.reserve(kept.size());
    for (const Shape *shape : kept) solids.push_back(shape->solid);
    Shape result;
    try {
        result.info = solid_call(request, solids, &result.solid);
    } catch (const ValueError &error) {
        throw too_fine(error);
    }
    return result;
}

// A question about a kept solid: "projection", "sections" and so on.
inline json question(const Shape &shape, const char *key, const json &value) {
    json request = json::object();
    request["shape"] = kept_node(0);
    request[key] = value;
    try {
        return solid_call(request, {shape.solid}, nullptr);
    } catch (const ValueError &error) {
        throw too_fine(error);
    }
}

inline json numbers(const std::vector<double> &values) {
    json result = json::array();
    for (double value : values) result.push_back(value);
    return result;
}

// Manifold.translate, rotate, scale and mirror: {how: values, "of": the solid}.
inline Shape mapped(const Shape &shape, const char *how, const std::vector<double> &values) {
    json node = json::object();
    node[how] = numbers(values);
    node["of"] = kept_node(0);
    return ask(node, {&shape});
}

// Manifold.batch_boolean and batch_hull: {op: [every solid]}.
inline Shape combined(const char *op, const std::vector<const Shape *> &shapes) {
    json list = json::array();
    for (size_t i = 0; i < shapes.size(); ++i) list.push_back(kept_node(i));
    json node = json::object();
    node[op] = std::move(list);
    return ask(node, shapes);
}

// A region as solid_kernel._polygon answers it: the rings of each polygon, its shell then its holes, and
// the area.
struct Region {
    std::vector<json> rings;
    double area = 0.0;
};

inline Region region_answer(const json &answer) {
    Region region;
    for (const json &polygon : answer.at("polygons")) {
        region.rings.push_back(polygon.at("shell"));
        for (const json &hole : polygon.at("holes")) region.rings.push_back(hole);
    }
    region.area = answer.at("area").get<double>();
    return region;
}

// One request to the polygon kernel.
inline Region polygon_region(const char *op, json request) {
    request["op"] = op;
    return region_answer(polygon_call("polygon", request));
}

// CrossSection(rings, fill): the polygon kernel's normal form of the rings; none when there are none.
inline Region region_of(const std::vector<json> &rings, const char *fill) {
    if (rings.empty()) return Region();
    json request = json::object();
    request["rings"] = json(rings);
    request["fill"] = fill;
    return polygon_region("normalise", std::move(request));
}

inline std::vector<json> normalised(const std::vector<json> &rings, const char *fill) { return region_of(rings, fill).rings; }

// solid_cad._rings: each ring closed by its first point again.
inline json closed(const std::vector<json> &rings) {
    json paths = json::array();
    for (const json &ring : rings) {
        json path = ring;
        if (ring.size()) path.push_back(ring[0]);
        paths.push_back(std::move(path));
    }
    return paths;
}

// solid_kernel._between: whether b lies within two grid units of the straight line from a to c.
inline bool between(const json &a, const json &b, const json &c) {
    const double ax = a[0].get<double>(), ay = a[1].get<double>();
    const double ux = c[0].get<double>() - ax, uy = c[1].get<double>() - ay;
    const double vx = b[0].get<double>() - ax, vy = b[1].get<double>() - ay;
    const double square = ux * ux + uy * uy, across = ux * vy - uy * vx, along = ux * vx + uy * vy;
    const double straight = 2e-6;
    return square > 0 && across * across <= straight * straight * square && 0 <= along && along <= square;
}

// solid_kernel._straightened: section rings without the points that only divide a straight side.
inline std::vector<json> straightened(const json &rings) {
    std::vector<json> result;
    for (const json &ring : rings) {
        std::vector<json> kept;
        for (const json &point : ring) {
            while (kept.size() >= 2 && between(kept[kept.size() - 2], kept.back(), point)) kept.pop_back();
            kept.push_back(point);
        }
        while (kept.size() > 3 && between(kept[kept.size() - 2], kept.back(), kept[0])) kept.pop_back();
        while (kept.size() > 3 && between(kept.back(), kept[0], kept[1])) kept.erase(kept.begin());
        result.push_back(json(kept));
    }
    return result;
}

inline std::vector<json> rings_in(const json &value) {
    std::vector<json> rings;
    for (const json &ring : value) rings.push_back(ring);
    return rings;
}

// The mesh of a solid as the Python reads it back (to_mesh64): vertices and triangles as lists.
inline void mesh_of(const Shape &shape, json &vertices, json &triangles) {
    vertices = json::array();
    for (const solid::Vec3 &vertex : shape.solid.vertices()) vertices.push_back(json::array({vertex.x, vertex.y, vertex.z}));
    triangles = json::array();
    for (const solid::Triangle &triangle : shape.solid.triangles())
        triangles.push_back(json::array({std::int64_t(triangle[0]), std::int64_t(triangle[1]), std::int64_t(triangle[2])}));
}

}  // namespace solid_requests
}  // namespace kerfkite

#endif
