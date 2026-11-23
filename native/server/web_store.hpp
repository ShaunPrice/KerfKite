// The hosted editor's accounts, revocable sessions, upload tokens and private files, as studio/web_store.py keeps
// them, in a folder of the operator's: web-store.json (the records) and files/ (each stored file's bytes), both
// written whole and renamed into place, under a lock that other kerfkite-web processes (creating a user while the
// server runs, say) take too. Sessions and upload tokens are kept as SHA-256 digests, passwords as PBKDF2 hashes
// (werkzeug's format, so the Python's hashes are read as they are), the files as given.
//
// A folder that holds the Python's web.sqlite3 and no store yet is moved over the first time: the accounts with
// their password hashes, the upload tokens and the files (sessions and failed logins are not kept, so everyone
// logs in again). The database is then renamed web.sqlite3.migrated. The Python's scrypt hashes are checked with
// the system's scrypt where there is one (OpenSSL's libcrypto); a login that succeeds replaces the hash with a
// PBKDF2 one. Where there is none (Windows, macOS), such an account's password must be set again with
// kerfkite-web reset-password.
#ifndef KERFKITE_SERVER_WEB_STORE_HPP
#define KERFKITE_SERVER_WEB_STORE_HPP

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

struct WebSession {
    std::string sid, csrf, user;  // sid only when made by check_login
    double expires = 0, last_seen = 0;
};

struct WebFile {
    std::string id, user, name, sha256, source;
    double created = 0;
    size_t bytes = 0;
};

struct LoginResult {
    bool ok = false, limited = false;
    WebSession session;
};

// web_store.username: 1 to 64 letters, digits or _.@-, in lower case; throws ValueError otherwise.
std::string web_username(const std::string &value);

// werkzeug.security.check_password_hash for the methods Studio writes (pbkdf2 and scrypt); unverifiable is set
// where the hash is scrypt and the system has no scrypt.
bool check_password(const std::string &hash, const std::string &password, bool *unverifiable = nullptr);
// A new hash: werkzeug's pbkdf2:sha256 format, 600,000 iterations, a 32-character salt.
std::string make_password_hash(const std::string &password);

class WebStore {
public:
    explicit WebStore(const std::string &folder);

    const std::string &root() const { return root_; }
    bool has_users();
    // add_user(name, password, replace): refused with web_store.py's messages.
    void add_user(const std::string &name, const std::string &password, bool replace);
    // check_login(name, password, address): a session, or not, and whether the attempt was refused as too many.
    LoginResult check_login(const std::string &name, const std::string &password, const std::string &address);
    // The live session of a cookie's value, its last use moved on; false for none.
    bool session(const std::string &sid, WebSession &out);
    void logout(const std::string &sid);
    std::string issue_token(const std::string &name);
    // The user of an upload token, or empty.
    std::string token_user(const std::string &token);
    void revoke_tokens(const std::string &name);
    // put_file: the record as the route answers it.
    Json put_file(const std::string &user, const std::string &name, const std::string &content, const std::string &source);
    // files(user), newest first, as the route answers it.
    Json files(const std::string &user);
    // A file of the user's, with its bytes; false when there is none.
    bool file(const std::string &user, const std::string &id, WebFile &record, std::string &content);
    void delete_file(const std::string &user, const std::string &id);

    // Accounts whose password hashes this system cannot check (scrypt without libcrypto's): their names.
    std::vector<std::string> unverifiable_accounts();

private:
    struct Attempt {
        long long count = 0;
        double start = 0;
    };
    struct Token {
        std::string id, user;
        double created = 0;
    };
    struct Records {
        std::map<std::string, std::string> users;  // name -> password hash
        std::vector<WebSession> sessions;           // sid holds the digest
        std::map<std::string, Attempt> attempts;
        std::vector<Token> tokens;
        std::vector<WebFile> files;
    };

    std::string root_, records_path_, lock_path_, files_dir_;
    std::string dummy_;
    std::mutex mutex_;

    // Runs change on the records under the lock, and writes them back when it returns true.
    void transaction(const std::function<bool(Records &)> &change);
    Records load();
    void save(const Records &records);
    void migrate(const std::string &database);
};

}  // namespace server
}  // namespace kerfkite

#endif
