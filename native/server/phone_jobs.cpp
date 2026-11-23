// studio/phone_jobs.py. See phone_jobs.hpp.
#include "phone_jobs.hpp"

#include <chrono>
#include <utility>

#include "kerfkite/errors.hpp"
#include "os_random.hpp"

namespace kerfkite {
namespace server {

namespace {

double steady() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool flag(const Json &state, const char *name) {
    const auto found = state.find(name);
    return found != state.end() && found->is_boolean() && found->get<bool>();
}

}  // namespace

PhoneJobs::PhoneJobs(Device &device, bool allow_physical, double confirmation_timeout, std::function<double()> clock)
    : device_(device), allow_physical_(allow_physical), confirmation_timeout_(confirmation_timeout), clock_(clock ? clock : steady) {
    control_epoch_ = token_urlsafe(18);
}

std::string PhoneJobs::arm(const std::vector<std::string> &lines, std::function<void()> validator, bool require_workstation) {
    Hold hold(device_.lock);
    const Json state = device_.status();
    if (!flag(state, "connected") || (!flag(state, "simulated") && !allow_physical_) || !(state.at("state") == Json("Idle")) || flag(state, "active"))
        throw ValueError("Connect an idle controller before arming (physical control needs operator opt-in)");
    if (!require_workstation && !flag(state, "simulated")) throw ValueError("A physical machine requires an armed workstation preview.");
    disarm();
    // A paired action approved for an older preview must not start its replacement.
    control_epoch_ = token_urlsafe(18);
    lines_ = lines;
    validator_ = std::move(validator);
    if (require_workstation) {
        // This opaque handle is returned only to the editor that owns the preview.
        arm_id_ = token_urlsafe(32);
        owner_seen_ = clock_();
    }
    return arm_id_ ? *arm_id_ : std::string();
}

bool PhoneJobs::disarm(const std::optional<std::string> &arm_id) {
    Hold hold(device_.lock);
    if (arm_id && arm_id != arm_id_) return false;
    lines_.reset();
    validator_ = nullptr;
    arm_id_.reset();
    challenge_.reset();
    confirmed_ = false;
    device_.lock.notify_all();
    return true;
}

void PhoneJobs::invalidate_controls() {
    Hold hold(device_.lock);
    control_epoch_ = token_urlsafe(18);
    disarm();
}

void PhoneJobs::require_current_controls(const Json *epoch) {
    Hold hold(device_.lock);
    if (!epoch || !epoch->is_string() || epoch->get<std::string>().empty())
        throw ValueError("Update the phone pendant or refresh the monitor before using job or pendant controls. Stop remains available.");
    if (control_epoch_ != epoch->get<std::string>())
        throw ValueError("Controller controls changed. Read the latest status before issuing another action.");
}

std::string PhoneJobs::control_epoch() {
    Hold hold(device_.lock);
    return control_epoch_;
}

void PhoneJobs::expire_absent_editor() {
    if (arm_id_ && clock_() - owner_seen_ > 8) disarm();
}

Json PhoneJobs::status() {
    Hold hold(device_.lock);
    expire_absent_editor();
    Json out = Json::object();
    out["armed"] = lines_.has_value();
    return out;
}

Json PhoneJobs::check_preview(const std::string &arm_id) {
    Hold hold(device_.lock);
    expire_absent_editor();
    Json out = Json::object();
    if (!arm_id_ || *arm_id_ != arm_id) {
        out["armed"] = false;
        return out;
    }
    owner_seen_ = clock_();
    out["armed"] = true;
    out["challenge"] = challenge_ ? Json(*challenge_) : Json();
    return out;
}

void PhoneJobs::confirm_preview(const std::string &arm_id, const Json *challenge) {
    Hold hold(device_.lock);
    const bool matches = challenge && challenge->is_string() && challenge_ && challenge->get<std::string>() == *challenge_;
    if (!challenge_ || !matches || !arm_id_ || *arm_id_ != arm_id || confirmed_ || clock_() >= challenge_deadline_)
        throw ValueError("Phone Start request expired or cancelled. Check controller status before rearming.");
    confirmed_ = true;
    device_.lock.notify_all();
}

void PhoneJobs::start() {
    Hold hold(device_.lock);
    expire_absent_editor();
    if (!lines_) throw ValueError("Preview and arm a job on the workstation first.");
    if (challenge_) throw ValueError("A phone Start is already waiting for the workstation.");
    const std::optional<std::string> arm_id = arm_id_;
    if (arm_id) {
        challenge_ = token_urlsafe(32);
        challenge_deadline_ = clock_() + confirmation_timeout_;
        // The wait gives up the controller lock: stop, disconnect and revocation can invalidate this exact arm
        // while its editor responds.
        device_.lock.wait_for([&] { return arm_id_ != arm_id || confirmed_; }, confirmation_timeout_);
        if (arm_id_ != arm_id) throw ValueError("The armed preview changed or was disarmed. No job started.");
        if (!confirmed_ || clock_() >= challenge_deadline_) {
            disarm();
            throw ValueError("The workstation did not confirm this preview in time. No job started. Reopen and rearm the preview.");
        }
    }
    const std::vector<std::string> lines = *lines_;
    const std::function<void()> validator = validator_;
    disarm();  // A failed or ambiguous dispatch also consumes the arm.
    if (!flag(device_.status(), "simulated") && (!allow_physical_ || !arm_id)) throw ValueError("Physical phone starts require commissioning.");
    if (validator) validator();
    device_.start(lines);
}

}  // namespace server
}  // namespace kerfkite
