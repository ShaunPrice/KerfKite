// Serial ports on POSIX and Windows. See serial_port.hpp.
#include "serial_port.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <fstream>
#endif

namespace kerfkite {
namespace server {

namespace {

#ifndef _WIN32

std::string real(const std::string &path) {
    char buffer[PATH_MAX];
    return ::realpath(path.c_str(), buffer) ? std::string(buffer) : std::string();
}

std::string base(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string first_line(const std::string &path) {
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line)) return std::string();
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

std::vector<std::string> matching(const std::string &directory, const std::vector<std::string> &prefixes) {
    std::vector<std::string> out;
    if (DIR *dir = ::opendir(directory.c_str())) {
        while (const dirent *entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            for (const std::string &prefix : prefixes)
                if (name.compare(0, prefix.size(), prefix) == 0) out.push_back(directory + "/" + name);
        }
        ::closedir(dir);
    }
    std::sort(out.begin(), out.end());
    return out;
}

class PosixLink : public Link {
public:
    explicit PosixLink(const std::string &port) {
        fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) throw std::runtime_error("[Errno " + std::to_string(errno) + "] could not open port " + port + ": " + std::strerror(errno));
        termios t{};
        if (tcgetattr(fd_, &t) != 0) {
            const int e = errno;
            ::close(fd_);
            throw std::runtime_error("Could not configure port " + port + ": " + std::strerror(e));
        }
        t.c_iflag &= static_cast<tcflag_t>(~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY));
        t.c_oflag &= static_cast<tcflag_t>(~OPOST);
        t.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
        t.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | CSTOPB));
#ifdef CRTSCTS
        t.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif
        t.c_cflag |= CS8 | CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        if (tcsetattr(fd_, TCSANOW, &t) != 0) {
            const int e = errno;
            ::close(fd_);
            throw std::runtime_error("Could not configure port " + port + ": " + std::strerror(e));
        }
    }
    ~PosixLink() override { close(); }

    void write(const std::string &bytes) override {
        size_t at = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (at < bytes.size()) {
            if (fd_ < 0) throw std::runtime_error("Attempting to use a port that is not open");
            const ssize_t sent = ::write(fd_, bytes.data() + at, bytes.size() - at);
            if (sent > 0) {
                at += static_cast<size_t>(sent);
                continue;
            }
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) throw std::runtime_error("Write timeout");
            pollfd p{fd_, POLLOUT, 0};
            ::poll(&p, 1, static_cast<int>(left));
        }
    }

    std::string readline(size_t limit) override {
        for (;;) {
            const size_t end = buffer_.find('\n');
            if (end != std::string::npos && (!limit || end < limit)) return take(end + 1);
            if (limit && buffer_.size() >= limit) return take(limit);
            if (fd_ < 0) throw std::runtime_error("Attempting to use a port that is not open");
            pollfd p{fd_, POLLIN, 0};
            const int ready = ::poll(&p, 1, 50);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
            if (ready == 0) return take(buffer_.size());  // a pause: what arrived so far
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
                throw std::runtime_error("device reports readiness to read but returned no data (device disconnected or multiple access on port?)");
            char chunk[512];
            const ssize_t got = ::read(fd_, chunk, sizeof chunk);
            if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (got <= 0)
                throw std::runtime_error("device reports readiness to read but returned no data (device disconnected or multiple access on port?)");
            buffer_.append(chunk, static_cast<size_t>(got));
        }
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    std::string take(size_t count) {
        std::string out = buffer_.substr(0, count);
        buffer_.erase(0, count);
        return out;
    }
    int fd_ = -1;
    std::string buffer_;
};

#else

class WindowsLink : public Link {
public:
    explicit WindowsLink(const std::string &port) {
        const std::string path = "\\\\.\\" + port;
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("could not open port '" + port + "': error " + std::to_string(GetLastError()));
        DCB dcb{};
        dcb.DCBlength = sizeof dcb;
        GetCommState(handle_, &dcb);
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutConstant = 1000;
        if (!SetCommState(handle_, &dcb) || !SetCommTimeouts(handle_, &timeouts)) {
            const DWORD e = GetLastError();
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("could not configure port '" + port + "': error " + std::to_string(e));
        }
    }
    ~WindowsLink() override { close(); }

