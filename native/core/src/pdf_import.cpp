// Vector paths of one PDF page as a review draft, in millimetres: studio/pdf_import.py's import_pdf, without
// pypdf or Shapely, statement for statement.
//
// The reader is written from the PDF format (ISO 32000) for the files PDF producers write: a header, a
// cross-reference table or stream (with object streams and incremental updates) found from startxref, streams
// whose Length is right and that end with endstream, and the standard filters (FlateDecode with PNG and TIFF
// predictors, LZWDecode, ASCII85Decode, ASCIIHexDecode, RunLengthDecode). It reads what pypdf reads in strict
// mode from such files: the page tree with inherited attributes, the crop and media boxes, the user unit and
// rotation, and the content streams' operators and operands, numbers included, as pypdf parses them.
//
// The extraction is the reference's: the graphics state, transformation matrices, sampled cubics
// (sketch_curves.sample_cubic), clipping by convex straight paths and form boxes, the fill boundaries kept
// under the fill rule, and every message and warning. Shapely's predicates are answered exactly: orientation
// by GEOS's test (geos_orientation), points on and inside rings by exact tests, rings' simplicity by the
// simple-line port.
//
// Declined (NotPorted), for the Python to answer: files that deviate from that form (pypdf has repairs for
// many of them), inline images (whose end pypdf finds by its own rules), operands pypdf would refuse in its
// own way, clipping boundaries whose computed corners lie within rounding of a point tested against them,
// and any decision near a limit of pypdf's.
#include "kerfkite/pdf_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base64.hpp"
#include "imported_drawing.hpp"
#include "inflate.hpp"
#include "kerfkite/cam.hpp"
#include "kerfkite/decimal.hpp"
#include "kerfkite/line_check.hpp"
#include "kerfkite/py_math.hpp"
#include "project.hpp"
#include "python_text.hpp"

namespace kerfkite {

namespace {

using json = Json;
using namespace detail;
using namespace imported;

const size_t kMaxBytes = 4000000;
const size_t kStreamLimit = 7500000;  // below pypdf's 8,000,000-byte limits on decoded streams

// pypdf's own refusals, for which the reference gives one message.
struct Unreadable {};
[[noreturn]] void unreadable() { throw Unreadable(); }
[[noreturn]] void decline(const char *what) { throw NotPorted(what); }

// ---- objects -----------------------------------------------------------------------------------------------

struct Object;
using Array = std::vector<Object>;
using Dict = std::map<std::string, Object>;

enum class Kind { Null, Bool, Int, Real, String, Name, Array, Dict, Stream, Ref, Operator };

struct Object {
    Kind kind = Kind::Null;
    bool flag = false;
    long long integer = 0;
    double real = 0;
    std::string text;  // a string's bytes, a name with its slash, or an operator
    std::shared_ptr<Array> array;
    std::shared_ptr<Dict> dict;
    std::shared_ptr<std::string> data;  // a stream's raw bytes
    long long number = 0, generation = 0;  // a reference

    bool is_number() const { return kind == Kind::Int || kind == Kind::Real; }
    double value() const { return kind == Kind::Int ? static_cast<double>(integer) : real; }
    bool is_name(const char *name) const { return kind == Kind::Name && text == name; }
    bool has_dict() const { return kind == Kind::Dict || kind == Kind::Stream; }
};

// Python's truth of a pypdf object.
bool truth(const Object &o) {
    switch (o.kind) {
        case Kind::Int: return o.integer != 0;
        case Kind::Real: return o.real != 0;
        case Kind::String:
        case Kind::Name: return !o.text.empty();
        case Kind::Array: return !o.array->empty();
        case Kind::Dict:
        case Kind::Stream: return !o.dict->empty();
        default: decline("the truth of an unusual PDF object");
    }
}

bool white(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool delimiter(unsigned char c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }
bool regular(unsigned char c) { return !white(c) && !delimiter(c); }
bool numeric(unsigned char c) { return (c >= '0' && c <= '9') || c == '+' || c == ',' || c == '-' || c == '.'; }

// FloatObject(text): float(text), or 0.0 where Python's float() refuses it.
double float_object(const std::string &text) {
    size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    size_t digits = 0, dots = 0;
    for (size_t k = i; k < text.size(); ++k) {
        if (text[k] >= '0' && text[k] <= '9') ++digits;
        else if (text[k] == '.') ++dots;
        else return 0.0;
    }
    if (digits == 0 || dots > 1) return 0.0;
    double value = 0;
    if (!detail::float_from_text(text, value)) return 0.0;
    return value;
}

// NumberObject(text): int(text), or 0 where Python's int() refuses it.
long long number_object(const std::string &text) {
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    if (i >= text.size()) return 0;
    long long value = 0;
    for (size_t k = i; k < text.size(); ++k) {
        if (text[k] < '0' || text[k] > '9') return 0;
        if (value > 100000000000000LL) decline("a PDF integer beyond the exact range of doubles");
        value = value * 10 + (text[k] - '0');
    }
    return negative ? -value : value;
}

class Parser {
public:
    // In a content stream there is no document: references cannot be read (pypdf asserts), and words are
    // operators.
    Parser(const std::string &bytes, size_t at, bool content) : bytes_(bytes), at_(at), content_(content) {}

    size_t at() const { return at_; }
    void seek(size_t at) { at_ = at; }
    bool done() {
        skip();
        return at_ >= bytes_.size();
    }

    unsigned char peek() const { return at_ < bytes_.size() ? static_cast<unsigned char>(bytes_[at_]) : 0; }

    // Whitespace and comments.
    void skip() {
        while (at_ < bytes_.size()) {
            const unsigned char c = static_cast<unsigned char>(bytes_[at_]);
            if (white(c)) {
                ++at_;
            } else if (c == '%') {
                while (at_ < bytes_.size() && bytes_[at_] != '\r' && bytes_[at_] != '\n') ++at_;
            } else {
                break;
            }
        }
    }

    std::string word() {
        const size_t start = at_;
        while (at_ < bytes_.size() && regular(static_cast<unsigned char>(bytes_[at_]))) ++at_;
        return bytes_.substr(start, at_ - start);
    }

    bool keyword(const char *text) {
        skip();
        const size_t start = at_;
        if (word() == text) return true;
        at_ = start;
        return false;
    }

    // One object, or in a content stream an operator.
    Object object(int depth = 0) {
        if (depth > 64) decline("PDF objects nested more than 64 deep");
        skip();
        if (at_ >= bytes_.size()) unreadable();
        const unsigned char c = peek();
        Object o;
        if (c == '/') {
            o.kind = Kind::Name;
            o.text = name();
        } else if (c == '<' && at_ + 1 < bytes_.size() && bytes_[at_ + 1] == '<') {
            at_ += 2;
            o.kind = Kind::Dict;
            o.dict = std::make_shared<Dict>();
            for (;;) {
                skip();
                if (at_ + 1 < bytes_.size() && bytes_[at_] == '>' && bytes_[at_ + 1] == '>') {
                    at_ += 2;
                    break;
                }
                if (peek() != '/') decline("a PDF dictionary key that is not a name");
                const std::string key = name();
                Object value = object(depth + 1);
                if (value.kind == Kind::Operator) decline("a PDF dictionary value that is not an object");
                if (o.dict->count(key)) decline("a PDF dictionary that defines a key twice");
                (*o.dict)[key] = std::move(value);
            }
        } else if (c == '<') {
            o.kind = Kind::String;
            o.text = hex_string();
        } else if (c == '[') {
            ++at_;
            o.kind = Kind::Array;
            o.array = std::make_shared<Array>();
            for (;;) {
                skip();
                if (peek() == ']') {
                    ++at_;
                    break;
                }
                if (at_ >= bytes_.size()) unreadable();
                Object item = object(depth + 1);
                if (item.kind == Kind::Operator) decline("a word in a PDF array");
                o.array->push_back(std::move(item));
            }
        } else if (c == '(') {
            o.kind = Kind::String;
            o.text = literal_string();
        } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            if (!content_) {
                // n g R
                const size_t start = at_;
                const std::string first = word();
                skip();
                const size_t middle = at_;
                const std::string second = word();
                skip();
                const size_t last = at_;
                if (is_digits(first) && is_digits(second) && last < bytes_.size() && bytes_[last] == 'R' &&
                    (last + 1 >= bytes_.size() || !regular(static_cast<unsigned char>(bytes_[last + 1])) || bytes_[last + 1] == '\0')) {
                    at_ = last + 1;
                    o.kind = Kind::Ref;
                    o.number = number_object(first);
                    o.generation = number_object(second);
                    return o;
                }
                (void)middle;
                at_ = start;
            }
            const size_t start = at_;
            while (at_ < bytes_.size() && numeric(static_cast<unsigned char>(bytes_[at_]))) ++at_;
            const std::string text = bytes_.substr(start, at_ - start);
            if (text.size() >= 20) decline("a PDF number of 20 characters or more");
            if (content_ && at_ < bytes_.size() && regular(static_cast<unsigned char>(bytes_[at_])))
                decline("a PDF number run into a word");
            if (text.find('.') != std::string::npos) {
                o.kind = Kind::Real;
                o.real = float_object(text);
            } else {
                o.kind = Kind::Int;
                o.integer = number_object(text);
            }
            if (content_) {
                // The reference reads references in content too, where it cannot resolve them.
                const size_t after = at_;
                skip();
                const std::string second = word();
                skip();
                if (is_digits(text) && is_digits(second) && peek() == 'R') decline("a reference in a PDF content stream");
                at_ = after;
            }
        } else if (regular(c)) {
            const std::string text = word();
            if (content_) {
                if (!(std::isalpha(c) || c == '\'' || c == '"')) decline("an unusual word in a PDF content stream");
                if (text.size() > 128) decline("a PDF operator longer than pypdf reads");
                o.kind = Kind::Operator;
                o.text = text;
            } else if (text == "true" || text == "false") {
                o.kind = Kind::Bool;
                o.flag = text == "true";
            } else if (text == "null") {
                o.kind = Kind::Null;
            } else {
                o.kind = Kind::Operator;  // obj, endobj, stream, R, xref, trailer, ...
                o.text = text;
            }
        } else {
            decline("an unusual delimiter in a PDF");
        }
        return o;
    }

private:
    const std::string &bytes_;
    size_t at_;
    bool content_;

    static bool is_digits(const std::string &text) {
        if (text.empty()) return false;
        for (const char c : text)
            if (c < '0' || c > '9') return false;
        return true;
    }

    std::string name() {
        ++at_;  // '/'
        std::string out = "/";
        while (at_ < bytes_.size() && regular(static_cast<unsigned char>(bytes_[at_]))) {
            const char c = bytes_[at_];
            if (c == '#') decline("a PDF name with # escapes");
            out.push_back(c);
            ++at_;
        }
        for (const unsigned char c : out)
            if (c >= 0x80) decline("a PDF name that is not ASCII");
        return out;
    }

    std::string hex_string() {
        ++at_;
        std::string digits;
        while (at_ < bytes_.size() && bytes_[at_] != '>') {
            const char c = bytes_[at_++];
            if (white(static_cast<unsigned char>(c))) continue;
            if (!std::isxdigit(static_cast<unsigned char>(c))) decline("a PDF hex string with other characters");
            digits.push_back(c);
        }
        if (at_ >= bytes_.size()) unreadable();
        ++at_;
        if (digits.size() % 2) digits.push_back('0');
        std::string out;
        for (size_t i = 0; i < digits.size(); i += 2) out.push_back(static_cast<char>(std::stoi(digits.substr(i, 2), nullptr, 16)));
        return out;
    }

    std::string literal_string() {
        ++at_;
        int depth = 1;
        std::string out;
        while (at_ < bytes_.size()) {
            const char c = bytes_[at_++];
            if (c == '\\') {
                if (at_ >= bytes_.size()) unreadable();
                const char e = bytes_[at_++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case '(': case ')': case '\\': out.push_back(e); break;
                    case '\r':
                        if (at_ < bytes_.size() && bytes_[at_] == '\n') ++at_;
                        break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int value = e - '0';
                            for (int k = 0; k < 2 && at_ < bytes_.size() && bytes_[at_] >= '0' && bytes_[at_] <= '7'; ++k) value = value * 8 + (bytes_[at_++] - '0');
                            out.push_back(static_cast<char>(value & 0xff));
                        } else {
                            decline("a PDF string with an unusual escape");
                        }
                }
            } else if (c == '(') {
                ++depth;
                out.push_back(c);
            } else if (c == ')') {
                if (--depth == 0) return out;
                out.push_back(c);
            } else {
                out.push_back(c);
            }
        }
        unreadable();
    }
};

// ---- filters -------------------------------------------------------------------------------------------------

std::string flate(const std::string &raw) {
    std::vector<unsigned char> out;
    const detail::Inflated result = detail::zlib_decompress(reinterpret_cast<const unsigned char *>(raw.data()), raw.size(), kStreamLimit, out);
    if (result == detail::Inflated::TooLong) decline("a PDF stream near pypdf's limit on decoded bytes");
    if (result != detail::Inflated::Ok) decline("a PDF stream that pypdf would repair to decompress");
    return std::string(out.begin(), out.end());
}

std::string ascii_hex(const std::string &raw) {
    std::string digits, out;
    bool ended = false;
    for (const char c : raw) {
        if (c == '>') {
            ended = true;
            break;
        }
        if (white(static_cast<unsigned char>(c))) continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) decline("an ASCIIHexDecode stream with other characters");
        digits.push_back(c);
    }
    if (!ended) decline("an ASCIIHexDecode stream without its end marker");
    if (digits.size() % 2) digits.push_back('0');
    for (size_t i = 0; i < digits.size(); i += 2) out.push_back(static_cast<char>(std::stoi(digits.substr(i, 2), nullptr, 16)));
    return out;
}

