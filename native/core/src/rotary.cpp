// Cylindrical 4-axis output: studio/rotary.py, statement for statement where order affects the result.
//
// The reference translates the planar job block by block. For a generated job it reads the cutting moves from
// the packed rows behind them, rounded as the planar program writes them (X, Y and Z to 3 decimals, F to 1,
// S to 0); here they are read back from those program lines, which is the same: Python's round(v, n) and the
// text f'{v:.nf}' read back are one double. The cut length uses CPython's hypot (pymath::vector_norm), the C
// library's asinh as CPython calls it, and libm pow for squares, as the reference does.
#include "kerfkite/rotary.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "kerfkite/number_text.hpp"
#include "kerfkite/py_math.hpp"
#include "project.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kPi = 3.141592653589793;  // math.pi

bool stream_post(const json *post) { return is_text(post, "grblhal") || is_text(post, "xpro-v5"); }

// _number(value, label, low, high): a JSON int or float, not a bool, finite and within the bounds. low_text and
// high_text are the bounds as the reference's message writes them (repr of a Python int or float).
double rotary_number(const json *value, const std::string &label, double low, double high, const std::string &low_text,
                     const std::string &high_text) {
    double result = 0;
    if (!value || value->is_boolean() || !plain_number(*value, result) || !std::isfinite(result) ||
        !(low <= result && result <= high))
        throw ValueError(label + " must be a finite number in " + low_text + ".." + high_text);
    return result;
}

// The same check for a value already read as a float.
double rotary_number(double value, const std::string &label, double low, double high, const std::string &low_text,
                     const std::string &high_text) {
    if (!std::isfinite(value) || !(low <= value && value <= high))
        throw ValueError(label + " must be a finite number in " + low_text + ".." + high_text);
    return value;
}

// math.hypot of finite values, as CPython computes it.
double hypot3(double a, double b, double c) {
    double values[3] = {std::fabs(a), std::fabs(b), std::fabs(c)};
    double most = 0.0;
    for (const double v : values)
        if (v > most) most = v;
    return pymath::vector_norm(3, values, most);
}

double hypot2(double a, double b) {
    double values[2] = {std::fabs(a), std::fabs(b)};
    double most = 0.0;
    for (const double v : values)
        if (v > most) most = v;
    return pymath::vector_norm(2, values, most);
}

// _cut_length(arc, axial, radial, radius, start_z, reference).
double cut_length(double arc, double axial, double radial, double radius, double start_z, bool surface) {
    if (surface || arc == 0) return hypot3(arc, axial, radial);
    const double angle = std::fabs(arc) / radius;
    const double first = radius + start_z, last = radius + start_z + radial;
    const double linear = hypot2(axial, radial);
    if (std::fabs(radial) < 1e-7 * py_max(first, last)) return hypot2(linear, angle * (first + last) / 2);
    // Integrate ds along a linear coordinated block with changing cutting radius.
    const auto primitive = [&](double r) {
        return (r * hypot2(linear, angle * r) + text::square(linear) / angle * std::asinh(angle * r / linear)) / 2;
    };
    return (primitive(last) - primitive(first)) / radial;
}

// Python's str.isspace() for ASCII.
bool space(char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= '\x1c' && c <= '\x1f'); }

// One block of the source: its first token, its words in order, and the stripped line.
struct Block {
    std::string command;
    std::vector<std::pair<char, double>> words;
    std::string raw;

    const double *word(char letter) const {
        for (const auto &w : words)
            if (w.first == letter) return &w.second;
        return nullptr;
    }
    // set(words) == letters.
    bool exactly(const char *letters) const {
        size_t count = 0;
        for (const char *l = letters; *l; ++l, ++count)
            if (!word(*l)) return false;
        return words.size() == count;
    }
};

// The reference's _WORD.fullmatch(token): a capital letter, then [+-]?(\d+(\.\d*)?|\.\d+).
bool word_token(const std::string &token) {
    if (token.size() < 2 || token[0] < 'A' || token[0] > 'Z') return false;
    size_t i = 1;
    if (token[i] == '+' || token[i] == '-') ++i;
    const auto digit = [&](size_t k) { return k < token.size() && token[k] >= '0' && token[k] <= '9'; };
    size_t whole = 0;
    while (digit(i)) ++i, ++whole;
    if (whole) {
        if (i < token.size() && token[i] == '.') {
            ++i;
            while (digit(i)) ++i;
        }
    } else {
        if (i >= token.size() || token[i] != '.') return false;
        ++i;
        size_t fraction = 0;
        while (digit(i)) ++i, ++fraction;
        if (!fraction) return false;
    }
    return i == token.size();
}

