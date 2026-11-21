// Gerber import: see kerfkite/gerber.hpp.
//
// The reader follows the Gerber Layer Format Specification. Where older files differ in a way that has one
// reading, that reading is taken and a warning says so; where a file needs something this reader does not
// do, the file is refused with the reason. Every count that a file controls has a limit.
#include "kerfkite/gerber.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "pcb_text.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;
using namespace pcb;

namespace {

const size_t MAX_TEXT = 16u << 20;
const size_t MAX_OBJECTS = 100000;  // after step-and-repeat
const size_t MAX_POINTS = 500000;   // of flattened arcs and region contours together
const size_t MAX_APERTURES = 10000;
const size_t MAX_MACROS = 1000;
const size_t MAX_MACRO_TEXT = 65536;    // characters in one macro definition
const size_t MAX_PARTS = 2000;          // shapes in one macro aperture
const size_t MAX_MACRO_POINTS = 20000;  // polygon points in one macro aperture
const size_t MAX_ALL_PARTS = 50000;     // shapes in all macro apertures together
const size_t MAX_ALL_MACRO_POINTS = 200000;
const size_t MAX_BLOCK = 4096;  // characters in one command
const int MAX_DEPTH = 32;       // nested brackets in a macro expression
const int MAX_REPEAT = 1000;    // step-and-repeat copies along one axis
const double MAX_SIZE = 1000;   // millimetres, for any aperture dimension
const double PI = 3.14159265358979323846;
const double CLOSED = 1e-9;  // a contour closes when its ends are this near, in millimetres

double distance(const Point &a, const Point &b) { return std::hypot(a.x - b.x, a.y - b.y); }

Point rotated(const Point &p, double degrees) {
    if (degrees == 0) return p;
    const double angle = degrees * PI / 180, c = std::cos(angle), s = std::sin(angle);
    return Point{p.x * c - p.y * s, p.x * s + p.y * c};
}

// One piece of a macro aperture, in millimetres about the flash point: a circle or a closed polygon that
// adds to the aperture's image (exposure on) or removes from what the earlier pieces built (exposure off).
struct Part {
    bool exposure = true, circle = false;
    double diameter = 0;
    Point centre;
    std::vector<Point> points;
};

struct Aperture {
    std::string shape;  // circle, rectangle, obround, polygon or macro
    double width = 0, height = 0, diameter = 0, rotation = 0, hole = 0;
    int vertices = 0;
    std::string macro;
    std::vector<Part> parts;
    double box[4] = {0, 0, 0, 0};  // the extent about the flash point
};

struct Object {
    char type = 'F';  // F flash, L line, A arc, R region
    bool dark = true, clockwise = false;
    int aperture = 0;
    Point start, end, centre;
    size_t first = 0, count = 0;  // the flattened arc or the contour, in Reader::points
};

// start to end about centre, as points after the start: the last is the end itself. The radius runs evenly
// from the start's to the end's, so a centre that is slightly off still gives a continuous curve.
void flatten(const Point &start, const Point &end, const Point &centre, bool clockwise, bool whole, double tolerance,
             std::vector<Point> &out) {
    const double r0 = distance(start, centre), r1 = distance(end, centre), radius = std::max(r0, r1);
    const double a0 = std::atan2(start.y - centre.y, start.x - centre.x);
    double sweep = 2 * PI;
    if (!whole) {
        const double a1 = std::atan2(end.y - centre.y, end.x - centre.x);
        sweep = std::fmod(clockwise ? a0 - a1 : a1 - a0, 2 * PI);
        if (sweep <= 0) sweep += 2 * PI;
    }
    // The chord of an angle t lies radius * (1 - cos(t / 2)) inside the arc. At least eight chords to a turn.
    const double finest = tolerance < radius ? 2 * std::acos(1 - tolerance / radius) : PI / 4;
    const double steps = std::max(1.0, std::ceil(sweep / std::min(finest, PI / 4) - 1e-9));
    for (double i = 1; i < steps; ++i) {
        const double t = i / steps, angle = clockwise ? a0 - sweep * t : a0 + sweep * t, r = r0 + (r1 - r0) * t;
        out.push_back(Point{centre.x + r * std::cos(angle), centre.y + r * std::sin(angle)});
    }
    out.push_back(end);
}

// The ends of an arc and the points where it reaches furthest along each axis: what its bounds rest on.
// The flattened points would do nearly as well, but they lie inside the arc by up to the tolerance.
std::vector<Point> extremes(const Point &start, const Point &end, const Point &centre, bool clockwise) {
    std::vector<Point> out{start, end};
    const double r0 = distance(start, centre), r1 = distance(end, centre);
    const double a0 = std::atan2(start.y - centre.y, start.x - centre.x);
    double sweep = 2 * PI;
    if (start.x != end.x || start.y != end.y) {
        const double a1 = std::atan2(end.y - centre.y, end.x - centre.x);
        sweep = std::fmod(clockwise ? a0 - a1 : a1 - a0, 2 * PI);
        if (sweep <= 0) sweep += 2 * PI;
    }
    for (int k = 0; k < 4; ++k) {
        const double axis = k * PI / 2;
        double along = std::fmod(clockwise ? a0 - axis : axis - a0, 2 * PI);
        if (along < 0) along += 2 * PI;
        if (along > sweep) continue;
        const double r = r0 + (r1 - r0) * along / sweep;
        out.push_back(Point{centre.x + r * std::cos(axis), centre.y + r * std::sin(axis)});
    }
    return out;
}

// An aperture macro expression: numbers, $n variables, + - x / and brackets.
class Expression {
  public:
    Expression(const std::string &text, const std::map<int, double> &variables, const std::string &macro,
               size_t at_line)
        : s(text), vars(variables), name(macro), line(at_line) {}

    double whole() {
        const double value = sum(0);
        if (i != s.size()) refuse("has an expression that is not understood: " + printable(s));
        return value;
    }

  private:
    const std::string &s;
    const std::map<int, double> &vars;
    const std::string &name;
    const size_t line;
    size_t i = 0;

    [[noreturn]] void refuse(const std::string &why) const {
        throw ValueError("Line " + std::to_string(line) + ": aperture macro " + printable(name) + " " + why);
    }

    double checked(double value) const {
        if (!std::isfinite(value) || std::fabs(value) > 1e9) refuse("computes a value that is out of range");
        return value;
    }

    double sum(int depth) {
        double value = product(depth);
        while (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            const char op = s[i++];
            const double right = product(depth);
            value = checked(op == '+' ? value + right : value - right);
        }
        return value;
    }

    double product(int depth) {
        double value = factor(depth);
        while (i < s.size() && (s[i] == 'x' || s[i] == 'X' || s[i] == '/')) {
            const char op = s[i++];
            const double right = factor(depth);
            if (op == '/' && right == 0) refuse("divides by zero");
            value = checked(op == '/' ? value / right : value * right);
        }
        return value;
    }

    double factor(int depth) {
        bool negative = false;
        while (i < s.size() && (s[i] == '+' || s[i] == '-')) negative ^= s[i++] == '-';
        double value = 0;
        if (i < s.size() && s[i] == '(') {
            if (depth == MAX_DEPTH) refuse("nests brackets more than " + std::to_string(MAX_DEPTH) + " deep");
            ++i;
            value = sum(depth + 1);
            if (i >= s.size() || s[i] != ')') refuse("has an unclosed bracket");
            ++i;
        } else if (i < s.size() && s[i] == '$') {
            const size_t from = ++i;
            while (i < s.size() && digit(s[i])) ++i;
            if (i == from || i - from > 5) refuse("names a variable that is not understood");
            const auto found = vars.find(static_cast<int>(whole_number(s, from, i)));
            value = found == vars.end() ? 0 : found->second;  // a variable never set is zero
        } else {
            const size_t from = i;
            while (i < s.size() && (digit(s[i]) || s[i] == '.')) ++i;
            if (!decimal(s, from, i, value)) refuse("has an expression that is not understood: " + printable(s));
        }
        return negative ? -value : value;
    }
};

class Reader {
  public:
    std::map<int, Aperture> apertures;
    std::vector<Object> objects;
    std::vector<Point> points;
    std::map<std::string, std::string> attributes;
    Warnings warnings;
    bool inch = false, units_set = false, format_set = false, trailing_omitted = false;
    bool incremental = false, declared_incremental = false;  // as now in force (G90, G91), and as %FS states
    int integer_digits = 0, decimal_digits = 0;
    double tolerance = 0.005;

