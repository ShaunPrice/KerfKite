// The JSON front of the solid kernel: see solid_command.hpp.
#include "kerfkite/solid_command.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "kerfkite/solid.hpp"

using json = nlohmann::json;

namespace kerfkite {

namespace {

using namespace solid;

const int kMaxDepth = 64;
const std::size_t kMaxNodes = 100000;

double number(const json &value, const char *what) {
    if (!value.is_number()) throw ValueError(std::string(what) + " must be a number");
    const double v = value.get<double>();
    if (!std::isfinite(v)) throw ValueError(std::string(what) + " must be finite");
    return v;
}

double number_or(const json &object, const char *key, double fallback) { return object.contains(key) ? number(object.at(key), key) : fallback; }

int whole(const json &value, const char *what, int low, int high) {
    if (!value.is_number_integer()) throw ValueError(std::string(what) + " must be a whole number");
    const long long v = value.get<long long>();
    if (v < low || v > high) throw ValueError(std::string(what) + " must be from " + std::to_string(low) + " to " + std::to_string(high));
    return static_cast<int>(v);
}

bool flag(const json &object, const char *key, bool fallback) {
    if (!object.contains(key)) return fallback;
    if (!object.at(key).is_boolean()) throw ValueError(std::string(key) + " must be true or false");
    return object.at(key).get<bool>();
}

Vec3 vec3(const json &value, const char *what) {
    if (!value.is_array() || value.size() != 3) throw ValueError(std::string(what) + " must be three numbers");
    return Vec3{number(value[0], what), number(value[1], what), number(value[2], what)};
}

FillRule fill_rule(const json &object) {
    if (!object.contains("fill")) return FillRule::EvenOdd;
    const std::string name = object.at("fill").is_string() ? object.at("fill").get<std::string>() : std::string();
    if (name == "evenodd") return FillRule::EvenOdd;
    if (name == "nonzero") return FillRule::NonZero;
    if (name == "positive") return FillRule::Positive;
    throw ValueError("fill must be \"evenodd\", \"nonzero\" or \"positive\"");
}

std::vector<Ring> rings_of(const json &value) {
    if (!value.is_array()) throw ValueError("rings must be a list of rings");
    std::vector<Ring> rings;
    for (const json &ring : value) {
        if (!ring.is_array()) throw ValueError("each ring must be a list of points");
        Ring out;
        for (const json &p : ring) {
            if (!p.is_array() || p.size() != 2) throw ValueError("each ring point must be two numbers");
            out.push_back(Vec2{number(p[0], "a ring coordinate"), number(p[1], "a ring coordinate")});
        }
        rings.push_back(out);
    }
    return rings;
}

json rings_json(const std::vector<Ring> &rings) {
    json out = json::array();
    for (const Ring &ring : rings) {
        json points = json::array();
        for (const Vec2 &p : ring) points.push_back({p.x, p.y});
        out.push_back(points);
    }
    return out;
}

std::vector<Triangle> triangles_of(const json &value) {
    if (!value.is_array()) throw ValueError("triangles must be a list");
    std::vector<Triangle> out;
    for (const json &t : value) {
        if (!t.is_array() || t.size() != 3) throw ValueError("each triangle must be three vertex numbers");
        Triangle tri;
        for (std::size_t k = 0; k < 3; ++k) {
            if (!t[k].is_number_unsigned() || t[k].get<std::uint64_t>() > 0xffffffffu) throw ValueError("a triangle names a vertex that does not exist");
            tri[k] = static_cast<std::uint32_t>(t[k].get<std::uint64_t>());
        }
        out.push_back(tri);
    }
    return out;
}

struct Builder {
    std::size_t nodes = 0;

    std::vector<Solid> list(const json &value, int depth) {
        if (!value.is_array()) throw ValueError("an operation needs a list of shapes");
        std::vector<Solid> out;
        for (const json &child : value) out.push_back(build(child, depth + 1));
        return out;
    }

