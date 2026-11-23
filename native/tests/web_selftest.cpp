// Checks of the C++ hosted editor (kerfkite-web), run by ctest: the system's hashes against werkzeug's and its RSA
// signatures (the assistant server's RS256 tokens), the SQLite reader and the move of the Python store (a synthetic
// web.sqlite3 the Python wrote, from validation/web_store_fixture.py), the store's accounts, sessions, limits,
// tokens and files, the uploads' and receivers' checks, every route of the app through its request checks, the
// PROXY protocol line of the HTTP server, and, with the TLS relay, a file sent to a receiver.
//
//     kerfkite_web_selftest STATIC_DIR FIXTURE_DIR [TLS_RELAY|-]
//
// Everything is written under a new folder in the system's temporary folder; nothing listens beyond 127.0.0.1.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "http.hpp"
#include "kerfkite/errors.hpp"
#include "kerfkite/json.hpp"
#include "os_crypto.hpp"
#include "os_random.hpp"
#include "sqlite_reader.hpp"
#include "web_app.hpp"
#include "web_store.hpp"
#include "web_transfer.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using kerfkite::Json;
using namespace kerfkite::server;
namespace http = kerfkite::http;
namespace fs = std::filesystem;

namespace {

int failures = 0, checks = 0;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        ++checks;                                                                              \
        if (!(condition)) {                                                                    \
            ++failures;                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                                      \
    } while (0)

void check_text(const char *file, int line, const std::string &whole, const std::string &part) {
    ++checks;
    if (whole.find(part) == std::string::npos) {
        ++failures;
        std::fprintf(stderr, "%s:%d: \"%s\" does not contain \"%s\"\n", file, line, whole.substr(0, 400).c_str(), part.c_str());
    }
}
#define CHECK_TEXT(text, part) check_text(__FILE__, __LINE__, (text), (part))

std::string static_dir, fixture_dir, relay_program;
const char *const kToken = "kerfkite-selftest-upload-token-0123456789abcdefghij";
const char *const kAlice = "correct horse battery staple";
const char *const kBob = "bob's long test password";

template <typename F> std::string refusal(F f) {
    try {
        f();
    } catch (const std::exception &e) {
        return e.what();
    }
    return std::string();
}

std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A folder of its own for each check, removed afterwards.
struct Folder {
    fs::path path;
    Folder() {
        path = fs::temp_directory_path() / ("kerfkite-web-selftest-" + hex(random_bytes(8)));
        fs::create_directories(path);
    }
    ~Folder() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    std::string str() const { return path.u8string(); }
};

// RS256 signatures, as the assistant server's tokens are checked: synthetic keys made for this test, with signatures
// of one message by their private halves (since discarded).
std::string unhex(const std::string &digits) {
    std::string out;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) out += static_cast<char>(std::stoi(digits.substr(i, 2), nullptr, 16));
    return out;
}

void signatures() {
    struct Vector {
        const char *modulus, *exponent, *signature;
    };
    const Vector vectors[] = {
        {// 2048 bits
         "9c58eb566d62669259d1d8a09776c184e51246bd13e50441b73e6790123c524b10c2a436edbaf22ecfab7efc6e12ef986aff3392eb5b405e5ac8aa457312f74a"
         "58a22e671f9be2158b2118587cf687ee7f02a9fff8fafdf9b7b560daad41ecf67b09bea4ec9f2c199b5289ac0e58902ab96a8564e16587ff5a6d28a21f7b5d51"
         "0c40ee0d225db482cfe103e02a2df88fccb17ec77b9b97a2ca1d5b81c4f5cfb27e63d218d603acee4d9c7cd238a5326282708294b03f0268f048f01a5177f07f"
         "3e41fbde52fc441908931843c742040d925eb3c38418974720e7555e7bcd8c5d3478f9b15da28483b25745e131a0de401e3cd6f9a77c1d9fbc6ba2c5e1d03163",
         "010001",
         "7b8e8f0ce91e38b598d36bbf3728508dd7a0aa0a7567190ebba14cfe59e43488e9266111330eeee0dc6187251dc11ac080f89dd64bb8c1a0f3f9045f437c5600"
         "e2647c6c17e5b26946ed595ede531acdf250c88d9f4a3bd4750f4db7d7191896248926e94a1bb98f40ef1ce110bc6ec9255ea9023ee6f0a3d90f95143a819c24"
         "d5ea5473d21361eda03ad53dcd57135c1805c51c01b7e833e25f3dd9346d7e8e4cc19b0cedb03738778169147d3c7ab3c0b0912fba27c755fb5624b8e155aa0f"
         "89b26f028b16694fd7424a529fbec343ee608e615588fbb8004401039776b2075e2a06f31c31b2fcc7a27327cf41187b5f2d32214c87620f2e5137642be439d9"},
        {// 1024 bits
         "c8063c11a236d0388705eaa4eb789e5cdc08e508fc92e2a98345d7482123f1a5d86077b8b0b08e1212b5e93300b1ea9a15554e5e60a4cd552b258f807971dcc4"
         "80fa7e4aefc2ae463eb59d7bfff697d253c4c3fc1bd97a1eae8898726930b95de5d4331b6302c320ad2771cc63ea1a503b3fcb14b7592dde382b537e4ce079a3",
         "03",
         "3ed14a6e2f9644d787d52a4c2a36cc0a2bc9e43c7cc0bfc94b4d4ea787346a174dbecfaff78ff73f55d4077bfed6de0851dd7df7fcd55c17be5533c5a379818d"
         "ee3bdbfee816afdd8e3215ee4958af3f86a0f67371783ffcda9f8a199b6605102543373ccd58649ff1b92ab3dd6e54551c9d05dc6451121eac163ca53b2df70a"},
    };
    const std::string message = "KerfKite workshop token";
    for (const Vector &v : vectors) {
        const std::string n = unhex(v.modulus), e = unhex(v.exponent), s = unhex(v.signature);
        CHECK(rsa_sha256_verify(n, e, message, s));
        CHECK(!rsa_sha256_verify(n, e, message + ".", s));
        std::string bent = s;
        bent[bent.size() / 2] = static_cast<char>(bent[bent.size() / 2] ^ 1);
        CHECK(!rsa_sha256_verify(n, e, message, bent));
        CHECK(!rsa_sha256_verify(n, e, message, s.substr(1)));  // shorter than the modulus
        CHECK(!rsa_sha256_verify(n, e, message, std::string(1, '\0') + s));  // longer
        CHECK(!rsa_sha256_verify(n, unhex("010003"), message, s));  // another exponent
    }
    // The two keys' signatures do not cross.
    CHECK(!rsa_sha256_verify(unhex(vectors[0].modulus), unhex(vectors[0].exponent), message, unhex(vectors[1].signature)));
}

