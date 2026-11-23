// One-use jobs prepared at the workstation for a paired phone: studio/phone_jobs.py. The phone's Start is
// confirmed by the editor that armed the preview, through a challenge only that editor sees.
#ifndef KERFKITE_SERVER_PHONE_JOBS_HPP
#define KERFKITE_SERVER_PHONE_JOBS_HPP

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kerfkite/json.hpp"
#include "machine.hpp"

namespace kerfkite {
namespace server {

class PhoneJobs {
public:
    PhoneJobs(Device &device, bool allow_physical, double confirmation_timeout = 1.8, std::function<double()> clock = {});

    // The arm id for an armed workstation preview (empty for a simulator-only arm without one).
    std::string arm(const std::vector<std::string> &lines, std::function<void()> validator, bool require_workstation);
    bool disarm(const std::optional<std::string> &arm_id = std::nullopt);
    void invalidate_controls();
    void require_current_controls(const Json *epoch);
    std::string control_epoch();
    Json status();
    Json check_preview(const std::string &arm_id);
    void confirm_preview(const std::string &arm_id, const Json *challenge);
    // The phone's Start: waits for the editor's confirmation, then starts the armed lines.
    void start();
    // The device's lock, which the jobs share (phone_jobs.lock).
    RLock &lock_ref() { return device_.lock; }
    bool allow_physical() const { return allow_physical_; }

private:
    void expire_absent_editor();
    Device &device_;
    bool allow_physical_;
    double confirmation_timeout_;
    std::function<double()> clock_;
    std::optional<std::string> arm_id_, challenge_;
    std::string control_epoch_;
    double challenge_deadline_ = 0, owner_seen_ = 0;
    bool confirmed_ = false;
    std::optional<std::vector<std::string>> lines_;
    std::function<void()> validator_;
};

}  // namespace server
}  // namespace kerfkite

#endif
