// The two questions the reference asks Shapely about a contour before it ramps along it or leaves tabs on
// it: LineString(path).is_simple and LineString(path).length. Internal to the core.
#ifndef KERFKITE_SIMPLE_LINE_HPP
#define KERFKITE_SIMPLE_LINE_HPP

#include <array>
#include <cstdint>
#include <vector>

namespace kerfkite {
namespace detail {

using Contour = std::vector<std::array<double, 2>>;

// What one call of line_is_simple did: the work, in units of about equal time (a few nanoseconds), and
// whether it used the tree over the segments. The units are counted, not timed, so they are the same on
// every machine.
struct SimpleLineWork {
    std::uint64_t units = 0;
    bool indexed = false;
};

// The most work line_is_simple does before it declines a contour: about a second.
extern const std::uint64_t kSimpleLineBudget;

// shapely's LineString(path).is_simple, by the rules GEOS applies to one line: no two segments cross,
// overlap or touch, except neighbours at their shared vertex and a closed line at its seam.
// Throws NotPorted for the rare contour whose answer depends on rounding inside GEOS, and for one whose
// test would take more than kSimpleLineBudget. work, when given, is filled in either way.
bool line_is_simple(const Contour &path, SimpleLineWork *work = nullptr);

// The same answer by GEOS's test of every pair of segments whose boxes overlap, with no tree and no limit
// on the work: what line_is_simple is compared with in tests. The two meet the pairs in different orders, so
// either may decline a contour the other answers; where both answer, they answer alike.
bool line_is_simple_every_pair(const Contour &path, SimpleLineWork *work = nullptr);

// LineString(path).length < limit. Throws NotPorted when the length is within rounding of the limit.
bool line_shorter_than(const Contour &path, double limit);

}  // namespace detail
}  // namespace kerfkite

#endif
