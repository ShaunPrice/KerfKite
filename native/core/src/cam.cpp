// Centreline G-code generation: studio/cam.py, statement for statement where order affects the result.
//
// Validation runs in the reference's order so that an invalid project fails with the reference's message.
// NotPorted is thrown where the reference would enter a branch this file does not yet reproduce; nothing
// before that point is skipped, so a project that the reference rejects earlier is still rejected here.
#include "kerfkite/cam.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

#include "kerfkite/number_text.hpp"
#include "kerfkite/py_math.hpp"
#include "drilling.hpp"
#include "knife.hpp"
#include "laser.hpp"
#include "operations.hpp"
#include "project.hpp"
#include "router_entry.hpp"
#include "tabs.hpp"

namespace kerfkite {

using json = Json;

using namespace detail;

namespace {

using Path = std::vector<std::array<double, 2>>;

// A broad nib drawn with a fine pen: every stroke is drawn again and again, side by side along the nib's edge,
// so that a stroke across the edge is thick and one along it is thin, as a calligraphy pen writes. pen_nib is
// the nib's width (0 or absent: none), pen_nib_angle the angle of its edge and pen_tip the width of the pen's
// own line, which sets how many strokes fill the nib.
void pen_nib(const json &project, std::vector<Path> &paths, std::vector<std::vector<double>> *depths, double width, double height) {
    const double nib = setting(project, "pen_nib", 0, "Nib width", 0, 20, "0", "20");
    if (nib <= 0) return;
    const double tip = setting(project, "pen_tip", 0.4, "Pen line width", 0.1, 5, "0.1", "5");
    const double angle = setting(project, "pen_nib_angle", 45, "Nib angle", -360, 360, "-360", "360");
    double cosine, sine;
    cos_sin_degrees(angle, cosine, sine);
    const int copies = std::max(2, static_cast<int>(std::ceil(nib / tip - 1e-9)) + 1);
    std::vector<Path> out;
    std::vector<std::vector<double>> out_depths;
    size_t count = 0;
    for (size_t p = 0; p < paths.size(); ++p)
        for (int k = 0; k < copies; ++k) {
            const double along = -nib / 2 + nib * k / (copies - 1);
            Path copy = paths[p];
            for (std::array<double, 2> &point : copy) {
                point[0] += along * cosine;
                point[1] += along * sine;
                if (point[0] < 0 || point[0] > width || point[1] < 0 || point[1] > height)
                    throw ValueError("The broad nib takes the drawing outside the work area: move it in by half the nib's width");
            }
            std::vector<double> row;
            if (depths) row = (*depths)[p];
            // There and back: every other stroke is drawn the other way, to save travel.
            if (k % 2) {
                std::reverse(copy.begin(), copy.end());
                std::reverse(row.begin(), row.end());
            }
            count += copy.size();
            if (count > 100000) throw ValueError("Too many vertices; use a narrower nib, a wider pen line or a simpler design");
            out.push_back(std::move(copy));
            if (depths) out_depths.push_back(std::move(row));
        }
    paths = std::move(out);
    if (depths) *depths = std::move(out_depths);
}

Job single(const json &project) {
    const json *mode_value = get(project, "mode");
    const bool router = is_text(mode_value, "router"), laser = is_text(mode_value, "laser"),
               knife = is_text(mode_value, "knife"), pen = is_text(mode_value, "pen");
    if (!router && !laser && !knife && !pen) throw ValueError("Choose router, laser, knife or pen");
    const std::string mode = mode_value->get<std::string>();
    const json *operation = get(project, "operation");
    const json *tabs = get(project, "tabs");
    const bool tabs_enabled = tabs && !tabs->is_null() && !(tabs->is_boolean() && !tabs->get<bool>());
    if (tabs_enabled) {
        const bool contour = !operation || is_text(operation, "line") || is_text(operation, "inside") ||
                             is_text(operation, "outside");
        if (!router || !contour)
            throw ValueError("Holding tabs require router line, inside or outside contour operations");
        if (!none(get(project, "path_depths")) || !is_text_or_absent(get(project, "tool_shape"), "flat"))
            throw ValueError(
                "Holding tabs require a flat end mill and cannot combine with relief or per-vertex depth paths");
    }
    const double width = number(get(project, "width"), "Work width", 1, 2000, "1", "2000");
    const double height = number(get(project, "height"), "Work height", 1, 2000, "1", "2000");
    const double feed = number(get(project, "feed"), "Feed", 1, 10000, "1", "10000");
    const double power = number(get(project, "power"), "Power / RPM", 1, 100000, "1", "100000");
    const double safe = setting(project, "safe", 5, "Clearance Z", 0.1, 100, "0.1", "100");
    double depth = setting(project, "depth", 1, "Cut depth", 0.01, 50, "0.01", "50");
    const double step = setting(project, "step", 0.5, "Step down", 0.01, 10, "0.01", "10");
    const double plunge = setting(project, "plunge", 100, "Plunge feed", 1, 1000, "1", "1000");
    const double passes = setting(project, "passes", 1, "Passes", 1, 100, "1", "100");
    if (passes != std::floor(passes)) throw ValueError("Passes must be a whole number");

    const json *paths = get(project, "paths");
    if (!paths || !paths->is_array() || paths->size() < 1 || paths->size() > 3000)
        throw ValueError("Add between 1 and 3000 paths");
    std::vector<Path> clean;
    clean.reserve(paths->size());
    size_t count = 0;
    const std::string width_text = text::repr(width), height_text = text::repr(height);
    for (const json &path : *paths) {
        if (!path.is_array() || path.size() < 2) throw ValueError("Each path needs at least two points");
        Path points;
        points.reserve(path.size());
        for (const json &point : path) {
            if (!point.is_array() || point.size() != 2) throw ValueError("Invalid XY point");
            double x = 0, y = 0;
            const bool plain = plain_number(point[0], x) && plain_number(point[1], y) && 0 <= x && x <= width &&
                               0 <= y && y <= height;
            if (!plain) {
                x = number(&point[0], "X", 0, width, "0", width_text);
                y = number(&point[1], "Y", 0, height, "0", height_text);
            }
            points.push_back({x, y});
        }
        count += points.size();
        if (count > 100000) throw ValueError("Too many vertices; simplify the design");
        clean.push_back(std::move(points));
    }

    // A pen in place of the cutter or the laser. It is lifted by the Z axis (a spring-loaded holder pressed a
    // little below Z zero) or by a servo or solenoid on the spindle or laser output (M3 S for down, M5 or M3 S
    // for up, with a pause for the holder to move).
    const bool pen_servo = pen && is_text(get(project, "pen_lift"), "servo");
    if (pen && !pen_servo && !is_text_or_absent(get(project, "pen_lift"), "z")) throw ValueError("Pen lift must be z or servo");
    if (pen && !(is_text_or_absent(operation, "line") || is_text(operation, "fill")))
        throw ValueError("A pen draws lines or hatch fill, not cutting operations");
    const bool entry_plunge = is_text_or_absent(get(project, "router_entry"), "plunge");
    const bool drilling = is_text(operation, "drill");
    if (router && !entry_plunge && drilling)
        throw ValueError("Use the drill peck setting for drilling; select plunge as the router entry");
    if (drilling) return drill_job(project, clean);
    if (router && is_text(get(project, "tool_shape"), "drill"))
        throw ValueError("Choose a milling cutter for a contour or pocket; a drill bit is for drilling");

    // studio.operations.prepare: the line operation leaves the paths as they are.
    std::vector<std::string> operation_warnings;
    const bool line = is_text_or_absent(operation, "line");
    // A helix is placed in the pocket as drawn, so those contours are kept beside the pocket's passes.
    std::vector<Path> drawn;
    if (router && is_text(get(project, "router_entry"), "helix") && is_text(operation, "pocket")) drawn = clean;
    // The depth an operation gives to every point of its paths: V-carving does.
    std::vector<std::vector<double>> depths;
    if (!line || knife) {
        if (!none(get(project, "path_depths")))
            throw ValueError("Per-vertex depth contours require the line operation");
        if (knife) {
            if (!line) throw ValueError("Knife uses contour lines, not pocket/fill operations");
            clean = knife_paths(project, clean, width, height, operation_warnings);
        } else {
            operation_warnings = prepare_operation(project, clean, depths, width, height);  // operations.cpp
        }
    }

    const bool prepared_depths = !depths.empty();
    if (tabs_enabled) holding_tabs(project, clean, depth, depths, operation_warnings);
    const json *depth_paths = get(project, "path_depths");
    const bool has_depths = tabs_enabled || prepared_depths || !none(depth_paths);
    if (has_depths && !tabs_enabled) {  // tab heights are the core's own and need no checking
        // Nor do the depths of an operation: they are the core's own, between the depth of cut and zero.
        if (!prepared_depths) {
            if (laser || !depth_paths->is_array() || depth_paths->size() != clean.size())
                throw ValueError("Depth paths must match router XY paths");
            depths.reserve(clean.size());
        }
        for (size_t p = 0; !prepared_depths && p < clean.size(); ++p) {
            const json &values = (*depth_paths)[p];
            if (!values.is_array() || values.size() != clean[p].size())
                throw ValueError("Each vertex must have a matching depth");
            std::vector<double> row;
            row.reserve(values.size());
            for (const json &value : values) {
                double z = 0;
                if (!(plain_number(value, z) && -50 <= z && z <= 0)) z = number(&value, "Vertex Z", -50, 0, "-50", "0");
                row.push_back(z);
            }
            depths.push_back(std::move(row));
        }
        // max(-min(path) for path in depths)
        bool first = true;
        for (const std::vector<double> &row : depths) {
            double lowest = row[0];
            for (size_t k = 1; k < row.size(); ++k) lowest = py_min(lowest, row[k]);
            depth = first ? -lowest : py_max(depth, -lowest);
            first = false;
        }
        if (depth <= 0) throw ValueError("No contour cuts below the stock surface");
    }
    if (pen) {
        if (pen_servo && has_depths) throw ValueError("Drawing with depths at each point needs the Z axis to lift the pen");
        pen_nib(project, clean, has_depths ? &depths : nullptr, width, height);
    }
    if (router && !entry_plunge) {
        Job job = entry_job(project, drawn, clean, has_depths ? &depths : nullptr, depth, step, safe, feed, plunge,
                            power, width, height, operation_warnings);
        job.prepared = true;
        job.prepared_paths = std::move(clean);
        job.has_prepared_depths = has_depths;
        if (has_depths) job.prepared_depths = std::move(depths);
        return job;
    }

    std::vector<std::vector<LaserVertex>> laser_paths;
    bool leads = false;
    if (laser) {
        const double options[6] = {
            setting(project, "lead_in", 0, "Lead-in distance", 0, 100, "0", "100"),
            setting(project, "lead_out", 0, "Lead-out distance", 0, 100, "0", "100"),
            setting(project, "ramp_in", 0, "Start ramp distance", 0, 1000, "0", "1000"),
            setting(project, "ramp_out", 0, "End ramp distance", 0, 1000, "0", "1000"),
            setting(project, "start_power", 100, "Start power percent", 0, 100, "0", "100"),
            setting(project, "end_power", 100, "End power percent", 0, 100, "0", "100"),
        };
        leads = options[0] != 0 || options[1] != 0 || options[2] != 0 || options[3] != 0;
        const LaserCut cut = cut_options(project);
        const json *pixel_powers = get(project, "path_powers");
        const bool raster = !none(pixel_powers);
        if (raster) {
            if (!line || leads) throw ValueError("Raster powers require line operation with lead/ramp distances zero");
            if (!pixel_powers->is_array() || pixel_powers->size() != clean.size())
                throw ValueError("Pixel powers must match raster paths");
        }
        const std::string power_text = text::repr(power);
        size_t expanded = 0;
        for (size_t p = 0; p < clean.size(); ++p) {
            std::vector<LaserVertex> prepared;
            if (!raster) {
                prepared = prepare(clean[p], power, options, width, height, cut);
            } else {
                const json &powers = (*pixel_powers)[p];
                if (!powers.is_array() || powers.size() != clean[p].size())
                    throw ValueError("Pixel powers must match vertices");
                for (size_t v = 0; v < clean[p].size(); ++v)
                    prepared.push_back({clean[p][v][0], clean[p][v][1],
                                        number(&powers[v], "Pixel power", 0, power, "0", power_text), false});
            }
            expanded += prepared.size();
            if (expanded * static_cast<size_t>(passes) > 200000) throw ValueError("Laser job exceeds 200,000 segments");
            laser_paths.push_back(std::move(prepared));
        }
    }

    std::vector<double> levels;
    if (laser) {
        levels.assign(static_cast<size_t>(passes), 0.0);
    } else if (pen) {
        // A pen goes over its lines as often as asked, always at the one height.
        levels.assign(static_cast<size_t>(passes), -depth);
    } else {
        const double level_count = std::ceil(depth / step);
        for (double i = 0; i < level_count; ++i) levels.push_back(-py_min(depth, step * (i + 1)));
    }
    count = 0;
    for (const Path &path : clean) count += path.size();
    if (count * levels.size() > 200000) throw ValueError("Job exceeds 200,000 cutting segments");

    Job job;
    job.mode = mode;
    std::vector<std::string> &lines = job.lines;
    lines = {"G21", "G90", "G17", "G94", "G54", "M5"};
    const std::string safe_line = "G0 Z" + fixed(safe, 3);
    if (!laser && !pen_servo) {
        lines.push_back(safe_line);
    }
    if (router) {
        lines.push_back("M3 S" + fixed(power, 0));
        lines.push_back("G4 P2");
    }
    const double cut_feed = has_depths ? py_min(feed, plunge) : feed;
    const std::string feed_word = " F" + fixed(cut_feed, 1);
    const std::string plunge_word = " F" + fixed(plunge, 1);
    double length = 0;
    job.segments.reserve(count * levels.size());
    lines.reserve(lines.size() + count * levels.size() + 4 * clean.size() * levels.size() + 8);
    std::string text;
    for (size_t pass = 0; laser && pass < levels.size(); ++pass) {
        for (const std::vector<LaserVertex> &path : laser_paths) {
            double x = path[0].x, y = path[0].y;
            lines.push_back("M5");
            lines.push_back("G0 X" + fixed(x, 3) + " Y" + fixed(y, 3));
            lines.push_back("M4 S" + fixed(path[0].power, 0));
            for (size_t v = 1; v < path.size(); ++v) {
                const double nx = path[v].x, ny = path[v].y;
                text.assign("G1 X");
                text::fixed(text, nx, 3);
                text.append(" Y");
                text::fixed(text, ny, 3);
                text.append(feed_word);
                text.append(" S");
                text::fixed(text, path[v].power, 0);
                lines.push_back(text);
                Segment segment{x, y, nx, ny, 0, true, path[v].power, 0};
                segment.planar = true;
                segment.whole_power = path[v].whole_zero;
                segment.packed = true;
                job.segments.push_back(segment);
                // The reference adds the square of a zero height difference, which changes nothing.
                length += std::sqrt(text::square(nx - x) + text::square(ny - y));
                x = nx;
                y = ny;
            }
            lines.push_back("M5");
        }
    }
    if (pen_servo) {
        // The holder is worked from the spindle or laser output; Z is not moved at all.
        const Protocol &controller = get_protocol(get(project, "protocol"));
        const double pause = setting(project, "pen_dwell", 0.2, "Pen pause", 0, 5, "0", "5");
        const double up_value = setting(project, "pen_up_power", 0, "Pen-up S value", 0, 100000, "0", "100000");
        // G4 P is seconds for the Grbl family and for RS274 controllers, milliseconds for Marlin and for a
        // Mach3 or UCCNC set that way.
        const bool configured = controller.id == "mach3" || controller.id == "uccnc";
        if (configured && pause > 0 && !is_text(get(project, "dwell_units"), "seconds") && !is_text(get(project, "dwell_units"), "milliseconds"))
            throw ValueError("Select configured dwell units: seconds or milliseconds");
        const bool milliseconds = controller.family == "marlin" || (configured && is_text(get(project, "dwell_units"), "milliseconds"));
        const std::string wait = "G4 P" + (milliseconds ? fixed(pause * 1000, 0) : fixed(pause, 3));
        const std::string up = up_value > 0 ? "M3 S" + fixed(up_value, 0) : std::string("M5");
        const std::string down = "M3 S" + fixed(power, 0);
        const auto holder = [&](const std::string &command) {
            lines.push_back(command);
            if (pause > 0) lines.push_back(wait);
        };
        holder(up);
        for (size_t pass = 0; pass < levels.size(); ++pass)
            for (const Path &path : clean) {
                double x = path[0][0], y = path[0][1];
                lines.push_back("G0 X" + fixed(x, 3) + " Y" + fixed(y, 3));
                holder(down);
                for (size_t v = 1; v < path.size(); ++v) {
                    const double nx = path[v][0], ny = path[v][1];
                    text.assign("G1 X");
                    text::fixed(text, nx, 3);
                    text.append(" Y");
                    text::fixed(text, ny, 3);
                    text.append(feed_word);
                    lines.push_back(text);
                    Segment segment{x, y, nx, ny, 0, true, power, 0};
                    segment.planar = true;
                    segment.packed = true;
                    job.segments.push_back(segment);
                    length += std::sqrt(text::square(nx - x) + text::square(ny - y));
                    x = nx;
                    y = ny;
                }
                holder(up);
            }
    }
    for (const double z : levels) {
        if (laser || pen_servo) break;
        for (size_t p = 0; p < clean.size(); ++p) {
            const Path &path = clean[p];
            const std::vector<double> *row = has_depths ? &depths[p] : nullptr;
            double x = path[0][0], y = path[0][1];
            double current_z = row ? py_max(z, (*row)[0]) : z;
            lines.push_back(safe_line);
            lines.push_back("G0 X" + fixed(x, 3) + " Y" + fixed(y, 3));
            lines.push_back("G1 Z" + fixed(current_z, 3) + plunge_word);
            double start_z = current_z;
            for (size_t v = 1; v < path.size(); ++v) {
                const double nx = path[v][0], ny = path[v][1];
                const double next_z = row ? py_max(z, (*row)[v]) : current_z;
                text.assign("G1 X");
                text::fixed(text, nx, 3);
                text.append(" Y");
                text::fixed(text, ny, 3);
                text.append(feed_word);
                if (row) {
                    text.append(" Z");
                    text::fixed(text, next_z, 3);
                }
                lines.push_back(text);
                Segment segment{x, y, nx, ny, next_z, false, 0, start_z};
                segment.packed = true;
                job.segments.push_back(segment);
                length += std::sqrt(text::square(nx - x) + text::square(ny - y) + text::square(next_z - start_z));
                current_z = next_z;
                start_z = next_z;
                x = nx;
                y = ny;
            }
            lines.push_back(safe_line);
        }
    }
    // The output goes off at the end, except where a pen holder is held up by a value of its own.
    if (!(pen_servo && setting(project, "pen_up_power", 0, "Pen-up S value", 0, 100000, "0", "100000") > 0)) lines.push_back("M5");

    const json *protocol = get(project, "protocol");
    const Protocol &profile = get_protocol(protocol);
    job.warnings = {profile.notes, "Review tool geometry, stock and clearance before running.",
                    "Time estimate excludes travel, plunge, acceleration and spindle startup."};
    job.warnings.insert(job.warnings.begin() + 1, operation_warnings.begin(), operation_warnings.end());
    if (profile.family == "rs274")
        job.warnings.push_back(
            "Before loading, clear controller scaling, rotation and local offsets. Set G54 Z zero for the current "
            "tool tip with tool-length compensation off. Exact-stop output may run slower on dense curves.");
    if (laser && leads)
        job.warnings.push_back("Tangent leads are cutting moves and may mark outside the design. Power ramps use steps "
                               "of at most 0.5 mm; M4 also scales power with speed.");
    if (pen) {
        job.warnings.push_back(pen_servo ? "The pen is lifted by the spindle or laser output and Z is not moved: set the holder's height by hand, "
                                           "and check which S values lift and lower it before putting a pen in."
                                         : "The spindle stays off. Z zero is where the pen touches the paper; the depth is how far the "
                                           "holder's spring is pressed.");
        job.warnings.push_back("Take the cutter or the laser off before drawing, and test on scrap paper first.");
    }
    // A pen is driven as a router is: any controller that can run a router job can draw.
    job.lines = postprocess(std::move(lines), protocol, pen ? "router" : mode, get(project, "dwell_units"));
    job.cut_length = rounded(length, 2);
    job.cut_minutes = rounded(length / cut_feed, 2);
    // [min x, min y, max x, max y] over every vertex of what is cut: for a laser, the paths with their leads.
    const auto cover = [&job](double x, double y, bool first) {
        if (first) job.bounds = {x, y, x, y};
        job.bounds = {py_min(job.bounds[0], x), py_min(job.bounds[1], y), py_max(job.bounds[2], x),
                      py_max(job.bounds[3], y)};
    };
    bool first = true;
    if (laser) {
        for (const std::vector<LaserVertex> &path : laser_paths)
            for (const LaserVertex &vertex : path) {
                cover(vertex.x, vertex.y, first);
                first = false;
            }
    } else {
        for (const Path &path : clean)
            for (const std::array<double, 2> &point : path) {
                cover(point[0], point[1], first);
                first = false;
            }
    }
    job.prepared = true;
    job.prepared_paths = std::move(clean);
    job.has_prepared_depths = has_depths;
    if (has_depths) job.prepared_depths = std::move(depths);
    return job;
}

// Python's == between two JSON values: numbers compare by value whatever their kind, and True equals 1.
bool py_equal(const json &a, const json &b) {
    const auto numeric = [](const json &v) { return v.is_number() || v.is_boolean(); };
    if (numeric(a) && numeric(b)) {
        if ((a.is_number_integer() || a.is_boolean()) && (b.is_number_integer() || b.is_boolean())) {
            const auto whole = [](const json &v) -> std::pair<bool, uint64_t> {  // (negative, magnitude)
                if (v.is_boolean()) return {false, v.get<bool>() ? 1u : 0u};
                if (v.is_number_unsigned()) return {false, v.get<uint64_t>()};
                const int64_t n = v.get<int64_t>();
                return {n < 0, n < 0 ? 0 - static_cast<uint64_t>(n) : static_cast<uint64_t>(n)};
            };
            return whole(a) == whole(b);
        }
        // An int and a float: no project setting is large enough for the conversion to lose digits.
        const auto real = [](const json &v) { return v.is_boolean() ? (v.get<bool>() ? 1.0 : 0.0) : v.get<double>(); };
        return real(a) == real(b);
    }
    if (a.type() != b.type()) return false;
    if (a.is_array()) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!py_equal(a[i], b[i])) return false;
        return true;
    }
    if (a.is_object()) {
        if (a.size() != b.size()) return false;
        for (auto it = a.begin(); it != a.end(); ++it) {
            const auto other = b.find(it.key());
            if (other == b.end() || !py_equal(it.value(), *other)) return false;
        }
        return true;
    }
    return a == b;  // text and null
}

