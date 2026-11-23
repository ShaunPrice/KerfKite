// Fixture clearance: the port of studio/fixture_collision.py. See kerfkite/fixture_collision.hpp.
#include "kerfkite/fixture_collision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/py_math.hpp"
#include "project.hpp"
#include "python_text.hpp"

namespace kerfkite {
namespace {

using json = Json;
using detail::get;
using detail::py_max;
using detail::py_min;
using Vector = std::array<double, 3>;

constexpr size_t kMaxEvents = 100;
constexpr double kMaxChecks = 2000000;

// _number(value, field, low, high): low_text and high_text are the bounds as the reference writes them.
double number(const json *value, const std::string &field, double low, double high, const char *low_text,
              const char *high_text) {
    double result = 0;
    if (!value || value->is_boolean() || !detail::plain_number(*value, result) || !(low <= result && result <= high))
        throw ValueError(field + " must be a finite number from " + low_text + " to " + high_text);
    return result;
}

Vector vector_of(const json *value, const std::string &field) {
    if (!value || !value->is_array() || value->size() != 3) throw ValueError(field + " needs three coordinates in millimetres");
    Vector out;
    for (size_t i = 0; i < 3; ++i) out[i] = number(&value->at(i), field, -1000000, 1000000, "-1000000", "1000000");
    return out;
}

struct Interval {
    double entry, leave;
};

// _slab: the part of the interval of time in which low <= origin + delta * t <= high.
bool slab(Interval &interval, double origin, double delta, double low, double high) {
    if (delta == 0) return low <= origin && origin <= high;
    const double a = (low - origin) / delta, b = (high - origin) / delta;
    const double entry = py_max(interval.entry, py_min(a, b)), leave = py_min(interval.leave, py_max(a, b));
    if (!(entry <= leave)) return false;
    interval = {entry, leave};
    return true;
}

// _contact: the first instant of the move at which the cylinder from bottom to top above the tool tip touches the
// box, or false.
bool contact(const Vector &start, const Vector &delta, const Vector &low, const Vector &high, double radius,
             double bottom, double top, double &first) {
    Interval interval{0.0, 1.0};
    if (!slab(interval, start[2], delta[2], low[2] - top, high[2] - bottom)) return false;
    // The XY travel must cross the bounding rectangle of the rounded footprint.
    for (int axis = 0; axis < 2; ++axis)
        if (!slab(interval, start[axis], delta[axis], low[axis] - radius, high[axis] + radius)) return false;
    std::vector<double> contacts;
    // The footprint is two rectangular strips and four corner discs.
    for (int axis = 0; axis < 2; ++axis) {
        Interval strip = interval;
        if (slab(strip, start[axis], delta[axis], low[axis], high[axis])) contacts.push_back(strip.entry);
    }
    const double dx = delta[0], dy = delta[1];
    const double speed = pymath::hypot(dx, dy);
    for (const double x : {low[0], high[0]}) {
        for (const double y : {low[1], high[1]}) {
            const double ox = start[0] - x, oy = start[1] - y;
            if (speed == 0) {
                if (pymath::hypot(ox, oy) <= radius) contacts.push_back(interval.entry);
                continue;
            }
            const double ux = dx / speed, uy = dy / speed;
            const double lateral = std::fabs(oy * ux - ox * uy);
            if (lateral > radius) continue;
            const double half = std::sqrt((radius - lateral) * (radius + lateral));
            const double centre = -(ox * ux + oy * uy);
            const double entry = py_max(interval.entry, (centre - half) / speed);
            if (entry <= py_min(interval.leave, (centre + half) / speed)) contacts.push_back(entry);
        }
    }
    if (contacts.empty()) return false;
    first = contacts[0];
    for (size_t i = 1; i < contacts.size(); ++i) first = py_min(first, contacts[i]);
    return true;
}

// Python's str.split() and str.strip() for ASCII text: whitespace is \t \n \v \f \r, \x1c to \x1f and space.
bool ascii_space(unsigned char c) { return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20); }

// WORD.fullmatch(word): a letter, then [-+]? (\d+(\.\d*)? | \.\d+).
bool word_value(const std::string &word, char &letter, std::string &number) {
    if (word.size() < 2 || word[0] < 'A' || word[0] > 'Z') return false;
    size_t i = 1;
    if (word[i] == '+' || word[i] == '-') ++i;
    const auto digit = [&](size_t k) { return k < word.size() && word[k] >= '0' && word[k] <= '9'; };
    const size_t from = i;
    while (digit(i)) ++i;
    if (i > from) {
        if (i < word.size() && word[i] == '.') {
            ++i;
            while (digit(i)) ++i;
        }
    } else {
        if (i >= word.size() || word[i] != '.' || !digit(i + 1)) return false;
        ++i;
        while (digit(i)) ++i;
    }
    if (i != word.size()) return false;
    letter = word[0];
    number = word.substr(1);
    return true;
}

struct Move {
    size_t line;
    std::string motion;
    Vector origin, destination;
};

// _moves: reads the explicit commands Studio posts, one line at a time, as the reference's generator gives them.
class Program {
public:
    Program(const json *lines, const Vector &start) : lines_(lines), position_(start) {
        if (!lines || !lines->is_array() || lines->size() < 1 || lines->size() > 250000)
            throw ValueError("Check a generated program with 1–250000 lines");
    }

