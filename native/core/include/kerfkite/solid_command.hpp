// The "solid" command: a small description of a solid (primitives, transforms, Boolean operations) in,
// measures and whatever else is asked for out. This is the only place where the solid kernel meets JSON;
// the kernel itself (solid.hpp) has no JSON in its interface. docs/SOLID-KERNEL.md lists the request.
#ifndef KERFKITE_SOLID_COMMAND_HPP
#define KERFKITE_SOLID_COMMAND_HPP

#include "kerfkite/cam.hpp"

namespace kerfkite {

nlohmann::json solid_call(const nlohmann::json &request);

}  // namespace kerfkite

#endif
