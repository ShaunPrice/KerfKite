// The parametric solid history without Python: studio/solid_cad.py's rebuild and horizontal_section, and the
// CAD script preview, on the solid kernel through the same requests the Python makes, so the solids, measures
// and meshes are the same to the bit. See core/src/solid_cad.cpp for what is reproduced and what is declined (NotPorted).
#ifndef KERFKITE_SOLID_CAD_HPP
#define KERFKITE_SOLID_CAD_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// "solid-cad": {"model", "order"} -> rebuild(model): {"vertices", "triangles", "bounds", "volume", "profiles",
// "parameters"}. "solid-cad-section": {"model", "z", "order"} -> horizontal_section(model, z): {"z",
// "profiles"}. "order" lists the model's parameters in the order Python's dictionary holds them, which is
// the order they are resolved in; without it they are resolved in the order of their names. "cad-script":
// {"source", "parameters", "overrides", "order"} -> the /api/cad-script preview (core/src/scad.cpp): {"dimension": 2,
// "profiles", "bounds", "area"} for a drawing, {"dimension": 3, "vertices", "triangles", "bounds", "volume"} for a
// solid.
bool solid_cad_command(const std::string &command);
Json solid_cad_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
