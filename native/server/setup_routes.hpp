// Machine setup at the workstation: finding work zero with a touch plate, writing one controller setting (Grbl family or Marlin), and
// running a short operator macro. Each needs the controller idle and the operator's confirmation, and holds the
// controller for the whole request. The programs are built here so that they can be checked without a machine.
#ifndef KERFKITE_SERVER_SETUP_ROUTES_HPP
#define KERFKITE_SERVER_SETUP_ROUTES_HPP

#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace server {

class Studio;

// The probing program for a touch plate lying on the front-left corner of the stock, and what it assumed.
struct TouchPlate {
    std::vector<std::string> lines;
    double thickness = 0, wall = 0, radius = 0;
    bool corner = false;  // X and Y as well as Z
};
// request: {"axes": "z" | "xyz", "thickness", "wall", "tool_diameter", "feed", "slow_feed", "travel", "inset",
// "drop", "retract"}. Refuses values outside their ranges and a probe that could outlast an acknowledgement.
// zeroing: how the controller sets work zero, "G10" (G10 L20 P1, the Grbl family) or "G92" (Marlin).
TouchPlate touch_plate_program(const Json &request, const std::string &zeroing = "G10");

// "$<key>=<value>" for one numbered Grbl setting: request {"key": "100", "value": 80}.
std::string setting_line(const Json &request);
// One Marlin setting, such as "M92 X80.2", or "M500" for {"store": true}.
std::string marlin_setting_line(const Json &request);

// The lines of an operator macro, checked: plain G-code words only, at most 100 lines of at most 78 characters.
std::vector<std::string> macro_lines(const Json &request);

void add_setup_routes(Studio &studio);

}  // namespace server
}  // namespace kerfkite

#endif
