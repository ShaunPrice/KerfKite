// The Grbl-family and Marlin transports, the device that selects between them, and the rotary readback check.
// Ported statement for statement from studio/controller.py, studio/marlin.py, studio/device.py and
// studio/rotary.py's controller_rotary_axes; see machine.hpp.
#include "machine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <regex>
#include <thread>
#include <utility>

#include "kerfkite/errors.hpp"
#include "kerfkite/number_text.hpp"

namespace kerfkite {
namespace server {

namespace {

using json = Json;

double steady() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// f'{value:.{places}f}'.
std::string fixed(double value, int places) {
    std::string out;
    if (text::fixed(out, value, places)) return out;
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", places, value);
    return buffer;
}

std::string lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string upper(std::string text) {
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string strip(const std::string &text) {
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    return text.substr(a, b - a);
}

std::vector<std::string> split_space(const std::string &text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

std::vector<std::string> split(const std::string &text, char separator) {
    std::vector<std::string> parts;
    size_t at = 0;
    for (;;) {
        const size_t next = text.find(separator, at);
        parts.push_back(text.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) return parts;
        at = next + 1;
    }
}

bool starts(const std::string &text, const char *prefix) { return text.compare(0, std::strlen(prefix), prefix) == 0; }

// float(text) for what controllers write: optional sign, digits, a point, an exponent, inf and nan.
bool python_float(const std::string &given, double &out) {
    const std::string text = strip(given);
    if (text.empty()) return false;
    const std::string word = lower(text);
    const std::string bare = word[0] == '+' || word[0] == '-' ? word.substr(1) : word;
    if (bare == "inf" || bare == "infinity" || bare == "nan") {
        out = bare == "nan" ? std::nan("") : (word[0] == '-' ? -INFINITY : INFINITY);
        return true;
    }
    static const std::regex number(R"([+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?)");
    if (!std::regex_match(text, number)) return false;
    out = std::strtod(text.c_str(), nullptr);
    return true;
}

// float(value) of a JSON value as Python takes it: a number, a boolean, or text.
bool python_float(const json *value, double &out) {
    if (!value) return false;
    if (value->is_boolean()) {
        out = value->get<bool>() ? 1.0 : 0.0;
        return true;
    }
    if (value->is_number()) {
        out = value->get<double>();
        return true;
    }
    if (value->is_string()) return python_float(value->get<std::string>(), out);
    return false;
}

// Controller text as Python decodes it: ASCII, anything else replaced.
std::string ascii(const std::string &raw) {
    std::string out;
    for (const unsigned char c : raw) {
        if (c < 0x80) out += static_cast<char>(c);
        else out += "\xef\xbf\xbd";
    }
    return out;
}

class Log {
public:
    void append(const std::string &line) {
        lines_.push_back(line);
        while (lines_.size() > 80) lines_.pop_front();
    }
    json to_json() const {
        json out = json::array();
        for (const std::string &line : lines_) out.push_back(line);
        return out;
    }

private:
    std::deque<std::string> lines_;
};

json opt(const std::optional<std::string> &value) { return value ? json(*value) : json(); }

// Progress as Python keeps it: 0 and 100 are integers, anything between a float.
json progress_json(double progress) {
    if (progress == 0) return json(0);
    if (progress == 100) return json(100);
    return json(progress);
}

// ---- Grbl ---------------------------------------------------------------------------------------------------

class Grbl : public Machine {
public:
    Grbl(const MachineOptions &options, std::vector<std::string> queries) : options_(options), queries_(std::move(queries)) {
        if (!options_.clock) options_.clock = steady;
    }
    ~Grbl() override { disconnect(); }

    Json ports() override {
        json out = json::array();
        for (const PortInfo &p : options_.ports()) {
            json port = json::object();
            port["device"] = p.device;
            port["description"] = p.description;
            out.push_back(port);
        }
        return out;
    }

    Json status() override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        json out = json::object();
        out["connected"] = connected_;
        out["simulated"] = simulated_;
        out["state"] = state_;
        out["error"] = opt(error_);
        out["position"] = position_.is_null() ? json() : position_;
        out["settings"] = settings_;
        out["position_count"] = position_count_;
        out["spindle_type"] = opt(spindle_type_);
        json identification = json::array();
        for (const std::string &row : identification_) identification.push_back(row);
        out["identification"] = identification;
        out["banner"] = opt(banner_);
        out["ready"] = ready_;
        out["progress"] = progress_json(progress_);
        out["probe_count"] = static_cast<long long>(probes_.size());
        out["log"] = log_.to_json();
        out["active"] = active_;
        return out;
    }

    void connect(const std::string &port) override {
        if (port != "simulator") {
            bool listed = false;
            for (const PortInfo &p : options_.ports()) listed = listed || p.device == port;
            if (!listed) throw ValueError("Select an available USB serial port");
        }
        disconnect();
        std::lock_guard<std::recursive_mutex> hold(m_);
        error_.reset();
        settings_ = json::object();
        position_ = json();
        position_count_ = 0;
        spindle_type_.reset();
        progress_ = 0;
        probes_.clear();
        offset_[0] = offset_[1] = offset_[2] = 0.0;
        init_remaining_.assign(queries_.begin(), queries_.end());
        initialized_ = false;
        identification_.clear();
        banner_.reset();
        simulated_ = port == "simulator";
        ready_ = simulated_;
        state_ = simulated_ ? "Idle" : "Connecting";
        if (simulated_) {
            settings_["30"] = 1000;
            settings_["32"] = 0;
        } else {
            try {
                link_ = options_.open(port);
            } catch (const std::exception &e) {
                state_ = "Disconnected";
                throw RuntimeError(std::string("Cannot open serial port: ") + e.what());
            }
        }
        connected_ = true;
        last_status_ = simulated_ ? options_.clock() : 0;
        stop_ = false;
        thread_ = std::thread([this] { loop(); });
    }

    void disconnect() override {
        {
            std::lock_guard<std::recursive_mutex> hold(m_);
            stop_ = true;
            if (link_) {
                try {
                    if (active_) link_->write("\x18");
                    link_->close();
                } catch (...) {
                }
                link_.reset();
            }
            connected_ = false;
            ready_ = false;
            state_ = "Disconnected";
            clear();
        }
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }

    void start(const std::vector<std::string> &lines) override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        idle();
        if (lines.empty()) throw ValueError("Empty job");
        for (const std::string &s : lines)
            if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos || s.size() > 78 ||
                std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; }))
                throw ValueError("Invalid G-code line");
        queue_.assign(lines.begin(), lines.end());
        probes_.clear();
        relative_ = false;
        total_ = static_cast<long>(lines.size());
        acked_ = 0;
        progress_ = 0;
        active_ = true;
        state_ = "Run";
    }

