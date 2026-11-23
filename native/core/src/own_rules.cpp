// The core's own rules where the Python reference answers otherwise. See own_rule in kerfkite/cam.hpp.
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "kerfkite/cam.hpp"

namespace kerfkite {
namespace {

std::atomic<bool> g_reference{false};
thread_local std::vector<std::string> t_applied;

}  // namespace

void set_reference_rules(bool on) { g_reference.store(on); }

bool reference_rules() { return g_reference.load(); }

void own_rule(const char *what) {
    if (g_reference.load()) throw NotPorted(what);
    if (t_applied.size() < 64 && std::find(t_applied.begin(), t_applied.end(), what) == t_applied.end()) t_applied.emplace_back(what);
}

std::vector<std::string> take_own_rules() {
    std::vector<std::string> out;
    out.swap(t_applied);
    return out;
}

}  // namespace kerfkite
