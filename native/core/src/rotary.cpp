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
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
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
[[noreturn]] void out_of_range(const char *label, const char *low_text, const std::string &high_text) {
    throw ValueError(std::string(label) + " must be a finite number in " + low_text + ".." + high_text);
}

double rotary_number(const json *value, const char *label, double low, double high, const char *low_text,
                     const std::string &high_text) {
    double result = 0;
    if (!value || value->is_boolean() || !plain_number(*value, result) || !std::isfinite(result) ||
        !(low <= result && result <= high))
        out_of_range(label, low_text, high_text);
    return result;
}

// The same check for a value already read as a float.
double rotary_number(double value, const char *label, double low, double high, const char *low_text,
                     const char *high_text) {
    if (!std::isfinite(value) || !(low <= value && value <= high)) out_of_range(label, low_text, high_text);
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

// One block of the source, read in place: its first token, its words in order (one for each letter at most),
// and the line without the white space around it.
struct Block {
    std::string_view command, raw;
    char letters[26];
    double values[26];
    int count = 0;
    unsigned seen = 0;  // a bit for each letter present

    const double *word(char letter) const {
        if (!(seen >> (letter - 'A') & 1u)) return nullptr;
        for (int i = 0; i < count; ++i)
            if (letters[i] == letter) return &values[i];
        return nullptr;
    }
    // set(words) == letters.
    bool exactly(const char *set) const {
        unsigned wanted = 0;
        for (const char *l = set; *l; ++l) wanted |= 1u << (*l - 'A');
        return seen == wanted;
    }
};

// The reference's _WORD.fullmatch(token): a capital letter, then [+-]?(\d+(\.\d*)?|\.\d+).
bool word_token(std::string_view token) {
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

// float() of a number that word_token accepted: the nearest double. With at most 2^53 as the digits and at
// most 22 places, the digits and the power of ten are both exact doubles, so their quotient, which IEEE
// division rounds correctly, is the nearest double to the decimal (Clinger's fast path). Longer numbers go
// to the core's exact reader.
double word_value(std::string_view number) {
    static const double kPowers[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                       1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    size_t i = 0;
    const bool negative = number[0] == '-';
    if (number[0] == '+' || number[0] == '-') ++i;
    uint64_t digits = 0;
    int places = 0;
    bool fraction = false, exact = true;
    for (; i < number.size(); ++i) {
        if (number[i] == '.') {
            fraction = true;
            continue;
        }
        if (digits > (uint64_t{1} << 53) / 10) {
            exact = false;
            break;
        }
        digits = digits * 10 + static_cast<uint64_t>(number[i] - '0');
        if (fraction) ++places;
    }
    if (exact && digits <= (uint64_t{1} << 53) && places <= 22) {
        const double value = static_cast<double>(digits) / kPowers[places];
        return negative ? -value : value;
    }
    double value = 0;
    if (!text::parse_double(number.data(), number.data() + number.size(), value))
        throw std::logic_error("A matched word could not be read");
    return value;
}

// _blocks(): each line split into tokens and checked as the reference checks a line of text.
void parse(const std::string &item, Block &block) {
    if (item.size() > 256) throw ValueError("Invalid or oversized source G-code line");
    for (const unsigned char c : item)
        if (c >= 0x80) throw NotPorted("G-code text that is not ASCII");
    size_t begin = 0, end = item.size();
    while (begin < end && space(item[begin])) ++begin;
    while (end > begin && space(item[end - 1])) --end;
    const std::string_view line(item);
    block.raw = line.substr(begin, end - begin);
    block.count = 0;
    block.seen = 0;
    bool first = true;
    for (size_t i = begin; i < end;) {
        while (i < end && space(item[i])) ++i;
        size_t j = i;
        while (j < end && !space(item[j])) ++j;
        if (j == i) break;
        const std::string_view token = line.substr(i, j - i);
        if (first) block.command = token;
        first = false;
        const unsigned bit = token[0] >= 'A' && token[0] <= 'Z' ? 1u << (token[0] - 'A') : 0;
        if (!word_token(token) || (block.seen & bit))
            throw ValueError("Rotary requires canonical generated G-code without comments or duplicate words");
        block.letters[block.count] = token[0];
        block.values[block.count++] =
            rotary_number(word_value(token.substr(1)), "Source word", -1e9, 1e9, "-1000000000.0", "1000000000.0");
        block.seen |= bit;
        i = j;
    }
    if (first) throw ValueError("Blank source lines are unsupported");
}

bool one_of(std::string_view text, std::initializer_list<const char *> options) {
    for (const char *option : options)
        if (text == option) return true;
    return false;
}

// What _wrap_job keeps between blocks for one coordinate: None, or a value.
struct Coordinate {
    bool known = false;
    double value = 0;
};

int axis(char letter) { return letter - 'X'; }  // X, Y and Z are 0, 1 and 2

// What the wrapper needs of a job: its mode, its program and its warnings. A given job is read through its
// JSON members; a generated one hands over its own lines and warnings.
struct Source {
    const json *mode = nullptr;
    const json *given_lines = nullptr;                    // the given job's "lines", unchecked
    const json *given_warnings = nullptr;                 // the given job's "warnings", if it has them
    const std::vector<std::string> *lines = nullptr;      // the generated job's program
    const std::vector<std::string> *warnings = nullptr;   // the generated job's warnings
};

struct Wrapped {
    std::vector<std::string> lines;
    json warnings, meta;
};

// _wrap_job(job, options, generated=...).
Wrapped wrap(const Source &job, const json &options, bool generated) {
    const json *post_value = get(options, "postprocessor"), *mode_value = job.mode;
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
    std::vector<const std::string *> source;
    if (job.lines) {
        if (job.lines->empty() || job.lines->size() > 200000)
            throw ValueError("Rotary source must contain 1..200000 G-code lines");
        source.reserve(job.lines->size());
        for (const std::string &line : *job.lines) source.push_back(&line);
    } else {
        const json *given = job.given_lines;
        if (!given || !given->is_array() || given->size() < 1 || given->size() > 200000)
            throw ValueError("Rotary source must contain 1..200000 G-code lines");
        source.reserve(given->size());
        for (const json &line : *given) {
            if (!line.is_string()) throw ValueError("Invalid or oversized source G-code line");
            source.push_back(&line.get_ref<const std::string &>());
        }
    }
    if (!generated)
        for (const std::string *line : source) {
            for (const unsigned char c : *line)
                if (c >= 0x80) throw NotPorted("G-code text that is not ASCII");
            if (line->size() > 256) throw ValueError("Invalid or oversized source G-code line");
        }
    if (*source.back() == "M30") source.pop_back();
    if (source.empty()) throw ValueError("Rotary source must end with spindle off (M5)");
    {
        const std::string &last = *source.back();
        for (const unsigned char c : last)
            if (c >= 0x80) throw NotPorted("G-code text that is not ASCII");
        size_t begin = 0, end = last.size();
        while (begin < end && space(last[begin])) ++begin;
        while (end > begin && space(last[end - 1])) --end;
        if (last.compare(begin, end - begin, "M5") != 0 || end - begin != 2)
            throw ValueError("Rotary source must end with spindle off (M5)");
    }
    Coordinate state[3];
    if (laser) state[axis('Z')] = {true, 0.0};  // Laser focus at the stock surface.
    std::vector<std::string> output;
    output.reserve(source.size() + source.size() / 8 + 2);
    double min_angle = kInf, min_axial = kInf, min_z = kInf;
    double max_angle = -kInf, max_axial = -kInf, max_z = -kInf;
    bool units = false, absolute = false, feed_mode = false, inverse_active = false, emitting = false;
    bool have_feed = false;
    double last_feed = 0, minutes = 0.0;
    const char *spindle = laser ? "M4" : "M3";
    const bool stream = stream_post(post_value), xpro = post == "xpro-v5";
    const bool surface = reference == "surface";
    const std::string drive_word = drive, along_word(1, along), radial_word(1, radial);
    Block block;
    std::string line;
    for (const std::string *item : source) {
        parse(*item, block);
        const std::string_view command = block.command;
        if (block.count == 1 && one_of(command, {"G21", "G90", "G17", "G54", "G94", "M5", "G40", "G49", "G80", "G61"})) {
            if (stream && one_of(command, {"G40", "G49", "G80", "G61"}))
                throw ValueError("Source contains a LinuxCNC-only setup command");
            if (command == "G21") units = true;
            if (command == "G90") absolute = true;
            if (command == "M5") emitting = false;
            if (command == "G94") {
                feed_mode = true;
                inverse_active = !xpro;
                output.emplace_back(inverse_active ? "G93" : "G94");
                continue;
            }
            output.emplace_back(command);
            continue;
        }
        if (command == spindle && block.exactly("MS")) {
            rotary_number(*block.word('S'), "Spindle power", laser ? 0 : 0.001, 100000, laser ? "0" : "0.001", "100000");
            emitting = true;
            output.emplace_back(block.raw);
            continue;
        }
        if (router && command == "G4" && block.exactly("GP") && 0 <= *block.word('P') && *block.word('P') <= 60) {
            output.emplace_back(block.raw);
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
        const unsigned bit_x = 1u << ('X' - 'A'), bit_y = 1u << ('Y' - 'A'), bit_z = 1u << ('Z' - 'A');
        unsigned allowed = (1u << ('G' - 'A')) | bit_x | bit_y;
        if (!laser) allowed |= bit_z;
        if (linear) allowed |= (1u << ('F' - 'A')) | (laser ? 1u << ('S' - 'A') : 0u);
        if ((block.seen & ~allowed) || !(block.seen & (bit_x | bit_y | bit_z)))
            throw ValueError("Unsupported words in rotary motion");
        // coords: the coordinates of the block, in the order X, Y, Z.
        const double *coords[3] = {block.word('X'), block.word('Y'), block.word('Z')};
        const Coordinate z = coords[2] ? Coordinate{true, *coords[2]} : state[2];
        if (z.known) {
            if (radius + z.value <= 0) throw ValueError("Cut depth reaches or crosses the cylinder center");
            if (fixture && radius + z.value < f_core - 1e-9)
                throw ValueError("Rotary toolpath enters the protected fixture core");
            if (z.value < min_z) min_z = z.value;
            if (z.value > max_z) max_z = z.value;
        }
        if (const double *x = coords[axis(axial)]) {
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
            for (int k = 0; k < 3; ++k)
                if (coords[k] && !state[k].known)
                    throw ValueError("Cutting move starts from an unknown position; establish rapid coordinates first");
            const int w = axis(wrapped), a = axis(axial);
            const double arc = coords[w] ? *coords[w] - state[w].value : 0;
            const double travel = coords[a] ? *coords[a] - state[a].value : 0;
            const double depth = coords[2] ? *coords[2] - state[2].value : 0;
            // state['Z'] or 0: None and zero both start from 0.
            const double from = state[2].known && state[2].value != 0 ? state[2].value : 0;
            length = cut_length(arc, travel, depth, radius, from, surface);
        }
        line.assign(command);
        for (int k = 0; k < 3; ++k) {
            if (!coords[k]) continue;
            const double destination = *coords[k];
            const char letter = static_cast<char>('X' + k);
            line.push_back(' ');
            if (letter == wrapped) {
                const double turn = std::nearbyint(destination / circumference);
                const double arc =
                    std::fabs(destination - turn * circumference) <= 0.00051 ? turn * circumference : destination;
                const double angle = offset + direction * 360 * arc / circumference;
                if (std::fabs(angle) > 360000) throw ValueError("Rotary extent exceeds 1000 revolutions");
                if (angle < min_angle) min_angle = angle;
                if (angle > max_angle) max_angle = angle;
                line += drive_word;
                text::fixed(line, angle, 6);
            } else {
                line += letter == axial ? along_word : radial_word;
                text::fixed(line, destination, 6);
            }
            state[k] = {true, destination};
        }
        if (linear) {
            if (length <= 1e-12) continue;
            double inverse = last_feed / length;
            if (!(1e-9 <= inverse && inverse <= 1e9))
                inverse = rotary_number(inverse, "Inverse-time feed", 1e-9, 1e9, "1e-09", "1000000000.0");
            if (!inverse_active) {
                output.emplace_back("G93");
                inverse_active = true;
            }
            line += " F";
            text::fixed(line, inverse, 9);
            if (laser) {
                line += " S";
                text::fixed(line, *block.word('S'), 0);
            }
            minutes += length / last_feed;
        } else if (xpro && inverse_active) {
            // Vendor GCode.cpp requires an F word for G0 under G93. Issue the rapid in normal mode and
            // restore inverse time on the next cut.
            output.emplace_back("G94");
            inverse_active = false;
        }
        output.push_back(line);
    }
    if (min_angle == kInf || min_axial == kInf || min_z == kInf || minutes == 0 || !have_feed)
        throw ValueError(
            "Rotary job requires initialized axial and angular coordinates and a nonzero cutting move");
    output.push_back("G94 F" + fixed(last_feed, 6));
    json fixture_value;  // None
    if (fixture) fixture_value = json{{"axial_min", f_low}, {"axial_max", f_high}, {"min_radius", f_core}};
    Wrapped result;
    result.meta = json{
        {"postprocessor", post},
        {"export_only", linuxcnc},
        {"diameter", diameter},
        {"circumference", circumference},
        {"wrap_axis", std::string(1, wrapped)},
        {"output_axis", drive},
        {"axial_output_axis", along_word},
        {"radial_output_axis", radial_word},
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
    result.warnings = json::array();
    if (job.warnings) {
        for (const std::string &warning : *job.warnings) result.warnings.push_back(warning);
    } else if (job.given_warnings) {
        if (!job.given_warnings->is_array()) throw NotPorted("rotary job warnings that are not a list");
        for (const json &warning : *job.given_warnings) result.warnings.push_back(warning);
    }
    result.warnings.push_back(drive + " rotates about " + along_word +
                              ". Set the axial work zero, angular seam and surface work zero on " + radial_word +
                              " for this fixture.");
    result.warnings.push_back(
        "Continuous angles retain full turns. Check angular travel and clearance of the tool, chuck and holder.");
    if (linuxcnc) result.warnings.push_back("LinuxCNC rotary output is file export only.");
    result.lines = std::move(output);
    return result;
}

// The lines as a JSON array, moved rather than copied.
json lines_json(std::vector<std::string> &&lines) {
    json result = json::array();
    for (std::string &line : lines) result.push_back(json(std::move(line)));
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

RotaryJob rotary_job(const Json &project, const Json &options) {
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
    RotaryJob result;
    result.planar = generate(project);
    const json mode(result.planar.mode);
    Source source;
    source.mode = &mode;
    source.lines = &result.planar.lines;
    source.warnings = &result.planar.warnings;
    Wrapped wrapped = wrap(source, options, true);
    result.lines = std::move(wrapped.lines);
    result.warnings = std::move(wrapped.warnings);
    result.rotary = std::move(wrapped.meta);
    return result;
}

Json generate_rotary(const Json &project, const Json &options) {
    RotaryJob job = rotary_job(project, options);
    json result = packed_job(job.planar);
    result["lines"] = lines_json(std::move(job.lines));
    result["warnings"] = std::move(job.warnings);
    result["rotary"] = std::move(job.rotary);
    return result;
}

Json wrap_job(const Json &job, const Json &options) {
    if (!job.is_object() || !options.is_object()) throw ValueError("Rotary job and options must be objects");
    Source source;
    source.mode = get(job, "mode");
    source.given_lines = get(job, "lines");
    source.given_warnings = get(job, "warnings");
    Wrapped wrapped = wrap(source, options, false);
    json result = job;
    result["lines"] = lines_json(std::move(wrapped.lines));
    result["warnings"] = std::move(wrapped.warnings);
    result["rotary"] = std::move(wrapped.meta);
    return result;
}

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
