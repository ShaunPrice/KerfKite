// Probe levelling: measure the stock surface on a grid, then follow it.
//
// A thin engraving, such as isolation routing on a circuit board, needs the cut depth held to a few
// hundredths of a millimetre over stock that is never that flat. The surface is probed at grid points,
// the heights are kept relative to one reference point, and every cutting move is split into short pieces
// whose depth is corrected by the height interpolated at each piece's end.
//
// There is no Python version of this module: it is written once, here, and Studio calls it.
#ifndef KERFKITE_LEVELING_HPP
#define KERFKITE_LEVELING_HPP

#include <string>
#include <vector>

#include "kerfkite/cam.hpp"

namespace kerfkite {

// Surface height offsets in millimetres on a regular grid, relative to the reference point (offset 0).
struct HeightMap {
    double x0 = 0, y0 = 0, dx = 0, dy = 0;  // first grid point and the spacing along each axis
    size_t columns = 0, rows = 0;
    std::vector<double> offsets;  // rows × columns, row by row from y0

    double at(double x, double y) const;  // bilinear between grid points, held at the edges
    double highest() const;
    double x1() const { return x0 + dx * static_cast<double>(columns - 1); }
    double y1() const { return y0 + dy * static_cast<double>(rows - 1); }
};

// {"x0","y0","dx","dy","columns","rows","offsets":[...]} with every limit checked.
HeightMap height_map_from_json(const Json &value);
Json to_json(const HeightMap &map);

// A probing program for Grbl-family controllers (G38.2) covering the cutting bounds of a job.
// Request: {"bounds":[x0,y0,x1,y1], "width", "height", and optionally "spacing" (10), "margin" (2),
// "clearance" (2), "depth" (1), "feed" (25)}, all in millimetres and millimetres a minute, and optionally
// "zero" (false): set work Z zero (G10 L20 P1) where the first point makes contact, less "plate" (0), the
// thickness of a touch plate. The spindle is switched off (M5) before the first move.
// Answer: {"lines":[...], "points":[[x,y],...] in probing order, "grid":{x0,y0,dx,dy,columns,rows},
// "order":[[column,row],...], and the "clearance", "depth" and "feed" used}.
Json probe_plan(const Json &request);

// The height map from the probe results of a plan.
// Request: {"grid":{...}, "order":[[column,row],...], and either "heights":[z,...] or
// "reports":["[PRB:x,y,z:1]",...] in probing order, optionally "reference" (index of the point taken as
// zero, default 0) and "limit" (largest accepted difference from the reference, default 5)}.
HeightMap height_map_from_probes(const Json &request);

// The program with every cutting move following the surface. Only absolute, millimetre programs of
// straight moves are accepted; a line that could move the tool in a way this does not handle is refused.
std::vector<std::string> level_program(const std::vector<std::string> &lines, const HeightMap &map,
                                       double max_segment);

// {"lines":[...], "height_map":{...}, optionally "max_segment"} -> {"lines":[...], "max_segment", "highest"}.
Json level_request(const Json &request);

// The levelling commands by name, each taking and returning JSON: "probe-plan", "height-map" and "level".
// This is what the Python extension exposes.
Json levelling_call(const std::string &command, const Json &request);

// Every core command by name, for the command-line tool: the above and "generate".
Json call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
