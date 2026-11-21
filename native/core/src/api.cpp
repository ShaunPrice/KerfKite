// One entry for the command-line tool: every core command takes and returns JSON.
#include "kerfkite/leveling.hpp"

namespace kerfkite {

nlohmann::json call(const std::string &command, const nlohmann::json &request) {
    if (command == "generate") return to_json(generate(request));
    return levelling_call(command, request);
}

}  // namespace kerfkite