// Equality of two .get() results, either of which may be an absent key (Python's None).
bool py_equal(const json *a, const json *b) {
    static const json null_value;
    return py_equal(a ? *a : null_value, b ? *b : null_value);
}

// An ordered job: each operation is generated with the project's shared settings and the programs joined.
Job ordered(const json &project) {
    const json &operations = project.at("operations");
    if (!operations.is_array() || operations.size() < 1 || operations.size() > 100)
        throw ValueError("Use 1\u2013100 ordered operations");
    json common = json::object();
    for (auto it = project.begin(); it != project.end(); ++it) {
        const std::string &key = it.key();
        if (key != "operations" && key != "paths" && key != "path_depths" && key != "path_powers") common[key] = it.value();
    }
    const bool router = is_text(get(project, "mode"), "router");
    std::vector<Job> jobs;
    size_t combined_lines = 0;
    bool have_signature = false;
    json signature;
    for (const json &operation : operations) {
        if (!operation.is_object() || operation.contains("operations"))
            throw ValueError("Nested operations are unsupported");
        for (const char *key : {"mode", "protocol", "width", "height", "dwell_units"}) {
            const json *shared = get(project, key), *own = get(operation, key);
            if (!py_equal(own ? own : shared, shared))
                throw ValueError("An ordered job must use one head, protocol and work area");
        }
        json merged = common;
        for (auto it = operation.begin(); it != operation.end(); ++it) merged[it.key()] = it.value();
        if (router) {
            // studio.operations.router_tool_shape, then the angle that belongs to that shape.
            const json *kind = get(merged, "operation");
            json shape;
            if (is_text(kind, "vcarve")) shape = "v";
            else if (const json *chosen = get(merged, "tool_shape")) shape = *chosen;
            else shape = is_text(kind, "drill") ? "drill" : "flat";
            json angle;  // None
            if (shape == "v") angle = get(merged, "v_angle") ? *get(merged, "v_angle") : json(60);
            else if (shape == "drill") angle = get(merged, "drill_point_angle") ? *get(merged, "drill_point_angle") : json(118);
            json now = json::array({get(merged, "tool_id") ? *get(merged, "tool_id") : json(),
                                    get(merged, "tool_diameter") ? *get(merged, "tool_diameter") : json(3.175), shape, angle});
            if (have_signature && !py_equal(now, signature))
                throw ValueError("Tool identity or geometry differs between operations; export separate jobs and "
                                 "change tools explicitly");
            signature = std::move(now);
            have_signature = true;
        }
        jobs.push_back(single(merged));
        combined_lines += jobs.back().lines.size();
        if (combined_lines > 200000) throw ValueError("Combined job exceeds line limit");
    }
    Job job;
    for (const Job &part : jobs)
        for (const std::string &line : part.lines)
            if (line != "M30") job.lines.push_back(line);
    if (jobs.back().lines.back() == "M30") job.lines.push_back("M30");
    std::vector<double> lengths, minutes;
    job.bounds = jobs[0].bounds;
    for (const Job &part : jobs) {
        job.segments.insert(job.segments.end(), part.segments.begin(), part.segments.end());
        lengths.push_back(part.cut_length);
        minutes.push_back(part.cut_minutes);
        job.bounds = {py_min(job.bounds[0], part.bounds[0]), py_min(job.bounds[1], part.bounds[1]),
                      py_max(job.bounds[2], part.bounds[2]), py_max(job.bounds[3], part.bounds[3])};
        for (const std::string &warning : part.warnings)
            if (std::find(job.warnings.begin(), job.warnings.end(), warning) == job.warnings.end())
                job.warnings.push_back(warning);
    }
    // sum() of floats in the reference is a compensated sum, not a running total.
    job.cut_length = rounded(pymath::sum(lengths.begin(), lengths.end()), 2);
    job.cut_minutes = rounded(pymath::sum(minutes.begin(), minutes.end()), 2);
    job.mode = project.at("mode").get<std::string>();
    job.operations = jobs.size();
    for (const Job &part : jobs) job.drill_holes.insert(job.drill_holes.end(), part.drill_holes.begin(), part.drill_holes.end());
    job.drilled = !job.drill_holes.empty();
    // The reference keeps the first part's drill when every part has one equal to it.
    const auto same = [&jobs](const Job &part) {
        const SimulationTool &a = part.simulation_tool, &b = jobs[0].simulation_tool;
        return part.has_simulation_tool && a.diameter == b.diameter && a.shape == b.shape && a.angle == b.angle;
    };
    if (job.drilled && jobs[0].has_simulation_tool && std::all_of(jobs.begin(), jobs.end(), same)) {
        job.has_simulation_tool = true;
        job.simulation_tool = jobs[0].simulation_tool;
    }
    return job;
}

}  // namespace

