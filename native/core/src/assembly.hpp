// studio/assembly.py's solve, for the "assembly-solve" command of solid_cad.cpp. Internal to the core.
#ifndef KERFKITE_ASSEMBLY_HPP
#define KERFKITE_ASSEMBLY_HPP

#include "kerfkite/cam.hpp"

namespace kerfkite {

// The /api/assembly/solve route: {"assembly", "mesh", "orders"} -> {"parts", "contacts", "interferences"}.
// "orders" lists, for each part, its model's parameters in the order of the Python dictionary.
Json assembly_solve(const Json &payload);

}  // namespace kerfkite

#endif
