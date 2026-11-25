// The controller transports of the local workstation, ported from studio/controller.py (Grbl family),
// studio/marlin.py (Marlin) and studio/device.py (the protocol in use): a single owner of the serial link,
// acknowledgement-paced, with the simulator the workstation starts in. Physical ports are opened only when
// the operator enables hardware; see docs/SAFETY.md and AGENTS.md.
#ifndef KERFKITE_SERVER_MACHINE_HPP
#define KERFKITE_SERVER_MACHINE_HPP

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "kerfkite/json.hpp"
#include "rlock.hpp"
#include "serial_port.hpp"

namespace kerfkite {
namespace server {

// A refusal Python raised as RuntimeError; answered with 400 as a ValueError is.
struct RuntimeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct MachineOptions {
    LinkFactory open = open_serial;
    std::function<std::vector<PortInfo>()> ports = serial_ports;
    std::function<double()> clock;  // monotonic seconds; the steady clock when empty
    double timeout = 15;            // seconds an acknowledgement may take
    // In place of the transport loop's 10 ms pause, for tests that step the loop: true ends the loop.
    std::function<bool()> wait;
    // The stock surface the Grbl simulator probes, Z at machine X and Y; the simulator's own when empty.
    std::function<double(double, double)> surface;
};

class Machine {
public:
    virtual ~Machine() = default;
    virtual Json status() = 0;
    virtual Json ports() = 0;
    virtual void connect(const std::string &port) = 0;
    virtual void disconnect() = 0;
    virtual void start(const std::vector<std::string> &lines) = 0;
    // action with its fields (axis, distance, feed for a jog), as the command route passes them.
    virtual void command(const std::string &action, const Json &fields) = 0;
    virtual bool probes() const { return false; }
    virtual std::vector<std::string> probe_reports() { return {}; }
    // A touch plate for the simulator to probe, announced before a zeroing program starts: its thickness, the
    // wall between its probing faces and the stock, and the cutter's radius. A real controller ignores it.
    virtual void expect_touch_plate(double, double, double) {}
};

std::unique_ptr<Machine> grbl_machine(const MachineOptions &options, std::vector<std::string> initialization_queries);
std::unique_ptr<Machine> marlin_machine(const MachineOptions &options, double boot_delay = 2);

// studio.device.DeviceController: the transport for the selected protocol, behind one lock that the
// workstation's machine routes and the phone jobs hold.
class Device {
public:
    explicit Device(MachineOptions options = MachineOptions());
    RLock lock;
    void select_protocol(const std::string &protocol);
    Json status();  // the driver's, with "protocol" and "rotary_axes"
    Json ports();
    void connect(const std::string &port);
    void disconnect();
    void start(const std::vector<std::string> &lines);
    void command(const std::string &action, const Json &fields);
    std::vector<std::string> probe_reports();
    bool probes();  // whether the controller reports probe contacts (the Grbl family)
    void expect_touch_plate(double thickness, double wall, double radius);
    const std::string &protocol() const { return protocol_; }

private:
    MachineOptions options_;
    std::string protocol_ = "grbl";
    std::unique_ptr<Machine> driver_;
};

// studio.rotary.controller_rotary_axes: the rotary axes the current readback supports for this connection.
std::vector<std::string> controller_rotary_axes(const std::string &protocol, const Json &state);
// studio.rotary.validate_rotary_machine: a rotary job's setup against the controller's readback; ValueError when
// the axis, its calibration or its travel is not confirmed (machine_routes.cpp).
void validate_rotary_machine(const Json &rotary, const Json &status);

}  // namespace server
}  // namespace kerfkite

#endif
