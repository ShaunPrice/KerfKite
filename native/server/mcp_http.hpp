// The assistant server's HTTP transport for a hosted workshop (studio/mcp_server.py --transport http over FastMCP,
// Starlette and uvicorn): the streamable HTTP transport without sessions at /mcp, behind FastMCP's guard of the Host
// and Origin headers and the SDK's bearer authentication with one operator's OAuth tokens (oauth.hpp), and the
// protected-resource metadata (RFC 9728) at /.well-known/oauth-protected-resource/mcp. It listens on the loopback
// interface, for an operator's HTTPS reverse proxy, whose X-Forwarded-Proto it takes as uvicorn does.
//
// Each POST carries one message: a request is answered as one server-sent event, a notification or a response with
// 202. The answers, refusals and their order are FastMCP's (docs/NATIVE.md lists the server's own rules): the guard
// (421, 403), the token (401 with its WWW-Authenticate challenge), the routes (404, 405, 307 for a final '/'), CORS
// for the metadata, and the SDK's checks of a message (406, 415, 400, 202).
#ifndef KERFKITE_SERVER_MCP_HTTP_HPP
#define KERFKITE_SERVER_MCP_HTTP_HPP

#include "http.hpp"
#include "mcp.hpp"
#include "oauth.hpp"

namespace kerfkite {
namespace server {
namespace mcp {

// port: the one the server listens on, which Starlette writes into a redirect when the Host header cannot be used.
http::Response serve_http(Server &server, TokenVerifier &verifier, const http::Request &request, int port);

}  // namespace mcp
}  // namespace server
}  // namespace kerfkite

#endif
