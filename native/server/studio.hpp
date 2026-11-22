// KerfKite Studio's local workstation server without Python: studio/server.py, its design routes
// (studio/design_api.py and the modules it registers) and the phone pendant, over the core. The editor in
// studio/static is served as it is.
#ifndef KERFKITE_SERVER_STUDIO_HPP
#define KERFKITE_SERVER_STUDIO_HPP

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "http.hpp"
#include "kerfkite/json.hpp"
#include "machine.hpp"
#include "phone_jobs.hpp"

namespace kerfkite {
namespace server {

class PhoneMonitor;

struct Options {
    int port = 8765;
    bool enable_hardware = false;
    bool monitor = false;
    int monitor_port = 8766;
    std::string monitor_host = "0.0.0.0";  // the phone pendant's listener: every interface, so a phone on the LAN reaches it
    bool monitor_plaintext = false;
    std::string static_dir;  // the editor's files (studio/static)
};

using Route = std::function<http::Response(const http::Request &)>;

class Studio {
public:
    Studio(Options options, MachineOptions machine = MachineOptions());
    ~Studio();

    // The answer to one request: the workstation's checks, then the route.
    http::Response handle(const http::Request &request);
    // The largest body a request may carry.
    size_t body_limit(const http::Request &request) const;

    const std::string &token() const { return token_; }
    Device &device() { return device_; }
    PhoneJobs &jobs() { return jobs_; }
    PhoneMonitor &monitor() { return *monitor_; }
    const Options &options() const { return options_; }

    void add(const std::string &method, const std::string &path, Route route);
    // A file of the editor, or 404.
    http::Response static_file(const std::string &relative) const;

    // Probe levelling: the plan and the surface measured with it, for one placement of the stock.
    std::mutex level_mutex;
    Json level_plan, level_surface;
    void forget_level();

private:
    Options options_;
    std::string token_;
    Device device_;
    PhoneJobs jobs_;
    std::unique_ptr<PhoneMonitor> monitor_;
    std::map<std::string, Route> routes_;
};

// ---- helpers shared by the routes -----------------------------------------------------------------------

// A refusal with its own status, such as 415 for a body that is not declared as JSON.
struct HttpError : std::runtime_error {
    HttpError(int code, const std::string &message) : std::runtime_error(message), status(code) {}
    int status;
};
// request.get_json(): the body as JSON; 415 when it is not declared as JSON and 400 when it does not parse.
Json strict_json(const http::Request &request);

// request.get_json(silent=True): the body as JSON when it is declared as JSON and parses, else null.
Json json_body(const http::Request &request);
// payload(*required) of the machine routes: a JSON object holding the required fields.
Json payload(const http::Request &request, std::initializer_list<const char *> required = {});
// A JSON response of a value the core or a route built.
http::Response json_response(const Json &value);
// A member of an object, or null.
const Json *member(const Json &object, const char *name);
bool truthy(const Json *value);
// cam.number(value, name, low, high).
double number(const Json *value, const std::string &name, double low, double high);

void add_design_routes(Studio &studio);
void add_machine_routes(Studio &studio);

}  // namespace server
}  // namespace kerfkite

#endif
