// The assistant server's own tools. See mcp_tools.hpp.
#include "mcp_tools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "inflate.hpp"
#include "kerfkite/cam.hpp"
#include "kerfkite/errors.hpp"
#include "kerfkite/number_text.hpp"
#include "os_random.hpp"
#include "os_crypto.hpp"
#include "step_exchange.hpp"
#include "studio.hpp"

namespace kerfkite {
namespace server {
namespace fs = std::filesystem;

namespace {

struct Declaration {
    const char *name, *description, *input, *output;
    bool wrapped;
    const char *annotations;
};

const Declaration kDeclarations[] = {
#include "mcp_declarations.inc"
};

const size_t kLimit = 8 * 1024 * 1024;

// uuid.uuid4().hex.
std::string uuid4_hex() {
    std::string bytes = random_bytes(16);
    bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0f) | 0x40);
    bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3f) | 0x80);
    return hex(bytes);
}

// A file made new, never over one that is there (open(..., 'x')).
void create_new(const fs::path &path, const std::string &bytes) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write " + path.u8string());
    bool ok = true;
    for (size_t at = 0; ok && at < bytes.size();) {
        DWORD wrote = 0;
        ok = WriteFile(file, bytes.data() + at, static_cast<DWORD>(std::min<size_t>(bytes.size() - at, 1 << 30)), &wrote, nullptr) && wrote > 0;
        at += wrote;
    }
    CloseHandle(file);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot write " + path.u8string());
    bool ok = true;
    for (size_t at = 0; ok && at < bytes.size();) {
        const ssize_t wrote = ::write(fd, bytes.data() + at, bytes.size() - at);
        ok = wrote > 0;
        if (ok) at += static_cast<size_t>(wrote);
    }
    ok = ::close(fd) == 0 && ok;
#endif
    if (!ok) throw std::runtime_error("Cannot write " + path.u8string());
}

