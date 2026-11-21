// Gerber import: an RS-274X or X2 layer as a list of drawn objects in millimetres.
//
// A circuit board layer is an ordered list of flashes, lines, arcs and filled regions, each dark (adds
// copper, ink, mask) or clear (removes whatever lies beneath it). This module reads the file into that list
// and stops there: turning the list into copper polygons, and those into isolation toolpaths, needs polygon
// union and offsetting, which is a later step. See docs/PCB.md for the answer in full and for what is declined.
//
// There is no Python version of this module: it is written once, here, and Studio calls it.
#ifndef KERFKITE_GERBER_HPP
#define KERFKITE_GERBER_HPP

#include "kerfkite/cam.hpp"

namespace kerfkite {

// Request: {"text": the file, and optionally "arc_tolerance" (0.005): the furthest a flattened arc may lie
// from the true arc, in millimetres, 0.0005 to 0.1}.
// Answer: {"format":"gerber", "units":"mm"|"inch" (of the file; the answer is always millimetres),
// "coordinate_format":{"integer","decimal","omitted_zeros":"leading"|"trailing","notation"},
// "file_function": text or null, "attributes":{name: text}, "arc_tolerance",
// "apertures":[{"code", "shape":"circle"|"rectangle"|"obround"|"polygon"|"macro", ...}],
// "objects":[{"type":"flash"|"line"|"arc"|"region", "polarity":"dark"|"clear", ...}] in drawing order,
// "counts":{"flash","line","arc","region"}, "bounds":[min X, min Y, max X, max Y] or null, "warnings":[...]}.
// A file that cannot be read in full is refused with ValueError; nothing is guessed.
Json gerber_import(const Json &request);

// The board outline of a profile (Edge.Cuts) layer as closed paths for a cut-out line job.
// Request: {"text", and optionally "arc_tolerance", "tolerance" (0.01): how far apart two line ends may be
// and still join, "offset":[dx,dy] added to every point, and "width" and "height" of the work area to have
// the fit checked}.
// Answer: {"paths":[[[x,y],...],...] each closed, largest first, "line_width", "bounds", "warnings"}.
// An outline that is open or that branches is refused.
Json gerber_outline(const Json &request);

}  // namespace kerfkite

#endif
