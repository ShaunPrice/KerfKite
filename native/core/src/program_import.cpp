// Foreign drawing and toolpath files as editable artwork: studio/program_import.py's import_program for
// posted G-code, HP-GL plotter files and LinuxCNC INI files, statement for statement.
//
// Reproduced to the bit: the payload's checks, the choice of reader by the file's suffix (pathlib's rules),
// Python's splitting into lines and statements, its regular expressions, float() and round(), the arcs
// flattened with the platform's maths library as Python calls it, the Drawing's limits and its grouping of
// long runs of contours, every message, and the warnings and presets with Python's '{:g}' format.
//
// DXF drawings go to the core's own reader (dxf.cpp), whose outlines agree with the reference's to the
// tolerance. Declined (NotPorted), for the Python to answer: program text that is not ASCII, which the
// reference upper-cases and splits by Unicode's rules.
#include "kerfkite/program_import.hpp"
#include "kerfkite/pdf_import.hpp"
#include "kerfkite/bundle_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/decimal.hpp"
#include "kerfkite/number_text.hpp"
#include "kerfkite/py_math.hpp"
#include "dxf_reader.hpp"
#include "imported_drawing.hpp"
#include "project.hpp"
#include "py_trig.hpp"
#include "python_text.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;
using namespace imported;

namespace {

const double kTau = 2 * pytrig::PI;  // math.tau

// ---- Python's text -----------------------------------------------------------------------------------

bool space(char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f); }  // str.isspace for ASCII
bool digit(char c) { return c >= '0' && c <= '9'; }

std::string strip(const std::string &text) {
    size_t begin = 0, end = text.size();
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string upper(std::string text) {
    for (char &c : text)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return text;
}

std::string lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// str.splitlines() for ASCII text.
std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\x0b' || c == '\x0c' || c == '\x1c' || c == '\x1d' || c == '\x1e') {
            lines.push_back(line);
            line.clear();
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::vector<std::string> split(const std::string &text, char separator) {
    std::vector<std::string> parts(1);
    for (const char c : text) {
        if (c == separator) parts.emplace_back();
        else parts.back().push_back(c);
    }
    return parts;
}

// The end of NUMBER = [-+]?(?:\d+(?:\.\d*)?|\.\d+) matched at i, or i when it does not match.
size_t number_end(const std::string &text, size_t i) {
    size_t k = i;
    if (k < text.size() && (text[k] == '-' || text[k] == '+')) ++k;
    if (k < text.size() && digit(text[k])) {
        while (k < text.size() && digit(text[k])) ++k;
        if (k < text.size() && text[k] == '.') {
            ++k;
            while (k < text.size() && digit(text[k])) ++k;
        }
        return k;
    }
    if (k + 1 < text.size() && text[k] == '.' && digit(text[k + 1])) {
        k += 2;
        while (k < text.size() && digit(text[k])) ++k;
        return k;
    }
    return i;
}

double read_float(const std::string &text) {
    double value = 0;
    float_from_text(text, value);
    return value;
}

// _arc(start, centre, sweep, radius): the arc flattened to the tolerance, its start left out.
std::vector<Point> arc(const Point &start, const Point &centre, double sweep, double radius) {
    if (!(0 < radius && radius <= 1e6) || !(0 < std::fabs(sweep) && std::fabs(sweep) <= kTau + 1e-9))
        throw ValueError("Arc radius or sweep outside supported range");
    const double ratio = kTolerance / (2 * radius);
    const double candidate = 4 * std::asin(std::sqrt(py_min(1.0, ratio)));
    const double delta = py_min(pytrig::PI / 8, candidate);
    const double steps = std::ceil(std::fabs(sweep) / delta);
    if (steps > static_cast<double>(kMaxPoints)) throw ValueError("Arc is too large to flatten");
    const long long count = static_cast<long long>(steps);
    const double angle = pytrig::atan2(start[1] - centre[1], start[0] - centre[0]);
    std::vector<Point> points;
    for (long long i = 1; i <= count; ++i) {
        const double turn = angle + sweep * static_cast<double>(i) / static_cast<double>(count);
        const double x = centre[0] + radius * pytrig::cosine(turn);
        points.push_back({x, centre[1] + radius * pytrig::sine(turn)});
    }
    return points;
}

double distance(const Point &a, const Point &b) { return pymath::dist(a[0], a[1], b[0], b[1]); }

// ---- HP-GL --------------------------------------------------------------------------------------------

// re.split(r'(?:\s*,\s*|\s+)', body).
std::vector<std::string> plotter_words(const std::string &body) {
    std::vector<std::string> words;
    std::string word;
    size_t i = 0;
    while (i < body.size()) {
        if (space(body[i]) || body[i] == ',') {
            size_t j = i;
            while (j < body.size() && space(body[j])) ++j;
            if (j < body.size() && body[j] == ',') {
                ++j;
                while (j < body.size() && space(body[j])) ++j;
                words.push_back(word);
                word.clear();
                i = j;
                continue;
            }
            if (j > i) {
                words.push_back(word);
                word.clear();
                i = j;
                continue;
            }
        }
        word.push_back(body[i++]);
    }
    words.push_back(word);
    return words;
}

// The distinct values of a set in the order first added, as Python's set keeps one of each.
void add_distinct(std::vector<double> &values, double value) {
    for (const double known : values)
        if (known == value) return;
    values.push_back(value);
}

std::vector<double> sorted_values(std::vector<double> values) {
    std::stable_sort(values.begin(), values.end(), [](double a, double b) { return a < b; });
    return values;
}

std::string joined(const std::vector<double> &values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) out += (i ? ", " : "") + general(values[i]);
    return out;
}

