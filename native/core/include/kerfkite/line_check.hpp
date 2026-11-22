// Whether a line crosses itself, as the core decides it before a ramp entry or holding tabs: the answer
// Shapely's LineString.is_simple gives, by the rules of GEOS. This header is for tests and measurements;
// generate() asks the same question itself. See core/src/simple_line.cpp.
#ifndef KERFKITE_LINE_CHECK_HPP
#define KERFKITE_LINE_CHECK_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace kerfkite {

enum class LineMethod {
    Usual,      // as generate() does: a tree over the segments for a long line, a plain sweep for a short one
    Tree,       // the tree whatever the length of the line
    EveryPair,  // GEOS's test of every pair of segments whose boxes overlap, with no limit on the work
};

struct LineCheck {
    bool simple = false;
    // The core would decline a job with this contour, for this reason: its answer depends on rounding
    // inside GEOS, or the test would take more work than line_check_budget().
    bool declined = false;
    std::string reason;
    // The work done, in units of about equal time. Counted, not timed: the same on every machine.
    std::uint64_t work = 0;
    bool tree = false;  // the tree was used
};

// budget, when not zero, takes the place of line_check_budget() for this call (EveryPair has no limit).
LineCheck check_line(const std::vector<std::array<double, 2>> &path, LineMethod method = LineMethod::Usual,
                     std::uint64_t budget = 0);

// The most work the usual method does before it declines a contour.
std::uint64_t line_check_budget();

// geos::algorithm::Orientation::index(p1, p2, q), as Shapely's predicates take it: 1 when q is to the left of
// p1 -> p2, -1 to the right, 0 on the line. Throws NotPorted where GEOS's double-double arithmetic would give
// another sign than the exact one.
int geos_orientation(const std::array<double, 2> &p1, const std::array<double, 2> &p2, const std::array<double, 2> &q);

}  // namespace kerfkite

#endif
