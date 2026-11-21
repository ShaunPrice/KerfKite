// One entry for the command-line tool: every core command takes and returns JSON.
#include "kerfkite/leveling.hpp"
#include "kerfkite/pcb.hpp"
#include "kerfkite/solid_command.hpp"

namespace kerfkite {

nlohmann::json call(const std::string &command, const nlohmann::json &request) {
    if (command == "generate") return to_json(generate(request));
    if (pcb_command(command)) return pcb_call(command, request);
    if (command == "solid") return solid_call(request);
    return levelling_call(command, request);
}

}  // namespace kerfkite
