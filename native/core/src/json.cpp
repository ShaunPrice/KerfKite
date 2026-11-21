// The JSON reader and writer: see kerfkite/json.hpp for what they accept and produce.
#include "kerfkite/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kerfkite/number_text.hpp"

namespace kerfkite {

namespace {

using Type = Json::Type;

// The length of the well-formed UTF-8 sequence that starts at p (RFC 3629), or 0 when the bytes there are
// not one: a stray continuation byte, an overlong form, a surrogate, a code point past U+10FFFF or a
// sequence cut short by the end.
inline size_t utf8_length(const unsigned char *p, const unsigned char *end) {
    const unsigned char lead = p[0];
    if (lead < 0x80) return 1;
    const auto follows = [&](size_t i, unsigned char low, unsigned char high) {
        return static_cast<size_t>(end - p) > i && p[i] >= low && p[i] <= high;
    };
    if (lead >= 0xC2 && lead <= 0xDF) return follows(1, 0x80, 0xBF) ? 2 : 0;
    if (lead >= 0xE0 && lead <= 0xEF) {
        const unsigned char low = lead == 0xE0 ? 0xA0 : 0x80, high = lead == 0xED ? 0x9F : 0xBF;
        return follows(1, low, high) && follows(2, 0x80, 0xBF) ? 3 : 0;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        const unsigned char low = lead == 0xF0 ? 0x90 : 0x80, high = lead == 0xF4 ? 0x8F : 0xBF;
        return follows(1, low, high) && follows(2, 0x80, 0xBF) && follows(3, 0x80, 0xBF) ? 4 : 0;
    }
    return 0;
}

void append_utf8(std::string &out, uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::string byte_text(unsigned char byte) {
    static const char HEX[] = "0123456789ABCDEF";
    if (byte >= 0x20 && byte < 0x7F) return std::string("'") + static_cast<char>(byte) + "'";
    return std::string("byte 0x") + HEX[byte >> 4] + HEX[byte & 15];
}

// Recursive descent over the text. Each array or object costs one level of recursion, and the depth is
// limited, so the stack used does not depend on the input.
class Reader {
public:
    explicit Reader(std::string_view text)
        : begin_(reinterpret_cast<const unsigned char *>(text.data())), at_(begin_), end_(begin_ + text.size()) {}

    Json document() {
        if (end_ - at_ >= 3 && at_[0] == 0xEF && at_[1] == 0xBB && at_[2] == 0xBF) at_ += 3;  // byte order mark
        space();
        Json value = read(0);
        space();
        if (at_ != end_) fail("the text continues after its value with " + byte_text(*at_));
        return value;
    }

private:
    [[noreturn]] void fail(const std::string &what) const {
        size_t line = 1, column = 1;
        for (const unsigned char *p = begin_; p != at_; ++p) {
            if (*p == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw Json::parse_error("line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + what);
    }

    std::string found() const { return at_ == end_ ? std::string("the text ends") : "found " + byte_text(*at_); }

    void space() {
        while (at_ != end_ && (*at_ == ' ' || *at_ == '\n' || *at_ == '\r' || *at_ == '\t')) ++at_;
    }

    void word(const char *text) {
        const size_t length = std::strlen(text);
        if (static_cast<size_t>(end_ - at_) < length || std::memcmp(at_, text, length) != 0)
            fail(std::string("a value was expected; this is not ") + text);
        at_ += length;
    }

    Json read(int depth) {
        if (at_ == end_) fail("a value was expected, but the text ends");
        switch (*at_) {
        case '{':
            return object(depth);
        case '[':
            return array(depth);
        case '"':
            return Json(string());
        case 't':
            word("true");
            return Json(true);
        case 'f':
            word("false");
            return Json(false);
        case 'n':
            word("null");
            return Json();
        default:
            if (*at_ == '-' || (*at_ >= '0' && *at_ <= '9')) return number();
            fail("a value was expected, " + found());
        }
    }

    Json array(int depth) {
        if (depth >= Json::MAX_DEPTH) fail("arrays and objects are nested more than " + std::to_string(Json::MAX_DEPTH) + " deep");
        ++at_;
        Json::Array entries;
        space();
        if (at_ != end_ && *at_ == ']') {
            ++at_;
            return Json(std::move(entries));
        }
        for (;;) {
            space();
            entries.push_back(read(depth + 1));
            space();
            if (at_ != end_ && *at_ == ',') {
                ++at_;
                continue;
            }
            if (at_ != end_ && *at_ == ']') {
                ++at_;
                return Json(std::move(entries));
            }
            fail("',' or ']' was expected in an array, " + found());
        }
    }

    Json object(int depth) {
        if (depth >= Json::MAX_DEPTH) fail("arrays and objects are nested more than " + std::to_string(Json::MAX_DEPTH) + " deep");
        ++at_;
        Json::Object members;
        space();
        if (at_ != end_ && *at_ == '}') {
            ++at_;
            return Json::object(std::move(members));
        }
        for (;;) {
            space();
            if (at_ == end_ || *at_ != '"') fail("a name in quotes was expected in an object, " + found());
            std::string name = string();
            space();
            if (at_ == end_ || *at_ != ':') fail("':' was expected after a name, " + found());
            ++at_;
            space();
            members.emplace_back(std::move(name), read(depth + 1));
            space();
            if (at_ != end_ && *at_ == ',') {
                ++at_;
                continue;
            }
            if (at_ != end_ && *at_ == '}') {
                ++at_;
                return Json::object(std::move(members));
            }
            fail("',' or '}' was expected in an object, " + found());
        }
    }

    unsigned hex4() {
        if (end_ - at_ < 4) fail("four hexadecimal digits were expected after \\u, but the text ends");
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned char c = at_[i];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else {
                at_ += i;
                fail("four hexadecimal digits were expected after \\u, " + found());
            }
            code = code * 16 + digit;
        }
        at_ += 4;
        return code;
    }

    std::string string() {
        ++at_;  // the opening quote
        std::string out;
        for (;;) {
            const unsigned char *run = at_;
            while (at_ != end_ && *at_ >= 0x20 && *at_ < 0x80 && *at_ != '"' && *at_ != '\\') ++at_;
            out.append(reinterpret_cast<const char *>(run), static_cast<size_t>(at_ - run));
            if (at_ == end_) fail("text in quotes is not closed");
            const unsigned char c = *at_;
            if (c == '"') {
                ++at_;
                return out;
            }
            if (c < 0x20) fail("a control character (" + byte_text(c) + ") in text must be written as an escape");
            if (c >= 0x80) {
                const size_t length = utf8_length(at_, end_);
                if (!length) fail("text is not valid UTF-8 at " + byte_text(c));
                out.append(reinterpret_cast<const char *>(at_), length);
                at_ += length;
                continue;
            }
            ++at_;  // the backslash
            if (at_ == end_) fail("text in quotes is not closed");
            const unsigned char escape = *at_++;
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(static_cast<char>(escape));
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t code = hex4();
                if (code >= 0xDC00 && code <= 0xDFFF) fail("\\u escape for the second half of a surrogate pair without the first");
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (end_ - at_ < 2 || at_[0] != '\\' || at_[1] != 'u')
                        fail("\\u escape for the first half of a surrogate pair without the second");
                    at_ += 2;
                    const uint32_t second = hex4();
                    if (second < 0xDC00 || second > 0xDFFF)
                        fail("\\u escape for the first half of a surrogate pair without the second");
                    code = 0x10000 + ((code - 0xD800) << 10) + (second - 0xDC00);
                }
                append_utf8(out, code);
                break;
            }
            default:
                --at_;
                fail("an escape that JSON does not have: backslash and " + byte_text(escape));
            }
        }
    }

    Json number() {
        const unsigned char *start = at_;
        const bool negative = *at_ == '-';
        if (negative) ++at_;
        if (at_ == end_ || *at_ < '0' || *at_ > '9') fail("a digit was expected in a number, " + found());
        uint64_t whole = 0;
        bool fits = true;
        if (*at_ == '0') {
            ++at_;
            if (at_ != end_ && *at_ >= '0' && *at_ <= '9') fail("a number may not begin with a needless 0");
        } else {
            for (; at_ != end_ && *at_ >= '0' && *at_ <= '9'; ++at_) {
                const uint64_t digit = *at_ - '0';
                if (whole > (UINT64_MAX - digit) / 10) fits = false;
                else whole = whole * 10 + digit;
            }
        }
        bool real = false;
        if (at_ != end_ && *at_ == '.') {
            real = true;
            ++at_;
            if (at_ == end_ || *at_ < '0' || *at_ > '9') fail("a digit was expected after the point of a number, " + found());
            while (at_ != end_ && *at_ >= '0' && *at_ <= '9') ++at_;
        }
        if (at_ != end_ && (*at_ == 'e' || *at_ == 'E')) {
            real = true;
            ++at_;
            if (at_ != end_ && (*at_ == '+' || *at_ == '-')) ++at_;
            if (at_ == end_ || *at_ < '0' || *at_ > '9') fail("a digit was expected in the exponent of a number, " + found());
            while (at_ != end_ && *at_ >= '0' && *at_ <= '9') ++at_;
        }
        if (!real && fits) {
            if (!negative) return Json(whole);
            if (whole <= static_cast<uint64_t>(INT64_MAX)) return Json(-static_cast<int64_t>(whole));
            if (whole == static_cast<uint64_t>(INT64_MAX) + 1) return Json(INT64_MIN);
        }
        // A float, or a whole number too large for 64 bits, which is read as one.
        double value = 0;
        text::parse_double(reinterpret_cast<const char *>(start), reinterpret_cast<const char *>(at_), value);
        if (!std::isfinite(value)) {
            at_ = start;
            fail("a number is outside the range of a double");
        }
        return Json(value);
    }

    const unsigned char *begin_, *at_, *end_;
};

void write_whole(std::string &out, uint64_t value) {
    char digits[20];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) out.push_back(digits[--n]);
}

void write_text(std::string &out, const std::string &text) {
    static const char HEX[] = "0123456789abcdef";
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text.data()), *end = p + text.size();
    out.push_back('"');
    while (p != end) {
        const unsigned char *run = p;
        while (p != end && *p >= 0x20 && *p < 0x80 && *p != '"' && *p != '\\') ++p;
        out.append(reinterpret_cast<const char *>(run), static_cast<size_t>(p - run));
        if (p == end) break;
        const unsigned char c = *p;
        if (c >= 0x80) {
            const size_t length = utf8_length(p, end);
            if (!length) throw Json::type_error("text to be written as JSON is not valid UTF-8 at " + byte_text(c));
            out.append(reinterpret_cast<const char *>(p), length);
            p += length;
            continue;
        }
        ++p;
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.append("\\u00");
            out.push_back(HEX[c >> 4]);
            out.push_back(HEX[c & 15]);
        }
    }
    out.push_back('"');
}

}  // namespace

