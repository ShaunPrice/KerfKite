// Descend along a closed contour, or down a helix inside a pocket, instead of plunging:
// studio/router_entry.py, statement for statement where order affects the result.
//
// The reference keeps the integer 0 as the first height of the first level, and the preview data tells it
// from 0.0, so the moves below carry that distinction with them.
//
// Where a helix goes is decided by pocket_entry.cpp, on the polygon kernel, which the reference calls too.
// Its points then come from the platform's sine and cosine, and the number of its chords from the
// platform's arc cosine, one call each as Python makes them: they are compared with the reference on the
// same machine, and the last bit may differ from one platform to another.
#include "router_entry.hpp"

#include <cmath>
#include <limits>

#include "kerfkite/py_math.hpp"
#include "operations.hpp"
#include "py_trig.hpp"

#if defined(_MSC_VER)
#define KERFKITE_NOINLINE __declspec(noinline)
#else
#define KERFKITE_NOINLINE __attribute__((noinline))
#endif

namespace kerfkite {
namespace detail {

namespace {

const double PI = 3.141592653589793;  // math.pi

// One library call, as Python makes it, kept out of line so that it is never evaluated at compile time.
KERFKITE_NOINLINE double tangent(double x) { return std::tan(x); }
KERFKITE_NOINLINE double arc_cosine(double x) { return std::acos(x); }

// studio.operations.value(project, key, default, lo, hi): cam.number with the key in the message.
double value(const json &project, const char *key, double fallback, double low, double high, const char *low_text,
             const char *high_text) {
    const json *found = get(project, key);
    if (!found) return fallback;
    double result = 0;
    const bool readable = !found->is_boolean() &&
                          (plain_number(*found, result) ||
                           (found->is_string() && float_from_text(found->get_ref<const std::string &>(), result)));
    if (!readable) throw ValueError(std::string(key) + " must be numeric");
    if (!std::isfinite(result) || !(low <= result && result <= high))
        throw ValueError(std::string(key) + " must be between " + low_text + " and " + high_text);
    return result;
}

// math.dist between two points in space.
double dist3(double ax, double ay, double az, double bx, double by, double bz) {
    double diffs[3] = {std::fabs(ax - bx), std::fabs(ay - by), std::fabs(az - bz)};
    double largest = 0.0;
    for (const double d : diffs)
        if (d > largest) largest = d;
    return pymath::vector_norm(3, diffs, largest);
}

}  // namespace

Job entry_job(const json &project, const std::vector<Contour> &source, const std::vector<Contour> &paths,
              const std::vector<std::vector<double>> *caps, double depth, double step, double safe, double feed,
              double plunge, double rpm, double width, double height, const std::vector<std::string> &warnings) {
    const json *strategy = get(project, "router_entry");
    const bool helix = is_text(strategy, "helix");
    if (!helix && !is_text(strategy, "ramp")) throw ValueError("Choose plunge, contour ramp or pocket helix");
    const json *operation = get(project, "operation");
    const bool pocket = is_text(operation, "pocket");
    const bool profile = !operation || is_text(operation, "line") || is_text(operation, "inside") ||
                         is_text(operation, "outside") || pocket;
    if (!is_text_or_absent(get(project, "tool_shape"), "flat") || !profile || !none(get(project, "path_depths")))
        throw ValueError(
            "Ramp and helix entries require flat end mills on closed profiles or pockets with one target depth");
    if (!none(get(project, "stock_thickness")) &&
        depth > value(project, "stock_thickness", 10, 0.01, 200, "0.01", "200") + 1e-8)
        throw ValueError("Entry depth exceeds the entered stock thickness");
    const double angle = value(project, "entry_angle", 3, 0.25, 15, "0.25", "15");
    const double slope = tangent(angle * (PI / 180.0));  // math.tan(math.radians(angle))
    const double bit_radius = value(project, "tool_diameter", 3.175, 0.01, 100, "0.01", "100") / 2;
    for (const Contour &path : paths)
        for (const std::array<double, 2> &point : path)
            if (point[0] - bit_radius < -1e-8 || point[1] - bit_radius < -1e-8 || point[0] + bit_radius > width + 1e-8 ||
                point[1] + bit_radius > height + 1e-8)
                throw ValueError("Router entry cutter footprint extends outside the work area");
    std::vector<std::vector<double>> lens;
    lens.reserve(paths.size());
    for (const Contour &path : paths) {
        if (path.size() < 4 || pymath::dist(path.front()[0], path.front()[1], path.back()[0], path.back()[1]) > 1e-6)
            throw ValueError("Contour ramp and helical entry require closed machining contours");
        if (!line_is_simple(path) || line_shorter_than(path, 1e-5))
            throw ValueError("Repair crossing or degenerate machining contours before using a ramp");
        std::vector<double> edges;
        edges.reserve(path.size() - 1);
        for (size_t i = 1; i < path.size(); ++i)
            edges.push_back(pymath::dist(path[i - 1][0], path[i - 1][1], path[i][0], path[i][1]));
        lens.push_back(std::move(edges));
    }
    std::vector<HelixEntry> helices;
    double radius = 0, chords = 0, turn_length = 0;
    if (helix) {
        if (!pocket || caps) throw ValueError("Helical entry requires a pocket with confirmed cutter clearance");
        radius = value(project, "entry_radius", 1, 0.05, 50, "0.05", "50");
        if (radius > bit_radius)
            throw ValueError("Helix radius cannot exceed the cutter radius; a larger helix leaves a central core");
        std::vector<std::array<double, 2>> seams;
        seams.reserve(paths.size());
        for (const Contour &path : paths) seams.push_back(path[0]);
        helices = pocket_entries(source, seams, bit_radius, radius);
        // Chords short enough to stay within 0.005 mm of the circle, and at least 36 to a turn.
        chords = py_max(36, std::ceil(PI / arc_cosine(py_max(-1, 1 - 0.005 / radius))));
        turn_length = 2 * radius * pytrig::sine(PI / chords) * chords;
    }

    Job job;
    job.mode = "router";
    const std::string safe_line = "G0 Z" + fixed(safe, 3);
    std::vector<std::string> lines = {"G21", "G90", "G17", "G94", "G54", "M5", safe_line, "M3 S" + fixed(rpm, 0), "G4 P2"};
    size_t line_count = lines.size();
    double length = 0.0, minutes = 0.0;
    const double infinity = std::numeric_limits<double>::infinity();
    job.bounds = {infinity, infinity, -infinity, -infinity};
    double at_x = 0, at_y = 0, at_z = 0;
    bool at_whole = false;  // the height where the tool stands is the reference's integer 0
    std::string text;
    const auto move = [&](double x, double y, double z, bool whole, double rate) {
        if (line_count >= 199996)
            throw ValueError("Entry exceeds the 200000-line limit; simplify the contour or adjust the depth and angle");
        if (x == at_x && y == at_y && z == at_z) return;
        text.assign("G1 X");
        text::fixed(text, x, 3);
        text.append(" Y");
        text::fixed(text, y, 3);
        text.append(" Z");
        text::fixed(text, z, 3);
        text.append(" F");
        text::fixed(text, rate, 1);
        lines.push_back(text);
        ++line_count;
        Segment segment{at_x, at_y, x, y, z, false, 0, at_z};
        segment.whole_end = whole;
        segment.whole_start = at_whole;
        job.segments.push_back(segment);
        const double d = dist3(at_x, at_y, at_z, x, y, z);
        length += d;
        minutes += d / rate;
        job.bounds = {py_min(py_min(job.bounds[0], at_x), x), py_min(py_min(job.bounds[1], at_y), y),
                      py_max(py_max(job.bounds[2], at_x), x), py_max(py_max(job.bounds[3], at_y), y)};
        at_x = x;
        at_y = y;
        at_z = z;
        at_whole = whole;
    };

    const double ramp_feed = py_min(feed, plunge);
    double previous = 0;
    bool previous_whole = true;  // the integer 0 until the first level is finished
    const double level_count = std::ceil(depth / step);
    for (double level = 1; level <= level_count; ++level) {
        const double bottom = -py_min(depth, level * step);
        const double drop = previous - bottom;
        if (drop < 1e-10) continue;
        for (size_t i = 0; i < paths.size(); ++i) {
            const Contour &path = paths[i];
            const std::vector<double> &lengths = lens[i];
            const double perimeter = pymath::sum(lengths.begin(), lengths.end());
            const std::vector<double> *limit = caps && !(*caps)[i].empty() ? &(*caps)[i] : nullptr;
            double x = path[0][0], y = path[0][1];
            double cx = 0, cy = 0, start_angle = 0;
            if (helix) {
                cx = helices[i].x;
                cy = helices[i].y;
                start_angle = helices[i].angle;
                x = cx + radius * pytrig::cosine(start_angle);
                y = cy + radius * pytrig::sine(start_angle);
            }
            lines.push_back(safe_line);
            lines.push_back("G0 X" + fixed(x, 3) + " Y" + fixed(y, 3));
            line_count += 2;
            at_x = x;
            at_y = y;
            at_z = safe;
            at_whole = false;
            if (limit && (*limit)[0] > previous) move(x, y, (*limit)[0], false, plunge);
            else move(x, y, previous, previous_whole, plunge);
            if (helix) {
                // Whole turns of the helix, each falling no more steeply than the entry angle, then a feed
                // at the level's depth from the circle to the contour.
                const double turns = py_max(1, std::ceil(drop / (slope * turn_length)));
                const double count = turns * chords;
                if (!(count <= 200000) || static_cast<double>(line_count) + count > 200000)
                    throw ValueError("Helix exceeds the path segment limit");
                for (double j = 1; j <= count; ++j) {
                    const double theta = start_angle + 2 * PI * std::fmod(j, chords) / chords;
                    move(cx + radius * pytrig::cosine(theta), cy + radius * pytrig::sine(theta), previous - drop * j / count,
                         false, ramp_feed);
                }
                move(path[0][0], path[0][1], bottom, false, feed);
            } else {
                // The reference divides by these; a zero would stop it with an error that is not a rejection.
                if (!(slope * perimeter > 0)) throw NotPorted("ramps along contours of no length");
                const double turns = py_max(1, std::ceil(drop / (slope * perimeter)));
                if (!(turns <= 200000) || line_count + static_cast<size_t>(turns) * (path.size() - 1) > 200000)
                    throw ValueError("Contour ramp exceeds the path segment limit");
                const double total = turns * perimeter;
                double along = 0;
                for (double turn = 0; turn < turns; ++turn)
                    for (size_t j = 1; j < path.size(); ++j) {
                        along += lengths[j - 1];
                        double z = py_max(bottom, previous - drop * along / total);
                        if (limit) z = py_max(z, (*limit)[j]);
                        move(path[j][0], path[j][1], z, false, ramp_feed);
                    }
            }
            // Finish a complete lap at this level, including raised tab plateaux.
            for (size_t j = 1; j < path.size(); ++j)
                move(path[j][0], path[j][1], limit ? py_max(bottom, (*limit)[j]) : bottom, false, limit ? ramp_feed : feed);
            lines.push_back(safe_line);
            ++line_count;
        }
        previous = bottom;
        previous_whole = false;
    }
    lines.push_back("M5");

    const json *protocol = get(project, "protocol");
    job.lines = postprocess(lines, protocol, "router", get(project, "dwell_units"));
    job.cut_length = rounded(length, 2);
    job.cut_minutes = rounded(minutes, 2);
    job.warnings.push_back(get_protocol(protocol).notes);
    job.warnings.insert(job.warnings.end(), warnings.begin(), warnings.end());
    job.warnings.push_back("Entry angle is a maximum. Entry and feed plunges use the configured plunge feed limit. "
                           "Inspect stock, fixtures and tool ramp capability.");
    return job;
}

}  // namespace detail
}  // namespace kerfkite