bool have_scrypt() {
    std::string out;
    return scrypt("x", "y", 2, 1, 1, 16, out);
}

void hashes() {
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(pbkdf2_sha256("password", "salt", 1, 32)) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    // werkzeug's formats, as generate_password_hash wrote them.
    CHECK(check_password("pbkdf2:sha256:1000$iB9ffxHn$91a1dc52a60044743ce5dcc4908efefc4caf6d7b433a777a1f618e24dd846d62", "a password of some length"));
    CHECK(!check_password("pbkdf2:sha256:1000$iB9ffxHn$91a1dc52a60044743ce5dcc4908efefc4caf6d7b433a777a1f618e24dd846d62", "a password of some lengtH"));
    CHECK(check_password("pbkdf2:sha256:2$oI7J$e3be7c670c5ed77ac620e2ca132cf2d48afe3019589828fefb03b3a342fb7121", "na\xc3\xafve p\xc3\xa4ssword \xe2\x9c\x93 long"));
    CHECK(!check_password("md5$x$y", "anything"));
    CHECK(!check_password("pbkdf2:sha1:1000$a$b", "anything"));
    CHECK(!check_password("no dollar signs", "anything"));
    const std::string made = make_password_hash("a password of fifteen");
    CHECK(made.compare(0, 21, "pbkdf2:sha256:600000$") == 0);
    CHECK(made.size() == 21 + 32 + 1 + 64);
    CHECK(check_password(made, "a password of fifteen") && !check_password(made, "a password of fifteen!"));
    CHECK(make_password_hash("a password of fifteen") != made);  // a salt of its own
    const char *scrypted =
        "scrypt:1024:8:1$JBdKtxIl$9354382a6dc012a1b8c1f30fc3db63354a5a151b344932b2a46ce2cd222c086c2b9c462c52545f0ddb880710ea738387d1264a4a857922760820423d52"
        "42d0f8";
    bool unverifiable = false;
    const bool scrypt_ok = check_password(scrypted, "a password of some length", &unverifiable);
    if (have_scrypt()) {
        CHECK(scrypt_ok && !unverifiable);
        CHECK(!check_password(scrypted, "a password of some lengtx"));
        std::string key;
        CHECK(scrypt("password", "NaCl", 1024, 8, 16, 64, key));
        CHECK(hex(key) ==
              "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640");
    } else {
        CHECK(!scrypt_ok && unverifiable);
        std::printf("scrypt: not on this system (%s); Python's scrypt hashes need reset-password here\n", crypto_source().c_str());
    }
}

void sqlite_reading() {
    const std::string bytes = read_file(fs::u8path(fixture_dir) / "web.sqlite3");
    CHECK(bytes.size() > 4096);
    const auto users = sqlite_rows(bytes, "users");
    CHECK(users.size() == 2);
    CHECK(users.size() == 2 && users[0][0].bytes == "alice" && users[0][1].bytes.compare(0, 16, "scrypt:32768:8:1") == 0);
    CHECK(users.size() == 2 && users[1][0].bytes == "bob" && users[1][1].bytes.compare(0, 21, "pbkdf2:sha256:600000$") == 0);
    const auto tokens = sqlite_rows(bytes, "tokens");
    CHECK(tokens.size() == 1 && tokens[0][0].bytes == sha256_hex(kToken) && tokens[0][1].bytes == "alice" && tokens[0][2].number() == 1795000000.5);
    const auto files = sqlite_rows(bytes, "files");
    CHECK(files.size() == 61);
    size_t big = 0;
    for (const auto &row : files) {
        CHECK(row.size() == 7 && row[3].kind == SqliteValue::Blob && sha256_hex(row[3].bytes) == row[4].bytes);
        if (row[2].bytes == "big.svg") big = row[3].bytes.size();
    }
    CHECK(big > 20000);  // overflow pages
    CHECK(files.size() == 61 && files[5][2].bytes == "part-5.nc" && files[5][5].number() == 1795000005.0);
    CHECK(sqlite_rows(bytes, "nothing").empty());
    CHECK(sqlite_rows(bytes, "sessions").size() == 1 && sqlite_rows(bytes, "attempts").size() == 1);
    CHECK_TEXT(refusal([] { sqlite_rows("not a database at all, not at all, not at all, not at all, not at all, not at all, not at all..", "users"); }),
               "not an SQLite 3 database");
    std::string cut = bytes.substr(0, 4096 * 3);
    CHECK_TEXT(refusal([&] { sqlite_rows(cut, "files"); }), "cannot be read");
}