    bool next(Move &move) {
        while (index_ < lines_->size()) {
            const size_t number = ++index_;
            const json &raw = lines_->at(number - 1);
            const auto reject = [&](const std::string &reason) {
                throw ValueError("Cannot check line " + std::to_string(number) + ": " + reason);
            };
            if (!raw.is_string()) reject("invalid line");
            const std::string &given = raw.get_ref<const std::string &>();
            const bool ascii = std::all_of(given.begin(), given.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
            // A line that is not ASCII is read in its ASCII rendering after str.upper() (python_text.hpp), and a
            // message quotes its words as the reference does, from the upper-cased line.
            std::vector<std::string> shown;
            std::string rendered;
            if (!ascii) {
                if (detail::code_points(given).size() > 256) reject("invalid line");
                const std::string upper = detail::python_upper(given);
                shown = detail::python_split(upper);
                rendered = detail::python_ascii(upper, false);
            } else if (given.size() > 256) {
                reject("invalid line");
            }
            const std::string &text = ascii ? given : rendered;
            std::vector<std::string> words;
            for (size_t i = 0; i < text.size();) {
                while (i < text.size() && ascii_space(static_cast<unsigned char>(text[i]))) ++i;
                const size_t from = i;
                while (i < text.size() && !ascii_space(static_cast<unsigned char>(text[i]))) ++i;
                if (i > from) {
                    std::string word = text.substr(from, i - from);
                    for (char &c : word)
                        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                    words.push_back(std::move(word));
                }
            }
            if (words.empty()) continue;
            if (ended_) reject("instructions follow program end");
            const std::string &op = words[0];
            if (op == "G21" && words.size() == 1) {
                metric_ = true;
                continue;
            }
            if (op == "G90" && words.size() == 1) {
                absolute_ = true;
                continue;
            }
            if ((op == "G17" || op == "G94" || op == "G54" || op == "G40" || op == "G49" || op == "G80" || op == "G61" ||
                 op == "M5" || op == "M30") &&
                words.size() == 1) {
                ended_ = op == "M30";
                continue;
            }
            const char *allowed = op == "G0"   ? "XYZ"
                                  : op == "G1" ? "XYZFS"
                                  : op == "G4" ? "P"
                                  : op == "M3" || op == "M4" ? "S"
                                                             : nullptr;
            if (!allowed) reject("unsupported instruction " + (ascii ? op : shown[0]));
            bool has[26] = {};
            double values[26] = {};
            for (size_t k = 1; k < words.size(); ++k) {
                char letter = 0;
                std::string digits;
                if (!word_value(words[k], letter, digits) || std::string(allowed).find(letter) == std::string::npos ||
                    has[letter - 'A'])
                    reject("unsupported or duplicate word " + (ascii ? words[k] : shown[k]));
                double value = 0;
                detail::float_from_text(digits, value);
                if (!std::isfinite(value) || std::fabs(value) > 1000000) reject("value outside supported range");
                has[letter - 'A'] = true;
                values[letter - 'A'] = value;
            }
            if (op != "G0" && op != "G1") {
                if (op == "G4" && !has['P' - 'A']) reject("missing dwell interval");
                continue;
            }
            if (!metric_ || !absolute_) reject("expected G21 millimetres and G90 absolute positioning before motion");
            if (!has['X' - 'A'] && !has['Y' - 'A'] && !has['Z' - 'A']) reject("missing XYZ coordinates");
            Vector destination = position_;
            for (int axis = 0; axis < 3; ++axis)
                if (has['X' - 'A' + axis]) destination[axis] = values['X' - 'A' + axis];
            move = {number, op, position_, destination};
            position_ = destination;
            return true;
        }
        return false;
    }

private:
    const json *lines_;
    Vector position_;
    size_t index_ = 0;
    bool metric_ = false, absolute_ = false, ended_ = false;
};

struct Component {
    const char *name;
    double radius, bottom, top;
};

struct Box {
    std::string name;
    Vector low, high;
};

}  // namespace

Json check_fixtures(const Json &payload) {
    if (!payload.is_object()) throw ValueError("Supply a posted program and fixture setup");
    const Vector start = vector_of(get(payload, "start"), "Initial tool-tip XYZ");
    const json *tool = get(payload, "tool");
    if (!tool || !tool->is_object()) throw ValueError("Supply the mounted tool and holder dimensions");
    double sizes[4];
    const char *keys[4] = {"diameter", "stickout", "holder_diameter", "holder_length"};
    for (int i = 0; i < 4; ++i) sizes[i] = number(get(*tool, keys[i]), keys[i], 0.001, 2000, "0.001", "2000");
    const double diameter = sizes[0], stickout = sizes[1], holder = sizes[2], length = sizes[3];
    const Component components[2] = {{"tool", diameter / 2, 0.0, stickout},
                                     {"holder", holder / 2, stickout, stickout + length}};
    const json *fixtures = get(payload, "fixtures");
    if (!fixtures || !fixtures->is_array() || fixtures->size() < 1 || fixtures->size() > 64)
        throw ValueError("Supply between 1 and 64 fixture boxes");
    std::vector<Box> boxes;
    for (const json &fixture : *fixtures) {
        if (!fixture.is_object()) throw ValueError("Invalid fixture box");
        const json *name = get(fixture, "name");
        const bool named = name && name->is_string();
        const std::string text = named ? name->get_ref<const std::string &>() : std::string();
        if (named) {
            const std::vector<detail::CodePoint> points = detail::code_points(text);
            if (detail::python_strip(text, points).empty() || points.size() > 80)
                throw ValueError("Give each fixture a name of 1–80 characters");
        } else {
            throw ValueError("Give each fixture a name of 1–80 characters");
        }
        Box box{text, vector_of(get(fixture, "min"), text + " minimum corner"),
                vector_of(get(fixture, "max"), text + " maximum corner")};
        for (int axis = 0; axis < 3; ++axis)
            if (box.low[axis] >= box.high[axis]) throw ValueError(text + " requires a positive size on every axis");
        boxes.push_back(std::move(box));
    }

    struct Hit {
        double t;
        size_t box;
        const char *component;
        json event;
    };
    json::Array events;
    int64_t count = 0, moves = 0;
    Program program(get(payload, "lines"), start);
    Move move;
    while (program.next(move)) {
        ++moves;
        if (double(moves) * double(boxes.size()) * 2 > kMaxChecks)
            throw ValueError("Program/fixture combination exceeds two million sweep checks");
        Vector delta;
        for (int axis = 0; axis < 3; ++axis) delta[axis] = move.destination[axis] - move.origin[axis];
        std::vector<Hit> hits;
        for (size_t index = 0; index < boxes.size(); ++index) {
            const Box &box = boxes[index];
            for (const Component &part : components) {
                double t = 0;
                if (!contact(move.origin, delta, box.low, box.high, part.radius, part.bottom, part.top, t)) continue;
                ++count;
                if (events.size() >= kMaxEvents) continue;
                json point = json::array();
                for (int axis = 0; axis < 3; ++axis)
                    point.push_back(detail::rounded(move.origin[axis] + t * delta[axis], 5));
                json event = json::object();
                event["line"] = static_cast<int64_t>(move.line);
                event["motion"] = move.motion;
                event["component"] = part.name;
                event["fixture"] = box.name;
                event["position"] = std::move(point);
                hits.push_back({t, index, part.name, std::move(event)});
            }
        }
        // sorted(hits): by the instant, then the fixture's place, then the part's name ('holder' before 'tool').
        std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
            if (a.t < b.t || b.t < a.t) return a.t < b.t;
            if (a.box != b.box) return a.box < b.box;
            return std::string(a.component) < std::string(b.component);
        });
        for (Hit &hit : hits) {
            if (events.size() >= kMaxEvents) break;
            events.push_back(std::move(hit.event));
        }
    }
    if (!moves) throw ValueError("Program contains no supported XYZ motion");
    json result = json::object();
    result["collision"] = count != 0;
    result["events"] = json(std::move(events));
    result["contact_count"] = count;
    result["moves_checked"] = moves;
    result["fixtures_checked"] = static_cast<int64_t>(boxes.size());
    return result;
}

bool fixture_command(const std::string &command) { return command == "fixture-collision"; }

Json fixture_call(const std::string &, const Json &request) { return check_fixtures(request); }

}  // namespace kerfkite
