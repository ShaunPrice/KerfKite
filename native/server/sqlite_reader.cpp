// SQLite's file format, read. See sqlite_reader.hpp.
#include "sqlite_reader.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>

namespace kerfkite {
namespace server {
namespace {

[[noreturn]] void broken(const std::string &why) { throw std::runtime_error("The SQLite database cannot be read: " + why); }

class File {
public:
    explicit File(const std::string &bytes) : bytes_(bytes) {
        if (bytes_.size() < 100 || std::memcmp(bytes_.data(), "SQLite format 3\0", 16) != 0) broken("it is not an SQLite 3 database");
        const unsigned size = u16(16);
        page_size_ = size == 1 ? 65536u : size;
        if (page_size_ < 512 || page_size_ > 65536 || (page_size_ & (page_size_ - 1))) broken("its page size is not one SQLite writes");
        const unsigned reserved = static_cast<unsigned char>(bytes_[20]);
        if (reserved >= page_size_ - 480) broken("too much of each page is reserved");
        usable_ = page_size_ - reserved;
        if (u32(56) != 1 && u32(56) != 0) broken("its text is not UTF-8");
        pages_ = bytes_.size() / page_size_;
    }

    // The rows of the table whose B-tree starts at root.
    std::vector<SqliteRow> rows(uint32_t root) {
        std::vector<SqliteRow> out;
        std::set<uint32_t> seen;
        walk(root, 0, seen, out);
        return out;
    }

private:
    const std::string &bytes_;
    unsigned page_size_ = 4096, usable_ = 4096;
    size_t pages_ = 0;

    unsigned u16(size_t at) const {
        if (at + 2 > bytes_.size()) broken("it ends early");
        return static_cast<unsigned>(static_cast<unsigned char>(bytes_[at])) << 8 | static_cast<unsigned char>(bytes_[at + 1]);
    }
    uint32_t u32(size_t at) const { return static_cast<uint32_t>(u16(at)) << 16 | u16(at + 2); }

    // The start of page number n (from 1).
    size_t page(uint32_t n) const {
        if (n < 1 || n > pages_) broken("a page number is outside the file");
        return static_cast<size_t>(n - 1) * page_size_;
    }

    // A variable-length integer of one to nine bytes, big-endian, seven bits a byte and eight in the ninth.
    uint64_t varint(size_t &at, size_t end) const {
        uint64_t value = 0;
        for (int i = 0; i < 9; ++i) {
            if (at >= end) broken("a number runs past its page");
            const unsigned char b = static_cast<unsigned char>(bytes_[at++]);
            if (i == 8) return (value << 8) | b;
            value = (value << 7) | (b & 0x7f);
            if (!(b & 0x80)) return value;
        }
        return value;
    }

    void walk(uint32_t number, int depth, std::set<uint32_t> &seen, std::vector<SqliteRow> &out) {
        if (depth > 40 || !seen.insert(number).second) broken("its B-tree is not a tree");
        const size_t start = page(number), header = start + (number == 1 ? 100 : 0), end = start + usable_;
        const unsigned char kind = static_cast<unsigned char>(bytes_[header]);
        if (kind != 0x0d && kind != 0x05) broken("a table page is of another kind");
        const unsigned cells = u16(header + 3);
        const size_t pointers = header + (kind == 0x05 ? 12 : 8);
        for (unsigned i = 0; i < cells; ++i) {
            size_t at = start + u16(pointers + 2 * i);
            if (at < pointers || at >= end) broken("a cell is outside its page");
            if (kind == 0x05) {
                walk(u32(at), depth + 1, seen, out);
            } else {
                const uint64_t payload = varint(at, end);
                varint(at, end);  // the rowid
                out.push_back(record(content(at, end, payload)));
            }
        }
        if (kind == 0x05) walk(u32(header + 8), depth + 1, seen, out);
    }

