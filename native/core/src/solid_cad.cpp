// The parametric solid history: studio/solid_cad.py's rebuild and horizontal_section, statement for statement
// where the order of the checks or of the kernel's operations shows in the result.
//
// The Python builds a model by asking the solid kernel for one operation at a time through the "solid"
// command (studio/solid_kernel.py), keeping each solid for the next request. This file asks the same
// command for the same operations, with the same requests, so the solids, measures and meshes are the
// same to the bit. What is reproduced: models of boxes, cylinders and spheres, placed by rotation and
// origin, repeated by linear and polar patterns, and combined in runs of additions, cuts and
// intersections; named parameters and their expressions; the mesh, measures and silhouette of rebuild();
// horizontal sections. Models with sketched, lofted, scripted or STEP features are declined (NotPorted),
// as are dimension expressions outside numbers, names, + - * /, unary signs, parentheses and the sketch
// crossing functions, which the reference reads with Python's own parser.
#include "kerfkite/solid_cad.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/polygon_command.hpp"
#include "kerfkite/py_math.hpp"
#include "kerfkite/solid_command.hpp"
#include "project.hpp"
#include "py_trig.hpp"
#include "python_text.hpp"
#include "simple_line.hpp"

namespace kerfkite {

using json = Json;
using namespace detail;

namespace {

const double kMaxValue = 100000;
const double kMaxFaces = 75000;
const int kSegments = 96;
const char *const kRange = "Dimensions must be finite and within ±100000";
const char *const kSyntaxSubset = "Use numbers, named parameters, parentheses and + − * / in dimensions";
const char *const kTooFine = "too fine for the 1 nm grid";

// _number(value): a number within range, as a float.
double number_of(const json *value) {
    if (!value || value->is_boolean() || !value->is_number()) throw ValueError(kRange);
    const double result = value->get<double>();
    if (!(-kMaxValue <= result && result <= kMaxValue)) throw ValueError(kRange);
    return result;
}

double ranged(double value) {
    if (!(-kMaxValue <= value && value <= kMaxValue)) throw ValueError(kRange);
    return value;
}

// ---- dimension expressions -----------------------------------------------------------------------

// A node of the expression as Python's ast module would read it, for the subset reproduced here.
struct Node {
    enum Kind { Constant, Name, Unary, Binary, Call } kind = Constant;
    // Constant: its value, or that it is not a number _number accepts (True, False, None, an integer out
    // of range); Name and Call: the name.
    bool usable = true;
    double value = 0;
    std::string name;
    char op = 0;
    std::vector<int> children;
};

struct Token {
    enum Kind { Number, Word, Symbol, End } kind = End;
    std::string text;
};

bool digit(char c) { return c >= '0' && c <= '9'; }
bool word_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool word_char(char c) { return word_start(c) || digit(c); }

[[noreturn]] void decline() { throw NotPorted("a dimension expression the native core does not read the way Python's parser does"); }

// Digits with single underscores between them, from text[i]; the end, or i when there are none.
size_t digit_part(const std::string &text, size_t i) {
    if (i >= text.size() || !digit(text[i])) return i;
    size_t j = i + 1;
    while (j < text.size()) {
        if (digit(text[j])) ++j;
        else if (text[j] == '_' && j + 1 < text.size() && digit(text[j + 1])) j += 2;
        else break;
    }
    return j;
}

// The tokens of an expression whose every character is one of those read here; anything else is declined.
std::vector<Token> tokens_of(const std::string &text) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (digit(c) || (c == '.' && i + 1 < text.size() && digit(text[i + 1]))) {
            size_t j = digit_part(text, i);
            bool real = false;
            if (j < text.size() && text[j] == '.') {
                real = true;
                j = digit_part(text, j + 1);
            }
            if (j < text.size() && (text[j] == 'e' || text[j] == 'E')) {
                size_t k = j + 1;
                if (k < text.size() && (text[k] == '+' || text[k] == '-')) ++k;
                const size_t end = digit_part(text, k);
                if (end == k) decline();
                real = true;
                j = end;
            }
            // A number run into a name, a further point or an underscore is read by Python's tokenizer in ways
            // that changed between releases.
            if (j < text.size() && (word_char(text[j]) || text[j] == '.')) decline();
            std::string literal;
            for (size_t k = i; k < j; ++k)
                if (text[k] != '_') literal += text[k];
            if (!real) {
                // Decimal integers may not have leading zeros unless they are all zeros.
                if (literal.size() > 1 && literal[0] == '0' && literal.find_first_not_of('0') != std::string::npos) decline();
            }
            tokens.push_back({Token::Number, (real ? "r" : "i") + literal});
            i = j;
            continue;
        }
        if (word_start(c)) {
            size_t j = i;
            while (j < text.size() && word_char(text[j])) ++j;
            tokens.push_back({Token::Word, text.substr(i, j - i)});
            i = j;
            continue;
        }
        if (c == '+' || c == '-' || c == '(' || c == ')' || c == ',') {
            tokens.push_back({Token::Symbol, std::string(1, c)});
            ++i;
            continue;
        }
        if (c == '*' || c == '/') {
            if (i + 1 < text.size() && text[i + 1] == c) decline();  // ** and //
            tokens.push_back({Token::Symbol, std::string(1, c)});
            ++i;
            continue;
        }
        decline();
    }
    tokens.push_back({Token::End, ""});
    return tokens;
}

const std::set<std::string> &keywords() {
    static const std::set<std::string> words = {"and", "as", "assert", "async", "await", "break", "class", "continue",
                                                "def", "del", "elif", "else", "except", "finally", "for", "from",
                                                "global", "if", "import", "in", "is", "lambda", "nonlocal", "not",
                                                "or", "pass", "raise", "return", "try", "while", "with", "yield"};
    return words;
}

// A recursive-descent reading with Python's precedence: sums of products of signed primaries. Whatever does
// not fit this grammar is declined rather than called a syntax error, since Python may read it otherwise.
class Parser {
public:
    explicit Parser(const std::string &text) : tokens_(tokens_of(text)) {}
    std::vector<Node> nodes;

