// Cylindrical 4-axis output without Python: the same programs as studio/rotary.py, to the byte.
//
// A planar job is wrapped onto a cylinder of the given diameter: the wrapped coordinate becomes an angle of
// the rotary axis, the depth a radial coordinate, and every cutting move an inverse-time feed. Validation runs
// in the reference's order, so an invalid request fails with the reference's message. NotPorted is thrown
// where the reference would go where this file does not follow (input that is not plain ASCII text, numbers
// the reference holds as Python integers beyond the double range); nothing is ever answered differently.
#ifndef KERFKITE_ROTARY_HPP
#define KERFKITE_ROTARY_HPP

#include <string>

#include "kerfkite/cam.hpp"
#include "kerfkite/json.hpp"

namespace kerfkite {

// studio.rotary.generate_rotary(project, options): the project's planar job, generated here, wrapped onto the
// cylinder. The answer is the job (lines, segments, cut_length, cut_minutes, mode, bounds, warnings and the
// rest of what generate() gives) with the rotary program as its lines, the rotary warnings added and the
// setup under "rotary". The preview segments are those of the planar job, as the reference packs them.
Json generate_rotary(const Json &project, const Json &options);

// The same job before it is written as JSON: the planar job, whose lines and warnings the rotary ones
// replace, and those.
struct RotaryJob {
    Job planar;
    std::vector<std::string> lines;  // the rotary program
    Json warnings;                   // the planar job's warnings and the rotary ones
    Json rotary;                     // the setup as worked out
};
RotaryJob rotary_job(const Json &project, const Json &options);

// studio.rotary.wrap_job(job, options): a job of canonical G-code lines, as an earlier generation or a file
// gave them, wrapped onto the cylinder. Every other member of the job is kept.
Json wrap_job(const Json &job, const Json &options);

// The commands: "rotary" takes {"project", "options"}, "rotary-wrap" takes {"job", "options"}.
bool rotary_command(const std::string &command);
Json rotary_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
