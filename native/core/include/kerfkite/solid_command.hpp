// The "solid" command: a small description of a solid (primitives, transforms, Boolean operations) in,
// measures and whatever else is asked for out. This is the only place where the solid kernel meets JSON;
// the kernel itself (solid.hpp) has no JSON in its interface. docs/SOLID-KERNEL.md lists the request.
#ifndef KERFKITE_SOLID_COMMAND_HPP
#define KERFKITE_SOLID_COMMAND_HPP

#include <vector>

#include "kerfkite/cam.hpp"
#include "kerfkite/solid.hpp"

namespace kerfkite {

kerfkite::Json solid_call(const kerfkite::Json &request);

// The same for a caller that keeps solids between requests, as Studio's Python does: a node {"kept": i}
// stands for kept[i], and `built`, when given, receives the solid of "shape". A model is then built one
// operation at a time without a mesh crossing as text, and a kept solid remembers what the kernel has
// already found out about it.
kerfkite::Json solid_call(const kerfkite::Json &request, const std::vector<solid::Solid> &kept, solid::Solid *built);

}  // namespace kerfkite

#endif