void moved_store() {
    Folder folder;
    fs::copy_file(fs::u8path(fixture_dir) / "web.sqlite3", folder.path / "web.sqlite3");
    {
        // A change left unfinished in the Python's journal is not moved past.
        std::ofstream(folder.path / "web.sqlite3-journal") << "pending";
        CHECK_TEXT(refusal([&] { WebStore store(folder.str()); }), "unfinished change");
        fs::remove(folder.path / "web.sqlite3-journal");
    }
    WebStore store(folder.str());
    CHECK(fs::exists(folder.path / "web-store.json"));
    CHECK(!fs::exists(folder.path / "web.sqlite3") && fs::exists(folder.path / "web.sqlite3.migrated"));
    CHECK(store.has_users());
    CHECK(store.token_user(kToken) == "alice");
    CHECK(store.token_user("another token").empty());
    const Json alice = store.files("alice"), bob = store.files("bob");
    CHECK(alice.size() == 41 && bob.size() == 20);
    CHECK(alice.size() == 41 && alice[0].at("name") == Json("big.svg"));  // newest first
    WebFile record;
    std::string content;
    CHECK(store.file("alice", std::string(32, 'f'), record, content) && record.name == "big.svg" && sha256_hex(content) == record.sha256 &&
          record.bytes == content.size());
    CHECK(!store.file("bob", std::string(32, 'f'), record, content));  // another account's file
    // Sessions and failed logins are not kept: everyone logs in again.
    WebSession session;
    CHECK(!store.session(std::string(64, '0'), session));
    // bob's PBKDF2 hash is werkzeug's own; alice's scrypt one needs the system's scrypt.
    LoginResult bob_login = store.check_login("BOB", kBob, "192.0.2.1");
    CHECK(bob_login.ok && bob_login.session.user == "bob");
    const LoginResult alice_login = store.check_login("alice", kAlice, "192.0.2.1");
    if (have_scrypt()) {
        CHECK(alice_login.ok);
        // The hash this system could only check is replaced by a PBKDF2 one.
        const Json data = Json::parse(read_file(folder.path / "web-store.json"));
        CHECK(data.at("users").at("alice").get<std::string>().compare(0, 21, "pbkdf2:sha256:600000$") == 0);
        CHECK(store.check_login("alice", kAlice, "192.0.2.1").ok);
        CHECK(store.unverifiable_accounts().empty());
    } else {
        CHECK(!alice_login.ok && !alice_login.limited);
        CHECK(store.unverifiable_accounts() == std::vector<std::string>{"alice"});
        store.add_user("alice", "a new password for alice", true);
        CHECK(store.check_login("alice", "a new password for alice", "192.0.2.1").ok);
    }
    // A second start finds the store and leaves it (where alice's password was set again, so were her tokens).
    WebStore again(folder.str());
    CHECK(again.token_user(kToken) == (have_scrypt() ? "alice" : "") && again.files("bob").size() == 20);
}