    Solid build(const json &node, int depth) {
        if (depth > kMaxDepth || ++nodes > kMaxNodes) throw ValueError("The shape is nested too deeply or has too many parts");
        if (!node.is_object()) throw ValueError("Each shape must be an object");
        if (node.contains("box")) return box(vec3(node.at("box"), "box"), flag(node, "center", false));
        if (node.contains("cylinder")) {
            const json &c = node.at("cylinder");
            if (!c.is_object()) throw ValueError("cylinder must be an object");
            const double radius = number_or(c, "radius", 1);
            return cylinder(number_or(c, "height", 1), number_or(c, "radius_low", radius), number_or(c, "radius_high", radius),
                            c.contains("segments") ? whole(c.at("segments"), "segments", 3, 65536) : 48, flag(c, "center", false));
        }
        if (node.contains("sphere")) {
            const json &s = node.at("sphere");
            if (!s.is_object()) throw ValueError("sphere must be an object");
            return sphere(number_or(s, "radius", 1), s.contains("segments") ? whole(s.at("segments"), "segments", 3, 65536) : 48);
        }
        if (node.contains("extrude")) {
            const json &e = node.at("extrude");
            if (!e.is_object() || !e.contains("rings")) throw ValueError("extrude needs rings and a height");
            ExtrudeOptions options;
            if (e.contains("slices")) options.slices = whole(e.at("slices"), "slices", 1, 4096);
            options.twist_degrees = number_or(e, "twist", 0);
            if (e.contains("scale")) {
                const json &s = e.at("scale");
                if (!s.is_array() || s.size() != 2) throw ValueError("scale must be two numbers");
                options.scale_x = number(s[0], "scale");
                options.scale_y = number(s[1], "scale");
            }
            options.fill = fill_rule(e);
            return extrude(rings_of(e.at("rings")), number_or(e, "height", 1), options);
        }
        if (node.contains("revolve")) {
            const json &r = node.at("revolve");
            if (!r.is_object() || !r.contains("rings")) throw ValueError("revolve needs rings");
            return revolve(rings_of(r.at("rings")), r.contains("segments") ? whole(r.at("segments"), "segments", 3, 65536) : 48, number_or(r, "degrees", 360), fill_rule(r));
        }
        if (node.contains("mesh") || node.contains("grid_mesh")) {
            const bool grid = node.contains("grid_mesh");
            const json &m = node.at(grid ? "grid_mesh" : "mesh");
            if (!m.is_object() || !m.contains("vertices") || !m.contains("triangles") || !m.at("vertices").is_array()) throw ValueError("a mesh needs vertices and triangles");
            const std::vector<Triangle> triangles = triangles_of(m.at("triangles"));
            if (grid) {
                std::vector<GridPoint> vertices;
                for (const json &p : m.at("vertices")) {
                    if (!p.is_array() || p.size() != 3 || !p[0].is_number_integer() || !p[1].is_number_integer() || !p[2].is_number_integer())
                        throw ValueError("each grid vertex must be three whole numbers of nanometres");
                    vertices.push_back(GridPoint{{p[0].get<std::int64_t>(), p[1].get<std::int64_t>(), p[2].get<std::int64_t>()}});
                }
                return from_grid_mesh(vertices, triangles, flag(m, "weld", false));
            }
            std::vector<Vec3> vertices;
            for (const json &p : m.at("vertices")) vertices.push_back(vec3(p, "a vertex"));
            return from_mesh(vertices, triangles, flag(m, "weld", false));
        }
        if (node.contains("hull")) return convex_hull(list(node.at("hull"), depth));
        if (node.contains("union")) return batch_boolean(list(node.at("union"), depth), Op::Union);
        if (node.contains("difference")) return batch_boolean(list(node.at("difference"), depth), Op::Difference);
        if (node.contains("intersection")) return batch_boolean(list(node.at("intersection"), depth), Op::Intersection);
        if (!node.contains("of")) throw ValueError("Unknown shape: use box, cylinder, sphere, extrude, revolve, mesh, hull, union, difference, intersection or a transform with \"of\"");
        const Solid inner = build(node.at("of"), depth + 1);
        if (node.contains("translate")) return inner.translated(vec3(node.at("translate"), "translate"));
        if (node.contains("rotate")) {
            const Vec3 r = vec3(node.at("rotate"), "rotate");
            return inner.rotated(r.x, r.y, r.z);
        }
        if (node.contains("rotate_about")) {
            const json &r = node.at("rotate_about");
            if (!r.is_object() || !r.contains("axis")) throw ValueError("rotate_about needs an axis and degrees");
            return inner.rotated_about(vec3(r.at("axis"), "axis"), number_or(r, "degrees", 0));
        }
        if (node.contains("scale")) return inner.scaled(vec3(node.at("scale"), "scale"));
        if (node.contains("mirror")) return inner.mirrored(vec3(node.at("mirror"), "mirror"));
        if (node.contains("matrix")) {
            const json &m = node.at("matrix");
            if (!m.is_array() || m.size() != 3) throw ValueError("matrix must be three rows of four numbers");
            std::array<double, 12> a{};
            for (std::size_t r = 0; r < 3; ++r) {
                if (!m[r].is_array() || m[r].size() != 4) throw ValueError("matrix must be three rows of four numbers");
                for (std::size_t c = 0; c < 4; ++c) a[4 * r + c] = number(m[r][c], "matrix");
            }
            return inner.transformed(a);
        }
        throw ValueError("Unknown transform: use translate, rotate, rotate_about, scale, mirror or matrix");
    }
};

double area_of(const std::vector<Ring> &rings) {
    double total = 0;
    for (const Ring &ring : rings) total += ring_area(ring);
    return total;
}

}  // namespace

json solid_call(const json &request) {
    if (!request.is_object() || !request.contains("shape")) throw ValueError("A solid request needs a \"shape\"");
    Builder builder;
    const Solid result = builder.build(request.at("shape"), 0);
    const Box3 box = result.bounds();
    const Vec3 centre = result.centroid();
    const BooleanStats stats = last_boolean_stats();
    json answer = {{"empty", result.empty()},
                   {"volume", result.volume()},
                   {"area", result.surface_area()},
                   {"bounds", {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z}},
                   {"centroid", {centre.x, centre.y, centre.z}},
                   {"vertices", result.num_vertices()},
                   {"triangles", result.num_triangles()},
                   {"bodies", result.num_bodies()},
                   {"last_operation",
                    {{"candidate_pairs", stats.candidate_pairs},
                     {"intersecting_pairs", stats.intersecting_pairs},
                     {"split_triangles", stats.split_triangles},
                     {"coplanar_groups", stats.coplanar_groups},
                     {"constructed_points", stats.constructed_points},
                     {"rounded_vertices", stats.rounded_vertices},
                     {"repair_passes", stats.repair_passes},
                     {"coarser_retries", stats.coarser_retries}}}};
    if (flag(request, "mesh", false)) {
        json vertices = json::array(), triangles = json::array();
        for (const Vec3 &p : result.vertices()) vertices.push_back({p.x, p.y, p.z});
        for (const Triangle &t : result.triangles()) triangles.push_back({t[0], t[1], t[2]});
        answer["mesh"] = {{"vertices", vertices}, {"triangles", triangles}};
    }
    if (flag(request, "grid_mesh", false)) {
        json vertices = json::array(), triangles = json::array();
        for (const GridPoint &p : result.grid_vertices()) vertices.push_back({p[0], p[1], p[2]});
        for (const Triangle &t : result.triangles()) triangles.push_back({t[0], t[1], t[2]});
        answer["grid_mesh"] = {{"vertices", vertices}, {"triangles", triangles}};
    }
    if (flag(request, "check", false)) {
        answer["self_intersections"] = count_self_intersections(result);
        answer["verified"] = verify_exact(result);
    }
    if (request.contains("sections")) {
        if (!request.at("sections").is_array() || request.at("sections").size() > 10000) throw ValueError("sections must be a list of up to 10000 heights");
        json sections = json::array();
        for (const json &z : request.at("sections")) {
            const std::vector<Ring> rings = section(result, number(z, "a section height"));
            sections.push_back({{"z", z}, {"area", area_of(rings)}, {"rings", rings_json(rings)}});
        }
        answer["sections"] = sections;
    }
    if (request.contains("plane_sections")) {
        if (!request.at("plane_sections").is_array() || request.at("plane_sections").size() > 10000) throw ValueError("plane_sections must be a list of up to 10000 planes");
        json sections = json::array();
        for (const json &p : request.at("plane_sections")) {
            if (!p.is_object() || !p.contains("origin") || !p.contains("normal")) throw ValueError("each plane needs an origin and a normal");
            const PlaneSection s = section(result, Plane3{vec3(p.at("origin"), "origin"), vec3(p.at("normal"), "normal")});
            sections.push_back({{"area", area_of(s.rings)}, {"u", {s.u.x, s.u.y, s.u.z}}, {"v", {s.v.x, s.v.y, s.v.z}}, {"rings", rings_json(s.rings)}});
        }
        answer["plane_sections"] = sections;
    }
    if (flag(request, "projection", false)) {
        const std::vector<Ring> rings = projection(result);
        answer["projection"] = {{"area", area_of(rings)}, {"rings", rings_json(rings)}};
    }
    if (request.contains("height_map")) {
        const json &h = request.at("height_map");
        if (!h.is_object() || !h.contains("columns") || !h.contains("rows")) throw ValueError("height_map needs x0, y0, dx, dy, columns and rows");
        const HeightMap map = height_map(result, number_or(h, "x0", 0), number_or(h, "y0", 0), number_or(h, "dx", 1), number_or(h, "dy", 1),
                                         static_cast<std::size_t>(whole(h.at("columns"), "columns", 1, 4096)), static_cast<std::size_t>(whole(h.at("rows"), "rows", 1, 4096)),
                                         number_or(h, "empty", 0));
        json rows = json::array();
        for (std::size_t j = 0; j < map.rows; ++j) {
            json row = json::array();
            for (std::size_t i = 0; i < map.columns; ++i) row.push_back(map.at(i, j));
            rows.push_back(row);
        }
        answer["height_map"] = rows;
    }
    if (request.contains("rays")) {
        if (!request.at("rays").is_array() || request.at("rays").size() > 1000000) throw ValueError("rays must be a list of up to 1000000 rays");
        const Index index(result);
        json hits = json::array();
        for (const json &r : request.at("rays")) {
            if (!r.is_object() || !r.contains("origin") || !r.contains("direction")) throw ValueError("each ray needs an origin and a direction");
            const Hit hit = index.raycast(vec3(r.at("origin"), "origin"), vec3(r.at("direction"), "direction"));
            if (!hit.hit) hits.push_back(nullptr);
            else hits.push_back({{"distance", hit.distance}, {"point", {hit.point.x, hit.point.y, hit.point.z}}, {"normal", {hit.normal.x, hit.normal.y, hit.normal.z}}, {"triangle", hit.triangle}});
        }
        answer["rays"] = hits;
    }
    if (request.contains("stl")) {
        if (request.at("stl") != "ascii") throw ValueError("stl must be \"ascii\"");
        answer["stl"] = to_stl_ascii(result);
    }
    return answer;
}

}  // namespace kerfkite