    int parse() {
        const int root = sum();
        if (peek().kind != Token::End) decline();
        return root;
    }

private:
    std::vector<Token> tokens_;
    size_t at_ = 0;

    const Token &peek() const { return tokens_[at_]; }
    bool symbol(const char *s) const { return peek().kind == Token::Symbol && peek().text == s; }
    int add(Node node) {
        nodes.push_back(std::move(node));
        return static_cast<int>(nodes.size()) - 1;
    }

    int sum() {
        int left = product();
        while (symbol("+") || symbol("-")) {
            const char op = peek().text[0];
            ++at_;
            const int right = product();
            Node node;
            node.kind = Node::Binary;
            node.op = op;
            node.children = {left, right};
            left = add(node);
        }
        return left;
    }

    int product() {
        int left = signed_primary();
        while (symbol("*") || symbol("/")) {
            const char op = peek().text[0];
            ++at_;
            const int right = signed_primary();
            Node node;
            node.kind = Node::Binary;
            node.op = op;
            node.children = {left, right};
            left = add(node);
        }
        return left;
    }

    int signed_primary() {
        if (symbol("+") || symbol("-")) {
            const char op = peek().text[0];
            ++at_;
            Node node;
            node.kind = Node::Unary;
            node.op = op;
            node.children = {signed_primary()};
            return add(node);
        }
        return primary();
    }

    int primary() {
        int result = -1;
        bool name = false;
        const Token token = peek();
        if (token.kind == Token::Number) {
            ++at_;
            Node node;
            if (token.text[0] == 'r') {
                double value = 0;
                if (!float_from_text(token.text.substr(1), value)) decline();
                node.value = value;
                node.usable = std::isfinite(value) && -kMaxValue <= value && value <= kMaxValue;
            } else {
                // An integer: within range only if it has at most six digits and is at most 100000.
                const std::string digits = token.text.substr(1);
                const size_t first = std::min(digits.find_first_not_of('0'), digits.size());
                const std::string significant = digits.substr(first);
                node.usable = significant.size() <= 6 && (significant.empty() || std::stod(significant) <= kMaxValue);
                node.value = node.usable && !significant.empty() ? std::stod(significant) : 0.0;
            }
            result = add(node);
        } else if (token.kind == Token::Word) {
            ++at_;
            Node node;
            if (token.text == "True" || token.text == "False" || token.text == "None") {
                node.usable = false;  // a constant _number refuses
            } else {
                if (keywords().count(token.text)) decline();
                node.kind = Node::Name;
                node.name = token.text;
                name = true;
            }
            result = add(node);
        } else if (symbol("(")) {
            ++at_;
            if (symbol(")")) decline();  // an empty tuple
            result = sum();
            if (!symbol(")")) decline();  // a tuple, or no closing parenthesis
            ++at_;
            name = nodes[result].kind == Node::Name;
        } else {
            decline();
        }
        if (symbol("(")) {
            // A call: only of a name, with plain arguments.
            if (!name) decline();
            ++at_;
            Node call;
            call.kind = Node::Call;
            call.name = nodes[result].name;
            if (!symbol(")")) {
                while (true) {
                    call.children.push_back(sum());
                    if (symbol(")")) break;
                    if (!symbol(",")) decline();
                    ++at_;
                    if (symbol(")")) decline();  // a trailing comma
                }
            }
            ++at_;
            result = add(call);
            if (symbol("(")) decline();  // a call of a call
        }
        return result;
    }
};

// How many nodes ast.walk would visit: the Expression, and for each node its operator and context nodes.
int walked(const std::vector<Node> &nodes, int index) {
    const Node &node = nodes[index];
    switch (node.kind) {
    case Node::Constant:
        return 1;
    case Node::Name:
        return 2;  // Name and its Load context
    case Node::Unary:
        return 2 + walked(nodes, node.children[0]);
    case Node::Binary:
        return 2 + walked(nodes, node.children[0]) + walked(nodes, node.children[1]);
    case Node::Call: {
        int count = 3;  // Call, its Name and the Name's Load
        for (int child : node.children) count += walked(nodes, child);
        return count;
    }
    }
    return 1;
}

// studio.sketch_profile.crossing_axis.
double crossing_axis(const std::vector<double> &v, int axis) {
    const double ax = v[0], ay = v[1], bx = v[2], by = v[3], cx = v[4], cy = v[5], dx = v[6], dy = v[7];
    const double rx = bx - ax, ry = by - ay, sx = dx - cx, sy = dy - cy;
    const double first = pymath::hypot(rx, ry), second = pymath::hypot(sx, sy);
    const double denominator = rx * sy - ry * sx;
    if (py_min(first, second) < 1e-8 || std::fabs(denominator) <= 1e-12 * first * second)
        throw ValueError("The sketch trim reference lines are parallel or collapsed");
    double t = ((cx - ax) * sy - (cy - ay) * sx) / denominator;
    const double u = ((cx - ax) * ry - (cy - ay) * rx) / denominator;
    if (!(-1e-8 / first <= t && t <= 1 + 1e-8 / first) || !(-1e-8 / second <= u && u <= 1 + 1e-8 / second))
        throw ValueError("The sketch trim reference segments no longer cross");
    t = py_min(1.0, py_max(0.0, t));
    return axis == 0 ? ax + t * rx : ay + t * ry;
}

bool parameter_name(const std::string &name) {
    if (name.empty() || name.size() > 40 || !word_start(name[0])) return false;
    return std::all_of(name.begin(), name.end(), word_char);
}

// studio.solid_cad.Dimensions: named parameters, resolved in the order given, and their expressions.
class Dimensions {
public:
    Dimensions(const json *definitions, const std::vector<std::string> &order) : definitions_(definitions) {
        if (!definitions || !definitions->is_object() || definitions->size() > 64)
            throw ValueError("Define at most 64 named parameters");
        for (const auto &item : definitions->items())
            if (!parameter_name(std::string(item.key())))
                throw ValueError("Use a letter or underscore followed by letters, digits or underscores for parameter names");
        for (const std::string &name : order) resolve(name);
    }

