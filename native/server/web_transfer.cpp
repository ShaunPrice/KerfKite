// The hosted editor's uploads and receivers. See web_transfer.hpp.
#include "web_transfer.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <vector>

#include "child_process.hpp"
#include "kerfkite/errors.hpp"
#include "os_crypto.hpp"

namespace kerfkite {
namespace server {
namespace {

const char *const kExtensions[] = {".nc", ".gcode", ".svg", ".cncstudio.json", ".templates.json", ".paths3d.json"};
const char *const kUnverified =
    "Upload result could not be verified. Check the receiver before retrying; verify its URL, token and TLS trust.";
const size_t kMaxAnswer = 65536;

std::string ascii_lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool ends_with(const std::string &text, const std::string &end) {
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

size_t code_points(const std::string &text) {
    size_t count = 0;
    for (const unsigned char c : text) count += (c & 0xc0) != 0x80;
    return count;
}

// Python's truth of a JSON value.
bool truthy(const Json *value) {
    if (!value || value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_number()) return value->get<double>() != 0;
    if (value->is_string()) return !value->get_ref<const std::string &>().empty();
    if (value->is_array() || value->is_object()) return !value->empty();
    return true;
}

const Json *member(const Json &object, const char *name) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

// ipaddress.IPv4Address's text: four decimal octets of up to three digits, no leading zeros, each at most 255.
bool is_ipv4(const std::string &text) {
    int parts = 0;
    for (size_t at = 0;;) {
        const size_t dot = text.find('.', at);
        const std::string part = text.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
        if (part.empty() || part.size() > 3 || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            (part.size() > 1 && part[0] == '0') || std::stoi(part) > 255)
            return false;
        ++parts;
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
    return parts == 4;
}

// ipaddress.IPv6Address's text (its _ip_int_from_string): hextets of one to four hexadecimal digits, one "::"
// at most, an IPv4 address in place of the last two if wanted, and a scope after '%'.
bool is_ipv6(std::string text) {
    const size_t percent = text.find('%');
    if (percent != std::string::npos) {
        const std::string scope = text.substr(percent + 1);
        if (scope.empty() || scope.find('%') != std::string::npos) return false;
        text.resize(percent);
    }
    if (text.empty()) return false;
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
        const size_t colon = text.find(':', at);
        parts.push_back(text.substr(at, colon == std::string::npos ? std::string::npos : colon - at));
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    if (parts.size() < 3) return false;
    if (parts.back().find('.') != std::string::npos) {
        if (!is_ipv4(parts.back())) return false;
        parts.back() = "0";
        parts.push_back("0");
    }
    if (parts.size() > 9) return false;
    size_t skip = 0;
    bool skipped = false;
    for (size_t i = 1; i + 1 < parts.size(); ++i)
        if (parts[i].empty()) {
            if (skipped) return false;
            skipped = true;
            skip = i;
        }
    size_t high = parts.size(), low = 0;
    if (skipped) {
        high = skip;
        low = parts.size() - skip - 1;
        if (parts.front().empty() && --high) return false;
        if (parts.back().empty() && --low) return false;
        if (8 < high + low + 1) return false;
    } else if (parts.size() != 8) {
        return false;
    }
    const auto hextet = [](const std::string &part) {
        return !part.empty() && part.size() <= 4 && std::all_of(part.begin(), part.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    };
    for (size_t i = 0; i < high; ++i)
        if (!hextet(parts[i])) return false;
    for (size_t i = parts.size() - low; i < parts.size(); ++i)
        if (!hextet(parts[i])) return false;
    return true;
}

// urllib.parse._check_bracketed_host.
void check_bracketed_host(const std::string &host) {
    if (!host.empty() && host[0] == 'v') {
        // \Av[a-fA-F0-9]+\..+\Z
        size_t at = 1;
        while (at < host.size() && std::isxdigit(static_cast<unsigned char>(host[at]))) ++at;
        if (at == 1 || at >= host.size() || host[at] != '.' || at + 1 >= host.size()) throw ValueError("IPvFuture address is invalid");
        return;
    }
    if (is_ipv4(host)) throw ValueError("An IPv4 address cannot be in brackets");
    if (!is_ipv6(host)) throw ValueError("'" + host + "' does not appear to be an IPv4 or IPv6 address");
}

std::string base64_decode(const std::string &text, bool &ok) {
    std::string out;
    unsigned value = 0;
    int bits = 0;
    ok = true;
    size_t padding = 0;
    for (const unsigned char c : text) {
        int digit;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+') digit = 62;
        else if (c == '/') digit = 63;
        else if (c == '=') {
            ++padding;
            continue;
        } else {
            ok = false;
            return std::string();
        }
        if (padding) {
            ok = false;
            return std::string();
        }
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((value >> bits) & 0xff);
        }
    }
    if ((text.size() % 4) != 0) ok = false;
    return out;
}

}  // namespace

UrlParts url_split(const std::string &given) {
    UrlParts parts;
    size_t start = 0;
    while (start < given.size() && static_cast<unsigned char>(given[start]) <= 0x20) ++start;
    std::string url;
    for (size_t i = start; i < given.size(); ++i)
        if (given[i] != '\t' && given[i] != '\r' && given[i] != '\n') url += given[i];
    const size_t colon = url.find(':');
    if (colon != std::string::npos && colon > 0 && std::isalpha(static_cast<unsigned char>(url[0])) &&
        static_cast<unsigned char>(url[0]) < 0x80) {
        const bool scheme = std::all_of(url.begin(), url.begin() + static_cast<std::ptrdiff_t>(colon), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        });
        if (scheme) {
            parts.scheme = ascii_lower(url.substr(0, colon));
            url = url.substr(colon + 1);
        }
    }
    if (url.compare(0, 2, "//") == 0) {
        size_t end = url.size();
        for (const char c : {'/', '?', '#'}) {
            const size_t at = url.find(c, 2);
            if (at != std::string::npos) end = std::min(end, at);
        }
        parts.netloc = url.substr(2, end - 2);
        url = url.substr(end);
        const bool open = parts.netloc.find('[') != std::string::npos, close = parts.netloc.find(']') != std::string::npos;
        if (open != close) throw ValueError("Invalid IPv6 URL");
        if (open) {
            const std::string host_and_port = parts.netloc.substr(parts.netloc.rfind('@') == std::string::npos ? 0 : parts.netloc.rfind('@') + 1);
            const size_t bracket = host_and_port.find('[');
            std::string host;
            if (bracket != std::string::npos) {
                if (bracket > 0) throw ValueError("Invalid IPv6 URL");
                const size_t shut = host_and_port.find(']', bracket + 1);
                host = host_and_port.substr(bracket + 1, shut == std::string::npos ? std::string::npos : shut - bracket - 1);
                const std::string after = shut == std::string::npos ? std::string() : host_and_port.substr(shut + 1);
                if (!after.empty() && after[0] != ':') throw ValueError("Invalid IPv6 URL");
            } else {
                host = host_and_port.substr(0, host_and_port.find(':'));
            }
            check_bracketed_host(host);
        }
    }
    const size_t hash = url.find('#');
    if (hash != std::string::npos) {
        parts.fragment = url.substr(hash + 1);
        url.resize(hash);
    }
    const size_t question = url.find('?');
    if (question != std::string::npos) {
        parts.query = url.substr(question + 1);
        url.resize(question);
    }
    parts.path = url;
    // hostname, port, username and password, as SplitResult reads them from netloc.
    const size_t at = parts.netloc.rfind('@');
    const std::string info = at == std::string::npos ? parts.netloc : parts.netloc.substr(at + 1);
    std::string host, port;
    const size_t bracket = info.find('[');
    if (bracket != std::string::npos) {
        const std::string bracketed = info.substr(bracket + 1);
        const size_t shut = bracketed.find(']');
        host = bracketed.substr(0, shut);
        const std::string after = shut == std::string::npos ? std::string() : bracketed.substr(shut + 1);
        const size_t c = after.find(':');
        port = c == std::string::npos ? std::string() : after.substr(c + 1);
    } else {
        const size_t c = info.find(':');
        host = info.substr(0, c);
        port = c == std::string::npos ? std::string() : info.substr(c + 1);
    }
    if (!host.empty()) {
        parts.has_hostname = true;
        const size_t percent = host.find('%');
        parts.hostname = ascii_lower(host.substr(0, percent)) + (percent == std::string::npos ? std::string() : host.substr(percent));
    }
    parts.has_port = !port.empty();
    parts.port = port;
    if (at != std::string::npos) {
        parts.has_user = true;
        const std::string user = parts.netloc.substr(0, at);
        const size_t c = user.find(':');
        parts.username = user.substr(0, c);
        parts.password = c == std::string::npos ? std::string() : user.substr(c + 1);
    }
    return parts;
}

bool simple_file_name(const std::string &name) {
    if (name.empty()) return true;  // secure_filename('') is '': the extension check refuses it
    for (const unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    if (name.front() == '.' || name.front() == '_' || name.back() == '.' || name.back() == '_') return false;
#ifdef _WIN32
    std::string stem = name.substr(0, name.find('.'));
    for (char &c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '0' && stem[3] <= '9') return false;
#endif
    return true;
}

Upload validate_upload(const Json &data) {
    if (!data.is_object()) throw ValueError("Upload must be a JSON object");
    const Json *name = member(data, "name");
    const Json *content = member(data, "content");
    if (name && !name->is_string()) throw ValueError("Use a simple file name without folders");
    const std::string given = name ? name->get<std::string>() : std::string();
    if (code_points(given) > 128 || !simple_file_name(given)) throw ValueError("Use a simple file name without folders");
    const std::string lowered = ascii_lower(given);
    if (!std::any_of(std::begin(kExtensions), std::end(kExtensions), [&](const char *e) { return ends_with(lowered, e); }))
        throw ValueError("Supported files: .nc, .gcode, .svg, .cncstudio.json, .templates.json, .paths3d.json");
    if (!content || !content->is_string() || content->get_ref<const std::string &>().find('\0') != std::string::npos)
        throw ValueError("Upload must contain text, without NUL characters");
    Upload upload;
    upload.name = given;
    upload.content = content->get<std::string>();
    if (upload.content.empty() || upload.content.size() > 8 * 1024 * 1024) throw ValueError("File must contain 1 byte to 8 MB");
    const Json *expected = member(data, "sha256");
    if (truthy(expected) && !(expected->is_string() && expected->get_ref<const std::string &>() == sha256_hex(upload.content)))
        throw ValueError("Upload checksum mismatch");
    return upload;
}

void validate_destination(const Json &entry) {
    const char *origin = "Destination must be an HTTP/S server origin, without credentials, path or query";
    const Json *url = member(entry, "url");
    if (!entry.is_object() || (url && !url->is_string())) throw ValueError(origin);
    const UrlParts parts = url_split(url ? url->get<std::string>() : std::string());
    if ((parts.scheme != "https" && parts.scheme != "http") || !parts.has_hostname || !parts.username.empty() || !parts.password.empty() ||
        !parts.fragment.empty() || !parts.query.empty() || (!parts.path.empty() && parts.path != "/"))
        throw ValueError(origin);
    if (parts.scheme == "http" && !truthy(member(entry, "allow_http"))) throw ValueError("Plain HTTP needs explicit --allow-http; HTTPS is the default");
    const Json *token = member(entry, "token");
    if (!token || !token->is_string() || code_points(token->get<std::string>()) < 32) throw ValueError("Configure the receiver-generated upload token");
    const Json *ca = member(entry, "ca_file");
    if (truthy(ca)) {
        std::error_code error;
        if (!ca->is_string() || !std::filesystem::is_regular_file(std::filesystem::u8path(ca->get<std::string>()), error))
            throw ValueError("CA certificate file does not exist");
    }
}

Json send_to_receiver(const Json &entry, const Upload &upload, const std::string &relay) {
    validate_destination(entry);
    if (relay.empty() || !file_exists(relay))
        throw ValueError("Sending to a receiver needs KerfKite's TLS relay (kerfkite-pendant-tls), which this installation does not include");
    std::string url = entry.at("url").get<std::string>();
    while (!url.empty() && static_cast<unsigned char>(url.back()) <= 0x20) url.pop_back();
    size_t first = 0;
    while (first < url.size() && static_cast<unsigned char>(url[first]) <= 0x20) ++first;
    url = url.substr(first);
    while (!url.empty() && url.back() == '/') url.pop_back();
    const std::string sha = sha256_hex(upload.content);
    Json payload = Json::object();
    payload["name"] = upload.name;
    payload["content"] = upload.content;
    payload["sha256"] = sha;
    Json request = Json::object();
    request["method"] = "POST";
    request["url"] = url + "/api/receive";
    Json headers = Json::object();
    headers["Authorization"] = "Bearer " + entry.at("token").get<std::string>();
    headers["Content-Type"] = "application/json";
    request["headers"] = headers;
    request["body"] = payload.dump();
    const Json *ca = member(entry, "ca_file");
    request["ca_file"] = truthy(ca) ? ca->get<std::string>() : std::string();
    request["timeout_seconds"] = 15;
    request["limit"] = static_cast<long long>(kMaxAnswer + 1);
    Json answer;
    try {
        // The token travels on the relay's standard input, never on its command line.
        ChildProcess fetch(relay, {"-fetch"});
        const bool sent = fetch.write(request.dump() + "\n");
        fetch.close_input();
        std::string line;
        if (!sent || !fetch.readline(line, 25.0)) throw ValueError(kUnverified);
        answer = Json::parse(line);
    } catch (const ValueError &) {
        throw;
    } catch (const std::exception &) {
        throw ValueError(kUnverified);
    }
    const Json *problem = member(answer, "error");
    if (problem && problem->is_string() && problem->get<std::string>() == "ca_file") throw ValueError("The receiver's CA certificate file holds no certificate");
    const Json *status = member(answer, "status");
    const Json *body = member(answer, "body");
    if (problem || !status || !status->is_number_integer() || status->get<long long>() < 200 || status->get<long long>() >= 300 || !body ||
        !body->is_string())
        throw ValueError(kUnverified);
    bool ok = false;
    std::string raw = base64_decode(body->get<std::string>(), ok);
    if (!ok || raw.size() > kMaxAnswer) throw ValueError(kUnverified);
    if (raw.compare(0, 3, "\xef\xbb\xbf") == 0) raw.erase(0, 3);  // json.loads reads bytes with a UTF-8 signature
    Json result;
    try {
        result = Json::parse(raw);
    } catch (const std::exception &) {
        throw ValueError(kUnverified);
    }
    const Json *bytes = member(result, "bytes");
    const double size = static_cast<double>(upload.content.size());
    const bool same_size = bytes && ((bytes->is_number() && bytes->get<double>() == size) || (bytes->is_boolean() && (bytes->get<bool>() ? 1.0 : 0.0) == size));
    const Json *digest = member(result, "sha256");
    const Json *stored = member(result, "status");
    if (!result.is_object() || !digest || !digest->is_string() || digest->get<std::string>() != sha || !same_size || !stored ||
        !stored->is_string() || stored->get<std::string>() != "stored_for_review")
        throw ValueError("Receiver did not confirm the expected file checksum and size; inspect it before retrying");
    Json out = Json::object();
    const Json *id = member(result, "id");
    out["id"] = id ? *id : Json();
    out["name"] = upload.name;
    out["sha256"] = sha;
    out["bytes"] = static_cast<long long>(upload.content.size());
    out["status"] = "stored_for_review";
    return out;
}

}  // namespace server
}  // namespace kerfkite