json hpgl(const std::string &text) {
    Drawing drawing;
    drawing.mode = "knife";
    bool absolute = true, down = false;
    long long pen = 1;
    Point position = {0.0, 0.0};
    std::vector<Point> stroke;
    const auto pen_name = [&](const char *what) { return "Pen " + std::to_string(pen) + " " + what; };
    const auto flush = [&]() {
        drawing.path(pen_name("stroke"), "Pen " + std::to_string(pen), stroke);
        stroke.clear();
    };
    const auto move = [&](const Point &point) {
        if (down && pen > 0 && position != point) {
            if (stroke.empty()) stroke.push_back(position);
            stroke.push_back(point);
            if (drawing.points + stroke.size() > kMaxPoints) throw ValueError("Simplify HP-GL pen strokes");
        }
        position = point;
    };
    std::vector<std::pair<long long, std::vector<double>>> velocities, forces;
    const std::vector<std::string> statements = split(text, ';');
    for (size_t idx = 1; idx <= statements.size(); ++idx) {
        const std::string statement = upper(strip(statements[idx - 1]));
        if (statement.empty()) continue;
        const std::string op = statement.substr(0, 2);
        const std::string body = strip(statement.size() > 2 ? statement.substr(2) : std::string());
        const std::vector<std::string> tokens = body.empty() ? std::vector<std::string>() : plotter_words(body);
        std::vector<double> values;
        for (const std::string &word : tokens) {
            if (word.empty() || number_end(word, 0) != word.size() || std::fabs(read_float(word)) > 40000000)
                throw ValueError("Invalid plotter values at instruction " + std::to_string(idx));
        }
        for (const std::string &word : tokens) values.push_back(read_float(word));
        if ((op == "IN" || op == "DF") && values.empty()) {
            flush();
            if (op == "IN") {
                pen = 1;
                position = {0.0, 0.0};
            }
            absolute = true;
            down = false;
        } else if (op == "SP" && values.size() <= 1) {
            flush();
            const double chosen = values.empty() ? 0.0 : values[0];
            if (std::trunc(chosen) != chosen || !(0 <= chosen && chosen <= 255)) throw ValueError("Pen number must be between 0 and 255");
            pen = static_cast<long long>(chosen);
        } else if ((op == "PA" || op == "PR" || op == "PU" || op == "PD") && values.size() % 2 == 0) {
            if (op == "PA" || op == "PR") {
                absolute = op == "PA";
            } else if (op == "PU") {
                flush();
                down = false;
            } else {
                down = true;
            }
            for (size_t i = 0; i < values.size(); i += 2)
                move({values[i] / 40 + (absolute ? 0.0 : position[0]), values[i + 1] / 40 + (absolute ? 0.0 : position[1])});
        } else if ((op == "AA" || op == "AR") && (values.size() == 3 || values.size() == 4)) {
            const Point centre = {values[0] / 40 + (op == "AR" ? position[0] : 0.0), values[1] / 40 + (op == "AR" ? position[1] : 0.0)};
            for (const Point &point : arc(position, centre, values[2] * (pytrig::PI / 180.0), distance(position, centre))) move(point);
        } else if (op == "CI" && (values.size() == 1 || values.size() == 2) && values[0] > 0) {
            flush();
            const double radius = values[0] / 40;
            const Point first = {position[0] + radius, position[1]};
            std::vector<Point> circle = {first};
            for (const Point &point : arc(first, position, kTau, radius)) circle.push_back(point);
            circle.back() = first;
            if (pen > 0) drawing.path(pen_name("circle"), "Pen " + std::to_string(pen), circle);
        } else if ((op == "VS" || op == "FS") && (values.size() == 1 || values.size() == 2)) {
            const double selector = values.size() == 2 ? values[1] : static_cast<double>(pen);
            if (std::trunc(selector) != selector || !(0 <= selector && selector <= 255) || values[0] < 0)
                throw ValueError("Invalid plotter pen setting");
            const long long pen_id = static_cast<long long>(selector);
            auto &settings = op == "VS" ? velocities : forces;
            auto entry = std::find_if(settings.begin(), settings.end(), [&](const auto &item) { return item.first == pen_id; });
            if (entry == settings.end()) {
                settings.push_back({pen_id, {}});
                entry = settings.end() - 1;
            }
            add_distinct(entry->second, values[0]);
            if (entry->second.size() > 16) throw ValueError("Too many different settings for a single pen");
        } else {
            throw ValueError("Unsupported HP-GL instruction " + std::to_string(idx) + ": " + op + ". Export plain, unscaled pen paths.");
        }
    }
    flush();
    for (const auto &entry : velocities) {
        const std::vector<double> speeds = sorted_values(entry.second);
        for (const double speed : speeds) {
            const double feed = speed * 600;  // HP-GL VS uses centimetres / second
            if (1 <= feed && feed <= 10000) {
                json settings = json::object();
                settings["feed"] = feed;
                drawing.presets.push_back(json{{"name", "Pen " + std::to_string(entry.first) + " · " + general(speed) + " cm/s"}, {"settings", settings}});
            }
        }
        drawing.warnings.push_back("Pen " + std::to_string(entry.first) + " velocity (cm/s): " + joined(speeds));
    }
    for (const auto &entry : forces)
        drawing.warnings.push_back("Pen " + std::to_string(entry.first) + " force (source units): " + joined(sorted_values(entry.second)));
    drawing.warnings.push_back(
        "HP-GL uses 40 plotter units per mm. Pen travel is omitted. Confirm blade offset, drag depth and pressure on the target cutter.");
    return drawing.finish();
}

