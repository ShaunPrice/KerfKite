// Excellon import: see kerfkite/excellon.hpp.
//
// The reader takes the common subset that board design programs write: a header with units and tools, drill
// hits, repeats, G85 slots and straight routed slots. Anything else that would move the tool is refused with
// the reason, in preference to being skipped.
#include "kerfkite/excellon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pcb_text.hpp"

namespace kerfkite {

using json = nlohmann::json;
using namespace detail;
using namespace pcb;

namespace {

const size_t MAX_TEXT = 8u << 20;
const size_t MAX_HITS = 100000;
const size_t MAX_SLOTS = 20000;
const size_t MAX_SLOT_POINTS = 100000;
const size_t MAX_TOOLS = 1000;
const size_t MAX_LINE = 1024;
const double MAX_REPEAT = 10000;
const double MAX_DIAMETER = 100;  // millimetres, the largest drill the generator accepts

const char *UNSTATED_UNITS =
    "The drill file does not state its units (METRIC, INCH, M71 or M72); give \"units\" in the request";

struct Tool {
    int number = 0;
    double raw = 0;  // the diameter as written, in the file's units
    std::vector<Point> hits;
};

struct Slot {
    int tool = 0;
    std::vector<Point> points;
};

struct Word {
    char letter;
    std::string text;
};

std::string upper(const std::string &text) {
    std::string out = text;
    for (char &c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// "3:3" at the start of text[at...]: the digits either side of the implied decimal point.
bool digit_pair(const std::string &text, size_t at, int &whole, int &fraction) {
    if (at + 2 >= text.size() || !digit(text[at]) || text[at + 1] != ':' || !digit(text[at + 2])) return false;
    whole = text[at] - '0';
    fraction = text[at + 2] - '0';
    return true;
}

class Drill {
  public:
    std::vector<Tool> tools;
    std::vector<Slot> slots;
    Warnings warnings;
    int units = 0;  // 1 millimetres, 2 inches; 0 until stated
    int integer = -1, fraction = -1;
    char omitted = 0;  // 'L' leading zeros omitted, 'T' trailing zeros omitted; 0 until stated
    int units_rank = 0, digits_rank = 0, zeros_rank = 0;
    bool fixed_read = false, zeros_mattered = false, point_read = false;
    int plated = -1;
    bool has_function = false;
    std::string file_function;
    size_t hit_count = 0;

    // What the request states outright takes precedence over the file.
    void overrides(const json &request) {
        if (const json *value = get(request, "units")) {
            if (is_text(value, "mm"))
                units = 1;
            else if (is_text(value, "inch"))
                units = 2;
            else
                throw ValueError("Units must be \"mm\" or \"inch\"");
            units_rank = REQUEST;
        }
        const json *whole = get(request, "integer"), *part = get(request, "decimal");
        if (whole || part) {
            if (!whole || !part) throw ValueError("Give both \"integer\" and \"decimal\" digits, or neither");
            const double i = number(whole, "Integer digits", 0, 7, "0", "7"),
                         d = number(part, "Decimal digits", 0, 7, "0", "7");
            if (i != std::floor(i) || d != std::floor(d))
                throw ValueError("Integer and decimal digits must be whole numbers");
            integer = static_cast<int>(i);
            fraction = static_cast<int>(d);
            digits_rank = REQUEST;
        }
        if (const json *value = get(request, "omitted_zeros")) {
            if (is_text(value, "leading"))
                omitted = 'L';
            else if (is_text(value, "trailing"))
                omitted = 'T';
            else
                throw ValueError("Omitted zeros must be \"leading\" or \"trailing\"");
            zeros_rank = REQUEST;
        }
    }

    void read(const std::string &text) {
        size_t at = after_mark(text);
        while (at <= text.size() && !ended) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            ++line;
            if (end - at > MAX_LINE) refuse("is longer than " + std::to_string(MAX_LINE) + " characters");
            size_t from = at, to = end;
            while (from < to && (text[from] == ' ' || text[from] == '\t' || text[from] == '\r')) ++from;
            while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\r')) --to;
            if (to > from) statement(text.substr(from, to - from));
            at = end + 1;
        }
        if (down) throw ValueError("The drill file ends with the routing tool still down (M15 without M16)");
        if (!ended) warnings.add("The file has no end-of-program code (M30); it may be truncated");
        if (!units && !tools.empty()) throw ValueError(UNSTATED_UNITS);
        for (const Tool &tool : tools) {
            const double diameter = tool.raw * scale();
            if (!(diameter > 0 && diameter <= MAX_DIAMETER))
                throw ValueError("Tool T" + std::to_string(tool.number) + " has a diameter of " + mm(diameter) +
                                 " mm; it must be above 0 and at most " + mm(MAX_DIAMETER) + " mm");
        }
        if (fixed_read && digits_rank == 0)
            warnings.add(
                "The file does not state its number format; coordinates without a decimal point were read with " +
                std::to_string(integer) + " digits before the point and " + std::to_string(fraction) +
                " after. State \"integer\" and \"decimal\" in the request if this is wrong");
        if (zeros_mattered && zeros_rank == 0)
            warnings.add(
                "The file does not say which zeros its coordinates omit; trailing zeros were taken as omitted, the "
                "Excellon default. State "
                "\"omitted_zeros\" in the request if this is wrong");
    }

    double scale() const { return units == 2 ? INCH : 1.0; }

    static const char *source(int rank, bool used) {
        return !used             ? "unused"
               : rank == REQUEST ? "request"
               : rank == HEADER  ? "header"
               : rank == COMMENT ? "comment"
                                 : "default";
    }

    json interpretation() const {
        json out{{"units", units == 2 ? "inch" : "mm"}, {"units_from", source(units_rank, true)}};
        out["integer"] = fixed_read ? json(integer) : json(nullptr);
        out["decimal"] = fixed_read ? json(fraction) : json(nullptr);
        out["omitted_zeros"] = fixed_read ? json(omitted == 'L' ? "leading" : "trailing") : json(nullptr);
        // Coordinates with a decimal point need no format.
        const char *unused = point_read ? "decimal points" : "unused";
        out["digits_from"] = fixed_read ? source(digits_rank, true) : unused;
        out["zeros_from"] = fixed_read ? source(zeros_rank, true) : unused;
        return out;
    }

    const Tool *tool(int number) const {
        for (const Tool &t : tools)
            if (t.number == number) return &t;
        return nullptr;
    }

  private:
    enum { COMMENT = 1, HEADER = 2, REQUEST = 3 };
    size_t line = 0;
    bool header = false, ended = false, body_started = false;
    bool routing = false, down = false, cutting = false, has_hit = false;
    int selected = 0;
    size_t selected_index = 0, slot_points = 0;
    Point position;
    std::vector<Point> path;

    [[noreturn]] void refuse(const std::string &why) const {
        throw ValueError("Line " + std::to_string(line) + " " + why);
    }

    // ---- the number format ----

    void set_units(int value, int rank) {
        if (rank < units_rank) {
            if (value != units) warnings.add("The request's units differ from those the file states");
            return;
        }
        if ((fixed_read || point_read || hit_count) && value != units)
            refuse("changes the units after coordinates have been read");
        units = value;
        units_rank = rank;
    }

    void set_digits(int whole, int part, int rank) {
        if (rank < digits_rank) {
            if (whole != integer || part != fraction)
                warnings.add(rank == COMMENT && digits_rank == HEADER
                                 ? "A comment and the header give different number formats; the header's was used"
                                 : "The request's number format differs from the one the file states");
            return;
        }
        if (fixed_read && (whole != integer || part != fraction))
            refuse("changes the number format after coordinates have been read");
        integer = whole;
        fraction = part;
        digits_rank = rank;
    }

    void set_zeros(char value, int rank) {
        if (rank < zeros_rank) {
            if (value != omitted) warnings.add("The request's zero suppression differs from the one the file states");
            return;
        }
        if (fixed_read && value != omitted) refuse("changes the zero suppression after coordinates have been read");
        omitted = value;
        zeros_rank = rank;
    }

    // A coordinate or a step as written, in millimetres.
    double length(const std::string &text) {
        if (!units) throw ValueError(UNSTATED_UNITS);
        double value = 0;
        if (text.find('.') != std::string::npos) {
            if (!decimal(text, value)) refuse("has a number that is not understood: " + printable(text));
            point_read = true;
            return value * scale();
        }
        size_t i = 0;
        bool negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
        const int digits = static_cast<int>(text.size() - i);
        if (digits < 1 || digits > 15) refuse("has a number that is not understood: " + printable(text));
        for (size_t k = i; k < text.size(); ++k)
            if (!digit(text[k])) refuse("has a number that is not understood: " + printable(text));
        // Settle the format at the first number that needs it: the description's defaults where nothing is stated.
        if (integer < 0) {
            integer = units == 2 ? 2 : 3;
            fraction = units == 2 ? 4 : 3;
        }
        if (!omitted) omitted = 'T';
        fixed_read = true;
        const int all = integer + fraction;
        if (digits != all) zeros_mattered = true;
        value = whole_number(text, i, text.size());
        if (omitted == 'T') {
            // Leading zeros are present: the digits start at the first integer place.
            if (digits > all)
                refuse("has a number with more digits than the " + std::to_string(integer) + "." +
                       std::to_string(fraction) + " format allows");
            value *= power_of_ten(all - digits);
        }
        value = value / power_of_ten(fraction) * scale();
        return negative ? -value : value;
    }

    // ---- lines ----

    void comment(const std::string &text) {
        const std::string u = upper(text);
        int whole = 0, part = 0;
        size_t at = u.find("FILE_FORMAT=");
        if (at != std::string::npos && digit_pair(u, at + 12, whole, part)) set_digits(whole, part, COMMENT);
        // KiCad: ; FORMAT={3:3/ absolute / metric / suppress leading zeros}, or {-:-/ ... / decimal}
        at = u.find("FORMAT={");
        if (at != std::string::npos) {
            if (digit_pair(u, at + 8, whole, part)) set_digits(whole, part, COMMENT);
            if (u.find("SUPPRESS LEADING ZEROS", at) != std::string::npos)
                set_zeros('L', COMMENT);
            else if (u.find("SUPPRESS TRAILING ZEROS", at) != std::string::npos)
                set_zeros('T', COMMENT);
        }
        if (u.find("TYPE=NON_PLATED") != std::string::npos)
            plated = 0;
        else if (u.find("TYPE=PLATED") != std::string::npos)
            plated = 1;
        at = text.find("TF.FileFunction,");
        if (at != std::string::npos && text.find("#@!") != std::string::npos) {
            file_function = printable(text.substr(at + 16), 200);
            has_function = true;
            if (file_function.compare(0, 9, "NonPlated") == 0)
                plated = 0;
            else if (file_function.compare(0, 6, "Plated") == 0)
                plated = 1;
        }
    }

    // METRIC,LZ,000.000 and the other header words.
    void keyword(const std::string &u) {
        std::vector<std::string> fields;
        size_t at = 0;
        while (true) {
            const size_t comma = u.find(',', at);
            fields.push_back(u.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        const std::string &name = fields[0];
        if (name == "INCH" || name == "METRIC") {
            set_units(name == "INCH" ? 2 : 1, HEADER);
            for (size_t k = 1; k < fields.size(); ++k) {
                const std::string &field = fields[k];
                // LZ: leading zeros are present, so trailing ones are omitted. TZ is the reverse.
                if (field == "LZ")
                    set_zeros('T', HEADER);
                else if (field == "TZ")
                    set_zeros('L', HEADER);
                else if (!field.empty() && field.find_first_not_of("0.") == std::string::npos &&
                         std::count(field.begin(), field.end(), '.') == 1) {
                    const size_t point = field.find('.');
                    if (point > 7 || field.size() - point - 1 > 7) refuse("gives a number format with too many digits");
                    set_digits(static_cast<int>(point), static_cast<int>(field.size() - point - 1), HEADER);
                } else {
                    warnings.add("Header word not understood and left out: " + printable(field));
                }
            }
        } else if (name == "FMAT") {
            if (fields.size() != 2 || fields[1] != "2")
                refuse("selects a format other than FMAT,2, which is not supported");
        } else if (name == "ICI") {
            if (fields.size() != 2 || fields[1] != "OFF")
                refuse("selects incremental coordinates (ICI), which are not supported");
        } else if (name == "VER" || name == "DETECT" || name == "ATC" || name == "TCST" || name == "OSTOP" ||
                   name == "NCSL" || name == "BLKD" || name == "SBK" || name == "SG" || name == "RTR") {
            // Machine settings that do not move a hole.
        } else if (header) {
            warnings.add("Header line not understood and left out: " + printable(u));
        } else {
            refuse("is not understood: " + printable(u));
        }
    }

    bool split(const std::string &u, std::vector<Word> &words) const {
        words.clear();
        size_t i = 0;
        while (i < u.size()) {
            const char letter = u[i];
            if (letter < 'A' || letter > 'Z') return false;
            const size_t from = ++i;
            while (i < u.size() && (digit(u[i]) || u[i] == '.' || u[i] == '+' || u[i] == '-')) ++i;
            words.push_back(Word{letter, u.substr(from, i - from)});
        }
        return !words.empty();
    }

    // The whole number after a code letter, or -1.
    static int code(const Word &word, size_t most) {
        if (word.text.empty() || word.text.size() > most) return -1;
        for (const char c : word.text)
            if (!digit(c)) return -1;
        return static_cast<int>(whole_number(word.text, 0, word.text.size()));
    }

    void statement(const std::string &text) {
        if (text[0] == ';') {
            comment(text);
            return;
        }
        const std::string u = upper(text);
        if (u == "%") {
            // The end of the header. Before the header it is a rewind stop, which some programs write first.
            if (header)
                header = false;
            else if (body_started)
                refuse("repeats a pattern (% in the body), which is not supported");
            return;
        }
        if (u.compare(0, 3, "M47") == 0) return;  // a message for the operator
        size_t letters = 0;
        while (letters < u.size() && u[letters] >= 'A' && u[letters] <= 'Z') ++letters;
        if (letters >= 2 && (letters == u.size() || u[letters] == ',')) {
            keyword(u);
            return;
        }
        std::vector<Word> words;
        if (!split(u, words)) refuse("is not understood: " + printable(text));
        const Word &first = words[0];
        switch (first.letter) {
            case 'T':
                tool_line(words, text);
                break;
            case 'M':
                m_code(words, text);
                break;
            case 'G':
            case 'X':
            case 'Y':
                motion(words, text);
                break;
            case 'R':
                repeat(words, text);
                break;
            case 'F':
            case 'S':
            case 'B':
            case 'H':
            case 'Z':
                break;  // feeds, speeds and depths are set on the machine, not taken from the file
            default:
                refuse("is not understood: " + printable(text));
        }
    }

    void tool_line(const std::vector<Word> &words, const std::string &text) {
        const int number = code(words[0], 4);
        if (number < 0) refuse("names a tool that is not understood: " + printable(text));
        bool sized = false;
        double raw = 0;
        for (size_t k = 1; k < words.size(); ++k) {
            const char letter = words[k].letter;
            if (letter == 'C') {
                if (sized || !decimal(words[k].text, raw))
                    refuse("has a tool diameter that is not understood: " + printable(text));
                sized = true;
            } else if (letter != 'F' && letter != 'S' && letter != 'B' && letter != 'H' && letter != 'Z') {
                refuse("is not understood: " + printable(text));
            }
        }
        if (sized) {
            if (number < 1) refuse("defines tool T0, which is not a tool");
            if (!(raw > 0)) refuse("defines tool T" + std::to_string(number) + " with no diameter");
            const Tool *known = tool(number);
            if (known && known->raw != raw)
                refuse("defines tool T" + std::to_string(number) + " a second time with another diameter");
            if (!known) {
                if (tools.size() == MAX_TOOLS) refuse("defines more than " + std::to_string(MAX_TOOLS) + " tools");
                Tool added;
                added.number = number;
                added.raw = raw;
                tools.push_back(added);
            }
            if (header) return;
        } else if (header) {
            warnings.add("Tool T" + std::to_string(number) + " is named in the header without a diameter");
            return;
        }
        if (down) refuse("changes the tool while routing (M15 without M16)");
        if (number && !tool(number)) refuse("selects tool T" + std::to_string(number) + ", which is not defined");
        selected = number;
        if (number) selected_index = static_cast<size_t>(tool(number) - tools.data());
        has_hit = false;
    }

    void m_code(const std::vector<Word> &words, const std::string &text) {
        const int m = code(words[0], 3);
        if (m == 48)
            header = true;
        else if (m == 95)
            header = false;
        else if (m == 30 || m == 0)
            ended = true;
        else if (m == 71 || m == 72)
            set_units(m == 71 ? 1 : 2, HEADER);
        else if (m == 15) {
            if (!routing) refuse("lowers the routing tool (M15) outside routing mode (G00)");
            if (!selected) refuse("routes before a tool is selected");
            down = true;
            path.assign(1, position);
        } else if (m == 16 || m == 17) {
            lift();
        } else if (m == 6 || m == 9) {
            // Tool change and inspection stops.
        } else if (m == 25 || m == 1 || m == 2 || m == 8 || m == 24 || m == 26) {
            refuse("uses pattern and step-and-repeat codes (M25, M01, M02, M08), which are not supported");
        } else if (m == 97 || m == 98) {
            refuse("drills text (M97, M98), which is not supported");
        } else {
            refuse("is not understood: " + printable(text));
        }
    }

    void lift() {
        if (!down) return;
        down = false;
        if (path.size() >= 2) {
            if (slots.size() == MAX_SLOTS)
                throw ValueError("The drill file has more than " + std::to_string(MAX_SLOTS) + " slots");
            Slot slot;
            slot.tool = selected;
            slot.points = path;
            slots.push_back(slot);
        }
        path.clear();
    }

    Point moved(const Point &from, bool has_x, double x, bool has_y, double y) const {
        Point to = from;
        if (has_x) to.x = x;
        if (has_y) to.y = y;
        if (!(std::fabs(to.x) <= MAX_COORDINATE && std::fabs(to.y) <= MAX_COORDINATE))
            refuse("has a coordinate outside -10000 to 10000 mm");
        return to;
    }

    void hit(const Point &at) {
        if (!selected) refuse("drills before a tool is selected");
        if (hit_count == MAX_HITS)
            throw ValueError("The drill file has more than " + std::to_string(MAX_HITS) + " holes");
        tools[selected_index].hits.push_back(at);
        ++hit_count;
        has_hit = true;
    }

    // G codes and coordinates: a hole, a G85 slot, or a move in routing mode.
    void motion(const std::vector<Word> &words, const std::string &text) {
        bool has[4] = {false, false, false, false};  // X, Y before G85 and X, Y after it
        double value[4] = {0, 0, 0, 0};
        bool slot = false, moves = false;
        int g = -1;
        for (size_t k = 0; k < words.size(); ++k) {
            const Word &word = words[k];
            if (word.letter == 'X' || word.letter == 'Y') {
                const size_t index = (slot ? 2u : 0u) + (word.letter == 'Y' ? 1u : 0u);
                if (has[index]) refuse("gives " + std::string(1, word.letter) + " twice: " + printable(text));
                has[index] = true;
                value[index] = length(word.text);
                moves = true;
            } else if (word.letter == 'G') {
                const int number = code(word, 3);
                if (number == 85) {
                    if (slot) refuse("is not understood: " + printable(text));
                    slot = true;
                } else if (k == 0 && number >= 0) {
                    g = number;
                } else {
                    refuse("is not understood: " + printable(text));
                }
            } else if (word.letter != 'F' && word.letter != 'S') {
                refuse("is not understood: " + printable(text));
            }
        }
        if (g == 90 || g == 40) {
            // Absolute coordinates and no cutter compensation: what this reader assumes.
        } else if (g == 91) {
            refuse("selects incremental coordinates (G91), which are not supported");
        } else if (g == 5) {
            if (down) refuse("returns to drilling with the routing tool down (M15 without M16)");
            routing = false;
        } else if (g == 0 || g == 1) {
            if (g == 0 && down) refuse("makes a rapid move (G00) with the routing tool down");
            routing = true;
            cutting = g == 1;
        } else if (g == 2 || g == 3) {
            refuse("routes an arc (G02, G03), which is not supported");
        } else if (g == 41 || g == 42) {
            refuse("uses cutter compensation (G41, G42), which is not supported");
        } else if (g == 81 || g == 82 || g == 83 || g == 84) {
            refuse("uses a canned cycle (G81 to G84), which is not supported");
        } else if (g == 93) {
            refuse("sets a zero offset (G93), which is not supported");
        } else if (g != -1) {
            refuse("uses the code G" + std::to_string(g) + ", which is not supported");
        }
        if (!moves) {
            if (slot) refuse("is not understood: " + printable(text));
            return;
        }
        body_started = true;
        const Point to = moved(position, has[0], value[0], has[1], value[1]);
        if (slot) {
            if (routing) refuse("drills a slot (G85) in routing mode");
            if (!selected) refuse("drills before a tool is selected");
            const Point end = moved(to, has[2], value[2], has[3], value[3]);
            if (slots.size() == MAX_SLOTS)
                throw ValueError("The drill file has more than " + std::to_string(MAX_SLOTS) + " slots");
            Slot made;
            made.tool = selected;
            made.points = {to, end};
            slots.push_back(made);
            position = end;
            has_hit = false;
        } else if (!routing) {
            hit(to);
            position = to;
        } else {
            if (cutting) {
                if (!down) refuse("routes (G01) without the tool down (M15)");
                if (++slot_points > MAX_SLOT_POINTS)
                    throw ValueError("The drill file routes more than " + std::to_string(MAX_SLOT_POINTS) + " points");
                path.push_back(to);
            } else if (down) {
                refuse("makes a rapid move (G00) with the routing tool down");
            }
            position = to;
        }
    }

    // R5X1.0: the last hole again, that many more times, each one step further on.
    void repeat(const std::vector<Word> &words, const std::string &text) {
        const int count = code(words[0], 5);
        if (count < 1 || count > MAX_REPEAT) refuse("repeats a hole a number of times outside 1 to 10000");
        if (routing || !has_hit) refuse("repeats a hole (R) with no hole before it");
        double dx = 0, dy = 0;
        bool has_x = false, has_y = false;
        for (size_t k = 1; k < words.size(); ++k) {
            const Word &word = words[k];
            if (word.letter == 'X' && !has_x) {
                has_x = true;
                dx = length(word.text);
            } else if (word.letter == 'Y' && !has_y) {
                has_y = true;
                dy = length(word.text);
            } else {
                refuse("is not understood: " + printable(text));
            }
        }
        if (!has_x && !has_y) refuse("repeats a hole (R) without a step");
        if (static_cast<size_t>(count) > MAX_HITS - hit_count)
            throw ValueError("The drill file has more than " + std::to_string(MAX_HITS) + " holes");
        for (int k = 0; k < count; ++k) {
            position = moved(position, true, position.x + dx, true, position.y + dy);
            hit(position);
        }
    }
};

Drill drill_from(const json &request, bool placed) {
    const std::string &text =
        placed ? file_text(request, "drill", MAX_TEXT,
                           {"units", "integer", "decimal", "omitted_zeros", "offset", "width", "height"})
               : file_text(request, "drill", MAX_TEXT, {"units", "integer", "decimal", "omitted_zeros"});
    Drill drill;
    drill.overrides(request);
    drill.read(text);
    return drill;
}

json path_json(const std::vector<Point> &points, const Point &offset) {
    json out = json::array();
    for (const Point &p : points) out.push_back(xy(Point{p.x + offset.x, p.y + offset.y}));
    return out;
}

// The extent of the hole and slot edges, moved by the offset.
struct Extent {
    bool any = false;
    double box[4] = {0, 0, 0, 0};
    void cover(const Point &p, double radius) {
        box[0] = any ? std::min(box[0], p.x - radius) : p.x - radius;
        box[1] = any ? std::min(box[1], p.y - radius) : p.y - radius;
        box[2] = any ? std::max(box[2], p.x + radius) : p.x + radius;
        box[3] = any ? std::max(box[3], p.y + radius) : p.y + radius;
        any = true;
    }
    json to_json() const {
        return any ? json::array({round6(box[0]), round6(box[1]), round6(box[2]), round6(box[3])}) : json(nullptr);
    }
};

json slots_json(const Drill &drill, const Point &offset, Extent &extent) {
    json out = json::array();
    for (const Slot &slot : drill.slots) {
        const double diameter = drill.tool(slot.tool)->raw * drill.scale();
        for (const Point &p : slot.points) extent.cover(Point{p.x + offset.x, p.y + offset.y}, diameter / 2);
        out.push_back(
            {{"tool", slot.tool}, {"diameter", round6(diameter)}, {"points", path_json(slot.points, offset)}});
    }
    return out;
}

}  // namespace

json excellon_import(const json &request) {
    const Drill drill = drill_from(request, false);
    Extent extent;
    json tools = json::array();
    for (const Tool &tool : drill.tools) {
        const double diameter = tool.raw * drill.scale();
        for (const Point &p : tool.hits) extent.cover(p, diameter / 2);
        tools.push_back(
            {{"number", tool.number}, {"diameter", round6(diameter)}, {"hits", path_json(tool.hits, Point{})}});
    }
    const json slots = slots_json(drill, Point{}, extent);
    return json{
        {"format", "excellon"},
        {"units", drill.units == 2 ? "inch" : "mm"},
        {"interpretation", drill.interpretation()},
        {"plated", drill.plated < 0 ? json(nullptr) : json(drill.plated == 1)},
        {"file_function", drill.has_function ? json(drill.file_function) : json(nullptr)},
        {"tools", tools},
        {"slots", slots},
        {"counts", {{"hits", drill.hit_count}, {"slots", drill.slots.size()}}},
        {"bounds", extent.to_json()},
        {"warnings", drill.warnings.list},
    };
}

json excellon_drills(const json &request) {
    Drill drill = drill_from(request, true);
    const Point offset = offset_of(request);
    double width = 0, height = 0;
    const bool fit = work_area(request, width, height);
    if (!drill.hit_count) throw ValueError("The drill file holds no holes");

    // One group for each diameter, smallest first; two tools of one diameter drill together.
    struct Group {
        std::vector<int> tools;
        std::vector<Point> holes;
    };
    std::map<double, Group> groups;
    Extent extent;
    for (const Tool &tool : drill.tools) {
        if (tool.hits.empty()) continue;
        const double diameter = round6(tool.raw * drill.scale());
        if (diameter < 0.01)
            throw ValueError("Tool T" + std::to_string(tool.number) +
                             " is finer than 0.01 mm, the smallest drill the generator accepts");
        Group &group = groups[diameter];
        group.tools.push_back(tool.number);
        for (const Point &p : tool.hits) {
            const Point moved{p.x + offset.x, p.y + offset.y};
            extent.cover(moved, diameter / 2);
            group.holes.push_back(moved);
        }
    }
    if (fit && (extent.box[0] < 0 || extent.box[1] < 0 || extent.box[2] > width || extent.box[3] > height))
        throw ValueError("The holes do not fit the work area with this offset: their edges span X" + mm(extent.box[0]) +
                         " to X" + mm(extent.box[2]) + " and Y" + mm(extent.box[1]) + " to Y" + mm(extent.box[3]));
    json operations = json::array(), summary = json::array();
    for (const auto &entry : groups) {
        const Group &group = entry.second;
        summary.push_back({{"tool_diameter", entry.first}, {"holes", group.holes.size()}, {"tools", group.tools}});
        // A project holds at most 3000 paths, so a larger group becomes several operations with the same drill.
        for (size_t first = 0; first < group.holes.size(); first += 3000) {
            json paths = json::array();
            for (size_t k = first; k < std::min(group.holes.size(), first + 3000); ++k) {
                const json point = xy(group.holes[k]);
                paths.push_back(json::array({point, point}));  // a point marker: a path that stays where it starts
            }
            operations.push_back(
                {{"operation", "drill"}, {"tool_shape", "drill"}, {"tool_diameter", entry.first}, {"paths", paths}});
        }
    }
    Extent unused;  // the bounds are those of the holes these operations drill
    const json slots = slots_json(drill, offset, unused);
    if (!drill.slots.empty())
        drill.warnings.add(std::to_string(drill.slots.size()) + (drill.slots.size() == 1 ? " slot is" : " slots are") +
                           " not drilled; each is listed with its centre line and width, to be cut as a line");
    if (groups.size() > 1)
        drill.warnings.add("The holes need " + std::to_string(groups.size()) +
                           " drill diameters; run each diameter as its own job and change the drill between them");
    return json{{"operations", operations},
                {"groups", summary},
                {"slots", slots},
                {"bounds", extent.to_json()},
                {"interpretation", drill.interpretation()},
                {"warnings", drill.warnings.list}};
}

}  // namespace kerfkite
