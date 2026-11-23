// A browser over the DevTools pipe. See browser.hpp.
#include "browser.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include "child_process.hpp"

namespace kerfkite {
namespace server {

namespace {

#ifdef _WIN32
std::wstring wide(const std::string &text) {
    if (text.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string utf8(const wchar_t *text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// A folder of the system's (Program Files, the user's local application data), whatever the environment says:
// an MCP client may start the server with few variables.
std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) out = utf8(path);
    CoTaskMemFree(path);
    return out;
}

// The program a name is registered as (App Paths), for the user and then for the computer.
std::string registered_program(const wchar_t *name) {
    const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + name;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t buffer[4096];
        DWORD size = sizeof buffer;
        if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
            std::string path = utf8(buffer);
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
            if (!path.empty()) return path;
        }
    }
    return std::string();
}

std::wstring quoted(const std::wstring &argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(2 * backslashes + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(2 * backslashes, L'\\');
    return out + L"\"";
}
#endif

}  // namespace

struct Browser::State {
    Events events;
    std::string log;
    std::mutex lock, write_lock;
    std::condition_variable answered;
    struct Pending {
        bool done = false, ok = false;
        Json result;
        std::string error;
    };
    std::map<long long, std::shared_ptr<Pending>> pending;
    long long next = 0;
    std::atomic<bool> gone{false};
    std::thread reader;
#ifdef _WIN32
    HANDLE process = nullptr, to_browser = nullptr, from_browser = nullptr;
#else
    pid_t pid = -1;
    int to_browser = -1, from_browser = -1;
#endif

    bool write(const std::string &bytes) {
        std::lock_guard<std::mutex> hold(write_lock);
        size_t sent = 0;
        while (sent < bytes.size()) {
#ifdef _WIN32
            DWORD wrote = 0;
            if (!WriteFile(to_browser, bytes.data() + sent, static_cast<DWORD>(std::min<size_t>(bytes.size() - sent, 1 << 20)), &wrote, nullptr) || wrote == 0)
                return false;
#else
            const ssize_t wrote = ::write(to_browser, bytes.data() + sent, bytes.size() - sent);
            if (wrote <= 0) {
                if (wrote < 0 && errno == EINTR) continue;
                return false;
            }
#endif
            sent += static_cast<size_t>(wrote);
        }
        return true;
    }

    // Reads messages until the pipe ends: answers go to their callers, events to the handler.
    void read() {
        std::string buffer;
        char chunk[65536];
        for (;;) {
#ifdef _WIN32
            DWORD got = 0;
            if (!ReadFile(from_browser, chunk, sizeof chunk, &got, nullptr) || got == 0) break;
#else
            const ssize_t got = ::read(from_browser, chunk, sizeof chunk);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
#endif
            buffer.append(chunk, static_cast<size_t>(got));
            size_t start = 0;
            for (size_t end; (end = buffer.find('\0', start)) != std::string::npos; start = end + 1) {
                Json message;
                try {
                    message = Json::parse(std::string_view(buffer).substr(start, end - start));
                } catch (const std::exception &) {
                    unreadable(std::string_view(buffer).substr(start, end - start));
                    continue;
                }
                dispatch(message);
            }
            buffer.erase(0, start);
        }
        gone = true;
        std::lock_guard<std::mutex> hold(lock);
        for (auto &p : pending) {
            p.second->done = true;
            p.second->error = "the browser has stopped";
        }
        answered.notify_all();
    }

    // A message the JSON reader refuses (text with a lone surrogate, which Chromium writes as an escape): when it
    // is an answer, its caller hears so at once rather than waiting out its time.
    void unreadable(std::string_view text) {
        const std::string_view lead = "{\"id\":";
        if (text.substr(0, lead.size()) != lead) return;
        long long id = 0;
        size_t i = lead.size();
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return;
        for (; i < text.size() && i < lead.size() + 15 && text[i] >= '0' && text[i] <= '9'; ++i) id = id * 10 + (text[i] - '0');
        std::lock_guard<std::mutex> hold(lock);
        const auto found = pending.find(id);
        if (found == pending.end()) return;
        found->second->error = "the browser's answer could not be read";
        found->second->done = true;
        answered.notify_all();
    }