bool extension_character(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// The longest run of one to three extensions (".x" of 1 to 20 letters, digits, _ or -) that ends the name and starts
// leftmost, as SUFFIX.search finds it; empty for none.
std::string suffix_of(const std::string &name) {
    for (size_t start = 0; start < name.size(); ++start) {
        if (name[start] != '.') continue;
        size_t at = start;
        int groups = 0;
        while (at < name.size() && name[at] == '.' && groups < 3) {
            size_t end = at + 1;
            while (end < name.size() && extension_character(name[end]) && end - at - 1 < 20) ++end;
            if (end == at + 1) break;
            at = end;
            ++groups;
        }
        if (groups > 0 && at == name.size()) return name.substr(start);
    }
    return std::string();
}

// PurePath(name).name.
std::string last_part(std::string name) {
#ifdef _WIN32
    for (char &c : name)
        if (c == '\\') c = '/';
    if (name.size() >= 2 && name[1] == ':') name = name.substr(2);
#endif
    while (!name.empty() && name.back() == '/') name.pop_back();
    const size_t slash = name.rfind('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

std::string ascii_lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// ---- the template collection (studio/template_catalog.py) ---------------------------------------------------

// builtin_templates.kkart: "KKART\x01\r\n", each template's paths compressed with zlib (a count, then each path's
// point count and its points as pairs of little-endian doubles), and at the end the index (JSON: [offset, length,
// metadata] for each template) and its length in four bytes.
class Catalog {
public:
    explicit Catalog(std::string path) : path_(std::move(path)) {}

    std::vector<Json> entries() {
        std::lock_guard<std::mutex> hold(lock_);
        load();
        return index_;
    }

    // The template of that identifier, its paths included, or null.
    Json get(const std::string &id) {
        std::lock_guard<std::mutex> hold(lock_);
        load();
        const auto found = positions_.find(id);
        if (found == positions_.end()) return Json();
        const Json &entry = index_[found->second];
        std::ifstream in(fs::u8path(path_), std::ios::binary);
        const uint64_t offset = entry[0].get<uint64_t>(), length = entry[1].get<uint64_t>();
        std::string packed(static_cast<size_t>(length), '\0');
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in.read(&packed[0], static_cast<std::streamsize>(length))) throw std::runtime_error("Truncated template path");
        std::vector<unsigned char> data;
        if (detail::zlib_decompress(reinterpret_cast<const unsigned char *>(packed.data()), packed.size(), 256u << 20, data) != detail::Inflated::Ok)
            throw std::runtime_error("Invalid template data");
        const auto u32 = [&](size_t at) {
            if (at + 4 > data.size()) throw std::runtime_error("Truncated template path");
            return static_cast<uint32_t>(data[at]) | static_cast<uint32_t>(data[at + 1]) << 8 | static_cast<uint32_t>(data[at + 2]) << 16 |
                   static_cast<uint32_t>(data[at + 3]) << 24;
        };
        const auto f64 = [&](size_t at) {
            uint64_t bits = 0;
            for (int k = 7; k >= 0; --k) bits = bits << 8 | data[at + static_cast<size_t>(k)];
            double v;
            std::memcpy(&v, &bits, 8);
            return v;
        };
        size_t at = 4;
        const uint32_t count = u32(0);
        Json paths = Json::array();
        for (uint32_t p = 0; p < count; ++p) {
            const uint32_t points = u32(at);
            at += 4;
            if (at + static_cast<size_t>(points) * 16 > data.size()) throw std::runtime_error("Truncated template path");
            Json path = Json::array();
            for (uint32_t k = 0; k < points; ++k, at += 16) {
                Json point = Json::array();
                point.push_back(f64(at));
                point.push_back(f64(at + 8));
                path.push_back(point);
            }
            paths.push_back(path);
        }
        if (at != data.size()) throw std::runtime_error("Unexpected data after template paths");
        Json out = entry[2];
        out["paths"] = paths;
        return out;
    }

private:
    std::string path_;
    std::mutex lock_;
    uintmax_t size_ = 0;
    fs::file_time_type time_{};
    std::vector<Json> index_;
    std::map<std::string, size_t> positions_;

    // The index, read again when the file was replaced (an installer may replace it while the server runs).
    void load() {
        std::error_code error;
        const fs::path path = fs::u8path(path_);
        const uintmax_t size = fs::file_size(path, error);
        if (error) throw std::runtime_error("The template collection is not installed: " + path_);
        const fs::file_time_type time = fs::last_write_time(path, error);
        if (!index_.empty() && size == size_ && time == time_) return;
        std::ifstream in(path, std::ios::binary);
        char magic[8];
        if (!in.read(magic, 8) || std::memcmp(magic, "KKART\x01\r\n", 8) != 0) throw std::runtime_error("Unsupported artwork catalog format");
        unsigned char tail[4];
        in.seekg(-4, std::ios::end);
        if (!in.read(reinterpret_cast<char *>(tail), 4)) throw std::runtime_error("Invalid artwork catalog index");
        const uint64_t index_size = tail[0] | tail[1] << 8 | tail[2] << 16 | static_cast<uint64_t>(tail[3]) << 24;
        if (index_size + 4 + 8 > size) throw std::runtime_error("Invalid artwork catalog index");
        std::string text(static_cast<size_t>(index_size), '\0');
        in.seekg(static_cast<std::streamoff>(size - 4 - index_size));
        in.read(&text[0], static_cast<std::streamsize>(index_size));
        const Json entries = Json::parse(text);
        index_.clear();
        positions_.clear();
        for (const Json &entry : entries) {
            positions_.emplace(entry[2].at("id").get<std::string>(), index_.size());
            index_.push_back(entry);
        }
        size_ = size;
        time_ = time;
    }
};

// ---- artwork (studio/artwork.py) ----------------------------------------------------------------------------

// label(value, field, limit): the text stripped, of 1 to limit characters.
std::string label(const Json &value, const std::string &field, size_t limit) {
    const auto refuse = [&] { return ValueError(field + " must contain 1\xe2\x80\x93" + std::to_string(limit) + " characters"); };
    if (!value.is_string()) throw refuse();
    const std::string &text = value.get_ref<const std::string &>();
    size_t characters = 0;
    for (const unsigned char c : text) characters += (c & 0xc0) != 0x80;
    size_t a = 0, b = text.size();
    const auto space = [](unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f); };
    while (a < b && space(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && space(static_cast<unsigned char>(text[b - 1]))) --b;
    if (a == b || characters > limit) throw refuse();
    return text.substr(a, b - a);
}

double coordinate(const Json &value) { return number(&value, "Coordinate", 0, 2000); }

// validate_paths: 1 to 3000 paths of at least two [x, y] points each, 100,000 points in all.
Json validate_paths(const Json &paths) {
    if (!paths.is_array() || paths.empty() || paths.size() > 3000) throw ValueError("Provide 1\xe2\x80\x93" "3000 paths");
    size_t count = 0;
    Json result = Json::array();
    for (const Json &path : paths) {
        if (!path.is_array() || path.size() < 2) throw ValueError("Each path needs at least two points");
        Json clean = Json::array();
        for (const Json &point : path) {
            if (!point.is_array() || point.size() != 2) throw ValueError("Each point must be [x, y]");
            Json xy = Json::array();
            xy.push_back(coordinate(point[0]));
            xy.push_back(coordinate(point[1]));
            clean.push_back(xy);
        }
        count += clean.size();
        if (count > 100000) throw ValueError("Artwork exceeds 100,000 points");
        result.push_back(clean);
    }
    return result;
}

// html.escape(text).
std::string html_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#x27;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string fixed6(double value) {
    std::string out;
    if (!text::fixed(out, value, 6)) throw ValueError("Coordinate must be between 0 and 2000");
    return out;
}

// files_for_client: each *_path also as *_artifact, its file name; over HTTP only the artifact.
Json for_client(Json result, bool remote) {
    std::vector<std::string> keys;
    for (const auto &item : result.items()) keys.emplace_back(item.key());
    for (const std::string &key : keys) {
        if (key.size() < 5 || key.compare(key.size() - 5, 5, "_path") != 0) continue;
        result[key.substr(0, key.size() - 5) + "_artifact"] = fs::u8path(result.at(key).get<std::string>()).filename().u8string();
        if (remote) result.erase(key);
    }
    return result;
}

mcp::Result value(Json v) {
    mcp::Result r;
    r.value = std::move(v);
    return r;
}

const char *const kGuide =
    "Workflow: list_templates \xe2\x86\x92 get_template \xe2\x86\x92 compose ordered paths \xe2\x86\x92 create_artwork/save_template \xe2\x86\x92 review in "
    "browser. Use preview_job before export_gcode. Separate disconnected contours; put holes first. Projects use pi-cnc-studio/1; library bundles use "
    "pi-cnc-templates/1. Use workshop_controls and workshop_action to access the full template library and parametric gear editor in a simulated "
    "workshop.";

}  // namespace

