// Drag-knife paths: the knife branch of studio/operations.py. Internal to the core.
#ifndef KERFKITE_KNIFE_HPP
#define KERFKITE_KNIFE_HPP

#include <array>
#include <vector>

#include "project.hpp"

namespace kerfkite {
namespace detail {

// The warning studio.operations.prepare returns with knife paths.
extern const char *const KNIFE_WARNING;

// studio.operations.prepare(project, paths) for a knife cutting lines, then the work-area check that
// studio.cam makes on compensated geometry. The blade trails its pivot by the offset, so each path gains an
// alignment lead-in and a swivel arc at every corner. The result is empty when no path has any travel.
std::vector<std::vector<std::array<double, 2>>> knife_paths(
    const json &project, const std::vector<std::vector<std::array<double, 2>>> &paths, double width, double height);

}  // namespace detail
}  // namespace kerfkite

#endif