    void dispatch(const Json &message) {
        const auto id = message.find("id");
        if (id != message.end() && id->is_number_integer()) {
            std::lock_guard<std::mutex> hold(lock);
            const auto found = pending.find(id->get<long long>());
            if (found == pending.end()) return;
            const auto error = message.find("error");
            if (error != message.end()) {
                const auto text = error->find("message");
                found->second->error = text != error->end() && text->is_string() ? text->get<std::string>() : "the browser refused the command";
            } else {
                const auto result = message.find("result");
                found->second->result = result != message.end() ? *result : Json::object();
                found->second->ok = true;
            }
            found->second->done = true;
            answered.notify_all();
            return;
        }
        const auto method = message.find("method");
        if (method == message.end() || !method->is_string() || !events) return;
        const auto params = message.find("params");
        const auto session = message.find("sessionId");
        try {
            events(method->get<std::string>(), params != message.end() ? *params : Json::object(),
                   session != message.end() && session->is_string() ? session->get<std::string>() : std::string());
        } catch (...) {
        }
    }

    std::string message(long long id, const std::string &method, const Json &params, const std::string &session) {
        Json m = Json::object();
        m["id"] = id;
        m["method"] = method;
        m["params"] = params;
        if (!session.empty()) m["sessionId"] = session;
        return m.dump() + std::string(1, '\0');
    }
};

Browser::Browser(const std::string &program, const std::vector<std::string> &arguments, const std::string &log, Events events)
    : state_(std::make_shared<State>()) {
    state_->events = std::move(events);
    state_->log = log;
#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_commands = nullptr, write_answers = nullptr;
    if (!CreatePipe(&read_commands, &state_->to_browser, &inherit, 1 << 20) || !CreatePipe(&state_->from_browser, &write_answers, &inherit, 1 << 20))
        throw std::runtime_error("the browser's pipe could not be made");
    SetHandleInformation(state_->to_browser, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(state_->from_browser, HANDLE_FLAG_INHERIT, 0);
    HANDLE nothing = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    HANDLE output = CreateFileW(wide(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) output = nothing;
    // The C runtime's descriptors 0 to 4 of the child (lpReserved2): its count, a flag byte each (FOPEN 0x01,
    // FPIPE 0x08, FDEV 0x40), then the handles. Chrome finds its pipe at descriptors 3 and 4.
    const int count = 5;
    std::vector<unsigned char> table(sizeof(int) + count + count * sizeof(HANDLE));
    std::memcpy(table.data(), &count, sizeof(int));
    const unsigned char flags[count] = {0x41, static_cast<unsigned char>(output == nothing ? 0x41 : 0x01),
                                        static_cast<unsigned char>(output == nothing ? 0x41 : 0x01), 0x09, 0x09};
    const HANDLE handles[count] = {nothing, output, output, read_commands, write_answers};
    std::memcpy(table.data() + sizeof(int), flags, count);
    std::memcpy(table.data() + sizeof(int) + count, handles, count * sizeof(HANDLE));
    HANDLE inherited[4] = {nothing, read_commands, write_answers, output};
    const DWORD distinct = output == nothing ? 3 : 4;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, distinct * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nothing;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.StartupInfo.cbReserved2 = static_cast<WORD>(table.size());
    startup.StartupInfo.lpReserved2 = table.data();
    startup.lpAttributeList = attributes;
    std::wstring line = quoted(wide(program));
    for (const std::string &argument : arguments) line += L" " + quoted(wide(argument));
    line += L" --remote-debugging-pipe";
    PROCESS_INFORMATION started{};
    const BOOL ok = CreateProcessW(wide(program).c_str(), &line[0], nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                   &startup.StartupInfo, &started);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(read_commands);
    CloseHandle(write_answers);
    CloseHandle(nothing);
    if (output != nothing) CloseHandle(output);
    if (!ok) {
        CloseHandle(state_->to_browser);
        CloseHandle(state_->from_browser);
        throw std::runtime_error("the browser could not be started: " + program);
    }
    CloseHandle(started.hThread);
    state_->process = started.hProcess;
#else
    int commands[2], answers[2];
    if (::pipe(commands) != 0) throw std::runtime_error("the browser's pipe could not be made");
    if (::pipe(answers) != 0) {
        ::close(commands[0]);
        ::close(commands[1]);
        throw std::runtime_error("the browser's pipe could not be made");
    }
    // Above the descriptors the child gets, so that each dup2 below makes a new descriptor without close-on-exec.
    const auto high = [](int fd) {
        const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
        ::close(fd);
        return moved;
    };
    for (int *fd : {&commands[0], &commands[1], &answers[0], &answers[1]}) *fd = high(*fd);
    state_->to_browser = commands[1];
    state_->from_browser = answers[0];
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawn_file_actions_adddup2(&actions, commands[0], 3);
    posix_spawn_file_actions_adddup2(&actions, answers[1], 4);
    std::vector<std::string> words;
    words.push_back(program);
    for (const std::string &a : arguments) words.push_back(a);
    words.push_back("--remote-debugging-pipe");
    std::vector<char *> argv;
    for (std::string &w : words) argv.push_back(&w[0]);
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int failed = ::access(program.c_str(), X_OK) != 0 ? ENOENT : posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(commands[0]);
    ::close(answers[1]);
    if (failed) {
        ::close(state_->to_browser);
        ::close(state_->from_browser);
        throw std::runtime_error("the browser could not be started: " + program);
    }
    state_->pid = pid;
#endif
    std::shared_ptr<State> state = state_;
    state_->reader = std::thread([state] { state->read(); });
}

Browser::~Browser() {
    if (!state_->gone) {
        try {
            call("Browser.close", Json::object(), std::string(), 5);
        } catch (...) {
        }
    }
#ifdef _WIN32
    if (state_->process && WaitForSingleObject(state_->process, 5000) != WAIT_OBJECT_0) TerminateProcess(state_->process, 1);
    if (state_->process) WaitForSingleObject(state_->process, 2000);
    CloseHandle(state_->to_browser);  // the reader sees the end of the pipe once the browser has gone
    if (state_->reader.joinable()) state_->reader.join();
    CloseHandle(state_->from_browser);
    if (state_->process) CloseHandle(state_->process);
#else
    int status = 0;
    bool ended = false;
    for (int i = 0; i < 100 && !ended; ++i) {
        ended = ::waitpid(state_->pid, &status, WNOHANG) == state_->pid;
        if (!ended) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!ended) {
        ::kill(state_->pid, SIGKILL);
        ::waitpid(state_->pid, &status, 0);
    }
    ::close(state_->to_browser);
    if (state_->reader.joinable()) state_->reader.join();
    ::close(state_->from_browser);
#endif
}

Json Browser::call(const std::string &method, const Json &params, const std::string &session, double seconds) {
    std::shared_ptr<State::Pending> pending = std::make_shared<State::Pending>();
    long long id;
    {
        std::lock_guard<std::mutex> hold(state_->lock);
        if (state_->gone) throw std::runtime_error("the workshop's browser has stopped");
        id = ++state_->next;
        state_->pending[id] = pending;
    }
    if (!state_->write(state_->message(id, method, params, session))) {
        std::lock_guard<std::mutex> hold(state_->lock);
        state_->pending.erase(id);
        throw std::runtime_error("the workshop's browser has stopped");
    }
    std::unique_lock<std::mutex> hold(state_->lock);
    const bool done = state_->answered.wait_for(hold, std::chrono::duration<double>(seconds), [&] { return pending->done; });
    state_->pending.erase(id);
    if (!done) throw std::runtime_error("The workshop's browser did not answer " + method + " in time");
    if (!pending->ok) throw std::runtime_error(pending->error);
    return pending->result;
}

std::vector<Json> Browser::call_all(const std::vector<std::pair<std::string, Json>> &commands, const std::string &session, double seconds) {
    std::vector<std::pair<long long, std::shared_ptr<State::Pending>>> sent;
    {
        std::lock_guard<std::mutex> hold(state_->lock);
        if (state_->gone) throw std::runtime_error("the workshop's browser has stopped");
        for (size_t i = 0; i < commands.size(); ++i) {
            const long long id = ++state_->next;
            sent.emplace_back(id, std::make_shared<State::Pending>());
            state_->pending[id] = sent.back().second;
        }
    }
    bool written = true;
    for (size_t i = 0; i < commands.size() && written; ++i) written = state_->write(state_->message(sent[i].first, commands[i].first, commands[i].second, session));
    std::unique_lock<std::mutex> hold(state_->lock);
    const bool done = written && state_->answered.wait_for(hold, std::chrono::duration<double>(seconds), [&] {
        for (const auto &s : sent)
            if (!s.second->done) return false;
        return true;
    });
    for (const auto &s : sent) state_->pending.erase(s.first);
    if (!written) throw std::runtime_error("the workshop's browser has stopped");
    if (!done) throw std::runtime_error("The workshop's browser did not answer in time");
    std::vector<Json> out;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (!sent[i].second->ok) throw std::runtime_error(commands[i].first + ": " + sent[i].second->error);
        out.push_back(sent[i].second->result);
    }
    return out;
}

void Browser::send(const std::string &method, const Json &params, const std::string &session) {
    long long id;
    {
        std::lock_guard<std::mutex> hold(state_->lock);
        id = ++state_->next;
    }
    state_->write(state_->message(id, method, params, session));
}

bool Browser::alive() const { return !state_->gone; }

std::string Browser::log_tail() const {
    std::ifstream in(state_->log, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.size() > 2000 ? text.substr(text.size() - 2000) : text;
}

std::string find_chromium(const std::string &given) {
    if (!given.empty()) return given;
    if (const char *chosen = std::getenv("KERFKITE_MCP_CHROMIUM"))
        if (*chosen) return chosen;
    std::vector<std::string> candidates;
#ifdef _WIN32
    // The folders the environment names, then the system's own, then where the browsers registered themselves.
    std::vector<std::string> roots;
    for (const wchar_t *base : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"})
        if (const wchar_t *root = _wgetenv(base)) roots.push_back(utf8(root));
    for (const KNOWNFOLDERID *id : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_LocalAppData}) roots.push_back(known_folder(*id));
    for (const std::string &root : roots) {
        if (root.empty()) continue;
        candidates.push_back(root + "\\Google\\Chrome\\Application\\chrome.exe");
        candidates.push_back(root + "\\Chromium\\Application\\chrome.exe");
        candidates.push_back(root + "\\Microsoft\\Edge\\Application\\msedge.exe");
    }
    candidates.push_back(registered_program(L"chrome.exe"));
    candidates.push_back(registered_program(L"msedge.exe"));
#elif defined(__APPLE__)
    for (const char *app : {"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/Applications/Chromium.app/Contents/MacOS/Chromium",
                            "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"})
        candidates.push_back(app);
    if (const char *home = std::getenv("HOME")) candidates.push_back(std::string(home) + "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
#else
    const char *path = std::getenv("PATH");
    const std::string directories = path ? path : "/usr/local/bin:/usr/bin:/bin";
    for (const char *name : {"google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "microsoft-edge", "microsoft-edge-stable"}) {
        size_t at = 0;
        while (at <= directories.size()) {
            const size_t colon = directories.find(':', at);
            const std::string folder = directories.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
            if (!folder.empty()) candidates.push_back(folder + "/" + name);
            if (colon == std::string::npos) break;
            at = colon + 1;
        }
    }
    candidates.push_back("/snap/bin/chromium");
#endif
    for (const std::string &c : candidates)
        if (!c.empty() && file_exists(c)) return c;
    return std::string();
}

}  // namespace server
}  // namespace kerfkite
