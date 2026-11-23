// HTTP/1.1 for the local workstation. See http.hpp.
#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace kerfkite {
namespace http {

namespace {

const size_t kMaxHead = 64 * 1024;  // request line and header fields

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
struct Winsock {
    Winsock() {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Windows sockets are unavailable");
    }
    ~Winsock() { WSACleanup(); }
};
void winsock() { static Winsock once; }
#else
using Socket = int;
const Socket kNoSocket = -1;
void close_socket(Socket s) { ::close(s); }
void winsock() {}
#endif

// Whether a connection is waiting on the listening socket, after at most `milliseconds`.
bool waiting(Socket s, int milliseconds) {
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd = s;
    p.events = POLLRDNORM;
    return WSAPoll(&p, 1, milliseconds) > 0;
#else
    pollfd p{};
    p.fd = s;
    p.events = POLLIN;
    return ::poll(&p, 1, milliseconds) > 0;
#endif
}

// Ends a connection's wait for more of its request; it can still send.
void cut_reading(Socket s) {
#ifdef _WIN32
    ::shutdown(s, SD_RECEIVE);
    CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);  // a recv blocked on another thread returns at once
#else
    ::shutdown(s, SHUT_RD);
#endif
}

bool send_all(Socket s, const char *data, size_t size) {
    while (size > 0) {
#ifdef _WIN32
        const int sent = ::send(s, data, static_cast<int>(std::min<size_t>(size, 1 << 30)), 0);
#elif defined(MSG_NOSIGNAL)
        const ssize_t sent = ::send(s, data, size, MSG_NOSIGNAL);
#else
        const ssize_t sent = ::send(s, data, size, 0);
#endif
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

// Reads what the connection has into buffer; false when it closed or timed out.
bool receive(Socket s, std::string &buffer) {
    char chunk[16384];
#ifdef _WIN32
    const int got = ::recv(s, chunk, sizeof chunk, 0);
#else
    const ssize_t got = ::recv(s, chunk, sizeof chunk, 0);
#endif
    if (got <= 0) return false;
    buffer.append(chunk, static_cast<size_t>(got));
    return true;
}

std::string lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trimmed(std::string_view text) {
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
    return std::string(text.substr(a, b - a));
}

// After a refusal: the sending side closed, then at most 4 MiB or two seconds of what the client still sends read
// and dropped.
void drain(Socket s) {
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
    DWORD timeout = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof timeout);
#else
    ::shutdown(s, SHUT_WR);
    timeval timeout{0, 500000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#endif
    char buffer[16384];
    size_t total = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (total < (4u << 20) && std::chrono::steady_clock::now() < until) {
        const auto got = ::recv(s, buffer, sizeof buffer, 0);
        if (got <= 0) break;
        total += static_cast<size_t>(got);
    }
}

void reply(Socket s, const Request *request, const Response &response) {
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " + reason(response.status) + "\r\n";
    bool typed = false;
    for (const auto &h : response.headers) {
        head += h.first + ": " + h.second + "\r\n";
        if (lower(h.first) == "content-type") typed = true;
    }
    if (!typed && !response.untyped && !response.body.empty()) head += "Content-Type: application/octet-stream\r\n";
    head += "Content-Length: " + std::to_string(response.body.size()) + "\r\nConnection: close\r\n\r\n";
    if (!send_all(s, head.data(), head.size())) return;
    if (request && request->method == "HEAD") return;
    send_all(s, response.body.data(), response.body.size());
}

// A request's chunked body (RFC 9112 7.1) from data, at most limit bytes; false while more is needed, and
// throws a status for a malformed or oversized body.
bool unchunk(const std::string &data, size_t limit, std::string &body, size_t &used) {
    body.clear();
    size_t at = 0;
    for (;;) {
        const size_t line = data.find("\r\n", at);
        if (line == std::string::npos) return false;
        const std::string size_text = data.substr(at, data.find(';', at) < line ? data.find(';', at) - at : line - at);
        size_t size = 0;
        if (size_text.empty()) throw 400;
        for (const char c : size_text) {
            const int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                       : -1;
            if (digit < 0 || size > (SIZE_MAX >> 4)) throw 400;
            size = size * 16 + static_cast<size_t>(digit);
        }
        at = line + 2;
        if (size == 0) {
            // Trailer fields, then the blank line.
            for (;;) {
                const size_t end = data.find("\r\n", at);
                if (end == std::string::npos) return false;
                if (end == at) {
                    used = end + 2;
                    return true;
                }
                at = end + 2;
            }
        }
        if (body.size() + size > limit) throw 413;
        if (data.size() < at + size + 2) return false;
        body.append(data, at, size);
        if (data.compare(at + size, 2, "\r\n") != 0) throw 400;
        at += size + 2;
    }
}

}  // namespace

const std::string *Request::header(std::string_view name) const {
    for (const auto &h : headers)
        if (h.first == name) return &h.second;
    return nullptr;
}

std::string Request::media_type() const {
    const std::string *type = header("content-type");
    if (!type) return std::string();
    return lower(trimmed(type->substr(0, type->find(';'))));
}

void Response::set(const std::string &name, const std::string &value) {
    for (auto &h : headers)
        if (lower(h.first) == lower(name)) {
            h.second = value;
            return;
        }
    headers.emplace_back(name, value);
}

Response Response::json(std::string text, int status) {
    Response r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "application/json");
    r.body = std::move(text);
    return r;
}

Response Response::error(int status, const std::string &message) {
    std::string text = "{\"error\": \"";
    for (const unsigned char c : message) {
        if (c == '"' || c == '\\') {
            text += '\\';
            text += static_cast<char>(c);
        } else if (c < 0x20) {
            static const char *hex = "0123456789abcdef";
            text += "\\u00";
            text += hex[c >> 4];
            text += hex[c & 15];
        } else {
            text += static_cast<char>(c);
        }
    }
    text += "\"}";
    return json(text, status);
}

const char *reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

bool percent_decode(std::string_view text, std::string &out) {
    out.clear();
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        const auto hex = [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                   : (c >= 'a' && c <= 'f')                    ? c - 'a' + 10
                   : (c >= 'A' && c <= 'F')                    ? c - 'A' + 10
                                                               : -1;
        };
        if (i + 2 >= text.size()) return false;
        const int a = hex(text[i + 1]), b = hex(text[i + 2]);
        if (a < 0 || b < 0) return false;
        out += static_cast<char>(a * 16 + b);
        i += 2;
    }
    return true;
}