// _blocks(): each line split into tokens and checked as the reference checks a line of text.
Block parse(const std::string &item) {
    if (item.size() > 256) throw ValueError("Invalid or oversized source G-code line");
    for (const unsigned char c : item)
        if (c >= 0x80) throw NotPorted("G-code text that is not ASCII");
    size_t begin = 0, end = item.size();
    while (begin < end && space(item[begin])) ++begin;
    while (end > begin && space(item[end - 1])) --end;
    Block block;
    block.raw = item.substr(begin, end - begin);
    std::vector<std::string> tokens;
    for (size_t i = 0; i < block.raw.size();) {
        while (i < block.raw.size() && space(block.raw[i])) ++i;
        size_t j = i;
        while (j < block.raw.size() && !space(block.raw[j])) ++j;
        if (j > i) tokens.push_back(block.raw.substr(i, j - i));
        i = j;
    }
    if (tokens.empty()) throw ValueError("Blank source lines are unsupported");
    for (const std::string &token : tokens) {
        if (!word_token(token) || block.word(token[0]))
            throw ValueError("Rotary requires canonical generated G-code without comments or duplicate words");
        double value;
        if (!float_from_text(token.substr(1), value)) throw std::logic_error("A matched word could not be read");
        block.words.emplace_back(token[0], rotary_number(value, "Source word", -1e9, 1e9, "-1000000000.0",
                                                         "1000000000.0"));
    }
    block.command = tokens[0];
    return block;
}

bool one_of(const std::string &text, std::initializer_list<const char *> options) {
    for (const char *option : options)
        if (text == option) return true;
    return false;
}

// What _wrap_job keeps between blocks for one coordinate: None, or a value.
struct Coordinate {
    bool known = false;
    double value = 0;
};

