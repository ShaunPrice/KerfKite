// KerfKite's own JSON reader and writer (RFC 8259), with only the C++ standard library behind it.
//
// A Json is one value: null, true or false, a number, text, an array of values or an object of named
// values. The class has the operations the core uses and little more; it is not a general-purpose library.
//
// Numbers keep the kind they were written with, because the Python reference tells an int from a float:
// 1 is a whole number (unsigned when read from text without a sign, signed when it has one or comes from
// a signed C++ integer), 1.0 and 1e3 are floats, and a whole number too large for 64 bits is read as a
// float. Floats are read to the nearest double and written as the shortest text that reads back as the
// same double, in the form of Python's repr() (kerfkite/decimal.hpp, kerfkite/number_text.hpp); a float
// that is not finite is written as null. Whole numbers and floats compare by value.
//
// Objects keep their names in byte order, each once; where a text repeats a name the last value stands.
//
// Text is UTF-8. The reader takes every JSON escape, joins surrogate pairs, and refuses control
// characters, unpaired surrogates and bytes that are not well-formed UTF-8; the writer escapes the quote,
// the backslash and control characters and leaves everything else as it is.
//
// Limits: arrays and objects nest at most Json::MAX_DEPTH deep in text that is read, so that hostile
// input cannot exhaust the stack; a literal past the range of a double is an error; nothing else is
// bounded but by memory, which grows in proportion to the text.
//
// Every failure is a Json::exception: parse_error for text that is not JSON, type_error for asking a value
// for something it is not, out_of_range for a name or index that is not there.
#ifndef KERFKITE_JSON_HPP
#define KERFKITE_JSON_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kerfkite {

class Json {
public:
    enum class Type : uint8_t {
        null,
        object,
        array,
        string,
        boolean,
        number_integer,   // a signed whole number
        number_unsigned,  // a whole number without a sign
        number_float,
        discarded,  // what parse() returns for bad text when asked not to throw
    };

    class exception : public std::exception {
    public:
        explicit exception(std::string message) : message_(std::move(message)) {}
        const char *what() const noexcept override { return message_.c_str(); }

    private:
        std::string message_;
    };
    struct parse_error : exception {
        using exception::exception;
    };
    struct type_error : exception {
        using exception::exception;
    };
    struct out_of_range : exception {
        using exception::exception;
    };

    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;  // in byte order of the names, each once

    static constexpr int MAX_DEPTH = 200;

    // One entry of a braced list: see Json(std::initializer_list<Init>).
    class Init;

    Json() noexcept : type_(Type::null) { value_.unsigned_integer = 0; }
    Json(std::nullptr_t) noexcept : Json() {}
    template <class T, std::enable_if_t<std::is_same<T, bool>::value, int> = 0>
    Json(T value) noexcept : type_(Type::boolean) {
        value_.boolean = value;
    }
    template <class T, std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value, int> = 0>
    Json(T value) noexcept : type_(std::is_signed<T>::value ? Type::number_integer : Type::number_unsigned) {
        if (std::is_signed<T>::value) value_.integer = static_cast<int64_t>(value);
        else value_.unsigned_integer = static_cast<uint64_t>(value);
    }
    template <class T, std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
    Json(T value) noexcept : type_(Type::number_float) {
        value_.real = static_cast<double>(value);
    }
    Json(const char *text);
    Json(const std::string &text);
    Json(std::string &&text);
    Json(const Array &values);
    Json(Array &&values);
    template <class T, std::enable_if_t<!std::is_same<T, Json>::value && std::is_constructible<Json, const T &>::value, int> = 0>
    Json(const std::vector<T> &values) : Json(Array()) {
        value_.array->reserve(values.size());
        for (const T &value : values) value_.array->emplace_back(value);
    }
    template <class T, size_t N, std::enable_if_t<std::is_constructible<Json, const T &>::value, int> = 0>
    Json(const std::array<T, N> &values) : Json(Array()) {
        value_.array->reserve(N);
        for (const T &value : values) value_.array->emplace_back(value);
    }
    // A braced list. {{"name", value}, ...} is an object: that is, a list whose every entry is an array of
    // two values, the first of them text. Any other list is an array; array() makes one of any list.
    Json(std::initializer_list<Init> list);

    Json(const Json &other);
    Json(Json &&other) noexcept : type_(other.type_), value_(other.value_) {
        other.type_ = Type::null;
        other.value_.unsigned_integer = 0;
    }
    Json &operator=(Json other) noexcept {
        swap(other);
        return *this;
    }
    ~Json() {
        if (type_ == Type::object || type_ == Type::array || type_ == Type::string) destroy();
    }
    void swap(Json &other) noexcept {
        std::swap(type_, other.type_);
        std::swap(value_, other.value_);
    }

