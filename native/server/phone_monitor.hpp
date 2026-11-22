// The phone pendant: a listener on the LAN, enabled from the workstation, that shows the controller's status to
// a paired phone and takes its bounded job and pendant actions (studio/phone_monitor_service.py and
// studio/monitor.py). Pairing hands the phone a bearer token in the link's fragment.
//
// This listener speaks HTTP. Studio's pinned-certificate HTTPS for Pocket 0.5 and later is not built into
// this program (it is not written here: TLS comes from a vetted library), so enabling pairing reports
// can_encrypt false and the workstation offers unencrypted pairing, as the Python server did without its TLS
// extra.
#ifndef KERFKITE_SERVER_PHONE_MONITOR_HPP
#define KERFKITE_SERVER_PHONE_MONITOR_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "http.hpp"
#include "kerfkite/json.hpp"
#include "machine.hpp"
#include "phone_jobs.hpp"

namespace kerfkite {
namespace server {

class PhoneMonitor {
public:
    // host is the address the listener binds: every interface for a phone on the LAN, the loopback one for tests.
    PhoneMonitor(Device &device, PhoneJobs &jobs, std::string static_dir, int port, bool plaintext, bool allow_physical,
                 std::string host = "0.0.0.0");
    ~PhoneMonitor();

    Json info();
    Json enable();
    Json disable();
    void set_plaintext(const Json &plaintext);

    // This machine's IPv4 addresses, the likeliest for a phone first.
    static std::vector<std::string> addresses();
    // The answer of the monitor app to one request (for the listener, and for tests).
    http::Response handle(const http::Request &request);

private:
    void begin_action(const Json &data, bool required);
    Device &device_;
    PhoneJobs &jobs_;
    std::string static_dir_, host_;
    int port_, bound_port_;
    bool plaintext_, allow_physical_;
    std::recursive_mutex lock_;
    std::unique_ptr<http::Server> listener_;
    std::thread worker_;
    std::string token_;
    std::atomic<bool> revoked_{false};
    std::mutex recent_mutex_;          // for recent_ alone, never held while another lock is taken
    std::vector<std::string> recent_;  // request ids handled, oldest first
};

}  // namespace server
}  // namespace kerfkite

#endif
