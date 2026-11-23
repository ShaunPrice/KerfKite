// kerfkite-web: KerfKite Studio's hosted editor and upload-only machine inbox (studio/web_server.py's main), with
// its accounts, upload tokens, receivers and certificate. It has no controller.
//
//   kerfkite-web [--data-dir DIR] create-user|reset-password USERNAME [--generate-password]
//   kerfkite-web [--data-dir DIR] issue-token|revoke-tokens USERNAME
//   kerfkite-web certificate --cert FILE --key FILE --hosts HOST [HOST ...]
//   kerfkite-web [--data-dir DIR] destination NAME --url URL [--ca-file FILE] [--allow-http]
//   kerfkite-web [--data-dir DIR] serve --public-url URL --cert FILE --key FILE [--host H] [--port N] [--allow-file-upload]
//   kerfkite-web [--data-dir DIR] serve --public-url URL --behind-proxy [--port N] [--allow-file-upload]
//
// serve listens with TLS 1.2 or later through KerfKite's TLS relay (kerfkite-pendant-tls, beside this program),
// which passes each connection to this server on the loopback interface. With --behind-proxy it listens on
// 127.0.0.1 for an operator's own TLS terminator instead, which must send the PROXY protocol (version 1) line.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "child_process.hpp"
#include "http.hpp"
#include "kerfkite/errors.hpp"
#include "key_order.hpp"
#include "os_random.hpp"
#include "static_files.hpp"
#include "web_app.hpp"
#include "web_store.hpp"
#include "web_transfer.hpp"

namespace {

namespace fs = std::filesystem;
using kerfkite::Json;
using namespace kerfkite::server;

const char *const kUsage =
    "usage: kerfkite-web [-h] [--data-dir DATA_DIR] [--static DIR] [--step PROGRAM] [--tls-relay PROGRAM]\n"
    "                    {create-user,reset-password,issue-token,revoke-tokens,certificate,destination,serve} ...\n";

[[noreturn]] void fail(const std::string &message) {
    std::fputs(kUsage, stderr);
    std::fprintf(stderr, "kerfkite-web: error: %s\n", message.c_str());
    std::exit(2);
}

// getpass: a line from the terminal without echo, or from standard input when it is not a terminal.
std::string secret(const char *prompt) {
    std::fputs(prompt, stderr);
    std::fflush(stderr);
    std::string line;
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode)) {
        SetConsoleMode(in, (mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        std::wstring wide;
        wchar_t buffer[512];
        DWORD got = 0;
        while (ReadConsoleW(in, buffer, 512, &got, nullptr) && got > 0) {
            wide.append(buffer, got);
            if (wide.find(L'\n') != std::wstring::npos) break;
        }
        SetConsoleMode(in, mode);
        std::fputs("\n", stderr);
        const size_t end = wide.find_first_of(L"\r\n");
        if (end != std::wstring::npos) wide.resize(end);
        if (!wide.empty()) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            line.resize(static_cast<size_t>(size));
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &line[0], size, nullptr, nullptr);
        }
        return line;
    }
#else
    termios saved{};
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        termios quiet = saved;
        quiet.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
        std::getline(std::cin, line);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        std::fputs("\n", stderr);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }
#endif
    std::getline(std::cin, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::string read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// destinations.json: the receivers by name, and their names in the order the operator added them.
Json destinations(const std::string &root, std::vector<std::string> &names) {
    const fs::path path = fs::u8path(root) / "destinations.json";
    names.clear();
    if (!fs::exists(path)) return Json::object();
    const std::string text = read_text(path);
    Json entries = Json::parse(text);
    if (!entries.is_object()) throw kerfkite::ValueError("destinations.json must hold an object of receivers");
    names = key_order(text).keys;
    return entries;
}

void write_private(const fs::path &path, const std::string &text) {
#ifdef _WIN32
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) throw std::runtime_error("Cannot write " + path.u8string());
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot write " + path.u8string());
    size_t at = 0;
    while (at < text.size()) {
        const ssize_t wrote = ::write(fd, text.data() + at, text.size() - at);
        if (wrote <= 0) break;
        at += static_cast<size_t>(wrote);
    }
    ::close(fd);
    if (at != text.size()) throw std::runtime_error("Cannot write " + path.u8string());
#endif
}

