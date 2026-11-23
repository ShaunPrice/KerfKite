// The Model Context Protocol (modelcontextprotocol.io, versions 2024-11-05 to 2025-11-25) for KerfKite's assistant
// server: JSON-RPC 2.0 messages for the server's tools and resources, over standard input and output (a message a
// line) or over HTTP (mcp_http.hpp). Written from the specification; no MCP library is used. Messages are read as
// the Python SDK's session read them for FastMCP (mcp.types under pydantic's lax mode): which of request,
// notification, response and error a message is, which parameters each method takes, and FastMCP's answers. A
// tool's arguments are checked against its input schema, with its defaults filled in, before it runs; what a tool
// refuses (ValueError) is reported to the client as a failed call, as FastMCP did.
#ifndef KERFKITE_SERVER_MCP_HPP
#define KERFKITE_SERVER_MCP_HPP

#include <functional>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

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

// What a message is to the SDK's JSONRPCMessage: an error (an id and valid error data) before a request (a method
// and a strict id: a whole number or text), a response (a strict id and a result object) and a notification (a
// method, any other id or none); anything else, a batch among them, is invalid.
enum class Kind { request, notification, response, error, invalid };
Kind classify(const Json &message);

// A resource's address as pydantic's AnyUrl reads it: false for text it refuses, else the address with its scheme
// in lower case (own rule: otherwise as given, where AnyUrl also encoded and resolved it).
bool resource_address(const std::string &given, std::string &address);

class Server {
public:
    Server(std::string name, std::string version, std::string instructions);

    void add(Tool tool);
    void add(Resource resource);

    // The response to a request (classify() said so), as text: its parameters checked as the SDK's ClientRequest
    // checks them for its method, then FastMCP's result or error. stateful: a session that has to begin with
    // initialize (standard input and output); over HTTP without sessions each request stands alone.
    std::string answer(const Json &request, Exchange &exchange, bool stateful);
    // A notification of the session: notifications/initialized ends its initialization.
    void notified(const Json &notification);
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
    bool initialized_ = false;

    std::string call(const Json &id, const Json &params, Exchange &exchange);
};

// An exchange that cannot ask the client: a confirmation fails at once.
class NoExchange : public Exchange {
public:
    std::string confirm(const std::string &) override { throw std::runtime_error("this transport cannot ask the client"); }
};

// Checks arguments against a schema of the subset the tools declare (type, enum, anyOf, items, properties,
// required, additionalProperties, default): the arguments with their defaults, or ValueError naming the argument.
Json checked_arguments(const Json &schema, const Json &arguments);

// The stdio transport: messages from in, a line each, answers to out, until in ends. Each tool call runs on a
// thread of its own, so the client's answer to a confirmation, and other requests, are read while a tool waits.
void serve_stdio(Server &server, std::istream &in, std::ostream &out);

}  // namespace mcp
}  // namespace server
}  // namespace kerfkite

#endif
