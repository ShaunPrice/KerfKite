// One entry for the command-line tool: every core command takes and returns JSON.
#include "kerfkite/leveling.hpp"
#include "kerfkite/pcb.hpp"

namespace kerfkite {

Json call(const std::string &command, const Json &request) {
    if (command == "generate") return to_json(generate(request));
    if (pcb_command(command)) return pcb_call(command, request);
    return levelling_call(command, request);
}

}  // namespace kerfkite
