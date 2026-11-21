// Vector laser paths: studio/laser.py. Internal to the core.
#ifndef KERFKITE_LASER_HPP
#define KERFKITE_LASER_HPP

#include <array>
#include <vector>

#include "project.hpp"

namespace kerfkite {
namespace detail {

struct LaserCut {
    enum Pattern { Continuous, Perforate, Bridges } pattern = Continuous;
    enum Shape { Straight, ArcLeft, ArcRight } shape = Straight;
    double gap = 0, dash = 0;
    long count = 0;
};

// A vertex of a prepared laser path and the power of the move that ends at it. whole_zero marks the
// integer 0 the reference uses inside a beam-off gap, which the preview data keeps apart from 0.0.
struct LaserVertex {
    double x, y, power;
    bool whole_zero;
};

// studio.laser.cut_options(project, number)
LaserCut cut_options(const json &project);

// studio.laser.prepare(path, power, options, width, height, cut).
// options is lead-in, lead-out, ramp-in, ramp-out, start percent, end percent.
std::vector<LaserVertex> prepare(const std::vector<std::array<double, 2>> &path, double power, const double *options,
                                 double width, double height, const LaserCut &cut);

}  // namespace detail
}  // namespace kerfkite

#endif
