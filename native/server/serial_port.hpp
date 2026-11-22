// USB serial ports for the controller transports: listed as pyserial's list_ports.comports() lists them, and
// opened at 115200 baud, 8N1, without flow control, reading with a 50 ms timeout and writing with a 1 s one,
// as the Python transports opened them. POSIX termios and Win32 communications; no library.
#ifndef KERFKITE_SERVER_SERIAL_PORT_HPP
#define KERFKITE_SERVER_SERIAL_PORT_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kerfkite {
namespace server {

struct PortInfo {
    std::string device, description;
};

// The serial ports there are now.
std::vector<PortInfo> serial_ports();

// A byte link to a controller. readline() returns a line with its '\n', or what arrived before a 50 ms pause
// (possibly nothing), at most limit bytes when limit is not zero; write() sends all of its bytes or throws.
class Link {
public:
    virtual ~Link() = default;
    virtual void write(const std::string &bytes) = 0;
    virtual std::string readline(size_t limit = 0) = 0;
    virtual void close() = 0;
};

using LinkFactory = std::function<std::unique_ptr<Link>(const std::string &port)>;

// Opens a serial port; throws std::runtime_error with the system's reason when it cannot.
std::unique_ptr<Link> open_serial(const std::string &port);

}  // namespace server
}  // namespace kerfkite

#endif