    void read(const std::string &text) {
        if (text.find("FS") == std::string::npos)
            throw ValueError("This is not a Gerber RS-274X file: it has no format statement (%FS)");
        size_t i = after_mark(text);
        const size_t n = text.size();
        while (i < n && !ended) {
            const char c = text[i];
            if (c == '\n') ++line;
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                ++i;
                continue;
            }
            if (c == '%') {
                const size_t close = text.find('%', i + 1);
                if (close == std::string::npos) refuse("the file ends inside an extended command; it may be truncated");
                extended(text.substr(i + 1, close - i - 1));
                i = close + 1;
                continue;
            }
            const size_t star = text.find('*', i);
            if (star == std::string::npos) refuse("the file ends in the middle of a command; it may be truncated");
            const std::string raw = text.substr(i, star - i);
            block(raw);
            line += static_cast<size_t>(std::count(raw.begin(), raw.end(), '\n'));
            i = star + 1;
        }
        if (!ended) throw ValueError("The Gerber file has no end-of-file command (M02); it may be truncated");
        if (!format_set) throw ValueError("This is not a Gerber RS-274X file: it has no format statement (%FS)");
        if (deviant) {
            warnings.add(std::to_string(deviant) + (deviant == 1 ? " arc has" : " arcs have") +
                         " a centre that does not match both ends, by up to " + mm(worst) +
                         " mm; the radius was run evenly from one end to the other");
        }
    }

  private:
    struct Macro {
        std::vector<std::string> blocks;
    };
    std::map<std::string, Macro> macros;
    size_t line = 1;
    bool ended = false, dark = true, region = false, point_set = false;
    int mode = 0;      // 1 linear, 2 clockwise arc, 3 anticlockwise arc; 0 until the file says
    int quadrant = 0;  // 1 single (G74), 2 multi (G75); 0 until the file says
    int last_operation = 0, selected = 0;
    Point current;
    std::vector<Point> contour;
    // An open step-and-repeat block: the objects from sr_first on are repeated when it closes.
    bool repeating = false;
    size_t sr_first = 0;
    int sr_x = 1, sr_y = 1;
    double sr_i = 0, sr_j = 0;
    size_t deviant = 0, all_parts = 0, all_macro_points = 0;
    double worst = 0;

    [[noreturn]] void refuse(const std::string &why) const {
        throw ValueError("Line " + std::to_string(line) + ": " + why);
    }

    double scale() const { return inch ? INCH : 1.0; }

    void room(size_t more_objects, size_t more_points) const {
        if (objects.size() + more_objects > MAX_OBJECTS)
            throw ValueError("The Gerber file draws more than " + std::to_string(MAX_OBJECTS) + " objects");
        if (points.size() + more_points > MAX_POINTS)
            throw ValueError("The Gerber file needs more than " + std::to_string(MAX_POINTS) +
                             " points once arcs are flattened; use a coarser arc tolerance");
    }

    // ---- extended commands: %...*% ----

    void extended(const std::string &content) {
        // The blocks between the percent signs, each ended by an asterisk.
        std::vector<std::string> blocks;
        std::string one;
        size_t lines = 0;
        for (const char c : content) {
            if (c == '\n') ++lines;
            if (c == '\n' || c == '\r') continue;
            if (c == '*') {
                blocks.push_back(one);
                one.clear();
            } else {
                one.push_back(c);
            }
        }
        if (one.find_first_not_of(" \t") != std::string::npos)
            refuse("an extended command is not ended with an asterisk");
        if (blocks.empty()) refuse("an extended command is empty");
        if (blocks[0].compare(0, 2, "AM") == 0) {
            define_macro(blocks, content.size());
        } else {
            // Older files put several commands between one pair of percent signs.
            for (const std::string &command : blocks) extended_command(command);
        }
        line += lines;
    }

    void define_macro(const std::vector<std::string> &blocks, size_t size) {
        std::string name;
        for (const char c : blocks[0].substr(2))
            if (c != ' ' && c != '\t') name.push_back(c);
        if (name.empty()) refuse("an aperture macro has no name");
        if (size > MAX_MACRO_TEXT)
            refuse("aperture macro " + printable(name) + " is longer than " + std::to_string(MAX_MACRO_TEXT) +
                   " characters");
        if (macros.size() == MAX_MACROS && !macros.count(name))
            refuse("the file defines more than " + std::to_string(MAX_MACROS) + " aperture macros");
        Macro macro;
        for (size_t k = 1; k < blocks.size(); ++k) {
            const std::string &raw = blocks[k];
            const size_t from = raw.find_first_not_of(" \t");
            if (from == std::string::npos) continue;
            // Primitive 0 is a comment.
            if (raw[from] == '0' && (from + 1 == raw.size() || raw[from + 1] == ' ' || raw[from + 1] == ',')) continue;
            std::string clean;
            for (const char c : raw)
                if (c != ' ' && c != '\t') clean.push_back(c);
            macro.blocks.push_back(clean);
        }
        macros[name] = macro;
    }

    // The decimal after a letter in commands such as %OFA0B0*%; the fallback when the letter is absent.
    double lettered(const std::string &command, char letter, double fallback) {
        const size_t at = command.find(letter, 2);
        if (at == std::string::npos) return fallback;
        size_t end = at + 1;
        while (end < command.size() &&
               (digit(command[end]) || command[end] == '.' || command[end] == '-' || command[end] == '+'))
            ++end;
        double value = 0;
        if (!decimal(command, at + 1, end, value)) refuse("command %" + printable(command) + " is not understood");
        return value;
    }

    void extended_command(const std::string &raw) {
        std::string command;
        const size_t from = raw.find_first_not_of(" \t");
        if (from == std::string::npos) return;
        command = raw.substr(from);
        const std::string code = command.substr(0, 2), rest = command.size() > 2 ? command.substr(2) : std::string();
        if (code == "TF") {
            file_attribute(rest);
        } else if (code == "TA" || code == "TO" || code == "TD" || code == "IN" || code == "LN") {
            // Aperture and object attributes and the image and layer names do not change the image.
        } else if (code == "FS") {
            format(rest);
        } else if (code == "MO") {
            if (rest == "MM")
                inch = false;
            else if (rest == "IN")
                inch = true;
            else
                refuse("the units command %MO" + printable(rest) + " is neither MM nor IN");
            units_set = true;
        } else if (code == "AD") {
            define_aperture(rest);
        } else if (code == "LP") {
            if (rest == "D")
                dark = true;
            else if (rest == "C")
                dark = false;
            else
                refuse("the polarity command %LP" + printable(rest) + " is neither D nor C");
        } else if (code == "SR") {
            step_and_repeat(command);
        } else if (code == "LM") {
            if (rest != "N") refuse("aperture mirroring (%LM) is not supported");
        } else if (code == "LR") {
            if (lettered("LRR" + rest, 'R', 0) != 0) refuse("aperture rotation (%LR) is not supported");
        } else if (code == "LS") {
            if (lettered("LSS" + rest, 'S', 1) != 1) refuse("aperture scaling (%LS) is not supported");
        } else if (code == "AB") {
            refuse("block apertures (%AB) are not supported");
        } else if (code == "IP") {
            if (rest != "POS") refuse("a negative image (%IPNEG) is not supported");
        } else if (code == "AS") {
            if (rest != "AXBY") refuse("swapped axes (%AS) are not supported");
        } else if (code == "IR") {
            if (lettered("IRR" + rest, 'R', 0) != 0) refuse("image rotation (%IR) is not supported");
        } else if (code == "MI") {
            if (lettered(command, 'A', 0) != 0 || lettered(command, 'B', 0) != 0)
                refuse("image mirroring (%MI) is not supported");
        } else if (code == "OF") {
            if (lettered(command, 'A', 0) != 0 || lettered(command, 'B', 0) != 0)
                refuse("an image offset (%OF) is not supported");
        } else if (code == "SF") {
            if (lettered(command, 'A', 1) != 1 || lettered(command, 'B', 1) != 1)
                refuse("an image scale factor (%SF) is not supported");
        } else {
            refuse("the extended command %" + printable(code) + " is not supported");
        }
    }