// ---- G-code -------------------------------------------------------------------------------------------

// _arc_centre(start, end, radius, clockwise).
Point arc_centre(const Point &start, const Point &end, double radius, bool clockwise) {
    const double chord = distance(start, end);
    if (chord == 0 || chord > 2 * std::fabs(radius) + 1e-8) throw ValueError("Ambiguous or impossible radius-format arc");
    const double offset = std::sqrt(py_max(0.0, text::square(radius) - text::square(chord) / 4));
    const double side = (clockwise ? -1.0 : 1.0) * (radius < 0 ? -1.0 : 1.0);
    const double dx = (end[0] - start[0]) / chord, dy = (end[1] - start[1]) / chord;
    return {(start[0] + end[0]) / 2 - side * offset * dy, (start[1] + end[1]) / 2 + side * offset * dx};
}

// re.sub(r'\([^()]*\)', '', text): each innermost parenthesis, once.
std::string without_comments(const std::string &text) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '(') {
            size_t j = i + 1;
            while (j < text.size() && text[j] != '(' && text[j] != ')') ++j;
            if (j < text.size() && text[j] == ')') {
                i = j + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

struct Word {
    size_t begin, end;
    char key;
    std::string number;
};

// re.compile(r'([A-Z])\s*(NUMBER)').finditer(text).
std::vector<Word> words_of(const std::string &text) {
    std::vector<Word> words;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] >= 'A' && text[i] <= 'Z') {
            size_t j = i + 1;
            while (j < text.size() && space(text[j])) ++j;
            const size_t end = number_end(text, j);
            if (end > j) {
                words.push_back({i, end, text[i], text.substr(j, end - j)});
                i = end;
                continue;
            }
        }
        ++i;
    }
    return words;
}

