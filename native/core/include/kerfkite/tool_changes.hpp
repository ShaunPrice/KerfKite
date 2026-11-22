// Manual tool changes without Python: studio/tool_changes.py's plan_tool_changes, to the byte.
//
// A router project's operations are split into stages, one for each run of consecutive operations with the same
// tool (number, diameter, shape, V angle and drill point angle). Each stage is a whole job, generated as
// studio.cam generates an ordered project, with the tool, the operations' numbers, a file name and what to do
// before running it. Nothing here talks to a machine, and stages are never joined into one program.
#ifndef KERFKITE_TOOL_CHANGES_HPP
#define KERFKITE_TOOL_CHANGES_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// The plan: {"format", "mode", "manual_only", "stages", "tool_changes", "warnings"}. A project the reference
// refuses throws ValueError with its message; the few cases where the reference itself fails other than with
// ValueError (a stage with fewer than two program lines) throw NotPorted.
Json plan_tool_changes(const Json &project);

bool tool_changes_command(const std::string &command);  // "tool-changes"
Json tool_changes_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