std::string ascii85(const std::string &raw) {
    std::string out;
    uint32_t group = 0;
    int count = 0;
    size_t i = 0;
    if (raw.compare(0, 2, "<~") == 0) i = 2;
    bool ended = false;
    for (; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (white(c)) continue;
        if (c == '~') {
            ended = i + 1 < raw.size() && raw[i + 1] == '>';
            break;
        }
        if (c == 'z' && count == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') decline("an ASCII85Decode stream with other characters");
        group = group * 85 + (c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((group >> (8 * k)) & 0xff));
            group = 0;
            count = 0;
        }
    }
    if (!ended) decline("an ASCII85Decode stream without its end marker");
    if (count == 1) decline("an ASCII85Decode stream with a single character in its last group");
    if (count) {
        for (int k = count; k < 5; ++k) group = group * 85 + 84;
        for (int k = 0; k < count - 1; ++k) out.push_back(static_cast<char>((group >> (8 * (3 - k))) & 0xff));
    }
    return out;
}

std::string run_length(const std::string &raw) {
    std::string out;
    size_t i = 0;
    while (i < raw.size()) {
        const unsigned char length = static_cast<unsigned char>(raw[i++]);
        if (length == 128) return out;
        if (length < 128) {
            if (i + length + 1 > raw.size()) decline("a RunLengthDecode stream that ends early");
            out.append(raw, i, length + 1u);
            i += length + 1u;
        } else {
            if (i >= raw.size()) decline("a RunLengthDecode stream that ends early");
            out.append(257u - length, raw[i++]);
        }
        if (out.size() > kStreamLimit) decline("a PDF stream near pypdf's limit on decoded bytes");
    }
    decline("a RunLengthDecode stream without its end marker");
}

std::string lzw(const std::string &raw, bool early) {
    std::vector<std::string> table;
    const auto reset = [&] {
        table.clear();
        for (int i = 0; i < 256; ++i) table.push_back(std::string(1, static_cast<char>(i)));
        table.emplace_back();  // 256: clear
        table.emplace_back();  // 257: end
    };
    reset();
    std::string out, previous;
    int width = 9;
    uint32_t buffer = 0;
    int held = 0;
    size_t i = 0;
    bool have_previous = false;
    for (;;) {
        while (held < width) {
            if (i >= raw.size()) decline("an LZWDecode stream without its end code");
            buffer = (buffer << 8) | static_cast<unsigned char>(raw[i++]);
            held += 8;
        }
        const uint32_t code = (buffer >> (held - width)) & ((1u << width) - 1);
        held -= width;
        if (code == 256) {
            reset();
            width = 9;
            have_previous = false;
            continue;
        }
        if (code == 257) return out;
        std::string entry;
        if (code < table.size()) {
            entry = table[code];
        } else if (code == table.size() && have_previous) {
            entry = previous + previous[0];
        } else {
            decline("an LZWDecode stream with an unknown code");
        }
        out += entry;
        if (out.size() > kStreamLimit) decline("a PDF stream near pypdf's limit on decoded bytes");
        if (have_previous && table.size() < 4096) table.push_back(previous + entry[0]);
        previous = entry;
        have_previous = true;
        const size_t next = table.size() + (early ? 1 : 0);
        if (next >= 4096) width = 12;
        else if (next >= 2048) width = 12;
        else if (next >= 1024) width = 11;
        else if (next >= 512) width = 10;
    }
}

// The PNG and TIFF predictors of FlateDecode and LZWDecode parameters.
std::string predicted(std::string data, const Dict *parameters, const std::function<const Object *(const Object &)> &resolve) {
    if (!parameters) return data;
    const auto get = [&](const char *key, long long fallback) {
        const auto found = parameters->find(key);
        if (found == parameters->end()) return fallback;
        const Object *o = resolve(found->second);
        if (!o || o->kind != Kind::Int) decline("an unusual PDF stream decoding parameter");
        return o->integer;
    };
    const long long predictor = get("/Predictor", 1);
    if (predictor == 1) return data;
    const long long colors = get("/Colors", 1), bits = get("/BitsPerComponent", 8), columns = get("/Columns", 1);
    if (colors < 1 || colors > 32 || (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) || columns < 1 || columns > 1000000)
        decline("unusual PDF predictor parameters");
    const size_t pixel = static_cast<size_t>(std::max(1LL, (colors * bits + 7) / 8));
    const size_t row = static_cast<size_t>((colors * bits * columns + 7) / 8);
    if (predictor >= 10 && predictor <= 15) {
        if (data.size() % (row + 1)) decline("a PNG-predicted PDF stream that is not whole rows");
        std::string out, previous(row, '\0');
        for (size_t at = 0; at < data.size(); at += row + 1) {
            const unsigned type = static_cast<unsigned char>(data[at]);
            std::string line = data.substr(at + 1, row);
            for (size_t x = 0; x < row; ++x) {
                const unsigned a = x >= pixel ? static_cast<unsigned char>(line[x - pixel]) : 0, b = static_cast<unsigned char>(previous[x]);
                const unsigned c = x >= pixel ? static_cast<unsigned char>(previous[x - pixel]) : 0;
                unsigned value = static_cast<unsigned char>(line[x]);
                if (type == 1) value += a;
                else if (type == 2) value += b;
                else if (type == 3) value += (a + b) / 2;
                else if (type == 4) {
                    const int p = static_cast<int>(a + b) - static_cast<int>(c);
                    const int pa = std::abs(p - static_cast<int>(a)), pb = std::abs(p - static_cast<int>(b)), pc = std::abs(p - static_cast<int>(c));
                    value += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                } else if (type != 0) {
                    decline("a PNG predictor of an unknown type");
                }
                line[x] = static_cast<char>(value & 0xff);
            }
            out += line;
            previous = line;
        }
        return out;
    }
    if (predictor == 2 && bits == 8) {
        for (size_t at = 0; at + row <= data.size(); at += row)
            for (size_t x = pixel; x < row; ++x) data[at + x] = static_cast<char>(data[at + x] + data[at + x - pixel]);
        return data;
    }
    decline("an unusual PDF predictor");
}

