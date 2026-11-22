// What a connected controller reports about its machine: studio/machine_setup.py's describe().
#ifndef KERFKITE_SERVER_MACHINE_SETUP_HPP
#define KERFKITE_SERVER_MACHINE_SETUP_HPP

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

// The /api/machine answer for a device status. Reading only: nothing is written to the controller.
Json describe_machine(const Json &status);

}  // namespace server
}  // namespace kerfkite

#endif