    void file_attribute(const std::string &rest) {
        const size_t comma = rest.find(',');
        const std::string name = printable(rest.substr(0, comma), 80);
        if (name.empty() || (attributes.size() == 100 && !attributes.count(name))) return;
        attributes[name] = comma == std::string::npos ? std::string() : printable(rest.substr(comma + 1), 200);
    }

    // %FSLAX24Y24*%: which zeros are omitted, absolute or incremental, and the digits either side of the point.
    void format(const std::string &rest) {
        size_t i = 0;
        bool trailing = false, relative = false;
        if (i < rest.size() && (rest[i] == 'L' || rest[i] == 'T')) trailing = rest[i++] == 'T';
        if (i < rest.size() && (rest[i] == 'A' || rest[i] == 'I')) relative = rest[i++] == 'I';
        const auto digits = [&](char axis, int &whole, int &fraction) {
            if (i + 2 >= rest.size() || rest[i] != axis || !digit(rest[i + 1]) || !digit(rest[i + 2]))
                refuse("the format statement %FS" + printable(rest) + " is not understood");
            whole = rest[i + 1] - '0';
            fraction = rest[i + 2] - '0';
            i += 3;
        };
        int xi = 0, xd = 0, yi = 0, yd = 0;
        digits('X', xi, xd);
        digits('Y', yi, yd);
        if (i != rest.size()) refuse("the format statement %FS" + printable(rest) + " is not understood");
        if (xi != yi || xd != yd) refuse("the format statement gives X and Y different digits, which is not supported");
        if (xi > 7 || xd > 7) refuse("the format statement asks for more than seven digits either side of the point");
        if (format_set && (xi != integer_digits || xd != decimal_digits || trailing != trailing_omitted ||
                           relative != declared_incremental))
            refuse("the coordinate format changes within the file");
        declared_incremental = relative;
        integer_digits = xi;
        decimal_digits = xd;
        trailing_omitted = trailing;
        incremental = relative;
        format_set = true;
        if (trailing) warnings.add("The file omits trailing zeros (%FST), a deprecated format");
        if (relative) warnings.add("The file uses incremental coordinates, a deprecated notation");
    }

    void step_and_repeat(const std::string &command) {
        if (region) refuse("step-and-repeat inside a region");
        close_repeat();
        if (command.size() == 2) return;  // %SR*% only closes
        const double x = lettered(command, 'X', 1), y = lettered(command, 'Y', 1);
        if (x != std::floor(x) || y != std::floor(y) || x < 1 || y < 1)
            refuse("step-and-repeat counts must be whole numbers of at least 1");
        if (x > MAX_REPEAT || y > MAX_REPEAT)
            refuse("step-and-repeat asks for more than " + std::to_string(MAX_REPEAT) + " copies along an axis");
        if (!units_set && (x > 1 || y > 1)) refuse("step-and-repeat appears before the units (%MO)");
        sr_x = static_cast<int>(x);
        sr_y = static_cast<int>(y);
        sr_i = lettered(command, 'I', 0) * scale();
        sr_j = lettered(command, 'J', 0) * scale();
        if (sr_i < 0 || sr_j < 0 || sr_i > MAX_COORDINATE || sr_j > MAX_COORDINATE)
            refuse("step-and-repeat distances are out of range");
        repeating = true;
        sr_first = objects.size();
    }

    // The block is copied along Y first, then along X, as the specification orders the copies.
    void close_repeat() {
        if (!repeating) return;
        repeating = false;
        const uint64_t copies = static_cast<uint64_t>(sr_x) * static_cast<uint64_t>(sr_y),
                       count = objects.size() - sr_first;
        if (copies == 1 || count == 0) return;
        uint64_t block_points = 0;
        for (size_t k = sr_first; k < objects.size(); ++k) block_points += objects[k].count;
        // Checked before anything is copied, so that a huge request costs nothing.
        if (count > (MAX_OBJECTS - sr_first) / copies)
            throw ValueError("Step-and-repeat would draw more than " + std::to_string(MAX_OBJECTS) + " objects");
        if (block_points * (copies - 1) > MAX_POINTS - points.size())
            throw ValueError("Step-and-repeat would need more than " + std::to_string(MAX_POINTS) + " points");
        const size_t last = objects.size();
        for (int ix = 0; ix < sr_x; ++ix) {
            for (int iy = 0; iy < sr_y; ++iy) {
                if (!ix && !iy) continue;
                const double dx = sr_i * ix, dy = sr_j * iy;
                for (size_t k = sr_first; k < last; ++k) {
                    Object copy = objects[k];
                    const auto move = [&](Point &p) {
                        p.x += dx;
                        p.y += dy;
                        if (std::fabs(p.x) > MAX_COORDINATE || std::fabs(p.y) > MAX_COORDINATE)
                            throw ValueError("Step-and-repeat places an object outside -10000 to 10000 mm");
                    };
                    move(copy.start);
                    move(copy.end);
                    move(copy.centre);
                    copy.first = points.size();
                    for (size_t p = 0; p < copy.count; ++p) {
                        Point point = points[objects[k].first + p];
                        move(point);
                        points.push_back(point);
                    }
                    objects.push_back(copy);
                }
            }
        }
        point_set = false;  // the specification leaves the current point undefined after a block
    }

    // ---- apertures ----

    double size(double value, const std::string &what, int code) {
        const double scaled = value * scale();
        if (!(scaled >= 0 && scaled <= MAX_SIZE))
            refuse("aperture D" + std::to_string(code) + " has a " + what + " outside 0 to " + mm(MAX_SIZE) + " mm");
        return scaled;
    }