// ---- the document --------------------------------------------------------------------------------------------

struct Entry {
    int type = 0;  // 1: at an offset; 2: in an object stream
    long long offset = 0, stream = 0, index = 0, generation = 0;
};

class Document {
public:
    explicit Document(std::string bytes) : bytes_(std::move(bytes)) { read(); }

    Dict trailer;

    const Object *resolve(const Object &o) {
        if (o.kind != Kind::Ref) return &o;
        const auto cached = cache_.find(o.number);
        if (cached != cache_.end()) return &cached->second;
        if (++resolutions_ > 200000) decline("a PDF with more objects than the core reads");
        if (loading_.count(o.number)) unreadable();
        loading_.insert(o.number);
        Object value = load(o.number, o.generation);
        loading_.erase(o.number);
        return &(cache_[o.number] = std::move(value));
    }

    const Object *get(const Dict &dict, const char *key) {
        const auto found = dict.find(key);
        return found == dict.end() ? nullptr : resolve(found->second);
    }

    // stream.get_data(): the decoded bytes.
    std::string data(const Object &stream) {
        if (stream.kind != Kind::Stream) unreadable();
        std::string data = *stream.data;
        const Object *filters = get(*stream.dict, "/Filter");
        const Object *parameters = get(*stream.dict, "/DecodeParms");
        std::vector<std::string> names;
        std::vector<const Object *> parms;
        if (!filters || filters->kind == Kind::Null) {
        } else if (filters->kind == Kind::Name) {
            names.push_back(filters->text);
            parms.push_back(parameters);
        } else if (filters->kind == Kind::Array) {
            for (size_t i = 0; i < filters->array->size(); ++i) {
                const Object *name = resolve((*filters->array)[i]);
                if (name->kind != Kind::Name) unreadable();
                names.push_back(name->text);
                const Object *p = nullptr;
                if (parameters && parameters->kind == Kind::Array && i < parameters->array->size()) p = resolve((*parameters->array)[i]);
                else if (parameters && parameters->kind == Kind::Dict && filters->array->size() == 1) p = parameters;
                parms.push_back(p);
            }
        } else {
            unreadable();
        }
        const std::function<const Object *(const Object &)> resolver = [this](const Object &o) { return resolve(o); };
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string &name = names[i];
            const Dict *p = parms[i] && parms[i]->kind == Kind::Dict ? parms[i]->dict.get() : nullptr;
            if (parms[i] && parms[i]->kind != Kind::Dict && parms[i]->kind != Kind::Null) decline("unusual PDF decoding parameters");
            if (name == "/FlateDecode" || name == "/Fl") {
                data = predicted(flate(data), p, resolver);
            } else if (name == "/LZWDecode" || name == "/LZW") {
                long long early = 1;
                if (p && p->count("/EarlyChange")) {
                    const Object *e = resolve(p->at("/EarlyChange"));
                    if (e->kind != Kind::Int) decline("an unusual LZW parameter");
                    early = e->integer;
                }
                data = predicted(lzw(data, early != 0), p, resolver);
            } else if (name == "/ASCIIHexDecode" || name == "/AHx") {
                data = ascii_hex(data);
            } else if (name == "/ASCII85Decode" || name == "/A85") {
                data = ascii85(data);
            } else if (name == "/RunLengthDecode" || name == "/RL") {
                data = run_length(data);
            } else {
                decline("a PDF stream filter the core does not read");
            }
            if (data.size() > kStreamLimit) decline("a PDF stream near pypdf's limit on decoded bytes");
        }
        return data;
    }