    static Json array() { return Json(Array()); }
    static Json array(std::initializer_list<Init> list);
    static Json object();
    // An object of these members, put in the order of their names; where a name repeats the last stands.
    static Json object(Object &&members);

    // Read one JSON text. The text may begin with a UTF-8 byte order mark and may have white space
    // either side of its value; anything else after the value is an error.
    static Json parse(std::string_view text);
    // The same, but with allow_exceptions false a text that is not JSON gives a value that
    // is_discarded() instead of an exception. The middle argument is always nullptr.
    static Json parse(std::string_view text, std::nullptr_t, bool allow_exceptions);

    // The value as JSON text on one line, with no spaces. Throws type_error for text that is not UTF-8.
    std::string dump() const;

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::null; }
    bool is_object() const noexcept { return type_ == Type::object; }
    bool is_array() const noexcept { return type_ == Type::array; }
    bool is_string() const noexcept { return type_ == Type::string; }
    bool is_boolean() const noexcept { return type_ == Type::boolean; }
    bool is_number_integer() const noexcept { return type_ == Type::number_integer || type_ == Type::number_unsigned; }
    bool is_number_unsigned() const noexcept { return type_ == Type::number_unsigned; }
    bool is_number_float() const noexcept { return type_ == Type::number_float; }
    bool is_number() const noexcept { return is_number_integer() || is_number_float(); }
    bool is_discarded() const noexcept { return type_ == Type::discarded; }

    // The value as a C++ type: bool from true or false only; a number type from any number (a float is
    // cut towards zero to make a whole number, and must fit the type); std::string from text;
    // std::vector<T> from an array whose every entry gives a T. Anything else throws type_error.
    template <class T>
    T get() const {
        return get_as(static_cast<T *>(nullptr));
    }
    // The text itself, without a copy: get_ref<const std::string &>().
    template <class T>
    T get_ref() const {
        static_assert(std::is_same<T, const std::string &>::value, "get_ref gives const std::string & only");
        return text();
    }

    // Entries of an array or an object; 0 for null and 1 for any other single value.
    size_t size() const noexcept;
    bool empty() const noexcept { return size() == 0; }

    // An entry by position. Throws out_of_range past the end and type_error for anything but an array.
    const Json &operator[](size_t index) const { return at(index); }
    Json &operator[](size_t index) { return at(index); }
    const Json &at(size_t index) const;
    Json &at(size_t index) { return const_cast<Json &>(static_cast<const Json &>(*this).at(index)); }

    // An entry by name. Reading a name that is absent throws out_of_range; writing through [] adds it,
    // and turns a null into an object first.
    const Json &operator[](std::string_view name) const { return at(name); }
    Json &operator[](std::string_view name);
    const Json &at(std::string_view name) const;
    Json &at(std::string_view name) { return const_cast<Json &>(static_cast<const Json &>(*this).at(name)); }
    bool contains(std::string_view name) const noexcept { return member(name) != nullptr; }
    // The entry of that name as a T (as get<T>() gives it), or the fallback when an object has no such
    // name. Throws type_error for anything but an object.
    template <class T>
    T value(std::string_view name, const T &fallback) const {
        const Json *found = member_of_object(name);
        return found ? found->template get<T>() : fallback;
    }
    std::string value(std::string_view name, const char *fallback) const { return value(name, std::string(fallback)); }
    // Remove the entry of that name from an object: 1 when there was one, otherwise 0.
    size_t erase(std::string_view name);

    // Add to the end of an array; a null becomes an array first.
    void push_back(Json value);
    template <class... Args>
    Json &emplace_back(Args &&...args) {
        push_back(Json(std::forward<Args>(args)...));
        return last();
    }

    // Iteration visits the entries of an array in order and the values of an object in the order of their
    // names; a single value is visited once and null not at all. key() is the name, for an object.
    template <bool Const>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Json;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<Const, const Json *, Json *>;
        using reference = std::conditional_t<Const, const Json &, Json &>;

        Iterator() noexcept : owner_(nullptr), index_(0) {}
        Iterator(pointer owner, size_t index) noexcept : owner_(owner), index_(index) {}
        template <bool Other, std::enable_if_t<Const && !Other, int> = 0>
        Iterator(const Iterator<Other> &other) noexcept : owner_(other.owner_), index_(other.index_) {}

        reference operator*() const { return const_cast<reference>(owner_->entry(index_)); }
        pointer operator->() const { return &**this; }
        reference value() const { return **this; }
        const std::string &key() const { return owner_->name(index_); }
        Iterator &operator++() noexcept {
            ++index_;
            return *this;
        }
        Iterator operator++(int) noexcept {
            const Iterator before = *this;
            ++index_;
            return before;
        }
        bool operator==(const Iterator &other) const noexcept { return owner_ == other.owner_ && index_ == other.index_; }
        bool operator!=(const Iterator &other) const noexcept { return !(*this == other); }

    private:
        template <bool> friend class Iterator;
        pointer owner_;
        size_t index_;
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    iterator begin() noexcept { return iterator(this, 0); }
    iterator end() noexcept { return iterator(this, size()); }
    const_iterator begin() const noexcept { return const_iterator(this, 0); }
    const_iterator end() const noexcept { return const_iterator(this, size()); }
    // The entry of that name, or end() when it is absent or this is not an object.
    const_iterator find(std::string_view name) const noexcept;

    // for (const auto &item : value.items()): item.key() and item.value() of each entry of an object.
    class Items {
    public:
        class Cursor {
        public:
            explicit Cursor(const_iterator at) noexcept : at_(at) {}
            const const_iterator &operator*() const noexcept { return at_; }
            Cursor &operator++() noexcept {
                ++at_;
                return *this;
            }
            bool operator!=(const Cursor &other) const noexcept { return at_ != other.at_; }

        private:
            const_iterator at_;
        };
        explicit Items(const Json &owner) noexcept : owner_(owner) {}
        Cursor begin() const noexcept { return Cursor(owner_.begin()); }
        Cursor end() const noexcept { return Cursor(owner_.end()); }

    private:
        const Json &owner_;
    };
    Items items() const noexcept { return Items(*this); }

    // Equal kinds and equal contents; a whole number and a float are equal when their values are.
    friend bool operator==(const Json &a, const Json &b) noexcept { return a.equals(b); }
    friend bool operator!=(const Json &a, const Json &b) noexcept { return !a.equals(b); }

