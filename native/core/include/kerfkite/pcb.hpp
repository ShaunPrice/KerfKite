// The circuit board commands by name, each taking and returning JSON: "gerber", "gerber-outline",
// "excellon" and "excellon-drills". This is what the command-line tool and the Python extension call.
#ifndef KERFKITE_PCB_HPP
#define KERFKITE_PCB_HPP

#include <string>

#include "kerfkite/excellon.hpp"
#include "kerfkite/gerber.hpp"

namespace kerfkite {

// Whether pcb_call answers this command.
bool pcb_command(const std::string &command);

nlohmann::json pcb_call(const std::string &command, const nlohmann::json &request);

}  // namespace kerfkite

#endif