// What the connections' threads share with the server. They hold it, never the Server, so a thread that ends
// after the server is gone touches nothing freed.
struct Server::Connections {
    std::mutex lock;
    std::condition_variable idle;
    bool stopping = false;
    int running = 0;            // connections being answered, each on a thread of its own
    std::set<Socket> reading;   // of those, the ones still sending their request
};

Server::Server(const std::string &host, int port) : connections_(std::make_shared<Connections>()) {
    winsock();
    const Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) throw std::runtime_error("Cannot create a listening socket");
#ifndef _WIN32
    ::fcntl(s, F_SETFD, FD_CLOEXEC);  // programs the server starts (the relay, the STEP add-on, a browser) do not hold it
#endif
    int yes = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&yes), sizeof yes);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close_socket(s);
        throw std::runtime_error("Listen on an IPv4 address, such as 127.0.0.1");
    }
    if (::bind(s, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 || ::listen(s, 64) != 0) {
        close_socket(s);
        throw std::runtime_error("Port " + std::to_string(port) + " on " + host + " is in use or not available");
    }
    socklen_t length = sizeof address;
    getsockname(s, reinterpret_cast<sockaddr *>(&address), &length);
    port_ = ntohs(address.sin_port);
    socket_ = static_cast<long long>(s);
}

Server::~Server() {
    stop();
    {
        std::unique_lock<std::mutex> hold(connections_->lock);
        connections_->idle.wait(hold, [this] { return connections_->running == 0; });
    }
    // Closed only now, when no accept can be using it: a number closed sooner could be another socket's by then.
    if (socket_ >= 0) close_socket(static_cast<Socket>(socket_));
}

