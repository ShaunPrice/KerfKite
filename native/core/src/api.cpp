// One entry for the command-line tool: every core command takes and returns JSON.
#include "kerfkite/leveling.hpp"
#include "kerfkite/pcb.hpp"
#include "kerfkite/polygon_command.hpp"

namespace kerfkite {

nlohmann::json call(const std::string &command, const nlohmann::json &request) {
    if (command == "generate") return to_json(generate(request));
    if (pcb_command(command)) return pcb_call(command, request);
    if (polygon_command(command)) return polygon_call(command, request);
    return levelling_call(command, request);
}

}  // namespace kerfkite