    double resolve(const std::string &name) {
        for (const auto &known : values_)
            if (known.first == name) return known.second;
        const json *definition = get(*definitions_, name.c_str());
        if (!definition) throw ValueError("Unknown parameter: " + name);
        if (resolving_.count(name)) throw ValueError("Circular parameter reference: " + name);
        resolving_.insert(name);
        double value = 0;
        try {
            value = evaluate(definition);
        } catch (...) {
            resolving_.erase(name);
            throw;
        }
        resolving_.erase(name);
        values_.push_back({name, value});
        return value;
    }

    double evaluate(const json *value) {
        if (!value || !value->is_string()) return number_of(value);
        const std::string &text = value->get_ref<const std::string &>();
        const std::vector<CodePoint> points = code_points(text);
        if (points.size() < 1 || points.size() > 160) throw ValueError("Use 1–160 characters per dimension expression");
        const std::string stripped = python_strip(text, points);
        Parser parser(stripped);
        const int root = parser.parse();
        if (1 + walked(parser.nodes, root) > 48) throw ValueError("Dimension expression is too complex");
        return visit(parser.nodes, root);
    }

    double evaluate_number(double value) { return ranged(value); }

    std::vector<double> vector(const json *value, size_t size) {
        if (!value || !value->is_array() || value->size() != size)
            throw ValueError("Expected " + std::to_string(size) + " dimensions");
        std::vector<double> result;
        for (size_t i = 0; i < size; ++i) result.push_back(evaluate(&(*value)[i]));
        return result;
    }

    double positive(const json *value) {
        const double result = evaluate(value);
        if (result <= 0) throw ValueError("Length or radius must be positive");
        return result;
    }

    json values() const {
        json result = json::object();
        for (const auto &known : values_) result[known.first] = known.second;
        return result;
    }

private:
    const json *definitions_;
    std::vector<std::pair<std::string, double>> values_;
    std::set<std::string> resolving_;

    double visit(const std::vector<Node> &nodes, int index) {
        const Node &node = nodes[index];
        switch (node.kind) {
        case Node::Constant:
            if (!node.usable) throw ValueError(kRange);
            return node.value;
        case Node::Name:
            return resolve(node.name);
        case Node::Call: {
            if (node.name != "cross_x" && node.name != "cross_y") throw ValueError(kSyntaxSubset);
            if (node.children.size() != 8) throw ValueError("A sketch crossing requires the eight endpoint coordinates");
            std::vector<double> values;
            for (int child : node.children) values.push_back(visit(nodes, child));
            return ranged(crossing_axis(values, node.name == "cross_y" ? 1 : 0));
        }
        case Node::Unary: {
            const double value = visit(nodes, node.children[0]);
            return node.op == '-' ? -value : value;
        }
        case Node::Binary: {
            const double left = visit(nodes, node.children[0]), right = visit(nodes, node.children[1]);
            switch (node.op) {
            case '+':
                return ranged(left + right);
            case '-':
                return ranged(left - right);
            case '*':
                return ranged(left * right);
            default:
                if (right == 0) throw ValueError("Division by zero in dimension");
                return ranged(left / right);
            }
        }
        }
        throw ValueError(kSyntaxSubset);
    }
};

// ---- the solid kernel, asked as studio/solid_kernel.py asks it -------------------------------------

struct Shape {
    solid::Solid solid;
    json info;
};

// As JSON text reads it: a whole number of no sign is unsigned, which the solid command requires here.
json kept_node(int index) { return json{{"kept", static_cast<std::uint64_t>(index)}}; }

// One request to the kernel: its answer and its solid. A result too fine for the grid is reported with the
// reference's advice.
Shape ask(const json &node, const std::vector<const Shape *> &kept) {
    json request = json::object();
    request["shape"] = node;
    std::vector<solid::Solid> solids;
    for (const Shape *shape : kept) solids.push_back(shape->solid);
    Shape result;
    try {
        result.info = solid_call(request, solids, &result.solid);
    } catch (const ValueError &error) {
        const std::string text = error.what();
        if (text.find(kTooFine) != std::string::npos)
            throw ValueError("The solid kernel cannot represent this result: " + text +
                             ". Two faces almost coincide here, usually after a rotation. Move, turn or resize one of the shapes by a small amount");
        throw;
    }
    return result;
}

// A question about a kept solid: projection or sections.
json question(const Shape &shape, const char *key, const json &value) {
    json request = json::object();
    request["shape"] = kept_node(0);
    request[key] = value;
    try {
        return solid_call(request, {shape.solid}, nullptr);
    } catch (const ValueError &error) {
        const std::string text = error.what();
        if (text.find(kTooFine) != std::string::npos)
            throw ValueError("The solid kernel cannot represent this result: " + text +
                             ". Two faces almost coincide here, usually after a rotation. Move, turn or resize one of the shapes by a small amount");
        throw;
    }
}

json numbers(const std::vector<double> &values) {
    json result = json::array();
    for (double value : values) result.push_back(value);
    return result;
}

Shape mapped(const Shape &shape, const char *how, const std::vector<double> &values) {
    json node = json::object();
    node[how] = numbers(values);
    node["of"] = kept_node(0);
    return ask(node, {&shape});
}

Shape combined(const char *op, const std::vector<const Shape *> &shapes) {
    json list = json::array();
    for (size_t i = 0; i < shapes.size(); ++i) list.push_back(kept_node(static_cast<int>(i)));
    json node = json::object();
    node[op] = std::move(list);
    return ask(node, shapes);
}

const char *op_name(const std::string &op) { return op == "add" ? "union" : op == "cut" ? "difference" : "intersection"; }

double triangles_of(const Shape &shape) { return shape.info.at("triangles").get<double>(); }

// _checked: a valid, non-empty solid within the triangle limit.
const Shape &checked(const Shape &shape) {
    if (shape.info.at("empty").get<bool>()) throw ValueError("The operation produced an empty or invalid solid");
    if (triangles_of(shape) > kMaxFaces) throw ValueError("Solid exceeds 75000 triangles");
    return shape;
}

// The polygon kernel's normal form of rings (below).
std::vector<json> normalised(const std::vector<json> &rings, const char *fill);

// ---- sketched profiles: studio/sketch_profile.py, sketch_curves.py and the unconstrained case of
// sketch_constraints.py --------------------------------------------------------------------------------

