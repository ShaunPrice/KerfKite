// Isolation toolpaths for a circuit board: the copper image of a Gerber layer, and the contours a cutter
// follows round it to separate the copper from the rest of the blank. docs/PCB.md is the full description.
//
// The copper image is built on KerfKite's polygon kernel (kerfkite/polygon.hpp) from the objects the Gerber
// importer returns, dark and clear in file order. Contour k lies tool_width / 2 + k * step outside the copper,
// with step = tool_width * (1 - overlap). There is no Python version of this module.
#ifndef KERFKITE_ISOLATION_HPP
#define KERFKITE_ISOLATION_HPP

#include "kerfkite/cam.hpp"

namespace kerfkite {

// Request: {"text": the Gerber copper layer, "tool_width": the width of cut at the engraving depth in
// millimetres (0.01 to 10), and optionally
//   "passes" (1; 1 to 50) and "overlap" (0.2; 0 to 0.9, the part of the tool width one pass shares with the next),
//   "direction": "climb" (the default: copper on the right of travel, for a clockwise spindle) or "conventional",
//   "order": "outer_first" (the default: the contour next to the copper is cut last) or "inner_first",
//   "offset": [dx, dy] added to every coordinate, then "mirror_x": X to turn the board over about that line,
//   "clip": closed paths, in placed coordinates, outside which nothing is cut (the board outline),
//   "width" and "height" of the work area to have the fit checked,
//   "arc_tolerance" (0.005; 0.0005 to 0.1) for arcs and round shapes}.
// Answer: {"operations": [{"operation": "line", "paths": [...]}] in cutting order, "cut": [{"pass", "distance",
// "paths"}], "copper": [{"shell", "holes"}], "islands", "bounds", "copper_bounds", "unreachable": [{"at", "gap",
// "between", "islands"}], "unreachable_count", "smallest_gap", "separating_tool_width", "warnings", ...}.
// Where two separate pieces of copper are nearer than the tool is wide, no contour can pass between them: each
// such place is listed under "unreachable" and a warning says so. The paths are never returned without it.
Json gerber_isolation(const Json &request);

}  // namespace kerfkite

#endif
