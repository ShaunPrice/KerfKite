// KerfKite Studio's hosted editor without Python: studio/web_server.py's create_app, an HTTPS editor for
// signed-in accounts and an upload-only inbox for machines. It has no controller. Its HTTPS is KerfKite's TLS
// relay (packaging/pendant-tls), which accepts each connection and passes it to this server on the loopback
// interface with a PROXY protocol line naming the client.
//
// Requests are checked as create_app checks them: the Host must be the public URL's, a page or route needs a
// session cookie (8 hours, 30 minutes idle) and every request that changes something the session's token and
// the public origin; /api/receive takes an upload token instead, and from a page opened as a file (Origin null)
// only with --allow-file-upload. Every answer carries the same security headers.
#ifndef KERFKITE_SERVER_WEB_APP_HPP
#define KERFKITE_SERVER_WEB_APP_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "http.hpp"
#include "kerfkite/json.hpp"
#include "studio.hpp"
#include "web_store.hpp"

namespace kerfkite {
namespace server {

struct WebOptions {
    std::string public_url;     // the HTTPS origin browsers use, such as https://studio.example.com
    bool allow_file_upload = false;
    std::string static_dir;     // the editor's files (studio/static)
    std::string step_program;   // the Open CASCADE add-on (kerfkite-step), or empty
    std::string relay;          // the TLS relay (kerfkite-pendant-tls), which also sends files to receivers
};

// The public URL as an origin: "https://" and the host in lower case, with its port unless 443. host is set to
// what a request's Host must be. ValueError("Public URL must be an HTTPS origin") for anything else.
std::string web_origin(const std::string &public_url, std::string &host);

class WebApp : public RouteTable {
public:
    // create_app(store, public_url, destinations, allow_file_upload): destinations maps each receiver's name to
    // its entry (web_transfer.validate_destination), names in the order the operator added them. Refuses with
    // create_app's messages.
    WebApp(WebStore &store, WebOptions options, Json destinations, std::vector<std::string> names);

    http::Response handle(const http::Request &request);
    size_t body_limit(const http::Request &request) const;

    void add(const std::string &method, const std::string &path, Route route) override;
    StepExchange &step() override { return *step_; }
    const std::string &origin() const { return origin_; }

private:
    WebStore &store_;
    WebOptions options_;
    std::string origin_, host_;
    Json destinations_;
    std::vector<std::string> names_;
    std::shared_ptr<StepExchange> step_;
    std::map<std::string, Route> routes_;

    http::Response protect(const http::Request &request, WebSession &session, std::string &user, bool &answered);
    http::Response dispatch(const http::Request &request, const WebSession &session, const std::string &user);
    std::string allowed(const std::string &path) const;
};

}  // namespace server
}  // namespace kerfkite

#endif
