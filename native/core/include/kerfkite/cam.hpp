// G-code generation without Python: the same programs as studio/cam.py, to the byte.
//
// The Python module remains the reference while the port is in progress. generate() covers the cases
// listed in native/README.md and throws NotPorted, at the point where the reference would enter the
// missing branch, for everything else. It never produces output for a case it does not fully reproduce.
#ifndef KERFKITE_CAM_HPP
#define KERFKITE_CAM_HPP

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "kerfkite/json.hpp"

namespace kerfkite {

// An input the reference rejects. what() is the reference's message.
struct ValueError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A valid request for a feature this library does not yet reproduce. what() names the feature.
struct NotPorted : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One cutting move for the preview: start and end in millimetres, and the laser power when there is one.
struct Segment {
    double x, y, nx, ny, end_z;
    bool has_power;
    double power;
    double start_z;
    bool planar = false;       // a laser move: the heights are the integer 0 in the job data
    bool whole_power = false;  // the power is the integer 0 of a beam-off gap
    bool whole_end = false;    // the end height is the integer 0 that starts a ramp entry
    bool whole_start = false;  // the start height is that integer 0
};

struct Job {
    std::vector<std::string> lines;
    std::vector<Segment> segments;
    double cut_length = 0, cut_minutes = 0;
    std::string mode;
    std::array<double, 4> bounds{};
    std::vector<std::string> warnings;
};

// The job for a project, as studio.cam.generate(project) returns it.
Job generate(const Json &project);

// The job as the JSON object the Studio API returns: lines, segments, cut_length, cut_minutes, mode,
// bounds and warnings.
Json to_json(const Job &job);

struct Protocol {
    std::string id, name, family, notes;
    std::vector<std::string> modes;
};

// The controller profile, from the table shared with the editor (studio/static/protocols.json).
const Protocol &get_protocol(const Json *protocol);

// The program as the selected controller family expects it.
std::vector<std::string> postprocess(const std::vector<std::string> &lines, const Json *protocol,
                                     const std::string &mode, const Json *dwell_units);

}  // namespace kerfkite

#endif
