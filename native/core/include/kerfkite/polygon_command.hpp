// The polygon kernel as a command taking and returning JSON: "polygon". It is how the kernel is tested from
// outside the core, and how Studio's script engine draws: studio/solid_kernel.py keeps a 2D drawing as the
// rings of a region and asks for each operation here. kerfkite/polygon.hpp is the interface the core itself
// uses.
//
// A request is {"op": ...} with rings as lists of [x, y] in millimetres. "fill" names how rings that
// overlap are read: "evenodd" (the default), "nonzero" or "positive".
//
//   normalise     {"rings"}                               the region of the rings
//   compose       {"paths"}                               as studio.operations.region: closed, simple
//                                                         contours combined by even-odd, or its message
//   union, intersection, difference, xor   {"a", "b"}     two sets of rings, each read with "fill"
//   offset        {"rings", "distance"}                   grown or shrunk by a distance, round joins
//                 with "join": "round", "miter" or "bevel"  the corners of a drawing instead (polygon::JoinOptions),
//                                                         with "turn_segments" and "miter_limit"
//   hull          {"rings"}                               the convex hull of every point of the rings
//   circle        {"radius", "segments"}                  a regular polygon inscribed in the circle, a vertex on +X
//   transform     {"rings", "matrix": [[a, b, c], [d, e, f]]}   the region moved by an affine map
//   rotate        {"rings", "degrees"}                    the region turned about the origin, counter-clockwise
//   pocket        {"rings", "first", "step"}              shrunk by first, then by step until nothing is left
//   clip-lines    {"rings", "lines": [[[x, y], [x, y]]]}  each line cut to the region
//   artwork       {"shapes": [...], "offsets": [...]}     dark and clear shapes in order, then contours
//   inspect       {"rings"}                               per ring: whether it is simple, its signed area
//   locate        {"rings", "points"}                     "inside", "boundary" or "outside" for each point
//   delaunay      {"points"}                              the Delaunay triangulation: "points" (distinct, in
//                                                         order), "triangles" [a, b, c] and "edges"
//                                                         [a, b, left triangle, right triangle], null on the hull
//   voronoi       {"points", "box": [[x, y], [x, y]]}     the Voronoi edges of the points cut to the box:
//                                                         "points" and "edges" [{"ends", "sites"}]
//   distance      {"rings", "points"}                     the distance in millimetres from each point to the
//                                                         boundary of the region
//   statistics    {}                                      "rounds": the most rounds of splitting any request
//                                                         of this run has needed
//
// "segments" (edges in a quarter turn) and "tolerance" (millimetres) set how arcs are drawn. A region is
// answered as {"polygons": [{"shell": [...], "holes": [[...]]}], "area": square millimetres}, with
// "valid" added when the request has "check": true and, with "outlines": true, "outlines": the rings in
// the order and directions the application cuts them (polygon::outlines).
#ifndef KERFKITE_POLYGON_COMMAND_HPP
#define KERFKITE_POLYGON_COMMAND_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// Whether polygon_call answers this command.
bool polygon_command(const std::string &command);

kerfkite::Json polygon_call(const std::string &command, const kerfkite::Json &request);

}  // namespace kerfkite

#endif
