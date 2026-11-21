// The "polygon" command: see kerfkite/polygon_command.hpp.
#include "kerfkite/polygon_command.hpp"

#include <cmath>

#include "kerfkite/cam.hpp"
#include "kerfkite/polygon.hpp"
#include "kerfkite/solid.hpp"
#include "../operations.hpp"
#include "kernel.hpp"

namespace kerfkite {

namespace {

using json = kerfkite::Json;
using namespace polygon;

const json &field(const json &request, const char *key) {
    if (!request.is_object() || !request.contains(key)) throw ValueError(std::string("The polygon request needs \"") + key + "\"");
    return request.at(key);
}

double number(const json &value, const char *what) {
    if (!value.is_number()) throw ValueError(std::string(what) + " must be a number");
    return value.get<double>();
}

double number(const json &request, const char *key, double fallback) {
    return request.contains(key) ? number(request.at(key), key) : fallback;
}

PathMm path(const json &value) {
    if (!value.is_array()) throw ValueError("A ring must be a list of [x, y] points");
    PathMm out;
    out.reserve(value.size());
    for (const json &point : value) {
        if (!point.is_array() || point.size() != 2) throw ValueError("A ring must be a list of [x, y] points");
        out.push_back({number(point[0], "A coordinate"), number(point[1], "A coordinate")});
    }
    return out;
}

std::vector<Ring> rings(const json &value) {
    if (!value.is_array()) throw ValueError("Rings must be a list of rings");
    std::vector<Ring> out;
    for (const json &ring : value) out.push_back(ring_from_mm(path(ring)));
    return out;
}

FillRule fill(const json &request) {
    const std::string name = request.value("fill", "evenodd");
    if (name == "evenodd") return FillRule::EvenOdd;
    if (name == "nonzero") return FillRule::NonZero;
    if (name == "positive") return FillRule::Positive;
    throw ValueError("fill must be evenodd, nonzero or positive");
}

ArcOptions arcs(const json &request) {
    ArcOptions options;
    const double segments = number(request, "segments", 32);
    if (!(segments >= 1 && segments <= 1024) || segments != std::floor(segments)) throw ValueError("segments must be a whole number from 1 to 1024");
    options.segments_per_quarter = static_cast<int>(segments);
    options.tolerance = number(request, "tolerance", 0);
    return options;
}

int whole(const json &request, const char *key, int fallback, int low, int high) {
    if (!request.contains(key)) return fallback;
    const double value = number(request.at(key), key);
    if (!(value >= low && value <= high) || value != std::floor(value))
        throw ValueError(std::string(key) + " must be a whole number from " + std::to_string(low) + " to " + std::to_string(high));
    return static_cast<int>(value);
}

// "join": "round", "miter" or "bevel" asks for the corners of a drawing, with "turn_segments" and "miter_limit".
JoinOptions joins(const json &request) {
    JoinOptions options;
    const std::string name = request.at("join").is_string() ? request.at("join").get<std::string>() : std::string();
    if (name == "round") options.join = Join::Round;
    else if (name == "miter") options.join = Join::Miter;
    else if (name == "bevel") options.join = Join::Bevel;
    else throw ValueError("join must be round, miter or bevel");
    options.segments = whole(request, "turn_segments", options.segments, 3, 4096);
    options.miter_limit = number(request, "miter_limit", options.miter_limit);
    return options;
}

// The rings of a region moved by x' = a x + b y + c, y' = d x + e y + f, in millimetres, and rounded to the
// grid. A map that turns the plane over turns every ring round, so each is reversed to bound its material
// on the left again; the rings are then read by the positive rule, which also settles any two that the
// rounding has made touch.
PolygonSet moved(const PolygonSet &region, const double m[6]) {
    for (int i = 0; i < 6; ++i)
        if (!std::isfinite(m[i])) throw ValueError("A transform must be finite");
    const bool turned_over = m[0] * m[4] - m[1] * m[3] < 0;
    std::vector<Ring> out;
    const auto move = [&](const Ring &ring) {
        Ring to;
        to.reserve(ring.size());
        for (const Point &p : ring) {
            const double x = to_mm(p.x), y = to_mm(p.y);
            to.push_back(to_grid(PointMm{m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]}));
        }
        if (turned_over) std::reverse(to.begin(), to.end());
        out.push_back(to);
    };
    for (const Polygon &polygon : region) {
        move(polygon.shell);
        for (const Ring &hole : polygon.holes) move(hole);
    }
    return normalise(out, FillRule::Positive);
}

json points(const Ring &ring) {
    json out = json::array();
    for (const Point &p : ring) out.push_back(json::array({to_mm(p.x), to_mm(p.y)}));
    return out;
}

json answer(const PolygonSet &region, const json &request) {
    json polygons = json::array();
    for (const Polygon &polygon : region) {
        json holes = json::array();
        for (const Ring &hole : polygon.holes) holes.push_back(points(hole));
        polygons.push_back(json{{"shell", points(polygon.shell)}, {"holes", holes}});
    }
    json out{{"polygons", polygons}, {"area", area(region)}};
    if (request.value("outlines", false)) {
        json paths = json::array();
        for (const PathMm &ring : outlines(region)) {
            json path = json::array();
            for (const PointMm &p : ring) path.push_back(json::array({p[0], p[1]}));
            paths.push_back(path);
        }
        out["outlines"] = paths;
    }
    if (request.value("check", false)) {
        std::string reason;
        out["valid"] = is_valid(region, &reason);
        if (!reason.empty()) out["reason"] = reason;
    }
    return out;
}

// studio.operations.region, with its messages: the code the operations themselves use.
PolygonSet compose_paths(const json &value) {
    if (!value.is_array()) throw ValueError("Contours must be lists of finite XY points");
    std::vector<PathMm> contours;
    for (const json &item : value) contours.push_back(path(item));
    return ::kerfkite::detail::contour_region(contours);
}

json artwork(const json &request) {
    Artwork image(arcs(request));
    for (const json &shape : field(request, "shapes")) {
        const std::string type = shape.value("type", "");
        const bool dark = shape.value("polarity", "dark") != "clear";
        const auto centre = [&](int axis) { return number(field(shape, "centre").at(static_cast<std::size_t>(axis)), "A centre"); };
        if (type == "disc")
            image.disc(centre(0), centre(1), number(field(shape, "diameter"), "diameter"), dark);
        else if (type == "rectangle")
            image.rectangle(centre(0), centre(1), number(field(shape, "width"), "width"), number(field(shape, "height"), "height"), dark);
        else if (type == "obround")
            image.obround(centre(0), centre(1), number(field(shape, "width"), "width"), number(field(shape, "height"), "height"), dark);
        else if (type == "stroke")
            image.stroke(path(field(shape, "points")), number(field(shape, "width"), "width"), dark);
        else if (type == "polygon")
            image.add(normalise(rings(field(shape, "rings")), fill(shape)), dark);
        else
            throw ValueError("A shape must be a disc, rectangle, obround, stroke or polygon");
    }
    const PolygonSet region = image.image();
    json out = answer(region, request);
    if (request.contains("offsets")) {
        json contours = json::array();
        for (const json &distance : request.at("offsets")) contours.push_back(answer(offset(region, number(distance, "An offset"), arcs(request)), request));
        out["contours"] = contours;
    }
    return out;
}

}  // namespace