using P2 = std::array<double, 2>;

double distance(const P2 &a, const P2 &b) { return pymath::dist(a[0], a[1], b[0], b[1]); }

// sketch_profile.Corner: a vertex, or a fillet arc from start to end.
struct Corner {
    P2 start{}, end{}, center{};
    double radius = 0, start_angle = 0, sweep = 0, setback = 0;
    P2 at(double fraction) const {
        const double angle = start_angle + sweep * fraction;
        const double x = center[0] + radius * pytrig::cosine(angle);
        return {x, center[1] + radius * pytrig::sine(angle)};
    }
};

// sketch_profile._corner.
Corner corner_of(const P2 &a, const P2 &b, const P2 &c, double radius) {
    Corner corner;
    if (radius == 0) {
        corner.start = corner.end = b;
        return corner;
    }
    P2 u = {a[0] - b[0], a[1] - b[1]}, v = {c[0] - b[0], c[1] - b[1]};
    u = {u[0] / pymath::hypot(u[0], u[1]), u[1] / pymath::hypot(u[0], u[1])};
    v = {v[0] / pymath::hypot(v[0], v[1]), v[1] / pymath::hypot(v[0], v[1])};
    const double cross = u[0] * v[1] - u[1] * v[0];
    if (std::fabs(cross) < 1e-10) throw ValueError("A fillet needs two edges meeting at a corner");
    const double angle = pytrig::atan2(std::fabs(cross), u[0] * v[0] + u[1] * v[1]);
    const double setback = radius / pytrig::tangent(angle / 2);
    corner.start = {b[0] + setback * u[0], b[1] + setback * u[1]};
    corner.end = {b[0] + setback * v[0], b[1] + setback * v[1]};
    // The centre is one radius normal to the incoming line at its point of tangency.
    const double side = std::copysign(radius, cross);
    corner.center = {corner.start[0] - side * u[1], corner.start[1] + side * u[0]};
    corner.radius = radius;
    corner.start_angle = pytrig::atan2(corner.start[1] - corner.center[1], corner.start[0] - corner.center[0]);
    corner.sweep = -std::copysign(pytrig::PI - angle, cross);
    corner.setback = setback;
    return corner;
}

size_t wrapped(long long index, size_t count) {
    const long long n = static_cast<long long>(count);
    return static_cast<size_t>(((index % n) + n) % n);
}

// sketch_curves.resolve_curves: the two handles of each cubic edge, with the tangent alignments applied.
std::vector<std::vector<P2>> resolve_curves(const std::vector<const json *> &raw, const std::vector<P2> &points, Dimensions &dimensions) {
    const size_t count = points.size();
    if (raw.size() != count) throw ValueError("Supply one curve entry per outgoing sketch edge");
    std::vector<std::vector<P2>> controls;
    auto option = [](const json &curve, const char *key) {
        const json *value = get(curve, key);
        return value && value->get<bool>();
    };
    for (const json *curve : raw) {
        if (!curve || curve->is_null()) {
            controls.emplace_back();
            continue;
        }
        if (!curve->is_object()) throw ValueError("Invalid cubic sketch edge");
        for (const auto &item : curve->items()) {
            const std::string key(item.key());
            if (key != "controls" && key != "tangentStart" && key != "tangentEnd") throw ValueError("Invalid cubic sketch edge");
        }
        const json *poles = get(*curve, "controls");
        if (!poles || !poles->is_array() || poles->size() != 2) throw ValueError("A cubic sketch edge needs two planar control points");
        for (const char *key : {"tangentStart", "tangentEnd"}) {
            const json *value = get(*curve, key);
            if (value && !value->is_boolean()) throw ValueError("A cubic tangent option must be true or false");
        }
        std::vector<P2> handles;
        for (size_t k = 0; k < 2; ++k) {
            const std::vector<double> point = dimensions.vector(&(*poles)[k], 2);
            handles.push_back({point[0], point[1]});
        }
        controls.push_back(handles);
    }
    for (size_t i = 0; i < count; ++i) {
        const json *curve = raw[i];
        if (!curve || curve->is_null()) continue;
        for (int side = 0; side < 2; ++side) {
            if (!option(*curve, side ? "tangentEnd" : "tangentStart")) continue;
            const P2 joint = points[wrapped(static_cast<long long>(i) + side, count)];
            const size_t neighbour = wrapped(static_cast<long long>(i) + (side ? 1 : -1), count);
            const json *neighbour_curve = raw[neighbour];
            const bool curved = neighbour_curve && !neighbour_curve->is_null();
            if (curved && option(*neighbour_curve, side ? "tangentStart" : "tangentEnd"))
                throw ValueError("At a shared cubic vertex, choose only one handle to follow its neighbour");
            const P2 guide = curved ? controls[neighbour][1 - side] : points[wrapped(static_cast<long long>(i) + (side ? 2 : -1), count)];
            const double length = distance(joint, controls[i][side]), baseline = distance(joint, guide);
            if (py_min(length, baseline) <= 1e-9) throw ValueError("Tangent alignment needs a nonzero handle and neighbouring direction");
            P2 aligned;
            for (int k = 0; k < 2; ++k) aligned[k] = joint[k] + (joint[k] - guide[k]) * length / baseline;
            controls[i][side] = aligned;
            if (std::fabs(aligned[0]) > 100000 || std::fabs(aligned[1]) > 100000)
                throw ValueError("Aligned curve handle exceeds the sketch coordinate range");
        }
    }
    return controls;
}

P2 midpoint(const P2 &a, const P2 &b) { return {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}; }