Json::Json(const char *text) : type_(Type::string) {
    if (!text) throw type_error("a null pointer is not text");
    value_.text = new std::string(text);
}

Json::Json(const std::string &text) : type_(Type::string) { value_.text = new std::string(text); }

Json::Json(std::string &&text) : type_(Type::string) { value_.text = new std::string(std::move(text)); }

Json::Json(const Array &values) : type_(Type::array) { value_.array = new Array(values); }

Json::Json(Array &&values) : type_(Type::array) { value_.array = new Array(std::move(values)); }

Json::Json(std::initializer_list<Init> list) : Json() {
    bool pairs = true;
    for (const Init &entry : list) {
        const Json &value = entry.value_;
        pairs = pairs && value.type_ == Type::array && value.value_.array->size() == 2 &&
                (*value.value_.array)[0].type_ == Type::string;
    }
    if (pairs) {
        Object members;
        members.reserve(list.size());
        for (const Init &entry : list) {
            Array &pair = *entry.value_.value_.array;
            // Of two entries with one name the first stands, as in a brace-initialised std::map.
            bool repeated = false;
            for (const Member &member : members) repeated = repeated || member.first == *pair[0].value_.text;
            if (!repeated) members.emplace_back(std::move(*pair[0].value_.text), std::move(pair[1]));
        }
        *this = object(std::move(members));
    } else {
        *this = array(list);
    }
}