using Maybe = std::optional<double>;

json gcode(const std::string &text, double default_scale) {
    Drawing drawing;
    double scale = default_scale;
    bool absolute = true, absolute_centre = false;
    int motion = -1;  // None
    std::string tool = "Toolpath";
    std::optional<long long> selected;
    std::array<Maybe, 3> position;
    std::vector<Point> stroke;
    std::vector<Maybe> heights;
    std::vector<double> feeds, speeds;
    bool ended = false, assumed_start = false, used_fallback = false, explicit_units = false;
    const auto flush = [&]() {
        if (!stroke.empty()) {
            const bool known = std::all_of(heights.begin(), heights.end(), [](const Maybe &h) { return h.has_value(); });
            std::vector<double> depth;
            if (known)
                for (const Maybe &h : heights) depth.push_back(*h);
            drawing.path(tool, tool, stroke, known ? &depth : nullptr);
        }
        stroke.clear();
        heights.clear();
    };
    const std::vector<std::string> lines = split_lines(text);
    for (size_t index = 1; index <= lines.size(); ++index) {
        const auto fail = [&](const std::string &why) { return ValueError("G-code line " + std::to_string(index) + ": " + why); };
        std::string raw = upper(split(lines[index - 1], ';')[0]);
        raw = strip(without_comments(raw));
        if (raw.empty() || raw == "%") continue;
        if (ended) throw fail("instructions after program end");
        size_t cursor = 0;
        std::map<char, double> args;
        std::vector<double> gcodes, mcodes;
        for (const Word &word : words_of(raw)) {
            if (!strip(raw.substr(cursor, word.begin - cursor)).empty()) throw fail("unsupported expressions or syntax");
            cursor = word.end;
            const double number = read_float(word.number);
            if (!std::isfinite(number) || std::fabs(number) > 1e7) throw fail("word outside supported range");
            if (word.key == 'G') {
                gcodes.push_back(number);
            } else if (word.key == 'M') {
                mcodes.push_back(number);
            } else {
                if (std::string("NXYZIJRFSTP").find(word.key) == std::string::npos || args.count(word.key))
                    throw fail(std::string("unsupported or duplicate ") + word.key + " word");
                args[word.key] = number;
            }
        }
        if (!strip(raw.substr(cursor)).empty()) throw fail("unsupported expressions or syntax");
        static const std::vector<std::vector<double>> groups = {{0, 1, 2, 3, 80}, {20, 21}, {90, 91}, {90.1, 91.1}};
        for (const auto &group : groups) {
            size_t count = 0;
            for (const double code : gcodes) count += std::find(group.begin(), group.end(), code) != group.end();
            if (count > 1) throw fail("conflicting modal commands");
        }
        for (const double code : gcodes) {
            if (code == 0 || code == 1 || code == 2 || code == 3) {
                motion = static_cast<int>(code);
            } else if (code == 80) {
                motion = -1;
            } else if (code == 20 || code == 21) {
                scale = code == 20 ? 25.4 : 1.0;
                explicit_units = true;
            } else if (code == 90 || code == 91) {
                absolute = code == 90;
            } else if (code == 90.1 || code == 91.1) {
                absolute_centre = code == 90.1;
            } else if (!(code == 4 || code == 17 || code == 40 || code == 49 || code == 54 || code == 61 || code == 94)) {
                throw fail("unsupported G" + general(code) + "; export linear or XY-plane arc toolpaths");
            }
        }
        const auto has = [&](char key) { return args.count(key) != 0; };
        if (has('F')) {
            double feed = 0;
            if (!text::rounded(args['F'] * scale, 5, feed)) throw NotPorted("a feed beyond the exact range of round()");
            add_distinct(feeds, feed);
        }
        if (has('S')) add_distinct(speeds, args['S']);
        if (has('T')) {
            if (args['T'] < 0 || std::trunc(args['T']) != args['T']) throw fail("invalid tool number");
            selected = static_cast<long long>(args['T']);
        }
        for (const double m : mcodes) {
            if (m == 2 || m == 30) {
                ended = true;
            } else if (m == 6) {
                if (!selected) throw fail("tool change without tool number");
                flush();
                tool = "Tool " + std::to_string(*selected);
            } else if (!(m == 0 || m == 1 || m == 3 || m == 4 || m == 5 || m == 7 || m == 8 || m == 9)) {
                throw fail("unsupported M" + general(m));
            }
        }
        const bool dwell = std::find(gcodes.begin(), gcodes.end(), 4.0) != gcodes.end();
        if (has('P') && !dwell) throw fail("only dwell accepts P");
        const bool coordinates = has('X') || has('Y') || has('Z') || has('I') || has('J') || has('R');
        if (dwell) {
            if (coordinates) throw fail("unsupported coordinates on dwell");
            continue;
        }
        if (!coordinates) continue;
        if (motion < 0) throw fail("missing motion mode");
        if ((has('I') || has('J') || has('R')) && motion != 2 && motion != 3) throw fail("arc parameters require G2 or G3");
        if (!explicit_units) used_fallback = true;
        std::array<Maybe, 3> target = position;
        const char axes[3] = {'X', 'Y', 'Z'};
        for (int i = 0; i < 3; ++i) {
            if (!has(axes[i])) continue;
            if (!absolute && !position[static_cast<size_t>(i)]) throw fail("set an absolute starting position before incremental moves");
            const double value = args[axes[i]] * scale + (absolute ? 0.0 : *position[static_cast<size_t>(i)]);
            target[static_cast<size_t>(i)] = value;
            if (std::fabs(value) > 1e6) throw fail("coordinate outside the supported range");
        }
        const bool known = position[0] && position[1] && target[0] && target[1];
        if (motion == 0) {
            flush();
        } else if (!known) {
            if (motion == 2 || motion == 3) throw fail("arc has no established start position");
            assumed_start = true;
        } else {
            const Point start = {*position[0], *position[1]}, end = {*target[0], *target[1]};
            if (start != end || motion == 2 || motion == 3) {
                std::vector<Point> points;
                if (motion == 2 || motion == 3) {
                    Point centre{};
                    if (has('R') && (has('I') || has('J'))) throw fail("choose R or I/J for an arc");
                    if (has('R')) {
                        centre = arc_centre(start, end, args['R'] * scale, motion == 2);
                    } else if (has('I') || has('J')) {
                        if (absolute_centre && !(has('I') && has('J'))) throw fail("absolute arc centres require I and J");
                        const double i = has('I') ? args['I'] : 0.0, j = has('J') ? args['J'] : 0.0;
                        centre = {i * scale + (absolute_centre ? 0.0 : start[0]), j * scale + (absolute_centre ? 0.0 : start[1])};
                    } else {
                        throw fail("missing arc centre");
                    }
                    const double radius = distance(start, centre);
                    if (std::fabs(radius - distance(end, centre)) > py_max(0.005, radius * 0.0001)) throw fail("inconsistent arc endpoints and centre");
                    const double a0 = pytrig::atan2(start[1] - centre[1], start[0] - centre[0]);
                    const double a1 = pytrig::atan2(end[1] - centre[1], end[0] - centre[0]);
                    const double sign = motion == 2 ? -1.0 : 1.0;
                    double sweep = pytrig::modulo((a1 - a0) * sign, kTau);
                    if (sweep == 0) sweep = kTau;
                    points = arc(start, centre, sign * sweep, radius);
                    points.back() = end;
                } else {
                    points = {end};
                }
                if (stroke.empty()) {
                    stroke.push_back(start);
                    heights.push_back(position[2]);
                }
                if (drawing.points + stroke.size() + points.size() > kMaxPoints) throw fail("simplify paths to 100000 vertices");
                for (size_t i = 1; i <= points.size(); ++i) {
                    stroke.push_back(points[i - 1]);
                    if (!position[2] || !target[2])
                        heights.push_back(std::nullopt);
                    else
                        heights.push_back(*position[2] + (*target[2] - *position[2]) * static_cast<double>(i) / static_cast<double>(points.size()));
                }
            } else if (target[2] != position[2]) {
                // Start a new contour at the changed height instead of inventing a sloping segment or
                // attaching a drill plunge to unrelated artwork.
                flush();
            }
        }
        position = target;
    }
    flush();
    for (const Object &object : drawing.objects)
        if (object.has_depths) drawing.mode = "router";
    const std::vector<double> ordered = sorted_values(feeds);
    for (size_t i = 0; i < ordered.size() && i < 32; ++i) {
        const double feed = ordered[i];
        if (1 <= feed && feed <= 10000) {
            json settings = json::object();
            settings["feed"] = feed;
            drawing.presets.push_back(json{{"name", "Posted feed · " + general(feed) + " mm/min"}, {"settings", settings}});
        }
    }
    if (!feeds.empty())
        drawing.warnings.push_back("Source feed range " + general(ordered.front()) + "–" + general(ordered.back()) +
                                   " mm/min. Up to 32 feeds retained as presets.");
    if (!speeds.empty()) {
        const std::vector<double> range = sorted_values(speeds);
        drawing.warnings.push_back("Source S range " + general(range.front()) + "–" + general(range.back()) +
                                   ". Power/RPM requires a matching controller configuration.");
    }
    if (assumed_start) drawing.warnings.push_back("Initial feed with unknown XY start excluded; subsequent positioned geometry retained.");
    if (used_fallback) drawing.warnings.push_back("Motion before a units command interpreted at " + general(default_scale) + " mm per unit.");
    drawing.warnings.push_back(
        "Feed contours imported. Rapids, stationary plunges, dwell and output commands are omitted. Compatible cutting Z is stored as "
        "depth; other known Z as source metadata. Regenerate with reviewed CAM settings.");
    return drawing.finish();
}

