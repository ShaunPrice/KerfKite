// A parametric solid history rebuilt to its solid, for the parts of an assembly (assembly.cpp). Internal to the
// core; solid_cad.cpp defines it.
#ifndef KERFKITE_SOLID_CAD_BUILD_HPP
#define KERFKITE_SOLID_CAD_BUILD_HPP

#include "kerfkite/cam.hpp"
#include "solid_requests.hpp"

namespace kerfkite {

// solid_cad._build(model)[0]; order lists the model's parameters in the order to resolve them, or is null.
solid_requests::Shape solid_cad_build(const Json *model, const Json *order);

}  // namespace kerfkite

#endif
