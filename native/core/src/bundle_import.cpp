// The text members of a LightBurn user bundle (a ZIP archive), read in memory as studio/import_bundle.py reads
// them, to the bit. The archive is read as Python 3.13's zipfile reads it: the end record found as _EndRecData
// finds it, with the ZIP64 records as _EndRecData64 reads them, the central directory walked as _RealGetContents
// walks it (names in code page 437 or, with flag bit 11, strict UTF-8; cut at a NUL, and on Windows with
// backslashes as slashes; the ZIP64 and Info-ZIP Unicode path fields as _decodeExtra reads them), a member
// opened as ZipFile.open opens it (the local header's name compared, entries that overrun the next one refused,
// and entries that share a header read) and read as ZipExtFile.read(4000001) reads it (stored bytes, or raw
// DEFLATE data inflated as zlib inflates it, cut at the directory's size and checked against its CRC-32). The
// member's text is decoded as UTF-16 after a byte-order mark, and otherwise as UTF-8 with an optional signature.
//
// The core's own answers, where zipfile stops with an error that is not one of its read errors (NotImplementedError
// for an entry that needs a ZIP version above 6.3, for patched data and for strong encryption; ValueError for a
// header before the start of the data) or where Python versions read differently (DEFLATE data past the 4 MB a
// member may hold): such an archive cannot be read, and data past the limit is not looked at. Written from the
// ZIP application note and zipfile's documented behaviour.
#include "kerfkite/bundle_import.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "base64.hpp"
#include "inflate.hpp"
#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "project.hpp"
#include "python_text.hpp"

