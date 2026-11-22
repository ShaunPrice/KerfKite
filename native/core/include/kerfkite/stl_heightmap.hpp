// STL height fields without Python: studio/mesh_import.py's stl_heightmap, to the bit.
//
// A binary or ASCII STL mesh, given as base64, is normalised to its bounds and rasterised from above: each cell
// of the grid is raised to the highest point of the triangles clipped to it, so that bilinear interpolation of
// the vertices cannot miss a thin peak, and a vertex next to an empty cell stays at the stock top. The result is
// a height field in 0..1 for studio.relief, with the mesh's bounds; it is a top-access envelope, not a solid.
#ifndef KERFKITE_STL_HEIGHTMAP_HPP
#define KERFKITE_STL_HEIGHTMAP_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// {"heightmap", "source_bounds", "warnings"} for {"content_base64", "resolution"}. Refusals throw ValueError with
// the reference's messages.
Json stl_heightmap(const Json &data);

bool stl_command(const std::string &command);  // "stl-heightmap"
Json stl_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
