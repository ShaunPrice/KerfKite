// The design routes that are more than one call of the core: Boolean operations and offsets on artwork
// (/api/geometry), artwork extruded to an STL (/api/extrude), the circuit board import (/api/pcb-import), the
// stock preview's binary request (/api/simulate), the sketch being edited (/api/solid-cad/sketch-points) and
// the example assembly. None of them reaches the controller.
#ifndef KERFKITE_SERVER_DESIGN_EXTRA_HPP
#define KERFKITE_SERVER_DESIGN_EXTRA_HPP

#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

// design_api.geometry: {"paths"} of the union, difference, intersection or offset of the objects' regions.
Json geometry(const Json &data);
// solid_text.extrude: {"stl", "triangles", "height"}, the region of closed artwork as a prism.
Json extrude_artwork(const Json &data);
// pcb_api.board: what a board's copper, outline and drill files hold, placed on the work area.
Json pcb_board(const Json &payload);
// stock_wire.from_stream: the stock preview settings of a binary request, and its segments in rows.
Json stock_wire(const std::string &body, std::vector<double> &rows);
// The sketch-points route on the core; order lists the sketch's parameters as the request wrote them.
Json sketch_points(const Json &data, const Json &order);
// assembly.example_assembly: a shaft constrained to the bore of an upright bearing plate.
Json example_assembly();

}  // namespace server
}  // namespace kerfkite

#endif
