// The Model Context Protocol (modelcontextprotocol.io, versions 2024-11-05 to 2025-11-25) for KerfKite's assistant
// server: JSON-RPC 2.0 messages for the server's tools and resources, over standard input and output (a message a
// line) or over HTTP (each message a POST, answered as JSON, without sessions). Written from the specification;
// no MCP library is used. A tool's arguments are checked against its input schema, with its defaults filled in,
// before it runs; what a tool refuses (ValueError) is reported to the client as a failed call, as FastMCP did.
#ifndef KERFKITE_SERVER_MCP_HPP
#define KERFKITE_SERVER_MCP_HPP

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "http.hpp"
#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {
namespace mcp {

// What a running tool can ask of the client.
class Exchange {
public:
    virtual ~Exchange() = default;
    // Asks the operator to confirm (elicitation/create with no fields): "accept", "decline" or "cancel".
    // Throws std::runtime_error when the client cannot ask.
    virtual std::string confirm(const std::string &message) = 0;
};

struct Result {
    Json value;         // the result; a wrapped tool's value goes in {"result": value}
    std::string image;  // PNG bytes: an image in place of a value
};

// A tool. Its schemas and annotations are JSON text, so that their members keep the order they are written in
// (the core's Json keeps names in byte order), as clients and the people reading them see them.
struct Tool {
    std::string name, description;
    std::string input_schema;   // a JSON Schema object of the arguments
    std::string output_schema;  // empty for none
    bool wrapped = false;       // the value is sent as {"result": value}
    std::string annotations;
    std::function<Result(const Json &arguments, Exchange &exchange)> run;
    int position = -1;  // where tools/list puts it among the tools with one; the rest follow in the order added
};

struct Resource {
    std::string uri, name, mime_type;
    std::function<std::string()> read;
};

class Server {
public:
    Server(std::string name, std::string version, std::string instructions);

    void add(Tool tool);
    void add(Resource resource);

    // The answer to one message from a client, as text: a response for a request, empty for a notification or
    // a response. A batch (an array) is answered with an array of responses.
    std::string receive(const Json &message, Exchange &exchange);
    // Whether the client said in its initialize request that it can ask its user (elicitation).
    bool client_confirms() const { return client_confirms_; }

private:
    struct Entry {
        Tool tool;
        Json schema;
    };
    std::string name_, version_, instructions_;
    std::vector<Entry> tools_;
    std::vector<Resource> resources_;
    bool client_confirms_ = false;

    std::string request(const Json &message, Exchange &exchange);
    std::string call(const Json &id, const Json &params, Exchange &exchange);
};

// Checks arguments against a schema of the subset the tools declare (type, enum, anyOf, items, properties,
// required, additionalProperties, default): the arguments with their defaults, or ValueError naming the argument.
Json checked_arguments(const Json &schema, const Json &arguments);

// The stdio transport: messages from in, a line each, answers to out, until in ends. Each tool call runs on a
// thread of its own, so the client's answer to a confirmation, and other requests, are read while a tool waits.
void serve_stdio(Server &server, std::istream &in, std::ostream &out);

// The streamable HTTP transport without sessions, for one request to the endpoint: POST of a message, answered
// with application/json (202 for notifications and responses); a tool cannot ask the client to confirm.
http::Response serve_http(Server &server, const http::Request &request);

}  // namespace mcp
}  // namespace server
}  // namespace kerfkite

#endif
