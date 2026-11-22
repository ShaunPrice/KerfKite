// The stock-removal preview: studio/simulation.py's simulate, statement for statement where the order of the
// arithmetic or of the checks shows in the result.
//
// Every check comes first, segment by segment in the reference's order, so that the same error stops the same job
// before any cell is computed. Then the cells, shared out between threads by grid rows: each thread takes every
// segment in order but changes only its own rows, so each cell sees the same minima in the same order as on one
// thread. Distances use CPython's hypot (pymath::vector_norm) and minima Python's min(), which keeps the first of
// two equal values, so that zeros keep the reference's sign.
#include "kerfkite/stock.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <system_error>
#include <vector>

#include "kerfkite/cam.hpp"  // NotPorted
#include "kerfkite/parallel.hpp"
#include "kerfkite/py_math.hpp"
#include "project.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;

namespace {

constexpr size_t kSegmentLimit = 200000;
constexpr double kVisitLimit = 8000000;
constexpr double kDegreesToRadians = 3.141592653589793 / 180.0;  // CPython's degToRad: Py_MATH_PI / 180.0

// studio.operations.value(data, key, default, lo, hi).
double value(const json &data, const char *key, double fallback, double lo, double hi, const char *lo_text,
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

// float(value) for one value of a preview row: a number, a bool, or numeric text. Anything else raises TypeError
// in the reference, which simulate reports as below; text that float() cannot read raises a message quoting it,
// which is declined.
double row_value(const json &item) {
    double result = 0;
    if (item.is_boolean()) return item.get<bool>() ? 1.0 : 0.0;
    if (plain_number(item, result)) return result;
    if (item.is_string()) {
        if (float_from_text(item.get_ref<const std::string &>(), result)) return result;
        throw NotPorted("preview rows with text that is not a number");
    }
    throw ValueError("Stock geometry and dimensions must be numeric");
}

// The cells a segment can reach; x1 < x0 when it reaches none.
struct Reach {
    int x0, x1, y0, y1;
};

// Python's min(a, b): b only when it is less, so of two equal values the first.
inline double first_min(double a, double b) { return b < a ? b : a; }

}  // namespace

Json simulate_stock(const Json &data, const std::vector<double> *rows) {
    if (!data.is_object()) throw ValueError("Stock preview requires an object");
    const json *segments = rows ? nullptr : get(data, "segments");
    if (!rows && segments && !segments->is_array()) throw ValueError("Invalid segment count");
    const size_t count = rows ? rows->size() / 7 : segments ? segments->size() : 0;
    if (count > kSegmentLimit) throw ValueError("Invalid segment count");
    const double origin_x = value(data, "x", 0, 0, 2000, "0", "2000");
    const double origin_y = value(data, "y", 0, 0, 2000, "0", "2000");
    const double width = value(data, "width", 100, 0.1, 2000, "0.1", "2000");
    const double height = value(data, "height", 100, 0.1, 2000, "0.1", "2000");
    const int n = static_cast<int>(value(data, "resolution", 48, 16, 96, "16", "96"));
    const double diameter = value(data, "tool_diameter", 3.175, 0.01, 100, "0.01", "100");
    const double radius = diameter / 2;
    const json *shape_value = get(data, "tool_shape");
    const double angle = value(data, "v_angle", 60, 10, 170, "10", "170");
    int shape = -1;  // flat, ball, v, drill
    if (!shape_value) shape = 0;
    else if (is_text(shape_value, "flat")) shape = 0;
    else if (is_text(shape_value, "ball")) shape = 1;
    else if (is_text(shape_value, "v")) shape = 2;
    else if (is_text(shape_value, "drill")) shape = 3;
    if (shape < 0) throw ValueError("Unknown simulated cutter");
    const double stock = value(data, "stock_thickness", 10, 0.01, 200, "0.01", "200");

    // Each segment as (ax, ay, bx, by, z, start z) relative to the stock origin, with the checks in order.
    std::vector<double> moves(count * 6);
    std::vector<Reach> reach(count);
    const double dx = width / (n - 1), dy = height / (n - 1);
    double visits = 0;
    for (size_t k = 0; k < count; ++k) {
        double raw[7];
        size_t given = 7;
        if (rows) {
            std::copy(rows->begin() + std::ptrdiff_t(k * 7), rows->begin() + std::ptrdiff_t(k * 7 + 7), raw);
        } else {
            const json &row = (*segments)[k];
            if (!row.is_array() || row.size() < 5) throw ValueError("Invalid preview segment");
            given = row.size();
            for (size_t f = 0; f < 5; ++f) raw[f] = row_value(row[f]);
            if (given > 6) raw[6] = row_value(row[6]);
        }
        const double ax = raw[0] - origin_x, bx = raw[2] - origin_x;
        const double ay = raw[1] - origin_y, by = raw[3] - origin_y;
        const double z = raw[4], start = given > 6 ? raw[6] : z;
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(z) ||
            !std::isfinite(start))
            throw ValueError("Non-finite segment");
        if (py_min(z, start) < -stock) throw ValueError("Cut exceeds simulated stock thickness");
        const double lo_x = (py_min(ax, bx) - radius) / dx, hi_x = (py_max(ax, bx) + radius) / dx;
        const double lo_y = (py_min(ay, by) - radius) / dy, hi_y = (py_max(ay, by) + radius) / dy;
        // math.floor and math.ceil of an infinity raise OverflowError, which the reference reports so.
        if (!std::isfinite(lo_x) || !std::isfinite(hi_x) || !std::isfinite(lo_y) || !std::isfinite(hi_y))
            throw ValueError("Segment exceeds numeric preview range");
        const double x0 = py_max(0.0, std::floor(lo_x)), x1 = py_min(double(n - 1), std::ceil(hi_x));
        const double y0 = py_max(0.0, std::floor(lo_y)), y1 = py_min(double(n - 1), std::ceil(hi_y));
        double *move = moves.data() + k * 6;
        move[0] = ax, move[1] = ay, move[2] = bx, move[3] = by, move[4] = z, move[5] = start;
        if (x1 < x0 || y1 < y0) {
            reach[k] = {1, 0, 1, 0};
            continue;
        }
        visits += (x1 - x0 + 1) * (y1 - y0 + 1);
        if (visits > kVisitLimit) throw ValueError("Simulation too large; lower preview resolution");
        reach[k] = {int(x0), int(x1), int(y0), int(y1)};
    }