bool polygon_command(const std::string &command) { return command == "polygon"; }

json polygon_call(const std::string &command, const json &request) {
    if (command != "polygon") throw ValueError("Unknown command: " + command);
    const std::string op = field(request, "op").is_string() ? request.at("op").get<std::string>() : std::string();
    if (op == "normalise") return answer(normalise(rings(field(request, "rings")), fill(request)), request);
    if (op == "compose") return answer(compose_paths(field(request, "paths")), request);
    if (op == "union" || op == "intersection" || op == "difference" || op == "xor") {
        const Operation operation = op == "union" ? Operation::Union : op == "intersection" ? Operation::Intersection : op == "difference" ? Operation::Difference : Operation::Xor;
        return answer(combine(operation, rings(field(request, "a")), fill(request), rings(field(request, "b")), fill(request)), request);
    }
    if (op == "offset") {
        const PolygonSet region = normalise(rings(field(request, "rings")), fill(request));
        const double distance = number(field(request, "distance"), "distance");
        return answer(request.contains("join") ? offset(region, distance, joins(request)) : offset(region, distance, arcs(request)), request);
    }
    if (op == "hull") {
        std::vector<Point> all;
        for (const Ring &ring : rings(field(request, "rings"))) all.insert(all.end(), ring.begin(), ring.end());
        const Ring hull = convex_hull(all);
        return answer(hull.empty() ? PolygonSet() : PolygonSet{Polygon{hull, {}}}, request);
    }
    if (op == "circle") {
        // The cross-section of the solid kernel's cylinder of this radius: the same vertices, rounded the same way.
        const double radius = number(field(request, "radius"), "radius") * kUnitsPerMm;
        if (!(radius >= 0.5 && radius <= static_cast<double>(kMaxDistance))) throw ValueError("A circle radius must be positive and within the range of the polygon kernel");
        const int count = whole(request, "segments", 48, 3, 65536);
        Ring ring;
        for (int k = 0; k < count; ++k) {
            double sine, cosine;
            solid::sincos_turn(k, count, sine, cosine);
            ring.push_back({static_cast<Coord>(std::round(radius * cosine)), static_cast<Coord>(std::round(radius * sine))});
        }
        return answer(normalise({ring}, FillRule::Positive), request);
    }
    if (op == "transform" || op == "rotate") {
        double m[6] = {1, 0, 0, 0, 1, 0};
        if (op == "rotate") {
            double sine, cosine;
            solid::sincos_degrees(number(field(request, "degrees"), "degrees"), sine, cosine);
            m[0] = cosine, m[1] = -sine, m[3] = sine, m[4] = cosine;
        } else {
            const json &rows = field(request, "matrix");
            if (!rows.is_array() || rows.size() != 2 || !rows[0].is_array() || rows[0].size() != 3 || !rows[1].is_array() || rows[1].size() != 3)
                throw ValueError("matrix must be two rows of three numbers");
            for (std::size_t i = 0; i < 6; ++i) m[i] = number(rows[i / 3][i % 3], "matrix");
        }
        return answer(moved(normalise(rings(field(request, "rings")), fill(request)), m), request);
    }
    if (op == "pocket") {
        const double first = number(field(request, "first"), "first"), step = number(field(request, "step"), "step");
        if (!(first > 0) || !(step > 0)) throw ValueError("first and step must be greater than zero");
        json passes = json::array();
        std::size_t vertices = 0;
        for (PolygonSet inset = offset(normalise(rings(field(request, "rings")), fill(request)), -first, arcs(request)); !inset.empty();
             inset = offset(inset, -step, arcs(request))) {
            passes.push_back(answer(inset, request));
            vertices += vertex_count(inset);
            if (vertices > 1000000) throw ValueError("Pocket exceeds vertex limit");
        }
        return json{{"passes", passes}};
    }
    if (op == "clip-lines") {
        const PolygonSet region = normalise(rings(field(request, "rings")), fill(request));
        const LineClipper clipper(region);
        json all = json::array();
        for (const json &line : field(request, "lines")) {
            const PathMm ends = path(line);
            if (ends.size() != 2) throw ValueError("A line must be two points");
            json parts = json::array();
            for (const polygon::Segment &part : clipper.clip(to_grid(ends[0]), to_grid(ends[1]))) parts.push_back(points({part[0], part[1]}));
            all.push_back(parts);
        }
        return json{{"segments", all}};
    }
    if (op == "artwork") return artwork(request);
    if (op == "inspect") {
        const std::vector<Ring> given = rings(field(request, "rings"));
        json each = json::array();
        for (const Ring &ring : given) each.push_back(json{{"simple", is_simple(ring)}, {"area", signed_area(ring)}, {"vertices", ring.size()}});
        return json{{"rings", each}};
    }
    if (op == "locate") {
        const PolygonSet region = normalise(rings(field(request, "rings")), fill(request));
        json where = json::array();
        for (const PointMm &point : path(field(request, "points"))) {
            const Location location = locate(region, to_grid(point));
            where.push_back(location == Location::Inside ? "inside" : location == Location::Boundary ? "boundary" : "outside");
        }
        return json{{"where", where}};
    }
    if (op == "delaunay" || op == "voronoi") {
        std::vector<Point> sites;
        for (const PointMm &point : path(field(request, "points"))) sites.push_back(to_grid(point));
        const Triangulation triangulation = delaunay(sites);
        json out{{"points", points(triangulation.points)}};
        if (op == "delaunay") {
            json triangles = json::array(), edges = json::array();
            for (const std::array<std::uint32_t, 3> &t : triangulation.triangles) triangles.push_back(json::array({t[0], t[1], t[2]}));
            const auto side = [](std::uint32_t triangle) { return triangle == kNoTriangle ? json() : json(triangle); };
            for (const Triangulation::Edge &e : triangulation.edges) edges.push_back(json::array({e.a, e.b, side(e.left), side(e.right)}));
            out["triangles"] = triangles;
            out["edges"] = edges;
            return out;
        }
        const PathMm box = path(field(request, "box"));
        if (box.size() != 2) throw ValueError("A box must be two points");
        json edges = json::array();
        for (const VoronoiEdge &e : voronoi_edges(triangulation, to_grid(box[0]), to_grid(box[1])))
            edges.push_back(json{{"ends", points({e.ends[0], e.ends[1]})}, {"sites", json::array({e.sites[0], e.sites[1]})}});
        out["edges"] = edges;
        return out;
    }
    if (op == "distance") {
        const PolygonSet region = normalise(rings(field(request, "rings")), fill(request));
        const BoundaryDistance wall(region);
        json distances = json::array();
        for (const PointMm &point : path(field(request, "points"))) distances.push_back(wall.to(to_grid(point)) / kUnitsPerMm);
        return json{{"distances", distances}};
    }
    if (op == "statistics") return json{{"rounds", polygon::detail::resolve_rounds()}};
    throw ValueError("Unknown polygon operation: " + op);
}

}  // namespace kerfkite