std::string beside(const char *argv0, const char *name) {
#ifdef _WIN32
    const std::string program = std::string(name) + ".exe";
#else
    const std::string program = name;
#endif
    const std::string path = program_directory(argv0) + "/" + program;
    return file_exists(path) ? path : std::string();
}

struct Arguments {
    std::string action;
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;
    std::vector<std::string> hosts;
    std::set<std::string> flags;
};

}  // namespace

int main(int argc, char **argv) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::string data_dir = "web-data", static_dir, step_program, relay;
    Arguments a;
    const std::set<std::string> valued = {"--cert", "--key", "--url", "--ca-file", "--public-url", "--host", "--port"};
    const std::set<std::string> switches = {"--generate-password", "--allow-http", "--allow-file-upload", "--behind-proxy"};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) fail("argument " + arg + ": expected one argument");
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (a.action.empty()) {
            if (arg == "--data-dir") data_dir = value();
            else if (arg == "--static") static_dir = value();
            else if (arg == "--step") step_program = value();
            else if (arg == "--tls-relay") relay = value();
            else if (!arg.empty() && arg[0] == '-') fail("unrecognized arguments: " + arg);
            else a.action = arg;
            continue;
        }
        if (arg == "--hosts") {
            while (i + 1 < argc && argv[i + 1][0] != '-') a.hosts.push_back(argv[++i]);
            if (a.hosts.empty()) fail("argument --hosts: expected at least one argument");
        } else if (valued.count(arg)) {
            a.values[arg] = value();
        } else if (switches.count(arg)) {
            a.flags.insert(arg);
        } else if (!arg.empty() && arg[0] == '-') {
            fail("unrecognized arguments: " + arg);
        } else {
            a.positional.push_back(arg);
        }
    }
    static const std::set<std::string> actions = {"create-user", "reset-password", "issue-token", "revoke-tokens", "certificate", "destination", "serve"};
    if (a.action.empty()) fail("the following arguments are required: action");
    if (!actions.count(a.action)) fail("argument action: invalid choice: '" + a.action + "'");
    const auto need = [&](const char *option) {
        if (!a.values.count(option)) fail(std::string("the following arguments are required: ") + option);
        return a.values.at(option);
    };
    const bool named = a.action != "certificate" && a.action != "serve";
    if (named && a.positional.size() != 1) fail(a.positional.empty() ? "the following arguments are required: username" : "unrecognized arguments: " + a.positional[1]);
    if (!named && !a.positional.empty()) fail("unrecognized arguments: " + a.positional[0]);
    if (relay.empty()) relay = beside(argv[0], "kerfkite-pendant-tls");
    if (step_program.empty()) step_program = beside(argv[0], "kerfkite-step");
    try {
        if (a.action == "certificate") {
            const std::string cert = need("--cert"), key = need("--key");
            if (a.hosts.empty()) fail("the following arguments are required: --hosts");
            std::error_code error;
            if (fs::exists(fs::u8path(cert), error) || fs::exists(fs::u8path(key), error))
                throw kerfkite::ValueError("Certificate/key already exists; refusing to overwrite");
            if (relay.empty()) throw kerfkite::ValueError("Making a certificate needs KerfKite's TLS relay (kerfkite-pendant-tls); give it with --tls-relay");
            std::string hosts;
            for (const std::string &h : a.hosts) hosts += (hosts.empty() ? "" : ",") + h;
            ChildProcess maker(relay, {"-make-cert", "-cert", cert, "-key", key, "-hosts", hosts, "-days", "90"});
            std::string line;
            if (!maker.readline(line, 30.0) || line != "certificate made") throw kerfkite::ValueError("The certificate could not be made");
            return 0;
        }
        WebStore store(data_dir);
        if (a.action == "create-user" || a.action == "reset-password") {
            const bool generated = a.flags.count("--generate-password") > 0;
            std::string password;
            if (generated) {
                password = token_urlsafe(32);
            } else {
                password = secret("Password (15\xe2\x80\x93" "256 characters): ");
                if (password != secret("Repeat password: ")) throw kerfkite::ValueError("Passwords did not match");
            }
            store.add_user(a.positional[0], password, a.action == "reset-password");
            std::printf("Account configured.\n");
            if (generated) std::printf("Save this generated password now: %s\n", password.c_str());
        } else if (a.action == "issue-token") {
            std::printf("%s\n", store.issue_token(a.positional[0]).c_str());
        } else if (a.action == "revoke-tokens") {
            store.revoke_tokens(a.positional[0]);
        } else if (a.action == "destination") {
            std::vector<std::string> names;
            Json entries = destinations(store.root(), names);
            Json entry = Json::object();
            entry["url"] = need("--url");
            if (a.values.count("--ca-file")) entry["ca_file"] = fs::weakly_canonical(fs::absolute(fs::u8path(a.values.at("--ca-file")))).u8string();
            else entry["ca_file"] = Json();
            entry["allow_http"] = a.flags.count("--allow-http") > 0;
            entry["token"] = secret("Receiver upload token: ");
            validate_destination(entry);
            const std::string &name = a.positional[0];
            if (!entries.contains(name)) names.push_back(name);
            entries[name] = entry;
            std::string text = "{";
            for (size_t i = 0; i < names.size(); ++i) text += (i ? ", " : "") + Json(names[i]).dump() + ": " + entries.at(names[i]).dump();
            text += "}";
            write_private(fs::u8path(store.root()) / "destinations.json", text);
        } else {  // serve
            WebOptions options;
            options.public_url = need("--public-url");
            options.allow_file_upload = a.flags.count("--allow-file-upload") > 0;
            options.static_dir = find_static_dir(static_dir, argv[0]);
            options.step_program = step_program;
            options.relay = relay;
            const bool behind = a.flags.count("--behind-proxy") > 0;
            const std::string host = a.values.count("--host") ? a.values.at("--host") : behind ? "127.0.0.1" : "0.0.0.0";
            const int port = a.values.count("--port") ? std::atoi(a.values.at("--port").c_str()) : 8443;
            std::string cert, key;
            if (!behind) {
                cert = need("--cert");
                key = need("--key");
            } else if (host != "127.0.0.1") {
                throw kerfkite::ValueError("--behind-proxy listens on 127.0.0.1 only: the PROXY protocol line names the client, so only the TLS terminator may connect");
            }
            std::vector<std::string> names;
            const Json entries = destinations(store.root(), names);
            WebApp app(store, options, entries, names);
            const auto handler = [&](const kerfkite::http::Request &r) { return app.handle(r); };
            const auto limit = [&](const kerfkite::http::Request &r) { return app.body_limit(r); };
            if (behind) {
                kerfkite::http::Server server(host, port);
                std::printf("HTTPS editor and upload inbox: %s\n", options.public_url.c_str());
                std::printf("Listening for the TLS terminator (PROXY protocol) on %s:%d\n", host.c_str(), server.port());
                std::fflush(stdout);
                server.serve(handler, limit, 64, true);
                return 0;
            }
            if (relay.empty())
                throw kerfkite::ValueError("HTTPS needs KerfKite's TLS relay (kerfkite-pendant-tls) beside kerfkite-web: give it with --tls-relay, or "
                                           "serve --behind-proxy behind a TLS terminator of your own");
            kerfkite::http::Server server("127.0.0.1", 0);
            const std::string listen = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
            ChildProcess tls(relay, {"-listen", listen, "-upstream", "127.0.0.1:" + std::to_string(server.port()), "-cert", cert, "-key", key,
                                     "-proxy-protocol", "-max-connections", "64", "-connection-time", "5m"});
            std::string line, shown_port;
            for (int k = 0; k < 2; ++k) {
                if (!tls.readline(line, 15.0)) throw kerfkite::ValueError("The TLS relay did not start; see its message above");
                if (line.compare(0, 5, "port ") == 0) shown_port = line.substr(5);
            }
            std::printf("HTTPS editor and upload inbox: %s\n", options.public_url.c_str());
            std::printf("Listening with TLS on %s:%s\n", host.c_str(), shown_port.c_str());
            std::fflush(stdout);
            std::thread watch([&] {
                // Without its relay nothing reaches the server: end with it.
                while (!tls.wait(0.5)) {
                }
                server.stop();
            });
            server.serve(handler, limit, 64, true);
            watch.detach();
            std::fprintf(stderr, "kerfkite-web: the TLS relay stopped\n");
            std::_Exit(1);
        }
    } catch (const kerfkite::ValueError &e) {
        fail(e.what());
    } catch (const std::exception &e) {
        fail(e.what());
    }
    return 0;
}
