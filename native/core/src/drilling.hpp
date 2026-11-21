// Router drilling at point markers and circular hole outlines: studio/drilling.py. Internal to the core.
#ifndef KERFKITE_DRILLING_HPP
#define KERFKITE_DRILLING_HPP

#include <array>
#include <vector>

#include "project.hpp"

namespace kerfkite {
namespace detail {

// studio.drilling.generate_drill(project, paths, number). paths are the validated XY paths of the project,
// and the project has already passed the checks that studio.cam makes before it reaches drilling.
Job drill_job(const json &project, const std::vector<std::vector<std::array<double, 2>>> &paths);

}  // namespace detail
}  // namespace kerfkite

#endif
