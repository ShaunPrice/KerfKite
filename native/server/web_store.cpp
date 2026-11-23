// The hosted editor's store. See web_store.hpp.
#include "web_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "kerfkite/errors.hpp"
#include "os_crypto.hpp"
#include "os_random.hpp"
#include "sqlite_reader.hpp"

namespace kerfkite {
namespace server {
namespace fs = std::filesystem;

namespace {

const char *const kSaltCharacters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
const unsigned kIterations = 600000;  // OWASP's figure for PBKDF2-HMAC-SHA256 (2023)
const size_t kMaxFiles = 200;
const size_t kMaxBytes = 64 * 1024 * 1024;

double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t code_points(const std::string &text) {
    size_t count = 0;
    for (const unsigned char c : text) count += (c & 0xc0) != 0x80;
    return count;
}

std::string first_code_points(const std::string &text, size_t limit) {
    size_t count = 0, at = 0;
    for (; at < text.size(); ++at)
        if ((static_cast<unsigned char>(text[at]) & 0xc0) != 0x80) {
            if (count == limit) break;
            ++count;
        }
    return text.substr(0, at);
}

std::string ascii_lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// werkzeug.security.gen_salt: characters drawn evenly from the letters and digits.
std::string salt(size_t length) {
    std::string out;
    while (out.size() < length) {
        for (const unsigned char b : random_bytes(64)) {
            if (b >= 248) continue;  // 248 = 62 * 4: every character as likely
            out += kSaltCharacters[b % 62];
            if (out.size() == length) break;
        }
    }
    return out;
}

std::string token_hex(size_t count) { return hex(random_bytes(count)); }

std::string digest(const std::string &value) { return sha256_hex(value); }

// secrets.compare_digest.
bool same(const std::string &a, const std::string &b) { return same_secret(a, b); }

std::string read_file(const std::string &path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Written beside path, flushed to the disk, then renamed over it: a reader sees the old bytes or the new.
void write_file(const std::string &path, const std::string &bytes) {
    const std::string temporary = path + ".tmp-" + token_hex(6);
#ifdef _WIN32
    const fs::path target = fs::u8path(path), staged = fs::u8path(temporary);
    HANDLE file = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write " + path);
    bool ok = true;
    for (size_t at = 0; ok && at < bytes.size();) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - at, 1 << 30));
        ok = WriteFile(file, bytes.data() + at, chunk, &wrote, nullptr) && wrote > 0;
        at += wrote;
    }
    ok = ok && FlushFileBuffers(file);
    CloseHandle(file);
    // A virus scanner or indexer that has the file open for a moment makes the rename fail: try again briefly.
    bool moved = false;
    for (int attempt = 0; ok && !moved && attempt < 20; ++attempt) {
        moved = MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!moved) {
            const DWORD error = GetLastError();
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
            Sleep(50);
        }
    }
    if (!ok || !moved) {
        DeleteFileW(staged.c_str());
        throw std::runtime_error("Cannot write " + path);
    }
#else
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot write " + path);
    bool ok = true;
    for (size_t at = 0; ok && at < bytes.size();) {
        const ssize_t wrote = ::write(fd, bytes.data() + at, bytes.size() - at);
        ok = wrote > 0;
        if (ok) at += static_cast<size_t>(wrote);
    }
    ok = ok && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        throw std::runtime_error("Cannot write " + path);
    }
    const std::string folder = fs::path(path).parent_path().string();
    const int directory = ::open(folder.empty() ? "." : folder.c_str(), O_RDONLY | O_CLOEXEC);
    if (directory >= 0) {
        ::fsync(directory);
        ::close(directory);
    }
#endif
}

// The lock other kerfkite-web processes take too, held while it lives.
class FileLock {
public:
    explicit FileLock(const std::string &path) {
#ifdef _WIN32
        handle_ = CreateFileW(fs::u8path(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED whole{};
        if (handle_ == INVALID_HANDLE_VALUE || !LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &whole))
            throw std::runtime_error("Cannot lock the web store");
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX) != 0) throw std::runtime_error("Cannot lock the web store");
#endif
    }
    ~FileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED whole{};
            UnlockFileEx(handle_, 0, 1, 0, &whole);
            CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) ::close(fd_);  // which releases the lock
#endif
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

void private_folder(const std::string &path) {
    std::error_code error;
    fs::create_directories(fs::u8path(path), error);
    if (!fs::is_directory(fs::u8path(path))) throw std::runtime_error("Cannot create " + path);
#ifndef _WIN32
    ::chmod(path.c_str(), 0700);
#endif
}

}  // namespace