Job generate(const json &project) {
    if (!project.is_object()) throw ValueError("Project must be an object");
    return project.contains("operations") ? ordered(project) : single(project);
}

json to_json(const Job &job) {
    json segments = json::array();
    for (const Segment &s : job.segments) {
        json row = json::array({s.x, s.y, s.nx, s.ny, s.end_z, nullptr, s.start_z});
        // A laser move has no height: the reference writes the integer 0 there, and inside a beam-off gap
        // an integer 0 for the power as well.
        if (s.planar) row[4] = row[6] = 0;
        if (s.whole_end) row[4] = 0;
        if (s.whole_start) row[6] = 0;
        if (s.has_power) row[5] = s.whole_power ? json(0) : json(s.power);
        segments.push_back(std::move(row));
    }
    json result{{"lines", job.lines},           {"segments", std::move(segments)}, {"cut_length", job.cut_length},
                {"cut_minutes", job.cut_minutes}, {"mode", job.mode},                {"bounds", job.bounds},
                {"warnings", job.warnings}};
    if (job.prepared) {
        json paths = json::array();
        for (const auto &path : job.prepared_paths) {
            json points = json::array();
            for (const auto &point : path) points.push_back(json::array({point[0], point[1]}));
            paths.push_back(std::move(points));
        }
        result["prepared_paths"] = std::move(paths);
        result["prepared_depths"] = job.has_prepared_depths ? json(job.prepared_depths) : json();
    }
    if (job.drilled) {
        json holes = json::array();
        for (const auto &hole : job.drill_holes) holes.push_back(json::array({hole[0], hole[1], hole[2]}));
        result["drill_holes"] = std::move(holes);
    }
    if (job.has_simulation_tool)
        result["simulation_tool"] = json{{"diameter", job.simulation_tool.diameter},
                                         {"shape", job.simulation_tool.shape},
                                         {"angle", job.simulation_tool.angle}};
    if (job.operations) result["operations"] = job.operations;
    return result;
}

}  // namespace kerfkite
