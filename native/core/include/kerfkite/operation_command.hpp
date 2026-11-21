// The "operation" command: the polygon CAM operations on closed contours, for Studio's Python.
//
// studio/operations.py validates a project's settings and then asks for the geometry here, which is the
// code generate() itself uses, so that Studio and the command-line tool cut the same paths.
//
//   {"op": "offset", "paths": [...], "distance": d}    outside (d > 0) or inside (d < 0) compensation
//   {"op": "pocket", "paths": [...], "radius": r, "step": s}
//   {"op": "fill", "paths": [...], "interval": i, "angle": a, "crosshatch": false}
//   {"op": "region", "paths": [...]}                   the contours' even-odd region alone
//   {"op": "helix", "paths": [...], "seams": [[x, y], ...], "bit_radius": r, "radius": h}
//
// paths are closed contours as lists of [x, y] in millimetres. The answer is {"paths": [...]}: the tool
// paths in cutting order. Each coordinate is a whole number of millionths of a millimetre divided by 1e6,
// written as the shortest text that reads back as the same double.
//
// "helix" places a helical entry into the pocket the contours describe, for a cutter of radius r and a
// helix of radius h, one for each seam (the first point of a pocket contour). Its answer is
// {"entries": [[x, y, angle], ...]}: the centre of each helix and the angle, in radians, at which it
// starts and ends, each written as the shortest text that reads back as the same double.
//
// A contour the operation cannot use, or a tool that does not fit, throws ValueError with the message
// Studio shows.
#ifndef KERFKITE_OPERATION_COMMAND_HPP
#define KERFKITE_OPERATION_COMMAND_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

bool operation_command(const std::string &command);
Json operation_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
