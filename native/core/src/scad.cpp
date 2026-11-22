// KerfKite's CAD script language: studio/scad_language.py (the reader and the evaluator) and
// studio/scad_geometry.py (the geometry), statement for statement where the order of the checks, of the
// work charged or of the kernel's operations shows in the result.
//
// What is reproduced, to the bit:
//
// * The reader: the same tokens (Python's re module reads \s and \d as Unicode classes, which the tables in
//   unicode_tables.hpp give), the same lines and columns (counted in code points), the same parse and the
//   same messages, with repr() of an unreadable character.
// * The evaluator: Python's int and float (an int stays an int through + - * and the functions that give
//   one), its equality, truth and comparisons, CPython's math functions on the platform's libm, sum() with
//   its compensation and math.hypot; lexical scopes, variables resolved when first read, modules, functions,
//   children(), $fn, loops, ranges and comprehensions; the evaluation budget charged unit for unit, so a
//   script that exceeds it fails at the same place.
// * The geometry: every request the Python makes of the solid and polygon kernels (solid_requests.hpp),
//   with the same numbers, so the drawings, solids and measures are the same; the budget, the limits and
//   the messages, each prefixed with the line and operation that failed.
//
// Two decisions depend on arithmetic the Python leaves to NumPy's linear algebra, whose last bits differ
// between platforms and BLAS libraries even in the reference: whether a multmatrix() is singular
// (|det| < 1e-14), and whether a polyhedron face has an area (|normal| <= tolerance²) and is planar and
// convex (distances within the tolerance). The core computes the determinant by LU decomposition with
// partial pivoting and NumPy's sign and logarithms, and the normal and distances by plain dot products;
// only a matrix or a face within rounding of a threshold can be judged differently. Whether a polygon path
// or a polyhedron face crosses itself is Shapely's answer (simple_line.hpp), which declines (NotPorted) the
// rare path whose answer depends on rounding inside GEOS.
#include "scad.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kerfkite/py_math.hpp"
#include "project.hpp"
#include "py_trig.hpp"
#include "python_text.hpp"
#include "simple_line.hpp"
#include "unicode_tables.hpp"

namespace kerfkite {
namespace scad {

using json = Json;
using namespace solid_requests;
using detail::CodePoint;

namespace {

// ---- limits (scad_language.py and scad_geometry.py) ---------------------------------------------------

const size_t kMaxSource = 65536;
const size_t kMaxTokens = 16000;
const int kMaxDepth = 48;
const size_t kMaxRange = 1024;
const long long kMaxVectorSize = 16384;
const int kMaxGeometry = 1500;
const long long kMaxWork = 180000;
const double kMaxNumber = 1e9;

const double kMaxCoord = 100000;
const long long kMaxVertices = 8000;
const long long kMaxTriangles = 75000;
const long long kMaxOverlap = 32000000;
const long long kMaxSegments = 256;
const int kDefaultSegments = 48;

const double kDegToRad = pytrig::PI / 180.0;  // math.radians
const double kRadToDeg = 180.0 / pytrig::PI;  // math.degrees

std::string quoted(const std::string &text) { return "'" + text + "'"; }  // repr() of text with no quote or backslash

// ---- characters ---------------------------------------------------------------------------------------

bool decimal_digit(uint32_t c, int *value = nullptr) {
    const uint32_t *runs = detail::kDecimalDigitRuns;
    const size_t count = sizeof(detail::kDecimalDigitRuns) / sizeof(detail::kDecimalDigitRuns[0]);
    const uint32_t *after = std::upper_bound(runs, runs + count, c);
    if (after == runs) return false;
    const uint32_t start = *(after - 1);
    if (c - start >= 10) return false;
    if (value) *value = static_cast<int>(c - start);
    return true;
}

bool printable(uint32_t c) {
    const size_t count = sizeof(detail::kNotPrintable) / sizeof(detail::kNotPrintable[0]);
    size_t low = 0, high = count;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (detail::kNotPrintable[middle][1] < c) low = middle + 1;
        else high = middle;
    }
    return !(low < count && detail::kNotPrintable[low][0] <= c);
}

std::string utf8(uint32_t c) {
    std::string out;
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
    return out;
}

// repr() of a string of one character.
std::string character_repr(uint32_t c) {
    if (c == '\'') return "\"'\"";
    char buffer[16];
    std::string body;
    if (c == '\\') body = "\\\\";
    else if (c == '\t') body = "\\t";
    else if (c == '\n') body = "\\n";
    else if (c == '\r') body = "\\r";
    else if (c < 0x20 || c == 0x7f) {
        std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned>(c));
        body = buffer;
    } else if (c < 0x7f || printable(c)) {
        body = utf8(c);
    } else {
        if (c <= 0xff) std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned>(c));
        else if (c <= 0xffff) std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
        else std::snprintf(buffer, sizeof buffer, "\\U%08x", static_cast<unsigned>(c));
        body = buffer;
    }
    return "'" + body + "'";
}

// ---- tokens --------------------------------------------------------------------------------------------

struct Token {
    enum Kind { Number, Name, Symbol, End } kind = End;
    std::string text;
    long long line = 1, column = 1;
    std::string digits;  // a number's text in ASCII digits, as float() reads it
};

