// The hosted editor's uploads and receivers, as studio/web_transfer.py checks and sends them. A file is a simple
// name with one of Studio's extensions and UTF-8 text; a receiver is an operator-configured HTTP/S origin with
// the upload token it generated, never a URL a browser supplied. The file goes to the receiver through
// KerfKite's TLS relay (packaging/pendant-tls, Go's standard library), which makes the one HTTPS request.
#ifndef KERFKITE_SERVER_WEB_TRANSFER_HPP
#define KERFKITE_SERVER_WEB_TRANSFER_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

// urllib.parse.urlsplit, and the parts of its result the checks read. Throws ValueError("Invalid IPv6 URL") where
// urlsplit does.
struct UrlParts {
    std::string scheme, netloc, path, query, fragment;
    bool has_hostname = false;  // hostname is not None
    std::string hostname;       // lower case, without the brackets of an IPv6 address
    bool has_port = false;
    std::string port;           // as written
    bool has_user = false;      // netloc holds '@'
    std::string username, password;
};
UrlParts url_split(const std::string &url);

struct Upload {
    std::string name, content;  // content: the text's UTF-8 bytes
};

// validate_upload(data): the name and bytes, or ValueError with web_transfer.py's messages.
Upload validate_upload(const Json &data);

// Whether werkzeug.utils.secure_filename leaves the name as it is: only letters, digits and _.-, not starting or
// ending with . or _, and, on Windows, not a device name such as CON or LPT1.
bool simple_file_name(const std::string &name);

// validate_destination(entry): ValueError with web_transfer.py's messages for an entry that is not usable.
void validate_destination(const Json &entry);

// send_to_receiver(entry, name, content): the receiver's confirmation as the route answers it, through the relay
// program. ValueError when the answer cannot be verified.
Json send_to_receiver(const Json &entry, const Upload &upload, const std::string &relay);

// One request made by KerfKite's TLS relay (kerfkite-pendant-tls -fetch): the request on its standard input, so
// that no token or password is on a command line, and its answer ({"status", "headers", "body" (base64)} or
// {"error"}). std::runtime_error when the relay cannot be run or gives no answer within the time.
Json relay_fetch(const std::string &relay, const Json &request, double seconds);

}  // namespace server
}  // namespace kerfkite

#endif