    bool probes() const override { return true; }

    // The simulator's touch plate sits on a corner of the stock, wherever the operator left the cutter: its left
    // face is 12.5 mm to the left of the cutter, its front face 9.5 mm in front and its top 7.3 mm below. The
    // zeroing program is not told this: it has to find the plate by probing.
    void expect_touch_plate(double thickness, double wall, double radius) override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        if (!simulated_) return;
        const auto at = [&](const char *axis) { return position_.is_null() ? 0.0 : position_[axis].get<double>(); };
        plate_.on = true;
        plate_.x = at("x") + offset_[0] - 12.5 + wall;  // the stock's corner is the plate's wall inside its faces
        plate_.y = at("y") + offset_[1] - 9.5 + wall;
        plate_.top = at("z") + offset_[2] - 7.3;
        plate_.thickness = thickness;
        plate_.wall = wall;
        plate_.radius = radius;
    }

    std::vector<std::string> probe_reports() override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        return probes_;
    }

    void command(const std::string &action, const Json &fields) override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        if (!connected_) throw RuntimeError("Connect first");
        if (action == "reset") {
            write("\x18");
            clear();
            ready_ = simulated_;
            state_ = simulated_ ? "Idle" : "Reset; reconnect required";
            if (simulated_) error_.reset();
            else error_ = "Reset sent. Reconnect before further commands.";
            return;
        }
        if (action == "hold") {
            write("!");
            if (simulated_) {
                if (active_) {
                    paused_ = true;
                    state_ = "Hold";
                }
                return;
            }
            if (!paused_ || resume_requested_at_) {
                hold_requested_at_ = options_.clock();
                resume_requested_at_.reset();
                paused_ = true;
                state_ = "Hold requested";
            }
            return;
        }
        if (action == "resume") {
            const double now = options_.clock();
            if (!simulated_) check_timeouts(now);
            if (error_ || !ready_ || !paused_) throw RuntimeError("No resumable held job");
            if (!simulated_ && (!held_since_ || state_ != "Hold" || now - last_status_ > 2))
                throw RuntimeError("Wait for a fresh, fully stopped feed hold before resuming");
            write("~");
            end_confirmed_hold(now);
            hold_requested_at_.reset();
            if (simulated_) {
                paused_ = false;
                state_ = active_ ? "Run" : "Idle";
            } else {
                // Keep subsequent host lines queued until firmware reports Run or Idle.
                resume_requested_at_ = now;
                state_ = "Resuming";
            }
            return;
        }
        idle();
        std::string line;
        if (action == "jog") {
            const json *axis = fields.is_object() ? fields.find("axis") != fields.end() ? &*fields.find("axis") : nullptr : nullptr;
            if (!axis || !(axis->is_string() && (*axis == json("X") || *axis == json("Y") || *axis == json("Z"))))
                throw ValueError("Invalid jog axis");
            double distance = 0, feed = 0;
            const auto field = [&](const char *name) -> const json * {
                const auto found = fields.find(name);
                return found == fields.end() ? nullptr : &*found;
            };
            if (!python_float(field("distance"), distance) || !python_float(field("feed"), feed))
                throw ValueError("Invalid jog distance/feed");
            if (!std::isfinite(distance) || !std::isfinite(feed) || !(0 < std::fabs(distance) && std::fabs(distance) <= 10) ||
                !(0 < feed && feed <= 1000))
                throw ValueError("Jog limited to 10 mm and 1000 mm/min");
            line = "$J=G91 G21 " + axis->get<std::string>() + fixed(distance, 3) + " F" + fixed(feed, 1);
        } else if (action == "home") {
            line = "$H";
        } else if (action == "unlock") {
            line = "$X";
        } else if (action == "zero") {
            line = "G10 L20 P1 X0 Y0 Z0";
        } else {
            throw ValueError("Unsupported command");
        }
        start({line});
    }

