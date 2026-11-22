// The core's own reader of ASCII DXF drawings, for Studio's import review: the drawing's model-space outlines
// in millimetres, with their layers, colours and the report studio/program_import.py's DXF reader gives.
// Internal to the core; program_import.cpp calls it for a .dxf file. See core/src/dxf.cpp.
#ifndef KERFKITE_DXF_READER_HPP
#define KERFKITE_DXF_READER_HPP

#include <string>

#include "kerfkite/json.hpp"

namespace kerfkite {

// {"objects", "layers", "presets", "devices", "warnings", "mode"} for the text of a DXF file, with
// default_scale millimetres per drawing unit when the drawing names no units. Throws ValueError with
// the reference's message for a drawing it refuses.
Json dxf_drawing(const std::string &text, double default_scale);

}  // namespace kerfkite

#endif
