// Ramp and helix entry moves: studio/router_entry.py. Internal to the core.
#ifndef KERFKITE_ROUTER_ENTRY_HPP
#define KERFKITE_ROUTER_ENTRY_HPP

#include <string>
#include <vector>

#include "project.hpp"
#include "simple_line.hpp"

namespace kerfkite {
namespace detail {

// studio.router_entry.generate_entry: the whole job for a router project whose entry is not a plunge.
// source holds the contours as drawn, paths the prepared contours and caps their per-vertex heights (null
// when there are none, as for a contour without tabs). width and height are the validated work area;
// warnings are the operation's. A ramp descends along each prepared contour. A helix descends inside a
// pocket, at a place the polygon kernel finds clear of the pocket's walls and islands (pocket_entry.cpp),
// and then feeds across to the contour.
Job entry_job(const json &project, const std::vector<Contour> &source, const std::vector<Contour> &paths,
              const std::vector<std::vector<double>> *caps, double depth, double step, double safe, double feed,
              double plunge, double rpm, double width, double height, const std::vector<std::string> &warnings);

}  // namespace detail
}  // namespace kerfkite

#endif
