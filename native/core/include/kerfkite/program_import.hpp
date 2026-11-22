// Foreign drawing and toolpath files read as editable artwork without Python: Studio's /api/program-import
// (studio/program_import.py) for posted G-code, HP-GL plotter files and LinuxCNC INI files, reproduced to the
// bit (core/src/program_import.cpp), and ASCII DXF drawings, read by the core's own reader (core/src/dxf.cpp).
// Program text that is not ASCII, and a DXF spline given only by fit points, are declined (NotPorted).
#ifndef KERFKITE_PROGRAM_IMPORT_HPP
#define KERFKITE_PROGRAM_IMPORT_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// "program-import": {"name", "text", "unit"} -> {"objects", "layers", "presets", "devices", "warnings", "mode"}.
// "pdf-import": {"content", "page"} -> the same with "page", "pageCount" and "pageSize" (kerfkite/pdf_import.hpp).
// "bundle-import": {"content", "index"} -> a LightBurn bundle's members or one's text (kerfkite/bundle_import.hpp).
bool program_import_command(const std::string &command);
Json program_import_call(const std::string &command, const Json &request);

}  // namespace kerfkite

#endif