    // A table leaf cell's payload: the part on its page, then the overflow pages.
    std::string content(size_t at, size_t end, uint64_t size) const {
        if (size > (uint64_t(1) << 31)) broken("a row is larger than this reader takes");
        const uint64_t most = usable_ - 35;
        uint64_t local = size;
        if (size > most) {
            const uint64_t least = (static_cast<uint64_t>(usable_ - 12) * 32 / 255) - 23;
            const uint64_t k = least + (size - least) % (usable_ - 4);
            local = k <= most ? k : least;
        }
        if (at + local > end) broken("a row runs past its page");
        std::string out = bytes_.substr(at, static_cast<size_t>(local));
        if (local == size) return out;
        uint32_t next = u32(at + static_cast<size_t>(local));
        std::set<uint32_t> seen;
        while (out.size() < size) {
            if (!next || !seen.insert(next).second) broken("an overflow chain ends early or loops");
            const size_t from = page(next);
            next = u32(from);
            const size_t piece = std::min<uint64_t>(usable_ - 4, size - out.size());
            out.append(bytes_, from + 4, piece);
        }
        return out;
    }

    // A record: its header of serial types, then the values.
    static SqliteRow record(const std::string &data) {
        SqliteRow row;
        size_t at = 0;
        const auto read_varint = [&](size_t &position, size_t end) {
            uint64_t value = 0;
            for (int i = 0; i < 9; ++i) {
                if (position >= end) broken("a record's header ends early");
                const unsigned char b = static_cast<unsigned char>(data[position++]);
                if (i == 8) return (value << 8) | b;
                value = (value << 7) | (b & 0x7f);
                if (!(b & 0x80)) return value;
            }
            return value;
        };
        const uint64_t header = read_varint(at, data.size());
        if (header > data.size()) broken("a record's header is longer than the record");
        std::vector<uint64_t> types;
        while (at < header) types.push_back(read_varint(at, static_cast<size_t>(header)));
        size_t value = static_cast<size_t>(header);
        for (const uint64_t type : types) {
            SqliteValue v;
            const auto integer = [&](size_t width) {
                if (value + width > data.size()) broken("a record ends early");
                int64_t n = static_cast<signed char>(data[value]);
                for (size_t k = 1; k < width; ++k) n = static_cast<int64_t>(static_cast<uint64_t>(n) << 8 | static_cast<unsigned char>(data[value + k]));
                value += width;
                v.kind = SqliteValue::Integer;
                v.integer = n;
            };
            if (type == 0) {
                v.kind = SqliteValue::Null;
            } else if (type >= 1 && type <= 4) {
                integer(static_cast<size_t>(type));
            } else if (type == 5) {
                integer(6);
            } else if (type == 6) {
                integer(8);
            } else if (type == 7) {
                if (value + 8 > data.size()) broken("a record ends early");
                uint64_t bits = 0;
                for (size_t k = 0; k < 8; ++k) bits = bits << 8 | static_cast<unsigned char>(data[value + k]);
                value += 8;
                v.kind = SqliteValue::Real;
                std::memcpy(&v.real, &bits, 8);
            } else if (type == 8 || type == 9) {
                v.kind = SqliteValue::Integer;
                v.integer = type == 9 ? 1 : 0;
            } else if (type >= 12) {
                const size_t length = static_cast<size_t>((type - 12) / 2);
                if (value + length > data.size()) broken("a record ends early");
                v.kind = type % 2 ? SqliteValue::Text : SqliteValue::Blob;
                v.bytes = data.substr(value, length);
                value += length;
            } else {
                broken("a record holds a value of a reserved kind");
            }
            row.push_back(std::move(v));
        }
        return row;
    }
};

}  // namespace

std::vector<SqliteRow> sqlite_rows(const std::string &bytes, const std::string &table) {
    File file(bytes);
    // sqlite_schema, whose B-tree starts at page 1: type, name, tbl_name, rootpage, sql.
    for (const SqliteRow &row : file.rows(1)) {
        if (row.size() < 4 || row[0].kind != SqliteValue::Text || row[0].bytes != "table" || row[1].kind != SqliteValue::Text ||
            row[1].bytes != table)
            continue;
        if (row[3].kind != SqliteValue::Integer || row[3].integer < 1 || row[3].integer > 0x7fffffff) broken("a table's first page is not a page");
        return file.rows(static_cast<uint32_t>(row[3].integer));
    }
    return {};
}

}  // namespace server
}  // namespace kerfkite
