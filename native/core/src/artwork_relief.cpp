// Artwork relief: Studio's /api/artwork-relief on KerfKite's own polygon kernel. Closed artwork contours become
// a height field of 0s and 1s, which prepare_relief then turns into finishing paths.
//
// Each sample point of a resolution x resolution grid stands for a cell reaching one grid step to each side. A
// raised relief keeps material at a point whose cell meets the artwork, its outline included, so that the cutter
// never enters the artwork; a recessed relief keeps material at a point whose cell the artwork does not cover
// entirely. The reference asked Shapely's intersects and covers of each cell; here the artwork is the kernel's
// even-odd region of the contours (operations.cpp's contour_region, with the reference's messages), each cell is
// placed on the kernel's grid of a millionth of a millimetre, and both questions are decided exactly:
//
// * a cell meets the artwork when an edge of the region meets the closed cell, or the cell's centre is inside;
// * the artwork covers a cell when no edge of the region enters the open cell and its centre is inside.
//
// The answers can differ from Shapely's only where the artwork's outline passes within a millionth of a
// millimetre of a cell's edge, or where contours cross one another and GEOS rounds the crossing points.
#include "kerfkite/artwork_relief.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "kerfkite/polygon.hpp"
#include "kerfkite/relief.hpp"
#include "operations.hpp"
#include "polygon/kernel.hpp"
#include "project.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;