// sketch_curves.sample_cubic: de Casteljau subdivision until the control hull lies within the tolerance.
struct Sampler {
    double tolerance = 0;
    std::vector<P2> result;
    void subdivide(const P2 &a, const P2 &b, const P2 &c, const P2 &d, int depth) {
        const double dx = d[0] - a[0], dy = d[1] - a[1];
        const double chord = pymath::hypot(dx, dy);
        if (chord != 0) {
            double projections[2], deviations[2];
            const P2 *inner[2] = {&b, &c};
            for (int k = 0; k < 2; ++k) {
                const P2 &p = *inner[k];
                projections[k] = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / chord;
                deviations[k] = std::fabs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / chord;
            }
            if (py_max(deviations[0], deviations[1]) <= tolerance && -1e-12 <= projections[0] &&
                projections[0] <= projections[1] + 1e-12 && projections[1] <= chord + 1e-12) {
                result.push_back(d);
                return;
            }
        }
        if (depth >= 20 || result.size() >= 4096)
            throw ValueError("Curve reverses direction at a cusp or exceeds the sketch sampling budget");
        const P2 ab = midpoint(a, b), bc = midpoint(b, c), cd = midpoint(c, d);
        const P2 abc = midpoint(ab, bc), bcd = midpoint(bc, cd);
        const P2 mid = midpoint(abc, bcd);
        subdivide(a, ab, abc, mid, depth + 1);
        subdivide(mid, bcd, cd, d, depth + 1);
    }
};

std::vector<P2> sample_cubic(const P2 &start, const std::vector<P2> &controls, const P2 &end) {
    const double span = py_max(py_max(distance(start, controls[0]), distance(start, controls[1])), distance(start, end));
    Sampler sampler;
    sampler.tolerance = py_min(0.01, span * 0.00025);
    sampler.subdivide(start, controls[0], controls[1], end, 0);
    return sampler.result;
}

// sketch_curves.minimum_coordinate: the least value of a scalar cubic on [0, 1].
double minimum_coordinate(double p0, double p1, double p2, double p3) {
    const double a = -p0 + 3 * p1 - 3 * p2 + p3, b = 2 * (p0 - 2 * p1 + p2), c = p1 - p0;
    const double disc = b * b - 4 * a * c;
    std::vector<double> roots;
    if (a == 0) {
        if (b != 0) roots = {-c / b};
    } else if (disc >= 0) {
        const double q = -0.5 * (b + std::copysign(std::sqrt(disc), b));
        if (q != 0) roots = {q / a, c / q};
        else roots = {0.0};
    }
    double least = py_min(p0, p3);
    for (double t : roots)
        if (0 < t && t < 1) {
            const double s = 1 - t;
            least = py_min(least, std::pow(s, 3.0) * p0 + 3 * s * s * t * p1 + 3 * s * t * t * p2 + std::pow(t, 3.0) * p3);
        }
    return least;
}

// GEOS's signed area of a closed ring (Area::ofRingSigned): the shoelace sum taken about the first x.
double ring_area(const std::vector<P2> &ring) {
    const size_t n = ring.size();
    if (n < 3) return 0.0;
    P2 p0{}, p1 = ring[0], p2 = ring[1];
    const double x0 = p1[0];
    p2[0] -= x0;
    double sum = 0.0;
    for (size_t i = 1; i < n - 1; ++i) {
        p0[1] = p1[1];
        p1[0] = p2[0];
        p1[1] = p2[1];
        p2 = ring[i + 1];
        p2[0] -= x0;
        sum += p1[0] * (p0[1] - p2[1]);
    }
    return sum / 2.0;
}

// sketch_profile.profile_geometry, for a sketch without constraints: the sampled outline.
std::vector<P2> profile_outline(const json &feature, Dimensions &dimensions) {
    const json *raw = get(feature, "points");
    if (!raw || !raw->is_array() || raw->size() < 3 || raw->size() > 256) throw ValueError("A sketch needs 3–256 vertices");
    // solve_profile: the vertices, then the constraints, which are the Python's for now.
    std::vector<P2> points;
    for (const json &point : *raw) {
        const std::vector<double> xy = dimensions.vector(&point, 2);
        points.push_back({xy[0], xy[1]});
    }
    const json *constraints = get(feature, "constraints");
    if (constraints) {
        if (!constraints->is_array() || constraints->size() > 128) throw ValueError("A sketch supports up to 128 geometric constraints");
        if (constraints->size()) throw NotPorted("sketch constraints");
    }
    const size_t given = points.size();
    std::vector<double> radii;
    const json *fillets = get(feature, "fillets");
    if (fillets) {
        if (!fillets->is_array() || fillets->size() != given) throw ValueError("Supply one fillet radius per sketch vertex");
        for (const json &radius : *fillets) radii.push_back(dimensions.evaluate(&radius));
    } else {
        radii.assign(given, 0.0);
        for (double &radius : radii) radius = dimensions.evaluate_number(radius);
    }
    if (*std::min_element(radii.begin(), radii.end()) < 0) throw ValueError("Fillet radii must be zero or positive");
    std::vector<const json *> curves;
    const json *raw_curves = get(feature, "curves");
    if (raw_curves) {
        if (!raw_curves->is_array() || raw_curves->size() != given) throw ValueError("Supply one curve entry per outgoing sketch edge");
        for (const json &curve : *raw_curves) curves.push_back(&curve);
    } else {
        curves.assign(given, nullptr);
    }
    if (points.front() == points.back()) {
        if (curves.back() && !curves.back()->is_null()) throw ValueError("A duplicate closing vertex cannot start another cubic edge");
        curves.pop_back();
        if (radii.back() != 0) throw ValueError("Set the fillet on the first vertex of a closed loop");
        points.pop_back();
        radii.pop_back();
    }
    bool distinct = points.size() >= 3;
    for (size_t i = 0; distinct && i < points.size(); ++i)
        for (size_t j = i + 1; distinct && j < points.size(); ++j) distinct = points[i] != points[j];
    if (!distinct) throw ValueError("Sketch vertices must be distinct");
    const size_t n = points.size();
    const std::vector<std::vector<P2>> controls = resolve_curves(curves, points, dimensions);
    for (size_t i = 0; i < n; ++i)
        if (!controls[i].empty() && (radii[i] != 0 || radii[(i + 1) % n] != 0))
            throw ValueError("Remove the endpoint fillet before making a curved edge. Use tangent handles to round a cubic join.");
    std::vector<Corner> corners;
    for (size_t i = 0; i < n; ++i) corners.push_back(corner_of(points[wrapped(static_cast<long long>(i) - 1, n)], points[i], points[(i + 1) % n], radii[i]));
    const bool revolve = is_text(get(feature, "kind"), "revolve");
    std::vector<P2> outline;
    for (size_t i = 0; i < n; ++i) {
        const Corner &corner = corners[i];
        const size_t before = wrapped(static_cast<long long>(i) - 1, n);
        if (corner.setback + corners[before].setback > distance(points[before], points[i]) + 1e-10)
            throw ValueError("Fillets exceed the available length of the edge leading to point " + std::to_string(i + 1));
        if (outline.empty() || distance(outline.back(), corner.start) > 1e-10) outline.push_back(corner.start);
        if (corner.radius != 0) {
            const int steps = static_cast<int>(py_max(2.0, std::ceil(std::fabs(corner.sweep) / (pytrig::PI / 48))));
            for (int j = 1; j < steps; ++j) outline.push_back(corner.at(static_cast<double>(j) / steps));
            outline.push_back(corner.end);
        }
        if (!controls[i].empty()) {
            const P2 end = corners[(i + 1) % n].start;
            if (revolve && minimum_coordinate(corner.end[0], controls[i][0][0], controls[i][1][0], end[0]) < -1e-10)
                throw ValueError("Revolve profile radii must be zero or greater along the entire curve");
            for (const P2 &point : sample_cubic(corner.end, controls[i], end)) outline.push_back(point);
        }
        if (outline.size() > 16384) throw ValueError("Sketch exceeds 16384 sampled vertices");
    }
    if (outline.size() > 1 && distance(outline.front(), outline.back()) <= 1e-10) outline.pop_back();
    // Shapely's Polygon(outline): a ring needs four coordinates with its closing one; is_valid is the closed
    // line's is_simple for a ring of distinct vertices, and the area is GEOS's.
    if (outline.size() < 3) throw NotPorted("a sketch outline of fewer than three points");
    Contour ring(outline.begin(), outline.end());
    ring.push_back(outline.front());
    const bool valid = line_is_simple(ring);
    const double area = std::fabs(ring_area(std::vector<P2>(ring.begin(), ring.end())));
    if (valid && std::fabs(area - 1e-8) <= 1e-14) throw NotPorted("a sketch whose area is within rounding of the limit");
    if (!valid || area <= 1e-8) throw ValueError("Sketch must enclose an area without crossing itself");
    if (revolve) {
        for (const P2 &point : points)
            if (point[0] < 0) throw ValueError("Revolve profile radii must be zero or greater");
        for (const P2 &point : outline)
            if (point[0] < 0) throw ValueError("Revolve profile radii must be zero or greater");
    }
    return outline;
}