namespace kerfkite {
namespace {

using json = Json;
using Bytes = std::vector<unsigned char>;
using Text = std::vector<uint32_t>;
using detail::get;

const size_t kMaxArchive = 4000000;
const size_t kMaxMember = 4000000;
const uint64_t kMaxTotal = 16000000;
const size_t kMaxMembers = 256;
const size_t kRead = kMaxMember + 1;  // what inspect_bundle asks a member for

// What zipfile raises and inspect_bundle reports with one message: BadZipFile, UnicodeError, OSError, EOFError
// and zlib.error.
struct Unreadable {};

// A position in the archive as zipfile computes it, with Python's integers: a ZIP64 record can give offsets near
// 2**64 that the adjustment for data before the archive brings back into the file. Two's complement in 128 bits,
// more than any sum of these fields needs.
struct Position {
    uint64_t high = 0, low = 0;
    static Position of(uint64_t value) { return Position{0, value}; }
    Position operator+(const Position &o) const {
        Position r;
        r.low = low + o.low;
        r.high = high + o.high + (r.low < low ? 1u : 0u);
        return r;
    }
    Position operator-(const Position &o) const {
        Position r;
        r.low = low - o.low;
        r.high = high - o.high - (low < o.low ? 1u : 0u);
        return r;
    }
    bool negative() const { return (high >> 63) != 0; }
    bool operator<(const Position &o) const {
        const uint64_t sign = uint64_t(1) << 63;
        return high != o.high ? (high ^ sign) < (o.high ^ sign) : low < o.low;
    }
    bool operator==(const Position &o) const { return high == o.high && low == o.low; }
    // The value where it is from 0 to limit.
    bool within(uint64_t limit, uint64_t &out) const {
        if (high != 0 || low > limit) return false;
        out = low;
        return true;
    }
};

// Code page 437's characters for bytes 0x80 to 0xFF; the lower half is ASCII.
const uint16_t kCodePage437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

// A name as zipfile decodes it: strict UTF-8 with flag bit 11, code page 437 otherwise.
bool decode_name(const Bytes &raw, bool utf8, Text &out) {
    if (utf8) return detail::python_utf8(raw.data(), raw.size(), out);
    out.clear();
    for (const unsigned char c : raw) out.push_back(c < 0x80 ? c : kCodePage437[c - 0x80]);
    return true;
}

uint32_t u16(const unsigned char *p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
uint32_t u32(const unsigned char *p) { return u16(p) | u16(p + 2) << 16; }
uint64_t u64(const unsigned char *p) { return static_cast<uint64_t>(u32(p)) | static_cast<uint64_t>(u32(p + 4)) << 32; }

bool signature(const Bytes &data, size_t at, unsigned char kind, unsigned char part) {
    return at + 4 <= data.size() && data[at] == 'P' && data[at + 1] == 'K' && data[at + 2] == kind && data[at + 3] == part;
}

std::string utf8(const Text &text) {
    std::string out;
    for (const uint32_t c : text) detail::append_utf8(out, c);
    return out;
}

// One central directory entry, as zipfile's ZipInfo holds it.
struct Member {
    Text original;  // orig_filename
    Text name;      // filename: cut at the first NUL, and on Windows with backslashes as slashes
    unsigned flags = 0, method = 0;
    uint32_t crc = 0, external = 0;
    uint64_t compressed = 0, size = 0;  // with a ZIP64 field, 64 bits
    Position header;  // the local header's offset, with any data before the archive
    Position end;     // _end_offset: the next header by offset, or the central directory
};

// _sanitize_filename: cut at the first NUL, and on Windows with backslashes as slashes.
Text sanitized(const Text &name) {
    Text out;
    for (const uint32_t ch : name) {
        if (ch == 0) break;
#ifdef _WIN32
        out.push_back(ch == '\\' ? '/' : ch);
#else
        out.push_back(ch);
#endif
    }
    return out;
}

// The components of PurePosixPath(name): split at slashes, without empty and "." parts.
std::vector<Text> parts(const Text &name) {
    std::vector<Text> out;
    Text part;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            if (!part.empty() && !(part.size() == 1 && part[0] == '.')) out.push_back(part);
            part.clear();
        } else {
            part.push_back(name[i]);
        }
    }
    return out;
}

// PurePosixPath(name).name.
Text last_part(const Text &name) {
    std::vector<Text> all = parts(name);
    return all.empty() ? Text() : all.back();
}

bool contains(const Text &text, uint32_t c) { return std::find(text.begin(), text.end(), c) != text.end(); }

// _eligible: a regular, unencrypted, stored or deflated member with a relative, printable name and a LightBurn
// suffix.
bool eligible(const Member &m) {
    const std::vector<Text> components = parts(m.name);
    if (!m.name.empty() && m.name[0] == '/') return false;
    for (const Text &part : components)
        if (part.size() == 2 && part[0] == '.' && part[1] == '.') return false;
    if (contains(m.name, '\\') || m.name.size() > 512 || contains(m.name, ':')) return false;
    for (const uint32_t c : m.original)
        if (!detail::python_printable(c)) return false;
    // PurePath.suffix: from the last dot of the last part, when it is neither the first nor the last character.
    const Text last = components.empty() ? Text() : components.back();
    std::string suffix;
    for (size_t i = last.size(); i-- > 0;) {
        if (last[i] != '.') continue;
        if (i > 0 && i + 1 < last.size())
            for (size_t k = i; k < last.size(); ++k) {
                // No character outside ASCII lowers to a letter of these suffixes.
                if (last[k] >= 0x80) return false;
                suffix += static_cast<char>(last[k] >= 'A' && last[k] <= 'Z' ? last[k] + 32 : last[k]);
            }
        break;
    }
    static const char *const kSuffixes[] = {".lbdev", ".lbprefs", ".clb", ".lbrn", ".lbrn2", ".lbset"};
    if (std::find(std::begin(kSuffixes), std::end(kSuffixes), suffix) == std::end(kSuffixes)) return false;
    if (!m.name.empty() && m.name.back() == '/') return false;  // is_dir()
    const uint32_t kind = (m.external >> 16) & 0170000;
    return (kind == 0 || kind == 0100000) && !(m.flags & 1) && (m.method == 0 || m.method == 8);
}

// The central directory, as _EndRecData, _EndRecData64 and _RealGetContents read it.
std::vector<Member> directory(const Bytes &data) {
    const size_t filesize = data.size();
    size_t location = 0;
    if (filesize >= 22 && signature(data, filesize - 22, 5, 6) && data[filesize - 2] == 0 && data[filesize - 1] == 0) {
        location = filesize - 22;
    } else {
        // The last signature in the final 64 KiB comment and record; a record cut short is not read.
        const size_t from = filesize > 65535 + 22 ? filesize - 65535 - 22 : 0;
        bool found = false;
        if (filesize >= 4)
            for (size_t at = filesize - 4;; --at) {
                if (signature(data, at, 5, 6)) {
                    found = true;
                    location = at;
                    break;
                }
                if (at == from) break;
            }
        if (!found || filesize - location < 22) throw Unreadable();
    }
    uint64_t size_cd = u32(&data[location + 12]), offset_cd = u32(&data[location + 16]);
    // The ZIP64 locator just before the record, where there is room for one, and the ZIP64 record it points to:
    // at its stated offset, or where it would be with data before the archive and no extensible data.
    if (location >= 20 && signature(data, location - 20, 6, 7)) {
        const unsigned char *locator = &data[location - 20];
        const uint64_t stated = u64(locator + 8);
        if (u32(locator + 4) != 0 || u32(locator + 16) > 1) throw Unreadable();  // spans disks
        if (location < 76 || stated > location - 76) throw Unreadable();
        const size_t offset = location - 76;
        size_t at = static_cast<size_t>(stated);
        size_t extra = offset - at;
        if (!signature(data, at, 6, 6) && at != offset) {
            at = offset;
            extra = 0;
        }
        if (!signature(data, at, 6, 6)) throw Unreadable();
        const unsigned char *record = &data[at];
        const uint64_t record_size = u64(record + 4), dir_size = u64(record + 40), dir_offset = u64(record + 48);
        // diroffset + dirsize == reloff and sz + 12 == 56 + extrasz, without overflow.
        if (dir_offset > stated || dir_size != stated - dir_offset || record_size != 44u + extra) throw Unreadable();
        size_cd = dir_size;
        offset_cd = dir_offset;
        location = at;
    }
    // concat = location - size_cd - offset_cd, and the directory starts at offset_cd + concat.
    if (size_cd > location) throw Unreadable();
    const size_t start = location - static_cast<size_t>(size_cd);
    const Position concat = Position::of(location) - Position::of(size_cd) - Position::of(offset_cd);
    const size_t stop = location;
    size_t at = start;
    const auto read = [&](size_t count) {
        const size_t n = std::min(count, stop - at);
        Bytes out(data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(at + n));
        at += n;
        return out;
    };
    std::vector<Member> members;
    uint64_t total = 0;
    while (total < size_cd) {
        if (stop - at < 46) throw Unreadable();
        const Bytes entry = read(46);
        const unsigned char *c = entry.data();
        if (!(c[0] == 'P' && c[1] == 'K' && c[2] == 1 && c[3] == 2)) throw Unreadable();
        const unsigned extract_version = c[6];
        Member m;
        m.flags = u16(c + 8);
        m.method = u16(c + 10);
        m.crc = u32(c + 16);
        m.compressed = u32(c + 20);
        m.size = u32(c + 24);
        const uint32_t name_length = u16(c + 28), extra_length = u16(c + 30), comment_length = u16(c + 32);
        m.external = u32(c + 38);
        uint64_t header = u32(c + 42);
        const Bytes raw_name = read(name_length);
        const uint32_t name_crc = detail::crc32(raw_name.data(), raw_name.size());
        if (!decode_name(raw_name, m.flags & 0x800, m.original)) throw Unreadable();
        m.name = sanitized(m.original);
        const Bytes extra = read(extra_length);
        read(comment_length);
        // zipfile raises NotImplementedError for a newer entry; the archive cannot be read.
        if (extract_version > 63) throw Unreadable();
        for (size_t i = 0; extra.size() - i >= 4;) {
            const uint32_t type = u16(&extra[i]), length = u16(&extra[i + 2]);
            if (length + 4 > extra.size() - i) throw Unreadable();
            const unsigned char *field = &extra[i + 4];
            if (type == 0x0001) {
                // ZIP64: the sizes and the offset that the entry leaves at their largest, in that order.
                size_t left = length;
                const auto next = [&](uint64_t &value) {
                    if (left < 8) throw Unreadable();
                    value = u64(field);
                    field += 8;
                    left -= 8;
                };
                if (m.size == 0xFFFFFFFFu || m.size == 0xFFFFFFFFFFFFFFFFull) next(m.size);
                if (m.compressed == 0xFFFFFFFFu) next(m.compressed);
                if (header == 0xFFFFFFFFu) next(header);
            } else if (type == 0x7075) {
                // Info-ZIP Unicode path: version 1 and the CRC-32 of the name it stands for.
                if (length < 5) throw Unreadable();
                if (field[0] == 1 && u32(field + 1) == name_crc) {
                    Text unicode;
                    if (!detail::python_utf8(field + 5, length - 5, unicode)) throw Unreadable();
                    if (!unicode.empty()) m.name = sanitized(unicode);
                }
            }
            i += length + 4;
        }
        m.header = Position::of(header) + concat;
        members.push_back(m);
        total += 46ull + name_length + extra_length + comment_length;
    }
    std::vector<size_t> order(members.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return members[a].header < members[b].header; });
    Position end = Position::of(start);
    for (size_t k = order.size(); k-- > 0;) {
        members[order[k]].end = end;
        end = members[order[k]].header;
    }
    return members;
}