// ---- LinuxCNC INI --------------------------------------------------------------------------------------

bool word_char(char c) { return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

// re.match(r'^\s*#\s*INCLUDE\b', line, re.I).
bool includes(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && space(line[i])) ++i;
    if (i >= line.size() || line[i] != '#') return false;
    ++i;
    while (i < line.size() && space(line[i])) ++i;
    if (upper(line.substr(i, 7)) != "INCLUDE") return false;
    i += 7;
    return i >= line.size() || !word_char(line[i]);
}

// entry.strip('[]').
std::string strip_brackets(const std::string &text) {
    size_t begin = 0, end = text.size();
    while (begin < end && (text[begin] == '[' || text[begin] == ']')) ++begin;
    while (end > begin && (text[end - 1] == '[' || text[end - 1] == ']')) --end;
    return text.substr(begin, end - begin);
}

// NUMBER + (?:E[+-]?\d+)? as a whole match.
bool ini_number(const std::string &text) {
    size_t end = number_end(text, 0);
    if (end == 0) return false;
    if (end < text.size() && text[end] == 'E') {
        size_t k = end + 1;
        if (k < text.size() && (text[k] == '+' || text[k] == '-')) ++k;
        const size_t digits_start = k;
        while (k < text.size() && digit(text[k])) ++k;
        if (k > digits_start) end = k;
    }
    return end == text.size();
}