void accounts() {
    Folder folder;
    WebStore store(folder.str());
    CHECK(!store.has_users());
    CHECK(refusal([&] { store.add_user("bad name", "a password of fifteen", false); }) == "Username must be 1\xe2\x80\x93" "64 letters, digits or _.@-");
    CHECK(refusal([&] { store.add_user(std::string(65, 'a'), "a password of fifteen", false); }) == "Username must be 1\xe2\x80\x93" "64 letters, digits or _.@-");
    CHECK(refusal([&] { store.add_user("carol", "too short", false); }) == "Choose a password of 15\xe2\x80\x93" "256 characters or use the secure generator");
    CHECK(refusal([&] { store.add_user("carol", std::string(257, 'x'), false); }) == "Choose a password of 15\xe2\x80\x93" "256 characters or use the secure generator");
    CHECK(refusal([&] { store.add_user("nobody", "a password of fifteen", true); }) == "Unknown user");
    store.add_user("Carol.W@example", "a password of fifteen", false);
    CHECK(store.has_users());
    CHECK(refusal([&] { store.add_user("carol.w@example", "a password of fifteen", false); }) == "User already exists; use reset-password");
    // fifteen characters, not bytes
    store.add_user("dave", "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4", false);
    CHECK(store.check_login("dave", "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4", "a").ok);

    LoginResult login = store.check_login("CAROL.W@EXAMPLE", "a password of fifteen", "198.51.100.4");
    CHECK(login.ok && login.session.user == "carol.w@example" && login.session.sid.size() == 43 && login.session.csrf.size() == 43);
    WebSession session;
    CHECK(store.session(login.session.sid, session) && session.user == "carol.w@example" && session.csrf == login.session.csrf);
    CHECK(!store.session(login.session.sid + "x", session));
    CHECK(!store.session(std::string(129, 'a'), session));
    // The session is kept as a digest, never as the cookie's value.
    CHECK(read_file(folder.path / "web-store.json").find(login.session.sid) == std::string::npos);
    store.logout(login.session.sid);
    CHECK(!store.session(login.session.sid, session));

    // Ten failures for an account or an address, then refusals for fifteen minutes; a success releases its own.
    for (int i = 0; i < 9; ++i) CHECK(!store.check_login("carol.w@example", "wrong password, wrong", "203.0.113." + std::to_string(i)).ok);
    CHECK(store.check_login("carol.w@example", "a password of fifteen", "203.0.113.50").ok);
    for (int i = 0; i < 10; ++i) {
        const LoginResult r = store.check_login("carol.w@example", "wrong password, wrong", "203.0.113.60");
        CHECK(!r.ok && !r.limited);
    }
    const LoginResult limited = store.check_login("carol.w@example", "a password of fifteen", "203.0.113.61");
    CHECK(!limited.ok && limited.limited);
    const LoginResult address = store.check_login("dave", "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4", "203.0.113.60");
    CHECK(!address.ok && address.limited);  // the address has had ten
    CHECK(!store.check_login("nobody", "a password of fifteen", "192.0.2.200").ok);
    CHECK(!store.check_login("dave", std::string(300, 'x'), "192.0.2.201").ok);

    // Upload tokens: kept as digests, revoked together, and gone when the password is reset.
    CHECK(refusal([&] { store.issue_token("nobody"); }) == "Unknown user");
    const std::string token = store.issue_token("dave");
    CHECK(token.size() == 43 && store.token_user(token) == "dave");
    CHECK(read_file(folder.path / "web-store.json").find(token) == std::string::npos);
    const std::string second = store.issue_token("dave");
    store.revoke_tokens("DAVE");
    CHECK(store.token_user(token).empty() && store.token_user(second).empty());
    const std::string third = store.issue_token("dave");
    LoginResult dave = store.check_login("dave", "\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4\xc3\xa4", "192.0.2.9");
    CHECK(dave.ok);
    store.add_user("dave", "a replacement password", true);
    CHECK(store.token_user(third).empty() && !store.session(dave.session.sid, session));

    // Files: the account's own, newest first, within the quota.
    const Json stored = store.put_file("dave", "a.nc", "G0 X0\n", "web");
    CHECK(stored.at("status") == Json("stored_for_review") && stored.at("bytes") == Json(6) && stored.at("sha256") == Json(sha256_hex("G0 X0\n")));
    const std::string id = stored.at("id").get<std::string>();
    CHECK(id.size() == 32);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    store.put_file("dave", "b.svg", "<svg/>", "upload");
    const Json list = store.files("dave");
    CHECK(list.size() == 2 && list[0].at("name") == Json("b.svg") && list[0].at("source") == Json("upload") && list[1].at("id") == Json(id));
    CHECK(store.files("carol.w@example").empty());
    store.delete_file("carol.w@example", id);  // not hers: nothing happens
    CHECK(store.files("dave").size() == 2);
    store.delete_file("dave", id);
    CHECK(store.files("dave").size() == 1 && !fs::exists(folder.path / "files" / id));
    const std::string large(16 * 1024 * 1024, 'x');
    for (int i = 0; i < 3; ++i) store.put_file("dave", "big.nc", large, "web");
    CHECK(refusal([&] { store.put_file("dave", "big.nc", large + "x", "web"); }) == "Account file quota reached; download and delete unused files");
}