// The member's bytes as ZipFile.open(member).read(4000001) gives them.
Bytes member_bytes(const Bytes &data, const Member &m) {
    const size_t filesize = data.size();
    // A header before the data is a seek zipfile cannot make (ValueError), one past it a truncated header.
    uint64_t offset = 0;
    if (!m.header.within(filesize, offset)) throw Unreadable();
    size_t pos = static_cast<size_t>(offset);
    const auto take = [&](size_t count) {
        const size_t n = pos < filesize ? std::min(count, filesize - pos) : 0;
        Bytes out(data.begin() + static_cast<long>(pos), data.begin() + static_cast<long>(pos + n));
        pos += n;
        return out;
    };
    const Bytes header = take(30);
    if (header.size() != 30 || !signature(header, 0, 3, 4)) throw Unreadable();
    const uint32_t local_flags = u16(&header[6]), name_length = u16(&header[26]), extra_length = u16(&header[28]);
    const Bytes name = take(name_length);
    pos += extra_length;
    // Patched data and strong encryption, for which zipfile raises NotImplementedError: not readable here.
    if (m.flags & 0x60) throw Unreadable();
    Text local;
    if (!decode_name(name, local_flags & 0x800, local) || local != m.original) throw Unreadable();
    // An entry that overruns the next is refused; one that shares its header with another is read (zipfile warns).
    if (m.end < Position::of(pos) + Position::of(m.compressed) && !(m.end == m.header)) throw Unreadable();
    // The first read takes all the compressed data there is; a second, needed when that falls short, finds none.
    const size_t available = pos < filesize ? static_cast<size_t>(std::min<uint64_t>(m.compressed, filesize - pos)) : 0;
    if (m.compressed && !available) throw Unreadable();
    const Bytes compressed(data.begin() + static_cast<long>(std::min(pos, filesize)),
                           data.begin() + static_cast<long>(std::min(pos, filesize) + available));
    Bytes out;
    bool ended = true;
    if (m.method == 8) {
        // zlib is asked for the 4000001 bytes read() wants. Data that inflates further is cut there, as zlib
        // stops, and nothing after it is looked at: a member holds at most 4 MB, so the cut is never short.
        const detail::Inflated result = detail::raw_inflate(compressed.data(), compressed.size(), kRead, out, ended);
        if (result == detail::Inflated::Broken) throw Unreadable();
        if (result == detail::Inflated::TooLong) ended = false;
    } else {
        out = compressed;
        ended = false;
    }
    bool eof = ended || available == m.compressed;
    if (out.size() >= m.size) {
        out.resize(static_cast<size_t>(m.size));
        eof = true;
    }
    if (!eof || detail::crc32(out.data(), out.size()) != m.crc) throw Unreadable();
    return out;
}