private:
    std::string bytes_;
    std::map<long long, Entry> xref_;
    std::map<long long, Object> cache_;
    std::set<long long> loading_;
    std::map<long long, std::vector<std::pair<long long, size_t>>> object_streams_;
    size_t resolutions_ = 0;

    // The object at an offset: "n g obj" with the expected numbers, then the object (and its stream).
    Object at(size_t offset, long long number, long long generation) {
        Parser parser(bytes_, offset, false);
        const Object n = parser.object(), g = parser.object(), word = parser.object();
        if (n.kind != Kind::Int || g.kind != Kind::Int || word.kind != Kind::Operator || word.text != "obj")
            decline("a PDF cross-reference entry that does not point at its object");
        if (n.integer != number || g.integer != generation) decline("a PDF object whose number differs from its entry");
        Object o = parser.object();
        if (o.kind == Kind::Operator) {
            if (o.text == "endobj") return Object();
            decline("an unusual PDF object");
        }
        if (o.kind == Kind::Dict) {
            const size_t mark = parser.at();
            if (parser.keyword("stream")) {
                stream_body(parser, o);
            } else {
                parser.seek(mark);
            }
        }
        return o;
    }

    void stream_body(Parser &parser, Object &o) {
        size_t start = parser.at();
        while (start < bytes_.size() && bytes_[start] == ' ') ++start;
        if (start >= bytes_.size() || (bytes_[start] != '\n' && bytes_[start] != '\r')) unreadable();
        if (bytes_[start] == '\r') {
            ++start;
            if (start < bytes_.size() && bytes_[start] == '\n') ++start;
        } else {
            ++start;
        }
        const auto length_entry = o.dict->find("/Length");
        if (length_entry == o.dict->end()) unreadable();
        Object length_value = length_entry->second;
        const Object *length = resolve(length_value);
        if (length->kind != Kind::Int || length->integer < 0) decline("a PDF stream whose length is not a number");
        const size_t end = start + static_cast<size_t>(length->integer);
        if (end > bytes_.size()) decline("a PDF stream longer than the file");
        Parser after(bytes_, end, false);
        after.skip();
        if (bytes_.compare(after.at(), 9, "endstream") != 0) decline("a PDF stream whose length pypdf would correct");
        o.kind = Kind::Stream;
        o.data = std::make_shared<std::string>(bytes_.substr(start, end - start));
    }

    Object load(long long number, long long generation) {
        const auto found = xref_.find(number);
        if (found == xref_.end() || found->second.type == 0) decline("a reference to a PDF object that is not in use");
        const Entry &entry = found->second;
        if (entry.type == 1) {
            if (entry.generation != generation) decline("a PDF reference whose generation differs from its entry");
            return at(static_cast<size_t>(entry.offset), number, generation);
        }
        if (entry.type == 2) {
            if (generation != 0) decline("an unusual reference into a PDF object stream");
            Object container_ref;
            container_ref.kind = Kind::Ref;
            container_ref.number = entry.stream;
            const Object *container = resolve(container_ref);
            if (container->kind != Kind::Stream) unreadable();
            const std::string data = this->data(*container);
            const Object *count = get(*container->dict, "/N"), *first = get(*container->dict, "/First");
            if (!count || !first || count->kind != Kind::Int || first->kind != Kind::Int) unreadable();
            Parser header(data, 0, false);
            std::vector<std::pair<long long, long long>> offsets;
            for (long long i = 0; i < count->integer; ++i) {
                const Object n = header.object(), offset = header.object();
                if (n.kind != Kind::Int || offset.kind != Kind::Int) unreadable();
                offsets.push_back({n.integer, offset.integer});
            }
            if (entry.index < 0 || entry.index >= static_cast<long long>(offsets.size()) || offsets[static_cast<size_t>(entry.index)].first != number)
                decline("a PDF object-stream entry that does not match its stream");
            Parser parser(data, static_cast<size_t>(first->integer + offsets[static_cast<size_t>(entry.index)].second), false);
            Object o = parser.object();
            if (o.kind == Kind::Operator) decline("an unusual object in a PDF object stream");
            return o;
        }
        return Object();
    }

    // The trailer chain from startxref, newest first.
    void read() {
        if (bytes_.compare(0, 5, "%PDF-") != 0) unreadable();
        // %%EOF at the start of the last line, after optional line ends, then startxref and its offset.
        size_t end = bytes_.size();
        while (end > 0 && white(static_cast<unsigned char>(bytes_[end - 1]))) --end;
        if (end < 5 || bytes_.compare(end - 5, 5, "%%EOF") != 0) {
            // In strict mode pypdf refuses a file with no %%EOF line, unless its last line looks like a cut one.
            std::string last = bytes_.substr(0, end);
            const size_t line = last.find_last_of("\r\n");
            if (line != std::string::npos) last = last.substr(line + 1);
            const bool truncated = !last.empty() && last.back() == '%';
            const bool cut = truncated || (last.size() >= 3 && (last.compare(last.size() - 3, 3, "%%E") == 0)) ||
                             (last.size() >= 4 && last.compare(last.size() - 4, 4, "%%EO") == 0);
            if (bytes_.find("%%EOF") == std::string::npos && !cut) unreadable();
            decline("a PDF whose end pypdf would search for");
        }
        const auto previous_line = [&](size_t &at) {
            size_t stop = at;
            while (stop > 0 && (bytes_[stop - 1] == '\r' || bytes_[stop - 1] == '\n')) --stop;
            size_t start = stop;
            while (start > 0 && bytes_[start - 1] != '\r' && bytes_[start - 1] != '\n') --start;
            at = start;
            return bytes_.substr(start, stop - start);
        };
        size_t at = end - 5;
        if (at > 0 && bytes_[at - 1] != '\r' && bytes_[at - 1] != '\n') decline("a PDF whose %%EOF is not on a line of its own");
        const std::string offset_line = previous_line(at), keyword_line = previous_line(at);
        const auto trimmed = [](std::string s) {
            while (!s.empty() && white(static_cast<unsigned char>(s.back()))) s.pop_back();
            size_t b = 0;
            while (b < s.size() && white(static_cast<unsigned char>(s[b]))) ++b;
            return s.substr(b);
        };
        const std::string offset_text = trimmed(offset_line);
        if (trimmed(keyword_line) != "startxref" || offset_text.empty() || offset_text.size() > 12 ||
            offset_text.find_first_not_of("0123456789") != std::string::npos)
            decline("a PDF whose startxref pypdf would search for");
        size_t offset = static_cast<size_t>(std::stoll(offset_text));
        std::set<size_t> seen;
        bool first = true;
        while (true) {
            if (offset >= bytes_.size() || !seen.insert(offset).second || seen.size() > 64) decline("an unusual chain of PDF cross-reference sections");
            Dict section = section_at(offset);
            if (first) trailer = section;
            first = false;
            const auto previous = section.find("/Prev");
            if (previous == section.end()) break;
            if (previous->second.kind != Kind::Int || previous->second.integer < 0) decline("an unusual PDF /Prev");
            offset = static_cast<size_t>(previous->second.integer);
        }
        if (!trailer.count("/Root")) unreadable();
    }

    void add(long long number, const Entry &entry) {
        if (!xref_.count(number)) xref_[number] = entry;  // the newest section wins
    }

    Dict section_at(size_t offset) {
        Parser parser(bytes_, offset, false);
        if (bytes_.compare(offset, 4, "xref") == 0) {
            parser.seek(offset + 4);
            for (;;) {
                parser.skip();
                if (bytes_.compare(parser.at(), 7, "trailer") == 0) break;
                const Object start = parser.object(), count = parser.object();
                if (start.kind != Kind::Int || count.kind != Kind::Int || start.integer < 0 || count.integer < 0 || count.integer > 1000000)
                    decline("an unusual PDF cross-reference table");
                // Each entry is 20 bytes: ten digits, a space, five digits, a space, n or f, and two bytes of
                // line end.
                size_t at = parser.at();
                while (at < bytes_.size() && (bytes_[at] == '\r' || bytes_[at] == '\n' || bytes_[at] == ' ')) ++at;
                for (long long i = 0; i < count.integer; ++i, at += 20) {
                    if (at + 20 > bytes_.size()) decline("a PDF cross-reference table that ends early");
                    const std::string line = bytes_.substr(at, 20);
                    if (line.find_first_not_of("0123456789", 0) != 10 || line[10] != ' ' || line.substr(11, 5).find_first_not_of("0123456789") != std::string::npos ||
                        line[16] != ' ' || (line[17] != 'n' && line[17] != 'f') ||
                        !((line[18] == '\r' && line[19] == '\n') || (line[18] == ' ' && (line[19] == '\n' || line[19] == '\r'))))
                        decline("a PDF cross-reference entry of an unusual form");
                    if (line[17] == 'n') {
                        Entry entry;
                        entry.type = 1;
                        entry.offset = std::stoll(line.substr(0, 10));
                        entry.generation = std::stoll(line.substr(11, 5));
                        add(start.integer + i, entry);
                    } else if (!xref_.count(start.integer + i)) {
                        xref_[start.integer + i] = Entry();
                    }
                }
                parser.seek(at);
            }
            parser.seek(parser.at() + 7);
            const Object trailer_dict = parser.object();
            if (trailer_dict.kind != Kind::Dict) unreadable();
            Dict section = *trailer_dict.dict;
            if (section.count("/XRefStm")) decline("a hybrid PDF cross-reference");
            return section;
        }
        // A cross-reference stream.
        const Object n = parser.object(), g = parser.object(), word = parser.object();
        if (n.kind != Kind::Int || g.kind != Kind::Int || word.kind != Kind::Operator || word.text != "obj") decline("a PDF whose startxref pypdf would correct");
        Object o = parser.object();
        if (o.kind != Kind::Dict || !parser.keyword("stream")) unreadable();
        stream_body(parser, o);
        const Dict &d = *o.dict;
        const auto type = d.find("/Type");
        if (type == d.end() || !type->second.is_name("/XRef")) unreadable();
        const auto widths = d.find("/W");
        const auto size = d.find("/Size");
        if (widths == d.end() || widths->second.kind != Kind::Array || widths->second.array->size() != 3 || size == d.end() || size->second.kind != Kind::Int)
            unreadable();
        int w[3];
        for (int i = 0; i < 3; ++i) {
            const Object &v = (*widths->second.array)[static_cast<size_t>(i)];
            if (v.kind != Kind::Int || v.integer < 0 || v.integer > 8) decline("unusual PDF cross-reference widths");
            w[i] = static_cast<int>(v.integer);
        }
        std::vector<long long> index;
        const auto given = d.find("/Index");
        if (given == d.end()) {
            index = {0, size->second.integer};
        } else {
            if (given->second.kind != Kind::Array || given->second.array->size() % 2) unreadable();
            for (const Object &v : *given->second.array) {
                if (v.kind != Kind::Int) unreadable();
                index.push_back(v.integer);
            }
        }
        // The stream's own filters are read before the table it holds exists: no references in them.
        if (d.count("/Filter") && d.at("/Filter").kind == Kind::Ref) decline("an indirect filter on a PDF cross-reference stream");
        const std::string table = data(o);
        const size_t row = static_cast<size_t>(w[0] + w[1] + w[2]);
        size_t at = 0;
        const auto field = [&](int width, long long fallback) {
            if (width == 0) return fallback;
            long long value = 0;
            for (int i = 0; i < width; ++i) value = (value << 8) | static_cast<unsigned char>(table[at++]);
            return value;
        };
        for (size_t k = 0; k < index.size(); k += 2) {
            for (long long i = 0; i < index[k + 1]; ++i) {
                if (at + row > table.size()) decline("a PDF cross-reference stream that ends early");
                const long long kind = field(w[0], 1), second = field(w[1], 0), third = field(w[2], 0);
                Entry entry;
                if (kind == 1) {
                    entry.type = 1;
                    entry.offset = second;
                    entry.generation = third;
                } else if (kind == 2) {
                    entry.type = 2;
                    entry.stream = second;
                    entry.index = third;
                } else if (kind != 0) {
                    decline("a PDF cross-reference entry of an unknown type");
                }
                if (kind == 0) {
                    if (!xref_.count(index[k] + i)) xref_[index[k] + i] = Entry();
                } else {
                    add(index[k] + i, entry);
                }
            }
        }
        return d;
    }
};

// ---- geometry --------------------------------------------------------------------------------------------------

using Point = std::array<double, 2>;
using Matrix = std::array<double, 6>;

const Matrix kIdentity = {1, 0, 0, 1, 0, 0};

[[noreturn]] void range_error() { throw ValueError("PDF coordinates exceed the supported numeric range"); }

void check(const double *values, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i]) || std::fabs(values[i]) > 1e9) range_error();
}

// numbers(args, count) for operands.
std::vector<double> numbers(const Array &args, size_t count) {
    if (args.size() != count) throw ValueError("PDF path command has the wrong number of coordinates");
    std::vector<double> values;
    for (const Object &o : args) {
        if (!o.is_number()) decline("a PDF operand float() would refuse in its own words");
        values.push_back(o.value());
    }
    check(values.data(), values.size());
    return values;
}

Matrix compose(const Matrix &outer, const Matrix &inner) {
    const double a = outer[0], b = outer[1], c = outer[2], d = outer[3], e = outer[4], f = outer[5];
    const double g = inner[0], h = inner[1], i = inner[2], j = inner[3], k = inner[4], l = inner[5];
    const Matrix out = {a * g + c * h, b * g + d * h, a * i + c * j, b * i + d * j, a * k + c * l + e, b * k + d * l + f};
    check(out.data(), 6);
    return out;
}

