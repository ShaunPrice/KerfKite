// Editable surfaces without Python: the rational Bézier and clamped B-spline patches of studio/cad_exchange.py,
// their joined sheets, UV trims and drawn outlines, tessellated for the editor and for STL. Internal to the core;
// the request is "solid-cad-surface" (kerfkite/solid_cad.hpp). See surface.cpp.
#ifndef KERFKITE_SURFACE_HPP
#define KERFKITE_SURFACE_HPP

#include <array>
#include <memory>

#include "kerfkite/json.hpp"

namespace kerfkite {
namespace surface {

// A patch's named parameters, as studio.solid_cad.Dimensions resolves them: vector(value, 3) for one pole.
class Parameters {
public:
    virtual ~Parameters() = default;
    virtual std::array<double, 3> point(const Json *value) = 0;
};

// Dimensions(definitions), resolved in the given order (JSON objects in the core do not keep theirs);
// defined with Dimensions in solid_cad.cpp. Throws ValueError as Dimensions does.
std::unique_ptr<Parameters> parameters(const Json *definitions, const Json *order);

// "solid-cad-surface": {"surface", "order", "orders"} -> studio.cad_exchange.surface_mesh(surface): the mesh's
// vertices, triangles, area, bounds, faces and solids, with the resolved patches of a joined sheet. order is the
// order of the surface's parameters, orders that of each patch's.
Json mesh(const Json &request);

// "step-faces": the same request -> the faces cad_exchange._surface_shape makes for STEP export, for Open CASCADE
// to build (kerfkite/solid_cad.hpp).
Json step_faces(const Json &request);

}  // namespace surface
}  // namespace kerfkite

#endif