    void define_aperture(const std::string &rest) {
        size_t i = 0;
        if (i >= rest.size() || rest[i] != 'D')
            refuse("the aperture definition %AD" + printable(rest) + " is not understood");
        const size_t from = ++i;
        while (i < rest.size() && digit(rest[i])) ++i;
        if (i == from || i - from > 10) refuse("the aperture definition %AD" + printable(rest) + " is not understood");
        const double number = whole_number(rest, from, i);
        if (number < 10 || number > 2147483647.0) refuse("aperture numbers run from D10 to D2147483647");
        const int code = static_cast<int>(number);
        if (apertures.count(code)) refuse("aperture D" + std::to_string(code) + " is defined twice");
        if (apertures.size() == MAX_APERTURES)
            refuse("the file defines more than " + std::to_string(MAX_APERTURES) + " apertures");
        if (!units_set) refuse("an aperture is defined before the units (%MO)");
        const size_t comma = rest.find(',', i);
        const std::string name = rest.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        std::vector<double> values;
        if (comma != std::string::npos) {
            size_t at = comma + 1;
            while (true) {
                const size_t next = rest.find('X', at);
                const size_t end = next == std::string::npos ? rest.size() : next;
                double value = 0;
                if (!decimal(rest, at, end, value))
                    refuse("aperture D" + std::to_string(code) + " has a parameter that is not a number");
                if (values.size() == 1000)
                    refuse("aperture D" + std::to_string(code) + " has more than 1000 parameters");
                values.push_back(value);
                if (next == std::string::npos) break;
                at = next + 1;
            }
        }
        Aperture a;
        const auto count = [&](size_t low, size_t high) {
            if (values.size() < low) refuse("aperture D" + std::to_string(code) + " has too few parameters");
            if (values.size() > high)
                refuse("aperture D" + std::to_string(code) +
                       (name == "P" ? " has too many parameters" : " has a rectangular hole, which is not supported"));
        };
        if (name == "C") {
            count(1, 2);
            a.shape = "circle";
            a.diameter = size(values[0], "diameter", code);
            a.hole = values.size() > 1 ? size(values[1], "hole", code) : 0;
            if (a.hole > 0 && a.hole >= a.diameter)
                refuse("aperture D" + std::to_string(code) + " has a hole that does not fit inside it");
            a.box[2] = a.box[3] = a.diameter / 2;
        } else if (name == "R" || name == "O") {
            count(2, 3);
            a.shape = name == "R" ? "rectangle" : "obround";
            a.width = size(values[0], "width", code);
            a.height = size(values[1], "height", code);
            a.hole = values.size() > 2 ? size(values[2], "hole", code) : 0;
            if (a.hole > 0 && a.hole >= std::min(a.width, a.height))
                refuse("aperture D" + std::to_string(code) + " has a hole that does not fit inside it");
            if (a.width == 0 || a.height == 0) warnings.add("Aperture D" + std::to_string(code) + " has no area");
            a.box[2] = a.width / 2;
            a.box[3] = a.height / 2;
        } else if (name == "P") {
            count(2, 4);
            a.shape = "polygon";
            a.diameter = size(values[0], "diameter", code);
            if (values[1] != std::floor(values[1]) || values[1] < 3 || values[1] > 12)
                refuse("aperture D" + std::to_string(code) + " needs 3 to 12 vertices");
            a.vertices = static_cast<int>(values[1]);
            a.rotation = values.size() > 2 ? values[2] : 0;
            if (std::fabs(a.rotation) > 36000)
                refuse("aperture D" + std::to_string(code) + " has a rotation that is out of range");
            a.hole = values.size() > 3 ? size(values[3], "hole", code) : 0;
            // The hole has to fit inside the polygon's inscribed circle.
            if (a.hole > 0 && a.hole >= a.diameter * std::cos(PI / a.vertices))
                refuse("aperture D" + std::to_string(code) + " has a hole that does not fit inside it");
            if (a.diameter == 0) warnings.add("Aperture D" + std::to_string(code) + " has no area");
            a.box[2] = a.box[3] = a.diameter / 2;
        } else {
            const auto found = macros.find(name);
            if (found == macros.end())
                refuse("aperture D" + std::to_string(code) + " uses the macro " + printable(name) +
                       ", which is not defined");
            a.shape = "macro";
            a.macro = name;
            build(found->second, name, values, a);
        }
        a.box[0] = a.shape == "macro" ? a.box[0] : -a.box[2];
        a.box[1] = a.shape == "macro" ? a.box[1] : -a.box[3];
        apertures[code] = a;
    }