Point apply(const Matrix &m, double x, double y) {
    const Point p = {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
    check(p.data(), 2);
    return p;
}

int orient(const Point &a, const Point &b, const Point &c) { return geos_orientation(a, b, c); }

bool between(double a, double b, double v) { return std::min(a, b) <= v && v <= std::max(a, b); }

bool on_segment(const Point &a, const Point &b, const Point &p) {
    return orient(a, b, p) == 0 && between(a[0], b[0], p[0]) && between(a[1], b[1], p[1]);
}

enum class Location { Outside, Boundary, Inside };

// Where a point lies against a closed ring (its last point the first), by exact tests.
Location locate(const std::vector<Point> &ring, const Point &p) {
    int crossings = 0;
    for (size_t i = 0; i + 1 < ring.size(); ++i) {
        const Point &a = ring[i], &b = ring[i + 1];
        if (on_segment(a, b, p)) return Location::Boundary;
        // A crossing of the ray to +x: the segment straddles p's y (half-open), and p is left of it.
        if ((a[1] > p[1]) != (b[1] > p[1])) {
            const int side = orient(a, b, p);
            if ((b[1] > a[1] && side > 0) || (b[1] < a[1] && side < 0)) ++crossings;
        }
    }
    return crossings % 2 ? Location::Inside : Location::Outside;
}

bool segments_meet(const Point &p1, const Point &p2, const Point &q1, const Point &q2) {
    if (std::max(p1[0], p2[0]) < std::min(q1[0], q2[0]) || std::max(q1[0], q2[0]) < std::min(p1[0], p2[0]) ||
        std::max(p1[1], p2[1]) < std::min(q1[1], q2[1]) || std::max(q1[1], q2[1]) < std::min(p1[1], p2[1]))
        return false;
    const int a = orient(p1, p2, q1), b = orient(p1, p2, q2), c = orient(q1, q2, p1), d = orient(q1, q2, p2);
    if (a * b < 0 && c * d < 0) return true;
    return (a == 0 && on_segment(p1, p2, q1)) || (b == 0 && on_segment(p1, p2, q2)) || (c == 0 && on_segment(q1, q2, p1)) ||
           (d == 0 && on_segment(q1, q2, p2));
}

std::vector<Point> closed(std::vector<Point> ring) {
    if (!ring.empty() && ring.front() != ring.back()) ring.push_back(ring.front());
    return ring;
}

double twice_area(const std::vector<Point> &ring) {
    double sum = 0;
    for (size_t i = 0; i + 1 < ring.size(); ++i) sum += ring[i][0] * ring[i + 1][1] - ring[i + 1][0] * ring[i][1];
    return sum;
}

// Shapely's Polygon(ring).is_valid for one closed ring.
bool valid_ring(const std::vector<Point> &ring) {
    std::set<Point> distinct(ring.begin(), ring.end());
    if (distinct.size() < 3) return false;
    const LineCheck check = check_line(ring);
    if (check.declined) throw NotPorted(check.reason);
    return check.simple;
}

// The ring's orientation, from its lowest leftmost corner, which a simple ring turns around convexly.
bool counter_clockwise(const std::vector<Point> &ring) {
    std::vector<Point> points(ring.begin(), ring.end() - 1);
    size_t low = 0;
    for (size_t i = 1; i < points.size(); ++i)
        if (points[i] < points[low]) low = i;
    size_t before = low, after = low;
    do before = (before + points.size() - 1) % points.size();
    while (points[before] == points[low]);
    do after = (after + 1) % points.size();
    while (points[after] == points[low]);
    return orient(points[before], points[low], points[after]) > 0;
}

// A convex clipping region: its corners in order, and whether any corner was computed by an intersection.
struct Clip {
    std::vector<Point> corners;  // counter-clockwise, not closed; empty for nothing
    bool computed = false;
};

std::vector<Point> counter_clockwise_corners(std::vector<Point> corners) {
    if (twice_area(closed(corners)) < 0) std::reverse(corners.begin(), corners.end());
    return corners;
}

// Clip.covers(points): every point inside the region or on its boundary.
bool covers(const Clip &clip, const std::vector<Point> &points) {
    if (clip.corners.size() < 3) return false;
    const std::vector<Point> ring = closed(clip.corners);
    for (const Point &p : points) {
        if (clip.computed) {
            // A computed corner is rounded, in GEOS as here: a point near the boundary may be judged either way.
            for (size_t i = 0; i + 1 < ring.size(); ++i) {
                const double dx = ring[i + 1][0] - ring[i][0], dy = ring[i + 1][1] - ring[i][1];
                const double size = std::hypot(dx, dy);
                const double scale = std::max({1.0, std::fabs(p[0]), std::fabs(p[1]), std::fabs(ring[i][0]), std::fabs(ring[i][1])});
                const double offset = std::fabs((p[0] - ring[i][0]) * dy - (p[1] - ring[i][1]) * dx);
                if (offset <= 1e-9 * scale * std::max(size, 1e-300)) decline("a point within rounding of a computed PDF clipping boundary");
            }
        }
        if (locate(ring, p) == Location::Outside) return false;
    }
    return true;
}

// The intersection of two convex regions: the corners of each inside or on the other, and the points where
// their edges cross, which are the only corners computed; ordered as their convex hull.
Clip intersect(const Clip &a, const std::vector<Point> &b_corners) {
    Clip out;
    if (a.corners.size() < 3 || b_corners.size() < 3) return out;
    out.computed = a.computed;
    const std::vector<Point> &p = a.corners;
    const std::vector<Point> q = counter_clockwise_corners(b_corners);
    const std::vector<Point> ring_p = closed(p), ring_q = closed(q);
    std::vector<Point> points;
    for (const Point &v : p)
        if (locate(ring_q, v) != Location::Outside) points.push_back(v);
    for (const Point &v : q)
        if (locate(ring_p, v) != Location::Outside) points.push_back(v);
    for (size_t i = 0; i < p.size(); ++i) {
        const Point &p1 = p[i], &p2 = p[(i + 1) % p.size()];
        for (size_t j = 0; j < q.size(); ++j) {
            const Point &q1 = q[j], &q2 = q[(j + 1) % q.size()];
            const int s1 = orient(p1, p2, q1), s2 = orient(p1, p2, q2), s3 = orient(q1, q2, p1), s4 = orient(q1, q2, p2);
            if (s1 * s2 < 0 && s3 * s4 < 0) {
                const double d1 = (q2[0] - q1[0]) * (p1[1] - q1[1]) - (q2[1] - q1[1]) * (p1[0] - q1[0]);
                const double d2 = (q2[0] - q1[0]) * (p2[1] - q1[1]) - (q2[1] - q1[1]) * (p2[0] - q1[0]);
                const double u = d1 / (d1 - d2);
                points.push_back({p1[0] + (p2[0] - p1[0]) * u, p1[1] + (p2[1] - p1[1]) * u});
                out.computed = true;
            }
        }
    }
    // The convex hull of the points (Andrew's monotone chain), with exact turns.
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) {
        if (!points.empty() && out.computed) decline("a PDF clipping region that narrows to a line or a point");
        return Clip();
    }
    std::vector<Point> hull(2 * points.size());
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && orient(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && orient(hull[k - 2], hull[k - 1], points[i - 1]) <= 0) --k;
        hull[k++] = points[i - 1];
    }
    hull.resize(k - 1);
    if (hull.size() < 3) {
        if (out.computed) decline("a PDF clipping region that narrows to a line or a point");
        return Clip();
    }
    out.corners = hull;
    return out;
}

Clip rectangle(const Matrix &m, const std::vector<double> &box) {
    Clip clip;
    std::vector<Point> corners = {apply(m, box[0], box[1]), apply(m, box[2], box[1]), apply(m, box[2], box[3]), apply(m, box[0], box[3])};
    const double area = twice_area(closed(corners));
    if (area == 0) decline("a PDF box with no area");
    clip.corners = counter_clockwise_corners(corners);
    return clip;
}

// pdf_import.fill_boundaries: the visible boundaries of disjoint or nested simple rings, or false for overlap.
bool fill_boundaries(const std::vector<std::vector<Point>> &contours, bool even_odd, std::vector<std::vector<Point>> &out) {
    std::vector<std::vector<Point>> rings;
    for (const auto &p : contours) {
        if (std::set<Point>(p.begin(), p.end()).size() >= 3) rings.push_back(closed(p));
    }
    for (const auto &ring : rings)
        if (!valid_ring(ring)) return false;
    struct Box {
        double x0, y0, x1, y1;
    };
    std::vector<Box> boxes;
    for (const auto &ring : rings) {
        Box b{ring[0][0], ring[0][1], ring[0][0], ring[0][1]};
        for (const Point &p : ring) b = {std::min(b.x0, p[0]), std::min(b.y0, p[1]), std::max(b.x1, p[0]), std::max(b.y1, p[1])};
        boxes.push_back(b);
    }
    const auto overlap = [&](size_t i, size_t j) {
        return boxes[i].x0 <= boxes[j].x1 && boxes[j].x0 <= boxes[i].x1 && boxes[i].y0 <= boxes[j].y1 && boxes[j].y0 <= boxes[i].y1;
    };
    size_t comparisons = 0;
    for (size_t i = 0; i < rings.size(); ++i)
        for (size_t j = 0; j < rings.size(); ++j) comparisons += overlap(i, j);
    if (comparisons > 60000) decline("a PDF fill whose overlap test reaches the reference's limit");
    size_t work = 0;
    std::vector<bool> ccw;
    for (const auto &ring : rings) ccw.push_back(counter_clockwise(ring));
    for (size_t i = 0; i < rings.size(); ++i) {
        int winding = 0;
        for (size_t j = 0; j < rings.size(); ++j) {
            if (i == j || !overlap(i, j)) continue;
            work += rings[i].size() * rings[j].size();
            if (work > 50000000) decline("a PDF fill whose overlap test takes more work than the core does");
            for (size_t a = 0; a + 1 < rings[i].size(); ++a)
                for (size_t b = 0; b + 1 < rings[j].size(); ++b)
                    if (segments_meet(rings[i][a], rings[i][a + 1], rings[j][b], rings[j][b + 1])) return false;
            // The boundaries are apart, so one corner tells whether the other ring holds this one.
            if (locate(rings[j], rings[i][0]) == Location::Inside) winding += ccw[j] ? 1 : -1;
        }
        if (even_odd || ((winding == 0) != (winding + (ccw[i] ? 1 : -1) == 0))) out.push_back(rings[i]);
    }
    return true;
}