Json::Json(const Json &other) : type_(other.type_), value_(other.value_) {
    if (type_ == Type::string) value_.text = new std::string(*other.value_.text);
    else if (type_ == Type::array) value_.array = new Array(*other.value_.array);
    else if (type_ == Type::object) value_.object = new Object(*other.value_.object);
}

void Json::destroy() noexcept {
    if (type_ == Type::string) delete value_.text;
    else if (type_ == Type::array) delete value_.array;
    else if (type_ == Type::object) delete value_.object;
}

Json Json::array(std::initializer_list<Init> list) {
    Array entries;
    entries.reserve(list.size());
    for (const Init &entry : list) entries.push_back(std::move(entry.value_));
    return Json(std::move(entries));
}

Json Json::object() { return object(Object()); }

Json Json::object(Object &&members) {
    const auto before = [](const Member &a, const Member &b) { return a.first < b.first; };
    bool ordered = true;
    for (size_t i = 1; i < members.size() && ordered; ++i) ordered = before(members[i - 1], members[i]);
    if (!ordered) {
        // Sorting keeps members of one name in the order given, and the last of them is kept.
        std::stable_sort(members.begin(), members.end(), before);
        size_t kept = 0;
        for (size_t i = 0; i < members.size(); ++i) {
            if (i + 1 < members.size() && members[i].first == members[i + 1].first) continue;
            if (kept != i) members[kept] = std::move(members[i]);
            ++kept;
        }
        members.erase(members.begin() + static_cast<std::ptrdiff_t>(kept), members.end());
    }
    Json out;
    out.value_.object = new Object(std::move(members));
    out.type_ = Type::object;
    return out;
}