// solid_cad._sketch: the outline as an even-odd cross section.
std::vector<json> sketch_rings(const json &feature, Dimensions &dimensions) {
    const std::vector<P2> outline = profile_outline(feature, dimensions);
    json ring = json::array();
    for (const P2 &point : outline) ring.push_back(json::array({point[0], point[1]}));
    return normalised({ring}, "evenodd");
}

// ---- features --------------------------------------------------------------------------------------

const json *field(const json &object, const char *key) { return get(object, key); }

Shape primitive(const json &feature, Dimensions &dimensions) {
    const json *kind = field(feature, "kind");
    Shape shape;
    if (is_text(kind, "box")) {
        const std::vector<double> size = dimensions.vector(field(feature, "size"), 3);
        if (*std::min_element(size.begin(), size.end()) <= 0) throw ValueError("Box dimensions must be positive");
        json node = json::object();
        node["box"] = numbers(size);
        node["center"] = false;
        shape = ask(node, {});
    } else if (is_text(kind, "cylinder")) {
        const double radius = dimensions.positive(field(feature, "radius"));
        const json *given_top = field(feature, "topRadius");
        const double top = given_top ? dimensions.evaluate(given_top) : dimensions.evaluate_number(radius);
        if (top < 0) throw ValueError("Top radius must be zero or greater");
        const double height = dimensions.positive(field(feature, "height"));
        json cylinder = json::object();
        cylinder["height"] = height;
        cylinder["radius_low"] = radius;
        cylinder["radius_high"] = top;
        cylinder["segments"] = kSegments;
        cylinder["center"] = false;
        shape = ask(json{{"cylinder", cylinder}}, {});
    } else if (is_text(kind, "sphere")) {
        json sphere = json::object();
        sphere["radius"] = dimensions.positive(field(feature, "radius"));
        sphere["segments"] = kSegments;
        shape = ask(json{{"sphere", sphere}}, {});
    } else if (is_text(kind, "extrude")) {
        // CrossSection.extrude(height): one layer, no twist, no scaling.
        const std::vector<json> rings = sketch_rings(feature, dimensions);
        json extrude = json::object();
        extrude["rings"] = json(rings);
        extrude["height"] = dimensions.positive(field(feature, "height"));
        extrude["slices"] = 1;
        extrude["twist"] = 0.0;
        extrude["scale"] = json::array({1.0, 1.0});
        extrude["fill"] = "positive";
        shape = ask(json{{"extrude", extrude}}, {});
    } else if (is_text(kind, "revolve")) {
        const json *given_angle = field(feature, "angle");
        const double angle = given_angle ? dimensions.evaluate(given_angle) : dimensions.evaluate_number(360);
        if (!(0 < angle && angle <= 360)) throw ValueError("Revolve angle must be above 0 and at most 360 degrees");
        json revolve = json::object();
        revolve["rings"] = json(sketch_rings(feature, dimensions));
        revolve["segments"] = kSegments;
        revolve["degrees"] = angle;
        revolve["fill"] = "positive";
        shape = ask(json{{"revolve", revolve}}, {});
    } else {
        throw ValueError("Unknown solid primitive");
    }
    static const json zero = json::array({0, 0, 0});
    const json *rotation_given = field(feature, "rotation");
    const json *origin_given = field(feature, "origin");
    const std::vector<double> rotation = dimensions.vector(rotation_given ? rotation_given : &zero, 3);
    const std::vector<double> origin = dimensions.vector(origin_given ? origin_given : &zero, 3);
    const Shape turned = mapped(shape, "rotate", rotation);
    return mapped(turned, "translate", origin);
}