// sketch_curves.sample_cubic.
struct Sampler {
    double tolerance = 0;
    std::vector<Point> result;
    static Point mid(const Point &a, const Point &b) { return {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}; }
    void subdivide(const Point &a, const Point &b, const Point &c, const Point &d, int depth) {
        const double dx = d[0] - a[0], dy = d[1] - a[1];
        const double chord = pymath::hypot(dx, dy);
        if (chord != 0) {
            const Point *inner[2] = {&b, &c};
            double projections[2], deviations[2];
            for (int k = 0; k < 2; ++k) {
                const Point &p = *inner[k];
                projections[k] = ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / chord;
                deviations[k] = std::fabs((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / chord;
            }
            if (py_max(deviations[0], deviations[1]) <= tolerance && -1e-12 <= projections[0] && projections[0] <= projections[1] + 1e-12 &&
                projections[1] <= chord + 1e-12) {
                result.push_back(d);
                return;
            }
        }
        if (depth >= 20 || result.size() >= 4096) throw ValueError("Curve reverses direction at a cusp or exceeds the sketch sampling budget");
        const Point ab = mid(a, b), bc = mid(b, c), cd = mid(c, d), abc = mid(ab, bc), bcd = mid(bc, cd), m = mid(abc, bcd);
        subdivide(a, ab, abc, m, depth + 1);
        subdivide(m, bcd, cd, d, depth + 1);
    }
};

std::vector<Point> sample_cubic(const Point &start, const Point &c1, const Point &c2, const Point &end) {
    const auto dist = [](const Point &a, const Point &b) { return pymath::dist(a[0], a[1], b[0], b[1]); };
    Sampler sampler;
    sampler.tolerance = py_min(0.01, py_max(py_max(dist(start, c1), dist(start, c2)), dist(start, end)) * 0.00025);
    sampler.subdivide(start, c1, c2, end, 0);
    return sampler.result;
}

// ---- extraction ------------------------------------------------------------------------------------------------

struct State {
    Matrix matrix = kIdentity;
    std::shared_ptr<Clip> clip;  // none: unknown
    bool dashed = false;
    double stroke_alpha = 1.0, fill_alpha = 1.0;
    bool soft_mask = false;
    long long text_mode = 0;
};

struct Operation {
    Array operands;
    std::string op;
};

std::vector<Operation> operations(const std::string &content) {
    std::vector<Operation> out;
    Parser parser(content, 0, true);
    Array operands;
    size_t tokens = 0;
    while (!parser.done()) {
        if (++tokens > 190000) decline("a PDF content stream near pypdf's parsing limit");
        Object o = parser.object();
        if (o.kind == Kind::Operator) {
            if (o.text == "BI") decline("an inline image, whose end pypdf finds by its own rules");
            out.push_back({std::move(operands), o.text});
            operands.clear();
        } else {
            operands.push_back(std::move(o));
        }
    }
    return out;
}

// An array of direct numbers, as the reference reads it with float().
Array direct(const Object *array) {
    if (!array || array->kind != Kind::Array) unreadable();
    for (const Object &v : *array->array)
        if (v.kind == Kind::Ref) decline("a PDF array with a reference where the reference reads a number");
    return *array->array;
}

// dict.get(key) as pypdf answers it: the entry itself, not the object it refers to.
const Object *raw_entry(const Dict &dict, const char *key) {
    const auto found = dict.find(key);
    return found == dict.end() ? nullptr : &found->second;
}

class Extraction {
public:
    Extraction(Document &doc, long long page) : doc_(doc), layer_("PDF page " + std::to_string(page)) {}

    Drawing drawing;
    std::vector<std::pair<std::string, long long>> skips;  // in order of first occurrence
    size_t ops = 0, constructed = 0, forms = 0, contours = 0;

    void skip(const std::string &reason, long long amount = 1) {
        for (auto &item : skips)
            if (item.first == reason) {
                item.second += amount;
                return;
            }
        skips.push_back({reason, amount});
    }

    // pdf_import.content_stream: the content, checked, as pypdf's ContentStream joins it.
    std::string content(const Object *raw) {
        if (!raw) return std::string();
        std::vector<const Object *> streams;
        if (raw->kind == Kind::Array) {
            if (raw->array->size() > 256) throw ValueError("Simplify the PDF page to at most 256 content streams");
            for (const Object &item : *raw->array) streams.push_back(doc_.resolve(item));
        } else {
            streams.push_back(raw);
        }
        for (const Object *block : streams) {
            if (block->kind != Kind::Stream || block->dict->count("/F")) throw ValueError("PDF uses an unsupported external content stream");
            const Object *filters = doc_.get(*block->dict, "/Filter");
            std::vector<std::string> names;
            if (filters && filters->kind == Kind::Array) {
                for (const Object &f : *filters->array) names.push_back(f.kind == Kind::Name ? f.text : std::string("?"));
            } else if (filters && filters->kind == Kind::Name) {
                names.push_back(filters->text);
            } else if (filters) {
                names.push_back("?");
            }
            static const std::set<std::string> allowed = {"/FlateDecode", "/Fl", "/ASCII85Decode", "/A85", "/ASCIIHexDecode", "/AHx",
                                                          "/LZWDecode", "/LZW", "/RunLengthDecode", "/RL"};
            for (const std::string &name : names)
                if (!allowed.count(name)) throw ValueError("Re-export this page using standard vector content compression");
        }
        if (raw->kind != Kind::Array) return doc_.data(*raw);
        std::string data;
        for (const Object *block : streams) {
            data += doc_.data(*block);
            if (data.size() > kStreamLimit) decline("a PDF content near pypdf's limit on decoded bytes");
            if (data.empty() || data.back() != '\n') data.push_back('\n');
        }
        return data;
    }

    void process(const std::string &stream, bool present, const Dict *resources, State state, int depth = 0) {
        if (!present) return;
        if (depth > 12) throw ValueError("PDF forms nest more than 12 levels");
        std::vector<std::vector<Point>> paths;
        std::vector<Point> hull;
        std::vector<State> stack;
        std::vector<bool> marks;
        int current = -1;  // the index of the current path, or none
        bool pending_clip = false, skip_text = false;
        const auto add = [&](const std::vector<Point> &points) {
            if (current < 0) throw ValueError("PDF line or curve is missing its starting point");
            constructed += points.size();
            if (constructed > kMaxPoints) throw ValueError("Simplify the PDF to 100000 constructed vertices");
            auto &path = paths[static_cast<size_t>(current)];
            path.insert(path.end(), points.begin(), points.end());
        };
        const auto close = [&] {
            if (current >= 0 && !paths[static_cast<size_t>(current)].empty() && paths[static_cast<size_t>(current)].back() != paths[static_cast<size_t>(current)].front())
                add({paths[static_cast<size_t>(current)].front()});
        };
        const auto paint = [&](bool fill, bool stroke, bool even_odd) {
            if (!marks.empty() && marks.back()) {
                skip("optional-content artwork omitted");
                return;
            }
            if (stroke && state.dashed) {
                skip("dashed strokes omitted; expand the dashes into separate paths");
                stroke = false;
            }
            fill = fill && state.fill_alpha > 0;
            stroke = stroke && state.stroke_alpha > 0;
            if (state.soft_mask || !(fill || stroke)) {
                skip("masked or non-visible paint omitted");
                return;
            }
            if (!state.clip || !covers(*state.clip, hull)) {
                skip("paint omitted because its full path could not be verified inside the clipping boundary");
                return;
            }
            std::vector<std::vector<Point>> kept;
            for (const auto &path : paths) {
                if (path.size() < 2 || std::all_of(path.begin(), path.end(), [&](const Point &p) { return p == path[0]; })) continue;
                std::vector<Point> contour = path;
                if (fill && contour.back() != contour.front()) contour.push_back(contour.front());
                kept.push_back(std::move(contour));
            }
            if (fill && !stroke) {
                std::vector<std::vector<Point>> boundaries;
                if (!fill_boundaries(kept, even_odd, boundaries)) {
                    skip("overlapping or self-intersecting fill omitted; unite the filled regions in the source");
                    return;
                }
                kept = std::move(boundaries);
            }
            if (kept.empty()) return;
            contours += kept.size();
            if (contours > kMaxPaths) throw ValueError("Simplify the PDF to 3000 vector paths");
            // A compound fill travels as one object, so that nested boundaries stay together.
            for (size_t k = 0; k < kept.size(); ++k) {
                drawing.path(layer_ + " artwork", layer_, kept[k]);
                if (k > 0) {
                    Object_ last = std::move(drawing.objects.back());
                    drawing.objects.pop_back();
                    auto &group = drawing.objects.back().paths;
                    group.insert(group.end(), last.paths.begin(), last.paths.end());
                }
            }
        };
        const auto name_of = [&](const Array &args) -> const Object & {
            if (args.empty()) unreadable();
            return args[0];
        };
        const auto lookup = [&](const char *category, const Object &name) -> const Object * {
            if (!resources) return nullptr;
            const Object *table = doc_.get(*resources, category);
            if (!table) return nullptr;
            if (!table->has_dict()) unreadable();
            if (name.kind != Kind::Name) decline("a PDF resource named by something other than a name");
            const auto found = table->dict->find(name.text);
            if (found == table->dict->end()) return nullptr;
            return doc_.resolve(found->second);
        };
        for (Operation &operation : operations(stream)) {
            if (++ops > 100000) throw ValueError("Simplify the PDF to 100000 graphics operations");
            const std::string &op = operation.op;
            const Array &args = operation.operands;
            if (op == "q") {
                if (stack.size() >= 64) throw ValueError("PDF graphics-state nesting exceeds 64");
                stack.push_back(state);
            } else if (op == "Q") {
                if (stack.empty()) throw ValueError("Unmatched PDF graphics-state restore");
                state = stack.back();
                stack.pop_back();
            } else if (op == "cm") {
                const std::vector<double> v = numbers(args, 6);
                state.matrix = compose(state.matrix, {v[0], v[1], v[2], v[3], v[4], v[5]});
            } else if (op == "m") {
                paths.emplace_back();
                current = static_cast<int>(paths.size()) - 1;
                const std::vector<double> v = numbers(args, 2);
                const Point p = apply(state.matrix, v[0], v[1]);
                add({p});
                hull.push_back(p);
            } else if (op == "l") {
                const std::vector<double> v = numbers(args, 2);
                const Point p = apply(state.matrix, v[0], v[1]);
                add({p});
                hull.push_back(p);
            } else if (op == "c" || op == "v" || op == "y") {
                if (current < 0 || paths[static_cast<size_t>(current)].empty()) throw ValueError("PDF curve is missing its starting point");
                const std::vector<double> v = numbers(args, op == "c" ? 6 : 4);
                std::vector<Point> poles;
                for (size_t i = 0; i < v.size(); i += 2) poles.push_back(apply(state.matrix, v[i], v[i + 1]));
                const Point last = paths[static_cast<size_t>(current)].back();
                if (op == "v") poles.insert(poles.begin(), last);
                if (op == "y") poles.push_back(poles.back());
                if (std::all_of(poles.begin(), poles.end(), [&](const Point &p) { return p == last; })) continue;
                hull.insert(hull.end(), poles.begin(), poles.end());
                add(sample_cubic(last, poles[0], poles[1], poles[2]));
            } else if (op == "h") {
                close();
            } else if (op == "re") {
                const std::vector<double> v = numbers(args, 4);
                const double x = v[0], y = v[1], w = v[2], h = v[3];
                paths.emplace_back();
                current = static_cast<int>(paths.size()) - 1;
                const std::vector<Point> corners = {apply(state.matrix, x, y), apply(state.matrix, x + w, y), apply(state.matrix, x + w, y + h),
                                                    apply(state.matrix, x, y + h)};
                std::vector<Point> ring = corners;
                ring.push_back(corners[0]);
                add(ring);
                hull.insert(hull.end(), corners.begin(), corners.end());
            } else if (op == "W" || op == "W*") {
                pending_clip = true;
            } else if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" || op == "B" || op == "B*" || op == "b" || op == "b*" || op == "n") {
                if (op == "s" || op == "b" || op == "b*") close();
                if (op != "n" && !hull.empty()) paint(op != "S" && op != "s", op != "f" && op != "F" && op != "f*", op.back() == '*');
                if (pending_clip && state.clip) {
                    // Convex straight-sided clipping keeps a definitive inside test.
                    bool kept = false;
                    if (paths.size() == 1 && paths[0].size() >= 3 && paths[0].size() <= 5 &&
                        std::set<Point>(paths[0].begin(), paths[0].end()) == std::set<Point>(hull.begin(), hull.end())) {
                        const std::vector<Point> ring = closed(paths[0]);
                        std::vector<Point> corners(ring.begin(), ring.end() - 1);
                        if (valid_ring(ring) && twice_area(ring) != 0 && convex(ring)) {
                            state.clip = std::make_shared<Clip>(intersect(*state.clip, corners));
                            kept = true;
                        }
                    }
                    if (!kept) state.clip.reset();
                }
                current = -1;
                paths.clear();
                hull.clear();
                pending_clip = false;
            } else if (op == "d") {
                state.dashed = truth(name_of(args));
            } else if (op == "gs") {
                const Object *ext = lookup("/ExtGState", name_of(args));
                if (!ext) throw ValueError("PDF refers to a missing graphics state");
                if (!ext->has_dict()) unreadable();
                const Dict &d = *ext->dict;
                if (const Object *ca = doc_.get(d, "/CA")) state.stroke_alpha = numbers({*ca}, 1)[0];
                if (const Object *ca = doc_.get(d, "/ca")) state.fill_alpha = numbers({*ca}, 1)[0];
                if (const Object *mask = doc_.get(d, "/SMask")) state.soft_mask = !mask->is_name("/None");
                if (const Object *dash = doc_.get(d, "/D")) {
                    if (dash->kind != Kind::Array || dash->array->empty()) unreadable();
                    if ((*dash->array)[0].kind == Kind::Ref) decline("a PDF dash given by a reference");
                    state.dashed = truth((*dash->array)[0]);
                }
            } else if (op == "BMC" || op == "BDC") {
                marks.push_back((!marks.empty() && marks.back()) || name_of(args).is_name("/OC"));
            } else if (op == "EMC") {
                if (marks.empty()) throw ValueError("Unmatched PDF marked-content end");
                marks.pop_back();
            } else if (op == "Tr") {
                const double mode = numbers(args, 1)[0];
                state.text_mode = static_cast<long long>(mode);
            } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
                skip("text objects omitted; convert text to paths in the source");
                skip_text = skip_text || state.text_mode >= 4;
            } else if (op == "ET" && skip_text) {
                state.clip.reset();
                skip_text = false;
            } else if (op == "sh") {
                skip("image or shading omitted; use a raster import for this artwork");
            } else if (op == "Do") {
                const Object *form = lookup("/XObject", name_of(args));
                if (!form) throw ValueError("PDF refers to a missing image or form");
                if (!form->has_dict()) unreadable();
                const Object *subtype = raw_entry(*form->dict, "/Subtype");
                if (subtype && subtype->kind == Kind::Ref) decline("a PDF form whose subtype is a reference");
                if (!subtype || !subtype->is_name("/Form")) {
                    skip("image omitted; use a raster import for this artwork");
                    continue;
                }
                if ((!marks.empty() && marks.back()) || form->dict->count("/OC") || form->dict->count("/Group") || state.soft_mask || !state.clip) {
                    skip("optional, masked, clipped or transparency-group form omitted");
                    continue;
                }
                if (++forms > 500) throw ValueError("PDF has more than 500 form invocations");
                const Object *given = raw_entry(*form->dict, "/Matrix");
                Array matrix_values;
                if (given) {
                    if (given->kind == Kind::Ref) decline("a PDF form whose matrix is a reference");
                    matrix_values = direct(given);
                } else {
                    for (const double v : kIdentity) {
                        Object o;
                        o.kind = Kind::Real;
                        o.real = v;
                        matrix_values.push_back(o);
                    }
                }
                const std::vector<double> m = numbers(matrix_values, 6);
                const Matrix matrix = compose(state.matrix, {m[0], m[1], m[2], m[3], m[4], m[5]});
                const Array box_values = direct(doc_.get(*form->dict, "/BBox"));
                State form_state = state;
                form_state.matrix = matrix;
                form_state.clip = std::make_shared<Clip>(intersect(*state.clip, rectangle(matrix, numbers(box_values, 4)).corners));
                const std::string inner = content(form);
                const Object *form_resources = doc_.get(*form->dict, "/Resources");
                if (form_resources && !form_resources->has_dict()) unreadable();
                process(inner, true, form_resources ? form_resources->dict.get() : resources, form_state, depth + 1);
            }
        }
        if (!stack.empty() || !marks.empty()) throw ValueError("PDF has an unclosed graphics state or marked-content block");
    }

private:
    using Object_ = imported::Object;
    Document &doc_;
    std::string layer_;

