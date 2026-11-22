// Foreign drawing and toolpath files read as editable artwork without Python: Studio's /api/program-import
// (studio/program_import.py) for posted G-code, HP-GL plotter files and LinuxCNC INI files. See
// core/src/program_import.cpp for what is reproduced; DXF drawings and text that is not ASCII are declined
// (NotPorted) for now.
#ifndef KERFKITE_PROGRAM_IMPORT_HPP
#define KERFKITE_PROGRAM_IMPORT_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// "program-import": {"name", "text", "unit"} -> {"objects", "layers", "presets", "devices", "warnings", "mode"}.
bool program_import_command(const std::string &command);
Json program_import_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
