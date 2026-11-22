// Artwork relief without Python: Studio's /api/artwork-relief on KerfKite's own polygon kernel. Closed artwork
// contours are sampled on a grid into a height field of 0s and 1s, raised (material kept wherever a sample's
// cell meets the artwork) or recessed (kept wherever the artwork does not cover the cell), and prepare_relief
// turns it into finishing paths. See core/src/artwork_relief.cpp for how the cells are decided and where the
// answers can differ from the Shapely reference.
#ifndef KERFKITE_ARTWORK_RELIEF_HPP
#define KERFKITE_ARTWORK_RELIEF_HPP

#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {

// The height field of a request: resolution rows of resolution 0s and 1s, row 0 at y. Settings are checked
// in the reference's order, with its messages: the contours ("paths"), width, height, x, y, resolution, kind.
std::vector<std::vector<int>> artwork_raster(const Json &data);

// The whole route: prepare_relief of the request with that height field, and the route's warning added.
Json artwork_relief(const Json &data);

// "artwork-relief": artwork_relief; "artwork-raster": the height field alone, for checks.
bool artwork_command(const std::string &command);
Json artwork_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