private:
    void write(const std::string &data) {
        if (link_) link_->write(data);
    }

    void clear() {
        queue_.clear();
        plate_.on = false;
        pending_.reset();
        active_ = paused_ = draining_ = false;
        hold_requested_at_.reset();
        held_since_.reset();
        resume_requested_at_.reset();
    }

    void fault(const std::string &text) {
        error_ = text;
        log_.append(text);
        ready_ = false;
        state_ = "Fault";
        clear();
        try {
            write("\x18");
        } catch (...) {
        }
    }

    void end_confirmed_hold(double now) {
        if (!held_since_) return;
        // A final acknowledgement can start the completion interval during the hold.
        if (pending_) sent_at_ += now - std::max(*held_since_, sent_at_);
        if (draining_) drain_after_ += now - std::max(*held_since_, drain_after_);
        held_since_.reset();
    }

    void check_timeouts(double now) {
        if (active_ && now - last_status_ > 3) {
            fault("Status timeout; job aborted");
        } else if (!held_since_) {
            if (pending_ && now - sent_at_ > options_.timeout) fault("Acknowledgement timeout; job aborted");
            else if (draining_ && now - drain_after_ > 120) fault("Completion timeout; job aborted");
            else if (hold_requested_at_ && now - *hold_requested_at_ > options_.timeout) fault("Feed hold was not confirmed; job aborted");
            else if (resume_requested_at_ && now - *resume_requested_at_ > options_.timeout) fault("Resume was not confirmed; job aborted");
        }
    }

    void idle() {
        if (!connected_ || !ready_ || error_) throw RuntimeError("Controller is not ready; reconnect after resolving the fault");
        if (active_ || paused_ || pending_ || !queue_.empty() || state_ != "Idle") throw RuntimeError("Controller must be idle");
        if (!simulated_ && options_.clock() - last_status_ > 2) throw RuntimeError("Controller status is stale");
    }

    static bool simulation_word(const std::string &word) {
        static const std::regex pattern(R"([XYZABCFLP][+-]?(?:\d+(?:\.\d*)?|\.\d+))");
        return std::regex_match(word, pattern);
    }

    double surface(double x, double y) const {
        if (options_.surface) return options_.surface(x, y);
        return 0.0015 * x - 0.001 * y + 0.00001 * std::pow(x - 40, 2);
    }

    // _simulate: the simulated position through one line; false when a probe finds no surface.
    bool simulate(const std::string &line) {
        if (line == "G91" || line == "G90") {
            relative_ = line == "G91";
            return true;
        }
        if (!line.empty() && line[0] == '$' && line.find('=') != std::string::npos) {
            // A setting written: the simulator keeps it, as a controller would report it back.
            const size_t equals = line.find('=');
            double parsed = 0;
            if (python_float(line.substr(equals + 1), parsed) && std::isfinite(parsed)) settings_[line.substr(1, equals - 1)] = parsed;
            return true;
        }
        std::vector<std::pair<char, double>> words;
        const std::vector<std::string> parts = split_space(line);
        for (size_t i = 1; i < parts.size(); ++i)
            if (simulation_word(parts[i])) {
                const char letter = parts[i][0];
                const double value = std::strtod(parts[i].c_str() + 1, nullptr);
                bool replaced = false;
                for (auto &w : words)
                    if (w.first == letter) {
                        w.second = value;
                        replaced = true;
                    }
                if (!replaced) words.emplace_back(letter, value);
            }
        std::vector<std::pair<std::string, double>> axes;
        for (const auto &w : words)
            if (std::strchr("XYZABC", w.first)) axes.emplace_back(std::string(1, static_cast<char>(std::tolower(w.first))), w.second);
        if (axes.empty()) return true;
        json position = position_;
        if (position.is_null()) {
            position = json::object();
            position["basis"] = "WPos";
            position["x"] = 0.0;
            position["y"] = 0.0;
            position["z"] = 0.0;
        }
        const auto word = [&](char letter, double &value) {
            for (const auto &w : words)
                if (w.first == letter) {
                    value = w.second;
                    return true;
                }
            return false;
        };
        const auto axis_index = [](const std::string &axis) { return axis == "x" ? 0 : axis == "y" ? 1 : axis == "z" ? 2 : -1; };
        const bool moves = starts(line, "G0 ") || starts(line, "G1 ") || starts(line, "G38.2 ");
        if (relative_ && moves)
            for (auto &a : axes)
                if (position.contains(a.first)) a.second += position[a.first].get<double>();
        const int probed = starts(line, "G38.2 ") && axes.size() == 1 ? axis_index(axes[0].first) : -1;
        if (probed >= 0 && plate_.on) {
            // Machine coordinates of the cutter's tip, and of where the probing move would end.
            const double m[3] = {position["x"].get<double>() + offset_[0], position["y"].get<double>() + offset_[1],
                                 position["z"].get<double>() + offset_[2]};
            const double target = axes[0].second + offset_[probed];
            const double low[2] = {plate_.x - plate_.wall, plate_.y - plate_.wall};  // the plate's outer faces
            double contact = 0;
            bool touches = false;
            if (probed == 2) {
                // Down onto the plate's top, when the cutter is over the plate.
                const bool over = m[0] >= low[0] && m[0] <= plate_.x + 40 && m[1] >= low[1] && m[1] <= plate_.y + 40;
                contact = plate_.top;
                touches = over && m[2] >= contact && target <= contact;
            } else {
                // Sideways against an outer face: the tip is below the plate's top and beside its other span,
                // and the cutter's edge, a radius ahead of its centre, meets the face.
                const int other = 1 - probed;
                contact = low[probed] - plate_.radius;
                touches = m[2] < plate_.top && m[other] >= low[other] && m[other] <= (other == 0 ? plate_.x : plate_.y) + 40 &&
                          m[probed] <= contact && target >= contact;
            }
            if (!touches) {
                position[axes[0].first] = axes[0].second;
                position_ = position;
                return false;
            }
            position[axes[0].first] = contact - offset_[probed];
            double at[3] = {m[0], m[1], m[2]};
            at[probed] = contact;
            const std::string report = "[PRB:" + fixed(at[0], 3) + "," + fixed(at[1], 3) + "," + fixed(at[2], 3) + ":1]";
            probes_.push_back(report);
            log_.append(report);
            position_ = position;
            return true;
        }
        if (starts(line, "G0 ") || starts(line, "G1 ")) {
            for (const auto &a : axes) position[a.first] = a.second;
        } else if (starts(line, "G10 ")) {
            double l = 0, p = 0;
            if (word('L', l) && l == 20 && word('P', p) && p == 1)
                for (const auto &a : axes) {
                    const int k = axis_index(a.first);
                    if (k >= 0) {
                        offset_[k] += position[a.first].get<double>() - a.second;
                        position[a.first] = a.second;
                    }
                }
        } else if (starts(line, "G38.2 ") && axes.size() == 1 && axes[0].first == "z") {
            const double mx = position["x"].get<double>() + offset_[0], my = position["y"].get<double>() + offset_[1];
            const double height = surface(mx, my);
            if (axes[0].second + offset_[2] > height) {  // the move ends above the stock: no contact
                position["z"] = axes[0].second;
                position_ = position;
                return false;
            }
            position["z"] = height - offset_[2];
            const std::string report = "[PRB:" + fixed(mx, 3) + "," + fixed(my, 3) + "," + fixed(height, 3) + ":1]";
            probes_.push_back(report);
            log_.append(report);
        }
        position_ = position;
        return true;
    }

    std::string motion_status(const std::string &status, double now) {
        const std::string motion = split(status, ':')[0];
        if (resume_requested_at_) {
            if (motion == "Run" || motion == "Idle") {
                resume_requested_at_.reset();
                paused_ = false;
            } else if (motion == "Hold") {
                return "Resuming";
            }
        }
        if (!active_ && motion == "Idle") {
            paused_ = false;
            hold_requested_at_.reset();
            held_since_.reset();
        }
        if (active_ && motion == "Hold" && !paused_) {
            // A controller-panel feed hold must also stop host transmission.
            paused_ = true;
            hold_requested_at_ = now;
        }
        if (paused_) {
            if (status == "Hold" || status == "Hold:0") {
                if (!held_since_) held_since_ = now;
            } else if (held_since_) {
                end_confirmed_hold(now);
                hold_requested_at_ = now;
            }
            if (hold_requested_at_ && (motion == "Idle" || motion == "Run") && !draining_) return "Hold requested";
        }
        return status == "Hold:1" ? "Decelerating" : motion;
    }

    void receive(const std::string &line) {
        const double now = options_.clock();
        ++receive_sequence_;
        log_.append(line);
        if (!line.empty() && line.front() == '<' && line.back() == '>' && line.size() >= 2) {
            const std::vector<std::string> parts = split(line.substr(1, line.size() - 2), '|');
            last_status_ = now;
            const std::string reported = split(parts[0], ':')[0];
            for (size_t i = 1; i < parts.size(); ++i) {
                const std::string &part = parts[i];
                if (starts(part, "MPos:") || starts(part, "WPos:")) {
                    std::vector<double> values;
                    bool good = true;
                    for (const std::string &v : split(split(part, ':')[1], ',')) {
                        double x = 0;
                        if (!python_float(v, x)) {
                            good = false;
                            break;
                        }
                        values.push_back(x);
                    }
                    if (good && values.size() >= 3 && values.size() <= 9 && std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); })) {
                        position_count_ = static_cast<int>(values.size());
                        json position = json::object();
                        static const char *names = "xyzabcuvw";
                        for (size_t k = 0; k < values.size(); ++k) position[std::string(1, names[k])] = values[k];
                        position["basis"] = part.substr(0, 4);
                        position_ = position;
                    } else {
                        position_count_ = 0;
                    }
                }
            }
            if (reported == "Alarm") {
                fault("Controller alarm; reconnect after resolving it");
                return;
            }
            if (!error_) state_ = motion_status(parts[0], now);
            if (draining_ && reported == "Idle" && receive_sequence_ > drain_sequence_ && !error_) {
                active_ = draining_ = paused_ = false;
                hold_requested_at_.reset();
                held_since_.reset();
                resume_requested_at_.reset();
                progress_ = 100;
            }
            if (settings_.contains("30") && settings_.contains("32") && !pending_ && init_remaining_.empty() && !error_) ready_ = true;
        } else if (!line.empty() && line[0] == '$' && line.find('=') != std::string::npos) {
            const size_t equals = line.find('=');
            std::string key = line.substr(1, equals - 1);
            const std::string value = line.substr(equals + 1);
            if (lower(key) == "spindle/type") {
                spindle_type_ = upper(strip(value));
                return;
            }
            if (lower(key) == "laser/fullpower") key = "Laser/FullPower";
            const std::vector<std::string> words = split_space(value);
            double parsed = 0;
            if (!words.empty() && python_float(words[0], parsed)) {
                if (!std::isfinite(parsed)) {
                    fault("Nonfinite controller setting readback");
                    return;
                }
                settings_[key] = parsed;
            }
        } else if (line == "ok") {
            if (!pending_) return;
            if (*pending_ == "$I" && std::find(identification_.begin(), identification_.end(), "[FIRMWARE:grblHAL]") != identification_.end() &&
                !settings_.contains("376")) {
                static const std::regex axes(R"(\[AXS:[4-9]:XYZ[A-Z]+\])");
                if (std::any_of(identification_.begin(), identification_.end(), [](const std::string &row) { return std::regex_match(row, axes); }))
                    init_remaining_.push_back("$376");
            }
            static const std::regex xpro(R"(\[MSG:Using machine:CNC_xPRO_V5_XYAZ_(?:NC|NO)\])");
            if (*pending_ == "$I" && std::any_of(identification_.begin(), identification_.end(), [](const std::string &row) { return std::regex_match(row, xpro); })) {
                init_remaining_.push_back("$Spindle/Type");
                init_remaining_.push_back("$Laser/FullPower");
            }
            pending_.reset();
            if (!active_ && !init_remaining_.empty() && !error_) {
                pending_ = init_remaining_.front();
                init_remaining_.pop_front();
                write(*pending_ + "\n");
                sent_at_ = now;
            }
            if (active_) {
                ++acked_;
                progress_ = std::min(99.0, 100.0 * static_cast<double>(acked_) / static_cast<double>(total_));
                if (queue_.empty()) {
                    draining_ = true;
                    drain_after_ = now;
                    // Serial receive order is causal even when the platform monotonic clock returns the same
                    // tick for both messages.
                    drain_sequence_ = receive_sequence_;
                    write("?");
                }
            }
        } else if (starts(line, "[PRB:")) {
            if (active_ && probes_.size() < 1000) probes_.push_back(line);
        } else if (starts(line, "error:") || starts(line, "ALARM:")) {
            fault(line);
        } else if (starts(line, "[VER:") || starts(line, "[OPT:") || starts(line, "[FIRMWARE:") || starts(line, "[AXS:") ||
                   starts(line, "[MSG:Using machine:")) {
            identification_.push_back(line);
            if (identification_.size() > 8) identification_.erase(identification_.begin(), identification_.end() - 8);
        } else if (starts(line, "Grbl") || starts(line, "FluidNC")) {
            banner_ = line.substr(0, 80);
            settings_ = json::object();
            identification_.clear();
            position_count_ = 0;
            spindle_type_.reset();
            ready_ = false;
            if (active_ || initialized_) fault("Unexpected controller reset; reconnect and verify settings");
        }
    }

    // stop.wait(0.01) of the Python loop: true when the loop is to end. A test steps the loop through options_.wait.
    bool pause() {
        if (options_.wait) {
            if (options_.wait()) return true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return stop_;
    }

    void loop() {
        const double started = options_.clock();
        double polled = started;
        bool initialized = false;
        try {
            for (;;) {
                if (pause()) return;
                std::lock_guard<std::recursive_mutex> hold(m_);
                if (stop_) return;
                double now = options_.clock();
                if (simulated_) {
                    if (active_ && !paused_) {
                        if (!queue_.empty()) {
                            const std::string line = queue_.front();
                            queue_.pop_front();
                            log_.append(line);
                            if (!simulate(line)) {
                                // As Grbl does when a G38.2 probe ends without contact.
                                fault("ALARM:5");
                                continue;
                            }
                            ++acked_;
                            progress_ = std::min(99.0, static_cast<double>(acked_) / static_cast<double>(total_) * 100);
                        } else {
                            active_ = false;
                            plate_.on = false;
                            state_ = "Idle";
                            progress_ = 100;
                        }
                    }
                    continue;
                }
                // Allow USB-reset boot banners to settle before querying configuration.
                if (!initialized && now - started > 2) {
                    write("$$\n");
                    pending_ = "$$";
                    sent_at_ = now;
                    initialized = true;
                    initialized_ = true;
                }
                if (!link_) return;
                const std::string raw = link_->readline();
                now = options_.clock();
                // An overdue reply cannot revive a lost link or confirm a late hold.
                check_timeouts(now);
                if (!raw.empty()) receive(strip(ascii(raw)));
                if (now - polled > 0.25) {
                    write("?");
                    polled = now;
                }
                if (active_ && !paused_ && !pending_ && !queue_.empty() && !error_) {
                    pending_ = queue_.front();
                    queue_.pop_front();
                    write(*pending_ + "\n");
                    sent_at_ = now;
                }
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::recursive_mutex> hold(m_);
            fault(std::string("Serial connection failed: ") + e.what());
            connected_ = false;
        }
    }

    MachineOptions options_;
    std::vector<std::string> queries_;
    std::recursive_mutex m_;
    std::deque<std::string> init_remaining_;
    bool initialized_ = false;
    std::vector<std::string> identification_;
    std::optional<std::string> banner_, spindle_type_, error_, pending_;
    int position_count_ = 0;
    std::unique_ptr<Link> link_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool connected_ = false, simulated_ = false, active_ = false, paused_ = false, draining_ = false, ready_ = false;
    std::string state_ = "Disconnected";
    json position_;
    json settings_ = json::object();
    Log log_;
    std::deque<std::string> queue_;
    double sent_at_ = 0, last_status_ = 0, drain_after_ = 0, progress_ = 0;
    long total_ = 0, acked_ = 0, receive_sequence_ = 0, drain_sequence_ = 0;
    std::optional<double> hold_requested_at_, held_since_, resume_requested_at_;
    std::vector<std::string> probes_;
    double offset_[3] = {0, 0, 0};
    bool relative_ = false;  // the simulator's distance mode (G91)
    struct {
        bool on = false;
        double x = 0, y = 0, top = 0, thickness = 0, wall = 0, radius = 0;
    } plate_;  // the simulator's touch plate: the stock corner under it and its top, in machine coordinates
};

// ---- Marlin -------------------------------------------------------------------------------------------------

class Marlin : public Machine {
public:
    static constexpr long kMaxLines = 200000;
    static constexpr int kMaxResends = 3;
    static constexpr double kQueryTimeout = 3;

    Marlin(const MachineOptions &options, double boot_delay) : options_(options), boot_delay_(boot_delay) {
        if (!options_.clock) options_.clock = steady;
    }
    ~Marlin() override { disconnect(); }

    Json ports() override {
        json out = json::array();
        for (const PortInfo &p : options_.ports()) {
            json port = json::object();
            port["device"] = p.device;
            port["description"] = p.description;
            out.push_back(port);
        }
        return out;
    }

    Json status() override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        json out = json::object();
        out["connected"] = connected_;
        out["simulated"] = simulated_;
        out["state"] = state_;
        out["error"] = opt(error_);
        out["position"] = json();
        out["settings"] = json::object();
        out["readback"] = readback_;
        out["ready"] = ready_;
        out["progress"] = progress_json(progress_);
        out["active"] = active_;
        out["log"] = log_.to_json();
        out["protocol"] = "marlin";
        out["notes"] =
            "Host pause lets buffered motion drain. Position is not polled. Completion uses M400. M112 immediacy depends on firmware; "
            "use the hardware emergency stop when needed.";
        return out;
    }

    static std::string frame(long number, const std::string &line) {
        const std::string payload = "N" + std::to_string(number) + " " + line;
        unsigned checksum = 0;
        for (const unsigned char c : payload) checksum ^= c;
        return payload + "*" + std::to_string(checksum) + "\n";
    }

    void connect(const std::string &port) override {
        if (port != "simulator") {
            bool listed = false;
            for (const PortInfo &p : options_.ports()) listed = listed || p.device == port;
            if (!listed) throw ValueError("Select an available USB serial port");
        }
        disconnect();
        std::lock_guard<std::recursive_mutex> hold(m_);
        simulated_ = port == "simulator";
        error_.reset();
        number_ = 0;
        progress_ = 0;
        acked_ = total_ = 0;
        receive_buffer_.clear();
        readback_ = json::object();
        queries_.clear();
        if (!simulated_) queries_ = {"M115", "M211", "M503"};
        ready_ = simulated_;
        state_ = simulated_ ? "Idle" : "Connecting";
        if (!simulated_) {
            try {
                link_ = options_.open(port);
            } catch (const std::exception &e) {
                state_ = "Disconnected";
                throw RuntimeError(std::string("Cannot open serial port: ") + e.what());
            }
        }
        connected_ = true;
        stop_ = false;
        thread_ = std::thread([this] { loop(); });
    }

    void disconnect() override {
        {
            std::lock_guard<std::recursive_mutex> hold(m_);
            stop_ = true;
            if (link_) {
                try {
                    if (active_) write("M112\n");
                    link_->close();
                } catch (...) {
                }
                link_.reset();
            }
            connected_ = ready_ = false;
            state_ = "Disconnected";
            clear();
        }
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }

    void start(const std::vector<std::string> &given) override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        idle();
        if (given.empty() || static_cast<long>(given.size()) > kMaxLines) throw ValueError("Job must contain 1\xe2\x80\x93" "200000 lines");
        static const std::regex numbered(R"(\s*[Nn]\d.*)"), renumber(R"(\s*[Mm]0*110(?:\D.*|$))");
        for (const std::string &s : given) {
            const bool printable = std::all_of(s.begin(), s.end(), [](char c) { return c >= 32 && c <= 126; });
            if (s.size() > 78 || !printable || s.find_first_of("*()\\\"") != std::string::npos || std::regex_match(s, numbered) ||
                std::regex_match(s, renumber))
                throw ValueError("Invalid or pre-numbered G-code line");
        }
        std::vector<std::string> lines;
        for (const std::string &s : given) {
            const std::string bare = strip(s.substr(0, s.find(';')));
            if (!bare.empty()) lines.push_back(bare);
        }
        if (lines.empty()) throw ValueError("Job contains no commands");
        queue_.clear();
        for (const std::string &s : lines) queue_.emplace_back(s, "job");
        queue_.emplace_back("M400", "finish");
        total_ = static_cast<long>(lines.size());
        acked_ = 0;
        progress_ = 0;
        active_ = true;
        state_ = "Run";
    }

    void command(const std::string &action, const Json &fields) override {
        std::lock_guard<std::recursive_mutex> hold(m_);
        if (!connected_) throw RuntimeError("Connect first");
        if (action == "reset") {
            fault("M112 emergency abort requested");
            return;
        }
        if (action == "hold") {
            if (!active_ || error_) throw RuntimeError("No active job to pause");
            paused_ = true;
            state_ = "Hold";
            log_.append("Host paused; buffered motion may continue");
            return;
        }
        if (action == "resume") {
            if (!active_ || !paused_ || !ready_ || error_) throw RuntimeError("No resumable held job");
            paused_ = false;
            state_ = "Run";
            return;
        }
        idle();
        if (action == "jog") {
            const auto field = [&](const char *name) -> const json * {
                const auto found = fields.find(name);
                return found == fields.end() ? nullptr : &*found;
            };
            double distance = 0, feed = 0;
            if (!python_float(field("distance"), distance) || !python_float(field("feed"), feed))
                throw ValueError("Invalid jog distance/feed");
            const json *axis = field("axis");
            if (!axis || !(axis->is_string() && (*axis == json("X") || *axis == json("Y") || *axis == json("Z"))) || !std::isfinite(distance) ||
                !std::isfinite(feed) || !(0 < std::fabs(distance) && std::fabs(distance) <= 10) || !(0 < feed && feed <= 1000))
                throw ValueError("Jog requires XYZ, at most 10 mm and 1000 mm/min");
            start({"G21", "G91", "G1 " + axis->get<std::string>() + fixed(distance, 3) + " F" + fixed(feed, 1), "G90"});
        } else if (action == "home") {
            start({"G28"});
        } else if (action == "zero") {
            start({"G92 X0 Y0 Z0"});
        } else {
            throw ValueError("Unsupported Marlin command");
        }
    }

