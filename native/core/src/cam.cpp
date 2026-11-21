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

namespace kerfkite {

using json = nlohmann::json;

namespace {

// project.get(key): null when the key is absent.
const json *get(const json &object, const char *key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

// Python's None: an absent key read with .get(), or a JSON null.
bool none(const json *value) { return !value || value->is_null(); }

bool is_text(const json *value, const char *text) {
    return value && value->is_string() && value->get_ref<const std::string &>() == text;
}

// project.get(key, text) == text: an absent key takes the default; a null does not.
bool is_text_or_absent(const json *value, const char *text) { return !value || is_text(value, text); }

// Python's float(str) for ASCII text. False when Python raises ValueError.
bool float_from_text(const std::string &raw, double &out) {
    for (const unsigned char c : raw)
        if (c >= 0x80) throw NotPorted("numbers written in non-ASCII text");
    const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f); };
    size_t begin = 0, end = raw.size();
    while (begin < end && space(raw[begin])) ++begin;
    while (end > begin && space(raw[end - 1])) --end;
    const std::string text = raw.substr(begin, end - begin);
    size_t i = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) ++i;
    std::string word;
    for (size_t k = i; k < text.size(); ++k) word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[k]))));
    if (word == "inf" || word == "infinity") {
        out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    if (word == "nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    std::string clean = text.substr(0, i);
    // digit (["_"] digit)*
    const auto digits = [&](bool required) {
        size_t count = 0;
        while (i < text.size()) {
            if (text[i] >= '0' && text[i] <= '9') {
                clean.push_back(text[i++]);
                ++count;
            } else if (text[i] == '_' && count && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
                ++i;
            } else {
                break;
            }
        }
        return count > 0 || !required;
    };
    const size_t whole_from = clean.size();
    digits(false);
    const bool whole = clean.size() > whole_from;
    bool fraction = false;
    if (i < text.size() && text[i] == '.') {
        clean.push_back(text[i++]);
        const size_t fraction_from = clean.size();
        digits(false);
        fraction = clean.size() > fraction_from;
    }
    if (!whole && !fraction) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        clean.push_back(text[i++]);
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) clean.push_back(text[i++]);
        if (!digits(true)) return false;
    }
    if (i != text.size()) return false;
    out = std::strtod(clean.c_str(), nullptr);
    return true;
}

// A plain JSON number that Python reads as an exact float or int (never a bool). False for anything else.
bool plain_number(const json &value, double &out) {
    if (value.is_number_unsigned()) {
        out = static_cast<double>(value.get<uint64_t>());
    } else if (value.is_number_integer()) {
        out = static_cast<double>(value.get<int64_t>());
    } else if (value.is_number_float()) {
        out = value.get<double>();
        // A literal beyond the double range reads as infinity here, but may be a Python int there.
        if (!std::isfinite(out)) throw NotPorted("numbers outside the double range");
    } else {
        return false;
    }
    return true;
}

// studio.cam.number(value, name, low, high). low_text and high_text are the bounds as the reference's
// message writes them.
double number(const json *value, const std::string &name, double low, double high, const std::string &low_text,
              const std::string &high_text) {
    double result = 0;
    const bool readable = value && !value->is_boolean() &&
                          (plain_number(*value, result) ||
                           (value->is_string() && float_from_text(value->get_ref<const std::string &>(), result)));
    if (!readable) throw ValueError(name + " must be a number");
    if (!std::isfinite(result) || !(low <= result && result <= high))
        throw ValueError(name + " must be between " + low_text + " and " + high_text);
    return result;
}

// number(project.get(key, default), ...): the default is used only when the key is absent.
double setting(const json &project, const char *key, double fallback, const std::string &name, double low,
               double high, const std::string &low_text, const std::string &high_text) {
    const json *value = get(project, key);
    return value ? number(value, name, low, high, low_text, high_text) : fallback;
}

std::string fixed(double value, int decimals) {
    std::string out;
    if (!text::fixed(out, value, decimals)) throw std::logic_error("A validated number could not be written");
    return out;
}

double rounded(double value, int decimals) {
    double out;
    if (!text::rounded(value, decimals, out)) throw std::logic_error("A validated number could not be rounded");
    return out;
}

// Python's max(a, b) and min(a, b): the first argument unless the second is strictly greater or smaller.
double py_max(double a, double b) { return b > a ? b : a; }
double py_min(double a, double b) { return b < a ? b : a; }

using Path = std::vector<std::array<double, 2>>;