    std::vector<double> grid(size_t(n) * n, 0.0);
    const double slope = std::tan((angle / 2) * kDegreesToRadians);
    const auto fill = [&](std::size_t first, std::size_t last) {
        for (size_t k = 0; k < count; ++k) {
            const Reach r = reach[k];
            const int j0 = std::max(r.y0, int(first)), j1 = std::min(r.y1, int(last) - 1);
            if (r.x1 < r.x0 || j1 < j0) continue;
            const double *move = moves.data() + k * 6;
            const double ax = move[0], ay = move[1], bx = move[2], by = move[3], z = move[4], start = move[5];
            const double vx = bx - ax, vy = by - ay, length_sq = vx * vx + vy * vy;
            for (int j = j0; j <= j1; ++j) {
                for (int i = r.x0; i <= r.x1; ++i) {
                    const double x = i * dx, y = j * dy;
                    const double t =
                        length_sq ? py_max(0.0, py_min(1.0, ((x - ax) * vx + (y - ay) * vy) / length_sq)) : 0.0;
                    const double distance = pymath::hypot(x - ax - t * vx, y - ay - t * vy);
                    if (distance > radius) continue;
                    double sag = 0;
                    if (shape == 1) sag = radius - std::sqrt(py_max(0.0, radius * radius - distance * distance));
                    else if (shape >= 2) sag = distance / slope;
                    // Visual approximation uses the depth at the closest XY point on the move.
                    const double contact = length_sq ? start + t * (z - start) : py_min(start, z);
                    double &cell = grid[size_t(j) * n + i];
                    cell = first_min(cell, contact + sag);
                }
            }
        }
    };
    // A thread for each half million cell visits, about 2 ms of work.
    const unsigned threads = parallel::threads_for(visits, 500000.0, std::size_t(n));
    try {
        parallel::for_ranges(std::size_t(n), threads, fill);
    } catch (const std::system_error &) {
        // A thread could not be started. Taking the minima again changes nothing, so one thread redoes every row.
        fill(0, std::size_t(n));
    }

    json::Array heights(static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) {
        json::Array row(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) row[i] = json(grid[size_t(j) * n + i]);
        heights[j] = json(std::move(row));
    }
    return json{
        {"heights", json(std::move(heights))},
        {"x", origin_x},
        {"y", origin_y},
        {"width", width},
        {"height", height},
        {"stock_thickness", stock},
        {"resolution", n},
        {"warnings", json::array({"Sampled stock preview: features smaller than a grid cell may disappear. Variable-depth "
                                  "cutter contact is approximate.",
                                  "No holder, fixture, machine acceleration, deflection or physical-clearance "
                                  "validation is performed."})},
    };
}

bool stock_command(const std::string &command) { return command == "stock"; }

Json stock_call(const std::string &, const Json &request) { return simulate_stock(request); }

}  // namespace kerfkite
