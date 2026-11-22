// The phone pendant: a listener on the LAN, enabled from the workstation, that shows the controller's status to
// a paired phone and takes its bounded job and pendant actions (studio/phone_monitor_service.py and
// studio/monitor.py). Pairing hands the phone a bearer token in the link's fragment.
//
// The monitor app speaks HTTP. For HTTPS (Pocket 0.5 and later pins the certificate), the listener binds the
// loopback interface and the pendant's TLS relay (packaging/pendant-tls, Go's standard library: TLS is not
// written here) listens for the phone, with a certificate made for the pairing whose fingerprint goes in the
// pairing link, or with an operator's certificate. Without the relay program, pairing is HTTP only and
// reports can_encrypt false, as the Python server did without its TLS extra.
#ifndef KERFKITE_SERVER_PHONE_MONITOR_HPP
#define KERFKITE_SERVER_PHONE_MONITOR_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "child_process.hpp"
#include "http.hpp"
#include "kerfkite/json.hpp"
#include "machine.hpp"
#include "phone_jobs.hpp"

namespace kerfkite {
namespace server {

class PhoneMonitor {
public:
    // host is the address the phone reaches: every interface for a phone on the LAN, the loopback one for tests.
    // relay is the path of kerfkite-pendant-tls, or empty; certificate and key are an operator's PEM files, or empty.
    PhoneMonitor(Device &device, PhoneJobs &jobs, std::string static_dir, int port, bool plaintext, bool allow_physical,
                 std::string host = "0.0.0.0", std::string relay = std::string(), std::string certificate = std::string(),
                 std::string key = std::string());
    ~PhoneMonitor();

    Json info();
    Json enable();
    Json disable();
    void set_plaintext(const Json &plaintext);

    // This machine's IPv4 addresses, the likeliest for a phone first.
    static std::vector<std::string> addresses();
    // The order addresses() gives: the address on the default route first, self-assigned 169.254 ones last.
    static std::vector<std::string> order_addresses(const std::vector<std::string> &found, const std::string &routed);
    // The answer of the monitor app to one request (for the listener, and for tests).
    http::Response handle(const http::Request &request);

private:
    void begin_action(const Json &data, bool required);
    bool can_encrypt() const;
    bool pinned() const;  // whether enabling makes a certificate pinned by the pairing link
    std::string fragment() const;
    Device &device_;
    PhoneJobs &jobs_;
    std::string static_dir_, host_, relay_program_, certificate_, key_, fingerprint_;
    std::unique_ptr<ChildProcess> relay_;
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
