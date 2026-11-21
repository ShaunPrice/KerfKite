// Flat holding tabs on final cutter paths, at stock-relative Z: studio.cam._holding_tabs, statement for
// statement where order affects the result.
#include "tabs.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "kerfkite/py_math.hpp"

namespace kerfkite {
namespace detail {

namespace {

using Point = std::array<double, 2>;

double dist(const Point &a, const Point &b) { return pymath::dist(a[0], a[1], b[0], b[1]); }

}  // namespace

void holding_tabs(const json &project, std::vector<Contour> &paths, double &depth,
                  std::vector<std::vector<double>> &depths, std::vector<std::string> &warnings) {
    size_t source_vertices = 0;
    for (const Contour &path : paths) source_vertices += path.size();
    if (source_vertices > 100000) throw ValueError("Holding-tab source geometry exceeds 100000 vertices");
    const json &config = project.at("tabs");
    bool known = config.is_object();
    if (known)
        for (auto it = config.begin(); it != config.end(); ++it)
            known = known && (it.key() == "width" || it.key() == "height" || it.key() == "count" || it.key() == "positions");
    if (!known) throw ValueError("Tabs require width, height and either count or positions");
    const double tab_width = number(get(config, "width"), "Tab width", 0.01, 100, "0.01", "100");
    const double tab_height = number(get(config, "height"), "Tab height", 0.01, 50, "0.01", "50");
    const double thickness = number(get(project, "stock_thickness"), "Stock thickness for tabs", 0.01, 50, "0.01", "50");
    const double diameter = setting(project, "tool_diameter", 3.175, "Tab cutter diameter", 0.01, 100, "0.01", "100");
    if (tab_height >= thickness) throw ValueError("Tab height must be less than stock thickness");
    if (depth < thickness) throw ValueError("Holding tabs require a cut depth reaching the stock bottom");
    const json *positions = get(config, "positions");
    if (config.contains("count") && positions) throw ValueError("Choose tab count or explicit positions, not both");
    std::vector<double> fractions;
    if (positions) {
        if (!positions->is_array() || positions->size() < 1 || positions->size() > 100)
            throw ValueError("Provide 1..100 tab positions as contour fractions in [0,1)");
        for (const json &position : *positions)
            fractions.push_back(number(&position, "Tab position fraction", 0, 1, "0", "1"));
        std::stable_sort(fractions.begin(), fractions.end());
        if (fractions.back() == 1)
            throw ValueError("Tab positions must be less than 1; the contour seam is position 0");
    } else {
        const double count = number(get(config, "count"), "Tab count", 1, 100, "1", "100");
        if (count != std::floor(count)) throw ValueError("Tab count must be a whole number");
        for (double i = 0; i < count; ++i) fractions.push_back((i + 0.5) / count);
    }
    const double plateau = tab_width + diameter;
    const double raised_z = -(thickness - tab_height);
    std::vector<Contour> result;
    std::vector<std::vector<double>> heights;
    size_t total_vertices = 0;
    for (const Contour &original : paths) {
        Contour points{original[0]};
        for (size_t i = 1; i < original.size(); ++i)
            if (dist(points.back(), original[i]) > 1e-9) points.push_back(original[i]);
        if (points.size() < 4 || dist(points.front(), points.back()) > 1e-6)
            throw ValueError("Holding tabs require closed contours with at least three distinct vertices");
        points.back() = points.front();
        if (!line_is_simple(points) || line_shorter_than(points, 1e-6))
            throw ValueError("Holding tabs require simple, non-crossing contours");
        std::vector<double> distances{0.0};
        for (size_t i = 0; i + 1 < points.size(); ++i) distances.push_back(distances.back() + dist(points[i], points[i + 1]));
        const double length = distances.back();
        std::vector<double> centers;
        for (const double fraction : fractions) centers.push_back(fraction * length);
        for (size_t i = 0; i < centers.size(); ++i) {
            const double next = centers[(i + 1) % centers.size()];
            // Adding the reference's integer 0 changes no comparison.
            if ((i == centers.size() - 1 ? next + length : next) - centers[i] <= plateau + 1e-8)
                throw ValueError("Tabs overlap or cover the contour; reduce count/width or change positions");
        }
        std::vector<std::pair<double, double>> intervals;
        for (const double center : centers) {
            const double lo = center - plateau / 2, hi = center + plateau / 2;
            if (lo < 0) {
                intervals.push_back({0.0, hi});
                intervals.push_back({length + lo, length});
            } else if (hi > length) {
                intervals.push_back({lo, length});
                intervals.push_back({0.0, hi - length});
            } else {
                intervals.push_back({lo, hi});
            }
        }
        // sorted(set(...)): the distinct values in rising order. The set keeps the first of equal values,
        // which for zero is the 0.0 that starts the distances.
        std::vector<double> cuts = distances;
        for (const std::pair<double, double> &interval : intervals) {
            cuts.push_back(interval.first);
            cuts.push_back(interval.second);
        }
        for (double &cut : cuts)
            if (cut == 0) cut = 0.0;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        // The reference indexes from the end of the list for a negative distance; none can arise from a
        // contour that passed the checks above, so one is declined, not followed.
        if (cuts.front() < 0) throw NotPorted("holding tabs that start before the contour seam");

        const auto at = [&](double distance) -> Point {
            if (distance >= length) return points[0];
            // bisect_right(distances, distance) - 1, limited to the last segment
            const size_t after = static_cast<size_t>(std::upper_bound(distances.begin(), distances.end(), distance) -
                                                     distances.begin());
            const size_t index = std::min(points.size() - 2, after - 1);
            const double span = distances[index + 1] - distances[index];
            if (span == 0) throw NotPorted("holding tabs on a contour with a segment of no length");
            const double ratio = (distance - distances[index]) / span;
            return {points[index][0] + (points[index + 1][0] - points[index][0]) * ratio,
                    points[index][1] + (points[index + 1][1] - points[index][1]) * ratio};
        };

        Contour path;
        std::vector<double> zs;
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            const double lo = cuts[i], hi = cuts[i + 1];
            if (hi - lo <= 1e-10) continue;
            const double midpoint = (lo + hi) / 2;
            bool raised = false;
            for (const std::pair<double, double> &interval : intervals)
                if (interval.first <= midpoint && midpoint <= interval.second) {
                    raised = true;
                    break;
                }
            const double z = raised ? raised_z : -depth;
            if (path.empty() || zs.back() != z) {
                path.push_back(at(lo));
                zs.push_back(z);
            }
            path.push_back(at(hi));
            zs.push_back(z);
        }
        if (zs.empty()) throw NotPorted("holding tabs on a contour with no measurable length");
        if (zs.back() != zs.front()) {
            path.push_back(points[0]);
            zs.push_back(zs.front());
        }
        total_vertices += path.size();
        if (total_vertices > 100000) throw ValueError("Holding-tab geometry exceeds 100000 vertices");
        result.push_back(std::move(path));
        heights.push_back(std::move(zs));
    }
    warnings.push_back("Holding tabs retain material above the configured stock bottom; verify stock thickness and "
                       "actual tool Z zero.");
    warnings.push_back("Tab plateau length includes one cutter diameter of padding. Nominal tab width follows the "
                       "contour; corners, nearby cuts and overlapping cutter footprints require inspection.");
    warnings.push_back("Tabs apply to every closed contour in this operation, including holes. Flat tab shoulders use "
                       "controlled vertical lifts and plunges; clamp stock and remove tabs after machining.");
    paths = std::move(result);
    depths = std::move(heights);
    // The generator then takes the deepest height as the job's depth: max(-min(path) for path in depths).
    bool first = true;
    for (const std::vector<double> &row : depths) {
        double lowest = row[0];
        for (size_t k = 1; k < row.size(); ++k) lowest = py_min(lowest, row[k]);
        depth = first ? -lowest : py_max(depth, -lowest);
        first = false;
    }
    if (depth <= 0) throw ValueError("No contour cuts below the stock surface");
}

}  // namespace detail
}  // namespace kerfkite
