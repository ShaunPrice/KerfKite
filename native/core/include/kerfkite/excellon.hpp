// Excellon import: a drill file as tools, holes and slots in millimetres.
//
// Excellon files often leave out how their numbers are written. Where the file states its units, digits and
// zero suppression those are used. Where it does not, the format description's defaults are used, the answer
// says which, and a warning is given; the request can state any of them outright. Units are never assumed:
// a file that does not state them is refused until the request does. See docs/PCB.md.
//
// There is no Python version of this module: it is written once, here, and Studio calls it.
#ifndef KERFKITE_EXCELLON_HPP
#define KERFKITE_EXCELLON_HPP

#include "kerfkite/cam.hpp"

namespace kerfkite {

// Request: {"text": the file, and optionally "units":"mm"|"inch", "integer" and "decimal" (the digits
// either side of the implied decimal point), "omitted_zeros":"leading"|"trailing"}.
// Answer: {"format":"excellon", "units", "interpretation":{"units","integer","decimal","omitted_zeros",
// "units_from","digits_from","zeros_from"}, "plated": true, false or null, "file_function": text or null,
// "tools":[{"number","diameter","hits":[[x,y],...]}], "slots":[{"tool","diameter","points":[[x,y],...]}],
// "counts":{"hits","slots"}, "bounds":[min X, min Y, max X, max Y] of the hole edges or null, "warnings":[...]}.
Json excellon_import(const Json &request);

// The holes of a drill file as drilling operations for studio.cam.generate, grouped by drill diameter.
// Request: as excellon_import, with optionally "offset":[dx,dy] added to every hole, "mirror_x": X to turn
// the result over about that line afterwards (holes drilled from the underside), and "width" and "height" of
// the work area to have the fit checked.
// Answer: {"operations":[{"operation":"drill","tool_shape":"drill","tool_diameter","paths":[[[x,y],[x,y]],...]}],
// "groups":[{"tool_diameter","holes","tools":[numbers]}], "slots":[...] (not drilled), "bounds",
// "interpretation", "warnings"}. Operations of one diameter may share an ordered job; each diameter is a
// separate job, because an ordered job has one tool.
Json excellon_drills(const Json &request);

}  // namespace kerfkite

#endif