    // The macro's shapes for one aperture: its statements run in order with $1, $2, ... from the definition.
    void build(const Macro &macro, const std::string &name, const std::vector<double> &parameters, Aperture &a) {
        std::map<int, double> variables;
        for (size_t k = 0; k < parameters.size(); ++k) variables[static_cast<int>(k) + 1] = parameters[k];
        const auto decline = [&](const std::string &why) { refuse("aperture macro " + printable(name) + " " + why); };
        size_t polygon_points = 0;
        const auto add = [&](Part part) {
            if (a.parts.size() == MAX_PARTS) decline("makes more than " + std::to_string(MAX_PARTS) + " shapes");
            polygon_points += part.points.size();
            if (polygon_points > MAX_MACRO_POINTS)
                decline("makes more than " + std::to_string(MAX_MACRO_POINTS) + " polygon points");
            a.parts.push_back(std::move(part));
        };
        const double s = scale();
        const auto length = [&](double value) {
            if (!(value * s >= 0 && value * s <= MAX_SIZE)) decline("has a size outside 0 to " + mm(MAX_SIZE) + " mm");
            return value * s;
        };
        const auto place = [&](double x, double y, double rotation) {
            const Point p = rotated(Point{x * s, y * s}, rotation);
            if (!(std::fabs(p.x) <= MAX_SIZE && std::fabs(p.y) <= MAX_SIZE))
                decline("places a shape more than " + mm(MAX_SIZE) + " mm from its centre");
            return p;
        };
        const auto polygon = [&](bool exposure, const std::vector<Point> &corners) {
            Part part;
            part.exposure = exposure;
            part.points = corners;
            part.points.push_back(corners.front());
            add(std::move(part));
        };
        for (const std::string &statement : macro.blocks) {
            if (statement[0] == '$') {
                const size_t equals = statement.find('=');
                size_t k = 1;
                while (k < statement.size() && digit(statement[k])) ++k;
                if (equals == std::string::npos || k != equals || k == 1 || k > 6)
                    decline("has a statement that is not understood: " + printable(statement));
                const int index = static_cast<int>(whole_number(statement, 1, k));
                if (index < 1) decline("names a variable that is not understood");
                if (variables.size() == 1000 && !variables.count(index)) decline("uses more than 1000 variables");
                const std::string right = statement.substr(equals + 1);
                variables[index] = Expression(right, variables, name, line).whole();
                continue;
            }
            std::vector<double> v;
            size_t at = 0;
            while (true) {
                const size_t comma = statement.find(',', at);
                const std::string field =
                    statement.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (v.size() > 2 * 5001 + 4) decline("has a primitive with too many parameters");
                v.push_back(Expression(field, variables, name, line).whole());
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            const double code = v[0];
            const auto needs = [&](size_t low, size_t high) {
                if (v.size() < low || v.size() > high)
                    decline("gives primitive " + std::to_string(static_cast<int>(code)) +
                            " the wrong number of parameters");
            };
            const auto exposure = [&]() {
                if (v[1] != 0 && v[1] != 1) decline("has an exposure that is neither 0 nor 1");
                return v[1] == 1;
            };
            if (code == 1) {  // circle: exposure, diameter, centre X, centre Y, and optionally rotation
                needs(5, 6);
                Part part;
                part.circle = true;
                part.exposure = exposure();
                part.diameter = length(v[2]);
                part.centre = place(v[3], v[4], v.size() > 5 ? v[5] : 0);
                add(std::move(part));
            } else if (code == 20 || code == 2) {  // vector line: exposure, width, start, end, rotation
                needs(7, 8);
                const double width = length(v[2]), rotation = v.size() > 7 ? v[7] : 0;
                const double dx = v[5] - v[3], dy = v[6] - v[4], span = std::hypot(dx, dy);
                if (span == 0 || width == 0) continue;  // no area
                const double nx = -dy / span * width / 2 / s, ny = dx / span * width / 2 / s;
                polygon(exposure(), {place(v[3] - nx, v[4] - ny, rotation), place(v[5] - nx, v[6] - ny, rotation),
                                     place(v[5] + nx, v[6] + ny, rotation), place(v[3] + nx, v[4] + ny, rotation)});
            } else if (code == 21 || code == 22) {  // rectangle by its centre (21) or its lower left corner (22)
                needs(6, 7);
                const double w = v[2], h = v[3], rotation = v.size() > 6 ? v[6] : 0;
                length(w);
                length(h);
                if (w == 0 || h == 0) continue;
                const double x0 = code == 21 ? v[4] - w / 2 : v[4], y0 = code == 21 ? v[5] - h / 2 : v[5];
                polygon(exposure(), {place(x0, y0, rotation), place(x0 + w, y0, rotation),
                                     place(x0 + w, y0 + h, rotation), place(x0, y0 + h, rotation)});
            } else if (code == 4) {  // outline: exposure, vertices n, then n + 1 points and the rotation
                if (v.size() < 3) needs(3, 3);
                const double n = v[2];
                if (n != std::floor(n) || n < 3 || n > 5000) decline("has an outline that needs 3 to 5000 vertices");
                const size_t pairs = static_cast<size_t>(n) + 1;
                needs(3 + 2 * pairs, 4 + 2 * pairs);
                const double rotation = v.size() > 3 + 2 * pairs ? v[3 + 2 * pairs] : 0;
                std::vector<Point> corners;
                for (size_t k = 0; k < pairs; ++k) corners.push_back(place(v[3 + 2 * k], v[4 + 2 * k], rotation));
                if (distance(corners.front(), corners.back()) > CLOSED)
                    decline("has an outline whose last point is not its first");
                corners.pop_back();
                polygon(exposure(), corners);
            } else if (code == 5) {  // regular polygon: exposure, vertices, centre, diameter, rotation
                needs(6, 7);
                const double n = v[2], radius = v[5] / 2, rotation = v.size() > 6 ? v[6] : 0;
                if (n != std::floor(n) || n < 3 || n > 12) decline("has a polygon that needs 3 to 12 vertices");
                length(v[5]);
                if (radius == 0) continue;
                std::vector<Point> corners;
                for (int k = 0; k < static_cast<int>(n); ++k) {
                    const double angle = 2 * PI * k / n;  // the first vertex lies on the positive X axis
                    corners.push_back(
                        place(v[3] + radius * std::cos(angle), v[4] + radius * std::sin(angle), rotation));
                }
                polygon(exposure(), corners);
            } else if (code == 7) {  // thermal: centre, outer and inner diameter, gap, rotation
                needs(6, 7);
                const double outer = v[3], inner = v[4], gap = v[5], rotation = v.size() > 6 ? v[6] : 0;
                length(outer);
                if (!(inner >= 0 && inner < outer))
                    decline("has a thermal whose inner diameter is not smaller than its outer");
                if (!(gap >= 0 && gap < outer / std::sqrt(2.0)))
                    decline("has a thermal whose gap is too wide for its diameter");
                // A ring with a cross cut out of it, written as a disc and three removals.
                Part disc;
                disc.circle = true;
                disc.diameter = outer * s;
                disc.centre = place(v[1], v[2], rotation);
                add(disc);
                if (inner > 0) {
                    Part bore = disc;
                    bore.exposure = false;
                    bore.diameter = inner * s;
                    add(bore);
                }
                if (gap > 0) {
                    const double x = v[1], y = v[2], r = outer / 2, g = gap / 2;
                    polygon(false, {place(x - r, y - g, rotation), place(x + r, y - g, rotation),
                                    place(x + r, y + g, rotation), place(x - r, y + g, rotation)});
                    polygon(false, {place(x - g, y - r, rotation), place(x + g, y - r, rotation),
                                    place(x + g, y + r, rotation), place(x - g, y + r, rotation)});
                }
            } else if (code == 6) {
                decline("uses the moire primitive (6), which is not supported");
            } else {
                decline("uses primitive " +
                        (code == std::floor(code) && std::fabs(code) < 1e6 ? std::to_string(static_cast<int>(code))
                                                                           : std::string("?")) +
                        ", which is not known");
            }
        }
        // The extent of what the macro can expose.
        bool any = false;
        const auto cover = [&](double x0, double y0, double x1, double y1) {
            a.box[0] = any ? std::min(a.box[0], x0) : x0;
            a.box[1] = any ? std::min(a.box[1], y0) : y0;
            a.box[2] = any ? std::max(a.box[2], x1) : x1;
            a.box[3] = any ? std::max(a.box[3], y1) : y1;
            any = true;
        };
        for (const Part &part : a.parts) {
            if (!part.exposure) continue;
            if (part.circle)
                cover(part.centre.x - part.diameter / 2, part.centre.y - part.diameter / 2,
                      part.centre.x + part.diameter / 2, part.centre.y + part.diameter / 2);
            for (const Point &p : part.points) cover(p.x, p.y, p.x, p.y);
        }
        if (!any) warnings.add("Aperture macro " + printable(name) + " exposes nothing");
        all_parts += a.parts.size();
        all_macro_points += polygon_points;
        if (all_parts > MAX_ALL_PARTS || all_macro_points > MAX_ALL_MACRO_POINTS)
            refuse("the macro apertures together hold more than " + std::to_string(MAX_ALL_PARTS) + " shapes or " +
                   std::to_string(MAX_ALL_MACRO_POINTS) + " polygon points");
    }

    // ---- word commands: ...* ----

    void comment(const std::string &text) {
        // X2 attributes are also written as comments, for readers that predate them: G04 #@! TF.FileFunction,...
        const size_t mark = text.find("#@!");
        if (mark == std::string::npos) return;
        const size_t from = text.find_first_not_of(" \t", mark + 3);
        if (from != std::string::npos && text.compare(from, 2, "TF") == 0) {
            std::string rest = text.substr(from + 2);
            while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n' || rest.back() == ' ')) rest.pop_back();
            file_attribute(rest);
        }
    }

    // The whole number after a code letter.
    int code_after(const std::string &s, size_t &i) {
        const size_t from = i;
        while (i < s.size() && digit(s[i])) ++i;
        if (i == from || i - from > 10) refuse("the command " + printable(s) + " is not understood");
        const double value = whole_number(s, from, i);
        if (value > 2147483647.0) refuse("the command " + printable(s) + " is not understood");
        return static_cast<int>(value);
    }

    // A coordinate in the file's fixed format, in millimetres.
    double coordinate(const std::string &s, size_t &i) {
        if (!format_set) refuse("coordinates appear before the format statement (%FS)");
        if (!units_set) refuse("coordinates appear before the units (%MO)");
        bool negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
        const size_t from = i;
        uint64_t value = 0;
        while (i < s.size() && digit(s[i])) {
            if (i - from == 16) refuse("a coordinate has more than 16 digits");
            value = value * 10 + static_cast<uint64_t>(s[i++] - '0');
        }
        if (i == from) refuse("the command " + printable(s) + " is not understood");
        if (i < s.size() && s[i] == '.') refuse("a coordinate holds a decimal point, which Gerber does not allow");
        const int digits = static_cast<int>(i - from), all = integer_digits + decimal_digits;
        double number = static_cast<double>(value);
        if (trailing_omitted) {
            if (digits > all) refuse("a coordinate has more digits than the format statement allows");
            number *= power_of_ten(all - digits);
        }
        number = number / power_of_ten(decimal_digits) * scale();
        return negative ? -number : number;
    }

    void block(const std::string &raw) {
        const size_t from = raw.find_first_not_of(" \t\r\n");
        if (from == std::string::npos) return;  // a stray asterisk
        if (raw.compare(from, 3, "G04") == 0 ||
            (raw.compare(from, 2, "G4") == 0 && (from + 2 == raw.size() || !digit(raw[from + 2])))) {
            comment(raw);
            return;
        }
        if (raw.size() - from > MAX_BLOCK)
            refuse("a command is longer than " + std::to_string(MAX_BLOCK) + " characters");
        std::string s;
        for (size_t k = from; k < raw.size(); ++k)
            if (raw[k] != ' ' && raw[k] != '\t' && raw[k] != '\r' && raw[k] != '\n') s.push_back(raw[k]);
        bool has_x = false, has_y = false, has_i = false, has_j = false, moves = false;
        double x = 0, y = 0, ci = 0, cj = 0;
        int operation = 0;
        size_t i = 0;
        while (i < s.size()) {
            const char letter = s[i++];
            if (letter == 'G') {
                const int g = code_after(s, i);
                if (g == 4) return;  // the rest of the block is a comment
                if (g == 1 || g == 2 || g == 3) {
                    mode = g;
                } else if (g == 36) {
                    open_region();
                } else if (g == 37) {
                    close_region();
                } else if (g == 70 || g == 71) {
                    set_units(g == 70);
                } else if (g == 74 || g == 75) {
                    quadrant = g == 74 ? 1 : 2;
                } else if (g == 90 || g == 91) {
                    set_notation(g == 91);
                } else if (g != 54 && g != 55) {  // those two are deprecated prefixes of a selection and a flash
                    refuse("the code G" + std::to_string(g) + " is not supported");
                }
            } else if (letter == 'X' || letter == 'Y' || letter == 'I' || letter == 'J') {
                const double value = coordinate(s, i);
                bool &seen = letter == 'X' ? has_x : letter == 'Y' ? has_y : letter == 'I' ? has_i : has_j;
                if (seen) refuse("the command " + printable(s) + " gives " + std::string(1, letter) + " twice");
                seen = true;
                (letter == 'X' ? x : letter == 'Y' ? y : letter == 'I' ? ci : cj) = value;
                moves = true;
            } else if (letter == 'D') {
                const int d = code_after(s, i);
                if (d >= 10) {
                    if (region) refuse("an aperture is selected inside a region");
                    if (!apertures.count(d)) refuse("aperture D" + std::to_string(d) + " is selected but not defined");
                    selected = d;
                } else if (d >= 1 && d <= 3) {
                    if (operation) refuse("the command " + printable(s) + " has two operation codes");
                    operation = d;
                } else {
                    refuse("the code D" + std::to_string(d) + " is not supported");
                }
            } else if (letter == 'M') {
                const int m = code_after(s, i);
                if (m == 2 || m == 0) {
                    if (region) refuse("the file ends inside a region (G36 without G37)");
                    close_repeat();
                    ended = true;
                    return;
                }
                if (m != 1) refuse("the code M" + std::to_string(m) + " is not supported");
            } else {
                refuse("the command " + printable(s) + " is not understood");
            }
        }
        if (!moves && !operation) return;
        if (!operation) {
            // Deprecated: coordinates alone repeat a preceding D01, and only a D01.
            if (last_operation != 1) refuse("coordinates are given without an operation code (D01, D02 or D03)");
            operation = 1;
            warnings.add("Coordinates without an operation code were read as D01, a deprecated usage");
        }
        last_operation = operation;
        Point target = current;
        if (incremental) {
            target.x += has_x ? x : 0;
            target.y += has_y ? y : 0;
        } else {
            if (has_x) target.x = x;
            if (has_y) target.y = y;
        }
        if (!(std::fabs(target.x) <= MAX_COORDINATE && std::fabs(target.y) <= MAX_COORDINATE))
            refuse("a coordinate lies outside -10000 to 10000 mm");
        if (operation == 2) {
            if (region) close_contour();
        } else if (operation == 3) {
            flash(target);
        } else {
            draw(target, has_i ? ci : 0, has_j ? cj : 0);
        }
        current = target;
        point_set = true;
    }

    void set_units(bool inches) {
        if (units_set && inches != inch) warnings.add("The file changes units part way through");
        inch = inches;
        units_set = true;
    }

    void set_notation(bool relative) {
        if (relative && !incremental) warnings.add("The file uses incremental coordinates, a deprecated notation");
        incremental = relative;
    }

    const Aperture &current_aperture(const char *what) {
        if (!selected) refuse(std::string(what) + " before any aperture is selected");
        return apertures.find(selected)->second;
    }

    void flash(const Point &at) {
        if (region) refuse("a flash (D03) inside a region");
        current_aperture("a flash is made");
        room(1, 0);
        Object object;
        object.type = 'F';
        object.dark = dark;
        object.aperture = selected;
        object.start = object.end = at;
        objects.push_back(object);
    }

    // The centre of an arc from the current point, by the quadrant mode in force.
    Point arc_centre(const Point &target, double ci, double cj, bool clockwise, bool &whole) {
        if (!quadrant) refuse("an arc is drawn before G74 or G75 selects the quadrant mode");
        const bool same = current.x == target.x && current.y == target.y;
        Point centre{current.x + ci, current.y + cj};
        whole = quadrant == 2 && same;
        if (quadrant == 1) {
            // Single quadrant: the offsets carry no sign. Of the four candidates, take the one that turns the
            // right way through no more than a quarter turn and fits both ends best.
            bool found = false;
            double best = 0;
            for (int k = 0; k < 4; ++k) {
                const Point c{current.x + ((k & 1) ? -std::fabs(ci) : std::fabs(ci)),
                              current.y + ((k & 2) ? -std::fabs(cj) : std::fabs(cj))};
                const double a0 = std::atan2(current.y - c.y, current.x - c.x),
                             a1 = std::atan2(target.y - c.y, target.x - c.x);
                double sweep = std::fmod(clockwise ? a0 - a1 : a1 - a0, 2 * PI);
                if (sweep < 0) sweep += 2 * PI;
                if (sweep > PI / 2 + 1e-6) continue;
                const double misfit = std::fabs(distance(current, c) - distance(target, c));
                if (!found || misfit < best) {
                    found = true;
                    best = misfit;
                    centre = c;
                }
            }
            if (!found) refuse("no single-quadrant arc fits the centre offsets I and J");
        }
        const double r0 = distance(current, centre), r1 = distance(target, centre), radius = std::max(r0, r1);
        const double misfit = std::fabs(r0 - r1);
        if (misfit > 0.25 * radius) refuse("an arc's centre is not the same distance from its two ends");
        if (misfit > 0.01) {
            ++deviant;
            worst = std::max(worst, misfit);
        }
        return centre;
    }

    void draw(const Point &target, double ci, double cj) {
        if (!point_set) warnings.add("A line is drawn before any position is set; it starts from the origin");
        if (!mode) {
            warnings.add("Lines are drawn before G01 selects straight moves; straight moves were assumed");
            mode = 1;
        }
        const bool arc = mode != 1, clockwise = mode == 2;
        bool whole = false;
        Point centre;
        if (arc) centre = arc_centre(target, ci, cj, clockwise, whole);
        // In single-quadrant mode an arc that ends where it starts has no length, and nor has one with no radius.
        const bool curved =
            arc && (whole || current.x != target.x || current.y != target.y) && distance(current, centre) > 0;
        if (region) {
            if (contour.empty()) contour.push_back(current);
            if (curved)
                flatten(current, target, centre, clockwise, whole, tolerance, contour);
            else
                contour.push_back(target);
            if (contour.size() > MAX_POINTS) room(0, contour.size());
            return;
        }
        const Aperture &aperture = current_aperture("a line is drawn");
        if (aperture.shape != "circle" && (curved || aperture.shape != "rectangle"))
            refuse(std::string(curved ? "an arc" : "a line") + " is drawn with aperture D" + std::to_string(selected) +
                   " (" + aperture.shape + "); only a circle" + (curved ? "" : " or a rectangle") + " may be used");
        if (aperture.hole > 0)
            refuse("a line is drawn with aperture D" + std::to_string(selected) + ", which has a hole");
        if (aperture.shape == "rectangle")
            warnings.add("Lines are drawn with a rectangular aperture, a deprecated usage");
        Object object;
        object.type = curved ? 'A' : 'L';
        object.dark = dark;
        object.aperture = selected;
        object.start = current;
        object.end = target;
        if (curved) {
            object.centre = centre;
            object.clockwise = clockwise;
            std::vector<Point> flat{current};
            flatten(current, target, centre, clockwise, whole, tolerance, flat);
            room(1, flat.size());
            object.first = points.size();
            object.count = flat.size();
            points.insert(points.end(), flat.begin(), flat.end());
        } else {
            room(1, 0);
        }
        objects.push_back(object);
    }

    void open_region() {
        if (region) refuse("a region is opened inside a region");
        region = true;
        contour.clear();
    }

    void close_region() {
        if (!region) refuse("a region is closed (G37) without having been opened (G36)");
        close_contour();
        region = false;
    }

    void close_contour() {
        if (contour.empty()) return;
        if (distance(contour.front(), contour.back()) > CLOSED) refuse("a region contour is not closed");
        contour.back() = contour.front();
        if (contour.size() < 4) {
            warnings.add("A region contour with no area was left out");
        } else {
            room(1, contour.size());
            Object object;
            object.type = 'R';
            object.dark = dark;
            object.first = points.size();
            object.count = contour.size();
            points.insert(points.end(), contour.begin(), contour.end());
            objects.push_back(object);
        }
        contour.clear();
    }
};

double arc_tolerance(const json &request) {
    return setting(request, "arc_tolerance", 0.005, "Arc tolerance", 0.0005, 0.1, "0.0005", "0.1");
}

json path_json(const std::vector<Point> &points, size_t first, size_t count) {
    json out = json::array();
    for (size_t k = 0; k < count; ++k) out.push_back(xy(points[first + k]));
    return out;
}

json aperture_json(int code, const Aperture &a) {
    json out{{"code", code}, {"shape", a.shape}};
    if (a.shape == "circle") {
        out["diameter"] = round6(a.diameter);
        out["hole"] = round6(a.hole);
    } else if (a.shape == "rectangle" || a.shape == "obround") {
        out["width"] = round6(a.width);
        out["height"] = round6(a.height);
        out["hole"] = round6(a.hole);
    } else if (a.shape == "polygon") {
        out["diameter"] = round6(a.diameter);
        out["vertices"] = a.vertices;
        out["rotation"] = a.rotation;
        out["hole"] = round6(a.hole);
    } else {
        out["macro"] = printable(a.macro, 80);
        json parts = json::array();
        for (const Part &part : a.parts) {
            if (part.circle)
                parts.push_back({{"shape", "circle"},
                                 {"exposure", part.exposure},
                                 {"diameter", round6(part.diameter)},
                                 {"x", round6(part.centre.x)},
                                 {"y", round6(part.centre.y)}});
            else
                parts.push_back({{"shape", "polygon"},
                                 {"exposure", part.exposure},
                                 {"points", path_json(part.points, 0, part.points.size())}});
        }
        out["parts"] = parts;
    }
    return out;
}

}  // namespace