// _wrap_job(job, options, generated=...). The job's lines and mode are given apart from the rest of it.
json wrap(const json &job, const json &options, bool generated) {
    if (!job.is_object() || !options.is_object()) throw ValueError("Rotary job and options must be objects");
    const json *post_value = get(options, "postprocessor"), *mode_value = get(job, "mode");
    const bool linuxcnc = is_text(post_value, "linuxcnc");
    if (!linuxcnc && !stream_post(post_value))
        throw ValueError("Select LinuxCNC, grblHAL or xPRO V5 independent-A rotary output");
    const std::string post = post_value->get<std::string>();
    const bool router = is_text(mode_value, "router"), laser = is_text(mode_value, "laser");
    if ((!router && !laser) || (linuxcnc && !router))
        throw ValueError("LinuxCNC supports router jobs only; rotary laser engraving requires grblHAL or xPRO V5");
    const json *wrapped_value = get(options, "wrap_axis");
    if (!is_text(wrapped_value, "X") && !is_text(wrapped_value, "Y"))
        throw ValueError("wrap_axis must explicitly be X or Y");
    const char wrapped = wrapped_value->get<std::string>()[0];
    const char axial = wrapped == 'X' ? 'Y' : 'X';
    const json *drive_value = get(options, "output_axis");
    const std::string drive = drive_value ? (drive_value->is_string() ? drive_value->get<std::string>() : "") : "A";
    if (!(drive_value == nullptr || drive_value->is_string()) || (drive != "A" && drive != "B" && drive != "C") ||
        (post == "xpro-v5" && drive != "A"))
        throw ValueError("Use A on xPRO V5, or A/B/C on grblHAL and LinuxCNC");
    // The conventional machine axis about which the stock spins, then the radial tool axis.
    const char along = drive == "A" ? 'X' : drive == "B" ? 'Y' : 'Z';
    const char radial = drive == "C" ? 'X' : 'Z';
    const double diameter = rotary_number(get(options, "diameter"), "Diameter", 1, 2000, "1", "2000");
    const double radius = diameter / 2, circumference = kPi * diameter;
    // _fixture(options, radius).
    bool fixture = false;
    double f_low = 0, f_high = 0, f_core = 0;
    if (options.contains("fixture")) {
        const json &f = options.at("fixture");
        if (!f.is_object()) throw ValueError("Supply an axial window and protected core radius for the fixture");
        f_low = rotary_number(get(f, "axial_min"), "Fixture axial minimum", -2000, 2000, "-2000", "2000");
        f_high = rotary_number(get(f, "axial_max"), "Fixture axial maximum", -2000, 2000, "-2000", "2000");
        f_core = rotary_number(get(f, "min_radius"), "Fixture protected core radius", 0, radius, "0", text::repr(radius));
        if (f_high <= f_low) throw ValueError("Fixture axial maximum must exceed its minimum");
        fixture = true;
    }
    const json zero(0), one(1);
    const json *offset_value = get(options, "angle_offset"), *direction_value = get(options, "direction");
    const double offset = rotary_number(offset_value ? offset_value : &zero, "Angle offset", -360000, 360000, "-360000",
                                        "360000");
    const double direction = rotary_number(direction_value ? direction_value : &one, "Direction", -1, 1, "-1", "1");
    if (direction != -1 && direction != 1) throw ValueError("Direction must be -1 or 1");
    const json *reference_value = get(options, "feed_reference");
    if (reference_value && !is_text(reference_value, "surface") && !is_text(reference_value, "cut"))
        throw ValueError("Choose surface or cut radius as the rotary feed reference");
    const std::string reference = reference_value ? reference_value->get<std::string>() : "surface";
    const json *source_value = get(job, "lines");
    if (!source_value || !source_value->is_array() || source_value->size() < 1 || source_value->size() > 200000)
        throw ValueError("Rotary source must contain 1..200000 G-code lines");
    std::vector<std::string> source;
    source.reserve(source_value->size());
    for (const json &line : *source_value) {
        if (!line.is_string()) {
            // A generated job holds text only; a given one is checked here as the reference checks it.
            throw ValueError("Invalid or oversized source G-code line");
        }
        source.push_back(line.get<std::string>());
    }
    if (!generated)
        for (const std::string &line : source) {
            for (const unsigned char c : line)
                if (c >= 0x80) throw NotPorted("G-code text that is not ASCII");
            if (line.size() > 256) throw ValueError("Invalid or oversized source G-code line");
        }
    if (source.back() == "M30") source.pop_back();
    if (source.empty()) throw ValueError("Rotary source must end with spindle off (M5)");
    {
        const std::string &last = source.back();
        for (const unsigned char c : last)
            if (c >= 0x80) throw NotPorted("G-code text that is not ASCII");
        size_t begin = 0, end = last.size();
        while (begin < end && space(last[begin])) ++begin;
        while (end > begin && space(last[end - 1])) --end;
        if (last.compare(begin, end - begin, "M5") != 0 || end - begin != 2)
            throw ValueError("Rotary source must end with spindle off (M5)");
    }
    std::map<char, Coordinate> state{{'X', {}}, {'Y', {}}, {'Z', {}}};
    if (laser) state['Z'] = {true, 0.0};  // Laser focus at the stock surface.
    std::vector<std::string> output;
    double min_angle = kInf, min_axial = kInf, min_z = kInf;
    double max_angle = -kInf, max_axial = -kInf, max_z = -kInf;
    bool units = false, absolute = false, feed_mode = false, inverse_active = false, emitting = false;
    bool have_feed = false;
    double last_feed = 0, minutes = 0.0;
    const std::string spindle = laser ? "M4" : "M3";
    const bool stream = stream_post(post_value);
    const bool surface = reference == "surface";
    for (const std::string &item : source) {
        Block block = parse(item);
        std::string command = block.command;
        if (one_of(command, {"G21", "G90", "G17", "G54", "G94", "M5", "G40", "G49", "G80", "G61"}) &&
            block.words.size() == 1) {
            if (stream && one_of(command, {"G40", "G49", "G80", "G61"}))
                throw ValueError("Source contains a LinuxCNC-only setup command");
            if (command == "G21") units = true;
            if (command == "G90") absolute = true;
            if (command == "M5") emitting = false;
            if (command == "G94") {
                feed_mode = true;
                inverse_active = post != "xpro-v5";
                command = inverse_active ? "G93" : "G94";
            }
            output.push_back(command);
            continue;
        }
        if (command == spindle && block.exactly("MS")) {
            rotary_number(*block.word('S'), "Spindle power", laser ? 0 : 0.001, 100000, laser ? "0" : "0.001", "100000");
            emitting = true;
            output.push_back(block.raw);
            continue;
        }
        if (router && command == "G4" && block.exactly("GP") && 0 <= *block.word('P') && *block.word('P') <= 60) {
            output.push_back(block.raw);
            continue;
        }
        if (command != "G0" && command != "G1") throw ValueError("Unsupported rotary source command");
        if (!(units && absolute && feed_mode)) throw ValueError("Rotary source must establish G21 G90 G94 before motion");
        const bool linear = command == "G1";
        if (laser) {
            if (linear) {
                if (!emitting) throw ValueError("Laser cutting requires M4");
                const double *s = block.word('S');
                if (!s) throw ValueError("Laser segment power must be a finite number in 0..100000");
                rotary_number(*s, "Laser segment power", 0, 100000, "0", "100000");
            } else if (emitting) {
                throw ValueError("Laser rapids require M5");
            }
        }
        // words.keys() <= the allowed words, and a coordinate among them.
        const std::string allowed = laser ? (linear ? "GXYFS" : "GXY") : (linear ? "GXYZF" : "GXYZ");
        bool fits = true, coordinate = false;
        for (const auto &w : block.words) {
            if (allowed.find(w.first) == std::string::npos) fits = false;
            if (w.first == 'X' || w.first == 'Y' || w.first == 'Z') coordinate = true;
        }
        if (!fits || !coordinate) throw ValueError("Unsupported words in rotary motion");
        // coords: the coordinates of the block, in the order X, Y, Z.
        std::vector<std::pair<char, double>> coords;
        for (const char k : {'X', 'Y', 'Z'})
            if (const double *v = block.word(k)) coords.emplace_back(k, *v);
        const auto coord = [&](char k) -> const double * {
            for (const auto &c : coords)
                if (c.first == k) return &c.second;
            return nullptr;
        };
        const Coordinate z = coord('Z') ? Coordinate{true, *coord('Z')} : state['Z'];
        if (z.known) {
            if (radius + z.value <= 0) throw ValueError("Cut depth reaches or crosses the cylinder center");
            if (fixture && radius + z.value < f_core - 1e-9)
                throw ValueError("Rotary toolpath enters the protected fixture core");
            if (z.value < min_z) min_z = z.value;
            if (z.value > max_z) max_z = z.value;
        }
        if (const double *x = coord(axial)) {
            if (fixture && !(f_low - 1e-9 <= *x && *x <= f_high + 1e-9))
                throw ValueError("Rotary toolpath exceeds the fixture axial window");
            if (*x < min_axial) min_axial = *x;
            if (*x > max_axial) max_axial = *x;
        }
        double length = 0;
        if (linear) {
            const double *f = block.word('F');
            if (!f) throw ValueError("Source linear feed must be a finite number in 0.001..100000");
            last_feed = rotary_number(*f, "Source linear feed", 0.001, 100000, "0.001", "100000");
            have_feed = true;
            for (const auto &c : coords)
                if (!state[c.first].known)
                    throw ValueError("Cutting move starts from an unknown position; establish rapid coordinates first");
            const double arc = coord(wrapped) ? *coord(wrapped) - state[wrapped].value : 0;
            const double travel = coord(axial) ? *coord(axial) - state[axial].value : 0;
            const double depth = coord('Z') ? *coord('Z') - state['Z'].value : 0;
            // state['Z'] or 0: None and zero both start from 0.
            const double from = state['Z'].known && state['Z'].value != 0 ? state['Z'].value : 0;
            length = cut_length(arc, travel, depth, radius, from, surface);
        }
        std::string transformed;
        const auto add = [&](const std::string &word) {
            if (!transformed.empty()) transformed.push_back(' ');
            transformed += word;
        };
        for (const auto &c : coords) {
            const double destination = c.second;
            if (c.first == wrapped) {
                const double turn = std::nearbyint(destination / circumference);
                const double arc =
                    std::fabs(destination - turn * circumference) <= 0.00051 ? turn * circumference : destination;
                const double angle = offset + direction * 360 * arc / circumference;
                if (std::fabs(angle) > 360000) throw ValueError("Rotary extent exceeds 1000 revolutions");
                if (angle < min_angle) min_angle = angle;
                if (angle > max_angle) max_angle = angle;
                add(drive + fixed(angle, 6));
            } else {
                add(std::string(1, c.first == axial ? along : radial) + fixed(destination, 6));
            }
            state[c.first] = {true, destination};
        }
        if (linear) {
            if (length <= 1e-12) continue;
            double inverse = last_feed / length;
            if (!(1e-9 <= inverse && inverse <= 1e9))
                inverse = rotary_number(inverse, "Inverse-time feed", 1e-9, 1e9, "1e-09", "1000000000.0");
            if (!inverse_active) {
                output.push_back("G93");
                inverse_active = true;
            }
            add("F" + fixed(inverse, 9));
            if (laser) add("S" + fixed(*block.word('S'), 0));
            minutes += length / last_feed;
        } else if (post == "xpro-v5" && inverse_active) {
            // Vendor GCode.cpp requires an F word for G0 under G93. Issue the rapid in normal mode and
            // restore inverse time on the next cut.
            output.push_back("G94");
            inverse_active = false;
        }
        output.push_back(command + " " + transformed);
    }
    if (min_angle == kInf || min_axial == kInf || min_z == kInf || minutes == 0 || !have_feed)
        throw ValueError(
            "Rotary job requires initialized axial and angular coordinates and a nonzero cutting move");
    output.push_back("G94 F" + fixed(last_feed, 6));
    const std::string along_text(1, along), radial_text(1, radial);
    json fixture_value;  // None
    if (fixture) fixture_value = json{{"axial_min", f_low}, {"axial_max", f_high}, {"min_radius", f_core}};
    json meta = json{
        {"postprocessor", post},
        {"export_only", linuxcnc},
        {"diameter", diameter},
        {"circumference", circumference},
        {"wrap_axis", std::string(1, wrapped)},
        {"output_axis", drive},
        {"axial_output_axis", along_text},
        {"radial_output_axis", radial_text},
        {"angle_offset", offset},
        {"direction", direction},
        {"angle_bounds", json::array({min_angle, max_angle})},
        {"axial_bounds", json::array({min_axial, max_axial})},
        {"radial_bounds", json::array({min_z, max_z})},
        {"feed_mode", "G93"},
        {"feed_reference", reference},
        {"feed_minutes", minutes},
        {"fixture", fixture_value},
        {"seam_snap_tolerance_mm", 0.00051},
        {"preview_coordinates", "planar"},
    };
    json warnings = json::array();
    if (const json *given = get(job, "warnings")) {
        if (!given->is_array()) throw NotPorted("rotary job warnings that are not a list");
        for (const json &warning : *given) warnings.push_back(warning);
    }
    warnings.push_back(drive + " rotates about " + along_text +
                       ". Set the axial work zero, angular seam and surface work zero on " + radial_text +
                       " for this fixture.");
    warnings.push_back("Continuous angles retain full turns. Check angular travel and clearance of the tool, chuck and holder.");
    if (linuxcnc) warnings.push_back("LinuxCNC rotary output is file export only.");
    json result = job;
    result["lines"] = json(output);
    result["warnings"] = std::move(warnings);
    result["rotary"] = std::move(meta);
    return result;
}