std::string web_username(const std::string &value) {
    const bool ok = !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '@' || c == '-';
    });
    if (!ok) throw ValueError("Username must be 1\xe2\x80\x93" "64 letters, digits or _.@-");
    return ascii_lower(value);
}

bool check_password(const std::string &hash, const std::string &password, bool *unverifiable) {
    if (unverifiable) *unverifiable = false;
    const size_t first = hash.find('$');
    if (first == std::string::npos) return false;
    const size_t second = hash.find('$', first + 1);
    if (second == std::string::npos) return false;
    const std::string method = hash.substr(0, first), given_salt = hash.substr(first + 1, second - first - 1), value = hash.substr(second + 1);
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
        const size_t colon = method.find(':', at);
        parts.push_back(method.substr(at, colon == std::string::npos ? std::string::npos : colon - at));
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    const auto whole = [](const std::string &text, uint64_t &out) {
        if (text.empty() || text.size() > 12 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        out = std::stoull(text);
        return true;
    };
    std::string computed;
    if (parts[0] == "pbkdf2") {
        uint64_t iterations = 1000000;  // werkzeug's default when none is written
        if (parts.size() > 3 || (parts.size() >= 2 && parts[1] != "sha256") || (parts.size() == 3 && !whole(parts[2], iterations)) || iterations == 0)
            return false;
        computed = hex(pbkdf2_sha256(password, given_salt, iterations, 32));
    } else if (parts[0] == "scrypt") {
        uint64_t n = 32768, r = 8, p = 1;
        if (parts.size() != 1 && (parts.size() != 4 || !whole(parts[1], n) || !whole(parts[2], r) || !whole(parts[3], p))) return false;
        if (n < 2 || (n & (n - 1)) || r == 0 || p == 0 || n * r > (uint64_t(1) << 26)) return false;
        std::string key;
        if (!scrypt(password, given_salt, n, r, p, 64, key)) {
            if (unverifiable) *unverifiable = true;
            return false;
        }
        computed = hex(key);
    } else {
        return false;
    }
    return same(computed, value);
}

std::string make_password_hash(const std::string &password) {
    const std::string chosen = salt(32);
    return "pbkdf2:sha256:" + std::to_string(kIterations) + "$" + chosen + "$" + hex(pbkdf2_sha256(password, chosen, kIterations, 32));
}

WebStore::WebStore(const std::string &folder) {
    root_ = fs::absolute(fs::u8path(folder)).lexically_normal().u8string();
    private_folder(root_);
    records_path_ = (fs::u8path(root_) / "web-store.json").u8string();
    lock_path_ = (fs::u8path(root_) / "web-store.lock").u8string();
    files_dir_ = (fs::u8path(root_) / "files").u8string();
    private_folder(files_dir_);
    {
        std::lock_guard<std::mutex> hold(mutex_);
        FileLock lock(lock_path_);
        const std::string database = (fs::u8path(root_) / "web.sqlite3").u8string();
        if (!fs::exists(fs::u8path(records_path_))) {
            if (fs::exists(fs::u8path(database))) migrate(database);
            else save(Records());
        }
    }
    // Unknown accounts take the same password-hash verification path.
    dummy_ = make_password_hash(token_urlsafe(32));
}

void WebStore::transaction(const std::function<bool(Records &)> &change) {
    std::lock_guard<std::mutex> hold(mutex_);
    FileLock lock(lock_path_);
    Records records = load();
    if (change(records)) save(records);
}

WebStore::Records WebStore::load() {
    const Json data = Json::parse(read_file(records_path_));
    Records records;
    for (const auto &item : data.at("users").items()) records.users[std::string(item.key())] = item.value().get<std::string>();
    for (const Json &s : data.at("sessions")) {
        WebSession session;
        session.sid = s.at("id").get<std::string>();
        session.user = s.at("user").get<std::string>();
        session.csrf = s.at("csrf").get<std::string>();
        session.expires = s.at("expires").get<double>();
        session.last_seen = s.at("last_seen").get<double>();
        records.sessions.push_back(session);
    }
    for (const auto &item : data.at("attempts").items()) {
        Attempt attempt;
        attempt.count = item.value().at("count").get<long long>();
        attempt.start = item.value().at("start").get<double>();
        records.attempts[std::string(item.key())] = attempt;
    }
    for (const Json &t : data.at("tokens")) records.tokens.push_back({t.at("id").get<std::string>(), t.at("user").get<std::string>(), t.at("created").get<double>()});
    for (const Json &f : data.at("files")) {
        WebFile file;
        file.id = f.at("id").get<std::string>();
        file.user = f.at("user").get<std::string>();
        file.name = f.at("name").get<std::string>();
        file.sha256 = f.at("sha256").get<std::string>();
        file.bytes = f.at("bytes").get<size_t>();
        file.created = f.at("created").get<double>();
        file.source = f.at("source").get<std::string>();
        records.files.push_back(file);
    }
    return records;
}

void WebStore::save(const Records &records) {
    Json data = Json::object();
    data["version"] = 1;
    Json users = Json::object();
    for (const auto &user : records.users) users[user.first] = user.second;
    data["users"] = users;
    Json sessions = Json::array();
    for (const WebSession &s : records.sessions) {
        Json row = Json::object();
        row["id"] = s.sid;
        row["user"] = s.user;
        row["csrf"] = s.csrf;
        row["expires"] = s.expires;
        row["last_seen"] = s.last_seen;
        sessions.push_back(row);
    }
    data["sessions"] = sessions;
    Json attempts = Json::object();
    for (const auto &a : records.attempts) {
        Json row = Json::object();
        row["count"] = a.second.count;
        row["start"] = a.second.start;
        attempts[a.first] = row;
    }
    data["attempts"] = attempts;
    Json tokens = Json::array();
    for (const Token &t : records.tokens) {
        Json row = Json::object();
        row["id"] = t.id;
        row["user"] = t.user;
        row["created"] = t.created;
        tokens.push_back(row);
    }
    data["tokens"] = tokens;
    Json files = Json::array();
    for (const WebFile &f : records.files) {
        Json row = Json::object();
        row["id"] = f.id;
        row["user"] = f.user;
        row["name"] = f.name;
        row["sha256"] = f.sha256;
        row["bytes"] = f.bytes;
        row["created"] = f.created;
        row["source"] = f.source;
        files.push_back(row);
    }
    data["files"] = files;
    write_file(records_path_, data.dump());
}

void WebStore::migrate(const std::string &database) {
    for (const char *suffix : {"-journal", "-wal"}) {
        std::error_code error;
        const auto size = fs::file_size(fs::u8path(database + suffix), error);
        if (!error && size > 0)
            throw std::runtime_error("web.sqlite3 has an unfinished change (web.sqlite3" + std::string(suffix) +
                                     "); finish it with the Python web server or SQLite before moving the store");
    }
    const std::string bytes = read_file(database);
    Records records;
    for (const SqliteRow &row : sqlite_rows(bytes, "users"))
        if (row.size() >= 2 && row[0].kind == SqliteValue::Text && row[1].kind == SqliteValue::Text) records.users[row[0].bytes] = row[1].bytes;
    for (const SqliteRow &row : sqlite_rows(bytes, "tokens"))
        if (row.size() >= 3 && row[0].kind == SqliteValue::Text && row[1].kind == SqliteValue::Text)
            records.tokens.push_back({row[0].bytes, row[1].bytes, row[2].number()});
    for (const SqliteRow &row : sqlite_rows(bytes, "files")) {
        if (row.size() < 7 || row[0].kind != SqliteValue::Text || row[1].kind != SqliteValue::Text || row[2].kind != SqliteValue::Text) continue;
        WebFile file;
        file.id = row[0].bytes;
        // The identifier names the file's bytes on disk: 32 hexadecimal digits, as the Python made it.
        if (file.id.size() != 32 || !std::all_of(file.id.begin(), file.id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            continue;
        file.user = row[1].bytes;
        file.name = row[2].bytes;
        const std::string &content = row[3].bytes;
        file.sha256 = row[4].kind == SqliteValue::Text ? row[4].bytes : sha256_hex(content);
        file.bytes = content.size();
        file.created = row[5].number();
        file.source = row[6].kind == SqliteValue::Text ? row[6].bytes : "web";
        write_file((fs::u8path(files_dir_) / file.id).u8string(), content);
        records.files.push_back(file);
    }
    save(records);
    std::error_code error;
    fs::rename(fs::u8path(database), fs::u8path(database + ".migrated"), error);
}

bool WebStore::has_users() {
    bool any = false;
    transaction([&](Records &r) {
        any = !r.users.empty();
        return false;
    });
    return any;
}

void WebStore::add_user(const std::string &given, const std::string &password, bool replace) {
    const std::string name = web_username(given);
    if (code_points(password) < 15 || code_points(password) > 256)
        throw ValueError("Choose a password of 15\xe2\x80\x93" "256 characters or use the secure generator");
    const std::string hashed = make_password_hash(password);
    transaction([&](Records &r) {
        if (replace) {
            if (!r.users.count(name)) throw ValueError("Unknown user");
            r.users[name] = hashed;
            r.sessions.erase(std::remove_if(r.sessions.begin(), r.sessions.end(), [&](const WebSession &s) { return s.user == name; }), r.sessions.end());
            r.tokens.erase(std::remove_if(r.tokens.begin(), r.tokens.end(), [&](const Token &t) { return t.user == name; }), r.tokens.end());
        } else {
            if (r.users.count(name)) throw ValueError("User already exists; use reset-password");
            r.users[name] = hashed;
        }
        return true;
    });
}

LoginResult WebStore::check_login(const std::string &given, const std::string &password, const std::string &address) {
    LoginResult result;
    const double start = now();
    const std::string name = ascii_lower(given);
    const std::vector<std::string> keys = {"ip:" + address, "user:" + first_code_points(name, 64)};
    std::string stored;
    bool known = false;
    // An attempt is reserved before hashing, so concurrent requests cannot pass the limit.
    transaction([&](Records &r) {
        for (auto it = r.attempts.begin(); it != r.attempts.end();)
            it = it->second.start < start - 900 ? r.attempts.erase(it) : std::next(it);
        for (const std::string &key : keys) {
            const auto found = r.attempts.find(key);
            if (found != r.attempts.end() && found->second.count >= 10) {
                result.limited = true;
                return true;
            }
        }
        for (const std::string &key : keys) {
            auto found = r.attempts.find(key);
            if (found == r.attempts.end()) r.attempts[key] = Attempt{1, start};
            else found->second.count += 1;
        }
        const auto user = r.users.find(name);
        known = user != r.users.end();
        if (known) stored = user->second;
        return true;
    });
    if (result.limited) return result;
    const std::string offered = code_points(password) > 256 ? std::string() : password;
    bool unverifiable = false;
    const bool valid = check_password(known ? stored : dummy_, offered, &unverifiable);
    if (unverifiable)
        std::fprintf(stderr, "kerfkite-web: the password of %s cannot be checked here (scrypt); set it again with reset-password\n", name.c_str());
    if (!known || !valid) return result;
    const std::string sid = token_urlsafe(32), csrf = token_urlsafe(32);
    // A hash of other parameters (the Python's scrypt, which only some systems can check, or fewer rounds) is
    // replaced by one of this store's, now that the password is known.
    const std::string current = "pbkdf2:sha256:" + std::to_string(kIterations) + "$";
    const std::string upgraded = stored.compare(0, current.size(), current) == 0 ? std::string() : make_password_hash(password);
    transaction([&](Records &r) {
        const auto user = r.users.find(name);
        if (user == r.users.end() || user->second != stored) return false;
        if (!upgraded.empty()) user->second = upgraded;
        const double later = now();
        r.sessions.erase(std::remove_if(r.sessions.begin(), r.sessions.end(),
                                        [&](const WebSession &s) { return s.expires < later || s.last_seen < later - 1800; }),
                         r.sessions.end());
        WebSession session;
        session.sid = digest(sid);
        session.user = name;
        session.csrf = csrf;
        session.expires = later + 8 * 3600;
        session.last_seen = later;
        r.sessions.push_back(session);
        // Only this successful reservation is released; unrelated failed attempts stay.
        const auto ip = r.attempts.find("ip:" + address);
        if (ip != r.attempts.end()) ip->second.count = std::max<long long>(0, ip->second.count - 1);
        r.attempts.erase("user:" + name);
        result.ok = true;
        result.session.sid = sid;
        result.session.csrf = csrf;
        result.session.user = name;
        return true;
    });
    return result;
}

bool WebStore::session(const std::string &sid, WebSession &out) {
    if (sid.size() > 128) return false;
    const std::string key = digest(sid);
    const double moment = now();
    bool found = false;
    transaction([&](Records &r) {
        for (WebSession &s : r.sessions)
            if (s.sid == key && s.expires > moment && s.last_seen > moment - 1800) {
                out = s;
                found = true;
                // The last use is written at most every 30 seconds, not on every request: the idle limit of 30
                // minutes may come that much sooner.
                if (moment - s.last_seen < 30) return false;
                s.last_seen = moment;
                out.last_seen = moment;
                return true;
            }
        return false;
    });
    return found;
}

void WebStore::logout(const std::string &sid) {
    const std::string key = digest(sid);
    transaction([&](Records &r) {
        const size_t before = r.sessions.size();
        r.sessions.erase(std::remove_if(r.sessions.begin(), r.sessions.end(), [&](const WebSession &s) { return s.sid == key; }), r.sessions.end());
        return r.sessions.size() != before;
    });
}

std::string WebStore::issue_token(const std::string &given) {
    const std::string name = web_username(given);
    const std::string token = token_urlsafe(32);
    transaction([&](Records &r) {
        if (!r.users.count(name)) throw ValueError("Unknown user");
        r.tokens.push_back({digest(token), name, now()});
        return true;
    });
    return token;
}

std::string WebStore::token_user(const std::string &token) {
    if (token.size() > 128) return std::string();
    const std::string key = digest(token);
    std::string user;
    transaction([&](Records &r) {
        for (const Token &t : r.tokens)
            if (t.id == key) user = t.user;
        return false;
    });
    return user;
}

void WebStore::revoke_tokens(const std::string &given) {
    const std::string name = web_username(given);
    transaction([&](Records &r) {
        r.tokens.erase(std::remove_if(r.tokens.begin(), r.tokens.end(), [&](const Token &t) { return t.user == name; }), r.tokens.end());
        return true;
    });
}

Json WebStore::put_file(const std::string &user, const std::string &name, const std::string &content, const std::string &source) {
    WebFile file;
    file.id = token_hex(16);
    file.user = user;
    file.name = name;
    file.sha256 = sha256_hex(content);
    file.bytes = content.size();
    file.created = now();
    file.source = source;
    transaction([&](Records &r) {
        size_t count = 0, size = 0;
        for (const WebFile &f : r.files)
            if (f.user == user) {
                ++count;
                size += f.bytes;
            }
        if (count >= kMaxFiles || size + content.size() > kMaxBytes) throw ValueError("Account file quota reached; download and delete unused files");
        write_file((fs::u8path(files_dir_) / file.id).u8string(), content);
        r.files.push_back(file);
        return true;
    });
    Json out = Json::object();
    out["id"] = file.id;
    out["name"] = name;
    out["sha256"] = file.sha256;
    out["bytes"] = file.bytes;
    out["status"] = "stored_for_review";
    return out;
}

Json WebStore::files(const std::string &user) {
    std::vector<WebFile> mine;
    transaction([&](Records &r) {
        for (const WebFile &f : r.files)
            if (f.user == user) mine.push_back(f);
        return false;
    });
    std::stable_sort(mine.begin(), mine.end(), [](const WebFile &a, const WebFile &b) { return a.created > b.created; });
    Json out = Json::array();
    for (const WebFile &f : mine) {
        Json row = Json::object();
        row["id"] = f.id;
        row["name"] = f.name;
        row["sha256"] = f.sha256;
        row["bytes"] = f.bytes;
        row["created"] = f.created;
        row["source"] = f.source;
        out.push_back(row);
    }
    return out;
}

bool WebStore::file(const std::string &user, const std::string &id, WebFile &record, std::string &content) {
    bool found = false;
    transaction([&](Records &r) {
        for (const WebFile &f : r.files)
            if (f.user == user && f.id == id) {
                record = f;
                content = read_file((fs::u8path(files_dir_) / f.id).u8string());
                found = true;
            }
        return false;
    });
    return found;
}

void WebStore::delete_file(const std::string &user, const std::string &id) {
    bool removed = false;
    transaction([&](Records &r) {
        const size_t before = r.files.size();
        r.files.erase(std::remove_if(r.files.begin(), r.files.end(), [&](const WebFile &f) { return f.user == user && f.id == id; }), r.files.end());
        removed = r.files.size() != before;
        return removed;
    });
    // The bytes go once no record names them.
    if (removed) {
        std::error_code error;
        fs::remove(fs::u8path(files_dir_) / id, error);
    }
}

std::vector<std::string> WebStore::unverifiable_accounts() {
    std::vector<std::string> names;
    std::string probe;
    const bool have_scrypt = scrypt("x", "y", 2, 1, 1, 16, probe);
    transaction([&](Records &r) {
        if (!have_scrypt)
            for (const auto &user : r.users)
                if (user.second.compare(0, 7, "scrypt:") == 0 || user.second.compare(0, 7, "scrypt$") == 0) names.push_back(user.first);
        return false;
    });
    return names;
}

}  // namespace server
}  // namespace kerfkite