// raw.decode('utf-16') after a byte-order mark: false where Python raises UnicodeDecodeError.
bool utf16(const Bytes &raw, Text &out) {
    out.clear();
    if (raw.size() % 2) return false;
    const bool little = raw[0] == 0xff;
    std::vector<uint32_t> units;
    for (size_t i = 2; i < raw.size(); i += 2)
        units.push_back(little ? u16(&raw[i]) : (static_cast<uint32_t>(raw[i]) << 8 | raw[i + 1]));
    for (size_t i = 0; i < units.size(); ++i) {
        const uint32_t u = units[i];
        if (u >= 0xdc00 && u <= 0xdfff) return false;
        if (u >= 0xd800 && u <= 0xdbff) {
            if (i + 1 == units.size() || units[i + 1] < 0xdc00 || units[i + 1] > 0xdfff) return false;
            out.push_back(0x10000 + ((u - 0xd800) << 10) + (units[i + 1] - 0xdc00));
            ++i;
        } else {
            out.push_back(u);
        }
    }
    return true;
}

}  // namespace

Json inspect_bundle(const Json &payload) {
    const json *content = payload.is_object() ? get(payload, "content") : nullptr;
    if (!content || !content->is_string()) throw ValueError("Provide bundle content in base64");
    const std::string &encoded = content->get_ref<const std::string &>();
    if (detail::code_points(encoded).size() > 4 * ((kMaxArchive + 2) / 3)) throw ValueError("Bundle exceeds the 4 MB archive limit");
    Bytes data;
    if (!detail::base64(encoded, data)) throw ValueError("Bundle encoding is invalid");
    if (data.empty() || data.size() > kMaxArchive) throw ValueError("Choose a bundle of up to 4 MB");
    try {
        const std::vector<Member> members = directory(data);
        // The sum as Python's integers make it: past the limit is all the check needs to know.
        uint64_t total = 0;
        for (const Member &m : members)
            if (total <= kMaxTotal) total = m.size > kMaxTotal - total ? kMaxTotal + 1 : total + m.size;
        if (members.size() > kMaxMembers || total > kMaxTotal) throw ValueError("Bundle exceeds 256 members or 16 MB uncompressed");
        std::vector<size_t> accepted;
        for (size_t i = 0; i < members.size(); ++i)
            if (eligible(members[i])) accepted.push_back(i);
        for (const size_t i : accepted)
            if (members[i].size > kMaxMember) throw ValueError("Bundle contains a supported file larger than 4 MB");
        if (!payload.contains("index")) {
            json files = json::array();
            for (const size_t i : accepted) {
                json file = json::object();
                file["index"] = static_cast<long long>(i);
                file["name"] = utf8(last_part(members[i].name));
                files.push_back(file);
            }
            long long skipped = 0;
            for (size_t i = 0; i < members.size(); ++i)
                if (!std::binary_search(accepted.begin(), accepted.end(), i) && !(!members[i].name.empty() && members[i].name.back() == '/'))
                    ++skipped;
            json result = json::object();
            result["files"] = files;
            result["skipped"] = skipped;
            return result;
        }
        const json &index = *get(payload, "index");
        long long chosen = -1;
        if (index.is_number_unsigned()) {
            const uint64_t value = index.get<uint64_t>();
            if (value < members.size()) chosen = static_cast<long long>(value);
        } else if (index.is_number_integer()) {
            chosen = index.get<long long>();
        }
        if (!index.is_number_integer() || chosen < 0 ||
            !std::binary_search(accepted.begin(), accepted.end(), static_cast<size_t>(chosen)))
            throw ValueError("Select a supported member of this bundle");
        const Member &m = members[static_cast<size_t>(chosen)];
        const Bytes raw = member_bytes(data, m);
        if (raw.size() != m.size || raw.size() > kMaxMember) throw ValueError("Bundle member size mismatch");
        Text text;
        if (raw.size() >= 2 && ((raw[0] == 0xff && raw[1] == 0xfe) || (raw[0] == 0xfe && raw[1] == 0xff))) {
            if (!utf16(raw, text)) throw Unreadable();
        } else {
            const size_t skip = raw.size() >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf ? 3 : 0;
            if (!detail::python_utf8(raw.data() + skip, raw.size() - skip, text)) throw Unreadable();
        }
        if (contains(text, 0)) throw ValueError("Selected bundle member is binary");
        json result = json::object();
        result["name"] = utf8(last_part(m.name));
        result["text"] = utf8(text);
        return result;
    } catch (const Unreadable &) {
        throw ValueError("Cannot read the bundle or selected text member");
    }
}

}  // namespace kerfkite