// The planar job as the reference's rotary generator sees it. The rows of its move loop and of ramp and helix
// entries were packed as seven floats, so an integer 0 there (the height of a laser move, the power of a
// beam-off gap, a whole-number entry height) is the float 0.0; drilling rows were not, and are as generate()
// gives them.
json packed_job(const Job &job) {
    json result = to_json(job);
    json &segments = result["segments"];
    for (size_t i = 0; i < job.segments.size(); ++i) {
        const Segment &s = job.segments[i];
        if (!s.packed) continue;
        json row = json::array({s.x, s.y, s.nx, s.ny, s.planar || s.whole_end ? 0.0 : s.end_z, nullptr,
                                s.planar || s.whole_start ? 0.0 : s.start_z});
        if (s.has_power) row[5] = s.whole_power ? json(0.0) : json(s.power);
        segments[i] = std::move(row);
    }
    return result;
}

}  // namespace

Json generate_rotary(const Json &project, const Json &options) {
    if (!project.is_object() || !options.is_object()) throw ValueError("Supply a project and rotary setup");
    const json *protocol = get(project, "protocol");
    const bool grbl_family = !protocol || is_text(protocol, "grbl") || is_text(protocol, "grblhal") ||
                             is_text(protocol, "xpro-v5") || is_text(protocol, "fluidnc");
    if (!grbl_family && !is_text(protocol, "linuxcnc"))
        throw ValueError("Select a GRBL-family or LinuxCNC source for rotary CAM");
    // Generate fresh design toolpaths here. Mode and axis changes are carried in the rotary post; incoming
    // files or raw lines are never executed.
    if (is_text(protocol, "linuxcnc") && stream_post(get(options, "postprocessor")))
        throw ValueError("Choose a GRBL-family source for this rotary postprocessor");
    return wrap(packed_job(generate(project)), options, true);
}

Json wrap_job(const Json &job, const Json &options) { return wrap(job, options, false); }

bool rotary_command(const std::string &command) { return command == "rotary" || command == "rotary-wrap"; }

Json rotary_call(const std::string &command, const Json &request) {
    if (!request.is_object()) throw ValueError("Supply a project and rotary setup");
    const json none;
    const json *options = get(request, "options");
    if (command == "rotary") {
        const json *project = get(request, "project");
        return generate_rotary(project ? *project : none, options ? *options : json::object());
    }
    const json *job = get(request, "job");
    return wrap_job(job ? *job : none, options ? *options : json::object());
}

}  // namespace kerfkite