private:
    struct Pending {
        long number;
        std::string framed, kind;
    };

    void write(const std::string &data) {
        if (link_) link_->write(data);
    }

    void clear() {
        queue_.clear();
        pending_.reset();
        resend_handshake_ = false;
        active_ = paused_ = false;
    }

    void fault(const std::string &message) {
        error_ = message + "; reconnect required. Motion stop is not confirmed.";
        log_.append(*error_);
        ready_ = false;
        state_ = "Fault";
        clear();
        try {
            write("M112\n");
        } catch (...) {
        }
    }

    void idle() {
        if (!connected_ || !ready_ || error_) throw RuntimeError("Controller not ready; reconnect after resolving faults");
        if (active_ || pending_ || state_ != "Idle") throw RuntimeError("Controller must be idle");
    }

    void send(const std::string &line, const std::string &kind) {
        const long number = number_++;
        pending_ = Pending{number, frame(number, line), kind};
        resends_ = 0;
        resend_handshake_ = false;
        sent_at_ = options_.clock();
        write(pending_->framed);
        log_.append("> N" + std::to_string(number) + " " + line);
    }

    static double round3(double value) {
        double out = 0;
        return text::rounded(value, 3, out) ? out : std::round(value * 1000) / 1000;
    }

    // _note: what a reply to M115, M211 or M503 says about the machine.
    void note(const std::string &line) {
        static const std::regex firmware(R"(FIRMWARE_NAME:(.+?)(?=\s+[A-Z_]+:|$))"), machine(R"(MACHINE_TYPE:(\S+))");
        std::smatch found;
        if (std::regex_search(line, found, firmware)) {
            readback_["firmware"] = strip(found[1].str()).substr(0, 80);
            std::smatch type;
            if (std::regex_search(line, type, machine)) readback_["machine_type"] = type[1].str().substr(0, 40);
        }
        const std::string number = R"((-?\d+(?:\.\d*)?))";
        static const std::regex endstops(R"(Soft endstops:\s*(ON|OFF))", std::regex::icase);
        if (std::regex_search(line, found, endstops)) {
            readback_["soft_endstops"] = upper(found[1].str()) == "ON";
            static const std::regex limits(R"(Min:\s*X)" + number + R"(\s*Y)" + number + R"(\s*Z)" + number + R"(\s*Max:\s*X)" + number +
                                           R"(\s*Y)" + number + R"(\s*Z)" + number);
            std::smatch range;
            if (std::regex_search(line, range, limits)) {
                json travel = json::array();
                for (int k = 0; k < 3; ++k) travel.push_back(round3(std::strtod(range[4 + k].str().c_str(), nullptr) - std::strtod(range[1 + k].str().c_str(), nullptr)));
                readback_["travel"] = travel;
            }
        }
        static const std::regex rates(R"(\bM203\s+X)" + number + R"(\s+Y)" + number + R"(\s+Z)" + number);
        if (std::regex_search(line, found, rates)) {
            json feeds = json::array();
            for (int k = 1; k <= 3; ++k) feeds.push_back(round3(std::strtod(found[k].str().c_str(), nullptr) * 60));  // units a second
            readback_["max_feed"] = feeds;
        }
    }

    void queries_done() {
        queries_.clear();
        ready_ = true;
        state_ = "Idle";
    }

    void receive(const std::string &line) {
        log_.append(line);
        static const std::regex resend(R"(^(?:Resend:\s*|rs\s+N?)(\d+)\b.*)", std::regex::icase);
        std::smatch found;
        if (std::regex_match(line, found, resend)) {
            const long number = std::strtol(found[1].str().c_str(), nullptr, 10);
            if (!pending_ || number != pending_->number || resends_ >= kMaxResends) {
                fault("Unsupported or excessive resend; refusing to replay acknowledged motion");
                return;
            }
            ++resends_;
            sent_at_ = options_.clock();
            // Marlin flush_and_request_resend emits Resend then an extra ok. That ok is NOT acknowledgement of the
            // retransmitted command.
            resend_handshake_ = true;
            return;
        }
        const std::string low = lower(line);
        // Marlin reports checksum/sequence errors before requesting a resend.
        if (starts(low, "error:")) {
            if (low.find("checksum") != std::string::npos || low.find("line number") != std::string::npos ||
                low.find("no line number") != std::string::npos)
                return;  // A matching resend must arrive before the ordinary timeout.
            if (pending_ && pending_->kind == "query") return;  // A firmware without this report still works.
            fault(line);
            return;
        }
        if (starts(low, "!!") || starts(low, "echo:busy: paused for user")) {
            fault(line);
            return;
        }
        if (low == "start") {
            if (ready_ || pending_) fault("Unexpected firmware restart");
            return;
        }
        static const std::regex ok(R"(^ok(?:\s.*|$))", std::regex::icase);
        if (!std::regex_match(line, ok) || !pending_) {
            if (pending_ && pending_->kind == "query") note(line);
            return;
        }
        if (resend_handshake_) {
            resend_handshake_ = false;
            sent_at_ = options_.clock();
            write(pending_->framed);
            return;
        }
        static const std::regex acknowledged(R"(\bN(\d+)\b)", std::regex::icase);
        if (std::regex_search(line, found, acknowledged) && std::strtol(found[1].str().c_str(), nullptr, 10) != pending_->number) {
            fault("Acknowledgement line number mismatch");
            return;
        }
        const std::string kind = pending_->kind;
        pending_.reset();
        if (kind == "init" || kind == "query") {
            if (!queries_.empty()) {
                const std::string next = queries_.front();
                queries_.pop_front();
                send(next, "query");
            } else {
                queries_done();
            }
        } else if (kind == "finish") {
            active_ = paused_ = false;
            progress_ = 100;
            state_ = "Idle";
        } else {
            ++acked_;
            progress_ = std::min(99.0, 100.0 * static_cast<double>(acked_) / static_cast<double>(total_));
        }
    }

    bool pause() {
        if (options_.wait) {
            if (options_.wait()) return true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return stop_;
    }

    void loop() {
        const double started = options_.clock();
        bool initialized = simulated_;
        try {
            for (;;) {
                if (pause()) return;
                std::lock_guard<std::recursive_mutex> hold(m_);
                if (stop_) return;
                const double now = options_.clock();
                if (!simulated_) {
                    if (!link_) return;
                    receive_buffer_ += link_->readline(257);
                    if (receive_buffer_.size() > 256) {
                        if (pending_ && pending_->kind == "query") note(ascii(receive_buffer_));  // what was read is kept
                        else fault("Oversized firmware response");
                        receive_buffer_.clear();
                    } else if (!receive_buffer_.empty() && receive_buffer_.back() == '\n') {
                        receive(strip(ascii(receive_buffer_)));
                        receive_buffer_.clear();
                    }
                    if (!initialized && !error_ && now - started >= boot_delay_) {
                        send("M110 N0", "init");
                        initialized = true;
                    }
                } else if (pending_) {
                    receive("ok");
                }
                if (pending_ && pending_->kind == "query" && now - sent_at_ > kQueryTimeout) {
                    // The machine is usable without its reports; say so and carry on.
                    log_.append("The firmware did not report its settings");
                    pending_.reset();
                    queries_done();
                }
                if (pending_ && now - sent_at_ > options_.timeout) fault("Acknowledgement/completion timeout");
                if (active_ && !paused_ && !pending_ && !queue_.empty() && !error_) {
                    const auto next = queue_.front();
                    queue_.pop_front();
                    send(next.first, next.second);
                }
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::recursive_mutex> hold(m_);
            fault(std::string("Serial connection failed: ") + e.what());
            connected_ = false;
        }
    }

    MachineOptions options_;
    double boot_delay_;
    std::recursive_mutex m_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::unique_ptr<Link> link_;
    bool connected_ = false, simulated_ = false, ready_ = false, active_ = false, paused_ = false, resend_handshake_ = false;
    std::string state_ = "Disconnected";
    std::optional<std::string> error_;
    json readback_ = json::object();
    std::deque<std::string> queries_;
    Log log_;
    std::deque<std::pair<std::string, std::string>> queue_;
    std::optional<Pending> pending_;
    long number_ = 0, total_ = 0, acked_ = 0;
    double progress_ = 0, sent_at_ = 0;
    int resends_ = 0;
    std::string receive_buffer_;
};

}  // namespace

std::unique_ptr<Machine> grbl_machine(const MachineOptions &options, std::vector<std::string> queries) {
    return std::make_unique<Grbl>(options, std::move(queries));
}

std::unique_ptr<Machine> marlin_machine(const MachineOptions &options, double boot_delay) {
    return std::make_unique<Marlin>(options, boot_delay);
}

}  // namespace server
}  // namespace kerfkite