json linuxcnc_ini(const std::string &text) {
    std::map<std::string, std::set<std::string>> known = {{"TRAJ", {"LINEAR_UNITS", "COORDINATES"}}};
    for (const char axis : std::string("XYZ")) known[std::string("AXIS_") + axis] = {"MIN_LIMIT", "MAX_LIMIT", "MAX_VELOCITY", "MAX_ACCELERATION"};
    std::map<std::pair<std::string, std::string>, std::string> values;
    std::string section;
    const std::vector<std::string> lines = split_lines(text);
    for (size_t index = 1; index <= lines.size(); ++index) {
        const std::string &line = lines[index - 1];
        if (includes(line)) throw ValueError("Supply a self-contained LinuxCNC INI with resolved axis limits");
        const size_t cut = line.find_first_of(";#");
        const std::string entry = strip(cut == std::string::npos ? line : line.substr(0, cut));
        if (!entry.empty() && entry[0] == '[') {
            section = upper(strip_brackets(entry));
            continue;
        }
        const size_t equals = entry.find('=');
        if (equals == std::string::npos || !known.count(section)) continue;
        const std::string key = upper(strip(entry.substr(0, equals))), value = upper(strip(entry.substr(equals + 1)));
        if (!known[section].count(key)) continue;
        if (values.count({section, key})) throw ValueError("Duplicate [" + section + "]" + key + " at line " + std::to_string(index));
        values[{section, key}] = value;
    }
    const auto unit = values.find({"TRAJ", "LINEAR_UNITS"});
    static const std::map<std::string, double> scales = {{"MM", 1.0}, {"METRIC", 1.0}, {"IN", 25.4}, {"INCH", 25.4}, {"IMPERIAL", 25.4}};
    if (unit == values.end() || !scales.count(unit->second)) throw ValueError("LinuxCNC INI needs explicit [TRAJ]LINEAR_UNITS in mm or inch");
    const double scale = scales.at(unit->second);
    const auto coords = values.find({"TRAJ", "COORDINATES"});
    if (coords != values.end() && (coords->second.find('X') == std::string::npos || coords->second.find('Y') == std::string::npos))
        throw ValueError("This workspace requires LinuxCNC X and Y motion axes");
    std::map<char, double> spans;
    std::vector<std::string> notes;
    for (const char axis : std::string("XYZ")) {
        const std::string name = std::string("AXIS_") + axis;
        std::map<std::string, double> fields;
        for (const std::string &key : known[name]) {  // sorted, as sorted(known[section])
            const auto found = values.find({name, key});
            if (found == values.end()) continue;
            if (!ini_number(found->second)) throw ValueError("[" + name + "]" + key + " needs an explicit numeric value");
            const double value = read_float(found->second) * scale;
            if (!std::isfinite(value) || std::fabs(value) > 1e7) throw ValueError("[" + name + "]" + key + " outside supported range");
            fields[key] = value;
        }
        if (axis == 'Z' && fields.empty()) continue;
        if (!fields.count("MIN_LIMIT") || !fields.count("MAX_LIMIT"))
            throw ValueError("Supply both [" + name + "]MIN_LIMIT and MAX_LIMIT");
        const double lo = fields["MIN_LIMIT"], hi = fields["MAX_LIMIT"], span = hi - lo;
        if (!(0 < span && span <= 2000) || ((axis == 'X' || axis == 'Y') && span < 1))
            throw ValueError(std::string("Invalid or unsupported ") + axis + " travel span");
        spans[axis] = span;
        notes.push_back(std::string(1, axis) + " limits " + general(lo) + " to " + general(hi) + " mm; travel " + general(span) + " mm.");
        const std::array<std::array<const char *, 3>, 2> rates = {{{"MAX_VELOCITY", "velocity", "mm/s"}, {"MAX_ACCELERATION", "acceleration", "mm/s²"}}};
        for (const auto &rate : rates) {
            if (!fields.count(rate[0])) continue;
            if (fields[rate[0]] <= 0) throw ValueError("[" + name + "]" + rate[0] + " must be positive");
            notes.push_back(std::string(1, axis) + " maximum " + rate[1] + " " + general(fields[rate[0]]) + " " + rate[2] + ".");
        }
    }
    notes.push_back(
        "The workspace setup stores XY travel. Axis limits, velocity and acceleration are source references. HAL, routing, joint "
        "settings, homing and external files are omitted.");
    json device = json::object();
    device["name"] = "LinuxCNC workspace";
    device["width"] = spans.at('X');
    device["height"] = spans.at('Y');
    json result = json::object();
    result["objects"] = json::array();
    result["layers"] = json::array();
    result["presets"] = json::array();
    result["devices"] = json::array({device});
    result["warnings"] = json(notes);
    result["mode"] = json();
    return result;
}