void uploads_and_receivers() {
    const auto upload_error = [](const char *text) { return refusal([&] { validate_upload(Json::parse(text)); }); };
    CHECK(upload_error("[1]") == "Upload must be a JSON object");
    CHECK(upload_error(R"({"name": "a/b.nc", "content": "x"})") == "Use a simple file name without folders");
    CHECK(upload_error(R"({"name": "..nc", "content": "x"})") == "Use a simple file name without folders");
    CHECK(upload_error(R"({"name": "_a.nc", "content": "x"})") == "Use a simple file name without folders");
    CHECK(upload_error(R"({"name": "a b.nc", "content": "x"})") == "Use a simple file name without folders");
    CHECK(upload_error(R"({"name": "café.nc", "content": "x"})") == "Use a simple file name without folders");
    CHECK(upload_error(R"({"name": 5, "content": "x"})") == "Use a simple file name without folders");
    CHECK(upload_error(R"({"content": "x"})") == "Supported files: .nc, .gcode, .svg, .cncstudio.json, .templates.json, .paths3d.json");
    CHECK(upload_error(R"({"name": "a.txt", "content": "x"})") == "Supported files: .nc, .gcode, .svg, .cncstudio.json, .templates.json, .paths3d.json");
    CHECK(upload_error(R"({"name": "A.GCODE", "content": 7})") == "Upload must contain text, without NUL characters");
    CHECK(upload_error(R"({"name": "a.nc", "content": "x\u0000y"})") == "Upload must contain text, without NUL characters");
    CHECK(upload_error(R"({"name": "a.nc", "content": ""})") == "File must contain 1 byte to 8 MB");
    CHECK(upload_error(R"({"name": "a.nc", "content": "x", "sha256": "00"})") == "Upload checksum mismatch");
    CHECK(upload_error(R"({"name": "a.nc", "content": "x", "sha256": 1})") == "Upload checksum mismatch");
    CHECK(upload_error(R"({"name": "a.nc", "content": "x", "sha256": ""})").empty());  // a false checksum is not checked
    const Upload upload = validate_upload(Json::parse(R"({"name": "Part-2.cncstudio.json", "content": "{\"é\": 1}"})"));
    CHECK(upload.name == "Part-2.cncstudio.json" && upload.content == "{\"\xc3\xa9\": 1}");
    CHECK(simple_file_name("x.nc") && simple_file_name("a-b_c.d.svg") && !simple_file_name("x.nc.") && !simple_file_name("x\\y.nc"));
#ifdef _WIN32
    CHECK(!simple_file_name("con.nc") && !simple_file_name("LPT1.svg") && simple_file_name("console.nc"));
#else
    CHECK(simple_file_name("con.nc"));
#endif

    const auto destination_error = [](const char *text) { return refusal([&] { validate_destination(Json::parse(text)); }); };
    const std::string token = "\"token\": \"" + std::string(32, 't') + "\"";
    const char *origin = "Destination must be an HTTP/S server origin, without credentials, path or query";
    CHECK(destination_error(("{\"url\": \"ftp://h\", " + token + "}").c_str()) == origin);
    CHECK(destination_error(("{\"url\": \"https://u:p@h\", " + token + "}").c_str()) == origin);
    CHECK(destination_error(("{\"url\": \"https://h/inbox\", " + token + "}").c_str()) == origin);
    CHECK(destination_error(("{\"url\": \"https://h/?a=1\", " + token + "}").c_str()) == origin);
    CHECK(destination_error(("{\"url\": \"https://h#f\", " + token + "}").c_str()) == origin);
    CHECK(destination_error(("{" + token + "}").c_str()) == origin);
    CHECK(destination_error(("{\"url\": \"https://[::1\", " + token + "}").c_str()) == "Invalid IPv6 URL");
    CHECK(destination_error(("{\"url\": \"https://[192.0.2.1]\", " + token + "}").c_str()) == "An IPv4 address cannot be in brackets");
    CHECK(destination_error(("{\"url\": \"http://h\", " + token + "}").c_str()) == "Plain HTTP needs explicit --allow-http; HTTPS is the default");
    CHECK(destination_error(R"({"url": "https://h", "token": "short"})") == "Configure the receiver-generated upload token");
    CHECK(destination_error(("{\"url\": \"https://h\", \"ca_file\": \"/no/such/ca.pem\", " + token + "}").c_str()) == "CA certificate file does not exist");
    CHECK(destination_error(("{\"url\": \"HTTPS://Receiver.example:8443/\", " + token + "}").c_str()).empty());
    CHECK(destination_error(("{\"url\": \"http://[2001:db8::1]:8080\", \"allow_http\": true, " + token + "}").c_str()).empty());

    const UrlParts parts = url_split("  HTTPS://User@[FE80::1%Eth0]:99/p?q#f");
    CHECK(parts.scheme == "https" && parts.hostname == "fe80::1%Eth0" && parts.port == "99" && parts.username == "User" && parts.path == "/p" &&
          parts.query == "q" && parts.fragment == "f");
    std::string host;
    CHECK(web_origin("https://Studio.Example.COM/", host) == "https://studio.example.com" && host == "studio.example.com");
    CHECK(web_origin("https://studio.example.com:443", host) == "https://studio.example.com" && host == "studio.example.com");
    CHECK(web_origin("https://[2001:DB8::5]:8443", host) == "https://[2001:db8::5]:8443" && host == "[2001:db8::5]:8443");
    for (const char *bad : {"http://studio.example", "https://", "https://h/editor", "https://h?x", "https://u@h", "https://h:0", "https://h:99999", "https://h:x"})
        CHECK(refusal([&] { web_origin(bad, host); }) == "Public URL must be an HTTPS origin");
}

// ---- the app's routes, through create_app's checks ------------------------------------------------------

struct Browser {
    explicit Browser(WebApp &a) : app(a) {}
    WebApp &app;
    std::string cookies, token;
    std::string origin = "https://studio.example";

    http::Request make(const std::string &method, const std::string &path, const std::string &body = std::string(),
                       const std::string &type = "application/json") {
        http::Request r;
        r.method = method;
        r.target = r.path = path;
        r.peer = "192.0.2.44";
        r.headers.emplace_back("host", "studio.example");
        if (!cookies.empty()) r.headers.emplace_back("cookie", cookies);
        if (!body.empty() && !type.empty()) r.headers.emplace_back("content-type", type);
        if (method == "POST") r.headers.emplace_back("origin", origin);
        if (method == "POST" && !token.empty()) r.headers.emplace_back("x-studio-token", token);
        r.body = body;
        return r;
    }
    http::Response send(const http::Request &r) {
        http::Response response = app.handle(r);
        for (const auto &h : response.headers)
            if (h.first == "Set-Cookie") {
                const std::string pair = h.second.substr(0, h.second.find(';'));
                const std::string name = pair.substr(0, pair.find('='));
                std::string kept;
                for (size_t at = 0; at < cookies.size();) {
                    size_t end = cookies.find("; ", at);
                    if (end == std::string::npos) end = cookies.size();
                    const std::string one = cookies.substr(at, end - at);
                    if (one.compare(0, name.size() + 1, name + "=") != 0) kept += (kept.empty() ? "" : "; ") + one;
                    at = end + 2;
                }
                if (pair.size() > name.size() + 1) kept += (kept.empty() ? "" : "; ") + pair;
                cookies = kept;
            }
        return response;
    }
    http::Response get(const std::string &path) { return send(make("GET", path)); }
    http::Response post(const std::string &path, const std::string &body = "{}", const std::string &type = "application/json") {
        return send(make("POST", path, body, type));
    }
};

std::string header_of(const http::Response &r, const std::string &name) {
    std::string out;
    for (const auto &h : r.headers)
        if (h.first == name) out += (out.empty() ? "" : "\n") + h.second;
    return out;
}

std::string form_value(const std::string &page, const std::string &name) {
    const std::string marker = "name=\"" + name + "\" value=\"";
    const size_t at = page.find(marker);
    if (at == std::string::npos) return std::string();
    const size_t from = at + marker.size();
    return page.substr(from, page.find('"', from) - from);
}

void secure_headers(const http::Response &r) {
    CHECK(header_of(r, "Cache-Control") == "no-store");
    CHECK(header_of(r, "X-Content-Type-Options") == "nosniff");
    CHECK(header_of(r, "Strict-Transport-Security") == "max-age=31536000");
    CHECK_TEXT(header_of(r, "Content-Security-Policy"), "frame-ancestors 'none'");
    CHECK(header_of(r, "Referrer-Policy") == "same-origin");
}

void app_routes() {
    Folder folder;
    WebStore store(folder.str());
    WebOptions options;
    options.public_url = "https://studio.example/";
    options.static_dir = static_dir;
    CHECK(refusal([&] { WebApp app(store, options, Json::object(), {}); }) == "Create a user first; there are no default credentials");
    store.add_user("erin", "erin's password of length", false);
    WebOptions wrong = options;
    wrong.public_url = "http://studio.example";
    CHECK(refusal([&] { WebApp app(store, wrong, Json::object(), {}); }) == "Public URL must be an HTTPS origin");
    CHECK(refusal([&] { WebApp app(store, options, Json::parse(R"({"shop": {"url": "https://shop"}})"), {"shop"}); }) ==
          "Configure the receiver-generated upload token");
    Json receivers = Json::object();
    Json entry = Json::object();
    entry["url"] = "https://receiver.example";
    entry["token"] = std::string(40, 'r');
    receivers["shop"] = entry;
    receivers["annex"] = entry;
    WebApp app(store, options, receivers, {"shop", "annex"});
    Browser browser(app);

    // The Host must be the public one.
    http::Request elsewhere = browser.make("GET", "/login");
    elsewhere.headers[0].second = "attacker.example";
    const http::Response refused_host = app.handle(elsewhere);
    CHECK(refused_host.status == 400);
    secure_headers(refused_host);
    http::Request with_port = browser.make("GET", "/favicon.ico");
    with_port.headers[0].second = "Studio.Example:443";
    CHECK(app.handle(with_port).status == 204);

    // Without a session: the pages send to the login, the routes say the login expired.
    const http::Response page = browser.get("/");
    CHECK(page.status == 302 && header_of(page, "Location") == "/login");
    CHECK_TEXT(page.body, "<a href=\"/login\">/login</a>");
    const http::Response route = browser.get("/api/files");
    CHECK(route.status == 401 && Json::parse(route.body).at("error") == Json("Login expired. Save your project locally, then log in again."));
    secure_headers(route);
    CHECK(browser.get("/favicon.ico").status == 204);

    // The login form and its own token.
    const http::Response form = browser.get("/login");
    CHECK(form.status == 200 && header_of(form, "Content-Type") == "text/html; charset=utf-8");
    CHECK_TEXT(form.body, "<title>CNC Studio login</title>");
    const std::string csrf = form_value(form.body, "csrf");
    CHECK(csrf.size() == 43);
    CHECK_TEXT(header_of(form, "Set-Cookie"), "__Host-cnc-login=" + csrf + "; Expires=");
    CHECK_TEXT(header_of(form, "Set-Cookie"), "; Max-Age=600; Secure; HttpOnly; Path=/; SameSite=Strict");
    CHECK(form_value(browser.get("/login").body, "csrf") == csrf);  // the cookie's token is kept
    const std::string login_body = "csrf=" + csrf + "&username=Erin&password=erin%27s+password+of+length";
    // From another origin, or without the form's token, the login is refused.
    browser.origin = "https://attacker.example";
    CHECK(browser.post("/login", login_body, "application/x-www-form-urlencoded").status == 403);
    browser.origin = "https://studio.example";
    CHECK(browser.post("/login", "csrf=other&username=erin&password=x", "application/x-www-form-urlencoded").status == 403);
    const http::Response bad = browser.post("/login", "csrf=" + csrf + "&username=erin&password=wrong", "application/x-www-form-urlencoded");
    CHECK(bad.status == 401 && bad.body == "Invalid username or password.");
    const http::Response in = browser.post("/login", login_body, "application/x-www-form-urlencoded");
    CHECK(in.status == 302 && header_of(in, "Location") == "/");
    CHECK_TEXT(header_of(in, "Set-Cookie"), "__Host-cnc-session=");
    CHECK_TEXT(header_of(in, "Set-Cookie"), "; Max-Age=28800; Secure; HttpOnly; Path=/; SameSite=Strict");
    CHECK_TEXT(header_of(in, "Set-Cookie"), "__Host-cnc-login=; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0;");

    const http::Response session = browser.get("/api/session");
    CHECK(session.status == 200);
    const Json info = Json::parse(session.body);
    CHECK(info.at("username") == Json("erin") && info.at("web_mode") == Json(true) && info.at("relief_map") == Json(1024));
    browser.token = info.at("token").get<std::string>();
    CHECK(browser.get("/").status == 200);
    CHECK(browser.get("/app.js").status == 200);
    CHECK(browser.get("/no-such-file.js").status == 404);

    // Writes need the session's token and the public origin.
    const std::string saved_token = browser.token;
    browser.token.clear();
    CHECK(browser.post("/api/files", R"({"name": "a.nc", "content": "G0 X1\n"})").status == 403);
    browser.token = saved_token;
    browser.origin = "https://attacker.example";
    CHECK(browser.post("/api/files", R"({"name": "a.nc", "content": "G0 X1\n"})").status == 403);
    browser.origin = "https://studio.example";
    CHECK(browser.post("/api/files", R"({"name": "a.nc", "content": "G0 X1\n"})", "text/plain").status == 415);
    const http::Response saved = browser.post("/api/files", R"({"name": "a.nc", "content": "G0 X1\n"})");
    CHECK(saved.status == 200);
    const std::string id = Json::parse(saved.body).at("id").get<std::string>();
    CHECK(Json::parse(browser.post("/api/files", R"({"name": "../a.nc", "content": "x"})").body).at("error") == Json("Use a simple file name without folders"));
    const Json files = Json::parse(browser.get("/api/files").body);
    CHECK(files.size() == 1 && files[0].at("name") == Json("a.nc") && files[0].at("source") == Json("web"));
    const http::Response download = browser.get("/api/files/" + id);
    CHECK(download.status == 200 && download.body == "G0 X1\n" && header_of(download, "Content-Type") == "application/octet-stream");
    CHECK(header_of(download, "Content-Disposition") == "attachment; filename=a.nc");
    CHECK(browser.get("/api/files/" + std::string(32, '0')).status == 404);
    CHECK(browser.post("/api/files/" + id).status == 405);
    CHECK(browser.post("/api/files/" + id + "/delete").status == 200);
    CHECK(Json::parse(browser.get("/api/files").body).empty());

    // Receivers: only the operator's, in the order the operator added them.
    CHECK(browser.get("/api/destinations").body == R"(["shop","annex"])");
    CHECK(Json::parse(browser.post("/api/send", R"({"name": "a.nc", "content": "x", "destination": "https://elsewhere"})").body).at("error") ==
          Json("Choose an operator-configured receiver"));

    // Flask's method handling: 405 and OPTIONS name the methods.
    http::Request put_request = browser.make("PUT", "/api/session");
    CHECK(app.handle(put_request).status == 403);  // a change without the session's token, refused first
    put_request.headers.emplace_back("origin", browser.origin);
    put_request.headers.emplace_back("x-studio-token", browser.token);
    const http::Response put = browser.send(put_request);
    CHECK(put.status == 405 && header_of(put, "Allow") == "GET, HEAD, OPTIONS");
    const http::Response options_reply = browser.send(browser.make("OPTIONS", "/api/files"));
    CHECK(options_reply.status == 200 && header_of(options_reply, "Allow") == "GET, HEAD, OPTIONS, POST");
    CHECK(browser.post("/api/session").status == 405);

    // The routes every server has.
    CHECK(Json::parse(browser.get("/api/profiles").body).is_array());
    const http::Response job = browser.post("/api/generate", R"({"mode": "router", "width": 100, "height": 100, "feed": 600, "power": 1000, "safe": 5,
        "depth": 1.2, "step": 0.5, "plunge": 100, "passes": 1, "paths": [[[10, 10], [20, 10], [20, 20]]]})");
    CHECK(job.status == 200);
    CHECK_TEXT(job.body, "\"lines\":[\"G21\"");
    CHECK(browser.post("/api/generate", "[1,", "application/json").status == 400);

    // The upload inbox: a token, not a session; from a page opened as a file only when allowed.
    Browser machine(app);
    const std::string inbox = store.issue_token("erin");
    http::Request upload = machine.make("POST", "/api/receive", R"({"name": "b.gcode", "content": "G1 X2\n"})");
    CHECK(app.handle(upload).status == 401);
    upload.headers.emplace_back("authorization", "Bearer " + inbox);
    const http::Response received = app.handle(upload);
    CHECK(received.status == 200 && Json::parse(received.body).at("status") == Json("stored_for_review"));
    CHECK(Json::parse(browser.get("/api/files").body)[0].at("source") == Json("upload"));
    http::Request preflight = machine.make("OPTIONS", "/api/receive");
    preflight.headers.emplace_back("origin", "null");
    CHECK(app.handle(preflight).status == 403);
    WebOptions open = options;
    open.allow_file_upload = true;
    WebApp file_pages(store, open, Json::object(), {});
    const http::Response allowed = file_pages.handle(preflight);
    CHECK(allowed.status == 204 && header_of(allowed, "Access-Control-Allow-Origin") == "null" && header_of(allowed, "Vary") == "Origin");
    CHECK(header_of(app.handle(preflight), "Access-Control-Allow-Origin").empty());

    // Logging out ends the session.
    CHECK(browser.post("/api/logout").status == 200);
    CHECK(browser.get("/api/session").status == 401);
}

// ---- the HTTP server's PROXY protocol line ----------------------------------------------------------------

std::string round_trip(int port, const std::string &raw) {
    const auto s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string reply;
    if (::connect(s, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0) {
        ::send(s, raw.data(), static_cast<int>(raw.size()), 0);
        char buffer[4096];
        for (;;) {
            const auto n = ::recv(s, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            reply.append(buffer, static_cast<size_t>(n));
        }
    }
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
    return reply;
}

void proxy_protocol() {
    http::Server server("127.0.0.1", 0);
    std::thread worker([&] {
        server.serve([](const http::Request &r) { return http::Response::json("\"" + r.peer + "\""); },
                     [](const http::Request &) { return size_t(1024); }, 8, true);
    });
    const int port = server.port();
    CHECK_TEXT(round_trip(port, "PROXY TCP4 192.0.2.5 127.0.0.1 51000 8443\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n"), "\"192.0.2.5\"");
    CHECK_TEXT(round_trip(port, "PROXY TCP6 2001:db8::9 ::1 51000 8443\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n"), "\"2001:db8::9\"");
    CHECK_TEXT(round_trip(port, "PROXY UNKNOWN\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n"), "\"127.0.0.1\"");
    CHECK_TEXT(round_trip(port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "400");  // a connection must say who it is for
    CHECK_TEXT(round_trip(port, "PROXY TCP4 not-an-address 127.0.0.1 1 2\r\nGET / HTTP/1.1\r\n\r\n"), "400");
    CHECK_TEXT(round_trip(port, "PROXY TCP4 192.0.2.5 127.0.0.1 1\r\nGET / HTTP/1.1\r\n\r\n"), "400");
    CHECK_TEXT(round_trip(port, "PROXY " + std::string(200, 'x')), "400");
    server.stop();
    worker.join();
}

// ---- a file sent to a receiver through the relay -----------------------------------------------------------

void send_through_the_relay() {
    if (relay_program.empty()) {
        std::printf("receiver: no relay program given, the send checks skipped\n");
        return;
    }
    // A stand-in receiver over plain HTTP (allowed with allow_http), answering as /api/receive does.
    std::string seen_authorization, seen_body;
    std::string answer;
    http::Server receiver("127.0.0.1", 0);
    std::thread worker([&] {
        receiver.serve(
            [&](const http::Request &r) {
                const std::string *a = r.header("authorization");
                seen_authorization = a ? *a : std::string();
                seen_body = r.body;
                return http::Response::json(answer);
            },
            [](const http::Request &) { return size_t(1 << 20); });
    });
    Json entry = Json::object();
    entry["url"] = "http://127.0.0.1:" + std::to_string(receiver.port()) + "/";
    entry["allow_http"] = true;
    entry["token"] = std::string(40, 'k');
    const Upload upload = validate_upload(Json::parse(R"({"name": "c.nc", "content": "G0 Y3\n"})"));
    const std::string sha = sha256_hex(upload.content);
    answer = "{\"id\": \"r1\", \"sha256\": \"" + sha + "\", \"bytes\": 6, \"status\": \"stored_for_review\"}";
    const Json sent = send_to_receiver(entry, upload, relay_program);
    CHECK(sent.at("id") == Json("r1") && sent.at("sha256") == Json(sha) && sent.at("bytes") == Json(6) && sent.at("name") == Json("c.nc"));
    CHECK(seen_authorization == "Bearer " + std::string(40, 'k'));
    const Json body = Json::parse(seen_body);
    CHECK(body.at("name") == Json("c.nc") && body.at("content") == Json("G0 Y3\n") && body.at("sha256") == Json(sha));
    answer = "{\"id\": \"r1\", \"sha256\": \"" + sha + "\", \"bytes\": 7, \"status\": \"stored_for_review\"}";
    CHECK(refusal([&] { send_to_receiver(entry, upload, relay_program); }) ==
          "Receiver did not confirm the expected file checksum and size; inspect it before retrying");
    answer = "not json";
    CHECK_TEXT(refusal([&] { send_to_receiver(entry, upload, relay_program); }), "Upload result could not be verified");
    answer = "\"" + std::string(70000, 'x') + "\"";
    CHECK_TEXT(refusal([&] { send_to_receiver(entry, upload, relay_program); }), "Upload result could not be verified");
    receiver.stop();
    worker.join();
    // Nobody there now.
    CHECK_TEXT(refusal([&] { send_to_receiver(entry, upload, relay_program); }), "Upload result could not be verified");
    entry["url"] = "https://127.0.0.1:1";
    entry["ca_file"] = (fs::u8path(fixture_dir) / "web.sqlite3").u8string();  // a file, not a certificate
    CHECK(refusal([&] { send_to_receiver(entry, upload, relay_program); }) == "The receiver's CA certificate file holds no certificate");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: kerfkite_web_selftest STATIC_DIR FIXTURE_DIR [TLS_RELAY|-]\n");
        return 2;
    }
    static_dir = argv[1];
    fixture_dir = argv[2];
    if (argc == 4 && std::string(argv[3]) != "-") relay_program = argv[3];
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    std::printf("hashes: %s\n", crypto_source().c_str());
    hashes();
    signatures();
    sqlite_reading();
    moved_store();
    accounts();
    uploads_and_receivers();
    app_routes();
    proxy_protocol();
    send_through_the_relay();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