mcp::Tool declared(const std::string &name, std::function<mcp::Result(const Json &, mcp::Exchange &)> run) {
    int position = 0;
    for (const Declaration &d : kDeclarations) {
        if (name == d.name) {
            mcp::Tool tool;
            tool.name = d.name;
            tool.description = d.description;
            tool.input_schema = d.input;
            tool.output_schema = d.output;
            tool.wrapped = d.wrapped;
            tool.annotations = d.annotations;
            tool.run = std::move(run);
            tool.position = position;  // as Studio's Python server registered them, which clients list
            return tool;
        }
        ++position;
    }
    throw std::logic_error("No declaration for the tool " + name);
}

Artifacts::Artifacts(const std::string &folder) {
    std::error_code error;
    fs::create_directories(fs::u8path(folder), error);
    root_ = fs::weakly_canonical(fs::absolute(fs::u8path(folder))).u8string();
    if (!fs::is_directory(fs::u8path(root_))) throw std::runtime_error("Cannot create the output folder " + folder);
}

std::string Artifacts::write(const std::string &suffix, const std::string &content) const {
    const fs::path target = fs::u8path(root_) / (uuid4_hex() + suffix);
    create_new(target, content);
    return target.u8string();
}

Json Artifacts::create_file(const std::string &name, const std::string &content) const {
    const std::string suffix = suffix_of(name);
    if (suffix.empty() || content.size() > kLimit) throw ValueError("Supply a filename with an extension and at most 8 MiB of content.");
    const std::string identifier = uuid4_hex() + ascii_lower(suffix);
    create_new(fs::u8path(root_) / identifier, content);
    Json out = Json::object();
    out["artifact"] = identifier;
    out["name"] = last_part(name);
    out["size"] = static_cast<long long>(content.size());
    return out;
}

