// Fixture clearance without Python: studio/fixture_collision.py's check_fixtures, to the byte.
//
// The cutter and its holder are upright cylinders and each fixture an axis-aligned box. Every linear move of a
// posted three-axis program is swept against every box, continuously: a cylinder touches a box when its Z
// interval overlaps the box and its centre is within one radius of the box's XY rectangle at the same instant.
// The answer lists the first contacts of each move, up to 100 in all, with the line, the motion, the part that
// touches and where. Only the commands Studio posts are read; anything else is refused with its line number.
#ifndef KERFKITE_FIXTURE_COLLISION_HPP
#define KERFKITE_FIXTURE_COLLISION_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// {"collision", "events", "contact_count", "moves_checked", "fixtures_checked"} for a payload of "start",
// "tool" (diameter, stickout, holder_diameter, holder_length), "fixtures" (name, min, max) and "lines".
// Refusals throw ValueError with the reference's message; a program line that is not ASCII, which the
// reference reads with Unicode's rules, throws NotPorted.
Json check_fixtures(const Json &payload);

bool fixture_command(const std::string &command);  // "fixture-collision"
Json fixture_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
