// Drag-knife paths: the knife branch of studio/operations.py. Internal to the core.
#ifndef KERFKITE_KNIFE_HPP
#define KERFKITE_KNIFE_HPP

#include <array>
#include <string>
#include <vector>

#include "project.hpp"

namespace kerfkite {
namespace detail {

// studio.operations.prepare(project, paths) for a knife cutting lines, then the work-area check that
// studio.cam makes on compensated geometry. The blade trails its pivot by the offset, so each path gains an
// alignment lead-in and a swivel arc at every corner. Points within 0.1 nm of the last point kept are
// dropped first, and a path that is then a single point is refused. warnings gains the reference's knife
// warning.
std::vector<std::vector<std::array<double, 2>>> knife_paths(
    const json &project, const std::vector<std::vector<std::array<double, 2>>> &paths, double width, double height,
    std::vector<std::string> &warnings);

}  // namespace detail
}  // namespace kerfkite

#endif