json gerber_import(const json &request) {
    const std::string &text = file_text(request, "Gerber", MAX_TEXT, {"arc_tolerance"});
    Reader reader;
    reader.tolerance = arc_tolerance(request);
    reader.read(text);

    json apertures = json::array();
    for (const auto &entry : reader.apertures) apertures.push_back(aperture_json(entry.first, entry.second));

    json objects = json::array();
    size_t flashes = 0, lines = 0, arcs = 0, regions = 0;
    bool any = false;
    double bounds[4] = {0, 0, 0, 0};
    const auto cover = [&](double x0, double y0, double x1, double y1) {
        bounds[0] = any ? std::min(bounds[0], x0) : x0;
        bounds[1] = any ? std::min(bounds[1], y0) : y0;
        bounds[2] = any ? std::max(bounds[2], x1) : x1;
        bounds[3] = any ? std::max(bounds[3], y1) : y1;
        any = true;
    };
    for (const Object &object : reader.objects) {
        const char *polarity = object.dark ? "dark" : "clear";
        // The bounds are those of the dark objects with their apertures; a clear object cannot extend the image.
        const double *box = object.type == 'R' ? nullptr : reader.apertures.find(object.aperture)->second.box;
        const auto stamp = [&](const Point &p) {
            if (object.dark) cover(p.x + box[0], p.y + box[1], p.x + box[2], p.y + box[3]);
        };
        if (object.type == 'F') {
            ++flashes;
            stamp(object.start);
            objects.push_back(
                {{"type", "flash"}, {"polarity", polarity}, {"aperture", object.aperture}, {"at", xy(object.start)}});
        } else if (object.type == 'L') {
            ++lines;
            stamp(object.start);
            stamp(object.end);
            objects.push_back({{"type", "line"},
                               {"polarity", polarity},
                               {"aperture", object.aperture},
                               {"start", xy(object.start)},
                               {"end", xy(object.end)}});
        } else if (object.type == 'A') {
            ++arcs;
            for (const Point &p : extremes(object.start, object.end, object.centre, object.clockwise)) stamp(p);
            objects.push_back({{"type", "arc"},
                               {"polarity", polarity},
                               {"aperture", object.aperture},
                               {"start", xy(object.start)},
                               {"end", xy(object.end)},
                               {"centre", xy(object.centre)},
                               {"clockwise", object.clockwise},
                               {"points", path_json(reader.points, object.first, object.count)}});
        } else {
            ++regions;
            if (object.dark)
                for (size_t k = 0; k < object.count; ++k)
                    cover(reader.points[object.first + k].x, reader.points[object.first + k].y,
                          reader.points[object.first + k].x, reader.points[object.first + k].y);
            objects.push_back({{"type", "region"},
                               {"polarity", polarity},
                               {"points", path_json(reader.points, object.first, object.count)}});
        }
    }
    const auto function = reader.attributes.find(".FileFunction");
    const auto negative = reader.attributes.find(".FilePolarity");
    if (negative != reader.attributes.end() && negative->second == "Negative")
        reader.warnings.add(
            "The file is marked as a negative image (.FilePolarity): what it draws is where material is absent");
    json attributes = json::object();
    for (const auto &entry : reader.attributes) attributes[entry.first] = entry.second;
    return json{
        {"format", "gerber"},
        {"units", reader.inch ? "inch" : "mm"},
        {"coordinate_format",
         {{"integer", reader.integer_digits},
          {"decimal", reader.decimal_digits},
          {"omitted_zeros", reader.trailing_omitted ? "trailing" : "leading"},
          {"notation", reader.declared_incremental ? "incremental" : "absolute"}}},
        {"file_function", function == reader.attributes.end() ? json(nullptr) : json(function->second)},
        {"attributes", attributes},
        {"arc_tolerance", reader.tolerance},
        {"apertures", apertures},
        {"objects", objects},
        {"counts", {{"flash", flashes}, {"line", lines}, {"arc", arcs}, {"region", regions}}},
        {"bounds", any ? json::array({round6(bounds[0]), round6(bounds[1]), round6(bounds[2]), round6(bounds[3])})
                       : json(nullptr)},
        {"warnings", reader.warnings.list},
    };
}

