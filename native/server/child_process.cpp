// A program on pipes. See child_process.hpp.
#include "child_process.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

namespace kerfkite {
namespace server {

namespace {

using clock_type = std::chrono::steady_clock;

double remaining(clock_type::time_point deadline) {
    return std::chrono::duration<double>(deadline - clock_type::now()).count();
}

#ifdef _WIN32
std::wstring wide(const std::string &text) {
    if (text.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string narrow(const std::wstring &text) {
    if (text.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

// One argument as the C runtime splits a command line: quoted, with backslashes doubled before a quote.
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

#ifdef _WIN32

struct ChildProcess::Handles {
    HANDLE process = nullptr, input = nullptr, output = nullptr;
};

ChildProcess::ChildProcess(const std::string &program, const std::vector<std::string> &arguments) : handles_(new Handles) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE child_in = nullptr, child_out = nullptr;
    if (!CreatePipe(&child_in, &handles_->input, &inherit, 0)) throw std::runtime_error("the relay's input could not be made");
    if (!CreatePipe(&handles_->output, &child_out, &inherit, 0)) {
        CloseHandle(child_in);
        CloseHandle(handles_->input);
        throw std::runtime_error("the relay's output could not be made");
    }
    // The server's own ends are not inherited; the child gets exactly its two ends and the server's stderr.
    SetHandleInformation(handles_->input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(handles_->output, HANDLE_FLAG_INHERIT, 0);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE inherited[3] = {child_in, child_out, error};
    DWORD count = error && error != INVALID_HANDLE_VALUE ? 3 : 2;
    if (count == 3 && !SetHandleInformation(error, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) count = 2;  // its errors join its output
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, count * sizeof(HANDLE), nullptr, nullptr)) {
        CloseHandle(child_in);
        CloseHandle(child_out);
        throw std::runtime_error("the relay could not be prepared");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_in;
    startup.StartupInfo.hStdOutput = child_out;
    startup.StartupInfo.hStdError = count == 3 ? error : child_out;
    startup.lpAttributeList = attributes;
    std::wstring line = quoted(wide(program));
    for (const std::string &argument : arguments) line += L" " + quoted(wide(argument));
    PROCESS_INFORMATION started{};
    const std::wstring path = wide(program);
    const BOOL ok = CreateProcessW(path.c_str(), &line[0], nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                   nullptr, &startup.StartupInfo, &started);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(child_in);
    CloseHandle(child_out);
    if (!ok) {
        CloseHandle(handles_->input);
        CloseHandle(handles_->output);
        throw std::runtime_error("the relay could not be started");
    }
    CloseHandle(started.hThread);
    handles_->process = started.hProcess;
}

ChildProcess::~ChildProcess() {
    close_input();
    if (!wait(2)) kill();
    if (handles_->output) CloseHandle(handles_->output);
    if (handles_->process) CloseHandle(handles_->process);
}

bool ChildProcess::readline(std::string &line, double seconds) {
    const auto deadline = clock_type::now() + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(seconds));
    for (;;) {
        const size_t end = pending_.find('\n');
        if (end != std::string::npos) {
            line = pending_.substr(0, end);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending_.erase(0, end + 1);
            return true;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(handles_->output, nullptr, 0, nullptr, &available, nullptr)) return false;  // ended
        if (available) {
            char buffer[512];
            DWORD got = 0;
            if (!ReadFile(handles_->output, buffer, available < sizeof buffer ? available : sizeof buffer, &got, nullptr) || got == 0) return false;
            pending_.append(buffer, got);
            continue;
        }
        if (remaining(deadline) <= 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::close_input() {
    if (handles_->input) {
        CloseHandle(handles_->input);
        handles_->input = nullptr;
    }
}

bool ChildProcess::wait(double seconds) {
    if (!handles_->process) return true;
    return WaitForSingleObject(handles_->process, static_cast<DWORD>(seconds * 1000)) == WAIT_OBJECT_0;
}

void ChildProcess::kill() {
    if (handles_->process) {
        TerminateProcess(handles_->process, 1);
        WaitForSingleObject(handles_->process, 2000);
    }
}

bool file_exists(const std::string &path) {
    const DWORD attributes = GetFileAttributesW(wide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::string program_directory(const char *argv0) {
    std::wstring buffer(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
    std::string path = n > 0 && n < buffer.size() ? narrow(buffer.substr(0, n)) : std::string(argv0 ? argv0 : "");
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

#else

struct ChildProcess::Handles {
    pid_t pid = -1;
    int input = -1, output = -1;
    bool reaped = false;
};

ChildProcess::ChildProcess(const std::string &program, const std::vector<std::string> &arguments) : handles_(new Handles) {
    int to_child[2], from_child[2];
    if (::pipe(to_child) != 0) throw std::runtime_error("the relay's input could not be made");
    if (::pipe(from_child) != 0) {
        ::close(to_child[0]);
        ::close(to_child[1]);
        throw std::runtime_error("the relay's output could not be made");
    }
    // The server's ends close in any program it starts later; the child's ends are moved to 0 and 1.
    ::fcntl(to_child[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(from_child[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, to_child[0], 0);
    posix_spawn_file_actions_adddup2(&actions, from_child[1], 1);
    posix_spawn_file_actions_addclose(&actions, to_child[0]);
    posix_spawn_file_actions_addclose(&actions, from_child[1]);
    std::vector<std::string> words;
    words.push_back(program);
    words.insert(words.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    for (std::string &word : words) argv.push_back(&word[0]);
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int failed = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(to_child[0]);
    ::close(from_child[1]);
    if (failed) {
        ::close(to_child[1]);
        ::close(from_child[0]);
        throw std::runtime_error("the relay could not be started");
    }
    handles_->pid = pid;
    handles_->input = to_child[1];
    handles_->output = from_child[0];
}

ChildProcess::~ChildProcess() {
    close_input();
    if (!wait(2)) kill();
    if (handles_->output >= 0) ::close(handles_->output);
}

bool ChildProcess::readline(std::string &line, double seconds) {
    const auto deadline = clock_type::now() + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(seconds));
    for (;;) {
        const size_t end = pending_.find('\n');
        if (end != std::string::npos) {
            line = pending_.substr(0, end);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending_.erase(0, end + 1);
            return true;
        }
        const double left = remaining(deadline);
        if (left <= 0) return false;
        pollfd waiting{handles_->output, POLLIN, 0};
        const int ready = ::poll(&waiting, 1, static_cast<int>(left * 1000) + 1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return false;
        char buffer[512];
        const ssize_t got = ::read(handles_->output, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;  // ended
        pending_.append(buffer, static_cast<size_t>(got));
    }
}

void ChildProcess::close_input() {
    if (handles_->input >= 0) {
        ::close(handles_->input);
        handles_->input = -1;
    }
}

bool ChildProcess::wait(double seconds) {
    if (handles_->pid < 0 || handles_->reaped) return true;
    const auto deadline = clock_type::now() + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(seconds));
    for (;;) {
        int status = 0;
        const pid_t done = ::waitpid(handles_->pid, &status, WNOHANG);
        if (done == handles_->pid || (done < 0 && errno != EINTR)) {
            handles_->reaped = true;
            return true;
        }
        if (remaining(deadline) <= 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::kill() {
    if (handles_->pid < 0 || handles_->reaped) return;
    ::kill(handles_->pid, SIGKILL);
    int status = 0;
    while (::waitpid(handles_->pid, &status, 0) < 0 && errno == EINTR) {
    }
    handles_->reaped = true;
}

bool file_exists(const std::string &path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string program_directory(const char *argv0) {
    std::string path;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(&buffer[0], &size) == 0) path = buffer.c_str();
#elif defined(__linux__)
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n > 0) path.assign(buffer, static_cast<size_t>(n));
#endif
    if (path.empty()) path = argv0 ? argv0 : "";
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

#endif

}  // namespace server
}  // namespace kerfkite
