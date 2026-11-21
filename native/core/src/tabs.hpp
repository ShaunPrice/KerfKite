// Holding tabs on router contours: studio.cam._holding_tabs. Internal to the core.
#ifndef KERFKITE_TABS_HPP
#define KERFKITE_TABS_HPP

#include <string>
#include <vector>

#include "project.hpp"
#include "simple_line.hpp"

namespace kerfkite {
namespace detail {

// Replace each closed contour with one that rises to the tab height over each tab, and give the height at
// every vertex of the result. depth is the validated cut depth on entry and, on return, the deepest height
// of the result as the reference recomputes it. The three tab warnings are appended to warnings.
void holding_tabs(const json &project, std::vector<Contour> &paths, double &depth,
                  std::vector<std::vector<double>> &depths, std::vector<std::string> &warnings);

}  // namespace detail
}  // namespace kerfkite

#endif
