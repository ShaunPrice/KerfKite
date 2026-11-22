// A small HTTP/1.1 server for Studio's local workstation: one request a connection, on a socket bound to an
// address the caller chooses (the loopback interface for the editor). Written from RFC 9110 and 9112 for what
// a browser sends: a request line, header fields, and a body given by Content-Length or in chunks. Responses
// close the connection. There is no TLS here; see docs/NATIVE.md for where the phone pendant's TLS lives.
#ifndef KERFKITE_SERVER_HTTP_HPP
#define KERFKITE_SERVER_HTTP_HPP

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kerfkite {
namespace http {

struct Request {
    std::string method;   // as sent, e.g. "GET"
    std::string target;   // as sent: path and query
    std::string path;     // percent-decoded, without the query
    std::string query;    // after '?', not decoded
    std::vector<std::pair<std::string, std::string>> headers;  // names in lower case
    std::string body;
    std::string peer;     // the client's address

    // The first header of that name (lower case), or null.
    const std::string *header(std::string_view name) const;
    // The media type of Content-Type, lower case, without parameters.
    std::string media_type() const;
};

struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    void set(const std::string &name, const std::string &value);
    // A JSON body (already written as text).
    static Response json(std::string text, int status = 200);
    // {"error": message}, as Flask's handlers give a refused request.
    static Response error(int status, const std::string &message);
};

// The body a request may carry, decided from its line and headers before the body is read. A body over it is
// refused with 413.
using Limit = std::function<size_t(const Request &)>;
using Handler = std::function<Response(const Request &)>;

class Server {
public:
    // Binds host:port (port 0 asks for any free port) and listens. Throws std::runtime_error when it cannot.
    Server(const std::string &host, int port);
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    int port() const { return port_; }
    // Accepts connections until stop(), each answered on a thread of its own, at most `connections` at once.
    void serve(const Handler &handler, const Limit &limit, int connections = 64);
    void stop();

private:
    long long socket_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> active_{0};
};

// The reason phrase of a status code.
const char *reason(int status);

// Percent-decoding of a path ("%2F" and the like); false for a malformed escape.
bool percent_decode(std::string_view text, std::string &out);

}  // namespace http
}  // namespace kerfkite

#endif