Json Json::parse(std::string_view text) { return Reader(text).document(); }

Json Json::parse(std::string_view text, std::nullptr_t, bool allow_exceptions) {
    if (allow_exceptions) return parse(text);
    try {
        return parse(text);
    } catch (const exception &) {
        Json out;
        out.type_ = Type::discarded;
        return out;
    }
}

std::string Json::dump() const {
    std::string out;
    write_to(out);
    return out;
}

void Json::write_to(std::string &out) const {
    switch (type_) {
    case Type::null:
        out.append("null");
        break;
    case Type::discarded:
        out.append("null");  // the answer of a failed parse holds no value; what is written stays valid JSON
        break;
    case Type::boolean:
        out.append(value_.boolean ? "true" : "false");
        break;
    case Type::number_integer:
        if (value_.integer < 0) {
            out.push_back('-');
            write_whole(out, 0 - static_cast<uint64_t>(value_.integer));
        } else {
            write_whole(out, static_cast<uint64_t>(value_.integer));
        }
        break;
    case Type::number_unsigned:
        write_whole(out, value_.unsigned_integer);
        break;
    case Type::number_float:
        // JSON has no text for an infinity or a NaN.
        if (std::isfinite(value_.real)) text::append_repr(out, value_.real);
        else out.append("null");
        break;
    case Type::string:
        write_text(out, *value_.text);
        break;
    case Type::array: {
        out.push_back('[');
        bool first = true;
        for (const Json &entry : *value_.array) {
            if (!first) out.push_back(',');
            first = false;
            entry.write_to(out);
        }
        out.push_back(']');
        break;
    }
    case Type::object: {
        out.push_back('{');
        bool first = true;
        for (const Member &member : *value_.object) {
            if (!first) out.push_back(',');
            first = false;
            write_text(out, member.first);
            out.push_back(':');
            member.second.write_to(out);
        }
        out.push_back('}');
        break;
    }
    }
}

size_t Json::size() const noexcept {
    switch (type_) {
    case Type::null:
        return 0;
    case Type::array:
        return value_.array->size();
    case Type::object:
        return value_.object->size();
    default:
        return 1;
    }
}

const char *Json::kind() const noexcept {
    switch (type_) {
    case Type::null:
        return "null";
    case Type::object:
        return "an object";
    case Type::array:
        return "an array";
    case Type::string:
        return "text";
    case Type::boolean:
        return "true or false";
    case Type::discarded:
        return "text that is not JSON";
    default:
        return "a number";
    }
}

void Json::wrong(const char *wanted) const {
    throw type_error(std::string("the JSON value should be ") + wanted + ", but is " + kind());
}

const std::string &Json::text() const {
    if (type_ != Type::string) wrong("text");
    return *value_.text;
}

int64_t Json::whole_signed() const {
    if (type_ != Type::number_float) wrong("a number");
    // Every double in [-2^63, 2^63) becomes a whole number by dropping its fraction.
    if (!(value_.real >= -9223372036854775808.0 && value_.real < 9223372036854775808.0))
        wrong("a whole number that fits");
    return static_cast<int64_t>(value_.real);
}

uint64_t Json::whole_unsigned() const {
    if (type_ != Type::number_float) wrong("a number");
    if (!(value_.real > -1.0 && value_.real < 18446744073709551616.0))
        wrong("a whole number that fits");
    return static_cast<uint64_t>(value_.real);
}