    // Polygon.equals(Polygon.convex_hull): every turn of the ring the same way, or straight.
    static bool convex(const std::vector<Point> &ring) {
        std::vector<Point> points;
        for (size_t i = 0; i + 1 < ring.size(); ++i)
            if (points.empty() || points.back() != ring[i]) points.push_back(ring[i]);
        while (points.size() > 1 && points.front() == points.back()) points.pop_back();
        int sign = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            const int turn = orient(points[i], points[(i + 1) % points.size()], points[(i + 2) % points.size()]);
            if (turn == 0) continue;
            if (sign == 0) sign = turn;
            else if (turn != sign) return false;
        }
        return sign != 0;
    }
};

// RectangleObject(page.cropbox) and the like: four numbers. pypdf reads a reference in the array as 0.
std::vector<double> box_of(const Object *box) {
    if (!box || box->kind == Kind::Null) decline("a PDF page without a media box");
    if (box->kind != Kind::Array) unreadable();
    for (const Object &v : *box->array)
        if (v.kind == Kind::Ref) decline("a PDF box with a reference among its numbers");
    return numbers(*box->array, 4);
}

json read_page(const std::string &bytes, long long number) {
    Document doc(bytes);
    // pypdf reads an encrypted file's security handler before the reference refuses it: the core refuses the
    // standard RC4 handlers pypdf reads, and declines the others.
    if (doc.trailer.count("/Encrypt")) {
        const Object *encrypt = doc.get(doc.trailer, "/Encrypt");
        const auto whole = [&](const char *key) -> long long {
            const Object *o = encrypt && encrypt->kind == Kind::Dict ? raw_entry(*encrypt->dict, key) : nullptr;
            if (!o || o->kind != Kind::Int) decline("an encrypted PDF of a kind the core does not read");
            return o->integer;
        };
        if (!encrypt || encrypt->kind != Kind::Dict) decline("an encrypted PDF of a kind the core does not read");
        const Object *filter = raw_entry(*encrypt->dict, "/Filter");
        const Object *id = raw_entry(doc.trailer, "/ID");
        const long long version = whole("/V"), revision = whole("/R");
        whole("/P");
        if (encrypt->dict->count("/Length")) whole("/Length");
        if (!filter || !filter->is_name("/Standard") || encrypt->dict->count("/SubFilter") || (version != 1 && version != 2) ||
            (revision != 2 && revision != 3) || !id || id->kind != Kind::Array || id->array->size() != 2 ||
            (*id->array)[0].kind != Kind::String)
            decline("an encrypted PDF of a kind the core does not read");
        throw ValueError("Export an unencrypted PDF for vector review");
    }
    const Object *root = doc.get(doc.trailer, "/Root");
    if (!root || root->kind != Kind::Dict) unreadable();
    const Object *top = doc.get(*root->dict, "/Pages");
    if (!top || top->kind != Kind::Dict) unreadable();
    // reader.pages: the leaves of the page tree, with /Resources, /MediaBox, /CropBox and /Rotate inherited.
    struct Leaf {
        const Object *page;
        Dict inherited;
    };
    std::vector<Leaf> leaves;
    size_t entries = 0;
    std::set<const Object *> visiting;
    std::function<void(const Object *, Dict)> walk = [&](const Object *node, Dict inherited) {
        if (!node || node->kind != Kind::Dict) unreadable();
        if (!visiting.insert(node).second) decline("a PDF page tree that refers to itself");
        const Object *type = doc.get(*node->dict, "/Type");
        const Object *kids = doc.get(*node->dict, "/Kids");
        const bool pages = type ? type->is_name("/Pages") : kids != nullptr;
        if (!type) decline("a PDF page-tree node without a /Type");
        if (pages) {
            if (!kids || kids->kind != Kind::Array) unreadable();
            for (const char *key : {"/Resources", "/MediaBox", "/CropBox", "/Rotate"}) {
                const auto found = node->dict->find(key);
                if (found != node->dict->end()) inherited[key] = found->second;
            }
            entries += kids->array->size();
            if (entries > 400) decline("a PDF page tree near pypdf's limit on entries");
            for (const Object &kid : *kids->array) walk(doc.resolve(kid), inherited);
        } else {
            if (!type->is_name("/Page")) decline("an unusual PDF page-tree node");
            leaves.push_back({node, inherited});
            if (leaves.size() > 400) decline("a PDF with more pages than the core reads");
        }
        visiting.erase(node);
    };
    walk(top, Dict());
    const long long count = static_cast<long long>(leaves.size());
    if (count > 500) throw ValueError("Split the PDF into documents of at most 500 pages");
    if (!(1 <= number && number <= count)) throw ValueError("Choose a page from 1 to " + std::to_string(count));
    const Leaf &leaf = leaves[static_cast<size_t>(number - 1)];
    const Dict &page = *leaf.page->dict;
    const auto attribute = [&](const char *key) -> const Object * {
        const auto own = page.find(key);
        if (own != page.end()) return doc.resolve(own->second);
        const auto inherited = leaf.inherited.find(key);
        return inherited == leaf.inherited.end() ? nullptr : doc.resolve(inherited->second);
    };
    double user_unit = 1.0;
    if (const Object *unit = raw_entry(page, "/UserUnit")) {
        if (!unit->is_number()) decline("an unusual PDF user unit");
        user_unit = unit->value();
    }
    const double scale = user_unit * 25.4 / 72;
    const Object *media = attribute("/MediaBox");
    const Object *crop = attribute("/CropBox");
    std::vector<double> c = box_of(crop && crop->kind != Kind::Null ? crop : media), mb = box_of(media);
    const double x = py_max(c[0], mb[0]), y = py_max(c[1], mb[1]), X = py_min(c[2], mb[2]), Y = py_min(c[3], mb[3]);
    if (!(0 < scale && scale <= 1e5) || !(x < X && y < Y)) throw ValueError("Invalid PDF page scale or crop box");
    long long rotation = 0;
    if (const Object *rotate = attribute("/Rotate")) {
        if (rotate->kind == Kind::Int) {
            rotation = ((rotate->integer % 360) + 360) % 360;
        } else if (rotate->kind == Kind::Real) {
            // float % 360, which keeps the divisor's sign.
            double turned = std::fmod(rotate->real, 360.0);
            if (turned < 0) turned += 360.0;
            if (turned != 0 && turned != 90 && turned != 180 && turned != 270) throw ValueError("PDF page rotation must be a multiple of 90 degrees");
            rotation = static_cast<long long>(turned);
        } else {
            decline("an unusual PDF page rotation");
        }
    }
    const double width = (X - x) * scale, height = (Y - y) * scale;
    Matrix turn;
    if (rotation == 0) turn = kIdentity;
    else if (rotation == 90) turn = {0, -1, 1, 0, 0, width};
    else if (rotation == 180) turn = {-1, 0, 0, -1, width, height};
    else if (rotation == 270) turn = {0, 1, -1, 0, height, 0};
    else throw ValueError("PDF page rotation must be a multiple of 90 degrees");
    const Matrix matrix = compose(turn, {scale, 0, 0, scale, -x * scale, -y * scale});
    State state;
    state.matrix = matrix;
    state.clip = std::make_shared<Clip>(rectangle(matrix, {x, y, X, Y}));
    Extraction extraction(doc, number);
    const auto contents = page.find("/Contents");
    const Object *raw = contents == page.end() ? nullptr : doc.resolve(contents->second);
    const std::string content = extraction.content(raw);
    const Object *resources = attribute("/Resources");
    if (resources && !resources->has_dict()) unreadable();
    extraction.process(content, raw != nullptr, resources ? resources->dict.get() : nullptr, state);
    if (const Object *annotations = raw_entry(page, "/Annots")) {
        // page.get('/Annots') is the entry itself, which a reference makes true; len() of what it refers to.
        if (annotations->kind == Kind::Ref || truth(*annotations)) {
            const Object *list = doc.resolve(*annotations);
            if (list->kind != Kind::Array) decline("unusual PDF annotations");
            extraction.skip("annotations omitted", static_cast<long long>(list->array->size()));
        }
    }
    Drawing &drawing = extraction.drawing;
    drawing.warnings.push_back(
        "Paths keep page scale. Strokes import as centrelines and fills as boundaries. Check stacked artwork, holes and orientation before "
        "choosing a cutting operation.");
    for (const auto &item : extraction.skips) drawing.warnings.push_back(std::to_string(item.second) + " \xc3\x97 " + item.first + ".");
    json result;
    if (drawing.objects.empty()) {
        drawing.warnings.push_back("No supported vector paths on this page. Select another page or convert source artwork to curves.");
        result = json::object();
        result["objects"] = json::array();
        result["layers"] = json::array();
        result["presets"] = json::array();
        result["devices"] = json::array();
        result["warnings"] = json(drawing.warnings);
        result["mode"] = json();
    } else {
        result = drawing.finish();
    }
    result["page"] = number;
    result["pageCount"] = count;
    result["pageSize"] = rotation == 90 || rotation == 270 ? json::array({height, width}) : json::array({width, height});
    return result;
}

}  // namespace