Job single(const json &project) {
    const json *mode_value = get(project, "mode");
    const bool router = is_text(mode_value, "router"), laser = is_text(mode_value, "laser"),
               knife = is_text(mode_value, "knife");
    if (!router && !laser && !knife) throw ValueError("Choose router, laser or knife");
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

    const bool entry_plunge = is_text_or_absent(get(project, "router_entry"), "plunge");
    const bool drilling = is_text(operation, "drill");
    if (router && !entry_plunge && drilling)
        throw ValueError("Use the drill peck setting for drilling; select plunge as the router entry");
    if (drilling) throw NotPorted("drilling");
    if (router && is_text(get(project, "tool_shape"), "drill"))
        throw ValueError("Choose a milling cutter for a contour or pocket; a drill bit is for drilling");

    // studio.operations.prepare: the line operation leaves the paths as they are.
    const bool line = is_text_or_absent(operation, "line");
    if (!line || knife) {
        if (!none(get(project, "path_depths")))
            throw ValueError("Per-vertex depth contours require the line operation");
        if (knife) {
            if (!line) throw ValueError("Knife uses contour lines, not pocket/fill operations");
            throw NotPorted("drag-knife paths");
        }
        throw NotPorted("offset, pocket and fill operations");
    }
    if (tabs_enabled) throw NotPorted("holding tabs");

    std::vector<std::vector<double>> depths;
    const json *depth_paths = get(project, "path_depths");
    const bool has_depths = !none(depth_paths);
    if (has_depths) {
        if (laser || !depth_paths->is_array() || depth_paths->size() != clean.size())
            throw ValueError("Depth paths must match router XY paths");
        depths.reserve(clean.size());
        for (size_t p = 0; p < clean.size(); ++p) {
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
    if (router && !entry_plunge) throw NotPorted("ramp and helix entry moves");
    if (laser) throw NotPorted("laser jobs");

    std::vector<double> levels;
    const double level_count = std::ceil(depth / step);
    for (double i = 0; i < level_count; ++i) levels.push_back(-py_min(depth, step * (i + 1)));
    count = 0;
    for (const Path &path : clean) count += path.size();
    if (count * levels.size() > 200000) throw ValueError("Job exceeds 200,000 cutting segments");

    Job job;
    job.mode = mode;
    std::vector<std::string> &lines = job.lines;
    lines = {"G21", "G90", "G17", "G94", "G54", "M5"};
    const std::string safe_line = "G0 Z" + fixed(safe, 3);
    lines.push_back(safe_line);
    lines.push_back("M3 S" + fixed(power, 0));
    lines.push_back("G4 P2");
    const double cut_feed = has_depths ? py_min(feed, plunge) : feed;
    const std::string feed_word = " F" + fixed(cut_feed, 1);
    const std::string plunge_word = " F" + fixed(plunge, 1);
    double length = 0;
    job.segments.reserve(count * levels.size());
    lines.reserve(lines.size() + count * levels.size() + 4 * clean.size() * levels.size() + 8);
    std::string text;
    for (const double z : levels) {
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
                job.segments.push_back({x, y, nx, ny, next_z, false, 0, start_z});
                length += std::sqrt(text::square(nx - x) + text::square(ny - y) + text::square(next_z - start_z));
                current_z = next_z;
                start_z = next_z;
                x = nx;
                y = ny;
            }
            lines.push_back(safe_line);
        }
    }
    lines.push_back("M5");

    const json *protocol = get(project, "protocol");
    const Protocol &profile = get_protocol(protocol);
    job.warnings = {profile.notes, "Review tool geometry, stock and clearance before running.",
                    "Time estimate excludes travel, plunge, acceleration and spindle startup."};
    if (profile.family == "rs274")
        job.warnings.push_back(
            "Before loading, clear controller scaling, rotation and local offsets. Set G54 Z zero for the current "
            "tool tip with tool-length compensation off. Exact-stop output may run slower on dense curves.");
    job.lines = postprocess(lines, protocol, mode, get(project, "dwell_units"));
    job.cut_length = rounded(length, 2);
    job.cut_minutes = rounded(length / cut_feed, 2);
    // [min x, min y, max x, max y] over every vertex.
    job.bounds = {clean[0][0][0], clean[0][0][1], clean[0][0][0], clean[0][0][1]};
    for (const Path &path : clean) {
        for (const std::array<double, 2> &point : path) {
            job.bounds[0] = py_min(job.bounds[0], point[0]);
            job.bounds[1] = py_min(job.bounds[1], point[1]);
            job.bounds[2] = py_max(job.bounds[2], point[0]);
            job.bounds[3] = py_max(job.bounds[3], point[1]);
        }
    }
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
    double length = 0, minutes = 0;
    job.bounds = jobs[0].bounds;
    for (const Job &part : jobs) {
        job.segments.insert(job.segments.end(), part.segments.begin(), part.segments.end());
        length += part.cut_length;
        minutes += part.cut_minutes;
        job.bounds = {py_min(job.bounds[0], part.bounds[0]), py_min(job.bounds[1], part.bounds[1]),
                      py_max(job.bounds[2], part.bounds[2]), py_max(job.bounds[3], part.bounds[3])};
        for (const std::string &warning : part.warnings)
            if (std::find(job.warnings.begin(), job.warnings.end(), warning) == job.warnings.end())
                job.warnings.push_back(warning);
    }
    job.cut_length = rounded(length, 2);
    job.cut_minutes = rounded(minutes, 2);
    job.mode = project.at("mode").get<std::string>();
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
        if (s.has_power) row[5] = s.power;
        segments.push_back(std::move(row));
    }
    return json{{"lines", job.lines},           {"segments", std::move(segments)}, {"cut_length", job.cut_length},
                {"cut_minutes", job.cut_minutes}, {"mode", job.mode},                {"bounds", job.bounds},
                {"warnings", job.warnings}};
}

}  // namespace kerfkite