const Json &Json::at(size_t index) const {
    if (type_ != Type::array) wrong("an array");
    if (index >= value_.array->size())
        throw out_of_range("the JSON array has " + std::to_string(value_.array->size()) + " entries, so none at position " +
                           std::to_string(index));
    return (*value_.array)[index];
}

namespace {

// The first member whose name is not before the one given.
Json::Object::const_iterator place(const Json::Object &members, std::string_view name) {
    return std::lower_bound(members.begin(), members.end(), name,
                            [](const Json::Member &member, std::string_view wanted) { return std::string_view(member.first) < wanted; });
}

}  // namespace

const Json *Json::member(std::string_view name) const noexcept {
    if (type_ != Type::object) return nullptr;
    const auto found = place(*value_.object, name);
    return found != value_.object->end() && found->first == name ? &found->second : nullptr;
}

const Json *Json::member_of_object(std::string_view name) const {
    if (type_ != Type::object) wrong("an object");
    return member(name);
}

size_t Json::erase(std::string_view name) {
    if (type_ != Type::object) wrong("an object");
    const auto found = place(*value_.object, name);
    if (found == value_.object->end() || found->first != name) return 0;
    value_.object->erase(found);
    return 1;
}

Json &Json::last() { return value_.array->back(); }

Json::const_iterator Json::find(std::string_view name) const noexcept {
    const Json *found = member(name);
    if (!found) return end();
    // A member's value is the second half of an entry of the object's own storage.
    const auto at = place(*value_.object, name);
    return const_iterator(this, static_cast<size_t>(at - value_.object->begin()));
}

const Json &Json::at(std::string_view name) const {
    if (type_ != Type::object) wrong("an object");
    const Json *found = member(name);
    if (!found) throw out_of_range("the JSON object has no \"" + std::string(name) + "\"");
    return *found;
}

Json &Json::operator[](std::string_view name) {
    if (type_ == Type::null) *this = object();
    if (type_ != Type::object) wrong("an object");
    const auto found = place(*value_.object, name);
    if (found != value_.object->end() && found->first == name) return const_cast<Json &>(found->second);
    return value_.object->emplace(found, std::string(name), Json())->second;
}

void Json::push_back(Json value) {
    if (type_ == Type::null) *this = array();
    if (type_ != Type::array) wrong("an array");
    value_.array->push_back(std::move(value));
}

const Json &Json::entry(size_t index) const {
    if (type_ == Type::array) return (*value_.array)[index];
    if (type_ == Type::object) return (*value_.object)[index].second;
    return *this;
}

const std::string &Json::name(size_t index) const {
    if (type_ != Type::object) wrong("an object");
    return (*value_.object)[index].first;
}

bool Json::equals(const Json &other) const noexcept {
    if (type_ == other.type_) {
        switch (type_) {
        case Type::null:
            return true;
        case Type::discarded:
            return false;
        case Type::boolean:
            return value_.boolean == other.value_.boolean;
        case Type::number_integer:
            return value_.integer == other.value_.integer;
        case Type::number_unsigned:
            return value_.unsigned_integer == other.value_.unsigned_integer;
        case Type::number_float:
            return value_.real == other.value_.real;
        case Type::string:
            return *value_.text == *other.value_.text;
        case Type::array:
            return *value_.array == *other.value_.array;
        case Type::object:
            return *value_.object == *other.value_.object;
        }
        return false;
    }
    // Numbers of different kinds compare by value, as the C++ conversions give it.
    const Type a = type_, b = other.type_;
    if (a == Type::number_integer && b == Type::number_float) return static_cast<double>(value_.integer) == other.value_.real;
    if (a == Type::number_float && b == Type::number_integer) return value_.real == static_cast<double>(other.value_.integer);
    if (a == Type::number_unsigned && b == Type::number_float) return static_cast<double>(value_.unsigned_integer) == other.value_.real;
    if (a == Type::number_float && b == Type::number_unsigned) return value_.real == static_cast<double>(other.value_.unsigned_integer);
    if (a == Type::number_unsigned && b == Type::number_integer) return static_cast<int64_t>(value_.unsigned_integer) == other.value_.integer;
    if (a == Type::number_integer && b == Type::number_unsigned) return value_.integer == static_cast<int64_t>(other.value_.unsigned_integer);
    return false;
}

}  // namespace kerfkite