Json import_pdf(const Json &payload) {
    const json *content = payload.is_object() ? get(payload, "content") : nullptr;
    if (!content || !content->is_string()) throw ValueError("Choose a PDF document");
    const std::string &encoded = content->get_ref<const std::string &>();
    if (detail::code_points(encoded).size() > 4 * ((kMaxBytes + 2) / 3)) throw ValueError("Choose a PDF no larger than 4 MB");
    std::vector<unsigned char> data;
    if (!detail::base64(encoded, data)) throw ValueError("Invalid PDF file encoding");
    if (data.size() > kMaxBytes || data.size() < 5 || std::string(data.begin(), data.begin() + 5) != "%PDF-")
        throw ValueError("Choose a PDF document no larger than 4 MB");
    long long number = 1;
    if (const json *page = get(payload, "page")) {
        if (!page->is_number_integer() && !page->is_number_unsigned()) throw ValueError("Select a positive PDF page number");
        if (page->is_number_unsigned()) {
            const uint64_t given = page->get<uint64_t>();
            number = given > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<long long>(given);
        } else {
            number = page->get<long long>();
        }
        if (number < 1) throw ValueError("Select a positive PDF page number");
    }
    try {
        return read_page(std::string(data.begin(), data.end()), number);
    } catch (const Unreadable &) {
        throw ValueError("Cannot read this PDF page. Export a simplified, unencrypted vector PDF.");
    }
}

}  // namespace kerfkite