private:
    union Value {
        bool boolean;
        int64_t integer;
        uint64_t unsigned_integer;
        double real;
        std::string *text;
        Array *array;
        Object *object;
    };

    void destroy() noexcept;
    void write_to(std::string &out) const;
    bool equals(const Json &other) const noexcept;
    const Json *member(std::string_view name) const noexcept;
    const Json *member_of_object(std::string_view name) const;
    Json &last();
    const Json &entry(size_t index) const;
    const std::string &name(size_t index) const;
    const std::string &text() const;
    const char *kind() const noexcept;
    [[noreturn]] void wrong(const char *wanted) const;
    int64_t whole_signed() const;
    uint64_t whole_unsigned() const;

    bool get_as(bool *) const {
        if (type_ != Type::boolean) wrong("true or false");
        return value_.boolean;
    }
    std::string get_as(std::string *) const { return text(); }
    template <class T, std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value, int> = 0>
    T get_as(T *) const {
        switch (type_) {
        case Type::number_integer:
            return static_cast<T>(value_.integer);
        case Type::number_unsigned:
            return static_cast<T>(value_.unsigned_integer);
        default:
            break;
        }
        // A float, cut towards zero; one that does not fit the type asked for is refused.
        if (std::is_signed<T>::value) {
            const int64_t whole = whole_signed();
            if (static_cast<int64_t>(static_cast<T>(whole)) != whole) wrong("a whole number that fits");
            return static_cast<T>(whole);
        }
        const uint64_t whole = whole_unsigned();
        if (static_cast<uint64_t>(static_cast<T>(whole)) != whole) wrong("a whole number that fits");
        return static_cast<T>(whole);
    }
    template <class T, std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
    T get_as(T *) const {
        switch (type_) {
        case Type::number_integer:
            return static_cast<T>(value_.integer);
        case Type::number_unsigned:
            return static_cast<T>(value_.unsigned_integer);
        case Type::number_float:
            return static_cast<T>(value_.real);
        default:
            wrong("a number");
        }
    }
    template <class T>
    std::vector<T> get_as(std::vector<T> *) const {
        if (type_ != Type::array) wrong("an array");
        std::vector<T> out;
        out.reserve(value_.array->size());
        for (const Json &entry : *value_.array) out.push_back(entry.template get<T>());
        return out;
    }

    Type type_;
    Value value_;
};

class Json::Init {
public:
    template <class T,
              std::enable_if_t<!std::is_same<std::decay_t<T>, Init>::value && std::is_constructible<Json, T &&>::value, int> = 0>
    Init(T &&value) : value_(std::forward<T>(value)) {}
    Init(std::initializer_list<Init> list) : value_(list) {}

private:
    friend class Json;
    mutable Json value_;  // a braced list is const, yet its entries are only ever moved on from here
};

}  // namespace kerfkite

#endif