Shape instances(Shape shape, const json *pattern, Dimensions &dimensions) {
    if (!pattern || pattern->is_null()) return shape;
    if (!pattern->is_object()) throw ValueError("Pattern must be an object");
    const double count = dimensions.evaluate(field(*pattern, "count"));
    if (count != std::floor(count) || !(1 <= count && count <= 64))
        throw ValueError("Pattern count must be a whole number from 1 to 64");
    if (triangles_of(shape) * count > kMaxFaces) throw ValueError("Pattern exceeds triangle budget");
    const int copies_wanted = static_cast<int>(count);
    std::vector<Shape> copies = {shape};
    const json *kind = field(*pattern, "kind");
    if (is_text(kind, "linear")) {
        const std::vector<double> step = dimensions.vector(field(*pattern, "step"), 3);
        if (count > 1 && step[0] == 0 && step[1] == 0 && step[2] == 0) throw ValueError("Pattern step must move each instance");
        for (int i = 1; i < copies_wanted; ++i)
            copies.push_back(mapped(shape, "translate", {i * step[0], i * step[1], i * step[2]}));
    } else if (is_text(kind, "polar")) {
        static const json origin = json::array({0, 0, 0});
        const json *centre_given = field(*pattern, "center");
        const std::vector<double> centre = dimensions.vector(centre_given ? centre_given : &origin, 3);
        const double angle = dimensions.evaluate(field(*pattern, "angle"));
        if (count > 1 && std::fmod(angle, 360.0) == 0) throw ValueError("Polar pattern needs a nonzero angular step");
        const Shape centred = mapped(shape, "translate", {-centre[0], -centre[1], -centre[2]});
        for (int i = 1; i < copies_wanted; ++i) {
            const Shape turned = mapped(centred, "rotate", {0.0, 0.0, angle * i});
            copies.push_back(mapped(turned, "translate", centre));
        }
    } else {
        throw ValueError("Pattern must be linear or polar");
    }
    std::vector<const Shape *> all;
    for (const Shape &copy : copies) all.push_back(&copy);
    return combined("union", all);
}

struct Pending {
    std::string identifier, op;
    Shape tool;
};

std::string prefixed(const std::string &identifier, const std::exception &error) { return identifier + ": " + error.what(); }

Shape one_step(const Shape &solid, const std::string &op, const Shape &tool) { return combined(op_name(op), {&solid, &tool}); }

// _apply_run: a run of features with one operation as one batch, or feature by feature when the batch fails
// or its result does not pass the checks, so that an error names its feature.
Shape apply_run(const Shape &start, const std::vector<Pending> &run) {
    const std::string op = run[0].op;
    if (run.size() == 1) {
        try {
            Shape result = one_step(start, op, run[0].tool);
            checked(result);
            return result;
        } catch (const ValueError &error) {
            throw ValueError(prefixed(run[0].identifier, error));
        }
    }
    try {
        std::vector<const Shape *> all = {&start};
        for (const Pending &item : run) all.push_back(&item.tool);
        Shape result = combined(op_name(op), all);
        checked(result);
        return result;
    } catch (const ValueError &) {
    }
    Shape solid = start;
    for (const Pending &item : run) {
        try {
            Shape next = one_step(solid, op, item.tool);
            checked(next);
            solid = std::move(next);
        } catch (const ValueError &error) {
            throw ValueError(prefixed(item.identifier, error));
        }
    }
    return solid;
}

struct Built {
    Shape solid;
    std::unique_ptr<Dimensions> dimensions;
};

// The order in which the parameters are resolved: the order the caller gives (the order of the Python
// dictionary), or else the order of the names.
std::vector<std::string> parameter_order(const json *parameters, const json *order) {
    std::vector<std::string> names;
    if (!parameters || !parameters->is_object()) return names;
    for (const auto &item : parameters->items()) names.emplace_back(item.key());
    if (!order) return names;
    std::vector<std::string> given;
    if (!order->is_array()) throw ValueError("The parameter order must list the parameters");
    for (const json &name : *order) {
        if (!name.is_string()) throw ValueError("The parameter order must list the parameters");
        given.push_back(name.get<std::string>());
    }
    std::vector<std::string> sorted_given = given;
    std::sort(sorted_given.begin(), sorted_given.end());
    if (sorted_given != names) throw ValueError("The parameter order must list the parameters");
    return given;
}

// _build: the enabled features in order, runs of one operation applied together, errors in feature order.
Built build(const json *model, const json *order) {
    if (!model || !model->is_object()) throw ValueError("Expected a version 1 solid model");
    const json *version = field(*model, "version");
    if (!version || !version->is_number_integer() || version->get<long long>() != 1) throw ValueError("Expected a version 1 solid model");
    const json *features = field(*model, "features");
    // Sketched, lofted, scripted and STEP features are the Python's for now.
    if (features && features->is_array())
        for (const json &feature : *features)
            if (feature.is_object()) {
                const json *kind = field(feature, "kind");
                for (const char *other : {"step", "script", "loft"})
                    if (is_text(kind, other)) throw NotPorted(std::string("solid features of kind ") + other);
            }
    static const json no_parameters = json::object();
    const json *parameters = model->contains("parameters") ? field(*model, "parameters") : &no_parameters;
    Built built;
    built.dimensions = std::make_unique<Dimensions>(parameters, parameter_order(parameters, order));
    Dimensions &dimensions = *built.dimensions;
    if (!features || !features->is_array() || features->size() < 1 || features->size() > 64)
        throw ValueError("A solid model needs 1–64 features");
    bool have_solid = false;
    std::vector<Pending> run;
    std::set<std::string> identifiers;
    size_t index = 0;
    for (const json &feature : *features) {
        ++index;
        std::string failure;
        std::string identifier, op;
        Shape tool;
        if (!feature.is_object()) {
            failure = "Invalid feature " + std::to_string(index);
        } else {
            const json *id = field(feature, "id");
            bool unique = false;
            if (id && id->is_string()) {
                identifier = id->get<std::string>();
                const std::vector<CodePoint> points = code_points(identifier);
                unique = !python_strip(identifier, points).empty() && points.size() <= 80 && !identifiers.count(identifier);
            }
            const json *enabled = field(feature, "enabled");
            if (!unique) failure = "Feature " + std::to_string(index) + " needs a unique ID of 1–80 characters";
            else if (enabled && !enabled->is_boolean()) failure = identifier + ": enabled must be true or false";
        }
        if (failure.empty()) {
            identifiers.insert(identifier);
            const json *enabled = field(feature, "enabled");
            if (enabled && !enabled->get<bool>()) continue;
            try {
                const json *given_op = field(feature, "op");
                op = given_op ? (given_op->is_string() ? given_op->get<std::string>() : std::string("\x01")) : "add";
                if (op != "add" && op != "cut" && op != "intersect") throw ValueError("Choose add, cut or intersect");
                if (!have_solid && run.empty() && op != "add") throw ValueError("First enabled feature must add material");
                tool = instances(primitive(feature, dimensions), field(feature, "pattern"), dimensions);
                if (!have_solid && run.empty()) {
                    checked(tool);
                    built.solid = std::move(tool);
                    have_solid = true;
                    continue;
                }
            } catch (const ValueError &error) {
                failure = prefixed(identifier, error);
            }
        }
        if (!failure.empty()) {
            // A feature before this one that fails comes first.
            if (!run.empty()) apply_run(built.solid, run);
            throw ValueError(failure);
        }
        if (!run.empty() && run.back().op != op) {
            built.solid = apply_run(built.solid, run);
            run.clear();
        }
        run.push_back({identifier, op, std::move(tool)});
    }
    if (!run.empty()) built.solid = apply_run(built.solid, run);
    if (!have_solid) throw ValueError("Enable at least one additive feature");
    return built;
}