namespace {

using polygon::Coord;
using polygon::Point;
using polygon::detail::cross;
using polygon::detail::sign;

// studio.operations.value(data, key, default, lo, hi), with the bounds as the reference's message writes them.
double setting(const json &data, const char *key, double fallback, double lo, double hi, const char *lo_text,
               const char *hi_text) {
    const json *found = get(data, key);
    if (!found) return fallback;
    double result = 0;
    const bool readable = !found->is_boolean() && (plain_number(*found, result) ||
                                                   (found->is_string() && float_from_text(found->get_ref<const std::string &>(), result)));
    if (!readable) throw ValueError(std::string(key) + " must be numeric");
    if (!std::isfinite(result) || !(lo <= result && result <= hi))
        throw ValueError(std::string(key) + " must be between " + lo_text + " and " + hi_text);
    return result;
}

// studio.operations._contours(paths): a list of lists of [x, y] finite numbers, or its message.
std::vector<std::vector<std::array<double, 2>>> artwork_contours(const json &given) {
    const char *message = "Contours must be lists of finite XY points";
    if (!given.is_array()) throw ValueError(message);
    std::vector<std::vector<std::array<double, 2>>> paths;
    paths.reserve(given.size());
    for (const json &contour : given) {
        if (!contour.is_array()) throw ValueError(message);
        std::vector<std::array<double, 2>> path;
        path.reserve(contour.size());
        for (const json &point : contour) {
            if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) throw ValueError(message);
            const double px = point[0].get<double>(), py = point[1].get<double>();
            if (!std::isfinite(px) || !std::isfinite(py)) throw ValueError(message);
            path.push_back({px, py});
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

struct Edge {
    Point a, b;
    Coord low_x, high_x, low_y, high_y;
};

struct Cell {
    Coord low_x, high_x, low_y, high_y;
    Point centre;
};

// Whether the line through an edge leaves the cell's corners on both sides (open = false: or on the line).
// With the boxes overlapping, that is whether the edge meets the cell: the box axes and the edge's normal are
// the only directions that could separate them.
bool line_meets(const Edge &edge, const Cell &cell, bool open) {
    const Point corners[4] = {{cell.low_x, cell.low_y}, {cell.high_x, cell.low_y}, {cell.high_x, cell.high_y}, {cell.low_x, cell.high_y}};
    bool left = false, right = false;
    for (const Point &corner : corners) {
        const int side = sign(cross(edge.a, edge.b, corner));
        if (side > 0) left = true;
        else if (side < 0) right = true;
        else if (!open) return true;
    }
    return left && right;
}

// The closed cell meets the edge.
bool meets_closed(const Edge &edge, const Cell &cell) {
    if (edge.high_x < cell.low_x || edge.low_x > cell.high_x || edge.high_y < cell.low_y || edge.low_y > cell.high_y) return false;
    return line_meets(edge, cell, false);
}

// The edge enters the open cell.
bool meets_open(const Edge &edge, const Cell &cell) {
    if (edge.high_x <= cell.low_x || edge.low_x >= cell.high_x || edge.high_y <= cell.low_y || edge.low_y >= cell.high_y) return false;
    return line_meets(edge, cell, true);
}

// Whether a point on no edge is inside the region: an odd number of edges cross the ray to its right. An edge
// counts when the point's y is in [low, high) of its ends, and it crosses to the right when the point is to
// the left of the edge directed upwards.
bool inside(const std::vector<const Edge *> &row, const Point &point) {
    bool odd = false;
    for (const Edge *edge : row) {
        const Point &low = edge->a.y < edge->b.y ? edge->a : edge->b, &high = edge->a.y < edge->b.y ? edge->b : edge->a;
        if (point.y < low.y || point.y >= high.y) continue;
        if (sign(cross(low, high, point)) > 0) odd = !odd;
    }
    return odd;
}

}  // namespace

std::vector<std::vector<int>> artwork_raster(const Json &data) {
    if (!data.is_object()) throw ValueError("Supply closed artwork paths");
    // data.get('paths', []): no paths at all is an empty list, which the even-odd rule leaves empty.
    std::vector<std::vector<std::array<double, 2>>> paths;
    if (data.contains("paths")) paths = artwork_contours(data.at("paths"));
    const polygon::PolygonSet region = contour_region(paths);
    const double width = setting(data, "width", 100, 0.1, 2000, "0.1", "2000");
    const double height = setting(data, "height", 100, 0.1, 2000, "0.1", "2000");
    const double x = setting(data, "x", 0, 0, 2000, "0", "2000");
    const double y = setting(data, "y", 0, 0, 2000, "0", "2000");
    const int n = static_cast<int>(setting(data, "resolution", 64, 16, 128, "16", "128"));
    const json *kind_value = get(data, "kind");
    const bool raised = !kind_value || is_text(kind_value, "raised");
    if (!raised && !is_text(kind_value, "recessed")) throw ValueError("Choose raised or recessed relief");

    std::vector<Edge> edges;
    for (const polygon::Polygon &polygon : region) {
        auto add = [&](const polygon::Ring &ring) {
            for (std::size_t i = 0; i < ring.size(); ++i) {
                const Point &a = ring[i], &b = ring[(i + 1) % ring.size()];
                edges.push_back({a, b, std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.y, b.y), std::max(a.y, b.y)});
            }
        };
        add(polygon.shell);
        for (const polygon::Ring &hole : polygon.holes) add(hole);
    }
    std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) { return a.low_y < b.low_y; });

    std::vector<std::vector<int>> matrix(std::size_t(n), std::vector<int>(std::size_t(n), 0));
    std::vector<const Edge *> band, row_edges;
    for (int row = 0; row < n; ++row) {
        // The reference's arithmetic for the cell, then each value on the grid.
        const double py = y + row * height / (n - 1), half_y = height / (n - 1);
        const Coord low_y = polygon::to_grid(py - half_y), high_y = polygon::to_grid(py + half_y);
        const Coord centre_y = polygon::to_grid(py);
        band.clear();
        row_edges.clear();
        for (const Edge &edge : edges) {
            if (edge.low_y > high_y) break;
            if (edge.high_y >= low_y) band.push_back(&edge);
            if (edge.low_y <= centre_y && centre_y < edge.high_y) row_edges.push_back(&edge);
        }
        for (int col = 0; col < n; ++col) {
            const double px = x + col * width / (n - 1), half_x = width / (n - 1);
            Cell cell{polygon::to_grid(px - half_x), polygon::to_grid(px + half_x), low_y, high_y, {polygon::to_grid(px), centre_y}};
            bool touched = false;
            for (const Edge *edge : band) {
                if (raised ? meets_closed(*edge, cell) : meets_open(*edge, cell)) {
                    touched = true;
                    break;
                }
            }
            // Untouched, the cell lies wholly inside the artwork or wholly outside it, and its centre says which.
            const bool within = !touched && inside(row_edges, cell.centre);
            matrix[std::size_t(row)][std::size_t(col)] = raised ? (touched || within ? 1 : 0) : (touched || !within ? 1 : 0);
        }
    }
    return matrix;
}

Json artwork_relief(const Json &data) {
    const std::vector<std::vector<int>> matrix = artwork_raster(data);
    json given = data;
    json heightmap = json::array();
    for (const std::vector<int> &row : matrix) {
        json values = json::array();
        for (int value : row) values.push_back(value);
        heightmap.push_back(std::move(values));
    }
    given["heightmap"] = std::move(heightmap);
    json result = prepare_relief(given);
    result["warnings"].push_back("Artwork relief is conservatively rasterized; increase map resolution for small glyph details.");
    return result;
}

bool artwork_command(const std::string &command) { return command == "artwork-relief" || command == "artwork-raster"; }

Json artwork_call(const std::string &command, const Json &request) {
    if (command == "artwork-raster") {
        json rows = json::array();
        for (const std::vector<int> &row : artwork_raster(request)) {
            json values = json::array();
            for (int value : row) values.push_back(value);
            rows.push_back(std::move(values));
        }
        return rows;
    }
    return artwork_relief(request);
}

}  // namespace kerfkite