// ---- the request ---------------------------------------------------------------------------------------

// Path(name).suffix: the last component's text from its last dot, unless the dot begins or ends it.
std::string suffix_of(const std::string &name) {
    std::string path = name;
#if defined(_WIN32)
    for (char &c : path)
        if (c == '\\') c = '/';
    // A UNC or device path's anchor (\\server\share, \\?\...) is no part of the name: Python's to parse.
    if (path.compare(0, 2, "//") == 0) throw NotPorted("a UNC or device drawing filename");
    if (path.size() >= 2 && path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) path = path.substr(2);
#endif
    std::string last;
    for (const std::string &part : split(path, '/'))
        if (!part.empty() && part != ".") last = part;
    const size_t dot = last.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= last.size()) return "";
    return last.substr(dot);
}

json import_program(const json &payload) {
    if (!payload.is_object()) throw ValueError("Provide a drawing filename and its text");
    const json *given_text = get(payload, "text"), *given_name = get(payload, "name");
    if (!given_text || !given_text->is_string()) throw ValueError("Provide a text drawing up to 4 MB");
    const std::string &raw = given_text->get_ref<const std::string &>();
    const size_t length = code_points(raw).size();
    if (length < 1 || length > 4000000 || raw.find('\0') != std::string::npos) throw ValueError("Provide a text drawing up to 4 MB");
    if (!given_name || !given_name->is_string() || code_points(given_name->get_ref<const std::string &>()).size() > 1024)
        throw ValueError("Invalid drawing filename");
    static const std::map<std::string, double> units = {{"mm", 1.0}, {"in", 25.4}, {"inch", 25.4}, {"cm", 10.0}, {"m", 1000.0}, {"ft", 304.8}};
    const json *given_unit = get(payload, "unit");
    const std::string unit = !given_unit ? std::string("mm") : given_unit->is_string() ? given_unit->get<std::string>() : std::string("\x01");
    if (!units.count(unit)) throw ValueError("Choose millimetres, inches, centimetres, metres or feet");
    const std::string suffix = lower(suffix_of(given_name->get<std::string>()));
    // text.lstrip('﻿')
    std::string text = raw;
    while (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    const bool ascii = std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    const bool plotter = suffix == ".plt" || suffix == ".hpgl" || suffix == ".hpg";
    const bool program = suffix == ".gcode" || suffix == ".nc" || suffix == ".ngc" || suffix == ".cnc" || suffix == ".tap";
    if (suffix == ".dxf") return dxf_drawing(text, units.at(unit));
    if ((suffix == ".ini" || plotter || program) && !ascii) throw NotPorted("drawing text that is not ASCII");
    if (suffix == ".ini") return linuxcnc_ini(text);
    if (plotter) return hpgl(text);
    if (program) return gcode(text, units.at(unit));
    throw ValueError("Choose an ASCII DXF, G-code or HP-GL drawing");
}

}  // namespace

bool program_import_command(const std::string &command) {
    return command == "program-import" || command == "pdf-import" || command == "bundle-import";
}

Json program_import_call(const std::string &command, const Json &request) {
    if (command == "pdf-import") return import_pdf(request);
    if (command == "bundle-import") return inspect_bundle(request);
    return import_program(request);
}

}  // namespace kerfkite
