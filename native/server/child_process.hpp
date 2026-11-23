// A program started with its standard input and output on pipes: the phone pendant's TLS relay
// (packaging/pendant-tls), whose lines the server reads and which ends when its input closes, and the STEP
// add-on (native/step), which reads one request and answers it.
#ifndef KERFKITE_SERVER_CHILD_PROCESS_HPP
#define KERFKITE_SERVER_CHILD_PROCESS_HPP

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace kerfkite {
namespace server {

class ChildProcess {
public:
    // Starts program (a path, not looked up on PATH) with the arguments. Its standard error is the server's.
    // Throws std::runtime_error when it cannot be started.
    ChildProcess(const std::string &program, const std::vector<std::string> &arguments);
    // Closes its input, waits a moment for it to end, and ends it otherwise.
    ~ChildProcess();
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    // The next line of its standard output, without the line end; false at the end of its output, or when
    // no whole line arrives within the time.
    bool readline(std::string &line, double seconds);
    // Writes all of bytes to its standard input; false when it has stopped reading.
    bool write(const std::string &bytes);
    // The program's signal to end.
    void close_input();
    // Whether it has ended within the time.
    bool wait(double seconds);
    void kill();

private:
    struct Handles;
    std::unique_ptr<Handles> handles_;
    std::string pending_;  // output read past the last line returned
};

// Whether a regular file is there.
bool file_exists(const std::string &path);
// The directory of the running program, from the operating system where it says, else from argv[0].
std::string program_directory(const char *argv0);

}  // namespace server
}  // namespace kerfkite

#endif