json gerber_outline(const json &request) {
    const std::string &text =
        file_text(request, "Gerber", MAX_TEXT, {"arc_tolerance", "tolerance", "offset", "mirror_x", "width", "height"});
    Reader reader;
    reader.tolerance = arc_tolerance(request);
    const double tolerance = setting(request, "tolerance", 0.01, "Joining tolerance", 0.000001, 1, "0.000001", "1");
    const Placement place = placement_of(request);
    double width = 0, height = 0;
    const bool fit = work_area(request, width, height);
    reader.read(text);

    // Each drawn line or arc is a run of points with two ends.
    struct Run {
        std::vector<Point> points;
        size_t a = 0, b = 0;  // the joints at its ends
        bool used = false;
    };
    std::vector<Run> runs;
    size_t flashes = 0, regions = 0, clear = 0, dots = 0, repeats = 0;
    double line_width = 0;
    for (const Object &object : reader.objects) {
        if (object.type == 'F')
            ++flashes;
        else if (object.type == 'R')
            ++regions;
        else if (!object.dark)
            ++clear;
        else {
            Run run;
            if (object.type == 'L')
                run.points = {object.start, object.end};
            else
                run.points.assign(reader.points.begin() + static_cast<std::ptrdiff_t>(object.first),
                                  reader.points.begin() + static_cast<std::ptrdiff_t>(object.first + object.count));
            double span = 0;
            for (size_t k = 1; k < run.points.size(); ++k) span += distance(run.points[k - 1], run.points[k]);
            if (span <= tolerance) {
                ++dots;
                continue;
            }
            const Aperture &aperture = reader.apertures.find(object.aperture)->second;
            line_width = std::max(line_width, std::max(aperture.diameter, std::max(aperture.width, aperture.height)));
            runs.push_back(std::move(run));
        }
    }
    if (runs.empty()) throw ValueError("The layer holds no lines or arcs to make a board outline from");

    // Ends within the tolerance of each other are one joint. A grid of tolerance-sized cells finds them.
    struct Joint {
        Point at;
        std::vector<size_t> runs;
    };
    std::vector<Joint> joints;
    std::unordered_map<uint64_t, std::vector<size_t>> cells;
    const auto cell = [&](double v) { return static_cast<int64_t>(std::floor(v / tolerance)); };
    const auto key = [](int64_t cx, int64_t cy) {
        return static_cast<uint64_t>(cx) * 0x9E3779B97F4A7C15ULL + static_cast<uint64_t>(cy);
    };
    const auto joint = [&](const Point &p) {
        const int64_t cx = cell(p.x), cy = cell(p.y);
        size_t best = joints.size();
        double nearest = tolerance;
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                const auto found = cells.find(key(cx + dx, cy + dy));
                if (found == cells.end()) continue;
                for (const size_t index : found->second) {
                    const double d = distance(joints[index].at, p);
                    if (d <= nearest) {
                        nearest = d;
                        best = index;
                    }
                }
            }
        }
        if (best == joints.size()) {
            joints.push_back(Joint{p, {}});
            cells[key(cx, cy)].push_back(best);
        }
        return best;
    };
    std::vector<Run> kept;
    std::map<std::pair<size_t, size_t>, bool> straight;  // the two-point runs seen, by their joints
    for (Run &run : runs) {
        run.a = joint(run.points.front());
        run.b = joint(run.points.back());
        if (run.points.size() == 2) {
            // The same edge drawn twice is one edge.
            const std::pair<size_t, size_t> ends{std::min(run.a, run.b), std::max(run.a, run.b)};
            if (run.a == run.b || straight.count(ends)) {
                ++repeats;
                continue;
            }
            straight[ends] = true;
        }
        kept.push_back(std::move(run));
    }
    for (size_t k = 0; k < kept.size(); ++k) {
        joints[kept[k].a].runs.push_back(k);
        if (kept[k].b != kept[k].a) joints[kept[k].b].runs.push_back(k);
    }
    // Every joint of a closed outline has exactly two ends at it. A run that closes on itself has one joint.
    for (const Joint &j : joints) {
        const std::string where = "X" + mm(j.at.x) + " Y" + mm(j.at.y);
        size_t ends = 0;
        for (const size_t k : j.runs) ends += kept[k].a == kept[k].b ? 2u : 1u;
        if (ends == 1) {
            double nearest = -1;
            size_t examined = 0;
            for (const Joint &other : joints) {
                if (&other == &j || other.runs.size() != 1 || kept[other.runs[0]].a == kept[other.runs[0]].b) continue;
                const double d = distance(other.at, j.at);
                if (nearest < 0 || d < nearest) nearest = d;
                if (++examined == 5000) break;
            }
            throw ValueError(
                "The board outline is open at " + where +
                (nearest < 0 ? std::string() : "; the nearest other loose end is " + mm(nearest) + " mm away") +
                ". Close the outline, or raise the joining tolerance if the gap is a drawing error");
        }
        if (ends > 2)
            throw ValueError("The board outline branches at " + where + ": " + std::to_string(ends) +
                             " lines meet there");
    }

    struct Loop {
        std::vector<Point> points;
        double area = 0;
    };
    std::vector<Loop> loops;
    size_t vertices = 0;
    for (size_t first = 0; first < kept.size(); ++first) {
        if (kept[first].used) continue;
        Loop loop;
        size_t index = first, from = kept[first].a;
        loop.points.push_back(kept[first].points.front());
        while (true) {
            Run &run = kept[index];
            run.used = true;
            const bool forward = run.a == from;
            const size_t n = run.points.size();
            for (size_t k = 1; k < n; ++k) loop.points.push_back(run.points[forward ? k : n - 1 - k]);
            const size_t to = forward ? run.b : run.a;
            if (to == kept[first].a) break;
            const std::vector<size_t> &there = joints[to].runs;
            index = there[0] == index ? there[1] : there[0];
            from = to;
        }
        loop.points.back() = loop.points.front();
        for (size_t k = 1; k < loop.points.size(); ++k)
            loop.area += loop.points[k - 1].x * loop.points[k].y - loop.points[k].x * loop.points[k - 1].y;
        loop.area = std::fabs(loop.area) / 2;
        vertices += loop.points.size();
        loops.push_back(std::move(loop));
    }
    if (loops.size() > 3000) throw ValueError("The board outline has more than 3000 separate loops");
    if (vertices > 100000)
        throw ValueError("The board outline has more than 100000 vertices; use a coarser arc tolerance");
    std::stable_sort(loops.begin(), loops.end(), [](const Loop &p, const Loop &q) { return p.area > q.area; });

    bool any = false;
    double bounds[4] = {0, 0, 0, 0};
    json paths = json::array();
    for (const Loop &loop : loops) {
        json path = json::array();
        for (const Point &p : loop.points) {
            const Point moved = place(p);
            bounds[0] = any ? std::min(bounds[0], moved.x) : moved.x;
            bounds[1] = any ? std::min(bounds[1], moved.y) : moved.y;
            bounds[2] = any ? std::max(bounds[2], moved.x) : moved.x;
            bounds[3] = any ? std::max(bounds[3], moved.y) : moved.y;
            any = true;
            path.push_back(xy(moved));
        }
        paths.push_back(path);
    }
    if (fit && (bounds[0] < 0 || bounds[1] < 0 || bounds[2] > width || bounds[3] > height))
        throw ValueError("The board outline does not fit the work area with this offset: it spans X" + mm(bounds[0]) +
                         " to X" + mm(bounds[2]) + " and Y" + mm(bounds[1]) + " to Y" + mm(bounds[3]));
    const auto left_out = [&](size_t count, const char *one, const char *many) {
        if (count)
            reader.warnings.add(std::to_string(count) + " " + (count == 1 ? one : many) + " left out of the outline");
    };
    left_out(flashes, "flash was", "flashes were");
    left_out(regions, "filled region was", "filled regions were");
    left_out(clear, "clear line was", "clear lines were");
    left_out(dots, "line with no length was", "lines with no length were");
    left_out(repeats, "repeated line was", "repeated lines were");
    if (loops.size() > 1)
        reader.warnings.add("The outline has " + std::to_string(loops.size()) +
                            " closed loops; the largest is first and the others may be cut-outs inside it");
    return json{{"paths", paths},
                {"line_width", round6(line_width)},
                {"bounds", json::array({round6(bounds[0]), round6(bounds[1]), round6(bounds[2]), round6(bounds[3])})},
                {"warnings", reader.warnings.list}};
}

}  // namespace kerfkite
