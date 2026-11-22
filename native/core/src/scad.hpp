// KerfKite's CAD script language without Python: studio/scad_language.py and studio/scad_geometry.py.
// Internal to the core; solid_cad.cpp offers it as the "cad-script" command and for the scripted features
// of a solid history. scad.cpp says what is reproduced.
#ifndef KERFKITE_SCAD_HPP
#define KERFKITE_SCAD_HPP

#include <string>
#include <utility>
#include <vector>

#include "kerfkite/cam.hpp"
#include "solid_requests.hpp"

namespace kerfkite {
namespace scad {

// scad_geometry.ScriptGeometryBudget: the geometry work of one preview, or of one rebuild of a solid
// history, which its scripts share.
struct Budget {
    long long remaining = 400000;
    long long nodes = 0;
};

// What build_script answers: a drawing (dimension 2) or a solid (dimension 3).
struct Built {
    int dimension = 0;
    solid_requests::Shape solid;
    solid_requests::Region region;
};

// scad_geometry.build_script(source, parameters, budget=budget). The parameters are floats, as
// solid_cad.Dimensions.script_parameters gives them.
Built build_script(const Json *source, const std::vector<std::pair<std::string, double>> &parameters, Budget &budget);

// The region's bounds as CrossSection.bounds() gives them: (min x, min y, max x, max y).
std::vector<double> region_bounds(const solid_requests::Region &region);

}  // namespace scad
}  // namespace kerfkite

#endif
