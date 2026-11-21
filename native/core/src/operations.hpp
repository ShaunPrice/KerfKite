// Polygon operations on closed contours: studio/operations.py. Internal to the core.
#ifndef KERFKITE_OPERATIONS_HPP
#define KERFKITE_OPERATIONS_HPP

#include <array>
#include <string>
#include <vector>

#include "kerfkite/polygon.hpp"
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
// concentric pockets, hatch and cross-hatch fills, and V-carving, all on KerfKite's own polygon kernel.
// Rejections are the reference's, in the reference's order.
// studio.operations.region(paths): the even-odd region of closed contours, or the reference's message for
// a contour that is open, crosses itself or has no area, or for contours that cancel.
polygon::PolygonSet contour_region(const std::vector<std::vector<std::array<double, 2>>> &paths);

// A helical entry into a pocket: the centre of the helix and the angle at which it starts and ends.
struct HelixEntry {
    double x, y, angle;
};

// studio.router_entry._pocket_entries(source, paths, bit_radius, radius) from the point where the reference
// builds the pocket's region: one entry for each seam, the first point of each pocket contour. source holds
// the pocket's contours as drawn. Throws the reference's messages when the pocket has no room for the
// cutter and helix, or no clear feed from a helix to a seam. See pocket_entry.cpp.
std::vector<HelixEntry> pocket_entries(const std::vector<std::vector<std::array<double, 2>>> &source,
                                       const std::vector<std::array<double, 2>> &seams, double bit_radius, double radius);

// The cosine and sine of an angle given in degrees, from the kernel's series: the same bits on every
// platform, and exactly 0 and 1 for a whole number of quarter turns.
void cos_sin_degrees(double degrees, double &cosine, double &sine);

// The paths of a V-carve and the depth of the cutter at each of their points, zero or below.
struct VCarve {
    std::vector<std::vector<std::array<double, 2>>> paths;
    std::vector<std::vector<double>> depths;
    // The points taken along the outlines, whose Voronoi diagram the paths follow: for checking the carve.
    std::vector<std::array<double, 2>> samples;
};

// studio.operations.prepare for V-carving, from the region on: the sampled medial axis of the region for
// a V-bit of the given whole angle in degrees and radius, sampled every `resolution` millimetres and cut
// no deeper than max_depth. Throws the reference's message for an outline of more than 12,000 samples,
// and the generator's for a carve of more than 200,000 points. The result may be empty. See vcarve.cpp.
VCarve vcarve_paths(const polygon::PolygonSet &g, double resolution, double angle, double max_depth, double radius);

// depths is left empty unless the operation gives a depth to every point of its paths (V-carving).
std::vector<std::string> prepare_operation(const json &project, std::vector<std::vector<std::array<double, 2>>> &paths,
                                           std::vector<std::vector<double>> &depths, double width, double height);

}  // namespace detail
}  // namespace kerfkite

#endif
