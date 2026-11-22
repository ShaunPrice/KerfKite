// The editor's files. See static_files.hpp.
#include "static_files.hpp"

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace kerfkite {
namespace server {

namespace {

// posixpath.normpath, and safe_join's refusals.
bool safe_relative(const std::string &path, std::string &out) {
    if (path.empty() || path[0] == '/') return false;
#ifdef _WIN32
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
#endif
    std::vector<std::string> parts;
    size_t at = 0;
    for (;;) {
        const size_t next = path.find('/', at);
        const std::string part = path.substr(at, next == std::string::npos ? std::string::npos : next - at);
        if (part == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (next == std::string::npos) break;
        at = next + 1;
    }
    if (parts.empty()) return false;
    out.clear();
    for (const std::string &part : parts) out += (out.empty() ? "" : "/") + part;
    return true;
}

std::string extension(const std::string &path) {
    const size_t slash = path.rfind('/'), dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    std::string e = path.substr(dot);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

const char *mime(const std::string &path) {
    const std::string e = extension(path);
    if (e == ".html" || e == ".htm") return "text/html; charset=utf-8";
    if (e == ".js" || e == ".mjs") return "text/javascript; charset=utf-8";
    if (e == ".css") return "text/css; charset=utf-8";
    if (e == ".json") return "application/json";
    if (e == ".svg") return "image/svg+xml";
    if (e == ".png") return "image/png";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".gif") return "image/gif";
    if (e == ".webp") return "image/webp";
    if (e == ".ico") return "image/vnd.microsoft.icon";
    if (e == ".woff2") return "font/woff2";
    if (e == ".woff") return "font/woff";
    if (e == ".ttf") return "font/ttf";
    if (e == ".otf") return "font/otf";
    if (e == ".txt" || e == ".md") return "text/plain; charset=utf-8";
    if (e == ".wasm") return "application/wasm";
    if (e == ".webmanifest") return "application/manifest+json";
    return "application/octet-stream";
}

bool is_file(const std::string &path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && (info.st_mode & S_IFMT) == S_IFREG;
}

bool is_directory(const std::string &path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}

}  // namespace

http::Response static_file(const std::string &directory, const std::string &relative) {
    std::string safe;
    if (!safe_relative(relative, safe) || !is_file(directory + "/" + safe)) return http::Response::error(404, "Not Found");
    std::ifstream in(directory + "/" + safe, std::ios::binary);
    std::ostringstream body;
    body << in.rdbuf();
    http::Response r;
    r.headers.emplace_back("Content-Type", mime(safe));
    r.headers.emplace_back("Cache-Control", "no-cache");
    r.body = body.str();
    return r;
}

std::string find_static_dir(const std::string &given, const char *argv0) {
    if (!given.empty()) return given;
    std::string program = argv0 ? argv0 : "";
    const size_t slash = program.find_last_of("/\\");
    const std::string here = slash == std::string::npos ? std::string(".") : program.substr(0, slash);
    for (const std::string &candidate : {here + "/../share/kerfkite/static", here + "/static", here + "/../studio/static",
                                         here + "/../../studio/static", std::string("studio/static")})
        if (is_directory(candidate) && is_file(candidate + "/index.html")) return candidate;
    return "studio/static";
}

}  // namespace server
}  // namespace kerfkite
