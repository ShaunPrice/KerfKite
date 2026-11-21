// The two questions the reference asks Shapely about a contour before it ramps along it or leaves tabs on
// it: LineString(path).is_simple and LineString(path).length. Internal to the core.
#ifndef KERFKITE_SIMPLE_LINE_HPP
#define KERFKITE_SIMPLE_LINE_HPP

#include <array>
#include <vector>

namespace kerfkite {
namespace detail {

using Contour = std::vector<std::array<double, 2>>;

// shapely's LineString(path).is_simple, by the rules GEOS applies to one line: no two segments cross,
// overlap or touch, except neighbours at their shared vertex and a closed line at its seam.
// Throws NotPorted for the rare contour whose answer depends on rounding inside GEOS.
bool line_is_simple(const Contour &path);

// LineString(path).length < limit. Throws NotPorted when the length is within rounding of the limit.
bool line_shorter_than(const Contour &path, double limit);

}  // namespace detail
}  // namespace kerfkite

#endif