void Server::stop() {
    std::lock_guard<std::mutex> hold(connections_->lock);
    connections_->stopping = true;
    for (const Socket s : connections_->reading) cut_reading(s);
#ifndef _WIN32
    // Linux stops listening and wakes serve() at once; elsewhere serve() sees the stop at its next look.
    if (socket_ >= 0) ::shutdown(static_cast<Socket>(socket_), SHUT_RD);
#endif
}

// The PROXY protocol's version 1 line (haproxy.org's proxy-protocol.txt, 2.1) at the start of data: true and the
// client's address when it is whole and well formed (empty for UNKNOWN: the connection's own address stands);
// throws 400 for anything else once a line is in.
static bool proxy_line(std::string &data, std::string &address) {
    const size_t end = data.find("\r\n");
    if (end == std::string::npos) {
        if (data.size() > 107) throw 400;
        return false;
    }
    if (end > 105) throw 400;  // 107 bytes with the line end, at most
    const std::string line = data.substr(0, end);
    if (line.compare(0, 13, "PROXY UNKNOWN") == 0 && (line.size() == 13 || line[13] == ' ')) {
        address.clear();
        data.erase(0, end + 2);
        return true;
    }
    std::vector<std::string> fields;
    for (size_t at = 0; at <= line.size();) {
        const size_t space = line.find(' ', at);
        fields.push_back(line.substr(at, space == std::string::npos ? std::string::npos : space - at));
        if (space == std::string::npos) break;
        at = space + 1;
    }
    if (fields.size() != 6 || fields[0] != "PROXY" || (fields[1] != "TCP4" && fields[1] != "TCP6")) throw 400;
    unsigned char probe[16];
    if (inet_pton(fields[1] == "TCP4" ? AF_INET : AF_INET6, fields[2].c_str(), probe) != 1) throw 400;
    address = fields[2];
    data.erase(0, end + 2);
    return true;
}

void Server::serve(const Handler &handler, const Limit &limit, int connections, bool proxied) {
    const std::shared_ptr<Connections> state = connections_;
    const Socket listening = static_cast<Socket>(socket_);
    for (;;) {
        {
            std::lock_guard<std::mutex> hold(state->lock);
            if (state->stopping) break;
        }
        if (!waiting(listening, 100)) continue;
        sockaddr_in peer{};
        socklen_t length = sizeof peer;
        const Socket client = ::accept(listening, reinterpret_cast<sockaddr *>(&peer), &length);
        if (client == kNoSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));  // out of descriptors, say: no busy loop
            continue;
        }
#ifndef _WIN32
        ::fcntl(client, F_SETFD, FD_CLOEXEC);
