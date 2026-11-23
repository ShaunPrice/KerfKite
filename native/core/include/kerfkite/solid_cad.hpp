// The parametric solid history without Python: studio/solid_cad.py's rebuild and horizontal_section, and the
// CAD script preview, on the solid kernel through the same requests the Python makes, so the solids, measures
// and meshes are the same to the bit. See core/src/solid_cad.cpp for what is reproduced and what is declined (NotPorted).
#ifndef KERFKITE_SOLID_CAD_HPP
#define KERFKITE_SOLID_CAD_HPP

#include <functional>
#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// "solid-cad": {"model", "order"} -> rebuild(model): {"vertices", "triangles", "bounds", "volume", "profiles",
// "parameters"}. "solid-cad-section": {"model", "z", "order"} -> horizontal_section(model, z): {"z",
// "profiles"}. "order" lists the model's parameters in the order Python's dictionary holds them, which is
// the order they are resolved in; without it they are resolved in the order of their names. "cad-script":
// {"source", "parameters", "overrides", "order"} -> the /api/cad-script preview (core/src/scad.cpp): {"dimension": 2,
// "profiles", "bounds", "area"} for a drawing, {"dimension": 3, "vertices", "triangles", "bounds", "volume"} for a
// solid. "sketch-solve": {"feature", "parameters", "order"} -> studio.sketch_constraints.solve_profile: {"points",
// "dof", "redundant", "iterations"}. "sketch-points": {"sketch", "order"} -> the /api/solid-cad/sketch-points route
// (studio/design_api.py) on the sketch being edited: {"points", "parameters", "outline", "handles", "issue",
// "diagnostics", "constraint_error"}. "assembly-solve": {"assembly", "mesh", "orders"} -> studio.assembly.solve:
// {"parts", "contacts", "interferences"} (core/src/assembly.cpp). "solid-cad-surface": {"surface", "order",
// "orders"} -> studio.cad_exchange.surface_mesh(surface): {"vertices", "triangles", "area", "bounds", "faces",
// "solids"}, with "resolvedPatches" for a joined sheet (core/src/surface.cpp); order is that of the surface's
// parameters, orders that of each patch's.
//
// For STEP exchange through Open CASCADE (the workstation server's kerfkite-step add-on): "step-part": {"model",
// "order"} -> the steps of cad_exchange._part_shape, {"features": [{"shape", "turns", "move", "copies", "op"}]},
// each shape a box, sphere, cylinder, cone, loft (exact rational quarter patches), prism or revol of a sketch's
// exact edges, embedded STEP content, or the error the reference raises there. "step-faces": {"surface", "order",
// "orders"} -> the faces of cad_exchange._surface_shape, {"faces": [{"kind", "poles", "weights", "degrees", "knots",
// "multiplicities", "window" or "domain"}]}.
bool solid_cad_command(const std::string &command);
Json solid_cad_call(const std::string &command, const Json &request);

// The tessellation of a solid feature of kind step, as studio.cad_exchange.inspect_step answers for the feature's
// content ({"vertices", "triangles", "solidOnly", ...}), or ValueError with the reason. The workstation server sets
// it when its Open CASCADE add-on is installed; until one is set such features are declined (NotPorted).
using StepMesher = std::function<Json(const Json &content)>;
void set_step_mesher(StepMesher mesher);

}  // namespace kerfkite

#endif
