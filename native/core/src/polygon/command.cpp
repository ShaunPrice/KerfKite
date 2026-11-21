// The "polygon" command: see kerfkite/polygon_command.hpp.
#include "kerfkite/polygon_command.hpp"

#include <cmath>

#include "kerfkite/cam.hpp"
#include "kerfkite/polygon.hpp"
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

// studio.operations.region, with its messages.
PolygonSet compose_paths(const json &value) {
    if (!value.is_array()) throw ValueError("Contours must be lists of finite XY points");
    std::vector<Ring> contours;
    for (const json &item : value) {
        const PathMm contour = path(item);
        const double gap_x = contour.empty() ? 0 : contour.front()[0] - contour.back()[0], gap_y = contour.empty() ? 0 : contour.front()[1] - contour.back()[1];
        if (contour.size() < 4 || std::sqrt(gap_x * gap_x + gap_y * gap_y) > 1e-6)
            throw ValueError("This operation needs closed contours");
        const Ring ring = ring_from_mm(contour);
        if (!is_simple(ring) || std::fabs(signed_area(ring)) < 1e-9) throw ValueError("Repair self-crossing or zero-area contours before CAM");
        contours.push_back(ring);
    }
    const PolygonSet region = compose(contours);
    if (region.empty()) throw ValueError("Contours cancel under even-odd fill");
    return region;
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
    if (op == "offset")
        return answer(offset(normalise(rings(field(request, "rings")), fill(request)), number(field(request, "distance"), "distance"), arcs(request)), request);
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
    if (op == "statistics") return json{{"rounds", polygon::detail::resolve_rounds()}};
    throw ValueError("Unknown polygon operation: " + op);
}

}  // namespace kerfkite