// ScadError: "Line L, column C: message".
ValueError located(long long line, long long column, const std::string &message) {
    return ValueError("Line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message);
}
ValueError scad_error(const Token &token, const std::string &message) { return located(token.line, token.column, message); }

bool name_start(uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool name_char(uint32_t c) { return name_start(c) || (c >= '0' && c <= '9'); }
bool single_symbol(uint32_t c) {
    static const std::string symbols = "{}()[],;:+-*/%^=!<>?";
    return c < 0x80 && symbols.find(static_cast<char>(c)) != std::string::npos;
}

std::vector<Token> tokenize(const json *source) {
    const Token start;
    if (!source || !source->is_string()) throw scad_error(start, "Source must be text of at most 65536 UTF-8 bytes");
    const std::string &text = source->get_ref<const std::string &>();
    const std::vector<CodePoint> points = detail::code_points(text);
    if (points.size() > kMaxSource || text.size() > kMaxSource) throw scad_error(start, "Source must be text of at most 65536 UTF-8 bytes");
    const size_t n = points.size();
    auto at = [&](size_t i) -> uint32_t { return i < n ? points[i].code : 0xffffffffu; };
    auto digit = [&](size_t i) { return i < n && decimal_digit(points[i].code); };
    std::vector<Token> result;
    long long line = 1, column = 1;
    size_t i = 0;
    while (i < n) {
        const uint32_t c = at(i);
        size_t j = i + 1;
        enum { Skip, Comment, Number, Name, Symbol, Invalid } kind = Invalid;
        if (detail::python_space(c)) {
            while (j < n && detail::python_space(at(j))) ++j;
            kind = Skip;
        } else if (c == '/' && at(i + 1) == '/') {
            j = i + 2;
            while (j < n && at(j) != '\n') ++j;
            kind = Skip;
        } else if (c == '/' && at(i + 1) == '*') {
            size_t k = i + 2;
            while (k + 1 < n && !(at(k) == '*' && at(k + 1) == '/')) ++k;
            if (k + 1 < n) {
                j = k + 2;
                kind = Skip;
            } else {
                j = i + 2;
                kind = Comment;
            }
        } else if (digit(i) || (c == '.' && digit(i + 1))) {
            size_t k = i;
            if (digit(k)) {
                while (digit(k)) ++k;
                if (at(k) == '.') {
                    ++k;
                    while (digit(k)) ++k;
                }
            } else {
                ++k;
                while (digit(k)) ++k;
            }
            if (at(k) == 'e' || at(k) == 'E') {
                size_t m = k + 1;
                if (at(m) == '+' || at(m) == '-') ++m;
                if (digit(m)) {
                    while (digit(m)) ++m;
                    k = m;
                }
            }
            j = k;
            kind = Number;
        } else if (name_start(c) || (c == '$' && name_start(at(i + 1)))) {
            j = i + (c == '$' ? 2 : 1);
            while (j < n && name_char(at(j))) ++j;
            kind = Name;
        } else if ((c == '<' || c == '>' || c == '=' || c == '!') && at(i + 1) == '=') {
            j = i + 2;
            kind = Symbol;
        } else if ((c == '&' && at(i + 1) == '&') || (c == '|' && at(i + 1) == '|')) {
            j = i + 2;
            kind = Symbol;
        } else if (single_symbol(c)) {
            kind = Symbol;
        }
        Token token;
        token.text = text.substr(points[i].begin, points[j - 1].end - points[i].begin);
        token.line = line;
        token.column = column;
        if (kind == Name && (token.text == "include" || token.text == "use" || token.text == "import"))
            throw scad_error(token, "External file command " + quoted(token.text) + " is unsupported");
        if (kind == Comment) throw scad_error(token, "Unterminated block comment");
        if (kind == Invalid) throw scad_error(token, "Unsupported character " + character_repr(c));
        if (kind != Skip) {
            token.kind = kind == Number ? Token::Number : kind == Name ? Token::Name : Token::Symbol;
            if (kind == Number)
                for (size_t k = i; k < j; ++k) {
                    int value = 0;
                    if (decimal_digit(points[k].code, &value)) token.digits.push_back(static_cast<char>('0' + value));
                    else token.digits.push_back(static_cast<char>(points[k].code));
                }
            result.push_back(token);
            if (result.size() > kMaxTokens) throw scad_error(token, "Source has too many tokens");
        }
        size_t breaks = 0, last = 0;
        for (size_t k = i; k < j; ++k)
            if (points[k].code == '\n') {
                ++breaks;
                last = k - i;
            }
        if (breaks) {
            line += static_cast<long long>(breaks);
            column = static_cast<long long>(j - i - last);
        } else {
            column += static_cast<long long>(j - i);
        }
        i = j;
    }
    Token end;
    end.line = line;
    end.column = column;
    result.push_back(end);
    return result;
}

// ---- values --------------------------------------------------------------------------------------------

struct List;

// A Python value of the language: True or False, an int, a float, or a Vector (a list).
struct Value {
    enum Kind : unsigned char { Bool, Int, Float, Vector } kind = Float;
    bool boolean = false;
    long long integer = 0;
    double real = 0.0;
    std::shared_ptr<const List> list;

    static Value of_bool(bool value) {
        Value v;
        v.kind = Bool;
        v.boolean = value;
        return v;
    }
    static Value of_int(long long value) {
        Value v;
        v.kind = Int;
        v.integer = value;
        return v;
    }
    static Value of_float(double value) {
        Value v;
        v.kind = Float;
        v.real = value;
        return v;
    }
    bool is_number() const { return kind == Int || kind == Float; }  // type(value) in (int, float)
    bool is_list() const { return kind == Vector; }
    double number() const { return kind == Int ? static_cast<double>(integer) : kind == Float ? real : boolean ? 1.0 : 0.0; }
};

struct List {
    std::vector<Value> items;
    long long cost = 1;
    int depth = 1;
};

long long value_cost(const Value &value) { return value.is_list() ? value.list->cost : 1; }
size_t length(const Value &value) { return value.list->items.size(); }
const std::vector<Value> &items(const Value &value) { return value.list->items; }

bool truthy(const Value &value) {
    switch (value.kind) {
    case Value::Bool:
        return value.boolean;
    case Value::Int:
        return value.integer != 0;
    case Value::Float:
        return value.real != 0.0;
    default:
        return !value.list->items.empty();
    }
}

// a == b in Python: True == 1, an int equals the float of the same value, lists item by item.
bool python_equal(const Value &a, const Value &b) {
    if (a.is_list() || b.is_list()) {
        if (!(a.is_list() && b.is_list()) || length(a) != length(b)) return false;
        for (size_t i = 0; i < length(a); ++i)
            if (!python_equal(items(a)[i], items(b)[i])) return false;
        return true;
    }
    if (a.kind != Value::Float && b.kind != Value::Float) {
        const long long x = a.kind == Value::Int ? a.integer : a.boolean, y = b.kind == Value::Int ? b.integer : b.boolean;
        return x == y;
    }
    return a.number() == b.number();
}

// value == 0 and value != 0 in Python, for any value.
bool equals_zero(const Value &value) { return !value.is_list() && value.number() == 0.0; }

const char *const kNumberRange = "Expected a finite number between -1e+09 and 1e+09";

// scad_language.number: an int or a float within ±1e9.
const Value &number(const Value &value, const Token &token) {
    if (value.kind == Value::Int) {
        if (value.integer < -1000000000LL || value.integer > 1000000000LL) throw scad_error(token, kNumberRange);
    } else if (value.kind != Value::Float || !(-kMaxNumber <= value.real && value.real <= kMaxNumber)) {
        throw scad_error(token, kNumberRange);
    }
    return value;
}

// A math function or operator that Python answers with ValueError or ArithmeticError.
struct MathError {};

Value add(const Value &a, const Value &b) {
    if (a.kind == Value::Int && b.kind == Value::Int) return Value::of_int(a.integer + b.integer);
    return Value::of_float(a.number() + b.number());
}
Value subtract(const Value &a, const Value &b) {
    if (a.kind == Value::Int && b.kind == Value::Int) return Value::of_int(a.integer - b.integer);
    return Value::of_float(a.number() - b.number());
}
// Both within ±1e9 (or a loop index times a step), so a product of ints fits 64 bits.
Value multiply(const Value &a, const Value &b) {
    if (a.kind == Value::Int && b.kind == Value::Int) return Value::of_int(a.integer * b.integer);
    return Value::of_float(a.number() * b.number());
}
Value divide(const Value &a, const Value &b) {
    if (b.number() == 0.0) throw MathError();  // ZeroDivisionError
    return Value::of_float(a.number() / b.number());
}

// SCALAR_OPS[op](a, b) for two numbers.
Value scalar(const std::string &op, const Value &a, const Value &b) {
    if (op == "+") return add(a, b);
    if (op == "-") return subtract(a, b);
    if (op == "*") return multiply(a, b);
    if (op == "/") return divide(a, b);
    const double x = a.number(), y = b.number();
    if (op == "%") {
        // math.fmod: a zero divisor is a domain error.
        if (y == 0.0) throw MathError();
        return Value::of_float(std::fmod(x, y));
    }
    if (op == "^") {
        // math.pow: a result that is not finite is a domain or range error.
        const double r = std::pow(x, y);
        if (!std::isfinite(r)) throw MathError();
        return Value::of_float(r);
    }
    if (op == "<") return Value::of_bool(x < y);
    if (op == "<=") return Value::of_bool(x <= y);
    if (op == ">") return Value::of_bool(x > y);
    return Value::of_bool(x >= y);  // ">="
}

// ---- Python's int beyond 64 bits and sum() -------------------------------------------------------------

// A Python int as large as a sum of products of ints within ±1e9 can be: two's complement in 128 bits.
struct Wide {
    uint64_t lo = 0, hi = 0;
    static Wide of(long long value) {
        Wide w;
        w.lo = static_cast<uint64_t>(value);
        w.hi = value < 0 ? ~uint64_t(0) : 0;
        return w;
    }
    Wide plus(const Wide &other) const {
        Wide r;
        r.lo = lo + other.lo;
        r.hi = hi + other.hi + (r.lo < lo ? 1 : 0);
        return r;
    }
    bool negative() const { return (hi >> 63) != 0; }
    bool fits64() const { return hi == (negative() ? ~uint64_t(0) : 0) && ((lo >> 63) != 0) == negative(); }
    // PyLong_AsDouble: the nearest double, ties to even.
    double to_double() const {
        uint64_t mlo = lo, mhi = hi;
        const bool sign = negative();
        if (sign) {
            mlo = ~mlo + 1;
            mhi = ~mhi + (mlo == 0 ? 1 : 0);
        }
        double magnitude;
        if (mhi == 0) {
            magnitude = static_cast<double>(mlo);
        } else {
            int bits = 64;
            for (uint64_t h = mhi; h; h >>= 1) ++bits;
            const int shift = bits - 53;  // 12 to 75
            // The top 53 bits, and what is shifted out of them.
            uint64_t top, rest_hi = 0, rest_lo;
            if (shift < 64) {
                top = (mhi << (64 - shift)) | (mlo >> shift);
                rest_lo = mlo & ((uint64_t(1) << shift) - 1);
            } else {
                top = mhi >> (shift - 64);
                rest_hi = mhi & ((uint64_t(1) << (shift - 64)) - 1);
                rest_lo = mlo;
            }
            // Compare the rest with half of 2^shift.
            int against_half;
            if (shift <= 64) {
                const uint64_t half = uint64_t(1) << (shift - 1);
                against_half = rest_lo < half ? -1 : rest_lo > half ? 1 : 0;
            } else {
                const uint64_t half_hi = uint64_t(1) << (shift - 65);
                against_half = rest_hi < half_hi ? -1 : rest_hi > half_hi ? 1 : rest_lo ? 1 : 0;
            }
            if (against_half > 0 || (against_half == 0 && (top & 1))) ++top;
            magnitude = std::ldexp(static_cast<double>(top), shift);
        }
        return sign ? -magnitude : magnitude;
    }
};

// One term of a sum: a Python int (within 64 bits) or a float.
struct Term {
    bool is_int;
    long long integer;
    double real;
};

// A Python number that may be an int beyond 64 bits.
struct Total {
    bool is_int = true;
    Wide integer;
    double real = 0.0;
    void add(const Term &term) {
        if (is_int && term.is_int) {
            integer = integer.plus(Wide::of(term.integer));
        } else if (is_int) {
            real = integer.to_double() + term.real;
            is_int = false;
        } else {
            real += term.is_int ? Wide::of(term.integer).to_double() : term.real;
        }
    }
};

bool fits_long(long long value) { return value >= LONG_MIN && value <= LONG_MAX; }

// sum(terms) as CPython 3.12 and later compute it from the int 0: ints in a C integer while they fit,
// then floats with Neumaier's compensation (ints that fit a C long added to them as they are), then plain
// Python addition for whatever is left once a term fits neither.
Total python_sum(const std::vector<Term> &terms) {
    size_t k = 0;
    std::ptrdiff_t whole = 0;
    for (; k < terms.size(); ++k) {
        const Term &term = terms[k];
        if (term.is_int && fits_long(term.integer)) {
            const std::ptrdiff_t b = static_cast<std::ptrdiff_t>(term.integer);
            if (whole >= 0 ? b <= PTRDIFF_MAX - whole : b >= PTRDIFF_MIN - whole) {
                whole += b;
                continue;
            }
        }
        break;
    }
    Total total;
    total.integer = Wide::of(static_cast<long long>(whole));
    if (k == terms.size()) return total;
    total.add(terms[k++]);
    if (!total.is_int) {
        double result = total.real, c = 0.0;
        for (; k < terms.size(); ++k) {
            const Term &term = terms[k];
            if (!term.is_int) {
                const double x = term.real, t = result + x;
                if (std::fabs(result) >= std::fabs(x)) c += (result - t) + x;
                else c += (x - t) + result;
                result = t;
                continue;
            }
            if (fits_long(term.integer)) {
                result += static_cast<double>(static_cast<long>(term.integer));
                continue;
            }
            break;
        }
        if (c != 0.0 && std::isfinite(c)) result += c;
        total.real = result;
    }
    for (; k < terms.size(); ++k) total.add(terms[k]);
    return total;
}

// ---- the parse tree ------------------------------------------------------------------------------------

struct Node;
using NodePtr = std::unique_ptr<Node>;

// A Node of scad_language: a statement or an expression, its token and its parts.
struct Node {
    enum Kind {
        Block, Module, Function, If, For, Assign, Call,                                               // statements
        Literal, Var, Items, Range, Comprehension, Index, Ternary, Unary, Binary, FunctionCall  // expressions
    } kind = Block;
    const Token *token = nullptr;
    std::string name;  // of a definition, assignment, call or variable; an operator
    Value literal;
    std::vector<NodePtr> list;                             // statements, vector items, child statements
    std::vector<std::pair<std::string, NodePtr>> pairs;    // parameters, loop bindings, arguments ("" positional)
    std::vector<bool> named;                               // of arguments: whether each was named
    NodePtr a, b, c;
};

int precedence(const std::string &op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    if (op == "^") return 8;
    return 0;
}

bool reserved(const std::string &name) {
    static const std::set<std::string> words = {"true", "false", "PI", "$children", "if", "else", "module", "function", "for", "children"};
    return words.count(name) != 0;
}

class Parser {
public:
    explicit Parser(const json *source) : tokens_(tokenize(source)) {}

    std::vector<NodePtr> program(int level = 0, const char *close = "") {
        std::vector<NodePtr> statements;
        while (token().text != close) {
            if (token().kind == Token::End) throw scad_error(token(), "Expected " + quoted(close));
            statements.push_back(statement(level));
        }
        if (*close) take();
        return statements;
    }

private:
    std::vector<Token> tokens_;
    size_t at_ = 0;

    const Token &token() const { return tokens_[at_]; }
    const Token &next() const { return tokens_[at_ + 1]; }
    const Token &take() { return tokens_[at_++]; }
    bool accept(const char *spelling) {
        if (token().text == spelling) {
            take();
            return true;
        }
        return false;
    }
    void expect(const char *spelling) {
        if (!accept(spelling)) throw scad_error(token(), "Expected " + quoted(spelling));
    }
    const Token &name(bool binding = false) {
        const Token &t = token();
        if (t.kind != Token::Name) throw scad_error(t, "Expected a name");
        if (binding && (reserved(t.text) || (t.text[0] == '$' && t.text != "$fn"))) throw scad_error(t, "Cannot bind " + quoted(t.text));
        return take();
    }
    void depth(int level) const {
        if (level >= kMaxDepth) throw scad_error(token(), "Source is nested too deeply");
    }
    static NodePtr node(Node::Kind kind, const Token &t) {
        NodePtr n(new Node);
        n->kind = kind;
        n->token = &t;
        return n;
    }

    // Arguments: (name or "", expression), positional first.
    void arguments(int level, Node &call) {
        expect("(");
        std::set<std::string> named;
        if (accept(")")) return;
        while (true) {
            std::string key;
            bool is_named = false;
            if (token().kind == Token::Name && next().text == "=") {
                const Token &n = take();
                take();
                if (named.count(n.text)) throw scad_error(n, "Duplicate argument " + quoted(n.text));
                named.insert(n.text);
                key = n.text;
                is_named = true;
            } else if (!named.empty()) {
                throw scad_error(token(), "Positional arguments must precede named arguments");
            }
            NodePtr value = expression(level + 1);
            call.pairs.emplace_back(key, std::move(value));
            call.named.push_back(is_named);
            if (accept(")")) break;
            expect(",");
        }
    }

    std::vector<std::pair<std::string, NodePtr>> bindings(int level, bool defaults) {
        std::vector<std::pair<std::string, NodePtr>> found;
        std::set<std::string> names;
        if (defaults && accept(")")) return found;
        while (true) {
            const Token &t = name(true);
            if (names.count(t.text)) throw scad_error(t, "Duplicate binding " + quoted(t.text));
            names.insert(t.text);
            NodePtr value;
            if (accept("=")) value = expression(level + 1);
            else if (!defaults) throw scad_error(token(), "Expected an initializer");
            found.emplace_back(t.text, std::move(value));
            if (accept(")")) return found;
            expect(",");
        }
    }

    NodePtr statement(int level) {
        depth(level);
        const Token &t = token();
        if (accept(";")) return node(Node::Block, t);
        if (accept("{")) {
            NodePtr block = node(Node::Block, t);
            block->list = program(level + 1, "}");
            return block;
        }
        if (t.text == "module" || t.text == "function") {
            take();
            const Token &defined = name(true);
            expect("(");
            NodePtr definition = node(t.text == "module" ? Node::Module : Node::Function, t);
            definition->name = defined.text;
            definition->pairs = bindings(level, true);
            if (t.text == "function") {
                expect("=");
                definition->a = expression(level + 1);
                expect(";");
            } else {
                definition->a = statement(level + 1);
            }
            return definition;
        }
        if (accept("if")) {
            expect("(");
            NodePtr choice = node(Node::If, t);
            choice->a = expression(level + 1);
            expect(")");
            choice->b = statement(level + 1);
            if (accept("else")) choice->c = statement(level + 1);
            return choice;
        }
        if (accept("for")) {
            expect("(");
            NodePtr loop = node(Node::For, t);
            loop->pairs = bindings(level, false);
            loop->a = statement(level + 1);
            return loop;
        }
        if (t.kind == Token::Name && next().text == "=") {
            name(true);
            take();
            NodePtr assign = node(Node::Assign, t);
            assign->name = t.text;
            assign->a = expression(level + 1);
            expect(";");
            return assign;
        }
        name();
        NodePtr call = node(Node::Call, t);
        call->name = t.text;
        arguments(level, *call);
        if (accept(";")) {
        } else if (accept("{")) {
            call->list = program(level + 1, "}");
        } else {
            call->list.push_back(statement(level + 1));
        }
        return call;
    }

    NodePtr expression(int level, int minimum = 0) {
        depth(level);
        const Token &t = token();
        NodePtr left;
        if (accept("(")) {
            left = expression(level + 1);
            expect(")");
        } else if (accept("[")) {
            left = vector(t, level);
        } else if (t.text == "+" || t.text == "-" || t.text == "!") {
            take();
            left = node(Node::Unary, t);
            left->name = t.text;
            left->a = expression(level + 1, 7);
        } else if (t.kind == Token::Number) {
            take();
            double value = 0;
            detail::float_from_text(t.digits, value);
            left = node(Node::Literal, t);
            left->literal = number(Value::of_float(value), t);
        } else if (t.kind == Token::Name) {
            take();
            if (token().text == "(") {
                left = node(Node::FunctionCall, t);
                left->name = t.text;
                arguments(level, *left);
            } else {
                left = node(Node::Var, t);
                left->name = t.text;
            }
        } else {
            throw scad_error(t, "Expected a number, variable, vector or expression");
        }
        while (true) {
            const Token &op = token();
            if (accept("[")) {
                NodePtr index = node(Node::Index, op);
                index->a = std::move(left);
                index->b = expression(level + 1);
                left = std::move(index);
                expect("]");
            } else if (op.kind == Token::Symbol && precedence(op.text) && precedence(op.text) >= minimum) {
                take();
                const int right = precedence(op.text) + (op.text != "^");
                NodePtr binary = node(Node::Binary, op);
                binary->name = op.text;
                binary->a = std::move(left);
                binary->b = expression(level + 1, right);
                left = std::move(binary);
            } else {
                break;
            }
        }
        if (minimum == 0 && accept("?")) {
            NodePtr ternary = node(Node::Ternary, t);
            ternary->a = std::move(left);
            ternary->b = expression(level + 1);
            expect(":");
            ternary->c = expression(level + 1);
            left = std::move(ternary);
        }
        return left;
    }

    NodePtr vector(const Token &t, int level) {
        if (accept("]")) return node(Node::Items, t);
        if (accept("for")) {
            expect("(");
            NodePtr comprehension = node(Node::Comprehension, t);
            comprehension->pairs = bindings(level, false);
            comprehension->a = expression(level + 1);
            expect("]");
            return comprehension;
        }
        NodePtr first = expression(level + 1);
        if (accept(":")) {
            NodePtr range = node(Node::Range, t);
            range->a = std::move(first);
            range->b = expression(level + 1);
            if (accept(":")) range->c = expression(level + 1);
            expect("]");
            return range;
        }
        NodePtr items = node(Node::Items, t);
        items->list.push_back(std::move(first));
        while (accept(",") && token().text != "]") items->list.push_back(expression(level + 1));
        expect("]");
        return items;
    }

public:
    // The tokens outlive the tree, which points into them.
    std::vector<Token> &tokens() { return tokens_; }
};

// ---- evaluation ----------------------------------------------------------------------------------------

// A GeometryNode: an operation, its arguments and children, and where in the source it was written.
struct Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;
using Geometries = std::vector<GeometryPtr>;
struct Geometry {
    std::string op;
    std::vector<Value> args;
    std::vector<std::pair<std::string, Value>> kwargs;
    Geometries children;
    long long line = 1, column = 1;
};

using Named = std::vector<std::pair<std::string, Value>>;  // a dictionary of keyword arguments, in order

const Value *lookup(const Named &named, const std::string &key) {
    for (const auto &item : named)
        if (item.first == key) return &item.second;
    return nullptr;
}

struct Scope {
    Scope *parent = nullptr;
    std::map<std::string, Value> values;
    std::map<std::string, const Node *> variables;
    std::map<std::pair<int, std::string>, const Node *> definitions;  // (Node::Module or Function, name)
    std::set<std::string> resolving;
    bool has_input = false;  // input_children is not None
    std::vector<const Node *> input_statements;
    Scope *input_owner = nullptr;

    explicit Scope(Scope *parent_scope = nullptr) : parent(parent_scope) {}

    Scope *find(const std::string &name) {
        for (Scope *scope = this; scope; scope = scope->parent)
            if (scope->values.count(name) || scope->variables.count(name)) return scope;
        return nullptr;
    }
    Scope *find_definition(int kind, const std::string &name) {
        for (Scope *scope = this; scope; scope = scope->parent)
            if (scope->definitions.count({kind, name})) return scope;
        return nullptr;
    }
};

const std::set<std::string> &geometry_commands() {
    static const std::set<std::string> commands = {"square", "circle", "polygon", "cube", "sphere", "cylinder", "union",
                                                   "difference", "intersection", "hull", "translate", "rotate", "scale",
                                                   "mirror", "linear_extrude", "rotate_extrude", "offset", "projection",
                                                   "polyhedron", "multmatrix"};
    return commands;
}
bool facet_operation(const std::string &name) {
    return name == "circle" || name == "sphere" || name == "cylinder" || name == "rotate_extrude" || name == "offset";
}

class Evaluator {
public:
    Geometries sequence(const std::vector<const Node *> &statements, Scope &scope, int level) {
        Geometries nodes;
        for (const Node *statement_node : declarations(statements, scope))
            for (GeometryPtr &geometry : statement(*statement_node, scope, level + 1)) nodes.push_back(std::move(geometry));
        return nodes;
    }

private:
    long long work_ = 0;
    int geometry_count_ = 0;

    void charge(const Token &token, long long amount = 1) {
        work_ += amount;
        if (work_ > kMaxWork) throw scad_error(token, "Script exceeds the evaluation budget");
    }

    void visit(const Node &node, int level) {
        if (level >= kMaxDepth) throw scad_error(*node.token, "Expression or module recursion is too deep");
        charge(*node.token);
    }

    Value vector(std::vector<Value> values, const Token &token) {
        auto list = std::make_shared<List>();
        long long cost = 1;
        int depth = 0;
        for (const Value &value : values) {
            cost += value_cost(value);
            if (value.is_list()) depth = std::max(depth, value.list->depth);
        }
        list->items = std::move(values);
        list->cost = cost;
        list->depth = 1 + depth;
        if (cost > kMaxVectorSize || list->depth > kMaxDepth) throw scad_error(token, "Vector exceeds the size or nesting limit");
        charge(token, cost);
        Value result;
        result.kind = Value::Vector;
        result.list = std::move(list);
        return result;
    }

    GeometryPtr geometry(const Token &token, const std::string &name, std::vector<Value> args, Named kwargs, Geometries children) {
        if (++geometry_count_ > kMaxGeometry) throw scad_error(token, "Script exceeds 1500 geometry nodes");
        long long cost = 1;
        for (const Value &value : args) cost += value_cost(value);
        for (const auto &item : kwargs) cost += value_cost(item.second);
        charge(token, cost);
        GeometryPtr node(new Geometry);
        node->op = name;
        node->args = std::move(args);
        node->kwargs = std::move(kwargs);
        node->children = std::move(children);
        node->line = token.line;
        node->column = token.column;
        return node;
    }

    Geometries group(Geometries nodes, const Token &token) {
        if (nodes.size() <= 1) return nodes;
        Geometries grouped;
        grouped.push_back(geometry(token, "union", {}, {}, std::move(nodes)));
        return grouped;
    }

    Value variable(const std::string &name, Scope &scope, const Token &token, int level) {
        Scope *owner = scope.find(name);
        if (!owner) throw scad_error(token, "Unknown variable " + quoted(name));
        const auto known = owner->values.find(name);
        if (known != owner->values.end()) return known->second;
        if (owner->resolving.count(name)) throw scad_error(token, "Circular variable definition " + quoted(name));
        owner->resolving.insert(name);
        Value value;
        try {
            value = expr(*owner->variables.at(name), *owner, level + 1);
        } catch (...) {
            owner->resolving.erase(name);
            throw;
        }
        owner->resolving.erase(name);
        owner->values[name] = value;
        return value;
    }

    void arguments(const Node &call, Scope &scope, int level, std::vector<Value> &positional, Named &named) {
        for (size_t i = 0; i < call.pairs.size(); ++i) {
            Value value = expr(*call.pairs[i].second, scope, level + 1);
            if (!call.named[i]) positional.push_back(value);
            else named.emplace_back(call.pairs[i].first, value);
        }
    }

    std::unique_ptr<Scope> call_scope(const Node &definition, Scope *owner, Scope &caller, const std::vector<Value> &args,
                                      Named kwargs, int level, const Token &token) {
        std::unique_ptr<Scope> env(new Scope(owner));
        const auto &params = definition.pairs;
        if (args.size() > params.size()) throw scad_error(token, "Too many arguments for " + quoted(definition.name));
        auto take = [&](const std::string &key, Value &out) {
            for (auto it = kwargs.begin(); it != kwargs.end(); ++it)
                if (it->first == key) {
                    out = it->second;
                    kwargs.erase(it);
                    return true;
                }
            return false;
        };
        for (size_t i = 0; i < params.size(); ++i) {
            const std::string &key = params[i].first;
            Value value;
            if (i < args.size()) {
                if (lookup(kwargs, key)) throw scad_error(token, "Duplicate argument " + quoted(key));
                env->values[key] = args[i];
            } else if (take(key, value)) {
                env->values[key] = value;
            } else if (params[i].second) {
                env->values[key] = expr(*params[i].second, *env, level + 1);
            } else {
                throw scad_error(token, "Missing argument " + quoted(key));
            }
        }
        if (!env->values.count("$fn")) {
            Value value;
            if (take("$fn", value)) env->values["$fn"] = value;
            else if (caller.find("$fn")) env->values["$fn"] = variable("$fn", caller, token, level);
        }
        if (!kwargs.empty()) throw scad_error(token, "Unknown argument " + quoted(kwargs.front().first));
        return env;
    }

    // The generator scad_language.Evaluator.iterations, with its consumer as body.
    void iterations(const std::vector<std::pair<std::string, NodePtr>> &bindings, Scope &scope, const Token &token, int level,
                    const std::function<void(Scope &)> &body) {
        if (level + static_cast<int>(bindings.size()) >= kMaxDepth) throw scad_error(token, "Loop is nested too deeply");
        expand(bindings, 0, scope, token, level, body);
    }

    void expand(const std::vector<std::pair<std::string, NodePtr>> &bindings, size_t i, Scope &env, const Token &token, int level,
                const std::function<void(Scope &)> &body) {
        if (i == bindings.size()) {
            body(env);
            return;
        }
        const Value values = expr(*bindings[i].second, env, level + static_cast<int>(i) + 1);
        if (!values.is_list()) throw scad_error(token, "for requires a range or vector");
        if (length(values) > kMaxRange) throw scad_error(token, "Loop sequence exceeds 1024 values");
        for (const Value &value : items(values)) {
            charge(token);
            Scope child(&env);
            child.values[bindings[i].first] = value;
            expand(bindings, i + 1, child, token, level, body);
        }
    }

    Value vector_arithmetic(const std::string &op, const Value &a, const Value &b, const Token &t) {
        const bool av = a.is_list(), bv = b.is_list();
        if (av && bv && length(a) == length(b)) {
            if (op == "+" || op == "-") {
                std::vector<Value> result;
                for (size_t i = 0; i < length(a); ++i) result.push_back(binary(op, items(a)[i], items(b)[i], t));
                return vector(std::move(result), t);
            }
            if (op == "*") {
                charge(t, static_cast<long long>(length(a)));
                std::vector<Term> terms;
                for (size_t i = 0; i < length(a); ++i) {
                    const Value &x = number(items(a)[i], t), &y = number(items(b)[i], t);
                    const Value product = multiply(x, y);
                    terms.push_back({product.kind == Value::Int, product.integer, product.real});
                }
                const Total total = python_sum(terms);
                if (total.is_int) {
                    if (!total.integer.fits64()) throw scad_error(t, kNumberRange);
                    return number(Value::of_int(static_cast<long long>(total.integer.lo)), t);
                }
                return number(Value::of_float(total.real), t);
            }
        }
        if (av && !bv && (op == "*" || op == "/")) {
            std::vector<Value> result;
            for (const Value &x : items(a)) result.push_back(binary(op, x, b, t));
            return vector(std::move(result), t);
        }
        if (bv && !av && op == "*") {
            std::vector<Value> result;
            for (const Value &y : items(b)) result.push_back(binary(op, a, y, t));
            return vector(std::move(result), t);
        }
        throw scad_error(t, "Unsupported vector operation or mismatched vector sizes");
    }

    Value binary(const std::string &op, const Value &a, const Value &b, const Token &t) {
        if (op == "==" || op == "!=") {
            charge(t, value_cost(a) + value_cost(b));
            const bool equal = ((a.kind == Value::Bool) == (b.kind == Value::Bool)) && python_equal(a, b);
            return Value::of_bool(op == "==" ? equal : !equal);
        }
        if (a.is_list() || b.is_list()) return vector_arithmetic(op, a, b, t);
        const Value &x = number(a, t);
        const Value &y = number(b, t);
        Value value;
        try {
            value = scalar(op, x, y);
        } catch (const MathError &) {
            throw scad_error(t, "Invalid arithmetic for " + quoted(op));
        }
        return value.kind == Value::Bool ? value : number(value, t);
    }

    // UNARY_MATH[name](x); MathError where Python raises.
    static Value unary(const std::string &name, const Value &x) {
        const double v = x.number();
        auto finite = [](double r) {
            if (!std::isfinite(r)) throw MathError();
            return Value::of_float(r);
        };
        if (name == "abs") return x.kind == Value::Int ? Value::of_int(x.integer < 0 ? -x.integer : x.integer) : Value::of_float(std::fabs(v));
        if (name == "sqrt") return finite(std::sqrt(v));
        if (name == "floor") return x.kind == Value::Int ? x : Value::of_int(static_cast<long long>(std::floor(v)));
        if (name == "ceil") return x.kind == Value::Int ? x : Value::of_int(static_cast<long long>(std::ceil(v)));
        if (name == "round") return Value::of_int(static_cast<long long>(v >= 0 ? std::floor(v + 0.5) : std::ceil(v - 0.5)));
        if (name == "sin") return finite(pytrig::sine(v * kDegToRad));
        if (name == "cos") return finite(pytrig::cosine(v * kDegToRad));
        if (name == "tan") return finite(pytrig::tangent(v * kDegToRad));
        if (name == "asin") return Value::of_float(finite(std::asin(v)).real * kRadToDeg);
        if (name == "acos") return Value::of_float(finite(std::acos(v)).real * kRadToDeg);
        if (name == "atan") return Value::of_float(std::atan(v) * kRadToDeg);
        if (name == "sign") return Value::of_int((v > 0) - (v < 0));
        if (name == "ln") {
            if (!(v > 0)) throw MathError();
            return finite(std::log(v));
        }
        if (name == "log") {
            if (!(v > 0)) throw MathError();
            return finite(std::log10(v));
        }
        return finite(std::exp(v));  // "exp"
    }

    static bool unary_math(const std::string &name) {
        static const std::set<std::string> names = {"abs", "sqrt", "floor", "ceil", "round", "sin", "cos", "tan",
                                                     "asin", "acos", "atan", "sign", "ln", "log", "exp"};
        return names.count(name) != 0;
    }

    Value builtin(const std::string &name, const std::vector<Value> &args, const Named &kwargs, const Token &t) {
        if (!kwargs.empty()) throw scad_error(t, "Built-in functions take positional arguments");
        if (name == "len" && args.size() == 1 && args[0].is_list()) return Value::of_int(static_cast<long long>(length(args[0])));
        if (name == "norm" && args.size() == 1 && args[0].is_list()) {
            charge(t, static_cast<long long>(length(args[0])));
            std::vector<double> magnitudes;
            double max = 0.0;
            for (const Value &item : items(args[0])) {
                const double x = std::fabs(number(item, t).number());
                magnitudes.push_back(x);
                if (x > max) max = x;
            }
            return number(Value::of_float(pymath::vector_norm(magnitudes.size(), magnitudes.data(), max)), t);
        }
        if (name == "min" || name == "max") {
            const std::vector<Value> &values = args.size() == 1 && args[0].is_list() ? items(args[0]) : args;
            if (values.empty()) throw scad_error(t, name + " requires at least one number");
            charge(t, static_cast<long long>(values.size()));
            Value best = number(values[0], t);
            for (size_t i = 1; i < values.size(); ++i) {
                const Value &item = number(values[i], t);
                if (name == "min" ? item.number() < best.number() : item.number() > best.number()) best = item;
            }
            return best;
        }
        try {
            if (unary_math(name) && args.size() == 1) return number(unary(name, number(args[0], t)), t);
            if (name == "atan2" && args.size() == 2) {
                const double y = number(args[0], t).number(), x = number(args[1], t).number();
                return Value::of_float(pytrig::atan2(y, x) * kRadToDeg);
            }
            if (name == "pow" && args.size() == 2) return binary("^", args[0], args[1], t);
        } catch (const MathError &) {
            throw scad_error(t, "Invalid input to " + name);
        }
        throw scad_error(t, "Unknown function or invalid arguments for " + quoted(name));
    }

    Value expr(const Node &node, Scope &env, int level) {
        visit(node, level);
        const Token &t = *node.token;
        auto ev = [&](const Node &part) { return expr(part, env, level + 1); };
        switch (node.kind) {
        case Node::Literal:
            return node.literal;
        case Node::Var:
            return variable(node.name, env, t, level);
        case Node::Items: {
            std::vector<Value> values;
            for (const NodePtr &item : node.list) values.push_back(ev(*item));
            return vector(std::move(values), t);
        }
        case Node::Range: {
            const Value start = number(ev(*node.a), t);
            const Value middle = number(ev(*node.b), t);
            Value step = Value::of_int(1), end = middle;
            if (node.c) {
                step = middle;
                end = number(ev(*node.c), t);
            }
            if (equals_zero(step)) throw scad_error(t, "Range step must be nonzero");
            const double span = subtract(end, start).number() / step.number();
            if (span < 0) return vector({}, t);
            if (!std::isfinite(span) || span >= static_cast<double>(kMaxRange)) throw scad_error(t, "Range exceeds 1024 values");
            const long long count = static_cast<long long>(std::floor(span + 1e-10)) + 1;
            if (count > static_cast<long long>(kMaxRange)) throw scad_error(t, "Range exceeds 1024 values");
            std::vector<Value> values;
            for (long long i = 0; i < count; ++i) values.push_back(number(add(start, multiply(Value::of_int(i), step)), t));
            return vector(std::move(values), t);
        }
        case Node::Comprehension: {
            std::vector<Value> values;
            iterations(node.pairs, env, t, level, [&](Scope &iteration) {
                if (values.size() >= kMaxRange) throw scad_error(t, "Vector comprehension is too large");
                values.push_back(expr(*node.a, iteration, level + 1));
            });
            return vector(std::move(values), t);
        }
        case Node::Index: {
            const Value vector_value = ev(*node.a);
            if (!vector_value.is_list()) throw scad_error(t, "Indexing requires a vector");
            const Value index = ev(*node.b);
            return items(vector_value)[index_number(index, length(vector_value), t)];
        }
        case Node::Ternary:
            return truthy(ev(*node.a)) ? ev(*node.b) : ev(*node.c);
        case Node::Unary: {
            const Value value = ev(*node.a);
            if (node.name == "!") return Value::of_bool(!truthy(value));
            return binary("*", Value::of_int(node.name == "-" ? -1 : 1), value, t);
        }
        case Node::Binary: {
            const Value a = ev(*node.a);
            if (node.name == "&&") return Value::of_bool(truthy(a) && truthy(ev(*node.b)));
            if (node.name == "||") return Value::of_bool(truthy(a) || truthy(ev(*node.b)));
            return binary(node.name, a, ev(*node.b), t);
        }
        case Node::FunctionCall: {
            std::vector<Value> args;
            Named kwargs;
            arguments(node, env, level, args, kwargs);
            Scope *owner = env.find_definition(Node::Function, node.name);
            if (owner) {
                const Node &definition = *owner->definitions.at({Node::Function, node.name});
                std::unique_ptr<Scope> scope = call_scope(definition, owner, env, args, kwargs, level, t);
                return expr(*definition.a, *scope, level + 1);
            }
            return builtin(node.name, args, kwargs, t);
        }
        default:
            throw scad_error(t, "Invalid expression");
        }
    }

public:
    static size_t index_number(const Value &value, size_t size, const Token &token) {
        const Value &checked = number(value, token);
        const double v = checked.number();
        if (std::trunc(v) != v || v < 0 || v >= static_cast<double>(size)) throw scad_error(token, "Index outside vector bounds");
        return static_cast<size_t>(v);
    }

private:
    std::vector<const Node *> declarations(const std::vector<const Node *> &statements, Scope &env) {
        std::vector<const Node *> geometry_statements;
        for (const Node *node : statements) {
            charge(*node->token);
            if (node->kind == Node::Assign) env.variables[node->name] = node->a.get();
            else if (node->kind == Node::Module || node->kind == Node::Function) env.definitions[{node->kind, node->name}] = node;
            else geometry_statements.push_back(node);
        }
        return geometry_statements;
    }

    static std::vector<const Node *> pointers(const std::vector<NodePtr> &nodes) {
        std::vector<const Node *> result;
        for (const NodePtr &node : nodes) result.push_back(node.get());
        return result;
    }

    Geometries children(const std::vector<Value> &args, const Named &kwargs, Scope &env, const Token &token, int level) {
        Scope *scope = &env;
        while (scope && !scope->has_input) scope = scope->parent;
        if (!scope) throw scad_error(token, "children() can only be used inside a module");
        std::vector<const Node *> statements = scope->input_statements;
        Scope *owner = scope->input_owner;
        if (!kwargs.empty() || args.size() > 1) throw scad_error(token, "Use children() or children(index)");
        if (!args.empty()) statements = {statements[index_number(args[0], statements.size(), token)]};
        Geometries nodes;
        for (const Node *statement_node : statements)
            for (GeometryPtr &geometry : statement(*statement_node, *owner, level + 1)) nodes.push_back(std::move(geometry));
        return group(std::move(nodes), token);
    }

    Geometries statement(const Node &node, Scope &env, int level) {
        visit(node, level);
        const Token &t = *node.token;
        switch (node.kind) {
        case Node::Block: {
            Scope scope(&env);
            return group(sequence(pointers(node.list), scope, level), t);
        }
        case Node::For: {
            Geometries nodes;
            iterations(node.pairs, env, t, level, [&](Scope &child_scope) {
                for (GeometryPtr &geometry : statement(*node.a, child_scope, level + 1)) nodes.push_back(std::move(geometry));
            });
            return group(std::move(nodes), t);
        }
        case Node::If: {
            const Node *choice = truthy(expr(*node.a, env, level + 1)) ? node.b.get() : node.c.get();
            if (!choice) return {};
            Scope scope(&env);
            return statement(*choice, scope, level + 1);
        }
        case Node::Assign:
        case Node::Module:
        case Node::Function:
            declarations({&node}, env);
            return {};
        default:
            break;
        }
        // A call of a module or of a geometry command.
        std::vector<Value> args;
        Named kwargs;
        arguments(node, env, level, args, kwargs);
        Scope *owner = env.find_definition(Node::Module, node.name);
        if (owner) {
            const Node &definition = *owner->definitions.at({Node::Module, node.name});
            std::unique_ptr<Scope> child_env = call_scope(definition, owner, env, args, kwargs, level, t);
            Scope caller(&env);
            const std::vector<const Node *> child_statements = declarations(pointers(node.list), caller);
            child_env->values["$children"] = Value::of_int(static_cast<long long>(child_statements.size()));
            child_env->has_input = true;
            child_env->input_statements = child_statements;
            child_env->input_owner = &caller;
            return group(statement(*definition.a, *child_env, level + 1), t);
        }
        if (node.name == "children") {
            if (!node.list.empty()) throw scad_error(t, "children() cannot take child geometry");
            return children(args, kwargs, env, t, level);
        }
        if (!geometry_commands().count(node.name)) throw scad_error(t, "Unknown or unsupported geometry command " + quoted(node.name));
        Scope child_scope(&env);
        const Value *facets = lookup(kwargs, "$fn");
        if (facets) {
            child_scope.values["$fn"] = *facets;
            if (!facet_operation(node.name))
                kwargs.erase(std::find_if(kwargs.begin(), kwargs.end(), [](const std::pair<std::string, Value> &item) { return item.first == "$fn"; }));
        } else if (facet_operation(node.name) && env.find("$fn")) {
            kwargs.emplace_back("$fn", variable("$fn", env, t, level));
        }
        Geometries child_nodes = sequence(pointers(node.list), child_scope, level);
        Geometries result;
        result.push_back(geometry(t, node.name, std::move(args), std::move(kwargs), std::move(child_nodes)));
        return result;
    }
};

// IDENTIFIER: a name a parameter may have.
bool identifier(const std::string &name) {
    if (name.empty() || !name_start(static_cast<unsigned char>(name[0]))) return false;
    for (const char c : name)
        if (!name_char(static_cast<unsigned char>(c))) return false;
    return true;
}

// scad_language.evaluate(source, parameters).
Geometries evaluate(const json *source, const std::vector<std::pair<std::string, double>> &parameters) {
    Parser parser(source);
    const std::vector<NodePtr> model = parser.program();
    const Token start;
    if (parameters.size() > 128) throw scad_error(start, "Supply at most 128 named numeric parameters");
    Scope base;
    base.values["true"] = Value::of_bool(true);
    base.values["false"] = Value::of_bool(false);
    base.values["PI"] = Value::of_float(pytrig::PI);
    Scope env(&base);
    for (const auto &parameter : parameters) {
        if (!identifier(parameter.first) || reserved(parameter.first)) throw scad_error(start, "Invalid numeric parameter name");
        env.values[parameter.first] = number(Value::of_float(parameter.second), start);
    }
    std::vector<const Node *> statements;
    for (const NodePtr &node : model) statements.push_back(node.get());
    Evaluator evaluator;
    return evaluator.sequence(statements, env, 0);
}

// ---- geometry (scad_geometry.py) -----------------------------------------------------------------------

const char *const kCoordinate = "Expected a finite number within ±100000";

// _number: an int or a float within ±100000, as it is.
const Value &coordinate(const Value *value) {
    if (!value || !value->is_number() || !(-kMaxCoord <= value->number() && value->number() <= kMaxCoord)) throw ValueError(kCoordinate);
    return *value;
}
const Value &coordinate(const Value &value) { return coordinate(&value); }

long long whole(const Value *value, long long low, long long high, const std::string &label) {
    if (!value || !value->is_number() || !(static_cast<double>(low) <= value->number() && value->number() <= static_cast<double>(high)) ||
        std::trunc(value->number()) != value->number())
        throw ValueError(label + " must be a whole number from " + std::to_string(low) + " to " + std::to_string(high));
    return static_cast<long long>(value->number());
}

bool flag(const Value *value) {
    if (!value || value->kind != Value::Bool) throw ValueError("Expected true or false");
    return value->boolean;
}

// _vector(value, length, scalar): the numbers of a list of that length; a number repeated when scalar.
std::vector<Value> numbers_of(const Value *value, size_t size, bool scalar = false) {
    if (scalar && value && value->is_number()) return std::vector<Value>(size, coordinate(*value));
    if (!value || !value->is_list() || length(*value) != size) throw ValueError("Expected a vector of " + std::to_string(size) + " numbers");
    std::vector<Value> result;
    for (const Value &item : items(*value)) result.push_back(coordinate(item));
    return result;
}

std::vector<double> floats(const std::vector<Value> &values) {
    std::vector<double> result;
    for (const Value &value : values) result.push_back(value.number());
    return result;
}

// _segments(f.get('$fn', 0)).
long long segments(const Value *value) {
    if (!value || (value->is_number() && value->number() == 0)) return kDefaultSegments;
    return whole(value, 3, kMaxSegments, "$fn");
}

// _bind(node, positional, named=named).
Named bind(const Geometry &node, std::initializer_list<const char *> positional, std::initializer_list<const char *> named = {}) {
    if (node.args.size() > positional.size()) throw ValueError("Too many positional arguments");
    Named fields;
    size_t i = 0;
    for (const char *key : positional) {
        if (i >= node.args.size()) break;
        fields.emplace_back(key, node.args[i++]);
    }
    auto listed = [](std::initializer_list<const char *> keys, const std::string &key) {
        for (const char *k : keys)
            if (key == k) return true;
        return false;
    };
    for (const auto &item : node.kwargs) {
        if (!listed(positional, item.first) && !listed(named, item.first)) throw ValueError("Unknown argument " + quoted(item.first));
        if (lookup(fields, item.first)) throw ValueError("Duplicate argument " + quoted(item.first));
        fields.push_back(item);
    }
    return fields;
}

const Value *get(const Named &fields, const char *key) { return lookup(fields, key); }
bool has(const Named &fields, const char *key) { return lookup(fields, key) != nullptr; }

// _radius(fields, r, d, default): half the diameter as a float, or the radius as it is.
Value radius(const Named &fields, const char *r = "r", const char *d = "d", const Value &fallback = Value::of_int(1)) {
    if (has(fields, d) && has(fields, r)) throw ValueError(std::string("Choose one of ") + r + " or " + d);
    const Value result = has(fields, d) ? Value::of_float(coordinate(get(fields, d)).number() / 2)
                                        : coordinate(has(fields, r) ? get(fields, r) : &fallback);
    if (result.number() < 0) throw ValueError("Radius cannot be negative");
    return result;
}

void convexity(const Named &fields) {
    // OpenSCAD uses convexity only for its preview renderer.
    if (has(fields, "convexity")) whole(get(fields, "convexity"), 1, 1000, "convexity");
}

// A drawing (CrossSection) or a solid (Manifold).
struct Drawn {
    int dim = 2;
    Region region;
    Shape solid;
};

long long vertex_count(const Region &region) {
    long long count = 0;
    for (const json &ring : region.rings) count += static_cast<long long>(ring.size());
    return count;
}
long long complexity(const Drawn &shape) {
    return shape.dim == 2 ? vertex_count(shape.region) : shape.solid.info.at("triangles").get<long long>();
}
bool is_empty(const Drawn &shape) { return shape.dim == 2 ? shape.region.rings.empty() : shape.solid.info.at("empty").get<bool>(); }
std::vector<double> box(const Drawn &shape) {
    if (shape.dim == 2) return region_bounds(shape.region);
    std::vector<double> bounds;
    for (const json &value : shape.solid.info.at("bounds")) bounds.push_back(value.get<double>());
    return bounds;
}

Drawn drawing(Region region) {
    Drawn shape;
    shape.dim = 2;
    shape.region = std::move(region);
    return shape;
}
Drawn solid_of(Shape solid) {
    Drawn shape;
    shape.dim = 3;
    shape.solid = std::move(solid);
    return shape;
}

json point(double x, double y) { return json::array({x, y}); }

// CrossSection(contours, fill): the rings as floats.
Region drawn_region(const std::vector<std::vector<std::array<double, 2>>> &contours, const char *fill) {
    std::vector<json> rings;
    for (const auto &contour : contours) {
        json ring = json::array();
        for (const auto &p : contour) ring.push_back(point(p[0], p[1]));
        rings.push_back(std::move(ring));
    }
    return region_of(rings, fill);
}

Region region_request(const char *op, const Region &shape, json request) {
    request["rings"] = json(shape.rings);
    return polygon_region(op, std::move(request));
}

// CrossSection._map: the region moved by the affine map of two rows [a b c] [d e f].
Region region_map(const Region &shape, const std::array<std::array<double, 3>, 2> &rows) {
    json request = json::object();
    request["fill"] = "positive";
    request["matrix"] = json::array({json::array({rows[0][0], rows[0][1], rows[0][2]}), json::array({rows[1][0], rows[1][1], rows[1][2]})});
    return region_request("transform", shape, std::move(request));
}

// CrossSection.mirror: in the line through the origin with this normal; nothing for no normal.
Region region_mirror(const Region &shape, double x, double y) {
    const double size = x * x + y * y;
    if (size == 0) return Region();
    return region_map(shape, {{{1 - 2 * x * x / size, -2 * x * y / size, 0.0}, {-2 * x * y / size, 1 - 2 * y * y / size, 0.0}}});
}

Region region_rotate(const Region &shape, double degrees) {
    json request = json::object();
    request["fill"] = "positive";
    request["degrees"] = degrees;
    return region_request("rotate", shape, std::move(request));
}

Shape solid_matrix(const Shape &shape, const std::array<std::array<double, 4>, 3> &rows) {
    json matrix = json::array();
    for (const auto &row : rows) matrix.push_back(json::array({row[0], row[1], row[2], row[3]}));
    json node = json::object();
    node["matrix"] = std::move(matrix);
    node["of"] = kept_node(0);
    return ask(node, {&shape});
}

// solid_kernel.triangulate: one simple polygon cut into triangles a corner at a time.
std::vector<std::array<size_t, 3>> triangulate(const std::vector<std::array<double, 2>> &points) {
    auto cross = [&](size_t a, size_t b, size_t c) {
        return (points[b][0] - points[a][0]) * (points[c][1] - points[a][1]) - (points[b][1] - points[a][1]) * (points[c][0] - points[a][0]);
    };
    std::vector<size_t> order;
    for (size_t i = 0; i < points.size(); ++i) order.push_back(i);
    std::vector<double> turns;
    for (size_t i = 1; i + 1 < order.size(); ++i) turns.push_back(cross(order[0], order[i], order[i + 1]));
    if (pymath::sum(turns.begin(), turns.end()) < 0) std::reverse(order.begin(), order.end());
    std::vector<std::array<size_t, 3>> triangles;
    const char *const failed = "The polygon could not be divided into triangles";
    while (order.size() > 3) {
        bool cut = false;
        for (size_t at = 0; at < order.size(); ++at) {
            const size_t a = order[(at + order.size() - 1) % order.size()], b = order[at], c = order[(at + 1) % order.size()];
            if (!(cross(a, b, c) > 0)) continue;
            bool inside = false;
            for (const size_t p : order) {
                if (p == a || p == b || p == c) continue;
                if (cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0) {
                    inside = true;
                    break;
                }
            }
            if (inside) continue;
            triangles.push_back({a, b, c});
            order.erase(order.begin() + static_cast<std::ptrdiff_t>(at));
            cut = true;
            break;
        }
        if (!cut) throw ValueError(failed);
    }
    if (cross(order[0], order[1], order[2]) <= 0) throw ValueError(failed);
    triangles.push_back({order[0], order[1], order[2]});
    return triangles;
}

// LinearRing(points).is_ring: Shapely closes the ring when its ends differ, then asks GEOS whether the
// closed line is simple.
bool ring_is_simple(const std::vector<std::array<double, 2>> &points) {
    detail::Contour ring(points.begin(), points.end());
    if (!(ring.front()[0] == ring.back()[0] && ring.front()[1] == ring.back()[1])) ring.push_back(ring.front());
    return detail::line_is_simple(ring);
}

// np.linalg.det, for the threshold test only: LU decomposition with partial pivoting, then the sign and
// the sum of the logarithms of the pivots, as NumPy forms the determinant from LAPACK's factors.
double determinant(std::vector<std::vector<double>> a) {
    const size_t n = a.size();
    double sign = 1.0;
    for (size_t j = 0; j < n; ++j) {
        size_t pivot = j;
        for (size_t i = j + 1; i < n; ++i)
            if (std::fabs(a[i][j]) > std::fabs(a[pivot][j])) pivot = i;
        if (a[pivot][j] == 0) return 0.0;
        if (pivot != j) {
            std::swap(a[pivot], a[j]);
            sign = -sign;
        }
        const double reciprocal = 1.0 / a[j][j];
        for (size_t i = j + 1; i < n; ++i) {
            a[i][j] *= reciprocal;
            for (size_t k = j + 1; k < n; ++k) a[i][k] -= a[i][j] * a[j][k];
        }
    }
    double logarithm = 0.0;
    for (size_t j = 0; j < n; ++j) {
        double element = a[j][j];
        if (element < 0) {
            sign = -sign;
            element = -element;
        }
        logarithm += std::log(element);
    }
    return sign * std::exp(logarithm);
}

class Builder {
public:
    explicit Builder(Budget &budget) : budget_(budget) {}

    void spend(long long units) {
        budget_.remaining -= units;
        if (budget_.remaining < 0) throw ValueError("Script exceeds the geometry work budget; reduce repetitions or $fn");
    }

    Drawn check(Drawn shape) {
        const long long count = complexity(shape);
        const long long limit = shape.dim == 2 ? kMaxVertices : kMaxTriangles;
        if (count > limit) throw ValueError("Operation exceeds the " + std::to_string(limit) + " geometry element limit");
        spend(count);
        if (!is_empty(shape))
            for (const double n : box(shape))
                if (!std::isfinite(n) || std::fabs(n) > kMaxCoord) throw ValueError("Geometry exceeds ±100000 mm");
        return shape;
    }

    Drawn combine(const std::string &op, std::vector<Drawn> shapes) {
        if (shapes.empty()) throw ValueError("Supply child geometry");
        const int dim = shapes[0].dim;
        for (const Drawn &shape : shapes)
            if (shape.dim != dim) throw ValueError("Cannot combine 2D and 3D geometry; extrude or project first");
        if (shapes.size() == 1 && op != "hull") return std::move(shapes[0]);
        std::vector<long long> sizes;
        long long total = 0;
        for (const Drawn &shape : shapes) {
            sizes.push_back(complexity(shape));
            total += sizes.back();
        }
        if (total > (dim == 2 ? kMaxVertices : kMaxTriangles)) throw ValueError("Too much input geometry in this operation; simplify the children");
        spend(total);
        if (op == "hull") {
            if (dim == 3) return solid_of(combined("hull", pointers(shapes)));
            json request = json::object();
            request["rings"] = json(all_rings(shapes, 0));
            return drawing(polygon_region("hull", std::move(request)));
        }
        // Pairwise intersection work for overlapping operands; the kernel's allowance is four times
        // manifold3d's, as in the reference.
        const long long allowed = kMaxOverlap * 4;
        std::vector<std::vector<double>> boxes;
        for (const Drawn &shape : shapes) boxes.push_back(box(shape));
        long long overlap = 0;
        for (size_t i = 0; i < boxes.size(); ++i)
            for (size_t j = 0; j < i; ++j) {
                const std::vector<double> &a = boxes[i], &b = boxes[j];
                bool meet = true;
                for (int axis = 0; axis < dim && meet; ++axis)
                    meet = a[axis] <= b[dim + axis] && b[axis] <= a[dim + axis];
                if (meet) {
                    overlap += sizes[i] * sizes[j];
                    if (overlap > allowed) throw ValueError("Overlapping geometry exceeds the intersection work budget; reduce $fn");
                }
            }
        if (dim == 3) return solid_of(combined(op.c_str(), pointers(shapes)));
        if (op == "union") return drawing(region_of(all_rings(shapes, 0), "positive"));
        if (op == "difference") {
            json request = json::object();
            request["a"] = json(shapes[0].region.rings);
            request["b"] = json(all_rings(shapes, 1));
            request["fill"] = "positive";
            return drawing(polygon_region("difference", std::move(request)));
        }
        Region result = shapes[0].region;
        for (size_t i = 1; i < shapes.size(); ++i) {
            json request = json::object();
            request["a"] = json(result.rings);
            request["b"] = json(shapes[i].region.rings);
            request["fill"] = "positive";
            result = polygon_region("intersection", std::move(request));
        }
        return drawing(std::move(result));
    }

    Drawn node(const Geometry &geometry, int depth) {
        if (depth > 64 || budget_.nodes >= 1024) throw ValueError("Script exceeds the geometry nesting or instance limit");
        budget_.nodes += 1;
        std::vector<Drawn> children;
        for (const GeometryPtr &child : geometry.children) children.push_back(node(*child, depth + 1));
        const std::string location = "Line " + std::to_string(geometry.line) + ", column " + std::to_string(geometry.column) + ": ";
        try {
            return check(operation(geometry, std::move(children)));
        } catch (const NotPorted &) {
            throw;
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &error) {
            // ValueError, and what the extension would raise as RuntimeError, are reported with the line.
            throw ValueError(location + geometry.op + ": " + error.what());
        }
    }

private:
    Budget &budget_;

    static std::vector<const Shape *> pointers(const std::vector<Drawn> &shapes) {
        std::vector<const Shape *> result;
        for (const Drawn &shape : shapes) result.push_back(&shape.solid);
        return result;
    }
    static std::vector<json> all_rings(const std::vector<Drawn> &shapes, size_t from) {
        std::vector<json> rings;
        for (size_t i = from; i < shapes.size(); ++i)
            for (const json &ring : shapes[i].region.rings) rings.push_back(ring);
        return rings;
    }

    Drawn polygon(const Named &fields) {
        const Value *points = get(fields, "points");
        if (!points || !points->is_list() || length(*points) < 3 || length(*points) > 2048) throw ValueError("A polygon needs 3–2048 points");
        std::vector<std::vector<Value>> vertices;
        for (const Value &item : items(*points)) vertices.push_back(numbers_of(&item, 2));
        std::vector<std::vector<size_t>> paths;
        const Value *given = get(fields, "paths");
        if (!given) {
            paths.emplace_back();
            for (size_t i = 0; i < vertices.size(); ++i) paths.back().push_back(i);
        } else {
            if (!given->is_list() || length(*given) < 1 || length(*given) > 128) throw ValueError("A polygon needs 1–128 paths");
            size_t total = 0;
            bool short_path = false;
            for (const Value &path : items(*given)) {
                if (!path.is_list() || length(path) < 3) {
                    short_path = true;
                    break;
                }
            }
            if (!short_path)
                for (const Value &path : items(*given)) total += length(path);
            if (short_path || total > 2048) throw ValueError("Polygon paths need at least three indices each and at most 2048 in total");
            for (const Value &path : items(*given)) {
                paths.emplace_back();
                for (const Value &index : items(path))
                    paths.back().push_back(static_cast<size_t>(whole(&index, 0, static_cast<long long>(vertices.size()) - 1, "Point index")));
                // Each path is checked as soon as its indices are read.
                check_path(vertices, paths.back());
            }
            return drawing(contours_region(vertices, paths));
        }
        check_path(vertices, paths.back());
        return drawing(contours_region(vertices, paths));
    }

    static void check_path(const std::vector<std::vector<Value>> &vertices, const std::vector<size_t> &path) {
        std::vector<std::array<double, 2>> contour;
        for (const size_t i : path) contour.push_back({vertices[i][0].number(), vertices[i][1].number()});
        if (!ring_is_simple(contour)) throw ValueError("Polygon paths must enclose an area without crossing themselves");
    }

    static Region contours_region(const std::vector<std::vector<Value>> &vertices, const std::vector<std::vector<size_t>> &paths) {
        std::vector<std::vector<std::array<double, 2>>> contours;
        for (const auto &path : paths) {
            contours.emplace_back();
            for (const size_t i : path) contours.back().push_back({vertices[i][0].number(), vertices[i][1].number()});
        }
        return drawn_region(contours, "evenodd");
    }

    Drawn polyhedron(const Named &fields) {
        const Value *raw_points = get(fields, "points"), *faces = get(fields, "faces");
        if (!raw_points || !raw_points->is_list() || length(*raw_points) < 4 || length(*raw_points) > 256)
            throw ValueError("A polyhedron needs 4–256 vertices");
        if (!faces || !faces->is_list() || length(*faces) < 4 || length(*faces) > 512) throw ValueError("A polyhedron needs 4–512 faces");
        size_t total = 0;
        for (const Value &face : items(*faces))
            if (!face.is_list() || length(face) < 3 || length(face) > 256) throw ValueError("Each face needs 3–256 indices");
        for (const Value &face : items(*faces)) total += length(face);
        if (total > 4096) throw ValueError("Polyhedron exceeds the face index budget");
        std::vector<std::array<double, 3>> points;
        for (const Value &item : items(*raw_points)) {
            const std::vector<double> p = floats(numbers_of(&item, 3));
            points.push_back({p[0], p[1], p[2]});
        }
        double widest = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            double low = points[0][axis], high = points[0][axis];
            for (const auto &p : points) {
                low = std::min(low, p[axis]);
                high = std::max(high, p[axis]);
            }
            widest = axis == 0 ? high - low : std::max(widest, high - low);
        }
        const double scaled = widest * 1e-9;
        const double tolerance = scaled > 1e-9 ? scaled : 1e-9;
        json triangles = json::array();
        for (const Value &face : items(*faces)) {
            // The kernels use counter-clockwise exterior winding; OpenSCAD's external faces are clockwise.
            std::vector<size_t> indices;
            for (auto it = items(face).rbegin(); it != items(face).rend(); ++it)
                indices.push_back(static_cast<size_t>(whole(&*it, 0, static_cast<long long>(points.size()) - 1, "Vertex index")));
            if (std::set<size_t>(indices.begin(), indices.end()).size() != indices.size()) throw ValueError("A face cannot repeat a vertex");
            const auto &origin = points[indices[0]];
            std::vector<std::array<double, 3>> relative;
            for (const size_t i : indices)
                relative.push_back({points[i][0] - origin[0], points[i][1] - origin[1], points[i][2] - origin[2]});
            std::array<double, 3> normal = {0.0, 0.0, 0.0};
            for (size_t i = 0; i < relative.size(); ++i) {
                const auto &a = relative[i], &b = relative[(i + 1) % relative.size()];
                normal[0] += a[1] * b[2] - a[2] * b[1];
                normal[1] += a[2] * b[0] - a[0] * b[2];
                normal[2] += a[0] * b[1] - a[1] * b[0];
            }
            const double size = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (size <= std::pow(tolerance, 2.0)) throw ValueError("A polyhedron face needs a nonzero area");
            for (double &component : normal) component /= size;
            std::vector<double> distances;
            for (const auto &p : points)
                distances.push_back((p[0] - origin[0]) * normal[0] + (p[1] - origin[1]) * normal[1] + (p[2] - origin[2]) * normal[2]);
            double off_plane = 0.0;
            for (const size_t i : indices) off_plane = std::max(off_plane, std::fabs(distances[i]));
            if (off_plane > tolerance) throw ValueError("Polyhedron faces must be planar");
            if (*std::max_element(distances.begin(), distances.end()) > tolerance)
                throw ValueError("Use a convex polyhedron with faces wound clockwise when seen from outside");
            int axis = 0;
            for (int k = 1; k < 3; ++k)
                if (std::fabs(normal[k]) > std::fabs(normal[axis])) axis = k;
            const double sign = normal[axis] > 0 ? 1.0 : -1.0;
            std::vector<std::array<double, 2>> xy;
            for (const auto &r : relative) xy.push_back({r[(axis + 1) % 3], r[(axis + 2) % 3] * sign});
            if (!ring_is_simple(xy)) throw ValueError("Polyhedron face edges cannot cross");
            for (const auto &triangle : triangulate(xy))
                triangles.push_back(json::array({static_cast<std::uint64_t>(indices[triangle[0]]), static_cast<std::uint64_t>(indices[triangle[1]]),
                                                 static_cast<std::uint64_t>(indices[triangle[2]])}));
        }
        json vertices = json::array();
        for (const auto &p : points) vertices.push_back(json::array({p[0], p[1], p[2]}));
        json mesh = json::object();
        mesh["vertices"] = std::move(vertices);
        mesh["triangles"] = std::move(triangles);
        mesh["weld"] = false;
        const char *const open = "Polyhedron faces must enclose one consistently oriented solid";
        Shape solid;
        try {
            solid = ask(json{{"mesh", mesh}}, {});
        } catch (const ValueError &) {
            throw ValueError(open);
        }
        if (solid.info.at("empty").get<bool>() || solid.info.at("volume").get<double>() <= 0) throw ValueError(open);
        return solid_of(std::move(solid));
    }

    Drawn primitive(const Geometry &geometry) {
        const std::string &op = geometry.op;
        if (op == "square" || op == "cube") {
            const Named f = bind(geometry, {"size", "center"});
            const size_t dim = op == "square" ? 2 : 3;
            static const Value one = Value::of_int(1);
            const std::vector<Value> size = numbers_of(has(f, "size") ? get(f, "size") : &one, dim, true);
            for (const Value &value : size)
                if (value.number() <= 0) throw ValueError("Size must be positive");
            static const Value no = Value::of_bool(false);
            const bool center = flag(has(f, "center") ? get(f, "center") : &no);
            const std::vector<double> lengths = floats(size);
            if (dim == 2) {
                const double width = lengths[0], height = lengths[1];
                const double x = center ? -width / 2 : 0.0, y = center ? -height / 2 : 0.0;
                return drawing(drawn_region({{{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}}}, "positive"));
            }
            json cube = json::object();
            cube["box"] = numbers(lengths);
            cube["center"] = center;
            return solid_of(ask(cube, {}));
        }
        if (op == "circle" || op == "sphere") {
            const Named f = bind(geometry, {"r"}, {"d", "$fn"});
            const Value r = radius(f);
            if (r.number() == 0) throw ValueError("Radius must be positive");
            const long long count = segments(get(f, "$fn"));
            if (op == "circle") {
                json request = json::object();
                request["radius"] = r.number();
                request["segments"] = count;
                return drawing(polygon_region("circle", std::move(request)));
            }
            json sphere = json::object();
            sphere["radius"] = r.number();
            sphere["segments"] = count;
            return solid_of(ask(json{{"sphere", sphere}}, {}));
        }
        if (op == "cylinder") {
            const Named f = bind(geometry, {"h", "r1", "r2", "center"}, {"r", "d", "d1", "d2", "$fn"});
            if ((has(f, "r") || has(f, "d")) && (has(f, "r1") || has(f, "r2") || has(f, "d1") || has(f, "d2")))
                throw ValueError("Choose a common radius/diameter or individual end radii");
            const Value common = radius(f);
            const Value low = radius(f, "r1", "d1", common), high = radius(f, "r2", "d2", common);
            static const Value one = Value::of_int(1);
            const Value height = coordinate(has(f, "h") ? get(f, "h") : &one);
            if (height.number() <= 0 || (high.number() > low.number() ? high : low).number() <= 0)
                throw ValueError("Cylinder height and at least one end radius must be positive");
            const long long count = segments(get(f, "$fn"));
            static const Value no = Value::of_bool(false);
            const bool center = flag(has(f, "center") ? get(f, "center") : &no);
            json cylinder = json::object();
            cylinder["height"] = height.number();
            cylinder["radius_low"] = low.number();
            cylinder["radius_high"] = high.number();
            cylinder["segments"] = count;
            cylinder["center"] = center;
            return solid_of(ask(json{{"cylinder", cylinder}}, {}));
        }
        if (op == "polyhedron") {
            const Named f = bind(geometry, {"points", "faces", "convexity"});
            convexity(f);
            return polyhedron(f);
        }
        const Named f = bind(geometry, {"points", "paths", "convexity"});
        convexity(f);
        return polygon(f);
    }

    Drawn extrude(const Drawn &shape, const std::string &op, const Named &f) {
        if (shape.dim != 2) throw ValueError("Extrusion requires a 2D drawing");
        convexity(f);
        const long long vertices = vertex_count(shape.region), contours = static_cast<long long>(shape.region.rings.size());
        if (op == "linear_extrude") {
            static const Value one = Value::of_int(1), zero = Value::of_int(0), no = Value::of_bool(false);
            const Value height = coordinate(has(f, "height") ? get(f, "height") : &one);
            if (height.number() <= 0) throw ValueError("Extrusion height must be positive");
            const Value twist = coordinate(has(f, "twist") ? get(f, "twist") : &zero);
            const double turn = std::fabs(twist.number());
            const Value fallback = Value::of_int(std::max<long long>(1, static_cast<long long>(std::ceil(turn / 12))));
            const long long slices = whole(has(f, "slices") ? get(f, "slices") : &fallback, 1, 256, "slices");
            if (turn / static_cast<double>(slices) >= 180) throw ValueError("Use more extrusion slices for this twist");
            const std::vector<Value> scale = numbers_of(has(f, "scale") ? get(f, "scale") : &one, 2, true);
            if (std::min(scale[0].number(), scale[1].number()) < 0) throw ValueError("Extrusion scale must be nonnegative");
            const bool center = flag(has(f, "center") ? get(f, "center") : &no);
            // Each wall edge becomes two triangles per slice, plus two triangulated caps.
            const long long faces = 2 * vertices * (slices + 1) + 4 * contours;
            if (faces > kMaxTriangles) throw ValueError("Extrusion exceeds the triangle budget; reduce slices or $fn");
            spend(faces);
            json extrusion = json::object();
            extrusion["rings"] = json(shape.region.rings);
            extrusion["height"] = height.number();
            extrusion["slices"] = slices;
            extrusion["twist"] = twist.kind == Value::Int ? static_cast<double>(-twist.integer) : -twist.real;
            extrusion["scale"] = numbers(floats(scale));
            extrusion["fill"] = "positive";
            Shape solid = ask(json{{"extrude", extrusion}}, {});
            if (center) solid = mapped(solid, "translate", {0.0, 0.0, -height.number() / 2});
            return solid_of(std::move(solid));
        }
        static const Value full = Value::of_int(360);
        const Value angle = coordinate(has(f, "angle") ? get(f, "angle") : &full);
        const double magnitude = std::fabs(angle.number());
        if (!(0 < magnitude && magnitude <= 360)) throw ValueError("Revolve angle must have a magnitude above 0 and at most 360 degrees");
        const long long count = segments(get(f, "$fn"));
        const long long faces = 2 * vertices * (count + 1) + 4 * contours;
        if (faces > kMaxTriangles) throw ValueError("Revolution exceeds the triangle budget; reduce profile vertices or $fn");
        spend(faces);
        const std::vector<double> bounds = region_bounds(shape.region);
        const double left = bounds[0], right = bounds[2];
        if (left < 0 && 0 < right) throw ValueError("A revolved profile must lie on one side of the Y axis");
        const bool negative_x = left < 0;
        const Region profile = negative_x ? region_mirror(shape.region, 1.0, 0.0) : shape.region;
        json revolution = json::object();
        revolution["rings"] = json(profile.rings);
        revolution["segments"] = count;
        revolution["degrees"] = magnitude;
        revolution["fill"] = "positive";
        Shape out = ask(json{{"revolve", revolution}}, {});
        if (angle.number() < 0) out = mapped(out, "mirror", {0.0, 1.0, 0.0});
        if (negative_x) out = mapped(out, "rotate", {0.0, 0.0, 180.0});
        return solid_of(std::move(out));
    }

    // _transform_vector(value, dim, pad).
    static std::vector<Value> transform_vector(const Value &value, int dim, long long pad) {
        if (value.is_list() && length(value) == 2 && dim == 3) {
            std::vector<Value> padded = items(value);
            padded.push_back(Value::of_int(pad));
            Value list;
            list.kind = Value::Vector;
            auto data = std::make_shared<List>();
            data->items = std::move(padded);
            list.list = data;
            return numbers_of(&list, 3);
        }
        if (value.is_list() && length(value) == 3 && dim == 2) {
            std::vector<Value> first = numbers_of(&value, 3);
            first.pop_back();
            return first;
        }
        return numbers_of(&value, static_cast<size_t>(dim));
    }

    Drawn rotate(const Drawn &shape, const Named &f) {
        static const Value zero = Value::of_int(0);
        const Value &angle = has(f, "a") ? *get(f, "a") : zero;
        const char *const in_plane = "Rotate a drawing about Z; extrude before rotating it out of plane";
        if (angle.is_list()) {
            if (has(f, "v")) throw ValueError("Use Euler angles or one angle and a rotation axis");
            const std::vector<double> xyz = floats(numbers_of(&angle, 3));
            if (shape.dim == 2) {
                if (xyz[0] != 0 || xyz[1] != 0) throw ValueError(in_plane);
                return drawing(region_rotate(shape.region, xyz[2]));
            }
            return solid_of(mapped(shape.solid, "rotate", xyz));
        }
        const double degrees = coordinate(angle).number();
        static const Value up = [] {
            Value v;
            v.kind = Value::Vector;
            auto data = std::make_shared<List>();
            data->items = {Value::of_int(0), Value::of_int(0), Value::of_int(1)};
            v.list = data;
            return v;
        }();
        const std::vector<double> axis = floats(numbers_of(has(f, "v") ? get(f, "v") : &up, 3));
        double x = axis[0], y = axis[1], z = axis[2];
        double magnitudes[3] = {std::fabs(x), std::fabs(y), std::fabs(z)};
        const double size = pymath::vector_norm(3, magnitudes, std::max(magnitudes[0], std::max(magnitudes[1], magnitudes[2])));
        if (size == 0) throw ValueError("Rotation axis must have a direction");
        if (shape.dim == 2) {
            if (x != 0 || y != 0) throw ValueError(in_plane);
            return drawing(region_rotate(shape.region, std::copysign(1.0, z) * degrees));
        }
        if (x == y && y == 0) return solid_of(mapped(shape.solid, "rotate", {0.0, 0.0, std::copysign(1.0, z) * degrees}));
        x /= size;
        y /= size;
        z /= size;
        const double c = pytrig::cosine(degrees * kDegToRad), s = pytrig::sine(degrees * kDegToRad);
        const double t = 1 - c;
        return solid_of(solid_matrix(shape.solid, {{{t * x * x + c, t * x * y - s * z, t * x * z + s * y, 0.0},
                                                    {t * x * y + s * z, t * y * y + c, t * y * z - s * x, 0.0},
                                                    {t * x * z - s * y, t * y * z + s * x, t * z * z + c, 0.0}}}));
    }

    Drawn matrix(const Drawn &shape, const Value *value) {
        if (!value || !value->is_list() || (length(*value) != 3 && length(*value) != 4)) throw ValueError("Use a 3×4 or 4×4 affine matrix");
        std::vector<std::vector<Value>> rows;
        for (const Value &row : items(*value)) rows.push_back(numbers_of(&row, 4));
        if (rows.size() == 4) {
            const long long last[4] = {0, 0, 0, 1};
            for (int k = 0; k < 4; ++k)
                if (!python_equal(rows[3][k], Value::of_int(last[k]))) throw ValueError("The affine matrix must end in [0,0,0,1]");
        }
        std::vector<std::vector<double>> m;
        for (size_t i = 0; i < 3; ++i) m.push_back(floats(rows[i]));
        if (shape.dim == 2) {
            if (m[2][0] != 0 || m[2][1] != 0 || m[2][3] != 0) throw ValueError("Keep the 2D drawing in the XY plane");
            if (std::fabs(determinant({{m[0][0], m[0][1]}, {m[1][0], m[1][1]}})) < 1e-14)
                throw ValueError("The affine matrix must have a nonsingular linear part");
            return drawing(region_map(shape.region, {{{m[0][0], m[0][1], m[0][3]}, {m[1][0], m[1][1], m[1][3]}}}));
        }
        if (std::fabs(determinant({{m[0][0], m[0][1], m[0][2]}, {m[1][0], m[1][1], m[1][2]}, {m[2][0], m[2][1], m[2][2]}})) < 1e-14)
            throw ValueError("The affine matrix must have a nonsingular linear part");
        return solid_of(solid_matrix(shape.solid, {{{m[0][0], m[0][1], m[0][2], m[0][3]},
                                                    {m[1][0], m[1][1], m[1][2], m[1][3]},
                                                    {m[2][0], m[2][1], m[2][2], m[2][3]}}}));
    }

    Drawn operation(const Geometry &geometry, std::vector<Drawn> children) {
        const std::string &op = geometry.op;
        if (op == "square" || op == "cube" || op == "circle" || op == "sphere" || op == "cylinder" || op == "polygon" || op == "polyhedron") {
            if (!children.empty()) throw ValueError("A primitive cannot contain children");
            return primitive(geometry);
        }
        if (op == "union" || op == "difference" || op == "intersection" || op == "hull") {
            bind(geometry, {});
            return combine(op, std::move(children));
        }
        const Drawn shape = combine("union", std::move(children));
        const int dim = shape.dim;
        if (op == "translate" || op == "scale" || op == "mirror") {
            const Named f = bind(geometry, {"v"});
            if (!has(f, "v")) throw ValueError("Supply a vector");
            Value value = *get(f, "v");
            if (op == "scale" && value.is_number()) {
                const Value factor = coordinate(value);
                Value list;
                list.kind = Value::Vector;
                auto data = std::make_shared<List>();
                data->items = std::vector<Value>(static_cast<size_t>(dim), factor);
                list.list = data;
                value = list;
            }
            if (op == "mirror" && dim == 2 && value.is_list() && length(value) == 3 && !equals_zero(items(value)[2]))
                throw ValueError("Use an in-plane mirror normal for a drawing");
            const std::vector<Value> vector = transform_vector(value, dim, op == "scale" ? 1 : 0);
            if (op == "scale")
                for (const Value &factor : vector)
                    if (equals_zero(factor)) throw ValueError("Scale factors must be nonzero");
            if (op == "mirror" && std::all_of(vector.begin(), vector.end(), equals_zero)) throw ValueError("Mirror normal must have a direction");
            const std::vector<double> v = floats(vector);
            if (dim == 3) return solid_of(mapped(shape.solid, op.c_str(), v));
            if (op == "translate") return drawing(region_map(shape.region, {{{1.0, 0.0, v[0]}, {0.0, 1.0, v[1]}}}));
            if (op == "scale") return drawing(region_map(shape.region, {{{v[0], 0.0, 0.0}, {0.0, v[1], 0.0}}}));
            return drawing(region_mirror(shape.region, v[0], v[1]));
        }
        if (op == "multmatrix") return matrix(shape, get(bind(geometry, {"m"}), "m"));
        if (op == "rotate") return rotate(shape, bind(geometry, {"a", "v"}));
        if (op == "linear_extrude") return extrude(shape, op, bind(geometry, {"height", "center", "convexity", "twist", "slices", "scale"}));
        if (op == "rotate_extrude") return extrude(shape, op, bind(geometry, {"angle", "convexity"}, {"$fn"}));
        if (op == "offset") {
            if (dim != 2) throw ValueError("Offset needs a 2D drawing");
            const Named f = bind(geometry, {"r"}, {"delta", "chamfer", "$fn"});
            if (has(f, "delta") && has(f, "r")) throw ValueError("Choose delta or r");
            static const Value no = Value::of_bool(false), one = Value::of_int(1);
            const bool chamfer = flag(has(f, "chamfer") ? get(f, "chamfer") : &no);
            if (chamfer && !has(f, "delta")) throw ValueError("Use chamfer with delta");
            const long long count = segments(get(f, "$fn"));
            const char *join = "round";
            if (has(f, "delta")) join = chamfer ? "bevel" : "miter";
            spend(vertex_count(shape.region) * (std::string(join) == "round" ? count : 2));
            const Value &distance = coordinate(has(f, "delta") ? get(f, "delta") : has(f, "r") ? get(f, "r") : &one);
            json request = json::object();
            request["fill"] = "positive";
            request["distance"] = distance.number();
            request["join"] = join;
            request["turn_segments"] = count;
            request["miter_limit"] = 2.0;
            return drawing(region_request("offset", shape.region, std::move(request)));
        }
        if (op == "projection") {
            if (dim != 3) throw ValueError("Projection needs a 3D solid");
            const Named f = bind(geometry, {"cut"}, {"convexity"});
            convexity(f);
            static const Value no = Value::of_bool(false);
            if (flag(has(f, "cut") ? get(f, "cut") : &no)) {
                const json answer = question(shape.solid, "sections", json::array({0.0}));
                return drawing(region_of(straightened(answer.at("sections")[0].at("rings")), "positive"));
            }
            const json answer = question(shape.solid, "projection", true);
            const Region outline = region_of(rings_in(answer.at("projection").at("rings")), "positive");
            return drawing(region_of(outline.rings, "positive"));
        }
        throw ValueError("Unknown geometry operation " + quoted(op));
    }
};

}  // namespace