    void write(const std::string &bytes) override {
        size_t at = 0;
        while (at < bytes.size()) {
            DWORD sent = 0;
            if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Attempting to use a port that is not open");
            if (!WriteFile(handle_, bytes.data() + at, static_cast<DWORD>(bytes.size() - at), &sent, nullptr))
                throw std::runtime_error("WriteFile failed: error " + std::to_string(GetLastError()));
            if (sent == 0) throw std::runtime_error("Write timeout");
            at += sent;
        }
    }

    std::string readline(size_t limit) override {
        for (;;) {
            const size_t end = buffer_.find('\n');
            if (end != std::string::npos && (!limit || end < limit)) return take(end + 1);
            if (limit && buffer_.size() >= limit) return take(limit);
            if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Attempting to use a port that is not open");
            char chunk[512];
            DWORD got = 0;
            if (!ReadFile(handle_, chunk, sizeof chunk, &got, nullptr))
                throw std::runtime_error("ReadFile failed: error " + std::to_string(GetLastError()));
            if (got == 0) return take(buffer_.size());
            buffer_.append(chunk, got);
        }
    }

    void close() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    std::string take(size_t count) {
        std::string out = buffer_.substr(0, count);
        buffer_.erase(0, count);
        return out;
    }
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string buffer_;
};

#endif

}  // namespace

std::vector<PortInfo> serial_ports() {
    std::vector<PortInfo> ports;
#if defined(_WIN32)
    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return ports;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof info;
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &info); ++index) {
        HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        char name[256] = {0};
        DWORD size = sizeof name - 1, type = 0;
        const LONG found = RegQueryValueExA(key, "PortName", nullptr, &type, reinterpret_cast<BYTE *>(name), &size);
        RegCloseKey(key);
        if (found != ERROR_SUCCESS || std::string(name).compare(0, 3, "COM") != 0) continue;
        char friendly[512] = {0};
        std::string description = "n/a";
        if (SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE *>(friendly), sizeof friendly - 1, nullptr))
            description = friendly;
        ports.push_back({name, description});
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(ports.begin(), ports.end(), [](const PortInfo &a, const PortInfo &b) { return a.device < b.device; });
#elif defined(__APPLE__)
    for (const std::string &device : matching("/dev", {"cu."})) ports.push_back({device, "n/a"});
#else
    // As pyserial's Linux listing: these device names, without the platform's absent built-in ports.
    for (const std::string &device :
         matching("/dev", {"ttyS", "ttyUSB", "ttyXRUSB", "ttyACM", "ttyAMA", "rfcomm", "ttyAP", "ttyGS"})) {
        const std::string name = base(device);
        const std::string device_path = real("/sys/class/tty/" + name + "/device");
        const std::string subsystem = device_path.empty() ? std::string() : base(real(device_path + "/subsystem"));
        if (subsystem == "platform") continue;
        std::string description = "n/a";
        if (subsystem == "usb" || subsystem == "usb-serial") {
            const std::string interface_path = subsystem == "usb-serial" ? parent(device_path) : device_path;
            const std::string usb_device = parent(interface_path);
            const std::string product = first_line(usb_device + "/product"), interface = first_line(interface_path + "/interface");
            description = !interface.empty() ? product + " - " + interface : !product.empty() ? product : name;
        } else if (subsystem == "pnp" || subsystem == "amba") {
            description = name;
        }
        ports.push_back({device, description});
    }
#endif
    return ports;
}

std::unique_ptr<Link> open_serial(const std::string &port) {
#ifdef _WIN32
    return std::make_unique<WindowsLink>(port);
#else
    return std::make_unique<PosixLink>(port);
#endif
}

}  // namespace server
}  // namespace kerfkite
