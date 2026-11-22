// One entry for the command-line tool: every core command takes and returns JSON.
#include "kerfkite/leveling.hpp"
#include "kerfkite/operation_command.hpp"
#include "kerfkite/pcb.hpp"
#include "kerfkite/solid_command.hpp"
#include "kerfkite/polygon_command.hpp"
#include "kerfkite/rotary.hpp"

namespace kerfkite {

Json call(const std::string &command, const Json &request) {
    if (command == "generate") return to_json(generate(request));
    if (rotary_command(command)) return rotary_call(command, request);
    if (pcb_command(command)) return pcb_call(command, request);
    if (command == "solid") return solid_call(request);
    if (polygon_command(command)) return polygon_call(command, request);
    if (operation_command(command)) return operation_call(command, request);
    return levelling_call(command, request);
}

}  // namespace kerfkite
