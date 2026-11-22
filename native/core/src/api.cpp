// One entry for the command-line tool: every core command takes and returns JSON.
#include "kerfkite/leveling.hpp"
#include "kerfkite/operation_command.hpp"
#include "kerfkite/pcb.hpp"
#include "kerfkite/solid_command.hpp"
#include "kerfkite/polygon_command.hpp"
#include "kerfkite/relief.hpp"
#include "kerfkite/artwork_relief.hpp"
#include "kerfkite/rotary.hpp"
#include "kerfkite/stock.hpp"
#include "kerfkite/tool_changes.hpp"
#include "kerfkite/fixture_collision.hpp"
#include "kerfkite/stl_heightmap.hpp"

namespace kerfkite {

Json call(const std::string &command, const Json &request) {
    if (command == "generate") return to_json(generate(request));
    if (rotary_command(command)) return rotary_call(command, request);
    if (relief_command(command)) return relief_call(command, request);
    if (stock_command(command)) return stock_call(command, request);
    if (tool_changes_command(command)) return tool_changes_call(command, request);
    if (fixture_command(command)) return fixture_call(command, request);
    if (stl_command(command)) return stl_call(command, request);
    if (artwork_command(command)) return artwork_call(command, request);
    if (pcb_command(command)) return pcb_call(command, request);
    if (command == "solid") return solid_call(request);
    if (polygon_command(command)) return polygon_call(command, request);
    if (operation_command(command)) return operation_call(command, request);
    return levelling_call(command, request);
}

}  // namespace kerfkite