std::string Artifacts::file(const std::string &artifact) const {
    bool ok = artifact.size() > 32 && std::all_of(artifact.begin(), artifact.begin() + 32, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    ok = ok && suffix_of(artifact.substr(32)) == artifact.substr(32);
    if (!ok) throw ValueError("Supply the generated artifact filename from this workshop.");
    const fs::path path = fs::u8path(root_) / artifact;
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(path, error)) || !fs::is_regular_file(fs::status(path, error))) throw ValueError("Unknown workshop artifact.");
    return path.u8string();
}

std::string find_catalog(const std::string &given, const std::string &static_dir) {
    if (!given.empty()) return given;
    return (fs::u8path(static_dir).parent_path() / "builtin_templates.kkart").u8string();
}

void add_artwork_tools(mcp::Server &server, const Artifacts &artifacts, const std::string &catalog_path, bool remote) {
    auto catalog = std::make_shared<Catalog>(catalog_path);
    const Artifacts *store = &artifacts;
    server.add(declared("list_templates", [catalog](const Json &a, mcp::Exchange &) {
        // TemplateCatalog.search: the name and category hold the query, without regard to case.
        const std::string query = ascii_lower(a.at("query").get<std::string>()), category = a.at("category").get<std::string>();
        const long long limit = std::max<long long>(1, std::min<long long>(100, a.at("limit").get<long long>()));
        Json out = Json::array();
        for (const Json &entry : catalog->entries()) {
            if (static_cast<long long>(out.size()) >= limit) break;
            const Json &meta = entry[2];
            const std::string name = meta.at("name").get<std::string>(), group = meta.at("category").get<std::string>();
            if (!category.empty() && group != category) continue;
            if (ascii_lower(name + " " + group).find(query) == std::string::npos) continue;
            Json item = Json::object();
            item["id"] = meta.at("id");
            item["name"] = name;
            item["category"] = group;
            out.push_back(item);
        }
        return value(out);
    }));
    server.add(declared("get_template", [catalog](const Json &a, mcp::Exchange &) {
        const Json t = catalog->get(a.at("template_id").get<std::string>());
        if (t.is_null()) throw ValueError("Unknown template id; use list_templates");
        const double x = number(&a.at("x"), "X", 0, 2000), y = number(&a.at("y"), "Y", 0, 2000);
        const double width = number(&a.at("width"), "Width", 0.1, 2000), height = number(&a.at("height"), "Height", 0.1, 2000);
        Json paths = Json::array();
        for (const Json &path : t.at("paths")) {
            Json placed = Json::array();
            for (const Json &point : path) {
                Json xy = Json::array();
                const Json px(x + point[0].get<double>() * width / 100), py(y + point[1].get<double>() * height / 100);
                xy.push_back(coordinate(px));
                xy.push_back(coordinate(py));
                placed.push_back(xy);
            }
            paths.push_back(placed);
        }
        Json out = Json::object();
        out["name"] = t.at("name");
        out["category"] = t.at("category");
        out["paths"] = paths;
        return value(out);
    }));
    server.add(declared("create_artwork", [store, remote](const Json &a, mcp::Exchange &) {
        const std::string name = label(a.at("name"), "Name", 80);
        const Json paths = validate_paths(a.at("paths"));
        const double width = a.at("width").get<double>(), height = a.at("height").get<double>();
        Json project = Json::object();
        project["width"] = width;
        project["height"] = height;
        project["mode"] = "laser";
        project["feed"] = 600;
        project["power"] = 1000;
        project["safe"] = 5;
        project["depth"] = 1;
        project["step"] = 0.5;
        project["plunge"] = 100;
        project["passes"] = 1;
        project["lead_in"] = 0;
        project["lead_out"] = 0;
        project["start_power"] = 100;
        project["end_power"] = 100;
        project["ramp_in"] = 0;
        project["ramp_out"] = 0;
        project["paths"] = paths;
        generate(project);  // the same bounds and parameter checks as the editor
        Json object = Json::object();
        object["name"] = name;
        object["paths"] = paths;
        Json document = Json::object();
        document["format"] = "pi-cnc-studio/1";
        document["settings"] = project;
        Json objects = Json::array();
        objects.push_back(object);
        document["objects"] = objects;
        const std::string w = text::repr(width), h = text::repr(height);
        std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + w + "mm\" height=\"" + h + "mm\" viewBox=\"0 0 " + w + " " + h + "\"><title>" +
                          html_escape(name) + "</title>";
        // Separate paths keep the drawing's breaks for the editor's importer.
        for (const Json &path : paths) {
            svg += "<path fill=\"none\" stroke=\"black\" d=\"M";
            for (size_t i = 0; i < path.size(); ++i)
                svg += (i ? " L" : "") + fixed6(path[i][0].get<double>()) + "," + fixed6(height - path[i][1].get<double>());
            svg += "\"/>";
        }
        svg += "</svg>";
        Json out = Json::object();
        out["project_path"] = store->write(".cncstudio.json", document.dump());
        out["svg_path"] = store->write(".svg", svg);
        out["object"] = object;
        out["note"] = "Open the project in KerfKite Studio. Defaults are placeholders; review operation settings. No machine commands sent.";
        return value(for_client(out, remote));
    }));
    server.add(declared("save_template", [store, remote](const Json &a, mcp::Exchange &) {
        const std::string name = label(a.at("name"), "Name", 80), category = label(a.at("category"), "Category", 40);
        const Json paths = validate_paths(a.at("paths"));
        double minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;
        for (const Json &path : paths)
            for (const Json &p : path) {
                minx = std::min(minx, p[0].get<double>());
                miny = std::min(miny, p[1].get<double>());
                maxx = std::max(maxx, p[0].get<double>());
                maxy = std::max(maxy, p[1].get<double>());
            }
        const double w = maxx - minx, h = maxy - miny;
        Json normalized = Json::array();
        for (const Json &path : paths) {
            Json out_path = Json::array();
            for (const Json &p : path) {
                Json xy = Json::array();
                xy.push_back(100 * (p[0].get<double>() - minx) / (w != 0 ? w : 1));
                xy.push_back(100 * (p[1].get<double>() - miny) / (h != 0 ? h : 1));
                out_path.push_back(xy);
            }
            normalized.push_back(out_path);
        }
        Json t = Json::object();
        t["id"] = "custom-" + uuid4_hex();
        t["name"] = name;
        t["category"] = category;
        t["paths"] = normalized;
        Json bundle = Json::object();
        bundle["format"] = "pi-cnc-templates/1";
        Json custom = Json::array();
        custom.push_back(t);
        bundle["custom"] = custom;
        bundle["favorites"] = Json::array();
        Json out = Json::object();
        out["library_path"] = store->write(".templates.json", bundle.dump());
        out["template"] = t;
        out["note"] = "Import this file using Template library \xe2\x86\x92 Import library. Browser storage is separate from MCP artifacts.";
        return value(for_client(out, remote));
    }));
    server.add(declared("preview_job", [](const Json &a, mcp::Exchange &) {
        const Job job = generate(a.at("project"));
        const Json result = to_json(job);
        Json out = Json::object();
        for (const char *key : {"bounds", "cut_length", "cut_minutes", "warnings"}) out[key] = result.at(key);
        out["line_count"] = static_cast<long long>(job.lines.size());
        Json first = Json::array();
        for (size_t i = 0; i < job.lines.size() && i < 25; ++i) first.push_back(job.lines[i]);
        out["first_lines"] = first;
        return value(out);
    }));
    server.add(declared("export_gcode", [store, remote](const Json &a, mcp::Exchange &) {
        const Job job = generate(a.at("project"));
        const Json result = to_json(job);
        std::string program;
        for (const std::string &line : job.lines) program += line + "\n";
        if (job.lines.empty()) program = "\n";
        Json out = Json::object();
        out["gcode_path"] = store->write(".nc", program);
        out["bounds"] = result.at("bounds");
        out["cut_minutes"] = result.at("cut_minutes");
        out["warnings"] = result.at("warnings");
        out["line_count"] = static_cast<long long>(job.lines.size());
        out["note"] = "Review only; no physical machine commands were sent.";
        return value(for_client(out, remote));
    }));
    mcp::Resource guide;
    guide.uri = "cnc-studio://artwork-guide";
    guide.name = "guide";
    guide.mime_type = "text/plain";
    guide.read = [] { return std::string(kGuide); };
    server.add(guide);
}