// CrossSection(rings, fill): the polygon kernel's normal form of the rings, as its shells and holes in turn.
std::vector<json> normalised(const std::vector<json> &rings, const char *fill) {
    if (rings.empty()) return {};
    json request = json::object();
    request["op"] = "normalise";
    request["rings"] = json(rings);
    request["fill"] = fill;
    const json answer = polygon_call("polygon", request);
    std::vector<json> result;
    for (const json &polygon : answer.at("polygons")) {
        result.push_back(polygon.at("shell"));
        for (const json &hole : polygon.at("holes")) result.push_back(hole);
    }
    return result;
}

// _rings(section): each ring closed by its first point again.
json closed(const std::vector<json> &rings) {
    json paths = json::array();
    for (const json &ring : rings) {
        json path = ring;
        if (ring.size()) path.push_back(ring[0]);
        paths.push_back(std::move(path));
    }
    return paths;
}

// solid_kernel._between and _straightened: section rings without the points that only divide a straight side.
bool between(const json &a, const json &b, const json &c) {
    const double ax = a[0].get<double>(), ay = a[1].get<double>();
    const double ux = c[0].get<double>() - ax, uy = c[1].get<double>() - ay;
    const double vx = b[0].get<double>() - ax, vy = b[1].get<double>() - ay;
    const double square = ux * ux + uy * uy, across = ux * vy - uy * vx, along = ux * vx + uy * vy;
    const double straight = 2e-6;
    return square > 0 && across * across <= straight * straight * square && 0 <= along && along <= square;
}

std::vector<json> straightened(const json &rings) {
    std::vector<json> result;
    for (const json &ring : rings) {
        std::vector<json> kept;
        for (const json &point : ring) {
            while (kept.size() >= 2 && between(kept[kept.size() - 2], kept.back(), point)) kept.pop_back();
            kept.push_back(point);
        }
        while (kept.size() > 3 && between(kept[kept.size() - 2], kept.back(), kept[0])) kept.pop_back();
        while (kept.size() > 3 && between(kept.back(), kept[0], kept[1])) kept.erase(kept.begin());
        result.push_back(json(kept));
    }
    return result;
}

std::vector<json> rings_in(const json &value) {
    std::vector<json> rings;
    for (const json &ring : value) rings.push_back(ring);
    return rings;
}

json rebuild(const json *model, const json *order) {
    Built built = build(model, order);
    const Shape &solid = built.solid;
    json vertices = json::array();
    for (const solid::Vec3 &vertex : solid.solid.vertices()) vertices.push_back(json::array({vertex.x, vertex.y, vertex.z}));
    json triangles = json::array();
    for (const solid::Triangle &triangle : solid.solid.triangles())
        triangles.push_back(json::array({std::int64_t(triangle[0]), std::int64_t(triangle[1]), std::int64_t(triangle[2])}));
    // The silhouette: CrossSection(solid.project().to_polygons(), Positive), normalised twice as the
    // reference's two cross sections are.
    const json projection = question(solid, "projection", true);
    const std::vector<json> outline = normalised(rings_in(projection.at("projection").at("rings")), "positive");
    const std::vector<json> silhouette = normalised(outline, "positive");
    json result = json::object();
    result["vertices"] = std::move(vertices);
    result["triangles"] = std::move(triangles);
    result["bounds"] = solid.info.at("bounds");
    result["volume"] = solid.info.at("volume");
    result["profiles"] = closed(silhouette);
    result["parameters"] = built.dimensions->values();
    return result;
}

json section(const json &request) {
    Built built = build(field(request, "model"), field(request, "order"));
    const json *z = field(request, "z");
    const double height = built.dimensions->evaluate(z);
    const json &bounds = built.solid.info.at("bounds");
    if (!(bounds[2].get<double>() <= height && height < bounds[5].get<double>()))
        throw ValueError("Section Z must be at or above the bottom and below the top of the model");
    const json answer = question(built.solid, "sections", json::array({height}));
    const std::vector<json> region = normalised(straightened(answer.at("sections")[0].at("rings")), "positive");
    json result = json::object();
    result["z"] = height;
    result["profiles"] = closed(region);
    return result;
}

}  // namespace

bool solid_cad_command(const std::string &command) { return command == "solid-cad" || command == "solid-cad-section"; }

Json solid_cad_call(const std::string &command, const Json &request) {
    if (!request.is_object()) throw ValueError("Supply a CAD model in the model field");
    if (command == "solid-cad-section") return section(request);
    return rebuild(field(request, "model"), field(request, "order"));
}

}  // namespace kerfkite
