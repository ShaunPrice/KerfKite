// studio.device.DeviceController and studio.rotary.controller_rotary_axes. See machine.hpp.
#include <algorithm>
#include <cmath>
#include <regex>
#include <utility>

#include "kerfkite/errors.hpp"
#include "machine.hpp"
#include "protocols.hpp"

namespace kerfkite {
namespace server {

using json = Json;

Device::Device(MachineOptions options) : options_(std::move(options)) {
    // Every Grbl-family controller is asked to identify itself ($I), so that the machine can be described.
    driver_ = grbl_machine(options_, {"$I"});
}

void Device::select_protocol(const std::string &name) {
    Hold hold(lock);
    const Protocol &profile = protocol_named(name);
    if (!profile.live_transport) throw ValueError(profile.name + " supports file export only; open the exported file in its controller software");
    if (name == protocol_) return;
    if (driver_->status().at("connected").get<bool>()) throw ValueError("Disconnect before changing protocol");
    driver_->disconnect();
    protocol_ = name;
    if (profile.family == "grbl")
        driver_ = grbl_machine(options_, name == "fluidnc" ? std::vector<std::string>{"$30", "$32", "$I"} : std::vector<std::string>{"$I"});
    else
        driver_ = marlin_machine(options_);
}

Json Device::status() {
    Hold hold(lock);
    json state = driver_->status();
    state["protocol"] = protocol_;
    json axes = json::array();
    for (const std::string &axis : controller_rotary_axes(protocol_, state)) axes.push_back(axis);
    state["rotary_axes"] = axes;
    return state;
}

Json Device::ports() {
    Hold hold(lock);
    return driver_->ports();
}

void Device::connect(const std::string &port) {
    Hold hold(lock);
    driver_->connect(port);
}

void Device::disconnect() {
    Hold hold(lock);
    driver_->disconnect();
}

void Device::start(const std::vector<std::string> &lines) {
    Hold hold(lock);
    driver_->start(lines);
}

void Device::command(const std::string &action, const Json &fields) {
    Hold hold(lock);
    driver_->command(action, fields);
}

std::vector<std::string> Device::probe_reports() {
    Hold hold(lock);
    if (!driver_->probes()) throw RuntimeError("This controller does not report probe contacts");
    return driver_->probe_reports();
}

bool Device::probes() {
    Hold hold(lock);
    return driver_->probes();
}

void Device::expect_touch_plate(double thickness, double wall, double radius) {
    Hold hold(lock);
    driver_->expect_touch_plate(thickness, wall, radius);
}

std::vector<std::string> controller_rotary_axes(const std::string &protocol, const Json &state) {
    const auto field = [&](const char *name) -> const json * {
        const auto found = state.find(name);
        return found == state.end() ? nullptr : &*found;
    };
    const json *connected = field("connected");
    if (!connected || !connected->is_boolean() || !connected->get<bool>() || (protocol != "grblhal" && protocol != "xpro-v5")) return {};
    const json *simulated = field("simulated");
    if (simulated && simulated->is_boolean() && simulated->get<bool>()) {
        if (protocol == "xpro-v5") return {"A"};
        return {"A", "B", "C"};
    }
    std::vector<std::string> info;
    if (const json *rows = field("identification"))
        if (rows->is_array())
            for (const json &row : *rows)
                if (row.is_string()) info.push_back(row.get<std::string>());
    const json empty = json::object();
    const json *settings_field = field("settings");
    const json &settings = settings_field && settings_field->is_object() ? *settings_field : empty;
    const json *count_field = field("position_count");
    if (!count_field || !count_field->is_number_integer()) return {};
    const long long count = count_field->get<long long>();
    if (count < 4 || count > 9) return {};
    const auto has = [&](const std::string &row) { return std::find(info.begin(), info.end(), row) != info.end(); };
    long long mask = 0;
    if (protocol == "xpro-v5") {
        static const std::regex machine(R"(\[MSG:Using machine:CNC_xPRO_V5_XYAZ_(?:NC|NO)\])");
        if (count != 4 || !std::any_of(info.begin(), info.end(), [](const std::string &row) { return std::regex_match(row, machine); })) return {};
        mask = 1;
    } else {
        if (!has("[FIRMWARE:grblHAL]") || !has("[AXS:" + std::to_string(count) + ":" + std::string("XYZABCUVW").substr(0, static_cast<size_t>(count)) + "]"))
            return {};
        const auto found = settings.find("376");
        if (found == settings.end() || !found->is_number() || found->is_boolean()) return {};
        const double value = found->get<double>();
        if (!std::isfinite(value) || value != std::floor(value) || value < 0 || value > 63) return {};
        mask = static_cast<long long>(value);
    }
    std::vector<std::string> axes;
    for (long long i = 0; i < count - 3 && i < 3; ++i) {
        // Confirm resolution, maximum rate and acceleration exist for the drive.
        bool calibrated = true;
        for (const int n : {103, 113, 123}) {
            const auto setting = settings.find(std::to_string(n + i));
            calibrated = calibrated && setting != settings.end() && setting->is_number() && !setting->is_boolean() &&
                         std::isfinite(setting->get<double>()) && setting->get<double>() > 0;
        }
        if (calibrated && (mask & (1LL << i))) axes.push_back(std::string(1, "ABC"[i]));
    }
    return axes;
}

}  // namespace server
}  // namespace kerfkite