// ---- Studio's design calculations, in this process ---------------------------------------------------------

namespace {

// The design routes, collected as add_design_routes registers them.
class DesignRoutes : public RouteTable {
public:
    explicit DesignRoutes(const std::string &step_program) : step_(std::make_shared<StepExchange>(step_program)) {
        use_step_mesher(step_);
        add_design_routes(*this);
    }
    void add(const std::string &method, const std::string &path, Route route) override { routes[path.substr(5)] = {method, std::move(route)}; }
    StepExchange &step() override { return *step_; }
    std::map<std::string, std::pair<std::string, Route>> routes;  // by name (the path without /api/), method and route

private:
    std::shared_ptr<StepExchange> step_;
};

// What each action takes, for design_actions.
const std::map<std::string, std::string> kFields = {
    {"cad-script", "source, parameters?, overrides?"},
    {"solid-cad", "model"},
    {"solid-cad/sketch-points", "points, parameters?, constraints?"},
    {"solid-cad/section", "model, z"},
    {"solid-cad/stl", "model"},
    {"solid-cad/surface", "surface"},
    {"solid-cad/step/import", "content"},
    {"solid-cad/step/export", "model or surface"},
    {"solid-cad/exchange-stl", "surface or model"},
    {"geometry", "action, objects, distance?"},
    {"rotary", "project, options"},
    {"tool-changes", "project"},
    {"simulate", "segments, width, height, stock_thickness, tool_diameter, resolution?"},
    {"assembly/solve", "assembly, mesh?"},
    {"assembly/example", "no inputs"},
    {"fixture-collision", "lines, start (XYZ), fixtures, tool"},
    {"protocols", "no inputs"},
    {"relief", "heightmap, width, height, depth, tool_diameter, tool_shape, stepover"},
    {"extrude", "paths, height"},
    {"artwork-relief", "paths, width, height and relief settings"},
    {"pdf-import", "content (base64 PDF), page?"},
    {"program-import", "name, text, unit?"},
    {"import-bundle", "content (base64 ZIP), index? to select an entry"},
};

// str.casefold for the action names and their inputs, which are ASCII.
std::string folded(const std::string &text) { return ascii_lower(text); }

}  // namespace

