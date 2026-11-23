// The rows of one table of an SQLite 3 database file, read from the file format alone (the SQLite documentation's
// "Database File Format"), with no SQLite library: for moving the Python hosted editor's accounts, upload tokens
// and stored files (web.sqlite3) to the C++ server's store. Read only; UTF-8 databases, as Python's sqlite3 makes
// them; rowid tables, as the Python's CREATE TABLE statements make them.
#ifndef KERFKITE_SERVER_SQLITE_READER_HPP
#define KERFKITE_SERVER_SQLITE_READER_HPP

#include <string>
#include <vector>

namespace kerfkite {
namespace server {

struct SqliteValue {
    enum Kind { Null, Integer, Real, Text, Blob } kind = Null;
    long long integer = 0;
    double real = 0;
    std::string bytes;  // Text (UTF-8) and Blob
    // The value as a number: an integer's, a real's, or 0.
    double number() const { return kind == Integer ? static_cast<double>(integer) : kind == Real ? real : 0.0; }
};

using SqliteRow = std::vector<SqliteValue>;

// The rows of the table named, in rowid order, each value in the order of the table's columns. A table that is
// not there has no rows. Throws std::runtime_error for a file this reader cannot read.
std::vector<SqliteRow> sqlite_rows(const std::string &file, const std::string &table);

}  // namespace server
}  // namespace kerfkite

#endif
