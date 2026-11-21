// Polygon operations on closed contours: studio/operations.py. Internal to the core.
#ifndef KERFKITE_OPERATIONS_HPP
#define KERFKITE_OPERATIONS_HPP

#include <array>
#include <string>
#include <vector>

#include "project.hpp"

namespace kerfkite {
namespace detail {

// studio.operations.prepare(project, paths) for an operation other than "line" on a router or laser,
// from the point where the reference builds the even-odd region, followed by the check studio.cam makes
// on the compensated coordinates against the work area.
//
// paths holds the validated contours on entry and the tool paths of the operation on return. The result
// is the operation's warnings, which the job lists after the controller's note.
//
// Reproduced: inside and outside offsets (router: half the tool diameter; laser: half the kerf),
// concentric pockets, and hatch and cross-hatch fills. Declined with NotPorted: V-carving, and every polygon
// operation in a build without GEOS. Rejections are the reference's, in the reference's order.
std::vector<std::string> prepare_operation(const json &project, std::vector<std::vector<std::array<double, 2>>> &paths,
                                           double width, double height);

}  // namespace detail
}  // namespace kerfkite

#endif