std::function<Json(const std::string &, const Json &)> add_design_tools(mcp::Server &server, const Artifacts &artifacts, const std::string &step_program) {
    auto design = std::make_shared<DesignRoutes>(step_program);
    const Artifacts *store = &artifacts;
    // design_call(action, payload): small JSON inline, anything larger or binary as an artifact.
    auto call = [design, store](const std::string &action, const Json &given) -> Json {
        const auto found = design->routes.find(action);
        if (found == design->routes.end()) throw ValueError("Unknown action. Use design_actions to list supported names.");
        const Json payload = given.is_null() || (given.is_object() && given.empty()) ? Json::object() : given;
        const std::string body = payload.dump();
        if (body.size() > kLimit) throw ValueError("Payload exceeds 8 MiB.");
        http::Request request;
        request.method = found->second.first;
        request.target = request.path = "/api/" + found->first;
        request.headers.emplace_back("content-type", "application/json");
        request.body = body;
        http::Response response;
        try {
            response = found->second.second(request);
        } catch (const HttpError &e) {
            throw ValueError("This design operation returned HTTP " + std::to_string(e.status));
        } catch (const std::exception &e) {
            throw ValueError(e.what());
        }
        if (response.status != 200) throw ValueError("This design operation returned HTTP " + std::to_string(response.status));
        std::string type, disposition;
        for (const auto &h : response.headers) {
            const std::string name = ascii_lower(h.first);
            if (name == "content-type") type = h.second;
            if (name == "content-disposition") disposition = h.second;
        }
        const std::string media = ascii_lower(type.substr(0, type.find(';')));
        const bool json = media == "application/json";
        if (json && response.body.size() <= 24576) {
            Json out = Json::object();
            out["result"] = Json::parse(response.body);
            return out;
        }
        std::string filename;
        const size_t at = disposition.rfind("filename=");
        if (at != std::string::npos) {
            filename = disposition.substr(at + 9);
            while (!filename.empty() && filename.front() == '"') filename.erase(0, 1);
            while (!filename.empty() && filename.back() == '"') filename.pop_back();
        }
        if (filename.empty()) filename = json ? "result.json" : media == "model/stl" ? "part.stl" : "result.bin";
        return store->create_file(filename, response.body);
    };
    server.add(declared("design_actions", [design](const Json &a, mcp::Exchange &) {
        const std::string query = folded(a.at("query").get<std::string>());
        Json out = Json::array();
        for (const auto &route : design->routes) {
            const auto field = kFields.find(route.first);
            const std::string inputs = field == kFields.end() ? "Studio JSON payload" : field->second;
            if (folded(route.first + " " + inputs).find(query) == std::string::npos) continue;
            Json item = Json::object();
            item["action"] = route.first;
            item["method"] = route.second.first;
            item["inputs"] = inputs;
            out.push_back(item);
        }
        return value(out);
    }));
    server.add(declared("design_call", [call](const Json &a, mcp::Exchange &) { return value(call(a.at("action").get<std::string>(), a.at("payload"))); }));
    return call;
}

