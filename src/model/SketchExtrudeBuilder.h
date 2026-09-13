#pragma once

#include <string>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Shape.hxx>

#include "model/ExtrudeFeature.h"

namespace solidar {
struct DocumentSketch;

struct SketchExtrudeGeometry {
  gp_Pnt centroid;
  gp_Dir normal;
};

// V1 direct-interaction profile contract: one solid closed line wire or one
// solid circle. Construction geometry is ignored; multiple loops and holes are
// rejected because ExtrudeFeature persists only the owning SketchId.
[[nodiscard]] bool isSupportedSingleSketchProfile(
    const DocumentSketch& profile, std::string* error = nullptr);

// Authoritative sketch extrusion builder shared by interactive previews and
// ExtrudeFeature recompute. baseShape must be null for NewBody and non-null for
// Join/Cut. The returned shape is one checked solid.
bool buildExtrusionFromSketch(const DocumentSketch& profile,
                              const TopoDS_Shape* baseShape,
                              double lengthMm, ExtrudeOperation operation,
                              bool reversed, TopoDS_Shape* result,
                              SketchExtrudeGeometry* geometry,
                              std::string* error);

}  // namespace solidar