std::vector<double> region_bounds(const Region &region) {
    if (region.rings.empty()) return {0.0, 0.0, 0.0, 0.0};
    bool first = true;
    double low_x = 0, low_y = 0, high_x = 0, high_y = 0;
    // min() and max() of the coordinates in order: the first of equal values stands.
    for (const json &ring : region.rings)
        for (const json &p : ring) {
            const double x = p[0].get<double>(), y = p[1].get<double>();
            if (first) {
                low_x = high_x = x;
                low_y = high_y = y;
                first = false;
                continue;
            }
            if (x < low_x) low_x = x;
            if (y < low_y) low_y = y;
            if (x > high_x) high_x = x;
            if (y > high_y) high_y = y;
        }
    return {low_x, low_y, high_x, high_y};
}

Built build_script(const Json *source, const std::vector<std::pair<std::string, double>> &parameters, Budget &budget) {
    const Geometries nodes = evaluate(source, parameters);
    if (nodes.empty()) throw ValueError("The script did not produce geometry");
    Builder builder(budget);
    std::vector<Drawn> shapes;
    for (const GeometryPtr &node : nodes) shapes.push_back(builder.node(*node, 0));
    Drawn result = builder.check(builder.combine("union", std::move(shapes)));
    if (is_empty(result) || (result.dim == 3 && result.solid.info.at("volume").get<double>() <= 0))
        throw ValueError("The script produced an empty result");
    Built built;
    built.dimension = result.dim;
    built.solid = std::move(result.solid);
    built.region = std::move(result.region);
    return built;
}

}  // namespace scad
}  // namespace kerfkite