void add_artifact_tools(mcp::Server &server, const Artifacts &artifacts) {
    const Artifacts *store = &artifacts;
    server.add(declared("workshop_upload", [store](const Json &a, mcp::Exchange &) {
        const std::string &text = a.at("content_base64").get_ref<const std::string &>();
        if (text.size() > 4 * ((kLimit + 2) / 3)) throw ValueError("Upload exceeds 8 MiB.");
        // base64.b64decode(validate=True): the standard alphabet, padded to whole quanta.
        std::string bytes;
        unsigned v = 0;
        int bits = 0;
        size_t padding = 0;
        bool ok = text.size() % 4 == 0;
        for (size_t i = 0; ok && i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            int d = -1;
            if (c >= 'A' && c <= 'Z') d = c - 'A';
            else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
            else if (c >= '0' && c <= '9') d = c - '0' + 52;
            else if (c == '+') d = 62;
            else if (c == '/') d = 63;
            else if (c == '=' && i + 2 >= text.size()) {
                ++padding;
                continue;
            }
            if (d < 0 || padding) {
                ok = false;
                break;
            }
            v = (v << 6) | static_cast<unsigned>(d);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                bytes += static_cast<char>((v >> bits) & 0xff);
            }
        }
        if (!ok || padding > 2) throw ValueError("Supply standard base64 file content.");
        return value(store->create_file(a.at("name").get<std::string>(), bytes));
    }));
    server.add(declared("workshop_read_file", [store](const Json &a, mcp::Exchange &) {
        const std::string artifact = a.at("artifact").get<std::string>();
        const std::string path = store->file(artifact);
        std::error_code error;
        const uintmax_t size = fs::file_size(fs::u8path(path), error);
        const long long offset = a.at("offset").get<long long>(), length = a.at("length").get<long long>();
        if (offset < 0 || static_cast<uintmax_t>(offset) > size || length < 1 || length > 131072)
            throw ValueError("Use a valid byte offset and a chunk length from 1 to 131072.");
        std::ifstream in(fs::u8path(path), std::ios::binary);
        in.seekg(offset);
        std::string data(static_cast<size_t>(length), '\0');
        in.read(&data[0], length);
        data.resize(static_cast<size_t>(in.gcount()));
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        for (size_t i = 0; i < data.size(); i += 3) {
            unsigned w = static_cast<unsigned char>(data[i]) << 16;
            if (i + 1 < data.size()) w |= static_cast<unsigned char>(data[i + 1]) << 8;
            if (i + 2 < data.size()) w |= static_cast<unsigned char>(data[i + 2]);
            encoded += alphabet[(w >> 18) & 63];
            encoded += alphabet[(w >> 12) & 63];
            encoded += i + 1 < data.size() ? alphabet[(w >> 6) & 63] : '=';
            encoded += i + 2 < data.size() ? alphabet[w & 63] : '=';
        }
        Json out = Json::object();
        out["artifact"] = artifact;
        out["offset"] = offset;
        out["next"] = offset + static_cast<long long>(data.size());
        out["size"] = static_cast<long long>(size);
        out["content_base64"] = encoded;
        return value(out);
    }));
}

}  // namespace server
}  // namespace kerfkite
