// A Chromium-based browser (Chrome, Chromium or Edge) driven over the Chrome DevTools Protocol, for the assistant
// server's workshop. The protocol runs over the browser's own pipe (--remote-debugging-pipe: file descriptors 3
// and 4 of the browser, handed over on Windows as the C runtime hands descriptors to a child), so no port is
// opened on the loopback interface for any other program to drive it. Each message is JSON ended by a NUL byte.
// Written from the protocol's documentation (chromedevtools.github.io/devtools-protocol); no driver library.
#ifndef KERFKITE_SERVER_BROWSER_HPP
#define KERFKITE_SERVER_BROWSER_HPP

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

class Browser {
public:
    using Events = std::function<void(const std::string &method, const Json &params, const std::string &session)>;

    // Starts the program with the arguments and --remote-debugging-pipe; its output is written to the log file.
    // events receives every event, on the thread that reads the pipe: it may send() but must not call().
    Browser(const std::string &program, const std::vector<std::string> &arguments, const std::string &log, Events events);
    // Asks the browser to close, then ends its process.
    ~Browser();
    Browser(const Browser &) = delete;
    Browser &operator=(const Browser &) = delete;

    // A command and its result (to a target's session when session is given). std::runtime_error with the
    // protocol's message, or when the browser has gone or has not answered in the time.
    Json call(const std::string &method, const Json &params = Json::object(), const std::string &session = std::string(), double seconds = 60);
    // Commands sent together, in order, and their results once all have answered: for a target that waits for
    // the debugger, whose answers to some commands come only after the last of them lets it run.
    std::vector<Json> call_all(const std::vector<std::pair<std::string, Json>> &commands, const std::string &session = std::string(),
                               double seconds = 60);
    // A command whose answer is not awaited.
    void send(const std::string &method, const Json &params, const std::string &session = std::string());
    // Whether the browser is still running and answering on its pipe.
    bool alive() const;
    // The end of its log, for a message about why it stopped.
    std::string log_tail() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

// The browser to drive: the one given, else KERFKITE_MCP_CHROMIUM, else Chrome, Chromium or Edge where they are
// installed. Empty when there is none.
std::string find_chromium(const std::string &given);

}  // namespace server
}  // namespace kerfkite

#endif