#endif
        {
            std::lock_guard<std::mutex> hold(state->lock);
            if (state->stopping || state->running >= connections) {
                close_socket(client);
                continue;
            }
            ++state->running;
            state->reading.insert(client);
        }
        char text[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, text, sizeof text);
        std::thread([state, client, address = std::string(text), handler, limit, proxied] {
#ifdef _WIN32
            DWORD timeout = 30000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof timeout);
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof timeout);
#else
            timeval timeout{30, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
            int yes = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
#endif
            // Once its request is in, stop() leaves the connection to be answered. Taken off before the socket
            // closes, so stop() never cuts a number that has been closed.
            const auto request_in = [&state, client] {
                std::lock_guard<std::mutex> hold(state->lock);
                state->reading.erase(client);
            };
            Request request;
            request.peer = address;
            std::string data;
            try {
                if (proxied) {
                    std::string client_address;
                    while (!proxy_line(data, client_address))
                        if (!receive(client, data)) throw 0;
                    if (!client_address.empty()) request.peer = client_address;
                }
                size_t end = std::string::npos;
                while ((end = data.find("\r\n\r\n")) == std::string::npos) {
                    if (data.size() > kMaxHead) throw 431;
                    if (!receive(client, data)) throw 0;
                    // A request starts with its method, a token of capital letters, and a space. Anything else
                    // (a TLS handshake sent to this HTTP listener, say) is answered at once, not after a timeout.
                    size_t i = 0;
                    while (i < data.size() && i < 16 && data[i] >= 'A' && data[i] <= 'Z') ++i;
                    if (i < data.size() && (i == 0 || i == 16 || data[i] != ' ')) throw 400;
                }
                if (end > kMaxHead) throw 431;
                const std::string head = data.substr(0, end);
                size_t line_end = head.find("\r\n");
                const std::string line = head.substr(0, line_end);
                const size_t a = line.find(' '), b = line.rfind(' ');
                if (a == std::string::npos || b == a) throw 400;
                request.method = line.substr(0, a);
                request.target = line.substr(a + 1, b - a - 1);
                if (line.compare(b + 1, 5, "HTTP/") != 0) throw 400;
                request.version = line.substr(b + 1);
                for (size_t at = line_end == std::string::npos ? head.size() : line_end + 2; at < head.size();) {
                    size_t next = head.find("\r\n", at);
                    if (next == std::string::npos) next = head.size();
                    const std::string field = head.substr(at, next - at);
                    const size_t colon = field.find(':');
                    if (colon == std::string::npos || colon == 0) throw 400;
                    request.headers.emplace_back(lower(field.substr(0, colon)), trimmed(field.substr(colon + 1)));
                    at = next + 2;
                }
                const size_t question = request.target.find('?');
                const std::string raw_path = request.target.substr(0, question);
                if (question != std::string::npos) request.query = request.target.substr(question + 1);
                if (raw_path.empty() || raw_path[0] != '/' || !percent_decode(raw_path, request.path)) throw 400;
                const size_t allowed = limit ? limit(request) : 8 * 1024 * 1024;
                std::string rest = data.substr(end + 4);
                const std::string *chunked = request.header("transfer-encoding");
                if (chunked) {
                    if (lower(*chunked).find("chunked") == std::string::npos) throw 501;
                    size_t used = 0;
                    while (!unchunk(rest, allowed, request.body, used)) {
                        if (rest.size() > allowed + 1024 * 1024) throw 413;
                        if (!receive(client, rest)) throw 400;
                    }
                } else if (const std::string *length = request.header("content-length")) {
                    if (length->empty() || !std::all_of(length->begin(), length->end(), [](char c) { return c >= '0' && c <= '9'; }) ||
                        length->size() > 15)
                        throw 400;
                    const size_t size = static_cast<size_t>(std::stoull(*length));
                    if (size > allowed) throw 413;
                    while (rest.size() < size)
                        if (!receive(client, rest)) throw 400;
                    request.body = rest.substr(0, size);
                }
                request_in();
                const Response response = handler(request);
                reply(client, &request, response);
            } catch (int status) {
                request_in();
                if (status) {
                    Response refused = Response::error(status, status == 413 ? "The request is larger than this route accepts"
                                                                            : status == 431 ? "The request's header fields are too large"
                                                                                            : "Malformed HTTP request");
                    reply(client, nullptr, refused);
                    // What the client still sends is read and dropped for a moment, so that closing does not reset the
                    // connection before the client has read the refusal.
                    drain(client);
                }
            } catch (const std::exception &) {
                request_in();
                reply(client, nullptr, Response::error(500, "Internal error"));
            }
#ifdef _WIN32
            ::shutdown(client, SD_SEND);
#else
            ::shutdown(client, SHUT_WR);
#endif
            close_socket(client);
            {
                std::lock_guard<std::mutex> hold(state->lock);
                --state->running;
            }
            state->idle.notify_all();
        }).detach();
    }
}

}  // namespace http
}  // namespace kerfkite
