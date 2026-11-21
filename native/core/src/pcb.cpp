// The circuit board commands by name: see kerfkite/pcb.hpp.
#include "kerfkite/pcb.hpp"

namespace kerfkite {

bool pcb_command(const std::string &command) {
    return command == "gerber" || command == "gerber-outline" || command == "excellon" || command == "excellon-drills";
}

nlohmann::json pcb_call(const std::string &command, const nlohmann::json &request) {
    if (command == "gerber") return gerber_import(request);
    if (command == "gerber-outline") return gerber_outline(request);
    if (command == "excellon") return excellon_import(request);
    if (command == "excellon-drills") return excellon_drills(request);
    throw ValueError("Unknown command: " + command);
}

}  // namespace kerfkite
